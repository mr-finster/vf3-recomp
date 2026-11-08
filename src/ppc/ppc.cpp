// The PowerPC 603e interpreter. See ppc.h.
//
// Reference: marks the places where this core does what the reference build does rather than what a real 603e
// does. They matter only for checking against the reference; each is a candidate for a deliberate departure
// later, with evidence (as VF2 did where MAME wasn't the arcade).
#include "ppc.h"
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("trapping-math,rounding-math,fp-contract=off")
#endif

namespace ppc {

namespace {

uint32_t rotate_mask[32][32];
uint32_t field_mask[256];   // mtfsf's FM field as a mask of 4-bit fields

struct Tables
{
	Tables()
	{
		for (int mb = 0; mb < 32; mb++)
			for (int me = 0; me < 32; me++) {
				uint32_t m = (0xFFFFFFFFu >> mb) ^ (me >= 31 ? 0 : 0xFFFFFFFFu >> (me + 1));
				rotate_mask[mb][me] = mb > me ? ~m : m;
			}
		for (int i = 0; i < 256; i++) {
			uint32_t m = 0;
			for (int k = 0; k < 8; k++)
				if (i & (0x80 >> k))
					m |= 0xF0000000u >> (4 * k);
			field_mask[i] = m;
		}
	}
} tables;

inline uint32_t rd(uint32_t op) { return (op >> 21) & 31; }
inline uint32_t ra(uint32_t op) { return (op >> 16) & 31; }
inline uint32_t rb(uint32_t op) { return (op >> 11) & 31; }
inline uint32_t rc(uint32_t op) { return (op >> 6) & 31; }
inline uint32_t simm(uint32_t op) { return uint32_t(int32_t(int16_t(op & 0xFFFF))); }
inline uint32_t uimm(uint32_t op) { return op & 0xFFFF; }
inline bool rcbit(uint32_t op) { return op & 1; }
inline bool oebit(uint32_t op) { return op & 0x400; }
inline uint32_t spr_field(uint32_t op) { return ((op >> 16) & 0x1F) | ((op >> 6) & 0x3E0); }

const uint32_t XER_SO = 0x80000000, XER_OV = 0x40000000, XER_CA = 0x20000000;
inline bool add_ca(uint32_t r, uint32_t a, uint32_t) { return r < a; }
inline bool sub_ca(uint32_t, uint32_t a, uint32_t b) { return !(a < b); }
inline uint32_t add_ov(uint32_t r, uint32_t a, uint32_t b) { return (~(a ^ b) & (a ^ r)) & 0x80000000; }
inline uint32_t sub_ov(uint32_t r, uint32_t a, uint32_t b) { return ((a ^ b) & (a ^ r)) & 0x80000000; }

// SPR numbers
enum
{
	SPR_XER = 1, SPR_LR = 8, SPR_CTR = 9, SPR_DSISR = 18, SPR_DAR = 19, SPR_DEC = 22, SPR_SDR1 = 25,
	SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_TBL_R = 268, SPR_TBU_R = 269, SPR_SPRG0 = 272, SPR_EAR = 282,
	SPR_TBL_W = 284, SPR_TBU_W = 285, SPR_PVR = 287, SPR_IBAT0U = 528, SPR_DBAT3L = 543,
	SPR_DMISS = 0x3D0, SPR_DCMP, SPR_HASH1, SPR_HASH2, SPR_IMISS, SPR_ICMP, SPR_RPA,
	SPR_HID0 = 1008, SPR_HID1 = 1009, SPR_IABR = 1010, SPR_HID2 = 1011,
};

enum { EXC_IRQ = 1, EXC_DEC, EXC_TRAP, EXC_SC, EXC_SMI, EXC_DSI, EXC_ISI };

// ---- floating point helpers (the reference's classification and FPSCR updates) ----

const uint64_t D_SIGN = 0x8000000000000000ull, D_EXP = 0x7FF0000000000000ull, D_FRAC = 0x000FFFFFFFFFFFFFull;
inline bool is_nan(Fpr x) { return std::isnan(x.d); }
inline bool is_qnan(Fpr x) { return (x.i & (D_EXP | D_FRAC)) >= (D_EXP | 0x0008000000000000ull); }
inline bool is_snan(Fpr x)
{
	uint64_t ef = x.i & (D_EXP | D_FRAC);
	return ef > D_EXP && ef < (D_EXP | 0x0008000000000000ull);
}
inline bool is_inf(Fpr x) { return (x.i & (D_EXP | D_FRAC)) == D_EXP; }
inline bool is_normal(Fpr x)
{
	uint64_t e = x.i & D_EXP;
	return e > 0 && e <= (2046ull << 52);
}
inline bool is_denormal(Fpr x)
{
	uint64_t ef = x.i & (D_EXP | D_FRAC);
	return ef > 0 && ef <= D_FRAC;
}

union F32
{
	uint32_t i;
	float f;
};

const int ROUNDING[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };

} // namespace

// ---- setup ----

void Cpu::attach(Bus *b, uint8_t *r, const uint8_t *c)
{
	bus = b;
	ram = r;
	crom = c;
}

void Cpu::configure(uint32_t pvr, int mult_x2, int cycles_per_second, uint32_t hid1)
{
	s.pvr = pvr;
	bus_mult_x2 = mult_x2;
	cps = cycles_per_second;
	s.timer_ratio = 2 * mult_x2;  // the time base and decrementer step every four bus cycles
	s.hid1 = hid1;
}

void Cpu::reset()
{
	s.fatal = false;
	fatal_message[0] = 0;
	s.pc = s.npc = 0xFFF00100;
	set_msr(0x40);
	check_pc(s.pc);
	s.hid0 = 1;
	s.interrupt_pending = 0;
	s.tb = 0;
	s.timer_frac = 0;
	s.dec = 0xFFFFFFFF;
	s.total_cycles = 0;
	s.cur_cycles = 0;
	s.icount = 0;
}

void Cpu::fail(const char *what, uint32_t value)
{
	if (!s.fatal)
		snprintf(fatal_message, sizeof(fatal_message), "%s %08X at PC %08X", what, value, s.pc);
	s.fatal = true;
}

void Cpu::invalid(uint32_t op)
{
	fail("invalid instruction", op);
}

// ---- memory ----

inline uint8_t Cpu::r8(uint32_t a)
{
	if (a < 0x00800000)
		return ram[a ^ 3];
	return bus->read8(a);
}

inline uint16_t Cpu::r16(uint32_t a)
{
	if (a < 0x00800000 && !(a & 1))
		return *(const uint16_t *)&ram[a ^ 2];
	return bus->read16(a);
}

inline uint32_t Cpu::r32(uint32_t a)
{
	if (a < 0x00800000 && !(a & 3))
		return *(const uint32_t *)&ram[a];
	return bus->read32(a);
}

inline uint64_t Cpu::r64(uint32_t a)
{
	if (a < 0x007FFFF8 && !(a & 3))
		return (uint64_t(*(const uint32_t *)&ram[a]) << 32) | *(const uint32_t *)&ram[a + 4];
	return bus->read64(a);
}

static void report_store(const State &s, uint32_t a, uint32_t v, int bits)
{
	fprintf(stderr, "store%d %08X = %X at PC %08X (LR %08X)\n", bits, a, v, s.pc, s.lr);
}

inline void Cpu::w8(uint32_t a, uint8_t v)
{
	if ((a & ~3u) == watch) report_store(s, a, v, 8);
	if (a < 0x00800000) {
		ram[a ^ 3] = v;
		ram_written(a);
	} else
		bus->write8(a, v);
}

inline void Cpu::w16(uint32_t a, uint16_t v)
{
	if ((a & ~3u) == watch) report_store(s, a, v, 16);
	if (a < 0x00800000 && !(a & 1)) {
		*(uint16_t *)&ram[a ^ 2] = v;
		ram_written(a);
	} else
		bus->write16(a, v);
}

inline void Cpu::w32(uint32_t a, uint32_t v)
{
	if ((a & ~3u) == watch) report_store(s, a, v, 32);
	if (a < 0x00800000 && !(a & 3)) {
		*(uint32_t *)&ram[a] = v;
		ram_written(a);
	} else
		bus->write32(a, v);
}

inline void Cpu::w64(uint32_t a, uint64_t v)
{
	if (a < 0x007FFFF8 && !(a & 3)) {
		*(uint32_t *)&ram[a] = uint32_t(v >> 32);
		*(uint32_t *)&ram[a + 4] = uint32_t(v);
		ram_written(a);
		ram_written(a + 4);
	} else
		bus->write64(a, v);
}

// Instructions come from RAM or the fixed CROM; anywhere else stops the CPU (as the reference does)
bool Cpu::check_pc(uint32_t pc)
{
	if (pc <= 0x007FFFFF || pc >= 0xFF800000)
		return true;
	fail("PC out of bounds:", pc);
	return false;
}

inline uint32_t Cpu::fetch(uint32_t pc) const
{
	if (pc < 0x00800000)
		return *(const uint32_t *)&ram[pc];
	return *(const uint32_t *)&crom[pc - 0xFF800000];
}

// ---- exceptions, interrupts, timers ----

void Cpu::set_msr(uint32_t v)
{
	if (v & (MSR_ILE | MSR_LE))
		fail("little-endian mode, MSR", v);
	s.msr = v;
	check_interrupts();
}

void Cpu::exception(int kind)
{
	uint32_t vector = 0;
	uint32_t srr0 = s.npc, extra = 0;
	switch (kind) {
	case EXC_IRQ: vector = 0x500; break;
	case EXC_DEC: vector = 0x900; break;
	case EXC_TRAP: vector = 0x700; srr0 = s.pc; extra = 0x20000; break;
	case EXC_SC: vector = 0xC00; break;
	case EXC_SMI: vector = 0x1400; break;
	case EXC_DSI: vector = 0x300; break;
	case EXC_ISI: vector = 0x400; break;
	}
	// External, decrementer and SMI only while enabled; the others always
	if ((kind == EXC_IRQ || kind == EXC_DEC || kind == EXC_SMI) && !(s.msr & MSR_EE))
		return;
	uint32_t msr = s.msr;
	s.srr0 = srr0;
	s.srr1 = (msr & 0xFF73) | extra;
	msr &= ~(MSR_POW | MSR_EE | MSR_PR | MSR_FP | MSR_FE0 | MSR_SE | MSR_BE | MSR_FE1 | MSR_IR | MSR_DR | MSR_RI);
	if (msr & MSR_ILE)
		msr |= MSR_LE;
	else
		msr &= ~MSR_LE;
	set_msr(msr);
	s.npc = (msr & MSR_IP ? 0xFFF00000 : 0) | vector;
	// Reference: the external interrupt stays pending (the board clears it); SMI, DSI and ISI clear bit 4
	if (kind == EXC_DEC)
		s.interrupt_pending &= ~2;
	else if (kind == EXC_SMI || kind == EXC_DSI || kind == EXC_ISI)
		s.interrupt_pending &= ~4;
	check_pc(s.npc);
}

void Cpu::check_interrupts()
{
	if (!(s.msr & MSR_EE) || !s.interrupt_pending)
		return;
	if (s.interrupt_pending & 1)
		exception(EXC_IRQ);
	else if (s.interrupt_pending & 2)
		exception(EXC_DEC);
	else if (s.interrupt_pending & 4)
		exception(EXC_SMI);
}

void Cpu::set_irq_line(bool on)
{
	if (on) {
		s.interrupt_pending |= 1;
		check_interrupts();
	} else
		s.interrupt_pending &= ~1;
}

uint64_t Cpu::read_tb() const
{
	int cycles = s.tb_base_icount - s.icount;
	return s.tb + (cycles / s.timer_ratio);
}

void Cpu::write_tbl(uint32_t v)
{
	uint64_t tb = read_tb();
	s.tb_base_icount = s.icount + ((s.tb_base_icount - s.icount) % s.timer_ratio);
	s.tb = (tb & ~0xFFFFFFFFull) | v;
}

void Cpu::write_tbu(uint32_t v)
{
	uint64_t tb = read_tb();
	s.tb_base_icount = s.icount + ((s.tb_base_icount - s.icount) % s.timer_ratio);
	s.tb = (tb & 0xFFFFFFFFull) | (uint64_t(v) << 32);
}

uint32_t Cpu::read_dec() const
{
	int cycles = s.dec_base_icount - s.icount;
	return s.dec - (cycles / s.timer_ratio);
}

void Cpu::write_dec(uint32_t v)
{
	if ((v & 0x80000000) && !(read_dec() & 0x80000000)) {
		s.interrupt_pending |= 2;
		check_interrupts();
	}
	s.dec_base_icount = s.icount + ((s.dec_base_icount - s.icount) % s.timer_ratio);
	s.dec = v;
	// The exception comes when the decrementer passes through zero within this slice
	if (uint32_t(s.dec_base_icount / s.timer_ratio) > s.dec)
		s.dec_trigger_cycle = int(s.dec_base_icount - ((1 + s.dec) * uint32_t(s.timer_ratio)));
	else
		s.dec_trigger_cycle = 0x7FFFFFFF;
}

uint32_t Cpu::get_spr(int spr)
{
	switch (spr) {
	case SPR_LR: return s.lr;
	case SPR_CTR: return s.ctr;
	case SPR_XER: return s.xer;
	case SPR_SRR0: return s.srr0;
	case SPR_SRR1: return s.srr1;
	case SPR_SPRG0: case SPR_SPRG0 + 1: case SPR_SPRG0 + 2: case SPR_SPRG0 + 3: return s.sprg[spr - SPR_SPRG0];
	case SPR_PVR: return s.pvr;
	case SPR_TBL_W: return uint32_t(read_tb());
	case SPR_TBU_W: return uint32_t(read_tb() >> 32);
	case SPR_HID0: return s.hid0;
	case SPR_HID1: return s.hid1;
	case SPR_HID2: return s.hid2;
	case SPR_DEC: return read_dec();
	case SPR_SDR1: return s.sdr1;
	case SPR_DSISR: return s.dsisr;
	case SPR_DAR: return s.dar;
	case SPR_EAR: return s.ear;
	case SPR_DMISS: return s.dmiss;
	case SPR_DCMP: return s.dcmp;
	case SPR_HASH1: return s.hash1;
	case SPR_HASH2: return s.hash2;
	case SPR_IMISS: return s.imiss;
	case SPR_ICMP: return s.icmp;
	case SPR_RPA: return s.rpa;
	default:
		if (spr >= SPR_IBAT0U && spr <= SPR_IBAT0U + 7)
			return s.ibat[spr - SPR_IBAT0U];
		if (spr >= SPR_IBAT0U + 8 && spr <= SPR_DBAT3L)
			return s.dbat[spr - SPR_IBAT0U - 8];
		break;
	}
	// Reference: mfspr of TBL/TBU (268, 269) is refused like any unknown register; mftb reads them
	fail("mfspr from unknown SPR", uint32_t(spr));
	return 0;
}

void Cpu::set_spr(int spr, uint32_t v)
{
	switch (spr) {
	case SPR_LR: s.lr = v; return;
	case SPR_CTR: s.ctr = v; return;
	case SPR_XER: s.xer = v; return;
	case SPR_SRR0: s.srr0 = v; return;
	case SPR_SRR1: s.srr1 = v; return;
	case SPR_SPRG0: case SPR_SPRG0 + 1: case SPR_SPRG0 + 2: case SPR_SPRG0 + 3: s.sprg[spr - SPR_SPRG0] = v; return;
	case SPR_PVR: return;
	case SPR_DEC: write_dec(v); return;
	case SPR_TBL_W: case SPR_TBL_R: write_tbl(v); return;
	case SPR_TBU_W: case SPR_TBU_R: write_tbu(v); return;
	case SPR_HID0: s.hid0 = v; return;
	case SPR_HID1: s.hid1 = v; return;
	case SPR_HID2: s.hid2 = v; return;
	case SPR_DSISR: s.dsisr = v; return;
	case SPR_DAR: s.dar = v; return;
	case SPR_EAR: s.ear = v; return;
	case SPR_DMISS: s.dmiss = v; return;
	case SPR_DCMP: s.dcmp = v; return;
	case SPR_HASH1: s.hash1 = v; return;
	case SPR_HASH2: s.hash2 = v; return;
	case SPR_IMISS: s.imiss = v; return;
	case SPR_ICMP: s.icmp = v; return;
	case SPR_RPA: s.rpa = v; return;
	case SPR_SDR1: s.sdr1 = v; return;
	case SPR_IABR: s.iabr = v; return;
	default:
		if (spr >= SPR_IBAT0U && spr <= SPR_IBAT0U + 7) {
			s.ibat[spr - SPR_IBAT0U] = v;
			return;
		}
		if (spr >= SPR_IBAT0U + 8 && spr <= SPR_DBAT3L) {
			s.dbat[spr - SPR_IBAT0U - 8] = v;
			return;
		}
		break;
	}
	fail("mtspr to unknown SPR", uint32_t(spr));
}

inline bool Cpu::cond(uint32_t bo, uint32_t bi)
{
	// Reference: the CTR is decremented whenever BO says so, bcctr included (an invalid form)
	if (!(bo & 4))
		--s.ctr;
	uint32_t ctr_ok = ((bo >> 2) & 1) | ((s.ctr != 0) ^ ((bo >> 1) & 1));
	uint32_t crbit = (s.cr[bi / 4] >> (3 - bi % 4)) & 1;
	uint32_t cond_ok = ((bo >> 4) & 1) | (crbit ^ (((bo >> 3) & 1) ^ 1));
	return ctr_ok & cond_ok;
}

// ---- the run loop ----

int Cpu::execute(int cycles)
{
	s.cur_cycles = cycles;
	s.icount = cycles;
	s.tb_base_icount = int(cycles + s.timer_frac);
	s.dec_base_icount = int(cycles + s.timer_frac);
	if (uint32_t(s.dec_base_icount / s.timer_ratio) > s.dec)
		s.dec_trigger_cycle = int(s.dec_base_icount - ((1 + s.dec) * uint32_t(s.timer_ratio)));
	else
		s.dec_trigger_cycle = 0x7FFFFFFF;
	check_pc(s.npc);
	check_interrupts();
	while (s.icount > 0 && !s.fatal) {
		if (rec && rec->run(*this))
			continue;
		s.pc = s.npc;
		uint32_t op = fetch(s.pc);
		s.npc = s.pc + 4;
		step(op);
		s.icount--;
		if (s.icount == s.dec_trigger_cycle) {
			s.interrupt_pending |= 2;
			check_interrupts();
		}
	}
	s.timer_frac = uint32_t((s.tb_base_icount - s.icount) % s.timer_ratio);
	s.tb += uint64_t(int64_t((s.tb_base_icount - s.icount) / s.timer_ratio));
	s.dec -= uint32_t((s.dec_base_icount - s.icount) / s.timer_ratio);
	int executed = cycles - s.icount;
	s.total_cycles += uint64_t(int64_t(executed));
	s.cur_cycles = 0;
	s.icount = 0;
	s.tb_base_icount = 0;
	s.dec_base_icount = 0;
	return executed;
}

#define R(n) s.r[n]
#define F(n) s.f[n]

static inline void set_cr0(State &s, uint32_t v)
{
	int32_t x = int32_t(v);
	s.cr[0] = x < 0 ? 8 : x > 0 ? 4 : 2;
	if (s.xer & XER_SO)
		s.cr[0] |= 1;
}

static inline void set_cr1(State &s) { s.cr[1] = (s.fpscr >> 28) & 0xF; }

static inline void set_add_ov(State &s, uint32_t r, uint32_t a, uint32_t b)
{
	if (add_ov(r, a, b))
		s.xer |= XER_SO | XER_OV;
	else
		s.xer &= ~XER_OV;
}

static inline void set_sub_ov(State &s, uint32_t r, uint32_t a, uint32_t b)
{
	if (sub_ov(r, a, b))
		s.xer |= XER_SO | XER_OV;
	else
		s.xer &= ~XER_OV;
}

static inline void set_ca(State &s, bool c)
{
	if (c)
		s.xer |= XER_CA;
	else
		s.xer &= ~XER_CA;
}

static inline void cmp_signed(State &s, int d, int32_t a, int32_t b)
{
	s.cr[d] = a < b ? 8 : a > b ? 4 : 2;
	if (s.xer & XER_SO)
		s.cr[d] |= 1;
}

static inline void cmp_unsigned(State &s, int d, uint32_t a, uint32_t b)
{
	s.cr[d] = a < b ? 8 : a > b ? 4 : 2;
	if (s.xer & XER_SO)
		s.cr[d] |= 1;
}

static inline uint32_t rotl(uint32_t v, int sh) { return (v << sh) | (v >> ((32 - sh) & 31)); }

static inline bool trap(int32_t a, int32_t b, uint32_t to)
{
	return ((a < b) && (to & 0x10)) || ((a > b) && (to & 0x08)) || ((a == b) && (to & 0x04)) ||
	       ((uint32_t(a) < uint32_t(b)) && (to & 0x02)) || ((uint32_t(a) > uint32_t(b)) && (to & 0x01));
}

// FPSCR helpers (the reference's: an SNaN operand sets FX only; FPRF from the result)
static inline void vxsnan(State &s, Fpr a) { if (is_snan(a)) s.fpscr |= 0x80000000; }
static inline void vxsnan2(State &s, Fpr a, Fpr b) { if (is_snan(a) || is_snan(b)) s.fpscr |= 0x80000000; }

static inline void set_fprf(State &s, Fpr f)
{
	bool neg = f.i >> 63;
	uint32_t c;
	if (is_qnan(f)) c = 0x11;
	else if (is_inf(f)) c = neg ? 0x09 : 0x05;
	else if (is_normal(f)) c = neg ? 0x08 : 0x04;
	else if (is_denormal(f)) c = neg ? 0x18 : 0x14;
	else c = neg ? 0x12 : 0x02;
	s.fpscr = (s.fpscr & ~0x0001F000u) | (c << 12);
}

// The arithmetic in the FPSCR's rounding mode (only switched when it isn't round-to-nearest)
struct Rounding
{
	uint32_t mode;
	explicit Rounding(uint32_t fpscr) : mode(fpscr & 3) { if (mode) std::fesetround(ROUNDING[mode]); }
	~Rounding() { if (mode) std::fesetround(FE_TONEAREST); }
};

// NaN results, made the same on every host. The reference does its arithmetic on x86-64's SSE: an invalid
// operation (0 * inf, inf - inf, 0 / 0, the square root of a negative) gives the "default NaN" 0xFFF8000000000000,
// and with NaN operands the result is the first operand's NaN if it is one, else the second's, made quiet. Which
// operand is first is how GCC compiled each instruction in the reference (see the instructions below). ARM64 has
// other rules, so every NaN result is made here rather than taken from the host.
//
// Reference: a 603e gives 0x7FF8000000000000 for an invalid operation and picks frA, then frB, then frC.
const uint64_t SSE_DEFAULT_NAN = 0xFFF8000000000000ull;

static inline double bits(uint64_t i) { Fpr f; f.i = i; return f.d; }
static inline uint64_t ubits(double d) { Fpr f; f.d = d; return f.i; }

// The result r of "x op y", with x the operand SSE takes first
static inline double sse(double r, double x, double y)
{
	if (!std::isnan(r))
		return r;
	if (std::isnan(x))
		return bits(ubits(x) | 0x0008000000000000ull);
	if (std::isnan(y))
		return bits(ubits(y) | 0x0008000000000000ull);
	return bits(SSE_DEFAULT_NAN);
}

static inline double sse_sqrt(double x)
{
	double r = std::sqrt(x);
	if (!std::isnan(r))
		return r;
	return std::isnan(x) ? bits(ubits(x) | 0x0008000000000000ull) : bits(SSE_DEFAULT_NAN);
}

static inline double neg(double d) { return bits(ubits(d) ^ 0x8000000000000000ull); }

// Rounding to single precision and back (cvtsd2ss, cvtss2sd): a NaN keeps its sign and top 23 fraction bits
static inline double to_single(double d)
{
	if (std::isnan(d))
		return bits((ubits(d) & 0xFFFFFFFFE0000000ull) | 0x0008000000000000ull);
	return double(float(d));
}

// double to int64 as x86-64 converts (the reference's platform): NaN and out of range give INT64_MIN, where
// ARM64 would saturate
static inline int64_t to_i64(double d)
{
	if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
		return INT64_MIN;
	return int64_t(d);
}

static inline void cr_op(State &s, uint32_t op, int (*f)(int, int))
{
	int bt = rd(op), ba = ra(op), bb = rb(op);
	int a = (s.cr[ba / 4] >> (3 - ba % 4)) & 1, b = (s.cr[bb / 4] >> (3 - bb % 4)) & 1;
	if (f(a, b))
		s.cr[bt / 4] |= 1 << (3 - bt % 4);
	else
		s.cr[bt / 4] &= ~(1 << (3 - bt % 4));
}

void Cpu::step(uint32_t op)
{
	if (s.pc == trace_pc)
		fprintf(stderr, "pc %08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X LR %08X\n", s.pc, s.r[3], s.r[4], s.r[5], s.r[6], s.r[7], s.r[8], s.lr);
	switch (op >> 26) {
	case 3:  // twi
		if (trap(int32_t(R(ra(op))), int32_t(simm(op)), rd(op)))
			exception(EXC_TRAP);
		return;
	case 7:  // mulli
		R(rd(op)) = R(ra(op)) * simm(op);
		return;
	case 8: {  // subfic
		uint32_t i = simm(op), a = R(ra(op));
		R(rd(op)) = i - a;
		set_ca(s, sub_ca(R(rd(op)), i, a));
		return;
	}
	case 10: cmp_unsigned(s, (op >> 23) & 7, R(ra(op)), uimm(op)); return;   // cmpli
	case 11: cmp_signed(s, (op >> 23) & 7, int32_t(R(ra(op))), int32_t(simm(op))); return;  // cmpi
	case 12: case 13: {  // addic, addic.
		uint32_t i = simm(op), a = R(ra(op));
		R(rd(op)) = a + i;
		set_ca(s, add_ca(R(rd(op)), a, i));
		if (op >> 26 == 13)
			set_cr0(s, R(rd(op)));
		return;
	}
	case 14: R(rd(op)) = simm(op) + (ra(op) ? R(ra(op)) : 0); return;  // addi
	case 15: R(rd(op)) = (uimm(op) << 16) + (ra(op) ? R(ra(op)) : 0); return;  // addis
	case 16: {  // bc
		if (cond(rd(op), ra(op))) {
			s.npc = (simm(op) & ~3u) + (op & 2 ? 0 : s.pc);
			check_pc(s.npc);
		}
		if (op & 1)
			s.lr = s.pc + 4;
		return;
	}
	case 17: exception(EXC_SC); return;  // sc
	case 18: {  // b
		uint32_t li = op & 0x3FFFFFC;
		if (li & 0x2000000)
			li |= 0xFC000000;
		s.npc = li + (op & 2 ? 0 : s.pc);
		if (op & 1)
			s.lr = s.pc + 4;
		check_pc(s.npc);
		return;
	}
	case 19: op19(op); return;
	case 20: {  // rlwimi
		uint32_t m = rotate_mask[(op >> 6) & 31][(op >> 1) & 31];
		uint32_t r = rotl(R(rd(op)), rb(op));
		R(ra(op)) = (R(ra(op)) & ~m) | (r & m);
		if (rcbit(op)) set_cr0(s, R(ra(op)));
		return;
	}
	case 21: {  // rlwinm
		uint32_t m = rotate_mask[(op >> 6) & 31][(op >> 1) & 31];
		R(ra(op)) = rotl(R(rd(op)), rb(op)) & m;
		if (rcbit(op)) set_cr0(s, R(ra(op)));
		return;
	}
	case 23: {  // rlwnm
		uint32_t m = rotate_mask[(op >> 6) & 31][(op >> 1) & 31];
		R(ra(op)) = rotl(R(rd(op)), R(rb(op)) & 31) & m;
		if (rcbit(op)) set_cr0(s, R(ra(op)));
		return;
	}
	case 24: R(ra(op)) = R(rd(op)) | uimm(op); return;           // ori
	case 25: R(ra(op)) = R(rd(op)) | (uimm(op) << 16); return;   // oris
	case 26: R(ra(op)) = R(rd(op)) ^ uimm(op); return;           // xori
	case 27: R(ra(op)) = R(rd(op)) ^ (uimm(op) << 16); return;   // xoris
	case 28: R(ra(op)) = R(rd(op)) & uimm(op); set_cr0(s, R(ra(op))); return;          // andi.
	case 29: R(ra(op)) = R(rd(op)) & (uimm(op) << 16); set_cr0(s, R(ra(op))); return;  // andis.
	case 31: op31(op); return;
	case 32: { uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0); R(rd(op)) = r32(ea); return; }  // lwz
	case 33: { uint32_t ea = R(ra(op)) + simm(op); R(rd(op)) = r32(ea); R(ra(op)) = ea; return; }  // lwzu
	case 34: { uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0); R(rd(op)) = r8(ea); return; }   // lbz
	case 35: { uint32_t ea = R(ra(op)) + simm(op); R(rd(op)) = r8(ea); R(ra(op)) = ea; return; }   // lbzu
	case 36: { uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0); w32(ea, R(rd(op))); return; }    // stw
	case 37: { uint32_t ea = R(ra(op)) + simm(op); w32(ea, R(rd(op))); R(ra(op)) = ea; return; }    // stwu
	case 38: { uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0); w8(ea, uint8_t(R(rd(op)))); return; }  // stb
	case 39: { uint32_t ea = R(ra(op)) + simm(op); w8(ea, uint8_t(R(rd(op)))); R(ra(op)) = ea; return; }  // stbu
	case 40: { uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0); R(rd(op)) = r16(ea); return; }   // lhz
	case 41: { uint32_t ea = R(ra(op)) + simm(op); R(rd(op)) = r16(ea); R(ra(op)) = ea; return; }   // lhzu
	case 42: { uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0); R(rd(op)) = uint32_t(int32_t(int16_t(r16(ea)))); return; }  // lha
	case 43: { uint32_t ea = R(ra(op)) + simm(op); R(rd(op)) = uint32_t(int32_t(int16_t(r16(ea)))); R(ra(op)) = ea; return; }  // lhau
	case 44: { uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0); w16(ea, uint16_t(R(rd(op)))); return; }  // sth
	case 45: { uint32_t ea = R(ra(op)) + simm(op); w16(ea, uint16_t(R(rd(op)))); R(ra(op)) = ea; return; }  // sthu
	case 46: {  // lmw
		uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0);
		for (uint32_t r = rd(op); r <= 31; r++, ea += 4)
			R(r) = r32(ea);
		return;
	}
	case 47: {  // stmw
		uint32_t ea = simm(op) + (ra(op) ? R(ra(op)) : 0);
		for (uint32_t r = rd(op); r <= 31; r++, ea += 4)
			w32(ea, R(r));
		return;
	}
	case 48: case 49: {  // lfs, lfsu
		uint32_t ea = simm(op) + ((op >> 26) == 49 || ra(op) ? R(ra(op)) : 0);
		F32 f;
		f.i = r32(ea);
		F(rd(op)).d = double(f.f);
		if ((op >> 26) == 49) R(ra(op)) = ea;
		return;
	}
	case 50: case 51: {  // lfd, lfdu
		uint32_t ea = simm(op) + ((op >> 26) == 51 || ra(op) ? R(ra(op)) : 0);
		F(rd(op)).i = r64(ea);
		if ((op >> 26) == 51) R(ra(op)) = ea;
		return;
	}
	case 52: case 53: {  // stfs, stfsu
		uint32_t ea = simm(op) + ((op >> 26) == 53 || ra(op) ? R(ra(op)) : 0);
		F32 f;
		f.f = float(F(rd(op)).d);
		w32(ea, f.i);
		if ((op >> 26) == 53) R(ra(op)) = ea;
		return;
	}
	case 54: case 55: {  // stfd, stfdu
		uint32_t ea = simm(op) + ((op >> 26) == 55 || ra(op) ? R(ra(op)) : 0);
		w64(ea, F(rd(op)).i);
		if ((op >> 26) == 55) R(ra(op)) = ea;
		return;
	}
	case 59: op59(op); return;
	case 63: op63(op); return;
	default: invalid(op); return;
	}
}

void Cpu::op19(uint32_t op)
{
	switch ((op >> 1) & 0x3FF) {
	case 0: s.cr[rd(op) >> 2] = s.cr[ra(op) >> 2]; return;  // mcrf
	case 16:  // bclr
		if (cond(rd(op), ra(op))) {
			s.npc = s.lr & ~3u;
			check_pc(s.npc);
		}
		if (op & 1)
			s.lr = s.pc + 4;
		return;
	case 33: cr_op(s, op, [](int a, int b) { return (a | b) ^ 1; }); return;   // crnor
	case 50:  // rfi (SRR0's low two bits not taken, as the 603e: a word-aligned address, never a fetch that runs past RAM's end)
		s.npc = s.srr0 & ~3u;
		set_msr(s.srr1);
		check_pc(s.npc);
		return;
	case 129: cr_op(s, op, [](int a, int b) { return a & (b ^ 1); }); return;  // crandc
	case 150: return;  // isync
	case 193: cr_op(s, op, [](int a, int b) { return a ^ b; }); return;        // crxor
	case 225: cr_op(s, op, [](int a, int b) { return (a & b) ^ 1; }); return;  // crnand
	case 257: cr_op(s, op, [](int a, int b) { return a & b; }); return;        // crand
	case 289: cr_op(s, op, [](int a, int b) { return (a ^ b) ^ 1; }); return;  // creqv
	case 417: cr_op(s, op, [](int a, int b) { return a | (b ^ 1); }); return;  // crorc
	case 449: cr_op(s, op, [](int a, int b) { return a | b; }); return;        // cror
	case 528:  // bcctr
		if (cond(rd(op), ra(op))) {
			s.npc = s.ctr & ~3u;
			check_pc(s.npc);
		}
		if (op & 1)
			s.lr = s.pc + 4;
		return;
	default: invalid(op); return;
	}
}

void Cpu::op31(uint32_t op)
{
	const uint32_t t = rd(op), a = ra(op), b = rb(op);
	auto ea_x = [&]() { return R(b) + (a ? R(a) : 0); };
	auto ea_ux = [&]() { return R(a) + R(b); };
	switch ((op >> 1) & 0x3FF) {
	case 0: cmp_signed(s, (op >> 23) & 7, int32_t(R(a)), int32_t(R(b))); return;  // cmp
	case 4: if (trap(int32_t(R(a)), int32_t(R(b)), t)) exception(EXC_TRAP); return;  // tw
	case 8: case 8 | 512: {  // subfc
		uint32_t x = R(a), y = R(b);
		R(t) = y - x;
		set_ca(s, sub_ca(R(t), y, x));
		if (oebit(op)) set_sub_ov(s, R(t), y, x);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 10: case 10 | 512: {  // addc
		uint32_t x = R(a), y = R(b);
		R(t) = x + y;
		set_ca(s, add_ca(R(t), x, y));
		if (oebit(op)) set_add_ov(s, R(t), x, y);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 11: R(t) = uint32_t((uint64_t(R(a)) * uint64_t(R(b))) >> 32); if (rcbit(op)) set_cr0(s, R(t)); return;  // mulhwu
	case 19: {  // mfcr
		uint32_t v = 0;
		for (int i = 0; i < 8; i++) v = (v << 4) | s.cr[i];
		R(t) = v;
		return;
	}
	case 20: { uint32_t ea = ea_x(); s.reserved_address = ea; s.reserved = 1; R(t) = r32(ea); return; }  // lwarx
	case 23: R(t) = r32(ea_x()); return;  // lwzx
	case 24: {  // slw
		int sh = R(b) & 0x3F;
		R(a) = sh > 31 ? 0 : R(t) << sh;
		if (rcbit(op)) set_cr0(s, R(a));
		return;
	}
	case 26: {  // cntlzw
		int n = 0;
		uint32_t m = 0x80000000;
		while (n < 32 && !(R(t) & m)) { m >>= 1; n++; }
		R(a) = uint32_t(n);
		if (rcbit(op)) set_cr0(s, R(a));
		return;
	}
	case 28: case 28 | 512: R(a) = R(t) & R(b); if (rcbit(op)) set_cr0(s, R(a)); return;  // and
	case 32: cmp_unsigned(s, (op >> 23) & 7, R(a), R(b)); return;  // cmpl
	case 40: case 40 | 512: {  // subf
		uint32_t x = R(a), y = R(b);
		R(t) = y - x;
		if (oebit(op)) set_sub_ov(s, R(t), y, x);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 54: case 86: case 246: case 278: case 470: case 1014: return;  // dcbst, dcbf, dcbtst, dcbt, dcbi, dcbz (Reference: dcbz does nothing)
	case 55: { uint32_t ea = ea_ux(); R(t) = r32(ea); R(a) = ea; return; }  // lwzux
	case 60: R(a) = R(t) & ~R(b); if (rcbit(op)) set_cr0(s, R(a)); return;  // andc
	case 75: R(t) = uint32_t((int64_t(int32_t(R(a))) * int64_t(int32_t(R(b)))) >> 32); if (rcbit(op)) set_cr0(s, R(t)); return;  // mulhw
	case 83: R(t) = s.msr; return;  // mfmsr
	case 87: R(t) = r8(ea_x()); return;  // lbzx
	case 104: case 104 | 512:  // neg
		R(t) = 0u - R(a);
		if (oebit(op)) {
			if (R(t) == 0x80000000) s.xer |= XER_OV | XER_SO;
			else s.xer &= ~XER_OV;
		}
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	case 119: { uint32_t ea = ea_ux(); R(t) = r8(ea); R(a) = ea; return; }  // lbzux
	case 124: R(a) = ~(R(t) | R(b)); if (rcbit(op)) set_cr0(s, R(a)); return;  // nor
	case 136: case 136 | 512: {  // subfe
		uint32_t x = R(a), y = R(b), carry = (s.xer >> 29) & 1;
		uint32_t r = ~x + carry;
		R(t) = y + r;
		set_ca(s, add_ca(r, ~x, carry));
		if (R(t) < r) s.xer |= XER_CA;
		if (oebit(op)) set_sub_ov(s, R(t), y, x);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 138: case 138 | 512: {  // adde
		uint32_t x = R(a), y = R(b), carry = (s.xer >> 29) & 1;
		uint32_t tmp = y + carry;
		R(t) = x + tmp;
		set_ca(s, add_ca(tmp, y, carry) || add_ca(R(t), x, tmp));
		if (oebit(op)) set_add_ov(s, R(t), x, y);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 144: {  // mtcrf
		uint32_t fxm = (op >> 12) & 0xFF;
		for (int i = 0; i < 8; i++)
			if (fxm & (0x80 >> i)) s.cr[i] = (R(t) >> (28 - 4 * i)) & 0xF;
		return;
	}
	case 146: set_msr(R(t)); return;  // mtmsr
	case 150: {  // stwcx.
		uint32_t ea = ea_x();
		// Reference: stores whenever a reservation exists, whatever its address
		if (s.reserved) {
			w32(ea, R(t));
			s.reserved = 0;
			s.reserved_address = 0;
			s.cr[0] = 2;
		} else
			s.cr[0] = 0;
		if (s.xer & XER_SO) s.cr[0] |= 1;
		return;
	}
	case 151: w32(ea_x(), R(t)); return;  // stwx
	case 183: { uint32_t ea = ea_ux(); w32(ea, R(t)); R(a) = ea; return; }  // stwux
	case 200: case 200 | 512: {  // subfze
		uint32_t x = R(a), carry = (s.xer >> 29) & 1;
		R(t) = ~x + carry;
		set_ca(s, add_ca(R(t), ~x, carry));
		if (oebit(op)) set_sub_ov(s, R(t), 0, R(a));  // Reference: RA read after the result is written
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 202: case 202 | 512: {  // addze
		uint32_t x = R(a), carry = (s.xer >> 29) & 1;
		R(t) = x + carry;
		set_ca(s, add_ca(R(t), x, carry));
		if (oebit(op)) set_add_ov(s, R(t), x, carry);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 210: s.sr[(op >> 16) & 15] = R(t); return;  // mtsr
	case 215: w8(ea_x(), uint8_t(R(t))); return;  // stbx
	case 232: case 232 | 512: {  // subfme
		uint32_t x = R(a), carry = (s.xer >> 29) & 1;
		uint32_t r = ~x + carry;
		R(t) = r - 1;
		set_ca(s, sub_ca(r, ~x, carry));
		if (R(t) < r) s.xer |= XER_CA;
		if (oebit(op)) set_sub_ov(s, R(t), 0xFFFFFFFF, x);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 234: case 234 | 512: {  // addme
		uint32_t x = R(a), carry = (s.xer >> 29) & 1;
		uint32_t tmp = x + carry;
		R(t) = tmp - 1;
		set_ca(s, add_ca(tmp, x, carry) || add_ca(R(t), tmp, 0xFFFFFFFF));
		if (oebit(op)) set_add_ov(s, R(t), x, carry - 1);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 235: case 235 | 512: {  // mullw
		int64_t r = int64_t(int32_t(R(a))) * int64_t(int32_t(R(b)));
		R(t) = uint32_t(r);
		if (oebit(op)) {
			s.xer &= ~XER_OV;
			if (r != int64_t(int32_t(r))) s.xer |= XER_OV | XER_SO;
		}
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 242: s.sr[R(b) >> 28] = R(t); return;  // mtsrin
	case 247: { uint32_t ea = ea_ux(); w8(ea, uint8_t(R(t))); R(a) = ea; return; }  // stbux
	case 266: case 266 | 512: {  // add
		uint32_t x = R(a), y = R(b);
		R(t) = x + y;
		if (oebit(op)) set_add_ov(s, R(t), x, y);
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	}
	case 279: R(t) = r16(ea_x()); return;  // lhzx
	case 284: R(a) = ~(R(t) ^ R(b)); if (rcbit(op)) set_cr0(s, R(a)); return;  // eqv
	case 306: case 370: case 566: return;  // tlbie, tlbia, tlbsync
	case 311: { uint32_t ea = ea_ux(); R(t) = r16(ea); R(a) = ea; return; }  // lhzux
	case 316: R(a) = R(t) ^ R(b); if (rcbit(op)) set_cr0(s, R(a)); return;  // xor
	case 339: R(t) = get_spr(int(spr_field(op))); return;  // mfspr
	case 343: R(t) = uint32_t(int32_t(int16_t(r16(ea_x())))); return;  // lhax
	case 371: {  // mftb
		uint32_t x = spr_field(op);
		if (x == 268) R(t) = uint32_t(read_tb());
		else if (x == 269) R(t) = uint32_t(read_tb() >> 32);
		else fail("mftb from unknown register", x);
		return;
	}
	case 375: { uint32_t ea = ea_ux(); R(t) = uint32_t(int32_t(int16_t(r16(ea)))); R(a) = ea; return; }  // lhaux
	case 407: w16(ea_x(), uint16_t(R(t))); return;  // sthx
	case 412: R(a) = R(t) | ~R(b); if (rcbit(op)) set_cr0(s, R(a)); return;  // orc
	case 439: { uint32_t ea = ea_ux(); w16(ea, uint16_t(R(t))); R(a) = ea; return; }  // sthux
	case 444: R(a) = R(t) | R(b); if (rcbit(op)) set_cr0(s, R(a)); return;  // or
	case 459: case 459 | 512:  // divwu
		if (R(b) == 0) {
			R(t) = 0;
			if (oebit(op)) s.xer |= XER_SO | XER_OV;
		} else {
			R(t) = R(a) / R(b);
			if (oebit(op)) s.xer &= ~XER_OV;
		}
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	case 467: set_spr(int(spr_field(op)), R(t)); return;  // mtspr
	case 476: R(a) = ~(R(t) & R(b)); if (rcbit(op)) set_cr0(s, R(a)); return;  // nand
	case 491: case 491 | 512:  // divw
		if (R(b) == 0 && R(a) < 0x80000000) {
			R(t) = 0;
			if (oebit(op)) s.xer |= XER_SO | XER_OV;
		} else if (R(b) == 0 || (R(b) == 0xFFFFFFFF && R(a) == 0x80000000)) {
			R(t) = 0xFFFFFFFF;
			if (oebit(op)) s.xer |= XER_SO | XER_OV;
		} else {
			R(t) = uint32_t(int32_t(R(a)) / int32_t(R(b)));
			if (oebit(op)) s.xer &= ~XER_OV;
		}
		if (rcbit(op)) set_cr0(s, R(t));
		return;
	case 512: s.cr[t >> 2] = (s.xer >> 28) & 0xF; s.xer &= ~0xF0000000u; return;  // mcrxr
	case 533: case 597: {  // lswx, lswi
		uint32_t ea;
		int n;
		if (((op >> 1) & 0x3FF) == 597) {
			ea = a ? R(a) : 0;
			n = b == 0 ? 32 : int(b);
		} else {
			ea = R(b) + (a ? R(a) : 0);
			n = int(s.xer & 0x7F);
		}
		int r = int(t) - 1, i = 0;
		while (n > 0) {
			if (i == 0) { r = (r + 1) % 32; R(r) = 0; }
			R(r) |= uint32_t(r8(ea) & 0xFF) << (24 - i);
			i += 8;
			if (i == 32) i = 0;
			ea++;
			n--;
		}
		return;
	}
	case 534: R(t) = __builtin_bswap32(r32(ea_x())); return;  // lwbrx
	case 535: case 567: {  // lfsx, lfsux
		uint32_t ea = ((op >> 1) & 0x3FF) == 567 ? ea_ux() : ea_x();
		F32 f;
		f.i = r32(ea);
		F(t).d = double(f.f);
		if (((op >> 1) & 0x3FF) == 567) R(a) = ea;
		return;
	}
	case 536: {  // srw
		int sh = R(b) & 0x3F;
		R(a) = sh > 31 ? 0 : R(t) >> sh;
		if (rcbit(op)) set_cr0(s, R(a));
		return;
	}
	case 595: R(t) = s.sr[(op >> 16) & 15]; return;  // mfsr
	case 598: return;  // sync
	case 599: case 631: {  // lfdx, lfdux
		uint32_t ea = ((op >> 1) & 0x3FF) == 631 ? ea_ux() : ea_x();
		F(t).i = r64(ea);
		if (((op >> 1) & 0x3FF) == 631) R(a) = ea;
		return;
	}
	case 659: R(t) = s.sr[R(b) >> 28]; return;  // mfsrin
	case 661: case 725: {  // stswx, stswi
		uint32_t ea;
		int n;
		if (((op >> 1) & 0x3FF) == 725) {
			ea = a ? R(a) : 0;
			n = b == 0 ? 32 : int(b);
		} else {
			ea = R(b) + (a ? R(a) : 0);
			n = int(s.xer & 0x7F);
		}
		int r = int(t) - 1, i = 0;
		while (n > 0) {
			if (i == 0) r = (r + 1) % 32;
			w8(ea, uint8_t((R(r) >> (24 - i)) & 0xFF));
			i += 8;
			if (i == 32) i = 0;
			ea++;
			n--;
		}
		return;
	}
	case 662: w32(ea_x(), __builtin_bswap32(R(t))); return;  // stwbrx
	case 663: case 695: {  // stfsx, stfsux
		uint32_t ea = ((op >> 1) & 0x3FF) == 695 ? ea_ux() : ea_x();
		F32 f;
		f.f = float(F(t).d);
		w32(ea, f.i);
		if (((op >> 1) & 0x3FF) == 695) R(a) = ea;
		return;
	}
	case 727: case 759: {  // stfdx, stfdux
		uint32_t ea = ((op >> 1) & 0x3FF) == 759 ? ea_ux() : ea_x();
		w64(ea, F(t).i);
		if (((op >> 1) & 0x3FF) == 759) R(a) = ea;
		return;
	}
	case 758: return;  // dcba
	case 790: R(t) = __builtin_bswap16(r16(ea_x())); return;  // lhbrx
	case 792: {  // sraw
		int sh = R(b) & 0x3F;
		s.xer &= ~XER_CA;
		if (sh > 31) {
			R(a) = (R(t) & 0x80000000) ? 0xFFFFFFFF : 0;
			if (R(a)) s.xer |= XER_CA;
		} else {
			uint32_t v = R(t);
			R(a) = uint32_t(int32_t(v) >> sh);
			if (int32_t(v) < 0 && (v & ((1u << sh) - 1))) s.xer |= XER_CA;
		}
		if (rcbit(op)) set_cr0(s, R(a));
		return;
	}
	case 824: {  // srawi
		int sh = int(b);
		uint32_t v = R(t);
		s.xer &= ~XER_CA;
		if (int32_t(v) < 0 && (v & ((1u << sh) - 1))) s.xer |= XER_CA;
		R(a) = uint32_t(int32_t(v) >> sh);
		if (rcbit(op)) set_cr0(s, R(a));
		return;
	}
	case 854: return;  // eieio
	case 918: w16(ea_x(), __builtin_bswap16(uint16_t(R(t)))); return;  // sthbrx
	case 922: R(a) = uint32_t(int32_t(int16_t(R(t)))); if (rcbit(op)) set_cr0(s, R(a)); return;  // extsh
	case 954: R(a) = uint32_t(int32_t(int8_t(R(t)))); if (rcbit(op)) set_cr0(s, R(a)); return;   // extsb
	case 982: return;  // icbi
	case 983: w32(ea_x(), uint32_t(F(t).i)); return;  // stfiwx
	default: invalid(op); return;
	}
}

void Cpu::op59(uint32_t op)
{
	const uint32_t t = rd(op), a = ra(op), b = rb(op), c = rc(op);
	uint32_t x = (op >> 1) & 0x3FF;
	const double A = F(a).d, B = F(b).d, C = F(c).d;
	// The reference's operand order: round-to-nearest takes c * a and b + a; the other modes a * c and a + b
	const bool nearest = !(s.fpscr & 3);
	auto mul = [&]() { return nearest ? sse(C * A, C, A) : sse(A * C, A, C); };
	switch (x & 31) {  // the A-form instructions match on their low 5 bits
	case 25: { vxsnan2(s, F(a), F(c)); Rounding m(s.fpscr); F(t).d = to_single(mul()); } break;  // fmuls
	case 28: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = to_single(sse(p - B, p, B)); } break;  // fmsubs
	case 29: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = to_single(sse(p + B, p, B)); } break;  // fmadds
	case 30: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = to_single(neg(sse(p - B, p, B))); } break;  // fnmsubs
	case 31: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = to_single(neg(sse(p + B, p, B))); } break;  // fnmadds
	default:
		switch (x) {
		case 18: { vxsnan2(s, F(a), F(b)); Rounding m(s.fpscr); F(t).d = to_single(sse(A / B, A, B)); } break;  // fdivs
		case 20: { vxsnan2(s, F(a), F(b)); Rounding m(s.fpscr); F(t).d = to_single(sse(A - B, A, B)); } break;  // fsubs
		case 21: { vxsnan2(s, F(a), F(b)); Rounding m(s.fpscr); F(t).d = to_single(nearest ? sse(B + A, B, A) : sse(A + B, A, B)); } break;  // fadds
		case 22: { vxsnan(s, F(b)); Rounding m(s.fpscr); F(t).d = to_single(sse_sqrt(B)); } break;  // fsqrts (not on a 603e)
		// Reference: fres is an exact 1/x, not the 603e's estimate
		case 24: { vxsnan(s, F(b)); Rounding m(s.fpscr); F(t).d = to_single(sse(1.0 / B, 1.0, B)); } break;  // fres
		default: invalid(op); return;
		}
	}
	set_fprf(s, F(t));
	if (rcbit(op)) set_cr1(s);
}

void Cpu::op63(uint32_t op)
{
	const uint32_t t = rd(op), a = ra(op), b = rb(op), c = rc(op);
	uint32_t x = (op >> 1) & 0x3FF;
	const double A = F(a).d, B = F(b).d, C = F(c).d;
	const bool nearest = !(s.fpscr & 3);
	auto mul = [&]() { return nearest ? sse(C * A, C, A) : sse(A * C, A, C); };
	// Reference: fmadd and friends round the product and then the sum (unfused), as the reference does on x86-64
	switch (x & 31) {
	case 23: F(t).i = (is_nan(F(a)) || F(a).d < 0.0) ? F(b).i : F(c).i; if (rcbit(op)) set_cr1(s); return;  // fsel
	case 25: { vxsnan2(s, F(a), F(c)); Rounding m(s.fpscr); F(t).d = mul(); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fmul
	case 28: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = sse(p - B, p, B); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fmsub
	case 29: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = sse(p + B, p, B); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fmadd
	case 30: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = neg(sse(p - B, p, B)); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fnmsub
	case 31: { vxsnan2(s, F(a), F(b)); vxsnan(s, F(c)); Rounding m(s.fpscr); double p = mul(); F(t).d = neg(sse(p + B, p, B)); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fnmadd
	default: break;
	}
	switch (x) {
	case 0: case 32: {  // fcmpu, fcmpo
		uint32_t cr = (t >> 2), v;
		vxsnan2(s, F(a), F(b));
		if (is_nan(F(a)) || is_nan(F(b))) {
			v = 1;
			if (is_snan(F(a)) || is_snan(F(b))) {
				s.fpscr |= 0x01000000;
				if (x == 32 && (!(s.fpscr & 0x40000000) || is_qnan(F(a)) || is_qnan(F(b))))
					s.fpscr |= 0x00080000;
			}
		} else if (F(a).d < F(b).d) v = 8;
		else if (F(a).d > F(b).d) v = 4;
		else v = 2;
		s.cr[cr] = uint8_t(v);
		s.fpscr = (s.fpscr & ~0x0001F000u) | (v << 12);
		return;
	}
	case 12: { vxsnan(s, F(b)); F(t).d = to_single(B); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // frsp
	case 14: case 15: {  // fctiw, fctiwz
		int64_t r = 0;
		vxsnan(s, F(b));
		uint32_t mode = x == 15 ? 1 : s.fpscr & 3;
		switch (mode) {
		case 0: { Rounding m(s.fpscr); r = to_i64(std::nearbyint(F(b).d)); } break;
		case 1: r = to_i64(std::trunc(F(b).d)); break;
		case 2: r = to_i64(std::ceil(F(b).d)); break;
		case 3: r = to_i64(std::floor(F(b).d)); break;
		}
		if (r > int64_t(int32_t(0x7FFFFFFF))) F(t).i = uint64_t(int64_t(int32_t(0x7FFFFFFF)));
		else if (r < int64_t(int32_t(0x80000000))) F(t).i = uint64_t(int64_t(int32_t(0x80000000)));
		else F(t).i = uint32_t(r);
		if (rcbit(op)) set_cr1(s);
		return;
	}
	case 18: { vxsnan2(s, F(a), F(b)); Rounding m(s.fpscr); F(t).d = sse(A / B, A, B); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fdiv
	case 20: { vxsnan2(s, F(a), F(b)); Rounding m(s.fpscr); F(t).d = sse(A - B, A, B); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fsub
	case 21: { vxsnan2(s, F(a), F(b)); Rounding m(s.fpscr); F(t).d = nearest ? sse(B + A, B, A) : sse(A + B, A, B); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fadd
	case 22: { vxsnan(s, F(b)); Rounding m(s.fpscr); F(t).d = sse_sqrt(B); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // fsqrt (not on a 603e)
	// Reference: frsqrte is an exact 1/sqrt(x), not the 603e's estimate
	case 26: { vxsnan(s, F(b)); Rounding m(s.fpscr); double r = sse_sqrt(B); F(t).d = sse(1.0 / r, 1.0, r); } set_fprf(s, F(t)); if (rcbit(op)) set_cr1(s); return;  // frsqrte
	case 38: case 70: {  // mtfsb1, mtfsb0
		uint32_t bit = t;
		if (bit != 1 && bit != 2) {
			if (x == 38) s.fpscr |= 1u << (31 - bit);
			else s.fpscr &= ~(1u << (31 - bit));
		}
		if (rcbit(op)) set_cr1(s);
		return;
	}
	case 40: F(t).i = F(b).i ^ D_SIGN; if (rcbit(op)) set_cr1(s); return;   // fneg
	case 64: {  // mcrfs
		uint32_t crfs = (op >> 18) & 7;
		uint32_t f = (s.fpscr >> ((7 - crfs) * 4)) & 0xF;
		static const uint32_t clear[8] = { 0x90000000, 0x0F000000, 0x00F00000, 0x00080000, 0, 0x00000700, 0, 0 };
		s.fpscr &= ~clear[crfs];
		s.cr[(op >> 23) & 7] = uint8_t(f);
		return;
	}
	case 72: F(t).i = F(b).i; if (rcbit(op)) set_cr1(s); return;  // fmr
	case 134: {  // mtfsfi
		uint32_t imm = (op >> 12) & 0xF;
		uint32_t sh = (7 - ((op >> 23) & 7)) * 4;
		if (sh == 28) {
			// Reference: also ORs the immediate into the low bits (the rounding mode)
			s.fpscr &= 0x9FFFFFFF;
			s.fpscr |= imm & 0x9FFFFFFF;
		}
		s.fpscr &= ~(0xFu << sh);
		s.fpscr |= imm << sh;
		if (rcbit(op)) set_cr1(s);
		return;
	}
	case 136: F(t).i = F(b).i | D_SIGN; if (rcbit(op)) set_cr1(s); return;  // fnabs
	case 264: F(t).i = F(b).i & ~D_SIGN; if (rcbit(op)) set_cr1(s); return;  // fabs
	case 583: F(t).i = uint32_t(s.fpscr); if (rcbit(op)) set_cr1(s); return;  // mffs
	case 711: {  // mtfsf
		uint32_t f = field_mask[(op >> 17) & 0xFF];
		const uint32_t FEX = 0x40000000, VX = 0x20000000;
		// Reference: only FEX and VX of the selected fields are cleared before the OR
		s.fpscr &= (~f) | ~(FEX | VX);
		s.fpscr |= uint32_t(F(b).i) & ~(FEX | VX);
		if (rcbit(op)) set_cr1(s);
		return;
	}
	default: invalid(op); return;
	}
}

void Cpu::dump_state(uint32_t *o) const
{
	memset(o, 0, 128 * 4);
	for (int i = 0; i < 32; i++) o[i] = s.r[i];
	for (int i = 0; i < 32; i++) {
		o[32 + 2 * i] = uint32_t(s.f[i].i >> 32);
		o[33 + 2 * i] = uint32_t(s.f[i].i);
	}
	uint32_t cr = 0;
	for (int i = 0; i < 8; i++) cr = (cr << 4) | (s.cr[i] & 0xF);
	o[96] = cr; o[97] = s.xer; o[98] = s.lr; o[99] = s.ctr; o[100] = s.msr; o[101] = s.npc;
	o[102] = s.srr0; o[103] = s.srr1; o[104] = s.dec; o[105] = uint32_t(s.tb); o[106] = uint32_t(s.tb >> 32);
	o[107] = s.fpscr;
	for (int i = 0; i < 4; i++) o[108 + i] = s.sprg[i];
	o[112] = s.hid0; o[113] = s.hid1; o[117] = uint32_t(s.interrupt_pending); o[118] = s.timer_frac;
}

} // namespace ppc
