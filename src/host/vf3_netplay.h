// VF3 in a netplay session (src/net): the board as the session's Game. What both sides start from (the host's
// backup RAM and EEPROM, so the same arcade settings and rankings), how they start (the PC way on both: free play,
// the FREE PLAY notices hidden, the boot run unseen), each player's switches as an input word, and the MIDI
// bytes for the sound board under rollback.
#pragma once
#include "../machine/model3.h"
#include "../net/session.h"
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace vf3net {

constexpr int BOOT_FRAMES = 960;   // the boot, unseen, up to the SEGA logo (as the PC way's, main.cpp)
constexpr size_t STAGE_SELECT = 0x1E017;   // the arcade's VS stage select in its backup RAM: 0 on, 1 off (found by changing it)
// The game's count of the matches between two players since it was switched on (a word of RAM, r13 - 0x1f80): each
// one's stage is the next of a list of 16 in its program (0x76fd6, at (count + 1) % 16), the stage select off. An
// arcade's ran all day, so a challenge's stage was anyone's guess; a game just booted has 0, and its first match
// between two players is always on the same stage (the lava's). Set after the boot: where in the list it starts.
constexpr size_t VERSUS_COUNT = 0x106080;
void set_versus_count(m3::Model3 &board, uint32_t count);
uint32_t random_versus_count();   // (0 to 15, a fresh one each call)

// A player's switches as the session's input word (coin, start, the stick, the four buttons; never test or
// service), and back
uint16_t pack(const m3::Controls &c, int player);
void unpack(uint16_t bits, int player, m3::Controls &c);

// A hash of the ROM's contents as loaded (the FREE PLAY notices as they are in the ROM, hidden or not)
uint64_t content_hash(m3::Model3 &board);
// The host's starting blob: its backup RAM and EEPROM, and where the stages' list starts (random_versus_count)
std::vector<uint8_t> start_blob(const m3::Model3 &board);
constexpr uint16_t START_BUTTON = 256;   // (the input word's Start: pack's)

// The score between the two players, read from the game's own RAM after each frame (as Fightcade's detectors do,
// tools/vf3detect.cpp found where): each player's rounds won in the match on now (0x10601a, 0x10601b: both back
// to 0 when it's over), and which players are playing (0x106029: bit 0 player 1, bit 1 player 2; a side not
// playing is the CPU's). A match counts only if both were playing at each round it was won (not the winner's
// fights against the CPU after it, nor one a challenger broke into); each reading only once it's held 30 frames,
// longer than any rollback, so a guessed frame never scores.
struct Score
{
	int matches[2] = {}, rounds[2] = {};   // between the two players: matches won, and rounds won in them
	void frame(const m3::Model3 &board);
	int players() const { return playing; }
	int round_ends = 0;   // (each round's end, any players': a moment no one's in control, ~2 s while the game shows who won)   // (who's in the game, as held: bit 0 player 1, bit 1 player 2)
	static constexpr uint32_t ROUNDS_P1 = 0x10601a, ROUNDS_P2 = 0x10601b, PLAYING = 0x106029;
private:
	uint8_t seen[2] = {}, now[3] = {}, playing = 0;
	int held = 0;
	bool versus = false;   // (the match on now: both playing at every round so far)
};

class Adapter : public vfnet::Game
{
public:
	explicit Adapter(m3::Model3 &board) : board(board) {}

	// Set by the host program:
	// one frame (the controls are set): resim, a frame run again (no picture, no sound needed). Default:
	// board.run_frame(). Whatever takes the frame's MIDI bytes for the sound board calls take_midi (else the
	// adapter does after the frame, and gives them to midi_out).
	std::function<void(bool resim)> run;
	std::function<void()> program_changed;     // the CROM changed (the recompiled code must be made again)
	std::function<void()> settle;              // before a save or load: anything running alongside the board waits
	std::function<void(const std::vector<uint8_t> &)> midi_out;   // bytes for the sound board
	std::function<void()> on_begin;            // a match's start, before the board is set up for it (keep the player's NVRAM)
	std::function<void()> booting;             // now and then in the match's boot (seconds of frames: the window's events)

	bool begin(const std::vector<uint8_t> &blob) override;
	void save(std::vector<uint8_t> &state) override;
	bool load(const std::vector<uint8_t> &state) override;
	void advance(const uint16_t input[2], bool resim) override;
	// (the state's hash without its header: that's this board's own bookkeeping, not the game's)
	uint64_t hash(const std::vector<uint8_t> &state);
	size_t hash_skip() const override { return m3::Model3::STATE_HEADER; }
	// The frame's MIDI bytes to send now (board.midi emptied): all of them the first time a frame runs; run again
	// after a rollback, none if its bytes went already (the sound board can't take back what it played: a wrong
	// guess's sound stays), and if it sent none and has some now, those (late)
	void take_midi(std::vector<uint8_t> &out);

	// tests: the state's hash after each frame a multiple of this, the last time it ran (0: none kept)
	uint32_t checkpoint_every = 0;
	std::map<uint32_t, uint64_t> checkpoints;

private:
	m3::Model3 &board;
	uint32_t frame = 0;   // the session's frame the board is at
	// per frame: the MIDI bytes the sound board was sent for it (a frame run again sends only what wasn't)
	std::map<uint32_t, std::vector<uint8_t>> midi_sent;
	std::vector<uint8_t> scratch, check_buf;
	bool midi_taken = false;
};

} // namespace vf3net
