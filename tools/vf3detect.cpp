// vf3detect: finds the game's round counters (the netplay detector's: src/host/vf3_detect.h), the way Fightcade's
// detectors do, from the RAM. Plays an input script (two random players: tools/ref/gen_scripts.py's versus.txt) on
// the recompiled code, and each frame looks at every byte of RAM that changed: a round counter only ever goes up by
// one, or back to 0, and stays small. Prints those, with when they changed, and (--check A,B) checks the two given
// as the players' counters against a run: their wins and the matches each won.
//   vf3detect <vf3.zip> --script FILE [--frames N] [--check P1ADDR,P2ADDR] [--shots DIR]
// Addresses are the PowerPC's (hex), as the RAM's bytes are seen by the game (it holds host-order words).
#include "../src/host/input_script.h"
#include "../src/host/vf3_netplay.h"
#include "../src/machine/model3.h"
#include "../src/rec/ppc_rec.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Byte
{
	uint16_t ups = 0, resets = 0, other = 0;
	uint8_t top = 0;
};

} // namespace

int main(int argc, char **argv)
{
	std::string rom, script_path;
	long frames = -1;
	uint32_t check[2] = { 0, 0 };
	bool checking = false, rare = false, scoring = false;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
		else if (a == "--check" && i + 1 < argc) {
			const std::string v = argv[++i];
			check[0] = uint32_t(strtoul(v.c_str(), nullptr, 16));
			check[1] = uint32_t(strtoul(v.substr(v.find(',') + 1).c_str(), nullptr, 16));
			checking = true;
		} else if (a == "--score") scoring = true;   // (the detector itself, vf3net::Score: what it counts, and when)
		else if (a == "--rare") rare = true;   // (every byte that changes only a few times: with when, for the players' joined flags)
		else if (a[0] != '-' && rom.empty()) rom = a;
		else { fprintf(stderr, "usage: vf3detect <vf3.zip> --script FILE [--frames N] [--check P1ADDR,P2ADDR]\n"); return 2; }
	}
	auto pb = std::make_unique<m3::Model3>();
	m3::Model3 &board = *pb;
	std::string error;
	InputScript script;
	if (rom.empty() || script_path.empty() || !board.load(rom, error) || !script.load(script_path, error)) {
		fprintf(stderr, "vf3detect: %s\n", error.empty() ? "a ROM and a script, please" : error.c_str());
		return 1;
	}
	ppcrec::Rec rec;
	if (!rec.build(board.cpu, board.program_image())) { fprintf(stderr, "vf3detect: the recompile failed\n"); return 1; }
	board.cpu.rec = &rec;
	board.rtc_time = 0;
	board.reset();

	const size_t n = board.ram.size();
	std::vector<uint8_t> prev = board.ram;
	std::vector<Byte> st(n);
	struct Change { uint64_t frame; uint32_t addr; uint8_t from, to; };
	std::vector<Change> log;
	std::vector<std::vector<std::pair<uint32_t, uint8_t>>> few(rare ? n : 0);   // (--rare: each byte's first 16 changes)   // (the candidates' changes, kept for the ones still candidates at the end: all, filtered after)
	auto ppc = [](size_t i) { return uint32_t(i ^ 3); };   // (a host-order word's byte: the game's address)
	uint8_t was[2] = { 0, 0 };
	int wins[2] = { 0, 0 }, matches[2] = { 0, 0 };
	vf3net::Score score;
	int said[2][2] = {};
	uint64_t f = 0;
	m3::Controls c{};   // (held from frame to frame, as the script says)
	for (;; f++) {
		if (frames >= 0 && f >= uint64_t(frames)) break;
		InputScript::Actions act = script.step(c, f);
		if (act.quit) break;
		board.controls = c;
		board.run_frame();
		const uint64_t *a = reinterpret_cast<const uint64_t *>(board.ram.data()), *b = reinterpret_cast<const uint64_t *>(prev.data());
		for (size_t w = 0; w < n / 8; w++) {
			if (a[w] == b[w]) continue;
			for (size_t k = w * 8; k < w * 8 + 8; k++) {
				const uint8_t v = board.ram[k], p = prev[k];
				if (v == p) continue;
				Byte &s = st[k];
				if (v == p + 1) s.ups++;
				else if (v == 0) s.resets++;
				else s.other++;
				s.top = std::max(s.top, v);
				if (s.other == 0 && s.top <= 5 && log.size() < 4000000) log.push_back({ f, ppc(k), p, v });
				if (rare && few[k].size() < 17) few[k].push_back({ uint32_t(f), v });
				prev[k] = v;
			}
		}
		if (scoring) {
			score.frame(board);
			static int was_players = -1;
			if (score.players() != was_players) { printf("frame %llu: playing %d\n", (unsigned long long)f, score.players()); was_players = score.players(); }
			for (int p = 0; p < 2; p++)
				if (score.matches[p] != said[0][p] || score.rounds[p] != said[1][p]) {
					printf("frame %llu: rounds %d-%d, matches %d-%d\n", (unsigned long long)f, score.rounds[0], score.rounds[1], score.matches[0], score.matches[1]);
					said[0][0] = score.matches[0]; said[0][1] = score.matches[1]; said[1][0] = score.rounds[0]; said[1][1] = score.rounds[1];
					break;
				}
		}
		if (checking) {
			uint8_t v[2];
			for (int p = 0; p < 2; p++) {
				v[p] = board.ram[check[p] ^ 3];
				if (v[p] == was[p] + 1) { wins[p]++; printf("frame %llu: player %d won a round (%d)\n", (unsigned long long)f, p + 1, v[p]); }
			}
			// both back to 0 after some: that match over, won by whoever had more
			if (v[0] == 0 && v[1] == 0 && was[0] + was[1] > 0) {
				const int w = was[0] > was[1] ? 0 : was[1] > was[0] ? 1 : -1;
				if (w >= 0) matches[w]++;
				printf("frame %llu: the match over: %d-%d (%s)\n", (unsigned long long)f, was[0], was[1], w < 0 ? "a draw" : w == 0 ? "player 1's" : "player 2's");
			}
			was[0] = v[0];
			was[1] = v[1];
		}
	}
	printf("%llu frames\n", (unsigned long long)f);
	if (scoring) { printf("the score: matches %d-%d (rounds in them %d-%d)\n", score.matches[0], score.matches[1], score.rounds[0], score.rounds[1]); return 0; }
	if (rare) {
		for (size_t k = 0; k < n; k++)
			if (!few[k].empty() && few[k].size() <= 16 && f > 2000 && few[k].front().first > 1500) {
				printf("%08x:", ppc(k));
				for (auto &[fr, v] : few[k]) printf(" %u:%u", fr, v);
				printf("\n");
			}
		return 0;
	}
	if (checking) { printf("rounds won: %d-%d, matches won: %d-%d\n", wins[0], wins[1], matches[0], matches[1]); return 0; }
	std::vector<size_t> cands;
	for (size_t k = 0; k < n; k++)
		if (st[k].other == 0 && st[k].ups >= 3 && st[k].resets >= 1 && st[k].top >= 2 && st[k].top <= 5) cands.push_back(k);
	printf("%zu candidates (only ever +1 or back to 0, 2 to 5 at most, 3 ups at least)\n", cands.size());
	for (size_t k : cands) {
		printf("%08x: %u ups, %u resets, top %u:", ppc(k), st[k].ups, st[k].resets, st[k].top);
		int shown = 0;
		for (const Change &ch : log)
			if (ch.addr == ppc(k) && shown++ < 40) printf(" %llu:%u", (unsigned long long)ch.frame, ch.to);
		printf("\n");
	}
	return 0;
}
