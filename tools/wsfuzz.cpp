// wsfuzz: the game's WebSocket frame reader (src/net/http's FrameReader, what reads the lobby's frames on macOS and
// Linux) against anything a server could send, built with AddressSanitizer and UndefinedBehaviorSanitizer (make
// build/bin/wsfuzz): any read or write out of bounds, any undefined behaviour, stops it with where.
//   - valid streams (messages of every length edge, 0, 125, 126, 127, 65535, 65536, fragmented, pings between the
//     fragments) cut into pieces of every size: every message comes out whole and the same, every ping handed back
//   - every rule a client must hold the server to (a masked frame, reserved bits and opcodes, a control frame
//     fragmented or over 125, a continuation with nothing begun, a new message in the middle of one, a close with
//     an odd payload, a length past the limit, 64-bit lengths with their top bit set): the connection ends, said why
//   - bytes at random, and valid streams spoilt (bits flipped, cut short, bytes spliced): never a crash, never a
//     message longer than the limit, the buffer never grown without bound
//   - a server that never stops sending (a frame too long, a message fragmented without end, pings without end), read
//     as the Socket reads it, no more than room() at a time: the buffer never past the limit and a frame's head, the
//     connection ended or every frame taken; a message of the limit's length exactly taken whole; a buffer of a
//     megabyte of tiny frames taken in one go, at once (not each frame moving the rest down)
//   wsfuzz [--runs N] [--seed S]
#include "../src/net/http.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using vfnet::http::FrameReader;

namespace {

std::mt19937 rng;
int fails = 0;
void check(bool ok, const char *what)
{
	if (!ok) { fails++; printf("  FAIL %s\n", what); }
}

// a server's frame: FIN, the opcode, the length in its three forms (or forced to one: len_form 0 the shortest)
std::string frame(uint8_t op, const std::string &data, bool fin = true, int len_form = 0, bool mask = false, uint8_t rsv = 0)
{
	std::string f;
	f += char((fin ? 0x80 : 0) | rsv | op);
	const uint64_t n = data.size();
	const int form = len_form ? len_form : n < 126 ? 1 : n < 65536 ? 2 : 3;
	const uint8_t m = mask ? 0x80 : 0;
	if (form == 1) f += char(m | n);
	else if (form == 2) { f += char(m | 126); f += char(n >> 8); f += char(n & 255); }
	else { f += char(m | 127); for (int i = 7; i >= 0; i--) f += char((n >> (8 * i)) & 255); }
	if (mask) f += std::string("\x01\x02\x03\x04", 4);
	return f + data;
}
std::string bytes(size_t n)
{
	std::string s(n, '\0');
	for (char &c : s) c = char(rng());
	return s;
}

struct Fed { std::vector<std::pair<std::string, bool>> msgs; std::vector<std::string> pings; FrameReader::Got end = FrameReader::Got::more; std::string why; size_t most_buf = 0; };
// the stream given in pieces of random sizes (1 byte to all of it), as a connection hands them over
Fed feed(const std::string &stream, size_t max = FrameReader::MOST)
{
	Fed f;
	FrameReader r;
	r.max = max;
	std::string buf;
	size_t at = 0;
	while (at < stream.size() && f.end == FrameReader::Got::more) {
		const size_t piece = std::min(stream.size() - at, size_t(1 + rng() % (rng() % 4 == 0 ? 3 : 70000)));
		buf.append(stream, at, piece);
		at += piece;
		f.most_buf = std::max(f.most_buf, buf.size());
		f.end = r.take(buf, [&](std::string &&m, bool b) { f.msgs.push_back({ std::move(m), b }); }, [&](const std::string &p) { f.pings.push_back(p); });
	}
	f.why = r.why;
	return f;
}

// a valid stream: messages, some fragmented, pings and pongs between frames (fragments' too)
std::string valid(std::vector<std::pair<std::string, bool>> &msgs, std::vector<std::string> &pings, int count)
{
	static const size_t edges[] = { 0, 1, 125, 126, 127, 128, 65535, 65536, 65537, 200000 };
	std::string s;
	for (int i = 0; i < count; i++) {
		const size_t n = rng() % 3 == 0 ? edges[rng() % 10] : rng() % 3000;
		const std::string m = bytes(n);
		const bool binary = rng() % 2;
		msgs.push_back({ m, binary });
		const int parts = rng() % 4 == 0 ? 1 + int(rng() % 5) : 1;
		size_t at = 0;
		for (int p = 0; p < parts; p++) {
			const size_t len = p + 1 == parts ? m.size() - at : std::min(m.size() - at, size_t(rng() % (m.size() + 1)));
			s += frame(p == 0 ? (binary ? 2 : 1) : 0, m.substr(at, len), p + 1 == parts, rng() % 5 == 0 && len < 126 ? 1 + int(rng() % 3) : 0);
			at += len;
			if (rng() % 3 == 0) { const std::string pp = bytes(rng() % 126); s += frame(9, pp); pings.push_back(pp); }
			if (rng() % 5 == 0) s += frame(10, bytes(rng() % 126));
		}
	}
	return s;
}

} // namespace

int main(int argc, char **argv)
{
	int runs = 3000;
	uint32_t seed = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = uint32_t(atoi(argv[++i]));
	}
	rng.seed(seed);

	// ---- valid streams, cut into pieces of every size: every message whole and the same, every ping handed back ----
	int good = 0;
	for (int r = 0; r < runs / 10; r++) {
		std::vector<std::pair<std::string, bool>> want;
		std::vector<std::string> pings;
		const std::string s = valid(want, pings, 1 + int(rng() % 8));
		const Fed f = feed(s);
		const bool same = f.end == FrameReader::Got::more && f.msgs == want && f.pings == pings;
		if (!same) { printf("  FAIL a valid stream (run %d): %zu of %zu messages, %zu of %zu pings, %s\n", r, f.msgs.size(), want.size(), f.pings.size(), pings.size(), f.why.c_str()); fails++; }
		good += same;
	}
	printf("valid streams: %d of %d read whole and the same (pieces of 1 byte to 70 KB; lengths at 0, 125, 126, 127, 65535, 65536; fragments; pings between)\n", good, runs / 10);

	// ---- what a client must refuse: the connection ends, said why ----
	struct Bad { const char *what; std::string s; };
	const std::string m = bytes(10);
	const std::vector<Bad> bad = {
		{ "a frame masked by the server", frame(2, m, true, 0, true) },
		{ "a reserved bit (RSV1)", frame(2, m, true, 0, false, 0x40) },
		{ "a reserved bit (RSV3)", frame(1, m, true, 0, false, 0x10) },
		{ "a reserved opcode (3)", frame(3, m) },
		{ "a reserved opcode (11)", frame(11, m) },
		{ "a control frame fragmented (a ping without FIN)", frame(9, m, false) },
		{ "a control frame over 125 (a ping of 126)", frame(9, bytes(126)) },
		{ "a close over 125", frame(8, bytes(200)) },
		{ "a continuation with nothing begun", frame(0, m) },
		{ "a new message in the middle of one", frame(1, m, false) + frame(2, m) },
		{ "a close with no payload", frame(8, "") },
		{ "a close with one byte", frame(8, "x") },
		{ "a close with a code and junk", frame(8, std::string("\x03\xe8", 2) + bytes(50)) },
		{ "a close in the middle of a message", frame(2, m, false) + frame(8, std::string("\x03\xe8", 2)) },
		{ "a length past the limit (16-bit form)", frame(2, bytes(70000)) },
		{ "a 64-bit length with its top bit set", std::string("\x82\x7f\x80\x00\x00\x00\x00\x00\x00\x01", 10) },
		{ "a 64-bit length of all ones", std::string("\x82\x7f\xff\xff\xff\xff\xff\xff\xff\xff", 10) },
		{ "fragments joined past the limit", frame(2, bytes(40000), false) + frame(0, bytes(40000), false) + frame(0, bytes(40000)) },
	};
	for (const Bad &b : bad) {
		const Fed f = feed(b.s, 65536);   // (a limit of 64 KB, so the too-long cases are short to build)
		char what[200];
		snprintf(what, sizeof what, "refused: %s (%s)", b.what, f.why.c_str());
		check(f.end == FrameReader::Got::closed, what);
		if (f.end == FrameReader::Got::closed) printf("  ok   %s\n", what);
	}
	// (the edges a client must take: lengths in a longer form than needed, a ping between fragments, an empty message)
	{
		const Fed f = feed(frame(2, bytes(5), true, 3) + frame(1, bytes(100), true, 2) + frame(2, "", true) + frame(1, "ab", false) + frame(9, "p") + frame(0, "cd"));
		check(f.end == FrameReader::Got::more && f.msgs.size() == 4 && f.msgs[3].first == "abcd" && f.pings.size() == 1, "taken: lengths in a longer form, an empty message, a ping between fragments");
		printf("  ok   taken: lengths in a longer form than needed, an empty message, a ping between a message's fragments\n");
	}

	// ---- a server that never stops: read as the Socket does (no more than room()), the buffer bounded, never stuck ----
	{
		struct Endless { const char *what; std::string head; std::string (*more)(); bool must_end; };
		const std::vector<Endless> endless = {
			{ "a frame longer than the limit, its bytes without end", std::string("\x82\x7f\x00\x00\x00\x00\x00\x10\x00\x01", 10), [] { return bytes(30000); }, true },
			{ "a message fragmented without end", frame(2, bytes(1000), false), [] { return frame(0, bytes(1 + rng() % 30000), false); }, true },
			{ "a message of the limit's length, then junk without end", std::string("\x82\x7f\x00\x00\x00\x00\x00\x10\x00\x00", 10), [] { return bytes(1 + rng() % 70000); }, true },
			{ "pings without end", "", [] { return frame(9, bytes(rng() % 126)); }, false },
			{ "small messages without end", "", [] { return frame(2, bytes(rng() % 3000)); }, false },
		};
		for (const Endless &e : endless) {
			FrameReader r;
			std::string buf, pending = e.head;
			size_t most = 0, taken = 0;
			bool stuck = false;
			FrameReader::Got g = FrameReader::Got::more;
			for (int pass = 0; pass < 20000 && g == FrameReader::Got::more; pass++) {
				while (pending.size() < 70000) pending += e.more();
				const size_t room = r.room(buf), n = std::min(room, size_t(1 + rng() % 70000));
				buf.append(pending, 0, n);
				pending.erase(0, n);
				most = std::max(most, buf.size());
				g = r.take(buf, [&](std::string &&, bool) { taken++; }, [&](const std::string &) { taken++; });
				if (g == FrameReader::Got::more && r.room(buf) == 0) { stuck = true; break; }   // (full, and nothing taken: it would wait for ever)
			}
			char what[240];
			snprintf(what, sizeof what, "%s: %s, the buffer at most %zu bytes (the limit %zu), %zu taken%s", e.what, g == FrameReader::Got::closed ? ("ended, " + r.why).c_str() : "read on",
			         most, r.max, taken, stuck ? ", STUCK full" : "");
			const bool ok = !stuck && most <= r.max + 14 && (g == FrameReader::Got::closed) == e.must_end && (e.must_end || taken > 0);
			check(ok, what);
			if (ok) printf("  ok   %s\n", what);
		}
		// (a message of the limit's length exactly, with the longest head: taken whole through room())
		{
			FrameReader r;
			const std::string s = frame(2, bytes(r.max), true, 3) + frame(1, "next");
			std::string buf;
			size_t at = 0, got = 0;
			FrameReader::Got g = FrameReader::Got::more;
			while (at < s.size() && g == FrameReader::Got::more) {
				const size_t n = std::min(r.room(buf), s.size() - at);
				if (n == 0) break;
				buf.append(s, at, n);
				at += n;
				g = r.take(buf, [&](std::string &&m, bool) { got += m.size() == r.max || m == "next"; }, [](const std::string &) {});
			}
			check(g == FrameReader::Got::more && got == 2, "a message of the limit's length, its head 10 bytes, taken whole through room()");
			if (g == FrameReader::Got::more && got == 2) printf("  ok   a message of the limit's length exactly (%zu bytes, its head 10), taken whole reading no more than room()\n", r.max);
		}
		// (a megabyte of the tiniest frames in the buffer at once: taken in one go, quickly)
		{
			std::string buf;
			for (size_t i = 0; buf.size() + 3 <= FrameReader::MOST; i++) buf += i % 2 ? std::string("\x8a\x00", 2) : std::string("\x82\x01x", 3);
			const size_t frames = buf.size() / 2;
			FrameReader r;
			size_t got = 0;
			const auto t0 = std::chrono::steady_clock::now();
			const FrameReader::Got g = r.take(buf, [&](std::string &&, bool) { got++; }, [](const std::string &) {});
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			char what[200];
			snprintf(what, sizeof what, "a megabyte of tiny frames (some %zu) taken in one go: %zu messages in %.0f ms, nothing left", frames, got, ms);
			const bool ok = g == FrameReader::Got::more && buf.empty() && got > 100000 && ms < 3000;
			check(ok, what);
			if (ok) printf("  ok   %s\n", what);
		}
	}

	// ---- anything at all, and valid streams spoilt: never a crash, never past the limit, the buffer bounded ----
	uint64_t frames_in = 0, closed = 0;
	size_t most = 0;
	for (int r = 0; r < runs; r++) {
		std::string s;
		const int kind = int(rng() % 4);
		if (kind == 0) s = bytes(rng() % 5000);
		else {
			std::vector<std::pair<std::string, bool>> w;
			std::vector<std::string> p;
			s = valid(w, p, 1 + int(rng() % 4));
			const int spoil = 1 + int(rng() % 8);
			for (int k = 0; k < spoil && !s.empty(); k++) {
				const int how = int(rng() % 4);
				const size_t at = rng() % s.size();
				if (how == 0) s[at] = char(s[at] ^ (1 << (rng() % 8)));
				else if (how == 1) s.resize(at);
				else if (how == 2) s.insert(at, bytes(1 + rng() % 16));
				else s[at] = char(rng() % 2 ? 0x7f : rng() % 2 ? 0x7e : 0xff);   // (lengths' edges)
			}
		}
		const Fed f = feed(s, 1u << 20);
		frames_in += f.msgs.size();
		closed += f.end == FrameReader::Got::closed;
		most = std::max(most, f.most_buf);
		for (const auto &x : f.msgs) if (x.first.size() > (1u << 20)) { fails++; printf("  FAIL a message past the limit\n"); }
		if (f.most_buf > s.size()) { fails++; printf("  FAIL the buffer grew past what was sent\n"); }
	}
	printf("anything at all: %d streams (random bytes, valid ones spoilt), %llu messages taken, %llu connections ended, the buffer at most %zu bytes; nothing out of bounds\n",
	       runs, (unsigned long long)frames_in, (unsigned long long)closed, most);
	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
