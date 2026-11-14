// VF3_WIDE_SELECT (a proposal, off by default): widescreen's sides on the character select and the stage screen,
// made from the screen's own background (TileGen's plate: the pixels of solid tiles, the pictures the letters,
// frames and cursors are drawn over). Only the sides: the 4:3 picture stays as the game draws it, in the middle.
// Each side is cut into bands of lines with about the same clean zone (how far in from the edge the background goes
// before the screen's pictures start: a frame's border, the grid's portraits, a panel's stills; 8 to 160 pixels).
//   1 mirror: the zone's columns mirrored out, and back (the game's pixels, flipped)
//   2 fill: the sides made of the zone's own columns, runs of them, each run starting where its first column best
//     matches the column before it (rows kept together: a band's column of the picture is copied whole), crossfaded
//     over a few columns; the runs chosen once a screen (cached), the pixels taken each frame (fades, animation)
//   3 soft: the edge's colours carried out, blurred up and down and darkening outwards
//   4 the board's own: the layers drawn on past the edges as the board would on a wider screen (their maps wrap)
//   5 mirror then soft: option 1 near the edge going over into option 3 further out
// Drawing only: nothing here is read by the game.
#pragma once
#include "../video/tilegen.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wide_select {

using video::W; using video::H; using video::WW; using video::EXT;

inline int ch(uint32_t c, int i) { return int((c >> (8 * i)) & 255); }
inline uint32_t rgb(int r, int g, int b) { return 0xFF000000u | uint32_t(std::clamp(b, 0, 255)) << 16 | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(r, 0, 255)); }
inline uint32_t mix(uint32_t a, uint32_t b, float t)
{
	return rgb(int(ch(a, 0) + (ch(b, 0) - ch(a, 0)) * t + 0.5f), int(ch(a, 1) + (ch(b, 1) - ch(a, 1)) * t + 0.5f), int(ch(a, 2) + (ch(b, 2) - ch(a, 2)) * t + 0.5f));
}

// the plate with its holes filled along each row (from the nearest pixels either side, blended), one side's view:
// column i of side s is i pixels in from that edge. A row with nothing: 0s.
constexpr int FZ = video::TileGen::PLATE_IN - 2;   // (as far in as the plate goes, aligned)
struct Filled
{
	std::vector<uint32_t> px;   // H x FZ, side's columns
	std::vector<uint8_t> row_ok;
	void make(const uint32_t *plate, int side)
	{
		px.assign(size_t(FZ) * H, 0);
		row_ok.assign(H, 0);
		for (int y = 0; y < H; y++) {
			uint32_t *o = &px[size_t(y) * FZ];
			for (int i = 0; i < FZ; i++) o[i] = plate[size_t(y) * W + (side ? W - 1 - i : i)];
			int prev = -1;
			for (int i = 0; i <= FZ; i++) {
				if (i < FZ && !(o[i] >> 24)) continue;
				if (i < FZ) row_ok[y] = 1;
				if (i > prev + 1) {   // (a hole between prev and i)
					for (int j = prev + 1; j < i; j++) {
						if (prev < 0 && i >= FZ) break;
						if (prev < 0) o[j] = o[i];
						else if (i >= FZ) o[j] = o[prev];
						else o[j] = mix(o[prev], o[i], float(j - prev) / float(i - prev));
					}
				}
				prev = i;
			}
		}
	}
	uint32_t at(int y, int i) const { return px[size_t(y) * FZ + i]; }
};

// How far in from a side the background goes before the screen's pictures start (a frame's border, the grid's
// portraits, a panel's stills): by bands of 8 lines, columns in from the edge while each column's mean colour stays
// near those before it (and near the edge's), and is mostly background at all; 8 to 160 (-1: a band with none)
inline void clean_zones(const Filled &f, const uint32_t *plate, int side, std::vector<int> &bands)
{
	bands.assign(H / 8, -1);
	for (int b = 0; b < H / 8; b++) {
		bool any = false;
		for (int y = b * 8; y < b * 8 + 8; y++) any |= f.row_ok[y] != 0;
		if (!any) continue;
		float ref[3] = {}, edge[3] = {};
		int i = 0;
		for (; i < 160; i++) {
			int m[3] = {}, holes = 0;
			for (int y = b * 8; y < b * 8 + 8; y++) {
				holes += !(plate[size_t(y) * W + (side ? W - 1 - i : i)] >> 24);
				for (int c = 0; c < 3; c++) m[c] += ch(f.at(y, i), c);
			}
			float col[3];
			for (int c = 0; c < 3; c++) col[c] = m[c] / 8.0f;
			if (i == 0) for (int c = 0; c < 3; c++) ref[c] = edge[c] = col[c];
			float dr = 0, de = 0;
			for (int c = 0; c < 3; c++) { dr += std::fabs(col[c] - ref[c]); de += std::fabs(col[c] - edge[c]); }
			if (i > 0 && (holes > 4 || dr > 45 || de > 120)) break;
			for (int c = 0; c < 3; c++) ref[c] += (col[c] - ref[c]) * 0.3f;
			if (i < 4) for (int c = 0; c < 3; c++) edge[c] += (col[c] - edge[c]) * 0.5f;
		}
		bands[size_t(b)] = std::max(i, 8);
	}
}

struct State
{
	int key = -1, option = -1;
	// option 2: each side's columns (d out from the edge) as a source column (in from that edge) and a blend with the
	// one before (a run's start: crossfaded over a few columns)
	struct Col { int16_t src, prev; float t; };
	// each side's bands' zones, and its segments (bands in a row with about the same zone: the least of them), each
	// with its own runs (option 2)
	std::vector<int> bands[2];
	struct Seg { int y0, y1, zone; std::vector<Col> cols; };
	std::vector<Seg> segs[2];
	std::vector<int16_t> seg_of[2];   // (each line's segment, -1: none)
	double solved_lum[2] = {};
	Filled f[2];
};

// mean luminance of the plate's first Z columns in from a side
inline double zone_lum(const Filled &f, int z)
{
	double s = 0; long n = 0;
	for (int y = 0; y < H; y += 4) for (int i = 0; i < z; i += 2) { const uint32_t c = f.at(y, i); s += ch(c, 0) + ch(c, 1) + ch(c, 2); n++; }
	return n ? s / n : 0;
}

// option 2's runs for a side: greedy, out from the edge; each run read outwards through the source (i rising: the
// picture's columns further in, mirrored: the next column out is the one further in), its start the column whose
// neighbour best matches the last column made, not one used near before
inline void solve(const Filled &f, std::vector<State::Col> &cols, int Z, int y0, int y1)
{
	const int RUN_MIN = std::max(6, Z / 4), RUN_MAX = std::max(RUN_MIN + 2, Z / 2), FADE = std::min(6, RUN_MIN / 2), YS = 2;
	cols.assign(EXT, { 0, -1, 0 });
	if (Z < 24) {   // (too little to make runs from: mirrored, as option 1)
		for (int d = 0; d < EXT; d++) { const int m = d % (2 * Z); cols[size_t(d)] = { int16_t(m < Z ? m : 2 * Z - 1 - m), -1, 0 }; }
		return;
	}
	std::vector<int> col_used(FZ, -1000);
	auto dist = [&](int a, int b) {   // two source columns' difference, all rows
		long s = 0;
		for (int y = y0; y < y1; y += YS) {
			const uint32_t p = f.at(y, a), q = f.at(y, b);
			for (int c = 0; c < 3; c++) { const int d = ch(p, c) - ch(q, c); s += d * d; }
		}
		return s;
	};
	// out from the edge: d 0 is the column just past it, whose neighbour is the edge's own column 0. The source runs
	// go towards the screen's middle (i rising), as a mirror's would, from a start chosen to match.
	int last = 0;   // (the source column the last column out came from)
	int d = 0;
	uint32_t seed = 12345;
	while (d < EXT) {
		// the start: s such that column s - 1 (its neighbour back towards the edge... read as the one before) matches
		// the last made: s in [1, Z - RUN_MIN)
		long best = -1; int bs = 1;
		for (int s = 1; s + RUN_MIN < Z; s++) {
			if (d - col_used[s] < 3 * Z / 4 && d > 0) continue;   // (not what was just used: a repeat)
			long v = dist(s - 1, last);
			seed = seed * 1664525u + 1013904223u;
			v += long(seed >> 24) * 8;   // (ties broken at random, the same each time)
			if (best < 0 || v < best) { best = v; bs = s; }
		}
		if (d == 0) bs = 0;   // (the first run: the edge's own columns mirrored, no seam at the 4:3 edge)
		seed = seed * 1664525u + 1013904223u;
		const int len = std::max(1, std::min({ RUN_MIN + int(seed >> 16) % (RUN_MAX - RUN_MIN), EXT - d, Z - bs }));
		const int prev_src = last;
		for (int j = 0; j < len && d < EXT; j++, d++) {
			const int s = std::min(bs + j, Z - 1);
			State::Col c{ int16_t(s), -1, 0 };
			if (d > 0 && j < FADE && bs != 0) {   // (crossfaded from the run before, carried on)
				c.prev = int16_t(std::min(prev_src + 1 + j, FZ - 1));
				c.t = float(j + 1) / float(FADE + 1);
			}
			cols[size_t(d)] = c;
			col_used[s] = d;
			last = s;
		}
	}
}

// out: WW x H; its sides (x < EXT, x >= EXT + W) written. key: the screen (a new one: option 2 solved again).
// reach: how far out the picture shows (up to EXT): the sides made only that far
inline void make(int option, int key, const std::vector<uint32_t> &plate, const std::vector<uint32_t> &hw, std::vector<uint32_t> &out, State &st, int reach = EXT)
{
	reach = std::clamp(reach, 0, EXT);
	out.assign(size_t(WW) * H, 0);
	if (plate.size() != size_t(W) * H) return;
	if (option == 4) {
		if (hw.size() != size_t(WW) * H) return;
		for (int y = 0; y < H; y++) {
			memcpy(&out[size_t(y) * WW], &hw[size_t(y) * WW], size_t(EXT) * 4);
			memcpy(&out[size_t(y) * WW + EXT + W], &hw[size_t(y) * WW + EXT + W], size_t(EXT) * 4);
		}
		return;
	}
	for (int s = 0; s < 2; s++) st.f[s].make(plate.data(), s);
	// (the zones and option 2's runs made again for a new screen, or once it's much brighter than when made: a fade in)
	for (int s = 0; s < 2; s++) {
		const double lum = zone_lum(st.f[s], 64);
		if (st.key == key && st.option == option && !st.seg_of[s].empty() && lum <= st.solved_lum[s] * 1.25 + 6) continue;
		st.solved_lum[s] = lum;
		clean_zones(st.f[s], plate.data(), s, st.bands[s]);
		st.segs[s].clear();
		st.seg_of[s].assign(H, -1);
		for (int b = 0; b < H / 8; b++) {
			const int z = st.bands[s][size_t(b)];
			if (z < 0) continue;
			State::Seg *g = st.segs[s].empty() ? nullptr : &st.segs[s].back();
			if (g && g->y1 == b * 8 && std::abs(z - g->zone) <= 4) { g->y1 += 8; g->zone = std::min(g->zone, z); }
			else st.segs[s].push_back({ b * 8, b * 8 + 8, z, {} });
		}
		for (size_t g = 0; g < st.segs[s].size(); g++) {
			State::Seg &sg = st.segs[s][g];
			for (int y = sg.y0; y < sg.y1; y++) st.seg_of[s][size_t(y)] = int16_t(g);
			solve(st.f[s], sg.cols, sg.zone, sg.y0, sg.y1);
		}
		if (getenv("VF3_WIDE_SELECT_LOG")) {
			fprintf(stderr, "(wide select) screen %d side %d lum %.0f segments", key, s, lum);
			for (auto &g : st.segs[s]) fprintf(stderr, " %d-%d:%d", g.y0, g.y1, g.zone);
			fprintf(stderr, "\n");
		}
	}
	st.key = key;
	st.option = option;
	auto put = [&](int s, int y, int d, uint32_t c) { out[size_t(y) * WW + (s ? EXT + W + d : EXT - 1 - d)] = c; };
	auto at = [&](int s, int y, int d) { return out[size_t(y) * WW + (s ? EXT + W + d : EXT - 1 - d)]; };
	if (option == 1 || option == 5) {
		for (int s = 0; s < 2; s++)
			for (int y = 0; y < H; y++) {
				if (!st.f[s].row_ok[y] || st.seg_of[s][size_t(y)] < 0) continue;
				const int P = st.segs[s][size_t(st.seg_of[s][size_t(y)])].zone;
				for (int d = 0; d < reach; d++) { const int m = d % (2 * P); put(s, y, d, st.f[s].at(y, m < P ? m : 2 * P - 1 - m)); }
			}
	} else if (option == 2) {
		for (int s = 0; s < 2; s++)
			for (int y = 0; y < H; y++) {
				if (!st.f[s].row_ok[y] || st.seg_of[s][size_t(y)] < 0) continue;
				const auto &cols = st.segs[s][size_t(st.seg_of[s][size_t(y)])].cols;
				for (int d = 0; d < reach; d++) {
					const State::Col &c = cols[size_t(d)];
					uint32_t v = st.f[s].at(y, c.src);
					if (c.prev >= 0) v = mix(st.f[s].at(y, c.prev), v, c.t);
					put(s, y, d, v);
				}
			}
	}
	if (option == 3 || option == 5) {
		// 3: the edge's colours (its 8 columns averaged) carried out, blurred over R lines up and down further out,
		// darkening to 40% at 200 pixels; 5: option 1's mirror near the edge going over into that (from 12 to 96
		// pixels out), darkening to 55%
		const int A = 8, R = 10;
		for (int s = 0; s < 2; s++) {
			std::vector<int> e(size_t(H) * 3, 0), ok(H, 0);
			for (int y = 0; y < H; y++) {
				if (!st.f[s].row_ok[y]) continue;
				ok[y] = 1;
				for (int i = 0; i < A; i++) for (int c = 0; c < 3; c++) e[size_t(y) * 3 + c] += ch(st.f[s].at(y, i), c);
				for (int c = 0; c < 3; c++) e[size_t(y) * 3 + c] /= A;
			}
			for (int y = 0; y < H; y++) {
				if (!ok[y]) continue;
				int sum[3] = {}, n = 0;
				for (int k = -R; k <= R; k++) {
					const int yy = y + k;
					if (yy < 0 || yy >= H || !ok[yy]) continue;
					for (int c = 0; c < 3; c++) sum[c] += e[size_t(yy) * 3 + c];
					n++;
				}
				for (int d = 0; d < reach; d++) {
					const float b = std::min(1.0f, d / 24.0f), k = 1.0f - (option == 5 ? 0.45f : 0.6f) * std::min(1.0f, d / 200.0f);
					float c3[3];
					for (int c = 0; c < 3; c++) c3[c] = (e[size_t(y) * 3 + c] * (1 - b) + float(sum[c]) / n * b) * k;
					const uint32_t soft = rgb(int(c3[0]), int(c3[1]), int(c3[2]));
					if (option == 3) put(s, y, d, soft);
					else {
						float t = std::clamp((d - 12) / 84.0f, 0.0f, 1.0f);
						t = t * t * (3 - 2 * t);
						const uint32_t m = at(s, y, d);
						// (the mirror darkened as the soft colour is, so they meet)
						const float km = 1.0f - 0.45f * std::min(1.0f, d / 200.0f);
						put(s, y, d, mix(rgb(int(ch(m, 0) * km), int(ch(m, 1) * km), int(ch(m, 2) * km)), soft, t));
					}
				}
			}
		}
	}
}

} // namespace wide_select
