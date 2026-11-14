// Widescreen's sides on the character select and the stage screen (VF3_WIDE_SELECT: 2 by default, 0 the bars as before),
// made from the screen's own background (TileGen's plate: the pixels of solid tiles, the pictures the letters,
// frames and cursors are drawn over). Only the sides: the 4:3 picture stays as the game draws it, in the middle.
// Each side is cut into bands of lines with about the same clean zone (how far in from the edge the background goes
// before the screen's pictures start: a frame's border, the grid's portraits, a panel's stills; 8 to 160 pixels).
//   1 mirror: the zone's columns mirrored out, and back (the game's pixels, flipped)
//   2 fill: a content-aware fill, made once a screen (wide_select_fill.h); on the stage screen the preview isn't
//     carried on: the panels' metal goes on down beside it as a frame. 6: the same, dark beside the preview
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
// (the aligned picture's last 2 columns are the one before them again, aligned()'s: not the game's, so a line down
// the right edge; the right side starts there, over them)
constexpr int RCUT = 2;
constexpr int FZ = W - RCUT;
struct Filled
{
	std::vector<uint32_t> px;   // H x FZ, side's columns
	std::vector<uint8_t> row_ok;
	void make(const uint32_t *plate, int side)
	{
		// short gaps along a line (a frame's border, up to 8 pixels) from either side; the rest from the lines
		// above and below in the same column (a line's start with no background: not smeared from across the screen);
		// what's left, along the line
		px.assign(size_t(FZ) * H, 0);
		row_ok.assign(H, 0);
		std::vector<uint8_t> got(size_t(FZ) * H, 0);
		for (int y = 0; y < H; y++) {
			uint32_t *o = &px[size_t(y) * FZ];
			uint8_t *g = &got[size_t(y) * FZ];
			for (int i = 0; i < FZ; i++) { o[i] = plate[size_t(y) * W + (side ? W - 1 - RCUT - i : i)]; g[i] = (o[i] >> 24) != 0; row_ok[y] |= g[i]; }
			int prev = -1;
			for (int i = 0; i < FZ; i++) {
				if (!(o[i] >> 24)) continue;
				if (prev >= 0 && i - prev > 1 && i - prev <= 9)
					for (int j = prev + 1; j < i; j++) { o[j] = mix(o[prev], o[i], float(j - prev) / float(i - prev)); g[j] = 1; }
				prev = i;
			}
		}
		for (int i = 0; i < FZ; i++) {
			int prev = -1;
			for (int y = 0; y <= H; y++) {
				if (y < H && !got[size_t(y) * FZ + i]) continue;
				if (y - prev > 1)
					for (int j = prev + 1; j < y; j++) {
						uint32_t &v = px[size_t(j) * FZ + i];
						if (prev < 0 && y >= H) break;
						if (prev < 0) v = px[size_t(y) * FZ + i];
						else if (y >= H) v = px[size_t(prev) * FZ + i];
						else v = mix(px[size_t(prev) * FZ + i], px[size_t(y) * FZ + i], float(j - prev) / float(y - prev));
					}
				prev = y;
			}
		}
		for (int y = 0; y < H; y++) for (int i = 0; i < FZ && !row_ok[y]; i++) row_ok[y] = px[size_t(y) * FZ + i] != 0;
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
				holes += !(plate[size_t(y) * W + (side ? W - 1 - RCUT - i : i)] >> 24);
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
		bands[size_t(b)] = std::max(i, 1);
	}
}

} // namespace wide_select
#include "wide_select_fill.h"
namespace wide_select {

struct State
{
	FillState fill;
	int key = -1, option = -1;
	// (options 1, 3, 5) each side's bands' zones, and its segments (bands in a row with about the same zone: the least)
	std::vector<int> bands[2];
	struct Seg { int y0, y1, zone; };
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

// out: WW x H; its sides (x < EXT, x >= EXT + W) written. key: the screen (a new one: made again).
// reach: how far out the picture shows (up to EXT): the sides made only that far. frame: the game's frame shown (option
// 2's fill counts frames, not pictures)
inline void make(int option, int key, const std::vector<uint32_t> &plate, const std::vector<uint32_t> &hw, std::vector<uint32_t> &out, State &st, int reach = EXT,
                 const std::vector<uint32_t> &above = {}, const std::vector<uint32_t> &below = {}, uint64_t frame = 0)
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
	if (option == 2 || option == 6) { fill(key, plate, above, below, out, st.fill, reach, option == 6, frame); return; }
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
			const int z = st.bands[s][size_t(b)] < 0 ? -1 : std::max(st.bands[s][size_t(b)], 8);
			if (z < 0) continue;
			State::Seg *g = st.segs[s].empty() ? nullptr : &st.segs[s].back();
			if (g && g->y1 == b * 8 && std::abs(z - g->zone) <= 4) { g->y1 += 8; g->zone = std::min(g->zone, z); }
			else st.segs[s].push_back({ b * 8, b * 8 + 8, z });
		}
		for (size_t g = 0; g < st.segs[s].size(); g++) {
			State::Seg &sg = st.segs[s][g];
			for (int y = sg.y0; y < sg.y1; y++) st.seg_of[s][size_t(y)] = int16_t(g);
		}
		if (getenv("VF3_WIDE_SELECT_LOG")) {
			fprintf(stderr, "(wide select) screen %d side %d lum %.0f segments", key, s, lum);
			for (auto &g : st.segs[s]) fprintf(stderr, " %d-%d:%d", g.y0, g.y1, g.zone);
			fprintf(stderr, "\n");
		}
	}
	st.key = key;
	st.option = option;
	auto put = [&](int s, int y, int d, uint32_t c) { out[size_t(y) * WW + (s ? EXT + W - RCUT + d : EXT - 1 - d)] = c; };
	auto at = [&](int s, int y, int d) { return out[size_t(y) * WW + (s ? EXT + W - RCUT + d : EXT - 1 - d)]; };
	if (option == 1 || option == 5) {
		for (int s = 0; s < 2; s++)
			for (int y = 0; y < H; y++) {
				if (!st.f[s].row_ok[y] || st.seg_of[s][size_t(y)] < 0) continue;
				const int P = st.segs[s][size_t(st.seg_of[s][size_t(y)])].zone;
				for (int d = 0; d < reach + (s ? RCUT : 0); d++) { const int m = d % (2 * P); put(s, y, d, st.f[s].at(y, m < P ? m : 2 * P - 1 - m)); }
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
				for (int d = 0; d < reach + (s ? RCUT : 0); d++) {
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

// The game's own thin lines at the 4:3 edges (a box's outline round the title's timer): those across, a few lines
// thick, ending at the edge, carried on across the sides (out: the sides, as far as reach); those down the edge's last
// 3 columns, as thin, closing a box there, taken out of the 2D above (above: a copy of the layers above the 3D), the
// background (plate) put back. A line is the 2D's own (its pixels not the background's) and of one colour. skip_y0
// to skip_y1: lines left alone (Dural's cell, past the right edge).
inline void edge_lines(std::vector<uint32_t> &above, const std::vector<uint32_t> &plate, std::vector<uint32_t> &out, int reach, int skip_y0 = 0, int skip_y1 = 0)
{
	if (plate.size() != size_t(W) * H || above.size() != size_t(W) * H) return;
	auto diff = [](uint32_t a, uint32_t b) { return std::abs(ch(a, 0) - ch(b, 0)) + std::abs(ch(a, 1) - ch(b, 1)) + std::abs(ch(a, 2) - ch(b, 2)); };
	auto ui = [&](int y, int x) {
		const uint32_t a = above[size_t(y) * W + x], p = plate[size_t(y) * W + x];
		return (a >> 24) && (!(p >> 24) || diff(a, p) > 36);
	};
	for (int s = 0; s < 2; s++) {
		const int e = s ? W - 1 - RCUT : 0, dir = s ? -1 : 1;
		std::vector<uint8_t> across(H, 0);
		for (int y = 0; y < H; y++) {
			if (y >= skip_y0 && y < skip_y1) continue;
			int n = 0;
			const uint32_t c = above[size_t(y) * W + e];
			while (n < 24 && ui(y, e + dir * n) && diff(above[size_t(y) * W + e + dir * n], c) < 40) n++;
			across[size_t(y)] = n >= 12;
		}
		for (int y = 0; y < H; y++) {
			if (!across[size_t(y)]) continue;
			int y1 = y;
			while (y1 < H && across[size_t(y1)]) y1++;
			if (y1 - y <= 3)   // (thin: a line, not a band of text)
				for (int yy = y; yy < y1; yy++) {
					const uint32_t c = above[size_t(yy) * W + e];
					if (!(out[size_t(yy) * WW + (s ? EXT + W : EXT - 1)] >> 24)) continue;   // (no side there yet)
					for (int d = 0; d < reach + (s ? RCUT : 0); d++) out[size_t(yy) * WW + (s ? EXT + W - RCUT + d : EXT - 1 - d)] = c;
				}
			y = y1 - 1;
		}
		for (int k = 0; k < 3; k++) {
			const int x = e + dir * k;
			for (int y = 0; y < H; y++) {
				auto thin = [&](int yy) { return yy >= 0 && yy < H && !(yy >= skip_y0 && yy < skip_y1) && ui(yy, x) && !ui(yy, x + dir * 3) && !across[size_t(yy)]; };
				if (!thin(y)) continue;
				int y1 = y;
				while (y1 < H && thin(y1)) y1++;
				if (y1 - y >= 6)
					for (int yy = y; yy < y1; yy++) above[size_t(yy) * W + x] = plate[size_t(yy) * W + x] >> 24 ? plate[size_t(yy) * W + x] : 0;
				y = y1 - 1;
			}
		}
	}
}

} // namespace wide_select
