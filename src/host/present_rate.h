// The screen's real rate with vsync, measured at the presents (VF4's, a90ee91). A screen that says 120 Hz and takes
// 60 pictures a second (an older PC, a Mac in Low Power Mode) had smooth motion's pictures each wait 2 refreshes: a
// stutter. While presents are what waits, their time apart is kept; 2 s of them 2 or more refreshes apart and the
// screen's rate is taken as the reported one / N (a picture a frame there); once they come each refresh again, the
// reported rate is back and trusted 30 s (a GPU merely slow doesn't make it flap). Only the pictures shown change: the
// game's own speed never does. Tests: <GAME>_TEST_SCREEN_HZ=real[,said] fakes a screen that says said Hz (120) and
// takes real pictures a second, vsync on (sleep_after_present, said_hz)
#pragma once
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

struct PresentRate
{
	double shown_hz = 0;   // (0: the screen's own rate)
	uint64_t last = 0, looked = 0, trust_until = 0;
	double apart = 0, waited = 0;
	int slow_looks = 0;
	const char *fake = nullptr;
	bool keep = false;   // (tests: <GAME>_TEST_SCREEN_KEEP=1, the fake's screen with no adapting: the stutter as before)
	explicit PresentRate(const char *env) : fake(getenv(env))
	{
		if (fake && atof(fake) <= 1) fake = nullptr;
		std::string k = env;   // (VF3_TEST_SCREEN_HZ: VF3_TEST_SCREEN_KEEP)
		k.replace(k.size() - 2, 2, "KEEP");
		keep = getenv(k.c_str()) != nullptr;
	}
	// the rate the screen says, or the fake's
	double said_hz(double reported) const { if (!fake) return reported; const char *c = strchr(fake, ','); return c ? atof(c + 1) : 120.0; }
	bool faked() const { return fake != nullptr; }
	void sleep_after_present()
	{
		if (!fake) return;
		static uint64_t next = SDL_GetTicksNS();
		const uint64_t per = uint64_t(1e9 / atof(fake));
		while (next <= SDL_GetTicksNS()) next += per;
		const uint64_t w = next - SDL_GetTicksNS();
		if (w < 1000000000ull) SDL_DelayNS(w);
	}
	// after a present that began at pr0 (SDL_GetTicksNS); reported: the screen's Hz as it says (said_hz's); paced: vsync on,
	// the window shown. The rate to pace pictures to: rate(reported)
	void after_present(uint64_t pr0, double reported, bool paced)
	{
		const uint64_t pr1 = SDL_GetTicksNS();
		const double iv = (pr1 - last) / 1e9, w = (pr1 - pr0) / 1e9;
		last = pr1;
		if (paced && iv < 0.25) { apart += (iv - apart) * 0.05; waited += (w - waited) * 0.05; }
		if (keep || pr1 - looked < 1000000000ull) return;
		looked = pr1;
		const double per = apart * reported, whole = std::max(1.0, std::round(per));
		if (!paced || apart <= 0 || reported <= 0) {
			if (shown_hz > 0) fprintf(stderr, "screen: no vsync pacing, its own rate (%.0f Hz) again\n", reported);
			shown_hz = 0;
		} else if (shown_hz == 0) {
			slow_looks = whole >= 2 && waited > apart * 0.4 && pr1 >= trust_until ? slow_looks + 1 : 0;
			if (slow_looks >= 2) {
				slow_looks = 0;
				shown_hz = reported / whole;
				fprintf(stderr, "screen: takes %.0f pictures a second, not %.0f (presents %.1f ms apart): a picture a frame\n", shown_hz, reported, apart * 1000);
			}
		} else if (per < 1.3) {
			shown_hz = 0;
			trust_until = pr1 + 30000000000ull;
			fprintf(stderr, "screen: takes a picture each refresh again (%.0f Hz): its rate trusted 30 s\n", reported);
		} else if (reported / whole != shown_hz && waited > apart * 0.4) {   // (the screen's wait, not the drawing's)
			shown_hz = reported / whole;
			fprintf(stderr, "screen: takes %.0f pictures a second\n", shown_hz);
		}
	}
	double rate(double reported) const { return shown_hz > 0 ? shown_hz : reported; }
	void reset() { shown_hz = 0; slow_looks = 0; }
};
