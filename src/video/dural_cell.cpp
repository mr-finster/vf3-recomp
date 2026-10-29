#include "dural_cell.h"
#include "tilegen.h"
#include "../machine/model3.h"
#include <algorithm>
#include <cstring>

namespace video {

namespace {

// The cell's place in the aligned picture: the grid's cells are 70 pixels apart, the top row's faces at y 272-313
// (Kage's x 392-459, his cursor's frame 390-459, 270-315): hers is the next, x 462-527 (the 4:3 picture shows to 495)
constexpr int CELL_X = 462, CELL_Y = 272, CELL_W = 66, CELL_H = 42;
// and the part of her picture in it: her face (x 178-417, y 130-282 of the 496 x 384), its middle in the cell's
// first 34 pixels (what the 4:3 picture shows of it)
constexpr int CROP_X = 178, CROP_Y = 130, CROP_W = 240, CROP_H = 153;

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

bool dural_cell(const m3::Model3 &board, std::vector<uint32_t> &above, DuralPicture &pic)
{
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
	for (int y = CELL_Y - 1; y <= CELL_Y + CELL_H; y++)
		for (int x = CELL_X - 1; x <= CELL_X + CELL_W && x < W; x++) {
			if (y < 0 || y >= H || a_shows(x, y) || !b_shows(x, y)) continue;
			const bool edge = y < CELL_Y || y >= CELL_Y + CELL_H || x < CELL_X || x >= CELL_X + CELL_W;
			above[size_t(y) * W + x] = faded(edge ? 0xFF000000u : pic.cell[size_t(y - CELL_Y) * CELL_W + (x - CELL_X)]);
		}
	return true;
}

} // namespace video
