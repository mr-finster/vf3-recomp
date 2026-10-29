#include <algorithm>
#include <array>
#include <cmath>
// vf3run: the headless runner. Runs the game on the interpreter for a number of frames (or until a script
// quits), playing an input script, and writes the CPU state and a RAM hash after every frame in the reference
// build's format, for tools/statecmp.py.
//   vf3run <vf3.zip> [--frames N] [--script FILE] [--state FILE] [--ram-at FRAME] [--watch ADDR] [--rec] [--quiet]
// --sets ZIP lists the programs the zip holds. --wav FILE runs the sound board and writes what it plays. --shots DIR writes the script's screenshots there as PNG. --rec runs the recompiled code (src/rec) with the interpreter for the rest. --ram-at writes RAM after that frame to ram_<frame>.bin in the current folder (backup RAM to backup_<frame>.bin) (the tile generator's RAM and registers to tiles_<frame>.bin); --watch ADDR (hex) reports
// the CPU's stores to that word (--trace-pc ADDR: r3-r8 and LR each time that instruction runs; both on the interpreter). VF3_VOLUMES=music,effects (percent) sets the sound board's two volumes for --wav. VF3_POKE=addr=value
// (hex address into --ram-at's dump, a byte; "b" first: the backup RAM's) writes RAM before every frame
// (VF3_POKE_FRAMES=from-to: only those). VF3_FREE_PLAY=1: the coin setting FREE PLAY before the boot, as the PC way.
// VF3_SET=vf3tb: Team Battle's profile (the zip must hold its set); VF3_MAP_OF=vf3: another profile's recompiler map (none: the interrupts' entries only);
// VF3_BORROW=backdrops,coin,settings,test_menu,versus_count,scene,score,free_play_text: those addresses from
// VF3_BORROW_FROM's profile (default vf3), to try them on another program; VF3_MAP_IN=file: more entry points (hex, a line each); VF3_MAP_OUT=file: appends every address the interpreter ran
// from for want of one (--rec; a map's learning: run again with them in until none are new).
#include <map>
#include "../src/host/input_script.h"
#include "../src/machine/model3.h"
#include "../src/machine/romset.h"
#include "../src/rec/ppc_rec.h"
#include "../src/host/png.h"
#include "../src/video/tilegen.h"
#include "../src/video/sky_lead.h"
#include "../src/video/r3d_raster.h"
#include "../src/video/hud_names.h"
#include "../src/sound/soundboard.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
	std::string rom, script_path, state_path, shots, wav_path;
	long frames = -1, ram_at = -1;
	uint32_t watch = 0xFFFFFFFF, trace_pc = 0xFFFFFFFF;
	bool quiet = false, rec = false;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
		else if (a == "--state" && i + 1 < argc) state_path = argv[++i];
		else if (a == "--shots" && i + 1 < argc) shots = argv[++i];
		else if (a == "--wav" && i + 1 < argc) wav_path = argv[++i];
		else if (a == "--quiet") quiet = true;
		else if (a == "--sets" && i + 1 < argc) {   // which programs a zip holds (romset::sets_in), and nothing else
			for (const m3::Profile *p : romset::sets_in(argv[++i])) printf("%s (%s)\n", p->set, p->title);
			return 0;
		}
		else if (a == "--rec") rec = true;
		else if (a == "--ram-at" && i + 1 < argc) ram_at = strtol(argv[++i], nullptr, 10);
		else if (a == "--watch" && i + 1 < argc) watch = uint32_t(strtoul(argv[++i], nullptr, 16));
		else if (a == "--trace-pc" && i + 1 < argc) trace_pc = uint32_t(strtoul(argv[++i], nullptr, 16));
		else if (a[0] != '-' && rom.empty()) rom = a;
		else {
			fprintf(stderr, "vf3run: unknown argument %s\nusage: vf3run <vf3.zip> [--frames N] [--script FILE] [--state FILE] [--quiet]\n", a.c_str());
			return 2;
		}
	}
	if (rom.empty() || (frames < 0 && script_path.empty())) {
		fprintf(stderr, "usage: vf3run <vf3.zip> [--frames N] [--script FILE] [--state FILE] [--quiet]\n");
		return 2;
	}
	static m3::Model3 board;
	if (const char *set = getenv("VF3_SET")) {   // tests: another program's profile ("vf3tb"; profile.h)
		if (!(board.profile = m3::find_profile(set))) { fprintf(stderr, "vf3run: no such set %s\n", set); return 2; }
	}
	if (const char *groups = getenv("VF3_BORROW")) {   // tests: "backdrops,coin,...": those addresses from VF3_BORROW_FROM's profile (vf3)
		static m3::Profile mixed = *board.profile;
		const m3::Profile *from = m3::find_profile(getenv("VF3_BORROW_FROM") ? getenv("VF3_BORROW_FROM") : "vf3");
		if (!from) { fprintf(stderr, "vf3run: no such set to borrow from\n"); return 2; }
		const std::string g = std::string(",") + groups + ",";
		auto has = [&](const char *k) { return g.find(std::string(",") + k + ",") != std::string::npos; };
		if (has("free_play_text")) mixed.free_play_text = from->free_play_text;
		if (has("coin")) mixed.coin = from->coin;
		if (has("settings")) mixed.settings = from->settings;
		if (has("test_menu")) mixed.test_menu = from->test_menu;
		if (has("versus_count")) mixed.versus_count = from->versus_count;
		if (has("scene")) mixed.scene = from->scene;
		if (has("score")) mixed.rounds_p1 = from->rounds_p1, mixed.rounds_p2 = from->rounds_p2, mixed.playing = from->playing;
		if (has("backdrops")) mixed.backdrops = from->backdrops;
		board.profile = &mixed;
	}
	std::string error;
	if (!board.load(rom, error)) {
		fprintf(stderr, "vf3run: %s\n", error.c_str());
		return 1;
	}
	InputScript script;
	if (!script_path.empty() && !script.load(script_path, error)) {
		fprintf(stderr, "vf3run: %s\n", error.c_str());
		return 1;
	}
	FILE *state = nullptr;
	if (!state_path.empty() && !(state = fopen(state_path.c_str(), "wb"))) {
		fprintf(stderr, "vf3run: can't write %s\n", state_path.c_str());
		return 1;
	}
	static ppcrec::Rec recompiler;
	if (rec) {
		if (watch != 0xFFFFFFFF || trace_pc != 0xFFFFFFFF) {
			fprintf(stderr, "vf3run: --watch needs the interpreter\n");
			return 2;
		}
		ppcrec::Stats st;
		const m3::Profile *map_of = getenv("VF3_MAP_OF") ? m3::find_profile(getenv("VF3_MAP_OF")) : board.profile;
		if (!map_of) map_of = board.profile;
		std::vector<uint32_t> seeds(map_of->map, map_of->map + map_of->map_count);
		if (getenv("VF3_MAP_OF") && strcmp(getenv("VF3_MAP_OF"), "none") == 0) seeds = { 0x100, 0x500, 0x900 };   // (the interrupts' only)
		if (const char *in = getenv("VF3_MAP_IN")) {   // (tests: more entry points, a hex address a line)
			if (FILE *f = fopen(in, "r")) { unsigned a; while (fscanf(f, "%x", &a) == 1) seeds.push_back(a); fclose(f); }
		}
		if (getenv("VF3_MAP_OUT")) recompiler.unmapped_max = size_t(1) << 20;
		if (!recompiler.build(board.cpu, board.program_image(), seeds.data(), seeds.size(), &st)) {
			fprintf(stderr, "vf3run: the recompile failed\n");
			return 1;
		}
		board.cpu.rec = &recompiler;
		if (!quiet)
			printf("recompiled %zu instructions (%zu native) into %zu KB in %.0f ms\n", st.instructions, st.native,
			       st.code_bytes / 1024, st.ms);
	}
	static video::TileGen tiles;
	static double tile_ms = 0;
	static std::vector<uint32_t> picture(video::W * video::H);
	static video::SceneBuilder scene_builder;
	if (getenv("VF3_WIDEN")) scene_builder.widen = float(atof(getenv("VF3_WIDEN")));
	static video::Scene scene;
	static video::Raster raster;
	raster.resize(video::W, video::H);
	if (!shots.empty())
		board.on_line = [&](unsigned y) {
			auto a = std::chrono::steady_clock::now();
			if (y == 0) tiles.clear();
			tiles.line(board, y);
			tile_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
		};
	// VF3_FLICKER=dir: smooth motion's in-between pictures checked (flicker.txt: frame, in between or a cut, pixels
	// in neither frame), the worst saved (VF3_FLICKER_MIN pixels, 200)
	const char *flicker_dir = getenv("VF3_FLICKER");
	FILE *flicker_log = flicker_dir ? fopen((std::string(flicker_dir) + "/flicker.txt").c_str(), "w") : nullptr;
	if (!flicker_log) flicker_dir = nullptr;
	const long flicker_save_min = getenv("VF3_FLICKER_MIN") ? atol(getenv("VF3_FLICKER_MIN")) : 200;
	int flicker_saved = 0;
	static SoundBoard sound;
	static std::vector<int16_t> audio;
	if (!wav_path.empty()) {
		sound.attach(&board.sound_rom, &board.samples);
		sound.reset();
		if (const char *v = getenv("VF3_VOLUMES")) {
			int m = 100, e = 100;
			sscanf(v, "%d,%d", &m, &e);
			sound.set_volumes(m / 100.0f, e / 100.0f);
		}
	}
	board.reset();
	if (const char *bl = getenv("VF3_BLANK_TEXT")) {   // (tests: "addr:n,..." CROM text, in the PowerPC's byte order, to spaces)
		unsigned long at = 0, n = 0;
		for (const char *q = bl; q && sscanf(q, "%lx:%lu", &at, &n) == 2; q = strchr(q, ','), q = q ? q + 1 : nullptr)
			for (unsigned long k = 0; k < n && ((at + k) ^ 3) < board.crom.size(); k++) board.crom[(at + k) ^ 3] = ' ';
	}
	if (getenv("VF3_FREE_PLAY") && board.hide_free_play_text() != 2) fprintf(stderr, "not the data known: the FREE PLAY notices stay\n");
	if (getenv("VF3_FREE_PLAY")) {   // (tests: the PC way's free play, set as the host does before its boot)
		for (int i = 0; i < 300 && board.coin_setting() < 0; i++) board.run_frame();
		if (!board.set_coin_setting(m3::Model3::COIN_FREE_PLAY) && !board.free_play()) fprintf(stderr, "couldn't set FREE PLAY\n");
		board.reset();
	}
	board.cpu.watch = watch & ~3u;
	board.cpu.trace_pc = trace_pc;
	auto t0 = std::chrono::steady_clock::now();
	uint64_t f = 0;
	for (;; f++) {
		if (frames >= 0 && f >= uint64_t(frames))
			break;
		InputScript::Actions act = script.step(board.controls, f);
		for (auto &m : act.marks)
			if (!quiet) printf("frame %llu: mark %s\n", (unsigned long long)f, m.c_str());
		static double t_board = 0, t_sound = 0, t_scene = 0;
		static const bool prof = getenv("VF3_PROF") != nullptr;
		if (static const char *pk = getenv("VF3_POKE"); pk) {   // tests: "addr=value,..." (bytes, as --ram-at's dump holds them) before every frame
			static const char *range = getenv("VF3_POKE_FRAMES");   // "from-to": only then
			long from = 0, to = -1;
			if (range) sscanf(range, "%ld-%ld", &from, &to);
			unsigned long pa = 0, pv = 0;
			if (!range || (long(f) >= from && long(f) <= to))
				for (const char *q = pk; q; q = strchr(q, ','), q = q ? q + 1 : nullptr) {
					const bool bk = *q == 'b';   // ("b" first: the backup RAM)
					if (sscanf(q + bk, "%lx=%lu", &pa, &pv) != 2) break;
					std::vector<uint8_t> &mem = bk ? board.backup : board.ram;
					if (pa < mem.size()) mem[pa] = uint8_t(pv);
				}
		}
		auto tp0 = std::chrono::steady_clock::now();
		board.run_frame();
		auto tp1 = std::chrono::steady_clock::now();
		t_board += std::chrono::duration<double, std::milli>(tp1 - tp0).count();
		if (prof) {
			static video::Scene ps;
			auto a = std::chrono::steady_clock::now();
			scene_builder.build(board, ps);
			t_scene += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
			if (f % 500 == 499) printf("prof %llu: board %.2f ms (tile layers %.2f of it), sound %.2f, scene build %.2f a frame\n", (unsigned long long)f + 1, t_board / 500, tile_ms / 500, t_sound / 500, t_scene / 500), t_board = t_sound = t_scene = tile_ms = 0;
		}
		auto ts0 = std::chrono::steady_clock::now();
		struct SoundTime { std::chrono::steady_clock::time_point t; double &acc; ~SoundTime() { acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); } } sound_time{ ts0, t_sound };
		if (!wav_path.empty()) {
			while (!board.midi.empty()) {
				if (getenv("VF3_MIDI_LOG")) {
					static FILE *ml = fopen(getenv("VF3_MIDI_LOG"), "w");
					fprintf(ml, "%llu %02x\n", (unsigned long long)f, board.midi.front());
					fflush(ml);
				}
				sound.midi_in(board.midi.front());
				board.midi.pop_front();
			}
			sound.run(1.0 / 57.524160, audio);
			if (getenv("VF3_DUMP_SOUND_RAM") && strstr((std::string(",") + getenv("VF3_DUMP_SOUND_RAM") + ",").c_str(), ("," + std::to_string(f + 1) + ",").c_str()))
				for (int n = 0; n < 2; n++) {
					std::vector<uint16_t> r(0x80000);
					for (uint32_t i = 0; i < r.size(); i++) r[i] = sound.ram_word(n, i);
					FILE *df = fopen(("sound_ram" + std::to_string(n + 1) + "_" + std::to_string(f + 1) + ".bin").c_str(), "wb");
					fwrite(r.data(), 2, r.size(), df);
					fclose(df);
				}
			if (getenv("VF3_DUMP_SOUND_RAM") && strstr((std::string(",") + getenv("VF3_DUMP_SOUND_RAM") + ",").c_str(), ("," + std::to_string(f + 1) + ",").c_str())) {
				FILE *df = fopen(("scsp_regs_" + std::to_string(f + 1) + ".bin").c_str(), "wb");
				for (int n = 0; n < 2; n++)
					for (uint32_t i = 0; i < 0x200; i++) { uint16_t v = sound.reg_word(n, i); fwrite(&v, 2, 1, df); }
				fclose(df);
			}
		}
		// VF3_HITCH=file: smooth motion's hitch finder's samples (tools/hitch/hitch.py reads them). Each frame, the
		// pictures between it and the frame before at t = 0.25, 0.5, 0.75 and 1 (as smooth motion would show them, 4 a
		// frame at even moments), and where the 3D is on average in each: the mean of the models' blended places in the
		// view, as its move since the sample before. A line a sample: "s <frame> <k> <blended> <dx> <dy> <dz> <models>
		// <depth>"; a line a frame: "p <frame> <total> <no_key> <turned> <moved> <matched>" (interpolate's stats) and
		// "c <frame> <share> <move> <n> <turn>" (a real cut's signs, from the two frames alone: the share of the models
		// found in the frame before, by address, and of the n alone at their address the median move over its depth and
		// the median turn, the most a matrix element changed).
		// VF3_HITCH_STRIP=dir,from-to,... (frames): those frames' samples drawn in a strip (dir/strip_<from>.png: a row a
		// frame, its 4 samples across, at half size (VF3_HITCH_STRIP_FULL: full), a red bar over those shown unblended),
		// the pictures to look at before believing a flag. VF3_HITCH_DEBUG=f,...: those frames' viewports' model counts.
		if (static const char *hitch_path = getenv("VF3_HITCH"); hitch_path) {
			static FILE *hl = fopen(hitch_path, "w");
			static video::Scene hs[2], hm;
			static int hc = 0;
			static bool have = false;
			hc ^= 1;
			scene_builder.build(board, hs[hc]);
			struct Strip { long from, to; std::vector<uint32_t> px; };
			static std::vector<Strip> strips;
			static std::string strip_dir;
			if (static bool parsed = false; !parsed) {
				parsed = true;
				if (const char *sp = getenv("VF3_HITCH_STRIP")) {
					std::string s = sp;
					size_t c = s.find(',');
					strip_dir = s.substr(0, c);
					while (c != std::string::npos) {
						long a = 0, b = 0;
						if (sscanf(s.c_str() + c + 1, "%ld-%ld", &a, &b) == 2) strips.push_back({ a, b, {} });
						c = s.find(',', c + 1);
					}
				}
			}
			// The models measured: all the viewports', but for a swarm's grains (under 40 vertices: the beach's spray
			// and Wolf's desert sand, hundreds of 4-6 polygon models put somewhere new each frame, a viewport of their
			// own at the beach: their mean was the swarm's)
			auto counted = [](const video::Model &m) {
				size_t nv = 0;
				if (m.meshes) for (auto &me : *m.meshes) nv += me.verts.size();
				return nv >= 40;
			};
			if (have) {
				const video::Scene &A = hs[hc ^ 1], &B = hs[hc];
				video::InterpStats st;
				std::vector<std::vector<std::array<float, 3>>> last;   // (the models' places in the sample before)
				std::vector<std::vector<char>> shown_as_b;             // (paired, but shown where they are in b: left out)
				for (int k = 0; k < 4; k++) {
					const float t = float(k + 1) / 4;
					const bool blended = video::interpolate(A, B, t, hm, k == 0 ? &st : nullptr);
					const video::Scene &S = blended ? hm : B;
					// The mean of the models' moves since the sample before (a's partner's place before the first): the move
					// of the mean place over the models b has a partner for, the same set in each of the 4 (one appearing or
					// gone moved the mean of all, a lurch each frame that isn't one). A model paired but not blended (one too
					// far in a frame) is left out, or its jump was a lurch each frame; when none is blended (a cut), all are.
					std::vector<int> av;
					const std::vector<std::vector<int>> *paired = video::interp_paired(A, B, &av);
					double move[3] = { 0, 0, 0 }, depth = 0;
					size_t n = 0;
					if (k == 0) { last.assign(S.viewports.size(), {}); shown_as_b.assign(S.viewports.size(), {}); }
					for (size_t v = 0; paired && v < S.viewports.size(); v++) {
						if (av[v] < 0) continue;
						const std::vector<video::Model> &now = S.viewports[v].models, &was = A.viewports[size_t(av[v])].models, &bm = B.viewports[v].models;
						const std::vector<int> &pr = (*paired)[v];
						if (k == 0) {
							last[v].assign(now.size(), {});
							shown_as_b[v].assign(now.size(), 0);
							for (size_t j = 0; j < now.size() && j < pr.size(); j++) {
								if (pr[j] < 0) continue;
								const video::Model &p = was[size_t(pr[j])], &m = bm[j];
								for (int r = 0; r < 3; r++) last[v][j][size_t(r)] = p.matrix.m[12 + r];
								shown_as_b[v][j] = blended && memcmp(now[j].matrix.m, m.matrix.m, sizeof m.matrix.m) == 0 &&
								                   memcmp(p.matrix.m, m.matrix.m, sizeof m.matrix.m) != 0;
							}
						}
						for (size_t j = 0; j < now.size() && j < pr.size(); j++) {
							if (pr[j] < 0 || shown_as_b[v][j] || !counted(bm[j])) continue;
							for (int r = 0; r < 3; r++) {
								move[r] += now[j].matrix.m[12 + r] - last[v][j][size_t(r)];
								last[v][j][size_t(r)] = now[j].matrix.m[12 + r];
							}
							depth += std::fabs(now[j].matrix.m[14]);
							n++;
						}
					}
					if (n) {
						for (double &x : move) x /= double(n);
						depth /= double(n);
					}
					fprintf(hl, "s %llu %d %d %.4f %.4f %.4f %zu %.2f\n", (unsigned long long)f, k, blended ? 1 : 0, move[0], move[1], move[2], n, depth);
					for (Strip &sp : strips) {
						if (long(f) < sp.from || long(f) > sp.to) continue;
						static const int z = getenv("VF3_HITCH_STRIP_FULL") ? 1 : 2;   // (full size: a swarm's grains seen)
						const int w = video::W / z, h = video::H / z, row = int(long(f) - sp.from);
						sp.px.resize(size_t(w) * 4 * h * size_t(sp.to - sp.from + 1), 0xFF000000u);
						raster.render(S, board.tex_ram.data());
						std::vector<uint32_t> o(size_t(video::W) * video::H, 0xFF000000u);
						raster.compose(o.data(), o.data());
						for (int y = 0; y < h; y++)
							for (int x = 0; x < w; x++) {
								// (z by z averaged, and a sample shown unblended marked by a red bar along its top)
								uint32_t c = 0;
								for (int sh = 0; sh < 24; sh += 8) {
									int s = 0;
									for (int dy = 0; dy < z; dy++) for (int dx = 0; dx < z; dx++) s += (o[size_t(z * y + dy) * video::W + z * x + dx] >> sh) & 255;
									c |= uint32_t(s / (z * z)) << sh;
								}
								if (!blended && y < 4) c = 0xFF0000FFu;
								sp.px[(size_t(row) * h + y) * w * 4 + size_t(k) * w + x] = c | 0xFF000000u;
							}
						if (long(f) == sp.to && k == 3)
							write_png((strip_dir + "/strip_" + std::to_string(sp.from) + ".png").c_str(), sp.px.data(), w * 4, h * int(sp.to - sp.from + 1));
					}
				}
				if (static const char *dbg = getenv("VF3_HITCH_DEBUG"); dbg && strstr((std::string(",") + dbg + ",").c_str(), ("," + std::to_string(f) + ",").c_str())) {
					fprintf(stderr, "hitch %llu: viewports' models", (unsigned long long)f);
					for (auto &vp : A.viewports) fprintf(stderr, " %zu", vp.models.size());
					fprintf(stderr, " ->");
					for (auto &vp : B.viewports) fprintf(stderr, " %zu", vp.models.size());
					fprintf(stderr, "\n");
					size_t same = 0, n = 0;
					for (size_t v = 0; v < B.viewports.size() && v < A.viewports.size(); v++)
						for (size_t j = 0; j < B.viewports[v].models.size() && j < A.viewports[v].models.size(); j++) {
							n++;
							same += memcmp(B.viewports[v].models[j].matrix.m, A.viewports[v].models[j].matrix.m, sizeof(float) * 16) == 0;
						}
					fprintf(stderr, "  %zu of %zu models' matrices the same, in order\n", same, n);
				}
				{
					// The flicker detector: each of b's models with a partner in a, by kind, as the in-betweens show it:
					// blended, or as it is in b while the rest are blended (as-is), and its move over the frame. Flagged:
					//   toggle: blended in one frame and as-is in the next, or the other way (the partner's status the frame
					//           before), the model alternating between gliding and jumping (both frames blended: a cut
					//           is the hitch finder's)
					//   jitter: blended both frames, its move unlike its move the frame before (they differ by over half the
					//           larger and 3% of its distance): a zigzag in the in-betweens
					//   stray:  blended across more than 4 times its own size and 2% of its distance in a frame: drawn
					//           along a line between two unrelated models (a swarm's grain paired with another)
					// A line a frame and kind: "f <frame> <kind> <models> <blended> <as-is> <toggle> <jitter> <stray>".
					// Kinds: 0 grain (under 40 vertices), 1 joint (built in polygon RAM), 2 flipbook (its texture moved),
					// 3 translucent (shadows, effects), 4 cut-out (alpha-textured: grass, leaves, flags), 5 small (under 300
					// vertices), 6 the rest (the fighters' limbs, the stage).
					struct Rec { char status = 0; float vel[3] = {}; };   // (status 0: no partner, 1 blended, 2 as-is)
					static std::vector<std::vector<Rec>> prev_rec;
					static bool prev_blended = false;
					std::vector<std::vector<Rec>> rec(B.viewports.size());
					std::vector<int> av;
					const std::vector<std::vector<int>> *paired = video::interp_paired(A, B, &av);
					video::Scene half;
					const bool blended = video::interpolate(A, B, 0.5f, half);
					long cnt[7][6] = {};
					for (size_t v = 0; v < B.viewports.size(); v++) {
						const std::vector<video::Model> &bm = B.viewports[v].models;
						rec[v].resize(bm.size());
						if (!paired || av[v] < 0) continue;
						const std::vector<video::Model> &am = A.viewports[size_t(av[v])].models;
						const std::vector<int> &pr = (*paired)[v];
						for (size_t j = 0; j < bm.size() && j < pr.size(); j++) {
							if (pr[j] < 0) continue;
							const video::Model &m = bm[j], &p = am[size_t(pr[j])], &h = half.viewports[v].models[j];
							size_t nv = 0;
							bool trans = m.alpha < 1, cutout = false, flip = false;
							float size = 0;
							if (m.meshes)
								for (size_t k = 0; k < m.meshes->size(); k++) {
									const video::Mesh &me = (*m.meshes)[k];
									nv += me.verts.size();
									trans |= me.poly_alpha;
									cutout |= me.texture_alpha;
									flip |= me.flip > 0 || (h.meshes && k < h.meshes->size() && (*h.meshes)[k].flip > 0);
									if (p.meshes && p.meshes->size() == m.meshes->size() && ((*p.meshes)[k].x != me.x || (*p.meshes)[k].y != me.y)) flip = true;
									for (auto &vx : me.verts) size = std::max(size, vx.pos[0] * vx.pos[0] + vx.pos[1] * vx.pos[1] + vx.pos[2] * vx.pos[2]);
								}
							const float scale = std::sqrt(m.matrix.m[0] * m.matrix.m[0] + m.matrix.m[1] * m.matrix.m[1] + m.matrix.m[2] * m.matrix.m[2]);
							size = std::sqrt(size) * scale;
							const int kind = nv < 40 ? 0 : m.addr < 0x100000 ? 1 : flip ? 2 : trans ? 3 : cutout ? 4 : nv < 300 ? 5 : 6;
							Rec &r = rec[v][j];
							const bool same = memcmp(h.matrix.m, m.matrix.m, sizeof m.matrix.m) == 0;
							const bool moves = memcmp(p.matrix.m, m.matrix.m, sizeof m.matrix.m) != 0;
							if (!moves) { r.status = 0; continue; }   // (standing still: nothing to see)
							r.status = blended && !same ? 1 : 2;
							float d = 0, z = 0;
							for (int c = 0; c < 3; c++) { r.vel[c] = m.matrix.m[12 + c] - p.matrix.m[12 + c]; d += r.vel[c] * r.vel[c]; z += m.matrix.m[12 + c] * m.matrix.m[12 + c]; }
							d = std::sqrt(d);
							z = std::sqrt(z);
							long *c = cnt[kind];
							c[0]++;
							c[r.status]++;
							const Rec *q = size_t(av[v]) < prev_rec.size() && size_t(pr[j]) < prev_rec[size_t(av[v])].size() ? &prev_rec[size_t(av[v])][size_t(pr[j])] : nullptr;
							if (q && q->status && q->status != r.status && blended && prev_blended) c[3]++;
							if (q && q->status == 1 && r.status == 1) {
								float e = 0, dq = 0;
								for (int k = 0; k < 3; k++) { e += (r.vel[k] - q->vel[k]) * (r.vel[k] - q->vel[k]); dq += q->vel[k] * q->vel[k]; }
								e = std::sqrt(e);
								if (e > 0.5f * std::max(d, std::sqrt(dq)) && e > 0.03f * z) c[4]++;
							}
							if (r.status == 1 && d > 4 * size && d > 0.02f * z) c[5]++;
						}
					}
					prev_rec = std::move(rec);
					prev_blended = blended;
					for (int k = 0; k < 7; k++)
						if (cnt[k][0]) fprintf(hl, "f %llu %d %ld %ld %ld %ld %ld %ld\n", (unsigned long long)f, k, cnt[k][0], cnt[k][1], cnt[k][2], cnt[k][3], cnt[k][4], cnt[k][5]);
				}
				fprintf(hl, "p %llu %zu %zu %zu %zu %zu\n", (unsigned long long)f, st.total, st.no_key, st.turned, st.moved, st.matched);
				{
					// A real cut's signs, from the two frames alone (each of b's viewports with a's with most addresses in
					// common): the share of b's models found in a, and the median move over depth of those alone at their
					// address in both
					size_t all = 0, found = 0;
					std::vector<float> rel, turn;
					for (auto &vb : B.viewports) {
						size_t best = 0, bu = 0;
						for (size_t u = 0; u < A.viewports.size(); u++) {
							std::map<uint32_t, int> left;
							for (auto &m : A.viewports[u].models) if (counted(m)) left[m.addr]++;
							size_t common = 0;
							for (auto &m : vb.models) if (counted(m)) if (auto it = left.find(m.addr); it != left.end() && it->second > 0) { it->second--; common++; }
							if (common > best) { best = common; bu = u; }
						}
						for (auto &m : vb.models) all += counted(m);
						found += best;
						if (!best) continue;
						std::map<uint32_t, int> na, nb;
						std::map<uint32_t, const video::Model *> at;
						for (auto &m : A.viewports[bu].models) if (counted(m)) { na[m.addr]++; at[m.addr] = &m; }
						for (auto &m : vb.models) if (counted(m)) nb[m.addr]++;
						for (auto &m : vb.models) {
							if (!counted(m) || nb[m.addr] != 1 || na[m.addr] != 1) continue;
							const video::Model &p = *at[m.addr];
							float d = 0, z = 0;
							for (int r = 0; r < 3; r++) { const float e = m.matrix.m[12 + r] - p.matrix.m[12 + r]; d += e * e; z += m.matrix.m[12 + r] * m.matrix.m[12 + r]; }
							rel.push_back(std::sqrt(d / std::max(z, 1.0f)));
							float most = 0;
							for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) most = std::max(most, std::fabs(m.matrix.m[c * 4 + r] - p.matrix.m[c * 4 + r]));
							turn.push_back(most);
						}
					}
					float med = 1;
					if (!rel.empty()) { std::nth_element(rel.begin(), rel.begin() + rel.size() / 2, rel.end()); med = rel[rel.size() / 2]; }
					float med_turn = 0;
					if (!turn.empty()) { std::nth_element(turn.begin(), turn.begin() + turn.size() / 2, turn.end()); med_turn = turn[turn.size() / 2]; }
					fprintf(hl, "c %llu %.3f %.4f %zu %.3f\n", (unsigned long long)f, all ? double(found) / double(all) : 1.0, med, rel.size(), med_turn);
				}
			}
			have = true;
		}
		static const long flicker_from = getenv("VF3_FLICKER_FROM") ? atol(getenv("VF3_FLICKER_FROM")) : 0;   // (tests: checked from this frame)
		if (flicker_dir && long(f) >= flicker_from) {   // VF3_FLICKER: the in-between pictures (t = 0.5) against the frames either side
			static video::Scene sc[2], mid;
			static std::vector<uint32_t> pf[2], pm;
			static int cur = 0;
			static bool have = false;
			auto shoot = [&](const video::Scene &s, std::vector<uint32_t> &o) {
				raster.render(s, board.tex_ram.data());
				o.assign(size_t(video::W) * video::H, 0xFF000000u);
				raster.compose(o.data(), o.data());
			};
			cur ^= 1;
			scene_builder.build(board, sc[cur]);
			static const bool mids_only = getenv("VF3_MIDS_ONLY") != nullptr;   // (just VF3_MIDS' pictures)
			if (!mids_only) shoot(sc[cur], pf[cur]);
			// VF3_MIDS=f,f,...: at those frames the in-between picture (t = 0.5) and the frame, at 2x, cropped to the 3D's
			// middle (mid_<f>.png): the fighters' joints between frames, by eye
			if (have && getenv("VF3_MIDS") && strstr((std::string(",") + getenv("VF3_MIDS") + ",").c_str(), ("," + std::to_string(f) + ",").c_str())) {
				static video::Raster two;
				two.resize(video::W * 2, video::H * 2);
				video::Scene half;
				video::interpolate(sc[cur ^ 1], sc[cur], 0.5f, half);
				std::vector<uint32_t> both(size_t(two.width()) * 2 * two.height());
				for (int k = 0; k < 2; k++) {
					two.render(k ? sc[cur] : half, board.tex_ram.data());
					std::vector<uint32_t> o(size_t(two.width()) * two.height(), 0xFF404040u);
					two.compose(o.data(), o.data());
					for (int y = 0; y < two.height(); y++)
						memcpy(&both[size_t(y) * two.width() * 2 + (k ? two.width() : 0)], &o[size_t(y) * two.width()], size_t(two.width()) * 4);
				}
				write_png((std::string(flicker_dir) + "/mid_" + std::to_string(f) + ".png").c_str(), both.data(), two.width() * 2, two.height());
			}
			// VF3_SEQ=from,to (tests, with VF3_FLICKER's folder): each frame and the in-between before it (t = 0.5), as smooth
			// motion shows them in turn: seq_<f>_0.png (between), seq_<f>_1.png (the frame)
			if (const char *sq = getenv("VF3_SEQ"); sq && have && flicker_dir) {
				long a = 0, b = 0;
				sscanf(sq, "%ld,%ld", &a, &b);
				if (long(f) >= a && long(f) <= b) {
					video::Scene half;
					video::interpolate(sc[cur ^ 1], sc[cur], getenv("VF3_MID_T") ? float(atof(getenv("VF3_MID_T"))) : 0.5f, half);
					std::vector<uint32_t> o;
					shoot(half, o);
					write_png((std::string(flicker_dir) + "/seq_" + std::to_string(f) + "_0.png").c_str(), o.data(), video::W, video::H);
					write_png((std::string(flicker_dir) + "/seq_" + std::to_string(f) + "_1.png").c_str(), pf[cur].data(), video::W, video::H);
				}
			}
			static const long gaps_at = getenv("VF3_GAPS") ? atol(getenv("VF3_GAPS")) : -1;
			if (have && gaps_at >= 0 && long(f) >= gaps_at && long(f) < gaps_at + 4) {
				static video::Raster big;
				big.resize(video::W * 3, video::H * 3);
				auto shoot3 = [&](const video::Scene &s, const char *tag) {
					big.render(s, board.tex_ram.data());
					std::vector<uint32_t> o(size_t(big.width()) * big.height(), 0xFF404040u);
					big.compose(o.data(), o.data());
					write_png((std::string(flicker_dir) + "/gap_" + std::to_string(f) + tag + ".png").c_str(), o.data(), big.width(), big.height());
				};
				video::Scene half;
				video::InterpStats is;
				video::interpolate(sc[cur ^ 1], sc[cur], 0.5f, half, &is);
				shoot3(sc[cur], "_frame");
				shoot3(half, "_mid");
				fprintf(stderr, "gaps %llu: turned %zu moved %zu nokey %zu of %zu\n", (unsigned long long)f, is.turned, is.moved, is.no_key, is.total);
			}
			if (have && !mids_only) {
				video::InterpStats ist;
				static const float mid_t = getenv("VF3_MID_T") ? float(atof(getenv("VF3_MID_T"))) : 0.5f;   // (tests: another in-between)
				const auto ti0 = std::chrono::steady_clock::now();
				const bool between = video::interpolate(sc[cur ^ 1], sc[cur], mid_t, mid, &ist);
				{
					static double tot = 0, worst = 0; static long n = 0;
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ti0).count();
					tot += ms; worst = std::max(worst, ms); n++;
					if (getenv("VF3_INTERP_TIME") && n % 100 == 0) fprintf(stderr, "interpolate: %ld calls, %.3f ms each, worst %.3f\n", n, tot / n, worst);
				}
				{
					const video::Scene &A = sc[cur ^ 1], &B = sc[cur];
					auto miss = [&](auto key) {
						std::map<uint64_t, int> ka;
						for (size_t v = 0; v < A.viewports.size(); v++) for (auto &m : A.viewports[v].models) ka[key(v, m)]++;
						int n = 0;
						for (size_t v = 0; v < B.viewports.size(); v++) for (auto &m : B.viewports[v].models) { auto it = ka.find(key(v, m)); if (it == ka.end() || it->second == 0) n++; else it->second--; }
						return n;
					};
					int m_addr = miss([](size_t v, const video::Model &m) { return (uint64_t(v) << 56) ^ m.addr; });
					int m_path = miss([](size_t v, const video::Model &m) { return (uint64_t(v) << 56) ^ m.node_path; });
					int dyn = 0;
					for (auto &vp : B.viewports) for (auto &m : vp.models) dyn += m.addr < 0x100000;
					fprintf(flicker_log, "alt %llu addr-only-miss %d path-only-miss %d dynamic %d\n", (unsigned long long)f, m_addr, m_path, dyn);
				}
				fprintf(flicker_log, "stats %llu total %zu nokey %zu turned %zu moved %zu matched %zu blended %zu not_blended %zu\n", (unsigned long long)f, ist.total, ist.no_key, ist.turned, ist.moved, ist.matched, ist.blended, ist.not_blended);
				shoot(mid, pm);
				auto diff = [](uint32_t a, uint32_t b) {
					int d = 0;
					for (int sh = 0; sh < 24; sh += 8) d = std::max(d, std::abs(int((a >> sh) & 255) - int((b >> sh) & 255)));
					return d;
				};
				// pixels the in-between has that neither frame has (the two frames agreeing there)
				long n = 0;
				const std::vector<uint32_t> &a = pf[cur ^ 1], &b = pf[cur];
				for (size_t i = 0; i < pm.size(); i++)
					if (diff(pm[i], a[i]) > 48 && diff(pm[i], b[i]) > 48 && (getenv("VF3_FLICKER_ANY") || diff(a[i], b[i]) <= 16)) n++;
				// models sharing a key (viewport, nodes, address), paired in order: how many have a nearer one
				long dups = 0, wrong = 0;
				{
					const video::Scene &A = sc[cur ^ 1], &B = sc[cur];
					std::map<uint64_t, std::vector<const video::Model *>> ka, kb;
					for (size_t v = 0; v < A.viewports.size(); v++) for (auto &m : A.viewports[v].models) ka[(uint64_t(v) << 56) ^ (uint64_t(m.node_path) << 24) ^ m.addr].push_back(&m);
					for (size_t v = 0; v < B.viewports.size(); v++) for (auto &m : B.viewports[v].models) kb[(uint64_t(v) << 56) ^ (uint64_t(m.node_path) << 24) ^ m.addr].push_back(&m);
					auto d2 = [](const video::Model *x, const video::Model *y) { float d = 0; for (int r = 0; r < 3; r++) { float t = x->matrix.m[12 + r] - y->matrix.m[12 + r]; d += t * t; } return d; };
					for (auto &[k, bs] : kb) {
						auto it = ka.find(k);
						if (it == ka.end() || bs.size() < 2) continue;
						for (size_t i = 0; i < bs.size() && i < it->second.size(); i++) {
							dups++;
							float best = 1e30f;
							for (auto *q : it->second) best = std::min(best, d2(bs[i], q));
							if (d2(bs[i], it->second[i]) > 4 * best + 1) wrong++;
						}
					}
				}
				fprintf(flicker_log, "%llu %d %ld %ld %ld\n", (unsigned long long)f, between ? 1 : 0, n, dups, wrong);
				if (n > flicker_save_min && flicker_saved < 40) {
					flicker_saved++;
					const std::string base = std::string(flicker_dir) + "/f" + std::to_string(f);
					write_png((base + "_a.png").c_str(), a.data(), video::W, video::H);
					write_png((base + "_m.png").c_str(), pm.data(), video::W, video::H);
					write_png((base + "_b.png").c_str(), b.data(), video::W, video::H);
				}
			}
			have = true;
		}
		if (getenv("VF3_PANO") && long(board.frame) == atol(getenv("VF3_PANO"))) {   // (tests: the stage's backdrop map, as the game keeps it)
			const uint32_t r13 = board.cpu.s.r[13];
			const int stage = int16_t(board.peek16(r13 - board.profile->backdrops.stage_below_r13));
			const uint32_t desc = stage >= 0 ? board.peek32(board.profile->backdrops.descriptors + 4 * stage) : 0;
			const int w = board.peek16(desc + 4), h = board.peek16(desc + 6);
			fprintf(stderr, "pano r13 %08x stage %d desc %08x w %d h %d\n", r13, stage, desc, w, h);
			auto entry = [&](int x, int y) {
				const uint16_t e = board.peek16(desc + 8 + 2 * uint32_t(std::clamp(y, 0, h - 1) * w + ((x % w) + w) % w));
				return int((e & 0x7F) + int16_t(board.peek16(board.profile->backdrops.banks + ((e >> 6) & 0xFE))));
			};
			const uint32_t *vr = reinterpret_cast<const uint32_t *>(board.tile_ram.data()) + (0xF8000 + 3 * 0x2000) / 4;
			int tm[4][64];
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 64; c++) { const uint32_t wd = vr[(r + 8) * 32 + c / 2]; tm[r][c] = int(((c & 1) ? wd & 0xFFFF : wd >> 16) & 0x3FFF); }
			if (const char *pr = getenv("VF3_PANO_ROWS")) {   // (tests: "x0,y0": each layer row's tiles unlike the map there)
				int x0, y0;
				sscanf(pr, "%d,%d", &x0, &y0);
				for (int lrow = 0; lrow < 64; lrow++) {
					int bad = 0, first = -1;
					for (int c = 0; c < 64; c++) {
						const uint32_t wd = vr[lrow * 32 + c / 2];
						const int t = int(((c & 1) ? wd & 0xFFFF : wd >> 16) & 0x3FFF);
						if ((entry(x0 + c, y0 + lrow) & 0x3FFF) != t) { bad++; if (first < 0) first = c; }
					}
					fprintf(stderr, "prow %d bad %d first %d\n", lrow, bad, first);
					if (bad > 32 && getenv("VF3_PANO_ROWX"))   // (where along the map this row is, if anywhere)
						for (int y = -1; y <= h; y++)
							for (int x = 0; x < w; x++) {
								int b2 = 0;
								for (int c = 0; c < 64 && b2 < 8; c++) {
									const uint32_t wd = vr[lrow * 32 + c / 2];
									b2 += (entry(x + c, y) & 0x3FFF) != int(((c & 1) ? wd & 0xFFFF : wd >> 16) & 0x3FFF);
								}
								if (b2 < 8) fprintf(stderr, "   row %d at map %d,%d (%d bad)\n", lrow, x, y, b2);
							}
				}
			}
			if (const char *rr = getenv("VF3_PANO_ROW")) {   // (tests: a map row against a layer row, x0, y0 given)
				int lrow, x0, y0;
				sscanf(rr, "%d,%d,%d", &lrow, &x0, &y0);
				for (int c = 0; c < 64; c++) {
					const uint32_t wd = vr[lrow * 32 + c / 2];
					const uint16_t e = board.peek16(desc + 8 + 2 * uint32_t((y0 + lrow) * w + ((x0 + c) % w)));
					fprintf(stderr, "c%d map e%04x(b%d i%d)=%04x tile %04x\n", c, e, (e >> 7) & 0x7F, e & 0x7F, entry(x0 + c, y0 + lrow) & 0x3FFF, ((c & 1) ? wd & 0xFFFF : wd >> 16) & 0x3FFF);
				}
				for (int i = 0; i < 40; i++) fprintf(stderr, "bank %d %04x\n", i, board.peek16(board.profile->backdrops.banks + 2 * i));
			}
			if (getenv("VF3_PANO_ALL"))   // (tests: every stage's map looked through for the layer's rows)
				for (int st = 0; st < 96; st++) {
					const uint32_t d2 = board.peek32(board.profile->backdrops.descriptors + 4 * st);
					if (d2 < 0xFF000000u) continue;
					const int w2 = board.peek16(d2 + 4), h2 = board.peek16(d2 + 6);
					if (w2 < 32 || w2 > 2048 || h2 < 1 || h2 > 256) continue;
					for (int y = 0; y < h2; y++)
						for (int x = 0; x < w2; x++) {
							int ok = 0;
							for (int c = 0; c < 60; c += 4) {
								const uint16_t e = board.peek16(d2 + 8 + 2 * uint32_t(y * w2 + (x + c) % w2));
								ok += ((e & 0x7F) + board.peek16(board.profile->backdrops.banks + ((e >> 6) & 0xFE)) & 0x3FFF) == tm[0][c];
							}
							if (ok >= 13) fprintf(stderr, "pano-all stage %d desc %08x %dx%d row %d col %d: %d/15\n", st, d2, w2, h2, y, x, ok);
						}
				}
			for (int y = -2; y < h + 2; y++)
				for (int x = 0; x < w; x++) {
					int ok = 0;
					for (int c = 0; c < 60; c++) ok += entry(x + c, y) == tm[0][c];
					if (ok >= 55) {
						int ok2 = 0;
						for (int r = 1; r < 4; r++) for (int c = 0; c < 60; c++) ok2 += entry(x + c, y + r) == tm[r][c];
						fprintf(stderr, "pano match map row %d (tilemap row 8) col %d: %d/60, next rows %d/180\n", y, x, ok, ok2);
					}
				}
		}
		if (getenv("VF3_SKY_AUDIT") && !shots.empty())   // (tests: the widescreen backdrop's state each frame)
		{
			static long r0, f0, b0, n0;
			fprintf(stderr, "sky %llu %s rows %ld refind %ld bad %ld nomap %ld\n", (unsigned long long)board.frame, tiles.audit(board).c_str(),
			        tiles.sky_rows - r0, tiles.sky_refinds - f0, tiles.sky_bad_rows - b0, tiles.sky_nomap_rows - n0);
			r0 = tiles.sky_rows; f0 = tiles.sky_refinds; b0 = tiles.sky_bad_rows; n0 = tiles.sky_nomap_rows;
		}
		if (getenv("VF3_MEMLOG") && board.frame % 30 == 0) {   // (tests: the widescreen sides' share drawn from what was seen)
			static long d0 = 0, s0 = 0;
			fprintf(stderr, "mem %llu %.2f\n", (unsigned long long)board.frame, tiles.mem_sides > s0 ? double(tiles.mem_drawn - d0) / double(tiles.mem_sides - s0) : -1.0);
			d0 = tiles.mem_drawn; s0 = tiles.mem_sides;
		}
		if (getenv("VF3_BGSCROLL") && !shots.empty()) {   // the layer below the 3D's move each frame (smooth motion's)
			static video::TileGen::Scroll before;
			static std::vector<uint32_t> before_px, now_px, a_px;
			tiles.aligned(now_px, a_px);
			float dx, dy;
			const bool ok = video::TileGen::scrolled(before, tiles.scroll, before_px, now_px, dx, dy);
			before_px = now_px;
			// and the camera's turn (the 3D's full-screen viewport): yaw and pitch, in pixels at its focal length
			static video::Scene sc_before, sc_now;
			scene_builder.build(board, sc_now);
			float yaw_px = 0, pitch_px = 0;
			int cam = 0;
			for (size_t v = 0; v < sc_now.viewports.size() && v < sc_before.viewports.size(); v++) {
				const auto &vb = sc_now.viewports[v];
				if (!vb.full_screen || vb.models.size() < 8) continue;
				video::Mat4 c;
				if (!video::camera_move(sc_before.viewports[v], vb, c)) continue;
				const float fx = video::W / (vb.right - vb.left), fy = video::H / (vb.top - vb.bottom);
				yaw_px = fx * std::atan2(c.m[8], c.m[0]);
				pitch_px = fy * std::atan2(-c.m[9], c.m[10]);
				cam = 1;
				const auto &va = sc_before.viewports[v];
				if (va.root_matrix >= 0 && va.root_matrix == vb.root_matrix) {
					video::Mat4 inv = video::Mat4::identity();
					// (the root's rotation only: its delta, b times a's transpose)
					video::Mat4 ra = va.root;
					for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) inv.m[j * 4 + i] = ra.m[i * 4 + j];
					const video::Mat4 d = vb.root * inv;
					printf("root %llu %d %.2f %.2f\n", (unsigned long long)f, vb.root_matrix, fx * std::atan2(d.m[8], d.m[0]), fy * std::atan2(-d.m[9], d.m[10]));
				}
				break;
			}
			// the sky's lead (sky_lead.h): the backdrop's move with it, against the camera's turn the same frame
			static video::SkyLead lead;
			const float before_x = lead.at_x(1), before_y = lead.at_y(1);
			lead.frame(sc_before, sc_now, ok, dx, dy);
			float tx = 0, ty = 0;
			const bool tk = video::SkyLead::turn(sc_before, sc_now, tx, ty);
			sc_before = sc_now;
			printf("bg %llu %d %.2f %.2f cam %d %.2f %.2f lead %.2f %.2f trust %.2f moved %.2f %.2f turn %d %.2f %.2f delay %d\n", (unsigned long long)f, ok ? 1 : 0, dx, dy, cam, yaw_px,
				pitch_px, lead.at_x(1), lead.at_y(1), lead.trust, dx + lead.at_x(1) - before_x, dy + lead.at_y(1) - before_y, tk ? 1 : 0, tx, ty, video::SkyLead::DELAY);
			if (getenv("VF3_BGLINES") && long(f) >= atol(getenv("VF3_BGLINES")) && long(f) < atol(getenv("VF3_BGLINES")) + 4) {
				int layer = 0;
				for (int k = 1; k < 4; k++) if (tiles.scroll.below_columns[k] > tiles.scroll.below_columns[layer]) layer = k;
				printf("  layer %d:", layer);
				for (int y = 0; y < video::H; y++) if (y == 0 || tiles.scroll.sx[layer][y] != tiles.scroll.sx[layer][y - 1] || tiles.scroll.sy[layer][y] != tiles.scroll.sy[layer][y - 1]) printf(" y%d=(%d,%d)", y, tiles.scroll.sx[layer][y], tiles.scroll.sy[layer][y]);
				printf("\n");
			}
			before = tiles.scroll;
		}
		if (getenv("VF3_COVER") && !shots.empty() && f % 10 == 0) {   // the 2D over the 3D: its share, but for full rows
			static std::vector<uint32_t> b, a;
			tiles.aligned(b, a);
			long n = 0, rows = 0;
			for (int y = 0; y < video::H; y++) {
				int c = 0;
				for (int x = 0; x < video::W; x++) c += (a[size_t(y) * video::W + x] >> 24) != 0;
				if (c == video::W) continue;
				rows++;
				n += c;
			}
			printf("cover %llu %.1f%%\n", (unsigned long long)f, rows ? 100.0 * n / (rows * video::W) : 100.0);
		}
		if (getenv("VF3_LETTERBOX") && !shots.empty() && f % 10 == 0) {   // frames with full-width bands over the 3D
			static std::vector<uint32_t> b, a;
			tiles.aligned(b, a);
			int rows = 0;
			for (int y = 0; y < video::H; y++) {
				bool across = true;
				for (int x = 0; x < video::W && across; x++) across = (a[size_t(y) * video::W + x] >> 24) != 0;
				rows += across;
			}
			if (rows >= 20 && rows < 300) printf("letterbox %llu rows %d\n", (unsigned long long)f, rows);
		}
		if (getenv("VF3_2DFLICK") && !shots.empty()) {   // the 2D: pixels that change for one frame and change back
			static std::vector<uint32_t> p1, p2, b, a, cur;
			tiles.aligned(b, a);
			cur.assign(b.size(), 0);
			for (size_t i = 0; i < cur.size(); i++) cur[i] = (a[i] >> 24) ? a[i] : (b[i] >> 24) ? b[i] : 0xFF000000u;
			if (p2.size() == cur.size()) {
				long n = 0;
				for (size_t i = 0; i < cur.size(); i++) n += p1[i] != p2[i] && p1[i] != cur[i] && p2[i] == cur[i];
				if (n > 20) {
					printf("2dflick %llu %ld\n", (unsigned long long)f - 1, n);
					static int saved = 0;
					if (f > 1400 && saved++ < 12) {
						char pth[64];
						snprintf(pth, sizeof(pth), "/flick_%06llu.png", (unsigned long long)f - 1);
						write_png((shots + pth).c_str(), p1.data(), video::W, video::H);
						snprintf(pth, sizeof(pth), "/flick_%06llu_before.png", (unsigned long long)f - 1);
						write_png((shots + pth).c_str(), p2.data(), video::W, video::H);
					}
				}
			}
			p2 = p1; p1 = cur;
		}
		if (state) {
			uint32_t s[128];
			board.cpu.dump_state(s);
			uint64_t h = board.ram_hash();
			s[114] = uint32_t(h >> 32);
			s[115] = uint32_t(h);
			s[116] = uint32_t(board.frame);
			fwrite(s, 4, 128, state);
		}
		if (!shots.empty())
			for (auto &name : act.shots) {
				scene_builder.build(board, scene);
				if (getenv("VF3_VP"))
					for (auto &v : scene.viewports) {
						printf("frame %llu vp pri %d  %.0f %.0f %.0f %.0f full %d models %zu fog %.2f %.2f %.2f dens %g start %.3f amb %.3f att %.3f ambient %.3f sun %.3f\n", (unsigned long long)board.frame, v.priority, v.vp_x, v.vp_y, v.vp_w, v.vp_h, v.full_screen, v.models.size(),
							v.fog_colour[0], v.fog_colour[1], v.fog_colour[2], v.fog_density, v.fog_start, v.fog_ambient, v.fog_attenuation, v.ambient, v.sun_intensity);
						printf("  spot colour %.3f %.3f %.3f ellipse %.1f %.1f %.1f %.1f range %g %g\n", v.spot_colour[0], v.spot_colour[1], v.spot_colour[2],
							v.spot_ellipse[0], v.spot_ellipse[1], v.spot_ellipse[2], v.spot_ellipse[3], v.spot_range[0], v.spot_range[1]);
					}
				raster.render(scene, board.tex_ram.data());
				static std::vector<uint32_t> below, above;
				tiles.aligned(below, above);
				if (const char *hn = getenv("VF3_HUD_NAMES")) {   // (tests: "P1NAME,P2NAME" under the energy bars, as netplay shows them)
					const std::string v = hn;
					const size_t comma = v.find(',');
					const std::string names[2] = { v.substr(0, comma), comma == std::string::npos ? std::string() : v.substr(comma + 1) };
					video::hud_names(board, above, names);
				}
				if (getenv("VF3_VP")) {
					for (int x : { 0, video::W - 3 }) {
						int a = 0, b = 0;
						std::string band;
						for (int y = 0; y < video::H - 2; y++) {
							bool oa = above[size_t(y) * video::W + x] >> 24, ob = below[size_t(y) * video::W + x] >> 24;
							a += oa; b += ob;
							if (y % 8 == 0) band += oa ? 'A' : ob ? 'b' : '.';
						}
						printf("frame %llu col %d above %d below %d  %s\n", (unsigned long long)board.frame, x, a, b, band.c_str());
					}
				}
				for (int i = 0; i < video::W * video::H; i++)
					picture[i] = (below[i] >> 24) ? below[i] : 0xFF000000u;
				raster.compose(picture.data(), picture.data());
				for (int i = 0; i < video::W * video::H; i++)
					if (above[i] >> 24) picture[i] = above[i];
				char p[64];
				snprintf(p, sizeof(p), "/shot_%06llu_%s.png", (unsigned long long)board.frame, name.c_str());
				write_png((shots + p).c_str(), picture.data(), video::W, video::H);
				if (getenv("VF3_VP")) {
					const uint32_t *vram = reinterpret_cast<const uint32_t *>(board.tile_ram.data());
					for (int L = 0; L < 4; L++) {
						uint32_t r = board.tile_regs[0x60 / 4 + L];
						printf("frame %llu layer %d on %d reg %08x below %d linescroll %d sx0 %d sx200 %d\n", (unsigned long long)board.frame, L, int(r >> 31), r,
							!((board.tile_regs[0x20 / 4] >> (8 + L)) & 1), int((r & 0x8000) != 0),
							int((vram[(0xF6000 + L * 0x400) / 4] >> 16) & 0xFFFF), int((vram[(0xF6000 + L * 0x400) / 4 + 100] >> 16) & 0xFFFF));
					}
					for (int y : { 0, 50, 100, 150, 200, 300 }) printf("  mask y%d %08x\n", y, vram[0xF7000 / 4 + y]);
					for (int row : { 0, 5, 10, 20, 30 }) {
						printf("  names row %d:", row);
						const uint32_t *nm = vram + (0xF8000 + 3 * 0x2000) / 4;
						for (int c = 0; c < 64; c++) { uint32_t pw = nm[row * 32 + c / 2]; printf(" %04x", (c & 1) ? pw & 0xFFFF : pw >> 16); }
						printf("\n");
					}
					snprintf(p, sizeof(p), "/below_%06llu_%s.png", (unsigned long long)board.frame, name.c_str());
					std::vector<uint32_t> bl(below);
					for (auto &v : bl) v |= 0xFF000000u;
					write_png((shots + p).c_str(), bl.data(), video::W, video::H);
				}
				if (const char *bs = getenv("VF3_BIGSHOT")) {   // the 3D alone at that scale, over grey (holes show), big_<frame>.png
					static video::Raster big;
					const int k = std::clamp(atoi(bs), 1, 8);
					big.resize(video::W * k, video::H * k);
					big.render(scene, board.tex_ram.data());
					std::vector<uint32_t> o(size_t(big.width()) * big.height(), 0xFF808080u);
					big.compose(o.data(), o.data());
					char p[64];
					snprintf(p, sizeof(p), "/big_%06llu_%s.png", (unsigned long long)board.frame, name.c_str());
					write_png((shots + p).c_str(), o.data(), big.width(), big.height());
				}
				if (getenv("VF3_WIDESHOT")) {   // the layer below the 3D widened (as widescreen's sides show it), the screen's edges marked
					char p[64];
					snprintf(p, sizeof(p), "/wide_%06llu_%s.png", (unsigned long long)board.frame, name.c_str());
					std::vector<uint32_t> wd;
					tiles.aligned_wide(wd);
					for (auto &v : wd) v |= 0xFF000000u;
					for (int y = 0; y < video::H; y += 4) wd[size_t(y) * video::WW + video::EXT] = wd[size_t(y) * video::WW + video::EXT + video::W - 1] = 0xFF0000FFu;
					write_png((shots + p).c_str(), wd.data(), video::WW, video::H);
				}
			}
		if (getenv("VF3_NVDUMP") && strstr((std::string(",") + getenv("VF3_NVDUMP") + ",").c_str(), ("," + std::to_string(board.frame) + ",").c_str())) {
			// the backup RAM and the EEPROM, to nv_<frame>.bin (backup then EEPROM)
			if (FILE *nf = fopen(("nv_" + std::to_string(board.frame) + ".bin").c_str(), "wb")) {
				fwrite(board.backup.data(), 1, board.backup.size(), nf);
				fwrite(board.eeprom.regs, 1, sizeof(board.eeprom.regs), nf);
				fclose(nf);
			}
		}
		if (getenv("VF3_TILEDUMP") && strstr((std::string(",") + getenv("VF3_TILEDUMP") + ",").c_str(), ("," + std::to_string(board.frame) + ",").c_str())) {
			// the tile generator's RAM then its registers, to tile_<frame>.bin
			if (FILE *tf = fopen(("tile_" + std::to_string(board.frame) + ".bin").c_str(), "wb")) {
				fwrite(board.tile_ram.data(), 1, board.tile_ram.size(), tf);
				fwrite(board.tile_regs, 4, 64, tf);
				fclose(tf);
			}
		}
		if (getenv("VF3_TEXDUMP") && long(board.frame) == atol(getenv("VF3_TEXDUMP"))) {   // texture RAM's first sheet as 16-bit ARGB1555
			std::vector<uint32_t> px(2048 * 1024);
			for (int i = 0; i < 2048 * 1024; i++) {
				const uint16_t t = board.tex_ram[i];
				const uint32_t r = (t >> 10) & 31, g = (t >> 5) & 31, b = t & 31;
				px[i] = 0xFF000000u | (b * 255 / 31) << 16 | (g * 255 / 31) << 8 | (r * 255 / 31);
			}
			write_png("texram.png", px.data(), 2048, 1024);
		}
		if (long(board.frame) == ram_at) {
			std::string p = "ram_" + std::to_string(board.frame) + ".bin";
			if (FILE *rf = fopen(p.c_str(), "wb")) { fwrite(board.ram.data(), 1, board.ram.size(), rf); fclose(rf); }
			p = "backup_" + std::to_string(board.frame) + ".bin";   // (the backup RAM, as Model3::backup holds it: the "b" pokes' indexes)
			if (FILE *bf = fopen(p.c_str(), "wb")) { fwrite(board.backup.data(), 1, board.backup.size(), bf); fclose(bf); }
			// (and the tile generator's RAM, then its 64 registers: the HUD's maps and patterns)
			p = "tiles_" + std::to_string(board.frame) + ".bin";
			if (FILE *tf = fopen(p.c_str(), "wb")) {
				fwrite(board.tile_ram.data(), 1, board.tile_ram.size(), tf);
				fwrite(board.tile_regs, 4, 64, tf);
				fclose(tf);
			}
		}
		if (board.cpu.s.fatal) {
			fprintf(stderr, "vf3run: frame %llu: CPU stopped: %s\n", (unsigned long long)board.frame, board.cpu.fatal_message);
			break;
		}
		if (act.quit) {
			f++;
			break;
		}
	}
	if (state)
		fclose(state);
	if (!wav_path.empty()) {
		if (FILE *wf = fopen(wav_path.c_str(), "wb")) {
			auto le32 = [&](uint32_t v) { fwrite(&v, 4, 1, wf); };
			auto le16 = [&](uint16_t v) { fwrite(&v, 2, 1, wf); };
			uint32_t bytes = uint32_t(audio.size() * 2);
			fwrite("RIFF", 1, 4, wf); le32(36 + bytes); fwrite("WAVEfmt ", 1, 8, wf);
			le32(16); le16(1); le16(2); le32(44100); le32(44100 * 4); le16(4); le16(16);
			fwrite("data", 1, 4, wf); le32(bytes);
			fwrite(audio.data(), 2, audio.size(), wf);
			fclose(wf);
		}
	}
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (!quiet)
		printf("%llu frames in %.1f s (%.0f frames a second)\n", (unsigned long long)board.frame, secs, board.frame / secs);
	if (rec && !quiet) {
		printf("interpreter instead: %llu before the boot copy, %llu outside the program, %llu without code",
		       (unsigned long long)recompiler.interp_disabled, (unsigned long long)recompiler.interp_outside,
		       (unsigned long long)recompiler.interp_unmapped);
		for (size_t i = 0; i < recompiler.unmapped.size() && i < 8; i++) printf(i ? " %05X" : " (%05X", recompiler.unmapped[i]);
		printf(recompiler.unmapped.empty() ? "\n" : ")\n");
	}
	if (const char *out = getenv("VF3_MAP_OUT"); rec && out) {   // (tests: where the interpreter ran for want of a map entry, added)
		if (FILE *f = fopen(out, "a")) { for (uint32_t a : recompiler.unmapped) fprintf(f, "%05X\n", a); fclose(f); }
	}
	return board.cpu.s.fatal ? 1 : 0;
}
