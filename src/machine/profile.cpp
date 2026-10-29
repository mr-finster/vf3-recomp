// The programs' profiles (profile.h)
#include "profile.h"
#include "../rec/ppc_rec.h"
#include <cstring>

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

// Team Battle's (MAME's vf3tb, a clone of vf3): its own program (epr-20126 to 20129) and first 16 MB of banked CROM
// (mpr-20130 to 20133), placed as Revision D's; the rest Revision D's
const RomFile TB_ROMS[] = {
	{ "epr-20129.20", 0x0DB897CE, ROM_CROM, 0, 8, 2, true },
	{ "epr-20128.19", 0xFFBDBDC5, ROM_CROM, 2, 8, 2, true },
	{ "epr-20127.18", 0x5C0F694B, ROM_CROM, 4, 8, 2, true },
	{ "epr-20126.17", 0x27ECD3B0, ROM_CROM, 6, 8, 2, true },
	{ "mpr-20133.4", 0x3D9B5171, ROM_BANKED, 0x0000000, 8, 2, true },
	{ "mpr-20132.3", 0xF7557474, ROM_BANKED, 0x0000002, 8, 2, true },
	{ "mpr-20131.2", 0x51FA69F1, ROM_BANKED, 0x0000004, 8, 2, true },
	{ "mpr-20130.1", 0x40640446, ROM_BANKED, 0x0000006, 8, 2, true },
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
	"vf3", "Virtua Fighter 3 (Revision D)", "Virtua Fighter 3", nullptr, true, REV_D_ROMS, sizeof REV_D_ROMS / sizeof REV_D_ROMS[0],
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
	0x108045,   // (found by choosing each fighter and comparing; 0-12 the select's, 13 Dural: issue #12)
	// (1 the boot, 3 the attract, 4 Start pressed, 5 the select, a challenger's and the stage select, 6-7 the VS screen
	// and the fight: logged through a 1P run and a challenger's; the scene byte above is 4 on player 2's and a
	// challenger's select, so it's no use there)
	0x102008,
	{ 0x1E0C, 0xFFB30004, 0x161614, 0x1617B4 },
	ppcrec::vf3_map, ppcrec::vf3_map_count,
	"",
};

// Team Battle: its ROMs, and the addresses found so far; what isn't known yet is 0 (that feature off: the select's
// byte, so no online yet).
const Profile VF3_TB = {
	"vf3tb", "Virtua Fighter 3 Team Battle", "Virtua Fighter 3 Team Battle", "vf3", false, TB_ROMS, sizeof TB_ROMS / sizeof TB_ROMS[0],
	0x710000, 0xA0000,
	// FREE PLAY: Revision D's text 0x61F0 on (0x82CD0), its picture's table 0x82D50, drawn by bl 0x37FFC at 0x5419C (the
	// same code as Revision D's 0x4E990-0x4E9C0, with one more argument): found by matching Revision D's code
	{ 0x82CBA, 0x82CD0, 0x82CE6, 0x82D50, 0x4018, 0x5419C, 0x4BFE3E61 },
	// the coin setting: Revision D's (FREE PLAY there shows FREE PLAY and Start needs no credit: checked)
	{ 0x1E033, 0x1E037, 0x1E03B },
	// GAME ASSIGNMENTS: Revision D's places (its test menu shows a change there: MATCH COUNT(1P)), a marker 0x2E31 in
	// the halves Revision D leaves 0; it has more (drink, naked, VS finish, default mode, energy recovery) not here yet.
	// Its STAGE SELECT: 0 CHALLENGER (Revision D's on)
	{ 0x1E000, 2, 3, 6, 7, 8, 12, 0x17, 0x1E },
	// the test menu's byte (4 while it's up: checked), the versus count and the score: Revision D's (the same code
	// refers to them, r13-relative: tools found every reference unchanged); the select byte not found yet (only
	// online needs it: 0x1001B3 is 0, 5, 1 in the attract, the select and a fight)
	960, 0x102002, 0x106080, 0, 0x10601A, 0x10601B, 0x106029,
	0,   // (the select's cursor: not looked for yet, so no Play as Dural)
	0x102008,   // (3, 5, 7 in the attract, the select and a fight, as Revision D's)
	// the backdrops: Revision D's (a fight's sky found with no tile on screen wrong, as Revision D's: checked on the dojo)
	{ 0x1E0C, 0xFFB30004, 0x161614, 0x1617B4 },
	ppcrec::vf3tb_map, ppcrec::vf3tb_map_count,
	"vf3tb",
};

const Profile *const PROFILES[] = { &VF3_REV_D, &VF3_TB, nullptr };

const Profile *find_profile(const char *set)
{
	for (const Profile *const *p = PROFILES; *p; p++)
		if (set && strcmp((*p)->set, set) == 0) return *p;
	return nullptr;
}

} // namespace m3
