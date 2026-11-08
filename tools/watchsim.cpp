// watchsim: watching a match (src/net/watch.h), simulated without a ROM: two players in a rollback session over a
// simulated network (as nettest's, a small deterministic game standing in for the real one), the host sending the
// match to a stand-in for the lobby's Watch (tools/lobby's, the same messages), and any number of watchers on links
// good and bad (high ping, loss, the connection dead for a while, joining late), each running its own copy of the game.
// It checks what watching promises:
//   - the players don't notice: their rollbacks, waits, frames held back, the bytes they send (to each other and to
//     the lobby) are the same with no watchers, one, or fifty; their steps' time the same but for the machine's noise
//   - every watcher's game is the players', frame for frame (each frame's state after it, against the host's)
//   - a slow or cut-off watcher falls behind and catches up by itself; a late one runs the match from its start,
//     unseen and silent, every one of the players' hashes checked, and catches up
//   watchsim [--watchers N] [--seconds S] [--verbose] [--trace N]     (no N: the runs with 0, 1 and 50, compared)
//   watchsim --lobby http://127.0.0.1:8787 [--watchers N] [--seconds S] [--quick] [--all-ws] [--https-feed] [--https-room] [--late S]
//            the same through a real lobby (tools/lobby/test/serve.mjs, or
//            wrangler dev): a listed room that stays, a guest joined to it, the host's uploads and the watchers' asks over HTTP
//            (--quick: a Quick match instead, two new builds matched at random (Show my Quick Match games in Rooms On,
//            both: qm_listed), listed to watch only; the host's
//            uploads on its WebSocket, --https-feed over HTTPS; one watcher on each way, a WebSocket, the HTTPS stream,
//            asking for pieces; --all-ws every watcher on a WebSocket; the room held by its WebSocket, --https-room by
//            /v1/hold every 15 s; --late S: the first watcher S seconds in, the host sending every 30 s till then)
#include "../src/net/http.h"
#include "../src/net/json.h"
#include "../src/net/watch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace {

namespace vw = vfnet::watch;
bool verbose = false;
constexpr double TIMEOUT_MS = 3000;
constexpr double FEED_MS = 200;   // (the host's uploads: 5 a second, as the game's)   // (a request to the lobby given up after this: the game's for these)
int trace_watcher = -1;   // (--trace N: that watcher's frame, buffer and the lobby's, every second)

// The stand-in game (nettest's): two positions and a mixing word, each frame's state hash kept
struct Toy : vfnet::Game
{
	struct S { uint64_t mix; int32_t x[2], y[2]; uint32_t frame; uint32_t pad; };   // (no padding: the bytes are hashed)
	static_assert(std::has_unique_object_representations_v<S>);
	S s{};
	std::vector<uint64_t> after;   // per frame: the state's hash after it, the last time it ran
	bool begin(const std::vector<uint8_t> &blob) override { s = {}; s.mix = vfnet::hash64(blob.data(), blob.size()); after.clear(); return true; }
	void save(std::vector<uint8_t> &st) override { st.resize(sizeof s); memcpy(st.data(), &s, sizeof s); }
	bool load(const std::vector<uint8_t> &st) override { if (st.size() != sizeof s) return false; memcpy(&s, st.data(), sizeof s); return true; }
	uint32_t snap_format() const override { return 0x70f; }   // (a watched match's snapshot: its state as it is)
	void advance(const uint16_t in[2], bool) override
	{
		for (int p = 0; p < 2; p++) {
			s.x[p] += (in[p] & 1) - ((in[p] >> 1) & 1);
			s.y[p] += ((in[p] >> 2) & 1) - ((in[p] >> 3) & 1);
			s.mix = (s.mix ^ (uint64_t(in[p]) << (p * 16)) ^ uint64_t(uint32_t(s.x[p] * 31 + s.y[p]))) * 0x9E3779B97F4A7C15ull;
			s.mix ^= s.mix >> 29;
		}
		if (after.size() <= s.frame) after.resize(size_t(s.frame) + 1);
		s.frame++;
		after[s.frame - 1] = vfnet::hash64(reinterpret_cast<const uint8_t *>(&s), sizeof s);
	}
};

struct Player
{
	std::mt19937 rng;
	uint16_t held = 0;
	int left = 0;
	explicit Player(uint32_t seed) : rng(seed) {}
	uint16_t next() { if (left-- <= 0) { held = uint16_t(rng() & 0x3FF); left = int(rng() % 12); } return held; }
};

// A link counted: the bytes a session sends
struct Counted : vfnet::Link
{
	vfnet::Link &in;
	uint64_t bytes = 0, packets = 0;
	explicit Counted(vfnet::Link &l) : in(l) {}
	void send(const uint8_t *p, size_t n) override { bytes += n; packets++; in.send(p, n); }
	bool recv(std::vector<uint8_t> &out) override { return in.recv(out); }
};

// Things happening later on the simulated clock (a request reaching the lobby, its answer coming back), in order
struct Events
{
	struct E { double at; uint64_t seq; std::function<void()> fn; bool operator<(const E &o) const { return at != o.at ? at > o.at : seq > o.seq; } };
	std::priority_queue<E> q;
	uint64_t seq = 0;
	void at(double t, std::function<void()> fn) { q.push({ t, seq++, std::move(fn) }); }
	void run_until(double t) { while (!q.empty() && q.top().at <= t) { E e = q.top(); q.pop(); e.fn(); } }
};

// A way to the lobby: each request and its answer late by the one-way time and up to the jitter, a lost packet sent
// again (TCP), and nothing at all through a spell cut off (a failure after the timeout)
struct Way
{
	double ms = 30, jitter = 0, loss = 0;
	std::vector<std::pair<double, double>> dead;   // (from, to), ms
	std::mt19937 rng;
	explicit Way(uint32_t seed) : rng(seed) {}
	double u() { return std::uniform_real_distribution<double>(0, 1)(rng); }
	bool down(double t) const { for (auto &d : dead) if (t >= d.first && t < d.second) return true; return false; }
	// a stream's message sent at t (TCP: none lost, a lost packet sent again a round trip and 200 ms later; in order)
	double one(double t, double after)
	{
		for (auto &d : dead) if (t >= d.first && t < d.second) t = d.second;   // (cut off: sent again once it's back)
		double at = t + ms + jitter * u();
		if (u() < loss) at += 200 + 2 * ms;
		return std::max(at, after);
	}
	// a request at t: the lobby gets it at *there (false: lost), the answer's back after (back)
	// (HTTPS: TCP, so a lost packet is sent again, a round trip and 200 ms later; only a link cut off fails a request)
	bool trip(double t, double &there, double &back)
	{
		double a = ms + jitter * u(), b = ms + jitter * u();
		if (u() < loss) a += 200 + 2 * ms;
		if (u() < loss) b += 200 + 2 * ms;
		there = t + a;
		back = there + b;
		return !down(t) && !down(there) && !down(back);
	}
};

// The lobby's Watch, as tools/lobby/src/index.js has it (in memory): the same messages and answers, and Cloudflare's
// edge in front of it (a whole piece kept for good, the last a second)
struct Relay
{
	static constexpr uint32_t PIECE = 256, PIECES = 8;
	static constexpr uint32_t HASH_BACK = 120;   // (a reply's hashes reach back this far before its first frame: made late, sent all the same)
	bool have = false;
	uint64_t match = 0;
	std::vector<uint8_t> start;
	std::vector<uint16_t> in;
	std::map<uint32_t, uint64_t> hashes;
	std::map<uint32_t, double> hash_at;   // (when each hash came)
	vw::Info info;
	double seen = 0;
	std::map<std::string, std::pair<double, double>> watchers;
	std::map<uint32_t, std::pair<double, std::vector<uint8_t>>> edge;   // piece -> (kept till, answer)
	uint64_t asked = 0, answered_by_edge = 0, bytes_out = 0, pushes = 0;

	int count(double now)
	{
		for (auto it = watchers.begin(); it != watchers.end();) it = now - it->second.second > 40000 ? watchers.erase(it) : std::next(it);
		return int(watchers.size());
	}
	bool post_start(const std::vector<uint8_t> &b, double now)
	{
		vw::Start s;
		if (!vw::decode(b, s)) return false;
		have = true; match = s.match; start = b; in.clear(); hashes.clear(); hash_at.clear(); info = {}; seen = now; edge.clear();
		return true;
	}
	// -> has (NONE: send it all), the watchers
	uint32_t push(const std::vector<uint8_t> &b, double now, int &w)
	{
		pushes++;
		w = count(now);
		vw::Frames f;
		if (!vw::decode_push(b, f)) return uint32_t(in.size() / 2);
		if (!have || f.match != match) return vw::NONE;
		const uint32_t n = uint32_t(in.size() / 2);
		if (f.first <= n && f.first + f.count() > n) in.insert(in.end(), f.in.begin() + ptrdiff_t(n - f.first) * 2, f.in.end());
		for (auto &[fr, h] : f.hashes) if (fr <= in.size() / 2) { hashes[fr] = h; hash_at.emplace(fr, now); }
		info = f.info;
		seen = now;
		return uint32_t(in.size() / 2);
	}
	bool over(double now) const { return (info.flags & 1) || now - seen > 30000; }
	std::vector<uint8_t> frames(uint32_t from, double now)
	{
		asked++;
		from = from / PIECE * PIECE;
		const uint32_t piece = from / PIECE;
		if (auto e = edge.find(piece); e != edge.end() && e->second.first > now) { answered_by_edge++; bytes_out += e->second.second.size(); return e->second.second; }
		vw::Reply r;
		r.match = match;
		r.state = over(now) ? 2 : 1;
		const uint32_t total = uint32_t(in.size() / 2);
		r.total = total;
		r.watchers = uint32_t(count(now));
		r.frames.info = info;
		r.frames.first = std::min(from, total);
		const uint32_t n = std::min(total - r.frames.first, PIECE * PIECES);
		r.frames.in.assign(in.begin() + ptrdiff_t(r.frames.first) * 2, in.begin() + ptrdiff_t(r.frames.first + n) * 2);
		for (auto &[fr, h] : hashes) if (fr + HASH_BACK >= r.frames.first && fr <= r.frames.first + n) r.frames.hashes.push_back({ fr, h });
		std::vector<uint8_t> out = vw::encode_reply(r);
		edge[piece] = { now + (n == PIECE * PIECES || (r.state == 2 && r.frames.first + n == total) ? 86400e3 : 1000), out };
		bytes_out += out.size();
		return out;
	}
};

struct Profile { const char *kind; double ms, jitter, loss, join_s; std::vector<std::pair<double, double>> dead; bool live = true; };

struct Watcher
{
	Toy game;
	vw::Spectator sp;
	Way way;
	Profile prof;
	std::string sid;
	double next_seen = 0;
	bool busy = false;
	uint64_t match = 0;
	uint64_t requests = 0, bytes_in = 0, bytes_out = 0;
	double most_behind = 0, behind_sum = 0;   // (behind the players' last confirmed frame, in s: the most, and summed every 100 ms watched)
	uint64_t behind_n = 0;
	std::vector<double> behinds;               // (every sample, for the 95th percentile)
	std::vector<double> ran_at;                // (when it ran each frame)
	std::map<uint32_t, double> hash_got;       // (when each of the players' hashes reached it)
	void got(const vw::Reply &r, double at) { for (const auto &[fr, h] : r.frames.hashes) hash_got.emplace(fr, at); }
	bool connected = false;                    // (on the stream: the frames sent as they come)
	uint32_t sent = 0;                         // (the stream's: frames sent it so far)
	double stream_last = 0;
	Watcher(const vw::Spectator::Config &c, uint32_t seed, const Profile &p) : sp(game, c), way(seed), prof(p)
	{
		char b[24];
		snprintf(b, sizeof b, "%016x", seed * 2654435761u);
		sid = b;
		way.ms = p.ms; way.jitter = p.jitter; way.loss = p.loss; way.dead = p.dead;
	}
};

struct Result
{
	uint32_t frames[2] = {};
	uint64_t rollbacks[2] = {}, resim[2] = {}, stalls[2] = {}, waits[2] = {}, wire_bytes[2] = {}, wire_packets[2] = {};
	uint64_t feed_bytes = 0, feed_uploads = 0, guest_asks = 0;
	double step_ms[2] = {};
	int watchers_seen = 0;   // (the most the host was told watched)
	bool same_as(const Result &o) const
	{
		for (int p = 0; p < 2; p++)
			if (frames[p] != o.frames[p] || rollbacks[p] != o.rollbacks[p] || resim[p] != o.resim[p] || stalls[p] != o.stalls[p] || waits[p] != o.waits[p] ||
			    wire_bytes[p] != o.wire_bytes[p] || wire_packets[p] != o.wire_packets[p])
				return false;
		return feed_bytes == o.feed_bytes && feed_uploads == o.feed_uploads && guest_asks == o.guest_asks;
	}
};

std::vector<Profile> profiles(int n)
{
	std::vector<Profile> v;
	std::mt19937 rng(99);
	auto u = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
	for (int i = 0; i < n; i++) {
		if (i % 10 == 0) v.push_back({ "20 ms", 20, 2, 0, u(0, 8), {} });
		else if (i % 10 == 1) v.push_back({ "80 ms", 80, 8, 0.005, u(0, 8), {} });
		else if (i % 10 == 2) v.push_back({ "200 ms, jitter", 200, 150, 0.01, u(0, 8), {} });
		else if (i % 10 == 3) v.push_back({ "400 ms, loss", 400, 40, 0.08, u(0, 8), {} });
		else if (i % 10 == 4) v.push_back({ "late (at 75 s)", 40, 10, 0.01, 75 + u(0, 5), {} });
		else if (i % 10 == 5) v.push_back({ "cut off 10 s in 40", 60, 10, 0.01, u(0, 8), { { 20e3, 30e3 }, { 60e3, 70e3 }, { 100e3, 110e3 } } });
		else if (i % 10 == 6) v.push_back({ "phone (bursts)", 90, 120, 0.05, u(0, 8), { { 50e3, 52e3 }, { 90e3, 93e3 } } });
		else if (i % 10 == 7) { Profile p{ "asks (no stream)", 60, 10, 0.01, u(0, 8), {} }; p.live = false; v.push_back(p); }
		else if (i == 8) v.push_back({ "cut off 45 s", 50, 10, 0.0, 2, { { 30e3, 75e3 } } });
		else v.push_back({ "good", u(10, 120), u(0, 15), u(0, 0.02), u(0, 10), {} });
	}
	if (n == 1) v[0] = { "40 ms", 40, 10, 0.01, 1, {} };
	return v;
}

// (lobby_gone_ms: the lobby unreachable from then on, by the players and every watcher: the match must go on as ever)
Result play(int n_watchers, double seconds, bool &ok, double lobby_gone_ms = 0)
{
	using Clock = std::chrono::steady_clock;
	Result res;
	const uint32_t frames = uint32_t(seconds * 57.524160);
	vfnet::SimWire wire({ 45, 10, 0.02 }, { 45, 10, 0.02 }, 7);
	Counted ca(wire.a()), cb(wire.b());
	Toy ga, gb;
	std::vector<uint8_t> blob(2000);
	for (size_t i = 0; i < blob.size(); i++) blob[i] = uint8_t(i * 7 + 3);
	vfnet::Config cfg;
	cfg.delay = 2;
	cfg.checksum_every = 120;   // (the game's)
	vfnet::Session a(ca, ga, { "toy", "v1", 42, "HOST" }, cfg, true, blob);
	vfnet::Session b(cb, gb, { "toy", "v1", 42, "GUEST" }, cfg, false);
	Player pa(11), pb(22);

	Events ev;
	Relay relay;
	Way host_way(5), guest_way(6);   // (the players' own ways to the lobby: their own random numbers, whatever the watchers do)
	host_way.ms = 30; host_way.jitter = 10; host_way.loss = 0.01;
	guest_way.ms = 35; guest_way.jitter = 10; guest_way.loss = 0.01;
	vw::Feed feed;
	feed.interval_ms = FEED_MS;
	bool feed_begun = false;
	std::map<uint32_t, double> made;   // (the host's hashes, when it made each: every one the lobby's to have)
	double acked_built = -1;           // (when the last upload the lobby took was made)
	double guest_ask_at = 15000;
	double step_ms[2] = {};
	uint64_t steps[2] = {};

	vw::Spectator::Config sc;
	std::vector<std::unique_ptr<Watcher>> ws;
	const std::vector<Profile> pr = profiles(n_watchers);
	for (int i = 0; i < n_watchers; i++) ws.push_back(std::make_unique<Watcher>(sc, 1000 + uint32_t(i), pr[size_t(i)]));
	if (lobby_gone_ms > 0) {
		host_way.dead.push_back({ 1000 + lobby_gone_ms, 1e18 });
		guest_way.dead.push_back({ 1000 + lobby_gone_ms, 1e18 });
		for (auto &w : ws) w->way.dead.push_back({ 1000 + lobby_gone_ms, 1e18 });
	}

	auto players_done = [&](Result &res) {
		const vfnet::Session *s[2] = { &a, &b };
		const Counted *c[2] = { &ca, &cb };
		for (int p = 0; p < 2; p++) {
		const auto &st = s[p]->stats();
		res.frames[p] = s[p]->frame();
		res.rollbacks[p] = st.rollbacks;
		res.resim[p] = st.resim_frames;
		res.stalls[p] = st.stalls;
		res.waits[p] = st.waits;
		res.wire_bytes[p] = c[p]->bytes;
		res.wire_packets[p] = c[p]->packets;
		res.step_ms[p] = steps[p] ? step_ms[p] / double(steps[p]) : 0;
	}
	res.feed_bytes = feed.bytes;
	res.feed_uploads = feed.uploads;
	};
	// the Watch's streams: each watcher on one sent what it hasn't had as soon as the host's frames come (in order, late
	// by its own way: a lost packet sent again)
	std::function<void(double)> fan = [&](double now) {
		for (auto &wp : ws) {
			Watcher &w = *wp;
			const uint32_t n = uint32_t(relay.in.size() / 2);
			if (!w.connected || w.sent >= n) continue;
			vw::Reply r;
			r.match = relay.match;
			r.state = relay.over(now) ? 2 : 1;
			r.total = n;
			r.watchers = uint32_t(relay.count(now));
			r.frames.info = relay.info;
			r.frames.first = w.sent;
			r.frames.in.assign(relay.in.begin() + ptrdiff_t(w.sent) * 2, relay.in.end());
			for (auto &[fr, h] : relay.hashes) if (fr + Relay::HASH_BACK >= w.sent && fr <= n) r.frames.hashes.push_back({ fr, h });
			w.sent = n;
			w.stream_last = w.way.one(now, w.stream_last);
			w.bytes_in += vw::encode_reply(r).size() + 4;
			ev.at(w.stream_last, [&w, r, at = w.stream_last] { if (w.sp.reply(r, at)) w.got(r, at); });
		}
	};
	const double fms = cfg.frame_ms;
	double t = 1000, next_a = 1000, next_b = 1037;
	const double end_at = 1000 + seconds * 1000 + 40000;
	bool over_said = false;
	for (; t < end_at; t += 1) {
		wire.now = t;
		ev.run_until(t);
		a.poll(t);
		b.poll(t);
		// ---- the players (exactly as with no watchers: nothing below reads anything of theirs but the feed) ----
		if (t >= next_a && a.frame() < frames + 20) {
			const auto t0 = Clock::now();
			if (a.step(pa.next())) { step_ms[0] += std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); steps[0]++; }
			next_a += fms;
			if (a.state() == vfnet::Session::State::running && !feed_begun) {
				vw::Start s;
				s.match = 0x5eed000000000001ull;
				s.meta = { "toy", "v1", vw::hex(42), "v1", { "HOST", "GUEST" }, { "KR", "US" } };
				s.blob = a.blob();
				feed.begin(s, t);
				feed_begun = true;
			}
			feed.take(a);
			for (const auto &[fr, hh] : a.hashes()) made.emplace(fr, t);
			vw::Info info;
			info.wins[0] = uint8_t(a.confirmed() / 2000);
			info.rounds[1] = uint8_t(a.confirmed() / 700 % 3);
			if (a.confirmed() >= frames && !over_said) { info.flags = 1; over_said = true; }
			if (over_said) info.flags = 1;
			feed.set_info(info);
		}
		if (t >= next_b && b.frame() < frames + 20) {
			const auto t0 = Clock::now();
			if (b.step(pb.next())) { step_ms[1] += std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); steps[1]++; }
			next_b += fms * 1.003;
		}
		// the host's upload (on a thread of its own in the game: here, an event later), its answer when it comes
		vw::Feed::Upload up;
		if (feed.next(t, up)) {
			double there, back;
			const bool through = host_way.trip(t, there, back);
			if (!through) ev.at(t + TIMEOUT_MS, [&feed, up, t] { feed.done(up, false, 0, 0, t + TIMEOUT_MS); });
			else ev.at(there, [&, up, there, back, built = t] {
				int w = 0;
				uint32_t has = 0;
				bool good = true;
				if (up.start) good = relay.post_start(up.body, there);
				else { has = relay.push(up.body, there, w); fan(there); }
				ev.at(back, [&feed, &acked_built, up, good, has, w, back, built] {
					feed.done(up, good, has, w, back);
					if (good && !up.start && has != vw::NONE) acked_built = std::max(acked_built, built);
				});
			});
		}
		// the guest asks the count every 15 s (shown in its match), on a thread in the game
		if (t >= guest_ask_at && b.frame() < frames + 20) {
			guest_ask_at = t + 15000;
			res.guest_asks++;
			double there, back;
			guest_way.trip(t, there, back);
		}
		// ---- the watchers ----
		for (auto &wp : ws) {
			Watcher &w = *wp;
			if (t < 1000 + w.prof.join_s * 1000) continue;
			// (here every 15 s: the count, and the match on)
			if (t >= w.next_seen) {
				w.next_seen = t + (w.match ? 15000 : 2000);
				double there, back;
				w.requests++;
				if (w.way.trip(t, there, back))
					ev.at(there, [&relay, &w, &ev, there, back] {
						auto &x = relay.watchers[w.sid];
						if (x.first == 0) x.first = there;
						x.second = there;
						const uint64_t m = relay.have ? relay.match : 0;
						ev.at(back, [&w, m] { if (m) w.match = m; });
					});
			}
			if (!w.match) continue;
			// on the stream once the start's in (connected a round trip later, then what's there at once)
			if (w.prof.live && !w.connected && w.sp.state() != vw::Spectator::State::waiting && !w.sp.live && relay.have) {
				w.sp.live = true;
				w.requests++;
				w.stream_last = w.way.one(t, 0) + w.way.ms;
				ev.at(w.stream_last, [&, wptr = &w] { wptr->connected = true; wptr->sent = std::min<uint32_t>(wptr->sp.ask_from() / 256 * 256, uint32_t(relay.in.size() / 2)); fan(wptr->stream_last); });
			}
			const std::string want = w.sp.want(t);
			if (!want.empty()) {
				w.sp.asked(t);
				w.requests++;
				double there, back;
				if (!w.way.trip(t, there, back)) { if (trace_watcher == int(&wp - &ws[0])) printf("      %.3f s: %s lost\n", (t - 1000) / 1000, want.c_str()); ev.at(t + TIMEOUT_MS, [&w, t] { w.sp.answered(t + TIMEOUT_MS); }); continue; }
				if (trace_watcher == int(&wp - &ws[0])) printf("      %.3f s: %s from %u\n", (t - 1000) / 1000, want.c_str(), w.sp.ask_from());
				ev.at(there, [&, want, there, back, wptr = &w] {
					std::vector<uint8_t> out;
					if (want == "start" && relay.have) out = relay.start;
					else if (want == "frames" && relay.have) out = relay.frames(wptr->sp.ask_from(), there);
					ev.at(back, [want, out, back, wptr] {
						Watcher &x = *wptr;
						x.bytes_in += out.size();
						if (want == "start") { vw::Start s; if (vw::decode(out, s)) x.sp.start(s); }
						else if (want == "frames") { vw::Reply r; if (vw::decode_reply(out, r) && x.sp.reply(r, back)) x.got(r, back); }
						x.sp.answered(back);
					});
				});
			}
			w.sp.run(t);
			while (w.ran_at.size() < w.sp.frame()) w.ran_at.push_back(t);
			if (trace_watcher == int(&wp - &ws[0]) && std::fmod(t, 1000.0) < 1)
				printf("      t %.0f s: frame %u, held to %u, the lobby %u, the players %u, state %d, %.1f s behind the players; the relay has %zu, the feed acked %u\n", (t - 1000) / 1000, w.sp.frame(), w.sp.ask_from(),
				       w.sp.total(), a.confirmed(), int(w.sp.state()), double(a.confirmed() - std::min(a.confirmed(), w.sp.frame())) * fms / 1000, relay.in.size() / 2, feed.acked());
			if ((w.sp.state() == vw::Spectator::State::playing || w.sp.state() == vw::Spectator::State::buffering) && a.frame() < frames && std::fmod(t, 100.0) < 1) {
				const double behind = double(a.confirmed() - std::min(a.confirmed(), w.sp.frame())) * fms / 1000;
				w.most_behind = std::max(w.most_behind, behind);
				w.behind_sum += behind;
				w.behind_n++;
				w.behinds.push_back(behind);
			}
		}
		res.watchers_seen = std::max(res.watchers_seen, feed.watchers());
		// (the players' figures as they finish: the run goes on only for the watchers still behind)
		if (a.frame() >= frames + 20 && b.frame() >= frames + 20 && !res.frames[0]) {
			players_done(res);
			if (ws.empty()) break;
		}
	}

	// ---- what came of it ----
	if (!res.frames[0]) players_done(res);
	if (a.end_reason() != vfnet::Session::End::none || b.end_reason() != vfnet::Session::End::none) { printf("  FAIL the players' session ended (%d/%d)\n", int(a.end_reason()), int(b.end_reason())); ok = false; }
	const uint32_t confirmed = std::min(a.confirmed(), b.confirmed());
	if (lobby_gone_ms > 0) {   // (the lobby gone: the players' match to its end; the host stops sending after a minute; watchers stuck, which their game says)
		printf("  the lobby gone at %.0f s: players' frames %u/%u, rollbacks %llu/%llu, waits %llu/%llu; the host gave up sending after %llu uploads%s\n", lobby_gone_ms / 1000,
		       res.frames[0], res.frames[1], (unsigned long long)res.rollbacks[0], (unsigned long long)res.rollbacks[1], (unsigned long long)res.waits[0], (unsigned long long)res.waits[1],
		       (unsigned long long)res.feed_uploads, feed.failed() ? " (it said so: this match can't be watched any more)" : "");
		return res;
	}
	if (feed.acked() + 60 < confirmed) { printf("  FAIL the lobby has %u frames of %u\n", feed.acked(), confirmed); ok = false; }
	// every one of the players' hashes at the lobby: each made before an upload it took (of a frame it has), the last few
	// made after the host's last upload the only ones it can't have
	uint32_t hashes_due = 0, hashes_got = 0;
	for (const auto &[fr, at] : made) {
		hashes_got += relay.hashes.count(fr) != 0;
		hashes_due += at <= acked_built && fr <= feed.acked();
	}
	if (hashes_got < hashes_due) { printf("  FAIL the lobby has %u of the players' %u hashes (%u made before its last upload)\n", hashes_got, uint32_t(made.size()), hashes_due); ok = false; }

	// every watcher: its game frame for frame the players' (the host's state after each frame, as it last ran: the
	// confirmed one), to the end of the match
	int finished = 0, late = 0, good = 0;
	double good_avg = 0, good_most = 0;
	uint64_t req = 0, in = 0;
	double secs = 0;
	for (auto &wp : ws) {
		Watcher &w = *wp;
		const uint32_t ran = w.sp.frame();
		bool same = true;
		uint32_t first_bad = 0;
		for (uint32_t f = 0; f < ran && f < confirmed; f++)
			if (f >= w.game.after.size() || w.game.after[f] != ga.after[f]) { same = false; first_bad = f; break; }
		// (one come late ran the match from its start: every one of the players' hashes in it checked, none skipped; and
		// at the live edge, as for everyone watching live, every one that reached it before it ran the hash's frame: one
		// made late and come just after can't be checked)
		uint32_t hashes_there = 0;
		for (const auto &[fr, h] : relay.hashes) {
			const auto got = w.hash_got.find(fr);
			hashes_there += fr + vw::HASH_PARTS < ran && got != w.hash_got.end() && fr < w.ran_at.size() && got->second <= w.ran_at[fr];
		}
		const bool came_late = w.prof.join_s >= 30, all_hashes = w.sp.hashes_checked >= hashes_there;
		const bool done = w.sp.state() == vw::Spectator::State::over && ran + 30 >= relay.in.size() / 2;   // (to the end: the match said over a few frames before the players stopped)
		finished += done;
		late += came_late;
		req += w.requests;
		if (!strcmp(w.prof.kind, "good") && w.behind_n) { good++; good_avg += w.behind_sum / double(w.behind_n); good_most = std::max(good_most, w.most_behind); }
		in += w.bytes_in;
		secs += (end_at - (1000 + w.prof.join_s * 1000)) / 1000;
		if (!same || !done || w.sp.hashes_checked == 0 || !all_hashes) {
			ok = false;
			printf("  FAIL watcher %s (%s): %s, frame %u of %zu, state %d, %u hashes checked (of %u)%s\n", w.sid.c_str(), w.prof.kind,
			       same ? "the same game" : ("differs at frame " + std::to_string(first_bad)).c_str(), ran, relay.in.size() / 2, int(w.sp.state()), w.sp.hashes_checked, hashes_there,
			       w.sp.why.empty() ? "" : (": " + w.sp.why).c_str());
		} else if (verbose)
			printf("    watcher %-16s %-16s ran %u frames (%llu unseen)%s, behind the players %.1f s on average, %.1f the most, %llu requests, %llu bytes\n", w.sid.c_str(), w.prof.kind, ran,
			       (unsigned long long)w.sp.frames_unseen, came_late ? (", from the start: " + std::to_string(w.sp.hashes_checked) + " of " + std::to_string(hashes_there) + " hashes").c_str() : "",
			       w.behind_n ? w.behind_sum / double(w.behind_n) : 0.0, w.most_behind, (unsigned long long)w.requests, (unsigned long long)w.bytes_in);
	}
	printf("  %d watchers: players' frames %u/%u, rollbacks %llu/%llu (%llu/%llu frames run again), held back %llu/%llu, waits %llu/%llu, sent each other %llu/%llu bytes,"
	       " host to the lobby %llu bytes in %llu uploads (%u of its %u hashes), the guest's count asked %llu times; a step %.4f/%.4f ms\n",
	       n_watchers, res.frames[0], res.frames[1], (unsigned long long)res.rollbacks[0], (unsigned long long)res.rollbacks[1], (unsigned long long)res.resim[0],
	       (unsigned long long)res.resim[1], (unsigned long long)res.stalls[0], (unsigned long long)res.stalls[1], (unsigned long long)res.waits[0],
	       (unsigned long long)res.waits[1], (unsigned long long)res.wire_bytes[0], (unsigned long long)res.wire_bytes[1], (unsigned long long)res.feed_bytes,
	       (unsigned long long)res.feed_uploads, hashes_got, uint32_t(made.size()), (unsigned long long)res.guest_asks, res.step_ms[0], res.step_ms[1]);
	if (n_watchers)
		printf("     watchers: %d of %d saw the whole match, the players' game frame for frame (%d came late: from the start, every hash checked); the host was told up to %d watch;"
		       " each asked the lobby %.2f times a second, %.0f bytes a second each; the Watch itself answered %llu of %llu asks (the edge the rest)\n",
		       finished, n_watchers, late, res.watchers_seen, double(req) / secs, double(in) / secs,
		       (unsigned long long)(relay.asked - relay.answered_by_edge), (unsigned long long)relay.asked);
	if (good) printf("     the %d on good links: %.2f s behind the players on average, %.2f the most\n", good, good_avg / good, good_most);
	if (n_watchers >= 10) {   // (by the kind of link: how far behind the players' last confirmed frame each kept, and what it took)
		printf("     %-20s %8s %8s %8s %9s %8s %6s\n", "link", "behind", "p95", "buffer", "ran dry", "extra", "jumps");
		std::map<std::string, std::vector<Watcher *>> by;
		for (auto &wp : ws) by[wp->prof.kind].push_back(wp.get());
		for (auto &[kind, list] : by) {
			std::vector<double> all;
			double buf = 0;
			uint64_t dry = 0, extra = 0, jumps = 0;
			for (Watcher *w : list) { all.insert(all.end(), w->behinds.begin(), w->behinds.end()); buf += w->sp.buffer_ms(); dry += w->sp.underruns; extra += w->sp.extra_frames; jumps += w->sp.jumps; }
			std::sort(all.begin(), all.end());
			double avg = 0;
			for (double x : all) avg += x;
			avg = all.empty() ? 0 : avg / double(all.size());
			const double p95 = all.empty() ? 0 : all[size_t(double(all.size() - 1) * 0.95)];
			printf("     %-20s %6.2f s %6.2f s %5.0f ms %9llu %8llu %6llu\n", kind.c_str(), avg, p95, buf / double(list.size()), (unsigned long long)dry, (unsigned long long)extra, (unsigned long long)jumps);
		}
	}
	return res;
}

// Through a real lobby: the same messages over HTTP (each request on this one thread, the simulated clock waiting for it)
// (all_ws: every watcher on a WebSocket, as most will be, to count what the lobby's asked; else one on each way: a
// WebSocket, the HTTPS stream, asking for pieces. https_feed: the host's uploads over HTTPS, its fallback)
bool play_lobby(const std::string &lobby, int n_watchers, double seconds, bool quick, bool all_ws, bool https_feed, bool https_room, double late_s)
{
	namespace http = vfnet::http;
	using vfnet::json::quote;
	bool ok = true;
	auto fail = [&](const std::string &why) { printf("  FAIL %s\n", why.c_str()); ok = false; };
	auto me = [&](const std::string &name, const std::string &extra) {
		return "{\"game\":\"vf3\",\"version\":\"watchsim\",\"content\":\"" + vw::hex(42) + "\",\"name\":" + quote(name) + ",\"cands\":[],\"relay\":\"\"" + extra + "}";
	};
	vfnet::json::Value h, g;
	std::string code, key, wid, wkey;
	// (what the lobby has on, from its /v1/online: its switches, or an older lobby that says nothing and has neither)
	bool has_rooms = false, has_watch = false;
	{
		vfnet::json::Value f;
		const http::Reply fr = http::request("GET", lobby + "/v1/online", "", 5000, 1 << 20);
		if (fr.status == 200 && vfnet::json::parse(fr.body, f)) {
			has_rooms = f["rooms"].kind == vfnet::json::Value::list;
			has_watch = f["features"]["watch"].kind == vfnet::json::Value::boolean && f["features"]["watch"].b;
		}
		printf("  the lobby: rooms %s, watching %s%s\n", has_rooms ? "on" : "off", has_watch ? "on" : "off", f["features"].kind == vfnet::json::Value::object ? "" : " (an older lobby)");
	}
	if (quick) {   // (--quick: a Quick match, two new builds matched at random; its host's game sends it, nobody asked anything)
		vfnet::json::Value x, y;
		const http::Reply xr = http::request("POST", lobby + "/v1/random", me("HOST", ",\"watchable\":true,\"qm_listed\":true"), 5000);
		if (xr.status != 200 || !vfnet::json::parse(xr.body, x) || x["ticket"].str().empty()) { fail("the lobby didn't take the first search (" + std::to_string(xr.status) + ")"); return false; }
		http::Reply wr;   // (the first waiting on its answer, as a game does: only one waiting is matched)
		std::thread waiting([&] { wr = http::request("POST", lobby + "/v1/random/wait", me("HOST", ",\"watchable\":true,\"qm_listed\":true,\"ticket\":" + quote(x["ticket"].str())), 25000); });
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const http::Reply yr = http::request("POST", lobby + "/v1/random", me("GUEST", ",\"watchable\":true,\"qm_listed\":true"), 5000);
		waiting.join();
		if (!vfnet::json::parse(yr.body, y) || !vfnet::json::parse(wr.body, x) || x["role"].str().empty() || y["role"].str().empty()) { fail("the two weren't matched (" + wr.body + ", " + yr.body + ")"); return false; }
		h = x["role"].str() == "host" ? x : y;
		g = x["role"].str() == "host" ? y : x;
		code = h["code"].str(); wid = h["wid"].str(); wkey = h["wkey"].str();
		if ((wid.size() == 6) != has_watch || g["wid"].str() != wid) { fail("the Quick match's watch code isn't as the lobby says (" + wid + ")"); return false; }
		printf("  a Quick match, %s\n", wid.empty() ? "no watch code" : ("watch code " + wid).c_str());
	} else {
		const http::Reply hr = http::request("POST", lobby + "/v1/host", me("HOST", ",\"listed\":true,\"persist\":true,\"room\":\"SIM ROOM\""), 5000);
		if (hr.status != 200 || !vfnet::json::parse(hr.body, h) || h["code"].str().size() != 6) { fail("the lobby didn't open a room (" + std::to_string(hr.status) + " " + hr.error + ")"); return false; }
		code = h["code"].str(); key = h["key"].str(); wid = h["wid"].str(); wkey = h["wkey"].str();
		if ((wid.size() == 6) != has_watch) fail("the room's watch code isn't as the lobby says");
		printf("  the room %s\n", h["persist"].kind == vfnet::json::Value::boolean && h["persist"].b ? "stays through its match" : "is a code-only room (rooms off, or an older lobby)");
		const http::Reply jr = http::request("POST", lobby + "/v1/join", me("GUEST", ",\"code\":" + quote(code) + ",\"queue\":true"), 5000);
		if (jr.status != 200 || !vfnet::json::parse(jr.body, g) || g["wid"].str() != wid) fail("the guest didn't join it (with its watch code, if any)");
		printf("  room %s, %s, the guest in\n", code.c_str(), wid.empty() ? "no watch code" : ("watch code " + wid).c_str());
	}
	if (wid.empty() && n_watchers > 0) {
		printf("  no watching here: the match played without it, no watchers\n");
		n_watchers = 0;
	}

	const uint32_t frames = uint32_t(seconds * 57.524160);
	vfnet::SimWire wire({ 45, 10, 0.02 }, { 45, 10, 0.02 }, 7);
	Toy ga, gb;
	std::vector<uint8_t> blob(2000);
	for (size_t i = 0; i < blob.size(); i++) blob[i] = uint8_t(i * 7 + 3);
	vfnet::Config cfg;
	cfg.checksum_every = 120;
	vfnet::Session a(wire.a(), ga, { "toy", "v1", 42, "HOST" }, cfg, true, blob), b(wire.b(), gb, { "toy", "v1", 42, "GUEST" }, cfg, false);
	Player pa(11), pb(22);
	vw::Feed feed;
	bool begun = false;
	struct Box { std::mutex lock; std::deque<std::vector<uint8_t>> got; std::atomic<bool> stop{ false }, done{ false }; };
	struct W { Toy game; std::unique_ptr<vw::Spectator> sp; std::string sid; double join = 0, seen_at = 0; uint64_t match = 0; uint32_t join_frame = vw::NONE;
	           bool stream = false, ws = false; std::shared_ptr<Box> box; std::thread reader; uint64_t streamed = 0; std::unique_ptr<http::Socket> sock; };
	std::vector<std::unique_ptr<W>> ws;
	vw::Spectator::Config sc;
	for (int i = 0; i < n_watchers; i++) {
		auto w = std::make_unique<W>();
		w->sp = std::make_unique<vw::Spectator>(w->game, sc);
		w->sid = vw::hex(0xabc000 + uint64_t(i));
		w->join = 1000 + late_s * 1000 + (i == 0 ? 0 : i == 1 ? 10000 : 75000 + 1000.0 * i);   // (one from the start, one soon after, the rest late: the match from its start, unseen)
		w->ws = all_ws || i == 0;           // (on a WebSocket, as the game is first)
		w->stream = !all_ws && i == 1;      // (the HTTPS stream, its fallback: read by http::stream; the rest ask for pieces)
		ws.push_back(std::move(w));
	}
	uint64_t requests = 0, ws_uploads = 0, wakes = 0;
	std::map<uint32_t, uint64_t> made;
	bool say_quick = true;   // (every watcher told rightly whether it's a Quick match's: /seen's and its socket's word)
	std::unique_ptr<http::Socket> feed_ws, room_ws;
	double hold_at = 0;
	const double fms = cfg.frame_ms;
	double next_a = 1000, next_b = 1037;
	for (double t = 1000; t < 1000 + seconds * 1000 + 30000; t += 1) {
		wire.now = t;
		a.poll(t);
		b.poll(t);
		if (t >= next_a && a.frame() < frames + 20) {
			a.step(pa.next());
			next_a += fms;
			for (const auto &[fr, hh] : a.hashes()) made[fr] = hh;   // (the players' hashes: a late watcher checks every one)
			if (!begun && a.state() == vfnet::Session::State::running) {
				vw::Start st;
				st.match = vw::new_match_id();
				st.meta = { "vf3", "watchsim", vw::hex(42), "watchsim", { "HOST", "GUEST" }, { "KR", "US" } };
				st.blob = a.blob();
				if (!wid.empty()) feed.begin(st, t);   // (no watch code: nothing sent, as an older build)
				begun = true;
			}
			feed.take(a);
			vw::Info info;
			info.wins[0] = uint8_t(a.confirmed() / 2000);
			if (a.confirmed() >= frames) info.flags = 1;
			feed.set_info(info);
		}
		if (t >= next_b && b.frame() < frames + 20) { b.step(pb.next()); next_b += fms * 1.003; }
		vw::Feed::Upload up;
		if (begun && !https_feed && !feed_ws && !wid.empty()) {   // (the host's WebSocket, as the game's: open before the first upload)
			feed_ws = std::make_unique<http::Socket>(lobby + "/v1/watch/feed?w=" + wid + "&k=" + wkey);
			for (int k = 0; k < 500 && feed_ws->state() == http::Socket::State::connecting; k++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
			if (feed_ws->state() != http::Socket::State::open)   // (WebSockets switched off, or refused: over HTTPS, as the game)
				printf("  the host's WebSocket wasn't taken (%d %s): its uploads over HTTPS\n", feed_ws->status(), feed_ws->error().c_str());
		}
		auto every = [&](const vfnet::json::Value &v) {   // (as the game's: 5 a second while anyone watches, every 30 s while nobody does; the match over, its last at once)
			const double e = std::clamp(v["every"].kind == vfnet::json::Value::number ? v["every"].n : 200.0, 200.0, 30000.0);
			feed.interval_ms = a.confirmed() >= frames ? std::min(e, 1000.0) : e;
		};
		auto woken = [&](const http::Socket::Message &msg) {   // (the lobby's word that a watcher came: sent at once)
			vfnet::json::Value v;
			if (msg.binary || !vfnet::json::parse(msg.data, v) || !v["wake"].b) return false;
			every(v);
			wakes++;
			return true;
		};
		if (feed_ws && feed_ws->state() == http::Socket::State::open)
			for (http::Socket::Message msg; feed_ws->take(msg); msg = {}) woken(msg);
		if (feed.next(t, up)) {
			vfnet::json::Value v;
			bool good = false;
			if (feed_ws && feed_ws->state() == http::Socket::State::open) {   // (a message, its answer a message back)
				feed_ws->send(std::string(up.body.begin(), up.body.end()));
				http::Socket::Message msg;
				for (int k = 0; k < 3000; k++) {
					if (feed_ws->take(msg)) { if (!woken(msg)) break; msg = {}; continue; }
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				good = !msg.binary && vfnet::json::parse(msg.data, v);
				ws_uploads++;
			} else {
				const http::Reply r = http::request("POST", lobby + "/v1/watch/" + (up.start ? "start" : "push") + "?w=" + wid + "&k=" + wkey, std::string(up.body.begin(), up.body.end()), 5000);
				good = r.status == 200 && vfnet::json::parse(r.body, v);
				requests++;
			}
			feed.done(up, good, v["has"].n < 0 ? vw::NONE : uint32_t(v["has"].n), int(v["watchers"].n), t);
			if (good) every(v);
		}
		// (the room held, as the game's: by its WebSocket, open through the match; over HTTPS every 15 s without one)
		if (!quick && !room_ws && !https_room) {
			room_ws = std::make_unique<http::Socket>(lobby + "/v1/room/ws?code=" + code + "&key=" + key);
			for (int k = 0; k < 500 && room_ws->state() == http::Socket::State::connecting; k++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
			if (room_ws->state() != http::Socket::State::open)   // (switched off, or an older lobby: held over HTTPS, as the game)
				printf("  the room's WebSocket wasn't taken (%d): the room held by /v1/hold every 15 s\n", room_ws->status());
		}
		if (room_ws) for (http::Socket::Message msg; room_ws->take(msg); msg = {}) {}
		if (!quick && (https_room || (room_ws && room_ws->state() != http::Socket::State::open)) && t >= hold_at && a.frame() < frames) { hold_at = t + 15000; http::request("POST", lobby + "/v1/hold", "{\"code\":" + quote(code) + ",\"key\":" + quote(key) + "}", 5000); requests++; }
		for (auto &wp : ws) {
			W &w = *wp;
			if (t < w.join) continue;
			if (w.join_frame == vw::NONE) w.join_frame = a.confirmed();   // (where the players were when it came)
			if (w.sock && w.sock->state() == http::Socket::State::open) w.seen_at = t + 15000;   // (counted by its socket: no /seen)
			if (t >= w.seen_at) {
				w.seen_at = t + (w.match ? 15000 : 2000);
				vfnet::json::Value v;
				const http::Reply r = http::request("POST", lobby + "/v1/watch/seen?w=" + wid + "&s=" + w.sid, "", 5000);
				if (r.status == 200 && vfnet::json::parse(r.body, v)) {
					if (v["match"].str().size() == 16) w.match = strtoull(v["match"].str().c_str(), nullptr, 16);
					if (v["random"].kind != vfnet::json::Value::boolean || v["random"].b != quick) say_quick = false;   // (a Quick match's, its watchers told so: no Play next)
				}
			}
			if (!w.match) continue;
			if (w.ws && !w.sock && w.sp->state() != vw::Spectator::State::waiting) {   // (its WebSocket, as the game's: the frames from where it is, then as they come)
				w.sock = std::make_unique<http::Socket>(lobby + "/v1/watch/ws?w=" + wid + "&m=" + vw::hex(w.match) + "&from=" + std::to_string(w.sp->ask_from()) + "&s=" + w.sid);
				for (int k = 0; k < 500 && w.sock->state() == http::Socket::State::connecting; k++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
				if (w.sock->state() == http::Socket::State::open) w.sp->live = true;
				else {   // (WebSockets switched off, or refused: the HTTPS stream, as the game)
					printf("  watcher %s: its WebSocket wasn't taken (%d): the HTTPS stream\n", w.sid.c_str(), w.sock->status());
					w.sock.reset();
					w.ws = false;
					w.stream = true;
				}
			}
			if (w.sock) {
				http::Socket::Message msg;
				w.sock->take(msg);
				do {
					if (msg.data.empty()) continue;
					if (msg.binary) { vw::Reply r; if (vw::decode_reply(std::vector<uint8_t>(msg.data.begin(), msg.data.end()), r)) { w.sp->reply(r, t); w.streamed++; } }
					else { vfnet::json::Value v; if (vfnet::json::parse(msg.data, v) && v["watchers"].kind == vfnet::json::Value::number && (v["random"].kind != vfnet::json::Value::boolean || v["random"].b != quick)) say_quick = false; }
					msg = {};
				} while (w.sock->take(msg));
			}
			if (w.stream && !w.box && w.sp->state() != vw::Spectator::State::waiting) {
				w.box = std::make_shared<Box>();
				w.sp->live = true;
				const std::string url = lobby + "/v1/watch/live?w=" + wid + "&m=" + vw::hex(w.match) + "&from=0&s=" + w.sid;
				w.reader = std::thread([box = w.box, url] {
					std::vector<uint8_t> buf;
					http::stream(url, [&](const char *p, size_t n) {
						if (box->stop) return false;
						buf.insert(buf.end(), p, p + n);
						size_t at = 0;
						while (buf.size() - at >= 4) {
							uint32_t len = 0;
							memcpy(&len, buf.data() + at, 4);
							if (buf.size() - at - 4 < len) break;
							if (len) { std::lock_guard<std::mutex> l(box->lock); box->got.emplace_back(buf.begin() + ptrdiff_t(at) + 4, buf.begin() + ptrdiff_t(at + 4 + len)); }
							at += 4 + len;
						}
						buf.erase(buf.begin(), buf.begin() + ptrdiff_t(at));
						return true;
					}, 5000, 15000);
					box->done = true;
				});
			}
			if (w.box) {
				std::deque<std::vector<uint8_t>> got;
				{ std::lock_guard<std::mutex> l(w.box->lock); got.swap(w.box->got); }
				for (auto &m : got) { vw::Reply r; if (vw::decode_reply(m, r)) { w.sp->reply(r, t); w.streamed++; } }
			}
			const std::string want = w.sp->want(t);
			if (!want.empty()) {
				w.sp->asked(t);
				const std::string q = "?w=" + wid + "&m=" + vw::hex(w.match);
				const http::Reply r = http::request("GET", lobby + "/v1/watch/" + (want == "start" ? "start" + q : "frames" + q + "&from=" + std::to_string(w.sp->ask_from())), "", 5000, 8u << 20);
				const std::vector<uint8_t> body(r.body.begin(), r.body.end());
				if (want == "start") { vw::Start st; if (r.status == 200 && vw::decode(body, st)) w.sp->start(st); }
				else if (want == "frames") { vw::Reply rr; if (r.status == 200 && vw::decode_reply(body, rr)) w.sp->reply(rr); }
				w.sp->answered(t);
				requests++;
			}
			w.sp->run(t);
		}
		if (t > 1000 + 20000 && std::fmod(t, 20000.0) < 1 && a.frame() < frames) {   // (the rooms list, as a game's browser and the site read it)
			vfnet::json::Value v;
			const http::Reply r = http::request("GET", lobby + "/v1/online", "", 5000);
			bool listed = false;
			if (r.status == 200 && vfnet::json::parse(r.body, v))
				for (const vfnet::json::Value &x : v["rooms"].items)
					listed = listed || (quick ? x["random"].b && x["code"].str().empty() && x["state"].str() == "playing" && x["p1"]["name"].str() == "HOST" && x["p2"]["name"].str() == "GUEST" && x["watch"].str() == wid
					                          : x["code"].str() == code && x["state"].str() == "playing" && x["name"].str() == "SIM ROOM" && x["p2"]["name"].str() == "GUEST" && x["watch"].str() == wid);
			const bool want = has_rooms && has_watch;   // (a match on is listed only to be watched)
			if (listed != want) fail(std::string(!want ? "listed, with rooms or watching off," : quick ? "the Quick match isn't listed to watch, with both players and no code," : "the room isn't listed as playing, with its name, both players and its watch code,") + " at " + std::to_string(int((t - 1000) / 1000)) + " s");
			else if (std::fmod(t, 60000.0) < 1) printf("  %s at %d s%s\n", want ? "listed" : "not listed (as the lobby says)", int((t - 1000) / 1000), !want ? "" : quick ? ": a Quick match, both players, to watch only" : ": the room, playing");
		}
	}
	// (the stream's last frames: a moment for them, then it's let go)
	for (int k = 0; k < 100; k++) {
		bool all = true;
		for (auto &wp : ws) if (wp->box || wp->sock) {
			std::deque<std::vector<uint8_t>> got;
			if (wp->box) { std::lock_guard<std::mutex> l(wp->box->lock); got.swap(wp->box->got); }
			for (http::Socket::Message msg; wp->sock && wp->sock->take(msg);) if (msg.binary) got.emplace_back(msg.data.begin(), msg.data.end());
			for (auto &m : got) { vw::Reply r; if (vw::decode_reply(m, r)) { wp->sp->reply(r, 0); wp->streamed++; } }
			for (int q = 0; q < 2000; q++) wp->sp->tick();
			all = all && wp->sp->frame() + 60 >= std::min(a.confirmed(), b.confirmed());
		}
		if (all) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	for (auto &wp : ws) if (wp->box) { wp->box->stop = true; wp->reader.join(); }
	const uint32_t confirmed = std::min(a.confirmed(), b.confirmed());
	if (!wid.empty() && feed.acked() + 60 < confirmed) fail("the lobby has " + std::to_string(feed.acked()) + " frames of " + std::to_string(confirmed));
	int good = 0;
	for (auto &wp : ws) {
		W &w = *wp;
		const uint32_t ran = w.sp->frame();
		bool same = ran + 60 >= confirmed;
		for (uint32_t f = 0; f < ran && f < confirmed && same; f++) same = f < w.game.after.size() && w.game.after[f] == ga.after[f];
		// (one come late ran the match from its start: every one of the players' hashes made before it came checked; those
		// made after, a few frames after their frames, reach a watcher at the live edge only if their frames haven't
		// gone to it yet, as for everyone watching live)
		uint32_t hashes_there = 0;
		for (const auto &[fr, hh] : made) hashes_there += fr + 60 < w.join_frame && fr + vw::HASH_PARTS < ran;
		const bool came_late = w.join >= 1000 + late_s * 1000 + 30000, all_hashes = !came_late || w.sp->hashes_checked >= hashes_there;
		if (same && w.sp->hashes_checked > 0 && all_hashes) good++;
		else fail("watcher " + w.sid + ": frame " + std::to_string(ran) + " of " + std::to_string(confirmed) + ", " + std::to_string(w.sp->hashes_checked) + " hashes checked (" + std::to_string(hashes_there) + " made before it came), state " + std::to_string(int(w.sp->state())));
		printf("  watcher %s: %u frames from the start%s, %u of the players' hashes matched%s\n", w.sid.c_str(), ran,
		       came_late ? (" (came late at frame " + std::to_string(w.join_frame) + ": unseen to where the players were, all " + std::to_string(hashes_there) + " hashes made before checked)").c_str() : "", w.sp->hashes_checked,
		       w.ws ? (", " + std::to_string(w.streamed) + " messages on its WebSocket").c_str() : w.stream ? (", " + std::to_string(w.streamed) + " messages on the HTTPS stream").c_str() : ", asking for pieces");
		if ((w.stream || w.ws) && w.streamed < 10) fail("the stream brought almost nothing");
	}
	if (n_watchers && !say_quick) fail(std::string("a watcher wasn't told rightly whether it's a Quick match's (") + (quick ? "it is" : "it isn't") + ")");
	printf("  through the lobby: %d of %d watchers saw the players' game frame for frame; the host sent %llu uploads (%llu bytes, %llu on its WebSocket, woken %llu times), the players made %zu hashes; %llu requests in all\n",
	       good, n_watchers, (unsigned long long)feed.uploads, (unsigned long long)feed.bytes, (unsigned long long)ws_uploads, (unsigned long long)wakes, made.size(), (unsigned long long)requests);
	return ok;
}

// The messages and a watcher's own checks, alone
bool unit()
{
	bool ok = true;
	auto check = [&](const char *what, bool v) { if (!v) { printf("  FAIL %s\n", what); ok = false; } else if (verbose) printf("  ok   %s\n", what); };
	vw::Start s;
	s.match = 0x1234abcd5678ef01ull;
	s.meta = { "vf3", "v0.1.7-online-preview+host1", "0123456789abcdef", "v0.1.18", { "MRFINSTER", "DENVERDAN" }, { "FR", "US" } };
	s.blob = std::vector<uint8_t>(5000, 7);
	vw::Start s2;
	check("start: there and back", vw::decode(vw::encode(s), s2) && s2.match == s.match && s2.meta.name[1] == "DENVERDAN" && s2.meta.country[0] == "FR" && s2.blob == s.blob);
	std::vector<uint8_t> bad = vw::encode(s);
	bad.resize(bad.size() - 1);
	check("start: cut short, refused", !vw::decode(bad, s2));
	vw::Frames f;
	f.match = s.match;
	f.first = 10;
	f.in = { 1, 2, 3, 4, 5, 6 };
	f.hashes = { { 120, 0xfedcba9876543210ull } };
	f.info.wins[1] = 2;
	f.info.flags = 1;
	vw::Frames f2;
	check("push: there and back", vw::decode_push(vw::encode_push(f), f2) && f2.first == 10 && f2.in == f.in && f2.hashes == f.hashes && f2.info == f.info);
	std::vector<uint8_t> big = vw::encode_push(f);
	big[18] = 0xff; big[19] = 0xff;   // (n: a lie)
	check("push: a count past what's there, refused", !vw::decode_push(big, f2));
	// the session's own hash of a state is a watcher's (state_hash): a match played, its hashes against a straight run's
	vfnet::SimWire wire({ 20, 5, 0 }, { 20, 5, 0 }, 3);
	Toy ga, gb, straight;
	std::vector<uint8_t> blob(100, 1);
	vfnet::Config cfg;
	cfg.checksum_every = 30;
	vfnet::Session a(wire.a(), ga, { "toy", "v1", 42, "A" }, cfg, true, blob), b(wire.b(), gb, { "toy", "v1", 42, "B" }, cfg, false);
	Player pa(1), pb(2);
	std::map<uint32_t, uint64_t> got;
	for (double t = 0; t < 30000 && a.frame() < 1500; t += 1) {
		wire.now = t;
		a.poll(t);
		b.poll(t);
		if (std::fmod(t, 17.0) < 1) { a.step(pa.next()); b.step(pb.next()); }
		for (auto &[fr, h] : a.hashes()) got[fr] = h;
	}
	straight.begin(blob);
	int matched = 0;
	std::vector<uint8_t> st;
	for (uint32_t fr = 0; fr < a.confirmed(); fr++) {
		if (auto it = got.find(fr); it != got.end()) { straight.save(st); matched += vw::state_hash(st, 0) == it->second ? 1 : -1000; }
		const uint16_t in[2] = { a.inputs(0)[fr], a.inputs(1)[fr] };
		straight.advance(in, false);
	}
	check("a watcher's state hash is the session's (every hash the host made)", matched > 20);
	// a snapshot (a late watcher drops straight in): the host's state at a confirmed frame, copied off its thread while
	// the session keeps it, sent in parts; a watcher loading it runs on from there as the players did, frame for frame
	{
		vw::Snapper sn;
		sn.part_bytes = 16;   // (the toy's state in a few parts)
		sn.min_gap_ms = 0;
		sn.want(30000);
		std::vector<uint8_t> up;
		bool whole = false;
		uint32_t parts = 0;
		for (double t = 30000; t < 90000 && !whole; t += 1) {
			wire.now = t;
			a.poll(t);
			b.poll(t);
			if (std::fmod(t, 17.0) < 1) {
				sn.before_step(a);
				a.step(pa.next());
				b.step(pb.next());
				sn.step(a, ga, s.match, t);
			}
			vw::Snapper::Part pt;
			if (sn.next(t, pt)) {
				if (pt.index == 0) up.clear();
				up.insert(up.end(), pt.body.begin(), pt.body.end());
				parts++;
				sn.done(pt, true, 5, t);
				whole = pt.index + 1 == pt.count;
			}
			if (sn.busy()) std::this_thread::yield();
		}
		vw::Snap got;
		const bool read = whole && vw::decode_snap(up, got);
		check("a snapshot: made from a confirmed state, sent in its parts, read", read && got.match == s.match && got.format == 0x70f && got.frame >= 1400 && got.frame <= a.confirmed() && parts > 1);
		// the watcher: its start, then the snapshot; the match from there with the players' inputs, the host's state frame
		// for frame (the host's last run of each, every one of them confirmed)
		for (double t = 90000; t < 95000; t += 1) { wire.now = t; a.poll(t); b.poll(t); if (std::fmod(t, 17.0) < 1) { a.step(pa.next()); b.step(pb.next()); } }
		Toy w;
		vw::Spectator sp(w, vw::Spectator::Config{});
		vw::Start st0 = s;
		st0.blob = blob;
		bool same = read && sp.start(st0) && sp.snapshot(got) && sp.frame() == got.frame && sp.snapped_at() == got.frame;
		uint32_t ran = 0;
		for (uint32_t fr = got.frame; same && fr < a.confirmed(); fr++, ran++) {
			const uint16_t in[2] = { a.inputs(0)[fr], a.inputs(1)[fr] };
			w.advance(in, false);
			same = w.after[fr] == ga.after[fr];
		}
		check("a watcher from the snapshot: the host's state after every frame from there", same && ran > 200);
		// refused, the game as it was: behind the watcher, another format, a hash that isn't its state's
		Toy w2;
		vw::Spectator sp2(w2, vw::Spectator::Config{});
		sp2.start(st0);
		vw::Snap other = got;
		other.format = 1;
		const bool fmt = !sp2.snapshot(other) && sp2.why == "another format";
		other = got;
		other.hash ^= 1;
		const bool hash = !sp2.snapshot(other) && sp2.state() != vw::Spectator::State::out_of_step;
		other = got;
		other.match ^= 1;
		const bool match = !sp2.snapshot(other);
		const bool took = sp2.snapshot(got) && !sp2.snapshot(got) && sp2.why == "behind";
		check("a snapshot refused, the watcher as it was: another format, a hash not its state's, another match; one not ahead of it", fmt && hash && match && took);
		// packed and read back, a big one with its extra; cut short or spoilt every way: refused or read within its limits
		vw::Snap big;
		big.match = 9;
		big.frame = 77;
		big.format = 3;
		std::mt19937 r(11);
		big.state.resize(1u << 20);
		for (size_t k = 0; k < big.state.size(); k++) big.state[k] = uint8_t(k % 3 == 0 ? r() : 0);
		big.extra.assign(5000, 0x42);
		big.hash = vw::state_hash(big.state, 4);
		const std::vector<uint8_t> enc = vw::encode_snap(big);
		vw::Snap back;
		check("a snapshot packed and read back, its extra too", vw::decode_snap(enc, back) && back.state == big.state && back.extra == big.extra && back.frame == 77 && back.format == 3 && back.hash == big.hash && enc.size() < big.state.size() / 2);
		vw::Snap none;
		none.match = 9;
		check("a snapshot with no state and no extra: there and back", vw::decode_snap(vw::encode_snap(none), back) && back.state.empty() && back.extra.empty());
		std::vector<uint8_t> cut = enc;
		cut.resize(cut.size() - 1);
		check("a snapshot cut short: refused", !vw::decode_snap(cut, back));
		cut = enc;
		cut.push_back(0);
		check("a snapshot with a byte too many: refused", !vw::decode_snap(cut, back));
		std::vector<uint8_t> lie = enc;
		lie[30] = 0xff; lie[31] = 0xff; lie[32] = 0xff; lie[33] = 0x7f;   // (its state's length: past its limit)
		check("a snapshot saying it's bigger than any may be: refused, nothing made that big", !vw::decode_snap(lie, back));
		const std::vector<uint8_t> small = vw::encode_snap(got);
		int read_ok = 0;
		for (int i = 0; i < 20000; i++) {
			std::vector<uint8_t> b2 = (i & 1) ? small : std::vector<uint8_t>(enc.begin(), enc.begin() + ptrdiff_t(std::min<size_t>(enc.size(), 600)));
			const int how = int(r() % 4);
			if (how == 0 && !b2.empty()) b2[r() % b2.size()] ^= uint8_t(1 + r() % 255);
			else if (how == 1) b2.resize(r() % (b2.size() + 1));
			else if (how == 2) for (int k = 0; k < 4 && !b2.empty(); k++) b2[r() % b2.size()] = 0xFF;
			else b2.insert(b2.begin() + ptrdiff_t(r() % (b2.size() + 1)), uint8_t(r()));
			vw::Snap x;
			if (vw::decode_snap(b2, x)) {
				read_ok++;
				if (x.state.size() > vw::MAX_SNAP_STATE || x.extra.size() > vw::MAX_SNAP_EXTRA) { check("a spoilt snapshot read past its limits", false); break; }
				Toy w3;
				vw::Spectator sp3(w3, vw::Spectator::Config{});
				sp3.start(st0);
				sp3.snapshot(x);   // (taken only if it's whole and its hash its own)
			}
		}
		check("20,000 spoilt snapshots: read or refused, nothing past its limits", true);
		// the snapshot's upload, gentle on the host's connection (the players' packets go on it too): each part's start
		// twice its sending time after the last's at the least (half of what the link carries, each sent before the next
		// starts), none sooner than max_rate allows, the lobby's round trip (heard) not counted as sending, two on their
		// way at once so a long round trip doesn't hold it back; and let go at once, not run on, if at its pace it
		// wouldn't be sent within give_up_ms. A link of link_bps bytes a second, rtt ms there and back, simulated (a part's
		// bytes on the link one after another: its sending can't start before the last one's is done)
		struct Paced { bool whole = false; uint32_t parts = 0, count = 0; int most_on_way = 0; double first_at = -1, last_start = 0, min_gap = 1e18, link_busy = 0, overlap = 0; size_t bytes = 0, last_bytes = 0; };
		double tt = wire.now + 1;
		auto paced = [&](vw::Snapper &p, double link_bps, double rtt) {
			Paced o;
			p.part_bytes = 4;
			p.min_gap_ms = 0;
			p.want(tt);
			struct Way { vw::Snapper::Part pt; double at, due; };
			std::vector<Way> on_way;
			const double end = tt + 600000;
			for (; tt < end; tt += 1) {
				wire.now = tt;
				a.poll(tt);
				b.poll(tt);
				if (std::fmod(tt, 17.0) < 1) {
					p.before_step(a);
					a.step(pa.next());
					b.step(pb.next());
					p.step(a, ga, s.match, tt);
				}
				if (std::fmod(tt, 200.0) < 1) p.heard(rtt, tt);   // (the host's small uploads, 5 a second)
				for (size_t k = 0; k < on_way.size();)
					if (tt >= on_way[k].due) {
						const Way w = on_way[k];
						on_way.erase(on_way.begin() + ptrdiff_t(k));
						p.done(w.pt, true, tt - w.at, tt);
						o.bytes += w.pt.body.size();
						o.last_bytes = w.pt.body.size();
						if (p.sent) o.whole = true;
					} else k++;
				if (o.whole) break;
				vw::Snapper::Part pt;
				while (p.next(tt, pt)) {
					if (o.first_at < 0) o.first_at = tt;
					if (o.parts) o.min_gap = std::min(o.min_gap, tt - o.last_start);
					o.last_start = tt;
					o.count = pt.count;
					o.parts++;
					const double send = double(pt.body.size()) * 1000.0 / link_bps, from = std::max(tt, o.link_busy);
					o.overlap = std::max(o.overlap, from - tt);   // (waiting behind the last part's bytes: a burst)
					o.link_busy = from + send;
					on_way.push_back({ pt, tt, o.link_busy + rtt });
					o.most_on_way = std::max(o.most_on_way, int(on_way.size()));
				}
				if (!p.busy() && o.parts && on_way.empty()) break;   // (let go)
				if (p.busy()) std::this_thread::yield();
			}
			tt += 1000;
			return o;
		};
		auto rate = [](const Paced &o) { return double(o.bytes - o.last_bytes) * 1000.0 / (o.last_start - o.first_at); };   // (start to start)
		{   // a fast link: the cap's pace, 40 bytes a second here (4-byte parts 100 ms apart at the least)
			vw::Snapper p;
			p.max_rate = 40;
			const Paced o = paced(p, 1e6, 5);
			check("a snapshot's upload on a fast link: no faster than its cap, start to start", o.whole && o.parts > 5 && o.min_gap >= 100 && rate(o) <= 40 * 1.02 && p.sent == 1);
			if (verbose) printf("  (fast: %u parts, %.1f bytes/s, start to start %.0f ms at the least)\n", o.parts, rate(o), o.min_gap);
		}
		{   // a slow link (10 bytes a second, under the cap): half of it at most, each part sent before the next starts
			vw::Snapper p;
			p.max_rate = 40;
			const Paced o = paced(p, 10, 5);
			check("... on a slow link: half of what it carries at most, no part waiting behind another's bytes", o.whole && rate(o) <= 10 / 2.0 * 1.02 && o.min_gap >= 799 && o.overlap == 0 && p.sent == 1);
			if (verbose) printf("  (slow: %u parts, %.2f bytes/s on a link of 10, start to start %.0f ms at the least)\n", o.parts, rate(o), o.min_gap);
		}
		{   // the lobby's round trip (300 ms) not taken for sending, and not holding the pace back: two parts on their way
			vw::Snapper p;
			p.max_rate = 0;
			const Paced o = paced(p, 10, 300);
			check("... a long round trip to the lobby: not counted as sending (half the slow link's pace still)", o.whole && o.min_gap >= 799 && rate(o) >= 4.5 && rate(o) <= 5.1 && o.overlap == 0);
			if (verbose) printf("  (300 ms round trip: %.2f bytes/s on a link of 10, start to start %.0f ms at the least)\n", rate(o), o.min_gap);
			vw::Snapper q;
			q.max_rate = 40;
			const Paced o2 = paced(q, 1e6, 300);
			check("... a fast link with a long round trip: two parts on their way, twice one a round trip's pace", o2.whole && o2.most_on_way == 2 && rate(o2) > 2 * 4 / 0.3 * 0.9 && rate(o2) <= 40 * 1.02);
			if (verbose) printf("  (fast, 300 ms round trip: %.1f bytes/s)\n", rate(o2));
		}
		{   // too slow to be sent in time: let go after its first parts, not run on to give_up_ms
			vw::Snapper p;
			p.max_rate = 40;
			p.give_up_ms = 3000;
			const uint32_t was = p.failed;
			const Paced o = paced(p, 2, 5);
			check("... a link too slow to send it within give_up_ms: let go at once (aborted), not run on", !o.whole && p.aborted == 1 && p.failed == was + 1 && !p.busy() && o.parts <= 2 && o.count > 5 && o.last_start - o.first_at < 3000);
		}
		if (verbose) printf("  (the snapshot at frame %u in %u parts; %d spoilt ones still read)\n", got.frame, parts, read_ok);
	}
	// a late watcher: the match from its start, unseen and silent (on_catch_up says when), every one of the players'
	// hashes checked, then shown; silent again catching up after a long spell cut off; and the odd extra frame run
	// unseen to keep up never silenced (its sound handed on late, as a rollback's)
	{
		struct Counting : Toy
		{
			const bool *muted = nullptr;
			uint64_t silent = 0, unseen_heard = 0;
			void advance(const uint16_t in[2], bool resim) override { if (*muted) silent++; else if (resim) unseen_heard++; Toy::advance(in, resim); }
		};
		const uint32_t N = 3000;
		std::vector<uint16_t> ins;   // (the match: both players' inputs, and the players' hashes every 30 frames, as a straight run has them)
		std::map<uint32_t, uint64_t> hs;
		Player qa(7), qb(8);
		Toy run;
		run.begin(blob);
		std::vector<uint8_t> st;
		for (uint32_t fr = 0; fr < N; fr++) {
			if (fr % 30 == 0 && fr) { run.save(st); hs[fr] = vw::state_hash(st, 0); }
			const uint16_t in[2] = { qa.next(), qb.next() };
			ins.push_back(in[0]);
			ins.push_back(in[1]);
			run.advance(in, false);
		}
		bool muted = false;
		int said_on = 0, said_off = 0;
		Counting t;
		t.muted = &muted;
		vw::Spectator sp(t, vw::Spectator::Config{});
		sp.on_catch_up = [&](bool on) { if (on == muted) said_on = -1000; muted = on; (on ? said_on : said_off)++; };
		vw::Start st0 = s;
		st0.blob = blob;
		auto give = [&](uint32_t from, uint32_t to, double now) {
			vw::Reply rr;
			rr.match = rr.frames.match = st0.match;
			rr.state = 1;
			rr.total = to;
			rr.frames.first = from;
			rr.frames.in.assign(ins.begin() + ptrdiff_t(from) * 2, ins.begin() + ptrdiff_t(to) * 2);
			for (const auto &[fr, h] : hs) if (fr >= from && fr <= to) rr.frames.hashes.push_back({ fr, h });
			sp.reply(rr, now);
		};
		auto checked = [&](uint32_t upto) { uint32_t n = 0; for (const auto &[fr, h] : hs) n += fr + vw::HASH_PARTS < upto; return n; };
		const bool taken = sp.start(st0);
		give(0, 1000, 0);   // (come late: 1,000 frames there already)
		double now = 0;
		for (; now < 60000 && sp.state() != vw::Spectator::State::playing; now += 1) sp.run(now);
		const uint32_t shown_at = sp.frame();
		const uint64_t silent_then = t.silent;
		check("a late watcher: from the match's start, unseen and silent, to where the players are, every one of their hashes checked",
		      taken && sp.state() == vw::Spectator::State::playing && shown_at + 60 >= 1000 && silent_then + 1 >= shown_at && said_on > 0 && said_on == said_off &&
		      !muted && sp.hashes_checked == checked(sp.frame()));
		// (on, a frame at a time, as they come; then nothing for 12 s, then all of it at once: far behind, a jump)
		uint32_t had = 1000;
		for (; now < 70000; now += 1) {
			if (std::fmod(now, 200.0) < 1 && had < 1500) { const uint32_t to = std::min<uint32_t>(1500, had + 12); give(had, to, now); had = to; }
			sp.run(now);
		}
		for (; now < 82000; now += 1) sp.run(now);
		give(had, N, now);
		const int on_before = said_on;
		for (; now < 200000 && sp.frame() + 30 < N; now += 1) sp.run(now);
		bool same = true;
		for (uint32_t fr = 0; fr < sp.frame() && same; fr++) same = fr < t.after.size() && t.after[fr] == run.after[fr];
		check("after a long spell cut off: caught up again unseen and silent, its frames the players'", sp.jumps >= 1 && said_on > on_before && said_on == said_off && !muted && same &&
		      t.silent > silent_then + 500);
		check("the odd frame run unseen to keep up isn't silenced; every other unseen one is", t.unseen_heard == sp.extra_frames && t.silent + sp.extra_frames == sp.frames_unseen);
		check("and every one of the players' hashes checked, from the start to the end", sp.hashes_checked == checked(sp.frame()) && sp.state() != vw::Spectator::State::out_of_step);
		if (verbose)
			printf("  (the late watcher: shown from frame %u, %llu frames silent, %llu unseen, %llu extra, %llu jumps, %u hashes checked)\n", shown_at, (unsigned long long)t.silent,
			       (unsigned long long)sp.frames_unseen, (unsigned long long)sp.extra_frames, (unsigned long long)sp.jumps, sp.hashes_checked);
	}
	// asking for the frames (no stream): at once while catching up and each answer brings more; after one that failed
	// (nothing came: the lobby busy, down, or a reply that couldn't be read) not again at once, but poll_ms later, then
	// twice that each time it fails again, to retry_most_ms; and a late one's catching up not taken for jitter
	{
		const double FMS = vw::Spectator::Config{}.frame_ms;
		vw::Start st0 = s;
		st0.blob = blob;
		auto piece = [&](uint32_t from, uint32_t total) {   // (the lobby's answer: from there, at most a reply's frames)
			vw::Reply rr;
			rr.match = rr.frames.match = st0.match;
			rr.state = 1;
			rr.total = total;
			rr.frames.first = std::min(from, total);
			rr.frames.in.assign(size_t(std::min(total - rr.frames.first, vw::MAX_REPLY)) * 2, 1);
			return rr;
		};
		Toy t;
		vw::Spectator sp(t, vw::Spectator::Config{});
		const double poll = vw::Spectator::Config{}.poll_ms, most = vw::Spectator::Config{}.retry_most_ms;
		sp.start(st0);
		sp.asked(0);
		sp.reply(piece(0, 5000), 50);
		sp.answered(50);
		bool fast = sp.want(51) == "frames";   // (more there, and that one brought some: at once)
		// the lobby failing from now on: each ask's answer nothing at all
		std::vector<double> gaps;
		double now = 51, last = 51;
		int asks = 0;
		for (; now < 61000; now += 1) {
			if (sp.want(now) != "frames") continue;
			gaps.push_back(now - last);
			sp.asked(now);
			sp.answered(now + 30);
			last = now + 30;
			asks++;
		}
		bool backs_off = gaps.size() >= 4 && gaps[0] < 2;
		for (size_t i = 1; i < gaps.size(); i++) backs_off = backs_off && gaps[i] >= std::min(most, poll * std::pow(2.0, double(i - 1))) && gaps[i] <= std::min(most, poll * std::pow(2.0, double(i - 1))) + 2;
		check("a failed ask for the frames: the next not at once, but a second later, then less often, to every 10 s", backs_off && asks < 12);
		// the lobby back: the next answer that brings frames, and the rest of the catching up at once again
		sp.asked(now);
		sp.reply(piece(sp.ask_from(), 5000), now + 30);
		sp.answered(now + 30);
		fast = fast && sp.want(now + 31) == "frames";
		sp.asked(now + 31);
		sp.reply(piece(sp.ask_from(), 5000), now + 60);   // (all of it now: caught up, poll_ms till the next)
		sp.answered(now + 60);
		check("an answer that brings frames: the next at once while there's more, poll_ms once caught up", fast && sp.have() == 5000 && sp.want(now + 61).empty() && sp.want(now + 60 + poll) == "frames");
		// a late watcher asking (no stream): the players 75 s in, each answer 60 ms after the ask, its catching up fast and
		// its buffer the link's, not the backlog's (a reply short of the lobby's last frame says nothing of its jitter)
		Toy t2;
		vw::Spectator late(t2, vw::Spectator::Config{});
		late.start(st0);
		bool pending = false, caught = false;
		double ask_at = 0, caught_at = -1, most_buffer = 0, most_behind = 0;
		int catch_asks = 0;
		for (now = 75000; now < 105000; now += 1) {
			const uint32_t lobby = uint32_t(now / FMS);
			if (pending && now >= ask_at + 60) { late.reply(piece(late.ask_from(), lobby), now); late.answered(now); pending = false; }
			if (!pending && late.want(now) == "frames") { late.asked(now); ask_at = now; pending = true; catch_asks += !caught; }
			late.run(now);
			if (!caught && late.state() == vw::Spectator::State::playing) { caught = true; caught_at = now; }
			if (caught && now > caught_at + 3000) {
				most_buffer = std::max(most_buffer, late.buffer_ms());
				most_behind = std::max(most_behind, double(lobby - std::min(lobby, late.frame())) * FMS);
			}
		}
		check("a late watcher asking: caught up at once (an ask after each answer), then as far behind as its asking makes it", caught && caught_at < 75000 + 3000 && catch_asks <= 4 && most_buffer < 1000 && most_behind < 2500);
		if (verbose)
			printf("  (failing: %d asks in 61 s; the late one: shown %.1f s in after %d asks, from 3 s after its buffer at most %.0f ms, %.0f ms behind)\n", asks, (caught_at - 75000) / 1000, catch_asks, most_buffer, most_behind);
	}
	// a lobby (or anyone between) sending anything: every message spoilt every way, read without a crash or a lie
	{
		std::mt19937 rng(5);
		vw::Reply rp;
		rp.match = 7; rp.state = 1; rp.total = 300; rp.watchers = 3;
		rp.frames.first = 256; rp.frames.in.assign(88, 5); rp.frames.hashes = { { 256, 1 }, { 300, 2 } };
		const std::vector<std::vector<uint8_t>> good = { vw::encode(s), vw::encode_push(f), vw::encode_reply(rp) };
		Toy t;
		vw::Spectator sp(t, vw::Spectator::Config{});
		sp.start(s);
		int read = 0;
		for (int i = 0; i < 40000; i++) {
			std::vector<uint8_t> b = good[size_t(i) % good.size()];
			const int how = int(rng() % 4);
			if (how == 0 && !b.empty()) b[rng() % b.size()] ^= uint8_t(1 + rng() % 255);
			else if (how == 1) b.resize(rng() % (b.size() + 1));
			else if (how == 2) for (int k = 0; k < 4 && !b.empty(); k++) b[rng() % b.size()] = 0xFF;
			else b.insert(b.begin() + ptrdiff_t(rng() % (b.size() + 1)), uint8_t(rng()));
			vw::Start s3; vw::Frames f3; vw::Reply r3;
			read += vw::decode(b, s3) + vw::decode_push(b, f3);
			if (vw::decode_reply(b, r3)) { read++; sp.reply(r3); }
			if (r3.frames.count() > vw::MAX_REPLY || f3.count() > vw::MAX_PUSH) { check("a spoilt message read past its limits", false); break; }
		}
		check("40,000 spoilt messages: read or refused, nothing past its limits", true);
		if (verbose) printf("  (%d of them still read as well formed)\n", read);
	}
	if (verbose) printf("  (%d of the session's hashes made again by a watcher)\n", matched);
	return ok;
}

} // namespace

int main(int argc, char **argv)
{
	int n = -1;
	double seconds = 150;
	std::string lobby;
	bool quick = false, all_ws = false, https_feed = false, https_room = false;
	double late_s = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verbose")) verbose = true;
		else if (!strcmp(argv[i], "--watchers") && i + 1 < argc) n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_watcher = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--lobby") && i + 1 < argc) lobby = argv[++i];
		else if (!strcmp(argv[i], "--quick")) quick = true;
		else if (!strcmp(argv[i], "--all-ws")) all_ws = true;
		else if (!strcmp(argv[i], "--https-feed")) https_feed = true;
		else if (!strcmp(argv[i], "--https-room")) https_room = true;
		else if (!strcmp(argv[i], "--late") && i + 1 < argc) late_s = atof(argv[++i]);
	}
	bool ok = unit();
	printf("watchsim: the messages and the hashes %s\n", ok ? "ok" : "FAILED");
	if (!lobby.empty()) {
		ok = play_lobby(lobby, n >= 0 ? n : 3, seconds, quick, all_ws, https_feed, https_room, late_s) && ok;
	} else if (n >= 0) {
		play(n, seconds, ok);
	} else {
		bool o0 = true, o1 = true, o50 = true;
		const Result r0 = play(0, seconds, o0), r1 = play(1, seconds, o1), r50 = play(50, seconds, o50);
		ok = ok && o0 && o1 && o50;
		const bool same = r0.same_as(r1) && r0.same_as(r50);
		// the lobby gone mid-match (60 s in): the players' match the same as with it there
		bool og = true;
		const Result rg = play(10, seconds, og, 60000);
		bool gone_same = true;
		for (int p = 0; p < 2; p++)
			gone_same = gone_same && rg.frames[p] == r0.frames[p] && rg.rollbacks[p] == r0.rollbacks[p] && rg.resim[p] == r0.resim[p] && rg.stalls[p] == r0.stalls[p] &&
			            rg.waits[p] == r0.waits[p] && rg.wire_bytes[p] == r0.wire_bytes[p];
		printf("  the lobby gone mid-match: the players' match %s\n", gone_same ? "the same in every count (frames, rollbacks, frames run again, held back, waits, bytes to each other)" : "DIFFERENT");
		ok = ok && gone_same;
		printf("  the players with 0, 1 and 50 watchers: %s\n", same ? "the same in every count (frames, rollbacks, frames run again, held back, waits, bytes to each other and to the lobby)" : "DIFFERENT");
		const double base = (r0.step_ms[0] + r0.step_ms[1]) / 2, with = (r50.step_ms[0] + r50.step_ms[1]) / 2;
		printf("  a player's step: %.4f ms with none, %.4f with 50 (the 50 watchers' games run in this same process: the difference is the machine's)\n", base, with);
		ok = ok && same;
	}
	printf(ok ? "all passed\n" : "FAILED\n");
	return ok ? 0 : 1;
}
