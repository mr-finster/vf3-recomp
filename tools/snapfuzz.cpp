// snapfuzz: a watcher's snapshot path against a hostile room host (a snapshot's bytes can be anything, its hash
// forged), with AddressSanitizer and UBSan (make snapfuzz: its own objects). From a real snapshot (vf3net --watch-snap
// --snap-out DIR: snap.bin, blob.bin, inputs.bin), each iteration spoils it one way and loads it as a watcher does
// (decode_snap with this build's lengths, Adapter::load_snap, the board's own state after it hashed as the
// Spectator's check), and some run frames after it (the board then runs whatever the state's RAM and registers hold):
//   state     byte flips, runs of random bytes, truncation, a random tail, 1/2/4/8-byte fields set to edge values
//   fields    the small part's (CPUs, scheduler, FIFOs, devices) words set to edge values, its vector lengths huge
//   guest     the CPU's RAM (its program too), the culling or polygon RAMs, the tile RAM filled at random, frames run
//   extra     the sound bytes' extra spoilt
//   packed    the whole message spoilt (its head, lengths, the zlib stream): decode_snap
//   sound     the sound board given the extra's bytes spoilt, or any bytes, and run (its 68000's program steered by them)
// A refused one must leave the game as it was (its state saved before and after: the same bytes). Each iteration's
// spoiling comes from (seed, iteration) alone: --only N repeats one. A hang (an iteration over --hang seconds) aborts.
//   snapfuzz <vf3 rom> <dir> [--iters N] [--seed S] [--only N] [--run-every K] [--frames F] [--hang S]
// (VF3: frames run after a taken one are drawn too, half of them: the scene read from the culling and polygon RAMs,
// rasterised with the texture RAM, the tile layers' lines; the recompiled code, as the game, while the RAM's program
// is the ROM's. the sound kind: the sound board given any bytes,
// as the board a snapshot made sends it after the live point)
// Also: SNAPFUZZ_SWEEP=1 (structured: every 4-byte place of the small parts, and each 8-byte one, set to each edge value
// in turn and loaded; one in 16 taken run 10 frames), SNAPFUZZ_KIND=K (every iteration kind K: 82 the sound board's),
// SNAPFUZZ_BENCH=1 (the loads' and saves' times), --start N (from iteration N: a stopped run carried on).
#include "../src/host/vf3_netplay.h"
#include "../src/machine/model3.h"
#include "../src/net/watch.h"
#include "../src/rec/ppc_rec.h"
#include "../src/sound/soundboard.h"
#include "../src/video/r3d_raster.h"
#include "../src/video/r3d_scene.h"
#include "../src/video/tilegen.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace vw = vfnet::watch;

struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
	uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
	uint32_t below(uint64_t n) { return n ? uint32_t(next() % n) : 0; }
};

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	out.clear();
	uint8_t b[65536];
	for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) out.insert(out.end(), b, b + n);
	fclose(f);
	return true;
}

uint64_t edge(Rng &r)
{
	static const uint64_t v[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 33, 63, 64, 255, 256, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000,
		0xffffffff, 0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull, ~0ull - 1, 0xfffffff0, 1000000, 0x500000, 0x5fffff };
	return r.below(5) ? v[r.below(std::size(v))] : r.next();
}

void put(std::vector<uint8_t> &b, size_t at, uint64_t v, int n)
{
	for (int k = 0; k < n && at + size_t(k) < b.size(); k++) b[at + size_t(k)] = uint8_t(v >> (8 * k));
}

// One spoiling of bytes, anywhere in [lo, hi) (most of them in it), or of the whole
size_t g_tlo = SIZE_MAX, g_thi = 0;   // (the bytes spoil changed in place: put back from the unspoilt copy after)
void touch(size_t a, size_t n) { g_tlo = std::min(g_tlo, a); g_thi = std::max(g_thi, a + n); }

void spoil(std::vector<uint8_t> &b, Rng &r, size_t lo, size_t hi)
{
	if (b.empty()) { b.push_back(uint8_t(r.next())); return; }
	hi = std::min(hi, b.size());
	if (lo >= hi) lo = 0, hi = b.size();
	auto pick = [&] { return r.below(4) ? lo + r.below(hi - lo) : r.below(b.size()); };
	switch (r.below(9)) {
	case 0: case 1: { const int n = 1 + int(r.below(8)); for (int i = 0; i < n; i++) { const size_t a = pick(); touch(a, 1); b[a] ^= uint8_t(1u << r.below(8)); } break; }
	case 2: { const int n = 1 + int(r.below(4)); for (int i = 0; i < n; i++) { const size_t a = pick(); touch(a, 1); b[a] = uint8_t(r.next()); } break; }
	case 3: { const size_t at = pick(), n = 1 + r.below(r.below(2) ? 64 : 4096); touch(at, n); for (size_t i = 0; i < n && at + i < b.size(); i++) b[at + i] = uint8_t(r.next()); break; }
	case 4: touch(0, SIZE_MAX / 2); b.resize(r.below(2) ? r.below(b.size()) : b.size() - std::min<size_t>(b.size(), 1 + r.below(64))); break;
	case 5: { touch(b.size(), SIZE_MAX / 2); const size_t n = 1 + r.below(r.below(2) ? 16 : 70000); for (size_t i = 0; i < n; i++) b.push_back(uint8_t(r.next())); break; }
	case 6: case 7: { const int w = 1 << r.below(4); size_t at = pick(); at -= at % size_t(w); touch(at, size_t(w)); put(b, at, edge(r), w); break; }
	case 8: { const size_t a = pick(), c = pick(), n = 1 + r.below(256); if (a + n <= b.size() && c + n <= b.size()) { touch(a, n); memmove(&b[a], &b[c], n); } break; }
	}
}

std::atomic<uint64_t> g_iter{ 0 };
std::atomic<int64_t> g_iter_at{ 0 };
int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: snapfuzz <vf3 rom> <dir> [--iters N] [--seed S] [--only N] [--run-every K] [--frames F] [--hang S]\n"); return 2; }
	const std::string rom = argv[1], dir = argv[2];
	uint64_t iters = 1000, seed = 1;
	int64_t only = -1;
	uint64_t start = 0;   // (--start N: from iteration N, a run that stopped carried on)
	uint32_t run_every = 40, frames = 40, hang_s = 120;
	for (int i = 3; i + 1 < argc; i += 2) {
		const std::string a = argv[i];
		if (a == "--iters") iters = strtoull(argv[i + 1], nullptr, 10);
		else if (a == "--seed") seed = strtoull(argv[i + 1], nullptr, 10);
		else if (a == "--only") only = strtoll(argv[i + 1], nullptr, 10);
		else if (a == "--start") start = strtoull(argv[i + 1], nullptr, 10);
		else if (a == "--run-every") run_every = uint32_t(atoi(argv[i + 1]));
		else if (a == "--frames") frames = uint32_t(atoi(argv[i + 1]));
		else if (a == "--hang") hang_s = uint32_t(atoi(argv[i + 1]));
	}
	std::vector<uint8_t> blob, packed, in_bytes;
	if (!read_file(dir + "/blob.bin", blob) || !read_file(dir + "/snap.bin", packed) || !read_file(dir + "/inputs.bin", in_bytes)) { fprintf(stderr, "snapfuzz: %s/{blob,snap,inputs}.bin\n", dir.c_str()); return 1; }
	m3::Model3 board;
	std::string error;
	if (!board.load(rom, error)) { fprintf(stderr, "snapfuzz: %s\n", error.c_str()); return 1; }
	ppcrec::Rec rec;
	std::vector<uint32_t> image;
	vf3net::Adapter game(board);
	game.program_changed = [&] {
		std::vector<uint32_t> img = board.program_image();
		if (img == image) return;
		image = std::move(img);
		if (rec.build(board.cpu, image, board.profile->map, board.profile->map_count)) board.cpu.rec = &rec;
		else board.cpu.rec = nullptr;
	};
	vw::Snap base;
	const auto [least, most] = game.snap_state_bytes();
	if (!vw::decode_snap(packed, base, least, most)) { fprintf(stderr, "snapfuzz: the real snapshot doesn't read\n"); return 1; }
	vw::Spectator sp(game, vw::Spectator::Config{});
	vw::Start st;
	st.match = base.match;
	st.meta.game = "vf3";
	st.blob = blob;
	if (!sp.start(st) || !sp.snapshot(base)) { fprintf(stderr, "snapfuzz: the real snapshot isn't taken: %s\n", sp.why.c_str()); return 1; }
	const size_t skip = game.hash_skip();
	std::vector<uint8_t> good;
	game.save(good);
	std::vector<m3::StateSection> sec;
	std::vector<uint8_t> tmp;
	board.save_state(tmp, &sec);
	auto where = [&](const char *name) -> std::pair<size_t, size_t> {
		for (size_t i = 0; i < sec.size(); i++)
			if (!strcmp(sec[i].name, name)) return { sec[i].offset, i + 1 < sec.size() ? sec[i + 1].offset : tmp.size() };
		return { 0, 0 };
	};
	const size_t small = where("cpu").first;   // (the small parts: after the memories)
	const auto ramr = where("ram"), cull = where("culling ram"), cullhi = where("culling ram (high)"), poly = where("polygon ram"), tile = where("tile ram");
	static video::SceneBuilder scenes;
	static video::Scene scene;
	static video::Raster raster;
	static video::TileGen tiles;
	raster.resize(video::W, video::H);
	bool draw = false;
	board.on_line = [&](unsigned y) { if (draw && y < unsigned(video::H)) tiles.line(board, y); };
	const uint32_t total = uint32_t(in_bytes.size() / 4);
	SoundBoard snd;   // (the watcher's sound board: after the live point it's given what the snapshot's board sends)
	snd.attach(&board.sound_rom, &board.samples);
	snd.reset();
	std::vector<int16_t> snd_out;
	printf("snapfuzz vf3: the snapshot at frame %u, its state %zu bytes (the small parts from %zu), %zu extra; lengths %zu-%zu\n", base.frame, base.state.size(), small,
	       base.extra.size(), least, most);
	fflush(stdout);
	if (getenv("SNAPFUZZ_BENCH")) {   // (the loads' and saves' times, ms)
		std::vector<uint8_t> b;
		auto t = [&](const char *what, auto f) { const auto a = now_ms(); for (int i = 0; i < 20; i++) f(); printf("%s: %.1f ms\n", what, double(now_ms() - a) / 20); };
		t("save", [&] { game.save(b); });
		t("load (the same)", [&] { game.load(good); });
		t("load_snap (the same)", [&] { game.load_snap(base.state, base.extra, base.frame); });
		std::vector<uint8_t> other = base.state;
		other[small + 100] ^= 1;
		t("load_snap alternating", [&] { static int n; game.load_snap((n++ & 1) ? other : base.state, base.extra, base.frame); });
		t("state hash", [&] { (void)vw::state_hash(base.state, skip); });
		return 0;
	}
	std::thread([hang_s] {
		for (;;) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			if (now_ms() - g_iter_at.load() > int64_t(hang_s) * 1000) { fprintf(stderr, "snapfuzz: HANG at iteration %llu\n", (unsigned long long)g_iter.load()); fflush(stderr); abort(); }
		}
	}).detach();
	double kind_ms[6] = {};
	int64_t it_t0 = 0;
	int last_k = -1;
	uint64_t taken = 0, refused = 0, ran = 0, threw = 0, checked = 0, decoded = 0, fast_n = 0, kinds[6] = {};
	std::vector<uint8_t> before, after;
	auto same_as = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) { return a.size() == b.size() && !memcmp(a.data() + skip, b.data() + skip, a.size() - skip); };
	// (the fast form: the snapshot's state with the header of a state this board saved, as if saved since (its last
	// save's number as high as can be), so a load copies only what it always copies (the RAMs and the small parts): the
	// device memories' pages, unchanged here, not compared; most iterations spoil it in place and load it with the
	// adapter's load (the rollback's: the same checks), a few take the whole way (load_snap, a copy of the whole)
	std::vector<uint8_t> fref = base.state;
	memcpy(fref.data(), good.data(), skip);
	{ const uint32_t most_gen = 0xffffffffu; memcpy(fref.data() + 16, &most_gen, 4); }
	std::vector<uint8_t> F = fref;
	uint32_t paged_at = 0;
	size_t paged_off = 0, paged_n = 0;
	uint64_t paged_seed = 0;
	const auto t0 = now_ms();
	const int force_kind = getenv("SNAPFUZZ_KIND") ? atoi(getenv("SNAPFUZZ_KIND")) : -1;
	if (getenv("SNAPFUZZ_SWEEP")) {   // (structured: every word of the small parts in turn (each 4-byte place, and each 8-byte one as a
		// length or a time), set to each edge value, loaded; one in 16 taken run 10 frames after)
		static const uint64_t vals[] = { 0, 1, 2, 7, 8, 9, 31, 32, 33, 63, 64, 255, 256, 0x7fff, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff,
			0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull, 0xfffffff0, 0x500000, 0x5fffff };
		const size_t from = small, to = F.size() - 4;
		uint64_t n = 0, t = 0;
		for (size_t at = from; at < to; at += 4)
			for (const int w : { 4, 8 })
				for (const uint64_t v : vals) {
					if (w == 8 && (at - from) % 8) continue;
					if (w == 4 && v > 0xffffffffull) continue;
					if (at + size_t(w) > F.size()) continue;
					g_iter = n;
					g_iter_at = now_ms();
					put(F, at, v, w);
					bool ok = false;
					try { ok = game.load(F); } catch (const std::exception &e) { threw++; printf("sweep %zu = %llx: load threw: %s\n", at, (unsigned long long)v, e.what()); }
					memcpy(F.data() + at, fref.data() + at, size_t(w));
					n++;
					if (!ok) continue;
					t++;
					if (n % 16) continue;
					try {
						for (uint32_t f = 0; f < 10; f++) {
							uint16_t in[2] = { 0, 0 };
							if (base.frame + f < total) memcpy(in, &in_bytes[size_t(base.frame + f) * 4], 4);
							game.advance(in, false);
							g_iter_at = now_ms();
						}
					} catch (const std::exception &e) { threw++; printf("sweep %zu = %llx: a frame threw: %s\n", at, (unsigned long long)v, e.what()); }
					if (!game.load(good)) { fprintf(stderr, "snapfuzz: the good state doesn't load back\n"); return 1; }
				}
		printf("snapfuzz vf3 sweep: bytes %zu-%zu, %llu loads, %llu taken, %llu threw, %.0f s\n", from, to, (unsigned long long)n, (unsigned long long)t,
		       (unsigned long long)threw, double(now_ms() - t0) / 1000);
		return 0;
	}
	for (uint64_t it = only >= 0 ? uint64_t(only) : start; it < (only >= 0 ? uint64_t(only) + 1 : iters); it++) {
		if (last_k >= 0) kind_ms[last_k] += double(now_ms() - it_t0);
		it_t0 = now_ms();
		g_iter = it;
		g_iter_at = now_ms();
		if ((it & 4095) == 0 && it) { printf("  %llu: %llu taken, %llu refused, %llu ran, %llu threw (%.0f s)\n", (unsigned long long)it, (unsigned long long)taken, (unsigned long long)refused,
		                                     (unsigned long long)ran, (unsigned long long)threw, double(now_ms() - t0) / 1000); fflush(stdout); }
		// (F as it was: the bytes the last iteration spoilt put back)
		if (F.size() != fref.size()) F = fref;
		else if (g_tlo < std::min(g_thi, F.size())) memcpy(F.data() + g_tlo, fref.data() + g_tlo, std::min(g_thi, F.size()) - g_tlo);
		g_tlo = SIZE_MAX, g_thi = 0;
		Rng r(seed * 1000003ull ^ it);
		uint32_t kind = r.below(100);
		if (force_kind >= 0) kind = uint32_t(force_kind);   // (SNAPFUZZ_KIND: every iteration that one, e.g. 82 the sound board's)
		if (kind >= 85 && kind < 97) kind -= 85;   // (VF3's snapshots have no extra: those iterations the state's instead)
		bool run = r.below(run_every) == 0;
		int k = 0;
		last_k = -1;
		const uint32_t gw = r.below(5);   // (guest: which memory; the device memories' (paged) only the whole way)
		const bool fast = kind < 82 && r.below(32) != 0;
		vw::Snap s;
		if (!fast) s = base;
		std::vector<uint8_t> &S = fast ? F : s.state;
		if (kind < 55) {   // state
			last_k = 0;
			const int n = 1 + int(r.below(r.below(3) ? 2 : 6));
			for (int i = 0; i < n; i++) spoil(S, r, fast || r.below(2) ? small : skip, S.size());
		} else if (kind < 70) {   // fields: the small part's words, edge values
			k = 1;
			last_k = 1;
			const int n = 1 + int(r.below(4));
			for (int i = 0; i < n; i++) {
				const int w = r.below(3) ? 4 : 8;
				size_t at = small + r.below(S.size() - small);
				at -= (at - small) % size_t(w);
				touch(at, size_t(w));
				put(S, at, edge(r), w);
			}
		} else if (kind < 82) {   // guest: a memory filled at random, frames run
			k = 2;
			last_k = 2;
			const auto reg = gw == 0 ? ramr : gw == 1 ? cull : gw == 2 ? cullhi : gw == 3 ? poly : tile;
			const size_t a = reg.first + r.below(reg.second - reg.first), n = std::min(reg.second - a, size_t(1 + r.below(r.below(2) ? 64 : 0x20000)));
			if (fast && gw != 0) {   // (the device memories: the fast load leaves them be, so spoilt in the board after it, run, and put back)
				paged_at = gw;
				paged_off = a - reg.first;
				paged_n = n;
				paged_seed = r.next();
				run = r.below(4) == 0;
			} else {
				touch(a, n);
				for (size_t i = 0; i < n; i++) S[a + i] = uint8_t(r.next());
				run = r.below(10) == 0;
			}
		} else if (kind < 83) {   // sound: the sound board given bytes, run
			k = 5;
			last_k = 5;
			kinds[k]++;
			std::vector<uint8_t> b;
			if (r.below(2) && !base.extra.empty()) { b = base.extra; for (int i = 0; i < 4; i++) spoil(b, r, 0, b.size()); }
			else { b.resize(1 + r.below(4096)); for (auto &x : b) x = uint8_t(r.below(3) ? r.next() : 0x80 | r.next()); }
			g_tlo = SIZE_MAX, g_thi = 0;
			if (r.below(8) == 0) snd.reset();
			for (const uint8_t x : b) snd.midi_in(x);
			for (int f = 0; f < 20; f++) {
				snd.run(1.0 / 57.524160, snd_out);
				snd_out.clear();
				g_iter_at = now_ms();
			}
			continue;
		} else if (kind < 97) {   // extra
			k = 3;
			last_k = 3;
			const int n = 1 + int(r.below(3));
			for (int i = 0; i < n; i++) spoil(s.extra, r, 0, s.extra.size());
			g_tlo = SIZE_MAX, g_thi = 0;
		} else {   // packed
			k = 4;
			last_k = 4;
			std::vector<uint8_t> p = packed;
			const int n = 1 + int(r.below(3));
			for (int i = 0; i < n; i++) spoil(p, r, 0, r.below(2) ? 64 : p.size());
			g_tlo = SIZE_MAX, g_thi = 0;
			decoded++;
			if (!vw::decode_snap(p, s, least, most)) { refused++; kinds[k]++; continue; }
		}
		kinds[k]++;
		bool ok = false;
		if (fast) {   // (the adapter's load: the board's checks, as load_snap's)
			fast_n++;
			try {
				ok = game.load(F);
			} catch (const std::exception &e) { threw++; printf("iteration %llu: load threw: %s\n", (unsigned long long)it, e.what()); }
			if (!ok) { refused++; continue; }
		} else {
			// as Spectator::snapshot: the lengths, (the hash: forged, so taken as the state's), load_snap, the board's own after
			const bool check = r.below(4) == 0;
			if (check) { before.clear(); game.save(before); }   // (whole saves, not one since the last into the same buffer)
			try {
				ok = s.state.size() >= least && s.state.size() <= most && game.load_snap(s.state, s.extra, s.frame);
			} catch (const std::exception &e) { threw++; printf("iteration %llu: load threw: %s\n", (unsigned long long)it, e.what()); }
			if (!ok) {
				refused++;
				if (check) {
					checked++;
					after.clear();
					game.save(after);
					if (!same_as(before, after)) {
					size_t d = skip;
					while (d < std::min(before.size(), after.size()) && before[d] == after[d]) d++;
					const char *in = "?";
					for (const auto &x : sec) if (x.offset + 0 <= d) in = x.name;
					fprintf(stderr, "snapfuzz: iteration %llu: refused, but the game isn't as it was (sizes %zu, %zu; the first byte that differs %zu, in %s)\n", (unsigned long long)it,
					        before.size(), after.size(), d, in);
					abort();
				}
				}
				continue;
			}
			game.save(after);
		}
		taken++;
		std::vector<uint8_t> paged_was;
		uint8_t *paged_mem = nullptr;
		if (paged_at) {
			paged_mem = paged_at == 1 ? reinterpret_cast<uint8_t *>(board.cull_lo.data()) : paged_at == 2 ? reinterpret_cast<uint8_t *>(board.cull_hi.data())
			          : paged_at == 3 ? reinterpret_cast<uint8_t *>(board.poly_ram.data()) : board.tile_ram.data();
			paged_was.assign(paged_mem + paged_off, paged_mem + paged_off + paged_n);
			Rng pr(paged_seed);
			for (size_t i = 0; i < paged_n; i++) paged_mem[paged_off + i] = uint8_t(pr.next());
			paged_at = 0;
		}
		if (!run) {
			if (paged_mem) memcpy(paged_mem + paged_off, paged_was.data(), paged_n);
			continue;
		}
		ran++;
		draw = r.below(2) == 0;
		const uint32_t from = fast ? base.frame : s.frame;
		try {
			for (uint32_t f = 0, nf = paged_mem ? std::min(frames, 8u) : frames; f < nf; f++) {
				const uint32_t at = from + f;
				uint16_t in[2] = { 0, 0 };
				if (at < total) memcpy(in, &in_bytes[size_t(at) * 4], 4);
				else in[0] = uint16_t(r.next()), in[1] = uint16_t(r.next());
				game.advance(in, false);
				if (draw) {
					scenes.build(board, scene);
					raster.render(scene, board.tex_ram.data());
				}
				g_iter_at = now_ms();
			}
		} catch (const std::exception &e) { threw++; printf("iteration %llu (kind %d): a frame threw: %s\n", (unsigned long long)it, k, e.what()); }
		draw = false;
		if (paged_mem) memcpy(paged_mem + paged_off, paged_was.data(), paged_n);
		if (!game.load(good)) { fprintf(stderr, "snapfuzz: the good state doesn't load back\n"); return 1; }
	}
	printf("snapfuzz vf3: %llu iterations (seed %llu, %llu the fast way): %llu taken (%llu run %u frames after), %llu refused (%llu checked: the game as it was), %llu threw; "
	       "kinds: state %llu, fields %llu, guest %llu, extra %llu, packed %llu (%llu decoded), sound %llu; %.0f s\n",
	       (unsigned long long)(only >= 0 ? 1 : iters), (unsigned long long)seed, (unsigned long long)fast_n, (unsigned long long)taken, (unsigned long long)ran, frames, (unsigned long long)refused,
	       (unsigned long long)checked, (unsigned long long)threw, (unsigned long long)kinds[0], (unsigned long long)kinds[1], (unsigned long long)kinds[2],
	       (unsigned long long)kinds[3], (unsigned long long)kinds[4], (unsigned long long)decoded, (unsigned long long)kinds[5], double(now_ms() - t0) / 1000);
	printf("  time by kind (s): state %.0f, fields %.0f, guest %.0f, extra %.0f, packed %.0f, sound %.0f\n", kind_ms[0] / 1000, kind_ms[1] / 1000, kind_ms[2] / 1000,
	       kind_ms[3] / 1000, kind_ms[4] / 1000, kind_ms[5] / 1000);
	return 0;
}
