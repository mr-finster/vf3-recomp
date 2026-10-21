// The PowerPC 603e, as an interpreter: the reference for the recompiler and the fallback for code it lacks.
//
// It follows the reference build (Supermodel's interpreter) instruction for instruction, including its timing
// model (one instruction a cycle, the time base and decrementer at a quarter of that) and the points where it
// checks for interrupts, so that a run on this core can be compared with the reference frame by frame.
// Where the reference differs from a real 603e, this core follows the reference for now; ppc.cpp lists each
// such place (search for "Reference:").
#pragma once
#include <cstdint>

namespace ppc {

class Cpu;

// The recompiled code (src/rec): run() goes on at NPC in native code while it can and says whether it ran
// anything; code_written() hears of every store to RAM below code_limit (to drop code the program changes)
struct Recompiler
{
	uint32_t code_limit = 0;
	const uint8_t *code_map = nullptr;   // per RAM word: compiled code there
	virtual bool run(Cpu &cpu) = 0;
	virtual void code_written(uint32_t addr) = 0;
	virtual void recount() = 0;          // RAM was changed behind the CPU's back (a reset)
	virtual ~Recompiler() = default;
};

// What the CPU sees outside RAM: the Model 3 board. RAM itself is read and written directly.
struct Bus
{
	virtual uint8_t read8(uint32_t addr) = 0;
	virtual uint16_t read16(uint32_t addr) = 0;
	virtual uint32_t read32(uint32_t addr) = 0;
	virtual uint64_t read64(uint32_t addr) = 0;
	virtual void write8(uint32_t addr, uint8_t data) = 0;
	virtual void write16(uint32_t addr, uint16_t data) = 0;
	virtual void write32(uint32_t addr, uint32_t data) = 0;
	virtual void write64(uint32_t addr, uint64_t data) = 0;
	virtual ~Bus() = default;
};

union Fpr
{
	uint64_t i;
	double d;
};

// MSR bits
enum : uint32_t
{
	MSR_POW = 0x00040000, MSR_ILE = 0x00010000, MSR_EE = 0x00008000, MSR_PR = 0x00004000, MSR_FP = 0x00002000,
	MSR_ME = 0x00001000, MSR_FE0 = 0x00000800, MSR_SE = 0x00000400, MSR_BE = 0x00000200, MSR_FE1 = 0x00000100,
	MSR_IP = 0x00000040, MSR_IR = 0x00000020, MSR_DR = 0x00000010, MSR_RI = 0x00000002, MSR_LE = 0x00000001,
};

// Everything that makes up the CPU's state. The timer fields are kept in the reference's form (and C types), so
// the time base and decrementer step at exactly the same instructions.
struct State
{
	uint32_t r[32];
	Fpr f[32];
	uint8_t cr[8];          // one 4-bit field each
	uint32_t xer, lr, ctr, msr, fpscr;
	uint32_t pc, npc;
	uint32_t srr0, srr1, sprg[4];
	uint32_t pvr, hid0, hid1, hid2, sdr1, dsisr, dar, ear, iabr;
	uint32_t dmiss, dcmp, hash1, hash2, imiss, icmp, rpa;
	uint32_t ibat[8], dbat[8];  // upper, lower for each of 4
	uint32_t sr[16];
	int reserved;
	uint32_t reserved_address;
	int interrupt_pending;      // 1 external, 2 decrementer, 4 SMI
	uint64_t tb;
	uint32_t dec;
	int timer_ratio;
	uint32_t timer_frac;
	int tb_base_icount, dec_base_icount, dec_trigger_cycle;
	uint64_t total_cycles;
	int icount, cur_cycles;
	bool fatal;                 // stopped on something it can't run; the message says what

	// Save states (src/machine/state_io.h): each field on its own, so the struct's padding isn't in the state
	template <class IO> void state(IO &io)
	{
		io.pod(r);
		for (Fpr &x : f) io.pod(x.i);
		io.pod(cr);
		io.pod(xer); io.pod(lr); io.pod(ctr); io.pod(msr); io.pod(fpscr);
		io.pod(pc); io.pod(npc);
		io.pod(srr0); io.pod(srr1); io.pod(sprg);
		io.pod(pvr); io.pod(hid0); io.pod(hid1); io.pod(hid2); io.pod(sdr1); io.pod(dsisr); io.pod(dar); io.pod(ear); io.pod(iabr);
		io.pod(dmiss); io.pod(dcmp); io.pod(hash1); io.pod(hash2); io.pod(imiss); io.pod(icmp); io.pod(rpa);
		io.pod(ibat); io.pod(dbat); io.pod(sr);
		io.pod(reserved); io.pod(reserved_address); io.pod(interrupt_pending);
		io.pod(tb); io.pod(dec); io.pod(timer_ratio); io.pod(timer_frac);
		io.pod(tb_base_icount); io.pod(dec_base_icount); io.pod(dec_trigger_cycle);
		io.pod(total_cycles); io.pod(icount); io.pod(cur_cycles);
		io.pod(fatal);
	}
};

class Cpu
{
public:
	State s{};
	char fatal_message[160] = {};
	uint32_t watch = 0xFFFFFFFF;  // debugging: report stores to this word (vf3run --watch)
	uint32_t trace_pc = 0xFFFFFFFF;   // debugging: report r3-r8 and LR each time this instruction runs (vf3run --trace-pc)

	// ram: 8 MB at 0, stored as host-order 32-bit words (byte address a is ram[a ^ 3] on a little-endian
	// host). crom: the fixed 8 MB at 0xFF800000, in the same form. Instructions are fetched from these two.
	void attach(Bus *bus, uint8_t *ram, const uint8_t *crom);

	// The Model 3's configurations: pvr, the bus multiplier (x2, as the reference keeps it), cycles a second
	void configure(uint32_t pvr, int bus_freq_multiplier_x2, int cycles_per_second, uint32_t hid1);
	void reset();
	int execute(int cycles);
	Recompiler *rec = nullptr;      // when set, execute() runs its native code where it has some

	// For the recompiled code: one instruction's work as the interpreter does it (PC and NPC set first), the
	// decrementer reaching its trigger cycle, and a RAM store the CPU didn't make (the 53C810's, DMA)
	void exec_op(uint32_t op) { step(op); }
	void dec_trigger() { s.interrupt_pending |= 2; check_interrupts(); }
	void branch_check(uint32_t t) { check_pc(t); }
	void ram_written(uint32_t a) { if (rec && a < rec->code_limit && rec->code_map[a >> 2]) rec->code_written(a); }
	Bus *bus_() const { return bus; }
	uint8_t *ram_() const { return ram; }
	void set_irq_line(bool on);
	void set_timer_ratio(int r) { s.timer_ratio = r; }
	int bus_freq_multiplier() const { return bus_mult_x2; }
	int cycles_per_second() const { return cps; }
	uint32_t pc() const { return s.pc; }

	// The state in the form the reference build dumps each frame (128 words; see tools/ref)
	void dump_state(uint32_t *out) const;

private:
	Bus *bus = nullptr;
	uint8_t *ram = nullptr;
	const uint8_t *crom = nullptr;
	int bus_mult_x2 = 2, cps = 66000000;

	void step(uint32_t op);
	void op19(uint32_t op);
	void op31(uint32_t op);
	void op59(uint32_t op);
	void op63(uint32_t op);
	void invalid(uint32_t op);
	void fail(const char *what, uint32_t value);

	bool check_pc(uint32_t pc);
	uint32_t fetch(uint32_t pc) const;
	void exception(int kind);
	void check_interrupts();
	void set_msr(uint32_t v);
	uint32_t get_spr(int spr);
	void set_spr(int spr, uint32_t v);
	uint64_t read_tb() const;
	void write_tbl(uint32_t v);
	void write_tbu(uint32_t v);
	uint32_t read_dec() const;
	void write_dec(uint32_t v);
	bool cond(uint32_t bo, uint32_t bi);

	uint8_t r8(uint32_t a);
	uint16_t r16(uint32_t a);
	uint32_t r32(uint32_t a);
	uint64_t r64(uint32_t a);
	void w8(uint32_t a, uint8_t v);
	void w16(uint32_t a, uint16_t v);
	void w32(uint32_t a, uint32_t v);
	void w64(uint32_t a, uint64_t v);
};

} // namespace ppc
