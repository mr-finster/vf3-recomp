#include "watch.h"
#include "json.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace vfnet::watch {

namespace {

struct Out
{
	std::vector<uint8_t> b;
	void u8(uint8_t v) { b.push_back(v); }
	void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
	void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
	void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
	void bytes(const uint8_t *p, size_t n) { b.insert(b.end(), p, p + n); }
	void head(uint8_t kind, uint64_t match) { u32(MAGIC); u8(kind); u8(1); u64(match); }
	void info(const Info &i) { u8(i.wins[0]); u8(i.wins[1]); u8(i.rounds[0]); u8(i.rounds[1]); u8(i.flags); }
	void frames(const Frames &f)
	{
		u32(f.first);
		u32(f.count());
		for (uint16_t v : f.in) u16(v);
		u16(uint16_t(std::min<size_t>(f.hashes.size(), 65535)));
		for (size_t i = 0; i < f.hashes.size() && i < 65535; i++) { u32(f.hashes[i].first); u64(f.hashes[i].second); }
	}
};

struct In
{
	const uint8_t *p, *e;
	bool ok = true;
	explicit In(const std::vector<uint8_t> &v) : p(v.data()), e(v.data() + v.size()) {}
	bool need(size_t n) { if (!ok || size_t(e - p) < n) ok = false; return ok; }
	uint8_t u8() { return need(1) ? *p++ : 0; }
	uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | u8() << 8); }
	uint32_t u32() { const uint32_t a = u16(); return a | uint32_t(u16()) << 16; }
	uint64_t u64() { const uint64_t a = u32(); return a | uint64_t(u32()) << 32; }
	bool head(uint8_t kind, uint64_t &match) { const bool good = u32() == MAGIC && u8() == kind && u8() == 1; match = u64(); return ok && good; }
	void info(Info &i) { i.wins[0] = u8(); i.wins[1] = u8(); i.rounds[0] = u8(); i.rounds[1] = u8(); i.flags = u8(); }
	bool bytes(std::vector<uint8_t> &out, size_t n) { if (!need(n)) return false; out.assign(p, p + n); p += n; return true; }
	bool frames(Frames &f, uint32_t most)
	{
		f.first = u32();
		const uint32_t n = u32();
		if (!ok || n > most || !need(size_t(n) * 4)) return ok = false;
		f.in.resize(size_t(n) * 2);
		for (uint16_t &v : f.in) v = u16();
		const uint16_t nh = u16();
		if (!need(size_t(nh) * 12)) return false;
		f.hashes.clear();
		for (uint16_t i = 0; i < nh; i++) { const uint32_t fr = u32(); f.hashes.push_back({ fr, u64() }); }
		return ok && uint64_t(f.first) + n <= MAX_FRAMES;
	}
};

// (what a game says of its players: the letters a name may have, two for a country)
std::string clean(const std::string &s, size_t max)
{
	std::string out;
	for (char c : s)
		if (c >= 0x20 && c < 0x7F && out.size() < max) out += c;
	return out;
}

} // namespace

std::string hex(uint64_t v)
{
	char b[17];
	snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
	return b;
}

uint64_t new_match_id()
{
	std::random_device rd;
	uint64_t v = (uint64_t(rd()) << 32 ^ rd()) ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull;
	return v ? v : 1;
}

std::string meta_json(const Meta &m)
{
	using json::quote;
	return "{\"game\":" + quote(m.game) + ",\"terms\":" + quote(m.terms) + ",\"content\":" + quote(m.content) + ",\"build\":" + quote(m.build) +
	       ",\"p1\":" + quote(m.name[0]) + ",\"p2\":" + quote(m.name[1]) + ",\"c1\":" + quote(m.country[0]) + ",\"c2\":" + quote(m.country[1]) + "}";
}

bool parse_meta(const std::string &s, Meta &m)
{
	json::Value v;
	if (!json::parse(s, v) || v.kind != json::Value::object) return false;
	m.game = clean(v["game"].str(), 16);
	m.terms = clean(v["terms"].str(), 64);
	m.content = clean(v["content"].str(), 32);
	m.build = clean(v["build"].str(), 64);
	m.name[0] = clean(v["p1"].str(), size_t(MAX_NAME));
	m.name[1] = clean(v["p2"].str(), size_t(MAX_NAME));
	m.country[0] = clean(v["c1"].str(), 2);
	m.country[1] = clean(v["c2"].str(), 2);
	return !m.game.empty();
}

std::vector<uint8_t> encode(const Start &s)
{
	Out o;
	o.head(START, s.match);
	const std::string m = meta_json(s.meta).substr(0, 4096);
	o.u16(uint16_t(m.size()));
	o.bytes(reinterpret_cast<const uint8_t *>(m.data()), m.size());
	o.u32(uint32_t(s.blob.size()));
	o.bytes(s.blob.data(), s.blob.size());
	return o.b;
}

bool decode(const std::vector<uint8_t> &b, Start &s)
{
	In i(b);
	if (!i.head(START, s.match)) return false;
	const uint16_t ml = i.u16();
	if (!i.need(ml)) return false;
	const std::string m(reinterpret_cast<const char *>(i.p), ml);
	i.p += ml;
	const uint32_t bl = i.u32();
	if (!i.ok || bl > MAX_START || !i.bytes(s.blob, bl)) return false;
	return parse_meta(m, s.meta) && s.match != 0;
}

std::vector<uint8_t> encode_push(const Frames &f)
{
	Out o;
	o.head(PUSH, f.match);
	o.frames(f);
	o.info(f.info);
	return o.b;
}

bool decode_push(const std::vector<uint8_t> &b, Frames &f)
{
	In i(b);
	if (!i.head(PUSH, f.match) || !i.frames(f, MAX_PUSH)) return false;
	i.info(f.info);
	return i.ok;
}

std::vector<uint8_t> encode_reply(const Reply &r)
{
	Out o;
	o.head(REPLY, r.match);
	o.u8(r.state);
	o.u32(r.total);
	o.u32(r.watchers);
	o.info(r.frames.info);
	o.u32(NONE);   // (once a checkpoint's frame: none now)
	o.frames(r.frames);
	return o.b;
}

bool decode_reply(const std::vector<uint8_t> &b, Reply &r)
{
	In i(b);
	if (!i.head(REPLY, r.match)) return false;
	r.state = i.u8();
	r.total = i.u32();
	r.watchers = i.u32();
	i.info(r.frames.info);
	i.u32();   // (once a checkpoint's frame: not read)
	if (!i.frames(r.frames, MAX_REPLY)) return false;
	r.frames.match = r.match;
	return i.ok && r.total <= MAX_FRAMES;
}

uint64_t state_hash_part(const std::vector<uint8_t> &st, size_t skip, int part, uint64_t h)
{
	skip = std::min(st.size(), skip);
	const size_t n = st.size() - skip;
	const size_t a = skip + n * size_t(part) / HASH_PARTS, b = skip + n * size_t(part + 1) / HASH_PARTS;
	return (h ^ hash64(st.data() + a, b - a)) * 0x100000001B3ull;
}

uint64_t state_hash(const std::vector<uint8_t> &st, size_t skip)
{
	uint64_t h = 0x9E3779B97F4A7C15ull;
	for (int p = 0; p < HASH_PARTS; p++) h = state_hash_part(st, skip, p, h);
	return h;
}

// ---- Feed ----

void Feed::begin(const Start &s, double now)
{
	const double every = interval_ms, give_up = give_up_ms;
	*this = Feed();
	interval_ms = every;
	give_up_ms = give_up;
	st = s;
	started = true;
	reached_at = now;
}

void Feed::take(const Session &s)
{
	if (!started || gave_up) return;
	const std::vector<uint16_t> &a = s.inputs(0), &b = s.inputs(1);
	const uint32_t upto = std::min({ s.confirmed(), uint32_t(a.size()), uint32_t(b.size()), MAX_FRAMES });
	for (uint32_t f = confirmed(); f < upto; f++) { in.push_back(a[f]); in.push_back(b[f]); }
	// (each hash once, as it's made: a few frames after its frame's confirmed, so often after the lobby has the frame
	// itself; sent with the next upload all the same, so a watcher checks every one of them, a late one too)
	for (const auto &[f, h] : s.hashes())
		if (f <= upto && f > last_hash) { hashes[f] = h; last_hash = f; }
}

bool Feed::next(double now, Upload &u)
{
	if (!started || gave_up || in_flight >= MOST_IN_FLIGHT) return false;
	if (in_flight && (!start_sent || now - last_at < 3 * interval_ms)) return false;   // (a second only once the first's late)
	if (!start_sent) {
		u = {};
		u.start = true;
		u.body = encode(st);
	} else {
		if (now - last_at < (missed ? std::min(interval_ms, 2000.0) : interval_ms)) return false;
		Frames f;
		f.match = st.match;
		f.first = std::min(lobby_has, confirmed());
		const uint32_t to = std::min(confirmed(), f.first + MAX_PUSH);
		f.in.assign(in.begin() + ptrdiff_t(f.first) * 2, in.begin() + ptrdiff_t(to) * 2);
		for (const auto &[fr, h] : hashes)
			if (fr <= to && f.hashes.size() < 1000) f.hashes.push_back({ fr, h });
		f.info = info;
		u = {};
		u.body = encode_push(f);
		u.upto = to;
	}
	in_flight++;
	last_at = now;
	bytes += u.body.size();
	uploads++;
	return true;
}

void Feed::done(const Upload &u, bool ok, uint32_t has, int w, double now)
{
	in_flight = std::max(0, in_flight - 1);
	missed = !ok;
	if (!ok) {
		if (now - reached_at > give_up_ms) gave_up = true;
		return;
	}
	reached_at = now;
	watching = std::max(0, w);
	if (u.start) { start_sent = true; return; }
	if (has == NONE) { start_sent = false; lobby_has = 0; return; }   // (the lobby lost the match: it all again)
	lobby_has = std::max(lobby_has, std::min(has, confirmed()));   // (two on their way: the later answer can be the smaller)
	for (auto it = hashes.begin(); it != hashes.end() && it->first <= std::min(u.upto, lobby_has);) it = hashes.erase(it);
	info_sent = info;
}

// ---- Spectator ----

bool Spectator::start(const Start &s)
{
	start_ = s;
	in.clear();
	hashes.clear();
	at = 0;
	hashing.on = false;
	if (!game.begin(s.blob)) { st = State::failed; why = "The match's start couldn't be loaded."; return false; }
	started = true;
	st = State::starting;   // (till its first frames come)
	return true;
}

bool Spectator::reply(const Reply &r, double now)
{
	if (!started || r.match != start_.match) {
		if (started && r.match && r.match != start_.match) { st = State::over; why = "next"; }   // (a new match on the same watch: the host program starts it)
		return false;
	}
	lobby_total = std::max(lobby_total, r.total);
	lobby_state = r.state;
	watching = int(std::min<uint32_t>(r.watchers, 1000000));
	info_now = r.frames.info;
	const uint32_t end = have();
	const uint32_t first = r.frames.first, n = r.frames.count();
	if (first <= end && first + n > end) {
		const size_t from = size_t(end - first) * 2;
		in.insert(in.end(), r.frames.in.begin() + ptrdiff_t(from), r.frames.in.end());
		// (how late these came against the best any have lately: the jitter the buffer's to cover)
		if (now >= 0) {
			const double offset = now - double(first + n) * cfg.frame_ms;
			arrivals.push_back({ now, offset });
			while (!arrivals.empty() && now - arrivals.front().first > 10000) arrivals.pop_front();
			last_arrival = now;
		}
	}
	for (const auto &[f, h] : r.frames.hashes) if (f >= at && f <= first + n) hashes[f] = h;
	// (the hashes of frames run already, and not being checked now: let go, so a long match keeps a few)
	for (auto it = hashes.begin(); it != hashes.end() && it->first < at && !(hashing.on && it->first == hashing.frame);) it = hashes.erase(it);
	// (joining: every frame from the match's start, unseen, as fast as it can, to where the players are)
	if (st == State::starting) st = State::catching_up;
	return true;
}

std::string Spectator::want(double now) const
{
	if (in_flight || st == State::failed || st == State::out_of_step) return "";
	if (!started) return now - asked_at >= 2000 ? "start" : "";
	if (live) return "";   // (a stream: the frames come by themselves)
	if (lobby_state == 2 && have() >= lobby_total && answered_at > asked_at - 1) return st == State::over ? "" : (now - answered_at >= cfg.poll_ms ? "frames" : "");
	if (have() < lobby_total) return "frames";   // (more there: at once)
	return now - answered_at >= cfg.poll_ms ? "frames" : "";
}

double Spectator::behind_s() const
{
	if (!started) return 0.0;
	const double held = double(std::max<int64_t>(0, int64_t(std::max(lobby_total, have())) - int64_t(at))) * cfg.frame_ms;
	return (held + (last_arrival >= 0 ? std::max(0.0, clock_now - last_arrival) : 0.0)) / 1000.0;
}

bool Spectator::check(uint32_t f)
{
	if (hashing.on) {   // (a part a frame: the state saved at its frame, as the session's)
		hashing.h = state_hash_part(hashing.state, game.hash_skip(), hashing.part, hashing.h);
		if (++hashing.part == HASH_PARTS) {
			hashing.on = false;
			if (hashing.h != hashes[hashing.frame]) {
				st = State::out_of_step;
				why = "The match went out of step here at frame " + std::to_string(hashing.frame) + " (the players' game and this one differ).";
				return false;
			}
			hashes_checked++;
		}
	}
	if (!hashing.on) {
		const auto h = hashes.find(f);
		if (h != hashes.end()) {
			hashing.on = true;
			hashing.frame = f;
			hashing.part = 0;
			hashing.h = 0x9E3779B97F4A7C15ull;
			game.save(hashing.state);
		}
	}
	return true;
}

void Spectator::run_one(bool seen)
{
	if (!check(at)) return;
	const size_t i = size_t(at) * 2;
	const uint16_t two[2] = { in[i], in[i + 1] };
	game.advance(two, !seen);
	at++;
	frames_run++;
	if (!seen) frames_unseen++;
}

int Spectator::tick(bool *shown, double budget_ms)
{
	if (shown) *shown = false;
	if (!started || st == State::starting || st == State::failed || st == State::out_of_step || st == State::over) return 0;
	const uint32_t end = have();
	const auto frames = [&](double ms) { return int64_t(std::ceil(ms / cfg.frame_ms)); };
	// the buffer: the jitter lately (the latest any came against the earliest, in the last 10 s), a frame over, and
	// not less than it's been pushed to by running dry in the last 10 s; down slowly when it's steady
	// (the jitter: how much later than the earliest the latest came, leaving out the latest few: a burst once in a while is
	// the running dry's to deal with, not the whole match's delay)
	double jitter = 0;
	if (arrivals.size() >= 4) {
		double lo = arrivals.front().second;
		for (const auto &a : arrivals) lo = std::min(lo, a.second);
		std::vector<double> late;
		for (const auto &a : arrivals) late.push_back(a.second - lo);
		const size_t k = late.size() - 1 - std::min<size_t>(1, late.size() / 50);   // (all but the latest in the 10 s)
		std::nth_element(late.begin(), late.begin() + ptrdiff_t(k), late.end());
		jitter = late[k];
	}
	const double want_ms = std::clamp(jitter + cfg.frame_ms, cfg.buffer_min_ms, cfg.buffer_max_ms);
	if (target_ms < want_ms) target_ms = want_ms;
	else if (clock_now - steady_since > 3000) target_ms -= (target_ms - want_ms) * 0.003;   // (down to it over some seconds once steady)
	const int64_t want = frames(target_ms), held = int64_t(end) - int64_t(at);
	const int64_t lag = int64_t(std::max(lobby_total, end)) - int64_t(at);
	int ran = 0;
	// far behind (joining late, or after a long spell cut off): unseen, as fast as the pass allows, to the buffer
	if (lag > want + frames(cfg.jump_ms) || (st == State::catching_up && lag > want)) {
		if (st != State::catching_up && st != State::waiting) jumps++;
		st = State::catching_up;
		const auto t0 = std::chrono::steady_clock::now();
		const double budget = budget_ms >= 0 ? budget_ms : cfg.catch_up_ms;
		if (on_catch_up) on_catch_up(true);   // (their sound dropped: none of it late, all at once)
		while (int64_t(std::max(lobby_total, end)) - int64_t(at) > want && at + 1 < end && ran < cfg.catch_up_frames && st == State::catching_up) {
			run_one(false);
			ran++;
			if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budget) break;
		}
		if (on_catch_up) on_catch_up(false);
		if (st != State::catching_up) return ran;
		if (int64_t(std::max(lobby_total, end)) - int64_t(at) > want && at + 1 < end) return ran;   // (still catching up: no picture yet)
	}
	if (at >= end) {   // (ran dry: the buffer a little more, from now on)
		if (lobby_state == 2 && at >= lobby_total) { st = State::over; why = "over"; return ran; }
		if (st == State::playing) { underruns++; target_ms = std::min(cfg.buffer_max_ms, target_ms + 2 * cfg.frame_ms); steady_since = clock_now; }
		st = State::buffering;
		return ran;
	}
	// (held after running dry, or before the first picture: till the buffer's there again, or the rest of a match over)
	if (st != State::playing && held < want && lobby_state != 2) return ran;
	st = State::playing;
	ticks++;
	// (the buffer nearly gone: a frame's time in 3 held, a little slower rather than running dry and stopping)
	if (held < std::max<int64_t>(2, want / 3) && lobby_state != 2 && ticks % 3 == 0) return ran;
	// (more held than the buffer wants: a frame in 4 more, unseen, till it's back to it: a quarter faster, no jump)
	if (held > want + std::max<int64_t>(2, want / 4) && ticks % 4 == 0 && at + 1 < end) { run_one(false); ran++; extra_frames++; }
	if (st != State::playing) return ran;
	run_one(true);
	ran++;
	if (shown) *shown = st == State::playing;
	return ran;
}

int Spectator::run(double now, bool *shown)
{
	if (shown) *shown = false;
	if (clock < 0) clock = now;
	clock_now = now;
	int ran = 0;
	for (int i = 0; i < 4 && now - clock >= cfg.frame_ms; i++) {
		clock += cfg.frame_ms;
		bool s = false;
		ran += tick(&s);
		if (shown && s) *shown = true;
	}
	if (now - clock > 4 * cfg.frame_ms) clock = now;   // (a long pass: not made up for)
	return ran;
}

} // namespace vfnet::watch
