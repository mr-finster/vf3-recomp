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
#include <map>
#include <string>
#include <vector>

namespace vfnet {

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
	virtual uint64_t hash(const std::vector<uint8_t> &state) { return hash64(state.data(), state.size()); }
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
		int rollback = 0;      // the last rollback's frames
		int rollback_max = 0;  // the most in the last second
		int ahead = 0;         // frames this side is ahead of the other (time sync)
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
	void set_delay(int frames) { cfg.delay = std::clamp(frames, 0, 10); }
	int delay() const { return cfg.delay; }

	State state() const { return st; }
	bool game_started() const { return began; }   // (in the handshake: the game's start is running or done)
	End end_reason() const { return why; }
	bool interrupted() const { return quiet; }
	bool next_event(Event &e);
	int local_player() const { return host ? 0 : 1; }
	const std::string &opponent_name() const { return their_name; }
	uint32_t frame() const { return cur; }           // the next frame to run
	uint32_t confirmed() const;                      // frames run with both inputs real
	const Stats &stats() const { return stats_; }
	// Every frame's inputs so far, both players' (a replay: the blob and these)
	const std::vector<uint16_t> &inputs(int player) const { return player == local_player() ? local : remote; }
	const std::vector<uint8_t> &blob() const { return start_blob; }
	void set_local_flags(uint8_t f) { my_flags = f; }   // e.g. 1: in the menu (shown to the other player)
	uint8_t remote_flags() const { return their_flags; }

	// (the protocol's version: both sides must have the same)
	static constexpr uint16_t PROTOCOL = 1;

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
	bool quiet = false, began = false;

	// the starting blob (the host's; the guest's as it comes, in chunks)
	std::vector<uint8_t> start_blob;
	std::vector<bool> chunk_have;
	uint32_t chunks_acked = 0, blob_size = 0, chunk_count = 0;

	// the rollback
	uint32_t cur = 0;                      // the next frame to run
	std::vector<uint16_t> local, remote;   // per frame: this side's inputs (delay frames ahead), the other's as come
	std::vector<uint16_t> guessed;         // per frame: the other side's input it ran with
	uint32_t rollback_from = UINT32_MAX;   // the first frame whose guess was wrong
	uint32_t their_ack = 0;                // how many of this side's inputs the other side has
	std::vector<std::vector<uint8_t>> ring;   // the state before frame f at f % size
	uint32_t their_frame = 0;              // the other side's frame, as last told, and when
	double their_frame_at = 0;
	int their_adv = 0;
	std::deque<int> adv_mine, adv_theirs;  // (the last second's, for time sync)
	int wait_cool = 0;
	std::deque<std::pair<double, int>> recent_rollbacks;

	// checksums: this side's and the other's, by frame
	uint32_t next_check = 0;
	std::map<uint32_t, uint64_t> my_hash, their_hash;

	// round trips: the other's last clock, when it came
	uint32_t their_t = 0;
	double their_t_at = 0;
	bool have_their_t = false;

	uint8_t my_flags = 0, their_flags = 0;

	void finish(End e, const std::string &text);
	void send_packet(const std::vector<uint8_t> &p);
	void send_hello();
	void send_blob();
	void send_inputs();
	void receive(const std::vector<uint8_t> &p);
	void got_inputs(uint32_t first, const uint16_t *in, uint32_t n);
	void start_running();
	void run_frame(uint32_t f, bool resim);
	void check_hashes();
	uint16_t remote_input(uint32_t f) const;
};

} // namespace vfnet
