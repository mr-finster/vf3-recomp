// license:BSD-3-Clause
// copyright-holders:ElSemi, R. Belmont
// The SCSP's effects DSP, from MAME's sound/scspdsp.h (unchanged but for its memory: the sound
// RAM it shares with the 68000, instead of an address_space; and the MEMS timing: see mame).
#pragma once
#include "../core/mame_shim.h"

//the DSP Context
struct SCSPDSP
{
//Config
	u16 *ram;        // the SCSP's address space: sound RAM, ram_bytes long (16-bit words, native order)
	u32 ram_bytes;
	u32 RBP; //Ring buf pointer
	u32 RBL; //Delay ram (Ring buffer) size in words

//context

	s16 COEF[64];     //16 bit signed
	u16 MADRS[32];   //offsets (in words), 16 bit
	u16 MPRO[128*4]; //128 steps 64 bit
	s32 TEMP[128];    //TEMP regs,24 bit signed
	s32 MEMS[32]; //MEMS regs,24 bit signed
	u32 DEC;

//input
	s32 MIXS[16]; //MIXS, 24 bit signed
	s16 EXTS[2];  //External inputs (CDDA)    16 bit signed

//output
	s16 EFREG[16];    //EFREG, 16 bit signed

	// MAME's step: a MEMS register written (IWT) and read (IRA) in the same step reads the value being written. On
	// the chip (Mednafen's and Supermodel's DSP, which follow its pipeline) INPUTS is read before the write, so the
	// step reads the register's old value. VF3's echo (its first SCSP's program, steps 55 and 95) reads MEMS 3 so:
	// MAME's way, each delay line feeds back on itself at a gain of 0.999 and its tail never dies (it grows, from
	// the float format's rounding); the chip's way, the two lines share it and the echo is gone in about 2.5 s.
	bool mame = false;

	bool Stopped;
	int LastStep;

	// The program decoded once (by Start, when it is written) rather than at every step of every sample
	struct Inst {
		u8 TRA, TWT, TWA, XSEL, YSEL, IRA, IWT, IWA, TABLE, MWT, MRD, EWT, EWA, ADRL, FRCL, SHIFT, YRL, NEGB, ZERO, BSEL,
			NOFL, COEF, MASA, ADREB, NXADR;
	};
	Inst prog[128];

	void Init();
	void SetSample(s32 sample, s32 SEL, s32 MXL);
	void Step();
	void Start();
	void Decode(int step);   // one step of MPRO into prog (Start does them all)

	u16 read_word(u32 a) const { return a < ram_bytes ? ram[a >> 1] : 0; }
	void write_word(u32 a, u16 v) { if (a < ram_bytes) ram[a >> 1] = v; }
};
