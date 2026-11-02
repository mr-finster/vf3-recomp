# Step 4: the pictures, the sound and the game

Status (2026-10-14): the whole board runs (pictures and sound), inside an SDL 3 game with VF2's menus and
conventions, widescreen, smooth motion and the 3D on the GPU.

## The tile generator (`src/video/tilegen.*`)

Four 64x64 maps of 8x8 tiles in two pairs, a per-line mask choosing one layer of each pair per 32 pixels, each
pair below or above the 3D. Drawn a line at a time at the start of each active line, as the hardware scans it out
(the board's `on_line` hook), so mid-frame changes show where they happen. The formats are written at the top of
the file. Checked against the reference's pictures: the same pixels. The layers are shown 2 pixels left and up of where
they're drawn, lined up with the 3D as Supermodel places them.

## The Real3D

- **The board's side** (`src/machine/model3.cpp`): the texture FIFO's uploads into the texture RAM (two 2048x1024
  sheets of 16-bit texels, with their mipmaps), flushed when the game writes 0x88000000 or at the next vertical
  blank; writes to the high culling RAM and polygon RAM after the ping-pong flip wait for the next vertical blank.
- **The scene** (`src/video/r3d_scene.*`): the viewports (a linked list), the culling node tree with its matrices,
  LOD tables and colour tables, and the models' polygons, parsed into meshes (cached for the ROM's models that
  don't use colour tables). The formats are written at the top of `r3d_scene.cpp`, from Supermodel's New3D
  (read for its documentation, not copied).
- **The CPU rasteriser** (`src/video/r3d_raster.*`): the polygons prepared once (transformed, clipped by the near
  plane, placed on the output), then drawn in bands on every core. Per priority: the opaque layer, then two
  translucent layers (each keeps its nearest polygon); high priority polygons again over them. Per pixel: the
  textures (12 formats, bilinear, mipmapped by the Real3D's LOD), lighting, specular, spotlight, fog.
- **The GPU** (`src/gpu/*`): the same prepared polygons drawn by SDL_GPU with the same shading (`r3d.hlsl`, compiled
  to SPIR-V, DXIL and MSL by `tools/gpu/shaders.sh`), in the same passes, into a texture the host's renderer
  composites directly: nothing is read back. At the start the GPU's picture of the game's own scene is read back
  once and compared with the CPU's (on llvmpipe's Vulkan: 29 pixels of 1,049,088 differ by more than 2).

The polygons are clipped by the near plane and by the view's sides (with a guard band): a polygon reaching the camera
otherwise projects to millions of pixels, beyond what the rasteriser's floats can take (the subway stage's floor).

Compared with the reference's pictures over 754 screenshots (attract mode, every fighter's stage, a versus match):
10% of pixels differ by more than 24 in a channel on average, nearly all of it texture sharpness (below); the worst
pictures look the same side by side.
Two choices where the reference build (Supermodel's Legacy3D: New3D doesn't run on a software OpenGL) and New3D
differ:

- Mipmap levels: New3D's (the Real3D's LOD from the viewport's cota and the polygon's LOD base). Far textures are
  softer than the reference's.
- The spotlight: New3D's. Its distances are Step 1.0's view space, and its colour is added to the sun and ambient
  light, the sum up to full. VF3's lava cave has a red spotlight 2,304 deep: it lights only what is near the camera,
  the arena brown and orange as in the arcade's video (issue #19). The reference's Legacy3D's distances (view space /
  2048) lit it red across the arena, 9,000 units away.

## The sound board (`src/sound/*`)

MAME's 68000 and two SCSPs, as VF2 has them, each SCSP with 1 MB of RAM (and MAME's mirror of the reverb's
addresses, which VF2 had to drop: VF3's sound program relies on it, its echoes otherwise ring on); the main board's MIDI
bytes into the first SCSP's MIDI input; run a frame at a time, 256 68000 cycles a sample at 44.1 kHz. The sound
ROMs are little-endian words as the files hold them (they had been byte-swapped once too often).

The 68000 runs a sample at a time: the music driver's tick is timer A (about 880 a second), which its interrupt sets
again each time; run to the next timer known before, it came once a frame and the music crawled. The monitored
slot's status (MSLC) is read as it is at that moment, as Supermodel reads it.

Against the reference's sound (Supermodel built with SUPERMODEL_LOG_AUDIO, the attract mode's first 4,000 frames):
the sound starts on the same frame (1,259 there, 1,260 here), and its first second follows the reference's
(loudness envelope correlation 0.5). Later they drift apart: the reference makes 735 samples a frame (it assumes 60
Hz) where the board's 57.5 Hz frame is 766, so the game's sound commands land at other times in the sound, and its
SCSP (not MAME's) is 4-5 times louder. Both SCSPs play throughout. The host raises the board's level 12 dB, with
VF2's peak limiter.

The music and the effects have their own volumes, as VF2's do. Both SCSPs play music, and the sound program starts
every voice from the same two places, so neither the chip nor the code tells them apart. Its voice records do: 32
bytes a slot at 0x400 + slot * 0x20 in each SCSP's RAM, the slot's register address and then the channel the voice
plays for (`VF3_KEYON_LOG` shows them). The music plays on channels 0x00 to 0x0E. The game's effect, voice and
announcer requests (0x11 to 0x14, after the 0xA0 that starts each request) play on channels 0x0F to 0x12. Recorded
to the first fight with one or the other off, the effects are silent through the title's and the stage intro's
music, and the music through the round call. The two parts add up to the full mix (the remainder is VF2's effects
leveller, here only as strong as the music is turned down).

## The game (`src/host/main.cpp`)

VF2's shell: the data folder, portable mode, the ROM folder (`--roms`, `VF3_ROMS`, settings.ini), the first
launch's file picker, log.txt, the menu (RmlUi: Display, Sound, Game, Arcade settings, Controls), keyboard and pads for both players
(VF3's four buttons: guard, punch, kick, escape), NVRAM and settings saved, pacing by the sound queue, and the
hidden test mode (`VF3_*`). New for VF3:

- The picture is put together by the renderer on the GPU: the tile generator's two layers (496x384 textures), the
  3D between them (premultiplied, at the render scale and the picture's shape), scaled once into the window.
- Widescreen: viewports covering the whole screen are widened to the picture's shape (Hor+), their culling's side
  planes with them. On a stage (the 3D full width, the 2D over it clear of the edges and covering under half of it)
  the layers below the 3D are drawn across the picture, 1:1. Their maps hold only what 4:3 shows, but every stage's
  panorama is whole in the tile patterns, a map row a run of consecutive patterns: past the screen's edges each
  row's run is carried on (the palette's bit 14 the pattern's bit 7, as the game sets it), wrapping where that
  panorama row wraps (rows differ in length; each is learned when the map shows it wrap). Rows that aren't one run
  (the tops of the clouds: patterns only where there's cloud) get their commonest tile. Checked against the game's
  own frames as the camera turns: sky predicted off-screen is out by 3.1 on average where it comes on, against
  2.5 for the sky that was already on screen. A replay's letterbox bands carry across. Elsewhere
  4:3 with bars the full height (the edge's colour where it is one colour, else black). The HUD stays 4:3.
- The backgrounds between frames: the layer below the 3D followed to the fraction of a pixel. Its registers hold only
  the scroll within a tile (the game rewrites the map for the rest), so its move is the register's change plus or
  minus whole tiles, whichever lines it up with the frame before's.
- The stages' skies tied to the 3D camera: the game scrolls a stage's backdrop with the camera of 3 frames before
  (on every stage measured, its move each frame is the camera's turn 3 frames earlier, 1.11 of it across and 1.08 up
  and down, R2 0.95-0.99), so the sky slid on behind the 3D for about 50 ms after every turn. The camera is the
  matrix of the viewport's first node (the root); the backdrop is drawn ahead by the last 3 frames' turn, eased in
  while it is seen to follow the camera so and out when it doesn't (a hit's shake averages out; a round's opening
  sweep, in step, leaves it off). On Lau's stage the frames where the sky's move is more than 3 pixels off the
  camera's go from 46 to 0 (`src/video/sky_lead.h`; `VF3_SKY_LAG=1` for the game's own). The game's backdrop moves
  by whole pixels, unevenly around the camera's move (4 pixels a step at 4x): the sky is moved by the camera's own
  turn each frame, to the fraction of a pixel, and drawn 15% of the way towards the game's backdrop with the lead
  each frame so it never drifts. Its move each frame against the camera's: 0.15 pixels rms across, 0.1 up and down
  (the game's own: 0.9 and 0.6).
- Smooth motion: a picture for each refresh of the screen, between the last two frames' scenes (each model's
  matrix blended; models matched by viewport, node path and address; a cut when most don't match).
- The governor: in-between pictures first, then the render scale, cut when the pictures leave too little of the
  screen's or the frame's time; each cut lifted again after 10 s of calm; the first seconds after a start or a
  change don't count. log.txt has a pace line every 10 s: frames and pictures a second, a picture's cost and its
  uploads.
- The PC way: the boot runs unseen (960 frames, about two seconds), and it is free play, as VF1's and VF2's: the
  test menu's COIN/CREDIT SETTING #27, FREE PLAY, set in backup RAM before the boot (0x1E033 the setting's number;
  #27 also sets 0x1E03B; no checksum; found by setting each in the test menu and comparing), the arcade's own kept in
  settings.ini's arcade_coins and given back for the arcade way. Its FREE PLAY notices are hidden: the fight's top
  line (the string at the program's 0x7CAE0, spaces) and the attract's corner (picture 0x4018, drawn by the call at
  0x4E9C0: a no-op), in the CROM the boot copies the program from, each checked against the data known first.

Testing in this container: SDL's gpu renderer crashes in llvmpipe's Vulkan driver (a 40-line SDL program does
too); `SDL_RENDER_GPU_DEBUG=1` runs it cleanly (and checks every call).
