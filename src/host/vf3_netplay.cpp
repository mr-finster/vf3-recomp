#include "vf3_netplay.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>

namespace vf3net {

namespace {

// The switches' places in the input word
enum : uint16_t { UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, PUNCH = 16, KICK = 32, GUARD = 64, ESCAPE = 128, START = 256, COIN = 512 };
static_assert(START == START_BUTTON);

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

std::string online_version(const std::string &b)
{
	// a release's name: v, three numbers, and maybe a word after them (-alpha, -online-preview), not git describe's
	// -<commits>-g<commit> (a build between releases)
	size_t i = 0;
	auto number = [&] { const size_t at = i; while (i < b.size() && isdigit((unsigned char)b[i])) i++; return i > at; };
	bool release = !b.empty() && b[i++] == 'v' && number() && i < b.size() && b[i++] == '.' && number() && i < b.size() && b[i++] == '.' && number();
	if (release && i < b.size()) {
		release = b[i] == '-' && i + 1 < b.size() && islower((unsigned char)b[i + 1]);
		for (size_t k = i + 1; release && k < b.size(); k++)
			release = islower((unsigned char)b[k]) || isdigit((unsigned char)b[k]) || b[k] == '-';
		// (a name's last parts "-<n>-g<hex>": git describe's between releases)
		const size_t g = b.rfind("-g");
		if (release && g != std::string::npos && g > i && g + 2 < b.size()) {
			size_t d = b.rfind('-', g - 1);
			const std::string n = b.substr(d + 1, g - d - 1), h = b.substr(g + 2);
			const bool num = !n.empty() && n.find_first_not_of("0123456789") == std::string::npos;
			const bool hex = h.find_first_not_of("0123456789abcdef") == std::string::npos;
			if (num && hex) release = false;
		}
	}
	return release ? NET_VERSION : b;
}

uint64_t content_hash(m3::Model3 &b)
{
	const bool hidden = !b.free_play_text.empty(), dural = b.dural_on();
	if (hidden) b.show_free_play_text();
	if (dural) b.set_dural(false);   // (Play as Dural's patches aside: online plays without them)
	uint64_t h = 0;
	for (const std::vector<uint8_t> *v : { &b.crom, &b.banked, &b.vrom, &b.sound_rom, &b.samples })
		h = h * 0x100000001B3ull ^ vfnet::hash64(v->data(), v->size());
	if (hidden) b.hide_free_play_text();
	if (dural) b.set_dural(true);
	return h;
}

void set_versus_count(m3::Model3 &b, uint32_t count)
{
	if (b.profile->versus_count) memcpy(b.ram.data() + b.profile->versus_count, &count, 4);   // (0: not known for this program)   // (the RAM's words in the host's order)
}

void set_random_seed(m3::Model3 &b, uint32_t seed)
{
	if (b.profile->random_seed) memcpy(b.ram.data() + b.profile->random_seed, &seed, 4);   // (the RAM's words in the host's order)
}

uint32_t random_versus_count()
{
	return std::random_device{}() % 16;
}

std::vector<uint8_t> start_blob(const m3::Model3 &b)
{
	std::vector<uint8_t> out(4);
	memcpy(out.data(), &BLOB_MAGIC, 4);
	out.insert(out.end(), b.backup.begin(), b.backup.end());
	const uint8_t *e = reinterpret_cast<const uint8_t *>(b.eeprom.regs);
	out.insert(out.end(), e, e + sizeof b.eeprom.regs);
	out.push_back(uint8_t(random_versus_count()));
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
	if (blob.size() != 4 + board.backup.size() + sizeof board.eeprom.regs + 1) return false;
	memcpy(&magic, blob.data(), 4);
	if (magic != BLOB_MAGIC) return false;
	if (settle) settle();
	if (on_begin) on_begin();
	memcpy(board.backup.data(), blob.data() + 4, board.backup.size());
	memcpy(board.eeprom.regs, blob.data() + 4 + board.backup.size(), sizeof board.eeprom.regs);
	// the PC way on both sides, whatever each plays offline: the FREE PLAY notices hidden in the CROM the boot
	// copies the program from, free play in the settings, the boot run unseen, the same clock
	if (!dural) board.set_dural(false);   // (Dural on the select: the match's, the same on both sides)
	else if (board.set_dural(true) == 0) return false;
	if (board.hide_free_play_text() != 2) return false;
	if (program_changed) program_changed();
	board.rtc_time = 0;
	board.power_on();
	for (int i = 0; i < 300 && board.coin_setting() < 0; i++) {
		board.run_frame();
		if (booting && i % 60 == 59) booting();
	}
	if (board.coin_setting() < 0 || (!board.set_coin_setting(m3::Model3::COIN_FREE_PLAY) && !board.free_play())) return false;
	// and its stage select off, as most arcades had it (no stage screen: the game's own next stage), starting at
	// the host's random place in the stages' list, as an arcade on all day would be
	board.backup[stage_select(board)] = 1;
	board.power_on();
	for (int i = 0; i < board.profile->boot_frames; i++) {
		board.run_frame();
		if (booting && i % 60 == 59) booting();
	}
	set_versus_count(board, blob.back() % 16);
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
		board.save_state(check_buf);
		checkpoints[frame] = hash(check_buf);
	}
}

void Score::frame(const m3::Model3 &board)
{
	auto byte = [&](uint32_t a) { return board.ram[a ^ 3]; };   // (the RAM's host-order words: the game's byte)
	const m3::Profile &p = *board.profile;
	if (!p.rounds_p1) return;   // (not known for this program)
	const uint8_t r[3] = { byte(p.rounds_p1), byte(p.rounds_p2), uint8_t(byte(p.playing) & 3) };
	if (memcmp(r, now, 3) != 0) { memcpy(now, r, 3); held = 0; return; }
	if (++held != 30) return;   // (once, when it's held long enough)
	playing = r[2];
	for (int p = 0; p < 2; p++)
		if (r[p] == seen[p] + 1) {
			round_ends++;
			if (seen[0] + seen[1] == 0) versus = playing == 3;   // (the match's first round)
			if (playing != 3) versus = false;
			if (versus) rounds[p]++;
		}
	if (r[0] == 0 && r[1] == 0 && seen[0] + seen[1] > 0) {   // (the match over: whoever had more won it)
		if (versus && seen[0] != seen[1]) matches[seen[0] > seen[1] ? 0 : 1]++;
		versus = false;
	}
	seen[0] = r[0];
	seen[1] = r[1];
}

} // namespace vf3net
