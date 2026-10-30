// The chime when an online match is found (Online settings: Match found chime): two bell notes, a coin's "ding-ding",
// made here (no sound file), on a stream of its own so the game's sound and its pacing are left as they are. With it
// the window flashes in the taskbar or the Dock if it's not the one in front (a player waiting in another window).
#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace chime {

constexpr int RATE = 48000;

// Stereo 16-bit: B5 then E6, a bell's partials each (the higher ones dying sooner), 0.75 s in all
inline std::vector<int16_t> make(float level)
{
	const double pi = 3.14159265358979323846;
	const struct { double at, hz, len; } notes[] = { { 0.0, 987.77, 0.12 }, { 0.11, 1318.51, 0.64 } };
	const int n = int(RATE * 0.75);
	std::vector<float> mix(size_t(n), 0.0f);
	for (const auto &nt : notes)
		for (int i = int(nt.at * RATE); i < n; i++) {
			const double t = double(i) / RATE - nt.at;
			if (t > nt.len) break;
			const double attack = std::min(1.0, t / 0.004), release = std::min(1.0, (nt.len - t) / 0.03);
			const double v = std::sin(2 * pi * nt.hz * t) * std::exp(-t / 0.22) + 0.30 * std::sin(2 * pi * nt.hz * 2.0 * t) * std::exp(-t / 0.09) +
			                 0.12 * std::sin(2 * pi * nt.hz * 3.01 * t) * std::exp(-t / 0.05);
			mix[size_t(i)] += float(v * attack * release);
		}
	std::vector<int16_t> out(size_t(n) * 2);
	for (int i = 0; i < n; i++) {
		const float s = std::clamp(mix[size_t(i)] * level * 0.35f, -1.0f, 1.0f);
		out[size_t(i) * 2] = out[size_t(i) * 2 + 1] = int16_t(s * 32767.0f);
	}
	return out;
}

// Played (the stream opened the first time: kept for the next), and the window flashed if it's not in front.
// level 0-1 (the game's volume). No sound device: the flash only.
inline void play(SDL_AudioStream *&stream, SDL_Window *win, float level)
{
	if (win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) SDL_FlashWindow(win, SDL_FLASH_UNTIL_FOCUSED);
	if (level <= 0) return;
	if (!stream) {
		const SDL_AudioSpec spec{ SDL_AUDIO_S16, 2, RATE };
		stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (!stream) return;
		SDL_ResumeAudioStreamDevice(stream);
	}
	const std::vector<int16_t> pcm = make(level);
	SDL_ClearAudioStream(stream);
	SDL_PutAudioStreamData(stream, pcm.data(), int(pcm.size() * sizeof(int16_t)));
}

} // namespace chime
