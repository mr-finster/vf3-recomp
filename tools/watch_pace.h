// watch_pace.h (tests): a watcher come late, timed as the game runs one: how long from its start (or its snapshot)
// to its first picture, the lobby's frames growing at the players' pace meanwhile (57.5 a second), every frame there
// at once (a WebSocket's). The old way: a pass a refresh (17.4 ms, the vsync or the frame limit), 10 ms of frames
// caught up unseen in each; the new: 40 ms a step, one after another (the picture only the line saying how far it
// is, drawn alongside). Shared by vf1net, vf2net and vf3net (--pace).
#pragma once
#include "../src/net/watch.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace watch_pace {

struct Result { double wall_s = 0; uint64_t frames = 0; bool caught = false; uint32_t at = 0, live = 0; };

// in: the match's inputs, pairs (taken round again past their end); joined: the lobby's frames as it comes in;
// old_way: the game before this change
inline Result run(vfnet::watch::Spectator &sp, const std::vector<uint16_t> &in, uint32_t joined, uint64_t match, bool old_way, double cap_s)
{
	namespace vw = vfnet::watch;
	using Clock = std::chrono::steady_clock;
	const double frame_ms = 1000.0 / 57.524160;
	const uint32_t n_in = uint32_t(in.size() / 2);
	const auto t0 = Clock::now();
	Result r;
	const uint64_t ran0 = sp.frames_run;
	for (;;) {
		const auto p0 = Clock::now();
		const double now = std::chrono::duration<double, std::milli>(p0 - t0).count();
		const uint32_t live = joined + uint32_t(now / frame_ms);
		if (sp.ask_from() < live) {   // (what's come since, as a WebSocket brings it)
			vw::Reply rp;
			rp.match = match;
			rp.state = 1;
			rp.total = live;
			rp.frames.first = sp.ask_from();
			const uint32_t n = std::min<uint32_t>(live - rp.frames.first, vw::MAX_REPLY);
			for (uint32_t f = rp.frames.first; f < rp.frames.first + n; f++) {
				rp.frames.in.push_back(in[size_t(f % n_in) * 2]);
				rp.frames.in.push_back(in[size_t(f % n_in) * 2 + 1]);
			}
			sp.reply(rp, now);
		}
		sp.time(now);
		bool shown = false;
		sp.tick(&shown, old_way ? 10.0 : 40.0);
		if (shown) { r.caught = true; r.at = sp.frame(); r.live = live; break; }
		if (now > cap_s * 1000) { r.at = sp.frame(); r.live = live; break; }
		// (the pass's picture: the old way waits for the refresh; the new draws alongside, a pass a refresh at least)
		const double pass = std::chrono::duration<double, std::milli>(Clock::now() - p0).count();
		if (pass < frame_ms) std::this_thread::sleep_for(std::chrono::microseconds(int64_t((frame_ms - pass) * 1000)));
	}
	r.wall_s = std::chrono::duration<double>(Clock::now() - t0).count();
	r.frames = sp.frames_run - ran0;
	return r;
}

inline void say(const char *what, const Result &r, double behind_s)
{
	const double rate = r.wall_s > 0 ? double(r.frames) / r.wall_s : 0;   // (frames a second while catching up)
	const double speed = rate / 57.524160;
	if (r.caught)
		printf("pace %s: %.0f s behind when it came: its first picture after %.1f s (%llu frames, %.0f a second: %.1fx the match's pace)\n", what, behind_s, r.wall_s, (unsigned long long)r.frames, rate, speed);
	else
		printf("pace %s: %.0f s behind when it came: still catching up after %.1f s (%llu frames, %.0f a second: %.1fx the match's pace; %s)\n", what, behind_s, r.wall_s, (unsigned long long)r.frames, rate, speed,
		       speed > 1.0 ? (std::to_string(int(behind_s / (speed - 1.0))) + " s to catch up at that pace").c_str() : "it never would at that pace");
}

} // namespace watch_pace
