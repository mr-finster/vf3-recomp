// The PowerPC recompiler. See ppc_rec.h.
//
// One piece of native code for the whole program: a label at every instruction, entered at the one for NPC.
// Each instruction does its work, takes its cycle off the count, and goes on: to the next instruction's code
// directly; through the dispatcher when the flow can't be known here (an indirect branch, or the interpreter's
// handler changed NPC: an exception, an interrupt); back to the interpreter's loop at the end of the slice, at
// code without a label, or when the CPU stopped. At the decrementer's trigger cycle it does what the
// interpreter's loop does there.
//
// The CPU's state stays in ppc::State: the native code keeps nothing in registers from one instruction to the
// next, so it can stop after any instruction, as the interpreter does. PC and NPC are stored only where
// something can look at them: before the interpreter's handlers and device accesses, and on the way out.
#include "ppc_rec.h"
#include "../../third_party/sljit/sljitLir.h"
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <algorithm>
#include <map>
#include <memory>

namespace ppcrec {

using ppc::Cpu;
using ppc::State;

// ---- which words are instructions (as the interpreter decodes them) ----

static bool valid(uint32_t op)
{
	const uint32_t p = op >> 26, x = (op >> 1) & 0x3FF;
	switch (p) {
	case 3: case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18:
	case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
		return true;
	case 19:
		switch (x) {
		case 0: case 16: case 33: case 50: case 129: case 150: case 193: case 225: case 257: case 289: case 417:
		case 449: case 528: return true;
		default: return false;
		}
	case 31: {
		static const uint16_t ok[] = { 0, 4, 8, 520, 10, 522, 11, 19, 20, 23, 24, 26, 28, 540, 32, 40, 552, 54, 86, 246,
			278, 470, 1014, 55, 60, 75, 83, 87, 104, 616, 119, 124, 136, 648, 138, 650, 144, 146, 150, 151, 183, 200, 712,
			202, 714, 210, 215, 232, 744, 234, 746, 235, 747, 242, 247, 266, 778, 279, 284, 306, 370, 566, 311, 316, 339,
			343, 371, 375, 407, 412, 439, 444, 459, 971, 467, 476, 491, 1003, 512, 533, 597, 534, 535, 567, 536, 595, 598,
			599, 631, 659, 661, 725, 662, 663, 695, 727, 759, 758, 790, 792, 824, 854, 918, 922, 954, 982, 983 };
		for (uint16_t v : ok) if (v == x) return true;
		return false;
	}
	case 59: {
		const uint32_t lo = x & 31;
		return lo == 25 || lo >= 28 || x == 18 || x == 20 || x == 21 || x == 22 || x == 24;
	}
	case 63: {
		const uint32_t lo = x & 31;
		if (lo == 23 || lo == 25 || lo >= 28) return true;
		static const uint16_t ok[] = { 0, 32, 12, 14, 15, 18, 20, 21, 22, 26, 38, 70, 40, 64, 72, 134, 136, 264, 583, 711 };
		for (uint16_t v : ok) if (v == x) return true;
		return false;
	}
	default:
		return p >= 32 && p <= 55;
	}
}

std::vector<uint32_t> analyse(const std::vector<uint32_t> &image, uint32_t lo, uint32_t hi, const std::vector<uint32_t> &seeds)
{
	std::vector<uint8_t> seen(hi / 4, 0);
	std::vector<uint32_t> work(seeds.begin(), seeds.end());
	for (uint32_t v = 0x100; v <= 0x1400; v += 0x100) work.push_back(v);   // the exception vectors (MSR[IP] clear)
	while (!work.empty()) {
		uint32_t a = work.back();
		work.pop_back();
		while (a >= lo && a < hi && !(a & 3) && !seen[a >> 2]) {
			const uint32_t op = image[a >> 2];
			if (!valid(op))
				break;
			seen[a >> 2] = 1;
			const uint32_t p = op >> 26;
			if (p == 18) {   // b
				uint32_t li = op & 0x3FFFFFC;
				if (li & 0x2000000) li |= 0xFC000000;
				work.push_back(li + (op & 2 ? 0 : a));
				if (!(op & 1)) break;
			} else if (p == 16) {   // bc
				work.push_back(uint32_t(int32_t(int16_t(op & 0xFFFC))) + (op & 2 ? 0 : a));
				if ((((op >> 21) & 0x14) == 0x14) && !(op & 1)) break;
			} else if (p == 19) {
				const uint32_t x = (op >> 1) & 0x3FF;
				if ((x == 16 || x == 528) && (((op >> 21) & 0x14) == 0x14) && !(op & 1)) break;  // blr, bctr
				if (x == 50) break;   // rfi
			}
			a += 4;
		}
	}
	std::vector<uint32_t> out;
	for (uint32_t i = 0; i < hi / 4; i++)
		if (seen[i]) out.push_back(i * 4);
	return out;
}

// A spin loop's shape (see h_spin)
struct Spin
{
	uint32_t head, len;
	std::vector<std::pair<int, int32_t>> loads;
	std::vector<int> sizes;
	uint8_t ran = 0;   // its first instruction ran since the native code was entered or dispatched (no interrupt between)
};

// ---- what the native code calls ----

namespace {

// VF3_REC_PROFILE=1: at exit, the instructions the interpreter's handlers ran most for the native code
const bool PROFILE = getenv("VF3_REC_PROFILE") != nullptr;
uint64_t prof[64][1024];
struct ProfileDump
{
	~ProfileDump()
	{
		if (!PROFILE) return;
		std::vector<std::pair<uint64_t, int>> v;
		for (int p = 0; p < 64; p++)
			for (int x = 0; x < 1024; x++)
				if (prof[p][x]) v.push_back({ prof[p][x], p * 1024 + x });
		std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
		for (size_t i = 0; i < v.size() && i < 30; i++)
			fprintf(stderr, "opcode %2d/%4d %12llu\n", v[i].second / 1024, v[i].second % 1024, (unsigned long long)v[i].first);
	}
} profile_dump;

void SLJIT_FUNC h_exec(Cpu *c, sljit_u32 op)
{
	if (PROFILE) {
		const uint32_t p = op >> 26;
		prof[p][(p == 19 || p == 31 || p == 59 || p == 63) ? (op >> 1) & 0x3FF : 0]++;
	}
	c->exec_op(op);
}
void SLJIT_FUNC h_dec(Cpu *c) { c->dec_trigger(); }
void SLJIT_FUNC h_badpc(Cpu *c, sljit_u32 t) { c->branch_check(t); }   // a branch to nowhere stops the CPU
sljit_uw SLJIT_FUNC h_label(Rec *r)
{
	const State &s = r->cpu->s;
	if (!r->enabled() || s.fatal || s.icount <= 0 || s.npc >= r->code_limit)
		return 0;
	return r->label[s.npc >> 2];
}
void SLJIT_FUNC h_codew(Rec *r, sljit_u32 a) { r->code_written(a); }

// A spin loop (loads from fixed places, compares, a branch back: the idle loop waiting for an interrupt). Within
// a slice nothing but the CPU changes RAM and interrupts come only at the decrementer's trigger, so while the
// places it reads are plain RAM every round does the same thing: the rounds that can't reach the end of the
// slice or the trigger are skipped, their cycles counted.
void SLJIT_FUNC h_spin(Cpu *c, sljit_uw spin)
{
	const Spin &sp = *reinterpret_cast<const Spin *>(spin);
	State &s = c->s;
	for (size_t i = 0; i < sp.loads.size(); i++) {
		const uint32_t a = (sp.loads[i].first ? s.r[sp.loads[i].first] : 0) + uint32_t(sp.loads[i].second);
		if (a >= 0x00800000 || (a & uint32_t(sp.sizes[i] - 1)))
			return;
	}
	const int ic = s.icount, n = int(sp.len);
	if (ic <= n)
		return;
	int k = (ic - 1) / n;
	const int trig = s.dec_trigger_cycle;
	if (trig < ic && trig >= ic - k * n)
		k = trig >= 0 ? (ic - trig - 1) / n : k;
	if (k > 0)
		s.icount = ic - k * n;
}
sljit_u32 SLJIT_FUNC h_r8(Cpu *c, sljit_u32 a) { return c->bus_()->read8(a); }
sljit_u32 SLJIT_FUNC h_r16(Cpu *c, sljit_u32 a) { return c->bus_()->read16(a); }
sljit_u32 SLJIT_FUNC h_r32(Cpu *c, sljit_u32 a) { return c->bus_()->read32(a); }
void SLJIT_FUNC h_w8(Cpu *c, sljit_u32 a, sljit_u32 v) { c->bus_()->write8(a, uint8_t(v)); }
void SLJIT_FUNC h_w16(Cpu *c, sljit_u32 a, sljit_u32 v) { c->bus_()->write16(a, uint16_t(v)); }
void SLJIT_FUNC h_w32(Cpu *c, sljit_u32 a, sljit_u32 v) { c->bus_()->write32(a, v); }

const bool ALL_CALLS = getenv("VF3_REC_CALLS") != nullptr;   // testing: every instruction through the interpreter's handler

struct Gen
{
	sljit_compiler *C = nullptr;
	Rec &rec;
	Cpu &cpu;
	const std::vector<uint32_t> &image;
	uint32_t hi;
	std::vector<uint8_t> is_code;
	std::vector<sljit_label *> lab;
	std::vector<std::pair<sljit_jump *, uint32_t>> to_label;
	std::vector<sljit_jump *> to_exit, to_dispatch;
	size_t natives = 0;

	// deferred out-of-line pieces
	struct Stub { sljit_jump *from; int kind; uint32_t pc, next; };
	enum { STUB_END, STUB_TRIG, STUB_DYN_TRIG };
	std::vector<Stub> stubs;

	const sljit_sw o_r = offsetof(State, r), o_cr = offsetof(State, cr), o_xer = offsetof(State, xer),
		o_lr = offsetof(State, lr), o_ctr = offsetof(State, ctr), o_pc = offsetof(State, pc), o_npc = offsetof(State, npc),
		o_ic = offsetof(State, icount), o_trig = offsetof(State, dec_trigger_cycle), o_fatal = offsetof(State, fatal),
		o_f = offsetof(State, f), o_fpscr = offsetof(State, fpscr);
	sljit_sw f_lo(int i) const { return o_f + 8 * i; }       // the FPR's low and high 32 bits (little-endian host)
	sljit_sw f_hi(int i) const { return o_f + 8 * i + 4; }

	Gen(Rec &r, Cpu &c, const std::vector<uint32_t> &img, uint32_t h, std::vector<std::unique_ptr<Spin>> &sp)
		: rec(r), cpu(c), image(img), hi(h), spins(sp) {}

	bool compiled(uint32_t a) const { return a < hi && !(a & 3) && is_code[a >> 2]; }

	std::vector<std::unique_ptr<Spin>> &spins;
	std::map<uint32_t, Spin *> spin_branch, spin_head;
	Spin *spin_of_branch(uint32_t b) { auto it = spin_branch.find(b); return it == spin_branch.end() ? nullptr : it->second; }
	void clear_spins()
	{
		for (auto &sp : spins) sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM0(), sljit_sw(&sp->ran), SLJIT_IMM, 0);
	}
	// Is the bc at b, branching back to h, a spin loop: only loads (D-form, bases the loop doesn't change) and
	// compares between, no CTR, no link?
	Spin *spin_at(uint32_t b, uint32_t h)
	{
		const uint32_t op = image[b >> 2];
		if (h >= b || (op & 1) || !(((op >> 21) & 31) & 4) || b - h > 64)
			return nullptr;
		auto sp = std::make_unique<Spin>();
		std::set<int> written, bases;
		for (uint32_t a = h; a < b; a += 4) {
			if (!compiled(a)) return nullptr;
			const uint32_t w = image[a >> 2], p = w >> 26, x = (w >> 1) & 0x3FF;
			const int rd = (w >> 21) & 31, ra = (w >> 16) & 31;
			if (p == 32 || p == 34 || p == 40 || p == 42) {   // lwz, lbz, lhz, lha
				sp->loads.push_back({ ra, int32_t(int16_t(w & 0xFFFF)) });
				sp->sizes.push_back(p == 32 ? 4 : p == 34 ? 1 : 2);
				written.insert(rd);
				if (ra) bases.insert(ra);
			} else if (p == 10 || p == 11 || (p == 31 && (x == 0 || x == 32))) {
				// compares: only CR
			} else
				return nullptr;
		}
		for (int r : bases) if (written.count(r)) return nullptr;
		sp->len = (b - h) / 4 + 1;
		sp->head = h;
		if (getenv("VF3_REC_SPINS")) fprintf(stderr, "spin %05X-%05X (%u)\n", h, b, sp->len);
		spins.push_back(std::move(sp));
		return spins.back().get();
	}

	// state access
	void ld(sljit_s32 R, sljit_sw off) { sljit_emit_op1(C, SLJIT_MOV_U32, R, 0, SLJIT_MEM1(SLJIT_S0), off); }
	void st(sljit_sw off, sljit_s32 R) { sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), off, R, 0); }
	void sti(sljit_sw off, uint32_t v) { sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), off, SLJIT_IMM, sljit_sw(int32_t(v))); }
	void ldr(sljit_s32 R, int i) { ld(R, o_r + 4 * i); }
	void str(int i, sljit_s32 R) { st(o_r + 4 * i, R); }
	void imm(sljit_s32 R, uint32_t v) { sljit_emit_op1(C, SLJIT_MOV32, R, 0, SLJIT_IMM, sljit_sw(int32_t(v))); }
	void pcs(uint32_t pc, uint32_t npc) { sti(o_pc, pc); sti(o_npc, npc); }
	void call(sljit_s32 types, void *fn) { sljit_emit_icall(C, SLJIT_CALL, types, SLJIT_IMM, SLJIT_FUNC_ADDR(fn)); }

	void go(uint32_t t)   // jump to t's code, or out with NPC = t (PC already stored)
	{
		if (compiled(t)) to_label.push_back({ sljit_emit_jump(C, SLJIT_JUMP), t });
		else { sti(o_npc, t); to_exit.push_back(sljit_emit_jump(C, SLJIT_JUMP)); }
	}

	// After an instruction whose flow is known here: one cycle, the decrementer's trigger, the end of the
	// slice, then on to next
	void tail(uint32_t pc, uint32_t next, bool fall)
	{
		ld(SLJIT_R0, o_ic);
		sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		st(o_ic, SLJIT_R0);
		stubs.push_back({ sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_trig), STUB_TRIG, pc, next });
		stubs.push_back({ sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0), STUB_END, pc, next });
		if (fall && next == pc + 4 && compiled(next))
			return;   // the next instruction's code follows
		if (compiled(next)) to_label.push_back({ sljit_emit_jump(C, SLJIT_JUMP), next });
		else { pcs(pc, next); to_exit.push_back(sljit_emit_jump(C, SLJIT_JUMP)); }
	}

	// After the interpreter's handler or a device access (PC and NPC stored before it): it may have stopped the
	// CPU or changed NPC
	void tail_dynamic(uint32_t pc)
	{
		// (the cycle and the trigger count even when the CPU has stopped, as in the interpreter's loop)
		ld(SLJIT_R0, o_ic);
		sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		st(o_ic, SLJIT_R0);
		stubs.push_back({ sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_trig), STUB_DYN_TRIG, pc, 0 });
		to_exit.push_back(sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0));
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_fatal);
		to_exit.push_back(sljit_emit_cmp(C, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0));
		ld(SLJIT_R0, o_npc);
		to_dispatch.push_back(sljit_emit_cmp(C, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(int32_t(pc + 4))));
		if (compiled(pc + 4)) to_label.push_back({ sljit_emit_jump(C, SLJIT_JUMP), pc + 4 });
		else to_exit.push_back(sljit_emit_jump(C, SLJIT_JUMP));
	}

	// After a branch to the address in S2 (NPC = S2): through the dispatcher
	void tail_indirect(uint32_t pc)
	{
		sti(o_pc, pc);
		st(o_npc, SLJIT_S2);
		// a branch to nowhere (not RAM, not the fixed CROM) stops the CPU, as the interpreter's does
		sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_S2, 0, SLJIT_IMM, 0x00800000);
		sljit_jump *ok = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(int32_t(0xFF000000u)));
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		call(SLJIT_ARGS2V(P, 32), (void *)h_badpc);
		sljit_set_label(ok, sljit_emit_label(C));
		ld(SLJIT_R0, o_ic);
		sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		st(o_ic, SLJIT_R0);
		stubs.push_back({ sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_trig), STUB_DYN_TRIG, pc, 0 });
		to_exit.push_back(sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0));
		to_dispatch.push_back(sljit_emit_jump(C, SLJIT_JUMP));
	}

	// Instructions whose handler can't change the flow or stop the CPU (no memory, no exceptions, no timers):
	// called without PC and NPC, and on to the next instruction directly
	static bool pure(uint32_t op)
	{
		const uint32_t p = op >> 26, x = (op >> 1) & 0x3FF;
		if (!valid(op)) return false;
		if (p == 59 || p == 63) return true;
		if (p == 19) return x == 0 || x == 33 || x == 129 || x == 193 || x == 225 || x == 257 || x == 289 || x == 417 || x == 449;
		if (p == 31) {
			switch (x) {
			case 8: case 520: case 10: case 522: case 11: case 19: case 26: case 75: case 136: case 648: case 138: case 650:
			case 144: case 200: case 712: case 202: case 714: case 232: case 744: case 234: case 746: case 235: case 747:
			case 266: case 778: case 40: case 552: case 104: case 616: case 459: case 971: case 491: case 1003: case 512:
			case 28: case 540: case 60: case 124: case 284: case 316: case 412: case 444: case 476: case 24: case 536:
			case 792: case 824: case 922: case 954: case 0: case 32:
				return true;
			default: return false;
			}
		}
		return p == 7 || p == 8 || (p >= 10 && p <= 15) || p == 20 || p == 21 || p == 23 || (p >= 24 && p <= 29);
	}
	void pure_helper(uint32_t pc, uint32_t op)
	{
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
		imm(SLJIT_R1, op);
		call(SLJIT_ARGS2V(P, 32), (void *)h_exec);
		tail(pc, pc + 4, true);
	}

	// The interpreter's handler for the instruction
	void helper(uint32_t pc, uint32_t op)
	{
		if (pure(op))
			return pure_helper(pc, op);
		helper_full(pc, op);
	}
	void helper_full(uint32_t pc, uint32_t op)
	{
		pcs(pc, pc + 4);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
		imm(SLJIT_R1, op);
		call(SLJIT_ARGS2V(P, 32), (void *)h_exec);
		tail_dynamic(pc);
	}

	// CR field d from R0 compared with R1 (signed or not); uses R2-R4
	void compare(int d, bool sig)
	{
		sljit_emit_op2u(C, SLJIT_SUB32 | (sig ? SLJIT_SET_SIG_LESS : SLJIT_SET_LESS), SLJIT_R0, 0, SLJIT_R1, 0);
		sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_R2, 0, sig ? SLJIT_SIG_LESS : SLJIT_LESS);
		sljit_emit_op2u(C, SLJIT_SUB32 | (sig ? SLJIT_SET_SIG_GREATER : SLJIT_SET_GREATER), SLJIT_R0, 0, SLJIT_R1, 0);
		sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_R3, 0, sig ? SLJIT_SIG_GREATER : SLJIT_GREATER);
		set_cr(d);
	}
	// CR field d from lt (R2) and gt (R3), with EQ when neither, and SO
	void set_cr(int d)
	{
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R4, 0, SLJIT_R2, 0, SLJIT_R3, 0);
		sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 1);
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 1);
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 3);
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 2);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R2, 0);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R3, 0);
		ld(SLJIT_R2, o_xer);
		sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 31);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R2, 0);
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), o_cr + d, SLJIT_R4, 0);
	}
	// CR0 from the result in R0 (Rc = 1)
	void cr0()
	{
		imm(SLJIT_R1, 0);
		compare(0, true);
	}

	// ---- memory: the RAM fast path, else the bus (as the interpreter's) ----
	// The effective address in S2. Loads leave the value in R0 and go on to `done`.
	enum Size { B, H, HA, W };
	void load(uint32_t pc, Size sz, int rd, int ra_update)
	{
		load_with(pc, sz, ra_update, nullptr, [&]() { str(rd, SLJIT_R0); });
	}
	// The loaded value in R0: post() changes it (byte reversal), put() writes it where it goes
	template <class Put>
	void load_with(uint32_t pc, Size sz, int ra_update, void (*post)(Gen &), Put put)
	{
		const uint32_t mask = sz == B ? 0xFF800000u : (sz == W ? 0xFF800003u : 0xFF800001u);
		sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_S2, 0, SLJIT_IMM, sljit_sw(int32_t(mask)));
		sljit_jump *slow = sljit_emit_jump(C, SLJIT_NOT_ZERO);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		if (sz == B) sljit_emit_op2(C, SLJIT_XOR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 3);
		if (sz == H || sz == HA) sljit_emit_op2(C, SLJIT_XOR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 2);
		const sljit_s32 mov = sz == B ? SLJIT_MOV_U8 : sz == H ? SLJIT_MOV_U16 : sz == HA ? SLJIT_MOV_S16 : SLJIT_MOV_U32;
		sljit_emit_op1(C, mov, SLJIT_R0, 0, SLJIT_MEM2(SLJIT_S1, SLJIT_R1), 0);
		if (post) post(*this);
		put();
		if (ra_update >= 0) str(ra_update, SLJIT_S2);
		tail(pc, pc + 4, false);
		sljit_set_label(slow, sljit_emit_label(C));
		pcs(pc, pc + 4);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		call(SLJIT_ARGS2(32, P, 32), sz == B ? (void *)h_r8 : sz == W ? (void *)h_r32 : (void *)h_r16);
		if (sz == HA) sljit_emit_op1(C, SLJIT_MOV32_S16, SLJIT_R0, 0, SLJIT_R0, 0);
		if (post) post(*this);
		put();
		if (ra_update >= 0) str(ra_update, SLJIT_S2);
		tail_dynamic(pc);
	}
	// The value in R0 (kept in S3 across calls)
	void store(uint32_t pc, Size sz, int ra_update)
	{
		sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S3, 0, SLJIT_R0, 0);
		const uint32_t mask = sz == B ? 0xFF800000u : (sz == W ? 0xFF800003u : 0xFF800001u);
		sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_S2, 0, SLJIT_IMM, sljit_sw(int32_t(mask)));
		sljit_jump *slow = sljit_emit_jump(C, SLJIT_NOT_ZERO);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		if (sz == B) sljit_emit_op2(C, SLJIT_XOR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 3);
		if (sz == H) sljit_emit_op2(C, SLJIT_XOR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 2);
		const sljit_s32 mov = sz == B ? SLJIT_MOV_U8 : sz == H ? SLJIT_MOV_U16 : SLJIT_MOV32;
		sljit_emit_op1(C, mov, SLJIT_MEM2(SLJIT_S1, SLJIT_R1), 0, SLJIT_S3, 0);
		if (ra_update >= 0) str(ra_update, SLJIT_S2);
		// a store to compiled code: the runtime is told (and the code may no longer run)
		sljit_jump *above = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL | SLJIT_32, SLJIT_S2, 0, SLJIT_IMM, sljit_sw(hi));
		sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R1, 0, SLJIT_S2, 0, SLJIT_IMM, 2);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_R1, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, sljit_sw(rec.code_map));
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0, SLJIT_MEM2(SLJIT_R2, SLJIT_R1), 0);
		sljit_jump *hit = sljit_emit_cmp(C, SLJIT_NOT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0);
		sljit_set_label(above, sljit_emit_label(C));
		tail(pc, pc + 4, false);
		sljit_set_label(hit, sljit_emit_label(C));
		pcs(pc, pc + 4);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&rec));
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		call(SLJIT_ARGS2V(P, 32), (void *)h_codew);
		tail_dynamic(pc);
		sljit_set_label(slow, sljit_emit_label(C));
		pcs(pc, pc + 4);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_S3, 0);
		call(SLJIT_ARGS3V(P, 32, 32), sz == B ? (void *)h_w8 : sz == W ? (void *)h_w32 : (void *)h_w16);
		if (ra_update >= 0) str(ra_update, SLJIT_S2);
		tail_dynamic(pc);
	}
	void ea_d(int ra, uint32_t op, bool update)   // S2 = (ra|0) + d
	{
		const sljit_sw d = sljit_sw(int32_t(int16_t(op & 0xFFFF)));
		if (ra || update) {
			ldr(SLJIT_S2, ra);
			if (d) sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S2, 0, SLJIT_S2, 0, SLJIT_IMM, d);
		} else
			imm(SLJIT_S2, uint32_t(d));
	}
	void ea_x(int ra, int rb, bool update)   // S2 = (ra|0) + rb
	{
		ldr(SLJIT_S2, rb);
		if (ra || update) {
			ldr(SLJIT_R0, ra);
			sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S2, 0, SLJIT_S2, 0, SLJIT_R0, 0);
		}
	}

	// lfs: the single in R0 to the FPR as a double (the host's conversion, as the interpreter's)
	void put_single(int rd)
	{
		sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), 0, SLJIT_R0, 0);
		sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), 0);
		sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_S0), f_lo(rd), SLJIT_FR0, 0);
	}
	// stfs: the FPR as a single, into R0
	void get_single(int rs)
	{
		sljit_emit_fop1(C, SLJIT_CONV_F32_FROM_F64, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(rs));
		sljit_emit_fop1(C, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), 0, SLJIT_FR0, 0);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), 0);
	}
	// lfd, stfd: two words, both in RAM (else the interpreter's handler does the whole instruction)
	void load_double(uint32_t pc, uint32_t op, int rd, int ra_update)
	{
		sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_S2, 0, SLJIT_IMM, 3);
		sljit_jump *slow = sljit_emit_jump(C, SLJIT_NOT_ZERO);
		sljit_jump *slow2 = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL | SLJIT_32, SLJIT_S2, 0, SLJIT_IMM, 0x007FFFF8);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM2(SLJIT_S1, SLJIT_R1), 0);
		st(f_hi(rd), SLJIT_R0);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 4);
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM2(SLJIT_S1, SLJIT_R1), 0);
		st(f_lo(rd), SLJIT_R0);
		if (ra_update >= 0) str(ra_update, SLJIT_S2);
		tail(pc, pc + 4, false);
		sljit_label *l = sljit_emit_label(C);
		sljit_set_label(slow, l);
		sljit_set_label(slow2, l);
		helper_full(pc, op);
	}
	void store_double(uint32_t pc, uint32_t op, int rs, int ra_update)
	{
		sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_S2, 0, SLJIT_IMM, 3);
		sljit_jump *slow = sljit_emit_jump(C, SLJIT_NOT_ZERO);
		sljit_jump *slow2 = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL | SLJIT_32, SLJIT_S2, 0, SLJIT_IMM, 0x007FFFF8);
		// (compiled code is below code_limit: a store there goes the slow way too)
		sljit_jump *slow3 = sljit_emit_cmp(C, SLJIT_LESS | SLJIT_32, SLJIT_S2, 0, SLJIT_IMM, sljit_sw(hi));
		sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_S2, 0);
		ld(SLJIT_R0, f_hi(rs));
		sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM2(SLJIT_S1, SLJIT_R1), 0, SLJIT_R0, 0);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 4);
		ld(SLJIT_R0, f_lo(rs));
		sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM2(SLJIT_S1, SLJIT_R1), 0, SLJIT_R0, 0);
		if (ra_update >= 0) str(ra_update, SLJIT_S2);
		tail(pc, pc + 4, false);
		sljit_label *l = sljit_emit_label(C);
		sljit_set_label(slow, l);
		sljit_set_label(slow2, l);
		sljit_set_label(slow3, l);
		helper_full(pc, op);
	}
	// fmr, fneg, fabs, fnabs (Rc = 0): bits only
	void fmove(uint32_t op, int t, int b, uint32_t x)
	{
		ld(SLJIT_R0, f_lo(b));
		ld(SLJIT_R1, f_hi(b));
		if (x == 40) sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(int32_t(0x80000000u)));
		if (x == 264) sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x7FFFFFFF);
		if (x == 136) sljit_emit_op2(C, SLJIT_OR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(int32_t(0x80000000u)));
		st(f_lo(t), SLJIT_R0);
		st(f_hi(t), SLJIT_R1);
	}
	// FP arithmetic in round-to-nearest with a result that isn't a NaN: the host computes what the reference does
	// (each step rounded on its own). A NaN result, another rounding mode: the interpreter's handler, which has
	// the reference's NaN rules.
	enum FKind { F_ADD, F_SUB, F_MUL, F_DIV, F_MADD, F_MSUB, F_NMADD, F_NMSUB };
	void farith(uint32_t pc, uint32_t op, bool single, FKind k)
	{
		const int t = (op >> 21) & 31, a = (op >> 16) & 31, b = (op >> 11) & 31, c = (op >> 6) & 31;
		std::vector<sljit_jump *> slow;
		ld(SLJIT_R0, o_fpscr);
		sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, 3);
		slow.push_back(sljit_emit_jump(C, SLJIT_NOT_ZERO));
		sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(a));
		switch (k) {
		case F_ADD: sljit_emit_fop2(C, SLJIT_ADD_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(b)); break;
		case F_SUB: sljit_emit_fop2(C, SLJIT_SUB_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(b)); break;
		case F_MUL: sljit_emit_fop2(C, SLJIT_MUL_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(c)); break;
		case F_DIV: sljit_emit_fop2(C, SLJIT_DIV_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(b)); break;
		default:
			sljit_emit_fop2(C, SLJIT_MUL_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(c));
			sljit_emit_fop2(C, (k == F_MADD || k == F_NMADD) ? SLJIT_ADD_F64 : SLJIT_SUB_F64, SLJIT_FR0, 0, SLJIT_FR0, 0,
			                SLJIT_MEM1(SLJIT_S0), f_lo(b));
			break;
		}
		slow.push_back(sljit_emit_fcmp(C, SLJIT_UNORDERED, SLJIT_FR0, 0, SLJIT_FR0, 0));
		if (k == F_NMADD || k == F_NMSUB) sljit_emit_fop1(C, SLJIT_NEG_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
		if (single) {
			sljit_emit_fop1(C, SLJIT_CONV_F32_FROM_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
			sljit_emit_fop1(C, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0, SLJIT_FR0, 0);
		}
		sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_S0), f_lo(t), SLJIT_FR0, 0);
		fprf(t);
		tail(pc, pc + 4, false);
		sljit_label *l = sljit_emit_label(C);
		for (auto *j : slow) sljit_set_label(j, l);
		pure_helper_to(pc, op);
	}
	// FPSCR[FPRF] from FPR t (not a NaN): +-infinity, normal, denormal, zero
	void fprf(int t)
	{
		ld(SLJIT_R0, f_hi(t));
		ld(SLJIT_R1, f_lo(t));
		sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R2, 0, SLJIT_R0, 0, SLJIT_IMM, 20);
		sljit_emit_op2(C, SLJIT_AND32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 0x7FF);          // exponent
		sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R3, 0, SLJIT_R0, 0, SLJIT_IMM, 31);            // sign
		sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFFFFF);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);                // fraction != 0
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R4, 0, SLJIT_R3, 0, SLJIT_IMM, 2);
		sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 4);              // 4 or 8 by sign
		sljit_jump *inf = sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R2, 0, SLJIT_IMM, 0x7FF);
		sljit_jump *normal = sljit_emit_cmp(C, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R2, 0, SLJIT_IMM, 0);
		sljit_jump *denormal = sljit_emit_cmp(C, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, 0);
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R4, 0, SLJIT_R3, 0, SLJIT_IMM, 4);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 0x02);             // zero: 0x02, 0x12
		sljit_jump *done1 = sljit_emit_jump(C, SLJIT_JUMP);
		sljit_set_label(inf, sljit_emit_label(C));
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 0x01);             // 0x05, 0x09
		sljit_jump *done2 = sljit_emit_jump(C, SLJIT_JUMP);
		sljit_set_label(denormal, sljit_emit_label(C));
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 0x10);             // 0x14, 0x18
		sljit_label *done = sljit_emit_label(C);
		sljit_set_label(done1, done);
		sljit_set_label(done2, done);
		sljit_set_label(normal, done);                                                        // 0x04, 0x08
		ld(SLJIT_R0, o_fpscr);
		sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(int32_t(~0x1F000u)));
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 12);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R4, 0);
		st(o_fpscr, SLJIT_R0);
	}
	// fcmpu: CR field and FPSCR[FPCC] from comparing a and b (not NaNs: else the handler)
	void fcmpu(uint32_t pc, uint32_t op)
	{
		const int crf = (op >> 23) & 7, a = (op >> 16) & 31, b = (op >> 11) & 31;
		sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_S0), f_lo(a));
		sljit_emit_fop1(C, SLJIT_MOV_F64, SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_S0), f_lo(b));
		sljit_jump *nan = sljit_emit_fcmp(C, SLJIT_UNORDERED, SLJIT_FR0, 0, SLJIT_FR1, 0);
		imm(SLJIT_R0, 2);
		sljit_jump *lt = sljit_emit_fcmp(C, SLJIT_ORDERED_LESS, SLJIT_FR0, 0, SLJIT_FR1, 0);
		sljit_jump *gt = sljit_emit_fcmp(C, SLJIT_ORDERED_GREATER, SLJIT_FR0, 0, SLJIT_FR1, 0);
		sljit_jump *eq = sljit_emit_jump(C, SLJIT_JUMP);
		sljit_set_label(lt, sljit_emit_label(C));
		imm(SLJIT_R0, 8);
		sljit_jump *d1 = sljit_emit_jump(C, SLJIT_JUMP);
		sljit_set_label(gt, sljit_emit_label(C));
		imm(SLJIT_R0, 4);
		sljit_label *done = sljit_emit_label(C);
		sljit_set_label(eq, done);
		sljit_set_label(d1, done);
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), o_cr + crf, SLJIT_R0, 0);
		ld(SLJIT_R1, o_fpscr);
		sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(int32_t(~0x1F000u)));
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 12);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		st(o_fpscr, SLJIT_R1);
		tail(pc, pc + 4, false);
		sljit_set_label(nan, sljit_emit_label(C));
		pure_helper_to(pc, op);
	}
	void pure_helper_to(uint32_t pc, uint32_t op)   // the handler, then on (not falling through)
	{
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
		imm(SLJIT_R1, op);
		call(SLJIT_ARGS2V(P, 32), (void *)h_exec);
		tail(pc, pc + 4, false);
	}

	// CR bit operations (crand, cror, ...): bit bt from ba and bb
	void crbit(uint32_t op, uint32_t x)
	{
		const int bt = (op >> 21) & 31, ba = (op >> 16) & 31, bb = (op >> 11) & 31;
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_cr + ba / 4);
		sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 3 - ba % 4);
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0), o_cr + bb / 4);
		sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 3 - bb % 4);
		switch (x) {
		case 257: sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;  // crand
		case 449: sljit_emit_op2(C, SLJIT_OR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;   // cror
		case 193: sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;  // crxor
		case 225: sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1); break;  // crnand
		case 33: sljit_emit_op2(C, SLJIT_OR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1); break;   // crnor
		case 289: sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1); break;  // creqv
		case 129: sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1); sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;  // crandc
		case 417: sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1); sljit_emit_op2(C, SLJIT_OR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;   // crorc
		}
		sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 3 - bt % 4);
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0), o_cr + bt / 4);
		sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, ~(1 << (3 - bt % 4)) & 0xF);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), o_cr + bt / 4, SLJIT_R1, 0);
	}

	// ---- branches ----
	// CTR decrement and the condition: jumps to the returned list when the branch is not taken
	std::vector<sljit_jump *> condition(uint32_t bo, uint32_t bi, bool ctr_target)
	{
		std::vector<sljit_jump *> no;
		if (!(bo & 4)) {
			ld(SLJIT_R0, o_ctr);
			sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
			st(o_ctr, SLJIT_R0);
		}
		if (ctr_target) {   // bcctr: the target after the decrement
			ld(SLJIT_S2, o_ctr);
			sljit_emit_op2(C, SLJIT_AND32, SLJIT_S2, 0, SLJIT_S2, 0, SLJIT_IMM, sljit_sw(int32_t(~3u)));
		}
		if (!(bo & 4)) {
			ld(SLJIT_R0, o_ctr);
			no.push_back(sljit_emit_cmp(C, ((bo & 2) ? SLJIT_NOT_EQUAL : SLJIT_EQUAL) | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0));
		}
		if (!(bo & 0x10)) {
			sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_cr + bi / 4);
			sljit_emit_op2u(C, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, 1 << (3 - bi % 4));
			no.push_back(sljit_emit_jump(C, (bo & 8) ? SLJIT_ZERO : SLJIT_NOT_ZERO));
		}
		return no;
	}
	static bool valid_pc(uint32_t t) { return t <= 0x007FFFFF || t >= 0xFF800000; }

	// ---- one instruction ----
	void insn(uint32_t pc, uint32_t op)
	{
		const uint32_t p = op >> 26, rd = (op >> 21) & 31, ra = (op >> 16) & 31, rb = (op >> 11) & 31;
		const uint32_t x = (op >> 1) & 0x3FF;
		const bool rc = op & 1;
		const sljit_sw simm = sljit_sw(int32_t(int16_t(op & 0xFFFF)));
		const uint32_t uimm = op & 0xFFFF;
		if (ALL_CALLS) return helper(pc, op);
		natives++;
		switch (p) {
		case 7:  // mulli
			ldr(SLJIT_R0, ra);
			sljit_emit_op2(C, SLJIT_MUL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, simm);
			str(rd, SLJIT_R0);
			return tail(pc, pc + 4, true);
		case 10: case 11:  // cmpli, cmpi
			ldr(SLJIT_R0, ra);
			imm(SLJIT_R1, p == 11 ? uint32_t(simm) : uimm);
			compare(int((op >> 23) & 7), p == 11);
			return tail(pc, pc + 4, true);
		case 12: case 13:  // addic, addic.
			ldr(SLJIT_R0, ra);
			sljit_emit_op2(C, SLJIT_ADD32 | SLJIT_SET_CARRY, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, simm);
			sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_CARRY);
			str(rd, SLJIT_R0);
			set_ca(SLJIT_R1);
			if (p == 13) { ldr(SLJIT_R0, rd); cr0(); }
			return tail(pc, pc + 4, true);
		case 14: case 15:  // addi, addis
			if (ra) {
				ldr(SLJIT_R0, ra);
				sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, p == 14 ? simm : sljit_sw(int32_t(uimm << 16)));
				str(rd, SLJIT_R0);
			} else
				sti(o_r + 4 * rd, p == 14 ? uint32_t(simm) : uimm << 16);
			return tail(pc, pc + 4, true);
		case 16: {  // bc
			const uint32_t t = uint32_t(int32_t(int16_t(op & 0xFFFC))) + (op & 2 ? 0 : pc);
			if (!valid_pc(t)) break;
			auto no = condition(rd, ra, false);
			if (op & 1) sti(o_lr, pc + 4);
			if (Spin *sp = spin_of_branch(pc)) {
				// the branch's cycle and checks, then the rounds that can be skipped, then round again
				ld(SLJIT_R0, o_ic);
				sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
				st(o_ic, SLJIT_R0);
				stubs.push_back({ sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_trig), STUB_TRIG, pc, t });
				stubs.push_back({ sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0), STUB_END, pc, t });
				// (only when the whole round ran here: an interrupt in the middle may have changed what it reads)
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM0(), sljit_sw(&sp->ran));
				sljit_jump *fresh = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(sp));
				call(SLJIT_ARGS2V(P, W), (void *)h_spin);
				sljit_set_label(fresh, sljit_emit_label(C));
				to_label.push_back({ sljit_emit_jump(C, SLJIT_JUMP), t });
			} else
				tail(pc, t, false);
			sljit_label *l = sljit_emit_label(C);
			for (auto *j : no) sljit_set_label(j, l);
			if (op & 1) sti(o_lr, pc + 4);
			return tail(pc, pc + 4, true);
		}
		case 18: {  // b
			uint32_t li = op & 0x3FFFFFC;
			if (li & 0x2000000) li |= 0xFC000000;
			const uint32_t t = li + (op & 2 ? 0 : pc);
			if (!valid_pc(t)) break;
			if (op & 1) sti(o_lr, pc + 4);
			return tail(pc, t, false);
		}
		case 19:
			if (x == 16 || x == 528) {  // bclr, bcctr
				if (x == 16) {
					ld(SLJIT_S2, o_lr);
					sljit_emit_op2(C, SLJIT_AND32, SLJIT_S2, 0, SLJIT_S2, 0, SLJIT_IMM, sljit_sw(int32_t(~3u)));
				}
				auto no = condition(rd, ra, x == 528);
				if (op & 1) sti(o_lr, pc + 4);
				tail_indirect(pc);
				sljit_label *l = sljit_emit_label(C);
				for (auto *j : no) sljit_set_label(j, l);
				if (op & 1) sti(o_lr, pc + 4);
				return tail(pc, pc + 4, true);
			}
			if (x == 150) return tail(pc, pc + 4, true);  // isync
			if (x == 257 || x == 449 || x == 193 || x == 225 || x == 33 || x == 289 || x == 129 || x == 417) {
				crbit(op, x);
				return tail(pc, pc + 4, true);
			}
			break;
		case 20: case 21: case 23: {  // rlwimi, rlwinm, rlwnm
			const uint32_t mb = (op >> 6) & 31, me = (op >> 1) & 31;
			uint32_t m = (0xFFFFFFFFu >> mb) ^ (me >= 31 ? 0 : 0xFFFFFFFFu >> (me + 1));
			if (mb > me) m = ~m;
			ldr(SLJIT_R0, rd);
			if (p == 23) {
				ldr(SLJIT_R1, rb);
				sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 31);
				sljit_emit_op2(C, SLJIT_ROTL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			} else if (rb)
				sljit_emit_op2(C, SLJIT_ROTL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, rb);
			sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(int32_t(m)));
			if (p == 20) {
				ldr(SLJIT_R1, ra);
				sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(int32_t(~m)));
				sljit_emit_op2(C, SLJIT_OR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			}
			str(ra, SLJIT_R0);
			if (rc) cr0();
			return tail(pc, pc + 4, true);
		}
		case 24: case 25: case 26: case 27: case 28: case 29: {  // ori, oris, xori, xoris, andi., andis.
			const sljit_s32 o = p <= 25 ? SLJIT_OR32 : p <= 27 ? SLJIT_XOR32 : SLJIT_AND32;
			const uint32_t v = (p & 1) ? uimm << 16 : uimm;
			ldr(SLJIT_R0, rd);
			sljit_emit_op2(C, o, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(int32_t(v)));
			str(ra, SLJIT_R0);
			if (p >= 28) cr0();
			return tail(pc, pc + 4, true);
		}
		case 31:
			if (insn31(pc, op, x, rd, ra, rb, rc)) return;
			break;
		case 32: case 33: ea_d(ra, op, p == 33); return load(pc, W, rd, p == 33 ? int(ra) : -1);   // lwz, lwzu
		case 34: case 35: ea_d(ra, op, p == 35); return load(pc, B, rd, p == 35 ? int(ra) : -1);   // lbz, lbzu
		case 40: case 41: ea_d(ra, op, p == 41); return load(pc, H, rd, p == 41 ? int(ra) : -1);   // lhz, lhzu
		case 42: case 43: ea_d(ra, op, p == 43); return load(pc, HA, rd, p == 43 ? int(ra) : -1);  // lha, lhau
		case 48: case 49: ea_d(ra, op, p == 49); return load_with(pc, W, p == 49 ? int(ra) : -1, nullptr, [&]() { put_single(int(rd)); });  // lfs, lfsu
		case 50: case 51: ea_d(ra, op, p == 51); return load_double(pc, op, rd, p == 51 ? int(ra) : -1);  // lfd, lfdu
		case 52: case 53: ea_d(ra, op, p == 53); get_single(rd); return store(pc, W, p == 53 ? int(ra) : -1);  // stfs, stfsu
		case 54: case 55: ea_d(ra, op, p == 55); return store_double(pc, op, rd, p == 55 ? int(ra) : -1);  // stfd, stfdu
		case 59: case 63:
			if (p == 63 && !rc && (x == 72 || x == 40 || x == 264 || x == 136)) { fmove(op, rd, rb, x); return tail(pc, pc + 4, true); }
			if (p == 63 && x == 0) return fcmpu(pc, op);
			if (!rc) {
				const bool single = p == 59;
				switch (x & 31) {
				case 25: return farith(pc, op, single, F_MUL);
				case 28: return farith(pc, op, single, F_MSUB);
				case 29: return farith(pc, op, single, F_MADD);
				case 30: return farith(pc, op, single, F_NMSUB);
				case 31: return farith(pc, op, single, F_NMADD);
				default: break;
				}
				if (x == 18) return farith(pc, op, single, F_DIV);
				if (x == 20) return farith(pc, op, single, F_SUB);
				if (x == 21) return farith(pc, op, single, F_ADD);
			}
			break;
		case 36: case 37: ea_d(ra, op, p == 37); ldr(SLJIT_R0, rd); return store(pc, W, p == 37 ? int(ra) : -1);  // stw, stwu
		case 38: case 39: ea_d(ra, op, p == 39); ldr(SLJIT_R0, rd); return store(pc, B, p == 39 ? int(ra) : -1);  // stb, stbu
		case 44: case 45: ea_d(ra, op, p == 45); ldr(SLJIT_R0, rd); return store(pc, H, p == 45 ? int(ra) : -1);  // sth, sthu
		default: break;
		}
		natives--;
		helper(pc, op);
	}

	void set_ca(sljit_s32 R)   // XER[CA] = R (0 or 1)
	{
		ld(SLJIT_R2, o_xer);
		sljit_emit_op2(C, SLJIT_AND32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, sljit_sw(int32_t(~0x20000000u)));
		sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R3, 0, R, 0, SLJIT_IMM, 29);
		sljit_emit_op2(C, SLJIT_OR32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R3, 0);
		st(o_xer, SLJIT_R2);
	}

	bool insn31(uint32_t pc, uint32_t op, uint32_t x, uint32_t rd, uint32_t ra, uint32_t rb, bool rc)
	{
		auto bin = [&](sljit_s32 o, bool inv_b, bool inv_result) {   // RA = RS o RB
			ldr(SLJIT_R0, rd);
			ldr(SLJIT_R1, rb);
			if (inv_b) sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, -1);
			sljit_emit_op2(C, o, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			if (inv_result) sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, -1);
			str(ra, SLJIT_R0);
			if (rc) cr0();
			tail(pc, pc + 4, true);
			return true;
		};
		switch (x) {
		case 0: case 32:  // cmp, cmpl
			ldr(SLJIT_R0, ra);
			ldr(SLJIT_R1, rb);
			compare(int((op >> 23) & 7), x == 0);
			tail(pc, pc + 4, true);
			return true;
		case 28: return bin(SLJIT_AND32, false, false);    // and
		case 60: return bin(SLJIT_AND32, true, false);     // andc
		case 124: return bin(SLJIT_OR32, false, true);     // nor
		case 284: return bin(SLJIT_XOR32, false, true);    // eqv
		case 316: return bin(SLJIT_XOR32, false, false);   // xor
		case 412: return bin(SLJIT_OR32, true, false);     // orc
		case 444: return bin(SLJIT_OR32, false, false);    // or
		case 476: return bin(SLJIT_AND32, false, true);    // nand
		case 24: case 536: {  // slw, srw: shifts of 32 to 63 give 0
			ldr(SLJIT_R0, rd);
			ldr(SLJIT_R1, rb);
			sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x3F);
			sljit_jump *big = sljit_emit_cmp(C, SLJIT_GREATER | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, 31);
			sljit_emit_op2(C, x == 24 ? SLJIT_SHL32 : SLJIT_LSHR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_jump *done = sljit_emit_jump(C, SLJIT_JUMP);
			sljit_set_label(big, sljit_emit_label(C));
			imm(SLJIT_R0, 0);
			sljit_set_label(done, sljit_emit_label(C));
			str(ra, SLJIT_R0);
			if (rc) cr0();
			tail(pc, pc + 4, true);
			return true;
		}
		case 824: {  // srawi
			const uint32_t sh = rb;
			ldr(SLJIT_R0, rd);
			// CA: negative and a 1 shifted out
			sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(int32_t((1u << sh) - 1)));
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z, SLJIT_R1, 0, SLJIT_IMM, 0);
			sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_NOT_ZERO);
			sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R2, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
			sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0);
			if (sh) sljit_emit_op2(C, SLJIT_ASHR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, sh);
			str(ra, SLJIT_R0);
			set_ca(SLJIT_R1);
			if (rc) { ldr(SLJIT_R0, ra); cr0(); }
			tail(pc, pc + 4, true);
			return true;
		}
		case 922: case 954:  // extsh, extsb
			ldr(SLJIT_R0, rd);
			sljit_emit_op1(C, x == 922 ? SLJIT_MOV32_S16 : SLJIT_MOV32_S8, SLJIT_R0, 0, SLJIT_R0, 0);
			str(ra, SLJIT_R0);
			if (rc) cr0();
			tail(pc, pc + 4, true);
			return true;
		case 266: case 40: case 235: case 104:  // add, subf, mullw, neg (not the OE forms)
			ldr(SLJIT_R0, ra);
			if (x == 104) {
				sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_IMM, 0, SLJIT_R0, 0);
			} else {
				ldr(SLJIT_R1, rb);
				if (x == 266) sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				else if (x == 40) sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R0, 0, SLJIT_R1, 0, SLJIT_R0, 0);
				else sljit_emit_op2(C, SLJIT_MUL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			}
			str(rd, SLJIT_R0);
			if (rc) cr0();
			tail(pc, pc + 4, true);
			return true;
		case 339: case 467: {  // mfspr, mtspr: LR and CTR here
			const uint32_t spr = ((op >> 16) & 0x1F) | ((op >> 6) & 0x3E0);
			if (spr != 8 && spr != 9) return false;
			const sljit_sw o = spr == 8 ? o_lr : o_ctr;
			if (x == 339) { ld(SLJIT_R0, o); str(rd, SLJIT_R0); }
			else { ldr(SLJIT_R0, rd); st(o, SLJIT_R0); }
			tail(pc, pc + 4, true);
			return true;
		}
		case 23: case 55: ea_x(ra, rb, x == 55); load(pc, W, rd, x == 55 ? int(ra) : -1); return true;     // lwzx, lwzux
		case 87: case 119: ea_x(ra, rb, x == 119); load(pc, B, rd, x == 119 ? int(ra) : -1); return true;  // lbzx, lbzux
		case 279: case 311: ea_x(ra, rb, x == 311); load(pc, H, rd, x == 311 ? int(ra) : -1); return true; // lhzx, lhzux
		case 343: case 375: ea_x(ra, rb, x == 375); load(pc, HA, rd, x == 375 ? int(ra) : -1); return true; // lhax, lhaux
		case 151: case 183: ea_x(ra, rb, x == 183); ldr(SLJIT_R0, rd); store(pc, W, x == 183 ? int(ra) : -1); return true;  // stwx, stwux
		case 215: case 247: ea_x(ra, rb, x == 247); ldr(SLJIT_R0, rd); store(pc, B, x == 247 ? int(ra) : -1); return true;  // stbx, stbux
		case 407: case 439: ea_x(ra, rb, x == 439); ldr(SLJIT_R0, rd); store(pc, H, x == 439 ? int(ra) : -1); return true;  // sthx, sthux
		case 534: ea_x(ra, rb, false); load_with(pc, W, -1, [](Gen &g) { sljit_emit_op1(g.C, SLJIT_REV32, SLJIT_R0, 0, SLJIT_R0, 0); }, [&]() { str(rd, SLJIT_R0); }); return true;  // lwbrx
		case 790: ea_x(ra, rb, false); load_with(pc, H, -1, [](Gen &g) { sljit_emit_op1(g.C, SLJIT_REV32_U16, SLJIT_R0, 0, SLJIT_R0, 0); }, [&]() { str(rd, SLJIT_R0); }); return true;  // lhbrx
		case 662: ea_x(ra, rb, false); ldr(SLJIT_R0, rd); sljit_emit_op1(C, SLJIT_REV32, SLJIT_R0, 0, SLJIT_R0, 0); store(pc, W, -1); return true;  // stwbrx
		case 918: ea_x(ra, rb, false); ldr(SLJIT_R0, rd); sljit_emit_op1(C, SLJIT_REV32_U16, SLJIT_R0, 0, SLJIT_R0, 0); store(pc, H, -1); return true;  // sthbrx
		case 535: case 567: ea_x(ra, rb, x == 567); load_with(pc, W, x == 567 ? int(ra) : -1, nullptr, [&]() { put_single(int(rd)); }); return true;  // lfsx, lfsux
		case 663: case 695: ea_x(ra, rb, x == 695); get_single(rd); store(pc, W, x == 695 ? int(ra) : -1); return true;  // stfsx, stfsux
		case 599: case 631: ea_x(ra, rb, x == 631); load_double(pc, op, rd, x == 631 ? int(ra) : -1); return true;  // lfdx, lfdux
		case 727: case 759: ea_x(ra, rb, x == 759); store_double(pc, op, rd, x == 759 ? int(ra) : -1); return true;  // stfdx, stfdux
		case 983: ea_x(ra, rb, false); ld(SLJIT_R0, f_lo(rd)); store(pc, W, -1); return true;  // stfiwx
		case 54: case 86: case 246: case 278: case 470: case 1014: case 598: case 854: case 982: case 306: case 370: case 566: case 758:
			tail(pc, pc + 4, true);   // cache, sync and TLB instructions: nothing to do (as the interpreter)
			return true;
		default:
			return false;
		}
	}

	bool build(const std::vector<uint32_t> &addrs, Stats &stats)
	{
		C = sljit_create_compiler(nullptr);
		if (!C) return false;
		is_code.assign(hi / 4, 0);
		for (uint32_t a : addrs) is_code[a >> 2] = 1;
		lab.assign(hi / 4, nullptr);
		// entry(state, label): RAM's base in S1, then straight to the instruction's code
		for (uint32_t a : addrs) {
			const uint32_t op = image[a >> 2];
			if ((op >> 26) != 16) continue;
			const uint32_t t = uint32_t(int32_t(int16_t(op & 0xFFFC))) + (op & 2 ? 0 : a);
			if (compiled(t))
				if (Spin *sp = spin_at(a, t)) { spin_branch[a] = sp; spin_head[t] = sp; }
		}
		sljit_emit_enter(C, 0, SLJIT_ARGS2V(P, W), 5 | SLJIT_ENTER_FLOAT(2), 4, 16);
		clear_spins();
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0, SLJIT_IMM, sljit_sw(cpu.ram_()));
		sljit_emit_ijump(C, SLJIT_JUMP, SLJIT_R0, 0);
		for (uint32_t a : addrs) {
			lab[a >> 2] = sljit_emit_label(C);
			auto h = spin_head.find(a);
			if (h != spin_head.end())
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM0(), sljit_sw(&h->second->ran), SLJIT_IMM, 1);
			insn(a, image[a >> 2]);
		}
		// the out-of-line pieces
		sljit_label *exit = sljit_emit_label(C);
		sljit_emit_return_void(C);
		sljit_label *dispatch = sljit_emit_label(C);
		clear_spins();
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&rec));
		call(SLJIT_ARGS1(W, P), (void *)h_label);
		sljit_jump *none = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
		sljit_emit_ijump(C, SLJIT_JUMP, SLJIT_R0, 0);
		sljit_set_label(none, exit);
		for (const Stub &s : stubs) {
			sljit_set_label(s.from, sljit_emit_label(C));
			if (s.kind != STUB_DYN_TRIG) pcs(s.pc, s.next);
			if (s.kind == STUB_END) {
				to_exit.push_back(sljit_emit_jump(C, SLJIT_JUMP));
				continue;
			}
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, sljit_sw(&cpu));
			call(SLJIT_ARGS1V(P), (void *)h_dec);
			to_dispatch.push_back(sljit_emit_jump(C, SLJIT_JUMP));
		}
		for (sljit_jump *j : to_exit) sljit_set_label(j, exit);
		for (sljit_jump *j : to_dispatch) sljit_set_label(j, dispatch);
		for (auto &[j, t] : to_label) sljit_set_label(j, lab[t >> 2]);
		void *code = sljit_generate_code(C, 0, nullptr);
		if (!code) {
			sljit_free_compiler(C);
			return false;
		}
		stats.code_bytes = sljit_get_generated_code_size(C);
		rec.label.assign(hi / 4, 0);
		for (uint32_t a : addrs) rec.label[a >> 2] = sljit_get_label_addr(lab[a >> 2]);
		sljit_free_compiler(C);
		result = code;
		return true;
	}
	void *result = nullptr;
};

} // namespace

Rec::Rec() = default;

Rec::~Rec()
{
	if (code) sljit_free_code(code, nullptr);
}

bool Rec::build(Cpu &c, const std::vector<uint32_t> &image, const uint32_t *seed_map, size_t seed_count, Stats *stats)
{
	auto t0 = std::chrono::steady_clock::now();
	cpu = &c;
	const uint32_t hi = uint32_t(image.size() * 4);
	std::vector<uint32_t> seeds(seed_map, seed_map + seed_count);
	std::vector<uint32_t> addrs = analyse(image, 0, hi, seeds);
	map.assign(hi / 4, 0);
	bad.assign(hi / 4, 0);
	compiled.assign(hi / 4, 0);
	for (uint32_t a : addrs) {
		map[a >> 2] = 1;
		compiled[a >> 2] = image[a >> 2];
	}
	code_limit = hi;
	code_map = map.data();
	Stats st;
	spins.clear();   // (the last build's: its code is replaced, and each entry clears every spin's flag)
	Gen g(*this, c, image, hi, spins);
	if (!g.build(addrs, st)) {
		built = false;
		return false;
	}
	if (code) sljit_free_code(code, nullptr);
	code = g.result;
	entry = reinterpret_cast<void (*)(State *, uintptr_t)>(code);
	built = true;
	recount();
	st.instructions = addrs.size();
	st.native = g.natives;
	st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (stats) *stats = st;
	return true;
}

void Rec::recount()
{
	mismatched = 0;
	const uint8_t *ram = cpu->ram_();
	for (size_t w = 0; w < map.size(); w++) {
		bad[w] = 0;
		if (map[w] && *reinterpret_cast<const uint32_t *>(&ram[w * 4]) != compiled[w]) {
			bad[w] = 1;
			mismatched++;
		}
	}
}

void Rec::code_written(uint32_t addr)
{
	const uint32_t w = addr >> 2;
	if (w >= map.size() || !map[w]) return;
	const bool now = *reinterpret_cast<const uint32_t *>(&cpu->ram_()[w * 4]) != compiled[w];
	if (now != bool(bad[w])) {
		bad[w] = now;
		if (now) mismatched++;
		else mismatched--;
	}
}

bool Rec::run(Cpu &c)
{
	if (!enabled()) {
		interp_disabled++;
		return false;
	}
	const uint32_t npc = c.s.npc;
	if (npc >= code_limit) {
		interp_outside++;
		return false;
	}
	const uintptr_t l = label[npc >> 2];
	if (!l) {
		interp_unmapped++;
		if (unmapped.size() < 512 && std::find(unmapped.begin(), unmapped.end(), npc) == unmapped.end())
			unmapped.push_back(npc);
		return false;
	}
	entries++;
	entry(&c.s, l);
	return true;
}

} // namespace ppcrec
