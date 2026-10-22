// delaytest: Auto's input delay (vfnet::auto_delay, vfnet::AutoDelay) against simulated connections: the rule's values
// at each ping; two sessions at every ping from a LAN's to the other side of the world, each driven by AutoDelay,
// held to the game's full speed (57.5 frames a second) with no desync; a ping that jumps mid-match (raised at once)
// and comes back (lowered only between rounds); and a slow machine's (shorter rollbacks, more delay).
//   delaytest [-v]
#include "../src/net/link.h"
#include "../src/net/session.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

struct Toy : vfnet::Game
{
	struct S { uint64_t mix; uint32_t frame; } s{};
	bool begin(const std::vector<uint8_t> &) override { s = {}; return true; }
	void save(std::vector<uint8_t> &st) override { st.resize(sizeof s); memcpy(st.data(), &s, sizeof s); }
	bool load(const std::vector<uint8_t> &st) override { if (st.size() != sizeof s) return false; memcpy(&s, st.data(), sizeof s); return true; }
	void advance(const uint16_t in[2], bool) override { s.mix = (s.mix ^ in[0] ^ (uint64_t(in[1]) << 16)) * 0x9E3779B97F4A7C15ull + s.frame; s.frame++; }
};

bool verbose = false;
int failed = 0;
void check(bool ok, const std::string &what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) failed++;
}

struct Run
{
	double fps = 0;                // side A's frames a second in the measured stretch
	int delay_end = 0, delay_max = 0;
	bool ended = false;
	std::vector<std::pair<double, int>> changes;   // (when, to) side A's delay changes
	std::vector<std::pair<uint32_t, int>> change_frames;   // (at which frame, by how much)
	std::vector<double> lowered_at;                // (when a lowering happened: between rounds?)
	bool lowered_outside_rounds = false;
};

// Two sessions over a wire for `secs`, both on AutoDelay; `wire_at` changes the wire as time goes; rounds end every
// 20 s (the 90 frames after: between rounds); fps measured over [from_s, to_s)
Run run(double secs, std::function<vfnet::SimWire::Params(double)> wire_at, double from_s, double to_s, bool slow = false, double b_fps = 0)
{
	vfnet::SimWire w(wire_at(0), wire_at(0));
	Toy ga, gb;
	vfnet::Identity id{ "vf3", "t", 1, "A" };
	vfnet::Config c;
	c.delay = 2;
	c.soft_sync = true;
	c.checksum_every = 120;
	vfnet::Session a(w.a(), ga, id, c, true, std::vector<uint8_t>(2000, 7)), b(w.b(), gb, id, c, false);
	vfnet::AutoDelay da, db;
	da.slow = db.slow = slow;
	Run r;
	double na = 0, nb = 0;
	double t_start = -1;
	uint32_t f_from = 0, f_to = 0;
	bool have_from = false, have_to = false;
	auto between = [](uint32_t f) { return f > 1200 && f % 1150 < 90; };   // (a round's end about every 20 s)
	for (double t = 1; t < secs * 1000; t += 1) {
		w.now = t;
		const vfnet::SimWire::Params p = wire_at(t / 1000);
		w.set(0, p);
		w.set(1, p);
		a.poll(t);
		b.poll(t);
		if (a.state() == vfnet::Session::State::running && t_start < 0) t_start = t;
		if (t_start >= 0) {
			const double rel = (t - t_start) / 1000;
			if (!have_from && rel >= from_s) { f_from = a.frame(); have_from = true; }
			if (!have_to && rel >= to_s) { f_to = a.frame(); have_to = true; }
		}
		// each side tries a frame every frame time, as the game does (a held-back one: again a moment later)
		if (t >= na) { if (a.step(uint16_t(a.frame() / 7 % 5))) na += c.frame_ms; else na += 1; }
		if (t >= nb) { if (b.step(uint16_t(b.frame() / 11 % 3))) nb += b_fps > 0 ? 1000.0 / b_fps : c.frame_ms; else nb += 1; }
		for (int s = 0; s < 2; s++) {
			vfnet::Session &x = s ? b : a;
			vfnet::AutoDelay &d = s ? db : da;
			const bool br = between(x.frame());
			const int was = x.delay(), n = d.update(x, t, br);
			if (n >= 0 && n != was) {
				x.set_delay(n);
				if (s == 0) {
					r.changes.push_back({ t / 1000, n });
					r.change_frames.push_back({ x.frame(), n - was });
					if (verbose) printf("    %.1f s: delay %d -> %d (%s)\n", t / 1000, was, n, d.why.c_str());
					if (n < was) { r.lowered_at.push_back(t / 1000); if (!br) r.lowered_outside_rounds = true; }
				}
			}
		}
		r.delay_max = std::max(r.delay_max, a.delay());
		if (a.state() == vfnet::Session::State::ended || b.state() == vfnet::Session::State::ended) {
			r.ended = true;
			printf("    ended: %d / %d\n", int(a.end_reason()), int(b.end_reason()));
			break;
		}
	}
	r.fps = have_to && to_s > from_s ? (f_to - f_from) / (to_s - from_s) : 0;
	r.delay_end = a.delay();
	return r;
}

} // namespace

int main(int argc, char **argv)
{
	verbose = argc > 1 && !strcmp(argv[1], "-v");
	const double FULL = 57.524160, FRAME = 1000.0 / FULL;

	printf("the rule (vfnet::auto_delay)\n");
	struct R { double rtt, jit; int target, floor, want; };
	const R rules[] = {
		{ 10, 1, 6, 1, 1 },     // a LAN: the least
		{ 60, 4, 6, 1, 2 },     // same country: half the round trip in frames, rounded
		{ 80, 10, 6, 1, 2 },    // a phone's 5G, the same city: 2 (the rollback takes a frame or two)
		{ 130, 8, 6, 1, 3 },    // across a continent: 3 at most from the ping alone
		{ 210, 10, 6, 1, 3 },   // still 3: the rollback covers the rest
		{ 410, 10, 6, 1, 7 },   // the other side of the world: more, so no slowdown
		{ 710, 15, 6, 1, 15 },  // as far as there is: the most
		{ 130, 8, 4, 2, 3 },    // a slow machine: shorter rollbacks
		{ 310, 8, 4, 2, 6 },
		{ 20, 2, 4, 2, 2 },     // a slow machine on a LAN: 2 at least
	};
	for (const R &x : rules) {
		const int got = vfnet::auto_delay(x.rtt, x.jit, FRAME, x.target, x.floor);
		check(got == x.want, "round trip " + std::to_string(int(x.rtt)) + " ms, jitter " + std::to_string(int(x.jit)) + ", rollback " +
		                         std::to_string(x.target) + ": delay " + std::to_string(got) + " (" + std::to_string(x.want) + " wanted)");
	}

	printf("full speed at every ping (AutoDelay, 1%% lost, jitter)\n");
	for (double one_way : { 10.0, 40.0, 80.0, 120.0, 160.0, 200.0, 250.0, 300.0, 330.0 }) {
		const Run r = run(45, [&](double) { return vfnet::SimWire::Params{ one_way, 8, 0.01 }; }, 15, 40);
		char b[200];
		snprintf(b, sizeof b, "one way %3.0f ms (round trip ~%3.0f): %.2f frames a second, delay %d", one_way, 2 * one_way + 8, r.fps, r.delay_end);
		check(!r.ended && r.fps >= FULL * 0.985, b);
		if (one_way <= 60) check(r.delay_end <= 4, "  and no more delay than it needs there (" + std::to_string(r.delay_end) + ")");
	}

	printf("a ping that jumps mid-match, and comes back\n");
	{
		// 40 ms one way; at 20 s 220 (a far route, a relay); at 60 s back to 40
		auto wire = [](double s) { return vfnet::SimWire::Params{ s >= 20 && s < 60 ? 220.0 : 40.0, 8, 0.01 }; };
		const Run r = run(110, wire, 27, 59);
		check(!r.ended, "no desync, no end");
		double raised_at = -1;
		for (auto &c : r.changes) if (c.first >= 20 && c.first < 60 && c.second >= 6) { raised_at = c.first; break; }
		check(raised_at > 0 && raised_at < 26, "raised for the far ping within 6 s (at " + std::to_string(raised_at) + " s, to " + std::to_string(r.delay_max) + ")");
		check(r.fps >= FULL * 0.985, "full speed once raised: " + std::to_string(r.fps) + " frames a second");
		check(!r.lowered_at.empty() && r.lowered_at.back() >= 70, "lowered again once the ping's back (at " + (r.lowered_at.empty() ? std::string("-") : std::to_string(r.lowered_at.back())) + " s)");
		check(!r.lowered_outside_rounds, "lowered only between rounds");
		check(r.delay_end <= 3, "back to a near ping's delay (" + std::to_string(r.delay_end) + ")");
	}

	printf("raised a frame at a time (no input held several frames longer at once)\n");
	{
		auto wire = [](double s) { return vfnet::SimWire::Params{ s >= 20 ? 250.0 : 20.0, 8, 0.01 }; };
		const Run r = run(50, wire, 30, 48);
		bool ok = true;
		uint32_t last = 0;
		for (size_t i = 1; i < r.change_frames.size(); i++) {   // (the first, at the character select, at once)
			const auto &c = r.change_frames[i];
			if (c.second > 1 || (c.second > 0 && last && c.first - last < 6)) ok = false;
			if (c.second > 0) last = c.first;
		}
		check(ok && r.delay_max >= 9, "20 to 250 ms one way: up by one every 6 frames at most, to " + std::to_string(r.delay_max));
		check(r.fps >= FULL * 0.985, "and at full speed after: " + std::to_string(r.fps));
	}

	printf("a ping on a boundary: no see-saw\n");
	{
		// one way swinging 190 to 205 ms every 7 s: either side of 6 and 7 frames
		auto wire = [](double s) { return vfnet::SimWire::Params{ int(s / 7) % 2 ? 205.0 : 190.0, 6, 0.005 }; };
		const Run r = run(150, wire, 20, 145);
		check(!r.ended && r.changes.size() <= 4, std::to_string(r.changes.size()) + " changes in 150 s");
		check(r.fps >= FULL * 0.985, "full speed: " + std::to_string(r.fps));
	}

	printf("the other side's machine slower (55 frames a second): no delay climbing for nothing\n");
	{
		const Run r = run(60, [](double) { return vfnet::SimWire::Params{ 40, 6, 0.01 }; }, 20, 55, false, 55.0);
		check(!r.ended && r.delay_max <= 5, "delay at most " + std::to_string(r.delay_max) + " (the ping wants 3); " + std::to_string(r.fps) + " frames a second");
	}

	printf("a burst of loss (30%% for 5 s): carried through\n");
	{
		auto wire = [](double s) { return vfnet::SimWire::Params{ 60, 8, s >= 20 && s < 25 ? 0.3 : 0.01 }; };
		const Run r = run(50, wire, 30, 48);
		check(!r.ended && r.fps >= FULL * 0.985, "no end, full speed after: " + std::to_string(r.fps) + ", delay " + std::to_string(r.delay_end));
	}

	printf("a slow machine (a Steam Deck's frame): shorter rollbacks, a little more delay\n");
	{
		const Run r = run(40, [](double) { return vfnet::SimWire::Params{ 120, 8, 0.01 }; }, 15, 35, true);
		check(!r.ended && r.fps >= FULL * 0.985, "one way 120 ms: " + std::to_string(r.fps) + " frames a second, delay " + std::to_string(r.delay_end));
	}

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed ? 1 : 0;
}
