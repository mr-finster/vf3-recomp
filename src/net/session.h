// A netplay session: GGPO-style rollback between two players over a Link, knowing nothing of the game but what
// its Game hooks do (save, load, run a frame). Meant to be shared by VF1, VF2 and VF3 Recomp.
//
// The host is player 1, the guest player 2. A session starts with a handshake: the guest says who it is (the game,
// its build, a hash of the ROM's contents, the player's name); the host checks both match and sends the starting
// blob (what the game needs to start both sides alike: VF3's, the host's backup RAM and EEPROM); then both run
// frame 0 on.
//
// Each frame the host calls step() with this player's input. The input is put input-delay frames on, sent at once
// (with every input the other side hasn't acknowledged, so a lost packet costs nothing), and the frame runs with
// the other player's input guessed (the last one come) where it hasn't arrived. When it arrives and the guess was
// wrong, the next step() goes back to that frame's state and runs the frames since again (rollback, up to
// max_rollback frames: past that it waits). The two sides keep in time (the one ahead waits a frame now and then),
// compare a hash of their state every checksum_every frames, and say when the other's gone quiet or gone.
#pragma once
#include "link.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vfnet {

constexpr int MAX_DELAY = 15;   // input delay's most, frames (Auto's at a very far opponent: ~260 ms)

constexpr int MAX_NAME = 10;   // a player's name, in letters

uint64_t hash64(const uint8_t *p, size_t n);   // (the default state hash: the same on every platform)

// What the session needs of the game
struct Game
{
	// The starting blob, from the host (on both sides, before frame 0): the game starts from it
	virtual bool begin(const std::vector<uint8_t> &blob) = 0;
	virtual void save(std::vector<uint8_t> &state) = 0;           // the state before the next frame
	virtual bool load(const std::vector<uint8_t> &state) = 0;
	// One frame, with both players' inputs; resim: a frame run again after a rollback (no picture or sound needed)
	virtual void advance(const uint16_t input[2], bool resim) = 0;
	// The bytes at a state's start the desync check leaves out (the game's own bookkeeping, not the same on both
	// sides); the rest is hashed in a few parts, one a frame (a whole state at once was a frame's worst cost)
	virtual size_t hash_skip() const { return 0; }
	virtual ~Game() = default;
};

// Who this side is, as the handshake compares it
struct Identity
{
	std::string game;          // "vf3"
	std::string build;         // the version (git describe): both sides' must be the same
	uint64_t content = 0;      // a hash of the ROM's contents
	std::string name;          // the player's, up to MAX_NAME letters
};

struct Config
{
	int delay = 2;             // input delay, frames (0-10)
	int max_rollback = 7;
	int checksum_every = 30;
	double frame_ms = 1000.0 / 57.524160;
	double interrupt_ms = 1000, disconnect_ms = 8000, handshake_ms = 15000;
	size_t max_blob = 4u << 20;   // the largest starting blob a guest takes (VF3's: 128 KB)
	// the host program keeps in time itself, its frames a little slower while stats().lead is over 0 (no frame
	// held back, which shows as a hitch); the session waits a whole frame only when it's this far ahead
	bool soft_sync = false;
	// a match made in a lobby: the session's token, from it (the guest's; a host takes no hello but with it), so
	// only the player who joined with the room's code gets in. 0: the guest picks one, as a direct match's
	uint64_t token = 0;
	double hard_wait_lead = 2.5;
};

class Session
{
public:
	enum class State { handshake, running, ended };
	enum class End { none, quit, opponent_quit, disconnected, desync, rejected, timeout };
	struct Event
	{
		enum Kind { connected, interrupted, resumed, ended } kind;
		std::string text;      // for the player, in words
	};
	struct Stats
	{
		double rtt_ms = 0;
		double jitter_ms = 0;  // how much the round trip varies (its average difference from the average)
		int rollback = 0;      // the last rollback's frames
		int rollback_max = 0;  // the most in the last second
		int ahead = 0;         // frames this side is ahead of the other (time sync)
		double lead = 0;       // the same, not rounded: what soft_sync slows for
		// what this side's steps cost, ms in all (for a log: where a slow machine's time goes)
		double save_ms = 0, run_ms = 0, resim_ms = 0, hash_ms = 0, step_max_ms = 0;
		uint64_t steps = 0;
		uint64_t rollbacks = 0, resim_frames = 0, stalls = 0, waits = 0;
	};

	// host: the starting blob is this side's; guest: it comes from the host
	Session(Link &link, Game &game, const Identity &me, const Config &cfg, bool host, std::vector<uint8_t> blob = {});

	// Packets in and out; call it often (each frame at least, and while waiting). now: milliseconds, any origin.
	void poll(double now);
	// One frame, with this player's input (bits: the game's own). False if it couldn't run yet: the handshake isn't
	// done, the other side is too far behind (max_rollback), or this side is ahead and waits a frame to keep in
	// time. Call it again next frame with the input as it is then.
	bool step(uint16_t input);
	void quit();               // leave: the other side is told
	// The input delay from now on (Auto's, once the ping is known): longer repeats the input for the frames between,
	// shorter keeps the inputs already sent and takes new ones once the frames reach them
	void set_delay(int frames) { cfg.delay = std::clamp(frames, 0, MAX_DELAY); }
	int delay() const { return cfg.delay; }

	State state() const { return st; }
	bool game_started() const { return began; }   // (in the handshake: the game's start is running or done)
	End end_reason() const { return why; }
	bool interrupted() const { return quiet; }
	int quiet_left_s() const;   // (interrupted) the seconds before the other side's given up for gone
	bool next_event(Event &e);
	int local_player() const { return host ? 0 : 1; }
	const std::string &opponent_name() const { return their_name; }
	uint32_t frame() const { return cur; }           // the next frame to run
	uint32_t confirmed() const;                      // frames run with both inputs real
	const Stats &stats() const { return stats_; }
	void clear_step_max() { stats_.step_max_ms = 0; }   // (a log's: the worst step since it last said)
	// Every frame's inputs so far, both players' (a replay: the blob and these)
	const std::vector<uint16_t> &inputs(int player) const { return player == local_player() ? local : remote; }
	const std::vector<uint8_t> &blob() const { return start_blob; }
	void set_local_flags(uint8_t f) { my_flags = f; }   // e.g. 1: in the menu (shown to the other player)
	std::function<void(const std::string &)> trace;   // the handshake's steps, in words (for a log)
	uint8_t remote_flags() const { return their_flags; }

	// (the protocol's version: both sides must have the same)
	static constexpr uint16_t PROTOCOL = 2;

private:
	Link &link;
	Game &game;
	Identity me;
	Config cfg;
	bool host;
	State st = State::handshake;
	End why = End::none;
	std::deque<Event> events;
	std::string their_name;
	Stats stats_;
	double now = 0, started = 0, last_heard = 0, last_sent = 0, last_hello = 0;
	bool quiet = false, began = false, restart_clock = false;

	// the starting blob (the host's; the guest's as it comes, in chunks)
	std::vector<uint8_t> start_blob;
	std::vector<bool> chunk_have;
	uint32_t chunks_acked = 0, blob_size = 0, chunk_count = 0;
	// the host's, at random in its welcome: the guest's acks and ready say it back, so the settings (tens of KB)
	// only go to an address that's shown it gets what's sent there (else a hello forged with someone else's address
	// sent them there: 33 KB for a 34-byte packet)
	uint64_t nonce = 0;
	bool proven = false;
	int welcomes = 0;
	void send_welcome();

	// the rollback
	uint32_t cur = 0;                      // the next frame to run
	std::vector<uint16_t> local, remote;   // per frame: this side's inputs (delay frames ahead), the other's as come
	std::vector<uint16_t> guessed;         // per frame: the other side's input it ran with
	uint32_t rollback_from = UINT32_MAX;   // the first frame whose guess was wrong
	uint32_t their_ack = 0;                // how many of this side's inputs the other side has
	std::vector<std::vector<uint8_t>> ring;   // the state before frame f at f % size
	std::vector<uint32_t> ring_frame;          // (which frame's each slot holds: UINT32_MAX none)
	uint32_t their_frame = 0;              // the other side's frame, as last told, and when
	double their_frame_at = 0;
	int their_adv = 0;
	std::deque<int> adv_mine, adv_theirs;  // (the last second's, for time sync)
	int wait_cool = 0;
	std::deque<std::pair<double, int>> recent_rollbacks;

	// checksums: this side's and the other's, by frame
	uint32_t next_check = 0;
	struct { bool on = false; uint32_t frame = 0; int part = 0; uint64_t h = 0; } hashing;   // (one in parts)
	static constexpr int HASH_PARTS = 4;
	std::map<uint32_t, uint64_t> my_hash, their_hash;

	// round trips: the other's last clock, when it came
	uint32_t their_t = 0;
	double their_t_at = 0;
	bool have_their_t = false;

	uint8_t my_flags = 0, their_flags = 0;

	// The session's token: the guest's, chosen at random and sent in its hello; every packet after carries it and
	// one without it is dropped (a third party who can't see the packets can't put one in)
	uint64_t token = 0;
	bool have_token = false;
	std::vector<uint8_t> out(uint8_t type) const;

	void finish(End e, const std::string &text);
	void send_packet(const std::vector<uint8_t> &p);
	void send_hello();
	void send_blob();
	void send_chunks(uint32_t from_chunk, uint32_t to_chunk);
	double blob_moved = 0;   // (when the settings' window last moved on, or went again)
	void send_inputs();
	bool receive(const std::vector<uint8_t> &p);   // false: not this session's
	void handle(const std::vector<uint8_t> &p);    // (this session's: its token checked)
	void guest_failed();
	void got_inputs(uint32_t first, const uint16_t *in, uint32_t n);
	void start_running();
	void run_frame(uint32_t f, bool resim);
	void check_hashes(bool work);   // work: a part of a hash done now (a step's; a packet's only compares)
	uint16_t remote_input(uint32_t f) const;
};

// Auto's input delay for a connection, as Fightcade picks one: from the round trip. Half of it in frames at least
// (1 to 4: how it feels), and as many more as keep the frames guessed (the one-way trip and its jitter, less the
// delay) within target_rollback, so the game is never held back for the other side's inputs and runs at its full
// speed at any ping: what a far opponent costs is input delay, not frames. floor: this machine's least.
int auto_delay(double rtt_ms, double jitter_ms, double frame_ms, int target_rollback, int floor);

// Auto's delay through a match: the first as soon as the ping's known (the character select); up once the ping's
// wanted more for half a second, or frames are being held back for the other side (a slowdown: an input held a
// frame longer is the lesser), a frame at a time every 6 (a jump of several, one input held that much longer, is
// felt); down only between rounds (a press dropped otherwise), once a lower one's been enough for 10 s with a
// tenth more ping to spare (no see-saw at a boundary), and not below one the holding back called for, for a minute.
class AutoDelay
{
public:
	bool slow = false;   // this machine: a frame's step past 7 ms (a Steam Deck: each frame run again costs it dear)
	double frame_ms = 1000.0 / 57.524160;
	int want(const Session::Stats &s) const;
	// Each frame: the delay to change to now, or -1; why it changed in `why`
	int update(const Session &session, double now_ms, bool between_rounds);
	std::string why;
private:
	bool started = false;
	double checked_at = 0, lower_since = -1, higher_since = -1, floor_until = 0;
	uint64_t stalls_at = 0;
	int floor = 0;    // (a delay the holding back called for: kept a while)
	int target = -1;  // (raising to: a frame at a time)
	uint32_t stepped_at = 0;
};

} // namespace vfnet
