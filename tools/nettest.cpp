// nettest: the netplay session (src/net) on a simulated network, with a small deterministic game standing in for
// the real one, so it runs anywhere without a ROM. Each case plays two sessions against each other through
// SimWire (latency, jitter, loss, cut cables), with inputs that change every few frames as a fighting game's do,
// and checks: both sides agree on every input, the state each side ended on for each frame is the state a straight
// run of those inputs gives (so every rollback put things right), and what should happen did (a desync found, a
// mismatch refused, a cut cable noticed). Prints what the rollback did in each.
//   nettest [--verbose]
#include "../src/net/session.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {

// The stand-in: two fighters' positions and a mixing word, all changed by every input bit
struct Toy : vfnet::Game
{
	struct S { uint64_t mix; int32_t x[2], y[2]; uint32_t frame; };
	S s{};
	std::vector<uint64_t> after;   // per frame: the hash after it, the last time it ran
	uint32_t corrupt_at = UINT32_MAX;   // (a desync's test: this frame's result spoilt, as a nondeterministic game would)
	uint64_t frames_run = 0, resims = 0;
	std::vector<uint8_t> began_with;

	bool begin(const std::vector<uint8_t> &blob) override
	{
		began_with = blob;
		s = {};
		s.mix = vfnet::hash64(blob.data(), blob.size());
		return true;
	}
	void save(std::vector<uint8_t> &st) override { st.resize(sizeof s); memcpy(st.data(), &s, sizeof s); }
	bool load(const std::vector<uint8_t> &st) override
	{
		if (st.size() != sizeof s) return false;
		memcpy(&s, st.data(), sizeof s);
		return true;
	}
	void advance(const uint16_t in[2], bool resim) override
	{
		for (int p = 0; p < 2; p++) {
			s.x[p] += (in[p] & 1) - ((in[p] >> 1) & 1);
			s.y[p] += ((in[p] >> 2) & 1) - ((in[p] >> 3) & 1);
			s.mix = (s.mix ^ (uint64_t(in[p]) << (p * 16)) ^ uint64_t(uint32_t(s.x[p] * 31 + s.y[p]))) * 0x9E3779B97F4A7C15ull;
			s.mix ^= s.mix >> 29;
		}
		if (s.frame == corrupt_at) s.mix ^= 1;
		if (after.size() <= s.frame) after.resize(size_t(s.frame) + 1);
		s.frame++;
		after[s.frame - 1] = hash_now();
		frames_run++;
		resims += resim;
	}
	uint64_t hash_now() const { return vfnet::hash64(reinterpret_cast<const uint8_t *>(&s), sizeof s); }
};

// A player: holds an input for a few frames, then another (seeded: each case the same every run)
struct Player
{
	std::mt19937 rng;
	uint16_t held = 0;
	int left = 0;
	explicit Player(uint32_t seed) : rng(seed) {}
	uint16_t next()
	{
		if (left-- <= 0) { held = uint16_t(rng() & 0x3FF); left = int(rng() % 12); }
		return held;
	}
};

struct Case
{
	std::string name;
	vfnet::SimWire::Params ab, ba;
	uint32_t frames = 3000;
	double b_late_ms = 37;       // the guest starts this much later
	double b_slower = 1.003;     // and its frames are this much longer (clocks drift)
	int delay_a = 2, delay_b = 2;
	std::function<void(vfnet::SimWire &, double t, uint32_t fa, uint32_t fb)> during;   // (cuts)
	vfnet::Identity id_b{ "toy", "v1", 42, "GUEST" };
	uint32_t corrupt_b = UINT32_MAX;
	uint32_t redelay_at = UINT32_MAX;   // (at this frame the host's delay becomes redelay_a, the guest's redelay_b)
	int redelay_a = 0, redelay_b = 0;
	size_t blob = 1000;
	// what should happen
	vfnet::Session::End expect_a = vfnet::Session::End::none, expect_b = vfnet::Session::End::none;
	bool expect_interrupt = false;
	bool expect_host_untouched = false;   // (the host's game never started: a guest that never got far enough)
};

bool verbose = false;

bool run(const Case &c)
{
	vfnet::SimWire wire(c.ab, c.ba, 7);
	Toy ga, gb;
	gb.corrupt_at = c.corrupt_b;
	std::vector<uint8_t> blob(c.blob);
	for (size_t i = 0; i < blob.size(); i++) blob[i] = uint8_t(i * 7 + 3);
	vfnet::Config ca, cb;
	ca.delay = c.delay_a;
	cb.delay = c.delay_b;
	vfnet::Session a(wire.a(), ga, { "toy", "v1", 42, "HOST" }, ca, true, blob);
	vfnet::Session b(wire.b(), gb, c.id_b, cb, false);
	Player pa(11), pb(22);
	const double fa_ms = ca.frame_ms, fb_ms = cb.frame_ms * c.b_slower;
	double t = 1000, next_a = 1000, next_b = 1000 + c.b_late_ms;
	bool interrupted = false;
	while (t < 1000 + 1e6) {
		wire.now = t;
		if (c.during) c.during(wire, t, a.frame(), b.frame());
		// (the network polled every millisecond, as the game does while it waits for the next frame's time)
		a.poll(t);
		b.poll(t);
		if (a.frame() == c.redelay_at && a.delay() != c.redelay_a) a.set_delay(c.redelay_a);
		if (b.frame() == c.redelay_at && b.delay() != c.redelay_b) b.set_delay(c.redelay_b);
		if (t >= next_a) { a.step(pa.next()); next_a += fa_ms; }
		if (t >= next_b) { b.step(pb.next()); next_b += fb_ms; }
		vfnet::Session::Event e;
		while (a.next_event(e)) { if (e.kind == e.interrupted) interrupted = true; if (verbose) printf("    A event %d: %s\n", int(e.kind), e.text.c_str()); }
		while (b.next_event(e)) { if (e.kind == e.interrupted) interrupted = true; if (verbose) printf("    B event %d: %s\n", int(e.kind), e.text.c_str()); }
		const bool done_a = a.state() == vfnet::Session::State::ended || a.frame() >= c.frames;
		const bool done_b = b.state() == vfnet::Session::State::ended || b.frame() >= c.frames;
		if (done_a && done_b) break;
		t += 1;
	}
	// a few more moments for the last inputs to arrive (the frames past `frames` aren't checked)
	bool ok = true;
	auto fail = [&](const std::string &why) { printf("  FAIL %s: %s\n", c.name.c_str(), why.c_str()); ok = false; };
	if (a.end_reason() != c.expect_a) fail("host ended " + std::to_string(int(a.end_reason())) + ", not " + std::to_string(int(c.expect_a)));
	if (b.end_reason() != c.expect_b) fail("guest ended " + std::to_string(int(b.end_reason())) + ", not " + std::to_string(int(c.expect_b)));
	if (c.expect_interrupt && !interrupted) fail("no interruption seen");
	if (c.expect_host_untouched && !ga.began_with.empty()) fail("the host's game was set up for a match that never began");
	// the ping shown: within a few ms of the wire's own (its latency both ways, half the jitter each on average)
	const double true_rtt = c.ab.latency_ms + c.ba.latency_ms + (c.ab.jitter_ms + c.ba.jitter_ms) / 2;
	if (c.expect_a == vfnet::Session::End::none && std::fabs(a.stats().rtt_ms - true_rtt) > 6 + true_rtt * 0.1)
		fail("the ping measured " + std::to_string(a.stats().rtt_ms) + " ms, the wire's " + std::to_string(true_rtt));

	if (c.expect_a == vfnet::Session::End::none) {
		// both sides' inputs the same, and each side's states those of a straight run of them
		const uint32_t n = std::min(a.confirmed(), b.confirmed());
		if (n + 20 < c.frames) fail("only " + std::to_string(n) + " frames confirmed");
		for (uint32_t f = 0; f < n && ok; f++)
			for (int p = 0; p < 2; p++)
				if (a.inputs(p)[f] != b.inputs(p)[f]) { fail("player " + std::to_string(p + 1) + "'s input differs at frame " + std::to_string(f)); break; }
		if (ga.began_with != blob || gb.began_with != blob) fail("the starting blob didn't arrive whole");
		Toy straight;
		straight.begin(blob);
		for (uint32_t f = 0; f < n && ok; f++) {
			const uint16_t in[2] = { a.inputs(0)[f], a.inputs(1)[f] };
			straight.advance(in, false);
			if (ga.after[f] != straight.after[f]) fail("the host's state differs from a straight run after frame " + std::to_string(f));
			else if (gb.after[f] != straight.after[f]) fail("the guest's state differs from a straight run after frame " + std::to_string(f));
		}
	}
	const auto &sa = a.stats(), &sb = b.stats();
	printf("  %-4s %-38s frames %5u/%5u  rtt %5.1f ms  rollbacks %4llu/%4llu (avg %.1f, %.1f frames)  stalls %llu/%llu  waits %llu/%llu  lost %llu/%llu\n",
	       ok ? "ok" : "FAIL", c.name.c_str(), a.frame(), b.frame(), sa.rtt_ms,
	       (unsigned long long)sa.rollbacks, (unsigned long long)sb.rollbacks,
	       sa.rollbacks ? double(sa.resim_frames) / double(sa.rollbacks) : 0.0, sb.rollbacks ? double(sb.resim_frames) / double(sb.rollbacks) : 0.0,
	       (unsigned long long)sa.stalls, (unsigned long long)sb.stalls, (unsigned long long)sa.waits, (unsigned long long)sb.waits,
	       (unsigned long long)wire.lost, (unsigned long long)wire.sent);
	return ok;
}

} // namespace

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--verbose")) verbose = true;
	using P = vfnet::SimWire::Params;
	using E = vfnet::Session::End;
	std::vector<Case> cases;
	cases.push_back({ "LAN (2 ms)", P{ 2, 0, 0 }, P{ 2, 0, 0 } });
	cases.push_back({ "same city (15 ms, jitter 3)", P{ 15, 3, 0 }, P{ 15, 3, 0 } });
	cases.push_back({ "same country (40 ms, jitter 8)", P{ 40, 8, 0 }, P{ 40, 8, 0 } });
	cases.push_back({ "wifi (30 ms, jitter 40, 2% loss)", P{ 30, 40, 0.02 }, P{ 30, 40, 0.02 } });
	cases.push_back({ "bad (70 ms, jitter 20, 10% loss)", P{ 70, 20, 0.10 }, P{ 70, 20, 0.10 } });
	cases.push_back({ "one-sided (15 / 90 ms)", P{ 15, 2, 0 }, P{ 90, 2, 0 } });
	cases.push_back({ "far (150 ms each way)", P{ 150, 5, 0 }, P{ 150, 5, 0 } });
	{
		Case c{ "delay 0 / 4 (30 ms)", P{ 30, 4, 0 }, P{ 30, 4, 0 } };
		c.delay_a = 0; c.delay_b = 4;
		cases.push_back(c);
	}
	{
		Case c{ "delays changed mid-match (2 to 5, 3 to 0)", P{ 30, 4, 0.01 }, P{ 30, 4, 0.01 } };
		c.delay_b = 3;
		c.redelay_at = 1200; c.redelay_a = 5; c.redelay_b = 0;
		cases.push_back(c);
	}
	{
		Case c{ "big starting blob (300 KB, 5% loss)", P{ 40, 10, 0.05 }, P{ 40, 10, 0.05 } };
		c.blob = 300 * 1024;
		cases.push_back(c);
	}
	{
		Case c{ "cable out 3 s, then back", P{ 20, 2, 0 }, P{ 20, 2, 0 } };
		c.during = [](vfnet::SimWire &w, double t, uint32_t, uint32_t) { w.cut(t > 21000 && t < 24000); };
		c.expect_interrupt = true;
		cases.push_back(c);
	}
	{
		Case c{ "cable out for good", P{ 20, 2, 0 }, P{ 20, 2, 0 } };
		c.during = [](vfnet::SimWire &w, double t, uint32_t, uint32_t) { w.cut(t > 21000); };
		c.expect_a = c.expect_b = E::disconnected;
		c.expect_interrupt = true;
		cases.push_back(c);
	}
	{
		Case c{ "desync (the guest's frame 900 spoilt)", P{ 20, 2, 0 }, P{ 20, 2, 0 } };
		c.corrupt_b = 900;
		c.expect_a = c.expect_b = E::desync;
		cases.push_back(c);
	}
	{
		// (a Mac guest's first build: the host heard it, it heard nothing back)
		Case c{ "the host's replies never arrive", P{ 20, 2, 1.0 }, P{ 20, 2, 0 } };
		c.expect_a = c.expect_b = E::timeout;
		c.expect_host_untouched = true;
		cases.push_back(c);
	}
	{
		Case c{ "another version", P{ 20, 2, 0 }, P{ 20, 2, 0 } };
		c.id_b.build = "v2";
		c.expect_a = c.expect_b = E::rejected;
		cases.push_back(c);
	}
	{
		Case c{ "another ROM", P{ 20, 2, 0 }, P{ 20, 2, 0 } };
		c.id_b.content = 43;
		c.expect_a = c.expect_b = E::rejected;
		cases.push_back(c);
	}
	int failed = 0;
	for (const Case &c : cases) failed += !run(c);

	// The UDP link: two sessions in this process on localhost (by IPv4 and by name), 600 frames
	for (const char *where : { "127.0.0.1", "localhost" }) {
		vfnet::UdpLink la, lb;
		std::string err;
		if (!la.bind(0, err) || !lb.connect(where, la.local_port(), err)) {
			printf("  FAIL UDP on %s: %s\n", where, err.c_str());
			failed++;
			continue;
		}
		Toy ga, gb;
		vfnet::Config cfg;
		vfnet::Session a(la, ga, { "toy", "v1", 42, "HOST" }, cfg, true, std::vector<uint8_t>(5000, 1));
		vfnet::Session b(lb, gb, { "toy", "v1", 42, "GUEST" }, cfg, false);
		Player pa(1), pb(2);
		double t = 1000;
		for (int i = 0; i < 1000000 && (a.frame() < 600 || b.frame() < 600); i++, t += 0.05) {
			a.poll(t); b.poll(t);
			if (i % 340 == 0) { a.step(pa.next()); b.step(pb.next()); }
		}
		const bool ok = a.frame() >= 600 && b.frame() >= 600 && a.end_reason() == vfnet::Session::End::none && gb.began_with == ga.began_with;
		printf("  %-4s UDP on %-10s frames %u/%u\n", ok ? "ok" : "FAIL", where, a.frame(), b.frame());
		failed += !ok;
	}
	// Strangers on the host's port: a junk "VFNP" packet first (the host mustn't lock on its sender), then a real guest
	{
		vfnet::UdpLink la, lx, lb;
		std::string err;
		bool ok = la.bind(0, err) && lx.connect("127.0.0.1", la.local_port(), err) && lb.connect("127.0.0.1", la.local_port(), err);
		Toy ga, gb;
		vfnet::Config cfg;
		vfnet::Session a(la, ga, { "toy", "v1", 42, "HOST" }, cfg, true, std::vector<uint8_t>(5000, 1));
		vfnet::Session b(lb, gb, { "toy", "v1", 42, "GUEST" }, cfg, false);
		const uint8_t junk[] = { 'V', 'F', 'N', 'P', 0xEE };
		lx.send(junk, sizeof junk);
		Player pa(1), pb(2);
		double t = 1000;
		for (int i = 0; ok && i < 1000000 && (a.frame() < 300 || b.frame() < 300); i++, t += 0.05) {
			a.poll(t); b.poll(t);
			if (i % 340 == 0) { a.step(pa.next()); b.step(pb.next()); }
		}
		ok = ok && a.frame() >= 300 && b.frame() >= 300;
		printf("  %-4s a stranger's junk first, then the guest: frames %u/%u\n", ok ? "ok" : "FAIL", a.frame(), b.frame());
		failed += !ok;
	}
	// A hello forged with someone else's address (that address never answers): the host sends it a few small
	// welcomes, never its settings, and gives up after its handshake time (so it can wait for a player again)
	{
		vfnet::UdpLink la, lx;
		std::string err;
		bool ok = la.bind(0, err) && lx.connect("127.0.0.1", la.local_port(), err);
		Toy ga;
		vfnet::Config cfg;
		cfg.handshake_ms = 3000;
		vfnet::Session a(la, ga, { "toy", "v1", 42, "HOST" }, cfg, true, std::vector<uint8_t>(60000, 1));
		std::vector<uint8_t> h = { 'V', 'F', 'N', 'P', 1 };
		for (int i = 0; i < 8; i++) h.push_back(uint8_t(0x11 * (i + 1)));   // (a token)
		h.push_back(uint8_t(vfnet::Session::PROTOCOL)); h.push_back(0);
		for (const char *str : { "toy", "v1" }) { h.push_back(uint8_t(strlen(str))); h.insert(h.end(), str, str + strlen(str)); }
		for (int i = 0; i < 8; i++) h.push_back(i ? 0 : 42);   // (the content hash)
		h.push_back(1); h.push_back('X');   // (a name)
		h.push_back(2);                     // (a delay)
		size_t got = 0, blobs = 0;
		std::vector<uint8_t> in;
		double t = 1000;
		for (int i = 0; ok && i < 100000 && a.state() == vfnet::Session::State::handshake; i++, t += 1) {
			if (i % 100 == 0 && i < 1000) lx.send(h.data(), h.size());   // (ten hellos, a second)
			a.poll(t);
			while (lx.recv(in)) { got += in.size(); blobs += in.size() > 4 && in[4] == 4; }
		}
		const size_t sent = h.size() * 10;
		ok = ok && blobs == 0 && got < sent * 4 && a.end_reason() == vfnet::Session::End::timeout;
		printf("  %-4s a forged hello: %zu bytes sent, %zu back, %zu settings packets; the host gave up (%s)\n", ok ? "ok" : "FAIL", sent, got, blobs,
		       a.end_reason() == vfnet::Session::End::timeout ? "timeout" : "not");
		failed += !ok;
	}
	if (failed) printf("%d cases FAILED\n", failed);
	else printf("all %zu cases passed\n", cases.size() + 4);
	return failed ? 1 : 0;
}
