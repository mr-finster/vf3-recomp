// vf3soak: the rollback soak test, netplay's first check (no network). Two boards play the same input script: one
// straight through, and one as rollback netplay would run it, with player 2's inputs arriving --delay frames late.
// The second board guesses each of player 2's frames (the last input it had, as GGPO does), and as each real input
// arrives it goes back to that frame's save state and runs the frames since again (every frame by default, even
// when its guess was right; --only-misses: only when it was wrong). Each time a frame becomes certain its state
// must be the straight board's, byte for byte; at the first that isn't, the two states are written out and the
// parts that differ named.
//   vf3soak <vf3.zip> --script FILE [--frames N] [--delay D] [--only-misses] [--rec] [--hashes FILE] [--quiet]
// --rec: the recompiled code on both boards (as the game runs), else the interpreter. --hashes FILE: the straight
// board's state hash for each frame, to compare runs on other machines (x86-64 and ARM64 must match). Also
// reports what a frame costs (save, restore, run), against the 17.4 ms a frame has at 57.5 Hz.
#include "../src/host/input_script.h"
#include "../src/machine/model3.h"
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
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Player 1's inputs from a, player 2's from b
m3::Controls mix(const m3::Controls &a, const m3::Controls &b)
{
	m3::Controls c = a;
	auto p2 = [&](bool (&to)[2], const bool (&from)[2]) { to[1] = from[1]; };
	p2(c.coin, b.coin); p2(c.start, b.start); p2(c.test, b.test); p2(c.service, b.service);
	p2(c.up, b.up); p2(c.down, b.down); p2(c.left, b.left); p2(c.right, b.right);
	p2(c.punch, b.punch); p2(c.kick, b.kick); p2(c.guard, b.guard); p2(c.escape, b.escape);
	return c;
}

bool same_p2(const m3::Controls &a, const m3::Controls &b)
{
	return a.coin[1] == b.coin[1] && a.start[1] == b.start[1] && a.test[1] == b.test[1] && a.service[1] == b.service[1] &&
	       a.up[1] == b.up[1] && a.down[1] == b.down[1] && a.left[1] == b.left[1] && a.right[1] == b.right[1] &&
	       a.punch[1] == b.punch[1] && a.kick[1] == b.kick[1] && a.guard[1] == b.guard[1] && a.escape[1] == b.escape[1];
}

struct Stat
{
	double total = 0, worst = 0;
	uint64_t n = 0;
	void add(double ms) { total += ms; worst = std::max(worst, ms); n++; }
	double mean() const { return n ? total / double(n) : 0; }
};

// Where two states differ, by the sections the save named
void report(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, const std::vector<m3::StateSection> &sec)
{
	if (a.size() != b.size())
		printf("  the states' sizes differ: %zu, %zu\n", a.size(), b.size());
	const size_t n = std::min(a.size(), b.size());
	for (size_t i = 0; i < sec.size(); i++) {
		const size_t from = sec[i].offset, to = i + 1 < sec.size() ? sec[i + 1].offset : n;
		if (from >= n) break;
		size_t count = 0, first = 0;
		for (size_t k = from; k < std::min(to, n); k++)
			if (a[k] != b[k]) { if (!count) first = k - from; count++; }
		if (count)
			printf("  %s: %zu bytes differ, the first %zu bytes in (offset %zu in the state)\n", sec[i].name, count, first, from + first);
	}
}

bool write_file(const char *path, const std::vector<uint8_t> &v)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(v.data(), 1, v.size(), f) == v.size();
	return fclose(f) == 0 && ok;
}

} // namespace

int main(int argc, char **argv)
{
	std::string rom, script_path, hashes_path;
	long frames = -1, delay = 7;
	bool quiet = false, rec = false, only_misses = false;
	const char *usage = "usage: vf3soak <vf3.zip> --script FILE [--frames N] [--delay D] [--only-misses] [--rec] [--hashes FILE] [--quiet]\n";
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
		else if (a == "--delay" && i + 1 < argc) delay = strtol(argv[++i], nullptr, 10);
		else if (a == "--hashes" && i + 1 < argc) hashes_path = argv[++i];
		else if (a == "--only-misses") only_misses = true;
		else if (a == "--rec") rec = true;
		else if (a == "--quiet") quiet = true;
		else if (a[0] != '-' && rom.empty()) rom = a;
		else {
			fprintf(stderr, "vf3soak: unknown argument %s\n%s", a.c_str(), usage);
			return 2;
		}
	}
	if (rom.empty() || script_path.empty() || delay < 1 || delay > 60) {
		fprintf(stderr, "%s", usage);
		return 2;
	}
	auto pa = std::make_unique<m3::Model3>(), pb = std::make_unique<m3::Model3>();
	m3::Model3 &A = *pa, &B = *pb;   // A straight through, B rolled back
	std::string error;
	if (!A.load(rom, error) || !B.load(rom, error)) {
		fprintf(stderr, "vf3soak: %s\n", error.c_str());
		return 1;
	}
	InputScript script;
	if (!script.load(script_path, error)) {
		fprintf(stderr, "vf3soak: %s\n", error.c_str());
		return 1;
	}
	FILE *hashes = nullptr;
	if (!hashes_path.empty() && !(hashes = fopen(hashes_path.c_str(), "w"))) {
		fprintf(stderr, "vf3soak: can't write %s\n", hashes_path.c_str());
		return 1;
	}
	ppcrec::Rec rec_a, rec_b;
	if (rec) {
		if (!rec_a.build(A.cpu, A.program_image()) || !rec_b.build(B.cpu, B.program_image())) {
			fprintf(stderr, "vf3soak: the recompile failed\n");
			return 1;
		}
		A.cpu.rec = &rec_a;
		B.cpu.rec = &rec_b;
	}
	A.rtc_time = B.rtc_time = 0;   // (as netplay will: the same clock on both sides)
	A.reset();
	B.reset();

	// B's save states, the one before each of the last delay + 1 frames (ring[f % size]: the state before frame f).
	// A runs delay frames behind B: it runs frame k once k is certain, after checking B's state before k is its own.
	const size_t ring_n = size_t(delay) + 1;
	std::vector<std::vector<uint8_t>> ring(ring_n);
	std::vector<m3::StateSection> sections;
	std::vector<uint8_t> straight;
	std::vector<m3::Controls> real, used;   // per frame: the script's inputs; what B last ran it with
	Stat t_save, t_load, t_run, t_tick;
	uint64_t rollbacks = 0, misses = 0, checked = 0;

	auto t0 = Clock::now();
	bool failed = false;
	for (uint64_t f = 0;; f++) {
		if (frames >= 0 && f >= uint64_t(frames)) break;
		m3::Controls c = real.empty() ? m3::Controls{} : real.back();
		InputScript::Actions act = script.step(c, f);
		if (act.quit) break;
		real.push_back(c);

		auto tick = Clock::now();
		if (f >= uint64_t(delay)) {
			// Player 2's input for frame k arrives: k is certain, and B's state before it was made with real inputs only
			const uint64_t k = f - uint64_t(delay);
			std::vector<uint8_t> &mine = ring[k % ring_n];
			A.save_state(straight, &sections);
			if (hashes) fprintf(hashes, "%llu %016llx\n", (unsigned long long)k, (unsigned long long)m3::state_hash(straight.data(), straight.size()));
			if (mine != straight) {
				printf("frame %llu: the rolled-back board's state differs from the straight one's (before the frame)\n", (unsigned long long)k);
				report(straight, mine, sections);
				if (write_file("soak_straight.bin", straight) && write_file("soak_rolled.bin", mine))
					printf("  both written: soak_straight.bin, soak_rolled.bin\n");
				failed = true;
				break;
			}
			checked++;
			A.controls = real[k];
			A.run_frame();

			// Back to that state and every frame since run again, with k's input (and it guessed held since), if the
			// guess for k was wrong, or always
			const bool miss = !same_p2(used[k], real[k]);
			misses += miss;
			tick = Clock::now();
			if (miss || !only_misses) {
				rollbacks++;
				auto t = Clock::now();
				if (!B.load_state(mine.data(), mine.size())) {
					printf("frame %llu: the save state didn't load\n", (unsigned long long)k);
					failed = true;
					break;
				}
				t_load.add(ms_since(t));
				for (uint64_t g = k; g < f; g++) {
					if (g != k) {
						t = Clock::now();
						B.save_state(ring[g % ring_n]);
						t_save.add(ms_since(t));
					}
					used[g] = mix(real[g], real[k]);
					t = Clock::now();
					B.controls = used[g];
					B.run_frame();
					t_run.add(ms_since(t));
				}
			}
		}
		// Frame f on B: its state before it kept, player 2's input guessed (the last one come: none yet at first)
		auto t = Clock::now();
		B.save_state(ring[f % ring_n]);
		t_save.add(ms_since(t));
		used.push_back(mix(real[f], f >= uint64_t(delay) ? real[f - uint64_t(delay)] : m3::Controls{}));
		t = Clock::now();
		B.controls = used[f];
		B.run_frame();
		t_run.add(ms_since(t));
		t_tick.add(ms_since(tick));

		if (!quiet && f % 1000 == 999)
			printf("frame %llu: %llu checked, %llu rollbacks, %llu guesses wrong\n", (unsigned long long)f + 1,
			       (unsigned long long)checked, (unsigned long long)rollbacks, (unsigned long long)misses);
	}
	if (hashes) fclose(hashes);
	if (!quiet || !failed) {
		const double budget = 1000.0 / 57.524160;
		printf("%s: %llu frames checked, %llu rollbacks of %ld frames (%llu guesses wrong), %.1f s\n", failed ? "FAILED" : "identical",
		       (unsigned long long)checked, (unsigned long long)rollbacks, delay, (unsigned long long)misses, ms_since(t0) / 1000);
		printf("state %.1f MB; ms mean / worst: save %.2f / %.2f, restore %.2f / %.2f, a frame's run %.2f / %.2f\n",
		       double(ring[0].size()) / 1048576, t_save.mean(), t_save.worst, t_load.mean(), t_load.worst, t_run.mean(), t_run.worst);
		printf("a frame with its rollback: %.2f ms mean, %.2f worst, of the %.1f ms a frame has (the board only: no picture, no sound)\n",
		       t_tick.mean(), t_tick.worst, budget);
	}
	return failed ? 1 : 0;
}
