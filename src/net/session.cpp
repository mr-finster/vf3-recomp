#include "session.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <random>

namespace vfnet {

uint64_t hash64(const uint8_t *p, size_t n)
{
	// four lanes a word in four, so the multiplies don't wait on each other; little-endian words on every platform
	constexpr uint64_t K = 0xFF51AFD7ED558CCDull;
	uint64_t h[4] = { 0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
	auto le64 = [](const uint8_t *q) { uint64_t w = 0; for (int i = 7; i >= 0; i--) w = w << 8 | q[i]; return w; };
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

constexpr uint32_t MAGIC = 0x504E4656;   // "VFNP"
constexpr size_t CHUNK = 1024;            // the starting blob's pieces
constexpr uint32_t CHUNK_WINDOW = 32;     // sent ahead of the last acknowledged
constexpr uint32_t MAX_INPUTS = 240;      // in one packet
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

std::string clean_name(std::string s)
{
	if (s.size() > size_t(MAX_NAME)) s.resize(size_t(MAX_NAME));
	return s;
}

} // namespace

Session::Session(Link &link, Game &game, const Identity &me_, const Config &cfg_, bool host, std::vector<uint8_t> blob)
	: link(link), game(game), me(me_), cfg(cfg_), host(host)
{
	me.name = clean_name(me.name);
	cfg.delay = std::clamp(cfg.delay, 0, 10);
	cfg.max_rollback = std::clamp(cfg.max_rollback, 1, 30);
	cfg.checksum_every = std::max(cfg.checksum_every, 1);
	ring.resize(size_t(cfg.max_rollback) + 2);
	if (!host) {   // (the guest's token, at random: the host takes it from the hello)
		std::random_device rd;
		token = (uint64_t(rd()) << 32 ^ rd()) ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
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

void Session::send_blob()
{
	Out w(WELCOME, token);
	w.str(me.name);
	w.u32(blob_size);
	w.u32(chunk_count);
	send_packet(w.b);
	for (uint32_t i = chunks_acked; i < std::min(chunk_count, chunks_acked + CHUNK_WINDOW); i++) {
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
	o.u8(uint8_t(int8_t(std::clamp(adv_mine.empty() ? 0 : adv_mine.back(), -100, 100))));
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

void Session::poll(double t)
{
	now = t;
	if (started == 0) started = last_heard = t ? t : 1e-9;
	std::vector<uint8_t> p;
	while (st != State::ended && link.recv(p)) {
		In in(p);
		if (in.u32() != MAGIC || !in.ok) continue;
		last_heard = now;
		if (quiet) { quiet = false; events.push_back({ Event::resumed, "" }); }
		receive(p);
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
				else if (began) { send_packet(Out(READY, token).b); last_hello = now; }
				else { Out a(BLOB_ACK, token); a.u32(chunks_acked); send_packet(a.b); last_hello = now; }
			} else if (!their_name.empty() && !began)
				send_blob();
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

void Session::receive(const std::vector<uint8_t> &p)
{
	In in(p);
	in.u32();
	const uint8_t type = in.u8();
	const uint64_t t_ = in.u64();
	if (!in.ok) return;
	if (have_token ? t_ != token : type != HELLO) return;   // (not this session's)
	switch (type) {
	case HELLO: {
		if (!host || st != State::handshake) return;
		const uint16_t proto = in.u16();
		const std::string game_ = in.str(), build = in.str();
		const uint64_t content = in.u64();
		const std::string name = clean_name(in.str());
		in.u8();
		if (!in.ok) return;
		std::string mine, theirs;
		if (proto != PROTOCOL || build != me.build)
			mine = "Your opponent has " + build + ", you have " + me.build + ": you both need the same version.",
			theirs = "The host has " + me.build + ", you have " + build + ": you both need the same version.";
		else if (game_ != me.game)
			mine = theirs = "Your opponent is playing another game.";
		else if (content != me.content)
			mine = theirs = "Your game files (the ROM) aren't the same as your opponent's.";
		if (!mine.empty()) {
			Out r(REJECT, t_);
			r.str(theirs);
			for (int i = 0; i < 3; i++) send_packet(r.b);
			finish(End::rejected, mine);
			return;
		}
		token = t_;   // (from now on only this guest's packets)
		have_token = true;
		if (their_name.empty()) started = now;   // (the handshake's time from the first hello)
		if (their_name.empty() && trace) trace("a player's hello: " + (name.empty() ? std::string("PLAYER 2") : name) + "; sending the settings");
		their_name = name.empty() ? "PLAYER 2" : name;
		send_blob();
		return;
	}
	case WELCOME: {
		if (host || st != State::handshake || chunk_count) return;
		const std::string name = clean_name(in.str());
		const uint32_t size = in.u32(), count = in.u32();
		if (!in.ok || count != (size + CHUNK - 1) / CHUNK || size > cfg.max_blob) return;
		their_name = name.empty() ? "PLAYER 1" : name;
		if (trace) trace("the host answered (" + their_name + "): its settings, " + std::to_string(size) + " bytes");
		blob_size = size;
		chunk_count = count;
		start_blob.assign(size, 0);
		chunk_have.assign(count, false);
		if (count == 0) {
			began = true;
			if (!game.begin(start_blob)) { finish(End::rejected, "The game couldn't start from the host's settings."); return; }
			send_packet(Out(READY, token).b);
		}
		return;
	}
	case REJECT: {
		const std::string text = in.str();
		if (!host && in.ok) finish(End::rejected, text);
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
		send_packet(a.b);
		if (chunks_acked == chunk_count) {
			if (trace) trace("the host's settings all here: starting the game");
			began = true;
			if (!game.begin(start_blob)) { finish(End::rejected, "The game couldn't start from the host's settings."); return; }
			send_packet(Out(READY, token).b);
		}
		return;
	}
	case BLOB_ACK: {
		if (!host || st != State::handshake) return;
		const uint32_t n = in.u32();
		if (in.ok && n > chunks_acked && n <= chunk_count) {
			chunks_acked = n;
			send_blob();   // (the window moves on)
		}
		return;
	}
	case READY: {
		if (!host || their_name.empty()) return;
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
			if (rtt >= 0 && rtt < 5000) stats_.rtt_ms = stats_.rtt_ms ? stats_.rtt_ms * 0.9 + rtt * 0.1 : rtt;
		}
		if (hash_frame != UINT32_MAX) {
			their_hash[hash_frame] = hash;
			while (their_hash.size() > 64) their_hash.erase(their_hash.begin());
		}
		got_inputs(first, v.data(), n);
		check_hashes();
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

void Session::check_hashes()
{
	const uint32_t R = uint32_t(ring.size());
	while (next_check <= remote.size() && next_check < cur && rollback_from >= next_check) {
		if (cur - next_check < R) {
			my_hash[next_check] = game.hash(ring[next_check % R]);
			while (my_hash.size() > 64) my_hash.erase(my_hash.begin());
		}
		next_check += uint32_t(cfg.checksum_every);
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
	while (local.size() <= size_t(cur) + size_t(cfg.delay)) local.push_back(input);   // (once a frame; more if the delay grew)
	send_inputs();

	// the other side's inputs that came since: back to the first wrong guess, and every frame since again
	const uint32_t R = uint32_t(ring.size());
	if (rollback_from < cur) {
		const uint32_t from = rollback_from;
		rollback_from = UINT32_MAX;
		if (cur - from >= R || !game.load(ring[from % R])) {
			finish(End::desync, "The game couldn't go back to a frame (rollback).");
			return false;
		}
		for (uint32_t g = from; g < cur; g++) {
			if (g != from) game.save(ring[g % R]);
			run_frame(g, true);
		}
		const int n = int(cur - from);
		stats_.rollback = n;
		stats_.rollbacks++;
		stats_.resim_frames += uint64_t(n);
		recent_rollbacks.push_back({ now, n });
	}
	while (!recent_rollbacks.empty() && now - recent_rollbacks.front().first > 1000) recent_rollbacks.pop_front();
	stats_.rollback_max = 0;
	for (auto &r : recent_rollbacks) stats_.rollback_max = std::max(stats_.rollback_max, r.second);
	check_hashes();
	if (st != State::running) return false;

	// too far ahead of what's come: wait
	if (cur >= remote.size() + uint32_t(cfg.max_rollback)) {
		stats_.stalls++;
		return false;
	}

	// time sync: how far ahead of the other side this one is (its frame now, from what it last said and the time
	// since, and half the round trip), against how far ahead it says it is; the one ahead waits a frame
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
	if (wait_cool > 0) wait_cool--;
	else if (lead >= 1.0 && have_their_t) {
		wait_cool = 20;
		stats_.waits++;
		return false;
	}

	game.save(ring[cur % R]);
	run_frame(cur, false);
	cur++;
	return true;
}

void Session::quit()
{
	if (st == State::ended) return;
	for (int i = 0; i < 3; i++) send_packet(Out(QUIT, token).b);
	finish(End::quit, "You left the match.");
}

} // namespace vfnet
