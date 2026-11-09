// Cheats > Dural's stage (issue #22): on the stage screen between two players (Arcade settings > Stage select:
// Challenger; online, Online > Settings > Stage select), the picking player confirms a stage with Guard and the match
// is on that stage's Dural version. Done through that player's inputs only, so online it's what the other side plays
// too (the input word is what netplay sends) and the other player needs no cheat.
//
// The game's own rule (the stage screen's routine, Revision D 0x2E220, Team Battle 0x32698; the same RAM in both,
// r13-relative): the cursor's row (0x1020C5, 0-1) and column (0x1020C6; 6 stages in the top row, 7 below, the column
// wrapping) give the stage's number from a table (Revision D 0x70418, Team Battle 0x76CF0, the same 13 bytes: 14 9 12
// 4 1 5 / 2 7 11 8 6 15 13), kept at 0x1020C7. The pick is made when the picker presses a button (Punch, Kick, Guard,
// Escape) or the screen's timer (a word at 0x10200C: 703 down to 0, about 12 s) runs out. Then a byte (0x10637C, 0 at
// power-on, kept between matches) has the timer's seconds left (timer / 64, 10 down to 0) added to it, and:
// - Revision D (0x2E33C-0x2E390): the stage's Dural version (+16) if that byte is then a multiple of 17 and the picker
//   holds Start, except stages 4 (the cave) and 6 (Aoi's river), by their number (cmpwi r4,6 / cmpwi cr1,r4,4);
//   else the plain stage.
// - Team Battle (0x327B4-0x32810): Start not held, the plain stage; held, the Dural version if the byte's a multiple
//   of 17 and the stage isn't 4 or 6, else a third version (+32, the stage as VF3 had it).
// So "Start held till the timer ends" works only while the byte's a multiple of 17 already (as at power-on), and a
// button pressed with Start held works in one second of the timer: the one that makes it 17's multiple (17 - byte % 17,
// when that's 10 or less; else no second can, by inputs).
// The versions themselves (each stage's record: Revision D 0x6E400 + 256 a stage, Team Battle 0x760C0 + 64; its
// backdrop at +0x34): Revision D's 20 and 21 are 4's and 5's records but for their names (DCAVE, DEDOMURA), and look
// the same as them; 22, Aoi's river at night, is there but the routine never picks it. Team Battle gave its plain cave
// and Kage's castle a new sky (a pale one with a skyline, a blue one), and kept VF3's darker cave and sunset as their
// +16 and +32 versions (20 and 36 the same picture, 21 and 37 too). So the cave's Dural look in Team Battle is its VF3
// version, which Start held gives whatever the byte: the cheat holds Start and picks at once there (online too).
//
// With the cheat on, the trigger's press (seen new) holds that player's Start from then till the screen ends, and
// holds back the rest of their stick and buttons; Guard is then pressed for them in the middle of that second (now,
// or up to about 12 s later: the toast says). Offline, a pick no second can make Dural's has the byte set so the
// second now does (a write to the game's RAM: never online, where both sides must play the same game; there the
// trigger then does what the game does, and the toast says why). Which stage is picked is the one the game will see
// when the trigger's press reaches it: the stick's presses still on their way (online, the input delay's) moved first.
// Aoi's river (both programs) and Revision D's cave it leaves to the game (the plain stage), and the toast says the
// stage screen never gives their Dural version (issue #36).
#pragma once
#include "../machine/model3.h"
#include <algorithm>
#include <cstdint>

namespace dural_stage {

// The stage screen's RAM (both programs': the mode byte's profile, 0x102008, says it's one of them)
constexpr uint32_t MODE = 0x102008, SCREEN = 0x102010, TIMER = 0x10200C, COUNTER = 0x10637C, PICKER = 0x106092 /* 1: player 2 */,
                   PICKED = 0x10636A, ROW = 0x1020C5, COLUMN = 0x1020C6, STAGE = 0x1020C7 /* the cursor's stage */, HELD = 0x100080;
// The stage screen's stages by row and column (the table above)
constexpr uint8_t STAGES[2][7] = { { 14, 9, 12, 4, 1, 5 }, { 2, 7, 11, 8, 6, 15, 13 } };
constexpr int CAVE = 4, AOIS = 6;
// The input word's bits (vf3net::pack's)
enum : uint16_t { PUNCH = 16, KICK = 32, GUARD = 64, ESCAPE = 128, START = 256, COIN = 512 };

// A player's switches as that word, and back (test and service untouched)
inline uint16_t word(const m3::Controls &c, int p)
{
	return uint16_t((c.up[p] ? 1 : 0) | (c.down[p] ? 2 : 0) | (c.left[p] ? 4 : 0) | (c.right[p] ? 8 : 0) | (c.punch[p] ? PUNCH : 0) |
	                (c.kick[p] ? KICK : 0) | (c.guard[p] ? GUARD : 0) | (c.escape[p] ? ESCAPE : 0) | (c.start[p] ? START : 0) | (c.coin[p] ? COIN : 0));
}
inline void set(m3::Controls &c, int p, uint16_t b)
{
	c.up[p] = b & 1; c.down[p] = b & 2; c.left[p] = b & 4; c.right[p] = b & 8; c.punch[p] = b & PUNCH; c.kick[p] = b & KICK;
	c.guard[p] = b & GUARD; c.escape[p] = b & ESCAPE; c.start[p] = b & START; c.coin[p] = b & COIN;
}

struct Picker
{
	int trigger = 0;   // 0 off, 1 Guard, 2 Start
	enum Said { nothing, waiting, no_version, not_this_time, vf3_version };

	// The input word the game gets for player p (0, 1) this frame, from the player's own: `lead` frames before it
	// counts (online, the input delay), `may_write` (offline only) to set the byte for a pick no second could make.
	// `said`: what to tell the player (waiting: `wait_s` seconds till the pick; vf3_version: Team Battle's cave)
	uint16_t filter(m3::Model3 &board, int p, uint16_t bits, int lead, bool may_write, Said &said, int &wait_s)
	{
		const uint16_t out = pick(board, p, bits, lead, may_write, said, wait_s);
		sent[p][at[p]] = out;   // (the words on their way, for picked())
		at[p] = (at[p] + 1) % HISTORY;
		return out;
	}

private:
	uint16_t pick(m3::Model3 &board, int p, uint16_t bits, int lead, bool may_write, Said &said, int &wait_s)
	{
		said = nothing;
		wait_s = 0;
		const uint16_t trig = trigger == 2 ? START : GUARD;
		const bool down = bits & trig, fresh = down && !was[p];
		was[p] = down;
		auto byte = [&](uint32_t a) { return board.ram.size() > a ? board.ram[a ^ 3] : 0; };
		const bool screen = trigger && board.profile->mode == MODE && byte(MODE) == 5 && (byte(SCREEN) == 19 || byte(SCREEN) == 20) &&
		                    (byte(PICKER) == 1) == (p == 1);   // (1 player 2, else player 1: the routine's test)
		if (!screen) { armed[p] = now[p] = false; return bits; }
		if (byte(PICKED) != 0xFF) return armed[p] ? uint16_t(START) : bits;   // (picked: Start held till the screen ends)
		const int timer = (byte(TIMER) << 24 | byte(TIMER + 1) << 16 | byte(TIMER + 2) << 8 | byte(TIMER + 3)) - lead;
		auto second = [&] { return (17 - byte(COUNTER) % 17) % 17; };   // (the timer's second that makes it Dural's)
		if (!armed[p]) {
			if (!fresh) return bits;
			const int c = picked(board, p, bits, lead);
			if (c == CAVE && board.profile == &m3::VF3_TB) {   // (its VF3 version: Start held is enough)
				armed[p] = now[p] = true;
				said = vf3_version;
				return START;
			}
			if (c == CAVE || c == AOIS) { said = no_version; return bits; }
			if (second() && timer - 6 < second() * 64) {   // (that second's past, or none is: 11-16; the last, 0, the timer's end makes too)
				if (!may_write) { said = not_this_time; return bits; }
				const int s = timer >= 72 ? (timer - 8) >> 6 : 0;   // (a second the timer's in for a while yet)
				int n = byte(COUNTER) + (17 - (byte(COUNTER) + s) % 17) % 17;
				if (n > 255) n -= 17;
				board.ram[COUNTER ^ 3] = uint8_t(n);
			}
			armed[p] = true;
			said = waiting;
			wait_s = std::max(0, timer - (second() * 64 + 32)) / 58;
		}
		// Start held, and once the game has it, Guard pressed in the middle of the second (or as near it as it gets)
		// (Team Battle's cave: at once)
		uint16_t out = START;
		if (byte(SCREEN) == 20 && (now[p] || timer <= second() * 64 + 32) && (byte(HELD + 3) & (0x10 << p))) out |= GUARD;
		return out;
	}

	// The stage the game will pick when this frame's word reaches it, `lead` frames on: the cursor moved by the words
	// sent before it that it hasn't run yet, then this one, as the routine moves it (Revision D 0x2E25C-0x2E324, Team
	// Battle 0x326D4-0x3279C): a new right or left press moves the column (not with a new up or down), wrapping in the
	// row; a new down or up the row, kept to 0-1, and the column on with right or left held
	int picked(m3::Model3 &board, int p, uint16_t bits, int lead) const
	{
		auto byte = [&](uint32_t a) { return board.ram.size() > a ? board.ram[a ^ 3] : 0; };
		int row = byte(ROW), col = byte(COLUMN);
		if (row > 1 || col > row + 5) return byte(STAGE);
		for (int k = std::clamp(lead, 0, int(HISTORY) - 2); k >= 0; k--) {
			const uint16_t w = k ? sent[p][(at[p] + HISTORY - k) % HISTORY] : bits, fresh = w & ~sent[p][(at[p] + HISTORY - k - 1) % HISTORY];
			int dr = 0, dc = 0;
			if ((fresh & 8) && !(fresh & 1)) dc = 1;    // (right)
			if ((fresh & 4) && !(fresh & 2)) dc = -1;   // (left)
			if (fresh & 2) { if (w & 8) dc = 1; dr = 1; }    // (down)
			if (fresh & 1) { if (w & 4) dc = -1; dr = -1; }  // (up)
			row = std::clamp(row + dr, 0, 1);
			col += dc;
			if (col < 0) col = row + 5;
			if (col > row + 5) col = 0;
		}
		return STAGES[row][col];
	}

	static constexpr int HISTORY = 32;
	uint16_t sent[2][HISTORY] = {};   // (each player's last words, as sent: at[p] the next's place)
	int at[2] = {};
	bool armed[2] = {}, now[2] = {}, was[2] = {};
};

} // namespace dural_stage
