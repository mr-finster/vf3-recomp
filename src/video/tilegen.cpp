// The tile generator. See tilegen.h.
//
// Its RAM (as the device stores it, little-endian 32-bit words):
//   00000-F5FFF  tile patterns
//   F6000-F6FFF  per-line X scroll, 16 bits a line, a table of 0x400 bytes per layer (two lines a word, the
//                even line in the high half)
//   F7000-F77FF  the mask, a word a line: bits 31-16 for A/A', 15-0 for B/B', bit 15 the leftmost 32 pixels;
//                1 shows the primary layer (A or B), 0 the alternate
//   F8000-FFFFF  the four name tables, 0x2000 bytes each: 64x64 16-bit entries, two to a word (the left tile in
//                the high half)
//   100000-      the palette, a word a colour: bit 15 transparent, then 5 bits each of blue, green, red
// Registers (words): 0x20 bits 8-11 layer above the 3D, 12-15 layer is 4-bit; 0x40, 0x44 colour offsets for A/A'
// and B/B' (signed bytes of red 0-7, green 8-15, blue 16-23, doubled); 0x60-0x6C per layer: bit 31 enabled, Y
// scroll 16-24, bit 15 X from the line table, X scroll 0-9.
// A name table entry, 4-bit: pattern ((e & 0x3FFF) << 1 | e >> 15) * 32 bytes, colour (e & 0x7FF0) | texel;
// 8-bit: pattern (e & 0x3FFF) * 64 bytes, colour (e & 0x7F00) | texel. The leftmost texel is the top bits.
#include "tilegen.h"
#include "../machine/model3.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace video {

const uint32_t *TileGen::regs_of(const m3::Model3 &board) const { return regs_now ? regs_now : board.tile_regs; }

void TileGen::clear()
{
	std::fill(below.begin(), below.end(), 0);
	std::fill(above.begin(), above.end(), 0);
	std::fill(below_wide.begin(), below_wide.end(), 0);
	std::fill(std::begin(scroll.below_columns), std::end(scroll.below_columns), 0);
}

bool TileGen::scrolled(const Scroll &b, const Scroll &n, const std::vector<uint32_t> &bp, const std::vector<uint32_t> &np,
	float &dx, float &dy)
{
	dx = dy = 0;
	int layer = 0, bl = 0;
	for (int k = 1; k < 4; k++) if (n.below_columns[k] > n.below_columns[layer]) layer = k;
	for (int k = 1; k < 4; k++) if (b.below_columns[k] > b.below_columns[bl]) bl = k;
	if (n.below_columns[layer] < 4 * H || bl != layer) return false;   // (under a quarter of it, or another layer)
	if (bp.size() != size_t(W) * H || np.size() != size_t(W) * H) return false;
	// the registers' change: the median of the lines' (a line's scroll can differ: the heat haze's)
	std::vector<int> mx, my;
	mx.reserve(H); my.reserve(H);
	auto wrap = [](int d) { d &= 511; return d >= 256 ? d - 512 : d; };
	for (int y = 0; y < H; y++) {
		mx.push_back(wrap(n.sx[layer][y] - b.sx[layer][y]));
		my.push_back(wrap(n.sy[layer][y] - b.sy[layer][y]));
	}
	std::nth_element(mx.begin(), mx.begin() + H / 2, mx.end());
	std::nth_element(my.begin(), my.begin() + H / 2, my.end());
	const int rx = mx[H / 2], ry = my[H / 2];
	// now(x, y) is before(x + dx, y + dy): the candidate with the most of the layer's pixels the same
	auto score = [&](int sx, int sy, int &n_seen) {
		int same = 0;
		n_seen = 0;
		for (int y = 8; y < H - 10; y += 6) {
			const int py = y + sy;
			if (py < 0 || py >= H - 2) continue;
			const uint32_t *row = &np[size_t(y) * W], *prow = &bp[size_t(py) * W];
			for (int x = 6; x < W - 8; x += 5) {
				const int px = x + sx;
				if (px < 0 || px >= W - 2) continue;
				const uint32_t a = row[x], c = prow[px];
				if (!(a >> 24) || !(c >> 24)) continue;
				n_seen++;
				same += a == c;
			}
		}
		return same;
	};
	int best_x = 0, best_y = 0, best_same = -1, best_seen = 0;
	// (nearest the registers' change first: a sky the same along its rows matches more than one)
	std::vector<std::pair<int, int>> ks;
	for (int ky = -3; ky <= 3; ky++)
		for (int kx = -3; kx <= 3; kx++) ks.push_back({ kx, ky });
	std::stable_sort(ks.begin(), ks.end(), [](auto a, auto b) { return std::abs(a.first) + std::abs(a.second) < std::abs(b.first) + std::abs(b.second); });
	for (auto [kx, ky] : ks) {
			const int cx = rx + 8 * kx, cy = ry + 8 * ky;
			if (std::abs(cx) > 40 || std::abs(cy) > 40) continue;
			int seen = 0;
			const int same = score(cx, cy, seen);
			if (seen > 400 && same * best_seen > best_same * seen) { best_same = same; best_seen = seen; best_x = cx; best_y = cy; }
		}
	if (best_same < 0 || best_same * 10 < best_seen * 9) return false;   // (no move lines it up: a cut, a change)
	dx = float(best_x);
	dy = float(best_y);
	return true;
}

namespace {

struct Offset { int r, g, b; };

Offset offset_of(uint32_t reg)
{
	return { int(int8_t(reg)) * 2, int(int8_t(reg >> 8)) * 2, int(int8_t(reg >> 16)) * 2 };
}

// a layer's X scroll on line y
int scroll_x(const uint32_t *vram, const uint32_t *regs, int layer, unsigned y)
{
	const uint32_t r = regs[0x60 / 4 + layer];
	return (r & 0x8000) ? int((vram[(0xF6000 + layer * 0x400) / 4 + y / 2] >> ((1 - (y & 1)) * 16)) & 0xFFFF) : int(r & 0x3FF);
}

// a name table's entry (the map wraps)
uint32_t name_at(const uint32_t *names, int row, int col)
{
	const uint32_t w = names[(row & 63) * 32 + ((col & 63) >> 1)];
	return (col & 1) ? (w & 0xFFFF) : (w >> 16);
}

} // namespace

// is the row on screen (i from 0 to n) mostly one run (a panorama's, not the tops of clouds: patterns only where
// there's cloud, the rest one blank tile)?
template <typename F, typename G> static bool one_run(F &&pat, int n, const G &next)
{
	int seq = 0;
	for (int i = 1; i < n; i++) seq += next(pat(i - 1)) == pat(i);
	return seq * 4 >= (n - 1) * 3;
}

int TileGen::next_along(int layer, int q, int d) const
{
	const Pano &P = pano[layer];
	// the row holding q, if its wrap was seen
	auto it = P.rows.upper_bound(q);
	int first = -1, len = 0;
	if (it != P.rows.begin()) {
		auto b = std::prev(it);
		if (q < b->first + b->second) { first = b->first; len = b->second; }
	}
	if (d > 0) {
		if (first >= 0 && q + 1 == first + len) return first;
		auto sk = skip_fwd[layer].find(q);
		return sk != skip_fwd[layer].end() ? sk->second : q + 1;
	}
	if (first >= 0 && q == first) return first + len - 1;
	auto sk = skip_bwd[layer].find(q);
	return sk != skip_bwd[layer].end() ? sk->second : q - 1;
}

void TileGen::add_row(int layer, int b, int len)
{
	Pano &P = pano[layer];
	const int a = b + len - 1;
	auto it = P.rows.lower_bound(b);
	if (it != P.rows.begin() && std::prev(it)->first + std::prev(it)->second > b) --it;
	while (it != P.rows.end() && it->first <= a) it = P.rows.erase(it);
	P.rows[b] = len;
}

// a layer's texel (bit 15: transparent): its map's entry e, the pattern's row fy and column fx
static uint32_t texel_at(const uint32_t *vram, const uint32_t *pal, uint32_t e, bool four, int fy, int fx)
{
	if (four) {
		const uint32_t pat = (((e & 0x3FFF) << 1) | (e >> 15)) * 8;
		return pal[((e & 0x7FF0) | ((vram[(pat + fy) & 0x3FFFF] >> ((7 - fx) * 4)) & 0xF)) & 0x7FFF];
	}
	const uint32_t pat = (e & 0x3FFF) * 16;
	return pal[((e & 0x7F00) | ((vram[(pat + fy * 2 + (fx >> 2)) & 0x3FFFF] >> ((3 - (fx & 3)) * 8)) & 0xFF)) & 0x7FFF];
}

// two remembered colours the same (but a little)
static bool same_colour(uint16_t u, uint16_t v)
{
	if ((u & 0x8000) != (v & 0x8000)) return false;
	if (u & 0x8000) return true;
	for (int sh = 0; sh < 15; sh += 5)
		if (std::abs(int(u >> sh & 31) - int(v >> sh & 31)) > 3) return false;
	return true;
}

void TileGen::place_seen(const m3::Model3 &board)
{
	const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
	const uint32_t *pal = vram + 0x40000;
	const uint32_t *regs = regs_of(board);
	for (int layer = 0; layer < 4; layer++) {
		Seen &m = seen[layer];
		if (!(regs[0x60 / 4 + layer] & 0x80000000u) || ((regs[0x20 / 4] >> (8 + layer)) & 1) || m.at_x.empty()) continue;
		if (layer == 3 && (backdrop.ok || backdrop.known)) { m.last_sx = -1; continue; }   // (not needed: the game's map)
		const int sx = scroll_x(vram, regs, layer, 0) & 511, sy = int((regs[0x60 / 4 + layer] >> 16) & 0x1FF);
		const bool four = (regs[0x20 / 4] >> (12 + layer)) & 1;
		const uint32_t *names = vram + (0xF8000 + layer * 0x2000) / 4;
		m.unsure = false;
		if (m.empty || m.last_sx < 0) { m.x = sx; m.y = sy; m.last_sx = sx; m.last_sy = sy; continue; }
		const int32_t bx = m.x + (((sx - m.last_sx + 256) & 511) - 256), by = m.y + (((sy - m.last_sy + 256) & 511) - 256);
		// How well the screen (a sample of it) agrees with what's remembered, the screen moved kx, ky whole tiles
		auto agreement = [&](int kx, int ky, int step, int &cmp) {
			cmp = 0;
			int agree = 0;
			for (int y = 1; y < H; y += step) {
				const int row = (y + sy) & 511;
				const int32_t ay = by + 8 * ky + y;
				const uint32_t *nr = names + (row / 8) * 32;
				const size_t line = size_t(ay & 511) * R;
				for (int x = 2; x < W; x += step) {
					const int32_t ax = bx + 8 * kx + x;
					const size_t i = line + size_t(ax & (R - 1));
					if (m.at_x[i] != ax || m.at_y[i] != ay) continue;
					const int px = (x + sx) & 511;
					const uint32_t pw = nr[px / 16];
					cmp++;
					agree += same_colour(m.col[i], uint16_t(texel_at(vram, pal, (px & 8) ? (pw & 0xFFFF) : (pw >> 16), four, row & 7, px & 7)));
				}
			}
			return agree;
		};
		// the best agreeing in proportion (of enough compared), nearest the last frame's move of those as good: near
		// it first, then anywhere
		int best_x = 0, best_y = 0, best_a = -1, best_c = 0;
		std::vector<std::array<int, 4>> tried;   // (kx, ky, agreeing, compared)
		auto search = [&](int x0, int x1, int y0, int y1, int step) {
			tried.clear();
			// (a rough look first, at twice the step: closely only those near the best of it)
			std::vector<std::array<int, 3>> rough;   // (kx, ky, agreeing per thousand)
			int rough_best = -1;
			for (int ky = y0; ky <= y1; ky++)
				for (int kx = x0; kx <= x1; kx++) {
					int c;
					const int a = agreement(kx, ky, step * 2, c);
					const int pm = c >= 40 ? a * 1000 / c : -1;
					rough.push_back({ kx, ky, pm });
					rough_best = std::max(rough_best, pm);
				}
			for (const auto &rk : rough) {
				if (rk[2] >= 0 && rk[2] < rough_best - 50) continue;
				const int kx = rk[0], ky = rk[1];
				{
					int c;
					const int a = agreement(kx, ky, step, c);
					if (c < 150) continue;
					tried.push_back({ kx, ky, a, c });
					const int64_t l = int64_t(a) * best_c, r = int64_t(best_a) * c;
					const bool nearer = std::abs(kx - m.kx) + std::abs(ky - m.ky) < std::abs(best_x - m.kx) + std::abs(best_y - m.ky);
					if (best_a < 0 || l > r + int64_t(c) * best_c / 100 || (l >= r && nearer)) { best_x = kx; best_y = ky; best_a = a; best_c = c; }
				}
			}
		};
		// The last move first: all of it agreeing (the usual frame), none nearby can do better (as good, the nearer
		// wins: it), so the rest is only looked at for one nearly as good (unsure), the first found enough
		bool done = false;
		{
			int c;
			const int a = agreement(m.kx, m.ky, 6, c);
			if (c >= 150 && a == c) {
				best_x = m.kx; best_y = m.ky; best_a = a; best_c = c;
				tried.clear();
				tried.push_back({ m.kx, m.ky, a, c });
				for (int ky = m.ky - 2; ky <= m.ky + 2 && tried.size() < 2; ky++)
					for (int kx = m.kx - 3; kx <= m.kx + 3 && tried.size() < 2; kx++) {
						if (kx == m.kx && ky == m.ky) continue;
						int rc;
						const int ra = agreement(kx, ky, 12, rc);
						if (rc < 40 || ra * 1000 < rc * 950) continue;
						int fc;
						const int fa = agreement(kx, ky, 6, fc);
						if (fc >= 150 && int64_t(fa) * best_c * 100 >= int64_t(best_a) * fc * 98) tried.push_back({ kx, ky, fa, fc });
					}
				done = true;
			}
		}
		if (!done) search(m.kx - 3, m.kx + 3, m.ky - 2, m.ky + 2, 6);
		if (best_a < 0 || best_a * 10 < best_c * 9) { best_a = -1; search(-12, 12, -8, 8, 12); }
		static const bool no_global = getenv("VF3_NO_GLOBAL") != nullptr;   // (tests: forgotten at a cut)
		if (!no_global && (best_a < 0 || best_a * 10 < best_c * 9)) {
			// A cut (a round's start, a replay): the screen looked for all along what's remembered (roughly, then the
			// likeliest closely)
			std::vector<std::array<int, 3>> rough;   // (agreeing per thousand, kx, ky)
			for (int ky = -16; ky <= 16; ky++)
				for (int kx = -R / 16; kx < R / 16; kx++) {
					int c;
					const int a = agreement(kx, ky, 24, c);
					if (c >= 40 && a * 10 >= c * 9) rough.push_back({ a * 1000 / c, kx, ky });
				}
			std::sort(rough.begin(), rough.end(), [](const auto &a, const auto &b) { return a[0] > b[0]; });
			best_a = -1;
			tried.clear();
			for (size_t i = 0; i < rough.size() && i < 12; i++) {
				int c;
				const int a = agreement(rough[i][1], rough[i][2], 6, c);
				if (c < 150) continue;
				tried.push_back({ rough[i][1], rough[i][2], a, c });
				if (best_a < 0 || int64_t(a) * best_c > int64_t(best_a) * c) { best_x = rough[i][1]; best_y = rough[i][2]; best_a = a; best_c = c; }
			}
		}
		if (best_a < 0 || best_a * 10 < best_c * 9) {
			// (nothing remembered to see, or none agrees: another stage)
			if (best_a >= 0) m.forget();
			best_x = best_y = 0;
		}
		// unsure: another place a tile or more away agreeing nearly as well (a sky smooth that way)
		m.unsure = false;
		for (const auto &t : tried)
			if ((t[0] != best_x || t[1] != best_y) && best_c > 0 && int64_t(t[2]) * best_c * 100 >= int64_t(best_a) * t[3] * 98) m.unsure = true;
		m.x = bx + 8 * best_x;
		m.y = by + 8 * best_y;
		m.kx = best_x;
		m.ky = best_y;
		m.last_sx = sx;
		m.last_sy = sy;
	}
}

uint32_t TileGen::backdrop_entry(const m3::Model3 &board, int x, int y, bool table) const
{
	const Backdrop &b = backdrop;
	// (above the map its first entry, below it its last row's first, as the game fills them; across, it wraps)
	const int at = y < 0 ? 0 : y >= b.h ? (b.h - 1) * b.w : y * b.w + ((x % b.w) + b.w) % b.w;
	const uint16_t e = size_t(at) < b.map.size() ? b.map[size_t(at)] : board.peek16(b.desc + 8 + 2 * uint32_t(at));
	const int bank = (e >> 7) & 0x7F;
	const uint32_t v = uint32_t((e & 0x7F) + (b.seen[bank] >= 0 && !table ? b.seen[bank] : b.bank[bank])) & 0x3FFF;
	return v | (v & 0x80) << 7;   // (the palette's bit 14 the pattern's bit 7, as the game's copy sets it)
}

std::string TileGen::audit(const m3::Model3 &board) const
{
	const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
	const uint32_t *regs = regs_of(board);
	char buf[256];
	const bool on = regs[0x60 / 4 + 3] & 0x80000000u;
	const int stage = int16_t(board.peek16(board.cpu.s.r[13] - board.profile->backdrops.stage_below_r13));
	if (!on) return "off";
	if (!backdrop.ok) {
		// (tests: why: the best place's mismatches on screen, and the bank table's changes since the last frame)
		static int16_t prev[128];
		int changed = 0;
		int16_t cur[128];
		for (int i = 0; i < 128; i++) { cur[i] = int16_t(board.peek16(board.profile->backdrops.banks + 2 * uint32_t(i))); changed += cur[i] != prev[i]; }
		memcpy(prev, cur, sizeof prev);
		snprintf(buf, sizeof buf, "NOMAP stage %d desc %08x bank-changes %d", stage, board.peek32(board.profile->backdrops.descriptors + 4 * uint32_t(stage)), changed);
		return buf;
	}
	const Backdrop &b = backdrop;
	const uint32_t *names = vram + (0xF8000 + 3 * 0x2000) / 4;
	const int sy = int((regs[0x60 / 4 + 3] >> 16) & 0x1FF), s = (scroll_x(vram, regs, 3, 0) & 511) >> 3;
	int bad = 0, n = 0;
	bool on_screen_bank[128] = {};
	for (int r = sy >> 3; r < (sy >> 3) + 48; r++)
		for (int c = s; c < s + 62; c++) {
			const int rr = r & 63;
			n++;
			bad += (backdrop_entry(board, b.x0 + c, b.y0 + rr) & 0x7FFF) != (name_at(names, rr, c) & 0x7FFF);
			const int at = b.y0 + rr < 0 ? 0 : b.y0 + rr >= b.h ? (b.h - 1) * b.w : (b.y0 + rr) * b.w + (((b.x0 + c) % b.w) + b.w) % b.w;
			on_screen_bank[(board.peek16(b.desc + 8 + 2 * uint32_t(at)) >> 7) & 0x7F] = true;
		}
	int stale = 0, sides = 0;
	for (int r = sy >> 3; r < (sy >> 3) + 48; r++)
		for (int side = 0; side < 2; side++)
			for (int j = 1; j <= 24; j++) {
				const int c = side ? s + 61 + j : s - j, rr = r & 63;
				const int at = b.y0 + rr < 0 ? 0 : b.y0 + rr >= b.h ? (b.h - 1) * b.w : (b.y0 + rr) * b.w + (((b.x0 + c) % b.w) + b.w) % b.w;
				const int bank = (board.peek16(b.desc + 8 + 2 * uint32_t(at)) >> 7) & 0x7F;
				sides++;
				if (on_screen_bank[bank]) continue;
				for (int o = 0; o < 128; o++)
					if (o != bank && on_screen_bank[o] && b.bank[o] == b.bank[bank]) { stale++; break; }
			}
	snprintf(buf, sizeof buf, "map stage %d %dx%d at %d,%d screen-bad %d/%d side-stale %d/%d", stage, b.w, b.h, b.x0, b.y0, bad, n, stale, sides);
	return buf;
}

// The backdrop's banks, the table's and those seen on screen, in a sum (the searches' inputs beside the tiles)
uint64_t TileGen::banks_sum(const Backdrop &b)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < 128; i++) h = (h ^ uint64_t(uint16_t(b.bank[i])) ^ uint64_t(uint32_t(b.seen[i])) << 16) * 1099511628211ull;
	return h;
}

// All find_backdrop reads, and the backdrop as it stands, in a sum: the same sum, the same search, the same answer
uint64_t TileGen::backdrop_inputs(const m3::Model3 &board, bool full) const
{
	const Backdrop &b = backdrop;
	const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
	const uint32_t *regs = regs_of(board);
	uint64_t h = 1469598103934665603ull;
	auto add = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
	add(full); add(full_failed_sum); add(board.frame);
	for (int i = 0; i < 64; i++) add(regs[i]);
	add(vram[(0xF6000 + 3 * 0x400) / 4]);   // (the layer's first line's own scroll)
	const uint32_t *names = vram + (0xF8000 + 3 * 0x2000) / 4;
	for (int i = 0; i < 64 * 32; i++) add(names[i]);
	const int stage = int16_t(board.peek16(board.cpu.s.r[13] - board.profile->backdrops.stage_below_r13));
	add(uint64_t(uint32_t(stage)));
	if (stage >= 0 && stage <= 255) add(board.peek32(board.profile->backdrops.descriptors + 4 * uint32_t(stage)));
	for (int i = 0; i < 128; i++) add(board.peek16(board.profile->backdrops.banks + 2 * uint32_t(i)));
	add(board.peek32(board.profile->backdrops.place + 4)); add(board.peek32(board.profile->backdrops.place + 8));
	add(b.ok); add(b.known); add(b.desc); add(uint64_t(b.w) << 32 | uint32_t(b.h)); add(uint64_t(uint32_t(b.x0)) << 32 | uint32_t(b.y0));
	add(uint64_t(uint32_t(b.game_x)) << 32 | uint32_t(b.game_y)); add(uint64_t(uint32_t(b.prev_x)) << 32 | uint32_t(b.prev_y)); add(b.game_frame);
	add(b.map.size()); add(banks_sum(b));
	return h;
}

// find_backdrop for a row of the frame that isn't the backdrop's place (again and again on a screen of its own, the
// select's: 60 times a frame), not looked for again while all it reads is as when it last found nothing and changed
// nothing (it would find nothing again)
void TileGen::refind_backdrop(const m3::Model3 &board, bool full)
{
	static const bool no_skip = getenv("VF3_NO_TILE_SKIP") != nullptr;   // (tests: every search made, the picture compared)
	if (no_skip) { find_backdrop(board, full); return; }
	const uint64_t before = backdrop_inputs(board, full);
	if (before == refind_failed) return;
	find_backdrop(board, full);
	refind_failed = !backdrop.ok && backdrop_inputs(board, full) == before ? before : 0;
}

void TileGen::find_backdrop(const m3::Model3 &board, bool full)
{
	Backdrop &b = backdrop;
	const bool had = b.ok;
	b.ok = false;
	b.known = false;
	const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
	const uint32_t *regs = regs_of(board);
	if (!(regs[0x60 / 4 + 3] & 0x80000000u)) return;
	if (!board.profile->backdrops.descriptors) return;   // (not known for this program: the sky kept to the middle 4:3)
	const int stage = int16_t(board.peek16(board.cpu.s.r[13] - board.profile->backdrops.stage_below_r13));
	if (stage < 0 || stage > 255) return;
	const uint32_t desc = board.peek32(board.profile->backdrops.descriptors + 4 * uint32_t(stage));
	if (desc < 0xFF000000u) return;
	const int w = board.peek16(desc + 4), h = board.peek16(desc + 6);
	if (w < 64 || w > 2048 || h < 1 || h > 256) return;
	if (desc != b.desc) { b.x0 = 0; b.y0 = 0; }   // (the last place kept while the same map: a frame not found starts from it)
	if (desc != b.desc) std::fill(std::begin(b.seen), std::end(b.seen), -1);
	if (desc != b.desc || b.map.size() != size_t(w) * size_t(h)) {   // (the map read once: the searches below look at it often)
		b.map.resize(size_t(w) * size_t(h));
		for (size_t i = 0; i < b.map.size(); i++) b.map[i] = board.peek16(desc + 8 + 2 * uint32_t(i));
	}
	b.desc = desc; b.w = w; b.h = h;
	b.known = true;
	for (int i = 0; i < 128; i++) b.bank[i] = int16_t(board.peek16(board.profile->backdrops.banks + 2 * uint32_t(i)));
	// where the layer's map is in it: some of the rows on screen compared
	const uint32_t *names = vram + (0xF8000 + 3 * 0x2000) / 4;
	const int sy = int((regs[0x60 / 4 + 3] >> 16) & 0x1FF), s = (scroll_x(vram, regs, 3, 0) & 511) >> 3;
	// the rows compared: the 4 on screen with the most different tiles (a row of the sky's or the sea's one tile
	// matches anywhere)
	int rows[4] = { -1, -1, -1, -1 }, kinds[4] = {};
	for (int r = (sy >> 3) + 1; r < (sy >> 3) + 46; r++) {
		uint16_t t[64];
		int m = 0;
		for (int c = s; c < s + 56; c += 3) t[m++] = uint16_t(name_at(names, r & 63, c) & 0x3FFF);
		std::sort(t, t + m);
		const int k = int(std::unique(t, t + m) - t);
		for (int i = 0; i < 4; i++)
			if (k > kinds[i]) {
				for (int j = 3; j > i; j--) { rows[j] = rows[j - 1]; kinds[j] = kinds[j - 1]; }
				rows[i] = r & 63; kinds[i] = k;
				break;
			}
	}
	const bool few = rows[3] < 0 || kinds[0] + kinds[1] + kinds[2] + kinds[3] < 24;
	// (a map entry and the layer's: the same tile of its bank, the bank where the layer has its others)
	auto raw = [&](int x, int y) {
		const int at = y < 0 ? 0 : y >= h ? (h - 1) * w : y * w + ((x % w) + w) % w;
		return int(b.map[size_t(at)]);
	};
	auto agrees = [&](int x0, int y0) {
		// (7 in 10 of the tiles compared as the table places their banks: the rest may be in a bank the game has just
		// moved, which the layer has where the table doesn't say yet; those found after. A bank "moved" anywhere
		// can't count: the patterns run on from row to row, so a place rows away looks like the right one moved)
		// (given up as soon as more than 3 in 10 of them miss)
		const int n = 4 * ((56 + 2) / 3), most_missed = n * 3 / 10;
		int missed = 0;
		for (int r : rows)
			for (int c = s; c < s + 56; c += 3) {
				const int e = raw(x0 + c, y0 + r), bank = (e >> 7) & 0x7F;
				const int t = int(name_at(names, r, c) & 0x3FFF);
				const bool exact = t == (((e & 0x7F) + b.bank[bank]) & 0x3FFF) || (b.seen[bank] >= 0 && t == (((e & 0x7F) + b.seen[bank]) & 0x3FFF));
				if (!exact && ++missed > most_missed) return false;
			}
		return true;
	};
	// each bank's place, from all of the layer on screen
	auto see = [&]() {
		std::fill(std::begin(b.seen), std::end(b.seen), -1);
		int votes[128] = {};
		for (int r = sy >> 3; r < (sy >> 3) + 48; r++)
			for (int c = s; c < s + 62; c++) {
				const int e = raw(b.x0 + c, b.y0 + (r & 63)), bank = (e >> 7) & 0x7F;
				const int at = (int(name_at(names, r & 63, c) & 0x3FFF) - (e & 0x7F)) & 0x3FFF;
				if (b.seen[bank] == at) votes[bank]++;
				else if (votes[bank] == 0) { b.seen[bank] = at; votes[bank] = 1; }
				else votes[bank]--;
			}
	};
	// The places tried first (the game's own, this frame's and the last's; the last place found; those near it) are
	// told apart by all of the screen, a tile in 5 of every row: the rows compared above are of the most different
	// tiles, but a sky's rows of few tiles (a moon's glow, a dusk's bands) agree with them a column or two off too, 14
	// of 76 missed where the right place misses none, and with banks just moved a place a row off can match them all
	// (the patterns running on from row to row: stage 49's sky, the camera looking up). Taken: the one that misses
	// fewest of the screen's, not the first good enough. The first looked at, the last place, had stuck there for
	// frames while the camera turned (Jeffry's beach in Team Battle, whose place the game keeps elsewhere: the sides
	// drawn from rows a few tiles off, a bright bar); the game's this frame's is often a frame ahead of the screen's.
	// One that misses none ends it (the usual frame: the first looked at); the rest each given up past the best so far.
	const int n_screen = 48 * ((62 + 4) / 5);
	auto screen_misses = [&](int x0, int y0, int limit) {
		int missed = 0;
		for (int r = sy >> 3; r < (sy >> 3) + 48; r++)
			for (int c = s; c < s + 62; c += 5) {
				const int e = raw(x0 + c, y0 + (r & 63)), bank = (e >> 7) & 0x7F;
				const int t = int(name_at(names, r & 63, c) & 0x3FFF);
				if (!(t == (((e & 0x7F) + b.bank[bank]) & 0x3FFF) || (b.seen[bank] >= 0 && t == (((e & 0x7F) + b.seen[bank]) & 0x3FFF))) && ++missed > limit) return missed;
			}
		return missed;
	};
	int pbest = INT32_MAX, pbx = 0, pby = INT32_MIN;
	bool must_agree = false;
	// (a place's misses of the screen, if it agrees where it must; ties: the one looked at first)
	auto consider = [&](int x0, int y0) {
		if (pbest == 0 || (must_agree && !agrees(x0, y0))) return;
		const int m = screen_misses(x0, y0, pbest == INT32_MAX ? INT32_MAX : pbest - 1);
		if (m < pbest) { pbest = m; pbx = ((x0 % w) + w) % w; pby = y0; }
	};
	// the game's own place, kept at 0x1617B4 + 4 and + 8 in pixels (Team Battle's 0x16195C; the routine at 0x5D000
	// writes the map from the place it kept the frame before, 0x5D12C: this frame's and the last's tried)
	const int gx = int(int32_t(board.peek32(board.profile->backdrops.place + 4))) >> 3, gy = int(int32_t(board.peek32(board.profile->backdrops.place + 8))) >> 3;
	if (board.frame != b.game_frame) { b.prev_x = b.game_x; b.prev_y = b.game_y; b.game_x = gx; b.game_y = gy; b.game_frame = board.frame; }
	const bool same = had && desc == b.desc;
	auto first_places = [&]() {
		// (a place off the map further than the screen's rows reach is none: another program's)
		if (gy >= -112 && gy <= h + 48) consider(gx, gy);
		if (b.prev_y >= -112 && b.prev_y <= h + 48) consider(b.prev_x, b.prev_y);
		if (same || must_agree) consider(b.x0, b.y0);
	};
	auto near_places = [&]() {
		for (int d = 1; d <= 6 && pbest > 0; d++)
			for (int dy = -2; dy <= 2 && pbest > 0; dy++) {
				const int ax = d - std::abs(dy);
				if (ax < 0 || ax > 4) continue;
				consider(b.x0 + ax, b.y0 + dy);
				if (ax) consider(b.x0 - ax, b.y0 + dy);
			}
	};
	if (few) {
		// Too little on screen to tell: as before, if the same backdrop; but a screen of the map's top or bottom
		// filler alone (the camera up at the sky, down at the sea) puts the map's place above or below it
		b.ok = same;
		// (the fillers as the table places their banks, or the screen had them)
		const uint32_t top = backdrop_entry(board, 0, -1) & 0x3FFF, bottom = backdrop_entry(board, 0, h) & 0x3FFF;
		const uint32_t top_t = backdrop_entry(board, 0, -1, true) & 0x3FFF, bottom_t = backdrop_entry(board, 0, h, true) & 0x3FFF;
		int n_top = 0, n_bottom = 0, n = 0;
		for (int r = (sy >> 3) + 1; r < (sy >> 3) + 46; r += 2)
			for (int c = s; c < s + 56; c += 5) {
				const uint32_t t = name_at(names, r & 63, c) & 0x3FFF;
				n++; n_top += t == top || t == top_t; n_bottom += t == bottom || t == bottom_t;
			}
		// (but first the places above, where one has 9 in 10 of the screen's tiles, the filler's place only where
		// it misses fewer: the camera down at the sea with the horizon's last rows at the screen's top, stage 47's,
		// had the filler's place, those rows a few tiles off, for a hundred frames)
		pbest = n_screen / 10 + 1;   // (more missed than 1 in 10: none)
		first_places();
		const int filler_y0 = n_bottom * 10 >= n * 9 ? h - (sy >> 3) : n_top * 10 >= n * 9 ? -(sy >> 3) - 48 : INT32_MIN;
		if (pbest > 0 && filler_y0 != INT32_MIN) {
			const int m = screen_misses(b.x0, filler_y0, INT32_MAX);
			if (m < pbest || pby == INT32_MIN) { pbest = m; pbx = b.x0; pby = filler_y0; }
		}
		if (same) near_places();
		if (pby != INT32_MIN) { b.x0 = pbx; b.y0 = pby; b.ok = true; see(); }
		else if (kinds[0] <= 2) b.ok = false;   // (the map cleared, one tile all over: no backdrop on show)
		return;
	}
	// First those places, of those that agree (as the table places the banks or, with banks the game has just moved,
	// each bank in one place), the one that misses fewest of the screen
	{
		must_agree = true;
		first_places();
		if (pbest > 0) near_places();
		if (pby != INT32_MIN) { b.x0 = pbx; b.y0 = pby; b.ok = true; see(); return; }
	}
	// (all of it: the one nearest the last place of those that agree)
	if (!full) return;
	// (looked through in vain with the same rows on screen, the same map and banks: not again, for it finds nothing
	// again; a screen of its own with the stage's map loaded, as the select's, had the whole map looked through every
	// 16 frames, a frame's hitch each time on a slow machine. What it compares is all in the sum: the rows' tiles it
	// looks at, the map, and the banks' places, the table's and the screen's)
	uint64_t screen_sum = desc ^ banks_sum(b);
	for (int r : rows)
		for (int c = s; c < s + 56; c += 3) screen_sum = screen_sum * 1000003u + (name_at(names, r, c) & 0x3FFF) + uint64_t(r) * 7;
	if (screen_sum == full_failed_sum && !getenv("VF3_NO_TILE_SKIP")) return;
	int best = INT32_MAX, bx = 0, by = 0;
	for (int y0 = -64; y0 <= h; y0++)
		for (int x0 = 0; x0 < w; x0++)
			if (agrees(x0, y0)) {
				const int dx = std::min((x0 - b.x0 + w) % w, (b.x0 - x0 + w) % w), dist = dx + std::abs(y0 - b.y0);
				if (dist < best) { best = dist; bx = x0; by = y0; }
			}
	if (best < INT32_MAX) { b.x0 = bx; b.y0 = by; b.ok = true; see(); return; }
	full_failed_sum = screen_sum;
	// (none exactly: the game's place with banks it has just moved, each bank in one place)
	{
		const int cand[2][2] = { { int(int32_t(board.peek32(board.profile->backdrops.place + 4))) >> 3, int(int32_t(board.peek32(board.profile->backdrops.place + 8))) >> 3 }, { b.prev_x, b.prev_y } };
		for (const auto &cxy : cand) {
			const int x0 = ((cxy[0] % w) + w) % w, y0 = cxy[1];
			int place[128], n = 0, bad = 0;
			std::fill(std::begin(place), std::end(place), -1);
			for (int r : rows)
				for (int c = s; c < s + 56; c += 3) {
					const int e = raw(x0 + c, y0 + r), bank = (e >> 7) & 0x7F;
					const int at = (int(name_at(names, r, c) & 0x3FFF) - (e & 0x7F)) & 0x3FFF;
					n++;
					if (place[bank] < 0) place[bank] = at;
					else bad += place[bank] != at;
				}
			if (bad * 10 <= n) { b.x0 = x0; b.y0 = y0; b.ok = true; see(); return; }
		}
	}
	// Last: the game has moved most banks on show (between rounds): near the last place only (far from it, a place rows
	// away looks the same, the patterns running on from row to row), each bank in one place for 9 in 10 compared
	if (b.desc == desc && (b.x0 || b.y0))
		for (int dy = -2; dy <= 2; dy++)
			for (int dx = -4; dx <= 4; dx++) {
				int place[128], n = 0, bad = 0;
				std::fill(std::begin(place), std::end(place), -1);
				for (int r : rows)
					for (int c = s; c < s + 56; c += 3) {
						const int e = raw(b.x0 + dx + c, b.y0 + dy + r), bank = (e >> 7) & 0x7F;
						const int at = (int(name_at(names, r, c) & 0x3FFF) - (e & 0x7F)) & 0x3FFF;
						n++;
						if (place[bank] < 0) place[bank] = at;
						else bad += place[bank] != at;
					}
				if (bad * 10 <= n) { b.x0 = ((b.x0 + dx) % w + w) % w; b.y0 += dy; b.ok = true; see(); return; }
			}
	if (getenv("VF3_BD_WHY")) {
		fprintf(stderr, "bd miss f%llu sy %d s %d x0 %d y0 %d:", (unsigned long long)board.frame, sy, s, b.x0, b.y0);
		for (int r : rows) {
			int bad = 0;
			for (int c = s; c < s + 56; c += 3) bad += int(backdrop_entry(board, b.x0 + c, b.y0 + r) & 0x3FFF) != int(name_at(names, r, c) & 0x3FFF);
			fprintf(stderr, " row %d bad %d (map %04x tile %04x)", r, bad, backdrop_entry(board, b.x0 + s + 3, b.y0 + r) & 0x3FFF, name_at(names, r, s + 3) & 0x3FFF);
		}
		fprintf(stderr, "\n");
	}
}

void TileGen::learn(const m3::Model3 &board)
{
	const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
	const uint32_t *regs = regs_of(board);
	const int n = W / 8 + 1;   // the map's columns on screen (from the one at its left edge)
	for (int layer = 0; layer < 4; layer++) {
		Pano &P = pano[layer];
		if (!(regs[0x60 / 4 + layer] & 0x80000000u) || ((regs[0x20 / 4] >> (8 + layer)) & 1)) { P = Pano(); continue; }
		const uint32_t *names = vram + (0xF8000 + layer * 0x2000) / 4;
		const int s = (scroll_x(vram, regs, layer, 0) & 511) >> 3;
		int runs = 0;
		static int stride[4097];
		std::fill(std::begin(stride), std::end(stride), 0);
		int above[W / 8 + 1] = {};
		bool above_run = false;
		for (int row = 0; row < 64; row++) {
			int p[W / 8 + 1];
			for (int i = 0; i < n; i++) p[i] = int(name_at(names, row, s + i) & 0x3FFF);
			auto pat = [&](int i) { return p[i]; };
			if (!one_run(pat, n, [&](int q) { return next_along(layer, q, 1); })) { above_run = false; continue; }
			runs++;
			// the step from the row above's run to this one's: a row's usual length
			if (above_run)
				for (int i = 0; i < n; i++)
					if (p[i] - above[i] >= 32 && p[i] - above[i] <= 4096) stride[p[i] - above[i]]++;
			std::copy(p, p + n, above);
			above_run = true;
			for (int i = 1; i < n; i++) {
				const int a = p[i - 1], b = p[i];
				if (b == a + 1) {
					// a run on past where a row was thought to end: it doesn't
					auto it = P.rows.upper_bound(a);
					if (it != P.rows.begin() && std::prev(it)->first + std::prev(it)->second - 1 == a) P.rows.erase(std::prev(it));
				} else if (b < a && a - b >= 31 && a - b < 4096 && (i < 2 || p[i - 2] + 1 == a) && (i + 1 >= n || p[i + 1] == b + 1)) {
					// a wrap: the row is b to a
					add_row(layer, b, a - b + 1);
				} else if (b != a && i >= 2 && p[i - 2] + 1 == a && i + 1 < n && p[i + 1] == b + 1) {
					// a run on across a gap in the patterns
					skip_fwd[layer][a] = b;
					skip_bwd[layer][b] = a;
				}
			}
		}
		if (!runs) { P = Pano(); continue; }   // (no panorama: a close-up, a title; it starts again after)
		const int len = int(std::max_element(std::begin(stride), std::end(stride)) - std::begin(stride));
		if (stride[len] >= 32) P.len = len;
	}
}

static const uint32_t *vram_words(const m3::Model3 &board) { return reinterpret_cast<const uint32_t *>(board.tile_ram.data()); }

void TileGen::line(const m3::Model3 &board, unsigned y, const uint32_t *line_regs)
{
	regs_now = line_regs;   // (the registers as they were at this line: a worker drawing it a moment later)
	struct Done { const uint32_t *&r; ~Done() { r = nullptr; } } done{ regs_now };
	if (y >= unsigned(H))
		return;
	if (y == 0) {
		// the sides' memory: forgotten where the screen stopped agreeing with it last frame
		for (Seen &m : seen) {
			if (m.compared > 4000 && m.differed * 3 > m.compared) m.forget();
			m.compared = m.differed = 0;
		}
		find_backdrop(board);
		place_seen(board);
		{
			int n2 = 0;
			for (int yy = 0; yy < H; yy++) {
				const uint32_t m = vram_words(board)[0xF7000 / 4 + yy] & 0xFFFF;
				n2 += (m >> 15) & 1 || (m & 1);
			}
			cover2 = n2 * 2 > H;
		}
		learn(board);
		ext_rows.clear(); ext_how.clear(); ext_edge.clear(); wrap_votes.clear(); mem_mirror.clear();
		std::fill(clear_known.begin(), clear_known.end(), 0);
		std::fill(solid_memo.begin(), solid_memo.end(), 0);
	}
	const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
	const uint32_t *pal = vram + 0x40000;
	const uint32_t *regs = regs_of(board);
	// B/B' first, then A/A' over it
	for (int pair = 1; pair >= 0; pair--) {
		const int prim = pair * 2;
		bool on[2] = { (regs[0x60 / 4 + prim] & 0x80000000u) != 0, (regs[0x60 / 4 + prim + 1] & 0x80000000u) != 0 };
		if (!on[0] && !on[1])
			continue;
		const Offset off = offset_of(regs[(0x40 + 4 * pair) / 4]);
		const uint32_t mask = (vram[0xF7000 / 4 + y] >> (pair ? 0 : 16)) & 0xFFFF;
		struct L { int sx, sy; bool four; uint32_t *dst; const uint32_t *names; } ls[2];
		for (int k = 0; k < 2; k++) {
			int layer = prim + k;
			uint32_t r = regs[0x60 / 4 + layer];
			int sx = scroll_x(vram, regs, layer, y);
			ls[k].sx = sx;
			ls[k].sy = int((r >> 16) & 0x1FF);
			scroll.sx[layer][y] = int16_t(sx);
			scroll.sy[layer][y] = int16_t(ls[k].sy);
			ls[k].four = (regs[0x20 / 4] >> (12 + layer)) & 1;
			ls[k].dst = ((regs[0x20 / 4] >> (8 + layer)) & 1 ? above : below).data() + size_t(y) * W;
			ls[k].names = vram + (0xF8000 + layer * 0x2000) / 4;
		}
		// the colours: each 5-bit channel with the pair's offset, looked up
		// (made again only when the offsets change: they hardly do, and it was made for every line)
		static uint8_t lut[3][32];
		static int lut_offs[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
		const int offs[3] = { off.r, off.g, off.b };
		if (offs[0] != lut_offs[0] || offs[1] != lut_offs[1] || offs[2] != lut_offs[2]) {
			for (int c = 0; c < 3; c++)
				for (int i = 0; i < 32; i++) lut[c][i] = uint8_t(std::clamp(i * 255 / 31 + offs[c], 0, 255));
			std::copy(offs, offs + 3, lut_offs);
		}
		auto colour_of = [&](uint32_t v) {
			return 0xFF000000u | uint32_t(lut[2][(v >> 10) & 31]) << 16 | uint32_t(lut[1][(v >> 5) & 31]) << 8 | lut[0][v & 31];
		};
		// a texel's colour (bit 15: transparent)
		auto texel = [&](uint32_t e, bool four, int fy, int fx) {
			if (four) {
				const uint32_t pat = (((e & 0x3FFF) << 1) | (e >> 15)) * 8;
				return pal[((e & 0x7FF0) | ((vram[(pat + fy) & 0x3FFFF] >> ((7 - fx) * 4)) & 0xF)) & 0x7FFF];
			}
			const uint32_t pat = (e & 0x3FFF) * 16;
			return pal[((e & 0x7F00) | ((vram[(pat + fy * 2 + (fx >> 2)) & 0x3FFFF] >> ((3 - (fx & 3)) * 8)) & 0xFF)) & 0x7FFF];
		};
		auto put_texel = [&](uint32_t *dst, int x, uint32_t e, bool four, int fy, int fx) {
			const uint32_t v = texel(e, four, fy, fx);
			if (v & 0x8000) return 0;
			dst[x] = colour_of(v);
			return int((v & 0x7FFF) != 0);
		};
		// how far apart two tiles are where they meet (a's column ca against b's cb, the 8 rows' mean of the
		// channels' differences, in 5-bit steps); -1 where either is all transparent or black there
		auto seam = [&](uint32_t a, bool af, uint32_t b, bool bf, bool four, int ca, int cb) {
			int sum = 0, n = 0;
			for (int fy = 0; fy < 8; fy++) {
				const uint32_t u = texel(a, four, fy, af ? 7 - ca : ca), v = texel(b, four, fy, bf ? 7 - cb : cb);
				if ((u & 0x8000) || (v & 0x8000) || !(u & 0x7FFF) || !(v & 0x7FFF)) continue;
				for (int sh = 0; sh < 15; sh += 5) sum += std::abs(int((u >> sh) & 31) - int((v >> sh) & 31));
				n++;
			}
			return n < 4 ? -1.0f : float(sum) / float(n);
		};
		// n texels of entry e's row fy from fx0 on, to dst[x...]; how many weren't black
		auto put = [&](uint32_t *dst, int x, uint32_t e, bool four, int fy, int fx0, int n, bool flip = false) {
			int drawn = 0;
			if (flip) {   // (mirrored: texel 7 first)
				for (int i = 0; i < n; i++) drawn += put_texel(dst, x + i, e, four, fy, 7 - (fx0 + i));
				return drawn;
			}
			if (four) {
				const uint32_t pat = (((e & 0x3FFF) << 1) | (e >> 15)) * 8;
				const uint32_t w = vram[(pat + fy) & 0x3FFFF], base = e & 0x7FF0;
				for (int i = 0; i < n; i++) {
					const uint32_t v = pal[(base | ((w >> ((7 - (fx0 + i)) * 4)) & 0xF)) & 0x7FFF];
					if (!(v & 0x8000)) { dst[x + i] = colour_of(v); drawn += (v & 0x7FFF) != 0; }
				}
			} else {
				const uint32_t pat = (e & 0x3FFF) * 16, base = e & 0x7F00;
				const uint32_t w2[2] = { vram[(pat + fy * 2) & 0x3FFFF], vram[(pat + fy * 2 + 1) & 0x3FFFF] };
				for (int i = 0; i < n; i++) {
					const int fx = fx0 + i;
					const uint32_t v = pal[(base | ((w2[fx >> 2] >> ((3 - (fx & 3)) * 8)) & 0xFF)) & 0x7FFF];
					if (!(v & 0x8000)) { dst[x + i] = colour_of(v); drawn += (v & 0x7FFF) != 0; }
				}
			}
			return drawn;   // (those not black)
		};
		// a 32-pixel column at a time (the mask's), a tile's row of texels at a time
		for (int c0 = 0; c0 < W; c0 += 32) {
			const int k = (mask >> (15 - c0 / 32)) & 1 ? 0 : 1;
			if (!on[k])
				continue;
			const L &l = ls[k];
			if (l.dst == below.data() + size_t(y) * W) scroll.below_columns[prim + k]++;
			const int py = (int(y) + l.sy) & 511, fy = py & 7;
			const uint32_t *row_names = l.names + (py / 8) * 32;
			const int c1 = std::min(c0 + 32, W);
			for (int x = c0; x < c1;) {
				const int px = (x + l.sx) & 511;
				const uint32_t pairw = row_names[px / 16];
				const uint32_t e = (px & 8) ? (pairw & 0xFFFF) : (pairw >> 16);
				// the texels of this tile's row from x on (to the tile's end or the column's)
				const int fx0 = px & 7, n = std::min(8 - fx0, c1 - x);
				put(l.dst, x, e, l.four, fy, fx0, n);
				x += n;
			}
		}
		// widescreen's sides of the layer below the 3D: the layer the mask shows at the screen's edge, its row's run of
		// patterns carried on (from the run nearest the edge, so a tile the game put over its panorama is skipped);
		// a row that isn't a run (a sea's, a fill's) its edge tile repeated
		bool remembered[2] = {};
		for (int side = 0; side < 2; side++) {
			// (with the game's backdrop known, the sides are it alone: the other layers there are the HUD's, its
			// scores and texts, which have nothing past the screen)
			// (but where the screen has layer 2 at its edges for most of the frame, as over all of it at a cut, the
			// sides follow the screen's mask)
			const int k = backdrop.ok && !cover2 ? 1 : (mask >> (side ? 0 : 15)) & 1 ? 0 : 1;
			if (backdrop.ok && prim != 2) continue;
			if (!on[k])
				continue;
			const L &l = ls[k];
			if (l.dst != below.data() + size_t(y) * W)
				continue;
			// The line as the screen shows it, remembered (once a line for each layer at a side)
			Seen &mem = seen[prim + k];
			const int mem_row = (int(y) + l.sy) & 511;
			if (!remembered[k] && !((backdrop.ok || backdrop.known) && prim + k == 3)) {   // (the backdrop: the game's map has it all)
				remembered[k] = true;
				if (mem.at_x.empty()) {
					mem.at_x.assign(size_t(R) * 512, INT32_MIN);
					mem.at_y.assign(size_t(R) * 512, 0);
					mem.col.assign(size_t(R) * 512, 0);
				}
				const int sx = l.sx & 511;
				if (mem.last_sx < 0) { mem.x = sx; mem.y = l.sy; mem.last_sx = sx; mem.last_sy = l.sy; }
				const int32_t ax0 = mem.x + (((sx - mem.last_sx + 256) & 511) - 256), ay = mem.y + int(y);   // (a line scrolled its own way)
				mem.empty = false;
				const uint32_t *names_row = l.names + (mem_row / 8) * 32;
				const size_t line_i = size_t(ay & 511) * R;
				// (but its outer tiles: the game may not have written them yet)
				for (int x = 16; x < W - 16 && !mem.unsure; x++) {
					const int px = (x + sx) & 511;
					const uint32_t pw = names_row[px / 16];
					const uint16_t v = uint16_t(texel((px & 8) ? (pw & 0xFFFF) : (pw >> 16), l.four, mem_row & 7, px & 7) & 0xFFFF);
					const int32_t a = ax0 + x;
					const size_t i = line_i + size_t(a & (R - 1));
					if (mem.at_x[i] == a && mem.at_y[i] == ay) {
						mem.compared++;
						mem.differed += !same_colour(mem.col[i], v);
					}
					mem.at_x[i] = a;
					mem.at_y[i] = ay;
					mem.col[i] = v;
				}
			}
			const Pano &P = pano[prim + k];
			const int fine = l.sx & 7, s = (l.sx & 511) >> 3, last = (W - 1 + fine) >> 3;
			const int py = (int(y) + l.sy) & 511, fy = py & 7, row = py / 8;
			auto ent = [&](int i) { return name_at(l.names, row, s + i); };
			auto pat = [&](int i) { return int(ent(i) & 0x3FFF); };
			const int d = side ? 1 : -1, edge = side ? last : 0;
			auto next = [&](int q) { return next_along(prim + k, q, d); };
			// The run along the row to the edge and where it starts (base: -1 none), for a row's tiles past the edge made
			// from the screen (worked out only then: not for the map's rows, nor the row's other lines)
			auto find_base = [&]() {
				// (a row mostly one run; or not, as a sky with a sun of one tile repeated and streamed in pieces, but one at
				// the edge for 8 tiles)
				bool run = one_run(pat, last + 1, [&](int q) { return next_along(prim + k, q, 1); });
				if (!run) {
					int n = 1;
					for (int c = edge; n < 8 && c - d >= 0 && c - d <= last && next(pat(c - d)) == pat(c); c -= d) n++;
					run = n >= 8;
				}
				// the run: three in a row and on to the edge (no tile the game put over it there)
				int base = -1;
				for (int j = 0; j < 6 && base < 0 && run; j++) {
					const int c = edge - d * j, c1 = c - d, c2 = c - 2 * d;
					if (std::min(c2, c) < 0 || std::max(c2, c) > last) break;
					if (next(pat(c2)) != pat(c1) || next(pat(c1)) != pat(c)) continue;
					int q = pat(c), i = c;
					while (i != edge && q >= 0) { i += d; q = next(q); if (q != pat(i)) q = -1; }
					if (q >= 0) base = c;
				}
				return base;
			};
			uint32_t *dst = below_wide.data() + size_t(y) * WW + EXT;
			// else the commonest tile of the row's 16 nearest the edge (a sea's, the sky's between the tops of the clouds;
			// not one from across the screen, as a sun's), or its edge's
			uint32_t fill = ent(edge);
			{
				uint32_t es[16];
				int m = 0;
				for (int c = edge; m < 16 && c >= 0 && c <= last; c -= d) es[m++] = ent(c);
				std::sort(es, es + m);
				int best = 3;
				for (int i = 0, j; i < m; i = j) {
					for (j = i; j < m && es[j] == es[i]; j++) {}
					if (j - i > best) { best = j - i; fill = es[i]; }
				}
			}
			const uint32_t edge_e = ent(edge);
			// the tile at the edge (its part past the screen), then on out
			{
				const int x0 = edge * 8 - fine;
				if (side) { const int from = W - x0; put(dst, W, edge_e, l.four, fy, from, 8 - from); }
				else if (fine) put(dst, -fine, edge_e, l.four, fy, 0, fine);
			}
			// The row's tiles out from the edge, made once a frame (and checked whole, not a line at a time). The run's
			// patterns may not all be the panorama's: the game keeps only the part on show loaded, the rest of the
			// pattern RAM other pictures (the HUD's letters, a title's). Each new tile is checked against the one before
			// where they meet: a photograph carries on, another picture doesn't (over 2.5 times the run's own seams):
			// there the rows' usual length back (a wrap not yet seen) is tried, else the row is mirrored from there
			// (its last good tiles, flipped), as is a run into tiles with nothing (or only black) in them.
			const uint64_t key = uint64_t(prim + k) << 40 | uint64_t(row) << 32 | uint64_t(side) << 31 | uint64_t(uint32_t(l.sx) & 0x7FFFFFFF);
			auto found = ext_rows.find(key);
			// The stage's backdrop is solid picture, but the patterns the game has let go of hold other pictures (the
			// HUD's digits, a round's letters), and the map points at them past what the game keeps loaded: at 21:9
			// the sides reach about 26 tiles out, where 16:9's reach 12. A tile with a see-through texel isn't the
			// backdrop's: the good one before it, mirrored, in its place (the seam stays whole).
			auto solid_only = [&](std::vector<std::pair<uint32_t, bool>> &row_tiles) {
				if (prim + k != 3) return;
				auto solid = [&](uint32_t e) {
					// the palette's see-through colours (bit 15), looked up once a frame; then the pattern's texels
					// tested against them, its words read as they are
					const uint32_t block = l.four ? (e & 0x7FF0) : (e & 0x7F00), colours = l.four ? 16 : 256;
					const size_t slot = size_t(block >> 4) + (l.four ? 0 : 2048);   // (a 4-bit and an 8-bit palette may start at one place)
					uint64_t *cc = &clear_cols[slot * 4];
					if (!clear_known[slot]) {
						clear_known[slot] = 1;
						cc[0] = cc[1] = cc[2] = cc[3] = 0;
						for (uint32_t i = 0; i < colours; i++)
							if (pal[(block | i) & 0x7FFF] & 0x8000) cc[i >> 6] |= 1ull << (i & 63);
					}
					if (!(cc[0] | cc[1] | cc[2] | cc[3])) return true;
					uint8_t &m = solid_memo[size_t(e & 0xFFFF) << 1 | size_t(l.four)];   // (and the pattern's answer: rows share them)
					if (m) return m == 1;
					m = 1;
					if (l.four) {
						const uint32_t pat = (((e & 0x3FFF) << 1) | (e >> 15)) * 8;
						const bool zero_only = cc[0] == 1;   // (the usual: colour 0 alone see-through: a word's nibbles tested at once)
						for (int fy = 0; fy < 8 && m == 1; fy++) {
							const uint32_t w = vram[(pat + fy) & 0x3FFFF];
							if (zero_only) { if ((w - 0x11111111u) & ~w & 0x88888888u) m = 2; }
							else for (int sh = 0; sh < 32; sh += 4) if (cc[0] >> ((w >> sh) & 15) & 1) { m = 2; break; }
						}
					} else {
						const uint32_t pat = (e & 0x3FFF) * 16;
						for (int i = 0; i < 16 && m == 1; i++) {
							const uint32_t w = vram[(pat + i) & 0x3FFFF];
							for (int sh = 0; sh < 32; sh += 8) { const uint32_t t = (w >> sh) & 0xFF; if (cc[t >> 6] >> (t & 63) & 1) { m = 2; break; } }
						}
					}
					return m == 1;
				};
				if (!solid(edge_e)) return;
				std::pair<uint32_t, bool> good_one = { edge_e, false };
				for (auto &t : row_tiles) {
					if (solid(t.first)) good_one = t;
					else t = good_one = { good_one.first, !good_one.second };
				}
			};
			// (the backdrop: as the game's own map has it)
			static const bool no_backdrop = getenv("VF3_NO_BACKDROP") != nullptr;   // (tests: the sides without the game's map)
			// (with the map's place not found for the frame, each row still looked for in it)
			bool from_map = prim + k == 3 && (backdrop.ok || backdrop.known) && !no_backdrop;
			int row_x0 = backdrop.x0, row_y = backdrop.y0 + row;   // (where this row is in the map)
			bool uniform_row = false;
			if (found == ext_rows.end() && from_map) {
				// The row as the map has it is the row on screen (the game writes its map during the frame: where it
				// moved since the frame's start, the map's place found again)
				auto row_bad = [&]() {
					int bad = 0;
					for (int c = 0; c <= last; c++) bad += (backdrop_entry(board, backdrop.x0 + s + c, backdrop.y0 + row) & 0x3FFF) != (ent(c) & 0x3FFF);
					static FILE *hist = getenv("VF3_BD_HIST") ? fopen(getenv("VF3_BD_HIST"), "w") : nullptr;
					if (hist) fprintf(hist, "%d %d %d %d %llu %u\n", bad, backdrop.y0 + row, backdrop.h, row, (unsigned long long)board.frame, y);
					return bad * 4 > last + 1;   // (some tiles of a row on screen may differ from the map, the game's own put over it: the place is still its)
				};
				sky_rows++;
				if (!backdrop.ok || row_bad()) {
					// (looked for again once a frame at most when it isn't found: rows of a map cleared, or between
					// scenes, are looked for alone below)
					const bool full = refind_frame != board.frame;
					refind_frame = board.frame;
					refind_backdrop(board, full);
					sky_refinds++;
					from_map = backdrop.ok && !row_bad();
					row_x0 = backdrop.x0; row_y = backdrop.y0 + row;
					if (!from_map && backdrop.w > 0) {
						// The screen part the old scene, part the new (the game rewriting its map at a cut): this row
						// found in the map alone (a row of enough different tiles; one of a filler alone, there)
						const Backdrop &b = backdrop;
						uint16_t t[64];
						int m = 0;
						for (int c = 0; c <= last; c++) t[m++] = uint16_t(ent(c) & 0x3FFF);
						std::sort(t, t + m);
						uint16_t sorted[64];
						std::copy(t, t + m, sorted);
						const int kinds = int(std::unique(t, t + m) - t);
						if (kinds >= 6) {
							// (anchored by 4 in 10 of its tiles where the table, or the screen before, has their banks; the
							// rest each in its bank's one other place, a bank the game has just moved: that place kept)
							// (looked for once a row a frame: the other side's the same row; touched banks only reset)
							uint64_t sum = 0;
							for (int c = 0; c <= last; c++) sum = sum * 1000003u + (ent(c) & 0x3FFF);
							RowFound &rf = row_found;
							// (a row not found stays not found for 16 frames while the same: a screen of its own, as the
							// select's, looked through the whole map for each of its rows every frame)
							// (and, as for the whole screen's, not again while its tiles, the map and the banks are the same:
							// it finds nothing again)
							const uint64_t row_key = sum ^ b.desc ^ banks_sum(b) * 31;
							const bool failed_lately = row_failed.count(row_key) != 0 && !getenv("VF3_NO_TILE_SKIP");
							if (failed_lately) { rf.frame = board.frame; rf.row = row; rf.sum = sum; rf.found = false; rf.moved.clear(); }
							if (rf.frame != board.frame || rf.row != row || rf.sum != sum) {
								rf.frame = board.frame; rf.row = row; rf.sum = sum; rf.found = false; rf.moved.clear();
								const int n = last + 1;
								int moved[128], moved_n[128], touched[64];
								std::fill(std::begin(moved), std::end(moved), -1);
								std::fill(std::begin(moved_n), std::end(moved_n), 0);
								// (near where the row was found last frame first: the camera moves it a tile or two at most, and a
								// screen whose rows each move their own way, as the desert's when the camera tilts, had every row
								// looked for through all the map every frame, 30 ms a frame on a fast machine. All of it only
								// when that fails, and for a row it failed for lately, once in 8 frames)
								RowHint &hint = row_hint[row & 63];
								if (hint.desc != b.desc) hint = RowHint{ b.desc };
								auto try_at = [&](int mx, int my) {
										int exact = 0, bad = 0, nt = 0;
										const uint16_t *mrow = &b.map[size_t(my) * size_t(b.w)];
										for (int c = 0; c <= last && bad * 4 <= n && (exact + (n - c)) * 2 >= n; c++) {
											const uint16_t e = mrow[(mx + c) % b.w];
											const int bank = (e >> 7) & 0x7F, t = int(ent(c) & 0x3FFF);
											if (t == (((e & 0x7F) + b.bank[bank]) & 0x3FFF) || (b.seen[bank] >= 0 && t == (((e & 0x7F) + b.seen[bank]) & 0x3FFF))) { exact++; continue; }
											const int place = (t - (e & 0x7F)) & 0x3FFF;
											if (moved[bank] < 0) { moved[bank] = place; touched[nt++] = bank; }
											if (moved[bank] != place) bad++;
											else moved_n[bank]++;
										}
										for (int i = 0; i < nt; i++)
											if (moved_n[touched[i]] < 4) bad += moved_n[touched[i]];   // (a bank moved: 4 of its tiles at least)
										if (bad * 4 <= n && exact * 2 >= n) {
											rf.found = true; rf.x = mx; rf.y = my;
											for (int i = 0; i < nt; i++) rf.moved.push_back({ touched[i], moved[touched[i]] });
										}
										for (int i = 0; i < nt; i++) { moved[touched[i]] = -1; moved_n[touched[i]] = 0; }
								};
								// (nearest first, the last place itself before any other: a repeating sky or dunes agree in places
								// a few tiles apart, and the nearest is the row's)
								if (hint.x >= 0)
									for (int d = 0; d <= 8 + 3 && !rf.found; d++)
										for (int dy = -3; dy <= 3 && !rf.found; dy++) {
											const int my = hint.y + dy, ax = d - std::abs(dy);
											if (ax < 0 || ax > 8 || my < 0 || my >= b.h) continue;
											try_at(((hint.x + ax) % b.w + b.w) % b.w, my);
											if (ax && !rf.found) try_at(((hint.x - ax) % b.w + b.w) % b.w, my);
										}
								const bool full_lately = hint.full_failed_at != ~0ull && board.frame >= hint.full_failed_at && board.frame - hint.full_failed_at < 8;
								static const bool all_rows = getenv("VF3_NO_TILE_SKIP") != nullptr;
								if (!full_lately || all_rows)
									for (int my = 0; my < b.h && !rf.found; my++)
										for (int mx = 0; mx < b.w && !rf.found; mx++) try_at(mx, my);
								rf.all = !full_lately || all_rows;
								if (rf.found) { hint.x = rf.x; hint.y = rf.y; hint.full_failed_at = ~0ull; }
								else if (!full_lately) hint.full_failed_at = board.frame;
							}
							// (remembered as not found only when all the map was looked through: while the 8 frames' gate
							// held it to the hint's neighbourhood, a place further off wasn't looked at)
							if (!rf.found && !failed_lately && rf.all) {
								if (row_failed.size() > 4096) row_failed.clear();
								row_failed[row_key] = board.frame;
							}
							if (rf.found) {
								from_map = true; row_x0 = rf.x - s; row_y = rf.y;
								for (auto &[bank, place] : rf.moved) backdrop.seen[bank] = place;
							}
						} else {
							// (9 in 10 of it a filler: there)
							const uint32_t top = backdrop_entry(board, 0, -1) & 0x3FFF, bottom = backdrop_entry(board, 0, b.h) & 0x3FFF;
							const uint32_t top_t = backdrop_entry(board, 0, -1, true) & 0x3FFF, bottom_t = backdrop_entry(board, 0, b.h, true) & 0x3FFF;
							int n_top = 0, n_bottom = 0;
							for (int c = 0; c <= last; c++) {
								const uint32_t t = ent(c) & 0x3FFF;
								n_top += t == top || t == top_t;
								n_bottom += t == bottom || t == bottom_t;
							}
							if (n_bottom * 10 >= (last + 1) * 9) { from_map = true; row_y = b.h; }
							else if (n_top * 10 >= (last + 1) * 9) { from_map = true; row_y = -1; }
							else {
								// (a row of one tile, or mostly (a map cleared to one tile, a band of fog): the sides its
								// commonest tile near each edge, below)
								int most = 0;
								for (int i = 0, j; i < m; i = j) {
									for (j = i; j < m && sorted[j] == sorted[i]; j++) {}
									most = std::max(most, j - i);
								}
								uniform_row = most * 4 >= m * 3;
							}
						}
					}
					if (!from_map && !uniform_row) sky_bad_rows++;
					static long logged = 0;
					if (!from_map && !uniform_row && getenv("VF3_BD_WHY") && long(board.frame) == atol(getenv("VF3_BD_WHY")) && logged++ < 60) {
						fprintf(stderr, "badrow f%llu y %u row %d sy %d x0 %d y0 %d h %d ok %d:", (unsigned long long)board.frame, y, row, l.sy, backdrop.x0, backdrop.y0, backdrop.h, backdrop.ok);
						for (int c = 0; c <= last; c++) if ((backdrop_entry(board, backdrop.x0 + s + c, backdrop.y0 + row) & 0x3FFF) != (ent(c) & 0x3FFF)) fprintf(stderr, " c%d %04x/%04x", c, backdrop_entry(board, backdrop.x0 + s + c, backdrop.y0 + row) & 0x3FFF, ent(c) & 0x3FFF);
						fprintf(stderr, "\n");
					}
				}
			}
			if (prim + k == 3 && !from_map && found == ext_rows.end()) sky_nomap_rows++;
			if (found == ext_rows.end() && from_map) {
				std::vector<std::pair<uint32_t, bool>> out;
				for (int j = 0; j < EXT / 8 + 2; j++)
					out.push_back({ backdrop_entry(board, row_x0 + s + edge + d * (j + 1), row_y), false });
				ext_how[key] = std::vector<uint8_t>(out.size(), 1);
				ext_edge[key] = backdrop_entry(board, row_x0 + s + edge, row_y);
				solid_only(out);
				found = ext_rows.emplace(key, std::move(out)).first;
			}
			if (found == ext_rows.end()) {
				std::vector<std::pair<uint32_t, bool>> out;
				std::vector<uint8_t> how;
				const int count = EXT / 8 + 2;
				const int base = find_base();
				if (base < 0) {
					out.assign(size_t(count), { fill, false });
					how.assign(size_t(count), 7);
				} else {
					int q = pat(base);
					bool rule = true;   // (the palette's bit 14 the pattern's bit 7, as the game sets it)
					for (int c = base; c != base - 3 * d; c -= d) rule = rule && ((ent(c) >> 14 & 1) == uint32_t(pat(c) >> 7 & 1));
					for (int c = base; c != edge; c += d) q = next(q);
					const uint32_t keep = ent(base) & (rule ? 0x8000u : 0xC000u);
					auto entry_of = [&](int p) { return uint32_t(p) | keep | (rule ? uint32_t(p & 0x80) << 7 : 0); };
					// the good tiles so far, outwards: the run on screen to the edge, then those made
					std::vector<std::pair<uint32_t, bool>> good;
					for (int c = base - 6 * d; c != edge + d; c += d)
						if (c >= 0 && c <= last) good.push_back({ ent(c), false });
					// the run's own seams
					float own = 0;
					int own_n = 0;
					for (size_t i = 1; i < good.size(); i++) {
						const float v = d > 0 ? seam(good[i - 1].first, false, good[i].first, false, l.four, 7, 0)
						                      : seam(good[i].first, false, good[i - 1].first, false, l.four, 7, 0);
						if (v >= 0) { own += v; own_n++; }
					}
					const float limit = std::max(6.0f, 2.5f * (own_n ? own / own_n : 3.0f) + 2.0f);
					auto fits = [&](const std::pair<uint32_t, bool> &prev, uint32_t e) {
						const float v = d > 0 ? seam(prev.first, prev.second, e, false, l.four, 7, 0)
						                      : seam(e, false, prev.first, prev.second, l.four, 7, 0);
						return v >= 0 && v <= limit;
					};
					// Each step the first of these that meets the tile before smoothly: the run on (along a row whose wrap
					// was seen, across a gap in the patterns learned), the next pattern plainly, across the learned gap,
					// or the row's other end (a wrap not yet seen: the rows' usual length back, along the gaps). None:
					// the row mirrored from there.
					auto plain = [&](int x) { return x + d; };
					auto gap = [&](int x) {
						const auto &m = d > 0 ? skip_fwd[prim + k] : skip_bwd[prim + k];
						auto it = m.find(x);
						return it != m.end() ? it->second : plain(x);
					};
					// The row's length: the step from its patterns to the next row's (the rows are stored one after
					// another, of their own lengths: a sky's top rows have fewer patterns), the same for 16 columns in
					// from the edge, or from the row before's (only as long as the one before it), else the rows' usual
					// length
					auto pat_at = [&](int r, int i) { return int(name_at(l.names, r, s + i) & 0x3FFF); };
					auto offset = [&](int r1, int r2) {   // (but a few tiles the game put over them)
						int diff[16], n = 0;
						for (int c = edge; n < 16 && c >= 0 && c <= last; c -= d) diff[n++] = pat_at(r2, c) - pat_at(r1, c);
						for (int i = 0; i < n; i++)
							if (std::count(diff, diff + n, diff[i]) * 4 >= n * 3) return diff[i];
						return INT32_MIN;
					};
					auto apart = [&](int r1, int r2) {
						if (r1 < 0 || r2 > 63) return -1;
						const int a = offset(r1, r2);
						return a >= 31 && a < 4096 ? a : -1;
					};
					auto len_of = [&](int r) {
						const int a = apart(r, r + 1);
						if (a > 0) return a;
						const int b = apart(r - 1, r);
						return b > 0 && b == apart(r - 2, r - 1) ? b : -1;
					};
					int row_len = len_of(row);
					if (row_len < 0) row_len = P.len;
					auto other_end = [&](int x) {
						if (row_len <= 0) return -1;
						const auto &m = d > 0 ? skip_bwd[prim + k] : skip_fwd[prim + k];
						for (int i = 0; i < row_len - 1; i++) {
							auto it = m.find(x);
							x = it != m.end() ? it->second : x - d;
						}
						return x;
					};
					bool mirror = false;
					int mirror_at = 0;   // (the first mirrored tile's j)
					for (int j = 0; j < count; j++) {
						if (!mirror) {
							int took = -1;
							const int cands[4] = { next(q), plain(q), gap(q), other_end(q) };
							int which = 0;
							for (int c : cands) {
								which++;
								if (c >= 0 && fits(good.back(), entry_of(c))) { took = c; break; }
							}
							// (none: the one that meets it best, within three times the limit: a panorama's own ends,
							// photographs that don't quite meet, still do; another picture doesn't)
							float best = 3 * limit;
							const bool found_one = took >= 0;
							for (int c : cands) {
								if (found_one || c < 0) continue;
								const std::pair<uint32_t, bool> &pv = good.back();
								const float v = d > 0 ? seam(pv.first, pv.second, entry_of(c), false, l.four, 7, 0)
								                      : seam(entry_of(c), false, pv.first, pv.second, l.four, 7, 0);
								if (v >= 0 && v <= best) { best = v; took = c; }
							}
							// (a wrap not yet seen, taken as the next pattern doesn't quite meet the tile before, only where it
							// meets it clearly better: a cloud's edge isn't a row's)
							if (took >= 0 && took == cands[3] && cands[0] >= 0 && took != cands[0]) {
								const std::pair<uint32_t, bool> &pv = good.back();
								auto sv = [&](int c) {
									return d > 0 ? seam(pv.first, pv.second, entry_of(c), false, l.four, 7, 0)
									             : seam(entry_of(c), false, pv.first, pv.second, l.four, 7, 0);
								};
								const float vn = sv(cands[0]), vo = sv(took);
								if (vn >= 0 && vn <= 3 * limit && !(vo < 0.5f * vn)) { took = cands[0]; which = 1; }
							}
							if (took >= 0 && took == cands[3] && took != cands[0] && took != cands[1] && took != cands[2] && j > 0) {
								// Taken across the row's other end (a wrap not yet seen; not one learned, which would have
								// a wrongly learned one confirm itself): two rows doing so at the same step out is the
								// panorama's wrap, and the rows lined up with them (starting at the same column going left,
								// ending at the same going right) are learned, so next frame each carries on along its row
								const uint64_t vk = uint64_t(prim + k) << 40 | uint64_t(side) << 39 | uint64_t(uint32_t(l.sx) & 0x7FFFFFFF);
								if (++wrap_votes[vk][j] == 2) {
									const int first = std::min(q, took), len = std::abs(took - q) + 1;
									for (int r = 0; r < 64; r++) {
										const int off = offset(row, r);
										if (off == INT32_MIN) continue;
										const int lr = r == row ? len : len_of(r);
										if (lr < 0) continue;
										const int b = d < 0 ? first + off : first + len - 1 + off - (lr - 1);
										if (b >= 0 && b + lr <= 0x4000) add_row(prim + k, b, lr);
									}
								}
							}
							if (took >= 0) {
								q = took;
								good.push_back({ entry_of(q), false });
								out.push_back(good.back());
								how.push_back(uint8_t(found_one ? which : 5));
								continue;
							}
							mirror = true;
							mirror_at = j;
						}
						// the good tiles back from the break, flipped (the last of them first)
						const int back = std::max(0, int(good.size()) - 1 - (j - mirror_at));
						out.push_back({ good[size_t(back)].first, !good[size_t(back)].second });
						how.push_back(6);
					}
				}
				ext_how[key] = std::move(how);
				solid_only(out);
				found = ext_rows.emplace(key, std::move(out)).first;
			}
			// The edge tile's part past the screen from the map too: the game rewrites the column at the screen's edge
			// as it scrolls, and what it has there mid-way (Wolf's sunset: red squares) is a few pixels on the arcade,
			// a whole tile and more here
			if (auto ee = ext_edge.find(key); ee != ext_edge.end()) {
				const int x0 = edge * 8 - fine;
				if (side) { const int from = W - x0; if (from < 8) put(dst, W, ee->second, l.four, fy, from, 8 - from); }
				else if (fine) put(dst, -fine, ee->second, l.four, fy, 0, fine);
				// and its part on the screen, a few pixels at the 4:3 edge (mid-picture in widescreen: a strip in the
				// sky), where the picture has this layer's tile: the map's
				if ((ent(edge) & 0x3FFF) != (ee->second & 0x3FFF)) {
					uint32_t was[8 + 2 * EXT] = {}, now[8 + 2 * EXT] = {};
					put(was + EXT, 0, ent(edge), l.four, fy, 0, 8);
					put(now + EXT, 0, ee->second, l.four, fy, 0, 8);
					uint32_t *row_px = below.data() + size_t(y) * W;
					const int a = side ? std::max(x0, 0) : 0, b = side ? W : std::min(x0 + 8, W);
					for (int x = a; x < b; x++)
						if (row_px[x] == was[EXT + x - x0]) row_px[x] = now[EXT + x - x0];
				}
			}
			const std::vector<std::pair<uint32_t, bool>> &tiles = found->second;
			for (int t = edge + d, j = 0; j < int(tiles.size()); t += d, j++) {
				const int x = t * 8 - fine;
				if (side ? x >= W + EXT : x + 8 <= -EXT) break;
				const int a = std::max(x, -EXT), b = std::min(x + 8, W + EXT);
				if (b > a) put(dst, a, tiles[size_t(j)].first, l.four, fy, a - x, b - a, tiles[size_t(j)].second);
				static const bool mark = getenv("VF3_EXT_MARK") != nullptr;
				if (mark && b > a) {   // (tests: tinted by how the tile was found)
					static const uint32_t TINT[8] = { 0, 0, 0x00FFFF, 0xFFFF00, 0xFF00FF, 0x0000FF, 0x00FF00, 0xFF0000 };
					const auto &hw = ext_how[key];
					const uint32_t tc = size_t(j) < hw.size() ? TINT[hw[size_t(j)]] : 0;
					if (tc)
						for (int xx = a; xx < b; xx++) {
							uint32_t &px = dst[xx];
							uint32_t r = 0;
							for (int sh = 0; sh < 24; sh += 8) r |= (((px >> sh & 255) + (tc >> sh & 255)) / 2) << sh;
							px = (px & 0xFF000000u) | r;
						}
				}
			}
			// Where the screen has shown this part of the layer: as it showed it. Past that, where the row as made
			// doesn't agree with what's remembered (a row made wrongly: the sky streamed in pieces, the sun's tile
			// repeated), what's remembered mirrored on out.
			static const bool no_mem = getenv("VF3_NO_SEEN") != nullptr;   // (tests: the sides without the memory)
			// (nothing remembered yet, as for the stage's backdrop until the game's map is lost: a cut, the test menu
			// left, find_backdrop above clearing backdrop.ok and .known mid-line after the memory was passed over)
			if (!no_mem && !from_map && !(prim + k == 3 && (backdrop.ok || backdrop.known)) && !mem.at_x.empty()) {   // (not for a stage's backdrop: as the screen has it, a cleared map's rows blank)
				const int32_t ax0 = mem.x + ((((l.sx & 511) - mem.last_sx + 256) & 511) - 256), ay = mem.y + int(y);
				const size_t line_i = size_t(ay & 511) * R;
				auto x_of = [&](int o) { return side ? W + o : -1 - o; };   // (o pixels out from the edge)
				bool known[EXT];
				uint32_t was[EXT];
				int n = 0, last = -1;
				long apart = 0;
				for (int o = 0; o < EXT; o++) {
					const int xx = x_of(o);
					const int32_t p = ax0 + xx;
					const size_t i = line_i + size_t(p & (R - 1));
					known[o] = mem.at_x[i] == p && mem.at_y[i] == ay && !(mem.col[i] & 0x8000);
					if (o < 96) { mem_drawn += known[o]; mem_sides++; }
					if (!known[o]) continue;
					was[o] = dst[xx];
					dst[xx] = colour_of(mem.col[i]);
					static const bool mem_mark = getenv("VF3_MEM_MARK") != nullptr;   // (tests: what's remembered tinted green)
					if (mem_mark) dst[xx] = (dst[xx] & 0xFF00FF00u) | 0x0000C000u | (dst[xx] & 0xFF) / 2 | ((dst[xx] >> 16 & 0xFF) / 2) << 16;
					for (int sh = 0; sh < 24; sh += 8) apart += std::abs(int(was[o] >> sh & 255) - int(dst[xx] >> sh & 255));
					n++;
					last = o;
				}
				// (decided once for the map row, this frame: its lines alike)
				const uint64_t mk = uint64_t(prim + k) << 40 | uint64_t(row) << 32 | uint64_t(side);
				auto dec = mem_mirror.find(mk);
				if (dec == mem_mirror.end()) dec = mem_mirror.emplace(mk, n >= 24 && apart > long(n) * 24).first;
				if (dec->second && last >= 0) {
					const int period = last + 1;
					for (int o = last + 1; o < EXT; o++) {
						if (known[o]) continue;
						const int m = (o - last - 1) % (2 * period);
						dst[x_of(o)] = dst[x_of(m < period ? last - m : m - period)];
					}
				}
			}
		}
	}
	memcpy(below_wide.data() + size_t(y) * WW + EXT, below.data() + size_t(y) * W, size_t(W) * 4);
	// A row the game leaves empty all across the screen (a replay's letterbox band, where the sky layer stops and
	// the band is the 2D's): empty out to the sides too. Carried on from the stage's panorama it was sky there, the
	// band's edge stepped where the 4:3 picture ends, wherever the sky was behind it.
	const uint32_t *row = below.data() + size_t(y) * W;
	bool empty = true;
	for (int x = 0; x < W && empty; x++) empty = row[x] == 0;
	if (empty) {
		std::fill_n(below_wide.data() + size_t(y) * WW, EXT, 0u);
		std::fill_n(below_wide.data() + size_t(y) * WW + EXT + W, EXT, 0u);
	}
}

void TileGen::aligned(std::vector<uint32_t> &b, std::vector<uint32_t> &a) const
{
	const int S = 2;
	b.assign(size_t(W) * H, 0);
	a.assign(size_t(W) * H, 0);
	for (int y = 0; y + S < H; y++) {
		memcpy(&b[size_t(y) * W], &below[size_t(y + S) * W + S], size_t(W - S) * 4);
		memcpy(&a[size_t(y) * W], &above[size_t(y + S) * W + S], size_t(W - S) * 4);
		// the S columns the shift leaves on the right: the last one's pixels carried on (else a strip of what's
		// under the 2D shows down the right edge: widescreen's sky, the 3D's sides)
		for (int x = W - S; x < W; x++) {
			b[size_t(y) * W + x] = b[size_t(y) * W + W - S - 1];
			a[size_t(y) * W + x] = a[size_t(y) * W + W - S - 1];
		}
	}
	// and the S rows at the bottom: the last one's
	for (int y = H - S; y < H; y++) {
		memcpy(&b[size_t(y) * W], &b[size_t(H - S - 1) * W], size_t(W) * 4);
		memcpy(&a[size_t(y) * W], &a[size_t(H - S - 1) * W], size_t(W) * 4);
	}
}

void TileGen::aligned_wide(std::vector<uint32_t> &b) const
{
	const int S = 2;
	b.assign(size_t(WW) * H, 0);
	for (int y = 0; y + S < H; y++) {
		memcpy(&b[size_t(y) * WW], &below_wide[size_t(y + S) * WW + S], size_t(WW - S) * 4);
		for (int x = WW - S; x < WW; x++) b[size_t(y) * WW + x] = b[size_t(y) * WW + WW - S - 1];
	}
	for (int y = H - S; y < H; y++) memcpy(&b[size_t(y) * WW], &b[size_t(H - S - 1) * WW], size_t(WW) * 4);
}

void TileGen::compose(const uint32_t *pic3d, uint32_t *out) const
{
	for (int i = 0; i < W * H; i++) {
		uint32_t c = 0xFF000000u;
		if (below[i] >> 24) c = below[i];
		if (pic3d) {
			uint32_t d = pic3d[i];
			if (d >> 24) c = d | 0xFF000000u;
		}
		if (above[i] >> 24) c = above[i];
		out[i] = c;
	}
}

} // namespace video
