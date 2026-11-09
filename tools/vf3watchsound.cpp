// vf3watchsound: a watcher's match sound after catching up, against one's who watched from the start. A match (a
// script's players, the game's board through vf3net::Adapter, as a watcher's runs it) gives each frame's MIDI bytes;
// then the match's sound board (main.cpp's SoundBoard, a frame's run each) is given them:
//   from the start: every frame's heard;
//   v0.1.19's late watcher: the frames before the join point dropped (the board run on with nothing: hushed), heard after;
//   this build's: the frames before the join point run through it unheard (Game::unheard), heard after.
// For each join point (--step apart): the loudness of each half second after it, from the start / dropped / unheard,
// and how much of the unheard board's output is the from-start board's, sample for sample.
//   vf3watchsound <vf3.zip> --script FILE [--frames N] [--step F] [--window F] [--interp]
#include "../src/host/input_script.h"
#include "../src/host/vf3_netplay.h"
#include "../src/machine/model3.h"
#include "../src/rec/ppc_rec.h"
#include "../src/sound/soundboard.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double FRAME_HZ = 57.524160;   // (main.cpp's)

double rms(const std::vector<int16_t> &v, size_t from, size_t n)
{
	double s = 0;
	size_t k = 0;
	for (size_t i = from; i < from + n && i < v.size(); i++, k++) s += double(v[i]) * double(v[i]);
	return k ? std::sqrt(s / double(k)) : 0;
}

} // namespace

int main(int argc, char **argv)
{
	std::string rom, script_path;
	long frames = 20000, step = 4000, window = 600;
	bool use_rec = true;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--step" && i + 1 < argc) step = strtol(argv[++i], nullptr, 10);
		else if (a == "--window" && i + 1 < argc) window = strtol(argv[++i], nullptr, 10);
		else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
		else if (a == "--interp") use_rec = false;
		else if (a[0] != '-' && rom.empty()) rom = a;
		else { fprintf(stderr, "usage: vf3watchsound <vf3.zip> --script FILE [--frames N] [--step F] [--window F] [--interp]\n"); return 2; }
	}
	std::string error;
	auto pm = std::make_unique<m3::Model3>();
	m3::Model3 &m = *pm;
	if (!m.load(rom, error)) { fprintf(stderr, "vf3watchsound: %s\n", error.c_str()); return 1; }
	ppcrec::Rec rec;
	std::vector<uint32_t> image;
	vf3net::Adapter ad(m);
	ad.program_changed = [&] {
		if (!use_rec) return;
		std::vector<uint32_t> img = m.program_image();
		if (img == image) return;
		image = std::move(img);
		m.cpu.rec = rec.build(m.cpu, image, m.profile->map, m.profile->map_count) ? &rec : nullptr;
	};
	InputScript script;
	if (!script.load(script_path, error)) { fprintf(stderr, "vf3watchsound: %s\n", error.c_str()); return 1; }
	m.reset();   // (a fresh board's NVRAM, its settings written as it starts: the host's, as vf3net's)
	for (int i = 0; i < 300 && m.coin_setting() < 0; i++) m.run_frame();
	const std::vector<uint8_t> blob = vf3net::start_blob(m);
	if (!ad.begin(blob)) { fprintf(stderr, "vf3watchsound: the match didn't start\n"); return 1; }
	std::vector<std::vector<uint8_t>> fr;
	ad.midi_out = [&](const std::vector<uint8_t> &b) { fr.back() = b; };
	m3::Controls c{};
	size_t bytes = 0;
	for (long f = 0; f < frames; f++) {
		if (script.step(c, uint64_t(f)).quit) { frames = f; break; }
		const uint16_t in[2] = { vf3net::pack(c, 0), vf3net::pack(c, 1) };
		fr.emplace_back();
		ad.advance(in, false);
		bytes += fr.back().size();
	}
	printf("the match: %ld frames, %zu MIDI bytes\n", frames, bytes);

	SoundBoard sb;
	sb.attach(&m.sound_rom, &m.samples);
	auto run = [&](long upto_heard_from, bool unheard_before, long from_frame, long to_frame, std::vector<int16_t> &kept) {
		// (from a reset: the frames before upto_heard_from given (unheard) or not (dropped, the board run on), the rest
		// heard; the output from from_frame to to_frame kept)
		sb.reset();
		sb.set_volumes(1.0f, getenv("MUSIC_ONLY") ? 0.0f : 1.0f);   // (MUSIC_ONLY: the effects turned off, the music alone)
		std::vector<int16_t> out;
		for (long f = 0; f < to_frame; f++) {
			const bool before = f < upto_heard_from;
			if (!before || unheard_before)
				for (uint8_t b : fr[size_t(f)]) sb.midi_in(b);
			out.clear();
			sb.run(1.0 / FRAME_HZ, out);
			if (f >= from_frame) kept.insert(kept.end(), out.begin(), out.end());
		}
	};
	for (long L = step; L + window <= frames; L += step) {
		std::vector<int16_t> a, d, u;
		run(0, true, L, L + window, a);
		run(L, false, L, L + window, d);
		run(L, true, L, L + window, u);
		size_t same = 0;
		for (size_t i = 0; i < std::min(a.size(), u.size()); i++) same += a[i] == u[i];
		const size_t hs = a.size() / size_t(std::max<long>(1, window / 29));   // (about half a second each)
		printf("join at %5ld: unheard the same as from the start %5.1f%% of samples; loudness each half second after it (from the start / dropped / unheard):", L, 100.0 * double(same) / double(std::max<size_t>(1, a.size())));
		for (size_t at = 0; at + hs <= a.size(); at += hs) printf(" %.0f/%.0f/%.0f", rms(a, at, hs), rms(d, at, hs), rms(u, at, hs));
		printf("\n");
		fflush(stdout);
	}
	return 0;
}
