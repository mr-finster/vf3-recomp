// vf3net: VF3 netplay, two boards in one process: a host and a guest, each with its own netplay session
// (src/net) and its own recompiled code, talking through a simulated network (latency, jitter, loss), each
// playing its own side of an input script. The sessions compare their states every 30 frames as netplay does (a
// desync ends the run); at the end a third board runs the same inputs straight through, and both players'
// boards must have been in its state at every checkpoint. Reports the rollbacks and what a frame cost each side
// (the wall time of a session's step: its rollback, its save and its frame).
//   vf3net <vf3.zip> --script FILE [--frames N] [--latency MS] [--jitter MS] [--loss PERCENT] [--delay D] [--interp]
//          [--set vf3tb] [--stage] [--dural] [--dural-stage host|guest|both] [--vs-rounds N] [--hashes FILE] [--blob-out FILE] [--blob-in FILE]
// --set: another program (the zip must hold it); --stage: the match's stage select on (the adapter's stage_select_on,
// Online > Settings); --dural: Dural on the select (the adapter's dural, Cheats); --dural-stage: Cheats > Dural's stage
// (Guard) on that side's own input word, as the game does it online (src/host/dural_stage.h: inputs only, the delay its lead). The script's p1 is the host's, p2 the guest's.
// --vs-rounds N: the host's own Rounds to win (vs player), set in its backup RAM before the match, as the Arcade settings
// menu does (the match plays the blob's); --hashes FILE: the straight board's state hash every 30 frames ("frame hash",
// hex); --blob-out FILE: the host's start blob written to FILE; --blob-in FILE: the match started from FILE's blob instead
// of this build's (a cross-version test: another build's host, this build's guest and straight board, the hashes compared)
// VF3NET_WATCH=addr,... (hex): those RAM bytes on the straight run, printed as they change ("bytes FRAME v...").
#include "../src/host/input_script.h"
#include "../src/host/dural_stage.h"
#include "../src/host/vf3_netplay.h"
#include "../src/machine/model3.h"
#include "../src/net/session.h"
#include "../src/rec/ppc_rec.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// A board with its recompiled code (made again when the program changes: the PC way's patch)
struct Board
{
	m3::Model3 m;
	ppcrec::Rec rec;
	bool use_rec = true;
	std::vector<uint32_t> image;
	vf3net::Adapter adapter{ m };
	bool open(const std::string &rom, const m3::Profile *profile, std::string &error)
	{
		if (profile) m.profile = profile;
		if (!m.load(rom, error)) return false;
		adapter.program_changed = [this] {
			if (!use_rec) return;
			std::vector<uint32_t> img = m.program_image();
			if (img == image) return;
			image = std::move(img);
			if (rec.build(m.cpu, image, m.profile->map, m.profile->map_count)) m.cpu.rec = &rec;
			else m.cpu.rec = nullptr;
		};
		return true;
	}
};

struct Timing
{
	double total = 0, worst = 0;
	uint64_t n = 0;
	void add(double ms) { total += ms; worst = std::max(worst, ms); n++; }
};

} // namespace

int main(int argc, char **argv)
{
	std::string rom, script_path;
	long frames = 3000;
	vfnet::SimWire::Params p{ 40, 8, 0 };
	int delay = 2;
	bool use_rec = true, soft = false;   // soft: each side's clock a little slower while ahead, as the game's
	bool stage = false, dural = false;
	bool host_terms = false;   // --host-terms: a pool match (the host's settings in its blob's trailer, the guest's own aside)
	int guest_stage = -1, guest_dural = -1, team = -1;   // (the guest's own, before the match: -1 the host's; team: the host's Team Battle mode byte)
	dural_stage::Picker pick_a, pick_b;   // (--dural-stage: each side's own)
	const m3::Profile *profile = nullptr;
	const bool slow = getenv("VF3NET_SLOW") != nullptr;   // (each step over 40 ms, the host's)
	const char *usage = "usage: vf3net <vf3.zip> --script FILE [--frames N] [--latency MS] [--jitter MS] [--loss PERCENT] [--delay D] [--interp] [--soft] [--set vf3tb] [--stage] [--dural] [--dural-stage host|guest|both] [--vs-rounds N] [--hashes FILE] [--blob-out FILE] [--blob-in FILE] [--host-terms] [--guest-stage 0|1] [--guest-dural 0|1] [--team 1v1|3v3]\n";
	int vs_rounds = 0;
	const char *hashes_path = nullptr, *blob_out = nullptr, *blob_in = nullptr;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
		else if (a == "--latency" && i + 1 < argc) p.latency_ms = atof(argv[++i]);
		else if (a == "--jitter" && i + 1 < argc) p.jitter_ms = atof(argv[++i]);
		else if (a == "--loss" && i + 1 < argc) p.loss = atof(argv[++i]) / 100;
		else if (a == "--delay" && i + 1 < argc) delay = atoi(argv[++i]);
		else if (a == "--interp") use_rec = false;
		else if (a == "--soft") soft = true;
		else if (a == "--stage") stage = true;
		else if (a == "--dural") dural = true;
		else if (a == "--host-terms") host_terms = true;
		else if (a == "--guest-stage" && i + 1 < argc) guest_stage = atoi(argv[++i]);
		else if (a == "--guest-dural" && i + 1 < argc) guest_dural = atoi(argv[++i]);
		else if (a == "--team" && i + 1 < argc) team = std::string(argv[++i]) == "1v1" ? 1 : 0;
		else if (a == "--dural-stage" && i + 1 < argc) {
			const std::string w = argv[++i];
			pick_a.trigger = w == "host" || w == "both";
			pick_b.trigger = w == "guest" || w == "both";
		}
		else if (a == "--vs-rounds" && i + 1 < argc) vs_rounds = atoi(argv[++i]);
		else if (a == "--hashes" && i + 1 < argc) hashes_path = argv[++i];
		else if (a == "--blob-out" && i + 1 < argc) blob_out = argv[++i];
		else if (a == "--blob-in" && i + 1 < argc) blob_in = argv[++i];
		else if (a == "--set" && i + 1 < argc) {
			if (!(profile = m3::find_profile(argv[++i]))) { fprintf(stderr, "vf3net: no such set %s\n", argv[i]); return 2; }
		}
		else if (a[0] != '-' && rom.empty()) rom = a;
		else { fprintf(stderr, "vf3net: unknown argument %s\n%s", a.c_str(), usage); return 2; }
	}
	if (rom.empty() || script_path.empty()) { fprintf(stderr, "%s", usage); return 2; }

	auto pa = std::make_unique<Board>(), pb = std::make_unique<Board>();
	Board &A = *pa, &B = *pb;
	std::string error;
	A.use_rec = B.use_rec = use_rec;
	if (!A.open(rom, profile, error) || !B.open(rom, profile, error)) { fprintf(stderr, "vf3net: %s\n", error.c_str()); return 1; }

	// the script's inputs for every frame (each side plays its own player's)
	InputScript script;
	if (!script.load(script_path, error)) { fprintf(stderr, "vf3net: %s\n", error.c_str()); return 1; }
	std::vector<m3::Controls> real;
	m3::Controls c{};
	for (long f = 0; f < frames + 200; f++) {
		if (script.step(c, uint64_t(f)).quit) break;
		real.push_back(c);
	}
	frames = std::min<long>(frames, long(real.size()) - 50);

	A.m.reset();   // (the host's NVRAM: a fresh board's, as on a first run)
	for (int i = 0; i < 300 && A.m.coin_setting() < 0; i++) A.m.run_frame();
	const m3::Profile::Settings &set = A.m.profile->settings;
	const size_t rounds_at = set.at + size_t(set.rounds_vs);
	printf("the host's settings as its game wrote them: Rounds to win (vs player) %d\n", set.at ? int(A.m.backup[rounds_at]) : -1);
	if (vs_rounds && set.at) {   // (the host's own, as the Arcade settings menu sets it)
		A.m.backup[rounds_at] = uint8_t(vs_rounds);
		printf("the host's Rounds to win (vs player): %d\n", int(A.m.backup[rounds_at]));
	}
	if (team >= 0 && set.team_mode && set.at) {   // (the host's own Team Battle mode, as Game > Team Battle mode writes it)
		A.m.backup[set.at + size_t(set.team_mode)] = uint8_t(team);
		printf("the host's Team Battle mode: %s\n", team ? "1 vs 1" : "3 vs 3");
	}
	A.adapter.stage_select_on = stage;
	A.adapter.dural = dural;
	A.adapter.host_terms = B.adapter.host_terms = host_terms;
	std::vector<uint8_t> blob = A.adapter.host_blob();
	if (blob_in) {
		FILE *f = fopen(blob_in, "rb");
		if (!f) { fprintf(stderr, "vf3net: can't read %s\n", blob_in); return 1; }
		blob.clear();
		for (int ch; (ch = fgetc(f)) != EOF;) blob.push_back(uint8_t(ch));
		fclose(f);
		printf("the start blob from %s (%zu bytes)\n", blob_in, blob.size());
	}
	if (blob_out) {
		FILE *f = fopen(blob_out, "wb");
		if (!f || fwrite(blob.data(), 1, blob.size(), f) != blob.size()) { fprintf(stderr, "vf3net: can't write %s\n", blob_out); return 1; }
		fclose(f);
	}
	if (set.at && blob.size() > 4 + rounds_at) printf("the blob's Rounds to win (vs player): %d\n", int(blob[4 + rounds_at]));
	const uint64_t content = vf3net::content_hash(A.m);
	if (content != vf3net::content_hash(B.m)) { fprintf(stderr, "vf3net: the two boards' ROM hashes differ\n"); return 1; }

	vfnet::SimWire wire(p, p, 7);
	vfnet::Config cfg;
	cfg.delay = delay;
	cfg.soft_sync = soft;
	cfg.handshake_ms = 120000;   // (both boots run here one after the other, on the one clock)
	const vfnet::Identity ia{ "vf3", "test", content, "HOST" }, ib{ "vf3", "test", content, "GUEST" };
	A.adapter.checkpoint_every = B.adapter.checkpoint_every = 300;
	B.adapter.stage_select_on = guest_stage < 0 ? stage : guest_stage != 0;
	B.adapter.dural = guest_dural < 0 ? dural : guest_dural != 0;
	printf("host: stage select %d, Dural %d; the guest's own: stage select %d, Dural %d%s\n", int(stage), int(dural), int(B.adapter.stage_select_on),
	       int(B.adapter.dural), host_terms ? " (a pool match: the host's)" : "");
	vfnet::Session sa(wire.a(), A.adapter, ia, cfg, true, blob);
	vfnet::Session sb(wire.b(), B.adapter, ib, cfg, false);

	// a side's input word as its player's game makes it (Cheats > Dural's stage, if on there)
	auto own_word = [&](dural_stage::Picker &pick, m3::Model3 &m, int player, uint16_t bits) {
		dural_stage::Picker::Said said;
		int wait_s;
		const uint16_t out = pick.filter(m, player, bits, delay, false, said, wait_s);
		if (said) printf("  Dural's stage, the %s's pick: %s (%d s)\n", player ? "guest" : "host",
		                 said == dural_stage::Picker::waiting ? "waiting" : said == dural_stage::Picker::no_version ? "no Dural version" : "not this time", wait_s);
		return out;
	};
	Timing ta, tb;
	size_t na = 0, nb = 0;   // each side's frames run (its next script frame)
	double t = 1000, next_a = 1000, next_b = 1000 + 23;
	const double fa = cfg.frame_ms, fb = cfg.frame_ms * 1.002;
	const auto t0 = Clock::now();
	bool connected = false;
	vf3net::Score score_a, score_b, score_c;   // (the detector on each side, as the game runs it: after each step, guesses and all)
	while (sa.state() != vfnet::Session::State::ended && sb.state() != vfnet::Session::State::ended &&
	       (long(sa.frame()) < frames || long(sb.frame()) < frames)) {
		wire.now = t;
		sa.poll(t);
		sb.poll(t);
		if (!connected && sa.state() == vfnet::Session::State::running && sb.state() == vfnet::Session::State::running) {
			connected = true;
			printf("connected (both boots run) after %.1f s\n", std::chrono::duration<double>(Clock::now() - t0).count());
		}
		if (t >= next_a) {
			const auto s0 = Clock::now();
			const uint64_t rb0 = sa.stats().resim_frames;
			if (sa.step(own_word(pick_a, A.m, 0, vf3net::pack(real[std::min(na, real.size() - 1)], 0)))) { na++; score_a.frame(A.m); }
			const double ms = std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
			if (connected) ta.add(ms);
			if (slow && ms > 40) printf("  slow step: host frame %u, %.1f ms, %llu frames run again\n", sa.frame(), ms, (unsigned long long)(sa.stats().resim_frames - rb0));
			next_a += fa / (1 - (soft ? std::clamp(sa.stats().lead * 0.02, 0.0, 0.04) : 0.0));
		}
		if (t >= next_b) {
			const auto s0 = Clock::now();
			if (sb.step(own_word(pick_b, B.m, 1, vf3net::pack(real[std::min(nb, real.size() - 1)], 1)))) { nb++; score_b.frame(B.m); }
			if (connected) tb.add(std::chrono::duration<double, std::milli>(Clock::now() - s0).count());
			next_b += fb / (1 - (soft ? std::clamp(sb.stats().lead * 0.02, 0.0, 0.04) : 0.0));
		}
		t += 1;
	}
	vfnet::Session::Event e;
	bool failed = false;
	while (sa.next_event(e)) if (e.kind == e.ended) { printf("host: %s\n", e.text.c_str()); failed = true; }
	while (sb.next_event(e)) if (e.kind == e.ended) { printf("guest: %s\n", e.text.c_str()); failed = true; }
	printf("the guest played: stage select %d, Dural %d\n", int(B.adapter.stage_select_on), int(B.adapter.dural));

	// the same inputs straight through, on a third board, against both players' checkpoints
	const uint32_t n = std::min(sa.confirmed(), sb.confirmed());
	uint32_t checked = 0;
	if (!failed) {
		auto pc = std::make_unique<Board>();
		Board &C = *pc;
		C.use_rec = use_rec;
		C.adapter.stage_select_on = stage;
		C.adapter.dural = dural;
		C.adapter.host_terms = host_terms;
		if (!C.open(rom, profile, error) || !C.adapter.begin(blob)) { fprintf(stderr, "vf3net: the straight board didn't start\n"); return 1; }
		C.adapter.checkpoint_every = hashes_path ? 30 : 300;
		std::vector<uint32_t> watch;
		std::string watched;
		if (const char *w = getenv("VF3NET_WATCH"))   // tests: "addr,addr,..." (hex, the game's RAM addresses)
			for (const char *q = w; *q;) { char *e; watch.push_back(uint32_t(strtoul(q, &e, 16))); q = *e ? e + 1 : e; }
		int won[2] = {}, by[2] = {};   // (each versus match as it ends: its rounds)
		for (uint32_t f = 0; f < n; f++) {
			const uint16_t in[2] = { sa.inputs(0)[f], sa.inputs(1)[f] };
			if (in[0] != sb.inputs(0)[f] || in[1] != sb.inputs(1)[f]) { printf("the two sides' inputs differ at frame %u\n", f); failed = true; break; }
			C.adapter.advance(in, false);
			score_c.frame(C.m);
			if (score_c.matches[0] + score_c.matches[1] != won[0] + won[1]) {
				printf("a versus match over at frame %u: player %d won it, rounds %d-%d\n", f, score_c.matches[0] != won[0] ? 1 : 2,
				       score_c.rounds[0] - by[0], score_c.rounds[1] - by[1]);
				for (int q = 0; q < 2; q++) won[q] = score_c.matches[q], by[q] = score_c.rounds[q];
			}
			if (!watch.empty()) {   // (VF3NET_WATCH: those bytes on the straight run, each time they change)
				std::string now;
				for (uint32_t w : watch) now += " " + std::to_string(C.m.ram[w ^ 3]);
				if (now != watched) printf("bytes %u%s\n", f + 1, now.c_str());
				watched = now;
			}
		}
		// the score each side's detector kept, through its rollbacks, against the straight run's
		printf("the score: host %d-%d, guest %d-%d, straight %d-%d (rounds %d-%d)\n", score_a.matches[0], score_a.matches[1], score_b.matches[0],
		       score_b.matches[1], score_c.matches[0], score_c.matches[1], score_c.rounds[0], score_c.rounds[1]);
		for (int p = 0; p < 2; p++)
			if (score_a.matches[p] != score_c.matches[p] || score_b.matches[p] != score_c.matches[p] || score_a.rounds[p] != score_c.rounds[p] || score_b.rounds[p] != score_c.rounds[p]) {
				printf("the score differs from the straight run's\n");
				failed = true;
				break;
			}
		if (hashes_path) {
			FILE *hf = fopen(hashes_path, "w");
			if (!hf) { fprintf(stderr, "vf3net: can't write %s\n", hashes_path); return 1; }
			for (auto &[f, h] : C.adapter.checkpoints) if (f <= n) fprintf(hf, "%u %016llx\n", f, (unsigned long long)h);
			fclose(hf);
		}
		for (auto &[f, h] : C.adapter.checkpoints) {
			if (f > n) break;
			if (f % 300) continue;
			if (A.adapter.checkpoints[f] != h) { printf("the host's board differs from a straight run after frame %u\n", f); failed = true; break; }
			if (B.adapter.checkpoints[f] != h) { printf("the guest's board differs from a straight run after frame %u\n", f); failed = true; break; }
			checked++;
		}
	}
	const auto &xa = sa.stats(), &xb = sb.stats();
	printf("%s: %u frames confirmed, %u checkpoints against a straight run; %.0f ms each way, jitter %.0f, %.0f%% lost; delay %d\n",
	       failed ? "FAILED" : "in step", n, checked, p.latency_ms, p.jitter_ms, p.loss * 100, delay);
	for (int s = 0; s < 2; s++) {
		const auto &x = s ? xb : xa;
		const Timing &tm = s ? tb : ta;
		printf("  %s: ping %.0f ms, %llu rollbacks (%.1f frames on average), %llu waits for the other side, %llu to keep in time; a step %.2f ms on average, %.2f the worst (a frame has %.1f)\n",
		       s ? "guest" : "host ", x.rtt_ms, (unsigned long long)x.rollbacks, x.rollbacks ? double(x.resim_frames) / double(x.rollbacks) : 0.0,
		       (unsigned long long)x.stalls, (unsigned long long)x.waits, tm.n ? tm.total / double(tm.n) : 0.0, tm.worst, cfg.frame_ms);
	}
	return failed ? 1 : 0;
}
