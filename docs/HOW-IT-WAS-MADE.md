# How Virtua Fighter 3 Recomp was made

This write-up covers how the port was built and checked: what is recompiled, what is new, what comes from MAME,
and how AI coding tools were used. The commit history and the step documents (`docs/step1-measure.md` to
`docs/step4-game.md`) have the detail; this is the overview.

## The approach

Virtua Fighter 3 (1996) runs on Sega's Model 3 board (step 1.0). The game logic is a program for the main CPU, a
PowerPC 603e at 66 MHz. Around that CPU are a Real3D Pro-1000 for the 3D, a tile generator for the 2D layers, a
53C810 SCSI controller used as a DMA engine, an I/O board, and a sound board: a 68000 and two Yamaha SCSPs, each with
its own DSP.

**The PowerPC program is recompiled. The rest of the board is this port's own code, except the sound board.** The
order was the one VF1 and VF2 had taught: measure first, then build an interpreter that matches a reference frame
for frame, then the recompiler, then the pictures.

- **Measured first (`docs/step1-measure.md`).** A Supermodel build was instrumented to log every PowerPC instruction
  that ran and every store, over 15 hidden runs (attract, all twelve fighters, versus, the test menu: 236,000
  frames). The finding was that VF3 never changes its code. One boot copy puts the program in RAM, nothing writes
  over code that has run, and the cache instructions that changed code would need never run. So the whole program
  can be recompiled ahead of time, as VF2's was.
- **An interpreter, frame-identical to the reference (`docs/step2-core.md`).** MAME's PowerPC core only runs
  inside MAME's own recompiler framework, so `src/ppc` is a new 603e interpreter. Where the reference differs from
  a real 603e (exact `fres` and `frsqrte`, no fused multiply-adds, its NaN signs), the core follows the reference,
  and each such place is marked. Over 14 scripted runs, 173,234 frames, every register and a hash of all 8 MB of
  RAM matched the reference on every frame.
- **The recompiler (`src/rec`, `docs/step3-recompiler.md`).** The port ships an address map,
  `src/rec/vf3_map.cpp`: 1,487 addresses (call targets, the targets of indirect jumps, interrupt entries). It holds
  addresses only, no bytes from the ROM. When the game starts, it reads the program from the player's `vf3.zip`,
  walks it from the map (78,252 instructions: all 65,664 seen running, and about 12,600 more), and turns it into
  ARM64 or x86-64 with SLJIT, in about 160 ms. Integer, memory, branch and FP arithmetic instructions are native.
  Anything that can take an interrupt or touch a device runs the interpreter's own handler. Every instruction
  counts its cycle and checks for the slice's end where the interpreter does, so interrupts land on the same
  instruction. The game's wait-for-vblank loops are skipped once a round has run, with their cycles counted. On
  this project's test machine the program runs at about 8 times the arcade's speed against the interpreter's 1.5.
- **The interpreter is kept.** It runs anything the map lacks (nothing in the runs so far) and is the reference
  for checking.

**New for the port:** everything between the CPU and the screen. Supermodel's source (GPL) was read as
documentation of the hardware, and none of its code is in this repo.

- **The board** (`src/machine`): the memory map, interrupts, the SCSI DMA, the JTAG chain, the EEPROM and the
  inputs, limited to what VF3 touches according to the reference's I/O log.
- **The tile generator** (`src/video/tilegen.*`): four layers drawn a line at a time as the hardware scans them
  out, so changes made mid-frame show where they happen.
- **The Real3D** (`src/video/r3d_*`): a scene builder for the viewports, the culling tree, the models and their
  polygons, and a CPU rasteriser with the Real3D's shading. That shading covers 12 texture formats with its mipmap
  levels, lighting, specular, a spotlight, fog and the translucent layers. It draws in bands on every core.
- **The GPU renderer** (`src/gpu`): the CPU's prepared polygons drawn by SDL_GPU, on Metal, Direct3D 12 or Vulkan.
  One shader source (`r3d.hlsl`) is compiled for all three and does the same shading in the same passes. The 3D is
  put together with the 2D layers and scaled on the GPU, so no picture is read back while the game plays. As the game
  starts, one picture of its own scene is drawn both ways and compared. The GPU is used only if they match (on
  Linux's software Vulkan, 29 pixels of a million differ by more than 2 of 255). Otherwise that run uses the CPU,
  and a driver that failed or crashed isn't tried again.
- **The SDL 3 host** (`src/host`), with VF1's and VF2's shell: the window, controls for two players, the menu
  (RmlUi), saves, portable mode and the ROM folder, plus these additions to the arcade:
  - **Render scale** from 1x to 6x, and antialiasing (2x or 3x supersampling).
  - **Widescreen** up to 48:9. In fights the 3D sees further to the sides, and the stages' backgrounds carry on
    from the game's own panoramas (below).
  - **Smooth motion**: a picture for each refresh of the screen, up to 120 a second, between the game's 57.5 frames.
    Each model's matrix is blended with the same model in the frame before, and the fighters' joints vertex by
    vertex.
  - **A governor** that cuts the in-between pictures, then antialiasing, then the render scale when a machine can't
    keep up, and lifts each cut again after 10 calm seconds.
  - **Separate volumes** for the music and for the effects and voices.
  - **The arcade's settings** in the menu (difficulty, rounds, energy, country), and the boot run unseen at full speed.

**From MAME:** the sound board's 68000 core and the SCSP with its DSP, as Virtua Fighter 2 Recomp has them, on a
small shim in place of MAME's device framework. Every file ported from MAME keeps MAME's licence and
copyright-holder lines. **Also used:** SLJIT for the native code, RmlUi for the menu, SDL 3.

## How it was checked

Everything is checked against the reference: Supermodel, built from source with a logging patch
(`tools/ref/supermodel-cov.patch`), running the same input scripts.

- **Lockstep against the reference.** `vf3run --state` writes the CPU's whole state and a RAM hash after every
  frame in the reference's format, and `tools/statecmp.py` compares them. Fighters 0 to 11, versus and the test
  menu all matched: 173,234 frames.
- **Lockstep between interpreter and recompiler.** `tools/vf3lock.cpp` runs two boards side by side, one
  interpreted and one recompiled, and compares the CPU, RAM, backup RAM, the Real3D's and the tile generator's
  memories and the MIDI going to the sound board after every frame. At the first difference it replays that frame
  slice by slice. The recompiled code is identical on every frame of every scripted run.
- **Pictures against the reference's.** 754 screenshots from the attract mode, every fighter's stage and a versus
  match, compared with the reference's at the same frames. The tile generator's layers are the same pixels. The 3D
  differs where the port chose differently (far textures are softer, from the Real3D's own mipmap choice). The
  worst pictures look the same side by side.
- **Sound against the reference's.** The reference's four channels were logged, and the port's sound recorded on
  the same inputs. They match in loudness to 0.1 dB on average, and in balance across the bands, with the port a
  little brighter above 8 kHz (MAME's SCSP, where the reference has its own).
- **Pictures against themselves.** The GPU's picture against the CPU's, and the backgrounds the widescreen
  extension predicts off-screen against what the game itself shows once the camera turns (out by 3.1 of 255 on
  average where the sky comes on screen, against 2.5 for sky already on screen, from one frame to the next).
- **Scripted, hidden runs.** `vf3run --script` plays the same input scripts as the reference. Its test switches
  (`VF3_*`) dump what a check needs: shots at any scale, the 3D alone, the tile maps, the sound voices, NVRAM.

## Working with AI

The port was built with Claude, through Claude Code, after the same process on Virtua Fighter 1 and 2. In practice:

- **A brief carried the lessons over.** VF3 started from a written brief: the goal, the Model 3 hardware, the
  reference build, the rules, and what VF1 and VF2 had learned the hard way. Examples: keep the GPU path free of
  readbacks from day one, make every cut the governor makes lift again, and carry over the reverb fix.
- **A person directs, tests and decides.** The human side was play-testing on a Mac, reporting what looked or
  sounded wrong (a wall missing its bricks, a background bending like a pinhole camera as the fighters moved),
  deciding the trade-offs, and reviewing anything public before it went out.
- **All runs headless, in their own data folders.** The game ran hidden (`VF3_HIDDEN=1`: no window, no dialog),
  never in a visible window on the machine the person was using.
- **Measure, don't assume.** Every claim came with a number: a lockstep run, a pixel count, a timing, a dB
  figure. Step 1 decided the whole design from a measurement, not from the assumption that VF2's answer would hold.
- **Memory in the repo.** Sessions don't share memory, so the step documents and the commit messages record what
  changed, why, and how it was checked. A new session reads those instead of rediscovering them.

The AI wrote nearly all the code. A person decided what was right.

## Problems worth knowing about

- **A NaN with the wrong sign at frame 1,121** → the reference uses the host's FPU, and on x86-64 an invalid
  operation gives −NaN, with the first operand in the compiled code winning between two NaNs → every NaN result is
  made in software by those rules, so the core gives the reference's answer on x86 and on ARM alike.
- **The recompiled code fell out of step once** → the vblank loop was skipped in a slice where an interrupt had
  changed its byte mid-round → a loop isn't skipped if an interrupt arrived inside the round. `vf3lock` found it.
- **The music crawled and broke up** → the sound driver's tick (SCSP timer A, about 880 a second) is set again in
  its own interrupt, and the 68000 ran to the end of the frame before it fired, so the tick came once a frame → the
  68000 runs a sample at a time, and a timer set within a slice fires on time (15.4 ticks a frame, the reference's
  14.7 at its 60 Hz).
- **The coin's echo rang on for seconds** → VF2 had needed MAME's mirror of the reverb's addresses removed. VF3's
  sound program needs it: it sets the reverb's addresses, then clears the mirrored range → the mirror is kept for
  VF3, and the echo is now down 30 dB in 0.97 s (the reference's 1.09 s). The same chip, the opposite fix.
- **The subway stage's floor rasterised wrong** → a polygon reaching the camera projected to millions of pixels,
  beyond what the rasteriser's floats can take → polygons are clipped by the view's sides too, with a guard band.
- **The lava cave wasn't lit red** → its spotlight is 9,000 units away, and with New3D's units it barely shows →
  the reference's units (step 1.0's view space / 2048), its colour added over the clamped sun and ambient. Wrong:
  the arcade's video (issue #19) shows the cave brown and orange, not red → New3D's units after all (the spotlight
  lights only what is near the camera), the sum of the lights up to full.
- **Kage's stage missing its brick wall, and the pictures darker overall** → the models' meshes were cached by their
  polygons' state, and polygons alike but for their colour took the first one's → a mesh per colour too; 204 of
  the 754 comparison shots came closer to the reference's.
- **Smooth motion stuttered on 35-50% of models** → the game rebuilds the culling nodes above them most frames, so
  matching by place in the tree failed → models are matched by address, then by shape (an animation's next model),
  then by place. In 6,000 attract frames the cuts went from 579 to 96. The fighters' joints, which the game builds
  each frame, are blended vertex by vertex, with no gaps at knees and elbows.
- **The widescreen backgrounds bent like an inverse pinhole camera as the camera moved** → the tile maps hold only
  what 4:3 shows, and stretching that to the sides moved them faster than the middle → every stage's panorama
  turns out to be whole in the tile patterns, each map row a run of consecutive patterns. Past the screen's edges
  each row is carried on 1:1, wrapping where that row wraps (the lengths learned when the map shows them, the gaps
  in pattern memory learned and skipped), each tile checked against the one before it.
- **Direct3D 12 could draw nothing** (VF2's finding on Windows) → Direct3D 12 binds storage buffers as raw views →
  the shaders read those buffers as raw bytes, Direct3D 12 first draws a test quad, and a GPU that fails falls back
  to Vulkan, then to the CPU. A crash at the start is remembered, so the next run doesn't repeat it.
- **No way to turn the music down under the hits** → both SCSPs play music, and every voice starts from the same
  code → the sound program keeps a record per voice that names the channel it plays for. The music has channels
  0x00 to 0x0E, and the effects, voices and announcer 0x0F to 0x12. That record decides each voice's volume.
- **Slow pictures on Windows above 1x** → SDL's Direct3D 11 renderer makes a new staging texture each time a texture
  is locked (VF2's finding) → the pictures are drawn in ordinary memory and copied into a texture mapped for
  writing only.

## Where to look

- `docs/step1-measure.md` to `docs/step4-game.md`: the four steps, with their numbers.
- `src/ppc/`: the 603e interpreter. `src/rec/`: the recompiler; `src/rec/vf3_map.cpp` is the address map.
- `tools/vf3lock.cpp`: the interpreter-against-recompiler checker. Start here to see what "identical" means.
- `tools/ref/`: the reference build's patch, the coverage tools and the input-script generator. `tools/recomp/`:
  making the map.
- `src/machine/`: the Model 3 board. `src/video/`: the tile generator and the Real3D. `src/gpu/`: the GPU renderer.
  `src/sound/`: the sound board.
- `src/host/`: the window, pacing, the governor, widescreen, smooth motion, the menu.
- `tools/vf3run.cpp` and `tools/statecmp.py`: the headless runner and the comparison with the reference.
- `git log`: each commit message explains what changed, why, and how it was checked.
