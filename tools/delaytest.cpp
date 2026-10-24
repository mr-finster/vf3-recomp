// delaytest: Auto's input delay (vfnet::auto_delay, vfnet::AutoDelay) against simulated connections: the rule's values
// at each ping; two sessions at every ping from a LAN's to the other side of the world, each driven by AutoDelay,
// held to the game's full speed (57.5 frames a second) with no desync; a ping that jumps mid-match (raised at once)
// and comes back (mid-fight a frame at a time); and a slow machine's (shorter rollbacks, more delay).
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
	bool lowered_outside_rounds = false;   // (mid-fight by more than a frame at once)
	uint64_t b_resim = 0, b_frames = 0;   // (side B's frames run again, and frames, in all)
	uint64_t a_stalls = 0, a_waits = 0;   // (side A stopped dead for B's inputs; held back a frame to keep in time)
};

// Two sessions over a wire for `secs`, both on AutoDelay; `wire_at` changes the wire as time goes; rounds end every
// 20 s (the 90 frames after: between rounds); fps measured over [from_s, to_s)
Run run(double secs, std::function<vfnet::SimWire::Params(double)> wire_at, double from_s, double to_s, bool slow = false, double b_fps = 0, int room_b = -1,
        double soft_gain = 0, double soft_max = 0, bool b_spiky = false)
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
		// (side A keeps in time as the game does, its frames a little slower while it's ahead: Online::slow_down)
		const double slow_a = soft_gain > 0 ? std::clamp(a.stats().lead * soft_gain, 0.0, soft_max) : 0.0;
		if (t >= na) { if (a.step(uint16_t(a.frame() / 7 % 5))) na += c.frame_ms / (1 - slow_a); else na += 1; }
		if (t >= nb) {
			if (b.step(uint16_t(b.frame() / 11 % 3))) {
				nb += b_fps > 0 ? 1000.0 / b_fps : c.frame_ms;
				if (b_spiky && b.frame() % 53 == 0) nb += 30;   // (a slow machine's frame now and then: 30 ms late)
			} else nb += 1;
		}
		a.set_room(da.rollback_room(a.stats()));
		b.set_room(room_b >= 0 ? room_b : db.rollback_room(b.stats()));
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
					// (the first, at the character select, may be below the session's starting 2: not a lowering mid-fight)
					if (n < was && r.changes.size() > 1) { r.lowered_at.push_back(t / 1000); if (!br && was - n > 1) r.lowered_outside_rounds = true; }
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
	r.b_resim = b.stats().resim_frames;
	r.a_stalls = a.stats().stalls;
	r.a_waits = a.stats().waits;
	r.b_frames = b.frame();
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
		{ 10, 1, 6, 0, 0 },     // a LAN (floor 0 within a frame's round trip): none
		{ 60, 4, 6, 1, 1 },     // same country: 1, the rollback the rest
		{ 80, 10, 6, 1, 1 },    // a phone's 5G, the same city: 1
		{ 130, 8, 6, 1, 1 },    // across a continent: still 1
		{ 210, 10, 6, 1, 1 },   // 1 still: up to 6 frames guessed
		{ 250, 10, 6, 1, 2 },   // past that, what keeps the guesses within 6
		{ 410, 10, 6, 1, 7 },   // the other side of the world: more, so no slowdown
		{ 710, 15, 6, 1, 15 },  // as far as there is: the most
		{ 80, 10, 4, 2, 2 },    // a slow machine (a Steam Deck) on 5G: 2
		{ 310, 8, 4, 2, 6 },
		{ 10, 1, 4, 1, 1 },     // a slow machine on a LAN: 1
	};
	for (const R &x : rules) {
		const int got = vfnet::auto_delay(x.rtt, x.jit, FRAME, x.target, x.floor);
		check(got == x.want, "round trip " + std::to_string(int(x.rtt)) + " ms, jitter " + std::to_string(int(x.jit)) + ", rollback " +
		                         std::to_string(x.target) + ": delay " + std::to_string(got) + " (" + std::to_string(x.want) + " wanted)");
	}

	printf("what a frame costs: each side's rollbacks fit its frames (its room), the other side's delay covering the rest\n");
	{
		// (from real logs, in a fight: an M4 Max's step 5.7 ms, a frame run again 4.9; a Steam Deck's 14.6 and 11.7)
		struct M { const char *what; double step, each; int room; };
		const M ms[] = { { "a fast Mac", 5.7, 4.9, 2 }, { "a Mac in a light scene", 3.9, 2.5, 3 }, { "a Steam Deck in a fight", 14.6, 11.7, 0 },
		                 { "a Steam Deck in a menu", 6.0, 4.5, 2 } };
		for (const M &m : ms) {
			vfnet::Session::Stats st;
			st.steps = 1000;
			st.step_recent_ms = m.step;
			st.resim_recent_ms = m.each;
			vfnet::AutoDelay d;
			const int room = d.rollback_room(st);
			check(room == m.room, std::string(m.what) + ": can run again " + std::to_string(room) + " frames in a step (" + std::to_string(m.room) + " wanted)");
		}
		struct W { const char *what; double rtt; int peer_room, want; };
		const W ws[] = {
			{ "a Mac against a Deck in a fight, 45 ms", 45, 0, 2 },   // (the Deck can't take any back: the Mac covers the trip)
			{ "the Deck against that Mac, 45 ms", 45, 2, 1 },         // (the Mac takes back 2: the Deck plays at 1)
			{ "two fast machines, 80 ms", 80, 2, 1 },
			{ "two fast machines on a LAN", 8, 2, 0 },
			{ "a Mac against a Deck, 130 ms", 130, 0, 5 },
		};
		for (const W &w : ws) {
			vfnet::Session::Stats st;
			st.rtt_ms = w.rtt;
			st.jitter_ms = 8;
			st.peer_room = w.peer_room;
			vfnet::AutoDelay d;
			const int want = d.want(st);
			check(want == w.want, std::string(w.what) + ": delay " + std::to_string(want) + " (" + std::to_string(w.want) + " wanted)");
		}
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
		check(!r.lowered_at.empty() && r.lowered_at.front() >= 61 && r.lowered_at.back() < 80, "lowered again once the ping's back (from " + (r.lowered_at.empty() ? std::string("-") : std::to_string(r.lowered_at.front())) + " s)");
		check(!r.lowered_outside_rounds, "lowered mid-fight only a frame at a time");
		check(r.delay_end <= 2, "back to a near ping's delay (" + std::to_string(r.delay_end) + ")");
	}

	printf("a burst on a phone network (a second and a half of 200 ms, jumping): rollback's, not a delay kept\n");
	{
		auto wire = [](double s) { return s >= 30 && s < 31.5 ? vfnet::SimWire::Params{ 200.0, 150, 0.05 } : vfnet::SimWire::Params{ 25.0, 6, 0.01 }; };
		const Run r = run(50, wire, 35, 48);
		check(!r.ended, "no desync, no end");
		int most = 0;
		for (auto &c : r.changes) if (c.first >= 29) most = std::max(most, c.second);
		check(most <= 3, "the delay not raised far for it (" + std::to_string(most) + ")");
		check(r.delay_end <= 2, "and at a near ping's after (" + std::to_string(r.delay_end) + ")");
	}

	printf("a ping that rises and stays (a phone network's worse spell): raised, then lowered mid-round once it's back\n");
	{
		auto wire = [](double s) { return vfnet::SimWire::Params{ s >= 20 && s < 30 ? 180.0 : 25.0, 10, 0.01 }; };
		const Run r = run(45, wire, 36, 44);
		int most = 0;
		for (auto &c : r.changes) if (c.first >= 20 && c.first < 30) most = std::max(most, c.second);
		check(most >= 3, "raised for it (to " + std::to_string(most) + ")");
		check(!r.lowered_at.empty() && r.lowered_at.front() >= 30 && r.lowered_at.front() < 36, "lowered within 6 s of it ending (at " + (r.lowered_at.empty() ? std::string("-") : std::to_string(r.lowered_at.front())) + " s)");
		check(r.delay_end <= 2, "back down (" + std::to_string(r.delay_end) + ")");
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

	printf("against a machine that can't run a frame again in time (a Steam Deck in a fight): this side covers the trip\n");
	{
		const Run r = run(40, [](double) { return vfnet::SimWire::Params{ 25, 6, 0.0 }; }, 15, 38, false, 0, 0);
		char b[200];
		snprintf(b, sizeof b, "45 ms or so: delay %d here, the Deck's frames run again %.1f%% of its frames, %.2f frames a second", r.delay_end,
		         r.b_frames ? 100.0 * double(r.b_resim) / double(r.b_frames) : 0.0, r.fps);
		check(!r.ended && r.delay_end >= 2 && r.b_frames && double(r.b_resim) / double(r.b_frames) < 0.02 && r.fps >= FULL * 0.985, b);
	}

	printf("a slower, uneven opponent (a Steam Deck at 55.6 frames a second, 30 ms late now and then): kept level smoothly\n");
	{
		auto wire = [](double) { return vfnet::SimWire::Params{ 22, 6, 0.0 }; };
		const Run old_r = run(60, wire, 15, 55, false, 55.6, -1, 0.01, 0.02, true);
		const Run r = run(60, wire, 15, 55, false, 55.6, -1, 0.02, 0.06, true);
		char b[240];
		snprintf(b, sizeof b, "this side stopped dead %llu times (at the 2%% most it kept in time before: %llu), %llu frames held back; %.2f frames a second",
		         (unsigned long long)r.a_stalls, (unsigned long long)old_r.a_stalls, (unsigned long long)r.a_waits, r.fps);
		check(!r.ended && r.a_stalls * 4 <= old_r.a_stalls + 4, b);
	}

	printf("far, against an opponent whose frames come unevenly (Korea from Australia): the delay settles, no see-saw\n");
	{
		// (frames held back the delay can't fix: raised for them, they kept on as often, and it went 3, 4, 3, 4 every
		// 10 s for 40 minutes, 433 changes)
		auto wire = [](double) { return vfnet::SimWire::Params{ 114, 8, 0.005 }; };
		const Run r = run(180, wire, 30, 175, false, 57.0, -1, 0.02, 0.06, true);
		int later = 0;
		for (auto &c : r.changes) later += c.first >= 30;
		char b[200];
		snprintf(b, sizeof b, "%d delay changes after the first 30 s (to %d), %llu frames held back, %.2f frames a second", later, r.delay_end,
		         (unsigned long long)r.a_waits, r.fps);
		check(!r.ended && later <= 6, b);
	}

	printf("a slow machine (a Steam Deck's frame): shorter rollbacks, a little more delay\n");
	{
		const Run r = run(40, [](double) { return vfnet::SimWire::Params{ 120, 8, 0.01 }; }, 15, 35, true);
		check(!r.ended && r.fps >= FULL * 0.985, "one way 120 ms: " + std::to_string(r.fps) + " frames a second, delay " + std::to_string(r.delay_end));
	}

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed ? 1 : 0;
}
