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
};

// above: the layers above the 3D as TileGen::aligned gives them (W x H). Draws her cell if the select's grid is on
// screen (tile layer B's map: the grid's panel, rows 34-47, columns 0-61); false if it isn't, or no picture.
bool dural_cell(const m3::Model3 &board, std::vector<uint32_t> &above, DuralPicture &pic);

// (tests: the picture alone)
bool dural_picture(const m3::Model3 &board, DuralPicture &pic);

} // namespace video
