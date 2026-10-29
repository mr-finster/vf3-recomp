// The Model 3 Step 1.0 board. See model3.h.
#include "model3.h"
#include "romset.h"
#include <cstring>
#include <ctime>
#include <random>

namespace m3 {

namespace {

inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t &word(std::vector<uint8_t> &v, size_t at) { return *reinterpret_cast<uint32_t *>(&v[at]); }
inline uint16_t &half(std::vector<uint8_t> &v, size_t at) { return *reinterpret_cast<uint16_t *>(&v[at]); }

const uint32_t IRQ_SCSI = 0x100;     // the 53C810's interrupt, not maskable (as the reference wires it)
const uint32_t IRQ_R3D_DMA = 0x100;
const uint32_t PCI_ID_REAL3D = 0x16C311DB;  // Step 1.x
const uint32_t PCI_ID_53C810 = 0x00011000;

} // namespace

// ---- IRQ controller ----

void Irq::assert_(unsigned bits)
{
	state |= bits;
	if (state & enable)
		cpu->set_irq_line(true);
	if (state & ~0xFFu)
		cpu->set_irq_line(true);
}

void Irq::deassert(unsigned bits)
{
	state &= ~bits;
	if (!(state & enable) && !(state & ~0xFFu))
		cpu->set_irq_line(false);
}

// ---- 93C46 EEPROM ----

static uint16_t reverse16(uint16_t v)
{
	uint16_t r = 0;
	for (int i = 0; i < 16; i++)
		if (v & (1 << i))
			r |= uint16_t(1 << (15 - i));
	return r;
}

Eeprom::Eeprom() { memset(regs, 0xFF, sizeof(regs)); }

void Eeprom::reset()
{
	receiving = true;
	locked = true;
	in = 0;
	out = 0;
	addr = 0;
	busy = 0;
	CS = 0;
}

void Eeprom::write(unsigned cs, unsigned clk, unsigned di)
{
	unsigned prev = CLK;
	CS = !!cs;
	CLK = !!clk;
	DI = !!di;
	if (!CS) {
		in = 0;
		receiving = true;
		busy = 5;
		return;
	}
	if (prev || !CLK)
		return;
	if (receiving) {
		in = (in << 1) | DI;
		if ((in & 0xFFFFFFC0) == 0x180) {             // READ
			addr = in & 0x3F;
			out = reverse16(regs[addr]);
			bits_out = 0;
			receiving = false;
		} else if (in == 0x13) {                      // write enable
			locked = false;
		} else if (in == 0x10) {                      // write disable
			locked = true;
		} else if ((in & 0xFFC00000) == 0x01400000) { // WRITE
			if (!locked) regs[(in >> 16) & 0x3F] = uint16_t(in);
			DO = 1;
		} else if ((in & 0xFFF00000) == 0x01100000) { // WRITE ALL
			if (!locked) for (auto &r : regs) r = uint16_t(in);
			DO = 1;
		} else if ((in & 0xFFFFFFC0) == 0x1C0) {      // ERASE
			if (!locked) regs[in & 0x3F] = 0xFFFF;
			DO = 1;
		} else if ((in & 0xFFFFFFF0) == 0x120) {      // ERASE ALL
			if (!locked) for (auto &r : regs) r = 0xFFFF;
			DO = 1;
		}
	} else {
		DO = out & 1;
		out >>= 1;
		if (++bits_out == 16) {
			addr = (addr + 1) & 0x3F;
			out = reverse16(regs[addr]);
			bits_out = 0;
		}
	}
}

unsigned Eeprom::read()
{
	if (receiving) {
		if (busy > 0) {
			--busy;
			return 0;
		}
		return 1;
	}
	return DO;
}

// ---- JTAG ----

namespace {
enum { TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PAUSE_DR, EX2_DR, UPD_DR, SEL_IR, CAP_IR, SH_IR, EX1_IR, PAUSE_IR, EX2_IR, UPD_IR };
const int FSM[16][2] = {
	{ RTI, TLR }, { RTI, SEL_DR }, { CAP_DR, SEL_IR }, { SH_DR, EX1_DR }, { SH_DR, EX1_DR }, { PAUSE_DR, UPD_DR },
	{ PAUSE_DR, EX2_DR }, { SH_DR, UPD_DR }, { RTI, SEL_DR }, { CAP_IR, TLR }, { SH_IR, EX1_IR }, { SH_IR, EX1_IR },
	{ PAUSE_IR, UPD_IR }, { PAUSE_IR, EX2_IR }, { SH_IR, UPD_IR }, { RTI, SEL_DR },
};
}

Jtag::Jtag()
{
	// The Step 1.x chain: Jupiter, Mercury, Venus, Earth, a 3D-RAM, two Mars, three 3D-RAMs
	struct { bool asic; int kind; uint32_t id; } chain[10] = {
		{ true, 4, 0x116c7057 }, { true, 0, 0x216c3057 }, { true, 1, 0x116c4057 }, { true, 2, 0x216c5057 },
		{ false, -1, 0 }, { true, 3, 0x116c6057 }, { true, -1, 0x116c6057 }, { false, -1, 0 }, { false, -1, 0 },
		{ false, -1, 0 },
	};
	for (int i = 0; i < 10; i++) {
		dev[i].asic = chain[i].asic;
		dev[i].kind = chain[i].kind;
		dev[i].id = chain[i].id;
		dev_reset(dev[i]);
	}
}

void Jtag::dev_reset(Device &d)
{
	if (d.asic) {
		d.size = 32;
		d.shift = d.id;
		d.ir = 3;   // IDCODE
	} else {
		d.size = 1;
		d.shift = 0;
		d.ir = 15;  // BYPASS
	}
}

void Jtag::capture_dr(Device &d)
{
	if (d.asic && d.ir == 3) { d.size = 32; d.shift = d.id; }
	else if (d.asic && d.ir == 8) { d.size = 32; d.shift = d.mode; }
	else { d.size = 1; d.shift = 0; }
}

void Jtag::capture_ir(Device &d)
{
	if (d.asic) { d.size = 5; d.shift = 1; }
	else { d.size = 4; d.shift = 0b1001; }
}

void Jtag::update_dr(Device &d)
{
	if (d.asic && d.ir == 8) {
		d.mode = uint32_t(d.shift);
		if (d.kind >= 0)
			modeword[d.kind] = d.mode;
	}
}

void Jtag::reset()
{
	state = TLR;
	for (auto &d : dev) dev_reset(d);
}

void Jtag::write(bool tck, bool tms, bool tdi, bool trst)
{
	if (!trst) {
		for (auto &d : dev) dev_reset(d);
		return;
	}
	if (last_tck == tck)
		return;
	last_tck = tck;
	if (tck) {
		switch (state) {
		case TLR: for (auto &d : dev) dev_reset(d); break;
		case CAP_DR: for (auto &d : dev) capture_dr(d); break;
		case CAP_IR: for (auto &d : dev) capture_ir(d); break;
		case SH_DR: case SH_IR:
			for (auto &d : dev) {
				bool out = d.shift & 1;
				d.shift >>= 1;
				if (tdi) d.shift |= uint64_t(1) << (d.size - 1);
				else d.shift &= ~(uint64_t(1) << (d.size - 1));
				tdi = out;
			}
			break;
		default: break;
		}
		state = FSM[state][tms];
	} else {
		switch (state) {
		case SH_DR: case SH_IR: tdo = dev[9].shift & 1; break;
		case UPD_DR: for (auto &d : dev) update_dr(d); break;
		case UPD_IR: for (auto &d : dev) d.ir = uint8_t(d.shift); break;
		default: break;
		}
	}
}

// ---- 53C810 ----

void Scsi::reset()
{
	memset(regs, 0, sizeof(regs));
	regs[0x00] = 0xC0;
	regs[0x0C] = 0x80;
	regs[0x0F] = 0x02;
	regs[0x18] = 0xFF;
	regs[0x19] = 0xF0;
	regs[0x1A] = 0x01;
	regs[0x46] = 0x60;
	regs[0x47] = 0x0F;
	regs[0x4C] = 0x03;
	temp = dsp = dsps = dbc = 0;
	dcmd = dcntl = dmode = dien = istat = 0;
	dstat = 0x80;
	halt = false;
}

uint32_t Scsi::fetch(uint32_t off)
{
	return bswap32(board->read32(dsp + off));
}

// One SCRIPTS instruction: memory moves and INT; anything else stops the processor
bool Scsi::exec()
{
	uint32_t op = fetch(0);
	dbc = op & 0x00FFFFFF;
	dcmd = uint8_t(op >> 24);
	dsps = fetch(4);
	if ((dcmd & 0xE0) == 0xC0) {        // memory move
		uint32_t src = dsps, dst = temp = fetch(8);
		uint32_t n = dbc;
		for (uint32_t i = 0; i < n / 4; i++, src += 4, dst += 4)
			board->write32(dst, board->read32(src));
		for (n &= 3; n; n--)
			board->write8(dst++, board->read8(src++));
		dbc = 0;
		dsps = src;
		temp = dst;
		dsp += 12;
		return true;
	}
	if ((dcmd & 0xF8) == 0x98) {        // INT / INTFLY
		halt = true;
		istat |= 1;
		dstat |= 4;
		if (dien & 4)
			board->irq.assert_(IRQ_SCSI);
		return !(dbc & 0x100000);       // INTFLY isn't emulated (nor by the reference)
	}
	return false;
}

void Scsi::run(bool single)
{
	if (single) {
		exec();
		istat |= 1;
		dstat |= 8;
		if (dien & 8)
			board->irq.assert_(IRQ_SCSI);
		return;
	}
	for (int i = 0; i < 100 && !halt; i++)
		if (!exec())
			break;
}

void Scsi::write(unsigned reg, uint8_t data)
{
	if (reg >= 0x60)
		return;
	regs[reg] = data;
	auto set_byte = [](uint32_t &r, int n, uint8_t v) { r = (r & ~(0xFFu << (8 * n))) | (uint32_t(v) << (8 * n)); };
	switch (reg) {
	case 0x14: istat = data; break;
	case 0x1C: case 0x1D: case 0x1E: case 0x1F: set_byte(temp, reg - 0x1C, data); break;
	case 0x24: case 0x25: case 0x26: set_byte(dbc, reg - 0x24, data); break;
	case 0x27: dcmd = data; break;
	case 0x2C: case 0x2D: case 0x2E: set_byte(dsp, reg - 0x2C, data); break;
	case 0x2F:
		set_byte(dsp, 3, data);
		halt = false;
		if (!(dmode & 1))
			run(false);
		break;
	case 0x30: case 0x31: case 0x32: case 0x33: set_byte(dsps, reg - 0x30, data); break;
	case 0x38: dmode = data; break;
	case 0x39: dien = data; break;
	case 0x3B:
		dcntl = data;
		if ((dcntl & 0x14) == 0x14)
			run(true);
		else if (dcntl & 0x04)
			run(false);
		break;
	default: break;
	}
}

uint8_t Scsi::read(unsigned reg)
{
	if (reg >= 0x60)
		return 0;
	switch (reg) {
	case 0x0C: {
		uint8_t r = dstat;
		istat &= 0xFE;
		dstat &= 0xF7;
		board->irq.deassert(IRQ_SCSI);
		return r;
	}
	case 0x14: return istat;
	case 0x1C: case 0x1D: case 0x1E: case 0x1F: return uint8_t(temp >> (8 * (reg - 0x1C)));
	case 0x24: case 0x25: case 0x26: return uint8_t(dbc >> (8 * (reg - 0x24)));
	case 0x27: return dcmd;
	case 0x2C: case 0x2D: case 0x2E: case 0x2F: return uint8_t(dsp >> (8 * (reg - 0x2C)));
	case 0x30: case 0x31: case 0x32: case 0x33: return uint8_t(dsps >> (8 * (reg - 0x30)));
	case 0x38: return dmode;
	case 0x39: return dien;
	case 0x3B: return dcntl;
	default: return regs[reg];
	}
}

// ---- the board ----

bool Model3::load(const std::string &zip, std::string &error)
{
	romset::result r = romset::load(zip, *profile);
	if (!r.ok) {
		error = r.error;
		return false;
	}
	crom = std::move(r.crom);
	banked = std::move(r.banked);
	vrom = std::move(r.vrom);
	sound_rom = std::move(r.sound);
	samples = std::move(r.samples);
	ram.assign(0x800000, 0);
	backup.assign(0x20000, 0);
	security.assign(0x20000, 0);
	tile_ram.assign(0x120000, 0);
	// (each with 64K words of zeros past its end: the scene builder reads a node's words, a matrix (up to 0xFFF * 12
	// words on), a LOD entry or a pointer list from wherever the game points, checking only the start)
	cull_lo.assign(0x400000 / 4 + CULL_PAD, 0);
	cull_hi.assign(0x100000 / 4 + CULL_PAD, 0);
	poly_ram.assign(0x400000 / 4, 0);
	tex_fifo.reserve(0x100000 / 4);
	tex_ram.assign(2048 * 2048, 0);
	setup_pages();
	cpu.attach(this, ram.data(), crom.data());
	// Step 1.0: a 66 MHz 603e on a 66 MHz bus (multiplier 1.0); the reference's PVR and HID1 for it
	cpu.configure(0x00071202, 2, 66000000, 0xF0000000);
	irq.cpu = &cpu;
	scsi.board = this;
	set_crom_bank(0xFF);
	return true;
}

void Model3::reset()
{
	std::fill(ram.begin(), ram.end(), 0);
	set_crom_bank(0xFF);
	input_bank = serial1 = serial2 = adc_channel = 0;
	midi_ctrl = 0;
	cpu.reset();
	if (cpu.rec)
		cpu.rec->recount();
	irq.reset();
	pci_reset();
	scsi.reset();
	eeprom.reset();
	std::fill(tile_ram.begin(), tile_ram.end(), 0);
	memset(tile_regs, 0, sizeof(tile_regs));
	std::fill(cull_lo.begin(), cull_lo.end(), 0);
	std::fill(cull_hi.begin(), cull_hi.end(), 0);
	std::fill(poly_ram.begin(), poly_ram.end(), 0);
	tex_fifo.clear();
	std::fill(tex_ram.begin(), tex_ram.end(), 0);
	snap_epoch++;   // (the memories filled: states saved before copy whole)
	tex_serial++;
	tex_uploads.clear();
	tex_uploads.push_back({ 0, 0, 2048, 2048 });
	command_written = tilegen_draw = false;
	pending_hi.clear();
	pending_poly.clear();
	memset(r3d_config, 0, sizeof(r3d_config));
	vrom_fifo_n = 0;
	ping_pong = ping_pong_copy = false;
	dma_src = dma_dst = dma_len = dma_data = 0;
	dma_status = dma_config = 0;
	jtag.reset();
	gpus_ready = false;
	frame = 0;
}

void Model3::power_on()
{
	// the PowerPC's registers (a reset leaves them: a board that had run kept its own), but what configure set
	const uint32_t pvr = cpu.s.pvr, hid1 = cpu.s.hid1;
	const int timer_ratio = cpu.s.timer_ratio;
	cpu.s = ppc::State{};
	cpu.s.pvr = pvr;
	cpu.s.hid1 = hid1;
	cpu.s.timer_ratio = timer_ratio;
	// the devices as made (the EEPROM keeping what it holds; the 53C810 its board)
	uint16_t words[64];
	memcpy(words, eeprom.regs, sizeof words);
	eeprom = Eeprom();
	memcpy(eeprom.regs, words, sizeof words);
	jtag = Jtag();
	scsi = Scsi{};
	scsi.board = this;
	std::fill(security.begin(), security.end(), 0);
	flushes = 0;
	vrom_fifo[0] = vrom_fifo[1] = 0;
	gun_reg = 0;
	outputs[0] = outputs[1] = 0;
	flip_line_ = 0;
	midi.clear();
	controls = Controls{};
	reset();
}

void Model3::set_crom_bank(uint8_t v)
{
	crom_bank_reg = v;
	crom_bank = banked.data() + size_t((~v) & 0xF) * 0x800000;
}

// One video frame, timed as the reference times it: 57.524160 Hz, 424 lines of which 40 are blanking; the
// vertical blank first (waiting for the game to acknowledge IRQ 2, then MIDI interrupts while it sends), then
// the 384 active lines, the ping-pong flip at the line the tile generator says and IRQ 2 at the start of the last.
void Model3::run_frame()
{
	run_frame_start();
	run_frame_end();
}

void Model3::run_lines(unsigned from, unsigned to)
{
	const unsigned cycles = unsigned(cpu.cycles_per_second());
	const unsigned line_cycles = unsigned(float(cycles) / 57.524160f) / 424;
	const uint32_t flip_line = flip_line_;
	for (unsigned i = from; i < to; i++) {
		if (i == flip_line) {
			ping_pong = !ping_pong;
			tilegen_draw = command_written = false;
		}
		if (i == 383)
			irq.assert_(0x02);
		if (on_line) on_line(i);
		cpu.execute(int(line_cycles));
		if (after_slice) after_slice();
	}
}

void Model3::run_frame_start()
{
	auto exec = [this](int n) {
		cpu.execute(n);
		if (after_slice) after_slice();
	};
	const unsigned cycles = unsigned(cpu.cycles_per_second());
	const unsigned frame_cycles = unsigned(float(cycles) / 57.524160f);
	const unsigned line_cycles = frame_cycles / 424;
	unsigned vblank_cycles = line_cycles * 40;
	cpu.set_timer_ratio(int(unsigned(cpu.bus_freq_multiplier() * 2) * cycles / unsigned(cpu.cycles_per_second())));

	if (gpus_ready) {
		ping_pong_copy = ping_pong;
		for (auto &w : pending_hi) { cull_hi[w.first] = w.second; touch(2, size_t(w.first) * 4); }
		for (auto &w : pending_poly) { poly_ram[w.first] = w.second; touch(3, size_t(w.first) * 4); }
		pending_hi.clear();
		pending_poly.clear();
		if (command_written)
			r3d_flush_textures();
		while ((irq.enable & 2) && (irq.state & 2) && vblank_cycles > 1000) {
			exec(1000);
			vblank_cycles -= 1000;
		}
		int n = 0;
		while (midi_ctrl & 0x20) {
			if (!(irq.enable & 0x40))
				break;
			irq.assert_(0x40);
			exec(1000);
			vblank_cycles -= 1000;
			if (++n > 128)
				break;
		}
		exec(int(vblank_cycles));
		irq.assert_(0x0D);
	}
	flip_line_ = tile_regs[0x08 / 4];
	run_lines(0, 192);
}

void Model3::run_frame_end()
{
	run_lines(192, 384);
	gpus_ready = true;
	frame++;
}

// ---- save states ----

namespace {

template <class IO> void pairs(IO &io, std::vector<std::pair<uint32_t, uint32_t>> &v)
{
	uint64_t n = v.size();
	io.pod(n);
	if constexpr (IO::loading) {
		if (!io.ok || n > 0x1000000) { io.ok = false; return; }
		v.resize(size_t(n));
	}
	for (auto &w : v) { io.pod(w.first); io.pod(w.second); }
}

} // namespace

// The state's small parts, after the memories: the CPU, the board's registers, the Real3D's and the tile
// generator's, the writes and bytes waiting
template <class IO> void Model3::state(IO &io)
{
	io.section("cpu");
	cpu.s.state(io);
	io.section("board");
	irq.save_state(io);
	eeprom.save_state(io);
	jtag.save_state(io);
	scsi.save_state(io);
	for (int p = 0; p < 2; p++) {
		Controls &c = controls;
		io.pod(c.coin[p]); io.pod(c.start[p]); io.pod(c.test[p]); io.pod(c.service[p]);
		io.pod(c.up[p]); io.pod(c.down[p]); io.pod(c.left[p]); io.pod(c.right[p]);
		io.pod(c.punch[p]); io.pod(c.kick[p]); io.pod(c.guard[p]); io.pod(c.escape[p]);
	}
	io.pod(crom_bank_reg); io.pod(input_bank); io.pod(serial1); io.pod(serial2); io.pod(adc_channel); io.pod(gun_reg);
	io.pod(outputs); io.pod(gpus_ready);
	io.pod(pci_regs); io.pod(pci_bus); io.pod(pci_device); io.pod(pci_function); io.pod(pci_reg);
	io.pod(frame); io.pod(flip_line_);
	io.deq(midi);
	io.pod(midi_ctrl);
	io.section("tile generator");
	io.pod(tile_regs);
	io.pod(tilegen_draw);
	io.section("real3d");
	io.pod(r3d_config); io.pod(vrom_fifo); io.pod(vrom_fifo_n);
	io.pod(ping_pong); io.pod(ping_pong_copy); io.pod(flushes); io.pod(command_written);
	io.pod(dma_src); io.pod(dma_dst); io.pod(dma_len); io.pod(dma_data); io.pod(dma_status); io.pod(dma_config);
	pairs(io, pending_hi);
	pairs(io, pending_poly);
	io.vec(tex_fifo);
	io.section("end");
}

namespace {

struct StateHeader { uint32_t magic, version, id, epoch, gen, zero; };
static_assert(sizeof(StateHeader) == Model3::STATE_HEADER);

const char *const PAGED_NAMES[] = { "tile ram", "culling ram", "culling ram (high)", "polygon ram", "texture ram" };

} // namespace

void Model3::setup_pages()
{
	uint8_t *base[PAGED] = { tile_ram.data(), reinterpret_cast<uint8_t *>(cull_lo.data()), reinterpret_cast<uint8_t *>(cull_hi.data()),
		reinterpret_cast<uint8_t *>(poly_ram.data()), reinterpret_cast<uint8_t *>(tex_ram.data()) };
	const size_t bytes[PAGED] = { tile_ram.size(), cull_lo.size() * 4, cull_hi.size() * 4, poly_ram.size() * 4, tex_ram.size() * 2 };
	for (int r = 0; r < PAGED; r++) {
		paged[r].base = base[r];
		paged[r].bytes = bytes[r];
		paged[r].gen.assign((bytes[r] + PAGE - 1) / PAGE, 0);
	}
	std::random_device rd;
	snap_id = rd() ^ uint32_t(uintptr_t(this));
	snap_epoch++;
}

size_t Model3::fixed_state_size() const
{
	size_t n = STATE_HEADER + ram.size() + backup.size() + security.size();
	for (const Paged &r : paged) n += r.bytes;
	return n;
}

// The state: the header (for this board's saves only: which board, since which reset, which save), the memories at
// fixed places, then the rest. A save into a buffer this board saved before, since its last reset, copies only the
// device memories' pages written since (the RAM, which the recompiled code writes directly, always whole).
void Model3::save_state(std::vector<uint8_t> &out, std::vector<StateSection> *sections)
{
	if (sections) sections->clear();
	const size_t fixed = fixed_state_size();
	StateHeader h{};
	if (out.size() >= fixed) memcpy(&h, out.data(), sizeof h);
	const bool since = out.size() >= fixed && h.magic == STATE_MAGIC && h.version == STATE_VERSION && h.id == snap_id && h.epoch == snap_epoch;
	const uint32_t prev = since ? h.gen : 0;
	out.resize(fixed);   // (no allocation once the buffer has had a state)
	h = { STATE_MAGIC, STATE_VERSION, snap_id, snap_epoch, snap_gen, 0 };
	memcpy(out.data(), &h, sizeof h);
	size_t at = STATE_HEADER;
	auto whole = [&](const char *name, const void *src, size_t n) {
		if (sections) sections->push_back({ name, at });
		memcpy(out.data() + at, src, n);
		at += n;
	};
	whole("ram", ram.data(), ram.size());
	whole("backup", backup.data(), backup.size());
	whole("security", security.data(), security.size());
	for (int r = 0; r < PAGED; r++) {
		const Paged &g = paged[r];
		if (sections) sections->push_back({ PAGED_NAMES[r], at });
		if (!since) memcpy(out.data() + at, g.base, g.bytes);
		else
			for (size_t q = 0; q < g.gen.size(); q++)
				if (g.gen[q] > prev) memcpy(out.data() + at + q * PAGE, g.base + q * PAGE, std::min(PAGE, g.bytes - q * PAGE));
		at += g.bytes;
	}
	StateWriter w(out, sections, true);
	state(w);
	snap_gen++;   // (writes from now on are after this save)
}

bool Model3::load_state(const uint8_t *p, size_t n)
{
	const size_t fixed = fixed_state_size();
	StateHeader h{};
	if (n < fixed) return false;
	memcpy(&h, p, sizeof h);
	if (h.magic != STATE_MAGIC || h.version != STATE_VERSION) return false;
	// a state this board saved since its last reset: only the pages written since it was saved differ
	const bool since = h.id == snap_id && h.epoch == snap_epoch;
	size_t at = STATE_HEADER;
	auto whole = [&](void *dst, size_t m) { memcpy(dst, p + at, m); at += m; };
	whole(ram.data(), ram.size());
	whole(backup.data(), backup.size());
	whole(security.data(), security.size());
	textures_loaded = false;
	for (int r = 0; r < PAGED; r++) {
		Paged &g = paged[r];
		for (size_t q = 0; q < g.gen.size(); q++) {
			const size_t len = std::min(PAGE, g.bytes - q * PAGE);
			uint8_t *dst = g.base + q * PAGE;
			const uint8_t *src = p + at + q * PAGE;
			if (since ? g.gen[q] <= h.gen : !memcmp(dst, src, len)) continue;
			memcpy(dst, src, len);
			g.gen[q] = snap_gen;   // (changed now: the next save into any buffer copies it)
			if (r == PAGED - 1) {   // the texture RAM: a page a row, given to the renderers as changed
				const unsigned y = unsigned(q);
				if (!tex_uploads.empty() && tex_uploads.back().x == 0 && tex_uploads.back().w == 2048 && tex_uploads.back().y + tex_uploads.back().h == y)
					tex_uploads.back().h++;   // (joined to the row above)
				else tex_uploads.push_back({ 0, y, 2048, 1 });
				textures_loaded = true;
			}
		}
		at += g.bytes;
	}
	StateReader rd(p + fixed, n - fixed);
	state(rd);
	// what isn't kept but follows from what is
	set_crom_bank(crom_bank_reg);
	if (cpu.rec)
		cpu.rec->recount();
	if (textures_loaded) tex_serial++;   // (the renderers: what changed is in tex_uploads)
	return rd.done();
}

std::vector<uint32_t> Model3::program_image() const
{
	// The boot copies the program from the fixed CROM to RAM 0 (Revision D's: 0xA0000 bytes from 0x710000,
	// docs/step1-measure.md)
	std::vector<uint32_t> img(profile->program_size / 4);
	memcpy(img.data(), &crom[profile->program_at], profile->program_size);
	return img;
}

uint64_t Model3::ram_hash() const
{
	uint64_t h = 1469598103934665603ull;
	const uint32_t *w = reinterpret_cast<const uint32_t *>(ram.data());
	for (size_t i = 0; i < ram.size() / 4; i++)
		h = (h ^ w[i]) * 1099511628211ull;
	return h;
}

// ---- PCI bridge (MPC105) ----

void Model3::pci_reset()
{
	memset(pci_regs, 0, sizeof(pci_regs));
	// The reference stores these as little-endian words and reads them back big-endian; kept as it does
	auto le = [this](unsigned at, uint32_t v) { for (int i = 0; i < 4; i++) pci_regs[at + i] = uint8_t(v >> (8 * i)); };
	pci_regs[0] = 0x57; pci_regs[1] = 0x10;  // vendor 0x1057 (Motorola)
	pci_regs[2] = 0x01; pci_regs[3] = 0x00;  // device 0x0001 (MPC105)
	le(0x04, 0x00800006);
	le(0x08, 0x00060000);
	le(0xA8, 0x0010FF00);
	le(0xAC, 0x060C000C);
	le(0xB8, 0x04000000);
	le(0xF0, 0x0000FF02);
	le(0xF4, 0x00030000);
	le(0xFC, 0x00000010);
	pci_bus = pci_device = pci_function = pci_reg = 0;
}

void Model3::pci_config_address(uint32_t data)
{
	uint32_t d = bswap32(data);
	pci_bus = (d >> 16) & 0xFF;
	pci_device = (d >> 11) & 0x1F;
	pci_function = (d >> 8) & 7;
	pci_reg = d & 0xFF;
}

uint32_t Model3::pci_config_read(unsigned bits, unsigned offset)
{
	if (pci_device == 0) {
		const uint8_t *r = &pci_regs[(pci_reg + offset) & 0xFF];
		switch (bits) {
		case 8: return r[0];
		case 16: return uint32_t(r[0] << 8 | r[1]);
		default: return uint32_t(r[0]) << 24 | uint32_t(r[1]) << 16 | uint32_t(r[2]) << 8 | r[3];
		}
	}
	unsigned reg = (pci_reg >> 2) & 0x3C;
	if (bits == 8)
		return 0;
	uint32_t id;
	switch (pci_device) {
	case 13: id = PCI_ID_REAL3D; break;
	case 14: id = PCI_ID_53C810; break;
	case 16: return bits == 32 && reg == 0 ? 0x182711DB : 0;
	default: return 0;
	}
	if (reg != 0)
		return 0;
	uint32_t d = bswap32(id);
	if (bits == 16)
		d = (d >> ((2 - offset) * 8)) & 0xFFFF;
	return d;
}

void Model3::pci_config_write(unsigned bits, unsigned offset, uint32_t data)
{
	if (pci_device == 0) {
		uint8_t *r = &pci_regs[(pci_reg + offset) & 0xFF];
		switch (bits) {
		case 8: r[0] = uint8_t(data); break;
		case 16: r[0] = uint8_t(data >> 8); r[1] = uint8_t(data); break;
		default: r[0] = uint8_t(data >> 24); r[1] = uint8_t(data >> 16); r[2] = uint8_t(data >> 8); r[3] = uint8_t(data); break;
		}
		return;
	}
	unsigned reg = (pci_reg >> 2) & 0x3C;
	if (pci_device == 14 && reg == 4)
		scsi.base = uint8_t(data);  // where the 53C810's registers appear (0xC0 for 0xC0000000)
}

// ---- inputs, system registers, RTC, security ----

uint8_t Model3::read_inputs(unsigned reg)
{
	const Controls &c = controls;
	uint8_t d;
	switch (reg & 0x3F) {
	case 0x00: return input_bank;
	case 0x04:
		d = 0xFF;
		if (!(input_bank & 1)) {
			d &= ~(c.coin[0] << 0); d &= ~(c.coin[1] << 1); d &= ~(c.test[0] << 2);
			d &= ~(c.service[0] << 3); d &= ~(c.start[0] << 4); d &= ~(c.start[1] << 5);
		} else {
			d &= ~(c.service[1] << 6); d &= ~(c.test[1] << 7);
			d = uint8_t((d & 0xDF) | (eeprom.read() << 5));
		}
		return d;
	case 0x08: case 0x0C: {
		int p = (reg & 0x3F) == 0x0C;
		d = 0xFF;
		d &= ~(c.up[p] << 5); d &= ~(c.down[p] << 4); d &= ~(c.left[p] << 7); d &= ~(c.right[p] << 6);
		d &= ~(c.escape[p] << 3); d &= ~(c.guard[p] << 2); d &= ~(c.kick[p] << 1); d &= ~(c.punch[p] << 0);
		return d;
	}
	case 0x10: return outputs[0];
	case 0x14: return outputs[1];
	case 0x18: return 0x7F;
	case 0x2C: return serial1;
	case 0x30: return serial2;
	case 0x34: return 0;
	case 0x3C: adc_channel++; return 0;  // no analogue inputs on VF3
	default: return 0xFF;
	}
}

void Model3::write_inputs(unsigned reg, uint8_t data)
{
	switch (reg & 0x3F) {
	case 0x00:
		eeprom.write((data >> 6) & 1, (data >> 7) & 1, (data >> 5) & 1);
		input_bank = data;
		break;
	case 0x10: outputs[0] = data; break;  // the drive board (VF3's billboard)
	case 0x14: outputs[1] = data; break;  // lamps
	case 0x24:                            // serial FIFO commands (light guns; none on VF3)
		if (data == 0x00) gun_reg = serial2;
		else if (data == 0x87) serial1 = serial2 = 0;
		break;
	case 0x28: serial2 = data; break;
	case 0x3C: adc_channel = data & 7; break;
	default: break;
	}
}

uint8_t Model3::read_sysreg(unsigned reg)
{
	switch (reg & 0x3F) {
	case 0x08: return crom_bank_reg;
	case 0x14: return uint8_t(irq.enable);
	case 0x18: return uint8_t(irq.state);
	case 0x1C: return 0xFF;
	case 0x10: return uint8_t(jtag.read() << 5);
	default: return 0xFF;
	}
}

void Model3::write_sysreg(unsigned reg, uint8_t data)
{
	switch (reg & 0x3F) {
	case 0x08: set_crom_bank(data); break;
	case 0x14: irq.enable = data; break;
	case 0x18: irq.deassert(data); break;
	case 0x0C: jtag.write((data >> 6) & 1, (data >> 2) & 1, (data >> 5) & 1, (data >> 7) & 1); break;
	default: break;
	}
}

uint8_t Model3::read_rtc(unsigned reg)
{
	// The host's local time, as the reference gives it (VF3 reads only registers 13 and 15, which read 0)
	time_t now = rtc_time >= 0 ? time_t(rtc_time) : time(nullptr);
	struct tm t;
#ifdef _WIN32
	if (rtc_time >= 0) gmtime_s(&t, &now);
	else localtime_s(&t, &now);
#else
	if (rtc_time >= 0) gmtime_r(&now, &t);
	else localtime_r(&now, &t);
#endif
	switch (reg & 0xF) {
	case 0: return (t.tm_sec % 10) & 0xF;
	case 1: return (t.tm_sec / 10) & 0x7;
	case 2: return (t.tm_min % 10) & 0xF;
	case 3: return (t.tm_min / 10) & 0x7;
	case 4: return (t.tm_hour % 10) & 0xF;
	case 5: return (t.tm_hour / 10) & 0x7;
	case 6: return (t.tm_mday % 10) & 0xF;
	case 7: return (t.tm_mday / 10) & 0x3;
	case 8: return ((t.tm_mon + 1) % 10) & 0xF;
	case 9: return ((t.tm_mon + 1) / 10) & 0x1;
	case 10: return (t.tm_year % 10) & 0xF;
	case 11: return ((t.tm_year % 100) / 10) & 0xF;
	case 12: return t.tm_wday & 0x7;
	default: return 0;
	}
}

uint32_t Model3::read_security(unsigned reg)
{
	// VF3 has no security board; the reference's answers for an unkeyed one
	switch (reg) {
	case 0x00: return 0;
	case 0x1C: return 0xFFFF0000;
	default: return 0xFFFFFFFF;
	}
}

bool Model3::scsi_mapped(uint32_t) const { return scsi.base == 0xC0; }

// ---- Real3D and tile generator ----

uint32_t Model3::r3d_register(unsigned reg) const
{
	if (reg == 0)
		return 0xFDFFFFFF | (ping_pong ? 0x02000000 : 0);
	if (reg >= 20 && reg <= 32)
		return 0;  // the line-of-sight values, from the renderer (0 until there is one)
	return 0xFFFFFFFF;
}

void Model3::r3d_flush()
{
	flushes++;
	command_written = true;
	if (ping_pong == ping_pong_copy)
		r3d_flush_textures();
}

// The texture FIFO: entries of a length word (in 16-bit units, less 2... as the reference reads it), a header and
// the texels
void Model3::r3d_flush_textures()
{
	size_t n = tex_fifo.size();
	if (n > 2) {
		for (size_t i = 0; i < n - 2;) {
			uint32_t size = (2 + tex_fifo[i] / 2) / 4;
			if (!size)
				break;
			const uint16_t *data = reinterpret_cast<const uint16_t *>(&tex_fifo[i + 2]);
			r3d_upload(tex_fifo[i + 1], data, (n - i - 2) * 2);
			i += size;
		}
	}
	tex_fifo.clear();
}

namespace {
// The texel order within an 8x8, 8x4 and 8x2 tile as the data arrives
const uint8_t decode8x8[64] = { 1, 0, 5, 4, 9, 8, 13, 12, 3, 2, 7, 6, 11, 10, 15, 14, 17, 16, 21, 20, 25, 24, 29, 28,
	19, 18, 23, 22, 27, 26, 31, 30, 33, 32, 37, 36, 41, 40, 45, 44, 35, 34, 39, 38, 43, 42, 47, 46, 49, 48, 53, 52,
	57, 56, 61, 60, 51, 50, 55, 54, 59, 58, 63, 62 };
const uint8_t decode8x4[32] = { 1, 0, 5, 4, 3, 2, 7, 6, 9, 8, 13, 12, 11, 10, 15, 14, 17, 16, 21, 20, 19, 18, 23, 22,
	25, 24, 29, 28, 27, 26, 31, 30 };
const uint8_t decode8x2[16] = { 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14 };
const int mip_x[] = { 0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047 };
const int mip_y[] = { 0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023 };
} // namespace

void Model3::r3d_store(unsigned x0, unsigned y0, unsigned w, unsigned h, const uint16_t *&data, const uint16_t *end,
	bool sixteen, bool lsb, bool msb)
{
	const unsigned tx = std::min(8u, w), ty = std::min(8u, h);
	const uint8_t *dec = tx == 8 ? decode8x8 : tx == 4 ? decode8x4 : decode8x2;
	auto at = [&](size_t i) -> uint16_t { return data + i < end ? data[i] : 0; };
	if (tx < 2) return;
	if (sixteen) {
		for (unsigned y = y0; y < y0 + h; y += ty)
			for (unsigned x = x0; x < x0 + w; x += tx) {
				for (unsigned yy = 0; yy < ty; yy++)
					for (unsigned xx = 0; xx < tx; xx++)
						tex_ram[((y + yy) & 2047) * 2048 + ((x + xx) & 2047)] = at(dec[yy * tx + xx]);
				data += tx * ty;
			}
	} else {
		// 8-bit texels go to the low or high byte of the 16-bit texel (the rows of the order swapped)
		static const uint16_t keep[4] = { 0xFFFF, 0xFF00, 0x00FF, 0x0000 };
		const unsigned sel = unsigned(lsb) | (unsigned(msb) << 1);
		const unsigned step = std::max(1u, tx * ty / 2);
		for (unsigned y = y0; y < y0 + h; y += ty)
			for (unsigned x = x0; x < x0 + w; x += tx) {
				if (sel)
					for (unsigned yy = 0; yy < ty; yy++)
						for (unsigned xx = 0; xx < tx; xx++) {
							uint16_t &t = tex_ram[((y + yy) & 2047) * 2048 + ((x + xx) & 2047)];
							unsigned shift = 8 * ((xx & 1) ^ 1);
							unsigned idx = (yy ^ 1) * tx + (xx ^ 1) - (tx & 1);
							uint16_t v = (at(dec[idx] / 2) >> shift) & 0xFF;
							v |= v << 8;
							t = (t & keep[sel]) | (v & (keep[sel] ^ 0xFFFF));
						}
				data += step;
			}
	}
	for (unsigned y = y0; y < y0 + h + ty; y++) touch(4, size_t(y & 2047) * 4096);   // (its rows, a page each)
	tex_uploads.push_back({ x0, y0, w, h });
	tex_serial++;
}

// Texture header: X 32*(h&0x3F), Y 32*((h>>7)&0x1F), width 32<<((h>>14)&7), height 32<<((h>>17)&7), sheet bit 20,
// 8-bit data to the low byte (21) or high byte (22), 16-bit (23), type (h>>24): 0 with mipmaps, 1 without, 2 only
// mipmaps (0x80: not a texture)
void Model3::r3d_upload(uint32_t header, const uint16_t *data, size_t avail)
{
	const uint16_t *end = data + avail;
	unsigned x = 32 * (header & 0x3F), y = 32 * ((header >> 7) & 0x1F);
	unsigned page = (header >> 20) & 1;
	unsigned w = 32u << ((header >> 14) & 7), h = 32u << ((header >> 17) & 7);
	unsigned type = header >> 24;
	bool sixteen = (header >> 23) & 1, msb = (header >> 22) & 1, lsb = (header >> 21) & 1;
	if (type > 2)
		return;
	if (type != 2)
		r3d_store(x, y + page * 1024, w, h, data, end, sixteen, lsb, msb);
	if (type == 1)
		return;
	for (int i = 1; w >= 4 && h >= 4; i++) {
		unsigned mx = mip_x[i] + (x >> i), my = mip_y[i] + (y >> i);
		w /= 2;
		h /= 2;
		r3d_store(mx, my + page * 1024, w, h, data, end, sixteen, lsb, msb);
	}
}

void Model3::r3d_texture_port(unsigned, uint32_t data)
{
	// Step 1.0: the data is a VROM word address; the words there go into the texture FIFO
	const uint32_t *v = reinterpret_cast<const uint32_t *>(vrom.data());
	uint32_t addr = data & 0xFFFFFF;
	uint32_t n = (2 + v[addr] / 2) / 4;
	for (uint32_t i = 0; i < n; i++)
		if (tex_fifo.size() < 0x100000 / 4)
			tex_fifo.push_back(v[(addr + i) & 0xFFFFFF]);
}

uint32_t Model3::r3d_dma_read32(unsigned reg) const { return reg == 0x14 ? dma_data : 0; }

void Model3::r3d_dma_write32(unsigned reg, uint32_t data)
{
	switch (reg) {
	case 0x00: dma_src = data; break;
	case 0x04: dma_dst = data; break;
	case 0x08:
		dma_len = data;
		while (dma_len) {
			uint32_t v = read32(dma_src);
			write32(dma_dst, (dma_config & 0x80) ? bswap32(v) : v);
			dma_src += 4;
			dma_dst += 4;
			--dma_len;
		}
		if (dma_config & 1) {
			dma_status |= 1;
			irq.assert_(IRQ_R3D_DMA);
		}
		break;
	case 0x10:
		if (data & 0x20000000) dma_data = PCI_ID_REAL3D;
		else if (data & 0x80000000) dma_data = r3d_register(data & 0x3F);
		break;
	case 0x14: dma_data = 0xFFFFFFFF; break;
	default: break;
	}
}

uint16_t Model3::peek16(uint32_t a) const
{
	if (a < 0x00800000) return *reinterpret_cast<const uint16_t *>(&ram[(a & 0x7FFFFE) ^ 2]);
	if ((a >> 24) == 0xFF) return *reinterpret_cast<const uint16_t *>(&(a < 0xFF800000 ? crom_bank : crom.data())[(a & 0x7FFFFE) ^ 2]);
	return 0;
}

uint32_t Model3::peek32(uint32_t a) const
{
	return uint32_t(peek16(a)) << 16 | peek16(a + 2);
}

void Model3::tile_ram_write32(unsigned addr, uint32_t data)
{
	word(tile_ram, addr) = data;   // the palette (addresses from 0x100000) is decoded by the renderer
	touch(0, addr);
}

void Model3::tile_reg_write(unsigned reg, uint32_t data)
{
	reg &= 0xFF;
	if (reg == 0x10)
		irq.deassert(data & 0xFF);
	tile_regs[reg / 4] = data;
}

// ---- the PowerPC's address space ----

uint8_t Model3::read8(uint32_t a)
{
	if (a < 0x00800000)
		return ram[a ^ 3];
	switch (a >> 24) {
	case 0xFF: return (a < 0xFF800000 ? crom_bank : crom.data())[(a & 0x7FFFFF) ^ 3];
	case 0xC2:
		switch (a & 0xFF) {
		case 0xC: return dma_status;
		case 0xE: return dma_config;
		default: return 0;
		}
	case 0xF0: case 0xFE:
		switch ((a >> 16) & 0xFF) {
		case 0x04: return read_inputs(a & 0x3F);
		case 0x08: return (a & 0xF) == 4 ? 0x83 : 0;   // the MIDI ports read as the reference returns them
		case 0x10: return read_sysreg(a & 0x3F);
		case 0x14:
			if ((a & 3) == 1) return 0x03;               // battery voltage
			if ((a & 3) == 0) return read_rtc((a >> 2) & 0xF);
			return 0;
		default: break;
		}
		break;
	case 0xF1:
		if (a < 0xF1120000) return tile_ram[a & 0x1FFFFF];
		break;
	case 0xC0:
		if (!scsi_mapped(a)) break;
		[[fallthrough]];
	case 0xF9: case 0xC1:
		return scsi.read(a & 0xFF);
	default: break;
	}
	return 0xFF;
}

uint16_t Model3::read16(uint32_t a)
{
	if (a & 1)
		return uint16_t(read8(a) << 8 | read8(a + 1));
	if (a < 0x00800000)
		return half(ram, a ^ 2);
	switch (a >> 24) {
	case 0xFF: return *reinterpret_cast<const uint16_t *>(&(a < 0xFF800000 ? crom_bank : crom.data())[(a & 0x7FFFFF) ^ 2]);
	case 0xF0: case 0xFE:
		switch ((a >> 16) & 0xFF) {
		case 0x0C: case 0x0D: return half(backup, (a & 0x1FFFF) ^ 2);
		case 0xC0: return uint16_t(pci_config_read(16, a & 3));
		default: break;
		}
		break;
	case 0xF1:
		if (a < 0xF1120000) return bswap16(half(tile_ram, a & 0x1FFFFF));
		break;
	default: break;  // 0xC0: the net board's RAM on later steppings (not on VF3's)
	}
	return 0xFFFF;
}

uint32_t Model3::read32(uint32_t a)
{
	if (a & 3)
		return uint32_t(read16(a)) << 16 | read16(a + 2);
	if (a < 0x00800000)
		return word(ram, a);
	switch (a >> 24) {
	case 0xFF: return *reinterpret_cast<const uint32_t *>(&(a < 0xFF800000 ? crom_bank : crom.data())[a & 0x7FFFFF]);
	case 0x84: return bswap32(r3d_register(a & 0x3F));
	case 0xC2: return bswap32(r3d_dma_read32(a & 0xFF));
	case 0xF0: case 0xFE:
		switch ((a >> 16) & 0xFF) {
		case 0x04:
			return uint32_t(read_inputs((a & 0x3F) + 0)) << 24 | uint32_t(read_inputs((a & 0x3F) + 1)) << 16 |
			       uint32_t(read_inputs((a & 0x3F) + 2)) << 8 | read_inputs((a & 0x3F) + 3);
		case 0x0C: case 0x0D: return word(backup, a & 0x1FFFF);
		case 0x10:
			return uint32_t(read_sysreg((a & 0x3F) + 0)) << 24 | uint32_t(read_sysreg((a & 0x3F) + 1)) << 16 |
			       uint32_t(read_sysreg((a & 0x3F) + 2)) << 8 | read_sysreg((a & 0x3F) + 3);
		case 0xC0: return pci_config_read(32, 0);
		case 0x14: return uint32_t(read_rtc((a >> 2) & 0xF)) << 24 | 0x00030000;
		case 0x18: case 0x19: return word(security, a & 0x1FFFF);
		case 0x1A: return read_security(a & 0x3F);
		default: break;
		}
		break;
	case 0xF1:
		if (a < 0xF1120000) return bswap32(word(tile_ram, a & 0x1FFFFF));
		if (a >= 0xF1180000 && a < 0xF1180100) return bswap32(tile_regs[(a & 0xFF) / 4]);
		break;
	case 0xC0:
		if (!scsi_mapped(a)) break;
		[[fallthrough]];
	case 0xF9: case 0xC1:
		return uint32_t(scsi.read((a + 0) & 0xFF)) << 24 | uint32_t(scsi.read((a + 1) & 0xFF)) << 16 |
		       uint32_t(scsi.read((a + 2) & 0xFF)) << 8 | scsi.read((a + 3) & 0xFF);
	default: break;
	}
	return 0xFFFFFFFF;
}

uint64_t Model3::read64(uint32_t a)
{
	uint64_t hi = read32(a);
	return hi << 32 | read32(a + 4);
}

void Model3::write8(uint32_t a, uint8_t d)
{
	if (a < 0x00800000) {
		ram[a ^ 3] = d;
		cpu.ram_written(a & ~3u);
		return;
	}
	switch (a >> 24) {
	case 0xC2:
		if ((a & 0xFF) == 0xD && (d & 1)) {
			dma_status &= ~1;
			irq.deassert(IRQ_R3D_DMA);
		} else if ((a & 0xFF) == 0xE)
			dma_config = d;
		break;
	case 0xF0: case 0xFE:
		switch ((a >> 16) & 0xFF) {
		case 0x04: write_inputs(a & 0x3F, d); break;
		case 0x08:
			if ((a & 0xF) == 0) {         // MIDI data
				midi.push_back(d);
				irq.deassert(0x40);
			} else if ((a & 0xF) == 4) {  // MIDI control
				midi_ctrl = d;
				if (!(d & 0x20))
					irq.deassert(0x40);
			}
			break;
		case 0x0C: case 0x0D: backup[(a & 0x1FFFF) ^ 3] = d; break;
		case 0x10: write_sysreg(a & 0x3F, d); break;
		default: break;
		}
		break;
	case 0xF1:
		if (a < 0xF1120000) {
			uint32_t at = a & 0x1FFFFF;
			uint32_t w = word(tile_ram, at & ~3u);
			uint32_t sh = (at & 3) * 8;
			tile_ram_write32(at & ~3u, (w & ~(0xFFu << sh)) | (uint32_t(d) << sh));
		}
		break;
	case 0xF8: pci_regs[a & 0xFF] = d; break;
	case 0xC0:
		if (!scsi_mapped(a)) break;
		[[fallthrough]];
	case 0xF9: case 0xC1:
		scsi.write(a & 0xFF, d);
		break;
	default: break;
	}
}

void Model3::write16(uint32_t a, uint16_t d)
{
	if (a & 1) {
		write8(a, uint8_t(d >> 8));
		write8(a + 1, uint8_t(d));
		return;
	}
	if (a < 0x00800000) {
		half(ram, a ^ 2) = d;
		cpu.ram_written(a & ~3u);
		return;
	}
	switch (a >> 24) {
	case 0xF0: case 0xFE:
		switch ((a >> 16) & 0xFF) {
		case 0x0C: case 0x0D: half(backup, (a & 0x1FFFF) ^ 2) = d; break;
		case 0xC0: pci_config_write(16, a & 2, d); break;
		default: break;
		}
		break;
	case 0xF1:
		if (a < 0xF1120000) {
			uint32_t at = a & 0x1FFFFF;
			tile_ram[at] = uint8_t(d >> 8);
			tile_ram[at + 1] = uint8_t(d);
			touch(0, at);
		}
		break;
	case 0xF8:
		pci_regs[a & 0xFF] = uint8_t(d >> 8);
		pci_regs[(a + 1) & 0xFF] = uint8_t(d);
		break;
	default: break;
	}
}

void Model3::write32(uint32_t a, uint32_t d)
{
	if (a & 3) {
		write16(a, uint16_t(d >> 16));
		write16(a + 2, uint16_t(d));
		return;
	}
	if (a < 0x00800000) {
		word(ram, a) = d;
		cpu.ram_written(a);
		return;
	}
	switch (a >> 24) {
	case 0x88: r3d_flush(); break;
	case 0x8C: cull_lo[(a & 0x3FFFFF) / 4] = bswap32(d); touch(1, a & 0x3FFFFC); break;
	case 0x8E:
		if (ping_pong != ping_pong_copy) pending_hi.push_back({ (a & 0xFFFFF) / 4, bswap32(d) });
		else { cull_hi[(a & 0xFFFFF) / 4] = bswap32(d); touch(2, a & 0xFFFFC); }
		break;
	case 0x90: r3d_texture_port(a & 0xFF, bswap32(d)); break;
	case 0x94: if (tex_fifo.size() < 0x100000 / 4) tex_fifo.push_back(bswap32(d)); break;
	case 0x98:
		if (ping_pong != ping_pong_copy) pending_poly.push_back({ (a & 0x3FFFFF) / 4, bswap32(d) });
		else { poly_ram[(a & 0x3FFFFF) / 4] = bswap32(d); touch(3, a & 0x3FFFFC); }
		break;
	case 0x9C: r3d_config[(a & 0xF) / 4] = bswap32(d); break;
	case 0xC2: r3d_dma_write32(a & 0xFF, bswap32(d)); break;
	case 0xF0: case 0xFE: {
		unsigned sub = (a >> 16) & 0xFF;
		switch (sub) {
		case 0x04:
			for (int i = 0; i < 4; i++) write_inputs((a & 0x3F) + i, uint8_t(d >> (24 - 8 * i)));
			break;
		case 0x0C: case 0x0D: word(backup, a & 0x1FFFF) = d; break;
		case 0x80: pci_config_address(d); break;
		case 0x10:
			for (int i = 0; i < 4; i++) write_sysreg((a & 0x3F) + i, uint8_t(d >> (24 - 8 * i)));
			break;
		case 0x18: case 0x19: word(security, a & 0x1FFFF) = d; break;
		default:
			if (sub >= 0xC0 && sub <= 0xEF && a >= 0xF0C00CF8 && a < 0xF0C00D00)
				pci_config_write(32, 0, d);
			break;
		}
		break;
	}
	case 0xF1:
		if (a < 0xF1120000)
			tile_ram_write32(a & 0x1FFFFF, bswap32(d));
		else if (a >= 0xF1180000 && a < 0xF1180100)
			tile_reg_write(a & 0xFF, bswap32(d));
		break;
	case 0xF8:
		for (int i = 0; i < 4; i++) pci_regs[((a & 0xFF) + i) & 0xFF] = uint8_t(d >> (24 - 8 * i));
		break;
	case 0xC0:
		if (!scsi_mapped(a)) break;
		[[fallthrough]];
	case 0xF9: case 0xC1:
		for (int i = 0; i < 4; i++) scsi.write(((a & 0xFF) + i) & 0xFF, uint8_t(d >> (24 - 8 * i)));
		break;
	default: break;
	}
}

void Model3::write64(uint32_t a, uint64_t d)
{
	write32(a, uint32_t(d >> 32));
	write32(a + 4, uint32_t(d));
}

// The FREE PLAY notices, in the fixed CROM the boot copies the program from (the program image's address + the
// profile's program_at; the CROM holds the PowerPC's 32-bit words, so byte a of the image is crom[(a + at) ^ 3]).
// Revision D's (the profile's free_play_text):
// - the fight's top line (alternating with PRESS START BUTTON): "     FREE PLAY       " (21 characters) at the
//   image's 0x7CAE0, between "PRESS START BUTTON   " and "CREDIT": spaces;
// - the attract's, bottom left: picture 0x4018 (its number from a table at 0x7CB50), drawn by the call at 0x4E9C0
//   (bl 0x33300, the routine that draws a picture into a text window): a no-op instead.
int Model3::hide_free_play_text()
{
	if (!free_play_text.empty()) return int(free_play_text.size());   // (hidden already)
	const size_t CROM_AT = profile->program_at;
	const Profile::FreePlayText &t = profile->free_play_text;
	if (!t.free_play) return 0;   // (not known for this program)
	auto at = [&](size_t a) -> uint8_t & { return crom[(a + CROM_AT) ^ 3]; };
	auto is = [&](size_t a, const char *t) {
		for (size_t k = 0; t[k]; k++)
			if (a + CROM_AT + k >= crom.size() || at(a + k) != uint8_t(t[k])) return false;
		return true;
	};
	auto word = [&](size_t a) { return uint32_t(at(a)) << 24 | uint32_t(at(a + 1)) << 16 | uint32_t(at(a + 2)) << 8 | at(a + 3); };
	auto patch = [&](size_t a, const uint8_t *to, size_t n) {
		std::vector<uint8_t> was;
		for (size_t k = 0; k < n; k++) { was.push_back(at(a + k)); at(a + k) = to[k]; }
		free_play_text.push_back({ a, was });
	};
	if (!is(t.press_start, "PRESS START BUTTON   ") || !is(t.free_play, "     FREE PLAY       ") || !is(t.credit, "CREDIT") ||
		word(t.picture_table) != t.picture || word(t.draw_call) != t.draw_word)
		return 0;
	const uint8_t spaces[21] = { ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
	const uint8_t nop[4] = { 0x60, 0x00, 0x00, 0x00 };
	patch(t.free_play, spaces, sizeof spaces);
	patch(t.draw_call, nop, sizeof nop);
	return int(free_play_text.size());
}

// Revision D's, found for issue #12 (Dural playable):
// - the crash: the dojo's (stage 14, the byte at 0x10201C) extra model. The routine at 0x49878 takes a share of the
//   pool 0xA0000-0xE0000 for it (bl 0x3B25C: the old top, or -1 and cr0 "less" when it doesn't fit) and copies table
//   entry 0x1A90 (0xD7B8 bytes) there (bl 0x3B2E0) without looking. Akira vs Kage there leaves 0xCBB4 bytes free;
//   Dural's 17 extra pieces (0x44430) take 0xE334, so the share fails and the copy writes from 0xFFFFFFFF up, over
//   the program at RAM 0 (0440004B at 0x18BC, frame 3254 of the issue's run). Now the share is checked, in as many
//   instructions as before (the stack frame's two addi r1 become one, so the bl 0x3B2E0 stays at 0x498C8 and all the
//   routine leaves in memory and registers is as it was): after the share, blt to dead words after 0x49634's blr
//   (0x49648-0x49650, zeros nothing branches to), which set the object's state to the blr at 0x498FC (what every
//   other stage's object gets) and join the routine's end. Nothing is copied: Dural on the dojo goes without that
//   model (with Akira there it's never drawn either: the same pictures with it left out). Without Dural the share
//   never fails: the same state every frame as without the patch (vf3run --state, the program's words aside).
// - her portrait: the select's big portrait is a 3D head. On a cursor move 0x55E68 loads the list of models and
//   textures at 0x7DE92[fighter] (her ids 9 and 22) and draws what the tables at 0x77160 and 0x77404 name: her own
//   head, 0x0095. But her lists are copies of Shun's (0x7DB80, 0x7DDC4), so it isn't loaded (the cyan box). The pair
//   table's own entry for her (0x77184: 0x0095 then the lists' end) makes a list of one: her head shows. Player 2's
//   side shows her second costume (id 22), whose head is 0x10AF, the gold one (0x0095 there drew nothing): the
//   program's FFFF 10AF FFFF at 0x77608 holds the list [0x10AF] at 0x7760A. Both duller than in a fight: the fight
//   rebuilds the model's vertices and normals every frame (0x37C84-0x37E2C), the select draws the template as stored.
int Model3::set_dural(bool on)
{
	const size_t CROM_AT = profile->program_at;
	auto at = [&](size_t a) -> uint8_t & { return crom[(a + CROM_AT) ^ 3]; };
	auto word = [&](size_t a) { return uint32_t(at(a)) << 24 | uint32_t(at(a + 1)) << 16 | uint32_t(at(a + 2)) << 8 | at(a + 3); };
	auto put = [&](size_t a, uint32_t v) { for (int k = 0; k < 4; k++) at(a + k) = uint8_t(v >> (24 - 8 * k)); };
	if (!on) {
		for (auto it = dural_words.rbegin(); it != dural_words.rend(); ++it) put(it->first, it->second);
		dural_words.clear();
		return 0;
	}
	if (!dural_words.empty()) return int(dural_words.size());   // (on already)
	if (profile != &VF3_REV_D || crom.size() < CROM_AT + 0xA0000) return 0;
	static const uint32_t known[][2] = {   // (the words around the patches, to be sure it's this program)
		{ 0x49878, 0x886DA01C }, { 0x4987C, 0x2803000E }, { 0x49880, 0x41820014 }, { 0x49888, 0x300098FC },
		{ 0x49898, 0x7C0802A6 }, { 0x498C0, 0x7C641B78 }, { 0x498C8, 0x4BFF1A19 }, { 0x498D4, 0x900F0000 },
		{ 0x498DC, 0x30009A08 }, { 0x498E4, 0xBA010000 }, { 0x498E8, 0x38210040 }, { 0x498EC, 0x80010000 },
		{ 0x498FC, 0x4E800020 }, { 0x49A08, 0x4E800020 }, { 0x49634, 0x4E800020 }, { 0x49644, 0x4BFFFFF4 },
		{ 0x49654, 0 }, { 0x49660, 0x3821FFF0 }, { 0x3B294, 0x3860FFFF }, { 0x3B29C, 0x2C030000 },
		{ 0x7DEB2, 0x7DB80 }, { 0x7DEE6, 0x7DDC4 }, { 0x77184, 0x0095FFFF }, { 0x77608, 0xFFFF10AF },
		{ 0x7760C, 0xFFFF122A }, { 0x77414, 0x10880095 }, { 0x77430, 0x10AF122A },
	};
	static const uint32_t patch[][3] = {   // address, the word known, the new one
		{ 0x49894, 0x3821FFF0, 0x3821FFB0 },   // addi r1,r1,-0x10 -> addi r1,r1,-0x50
		{ 0x4989C, 0x90010000, 0x90010040 },   // stw r0,0(r1) -> stw r0,0x40(r1) (the same word)
		{ 0x498A0, 0x3821FFC0, 0xBE010000 },   // addi r1,r1,-0x40 -> stmw r16,0(r1) (from 0x498A4)
		{ 0x498A4, 0xBE010000, 0x7E138378 },   // mr r19,r16
		{ 0x498A8, 0x7E138378, 0x3A201A90 },   // li r17,0x1A90
		{ 0x498AC, 0x3A201A90, 0xB2330038 },   // sth r17,0x38(r19)
		{ 0x498B0, 0xB2330038, 0x7E238B78 },   // mr r3,r17
		{ 0x498B4, 0x7E238B78, 0x4BFF19A9 },   // bl 0x3B25C (the share)
		{ 0x498B8, 0x4BFF19A5, 0x90730058 },   // stw r3,0x58(r19)
		{ 0x498BC, 0x90730058, 0x4180FD8C },   // blt 0x49648 (new: it failed)
		{ 0x49648, 0x00000000, 0x3C000005 },   // lis r0,5
		{ 0x4964C, 0x00000000, 0x300098FC },   // addic r0,r0,-0x6704 (0x498FC, as at 0x49888)
		{ 0x49650, 0x00000000, 0x48000284 },   // b 0x498D4 (stw r0,0(r15) and the rest of the routine)
		{ 0x7DEB6, 0x7DB80, 0x77184 },         // her portrait's list (unaligned words: the table's)
		{ 0x7DEEA, 0x7DDC4, 0x7760A },         // and her second costume's: [0x10AF], her gold head (0x0095 drew nothing on the right)
	};
	for (auto &k : known)
		if (word(k[0]) != k[1]) return 0;
	for (auto &p : patch)
		if (word(p[0]) != p[1]) return 0;
	for (auto &p : patch) {
		dural_words.push_back({ p[0], p[1] });
		put(p[0], p[2]);
	}
	return int(dural_words.size());
}

void Model3::show_free_play_text()
{
	for (auto &[a, bytes] : free_play_text)
		for (size_t k = 0; k < bytes.size(); k++) crom[(a + profile->program_at + k) ^ 3] = bytes[k];
	free_play_text.clear();
}

} // namespace m3
