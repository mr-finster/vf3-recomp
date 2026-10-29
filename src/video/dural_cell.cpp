#include "dural_cell.h"
#include "tilegen.h"
#include "../machine/model3.h"
#include <algorithm>
#include <cstring>

namespace video {

namespace {

// The cell's place in the aligned picture: the grid's cells are 70 pixels apart, each a face in a 1-pixel white
// ring, 2 black pixels between (Kage's: black 389-390, white 391, face 392-458, white 459, black 460-461; the top
// row's black 270-271, white 272, face 273-315, white 316, black 317-318, the bottom row's white 319, face 320-362,
// white 363, black 364-365). The cursor's frame on her (the game's own, 3 pixels of bevel: x 462-531, y 270-316 on
// the top row, 317-363 on the bottom, where her seventh column is too) is 2 pixels on from the step after Kage's,
// so hers is set inside it, one cell the height of both rows: face x 464-530, y 273-362, the ring round it, black
// round that (the 4:3 picture shows to x 495)
constexpr int CELL_X = 464, CELL_Y = 273, CELL_W = 67, CELL_H = 90;
constexpr int FRAME_X0 = 462, FRAME_X1 = 531, FRAME_Y0[2] = { 270, 317 }, FRAME_H = 47;
// and the part of her picture in it: her face (x 166-357, y 50-306 of the 496 x 384), its middle in the cell's first
// 32 pixels (what the 4:3 picture shows of it)
constexpr int CROP_X = 166, CROP_Y = 50, CROP_W = 191, CROP_H = 257;
// the 2P marker in its frame (its lower right: the frame's x + 44-66, y + 26-43), kept from where the game draws it
// (on Kage's, or any of the 12) for hers, where the game's is past the picture's edge
constexpr int MARK_DX = 44, MARK_DY = 26, MARK_W = 23, MARK_H = 18;

// The ROM as the game reads it: a big-endian byte of the fixed CROM (0xFF800000 on) or of a bank of the banked
struct Rom
{
	const m3::Model3 &b;
	int bank;
	bool ok(uint32_t a, size_t n) const
	{
		const size_t o = off(a);
		return a >= 0xFF000000u && o + n <= (a >= 0xFF800000u ? b.crom.size() : b.banked.size());
	}
	size_t off(uint32_t a) const { return a >= 0xFF800000u ? size_t(a & 0x7FFFFF) : size_t(bank) * 0x800000 + (a & 0x7FFFFF); }
	uint8_t u8(uint32_t a) const { const size_t o = off(a); return (a >= 0xFF800000u ? b.crom : b.banked)[o ^ 3]; }
	uint16_t u16(uint32_t a) const { return uint16_t(u8(a) << 8 | u8(a + 1)); }
	uint32_t u32(uint32_t a) const { return uint32_t(u16(a)) << 16 | u16(a + 2); }
};

// The game's unpacker (0x32DB0): 4-bit counts, then bytes, each a recent one (the last 8, moved to the front) or new
bool unpack(const Rom &r, uint32_t a, size_t bytes, std::vector<uint8_t> &out)
{
	uint32_t w = 0;
	int n = 0;
	auto next = [&] { if (!r.ok(a, 4)) return false; w = r.u32(a); a += 4; n = 32; return true; };
	if (!next()) return false;
	auto bits = [&](int k, uint32_t &v) {
		v = 0;
		while (k > 0) {
			if (n == 0 && !next()) return false;
			const int t = std::min(k, n);
			v = (v << t) | ((w >> (n - t)) & ((1u << t) - 1));
			n -= t; k -= t;
		}
		return true;
	};
	uint8_t recent[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	auto byte = [&](uint8_t &v) {
		uint32_t f, x;
		if (!bits(1, f)) return false;
		if (!f) {
			if (!bits(3, x)) return false;
			v = recent[x];
			memmove(recent + 1, recent, x);
		} else {
			if (!bits(8, x)) return false;
			v = uint8_t(x);
			memmove(recent + 1, recent, 7);
		}
		recent[0] = v;
		return true;
	};
	out.clear();
	while (out.size() < bytes) {
		uint32_t c;
		uint8_t v;
		if (!bits(4, c)) return false;
		if (!(c & 8)) {
			for (uint32_t i = 0; i <= c; i++) { if (!byte(v)) return false; out.push_back(v); }
		} else {
			if (!byte(v)) return false;
			for (uint32_t i = 0; i <= (c & 7); i++) out.push_back(v);
		}
	}
	out.resize(bytes);
	return true;
}

uint32_t rgba(uint16_t c) { return (c & 0x8000) ? 0 : 0xFF000000u | uint32_t(((c >> 10) & 31) * 255 / 31) << 16 | uint32_t(((c >> 5) & 31) * 255 / 31) << 8 | uint32_t((c & 31) * 255 / 31); }

} // namespace

bool dural_picture(const m3::Model3 &board, DuralPicture &pic)
{
	if (pic.w) return pic.w > 0;
	pic.w = -1;
	const m3::DuralPatch *d = board.profile->dural;
	if (!d) return false;
	const Rom r{ board, int(d->data_bank) };
	// the descriptor: 1, chunks, width, height (in tiles), then the map; the data: each chunk's palette (a count,
	// then that many colours) and patterns (a count, bit 15 packed: then its size, a halfword, and the packed bytes)
	const uint32_t at = d->descriptors + 4 * d->picture, data_at = d->data + 4 * d->picture;
	if (!r.ok(at, 4) || !r.ok(data_at, 4)) return false;
	const uint32_t desc = r.u32(at);
	uint32_t p = r.u32(data_at);
	if (!r.ok(desc, 8)) return false;
	const int chunks = r.u16(desc + 2), tw = r.u16(desc + 4), th = r.u16(desc + 6);
	if (chunks < 1 || chunks > 64 || tw < 1 || tw > 64 || th < 1 || th > 64 || !r.ok(desc + 8, size_t(tw * th) * 2)) return false;
	std::vector<std::vector<uint16_t>> pal(chunks);
	std::vector<std::vector<uint8_t>> pat(chunks);
	for (int c = 0; c < chunks; c++) {
		if (!r.ok(p, 2)) return false;
		int n = int16_t(r.u16(p));
		if (n < 0 || n > 256) return false;
		n += n & 1;
		for (int i = 0; i < n; i++) pal[c].push_back(r.u16(p + 2 + 2 * i));
		p += 2 + 2 * n;
		const uint16_t k = r.u16(p);
		const size_t count = k & 0x7FFF;
		if (k & 0x8000) {
			const uint16_t size = r.u16(p + 2);
			if (!unpack(r, p + 6, count * 64, pat[c])) return false;
			p += 6 + size;
		} else {
			if (!r.ok(p + 2, count * 64)) return false;
			for (size_t i = 0; i < count * 64; i++) pat[c].push_back(r.u8(p + 2 + uint32_t(i)));
			p += 2 + uint32_t(count) * 64;
		}
	}
	// the map's entries: the chunk (bits 7-13) and its pattern (0-6); 8-bit patterns, their chunk's colours
	pic.px.assign(size_t(tw * 8) * (th * 8), 0);
	for (int ty = 0; ty < th; ty++)
		for (int tx = 0; tx < tw; tx++) {
			const uint16_t e = r.u16(desc + 8 + 2 * uint32_t(ty * tw + tx));
			const int c = (e >> 7) & 0x7F, i = e & 0x7F;
			if (c >= chunks || size_t(i + 1) * 64 > pat[c].size()) continue;
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++) {
					const uint8_t t = pat[c][size_t(i) * 64 + y * 8 + x];
					pic.px[size_t(ty * 8 + y) * (tw * 8) + tx * 8 + x] = t < pal[c].size() ? rgba(pal[c][t]) : 0;
				}
		}
	pic.w = tw * 8;
	pic.h = th * 8;
	// the cell's: the crop's pixels averaged (a box each)
	pic.cell.assign(size_t(CELL_W) * CELL_H, 0xFF000000u);
	for (int y = 0; y < CELL_H; y++)
		for (int x = 0; x < CELL_W; x++) {
			const int x0 = CROP_X + x * CROP_W / CELL_W, x1 = std::max(x0 + 1, CROP_X + (x + 1) * CROP_W / CELL_W);
			const int y0 = CROP_Y + y * CROP_H / CELL_H, y1 = std::max(y0 + 1, CROP_Y + (y + 1) * CROP_H / CELL_H);
			uint32_t s[3] = {}, n = 0;
			for (int yy = y0; yy < y1 && yy < pic.h; yy++)
				for (int xx = x0; xx < x1 && xx < pic.w; xx++) {
					const uint32_t v = pic.px[size_t(yy) * pic.w + xx];
					for (int k = 0; k < 3; k++) s[k] += (v >> (8 * k)) & 0xFF;   // (see-through counts as black)
					n++;
				}
			if (n) pic.cell[size_t(y) * CELL_W + x] = 0xFF000000u | (s[2] / n) << 16 | (s[1] / n) << 8 | (s[0] / n);
		}
	return true;
}

bool dural_cell(const m3::Model3 &board, std::vector<uint32_t> &above, DuralPicture &pic, DuralWide *wide)
{
	if (wide) wide->on = false;
	if (board.tile_ram.size() < 0x120000 || above.size() < size_t(W) * H) return false;
	const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
	// layer B (2): on and unscrolled, its map the grid's panel: rows 34-47, columns 0-61, nothing round it
	const uint32_t reg = board.tile_regs[0x68 / 4];
	if (!(reg & 0x80000000u) || (reg & 0x3FF) || ((reg >> 16) & 0x1FF)) return false;
	const uint32_t *names = vram + (0xF8000 + 2 * 0x2000) / 4;
	auto ent = [&](const uint32_t *nm, int row, int col) { const uint32_t w = nm[row * 32 + col / 2]; return (col & 1) ? (w & 0xFFFF) : (w >> 16); };
	for (int row = 33; row <= 48; row++)
		for (int col = 0; col < 64; col++) {
			const bool panel = row >= 34 && row <= 47 && col <= 61;
			if ((ent(names, row, col) == 0x20) == panel) return false;
		}
	if (!dural_picture(board, pic)) return false;
	// shown as layer B is: its pair's colour offset (the select's fades), where the line's mask shows B, not B'
	const uint32_t off_reg = board.tile_regs[0x44 / 4];
	const int off[3] = { int(int8_t(off_reg)) * 2, int(int8_t(off_reg >> 8)) * 2, int(int8_t(off_reg >> 16)) * 2 };
	auto faded = [&](uint32_t v) {
		uint32_t o = 0xFF000000u;
		for (int c = 0; c < 3; c++) o |= uint32_t(std::clamp(int((v >> (8 * c)) & 0xFF) + off[c], 0, 255)) << (8 * c);
		return o;
	};
	auto b_shows = [&](int x, int y) {   // (the line's mask: bit 15 the leftmost 32 pixels, the B pair's in the low half)
		const int py = y + 2, px = x + 2;
		return py >= 0 && py < 384 && ((vram[0xF7000 / 4 + py] >> (15 - px / 32)) & 1);
	};
	// over the grid's panel, under layer A's (0) cursor frame and its marker: layer A's texel seen-through or not
	const uint32_t *layer_a = vram + 0xF8000 / 4, *palette = vram + 0x40000;
	const uint32_t reg_a = board.tile_regs[0x60 / 4];
	const bool a_on = (reg_a & 0x80000000u) && !(reg_a & 0x3FF) && !((reg_a >> 16) & 0x1FF), a_four = (board.tile_regs[0x20 / 4] >> 12) & 1;
	auto a_shows = [&](int x, int y) {   // (the aligned picture's x, y: the layers' 2 pixels on)
		if (!a_on) return false;
		const int px = x + 2, py = y + 2;
		const uint32_t e = ent(layer_a, (py / 8) & 63, (px / 8) & 63);
		const int fy = py & 7, fx = px & 7;
		uint32_t c;
		if (a_four) {
			const uint32_t pat = (((e & 0x3FFF) << 1) | (e >> 15)) * 8;
			c = palette[((e & 0x7FF0) | ((vram[(pat + fy) & 0x3FFFF] >> ((7 - fx) * 4)) & 0xF)) & 0x7FFF];
		} else {
			const uint32_t pat = (e & 0x3FFF) * 16;
			c = palette[((e & 0x7F00) | ((vram[(pat + fy * 2 + (fx >> 2)) & 0x3FFFF] >> ((3 - (fx & 3)) * 8)) & 0xFF)) & 0x7FFF];
		}
		return !(c & 0x8000);
	};
	// the cell's pixel: the face, its white ring, the black round that
	auto cell_px = [&](int x, int y) -> uint32_t {
		if (x >= CELL_X && x < CELL_X + CELL_W && y >= CELL_Y && y < CELL_Y + CELL_H) return pic.cell[size_t(y - CELL_Y) * CELL_W + (x - CELL_X)];
		if (x >= CELL_X - 1 && x <= CELL_X + CELL_W && y >= CELL_Y - 1 && y <= CELL_Y + CELL_H) return 0xFFFFFFFFu;
		return 0xFF000000u;
	};
	for (int y = WIDE_Y; y < WIDE_Y + WIDE_H; y++)
		for (int x = WIDE_X; x < W; x++) {
			if (a_shows(x, y) || !b_shows(x, y)) continue;
			above[size_t(y) * W + x] = faded(cell_px(x, y));
		}
	// the 2P marker as the game draws it in player 2's frame (layer A's texels, as shown), kept for hers
	const uint32_t at = board.profile->select_cursor;
	const int col2 = at ? board.ram[(at + 0x80) ^ 3] : -1, row2 = at ? board.ram[(at + 0x80 - 1) ^ 3] & 1 : 0;
	if (col2 >= 0 && col2 < 6) {
		const int fx = 390 - 70 * (5 - col2), fy = FRAME_Y0[row2];
		if (a_shows(fx, fy + 10) && a_shows(fx + 1, fy + 10)) {
			pic.mark2.assign(size_t(MARK_W) * MARK_H, 0);
			for (int y = 0; y < MARK_H; y++)
				for (int x = 0; x < MARK_W; x++)
					if (a_shows(fx + MARK_DX + x, fy + MARK_DY + y)) pic.mark2[size_t(y) * MARK_W + x] = above[size_t(fy + MARK_DY + y) * W + fx + MARK_DX + x];
		}
	}
	if (wide) {
		// past the 4:3 edge: the rest of the cell, and of the cursor's frame where the game's shows on her (the game
		// draws it in the layer's columns past 496, which the picture leaves out): its left edge (x 462-464) mirrored for
		// the right (531-529), its top and bottom bevels as at x 495, and player 2's marker in its lower right
		wide->on = true;
		wide->px.assign(size_t(WIDE_W) * WIDE_H, 0);
		for (int y = WIDE_Y; y < WIDE_Y + WIDE_H; y++)
			for (int x = WIDE_X; x < WIDE_X + WIDE_W; x++) {
				uint32_t &o = wide->px[size_t(y - WIDE_Y) * WIDE_W + (x - WIDE_X)];
				if (x < W) { o = above[size_t(y) * W + x]; continue; }
				if (!b_shows(W - 1, y)) continue;
				o = faded(cell_px(x, y));
				if (x > FRAME_X1) continue;
				for (int r = 0; r < 2; r++) {
					const int y0 = FRAME_Y0[r], y1 = y0 + FRAME_H - 1;
					if (y < y0 || y > y1) continue;
					int sx = -1;
					if (x > FRAME_X1 - 3) sx = FRAME_X0 + (FRAME_X1 - x);   // (the right edge, as the left)
					else if (y < y0 + 3 || y > y1 - 3) sx = W - 1;   // (the top and bottom)
					if (sx >= 0 && a_shows(sx, y)) o = above[size_t(y) * W + sx];
				}
			}
		// player 2's frame on her (its left edge blue, not player 1's red): the marker
		if (col2 == 6 && !pic.mark2.empty()) {
			const int fy = FRAME_Y0[row2];
			const uint32_t edge = above[size_t(fy + 10) * W + FRAME_X0];
			if (a_shows(FRAME_X0, fy + 10) && ((edge >> 16) & 0xFF) > (edge & 0xFF))
				for (int y = 0; y < MARK_H; y++)
					for (int x = 0; x < MARK_W; x++) {
						const int X = FRAME_X0 + MARK_DX + x, Y = fy + MARK_DY + y;
						const uint32_t v = pic.mark2[size_t(y) * MARK_W + x];
						if ((v >> 24) && X >= W && X < WIDE_X + WIDE_W) wide->px[size_t(Y - WIDE_Y) * WIDE_W + (X - WIDE_X)] = v;
					}
		}
	}
	return true;
}

} // namespace video
