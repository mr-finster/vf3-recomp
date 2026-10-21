# Netplay: rollback, step by step

The plan: GGPO-style rollback netcode, as a library shared by VF1, VF2 and VF3 Recomp, with a small lobby server
(rooms, NAT traversal, a relay, spectators). VF3 first: its board's state is the easiest to save.

1. **Save states and the soak test** (this step, no network): the board's whole state saved and restored, and
   checked the way rollback uses it.
2. The shared library (rollback, the UDP link, replays, desync checks), two copies of the game on one machine
   with latency and packet loss simulated.
3. The lobby server, then spectators.
4. VF2, then VF1 (whose recompiled code runs on its own stack: that has to change first).

## Player names on the energy bars

In a netplay match each player's name (up to 10 letters, typed in the menu, sent when the session starts) takes the
fighter's name's place under their energy bar, as VF1 Recomp spells its random fighters' names there.

- Drawn by the host only, as the tile generator's picture is made (a cell's entry swapped as it's read), never
  written to the board's tile RAM: the board's state stays the same whatever the names are, so they can't cause a
  desync, and a rollback can't leave a name half drawn. (VF1 writes its names into the guest's tile map, which is
  in the state there: fine with one machine, a hazard with two.)
- Only on the fight's HUD, recognised by its own tiles (as VF1 checks for its WIN markers), never on another screen.
- The letters the HUD's font has (A-Z, 0-9 and a few signs, upper case); anything else is dropped as it's typed. If
  the HUD has pictures of the names and no font, the host draws the letters itself in a matching style.
- Still to find, with the ROM: where the name boxes are in the HUD's map and how its letters are made. A fight's
  tiles: `vf3run vf3.zip --script versus.txt --rec --frames 2511 --ram-at 2510 --shots shots` writes
  `tiles_2510.bin` (the tile RAM, then the 64 registers) and the frame's picture.

## Save states

`Model3::save_state` and `load_state` (src/machine/model3.cpp, src/machine/state_io.h). One function,
`Model3::state`, lists every field and is run both ways, so saving and restoring can't drift apart. In the state:
the PowerPC's state, the board's registers (IRQs, EEPROM, JTAG, 53C810, PCI bridge, inputs), RAM, backup and
security RAM, the tile generator's RAM and registers, the Real3D's culling, polygon and texture RAMs and its
pending writes, and the MIDI bytes not yet taken. About 27 MB.

- Not in it: the ROMs (both sides must have the same, checked when a session starts), and the sound board. The
  sound board only hears the MIDI bytes and nothing it does reaches the main board, so netplay will feed it the
  MIDI of certain frames only and never roll it back.
- Plain structs with padding are written field by field (`pod` refuses a type with padding), so equal states are
  equal bytes and `m3::state_hash` can be compared between machines.
- After a restore: the CROM bank pointer is set from its register, the recompiled code rechecks RAM
  (`recount`), and the renderers are told every texture changed.
- `rtc_time`: netplay sets the clock to a fixed time on both sides (VF3 reads only RTC registers that read 0, but
  the clock is not to depend on the machine).
- A state is for the build that made it; `STATE_VERSION` goes up when the layout changes.

## The soak test

`build/bin/vf3soak` runs two boards on the same script: one straight through, and one as rollback runs it,
with player 2's inputs arriving `--delay` frames late (7 by default). The second board guesses player 2's input
(held from the last one it had), and as each real one arrives it restores that frame's state and runs every frame
since again, every frame by default (`--only-misses`: only when the guess was wrong). Each frame, once certain,
must be byte for byte the straight board's. At the first that isn't, it names the parts that differ and writes
both states (`soak_straight.bin`, `soak_rolled.bin`).

```sh
make build/bin/vf3soak
tools/ref/gen_scripts.py /tmp/scripts           # versus.txt: two random players
build/bin/vf3soak vf3.zip --script /tmp/scripts/versus.txt --rec
build/bin/vf3soak vf3.zip --script /tmp/scripts/versus.txt --rec --hashes vf3-hashes-$(uname -m).txt
```

`--hashes` writes the straight board's state hash for each frame. The same script on an x86-64 and an ARM64
machine must give the same file (`diff`): rollback needs both players' boards to agree bit for bit.

It also reports what each part costs: save, restore, a frame's run, and a frame with its rollback, against the
17.4 ms a frame has at 57.5 Hz. With every save copying all 27 MB (about 4 ms in a slow cloud container), a
7-frame rollback every frame is over budget; the next things to try are copying only the memories' pages that
changed (most of the texture and polygon RAM doesn't change from frame to frame) and rolling back only on a wrong
guess.

### Still to check (needs the ROM)

- [ ] `versus.txt`, `--rec`, every frame rolled back: identical
- [ ] the same with `--only-misses`, and with `--delay` 1, 3 and 12
- [ ] the attract and a fighter script (`attract.txt`, `fighter00.txt`)
- [ ] `--hashes` from an x86-64 machine and an ARM64 one: the same
- [ ] the costs on a Mac and a Steam Deck
