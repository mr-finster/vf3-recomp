// The programs' profiles (profile.h)
#include "profile.h"
#include "../rec/ppc_rec.h"

namespace m3 {

namespace {

// Revision D's 39 ROMs (MAME's vf3), each checked by CRC: the 4 program ROMs (epr-19227d to 19230d), banked CROM,
// the Real3D's textures, the sound board's program and its samples
const RomFile REV_D_ROMS[] = {
	{ "epr-19230d.20", 0x43C08240, ROM_CROM, 0, 8, 2, true },
	{ "epr-19229d.19", 0x6773F715, ROM_CROM, 2, 8, 2, true },
	{ "epr-19228d.18", 0xA2470C78, ROM_CROM, 4, 8, 2, true },
	{ "epr-19227d.17", 0x8B650966, ROM_CROM, 6, 8, 2, true },
	{ "mpr-19196.4", 0xF386B850, ROM_BANKED, 0x0000000, 8, 2, true },
	{ "mpr-19195.3", 0xBD5E27A3, ROM_BANKED, 0x0000002, 8, 2, true },
	{ "mpr-19194.2", 0x66254702, ROM_BANKED, 0x0000004, 8, 2, true },
	{ "mpr-19193.1", 0x7BAB33D2, ROM_BANKED, 0x0000006, 8, 2, true },
	{ "mpr-19200.8", 0x74941091, ROM_BANKED, 0x1000000, 8, 2, true },
	{ "mpr-19199.7", 0x9F80D6FE, ROM_BANKED, 0x1000002, 8, 2, true },
	{ "mpr-19198.6", 0xD8EE5032, ROM_BANKED, 0x1000004, 8, 2, true },
	{ "mpr-19197.5", 0xA22D76C9, ROM_BANKED, 0x1000006, 8, 2, true },
	{ "mpr-19204.12", 0x2F93310A, ROM_BANKED, 0x2000000, 8, 2, true },
	{ "mpr-19203.11", 0x0AFA6334, ROM_BANKED, 0x2000002, 8, 2, true },
	{ "mpr-19202.10", 0xAAA086C6, ROM_BANKED, 0x2000004, 8, 2, true },
	{ "mpr-19201.9", 0x7C4A8C31, ROM_BANKED, 0x2000006, 8, 2, true },
	{ "mpr-19208.16", 0x08F30F71, ROM_BANKED, 0x3000000, 8, 2, true },
	{ "mpr-19207.15", 0x2CE1612D, ROM_BANKED, 0x3000002, 8, 2, true },
	{ "mpr-19206.14", 0x71A98D73, ROM_BANKED, 0x3000004, 8, 2, true },
	{ "mpr-19205.13", 0x199C328E, ROM_BANKED, 0x3000006, 8, 2, true },
	{ "mpr-19211.26", 0x9C8F5DF1, ROM_VROM, 0, 32, 2, false },
	{ "mpr-19212.27", 0x75036234, ROM_VROM, 2, 32, 2, false },
	{ "mpr-19213.28", 0x67B123CF, ROM_VROM, 4, 32, 2, false },
	{ "mpr-19214.29", 0xA6F5576B, ROM_VROM, 6, 32, 2, false },
	{ "mpr-19215.30", 0xC6FD9F0D, ROM_VROM, 8, 32, 2, false },
	{ "mpr-19216.31", 0x201BB1ED, ROM_VROM, 10, 32, 2, false },
	{ "mpr-19217.32", 0x4DADD41A, ROM_VROM, 12, 32, 2, false },
	{ "mpr-19218.33", 0xCFF91953, ROM_VROM, 14, 32, 2, false },
	{ "mpr-19219.34", 0xC610D521, ROM_VROM, 16, 32, 2, false },
	{ "mpr-19220.35", 0xE62924D0, ROM_VROM, 18, 32, 2, false },
	{ "mpr-19221.36", 0x24F83E3C, ROM_VROM, 20, 32, 2, false },
	{ "mpr-19222.37", 0x61A6AA7D, ROM_VROM, 22, 32, 2, false },
	{ "mpr-19223.38", 0x1A8C1980, ROM_VROM, 24, 32, 2, false },
	{ "mpr-19224.39", 0x0A79A1BD, ROM_VROM, 26, 32, 2, false },
	{ "mpr-19225.40", 0x91A985EB, ROM_VROM, 28, 32, 2, false },
	{ "mpr-19226.41", 0x00091722, ROM_VROM, 30, 32, 2, false },
	{ "epr-19231.21", 0xB416FE96, ROM_SOUND, 0, 2, 2, false },
	{ "mpr-19209.22", 0x3715E38C, ROM_SAMPLES, 0x000000, 2, 2, false },
	{ "mpr-19210.24", 0xC03D6502, ROM_SAMPLES, 0x400000, 2, 2, false },
};

} // namespace

const Profile VF3_REV_D = {
	"vf3", "Virtua Fighter 3 (Revision D)", REV_D_ROMS, sizeof REV_D_ROMS / sizeof REV_D_ROMS[0],
	// the boot copy (docs/step1-measure.md)
	0x710000, 0xA0000,
	// FREE PLAY: the fight's line at 0x7CAE0; the attract's picture 0x4018 (table 0x7CB50), drawn by bl 0x33300 at 0x4E9C0
	{ 0x7CACA, 0x7CAE0, 0x7CAF6, 0x7CB50, 0x4018, 0x4E9C0, 0x4BFE4941 },
	// the coin setting (found by setting each in the test menu)
	{ 0x1E033, 0x1E037, 0x1E03B },
	// GAME ASSIGNMENTS: rounds vs CPU 2, vs player 3, difficulty 6, demo sound 7, energies 8 and 12, stage select
	// 0x17, country 0x1E
	{ 0x1E000, 2, 3, 6, 7, 8, 12, 0x17, 0x1E },
	960, 0x102002, 0x106080, 0xE4046, 0x10601A, 0x10601B, 0x106029,
	{ 0x1E0C, 0xFFB30004, 0x161614, 0x1617B4 },
	ppcrec::vf3_map, ppcrec::vf3_map_count,
	"",
};

} // namespace m3
