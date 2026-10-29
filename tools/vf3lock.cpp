// vf3lock: the lockstep checker. Two boards side by side, one on the interpreter and one on the recompiled code
// (src/rec), playing the same input script; after every frame their CPU state, RAM, backup RAM and the Real3D's
// and tile generator's memories are compared. At the first difference the frame is run again on both, compared
// after every slice, and the first slice that differs is reported with what differs.
//   vf3lock <vf3.zip> --script FILE [--frames N] [--quiet]
#include "../src/host/input_script.h"
#include "../src/machine/model3.h"
#include "../src/rec/ppc_rec.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

const char *field_name(int i, char *buf)
{
	static const char *names[] = { "cr", "xer", "lr", "ctr", "msr", "npc", "srr0", "srr1", "dec", "tbl", "tbu", "fpscr",
		"sprg0", "sprg1", "sprg2", "sprg3", "hid0", "hid1" };
	if (i < 32) snprintf(buf, 16, "r%d", i);
	else if (i < 96) snprintf(buf, 16, "f%d.%s", (i - 32) / 2, (i & 1) ? "lo" : "hi");
	else if (i < 114) return names[i - 96];
	else if (i == 117) return "irq_pending";
	else if (i == 118) return "timer_frac";
	else snprintf(buf, 16, "w%d", i);
	return buf;
}

struct Snap
{
	uint32_t s[128];
	int icount;
	uint64_t total;
};

Snap snap(m3::Model3 &b)
{
	Snap x;
	b.cpu.dump_state(x.s);
	x.icount = b.cpu.s.icount;
	x.total = b.cpu.s.total_cycles;
	return x;
}

// What differs between two boards (CPU state, then the memories): empty when nothing does
std::string diff(m3::Model3 &a, m3::Model3 &b)
{
	std::string out;
	char buf[160], nb[16];
	Snap x = snap(a), y = snap(b);
	for (int i = 0; i < 128; i++)
		if (x.s[i] != y.s[i]) {
			snprintf(buf, sizeof(buf), "  %-12s interpreter %08X  recompiled %08X\n", field_name(i, nb), x.s[i], y.s[i]);
			out += buf;
		}
	if (a.cpu.s.pc != b.cpu.s.pc) {
		snprintf(buf, sizeof(buf), "  %-12s interpreter %08X  recompiled %08X\n", "pc", a.cpu.s.pc, b.cpu.s.pc);
		out += buf;
	}
	if (x.total != y.total) {
		snprintf(buf, sizeof(buf), "  %-12s interpreter %llu  recompiled %llu\n", "cycles", (unsigned long long)x.total, (unsigned long long)y.total);
		out += buf;
	}
	auto mem = [&](const char *name, const void *p, const void *q, size_t n, int unit) {
		const uint8_t *u = (const uint8_t *)p, *v = (const uint8_t *)q;
		if (!memcmp(u, v, n)) return;
		size_t first = 0, count = 0;
		for (size_t i = 0; i < n; i += size_t(unit))
			if (memcmp(u + i, v + i, size_t(unit))) { if (!count) first = i; count++; }
		snprintf(buf, sizeof(buf), "  %s: %zu words differ, the first at %zX\n", name, count, first);
		out += buf;
	};
	mem("RAM", a.ram.data(), b.ram.data(), a.ram.size(), 4);
	mem("backup RAM", a.backup.data(), b.backup.data(), a.backup.size(), 4);
	mem("tile RAM", a.tile_ram.data(), b.tile_ram.data(), a.tile_ram.size(), 4);
	mem("polygon RAM", a.poly_ram.data(), b.poly_ram.data(), a.poly_ram.size() * 4, 4);
	mem("culling RAM", a.cull_lo.data(), b.cull_lo.data(), a.cull_lo.size() * 4, 4);
	mem("culling RAM (high)", a.cull_hi.data(), b.cull_hi.data(), a.cull_hi.size() * 4, 4);
	if (a.midi != b.midi) out += "  MIDI bytes differ\n";
	return out;
}

} // namespace

int main(int argc, char **argv)
{
	std::string rom, script_path;
	long frames = -1;
	bool quiet = false;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc) frames = strtol(argv[++i], nullptr, 10);
		else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
		else if (a == "--quiet") quiet = true;
		else if (a[0] != '-' && rom.empty()) rom = a;
		else {
			fprintf(stderr, "vf3lock: unknown argument %s\nusage: vf3lock <vf3.zip> --script FILE [--frames N] [--quiet]\n", a.c_str());
			return 2;
		}
	}
	if (rom.empty() || (frames < 0 && script_path.empty())) {
		fprintf(stderr, "usage: vf3lock <vf3.zip> --script FILE [--frames N] [--quiet]\n");
		return 2;
	}
	auto ia = std::make_unique<m3::Model3>(), ib = std::make_unique<m3::Model3>();
	m3::Model3 &A = *ia, &B = *ib;
	std::string error;
	if (!A.load(rom, error) || !B.load(rom, error)) {
		fprintf(stderr, "vf3lock: %s\n", error.c_str());
		return 1;
	}
	InputScript sa, sb;
	if (!script_path.empty() && (!sa.load(script_path, error) || !sb.load(script_path, error))) {
		fprintf(stderr, "vf3lock: %s\n", error.c_str());
		return 1;
	}
	ppcrec::Rec rec;
	ppcrec::Stats st;
	if (!rec.build(B.cpu, B.program_image(), B.profile->map, B.profile->map_count, &st)) {
		fprintf(stderr, "vf3lock: the recompile failed\n");
		return 1;
	}
	B.cpu.rec = &rec;
	A.reset();
	B.reset();
	auto t0 = std::chrono::steady_clock::now();
	for (uint64_t f = 0;; f++) {
		if (frames >= 0 && f >= uint64_t(frames))
			break;
		auto act = sa.step(A.controls, f);
		sb.step(B.controls, f);
		A.run_frame();
		B.run_frame();
		std::string d = diff(A, B);
		if (d.empty()) {
			if (act.quit) break;
			continue;
		}
		printf("frame %llu differs:\n%s", (unsigned long long)A.frame, d.c_str());
		// Again from the start on fresh boards, to the frame before; then that frame compared after every slice
		auto ja = std::make_unique<m3::Model3>(), jb = std::make_unique<m3::Model3>();
		m3::Model3 &a2 = *ja, &b2 = *jb;
		a2.load(rom, error);
		b2.load(rom, error);
		ppcrec::Rec rec2;
		rec2.build(b2.cpu, b2.program_image(), b2.profile->map, b2.profile->map_count);
		b2.cpu.rec = &rec2;
		a2.reset();
		b2.reset();
		InputScript ta, tb;
		if (!script_path.empty()) { ta.load(script_path, error); tb.load(script_path, error); }
		for (uint64_t g = 0; g < f; g++) {
			ta.step(a2.controls, g); a2.run_frame();
			tb.step(b2.controls, g); b2.run_frame();
		}
		ta.step(a2.controls, f);
		tb.step(b2.controls, f);
		struct S { Snap x; uint32_t pc; uint64_t ram; };
		std::vector<S> slices;
		a2.after_slice = [&]() { slices.push_back({ snap(a2), a2.cpu.s.pc, a2.ram_hash() }); };
		a2.run_frame();
		size_t n = 0;
		bool told = false;
		b2.after_slice = [&]() {
			if (told || n >= slices.size()) { n++; return; }
			Snap y = snap(b2);
			const S &x = slices[n];
			std::string dd;
			char buf[160], nb[16];
			for (int i = 0; i < 128; i++)
				if (x.x.s[i] != y.s[i]) {
					snprintf(buf, sizeof(buf), "  %-12s interpreter %08X  recompiled %08X\n", field_name(i, nb), x.x.s[i], y.s[i]);
					dd += buf;
				}
			if (x.pc != b2.cpu.s.pc) { snprintf(buf, sizeof(buf), "  %-12s interpreter %08X  recompiled %08X\n", "pc", x.pc, b2.cpu.s.pc); dd += buf; }
			if (x.ram != b2.ram_hash()) dd += "  RAM differs\n";
			if (x.x.total != y.total) { snprintf(buf, sizeof(buf), "  cycles      interpreter %llu  recompiled %llu\n", (unsigned long long)x.x.total, (unsigned long long)y.total); dd += buf; }
			if (!dd.empty()) {
				printf("slice %zu of frame %llu differs first:\n%s", n, (unsigned long long)f + 1, dd.c_str());
				told = true;
			}
			n++;
		};
		b2.run_frame();
		if (!told) printf("(no slice differs when run again)\n");
		return 1;
	}
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (!quiet)
		printf("%llu frames identical (interpreter and recompiled), %.1f s\n", (unsigned long long)A.frame, secs);
	return 0;
}
