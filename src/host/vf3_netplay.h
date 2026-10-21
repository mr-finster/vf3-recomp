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

// A player's switches as the session's input word (coin, start, the stick, the four buttons; never test or
// service), and back
uint16_t pack(const m3::Controls &c, int player);
void unpack(uint16_t bits, int player, m3::Controls &c);

// A hash of the ROM's contents as loaded (the FREE PLAY notices as they are in the ROM, hidden or not)
uint64_t content_hash(m3::Model3 &board);
// The host's starting blob: its backup RAM and EEPROM
std::vector<uint8_t> start_blob(const m3::Model3 &board);

class Adapter : public vfnet::Game
{
public:
	explicit Adapter(m3::Model3 &board) : board(board) {}

	// Set by the host program:
	// one frame (the controls are set): resim, a frame run again (no picture, no sound needed). Leaves board.midi
	// for the adapter. Default: board.run_frame().
	std::function<void(bool resim)> run;
	std::function<void()> program_changed;     // the CROM changed (the recompiled code must be made again)
	std::function<void()> settle;              // before a save or load: anything running alongside the board waits
	std::function<void(const std::vector<uint8_t> &)> midi_out;   // bytes for the sound board

	bool begin(const std::vector<uint8_t> &blob) override;
	void save(std::vector<uint8_t> &state) override;
	bool load(const std::vector<uint8_t> &state) override;
	void advance(const uint16_t input[2], bool resim) override;

	// tests: the state's hash after each frame a multiple of this, the last time it ran (0: none kept)
	uint32_t checkpoint_every = 0;
	std::map<uint32_t, uint64_t> checkpoints;

private:
	m3::Model3 &board;
	uint32_t frame = 0;   // the session's frame the board is at
	// per frame: the MIDI bytes the sound board was sent for it (a frame run again sends only what wasn't)
	std::map<uint32_t, std::vector<uint8_t>> midi_sent;
	std::vector<uint8_t> scratch;
};

} // namespace vf3net
