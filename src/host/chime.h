// The chime when an online match is found (Online settings: Match found chime): the game's own coin sound (VF1 issue
// #44), made at run time from the ROM's sound program and samples by a sound board of the game's own (each game's
// coin_sound), never a sound stored with the game. Played on a stream of its own, so the game's sound and its pacing
// are left as they are. With it the window flashes in the taskbar or the Dock if it's not the one in front (a player
// waiting in another window).
#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace chime {

// The bell the chime was before the coin sound (B5 then E6, a bell's partials each, the higher ones dying sooner;
// 0.75 s), stereo 16-bit at rate: the chime if the coin sound can't be made (a board that played nothing)
inline std::vector<int16_t> bell(int rate)
{
	const double pi = 3.14159265358979323846;
	const struct { double at, hz, len; } notes[] = { { 0.0, 987.77, 0.12 }, { 0.11, 1318.51, 0.64 } };
	const int n = int(rate * 0.75);
	std::vector<float> mix(size_t(n), 0.0f);
	for (const auto &nt : notes)
		for (int i = int(nt.at * rate); i < n; i++) {
			const double t = double(i) / rate - nt.at;
			if (t > nt.len) break;
			const double attack = std::min(1.0, t / 0.004), release = std::min(1.0, (nt.len - t) / 0.03);
			const double v = std::sin(2 * pi * nt.hz * t) * std::exp(-t / 0.22) + 0.30 * std::sin(2 * pi * nt.hz * 2.0 * t) * std::exp(-t / 0.09) +
			                 0.12 * std::sin(2 * pi * nt.hz * 3.01 * t) * std::exp(-t / 0.05);
			mix[size_t(i)] += float(v * attack * release);
		}
	std::vector<int16_t> out(size_t(n) * 2);
	for (int i = 0; i < n; i++) {
		const float s = std::clamp(mix[size_t(i)] * 0.35f, -1.0f, 1.0f);
		out[size_t(i) * 2] = out[size_t(i) * 2 + 1] = int16_t(s * 32767.0f);
	}
	return out;
}

// The coin sound as made, or the bell if it came out empty; the log says which
inline std::vector<int16_t> coin_or_bell(std::vector<int16_t> pcm, int rate)
{
	if (!pcm.empty()) fprintf(stderr, "the match found chime: the game's coin sound (%.2f s)\n", double(pcm.size() / 2) / rate);
	else fprintf(stderr, "the match found chime: the coin sound couldn't be made, the bell instead\n");
	return pcm.empty() ? bell(rate) : pcm;
}

// Played (the stream opened the first time, for the sound's rate: kept for the next), and the window flashed if
// it's not in front. pcm: stereo 16-bit at rate; level 0-1 (the game's volume). No sound device or no sound: the
// flash only.
inline void play(SDL_AudioStream *&stream, SDL_Window *win, float level, const std::vector<int16_t> &pcm, int rate)
{
	if (win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) SDL_FlashWindow(win, SDL_FLASH_UNTIL_FOCUSED);
	if (level <= 0 || pcm.empty() || rate <= 0) return;
	if (!stream) {
		const SDL_AudioSpec spec{ SDL_AUDIO_S16, 2, rate };
		stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (!stream) return;
		SDL_ResumeAudioStreamDevice(stream);
	}
	std::vector<int16_t> out(pcm.size());
	for (size_t i = 0; i < pcm.size(); i++) out[i] = int16_t(std::clamp(float(pcm[i]) * level, -32768.0f, 32767.0f));
	SDL_ClearAudioStream(stream);
	SDL_PutAudioStreamData(stream, out.data(), int(out.size() * sizeof(int16_t)));
}

} // namespace chime
