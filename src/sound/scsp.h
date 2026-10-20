// license:BSD-3-Clause
// copyright-holders:ElSemi, R. Belmont
// thanks-to: kingshriek
// Sega/Yamaha YMF292-F SCSP, from MAME's sound/scsp.cpp and scsp.h (By ElSemi; MAME/M1 conversion
// and cleanup by R. Belmont; additional code and bugfixes by kingshriek), with the parts of
// MAME's framework it uses brought in: its sound stream (samples made up to the current time
// when the 68000 touches the chip, and when the sound manager asks), its timers (the board's
// scheduler runs them), and device_serial_interface (emu/diserial.cpp, Carl, Miodrag Milanovic)
// for the MIDI link the Model 2 main board talks to the sound board through.
#pragma once
#include "scspdsp.h"
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

#define SCSP_FM_DELAY    0    // delay in number of slots processed before samples are written to the FM ring buffer
				// driver code indicates should be 4, but sounds distorted then

class scsp_device
{
public:
	using attotime = unsigned __int128;   // attoseconds
	static constexpr attotime NEVER = 0;  // timer_adjust's delay for "never" (a disable)

	// What the board gives the chip: its RAM (shared with the 68000), and the framework's services
	struct host
	{
		std::function<void(int line, int state)> irq;             // irq_cb: the 68000's IPL lines
		std::function<void(int state)> midi_out;                  // midi_out_cb: TxD to the main board
		// timers: 0-2 timers A, B and C, 3 the MIDI receiver's clock, 4 the transmitter's; a
		// periodic one (period != 0) re-arms itself after its callback, as MAME's emu_timer
		std::function<void(int timer, attotime delay, attotime period)> timer_adjust;
		std::function<uint64_t()> sample_index;                   // the stream's current sample (now * 44100)
		std::function<bool()> time_is_zero;
	};
	enum { TIMER_A, TIMER_B, TIMER_C, TIMER_RCV, TIMER_TRA, N_TIMERS };

	scsp_device(u32 clock, u16 *ram, u32 ram_bytes, const host &h);

	// SCSP register access (offset in words from the chip's base)
	u16 read(offs_t offset);
	void write(offs_t offset, u16 data, u16 mem_mask = ~0);

	// MIDI I/O access (used for comms on Model 2/3)
	void midi_in(int state) { rx_w(state); }
	// A whole byte, as the receiver hands one over when it has read the stop bit (rcv_complete)
	void midi_in_byte(u8 data);

	void timer_fired(int timer);
	static u32 timer_samples(u16 reg);   // timers A-C: samples from a write of the register to the interrupt
	// Brings the stream up to now (sound_stream::update)
	void update();

	// The samples made so far, stereo pairs as MAME's stream holds them (-1..1, before the
	// output gain), and the output gain (MVOL / 15).
	std::vector<float> m_out;
	float output_gain() const { return m_gain; }
	uint64_t samples_made() const { return m_written; }

	// Looking in (for working out the game's mix; no effect on the output): each key-on, and
	// per slot the energy it sent straight out (DISDL) and into the DSP (IMXL), and the energy
	// of the DSP's outputs (EFSDL), summed over both channels.
	struct key_on { uint64_t sample; int slot; u32 sa, lsa, lea; int pcm8b, lpctl, tl, disdl, dipan, isel, imxl, sdir, oct, fns, ar, d1r, d2r, rr, dl, eghold, krs, lpslnk; };
	std::function<void(const key_on &)> on_key_on;
	bool m_probe = false;
	double m_probe_direct[32] = {}, m_probe_dsp_in[32] = {}, m_probe_efreg = 0;
	double m_probe_state[2][4] = {};   // per group, per EG state: the energy sent (direct and into the DSP)
	int m_probe_efsdl[16] = {};   // EFSDL per DSP output, as last seen

	// Separate volumes for groups of voices (not the chip's: a host's mixer). The board says
	// which group a slot's next voice belongs to (set_next_group); each slot's output is scaled
	// by its group's gain before it goes out and into the DSP. At 1 (the default) nothing is
	// touched: the output is MAME's.
	enum { GROUPS = 2 };
	void set_group_gain(int group, float gain);
	void set_next_group(int slot, int group) { m_next_group[slot & 31] = u8(group); }
	std::function<int(int slot)> group_of;   // or, if set, asked as each voice starts
	int slot_group(int slot) const { return m_slot_group[slot & 31]; }
	const SCSPDSP &dsp() const { return m_DSP; }

private:
	enum SCSP_STATE { SCSP_ATTACK, SCSP_DECAY1, SCSP_DECAY2, SCSP_RELEASE };

	struct SCSP_EG_t
	{
		int volume; //
		SCSP_STATE state;
		int step;
		//step vals
		int AR;     //Attack
		int D1R;    //Decay1
		int D2R;    //Decay2
		int RR;     //Release

		int DL;     //Decay level
		u8 EGHOLD;
		u8 LPLINK;
	};

	struct SCSP_LFO_t
	{
		u16 phase;
		u32 phase_step;
		int *table;
		int *scale;
	};

	struct SCSP_SLOT
	{
		union
		{
			u16 data[0x10];  //only 0x1a bytes used
			u8 datab[0x20];
		} udata;

		u8 Backwards;    //the wave is playing backwards
		u8 active;   //this slot is currently playing
		u32 cur_addr;    //current play address (24.8)
		u32 nxt_addr;    //next play address
		u32 step;        //pitch step (24.8)
		SCSP_EG_t EG;            //Envelope
		SCSP_LFO_t PLFO;     //Phase LFO
		SCSP_LFO_t ALFO;     //Amplitude LFO
		int slot;
		s16 Prev;  //Previous sample (for interpolation)
	};

	float m_group_gain[GROUPS] = { 1, 1 };
	bool m_group_gains = false;   // any gain not 1
	// The effects' leveller, on while the groups have their own volumes (with the music turned
	// down, nothing masks the effects' own range, 16 dB from a jab to a KO's last hit): above
	// FX_THRESHOLD (their power, as the board mixes them, before their volume) it takes the
	// effects down at FX_RATIO (VF2's, with the music off: as far as it is turned down), quickly (5 ms)
	// and back over a quarter of a second
	static constexpr float FX_THRESHOLD = 0.0015849f, FX_RATIO = 3;   // -28 dBFS
	static inline const float FX_ATTACK = std::exp(-1.0f / (0.005f * 44100)), FX_RELEASE = std::exp(-1.0f / (0.25f * 44100));
	float m_fx_env = 0, m_fx_level = 1;
	u8 m_slot_group[32] = {}, m_next_group[32] = {};
	u32 m_clock;
	u32 clock() const { return m_clock; }
	u16 *m_ram;
	u32 m_ram_bytes;
	host m_host;
	float m_gain = 0;
	uint64_t m_written = 0;   // the stream's write_sample(): samples made since power-on
	bool m_in_update = false;
	u32 m_rand_seed = 0x9d14abd7;   // running_machine::rand
	u32 machine_rand() { m_rand_seed = 1664525 * m_rand_seed + 1013904223; return (m_rand_seed >> 16) | (m_rand_seed << 16); }

	// device_rom_interface<20, 1, 0, ENDIANNESS_BIG> on the sound RAM
	u8 read_byte(u32 a) const { a &= 0xfffff; return a < m_ram_bytes ? u8(m_ram[a >> 1] >> ((a & 1) ? 0 : 8)) : 0; }
	u16 read_word(u32 a) const { a &= 0xffffe; return a < m_ram_bytes ? m_ram[a >> 1] : 0; }
	void space_write_word(u32 a, u16 v) { a &= 0xffffe; if (a < m_ram_bytes) m_ram[a >> 1] = v; }

	union
	{
		u16 data[0x30/2];
		u8 datab[0x30];
	} m_udata;

	SCSP_SLOT m_Slots[32];
	s16 m_RINGBUF[128];
	// 0x7c0-0x7ff, after the DSP's MADRS (0x780-0x7bf): a deliberate departure from MAME, whose
	// SCSP mirrors MADRS there. Mednafen's SCSP (its MADRS at 0x780-0x7bf only) and Yabause's
	// keep them apart, and VF2 shows why: its sound program writes COEF and MADRS, then zeros
	// 0x7c0-0x7ff. With the mirror that wipes every delay-line address, and its stereo reverb
	// collapses to a loud near-mono smear (left/right correlation 0.99, the music about 11 dB
	// louder); kept apart, the reverb is wide (correlation 0.73-0.85 in the music) and the music
	// sits under the hits as the game means. Another VF2 port reported the same finding. Writes
	// here are kept and read back; nothing else uses them. VF2_SCSP_MADRS_MIRROR=1 restores
	// MAME's mirror, for comparing the sound with MAME's (tools/sound_compare.py).
	u16 m_madrs_hi[32] = {};
	// 0x7C0-0x7FF a mirror of MADRS (MAME's, and what Supermodel's VF3 sounds like): VF3's sound program sets its
	// reverb's addresses, then clears that range, which with the mirror leaves them all at one place: the reverb
	// short, as the reference's (the coin's and start's echoes die in about a second; kept apart, VF2's way, they
	// rang on for several). VF3_SCSP_MADRS_SEPARATE=1: kept apart.
	const bool m_madrs_mirror = [] { const char *e = getenv("VF3_SCSP_MADRS_SEPARATE"); return !(e && *e == '1'); }();
	// VF3_SCSP_MSLC_LATCH=1: MAME's MSLC status latched once a sample (for comparisons)
	const bool m_latch_mslc_per_sample = [] { const char *e = getenv("VF3_SCSP_MSLC_LATCH"); return e && *e == '1'; }();
	u8 m_BUFPTR;
#if SCSP_FM_DELAY
	s16 m_DELAYBUF[SCSP_FM_DELAY];
	u8 m_DELAYPTR;
#endif

	u32 m_IrqTimA;
	u32 m_IrqTimBC;
	u32 m_IrqMidi;
	u32 m_IrqCPU;
	u32 m_IrqDMA;

	u8 m_latched_MSLC;
	u16 m_latched_MSLC_data;
	u8 m_MidiOutStack[32];
	u8 m_MidiOutW, m_MidiOutR;
	u8 m_MidiStack[32];
	u8 m_MidiW, m_MidiR;

	s32 m_EG_TABLE[0x400];

	int m_LPANTABLE[0x10000];
	int m_RPANTABLE[0x10000];

	int m_TimPris[3];
	int m_TimCnt[3];

	// DMA stuff
	struct
	{
		u32 dmea;
		u16 drga;
		u16 dtlg;
		u8 dgate;
		u8 ddir;
	} m_dma;

	u16 m_mcieb;
	u16 m_mcipd;

	int m_ARTABLE[64], m_DRTABLE[64];

	SCSPDSP m_DSP;

	s16 *m_RBUFDST;   //this points to where the sample will be stored in the RingBuf

	//LFO
	int m_PLFO_TRI[256], m_PLFO_SQR[256], m_PLFO_SAW[256], m_PLFO_NOI[256];
	int m_ALFO_TRI[256], m_ALFO_SQR[256], m_ALFO_SAW[256], m_ALFO_NOI[256];
	int m_PSCALES[8][256];
	int m_ASCALES[8][256];

	void sound_stream_update(int samples);
	void exec_dma();       /*state DMA transfer function*/
	u8 DecodeSCI(u8 irq);
	void CheckPendingIRQ();
	void MainCheckPendingIRQ(u16 irq_type);
	void ResetInterrupts();
	void timerA_cb();
	void timerB_cb();
	void timerC_cb();
	int Get_AR(int base, int R);
	int Get_DR(int base, int R);
	void Compute_EG(SCSP_SLOT *slot);
	int EG_Update(SCSP_SLOT *slot);
	u32 Step(SCSP_SLOT *slot);
	void Compute_LFO(SCSP_SLOT *slot);
	void StartSlot(SCSP_SLOT *slot);
	void StopSlot(SCSP_SLOT *slot, int keyoff);
	void init();
	void UpdateSlotReg(int s, int r);
	void UpdateReg(int reg);
	void UpdateSlotRegR(int slot, int reg);
	void UpdateRegR(int reg);
	void w16(u32 addr, u16 val);
	u16 r16(u32 addr);
	inline s32 UpdateSlot(SCSP_SLOT *slot);
	void DoMasterSamples(int samples);

	//LFO
	void LFO_Init();
	s32 PLFO_Step(SCSP_LFO_t *LFO);
	s32 ALFO_Step(SCSP_LFO_t *LFO);
	void LFO_ComputeStep(SCSP_LFO_t *LFO, u32 LFOF, u32 LFOWS, u32 LFOS, int ALFO);

	// ---- device_serial_interface (emu/diserial.cpp), as the SCSP sets it up: 1 start bit,
	// 8 data bits, no parity, 1 stop bit, 31250 baud both ways ----
	enum { RECEIVE_REGISTER_FULL = 1, RECEIVE_REGISTER_WAITING_FOR_START_BIT = 2, RECEIVE_REGISTER_SYNCHRONISED = 4 };
	enum { TRANSMIT_REGISTER_EMPTY = 1 };
	int m_df_start_bit_count = 1, m_df_word_length = 8, m_df_min_rx_stop_bit_count = 1, m_df_stop_bit_count = 1;
	u16 m_rcv_register_data = 0x8000;
	u8 m_rcv_flags = 0;
	u8 m_rcv_bit_count_received = 0, m_rcv_bit_count = 9;
	u8 m_rcv_byte_received = 0;
	bool m_rcv_framing_error = false;
	u32 m_tra_register_data = 0;
	u8 m_tra_flags = TRANSMIT_REGISTER_EMPTY;
	u8 m_tra_bit_count_transmitted = 0, m_tra_bit_count = 0;
	attotime m_rcv_rate = 0, m_tra_rate = 0;   // half a bit
	int m_rcv_line = 0;
	bool m_tra_clock_state = false, m_rcv_clock_state = false;
	void rx_w(int state);
	void receive_register_reset();
	void receive_register_update_bit(int bit);
	void receive_register_extract();
	void transmit_register_setup(u8 data_byte);
	u8 transmit_register_get_data_bit();
	bool is_receive_register_full() const { return m_rcv_flags & RECEIVE_REGISTER_FULL; }
	bool is_transmit_register_empty() const { return m_tra_flags & TRANSMIT_REGISTER_EMPTY; }
	u8 get_received_char() const { return m_rcv_byte_received; }
	void tra_edge();
	void rcv_edge();
	void tra_callback();
	void tra_complete();
	void rcv_complete();
};
