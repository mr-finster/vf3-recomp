// The game's own coin sound (Online > Settings > Match found chime, issue VF1 #44): what the cabinet plays when a coin
// goes in, made at run time by a sound board of its own from the ROM's sound program and samples (nothing of it
// stored). The main board asks for it with three MIDI bytes, A0 14 05 (request 0x14, sound 5: logged with vf3run's
// VF3_MIDI_LOG at a coin, Revision D and Team Battle alike; played alone on a fresh board it matches the effects heard
// then). A fresh board's program takes about 64 frames to start before it plays a request; the sound itself is about
// 1.3 s. The game's own board is never touched: this one is apart, its own 68000 and SCSPs, and thrown away after.
#pragma once
#include "soundboard.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace coin_sound {

constexpr int RATE = 44100;   // (the board's: 256 68000 cycles a sample)

// Stereo 16-bit at RATE, the board's output times gain (as the game's: the host's LEVEL), from the sound's first
// sample to where it has died away; empty if the board played nothing
inline std::vector<int16_t> make(const std::vector<uint8_t> &rom, const std::vector<uint8_t> &samples, double gain)
{
	if (rom.empty() || samples.empty()) return {};
	SoundBoard sb;
	sb.attach(&rom, &samples);
	sb.reset();
	sb.gain = gain;
	for (uint8_t b : { 0xA0, 0x14, 0x05 }) sb.midi_in(b);   // (queued: the program takes them once it's started)
	std::vector<int16_t> out;
	const double frame = 1.0 / 57.524160;
	for (int f = 0; f < 64 + 150; f++) sb.run(frame, out);   // (its start, then 2.6 s: the sound and its tail)
	// from the first sample heard to the last above about -60 dB
	size_t first = 0, last = 0;
	while (first < out.size() && std::abs(int(out[first])) < 8) first++;
	for (size_t i = first; i < out.size(); i++)
		if (std::abs(int(out[i])) >= 32) last = i;
	if (first >= out.size()) return {};
	first &= ~size_t(1);
	last = std::min(out.size(), (last | 1) + 1);
	std::vector<int16_t> pcm(out.begin() + ptrdiff_t(first), out.begin() + ptrdiff_t(last));
	// a 5 ms fade at the end (cut where it's quiet, but not at a zero)
	const size_t fade = std::min<size_t>(pcm.size() / 2, RATE / 200);
	for (size_t i = 0; i < fade; i++) {
		const float k = float(i) / float(fade);
		pcm[pcm.size() - 2 - 2 * i] = int16_t(pcm[pcm.size() - 2 - 2 * i] * k);
		pcm[pcm.size() - 1 - 2 * i] = int16_t(pcm[pcm.size() - 1 - 2 * i] * k);
	}
	return pcm;
}

} // namespace coin_sound
