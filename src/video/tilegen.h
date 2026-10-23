// The tile generator's pictures: four 64x64 maps of 8x8 tiles in two pairs (A/A' and B/B'), one layer of each
// pair shown per 32-pixel column by the line's mask, each pair below or above the 3D. Drawn a line at a time at
// the start of each active line, as the hardware scans it out, into two 496x384 layers (below and above the 3D).
#pragma once
#include <cstdint>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>

namespace m3 { class Model3; }

namespace video {

const int W = 496, H = 384;
// the layer below the 3D widened for widescreen: EXT more pixels each side
const int EXT = 256, WW = W + 2 * EXT;

class TileGen
{
public:
	// RGBA8 (r in the low byte), alpha 0 where nothing is drawn
	std::vector<uint32_t> below = std::vector<uint32_t>(W * H), above = std::vector<uint32_t>(W * H);
	// The layer below the 3D WW wide, the screen's in the middle. The game writes its maps with no more than the
	// screen shows, the panoramas (the skies, VF3's every stage) whole in the tile patterns, a row of the map a run of
	// consecutive patterns: past the screen's edges each row's run carried on, wrapping where the panorama's rows
	// do (learned from the maps as the camera turns: where a run wraps, or where no row can start)
	std::vector<uint32_t> below_wide = std::vector<uint32_t>(WW * H);
	// Each layer's scroll as each line was drawn, and how much of the layer below the 3D each layer drew (in
	// 32-pixel columns, this frame): what moved, for smooth motion's pictures between frames
	struct Scroll { int16_t sx[4][H] = {}, sy[4][H] = {}; int below_columns[4] = {}; };
	Scroll scroll;
	// The layer below the 3D that drew most of it and how far it moved from `before` to `now` (in its pixels;
	// false: none, another layer, or a jump: a cut)
	// The registers hold the layer's scroll within a tile, the game moving the rest by rewriting the map, so the
	// move is found among the register's change and it plus or minus whole tiles: the one that lines `now` (the
	// aligned layer) up with `before`'s.
	static bool scrolled(const Scroll &before, const Scroll &now, const std::vector<uint32_t> &before_px,
		const std::vector<uint32_t> &now_px, float &dx, float &dy);
	void clear();
	// (tests: the backdrop's state this frame, a line: its map found, how it agrees with the layer on screen, and the
	// sides' tiles (as far as 21:9 shows them) from banks of tiles not loaded: whose place another bank on screen has)
	std::string audit(const m3::Model3 &board) const;
	// line_regs: the tile generator's 64 registers as they were when the line began (a worker thread drawing the line
	// while the game runs on: only the registers change line by line); null, the board's now
	void line(const m3::Model3 &board, unsigned y, const uint32_t *line_regs = nullptr);
	// The two layers as they line up with the 3D: 2 pixels left and up of where they're drawn (as Supermodel
	// places them, its 3D drawn as ours is)
	void aligned(std::vector<uint32_t> &below_out, std::vector<uint32_t> &above_out) const;
	void aligned_wide(std::vector<uint32_t> &out) const;   // below_wide, as aligned() (WW wide)
	// the 3D (RGBA, alpha ignored) between the layers, into out (RGBA); out may be the 3D's own buffer
	void compose(const uint32_t *pic3d, uint32_t *out) const;

private:
	const uint32_t *regs_now = nullptr;   // (line's line_regs while it draws)
	const uint32_t *regs_of(const m3::Model3 &board) const;
	// a layer's panorama: its rows as runs of patterns (first, length), learned where the map shows one wrap
	struct Pano { std::map<int, int> rows; int len = 0; };   // (len: the rows' usual length, the map's rows apart)
	Pano pano[4];
	// The patterns a run skips (the game's panoramas wrap round the pattern RAM, around what else is in it: 0x500 to
	// 0xEFF, 0x3C00 on): after a run's a comes b, and before b comes a. Kept from stage to stage, as the gaps stay.
	std::map<int, int> skip_fwd[4], skip_bwd[4];
	// a row's tiles past the screen's edge (entry, mirrored), this frame's: by layer, map row, side and scroll
	std::map<uint64_t, std::vector<std::pair<uint32_t, bool>>> ext_rows;
	std::map<uint64_t, uint32_t> ext_edge;   // (a row from the backdrop map: the map's tile at the edge, for its part past the screen)
	std::map<uint64_t, std::vector<uint8_t>> ext_how;   // (tests, VF3_EXT_MARK: how each was found, 1-4 the candidate, 5 near, 6 mirrored, 7 fill)
	// this frame's rows taken across their other end (a wrap not yet seen), by layer, side and scroll: the step out it
	// was taken at, and how many rows took it there
	std::map<uint64_t, std::map<int, int>> wrap_votes;
	// The layers below the 3D as the game has shown them: each pixel at its place along the panorama (the layer's
	// scroll unwrapped, line by line) and in the map's lines, for the sides to show where the screen has been (a
	// round's opening sweep shows most of a stage's sky). Forgotten when the screen stops agreeing with it (a cut,
	// another stage). R pixels along, the oldest overwritten.
	static constexpr int R = 2048;
	struct Seen {
		std::vector<int32_t> at_x, at_y;   // (R x 512: the place each pixel holds, INT32_MIN: none)
		std::vector<uint16_t> col;         // (its palette entry's colour, bit 15 transparent)
		// the screen's top left's place (the scroll registers have only the layer's move within a tile, the game
		// moving it further by rewriting the map: found each frame as the whole tiles that line the screen up with
		// what's remembered), the registers then, and the last frame's move in whole tiles
		int32_t x = 0, y = 0;
		int last_sx = -1, last_sy = 0, kx = 0, ky = 0;
		bool empty = true;
		bool unsure = false;   // (this frame's place not certain: nothing remembered from it)
		long compared = 0, differed = 0;   // (this frame's)
		void forget() { std::fill(at_x.begin(), at_x.end(), INT32_MIN); empty = true; kx = ky = 0; }
	};
	Seen seen[4];
public:
	long mem_drawn = 0, mem_sides = 0;
	long sky_rows = 0, sky_refinds = 0, sky_bad_rows = 0, sky_nomap_rows = 0;   // (tests: the backdrop's sides' rows, ever: from the map, the map found again for one, not as the screen even then, made without the map)   // (tests: the sides' pixels drawn from memory, of those drawn, ever)
private:
	// The game's own map of the stage's backdrop (layer 3: VF3 keeps each stage's whole panorama in its ROM, desc: its
	// width and height then an entry a tile, a 128-tile bank and the tile in it; the game writes the part around the
	// camera to the layer each frame, a bank's tiles where the table at 0x161614 says it loaded them): the sides drawn
	// from it, as the game would. x0, y0: where the layer's map starts in it (found each frame)
	// (seen: each bank's place as the layer on screen has it, -1 where none of it is on screen: the game moves banks
	// in the pattern RAM as it loads them, and its table can be ahead of the layer for a frame)
	struct Backdrop { bool ok = false; uint32_t desc = 0; int w = 0, h = 0, x0 = 0, y0 = 0; int16_t bank[128] = {}; int seen[128] = {}; bool known = false; int game_x = 0, game_y = 0, prev_x = 0, prev_y = 0; uint64_t game_frame = ~0ull; std::vector<uint16_t> map; };   // (map: the stage's entries, read from the ROM once a stage)   // (game_*: the game's place this frame, prev_*: the frame before's, in tiles)   // (known: the stage's map read this frame, its place found or not)
	Backdrop backdrop;
	uint64_t refind_frame = ~0ull;
	// (this frame: a palette's see-through colours, by its first colour's place / 16: 4-bit, a bit each; 8-bit, 256 bits)
	std::vector<uint64_t> clear_cols = std::vector<uint64_t>(4096 * 4);
	std::vector<uint8_t> clear_known = std::vector<uint8_t>(4096);   // (8-bit's from 2048)
	std::vector<uint8_t> solid_memo = std::vector<uint8_t>(1 << 17);   // (this frame: a pattern's entry and depth: 0 not looked at, 1 solid, 2 see-through)
	uint64_t full_failed_sum = 0;   // (the backdrop looked for in the whole map in vain: the screen's rows, the map and banks then)
	static uint64_t banks_sum(const Backdrop &b);
	std::unordered_map<uint64_t, uint64_t> row_failed;   // (a row's tiles, the map and banks: looked for in vain, and the frame)
	struct RowFound { uint64_t frame = ~0ull; int row = -1; uint64_t sum = 0; bool found = false; int x = 0, y = 0; std::vector<std::pair<int, int>> moved; } row_found;   // (a row looked for alone in the map: the other side's the same)
	bool cover2 = false;   // (this frame: layer 2 at the screen's edges for most of its lines, the mask's)
	void find_backdrop(const m3::Model3 &board, bool full = true);
	void refind_backdrop(const m3::Model3 &board, bool full);   // (a row's: not again while nothing it reads has changed)
	uint64_t backdrop_inputs(const m3::Model3 &board, bool full) const;
	uint64_t refind_failed = 0;   // (full: all the map looked through, else near the last place)
	uint32_t backdrop_entry(const m3::Model3 &board, int x, int y, bool table = false) const;   // (table: its bank where the game's table has it, not the screen)   // (the layer's entry, as the game writes it)
	std::map<uint64_t, bool> mem_mirror;   // (this frame's: by layer, map row and side, the row as made mirrored past what's remembered)
	void place_seen(const m3::Model3 &board);   // (each frame, before its lines)
	// the pattern after q along its row (d 1 or -1), wrapping where the row's known to
	int next_along(int layer, int q, int d) const;
	void learn(const m3::Model3 &board);
	// row b (len patterns) learned: the rows it overlaps forgotten
	void add_row(int layer, int b, int len);

};

} // namespace video
