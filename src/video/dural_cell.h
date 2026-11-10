// Dural on the select (Model3::set_dural): her spot's picture, a 13th cell past Kage. The grid's cells are the
// game's own pictures, and she has none; the one picture of her is the FINAL MATCH screen's (the tile generator's
// picture 0x4C: Dural, 496 x 384), decoded here from the ROM as the game's loader does (0x32C50 and its unpacker
// 0x32DB0) and drawn small into the layers above the 3D, where the select's grid has the seventh column. Only the
// picture, never the board: a save state or a rollback can't differ by it.
#pragma once
#include <cstdint>
#include <vector>

namespace m3 { class Model3; }

namespace video {

struct DuralPicture
{
	int w = 0, h = 0;               // her picture (0: not decoded yet, -1: couldn't be)
	std::vector<uint32_t> px;       // 0xAABBGGRR, alpha 0 where see-through
	std::vector<uint32_t> cell;     // the cell's picture (CELL_W x CELL_H), made from px once
	std::vector<uint32_t> mark2;    // player 2's marker as the game draws it (kept when seen on another cell)
};

// Her cell and what's round it, in the aligned picture's pixels: x 461-533, y 270-365 (past the 4:3 picture's 496)
constexpr int WIDE_X = 461, WIDE_Y = 270, WIDE_W = 73, WIDE_H = 96;
struct DuralWide
{
	bool on = false;                // (drawn this frame)
	std::vector<uint32_t> px;       // WIDE_W x WIDE_H, 0xAABBGGRR: the cell whole, for a picture wider than 4:3
};

// above: the layers above the 3D as TileGen::aligned gives them (W x H). Draws her cell if the select's grid is on
// screen (tile layer B's map: the grid's panel, rows 34-47, columns 0-61), as far as the 4:3 picture goes; wide: all
// of it (the cell, and the cursor's frame when the game's is on her), for widescreen's right side. False if it isn't
// there, or no picture.
class TileGen;
// tiles: the generator that drew `above` (each line's registers as it drew it: her cell's lines shown as the grid's)
bool dural_cell(const m3::Model3 &board, const TileGen &tiles, std::vector<uint32_t> &above, DuralPicture &pic, DuralWide *wide = nullptr);

// (tests: the picture alone)
bool dural_picture(const m3::Model3 &board, DuralPicture &pic);

// A tile generator picture's data in the ROM, as the game's loader reads it (0x32F90 and 0x32C50: the same in
// Revision D and Team Battle): picture n's descriptor at descriptors + 4n (in the fixed CROM: 1, chunks, width and
// height in tiles, then the map, a halfword a tile: its chunk in bits 7-13, its pattern in the chunk 0-6) and its
// data at data + 4n (in the banked CROM, bank `bank`): each chunk's palette (a count, then that many 15-bit colours)
// and its patterns (a count of 8 x 8 tiles, 8 bits a pixel; bit 15: packed, then its size and the packed bytes, the
// unpacker 0x32DB0). The game loads a chunk into a 128-tile slot of the pattern RAM and its colours into the slot's
// palette; the stages' backdrops are such pictures, the chunks loaded as the camera needs them.
struct TilePictureData
{
	int chunks = 0, tw = 0, th = 0;
	uint32_t desc = 0;                         // (the descriptor's address: the map from desc + 8)
	std::vector<std::vector<uint16_t>> pal;    // each chunk's colours
	std::vector<std::vector<uint8_t>> pat;     // each chunk's patterns: 64 bytes a tile
};
bool tile_picture_data(const m3::Model3 &board, int bank, uint32_t descriptors, uint32_t data, int picture, TilePictureData &out);

} // namespace video
