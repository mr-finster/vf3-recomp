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
// --terms VERSION: the match played on the lobby's terms as Online::use_terms plays them, an older version's
// ("v0.1.7-online-preview+1v1": its stage select, Dural and Team Battle mode, whatever --stage, --dural and --team) or
// the pool's (VERSION's base with +host1: as --host-terms; "+host1+dural3": Dural on; "+host1+dural2", Team Battle's
// "+host1+dural": Dural on with v0.1.16-alpha's patches, as an older build plays it)
// VF3NET_WATCH=addr,... (hex): those RAM bytes on the straight run, printed as they change ("bytes FRAME v...").
// --watchers N: N more boards watching the match (src/net/watch.h), as the game's watchers do: the host's uploads once a
// second to a stand-in for the lobby (--watch-ms each way), each watcher asking it for the frames and running them a few
// seconds behind; the first from the start, the others joining at --watch-late F (catching up from the start, unseen).
// Each watcher's board must be the straight run's at every checkpoint, its own checks of the players' hashes all good;
// the players' rollbacks and waits are printed as ever, to be compared with a run without watchers.
// --watch-snap: the late watchers come in from the host's snapshot (watch.h's Snapper, asked for a little before they
// come, as the lobby asks when one does), not from the match's start; --snap-out DIR: the last snapshot the stand-in
// lobby had, the match's start blob and both players' inputs written to DIR (snap.bin, blob.bin, inputs.bin);
// --replay DIR: none of the above, but a watcher in this process loading DIR's snapshot as the game does
// (Spectator::snapshot) and running the inputs after it, its state hash every 30 frames to --hashes: another process's
// (another build's, another machine's) run against the first's --hashes from the snapshot's frame on.
#include "../src/host/input_script.h"
#include "../src/host/dural_stage.h"
#include "../src/host/vf3_netplay.h"
#include "../src/machine/model3.h"
#include "../src/net/session.h"
#include "../src/net/watch.h"
#include "watch_pace.h"
#include "../src/rec/ppc_rec.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// --pace old|new (with --replay): a watcher come late timed as the game runs one (watch_pace.h), --behind S seconds
// behind the match as it comes (from the snapshot's frame; --from-start: from the match's start, no snapshot), for
// --cap S seconds at most
const char *g_pace = nullptr;
double g_behind = 60, g_cap = 120;
bool g_from_start = false;

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

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	out.clear();
	for (int ch; (ch = fgetc(f)) != EOF;) out.push_back(uint8_t(ch));
	fclose(f);
	return true;
}

bool write_file(const std::string &path, const std::vector<uint8_t> &v)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	const bool ok = fwrite(v.data(), 1, v.size(), f) == v.size();
	return fclose(f) == 0 && ok;
}

// --replay DIR: a watcher in a process of its own, from DIR's snapshot (another process's host made it), through the
// game's own way (Spectator: start, snapshot, the frames after it), its state hashed every 30 frames
int replay(const std::string &rom, const m3::Profile *profile, const std::string &dir, const char *hashes_path, bool use_rec, bool stage, bool dural, bool host_terms)
{
	namespace vw = vfnet::watch;
	std::vector<uint8_t> blob, snap_bytes, in_bytes;
	if (!read_file(dir + "/blob.bin", blob) || !read_file(dir + "/snap.bin", snap_bytes) || !read_file(dir + "/inputs.bin", in_bytes)) { fprintf(stderr, "vf3net: --replay: %s/blob.bin, snap.bin, inputs.bin\n", dir.c_str()); return 1; }
	vw::Snap sn;
	const auto d0 = Clock::now();
	if (!vw::decode_snap(snap_bytes, sn)) { fprintf(stderr, "vf3net: --replay: the snapshot can't be read\n"); return 1; }
	const double decode_ms = std::chrono::duration<double, std::milli>(Clock::now() - d0).count();
	std::string error;
	auto pb = std::make_unique<Board>();
	Board &W = *pb;
	W.use_rec = use_rec;
	W.adapter.stage_select_on = stage;
	W.adapter.dural = dural;
	W.adapter.host_terms = host_terms;
	if (!W.open(rom, profile, error)) { fprintf(stderr, "vf3net: %s\n", error.c_str()); return 1; }
	W.adapter.checkpoint_every = 30;
	vw::Spectator sp(W.adapter, vw::Spectator::Config{});
	vw::Start st;
	st.match = sn.match;
	st.meta.game = "vf3";
	st.blob = blob;
	const auto b0 = Clock::now();
	if (!sp.start(st)) { fprintf(stderr, "vf3net: --replay: the start blob didn't start\n"); return 1; }
	const double begin_ms = std::chrono::duration<double, std::milli>(Clock::now() - b0).count();
	if (g_pace) {   // (timed as the game runs a late watcher: from the match's start, or from the snapshot)
		const uint32_t total_in = uint32_t(in_bytes.size() / 4);
		std::vector<uint16_t> pin(size_t(total_in) * 2);
		memcpy(pin.data(), in_bytes.data(), size_t(total_in) * 4);
		const bool old_way = std::string(g_pace) == "old";
		double snap_ms = 0;
		uint32_t from = 0;
		if (!g_from_start) {
			const auto l1 = Clock::now();
			if (!sp.snapshot(sn)) { fprintf(stderr, "--pace: the snapshot wasn't taken: %s\n", sp.why.c_str()); return 1; }
			snap_ms = std::chrono::duration<double, std::milli>(Clock::now() - l1).count();
			from = sn.frame;
		}
		const watch_pace::Result r = watch_pace::run(sp, pin, from + uint32_t(g_behind * 57.524160), sn.match, old_way, g_cap);
		char what[200];
		snprintf(what, sizeof what, "%s, %s (its start %.1f s before; the snapshot read in %.0f ms, loaded and checked in %.0f ms)", old_way ? "the old way" : "the new way",
		         g_from_start ? "from the match's start" : "from the snapshot", begin_ms / 1000, g_from_start ? 0.0 : decode_ms, snap_ms);
		watch_pace::say(what, r, g_behind);
		return 0;
	}
	const auto l0 = Clock::now();
	if (!sp.snapshot(sn)) { fprintf(stderr, "vf3net: --replay: the snapshot wasn't taken: %s\n", sp.why.c_str()); return 1; }
	const double load_ms = std::chrono::duration<double, std::milli>(Clock::now() - l0).count();
	const uint32_t total = uint32_t(in_bytes.size() / 4);
	std::vector<uint16_t> in(size_t(total) * 2);
	memcpy(in.data(), in_bytes.data(), size_t(total) * 4);
	double t = 1000;
	const auto r0 = Clock::now();
	while (sp.frame() < total && sp.state() != vw::Spectator::State::out_of_step && sp.state() != vw::Spectator::State::failed) {
		if (sp.want(t) == "frames") {
			sp.asked(t);
			vw::Reply r;
			r.match = sn.match;
			r.state = 2;
			r.total = total;
			r.frames.first = sp.ask_from();
			const uint32_t n = std::min<uint32_t>(total - r.frames.first, vw::MAX_REPLY);
			r.frames.in.assign(in.begin() + ptrdiff_t(r.frames.first) * 2, in.begin() + ptrdiff_t(r.frames.first + n) * 2);
			sp.reply(r, t);
			sp.answered(t);
		}
		sp.run(t);
		t += 1;
	}
	const double run_ms = std::chrono::duration<double, std::milli>(Clock::now() - r0).count();
	printf("replay: the snapshot at frame %u (%zu bytes packed, %zu its state), read in %.0f ms; the match's start %.0f ms, the snapshot loaded and checked in %.0f ms; %u frames after it in %.0f ms (%.2f ms a frame)\n",
	       sn.frame, snap_bytes.size(), sn.state.size(), decode_ms, begin_ms, load_ms, sp.frame() - sn.frame, run_ms, run_ms / std::max(1u, sp.frame() - sn.frame));
	if (sp.state() == vw::Spectator::State::out_of_step) { printf("replay: FAILED: %s\n", sp.why.c_str()); return 1; }
	if (hashes_path) {
		FILE *hf = fopen(hashes_path, "w");
		if (!hf) return 1;
		for (auto &[f, h] : W.adapter.checkpoints) fprintf(hf, "%u %016llx\n", f, (unsigned long long)h);
		fclose(hf);
	}
	return 0;
}

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
	const char *usage = "usage: vf3net <vf3.zip> --script FILE [--frames N] [--latency MS] [--jitter MS] [--loss PERCENT] [--delay D] [--interp] [--soft] [--set vf3tb] [--stage] [--dural] [--dural-stage host|guest|both] [--vs-rounds N] [--hashes FILE] [--blob-out FILE] [--blob-in FILE] [--host-terms] [--guest-stage 0|1] [--guest-dural 0|1] [--team 1v1|3v3] [--terms VERSION]\n";
	int vs_rounds = 0, watchers = 0;
	long watch_late = 1500;
	double watch_ms = 60;
	const char *hashes_path = nullptr, *blob_out = nullptr, *blob_in = nullptr, *terms = nullptr, *snap_out = nullptr, *replay_dir = nullptr;
	bool watch_snap = false;
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
		else if (a == "--terms" && i + 1 < argc) terms = argv[++i];
		else if (a == "--watchers" && i + 1 < argc) watchers = std::clamp(atoi(argv[++i]), 0, 4);
		else if (a == "--watch-late" && i + 1 < argc) watch_late = strtol(argv[++i], nullptr, 10);
		else if (a == "--watch-ms" && i + 1 < argc) watch_ms = atof(argv[++i]);
		else if (a == "--watch-snap") watch_snap = true;
		else if (a == "--snap-out" && i + 1 < argc) snap_out = argv[++i];
		else if (a == "--replay" && i + 1 < argc) replay_dir = argv[++i];
		else if (a == "--pace" && i + 1 < argc) g_pace = argv[++i];
		else if (a == "--behind" && i + 1 < argc) g_behind = atof(argv[++i]);
		else if (a == "--cap" && i + 1 < argc) g_cap = atof(argv[++i]);
		else if (a == "--from-start") g_from_start = true;
		else if (a == "--set" && i + 1 < argc) {
			if (!(profile = m3::find_profile(argv[++i]))) { fprintf(stderr, "vf3net: no such set %s\n", argv[i]); return 2; }
		}
		else if (a[0] != '-' && rom.empty()) rom = a;
		else { fprintf(stderr, "vf3net: unknown argument %s\n%s", a.c_str(), usage); return 2; }
	}
	if (!rom.empty() && replay_dir) return replay(rom, profile, replay_dir, hashes_path, use_rec, stage, dural, host_terms);
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
	if (terms) {   // (the lobby's terms, played as Online::use_terms plays them: the pool's, or an older version's flags)
		const std::string t = terms, own = t.substr(0, t.find('+'));
		vf3net::Terms m;
		if (!vf3net::terms_of(t, own, vf3net::pool_version(own), set.team_mode != 0, m)) { fprintf(stderr, "vf3net: terms %s not played by this build\n", terms); return 2; }
		host_terms = m.host_terms;
		A.adapter.dural_legacy = B.adapter.dural_legacy = m.dural_legacy;   // (an older version's Dural flag: its patches)
		dural = m.dural;   // (a pool's: marked when either player has Dural on, the guest's own aside, --guest-dural)
		if (!host_terms) { stage = m.stage; guest_stage = guest_dural = -1; }
		A.adapter.team_one = B.adapter.team_one = m.team_one;
		printf("the match's terms: %s (%s; Team Battle's mode %s)\n", terms, host_terms ? "the pool's: the host's settings" : "an older version's",
		       m.team_one < 0 ? "the host's" : m.team_one ? "1 vs 1" : "3 vs 3");
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
	// watching: the host's feed (the same whether anyone watches), a stand-in lobby, the watchers' boards
	namespace vw = vfnet::watch;
	vw::Feed feed;
	bool feed_on = false;
	struct Lobby { std::vector<uint8_t> start; std::vector<uint16_t> in; std::map<uint32_t, uint64_t> hashes; vw::Info info; uint64_t match = 0; std::vector<uint8_t> snap, snap_in; } lobby;
	vw::Snapper snapper;   // (--watch-snap: the host's snapshots, as the game makes them)
	struct Due { double at; std::function<void()> fn; };
	std::deque<Due> dues;   // (the uploads and answers on their way, in the order sent: the same delay for all)
	struct Watcher { std::unique_ptr<Board> b; std::unique_ptr<vw::Spectator> sp; long join = 0; bool on = false; std::vector<double> ran_at; double on_at = 0; uint32_t snapped = 0, was_at = 0; };   // (ran_at: when it ran each frame)
	std::map<const vw::Spectator *, std::map<uint32_t, double>> hash_got;   // (when each of the players' hashes reached each watcher)
	std::vector<Watcher> ws;
	for (int i = 0; i < watchers; i++) {
		Watcher w;
		w.b = std::make_unique<Board>();
		w.b->use_rec = use_rec;
		if (!w.b->open(rom, profile, error)) { fprintf(stderr, "vf3net: a watcher's board: %s\n", error.c_str()); return 1; }
		w.b->adapter.host_terms = host_terms;   // (the terms as a guest plays them: the host's blob says the rest)
		w.b->adapter.dural_legacy = A.adapter.dural_legacy;
		w.b->adapter.team_one = A.adapter.team_one;
		w.b->adapter.stage_select_on = stage;
		w.b->adapter.dural = dural;
		w.b->adapter.checkpoint_every = 300;
		w.sp = std::make_unique<vw::Spectator>(w.b->adapter, vw::Spectator::Config{});
		w.join = i == 0 ? 0 : watch_late;
		ws.push_back(std::move(w));
	}
	auto serve = [&](vw::Spectator &sp, const std::string &want) -> std::function<void(double)> {
		if (want == "start") { std::vector<uint8_t> b = lobby.start; return [&sp, b](double) { vw::Start st; if (vw::decode(b, st)) sp.start(st); }; }
		vw::Reply r;
		r.match = lobby.match;
		r.state = 1;
		const uint32_t total = uint32_t(lobby.in.size() / 2), from = std::min(sp.ask_from() / 256 * 256, total), n = std::min<uint32_t>(total - from, 2048);
		r.total = total;
		r.frames.first = from;
		r.frames.in.assign(lobby.in.begin() + ptrdiff_t(from) * 2, lobby.in.begin() + ptrdiff_t(from + n) * 2);
		for (auto &[f, h] : lobby.hashes) if (f + 120 >= from && f <= from + n) r.frames.hashes.push_back({ f, h });   // (and those made late, as the lobby's)
		r.frames.info = lobby.info;
		return [&sp, r, &hash_got](double at) { if (sp.reply(r, at)) for (auto &[f, h] : r.frames.hashes) hash_got[&sp].emplace(f, at); };
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
			snapper.before_step(sa);
			if (sa.step(own_word(pick_a, A.m, 0, vf3net::pack(real[std::min(na, real.size() - 1)], 0)))) { na++; score_a.frame(A.m); }
			if (!feed_on && sa.state() == vfnet::Session::State::running) {   // (the host's match to the lobby, as the game sends it)
				vw::Start st;
				st.match = 0x5eed;
				st.meta = { "vf3", "test", vw::hex(content), "test", { "HOST", "GUEST" }, { "", "" } };
				st.blob = blob;
				feed.begin(st, t);
				feed_on = true;
			}
			feed.take(sa);
			if (watch_snap) {
				for (const Watcher &w : ws)   // (a late watcher about to come: asked for, as the lobby asks)
					if (w.join && long(sa.frame()) == w.join - 240) snapper.want(t);
				snapper.step(sa, A.adapter, feed.match(), t);
			}
			vw::Info info;
			for (int q = 0; q < 2; q++) { info.wins[q] = uint8_t(score_a.matches[q]); info.rounds[q] = uint8_t(score_a.rounds[q]); }
			feed.set_info(info);
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
		// the host's upload, the lobby's keeping it, the watchers asking and running theirs
		vw::Feed::Upload up;
		if (feed.next(t, up))
			dues.push_back({ t + watch_ms, [&, up, at = t + watch_ms] {
				uint32_t has = 0;
				if (up.start) { vw::Start st; if (vw::decode(up.body, st)) { lobby.start = up.body; lobby.match = st.match; } }
				else {
					vw::Frames f;
					if (vw::decode_push(up.body, f)) {
						const uint32_t had = uint32_t(lobby.in.size() / 2);
						if (f.first <= had && f.first + f.count() > had) lobby.in.insert(lobby.in.end(), f.in.begin() + ptrdiff_t(had - f.first) * 2, f.in.end());
						for (auto &[fr, h] : f.hashes) lobby.hashes[fr] = h;
						lobby.info = f.info;
					}
					has = uint32_t(lobby.in.size() / 2);
				}
				dues.push_back({ at + watch_ms, [&feed, up, has, done = at + watch_ms] { feed.done(up, true, has, int(0), done); } });
			} });
		vw::Snapper::Part part;
		if (snapper.next(t, part))
			dues.push_back({ t + watch_ms, [&, part, at = t + watch_ms] {
				if (part.index == 0) lobby.snap_in.clear();
				if (part.match == lobby.match && size_t(part.index) * snapper.part_bytes == lobby.snap_in.size()) lobby.snap_in.insert(lobby.snap_in.end(), part.body.begin(), part.body.end());
				if (part.index + 1 == part.count) lobby.snap = lobby.snap_in;
				dues.push_back({ at + watch_ms, [&snapper, part, done = at + watch_ms] { snapper.done(part, true, 2 * 60.0, done); } });
			} });
		while (!dues.empty() && dues.front().at <= t) { auto fn = dues.front().fn; dues.pop_front(); fn(); }
		for (Watcher &w : ws) {
			if (!w.on && long(sa.frame()) >= w.join && !lobby.start.empty()) { w.on = true; w.on_at = t; }
			if (!w.on) continue;
			const bool snap_wait = watch_snap && w.join && !w.snapped && t - w.on_at < 20000;   // (the frames from the snapshot's, as the game holds them)
			if (snap_wait && !lobby.snap.empty() && w.sp->state() != vw::Spectator::State::waiting) {
				vw::Snap sn;
				w.was_at = w.sp->frame();
				if (vw::decode_snap(lobby.snap, sn) && w.sp->snapshot(sn)) w.snapped = sn.frame;
				else printf("watcher: the snapshot wasn't taken: %s\n", w.sp->why.c_str());
			}
			const std::string want = w.sp->want(t);
			if (!want.empty() && !(want == "frames" && snap_wait)) {
				w.sp->asked(t);
				dues.push_back({ t + watch_ms, [&, sp = w.sp.get(), want, at = t + watch_ms] {
					auto answer = serve(*sp, want);
					dues.push_back({ at + watch_ms, [sp, answer, done = at + watch_ms] { answer(done); sp->answered(done); } });
				} });
			}
			w.sp->run(t);
			while (w.ran_at.size() < w.sp->frame()) w.ran_at.push_back(t);
		}
		t += 1;
	}
	// (the watchers to the end of what the lobby has: the players are done)
	for (double end = t + 30000; t < end && !ws.empty(); t += 1) {
		while (!dues.empty() && dues.front().at <= t) { auto fn = dues.front().fn; dues.pop_front(); fn(); }
		bool all = true;
		for (Watcher &w : ws) {
			if (!w.on) continue;
			const std::string want = w.sp->want(t);
			if (!want.empty()) {
				w.sp->asked(t);
				dues.push_back({ t + watch_ms, [&, sp = w.sp.get(), want, at = t + watch_ms] {
					auto answer = serve(*sp, want);
					dues.push_back({ at + watch_ms, [sp, answer, done = at + watch_ms] { answer(done); sp->answered(done); } });
				} });
			}
			w.sp->run(t);
			while (w.ran_at.size() < w.sp->frame()) w.ran_at.push_back(t);
			all = all && w.sp->frame() >= lobby.in.size() / 2;
		}
		if (all) break;
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
		C.adapter.dural_legacy = A.adapter.dural_legacy;
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
		for (size_t i = 0; i < ws.size(); i++) {   // (each watcher: the straight run's board at every checkpoint it ran past, the players' hashes matched)
			Watcher &w = ws[i];
			uint32_t same = 0;
			for (auto &[f, h] : w.b->adapter.checkpoints) {
				if (f > n || f % 300) continue;
				if (C.adapter.checkpoints[f] != h) { printf("watcher %zu's board differs from a straight run after frame %u\n", i + 1, f); failed = true; break; }
				same++;
			}
			const bool whole = w.sp->frame() + 60 >= n;
			// (one come late ran the match from its start: every one of the players' hashes in it checked, none skipped; and
			// every watcher, at the live edge, every one that reached it before it ran the hash's frame: one made a few
			// frames after its frame and come just after the watcher ran that frame can't be checked)
			uint32_t there = 0;
			const std::map<uint32_t, double> &got = hash_got[w.sp.get()];
			for (auto &[f, h] : lobby.hashes) {
				const auto at = got.find(f);
				there += f >= w.snapped && f + vw::HASH_PARTS < w.sp->frame() && at != got.end() && f < w.ran_at.size() && at->second <= w.ran_at[f];
			}
			if (w.snapped) printf("watcher %zu came in from the host's snapshot at frame %u (it had run %u from the start): %u frames run after it\n", i + 1, w.snapped, w.was_at, w.sp->frame() - w.snapped);
			else if (watch_snap && w.join) { printf("watcher %zu: no snapshot taken\n", i + 1); failed = true; }
			const bool all = w.sp->hashes_checked >= there;
			printf("watcher %zu (from frame %ld): ran %u frames (%llu unseen, catching up), the straight run's board at %u checkpoints, %u of the players' hashes matched%s%s\n",
			       i + 1, w.join, w.sp->frame(), (unsigned long long)w.sp->frames_unseen, same, w.sp->hashes_checked,
			       w.join ? (" (all " + std::to_string(there) + " that reached it before it ran their frames: from the match's start)").c_str() : "", whole ? "" : " (NOT to the end)");
			if (!whole || !same || !w.sp->hashes_checked || !all || w.sp->state() == vw::Spectator::State::out_of_step) failed = true;
		}
		if (!ws.empty()) {
			printf("the host's uploads: %llu, %llu bytes\n", (unsigned long long)feed.uploads, (unsigned long long)feed.bytes);
		}
		if (watch_snap)
			printf("the host's snapshots: %u made, %u sent; the last at frame %u: %.1f MB, %.2f MB packed, copied in %.0f ms and packed in %.0f ms on its own thread; the steps held for it %.2f ms in all\n",
			       snapper.made, snapper.sent, snapper.last_frame, double(snapper.last_raw) / 1e6, double(snapper.last_packed) / 1e6, snapper.last_copy_ms, snapper.last_pack_ms, snapper.stall_ms);
		if (snap_out) {
			std::filesystem::create_directories(snap_out);
			std::vector<uint8_t> ib(size_t(n) * 4);
			for (uint32_t f = 0; f < n; f++) {
				const uint16_t a = sa.inputs(0)[f], b = sa.inputs(1)[f];
				memcpy(ib.data() + size_t(f) * 4, &a, 2);
				memcpy(ib.data() + size_t(f) * 4 + 2, &b, 2);
			}
			if (lobby.snap.empty() || !write_file(std::string(snap_out) + "/snap.bin", lobby.snap) || !write_file(std::string(snap_out) + "/blob.bin", blob) || !write_file(std::string(snap_out) + "/inputs.bin", ib)) {
				printf("--snap-out: nothing written (no snapshot)\n");
				failed = true;
			}
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
