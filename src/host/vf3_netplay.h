// VF3 in a netplay session (src/net): the board as the session's Game. What both sides start from (the host's
// backup RAM and EEPROM, so the same arcade settings and rankings), how they start (the PC way on both: free play,
// the FREE PLAY notices hidden, the boot run unseen), each player's switches as an input word, and the MIDI
// bytes for the sound board under rollback.
#pragma once
#include "../machine/model3.h"
#include "../net/session.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vf3net {

// The online version: what the handshake and the lobby compare (vfnet::Identity::build), not the release's. Two builds
// with the same one must run a match identically, so it changes only with what a match depends on: the game's code
// (the board, the recompiler, their compiler flags), this adapter (the start, the inputs, the settings it forces), the
// match's settings (online.cpp's config(): the desync check's frames and the rest) or src/net's protocol. A release
// that changes only the picture, the menus, the GPU or the packaging keeps it, and its players still play everyone on
// the release before. It stays the online preview's tag for now: v0.1.8 runs the same game as the preview's hotfix 3
// (220c2e7), whose builds send exactly this. (A wrong one ends a match as a desync: vf3soak --hashes against the last
// release's file says whether the game changed. A new one is named after the release that brings it.)
inline constexpr const char *NET_VERSION = "v0.1.7-online-preview";
// Dural on the select's flag on the online version, Revision D's and Team Battle's: "+dural3" since her patches
// changed again (issue #23: each Dural's half of her reflection redone in turn, so two on screen both get theirs;
// before it Revision D's "+dural2", her reflection's place by her side, and Team Battle's "+dural"), so a match never mixes
// them (the program's words are in RAM, in the desync check's state: a "+dural" build of v0.1.15 or before and this
// one would go out of step at the first check)
inline constexpr const char *DURAL_FLAG = "+dural3";
// And the flag v0.1.16-alpha has (Revision D's "+dural2", Team Battle's "+dural"): a match on it plays her as that
// version does, its patches without issue #23's fix (Model3::set_dural's `legacy`), so a newer build still plays an
// older one with Dural on (the reflection's old glitch with two Durals, gold 1P and chrome 2P, as there)
inline const char *legacy_dural_flag(bool tb) { return tb ? "+dural" : "+dural2"; }
// The online version a build plays as: NET_VERSION for a release (its version a release's name, as the release kit
// builds it, or a build at a tag: "v0.1.8-alpha"), else the build's own version (git describe's "v0.1.8-alpha-3-gabc1234"
// between releases, a bare commit, "dev"), so a test build, which may carry a change to the game, never meets the
// release's players: only the same build
std::string online_version(const std::string &build);
// Random and room matches across settings (issues #25, #32): a build that has these sends the lobby, besides its own
// online version, a pool (the online version with HOST_TERMS_FLAG: everyone on it plays the host's settings) and the
// flags it can play an older build's way (its accepts). The lobby answers with the match's terms ("as"): the pool, a
// match on the host's stage select, Dural and Team Battle mode (the start blob's trailer, below), or one side's own
// online version, played exactly as a build of that version plays it (its flags). The handshake compares the terms.
// A change to what a pool match plays (Dural's patches, the trailer) is a new flag here ("+host2")
inline constexpr const char *HOST_TERMS_FLAG = "+host1";
std::string pool_version(const std::string &online);   // (online's base, before any "+", with HOST_TERMS_FLAG)
// What a build plays a match as, from the lobby's terms: host_terms (the pool's), else that version's flags (a
// version with another base, or a flag this build hasn't got: false, not played)
// (host_terms with dural: the pool's marked "+dural3" (both games), either player's Play as Dural)
// (team_one: Team Battle's mode an older version's terms play, 1 vs 1 with "+1v1", else 3 vs 3; -1 the host's own, a pool's)
// (dural_legacy: Dural on an older version's flag, legacy_dural_flag: v0.1.16-alpha's patches)
struct Terms { bool host_terms = false, dural = false, dural_legacy = false, stage = false; int team_one = -1; };
bool terms_of(const std::string &terms, const std::string &own, const std::string &pool, bool tb, Terms &out);

// The addresses below are Revision D's, kept in the board's profile (src/machine/profile.h): the boot, unseen, up
// to the SEGA logo (as the PC way's, main.cpp): 960 frames, the profile's boot_frames; the arcade's VS stage
// select in its backup RAM (found by changing it): 0x1E017, its settings' at + stage_select. Its test menu names
// 0 CHALLENGER and 1 OFF (Revision D, 2 on not a value); Team Battle's 2 VARIABLE and 3 FULL too.
inline size_t stage_select(const m3::Model3 &board) { return board.profile->settings.at + size_t(board.profile->settings.stage_select); }
// The game's count of the matches between two players since it was switched on (a word of RAM, r13 - 0x1f80: the
// profile's versus_count, 0x106080): each one's stage is the next of a list of 16 in its program (0x76fd6, at
// (count + 1) % 16), the stage select off. An
// arcade's ran all day, so a challenge's stage was anyone's guess; a game just booted has 0, and its first match
// between two players is always on the same stage (the lava's). Set after the boot: where in the list it starts.
void set_versus_count(m3::Model3 &board, uint32_t count);
uint32_t random_versus_count();   // (0 to 15, a fresh one each call)
// The game's random numbers' seed (the profile's random_seed): offline only, after the boot (online, both sides boot
// the same and the seed stays the boot's)
void set_random_seed(m3::Model3 &board, uint32_t seed);

// A player's switches as the session's input word (coin, start, the stick, the four buttons; never test or
// service), and back
uint16_t pack(const m3::Controls &c, int player);
void unpack(uint16_t bits, int player, m3::Controls &c);

// The select in lockstep (Session::set_lockstep) or not: off for VF3, which plays the select on guesses as every
// public release has. On (hotfix 2, never released), a match against an older build or a far opponent through a
// relay waited for the other side's inputs in the select and after blips of its byte mid-match, and a player saw fights
// drop to 40 fps (28 Oct, v0.1.11-alpha's check, 180 ms to Japan) where v0.1.10-alpha held 57.5. VF1 and VF2, whose
// online play every player gets with lockstep from its first release, keep it.
constexpr bool SELECT_LOCKSTEP = false;


// A hash of the ROM's contents as loaded (the FREE PLAY notices as they are in the ROM, hidden or not)
uint64_t content_hash(m3::Model3 &board);
// Every online match first to 3 rounds: the blob's copy of the host's backup RAM has Rounds to win (vs player) (the
// profile's settings.rounds_vs, a plain byte: no checksum, the Arcade settings menu writes it as it is) at this, the
// host's own setting untouched. A guest plays the blob's backup RAM as it is (Adapter::begin), so a guest of any
// version plays it, in step; an older host's blob has its own setting
inline constexpr int ONLINE_VS_ROUNDS = 3;
// The host's starting blob: its backup RAM (Rounds to win (vs player) ONLINE_VS_ROUNDS) and EEPROM, and where the
// stages' list starts (random_versus_count)
std::vector<uint8_t> start_blob(const m3::Model3 &board);
constexpr uint16_t START_BUTTON = 256;   // (the input word's Start: pack's)
// The character select on screen: a byte of RAM (the profile's scene, 0xe4046) 5 from the first frame after Start
// to the VS screen, and never elsewhere (0 at the boot, the title and the attract, 4 at the VS screens and fights, 7 between a 1P run's
// fights; found by probing every byte of RAM over two 9,000-frame runs, a 1P and a 2P). A match plays it in
// lockstep (vfnet::Session::set_lockstep): no frame on a guess, so a face never flashes as a guess is put right.
inline int scene_byte(const m3::Model3 &board) { const size_t a = board.profile->scene; return a && board.ram.size() > a ? board.ram[a ^ 3] : -1; }
inline bool in_select(const m3::Model3 &board) { return scene_byte(board) == 5; }

// The score between the two players, read from the game's own RAM after each frame (as Fightcade's detectors do,
// tools/vf3detect.cpp found where; the profile's rounds_p1, rounds_p2 and playing): each player's rounds won in the
// match on now (0x10601a, 0x10601b: both back to 0 when it's over), and which players are playing (0x106029: bit 0
// player 1, bit 1 player 2; a side not playing is the CPU's). A match counts only if both were playing at each round it was won (not the winner's
// fights against the CPU after it, nor one a challenger broke into); each reading only once it's held 30 frames,
// longer than any rollback, so a guessed frame never scores.
struct Score
{
	int matches[2] = {}, rounds[2] = {};   // between the two players: matches won, and rounds won in them
	void frame(const m3::Model3 &board);
	int players() const { return playing; }
	uint8_t players_now() const { return now[2]; }   // (as the RAM has it this frame, not yet held: when one went out)
	int round_ends = 0;   // (each round's end, any players': a moment no one's in control, ~2 s while the game shows who won)   // (who's in the game, as held: bit 0 player 1, bit 1 player 2)
private:
	uint8_t seen[2] = {}, now[3] = {}, playing = 0;
	int held = 0;
	bool versus = false;   // (the match on now: both playing at every round so far)
};

class Adapter : public vfnet::Game
{
public:
	explicit Adapter(m3::Model3 &board) : board(board) {}

	// Set by the host program:
	// one frame (the controls are set): resim, a frame run again (no picture, no sound needed). Default:
	// board.run_frame(). Whatever takes the frame's MIDI bytes for the sound board calls take_midi (else the
	// adapter does after the frame, and gives them to midi_out).
	std::function<void(bool resim)> run;
	std::function<void()> program_changed;     // the CROM changed (the recompiled code must be made again)
	// Dural on the select (Cheats) in this match: both sides the same (their online versions match, "+dural"
	// on it: the host program sets both), her patches applied at the start (Model3::set_dural)
	bool dural = false;
	bool dural_legacy = false;   // (her patches v0.1.16-alpha's: the match's terms on its flag, both sides the same)
	// The arcade's stage select in this match (Online > Settings: "+stage" on the online version, both sides the
	// same): on, CHALLENGER (0), the challenger picks and so the loser on every rematch; off (as every build before
	// it), OFF (1)
	bool stage_select_on = false;
	// The match plays the host's settings (the pool's terms): the host's blob with its stage select and Dural after
	// it (host_blob), the guest's own dural and stage_select_on set from it at the start; off, the blob as ever
	bool host_terms = false;
	// Team Battle's mode in the host's blob: an older version's terms' (1 vs 1 with "+1v1", 0 for 3 vs 3), as that
	// version's host sends it; -1 the host's own (a pool match, or no Team Battle)
	int team_one = -1;
	std::vector<uint8_t> host_blob() const;
	std::function<void()> settle;              // before a save or load: anything running alongside the board waits
	std::function<void(const std::vector<uint8_t> &)> midi_out;   // bytes for the sound board
	std::function<void()> on_begin;            // a match's start, before the board is set up for it (keep the player's NVRAM)
	std::function<void()> booting;             // now and then in the match's boot (seconds of frames: the window's events)
	// (a watcher catching up from the match's start, unseen: the frames' sound bytes dropped, not played in a burst;
	// only what's heard, never the game)
	bool mute = false;

	bool begin(const std::vector<uint8_t> &blob) override;
	void save(std::vector<uint8_t> &state) override;
	bool load(const std::vector<uint8_t> &state) override;
	void advance(const uint16_t input[2], bool resim) override;
	// (the state's hash without its header: that's this board's own bookkeeping, not the game's)
	uint64_t hash(const std::vector<uint8_t> &state);
	size_t hash_skip() const override { return m3::Model3::STATE_HEADER; }
	// The frame's MIDI bytes to send now (board.midi emptied): all of them the first time a frame runs; run again
	// after a rollback, none if its bytes went already (the sound board can't take back what it played: a wrong
	// guess's sound stays), and if it sent none and has some now, those (late)
	void take_midi(std::vector<uint8_t> &out);

	// tests: the state's hash after each frame a multiple of this, the last time it ran (0: none kept)
	uint32_t checkpoint_every = 0;
	std::map<uint32_t, uint64_t> checkpoints;

private:
	m3::Model3 &board;
	uint32_t frame = 0;   // the session's frame the board is at
	// per frame: the MIDI bytes the sound board was sent for it (a frame run again sends only what wasn't)
	std::map<uint32_t, std::vector<uint8_t>> midi_sent;
	std::vector<uint8_t> scratch, check_buf;
	bool midi_taken = false;
};

} // namespace vf3net
