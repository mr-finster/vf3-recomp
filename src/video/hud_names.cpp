#include "hud_names.h"
#include "tilegen.h"
#include "../machine/model3.h"
#include <algorithm>
#include <cstring>

namespace video {

namespace {

// The font: this project's own, 7 x 10, in the HUD's style ('#' lit; the outline is drawn around)
struct Glyph { char c; const char *rows[10]; };
const Glyph FONT[] = {
	{ 'A', { ".#####.", "###.###", "###.###", "###.###", "###.###", "#######", "###.###", "###.###", "###.###", "###.###" } },
	{ 'B', { "######.", "###.###", "###.###", "###.###", "######.", "###.###", "###.###", "###.###", "###.###", "######." } },
	{ 'C', { ".######", "###....", "###....", "###....", "###....", "###....", "###....", "###....", "###....", ".######" } },
	{ 'D', { "######.", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "######." } },
	{ 'E', { "#######", "###....", "###....", "###....", "######.", "###....", "###....", "###....", "###....", "#######" } },
	{ 'F', { "#######", "###....", "###....", "###....", "######.", "###....", "###....", "###....", "###....", "###...." } },
	{ 'G', { ".######", "###....", "###....", "###....", "###.###", "###.###", "###.###", "###.###", "###.###", ".######" } },
	{ 'H', { "###.###", "###.###", "###.###", "###.###", "#######", "###.###", "###.###", "###.###", "###.###", "###.###" } },
	{ 'I', { ".#####.", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", ".#####." } },
	{ 'J', { "....###", "....###", "....###", "....###", "....###", "....###", "###.###", "###.###", "###.###", ".#####." } },
	{ 'K', { "###.###", "###.###", "###.##.", "###.##.", "#####..", "#####..", "###.##.", "###.###", "###.###", "###.###" } },
	{ 'L', { "###....", "###....", "###....", "###....", "###....", "###....", "###....", "###....", "###....", "#######" } },
	{ 'M', { "##...##", "###.###", "#######", "#######", "##.#.##", "##...##", "##...##", "##...##", "##...##", "##...##" } },
	{ 'N', { "###..##", "####.##", "####.##", "#######", "#######", "##.####", "##.####", "##..###", "##..###", "##...##" } },
	{ 'O', { ".#####.", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", ".#####." } },
	{ 'P', { "######.", "###.###", "###.###", "###.###", "###.###", "######.", "###....", "###....", "###....", "###...." } },
	{ 'Q', { ".#####.", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.##.", ".####.#" } },
	{ 'R', { "######.", "###.###", "###.###", "###.###", "###.###", "######.", "###.##.", "###.###", "###.###", "###.###" } },
	{ 'S', { ".######", "###....", "###....", "###....", ".#####.", "....###", "....###", "....###", "....###", "######." } },
	{ 'T', { "#######", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###.." } },
	{ 'U', { "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", ".#####." } },
	{ 'V', { "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", ".##.##.", ".#####.", "..###.." } },
	{ 'W', { "##...##", "##...##", "##...##", "##...##", "##...##", "##.#.##", "#######", "#######", "###.###", "##...##" } },
	{ 'X', { "###.###", "###.###", ".##.##.", ".#####.", "..###..", "..###..", ".#####.", ".##.##.", "###.###", "###.###" } },
	{ 'Y', { "###.###", "###.###", "###.###", "###.###", ".#####.", "..###..", "..###..", "..###..", "..###..", "..###.." } },
	{ 'Z', { "#######", "....###", "....###", "...###.", "..###..", ".###...", "###....", "###....", "###....", "#######" } },
	{ '0', { ".#####.", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", "###.###", ".#####." } },
	{ '1', { "..###..", ".####..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", ".#####." } },
	{ '2', { ".#####.", "###.###", "....###", "....###", "...###.", "..###..", ".###...", "###....", "###....", "#######" } },
	{ '3', { "######.", "....###", "....###", "....###", ".#####.", "....###", "....###", "....###", "....###", "######." } },
	{ '4', { "###.###", "###.###", "###.###", "###.###", "#######", "....###", "....###", "....###", "....###", "....###" } },
	{ '5', { "#######", "###....", "###....", "###....", "######.", "....###", "....###", "....###", "....###", "######." } },
	{ '6', { ".######", "###....", "###....", "###....", "######.", "###.###", "###.###", "###.###", "###.###", ".#####." } },
	{ '7', { "#######", "....###", "....###", "....###", "...###.", "..###..", "..###..", "..###..", "..###..", "..###.." } },
	{ '8', { ".#####.", "###.###", "###.###", "###.###", ".#####.", "###.###", "###.###", "###.###", "###.###", ".#####." } },
	{ '9', { ".#####.", "###.###", "###.###", "###.###", "###.###", ".######", "....###", "....###", "....###", "######." } },
	{ '.', { ".......", ".......", ".......", ".......", ".......", ".......", ".......", ".......", "..###..", "..###.." } },
	{ '-', { ".......", ".......", ".......", ".......", ".#####.", ".#####.", ".......", ".......", ".......", "......." } },
	{ '!', { "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", "..###..", ".......", "..###..", "..###.." } },
	{ '?', { ".#####.", "###.###", "....###", "....###", "...###.", "..###..", "..###..", ".......", "..###..", "..###.." } },
};
constexpr int GW = 7, GH = 10, PITCH = 10, SPACE = 5;

const Glyph *glyph(char c)
{
	for (const Glyph &g : FONT)
		if (g.c == c) return &g;
	return nullptr;
}

// the colours down a letter, as the HUD's names are lit: white at the top to lavender, and a stroke's right-hand
// pixel a darker blue (its shadow side), as 0xAABBGGRR
uint32_t shade(int row, bool right_edge)
{
	static const uint32_t LIGHT[GH] = { 0xFFFFFFFF, 0xFFFFF6F6, 0xFFFFEEEE, 0xFFFFE6E6, 0xFFFFDEDE,
		0xFFFFD5D5, 0xFFFFCDCD, 0xFFFFBDBD, 0xFFFFACAC, 0xFFFFA4A4 };
	static const uint32_t DARK[GH] = { 0xFFFF8383, 0xFFFF7B7B, 0xFFFF7B7B, 0xFFFF7373, 0xFFFF736A,
		0xFFFF6A6A, 0xFFFF6262, 0xFFFF625A, 0xFFFF5252, 0xFFFF4141 };
	return right_edge ? DARK[row] : LIGHT[row];
}

int text_width(const std::string &s)
{
	int w = 0;
	for (char c : s) w += c == ' ' ? SPACE : PITCH;
	return w > 0 ? w - (PITCH - GW) : 0;
}

// A map entry's tile (the colour bits off); 0x20 is the HUD's blank
uint32_t tile_at(const uint32_t *names, int row, int col)
{
	const uint32_t w = names[row * 32 + col / 2];
	return ((col & 1) ? w : (w >> 16)) & 0x3FFF;
}

// The fight's HUD: its round markers under both bars, two tiles side by side each, at least two a side (columns
// 22-25 and 36-39 of rows 6 and 7), the same on every stage and for every fighter. A round won turns its marker red: other
// tiles, still side by side (so each marker on its own, not all alike: from the second round they differ).
bool marker_at(const uint32_t *names, int c)
{
	if (c < 0 || c + 1 >= 64) return false;
	for (int row = 6; row <= 7; row++) {
		const uint32_t a = tile_at(names, row, c);
		if (a == 0 || a == 0x20 || tile_at(names, row, c + 1) != a + 1) return false;
	}
	return true;
}

// ... and as many markers as the rounds to win (2 to 5: a match between two players has 3 by default), out from
// the timer: player 1's from column 22 leftwards, player 2's from 36 rightwards. first / last: the columns they
// take, the names' places end there.
bool fight_hud(const uint32_t *names, int &p1_first, int &p2_last)
{
	for (int c : { 22, 24, 36, 38 })
		if (!marker_at(names, c)) return false;
	p1_first = 22;
	while (p1_first > 22 - 2 * 4 && marker_at(names, p1_first - 2)) p1_first -= 2;
	p2_last = 39;
	while (p2_last < 39 + 2 * 3 && marker_at(names, p2_last + 1)) p2_last += 2;
	return true;
}

// The columns a fighter's name picture covers, rows 6 and 7, within [from, to]: its width is the name's (PAI's 4
// tiles, TAKA-ARASHI's 10 in two pieces), and the two rows needn't be the same. False if there's none.
bool picture_span(const uint32_t *names, int from, int to, int &c0, int &c1)
{
	c0 = 64; c1 = -1;
	for (int row = 6; row <= 7; row++)
		for (int c = from; c <= to; c++) {
			const uint32_t t = tile_at(names, row, c);
			if (t != 0 && t != 0x20) { c0 = std::min(c0, c); c1 = std::max(c1, c); }
		}
	return c1 >= c0;
}

void draw(std::vector<uint32_t> &px, int x0, int y0, const std::string &s)
{
	// the outline first (every pixel next to a lit one), then the letters over it
	for (int pass = 0; pass < 2; pass++) {
		int x = x0;
		for (char c : s) {
			if (c == ' ') { x += SPACE; continue; }
			if (const Glyph *g = glyph(c))
				for (int r = 0; r < GH; r++)
					for (int k = 0; k < GW; k++) {
						if (g->rows[r][k] != '#') continue;
						if (pass == 0) {
							for (int dy = -1; dy <= 1; dy++)
								for (int dx = -1; dx <= 1; dx++) {
									const int X = x + k + dx, Y = y0 + r + dy;
									if (X >= 0 && X < W && Y >= 0 && Y < H) px[size_t(Y) * W + X] = 0xFF000000u;
								}
						} else {
							const int X = x + k, Y = y0 + r;
							const bool edge = k + 1 >= GW || g->rows[r][k + 1] != '#';   // (the stroke's right-hand pixel)
							if (X >= 0 && X < W && Y >= 0 && Y < H) px[size_t(Y) * W + X] = shade(r, edge);
						}
					}
			x += PITCH;
		}
	}
}

} // namespace

bool hud_names(const m3::Model3 &board, std::vector<uint32_t> &above, const std::string names[2], const HudFlag *flags[2], int flag_at[2][2])
{
	if (flag_at) for (int p = 0; p < 2; p++) flag_at[p][0] = flag_at[p][1] = -1;
	if (board.tile_ram.size() < 0x100000 || above.size() < size_t(W) * H) return false;
	// layer A (0): on, and not scrolled (the HUD's own place)
	const uint32_t reg = board.tile_regs[0x60 / 4];
	if (!(reg & 0x80000000u) || (reg & 0x3FF) || ((reg >> 16) & 0x1FF)) return false;
	const uint32_t *map = reinterpret_cast<const uint32_t *>(board.tile_ram.data() + 0xF8000);
	int p1_first = 0, p2_last = 0;
	if (!fight_hud(map, p1_first, p2_last)) return false;
	// each player's part of the row (short of the round markers, however many): player 1's name from column 2,
	// player 2's to column 59, as the game places its pictures
	constexpr int ROW = 6;
	const int P1_FROM = 0, P1_TO = p1_first - 1, P2_FROM = p2_last + 1, P2_TO = 63;
	int span[2][2];
	if (!picture_span(map, P1_FROM, P1_TO, span[0][0], span[0][1]) || !picture_span(map, P2_FROM, P2_TO, span[1][0], span[1][1]))
		return false;
	// (the picture as aligned: TileGen::aligned moves it 2 pixels up and left)
	constexpr int S = 2, TOP = ROW * 8 - S, TEXT_Y = TOP + 3, P1_X = 2 * 8 - S + 3, P2_RIGHT = 60 * 8 - S - 3;
	for (int p = 0; p < 2; p++) {
		if (names[p].empty()) continue;
		const int tw = text_width(names[p]);
		// the flag, out from the middle of the name, if it fits short of the round markers (5 of them leave no room)
		const HudFlag *fl = flags && flags[p] && flags[p]->w > 0 ? flags[p] : nullptr;
		const int gap = 4, limit = p ? (p2_last + 1) * 8 - S + 1 : p1_first * 8 - S - 1;
		if (fl && (p ? P2_RIGHT - tw - gap - fl->w < limit : P1_X + fl->w + gap + tw > limit)) fl = nullptr;
		const int w = tw + (fl ? fl->w + gap : 0);
		const int x = p ? P2_RIGHT - w : P1_X;
		// the fighter's name cleared: its picture's tiles, and the place of the longer name
		int from = std::max(0, std::min(span[p][0] * 8 - S, x - 2)), to = std::min(W, std::max((span[p][1] + 1) * 8 - S, x + w + 2));
		if (p == 0) to = std::min(to, p1_first * 8 - S - 1);       // (never into the round markers)
		else from = std::max(from, (p2_last + 1) * 8 - S + 1);
		for (int y = TOP; y < TOP + 16; y++)
			for (int X = from; X < to; X++) above[size_t(y) * W + X] = 0;
		if (fl) {
			// in a black edge, as the letters are, its middle on the letters' middle
			const int fx = p ? x + tw + gap : x, fy = TEXT_Y + (GH - fl->h) / 2;
			if (flag_at) { flag_at[p][0] = fx; flag_at[p][1] = TEXT_Y; }
			for (int y = -1; y <= fl->h && fl->px.size() >= size_t(fl->w) * size_t(fl->h); y++)   // (none: the host's to draw)
				for (int X = -1; X <= fl->w; X++) {
					const int ax = fx + X, ay = fy + y;
					if (ax < 0 || ax >= W || ay < 0 || ay >= H) continue;
					const bool edge = y < 0 || y >= fl->h || X < 0 || X >= fl->w;
					above[size_t(ay) * W + ax] = edge ? 0xFF000000u : fl->px[size_t(y) * size_t(fl->w) + size_t(X)];
				}
		}
		draw(above, p ? x : x + (fl ? fl->w + gap : 0), TEXT_Y, names[p]);
	}
	return true;
}

} // namespace video
