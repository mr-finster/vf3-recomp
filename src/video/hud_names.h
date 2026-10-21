// The players' names under the energy bars, in a netplay match: each player's own name (up to 10 letters) where the
// fight's HUD has its fighter's. Drawn into the tile layers' picture as it's made, never into the board: the game's
// state is the same whatever the names, so they can't cause a desync, and a rollback can't leave one half drawn.
//
// The HUD (tile layer A, rows 6 and 7 of its map) has each fighter's name as a picture, 6 tiles by 2: player 1's
// from column 2, player 2's to column 59. Where both are there, unscrolled, each picture is cleared and the name
// drawn in its place, in a font of this project's own in the HUD's style (7 x 10, strokes of 3 with a black
// outline, white to blue): player 1's from the left of its picture, player 2's to the right of its.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace m3 { class Model3; }

namespace video {

// above: the layers above the 3D as TileGen::aligned gives them (W x H). False if the HUD wasn't there.
bool hud_names(const m3::Model3 &board, std::vector<uint32_t> &above, const std::string names[2]);

} // namespace video
