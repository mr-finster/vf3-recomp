// The Model 3 Step 1.0 board as Virtua Fighter 3 uses it: the PowerPC's memory map, the IRQ controller and system
// registers, the inputs and EEPROM, the JTAG chain, the MPC105 PCI bridge, the 53C810 (whose SCRIPTS processor
// carries display lists to the Real3D), the Real3D's and tile generator's registers and memories, and the frame
// timing.
//
// Written for this project. Every value the PowerPC can see follows the reference build (Supermodel), so a run
// here can be compared with it frame by frame; the comments say where the reference's behaviour is followed
// rather than known hardware behaviour.
#pragma once
#include "../ppc/ppc.h"
#include "profile.h"
#include "state_io.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace m3 {

// The switches, as the game reads them (true = pressed)
struct Controls
{
	bool coin[2] = {}, start[2] = {}, test[2] = {}, service[2] = {};
	bool up[2] = {}, down[2] = {}, left[2] = {}, right[2] = {};
	bool punch[2] = {}, kick[2] = {}, guard[2] = {}, escape[2] = {};
};

// The IRQ controller: eight maskable lines, and lines above bit 7 that aren't masked (the 53C810's)
class Irq
{
public:
	ppc::Cpu *cpu = nullptr;
	unsigned enable = 0, state = 0;
	void assert_(unsigned bits);
	void deassert(unsigned bits);
	void reset() { enable = 0; state = 0; }
	template <class IO> void save_state(IO &io) { io.pod(enable); io.pod(state); }
};

// The 93C46 serial EEPROM (64 words) behind input register 0
class Eeprom
{
public:
	uint16_t regs[64];
	Eeprom();
	void write(unsigned cs, unsigned clk, unsigned di);
	unsigned read();
	void reset();
	template <class IO> void save_state(IO &io)
	{
		io.pod(regs); io.pod(CS); io.pod(CLK); io.pod(DI); io.pod(DO); io.pod(out); io.pod(in);
		io.pod(bits_out); io.pod(receiving); io.pod(addr); io.pod(busy); io.pod(locked);
	}
private:
	unsigned CS = 0, CLK = 0, DI = 0, DO = 0;
	uint32_t out = 0, in = 0;
	int bits_out = 0;
	bool receiving = true;
	unsigned addr = 0;
	int busy = 0;
	bool locked = true;
};

// The JTAG chain to the Real3D's ASICs and 3D-RAMs (the game reads the ID codes)
class Jtag
{
public:
	void write(bool tck, bool tms, bool tdi, bool trst);
	bool read() const { return tdo; }
	void reset();
	uint32_t modeword[5] = {};  // per ASIC kind, for the renderer
	template <class IO> void save_state(IO &io)
	{
		for (Device &d : dev) {
			io.pod(d.asic); io.pod(d.kind); io.pod(d.id); io.pod(d.shift); io.pod(d.size); io.pod(d.ir); io.pod(d.mode);
		}
		io.pod(state); io.pod(last_tck); io.pod(tdo); io.pod(modeword);
	}
private:
	struct Device
	{
		bool asic = false;
		int kind = -1;          // which ASIC (0 Mercury .. 4 Jupiter), -1 for a dummy or a 3D-RAM
		uint32_t id = 0;
		uint64_t shift = 0;
		int size = 0;
		uint8_t ir = 0;
		uint32_t mode = 0;
	};
	Device dev[10];
	int state = 0;
	bool last_tck = false, tdo = false;
	void dev_reset(Device &d);
	void capture_dr(Device &d);
	void capture_ir(Device &d);
	void update_dr(Device &d);
public:
	Jtag();
};

class Model3;

// The NCR/LSI 53C810: the registers and the SCRIPTS memory-move and interrupt instructions
class Scsi
{
public:
	Model3 *board = nullptr;
	uint8_t regs[0x60];
	uint32_t temp = 0, dsp = 0, dsps = 0, dbc = 0;
	uint8_t dcmd = 0, dcntl = 0, dmode = 0, dstat = 0, dien = 0, istat = 0;
	bool halt = false;
	uint8_t base = 0;
	void reset();
	template <class IO> void save_state(IO &io)
	{
		io.pod(regs); io.pod(temp); io.pod(dsp); io.pod(dsps); io.pod(dbc);
		io.pod(dcmd); io.pod(dcntl); io.pod(dmode); io.pod(dstat); io.pod(dien); io.pod(istat); io.pod(halt); io.pod(base);
	}
	uint8_t read(unsigned reg);
	void write(unsigned reg, uint8_t data);
private:
	void run(bool single);
	bool exec();
	uint32_t fetch(uint32_t off);
};

class Model3 : public ppc::Bus
{
public:
	ppc::Cpu cpu;
	Irq irq;
	Eeprom eeprom;
	Jtag jtag;
	Scsi scsi;
	Controls controls;
	const Profile *profile = &VF3_REV_D;   // the program's (profile.h): set before load

	std::vector<uint8_t> ram;        // 8 MB, host-order words
	std::vector<uint8_t> crom;       // fixed CROM, 8 MB, host-order words
	std::vector<uint8_t> banked;     // banked CROM, 128 MB, host-order words
	std::vector<uint8_t> vrom;       // 64 MB
	std::vector<uint8_t> sound_rom, samples;   // the sound board's: 68000 program, samples (16-bit words, host order)
	std::vector<uint8_t> backup;     // 128 KB backup RAM, host-order words

	// The test menu's COIN ASSIGNMENTS > COIN/CREDIT SETTING, in backup RAM (found by setting each in the test menu,
	// leaving it, and comparing; Revision D's at 0x1E033, 0x1E037 and 0x1E03B: the profile's coin): the setting's
	// number (1-27) at `setting`; for #1-#26 the same at `credits` and 0 at `free_play`; #27, FREE PLAY, sets
	// `free_play` to 1 and leaves `credits`. No checksum. A board that has never run has 0 there until the game writes
	// its settings (by frame 120): -1, not written yet.
	static constexpr int COIN_FACTORY = 1, COIN_FREE_PLAY = 27;
	int coin_setting() const
	{
		const Profile::Coin &k = profile->coin;
		const int v = k.setting && backup.size() > k.free_play ? backup[k.setting] : 0;   // (0: not known for this program)
		return v >= 1 && v <= COIN_FREE_PLAY ? v : -1;
	}
	bool set_coin_setting(int v)   // (true if it changed)
	{
		if (coin_setting() < 0 || v < 1 || v > COIN_FREE_PLAY) return false;
		const Profile::Coin &k = profile->coin;
		const uint8_t was[3] = { backup[k.setting], backup[k.credits], backup[k.free_play] };
		backup[k.setting] = uint8_t(v);
		if (v == COIN_FREE_PLAY) backup[k.free_play] = 1;
		else backup[k.credits] = uint8_t(v), backup[k.free_play] = 0;
		return was[0] != backup[k.setting] || was[1] != backup[k.credits] || was[2] != backup[k.free_play];
	}
	bool free_play() const { return coin_setting() == COIN_FREE_PLAY && backup[profile->coin.free_play] == 1; }
	// Blanks the FREE PLAY notices (the PC way: always free play, nothing to say), in the fixed CROM the boot copies
	// the program from (so from the next boot). Checked against the known data around them first; returns how many
	// it changed (2; 0: not the data known). show_free_play_text puts them back.
	int hide_free_play_text();
	void show_free_play_text();
	std::vector<uint8_t> security;   // 128 KB, host-order words
	std::vector<std::pair<size_t, std::vector<uint8_t>>> free_play_text;   // what hide_free_play_text replaced

	// The tile generator: its RAM as the device stores it (little-endian words) and its registers
	std::vector<uint8_t> tile_ram;   // 0x120000
	uint32_t tile_regs[64] = {};

	// The Real3D: what the CPU and the 53C810 write, kept as the device sees it (little-endian words)
	static constexpr size_t CULL_PAD = 0x10000;   // zero words past each culling RAM's end (model3.cpp)
	std::vector<uint32_t> cull_lo, cull_hi, poly_ram, tex_fifo;
	uint32_t r3d_config[4] = {};
	uint32_t vrom_fifo[2] = {};
	int vrom_fifo_n = 0;
	bool ping_pong = false, ping_pong_copy = false;
	unsigned flushes = 0;            // writes to 0x88000000 this frame
	uint32_t dma_src = 0, dma_dst = 0, dma_len = 0, dma_data = 0;
	uint8_t dma_status = 0, dma_config = 0;
	// The texture RAM: two 2048x1024 sheets of 16-bit texels (sheet 1 from row 1024), filled from the texture FIFO
	std::vector<uint16_t> tex_ram;
	uint32_t tex_serial = 0;         // counts texture uploads (renderers keep their copies by it)
	struct TexUpload { unsigned x, y, w, h; };
	std::vector<TexUpload> tex_uploads;   // the areas written since a renderer last took them
	bool command_written = false, tilegen_draw = false;
	// Writes to the high culling RAM and polygon RAM after the ping-pong flip wait for the next vertical blank
	std::vector<std::pair<uint32_t, uint32_t>> pending_hi, pending_poly;

	// The MIDI bytes sent to the sound board (played by the sound board once it exists)
	std::deque<uint8_t> midi;
	uint8_t midi_ctrl = 0;

	uint64_t frame = 0;
	std::function<void()> after_slice;   // checking: called after every slice the CPU runs (vf3lock)
	std::function<void(unsigned)> on_line;  // the video: called at the start of each of the 384 active lines

	bool load(const std::string &zip, std::string &error);
	void reset();
	// A reset as from cold, as the board is just loaded: also what reset() leaves as it was (the security RAM, the
	// Real3D's flush count and VROM FIFO, the outputs, the MIDI bytes not taken). Netplay's start, so both sides
	// begin alike whatever each ran before.
	void power_on();
	void run_frame();
	// a frame in two halves (the host's smooth motion: each half in its own picture's time): the vertical blank and
	// lines 0-191, then lines 192-383
	void run_frame_start();
	void run_frame_end();
	void run_lines(unsigned from, unsigned to);
	uint32_t flip_line_ = 0;   // (read at the frame's start, as one loop did)

	// ppc::Bus
	uint8_t read8(uint32_t addr) override;
	uint16_t read16(uint32_t addr) override;
	uint32_t read32(uint32_t addr) override;
	uint64_t read64(uint32_t addr) override;
	void write8(uint32_t addr, uint8_t data) override;
	void write16(uint32_t addr, uint16_t data) override;
	void write32(uint32_t addr, uint32_t data) override;
	void write64(uint32_t addr, uint64_t data) override;

	// Save states (state_io.h): everything the game's run depends on, the PowerPC's state, the board's registers and
	// all its memories but the ROMs (about 27 MB; the sound board is apart: it only hears the MIDI bytes, and nothing
	// it does reaches this board). save_state reuses the buffer's memory; sections, if given, are filled with where
	// each part starts. load_state restores one this build saved, and false (the board then half restored: reset
	// it) if it isn't one. The renderers are told which textures changed, and the recompiled code rechecks RAM.
	void save_state(std::vector<uint8_t> &out, std::vector<StateSection> *sections = nullptr);
	bool load_state(const uint8_t *p, size_t n);
	static constexpr uint32_t STATE_MAGIC = 0x53334656, STATE_VERSION = 2;   // "VF3S"
	// The state's first bytes are for this board's saves alone (which board, since which reset, which save): two
	// boards in the same state have the same bytes after them, and only those are compared or hashed
	static constexpr size_t STATE_HEADER = 24;
	// The real-time clock: the host's local time, or (rtc_time >= 0: netplay, tests) this time, seconds since 1970,
	// as UTC, the same on every machine
	int64_t rtc_time = -1;

	uint64_t ram_hash() const;
	// RAM or CROM as the CPU reads it, without its side effects (0 elsewhere)
	uint16_t peek16(uint32_t addr) const;
	uint32_t peek32(uint32_t addr) const;
	std::vector<uint32_t> program_image() const;  // the program the boot copies to RAM 0, host-order words

private:
	uint8_t crom_bank_reg = 0xFF;
	const uint8_t *crom_bank = nullptr;
	uint8_t input_bank = 0, serial1 = 0, serial2 = 0, adc_channel = 0, gun_reg = 0;
	uint8_t outputs[2] = {};
	bool gpus_ready = false;

	// MPC105
	uint8_t pci_regs[256];
	uint32_t pci_bus = 0, pci_device = 0, pci_function = 0, pci_reg = 0;
	void pci_reset();
	void pci_config_address(uint32_t data);
	uint32_t pci_config_read(unsigned bits, unsigned offset);
	void pci_config_write(unsigned bits, unsigned offset, uint32_t data);

	void set_crom_bank(uint8_t v);
	uint8_t read_inputs(unsigned reg);
	void write_inputs(unsigned reg, uint8_t data);
	uint8_t read_sysreg(unsigned reg);
	void write_sysreg(unsigned reg, uint8_t data);
	uint8_t read_rtc(unsigned reg);
	uint32_t read_security(unsigned reg);
	bool scsi_mapped(uint32_t addr) const;

	template <class IO> void state(IO &io);
	bool textures_loaded = false;
	// Save states copy the device memories by the 4 KB pages written since (save_state): each page's save number
	// when last written. The RAM is copied whole (the recompiled code writes it directly).
	static constexpr int PAGED = 5;   // the tile RAM, the culling RAMs, the polygon RAM, the texture RAM
	static constexpr size_t PAGE = 4096;
	struct Paged { uint8_t *base = nullptr; size_t bytes = 0; std::vector<uint32_t> gen; };
	Paged paged[PAGED];
	uint32_t snap_id = 0, snap_epoch = 0, snap_gen = 1;
	void touch(int r, size_t byte) { paged[r].gen[byte / PAGE] = snap_gen; }
	void setup_pages();
	size_t fixed_state_size() const;

	uint32_t r3d_register(unsigned reg) const;
	void r3d_flush();
	void r3d_flush_textures();
	void r3d_upload(uint32_t header, const uint16_t *data, size_t avail);
	void r3d_store(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *&data, const uint16_t *end, bool sixteen, bool lsb, bool msb);
	void r3d_texture_port(unsigned reg, uint32_t data);
	void r3d_dma_write32(unsigned reg, uint32_t data);
	uint32_t r3d_dma_read32(unsigned reg) const;
	void tile_ram_write32(unsigned addr, uint32_t data);
	void tile_reg_write(unsigned reg, uint32_t data);
};

} // namespace m3
