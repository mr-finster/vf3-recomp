#include "session.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <random>

namespace vfnet {

uint64_t hash64(const uint8_t *p, size_t n)
{
	// four lanes a word in four, so the multiplies don't wait on each other; little-endian words on every platform
	constexpr uint64_t K = 0xFF51AFD7ED558CCDull;
	uint64_t h[4] = { 0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
	// (a word loaded as it is, little-endian made so on a big-endian machine: the byte-at-a-time loop this was made
	// the 27 MB state's hash ~30 ms, a hitch every checkpoint on a Steam Deck; the same value, ~4x faster)
	auto le64 = [](const uint8_t *q) {
		uint64_t w;
		memcpy(&w, q, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		w = __builtin_bswap64(w);
#endif
		return w;
	};
	size_t i = 0;
	for (; i + 32 <= n; i += 32)
		for (int l = 0; l < 4; l++) {
			h[l] = (h[l] ^ le64(p + i + 8 * l)) * K;
			h[l] ^= h[l] >> 29;
		}
	uint64_t x = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
	for (; i < n; i++)
		x = (x ^ p[i]) * 0x100000001B3ull;
	x ^= x >> 33;
	x *= 0xC4CEB9FE1A85EC53ull;
	return x ^ (x >> 33);
}

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

constexpr uint32_t MAGIC = 0x504E4656;   // "VFNP"
constexpr size_t CHUNK = 1024;            // the starting blob's pieces
constexpr uint32_t CHUNK_WINDOW = 32;     // sent ahead of the last acknowledged
constexpr uint32_t MAX_INPUTS = 240;      // in one packet
constexpr int MAX_PACKETS = 64;           // read in one poll (a flood can't hold the frame up)
constexpr int MAX_WELCOMES = 10;          // a host's welcome to a hello that's never answered, at most
// why a host refused (the guest says it in its own words: no text from the other side is shown as it came)
enum : uint8_t { R_VERSION = 1, R_GAME, R_ROM };
enum : uint8_t { HELLO = 1, WELCOME, REJECT, BLOB, BLOB_ACK, READY, GO, INPUT, QUIT };

// The packets' fields, little-endian
struct Out
{
	std::vector<uint8_t> b;
	Out(uint8_t type, uint64_t token) { u32(MAGIC); u8(type); u64(token); }
	void u8(uint8_t v) { b.push_back(v); }
	void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
	void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
	void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
	void str(const std::string &s) { const size_t n = std::min<size_t>(s.size(), 255); u8(uint8_t(n)); b.insert(b.end(), s.begin(), s.begin() + long(n)); }
};

struct In
{
	const uint8_t *p, *e;
	bool ok = true;
	In(const std::vector<uint8_t> &v) : p(v.data()), e(v.data() + v.size()) {}
	bool need(size_t n) { if (!ok || size_t(e - p) < n) ok = false; return ok; }
	uint8_t u8() { return need(1) ? *p++ : 0; }
	uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | u8() << 8); }
	uint32_t u32() { const uint32_t a = u16(); return a | uint32_t(u16()) << 16; }
	uint64_t u64() { const uint64_t a = u32(); return a | uint64_t(u32()) << 32; }
	std::string str() { const size_t n = u8(); if (!need(n)) return {}; std::string s(reinterpret_cast<const char *>(p), n); p += n; return s; }
};

// what the other side says of itself, shown to the player: printable ASCII only, so short
std::string printable(const std::string &s, size_t max)
{
	std::string out;
	for (char c : s)
		if (c >= 0x20 && c < 0x7F && out.size() < max) out += c;
	return out;
}

std::string clean_name(const std::string &s) { return printable(s, size_t(MAX_NAME)); }
std::string clean_build(const std::string &s)
{
	std::string out;
	for (char c : printable(s, 40))
		if (isalnum(uint8_t(c)) || c == '.' || c == '-' || c == '+' || c == '_') out += c;
	return out.empty() ? std::string("another version") : out;
}

uint64_t random64()
{
	std::random_device rd;
	return (uint64_t(rd()) << 32 ^ rd()) ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
}

} // namespace

Session::Session(Link &link, Game &game, const Identity &me_, const Config &cfg_, bool host, std::vector<uint8_t> blob)
	: link(link), game(game), me(me_), cfg(cfg_), host(host)
{
	me.name = clean_name(me.name);
	cfg.delay = std::clamp(cfg.delay, 0, MAX_DELAY);
	cfg.max_rollback = std::clamp(cfg.max_rollback, 1, 30);
	cfg.checksum_every = std::max(cfg.checksum_every, 1);
	ring.resize(size_t(cfg.max_rollback) + 2);
	ring_frame.assign(ring.size(), UINT32_MAX);
	if (!host) {   // (the guest's token, at random: the host takes it from the hello)
		token = cfg.token ? cfg.token : random64();
		have_token = true;
	}
	if (host) {
		start_blob = std::move(blob);
		blob_size = uint32_t(start_blob.size());
		chunk_count = uint32_t((start_blob.size() + CHUNK - 1) / CHUNK);
	}
}

void Session::finish(End e, const std::string &text)
{
	if (st == State::ended) return;
	if (trace) trace("ended: " + text);
	st = State::ended;
	why = e;
	events.push_back({ Event::ended, text });
}

bool Session::next_event(Event &e)
{
	if (events.empty()) return false;
	e = events.front();
	events.pop_front();
	return true;
}

uint32_t Session::confirmed() const { return std::min<uint32_t>(cur, uint32_t(remote.size())); }

void Session::send_packet(const std::vector<uint8_t> &p)
{
	link.send(p.data(), p.size());
	last_sent = now;
}

void Session::send_hello()
{
	Out o(HELLO, token);
	o.u16(PROTOCOL);
	o.str(me.game);
	o.str(me.build);
	o.u64(me.content);
	o.str(me.name);
	o.u8(uint8_t(cfg.delay));
	send_packet(o.b);
	last_hello = now;
}

void Session::send_welcome()
{
	Out w(WELCOME, token);
	w.str(me.name);
	w.u32(blob_size);
	w.u32(chunk_count);
	w.u64(nonce);
	send_packet(w.b);
	welcomes++;
	last_hello = now;
}

void Session::send_blob()
{
	if (!proven) return;
	send_chunks(chunks_acked, chunks_acked + CHUNK_WINDOW);
}

void Session::send_chunks(uint32_t from_chunk, uint32_t to_chunk)
{
	for (uint32_t i = from_chunk; i < std::min(chunk_count, to_chunk); i++) {
		Out o(BLOB, token);
		o.u32(i);
		const size_t from = size_t(i) * CHUNK, n = std::min(CHUNK, start_blob.size() - from);
		o.u16(uint16_t(n));
		o.b.insert(o.b.end(), start_blob.begin() + long(from), start_blob.begin() + long(from + n));
		send_packet(o.b);
	}
	last_hello = now;
}

void Session::send_inputs()
{
	Out o(INPUT, token);
	o.u32(uint32_t(remote.size()));   // (their inputs this side has)
	o.u32(cur);
	o.u8(uint8_t(int8_t(have_their_t && !adv_mine.empty() ? std::clamp(adv_mine.back(), -100, 100) : 0)));   // (0 till it's heard: no lead to tell)
	o.u8(my_flags);
	o.u32(uint32_t(int64_t(now)));
	o.u32(have_their_t ? their_t : 0);
	o.u16(uint16_t(std::clamp(now - their_t_at, 0.0, 60000.0)));
	if (my_hash.empty()) { o.u32(UINT32_MAX); o.u64(0); }
	else { o.u32(my_hash.rbegin()->first); o.u64(my_hash.rbegin()->second); }
	const uint32_t first = std::min<uint32_t>(their_ack, uint32_t(local.size()));
	const uint32_t n = std::min<uint32_t>(uint32_t(local.size()) - first, MAX_INPUTS);
	o.u32(first);
	o.u8(uint8_t(n));
	for (uint32_t i = 0; i < n; i++) o.u16(local[first + i]);
	send_packet(o.b);
}

void Session::start_running()
{
	if (st != State::handshake) return;
	if (trace) trace("the match begins (input delay " + std::to_string(cfg.delay) + ")");
	st = State::running;
	local.assign(size_t(cfg.delay), 0);   // (the first delay frames: nothing pressed)
	// the rollback's buffers made now, not in the match's first frames (a state is tens of MB: making them as the
	// frames first ran held the first frames up, 0.2 s)
	for (auto &r : ring) game.save(r);
	events.push_back({ Event::connected, their_name });
}

void Session::guest_failed()
{
	for (int i = 0; i < 3; i++) send_packet(Out(QUIT, token).b);   // (the host told at once, not left waiting out its time)
	finish(End::rejected, "The game couldn't start from the host's settings.");
}

int Session::quiet_left_s() const
{
	if (!quiet) return 0;
	return std::max(0, int(std::ceil((cfg.disconnect_ms - (now - last_heard)) / 1000.0)));
}

void Session::poll(double t)
{
	now = t;
	if (started == 0) started = last_heard = t ? t : 1e-9;
	if (restart_clock) { restart_clock = false; started = last_heard = t; }
	std::vector<uint8_t> p;
	for (int k = 0; k < MAX_PACKETS && st != State::ended && link.recv(p); k++) {
		In in(p);
		if (in.u32() != MAGIC || !in.ok) continue;
		if (!receive(p)) continue;   // (only this session's packets say the other side's there)
		last_heard = now;
		if (quiet) { quiet = false; events.push_back({ Event::resumed, "" }); }
	}
	if (st == State::ended) return;
	if (st == State::handshake) {
		const bool talking = host ? !their_name.empty() || began : true;
		if (talking && now - started > cfg.handshake_ms) {
			finish(End::timeout, host ? "Your opponent couldn't finish connecting." : "Couldn't reach the host: check the address, and that the host's game is waiting.");
			return;
		}
		if (now - last_hello >= 100) {
			if (!host) {
				if (chunk_count == 0 && their_name.empty()) send_hello();
				else if (began) { Out r(READY, token); r.u64(nonce); send_packet(r.b); last_hello = now; }
				else { Out a(BLOB_ACK, token); a.u32(chunks_acked); a.u64(nonce); send_packet(a.b); last_hello = now; }
			} else if (!their_name.empty() && !began) {
				if (proven) { if (now - blob_moved >= std::max(300.0, 2 * stats_.rtt_ms)) { send_blob(); blob_moved = now; } }   // (the window again only once acks stop: not every 100 ms)
				else if (welcomes < MAX_WELCOMES && now - last_hello >= 200) send_welcome();   // (unanswered: soon given up)
			}
		}
		return;
	}
	// running
	if (now - last_heard > cfg.disconnect_ms) {
		finish(End::disconnected, "The connection to your opponent was lost.");
		return;
	}
	if (!quiet && now - last_heard > cfg.interrupt_ms) {
		quiet = true;
		events.push_back({ Event::interrupted, "Connection interrupted" });
	}
	if (now - last_sent >= 50) send_inputs();   // (keeps the other side hearing from us while waiting)
}

bool Session::receive(const std::vector<uint8_t> &p)
{
	In in(p);
	in.u32();
	const uint8_t type = in.u8();
	const uint64_t t_ = in.u64();
	if (!in.ok) return false;
	if (have_token ? t_ != token : type != HELLO) return false;   // (not this session's)
	if (!have_token && cfg.token && t_ != cfg.token) return false;   // (a lobby's room: only the one who joined it)
	handle(p);
	return true;
}

void Session::handle(const std::vector<uint8_t> &p)
{
	In in(p);
	in.u32();
	const uint8_t type = in.u8();
	const uint64_t t_ = in.u64();
	switch (type) {
	case HELLO: {
		if (!host || st != State::handshake) return;
		const uint16_t proto = in.u16();
		const std::string game_ = in.str(), build = in.str();
		const uint64_t content = in.u64();
		const std::string name = clean_name(in.str());
		in.u8();
		if (!in.ok) return;
		std::string mine;
		uint8_t reason = 0;
		if (proto != PROTOCOL || build != me.build)
			mine = "Your opponent has " + clean_build(build) + ", you have " + me.build + ": you both need the same version.", reason = R_VERSION;
		else if (game_ != me.game)
			mine = "Your opponent is playing another game.", reason = R_GAME;
		else if (content != me.content)
			mine = "Your game files (the ROM) aren't the same as your opponent's.", reason = R_ROM;
		if (reason) {
			Out r(REJECT, t_);
			r.u8(reason);
			r.str(me.build);
			for (int i = 0; i < 2; i++) send_packet(r.b);
			finish(End::rejected, mine);
			return;
		}
		if (!have_token) {   // (the first good hello: from now on only this guest's packets, from its address)
			token = t_;
			have_token = true;
			nonce = random64();
			link.lock();
			started = now;   // (the handshake's time from the first hello)
			their_name = name.empty() ? "PLAYER 2" : name;
			if (trace) trace("a player's hello: " + their_name + "; sending the settings");
		}
		// (again for a hello again, its welcome lost; not more often than the resends, nor once it's answered)
		if (!proven && welcomes < MAX_WELCOMES && (welcomes == 0 || now - last_hello >= 100)) send_welcome();
		return;
	}
	case WELCOME: {
		if (host || st != State::handshake || chunk_count || began) return;
		const std::string name = clean_name(in.str());
		const uint32_t size = in.u32(), count = in.u32();
		const uint64_t n_ = in.u64();
		if (!in.ok || size > cfg.max_blob || count != (size_t(size) + CHUNK - 1) / CHUNK) return;
		nonce = n_;
		link.lock();   // (the way the host's welcome came by, with this side's token: only that one from now)
		their_name = name.empty() ? "PLAYER 1" : name;
		if (trace) trace("the host answered (" + their_name + "): its settings, " + std::to_string(size) + " bytes");
		blob_size = size;
		chunk_count = count;
		start_blob.assign(size, 0);
		chunk_have.assign(count, false);
		if (count == 0) {
			began = true;
			if (!game.begin(start_blob)) { guest_failed(); return; }
			restart_clock = true;   // (its own boot took seconds: the handshake's time from now, for the host's)
			Out r(READY, token);
			r.u64(nonce);
			send_packet(r.b);
		} else {   // (at once: the host sends its settings only once it's heard this)
			Out a(BLOB_ACK, token);
			a.u32(0);
			a.u64(nonce);
			send_packet(a.b);
			last_hello = now;
		}
		return;
	}
	case REJECT: {
		if (host || st != State::handshake) return;
		const uint8_t reason = in.u8();
		const std::string build = clean_build(in.str());
		if (!in.ok) return;
		finish(End::rejected, reason == R_VERSION ? "The host has " + build + ", you have " + me.build + ": you both need the same version."
		                      : reason == R_GAME ? "The host is playing another game."
		                      : reason == R_ROM ? "Your game files (the ROM) aren't the same as the host's."
		                                        : "The host couldn't take you.");
		return;
	}
	case BLOB: {
		if (host || st != State::handshake || !chunk_count || began) return;
		const uint32_t i = in.u32();
		const uint16_t n = in.u16();
		if (!in.need(n) || i >= chunk_count || size_t(i) * CHUNK + n > start_blob.size()) return;
		memcpy(&start_blob[size_t(i) * CHUNK], in.p, n);
		chunk_have[i] = true;
		while (chunks_acked < chunk_count && chunk_have[chunks_acked]) chunks_acked++;
		Out a(BLOB_ACK, token);
		a.u32(chunks_acked);
		a.u64(nonce);
		send_packet(a.b);
		if (chunks_acked == chunk_count) {
			if (trace) trace("the host's settings all here: starting the game");
			began = true;
			if (!game.begin(start_blob)) { guest_failed(); return; }
			restart_clock = true;   // (its own boot took seconds: the handshake's time from now, for the host's)
			Out r(READY, token);
			r.u64(nonce);
			send_packet(r.b);
		}
		return;
	}
	case BLOB_ACK: {
		if (!host || st != State::handshake || began) return;
		const uint32_t n = in.u32();
		if (!in.ok || in.u64() != nonce || !in.ok) return;   // (not from where the welcome went)
		const bool first = !proven;
		proven = true;
		if (first) { if (n <= chunk_count) chunks_acked = std::max(chunks_acked, n); send_blob(); blob_moved = now; return; }   // (the first window)
		if (n <= chunks_acked || n > chunk_count) return;
		// the window moved on: only what's come into it (what's lost in it is sent again by poll's timer, not
		// on every ack: the whole window again each ack was the settings sent ~30 times over)
		const uint32_t was = chunks_acked;
		chunks_acked = n;
		blob_moved = now;
		send_chunks(std::max(was + CHUNK_WINDOW, n), n + CHUNK_WINDOW);
		return;
	}
	case READY: {
		if (!host || their_name.empty()) return;
		if (!proven) {   // (an empty blob: the ready is the first answer to the welcome)
			if (in.u64() != nonce || !in.ok) return;
			proven = true;
		}
		// the host's game starts once the guest's has (not at the hello: a guest that never got this far left the
		// host's game set up for a match it had never had, and playing on it alone after)
		if (!began) {
			if (trace) trace("the player's game is ready: starting this one");
			began = true;
			if (!game.begin(start_blob)) { finish(End::rejected, "The game couldn't start."); return; }
		}
		start_running();
		send_packet(Out(GO, token).b);   // (again for each READY: a lost GO costs a resend)
		return;
	}
	case GO:
		if (!host && began) start_running();
		return;
	case INPUT: {
		if (!host && began && st == State::handshake) start_running();   // (the GO was lost, the inputs weren't)
		if (st != State::running) return;
		const uint32_t ack = in.u32(), frame = in.u32();
		const int adv = int8_t(in.u8());
		const uint8_t flags = in.u8();
		const uint32_t t = in.u32(), echo = in.u32();
		const uint16_t echo_age = in.u16();
		const uint32_t hash_frame = in.u32();
		const uint64_t hash = in.u64();
		const uint32_t first = in.u32();
		const uint32_t n = in.u8();
		if (!in.need(size_t(n) * 2)) return;
		std::vector<uint16_t> v(n);
		for (uint32_t i = 0; i < n; i++) v[i] = in.u16();
		their_ack = std::max(their_ack, std::min<uint32_t>(ack, uint32_t(local.size())));
		if (frame >= their_frame) { their_frame = frame; their_frame_at = now; their_adv = adv; }
		their_flags = flags;
		their_t = t;
		their_t_at = now;
		have_their_t = true;
		if (echo) {
			const double rtt = double(uint32_t(int64_t(now)) - echo) - echo_age;
			if (rtt >= 0 && rtt < 5000) {
				if (stats_.rtt_ms) stats_.jitter_ms = stats_.jitter_ms * 0.9 + std::fabs(rtt - stats_.rtt_ms) * 0.1;
				stats_.rtt_ms = stats_.rtt_ms ? stats_.rtt_ms * 0.9 + rtt * 0.1 : rtt;
			}
		}
		if (hash_frame != UINT32_MAX) {
			their_hash[hash_frame] = hash;
			while (their_hash.size() > 64) their_hash.erase(their_hash.begin());
		}
		got_inputs(first, v.data(), n);
		check_hashes(false);
		return;
	}
	case QUIT:
		finish(End::opponent_quit, "Your opponent left the match.");
		return;
	default:
		return;
	}
}

void Session::got_inputs(uint32_t first, const uint16_t *in, uint32_t n)
{
	// (an honest peer is never further ahead than its rollback and its delay allow: it waits; inputs past that are
	// dropped, so a bad one can't make this side hold more and more. An honest one sends them again later.)
	const uint64_t limit = uint64_t(cur) + uint64_t(cfg.max_rollback) + 10 + 16;
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t f = first + i;
		if (f >= limit) break;
		if (f < remote.size()) continue;
		if (f > remote.size()) break;   // (a gap: later packets fill it)
		remote.push_back(in[i]);
		if (f < cur && guessed[f] != in[i]) rollback_from = std::min(rollback_from, f);
	}
}

uint16_t Session::remote_input(uint32_t f) const
{
	if (f < remote.size()) return remote[f];
	return remote.empty() ? 0 : remote.back();
}

void Session::run_frame(uint32_t f, bool resim)
{
	uint16_t in[2];
	in[local_player()] = local[f];
	in[local_player() ^ 1] = remote_input(f);
	if (guessed.size() <= f) guessed.resize(size_t(f) + 1);
	guessed[f] = in[local_player() ^ 1];
	game.advance(in, resim);
}

void Session::check_hashes(bool work)
{
	const uint32_t R = uint32_t(ring.size());
	// (a confirmed frame's state, hashed a part each step: it stays in the ring R frames, and a rollback never saves
	// it again, a confirmed frame being before any wrong guess)
	// (the ring's slot for frame f is saved over by frame f + R: at this check, before this step's save, f's is there
	// while cur - f <= R; a resim before it saves only frames after f and before cur. A frame confirmed only when
	// it's nearly gone from the ring, as at a high ping or a small input delay, is hashed whole at once: hashing it a
	// part a step there, every checkpoint ran out of the ring unchecked and a desync went on unseen.)
	if (work && !hashing.on) {
		while (next_check <= remote.size() && next_check < cur && rollback_from >= next_check && cur - next_check > R)
			next_check += uint32_t(cfg.checksum_every);   // (gone from the ring meanwhile: that one's not checked)
		if (next_check <= remote.size() && next_check < cur && rollback_from >= next_check) {
			hashing.on = true;
			hashing.frame = next_check;
			hashing.part = 0;
			hashing.h = 0x9E3779B97F4A7C15ull;
			next_check += uint32_t(cfg.checksum_every);
		}
	} else if (work && hashing.on && cur - hashing.frame > R)
		hashing.on = false;   // (it stalled out of the ring: not checked)
	// (the parts left spread over the steps the slot has left: not three at once on the last one, a hitch)
	int budget = 1;
	if (work && hashing.on) {
		const int steps_left = std::max(1, int(R) - 1 - int(cur - hashing.frame));
		budget = (HASH_PARTS - hashing.part + steps_left - 1) / steps_left;
	}
	while (work && hashing.on && budget-- > 0) {
		const auto t0 = Clock::now();
		if (ring_frame[hashing.frame % R] != hashing.frame) { hashing.on = false; break; }   // (not its state there: not checked)
		const std::vector<uint8_t> &st = ring[hashing.frame % R];
		const size_t skip = std::min(st.size(), game.hash_skip()), n = st.size() - skip;
		const size_t a = skip + n * size_t(hashing.part) / HASH_PARTS, b = skip + n * size_t(hashing.part + 1) / HASH_PARTS;
		hashing.h = (hashing.h ^ hash64(st.data() + a, b - a)) * 0x100000001B3ull;
		stats_.hash_ms += ms_since(t0);
		if (++hashing.part == HASH_PARTS) {
			hashing.on = false;
			my_hash[hashing.frame] = hashing.h;
			while (my_hash.size() > 64) my_hash.erase(my_hash.begin());
		}
	}
	for (auto it = their_hash.begin(); it != their_hash.end();) {
		auto m = my_hash.find(it->first);
		if (m == my_hash.end()) { ++it; continue; }
		if (m->second != it->second) {
			finish(End::desync, "The match went out of step (a desync) at frame " + std::to_string(it->first) + ".");
			return;
		}
		it = their_hash.erase(it);
	}
}

bool Session::step(uint16_t input)
{
	if (st != State::running) return false;
	const auto t_step = Clock::now();
	while (local.size() <= size_t(cur) + size_t(cfg.delay)) local.push_back(input);   // (once a frame; more if the delay grew)
	send_inputs();

	// the other side's inputs that came since: back to the first wrong guess, and every frame since again
	const uint32_t R = uint32_t(ring.size());
	if (rollback_from < cur) {
		const uint32_t from = rollback_from;
		rollback_from = UINT32_MAX;
		if (cur - from >= R || ring_frame[from % R] != from || !game.load(ring[from % R])) {
			finish(End::desync, "The game couldn't go back to a frame (rollback).");
			return false;
		}
		const auto t0 = Clock::now();
		for (uint32_t g = from; g < cur; g++) {
			// (a frame whose inputs are all here now is never gone back to again, a rollback starting at the first
			// not yet here: its state's kept only if the desync check wants it; a save each is a ms or two on a Deck)
			const bool wanted = g >= remote.size() || g == next_check || (hashing.on && g == hashing.frame);
			if (g != from && wanted) { game.save(ring[g % R]); ring_frame[g % R] = g; }
			else if (g != from) ring_frame[g % R] = UINT32_MAX;   // (what's there is another frame's now)
			run_frame(g, true);
		}
		stats_.resim_ms += ms_since(t0);
		const int n = int(cur - from);
		stats_.rollback = n;
		stats_.rollbacks++;
		stats_.resim_frames += uint64_t(n);
		recent_rollbacks.push_back({ now, n });
	}
	while (!recent_rollbacks.empty() && now - recent_rollbacks.front().first > 1000) recent_rollbacks.pop_front();
	stats_.rollback_max = 0;
	for (auto &r : recent_rollbacks) stats_.rollback_max = std::max(stats_.rollback_max, r.second);
	check_hashes(true);
	if (st != State::running) return false;

	// too far ahead of what's come: wait
	if (cur >= remote.size() + uint32_t(cfg.max_rollback)) {
		stats_.stalls++;
		return false;
	}

	// time sync: how far ahead of the other side this one is (its frame now, from what it last said and the time
	// since, and half the round trip), against how far ahead it says it is; the one ahead waits a frame. Only once
	// it's heard from the other side: before, "their frame" is 0 and this side seemed thousands of frames ahead,
	// which held the start of every match up, and slowed it after.
	if (have_their_t) {
		const double theirs_now = double(their_frame) + (now - their_frame_at + stats_.rtt_ms / 2) / cfg.frame_ms;
		const int ahead = int(std::lround(std::clamp(double(cur) - theirs_now, -120.0, 120.0)));   // (whatever the other side says)
		adv_mine.push_back(ahead);
		adv_theirs.push_back(their_adv);
		while (adv_mine.size() > 60) adv_mine.pop_front();
		while (adv_theirs.size() > 60) adv_theirs.pop_front();
		double am = 0, at = 0;
		for (int a : adv_mine) am += a;
		for (int a : adv_theirs) at += a;
		am /= double(adv_mine.size());
		at /= double(adv_theirs.size());
		const double lead = (am - at) / 2;   // (half the difference: the other side catches up the other half)
		stats_.ahead = int(std::lround(lead));
		stats_.lead = lead;
		if (wait_cool > 0) wait_cool--;
		else if (lead >= (cfg.soft_sync ? cfg.hard_wait_lead : 1.0)) {
			wait_cool = 20;
			stats_.waits++;
			return false;
		}
	}

	const auto t0 = Clock::now();
	game.save(ring[cur % R]);
	ring_frame[cur % R] = cur;
	stats_.save_ms += ms_since(t0);
	const auto t1 = Clock::now();
	run_frame(cur, false);
	stats_.run_ms += ms_since(t1);
	stats_.steps++;
	stats_.step_max_ms = std::max(stats_.step_max_ms, ms_since(t_step));
	cur++;
	return true;
}

void Session::quit()
{
	if (st == State::ended) return;
	for (int i = 0; i < 3; i++) send_packet(Out(QUIT, token).b);
	finish(End::quit, "You left the match.");
}

int auto_delay(double rtt_ms, double jitter_ms, double frame_ms, int target_rollback, int floor)
{
	const int feel = std::clamp(int(std::lround(rtt_ms / 2 / frame_ms)), 1, 3);
	const int cover = int(std::ceil((rtt_ms / 2 + jitter_ms) / frame_ms)) - target_rollback;
	return std::clamp(std::max({ feel, cover, floor }), 0, MAX_DELAY);
}

int AutoDelay::want(const Session::Stats &s) const
{
	// (a rollback of 6 of the 7 the session can go back: one spare for a late packet; 4 on a slow machine)
	return auto_delay(s.rtt_ms, s.jitter_ms, frame_ms, slow ? 4 : 6, slow ? 2 : 1);
}

int AutoDelay::update(const Session &session, double now, bool between_rounds)
{
	const Session::Stats &s = session.stats();
	const uint32_t f = session.frame();
	if (f <= 120 || s.rtt_ms <= 0) return -1;   // (the ping not known well yet)
	const int cur = session.delay(), keep = now < floor_until ? floor : 0, w = std::max(want(s), keep);
	char b[160];
	if (!started) {   // (the first: at once, the character select's)
		started = true;
		started_at = checked_at = now;
		stalls_at = s.stalls;
		snprintf(b, sizeof b, "ping %.0f ms, jitter %.0f ms%s", s.rtt_ms, s.jitter_ms, slow ? ", a slow frame here" : "");
		why = b;
		return w;
	}
	// up: the ping's wanted more for half a second, or frames held back in the last 3 s
	if (w > cur) {
		if (higher_since < 0) higher_since = now;
		if (now - higher_since >= 500 && w > target) {
			target = w;
			snprintf(b, sizeof b, "ping %.0f ms, jitter %.0f ms", s.rtt_ms, s.jitter_ms);
			why = b;
		}
	} else
		higher_since = -1;
	if (now - checked_at >= 3000) {
		const uint64_t held = s.stalls - stalls_at;
		stalls_at = s.stalls;
		checked_at = now;
		// (not in a match's first 15 s: the two sides settling in, the Start presses, aren't the connection)
		if (now - started_at >= 15000 && held >= 10 && cur < std::min(MAX_DELAY, want(s) + 2) && target <= cur) {
			target = cur + 1;
			floor = target;
			floor_until = now + 30000;
			snprintf(b, sizeof b, "%llu frames held back in 3 s", (unsigned long long)held);
			why = b;
		}
	}
	if (target > cur) {   // (a frame at a time, every 6)
		lower_since = -1;
		if (f - stepped_at >= 6) { stepped_at = f; return cur + 1; }
		return -1;
	}
	target = -1;
	// down: between rounds only, a lower one enough (with a tenth more ping) for 10 s
	Session::Stats margin = s;
	margin.rtt_ms *= 1.1;
	const int lower = std::max(want(margin), keep);
	if (lower < cur) {
		if (lower_since < 0) lower_since = now;
		if (between_rounds && now - lower_since >= 10000) {
			lower_since = -1;
			snprintf(b, sizeof b, "ping %.0f ms, jitter %.0f ms, between rounds", s.rtt_ms, s.jitter_ms);
			why = b;
			return lower;
		}
	} else
		lower_since = -1;
	return -1;
}

} // namespace vfnet
