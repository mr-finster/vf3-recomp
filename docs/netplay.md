# Netplay: rollback, step by step

The plan: GGPO-style rollback netcode, as a library shared by VF1, VF2 and VF3 Recomp, with a small lobby server
(rooms, NAT traversal, a relay, spectators). VF3 first: its board's state is the easiest to save.

1. **Save states and the soak test** (done; to run with the ROM): the board's whole state saved and restored,
   and checked the way rollback uses it.
2. **The shared library, the VF3 adapter and the Online menu** (done, but for the ROM's runs): `src/net` (rollback
   sessions, UDP, a simulated network), `src/host/vf3_netplay` (the board as a session's game), `src/host/online`
   (the Online pages, hosting and joining by address), wired into `main.cpp`.
3. The lobby server (room codes, quick match, NAT traversal and a relay, watching), then the rest of the menu's
   plan below: the lobby's rows, rematch after a match, the names on the energy bars, replays.
4. VF2, then VF1 (whose recompiled code runs on its own stack: that has to change first).

## Where it stands, and how to try it

| Part | State |
| --- | --- |
| `src/net`: rollback, handshake, input redundancy, time sync, desync checks, disconnects | `nettest`: 17 network cases and UDP on localhost, all pass (no ROM needed) |
| Save states | `vf3soak`, versus script, 7-frame rollback every frame: 4,303 frames identical. Save 1.3 ms, restore 1.5 ms (device memories copied by the 4 KB pages written since; was 6.9 and 4.7) |
| VF3 adapter (start, inputs, MIDI under rollback) | `vf3net`, 40 ms each way: 4,000 frames in step, every checkpoint the straight run's; 1.0-1.3 frame rollbacks, a step 4.7 ms on average (a slow cloud machine) |
| Names under the energy bars | drawn in the HUD's style (`VF3_HUD_NAMES=P1,P2` for vf3run's shots) |
| Online menu, notices, connection display | built into the game; drawn without the ROM by `menutest` (pictures below); not yet played |
| Lobby (`tools/lobby`, a Cloudflare Worker), room codes, STUN, hole punching, the TURN relay (`src/net/room_link`, `stun`, `http`) | `roomtest` against a local coturn: direct, both relays, one side's relay only, a wrong token, all pass. Two games through a local lobby (`wrangler dev`): a match direct, and one through the relays (Hide my IP on both), 30 s each, in step |
| Quick match, watching, rematch, replays | not yet |

```sh
build/bin/nettest                                              # the session, no ROM
build/bin/vf3net vf3.zip --script /tmp/scripts/versus.txt      # two boards, 40 ms each way (--latency, --jitter, --loss, --delay)
SDL_VIDEO_DRIVER=offscreen build/bin/menutest /tmp/shots       # the Online pages as pictures (VF_LOBBY=... adds the room pages)
build/bin/roomtest                                             # room links against a TURN server on 127.0.0.1:3478 (coturn: tools/roomtest.cpp)
```

To play: both start the game; one opens Online > HOST (a room code), the other Online > JOIN and types it (joined at
the 6th letter). The lobby only hands each side the other's addresses (on their network, public as STUN saw
it, their relay) and a token; the games try every way at once and keep the first that answers. By address (Online::host
/ join, a UdpLink) is still in the code for tests, not in the menu: codes only. A match starts at the title screen, both on free
play (both press Start); the menu doesn't pause it; Leave the match (or the other side going) goes back to playing alone, with
the player's own NVRAM given back (a match plays on the host's).

## What's enough: the bar to clear

From a survey of other projects' online play (Oct 2026; many sources secondhand, marked so in the notes).

- **The closest thing there is**: Supermodel Dojo plays VF3 online, delay-based only, by IP and port, and desyncs
  unless both sides match the PowerPC's clock, refresh rate and delay. Fightcade (FBNeo, GGPO rollback, lobbies
  with ping, challenges, spectators, replays) is what players will compare us with. VF5 Final Showdown's
  delay-based online was disliked; VF5 R.E.V.O.'s rollback (2025) was well received, but forcing both players to
  the lower of their two delays was not.
- **Input delay**: Auto by default, or 0 to 10 frames by choice. Each side keeps its own; never forced down to the
  other's. A side's delay is for the other's sake: its inputs reaching the other side in time are what spare the
  other side a rollback. So each side measures what it can afford (`AutoDelay::rollback_room`: the frames it can run
  again in one step and still make its frame, from its recent steps' and re-run frames' cost, 0 to 6) and tells the
  other side in every input packet; Auto (`vfnet::auto_delay`, `vfnet::AutoDelay`, tested by `delaytest`) is then as
  little as covers the trip beyond the other side's room: none within a frame's round trip (a LAN), else 1, more
  where the one-way trip and its jitter would pass what the other side can take back. A Steam Deck in a fight (a
  frame ~12 ms of its 17) says 0: its opponent's inputs then reach it in time, it rolls back nothing and never
  stutters, while the Deck itself plays at 1 against a fast machine. Up to 15, so a far opponent costs input delay,
  never a slowed or choppy game. Through a match: the first as the ping's known (the character select), up a frame
  at a time every 6 frames once wanted for 2 s on end, to the least wanted in them (a burst, a phone network's
  second of 200 ms, is rollback's: it no longer raises the delay for the rest of the round), or frames held back for
  the other side two 3 s on end (past the first 15 s; a raise that doesn't hold back fewer let go, and no more for
  them for 2 minutes, then the match: they weren't the delay's to fix); down once a lower one's been enough for 3 s with a tenth more
  ping to spare, a frame a second mid-round, the whole way between rounds. log.txt says each change and why, and
  every 10 s both sides' rooms.
- **Keeping in time**: the side ahead of the other runs its frames a little slower until they're level
  (`Online::slow_down`: 2% a frame ahead, up to 6%). So a slower
  machine (a Steam Deck a few percent short of full speed) is followed smoothly; only past that does a side hold a
  frame back, and it stops dead for the other's inputs only at the rollback's end (`delaytest`: against a 55.6-fps
  opponent with 30 ms hitches, 0 of those, where a 2% limit had 2,351).
- **The sound in a match runs to the real clock** (`Game::set_sound_free`): the sound board runs on its own thread
  and keeps 4 frames of sound queued whatever the frames do, the game's frames only handing it their bytes. A frame
  held back for the other side, a slow one (a Steam Deck's character select), or the game a few percent slower to
  keep in time: none of it reaches the sound, which never drops out or changes pitch (the sound board only hears
  the game, never answers it, so it isn't part of what the two sides must agree on). log.txt's pace line says how
  many times the queue ran dry.
- **Tests on a worse network and a slower machine, with the real game:** `VF3_NET_LAG="one-way ms,jitter,loss[,a
  burst every s,its ms,its length s]"` sends this side's packets late; `VF3_TEST_FRAME_MS="ms[,every s,its ms]"`
  makes each frame cost that much (a Deck's 9, its character select's 13).
- **The tile generator's lines** are drawn on a thread of their own as the game reaches each (`video::TileWorker`:
  the line's registers copied, the line drawn a moment later), and the 2D layers put together there while the
  frame's 3D is built: about half a fight frame's time (2.8 of 5.5 ms here, more on a Steam Deck) off the game's own
  thread. They're only seen, never read by the game, so a match's frames are the same either way.
- **The widescreen sides' searches** (the stage's map looked for behind a screen of its own, the character select's)
  aren't made again while all they read is as when they found nothing: they'd find nothing again. They were made
  every 16 frames in one go, and up to 60 times a frame for its rows: 2.5 ms a frame, in bursts of 20 ms or so (a
  Deck's hitch at the select, several a second), now about 0.3. The picture is the same (VF3_NO_TILE_SKIP makes
  every search, to compare).
- **Rollback**: up to 7 frames; beyond that the game waits rather than guessing further. Rollback runs only when
  a guess was wrong (the soak test's every-frame mode is the stress case).
- **The cost**: up to 8 frames run in one frame's 17.4 ms, with the picture, on a Steam Deck. So a frame's run
  with no picture about 1.5 ms at most, and a save well under 1 ms. A full 27 MB copy (about 4 ms) is too slow:
  saves must copy only what changed.
- **Ping**: under 60 ms good, 60-120 playable, 120-150 shown as poor, over 150 or jittery (wifi) warned about before
  the match. The connection indicator shows real numbers (ping, rollback frames), not bars.
- **The game's rate is fixed**: 57.524 Hz on both sides whatever the screen, smooth motion or frame limit;
  fast-forward and pause are off, and the menu never stops the game.
- **The handshake** compares the build, a hash of the ROM's contents (not its file name) and the settings that
  change the game (the host's EEPROM and backup RAM go in the starting state), and says plainly what doesn't match.
- **Desyncs**: a state hash every 30 frames; on a difference the match ends with "desync", the states are saved
  for a report, and a rematch is offered. Never a hang or a crash.
- **Disconnects**: "connection interrupted" after 1 s, the match given up after about 8 s.
- **Joining**: room codes, and a lobby with a ping column and challenge / accept; UDP hole punching first, the
  relay when that fails (shown as relayed, with what it adds). Rematch and back to the lobby after a match.
- **Sound**: VF3's sound board is apart from the game's state, so it can't be rolled back. Waiting for certain
  frames would make every sound up to 7 frames late, so the MIDI bytes of guessed frames go to it at once, and after
  a rollback only bytes that weren't sent already: a wrong guess can leave a stray sound, never a late one.
- **Later**: spectators, shared replays, ranked play, a region filter, a lag simulator for practice, a wifi badge.

## Safety: a stranger on the other end

The code is open, so anyone can read exactly what the game accepts from the network; it has to be safe because
of what it does, not because nobody looks. What another player can reach, and what stops them:

- **Packets** are small and fixed in form (src/net/session.cpp): every field read with its length checked, strings
  at most 255 bytes, a packet's inputs at most 255, inputs for frames too far ahead dropped (a bad peer can't make
  a side hold more and more), the starting blob at most 4 MB. `netfuzz` plays sessions while a man in the middle
  spoils packets every way it can (bits, lengths, extreme values, splices, replays, made-up packets), built with
  AddressSanitizer and UBSan: 48,000 sessions and about 14 million spoilt packets, nothing out of bounds.
- **A session's token**: the guest picks 64 random bits; every packet carries them and any other is dropped (and
  doesn't count as hearing from the other side). Someone who can't see the packets can't put one in.
- **Whom a host listens to**: it locks onto a sender only once its hello is good (same protocol, build, game and
  ROM), not onto the first datagram: a junk packet from anyone used to stop a host from ever being joined. A host
  that locked onto someone who never finishes gives up after its handshake time and waits again.
- **No amplifier**: a hello forged with someone else's address gets a few small welcomes back (at most 10, as small
  as the hello), never the settings: those go only once the guest has said back the host's random 64 bits from
  its welcome, which only the real address got. (Before, 340 bytes of forged hellos got 5 MB sent to a victim.)
- **Floods**: a poll reads at most 64 of the session's packets and skips at most 256 strangers', so no flood holds
  a frame up; a datagram too big for the buffer, or an ICMP error, is skipped, not taken for "nothing more".
- **Text from the other side**: a name keeps only the letters the HUD has (A-Z, 0-9, space . - ! ?), and a refusal
  is a reason code that each side words itself; the other side's version string is shown filtered and short.
  Everything shown in the menu is escaped for its markup.
- **The game itself** only ever gets the other player's buttons, an input word a frame, and (from a host) the
  arcade's backup RAM and EEPROM to start from. The game runs in the emulated board: the PowerPC's RAM accesses are
  checked against its 8 MB (the interpreter's and the recompiled code's), and the board's devices mask every
  address into their own memories, so nothing the game is made to do reaches outside them. (Still to do: the scene
  builder reads geometry wherever the game points, checking only where it starts, with padding after; a start
  blob made to trip that could crash the game, not run code. To be bounded.)
- **Nothing is ever run or loaded** from the other side: no files, no code, no settings beyond those two memories,
  which go back to the player's own after the match (and aren't saved meanwhile).
- **Cheating**: each side runs the whole game itself and only inputs cross, so a modified game can't change the
  other player's game; anything but the real inputs ends the match as a desync. What it can't stop is what no
  input-only netcode can: a bot or a macro playing for someone.
- **Your address**: playing directly, the other player sees your IP address, as in any peer-to-peer game. Hide my IP
  address (Online > Settings) makes matches go only through this side's relay: its own addresses are
  never given to the lobby nor sent from.
- **The lobby**: rooms are random 6-letter codes (31^6, about 900 million) with a per-IP limit on joins, so guessing
  one is impractical; a guest gets in only with the room's token, which the session checks in its hello. TURN logins
  come from the Worker for 6 hours, only to an ask naming the game, its version and ROM hash (200 a day an address);
  the TURN key stays in the Worker's secrets.
- **Random matches**: one Queue (a Durable Object) holds who's looking, with roughly where Cloudflare places them
  (its `cf` latitude and longitude, never sent on): each is paired with the nearest within a reach that widens with
  the wait (1500 km, then 4000, 9000, then anyone). The one who waited longer hosts: the Queue opens a room for them
  and joins the other into it, so a random match is an ordinary room match from there. The flags are Cloudflare's
  country for each side's request, never what a game says of itself.
- **Not in yet**: a SECURITY.md saying where to report a hole privately.

## The menu: Online

In the menu the game already has (src/host/menu.cpp, RmlUi), so it looks and works like every other page: the
D-pad or stick to choose, Left / Right to change, A to select, B back, Y to clear; the keyboard and mouse the same.
Nothing is reached only by mouse or only by keyboard.

**Typing** (a name, a room code), as VF1's seed entry: on a keyboard, type; on a pad, Up / Down change the letter,
Right adds one, Left takes one off, A done, B as it was. Room codes are 6 letters from an alphabet with no
look-alikes (no O/0, I/1), so they can be read out over voice chat and entered quickly on a pad.

**Online** (a new row in the root menu, between Game and Controls):

| Row | What it does |
| --- | --- |
| Your name | Up to 10 letters, shown under your energy bar in a match. Asked for the first time Online is opened. |
| Quick match | Finds an opponent with a good ping. You go back to the game (attract mode, or a fight with the CPU) while it looks; a notice over the game says when someone's found. |
| Host a room | Shows the room's code, big, with Copy (to the clipboard) and how many are waiting. Back to the game while you wait. |
| Join a room | Type the code, A to join. |
| Lobby | The players online: name, ping (colour-coded, a wifi mark), playing or free. A on a name challenges; your own row says if you're free to be challenged. |
| Watch | Matches being played now; A to watch one. |
| Input delay | Auto (shows what it picked) or 0 to 10 frames. Says what it means: "fewer frames: more rollback". |
| Connection display | On / Detailed / Off: the ping, input delay, rollback and DIRECT / RELAY in a match (Detailed: the jitter, average rollback, frames ahead, frame cost). |
| Server | Advanced: the lobby server's address. |

**Being challenged**: a notice over the game, menu open or not: "MRFINSTER (42 ms) challenges you: A accept, B
decline". It's declined by itself after 15 s.

**Starting a match**: one screen with each step, ticked off as it's done: connecting (and "directly" or "through the
relay, +18 ms"), checking both have the same version and ROM, sending the starting state, both ready. Then both
boards start from it, on free play, at the title screen (both press Start). Each player plays with
their own Player 1 controls, whichever side they're on, so nobody has to rebind anything.

**In a match**: the game never pauses. Start or Esc opens a smaller menu over the game while it goes on: the
connection's details, Input delay (from the next match), Leave the match (asks first). The opponent sees "in the
menu" by your name. The connection display is small and out of the way at the bottom of the screen (the game's own bars and time are at the top): the ping, the
input delay, the rollback frames and DIRECT / RELAY in fixed cells (a mono font, so nothing shifts as they change),
green / yellow / red by what the player feels, not the ping: green up to 7 frames of input delay and rollback with the
game at full speed (a 310 ms match at delay 7 plays well), yellow up to 10 or a few frames a second held back for the
other side, red past that or with the connection dropped.

**When something goes wrong**, it says what happened and what to do, in words, never a code:
- "Connection interrupted" with a count down over the game, which waits; then "Your opponent left" and back to the
  lobby.
- "The match went out of step (a desync). A report was saved to send us." and Rematch / Lobby.
- "Your opponent has v0.1.4, you have v0.1.3: you both need the same version." "Your ROM isn't the same as your
  opponent's." "Couldn't reach the server: check the internet connection."

**After a match**: Rematch / Lobby, with what the other player picked ("waiting for MRFINSTER"), instead of the
game's continue countdown.

What's built of this so far (direct play, before the lobby): however a match ends (the other player left, closed
the game, the connection was lost, a desync, or you left), both games go back to playing on their own and open the
menu on the Online page, under MATCH OVER: what ended it ("RENODECK left the match."), against whom, how long and
the ping, and one row on the selection to play again: Host again for the host, Join RENODECK again for the guest
(the same address). Closing the game in a match tells the other side at once, so they don't wait out a silence.
While hosting or joining with the menu closed, the line at the bottom says so (Hosting, the address, the status); in
a match it's the connection line, on both players' screens; "Connection back" after an interruption.

What the menu needs for this, beyond what it has: VF1's additions (typing, a line under a row for status or an
error, a page replaced in place), notices drawn over the game when the menu is closed (overlay.rml, which now draws
only the FPS), a menu that doesn't pause the game in a match, and rows whose values change on their own (the
lobby's pings, a search's status). The pages are built against the netplay library's session interface (step 2),
so they can be tried on one machine with two copies of the game before the server exists.

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
- Found: the HUD (tile layer A, rows 6-7) has each fighter's name as a picture of 6 x 2 tiles, player 1's from
  column 2, player 2's to column 59, and no font. So the names are drawn in a font of this project's own
  (src/video/hud_names.cpp: 7 x 10, strokes of 3 with a black outline, white to lavender, each stroke's right-hand
  pixel darker blue, as the HUD's letters are), player 1's from the left, player 2's to the right; ten letters
  stay short of the round markers.
- A match's start is a cold one on both sides (`Model3::power_on`: the PowerPC's registers, the EEPROM's shifter,
  the JTAG chain, the 53C810, the Real3D's flush count and more, which a reset leaves): before it, a host that had
  played offline started its match out of step.

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
