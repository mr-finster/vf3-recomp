// Cheats > Dural's stage (issue #22): on the stage screen between two players (Arcade settings > Stage select:
// Challenger; online, Online > Settings > Stage select), the picking player confirms a stage with Guard (or Start, as
// chosen) and the match is on that stage's Dural version. Done through that player's inputs only, so online it's
// what the other side plays too (the input word is what netplay sends) and the other player needs no cheat.
//
// The game's own rule (the stage screen's routine, Revision D 0x2E220, Team Battle 0x32698; the same RAM in both,
// r13-relative): the pick is made when the picker presses a button (Punch, Kick, Guard, Escape) or the screen's timer
// (a word at 0x10200C: 703 down to 0, about 12 s) runs out. Then a byte (0x10637C, 0 at power-on, kept between
// matches) has the timer's seconds left (timer / 64, 10 down to 0) added to it; the stage is its Dural version (+16)
// if the picker holds Start at that moment and that byte is then a multiple of 17 (not stages 4 and 6, which have
// none). Team Battle's Start held otherwise gives a third version (+32); Revision D's the plain stage. So "Start
// held till the timer ends" works only while the byte's a multiple of 17 already (as at power-on), and a button
// pressed with Start held works in one second of the timer: the one that makes it 17's multiple (17 - byte % 17,
// when that's 10 or less; else no second can, by inputs).
//
// So with the cheat on, the trigger's press (seen new) holds that player's Start from then till the screen ends, and
// holds back the rest of their stick and buttons; Guard is then pressed for them in the middle of that second (now,
// or up to about 12 s later: the toast says). Offline, a pick no second can make Dural's has the byte set so the
// second now does (a write to the game's RAM: never online, where both sides must play the same game; there the
// trigger then does what the game does, and the toast says why).
#pragma once
#include "../machine/model3.h"
#include <algorithm>
#include <cstdint>

namespace dural_stage {

// The stage screen's RAM (both programs': the mode byte's profile, 0x102008, says it's one of them)
constexpr uint32_t MODE = 0x102008, SCREEN = 0x102010, TIMER = 0x10200C, COUNTER = 0x10637C, PICKER = 0x106092 /* 1: player 2 */,
                   PICKED = 0x10636A, CURSOR = 0x1020C7, HELD = 0x100080;
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
	enum Said { nothing, waiting, no_version, not_this_time };

	// The input word the game gets for player p (0, 1) this frame, from the player's own: `lead` frames before it
	// counts (online, the input delay), `may_write` (offline only) to set the byte for a pick no second could make.
	// `said`: what to tell the player (waiting: `wait_s` seconds till the pick)
	uint16_t filter(m3::Model3 &board, int p, uint16_t bits, int lead, bool may_write, Said &said, int &wait_s)
	{
		said = nothing;
		wait_s = 0;
		const uint16_t trig = trigger == 2 ? START : GUARD;
		const bool down = bits & trig, fresh = down && !was[p];
		was[p] = down;
		auto byte = [&](uint32_t a) { return board.ram.size() > a ? board.ram[a ^ 3] : 0; };
		const bool screen = trigger && board.profile->mode == MODE && byte(MODE) == 5 && (byte(SCREEN) == 19 || byte(SCREEN) == 20) &&
		                    (byte(PICKER) == 1) == (p == 1);   // (1 player 2, else player 1: the routine's test)
		if (!screen) { armed[p] = false; return bits; }
		if (byte(PICKED) != 0xFF) return armed[p] ? uint16_t(START) : bits;   // (picked: Start held till the screen ends)
		const int timer = (byte(TIMER) << 24 | byte(TIMER + 1) << 16 | byte(TIMER + 2) << 8 | byte(TIMER + 3)) - lead;
		auto second = [&] { return (17 - byte(COUNTER) % 17) % 17; };   // (the timer's second that makes it Dural's)
		if (!armed[p]) {
			if (!fresh) return bits;
			const int c = byte(CURSOR);
			if (c == 4 || c == 6) { said = no_version; return bits; }
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
		uint16_t out = START;
		if (byte(SCREEN) == 20 && timer <= second() * 64 + 32 && (byte(HELD + 3) & (0x10 << p))) out |= GUARD;
		return out;
	}

private:
	bool armed[2] = {}, was[2] = {};
};

} // namespace dural_stage
