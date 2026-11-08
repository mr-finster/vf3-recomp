// The chime when an online match is found (Online settings: Match found chime): the game's own coin sound (VF1 issue
// #44), made at run time from the ROM's sound program and samples by a sound board of the game's own (each game's
// coin_sound), never a sound stored with the game. Played on a stream of its own, so the game's sound and its pacing
// are left as they are. With it the window flashes in the taskbar or the Dock if it's not the one in front (a player
// waiting in another window).
#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace chime {

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
