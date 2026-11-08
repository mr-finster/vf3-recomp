// Watching a match (spectating): what a match's host sends the lobby, and what a watcher makes of it. Nothing here
// knows about any game (shared by VF1, VF2 and VF3 Recomp, as the rest of src/net).
//
// The players never talk to a watcher. The host, once a second, sends the lobby (tools/lobby, HTTPS) the frames
// confirmed since the last time: both players' inputs, a word each a frame, and the desync check's state hashes. That's
// all a player does for watchers, the same whether none watch or a thousand: one small request a second (Feed), on a
// thread of its own, never waited for. The lobby keeps the match (its start, every frame's inputs from the first) and
// hands it to any number of watchers, each asking for the frames from where it is (Cloudflare's edge keeps each
// finished piece, so a crowd costs the lobby little). A watcher (Spectator) starts its own copy of the game from the
// match's start and runs the same inputs a few seconds behind the players: the same game, frame for frame, its
// state checked against the players' hashes. One that's slow, far or cut off only falls further behind; nobody
// upstream knows. A watcher joining late runs from the match's start as fast as it can, unseen, then catches up: only
// the host's start and the players' own inputs, never a state another watcher made (one could be forged to the
// players' hash).
//
// The messages, little-endian (docs/netplay.md, Watching: the lobby's side):
//   start       magic, START, 1, match (u64), meta (u16 length, JSON text), blob (u32 length, bytes)
//   push        magic, PUSH, 1, match, first frame (u32), n (u32), n x (player 1's u16, player 2's u16),
//               hashes (u16 n, n x (frame u32, hash u64)), info (5 bytes: matches won 1, 2, rounds won 1, 2, flags)
//   reply       magic, REPLY, 1, match, state (u8: 0 none yet, 1 on, 2 over), total frames (u32), watchers (u32),
//               info (5), ~0 (u32: once a checkpoint's frame, none now), first (u32), n (u32), the pairs, hashes as push's
//   snap        magic, SNAP, 1, match, frame (u32), format (u32: the game's, Game::snap_format), hash (u64: the state's,
//               as the desync check makes it), state's length (u32), extra's length (u32), packed length (u32), the
//               state and the extra (the game's own: VF2's sound bytes before it) deflated together (zlib)
//
// A snapshot (Snap, Snapper): the host's state at a confirmed frame, made when the lobby asks (a watcher come late), off
// the game's thread, sent in parts a little at a time; a watcher who comes late loads it and runs only the frames after
// it (seconds, not the whole match). Only from the match's host, as its start is (sent with the room's key): never a
// state a watcher made. Checked as it's loaded (its hash, the board's own state's after it), then at every one of the
// players' hashes after it as ever; a build whose states differ (snap_format) never takes it, nor one whose game can't
// (0), and with none, or one that didn't take, the watcher runs the match from its start as before.
#pragma once
#include "session.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vfnet::watch {

constexpr uint32_t MAGIC = 0x31574656;   // "VFW1"
enum : uint8_t { START = 1, PUSH = 2, REPLY = 4, SNAP = 5 };
constexpr uint32_t MAX_FRAMES = 500000;   // a match's most (2.4 hours): the lobby keeps no more
constexpr uint32_t MAX_PUSH = 4096;       // frames in one push (the rest go in the next)
constexpr uint32_t MAX_REPLY = 2048;      // frames in one reply
constexpr size_t MAX_START = 4u << 20;    // a start's blob, at most (as a session's)
constexpr uint32_t NONE = ~0u;
constexpr size_t MAX_SNAP_STATE = 64u << 20;   // a snapshot's state at most (VF3's is 27 MB), its extra 1 MB
constexpr size_t MAX_SNAP_EXTRA = 1u << 20;
constexpr size_t MAX_SNAP = 16u << 20;         // and packed (the lobby keeps no bigger)
constexpr size_t SNAP_PART = 32u << 10;        // its upload's parts (paced: Snapper; the lobby takes 16 to 128 KB)
constexpr double SNAP_RATE = 375000;           // its upload's most, bytes a second (3 Mbit/s: Snapper)

// The match's own: who plays, and what game (as the start carries it, JSON)
struct Meta
{
	std::string game, terms, content, build;   // the lobby's game ("vf3"), the session's identity: terms (its build), ROM hash
	std::string name[2], country[2];           // player 1's (the host's) and player 2's
	bool snap = false;                         // (the host makes snapshots when asked: "snap":1, a newer build's)
};

// What the scoreboard says (the game's RAM, read by the host after each frame: VF3's Score)
struct Info
{
	uint8_t wins[2] = {}, rounds[2] = {};
	uint8_t flags = 0;   // bit 0: the match is over
	bool operator==(const Info &o) const { return wins[0] == o.wins[0] && wins[1] == o.wins[1] && rounds[0] == o.rounds[0] && rounds[1] == o.rounds[1] && flags == o.flags; }
};

struct Start
{
	uint64_t match = 0;
	Meta meta;
	std::vector<uint8_t> blob;
};
std::vector<uint8_t> encode(const Start &s);
bool decode(const std::vector<uint8_t> &b, Start &s);

struct Frames
{
	uint64_t match = 0;
	uint32_t first = 0;
	std::vector<uint16_t> in;   // pairs: player 1's, player 2's, a frame each
	std::vector<std::pair<uint32_t, uint64_t>> hashes;
	Info info;
	uint32_t count() const { return uint32_t(in.size() / 2); }
};
std::vector<uint8_t> encode_push(const Frames &f);
bool decode_push(const std::vector<uint8_t> &b, Frames &f);

struct Reply
{
	uint64_t match = 0;
	uint8_t state = 0;          // 0 nothing yet, 1 on, 2 over
	uint32_t total = 0, watchers = 0;
	Frames frames;              // (its match and info the reply's)
};
std::vector<uint8_t> encode_reply(const Reply &r);
bool decode_reply(const std::vector<uint8_t> &b, Reply &r);

struct Snap
{
	uint64_t match = 0;
	uint32_t frame = 0;    // the state before this frame
	uint32_t format = 0;   // Game::snap_format's
	uint64_t hash = 0;     // state_hash's, the game's hash_skip left out
	std::vector<uint8_t> state, extra;
};
std::vector<uint8_t> encode_snap(const Snap &s, int level = 1);   // (deflated: VF3's 27 MB about 6)
// (the state's length must be from least to most, a game's snap_state_bytes, and no more than MAX_SNAP_STATE: nothing
// bigger is made, or inflated, whatever the snapshot says)
bool decode_snap(const std::vector<uint8_t> &b, Snap &s, size_t least = 0, size_t most = MAX_SNAP_STATE);
bool snap_head(const std::vector<uint8_t> &b, uint64_t &match, uint32_t &frame);   // (the head alone: whose, which frame)

std::string meta_json(const Meta &m);
bool parse_meta(const std::string &s, Meta &m);
uint64_t new_match_id();
std::string hex(uint64_t v);
// A state's hash as the session's desync check makes it (Session::check_hashes: skip bytes left out, the rest in
// HASH_PARTS parts), so a watcher's state is checked against the players' own
uint64_t state_hash(const std::vector<uint8_t> &state, size_t skip);
uint64_t state_hash_part(const std::vector<uint8_t> &state, size_t skip, int part, uint64_t h);   // (h: 0x9E3779B97F4A7C15 before part 0)
constexpr int HASH_PARTS = 4;

// ---- a match's host: what goes to the lobby ----
// Fed after each step with the session (its confirmed frames, both players' inputs, its hashes); gives an upload when
// one's due: the start once, then the frames since the lobby last had them, every interval_ms (100: watchers a frame or
// a few behind what's confirmed). One on its way at a time, or two if the first's answer is late (a lost packet sent
// again: the second carries the same frames and more, and the lobby takes each frame once), never more. If the lobby
// isn't reached for give_up_ms, it stops (watching this match ends; the match goes on as ever).
class Feed
{
public:
	// (5 uploads a second: on a WebSocket, a twentieth of a request each; the lobby asks for one every 30 s while
	// nobody watches. One not answered: the next 2 s later at most, whatever the pace)
	double interval_ms = 200, give_up_ms = 60000;
	static constexpr int MOST_IN_FLIGHT = 2;
	void begin(const Start &s, double now);
	void take(const Session &s);           // after each step (cheap: only frames confirmed since)
	void set_info(const Info &i) { info = i; }
	void over() { info.flags |= 1; }       // the match is over: said with the next push
	// (upto: the frames it has; hashed: the last of the hashes in it, -1 none: those taken since aren't in it, kept)
	struct Upload { bool start = false; std::vector<uint8_t> body; uint32_t upto = 0; int64_t hashed = -1; };
	bool next(double now, Upload &u);      // an upload due now (none in flight)
	// its answer: ok, and what the lobby has (frames) and how many watch
	void done(const Upload &u, bool ok, uint32_t lobby_has, int watchers, double now);
	bool on() const { return started && !gave_up; }
	bool failed() const { return gave_up; }
	int watchers() const { return watching; }
	uint32_t confirmed() const { return uint32_t(in.size() / 2); }
	uint32_t acked() const { return lobby_has; }
	uint64_t match() const { return st.match; }
	uint64_t bytes = 0, uploads = 0;       // (sent: what this side's watchers cost it)
private:
	Start st;
	bool started = false, start_sent = false, gave_up = false, missed = false;
	int in_flight = 0;                     // (uploads on their way: a second one sent if the first's slow, never more)
	std::vector<uint16_t> in;              // every confirmed frame's pair
	std::map<uint32_t, uint64_t> hashes;   // (not yet in an upload the lobby has)
	Info info, info_sent;
	uint32_t lobby_has = 0;
	double last_at = -1e18, reached_at = 0;
	int watching = 0;
	int64_t last_hash = -1;                // (the last hash taken: frame 0's, the match's start, one too)
};

// ---- a match's host: a snapshot for watchers who come late ----
// Asked for by the lobby (want: a watcher come late, the last one too old or none), one at most every min_gap_ms. After
// each step, the newest confirmed state the session keeps (Session::kept_state) is copied on a thread of its own while
// the session leaves it be (before_step stops it first if the next step could save over it: a millisecond's wait at
// most, then tried again), then hashed and deflated there. Its upload is gentle on the host's connection, which carries
// the players' packets too: parts of SNAP_PART (32 KB: a burst its modem holds 0.13 s at 2 Mbit/s, 26 ms at 10), each
// on a connection kept open, a part's start twice its sending time after the last's at the least (its time less the
// lobby's round trip, heard: from the host's small uploads), so each is sent before the next starts and half of what
// the connection carries at most, and none sooner than max_rate allows (SNAP_RATE, 3 Mbit/s: a fast connection's share
// kept small, VF3's 6.3 MB in 17 s or more, inside the 35 s a snapshot counts as fresh). Two parts may be on their way
// at once (max_in_flight; one till the first's back and its sending time known), so a long round trip to the lobby
// doesn't hold the pace back to a part each round trip. If, at the pace its parts
// have gone, it wouldn't be sent within give_up_ms of being asked for, it's let go at once (aborted: a slow uplink's
// watcher catches up from the match's start, as without snapshots) rather than run on. Nothing the players do waits
// on any of it.
class Snapper
{
public:
	double min_gap_ms = 15000, gap_ms = 50, give_up_ms = 120000;
	double max_rate = SNAP_RATE;           // bytes a second at most, on average (0: no cap)
	int max_in_flight = 2;                 // parts on their way at once (each on its own connection)
	size_t part_bytes = SNAP_PART;
	~Snapper() { stop(); }
	Snapper() = default;
	Snapper(const Snapper &) = delete;
	Snapper &operator=(const Snapper &) = delete;
	void want(double now);
	// after each step (the game's thread; the game's snap_extra asked for its frame as the copy starts)
	void step(const Session &s, Game &g, uint64_t match, double now);
	void before_step(const Session &s);    // (before each step)
	void stop();                           // (the match over, its session to go: none copied or made from it any more)
	struct Part { uint32_t index = 0, count = 0, frame = 0; uint64_t match = 0; std::vector<uint8_t> body; };
	bool next(double now, Part &p);        // a part due now (fewer than max_in_flight on their way); each given to done
	void done(const Part &p, bool ok, double ms, double now);
	void heard(double ms, double now);     // (a small request's round trip to the lobby: the least of the last 10 s kept)
	bool busy() const { return phase != Phase::idle; }
	// (the log's)
	uint32_t made = 0, sent = 0, failed = 0, aborted = 0;   // (aborted: too slow to be sent in time, counted in failed too)
	uint32_t last_frame = 0;
	size_t last_raw = 0, last_packed = 0;
	double last_copy_ms = 0, last_pack_ms = 0, last_upload_ms = 0, stall_ms = 0;
private:
	enum class Phase { idle, copying, uploading };
	// (the copy and its packing, on a thread of its own that may outlive this: let go, never waited for, once it has
	// stopped reading the session's state)
	struct Job
	{
		std::atomic<bool> cancel{ false }, reading{ true }, done{ false };
		std::vector<uint8_t> packed;
		double copy_ms = 0, pack_ms = 0;
		size_t raw = 0;
	};
	std::shared_ptr<Job> job;
	Phase phase = Phase::idle;
	bool wanted = false;
	int in_flight = 0;                     // (parts given by next, not yet to done: any upload's)
	double wanted_at = 0, made_at = -1e18, started_at = 0, next_at = 0, upload_at = 0, part_at = 0, send_ms = -1, rtt_ms = 0, rtt_at = -1e18;
	uint32_t frame = 0, kept_for = 0, part = 0, acked = 0, tries = 0;
	std::vector<uint32_t> again;           // (parts to send again: not taken)
	uint64_t match = 0;
	std::vector<uint8_t> packed;
	void let_go();
	void give_up(bool slow);
};

// ---- a watcher ----
// The match's frames as the lobby gives them, and the game run from them a few seconds behind. The host program
// asks the lobby (what next_request says) and gives the answers back; each pass, run() runs the frames due (one a
// frame's time; more, unseen, to catch up) and says how many.
class Spectator
{
public:
	struct Config
	{
		// The frames held ahead of the one shown, a buffer against the frames' arrival: as little as this watcher's own
		// connection allows, from how unevenly they've come lately (their jitter), more after it ran dry, less again
		// once steady (a voice call's playout buffer's way)
		double buffer_min_ms = 50, buffer_max_ms = 8000;
		double jump_ms = 8000;        // further behind than the buffer and this: unseen to it again (else a quarter faster)
		double poll_ms = 1000;        // asking the lobby for more, once caught up (no stream)
		double retry_most_ms = 10000; // after an ask that failed: poll_ms, twice that each time again, to this at most
		double frame_ms = 1000.0 / 57.524160;
		double catch_up_ms = 8;       // a pass's time for frames run unseen, catching up
		int catch_up_frames = 600;    // and their most in a pass (tests count frames, not time)
	};
	enum class State { waiting, starting, catching_up, playing, buffering, over, out_of_step, failed };

	Spectator(Game &game, const Config &c) : game(game), cfg(c) {}
	// The lobby's answers: the start (the game's begun from it), a reply
	bool start(const Start &s);
	bool reply(const Reply &r, double now = -1);   // (now: when it came, ms: the buffer's from how evenly they come)
	// The host's snapshot (the lobby's latest): loaded if it's ahead of where this one is, its hash the state's and the
	// board's after it, and this game takes its format; the frames from its frame then (ask_from), none before it
	// needed. False if not taken (why says, the game as it was), or the board couldn't take it (out_of_step: from the
	// start again, without)
	bool snapshot(const Snap &s);
	uint32_t snapped_at() const { return snap_from; }   // (the frame it came in at from a snapshot, 0 none)
	// What to ask the lobby for next: "" nothing now, "start", "frames" (from what's had)
	std::string want(double now) const;
	void asked(double now) { asked_at = now; in_flight = true; had_at_ask = have(); replied = false; }
	void answered(double now);                   // (its reply, if any came, given first)
	void time(double now) { clock_now = now; }   // (the host program's clock, ms, before tick)
	bool live = false;                           // (the frames come on a stream, as they're sent: no asking for them)
	void ask_every(double ms) { cfg.poll_ms = ms; }   // (the lobby busy: asking for the frames less often)
	// One frame's time (the host program's frame clock): the frame due, with its picture; more unseen first when far
	// behind (catching up, for up to budget_ms: the config's if < 0), none while the buffer's empty. Returns the frames
	// run; *shown, the last ran with its picture
	int tick(bool *shown = nullptr, double budget_ms = -1);
	// The same on a clock of its own (tests): the ticks due at now (ms)
	int run(double now, bool *shown = nullptr);
	// Told true before frames are run unseen to catch up (from the match's start, or far behind after a long spell cut
	// off) and false after: the host program drops their sound meanwhile, else it would come out late, all at once.
	// (The odd extra frame run unseen to keep up isn't one: its sound is handed on late, as a rollback's.)
	std::function<void(bool)> on_catch_up;

	State state() const { return st; }
	uint32_t frame() const { return at; }                 // the next frame to run
	uint32_t have() const { return uint32_t(in.size() / 2); }   // frames held (from the match's start)
	uint32_t ask_from() const { return have(); }          // ("frames": from here)
	uint32_t total() const { return lobby_total; }        // frames the lobby has
	double behind_s() const;                              // how far behind the players' last frame here (the buffer, and since it came)
	double buffer_ms() const { return target_ms; }        // (the buffer it keeps now)
	uint64_t underruns = 0, extra_frames = 0, jumps = 0;  // (ran dry; frames run unseen to catch up a little; jumps ahead)
	int watchers() const { return watching; }
	const Meta &meta() const { return start_.meta; }
	const Info &info() const { return info_now; }
	uint64_t match() const { return start_.match; }
	uint32_t hashes_checked = 0;                          // (the players' hashes this one's matched)
	std::string why;                                      // (out_of_step, failed: in words)
	uint64_t frames_run = 0, frames_unseen = 0;
	double started_at = -1;

private:
	Game &game;
	Config cfg;
	State st = State::waiting;
	Start start_;
	bool started = false, in_flight = false;
	std::vector<uint16_t> in;              // the frames' pairs from the match's start
	std::map<uint32_t, uint64_t> hashes;
	Info info_now;
	uint32_t at = 0, lobby_total = 0;
	uint8_t lobby_state = 0;
	int watching = 0;
	double asked_at = -1e18, answered_at = -1e18, clock = -1, clock_now = 0;
	double again_ms = 0;                  // (after the last answer: the next ask no sooner than this)
	uint32_t had_at_ask = 0;              // (the frames held when it was asked)
	bool replied = false;                 // (a reply to this match since it was asked)
	double target_ms = 100, last_arrival = -1, steady_since = 0;
	std::deque<std::pair<double, double>> arrivals;       // (when frames came, against when they'd have come at best: the last 10 s)
	uint64_t ticks = 0;
	uint32_t snap_from = 0;
	// the hash being made (a part a frame, as the session's)
	struct { bool on = false; uint32_t frame = 0; int part = 0; uint64_t h = 0; std::vector<uint8_t> state; } hashing;
	void run_one(bool seen);
	bool check(uint32_t f);   // before frame f: its hash begun if the players' is known
};

} // namespace vfnet::watch
