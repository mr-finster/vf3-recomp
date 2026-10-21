#include "vf3_netplay.h"
#include <algorithm>
#include <cstring>

namespace vf3net {

namespace {

// The switches' places in the input word
enum : uint16_t { UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, PUNCH = 16, KICK = 32, GUARD = 64, ESCAPE = 128, START = 256, COIN = 512 };

constexpr uint32_t BLOB_MAGIC = 0x42334656;   // "VF3B"

} // namespace

uint16_t pack(const m3::Controls &c, int p)
{
	return uint16_t((c.up[p] ? UP : 0) | (c.down[p] ? DOWN : 0) | (c.left[p] ? LEFT : 0) | (c.right[p] ? RIGHT : 0) |
	                (c.punch[p] ? PUNCH : 0) | (c.kick[p] ? KICK : 0) | (c.guard[p] ? GUARD : 0) | (c.escape[p] ? ESCAPE : 0) |
	                (c.start[p] ? START : 0) | (c.coin[p] ? COIN : 0));
}

void unpack(uint16_t b, int p, m3::Controls &c)
{
	c.up[p] = b & UP; c.down[p] = b & DOWN; c.left[p] = b & LEFT; c.right[p] = b & RIGHT;
	c.punch[p] = b & PUNCH; c.kick[p] = b & KICK; c.guard[p] = b & GUARD; c.escape[p] = b & ESCAPE;
	c.start[p] = b & START; c.coin[p] = b & COIN;
	c.test[p] = c.service[p] = false;
}

uint64_t content_hash(m3::Model3 &b)
{
	const bool hidden = !b.free_play_text.empty();
	if (hidden) b.show_free_play_text();
	uint64_t h = 0;
	for (const std::vector<uint8_t> *v : { &b.crom, &b.banked, &b.vrom, &b.sound_rom, &b.samples })
		h = h * 0x100000001B3ull ^ vfnet::hash64(v->data(), v->size());
	if (hidden) b.hide_free_play_text();
	return h;
}

std::vector<uint8_t> start_blob(const m3::Model3 &b)
{
	std::vector<uint8_t> out(4);
	memcpy(out.data(), &BLOB_MAGIC, 4);
	out.insert(out.end(), b.backup.begin(), b.backup.end());
	const uint8_t *e = reinterpret_cast<const uint8_t *>(b.eeprom.regs);
	out.insert(out.end(), e, e + sizeof b.eeprom.regs);
	return out;
}

// (a frame's MIDI bytes go at once, a guessed frame's as much as a certain one's, so no sound waits for the other
// side's input)
void Adapter::take_midi(std::vector<uint8_t> &out)
{
	midi_taken = true;
	scratch.assign(board.midi.begin(), board.midi.end());
	board.midi.clear();
	out.clear();
	auto it = midi_sent.find(frame);
	const bool sent = it != midi_sent.end() && !it->second.empty();
	if (!scratch.empty() && !sent) {
		out = scratch;
		midi_sent[frame] = scratch;
	} else if (it == midi_sent.end())
		midi_sent[frame] = {};
	while (!midi_sent.empty() && midi_sent.begin()->first + 64 < frame) midi_sent.erase(midi_sent.begin());
}

bool Adapter::begin(const std::vector<uint8_t> &blob)
{
	uint32_t magic = 0;
	if (blob.size() != 4 + board.backup.size() + sizeof board.eeprom.regs) return false;
	memcpy(&magic, blob.data(), 4);
	if (magic != BLOB_MAGIC) return false;
	if (settle) settle();
	if (on_begin) on_begin();
	memcpy(board.backup.data(), blob.data() + 4, board.backup.size());
	memcpy(board.eeprom.regs, blob.data() + 4 + board.backup.size(), sizeof board.eeprom.regs);
	// the PC way on both sides, whatever each plays offline: the FREE PLAY notices hidden in the CROM the boot
	// copies the program from, free play in the settings, the boot run unseen, the same clock
	if (board.hide_free_play_text() != 2) return false;
	if (program_changed) program_changed();
	board.rtc_time = 0;
	board.power_on();
	for (int i = 0; i < 300 && board.coin_setting() < 0; i++) board.run_frame();
	if (board.coin_setting() < 0 || (!board.set_coin_setting(m3::Model3::COIN_FREE_PLAY) && !board.free_play())) return false;
	board.power_on();
	for (int i = 0; i < BOOT_FRAMES; i++) board.run_frame();
	board.midi.clear();
	frame = 0;
	midi_sent.clear();
	checkpoints.clear();
	return true;
}

uint64_t Adapter::hash(const std::vector<uint8_t> &state)
{
	const size_t skip = std::min(state.size(), m3::Model3::STATE_HEADER);
	return vfnet::hash64(state.data() + skip, state.size() - skip);
}

void Adapter::save(std::vector<uint8_t> &state)
{
	if (settle) settle();
	board.save_state(state);
	// (the frame number with it: a load puts the adapter back at that frame)
	const size_t n = state.size();
	state.resize(n + 4);
	memcpy(state.data() + n, &frame, 4);
}

bool Adapter::load(const std::vector<uint8_t> &state)
{
	if (state.size() < 4) return false;
	if (settle) settle();
	memcpy(&frame, state.data() + state.size() - 4, 4);
	return board.load_state(state.data(), state.size() - 4);
}

void Adapter::advance(const uint16_t input[2], bool resim)
{
	unpack(input[0], 0, board.controls);
	unpack(input[1], 1, board.controls);
	midi_taken = false;
	if (run) run(resim);
	else board.run_frame();
	if (!midi_taken) {
		std::vector<uint8_t> out;
		take_midi(out);
		if (!out.empty() && midi_out) midi_out(out);
	}
	frame++;
	if (checkpoint_every && frame % checkpoint_every == 0) {
		board.save_state(scratch);
		checkpoints[frame] = hash(scratch);
	}
}

} // namespace vf3net
