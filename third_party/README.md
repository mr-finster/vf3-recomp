# Third-party code

- `sljit/`: Zoltan Herczeg's SLJIT (BSD-2-Clause, see sljit/LICENSE; the commit in sljit/VERSION), as VF1 and
  VF2 have it: the native code VF3's PowerPC program is recompiled to when the game starts (src/rec).
- `rmlui/`: RmlUi 6.3 (MIT, see rmlui/LICENSE.txt), the in-game menu, as VF2 has it (its SDL renderer loads images
  with SDL 3.4's SDL_LoadSurface_IO and has BeginOverlay(); see VF2's notes). For the menus' languages: fallback
  faces ordered by language, then style and weight (`Rml::SetFallbackFontOrder`, FontProvider, FontFaceHandleDefault),
  and lines broken between Chinese, Japanese and Korean letters with the kinsoku rules (ElementText's BuildToken).
- `mame/m68000/`: MAME's 68000 (BSD-3-Clause, Olivier Galibert), as VF2 has it: the generated instruction handlers
  and decode table unchanged, the device reduced to the core on a bus interface (see mame/README.md). It compiles
  against `src/core/shim_include/emu.h` and `src/core/mame_shim.*`, VF2's stand-in for MAME's device framework.

Code ported from MAME (BSD-3-Clause) in `src/`, each file starting with the licence and copyright-holders lines
of the MAME files it comes from:

- `src/sound/scsp.*`, `src/sound/scspdsp.*`: the Yamaha YMF292-F SCSP and its DSP, `sound/scsp.cpp` and
  `sound/scspdsp.cpp` (ElSemi, R. Belmont; thanks to kingshriek), with the serial receiver and transmitter of
  `emu/diserial.cpp` (Carl, Miodrag Milanovic), as VF2 has them.
- the sound board's map in `src/sound/soundboard.cpp` follows `sega/model3.cpp`'s `model3_snd` (R. Belmont,
  Ville Linde).

BSD-3-Clause: redistribution in source and binary form requires keeping the copyright notice, the list of
conditions and the disclaimer; see https://opensource.org/license/bsd-3-clause.

The menu's typeface, Barlow Condensed, is in assets/fonts (SIL Open Font License 1.1, assets/fonts/OFL.txt).
