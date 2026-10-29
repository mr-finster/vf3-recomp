// The PowerPC recompiler: VF3's program, made into native code (ARM64 or x86-64, with SLJIT) when the game
// starts, from the player's own ROM.
//
// The program is in the ROM: the boot copies 0xA0000 bytes of the fixed CROM (from offset 0x710000) to RAM 0, and
// VF3 never changes that code afterwards (docs/step1-measure.md). The code is found by walking the image from
// the address map (src/rec/vf3_map.cpp: entry points, the indirect jumps' targets and the places reached only
// by returning from an interrupt; addresses only), and every instruction found gets native code and a label,
// so it can be entered anywhere an interrupt returns to.
//
// The interpreter's order is kept exactly (ppc.h): every instruction counts its cycle and checks the end of the
// slice and the decrementer's trigger where the interpreter does, and anything that can take an interrupt or
// change the flow in other ways (device accesses, mtmsr, rfi, sc, traps, timers) goes through the interpreter's
// own handler for it and then back to the dispatcher. Common instructions are native code; the rest call that
// handler. The native code runs only while every instruction it was made from is in RAM unchanged; anything
// else (the boot code in CROM, code it has no label for) runs on the interpreter.
#pragma once
#include "../ppc/ppc.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace ppcrec {

struct Spin;

struct Stats
{
	size_t instructions = 0, native = 0, code_bytes = 0;
	double ms = 0;
};

// The address maps this build ships with: Revision D's (src/rec/vf3_map.cpp, from coverage runs) and Team Battle's
// (src/rec/vf3tb_map.cpp, learnt: tools/recomp/mklearnt.py)
extern const uint32_t vf3_map[], vf3tb_map[];
extern const size_t vf3_map_count, vf3tb_map_count;

// Which instructions to compile: walked from the seeds through the direct flow, within [lo, hi)
std::vector<uint32_t> analyse(const std::vector<uint32_t> &image, uint32_t lo, uint32_t hi, const std::vector<uint32_t> &seeds);

class Rec : public ppc::Recompiler
{
public:
	Rec();
	~Rec() override;
	// image: the program as RAM holds it after the boot copy, words from 0 (host order); map: its address map
	// (the board's profile's: vf3_map for Revision D). False if no code could be made (the interpreter then runs
	// everything).
	bool build(ppc::Cpu &cpu, const std::vector<uint32_t> &image, const uint32_t *map, size_t map_count, Stats *stats = nullptr);
	bool run(ppc::Cpu &cpu) override;
	void code_written(uint32_t addr) override;
	void recount() override;
	bool enabled() const { return built && mismatched == 0; }

	// for the native code's helpers
	ppc::Cpu *cpu = nullptr;
	std::vector<uintptr_t> label;   // per RAM word: its native code, or 0
	uint64_t entries = 0;           // times the native code was entered
	// what the interpreter ran instead: while the code in RAM wasn't the program (the boot, before its copy),
	// outside the program (the boot code in CROM), or where no code was made (an address the map didn't have)
	uint64_t interp_disabled = 0, interp_outside = 0, interp_unmapped = 0;
	std::vector<uint32_t> unmapped;   // the addresses of the last kind (the first unmapped_max)
	size_t unmapped_max = 512;
	std::vector<uint8_t> unmapped_seen;   // (past 512: which are in it, a byte a word)

private:
	bool built = false;
	void *code = nullptr;
	void (*entry)(ppc::State *, uintptr_t) = nullptr;
	std::vector<uint32_t> compiled;  // per RAM word: the instruction compiled there
	std::vector<uint8_t> map, bad;   // per RAM word: compiled; compiled and RAM now differs
	size_t mismatched = 0;
	std::vector<std::unique_ptr<Spin>> spins;
};

} // namespace ppcrec
