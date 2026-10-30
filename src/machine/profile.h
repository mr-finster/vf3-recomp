// What's particular to one Virtua Fighter 3 program, in one place: its ROMs, where the boot copies the program
// from, the FREE PLAY notices the PC way hides, where the game keeps its settings and the state the host reads (the
// select, the score, the test menu, the stages' list), the stages' backdrops, the recompiler's map, the data folder
// and the online game. Everything else (the board, the recompiler, the renderers, netplay) is the same for every
// program; it reads these from the board's profile. Revision D (MAME's and Supermodel's "vf3") is the only one so far.
#pragma once
#include <cstddef>
#include <cstdint>

namespace m3 {

// Where a ROM's bytes go, as the Model 3 sees them (romset.cpp lays them out)
enum RomRegion { ROM_CROM, ROM_BANKED, ROM_VROM, ROM_SOUND, ROM_SAMPLES, ROM_REGIONS };

struct RomFile
{
	const char *name;
	uint32_t crc;
	RomRegion region;
	uint32_t offset, stride, chunk;
	bool swap;
};

// Play as Dural (Model3::set_dural, src/machine/dural.cpp): the program's words its patches check and change, and what
// the host needs to draw her spot on the select (src/video/dural_cell.cpp)
struct DuralPatch
{
	const uint32_t (*known)[2];   // program words that must be as listed (the program image's addresses)
	size_t known_count;
	const uint32_t (*patch)[3];   // the patches: address, the word known there, the new word
	size_t patch_count;
	uint32_t ram_from, ram_to;    // RAM the patched code keeps its copies in (cleared when the patches go)
	// her picture, the FINAL MATCH screen's (the tile generator's pictures: a descriptor (in the fixed CROM) of chunk
	// count, width and height in tiles and the map, then chunks of palette and patterns in the banked CROM)
	uint32_t picture, descriptors, data, data_bank;
};

struct Profile
{
	const char *set;     // MAME's set: the zip's name ("vf3" for vf3.zip) and the online game
	const char *title;   // for the player ("Virtua Fighter 3 (Revision D)")
	const char *name;    // the window's and the menu's ("Virtua Fighter 3")
	const char *parent;  // a clone's: the set its other ROMs are in (a split zip takes them from that zip beside it)
	bool online;         // online play (needs the FREE PLAY patch, the select and the score: all known)
	const RomFile *roms;
	size_t rom_count;

	// The boot copies this much of the fixed CROM to RAM 0: the program
	uint32_t program_at, program_size;

	// The FREE PLAY notices (Model3::hide_free_play_text), as the program image's addresses: the fight's top line
	// ("     FREE PLAY       ", 21 characters, between "PRESS START BUTTON   " and "CREDIT"), and the attract's
	// picture (its number in a table) and the call that draws it (a no-op instead)
	struct FreePlayText
	{
		uint32_t press_start, free_play, credit;
		uint32_t picture_table, picture;
		uint32_t draw_call, draw_word;
	} free_play_text;

	// Backup RAM (offsets into Model3::backup, as the code has always used them)
	struct Coin { uint32_t setting, credits, free_play; } coin;   // (Model3::coin_setting)
	// The test menu's GAME ASSIGNMENTS (main.cpp's Arcade settings), a byte each from `at` on, the energies 32-bit
	struct Settings
	{
		uint32_t at;
		int rounds_cpu, rounds_vs, difficulty, demo_sound, energy_cpu, energy_vs, stage_select, country;
	} settings;

	// RAM (offsets into Model3::ram, the game's own addresses)
	int boot_frames;          // the boot, up to the SEGA logo
	uint32_t test_menu;       // a byte: 4 while the arcade's test menu is up
	uint32_t versus_count;    // a word: the matches between two players since power-on (vf3_netplay.h)
	uint32_t scene;           // a byte: 5 on the character select (vf3_netplay.h)
	uint32_t rounds_p1, rounds_p2, playing;   // the score's bytes (vf3_netplay.h's Score)
	uint32_t select_cursor;   // a byte: player 1's column on the character select (player 2's 0x80 on; the row the byte before)
	uint32_t mode;            // a byte: the game's mode, 5 from Start through the select (the stage select too)

	// The stages' backdrops (tilegen.cpp): the stage's number at r13 minus this, the backdrops' descriptors (a word
	// per stage), the bank table (128 halfwords) and the backdrop's place (+4, +8)
	struct Backdrops { uint32_t stage_below_r13, descriptors, banks, place; } backdrops;

	// The recompiler's address map (src/rec: entry points from the coverage runs)
	const uint32_t *map;
	size_t map_count;

	// The player's data (backup, eeprom) in this folder under the data folder ("": the data folder itself)
	const char *data_folder;

	const DuralPatch *dural;   // Play as Dural's (none: not known for this program)

	// RAM: the seed of the random numbers the game draws from (a word; 0: not known): Team Battle's CPU teams and
	// their stages, the attract's fights (Revision D's 1P order is its own, the same whatever the seed: checked). The
	// boot leaves it the same every time (Revision D 0, Team Battle a constant at frame 789), so offline, the PC way
	// skipping the boot, the same teams came but for when Start was pressed: the host sets it at random after the
	// boot (an arcade on all day had it anywhere)
	uint32_t random_seed;
};

extern const Profile VF3_REV_D, VF3_TB;
extern const DuralPatch REV_D_DURAL, TB_DURAL;
extern const Profile *const PROFILES[];   // every program known, Revision D first; nullptr after the last
const Profile *find_profile(const char *set);   // by MAME's set name ("vf3", "vf3tb"); nullptr if none

} // namespace m3
