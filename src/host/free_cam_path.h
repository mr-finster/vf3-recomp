// The free camera flown along a path from a file (tests and footage: VF3_TEST_FREECAM_PATH; nothing of this reaches
// players). The file's format is the same in the three games (its times and distances in the game's own units):
//
//   # a comment
//   notoast                       no "Free camera: ..." notice as it turns on (a recording)
//   aim <up>                      "lookat fighters" aims this far above the fighters' middle (default 0)
//   at <T> on|off|pause|unpause|quit
//   key <T> <where> <look>        the camera at time T: there, looking so
//       <where>:  game                       where the game's own camera is at that moment
//                 rel <radius> <az> <height>  round the fighters' middle: that far out (level), az degrees round
//                                             the world's up from where the game's camera was when the free camera
//                                             started (positive: round to its right), that far above the middle
//                 pos <x> <y> <z>             a place in the game's world
//       <look>:   lookat game                 as the game's camera looks at that moment
//                 lookat fighters             at the fighters' middle
//                 look <yaw> <pitch>          degrees: yaw from the game camera's heading when the free camera
//                                             started (positive right), pitch up from level
//   orbit <T0> <T1> through <deg> [radius <r>] [height <h>]
//                                 from where the camera is at T0, round the fighters' middle through that angle by
//                                 T1 (eased in and out), going to that radius and height on the way (else kept),
//                                 looking at the fighters; the camera is then there, as a key at T1
//
// A time T is on the test hooks' clock: game frames while the game runs, then a tick for each picture while it is
// stopped (paused), so a paused game's camera goes on moving; "+N" is N after the line before's time. Between keys the
// place goes smoothly (a Catmull-Rom spline through the keys in radius, angle and height about the fighters' middle,
// at rest at the first and last key and at an orbit's ends), the look eased from one key's to the next's the short
// way round. Before the first key the camera is where it was; after the last it stays.
// The fighters' middle: each game finds its fighters its own way (VF3: Path::fighters, their viewports); it is
// followed smoothly (about a quarter of a second behind), so a jump or a throw doesn't shake the camera.
#pragma once
#include "free_cam.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace freecam {

struct Path
{
	struct Key
	{
		double t = 0;
		int where = 0;            // 0 game, 1 rel, 2 pos
		float a[3] = {};          // rel: radius, az (radians), height; pos: x, y, z
		int look = 0;             // 0 game, 1 fighters, 2 angles
		float yaw = 0, pitch = 0;
	};
	struct Orbit { double t0 = 0, t1 = 0; float through = 0; bool has_r = false, has_h = false; float r = 0, h = 0; bool started = false; float from[3] = {}, look0[2] = {}; };
	struct Event { double t = 0; std::string what; bool done = false; };
	std::vector<Key> keys;
	std::vector<Orbit> orbits;
	std::vector<Event> events;
	bool no_toast = false, loaded = false;
	float aim = 0;
	// (the fighters' middle as followed, in the camera's start frame E0; the clock it was last followed at)
	bool have_mid = false;
	float mid[3] = {};
	double mid_clock = 0;
	std::string error;

	bool load(const char *file)
	{
		FILE *f = fopen(file, "r");
		if (!f) { error = std::string("can't open ") + file; return false; }
		char line[512];
		double last = 0;
		int n = 0;
		auto time = [&](const char *s) { const double v = *s == '+' ? last + atof(s + 1) : atof(s); last = v; return v; };
		while (fgets(line, sizeof line, f)) {
			n++;
			if (char *c = strchr(line, '#')) *c = 0;
			std::vector<std::string> w;
			for (char *tok = strtok(line, " \t\r\n"); tok; tok = strtok(nullptr, " \t\r\n")) w.push_back(tok);
			if (w.empty()) continue;
			auto bad = [&] { error = std::string(file) + " line " + std::to_string(n) + ": not understood"; fclose(f); return false; };
			auto num = [&](size_t i) { return i < w.size() ? float(atof(w[i].c_str())) : 0.0f; };
			if (w[0] == "notoast") no_toast = true;
			else if (w[0] == "aim" && w.size() == 2) aim = num(1);
			else if (w[0] == "at" && w.size() == 3) events.push_back({ time(w[1].c_str()), w[2], false });
			else if (w[0] == "key" && w.size() >= 3) {
				Key k;
				k.t = time(w[1].c_str());
				size_t i = 2;
				if (w[i] == "game") { k.where = 0; i += 1; }
				else if (w[i] == "rel" && w.size() >= i + 4) { k.where = 1; k.a[0] = num(i + 1); k.a[1] = num(i + 2) * DEG; k.a[2] = num(i + 3); i += 4; }
				else if (w[i] == "pos" && w.size() >= i + 4) { k.where = 2; for (int c = 0; c < 3; c++) k.a[c] = num(i + 1 + size_t(c)); i += 4; }
				else return bad();
				if (i + 1 < w.size() && w[i] == "lookat" && w[i + 1] == "game") k.look = 0;
				else if (i + 1 < w.size() && w[i] == "lookat" && w[i + 1] == "fighters") k.look = 1;
				else if (i + 2 < w.size() && w[i] == "look") { k.look = 2; k.yaw = num(i + 1) * DEG; k.pitch = num(i + 2) * DEG; }
				else return bad();
				keys.push_back(k);
			}
			else if (w[0] == "orbit" && w.size() >= 5 && w[3] == "through") {
				Orbit o;
				o.t0 = time(w[1].c_str());
				o.t1 = time(w[2].c_str());
				o.through = num(4) * DEG;
				for (size_t i = 5; i + 1 < w.size(); i += 2) {
					if (w[i] == "radius") { o.has_r = true; o.r = num(i + 1); }
					else if (w[i] == "height") { o.has_h = true; o.h = num(i + 1); }
					else return bad();
				}
				if (!(o.t1 > o.t0)) return bad();
				orbits.push_back(o);
			}
			else return bad();
		}
		fclose(f);
		loaded = true;
		return true;
	}

	static float ease(float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * u * (u * (u * 6 - 15) + 10); }   // (at rest at both ends, no jerk)
	static void polar(const Cam &c, const float *p, const float *m, float out[3])
	{
		const float v[3] = { p[0] - m[0], p[1] - m[1], p[2] - m[2] };
		const float h = Cam::dot(v, c.u);
		const float lv[3] = { v[0] - h * c.u[0], v[1] - h * c.u[1], v[2] - h * c.u[2] };
		out[0] = std::sqrt(Cam::dot(lv, lv));
		out[1] = std::atan2(Cam::dot(lv, c.s0), -Cam::dot(lv, c.h0));
		out[2] = h;
	}
	static void place(const Cam &c, const float q[3], const float *m, float p[3])
	{
		const float cs = std::cos(q[1]), sn = std::sin(q[1]);
		for (int k = 0; k < 3; k++) p[k] = m[k] + q[0] * (-cs * c.h0[k] + sn * c.s0[k]) + q[2] * c.u[k];
	}
	static void angles(const Cam &c, const float *dir, float &yaw, float &pitch)
	{
		float d[3] = { dir[0], dir[1], dir[2] };
		if (!Cam::normalise(d)) { yaw = c.yaw; pitch = c.pitch; return; }
		pitch = std::asin(std::clamp(Cam::dot(d, c.u), -1.0f, 1.0f));
		yaw = std::atan2(Cam::dot(d, c.s0), Cam::dot(d, c.h0));
	}

	// The fighters' middle in the scene, in E0 (g: the game camera's view into E0): each fighter is a full-screen
	// viewport of its own in VF3, many models close together (a stage's are spread wide, as are its grass and
	// leaves): the two such viewports with the most vertices, the middle of their middles. False: none found.
	// SPREAD: how far from their middle half of a fighter's models are at most, in the game's units.
	static constexpr float SPREAD = 2500;
	static bool fighters(const Scene &s, const Mat4 &g, float out[3], int *found = nullptr)
	{
		struct Body { float c[3]; size_t verts; };
		std::vector<Body> bodies;
		for (const video::Viewport &vp : s.viewports) {
			if (!vp.full_screen || vp.models.size() < 20) continue;
			std::vector<std::array<float, 3>> pts;
			size_t verts = 0;
			for (const video::Model &m : vp.models) {
				const float *t = &m.matrix.m[12];
				pts.push_back({ g.m[0] * t[0] + g.m[4] * t[1] + g.m[8] * t[2] + g.m[12], g.m[1] * t[0] + g.m[5] * t[1] + g.m[9] * t[2] + g.m[13],
				                g.m[2] * t[0] + g.m[6] * t[1] + g.m[10] * t[2] + g.m[14] });
				if (m.meshes) for (const video::Mesh &mm : *m.meshes) verts += mm.verts.size();
			}
			// (its middle: each of x, y, z the middle one of its models'; its spread: the middle distance from it)
			Body b{ {}, verts };
			for (int k = 0; k < 3; k++) {
				std::vector<float> v;
				for (auto &p : pts) v.push_back(p[size_t(k)]);
				std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
				b.c[k] = v[v.size() / 2];
			}
			std::vector<float> d;
			for (auto &p : pts) d.push_back(std::sqrt((p[0] - b.c[0]) * (p[0] - b.c[0]) + (p[1] - b.c[1]) * (p[1] - b.c[1]) + (p[2] - b.c[2]) * (p[2] - b.c[2])));
			std::nth_element(d.begin(), d.begin() + std::ptrdiff_t(d.size() / 2), d.end());
			if (d[d.size() / 2] > SPREAD || verts < 2000) continue;
			bodies.push_back(b);
		}
		if (found) *found = int(bodies.size());
		if (bodies.empty()) return false;
		std::sort(bodies.begin(), bodies.end(), [](const Body &a, const Body &b) { return a.verts > b.verts; });
		const size_t n = std::min<size_t>(bodies.size(), 2);
		for (int k = 0; k < 3; k++) out[k] = n == 2 ? (bodies[0].c[k] + bodies[1].c[k]) / 2 : bodies[0].c[k];
		return true;
	}

	// The camera's place and look at `clock`, into c (anchored): g the game's camera's view into E0 (r0 R^-1), s the
	// scene being drawn (its models in the game camera's view)
	void fly(Cam &c, const Scene &s, const Mat4 &g, double clock)
	{
		if (keys.empty() && orbits.empty()) return;
		// the fighters' middle, followed smoothly
		int found = 0;
		{
			float cen[3];
			if (fighters(s, g, cen, &found)) {
				const float a = have_mid ? float(1 - std::exp(-std::max(0.0, clock - mid_clock) / 14.0)) : 1.0f;
				for (int k = 0; k < 3; k++) mid[k] += (cen[k] - mid[k]) * a;
				have_mid = true;
			}
			mid_clock = clock;
		}
		if (!have_mid) return;
		// the game's camera now, in E0
		const float gp[3] = { g.m[12], g.m[13], g.m[14] };
		float gf[3] = { -g.m[8], -g.m[9], -g.m[10] }, gyaw, gpitch;
		angles(c, gf, gyaw, gpitch);
		const float target[3] = { mid[0] + aim * c.u[0], mid[1] + aim * c.u[1], mid[2] + aim * c.u[2] };
		// (VF3_TEST_FREECAM_PATH_LOG: every 30 ticks, where the game's camera is round the fighters' middle, for writing a
		// path: its radius, angle and height as a "rel" key's, and how many fighters were found)
		if (static const bool log = getenv("VF3_TEST_FREECAM_PATH_LOG") != nullptr; log) {
			static double said = -1e9;
			if (clock - said >= 30) {
				said = clock;
				float gq[3];
				polar(c, gp, mid, gq);
				fprintf(stderr, "(path) %.1f: the game's camera rel %.0f %.1f %.0f, looking %.1f %.1f; %d fighters found\n",
				        clock, gq[0], gq[1] / DEG, gq[2], gyaw / DEG, gpitch / DEG, found);
			}
		}
		auto look_of = [&](const Key &k, const float *p, float &yaw, float &pitch) {
			if (k.look == 0) { yaw = gyaw; pitch = gpitch; }
			else if (k.look == 2) { yaw = k.yaw; pitch = k.pitch; }
			else { const float d[3] = { target[0] - p[0], target[1] - p[1], target[2] - p[2] }; angles(c, d, yaw, pitch); }
		};
		auto polar_of = [&](const Key &k, float q[3]) {
			if (k.where == 1) { q[0] = k.a[0]; q[1] = k.a[1]; q[2] = k.a[2]; return; }
			float p[3];
			if (k.where == 0) { p[0] = gp[0]; p[1] = gp[1]; p[2] = gp[2]; }
			else for (int i = 0; i < 3; i++) p[i] = c.r0.m[i] * k.a[0] + c.r0.m[4 + i] * k.a[1] + c.r0.m[8 + i] * k.a[2] + c.r0.m[12 + i];
			polar(c, p, mid, q);
		};
		// the keys in time's order, an orbit's end among them once it has started (its keys in between left out)
		std::vector<Key> ks;
		for (const Key &k : keys) {
			bool inside = false;
			for (const Orbit &o : orbits) inside |= k.t > o.t0 && k.t <= o.t1;
			if (!inside) ks.push_back(k);
		}
		std::sort(ks.begin(), ks.end(), [](const Key &a, const Key &b) { return a.t < b.t; });
		// the keys' path at t: place (polar) and look
		auto along = [&](const std::vector<Key> &v, double t, float q[3], float &yaw, float &pitch) {
			if (v.empty()) { polar(c, c.p, mid, q); yaw = c.yaw; pitch = c.pitch; return; }
			std::vector<std::array<float, 3>> pq(v.size());
			for (size_t i = 0; i < v.size(); i++) {
				polar_of(v[i], pq[i].data());
				// (the angle on from the key before's: two "rel" keys as far apart as written, 0 to 270 the long way; else
				// the short way round)
				if (i) pq[i][1] = pq[i - 1][1] + (v[i].where == 1 && v[i - 1].where == 1 ? v[i].a[1] - v[i - 1].a[1] : std::remainder(pq[i][1] - pq[i - 1][1], 2 * PI));
			}
			size_t i = 0;
			while (i + 1 < v.size() && t >= v[i + 1].t) i++;
			float p[3];
			if (t <= v[0].t || i + 1 >= v.size()) {
				const size_t e = t <= v[0].t ? 0 : v.size() - 1;
				for (int k = 0; k < 3; k++) q[k] = pq[e][size_t(k)];
				place(c, q, mid, p);
				look_of(v[e], p, yaw, pitch);
				return;
			}
			const double t0 = v[i].t, t1 = v[i + 1].t;
			const float s = float((t - t0) / (t1 - t0));
			for (int k = 0; k < 3; k++) {
				// (the slopes: through the keys either side, none at the path's ends)
				const size_t kk = size_t(k);
				const float m0 = i > 0 ? (pq[i + 1][kk] - pq[i - 1][kk]) / float(v[i + 1].t - v[i - 1].t) * float(t1 - t0) : 0.0f;
				const float m1 = i + 2 < v.size() ? (pq[i + 2][kk] - pq[i][kk]) / float(v[i + 2].t - v[i].t) * float(t1 - t0) : 0.0f;
				const float s2 = s * s, s3 = s2 * s;
				q[k] = (2 * s3 - 3 * s2 + 1) * pq[i][kk] + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * pq[i + 1][kk] + (s3 - s2) * m1;
			}
			q[0] = std::max(q[0], 0.0f);
			place(c, q, mid, p);
			float y0, p0, y1, p1;
			look_of(v[i], p, y0, p0);
			look_of(v[i + 1], p, y1, p1);
			const float w = ease(s);
			yaw = y0 + std::remainder(y1 - y0, 2 * PI) * w;
			pitch = p0 + (p1 - p0) * w;
		};
		float q[3], yaw, pitch;
		// an orbit under way, or the last one done (the camera then at its end, a key there for what follows)
		Orbit *now = nullptr;
		for (Orbit &o : orbits) if (clock >= o.t0 && (!now || o.t0 > now->t0)) now = &o;
		if (now) {
			Orbit &o = *now;
			if (!o.started) {
				std::vector<Key> before;
				for (const Key &k : ks) if (k.t <= o.t0) before.push_back(k);
				along(before, o.t0, o.from, o.look0[0], o.look0[1]);
				if (before.empty()) { polar(c, c.p, mid, o.from); o.look0[0] = c.yaw; o.look0[1] = c.pitch; }
				o.started = true;
			}
			const float end[3] = { o.has_r ? o.r : o.from[0], o.from[1] + o.through, o.has_h ? o.h : o.from[2] };
			std::vector<Key> after;
			for (const Key &k : ks) if (k.t > o.t1) after.push_back(k);
			if (clock >= o.t1 && !after.empty()) {
				Key e;
				e.t = o.t1; e.where = 1; e.look = 1;
				for (int k = 0; k < 3; k++) e.a[k] = end[k];
				after.insert(after.begin(), e);
				along(after, clock, q, yaw, pitch);
			} else {
				const float u = float((std::min(clock, o.t1) - o.t0) / (o.t1 - o.t0)), e = ease(u);
				for (int k = 0; k < 3; k++) q[k] = o.from[k] + (end[k] - o.from[k]) * e;
				float p[3];
				place(c, q, mid, p);
				const float d[3] = { target[0] - p[0], target[1] - p[1], target[2] - p[2] };
				float fy, fp;
				angles(c, d, fy, fp);
				const float w = ease(u / 0.25f);   // (from the look it had to the fighters, over its first quarter)
				yaw = o.look0[0] + std::remainder(fy - o.look0[0], 2 * PI) * w;
				pitch = o.look0[1] + (fp - o.look0[1]) * w;
			}
		} else {
			if (ks.empty() || clock < ks[0].t) return;   // (before the first key: where it was)
			// (an orbit still to come: the keys up to its start only, the camera waiting at the last of them)
			double next = 1e300;
			for (const Orbit &o : orbits) if (o.t0 > clock) next = std::min(next, o.t0);
			std::vector<Key> till;
			for (const Key &k : ks) if (k.t <= next) till.push_back(k);
			along(till, clock, q, yaw, pitch);
		}
		place(c, q, mid, c.p);
		c.yaw = yaw;
		c.pitch = std::clamp(pitch, -89 * DEG, 89 * DEG);
	}
};

} // namespace freecam
