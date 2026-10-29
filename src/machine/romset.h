// The player's own Virtua Fighter 3 set (a profile's: Revision D, MAME's and Supermodel's "vf3", or Team Battle's
// "vf3tb"), read straight from the zip: every ROM checked by CRC and laid out as the Model 3 sees it.
#pragma once
#include "profile.h"
#include <cstdint>
#include <string>
#include <vector>

namespace romset {

struct result
{
	bool ok = false;
	std::string error;               // what's missing or wrong, for the player
	std::vector<uint8_t> crom;       // fixed CROM, 8 MB at 0xFF800000: the program ROMs at the top, banked CROM 0 below
	std::vector<uint8_t> banked;     // banked CROM, 128 MB (64 MB of ROMs, mirrored)
	std::vector<uint8_t> vrom;       // the Real3D's texture ROMs, 64 MB (32 MB, mirrored)
	std::vector<uint8_t> sound;      // the sound board's 68000 program, 512 KB
	std::vector<uint8_t> samples;    // the SCSPs' samples, 16 MB (8 MB, mirrored)
};

// The CROM regions come back as host-order 32-bit words (as the CPU core reads them); the others as the bytes
// of the big-endian words.
result load(const std::string &zip, const m3::Profile &profile = m3::VF3_REV_D);
// The programs a zip holds whole (every ROM's CRC in its directory, or a clone's in its parent's zip beside it),
// from the zips' directories only: load() still checks each ROM's data
std::vector<const m3::Profile *> sets_in(const std::string &zip);

} // namespace romset
