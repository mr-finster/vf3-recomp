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
#pragma once
#include "session.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vfnet::watch {

constexpr uint32_t MAGIC = 0x31574656;   // "VFW1"
enum : uint8_t { START = 1, PUSH = 2, REPLY = 4 };
constexpr uint32_t MAX_FRAMES = 500000;   // a match's most (2.4 hours): the lobby keeps no more
constexpr uint32_t MAX_PUSH = 4096;       // frames in one push (the rest go in the next)
constexpr uint32_t MAX_REPLY = 2048;      // frames in one reply
constexpr size_t MAX_START = 4u << 20;    // a start's blob, at most (as a session's)
constexpr uint32_t NONE = ~0u;

// The match's own: who plays, and what game (as the start carries it, JSON)
struct Meta
{
	std::string game, terms, content, build;   // the lobby's game ("vf3"), the session's identity: terms (its build), ROM hash
	std::string name[2], country[2];           // player 1's (the host's) and player 2's
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
	struct Upload { bool start = false; std::vector<uint8_t> body; uint32_t upto = 0; };
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
	uint32_t last_hash = 0;
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
		double frame_ms = 1000.0 / 57.524160;
		double catch_up_ms = 8;       // a pass's time for frames run unseen, catching up
		int catch_up_frames = 600;    // and their most in a pass (tests count frames, not time)
	};
	enum class State { waiting, starting, catching_up, playing, buffering, over, out_of_step, failed };

	Spectator(Game &game, const Config &c) : game(game), cfg(c) {}
	// The lobby's answers: the start (the game's begun from it), a reply
	bool start(const Start &s);
	bool reply(const Reply &r, double now = -1);   // (now: when it came, ms: the buffer's from how evenly they come)
	// What to ask the lobby for next: "" nothing now, "start", "frames" (from what's had)
	std::string want(double now) const;
	void asked(double now) { asked_at = now; in_flight = true; }
	void answered(double now) { in_flight = false; answered_at = now; }
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
	double target_ms = 100, last_arrival = -1, steady_since = 0;
	std::deque<std::pair<double, double>> arrivals;       // (when frames came, against when they'd have come at best: the last 10 s)
	uint64_t ticks = 0;
	// the hash being made (a part a frame, as the session's)
	struct { bool on = false; uint32_t frame = 0; int part = 0; uint64_t h = 0; std::vector<uint8_t> state; } hashing;
	void run_one(bool seen);
	bool check(uint32_t f);   // before frame f: its hash begun if the players' is known
};

} // namespace vfnet::watch
