// Rooms 2 (docs/ROOMS2-DESIGN.md): a room the lobby owns, winner stays on, a visible line with a ready check. The game
// is a member of the room (RoomMember): in its line, watching its match or not, or at its table playing. The
// membership sits above what Online is doing (an activity: watching, a seat's session), never torn down by it: a
// seat's match ending, or watching stopping, leaves the player in the room; only leaving it (Leave room, Leave line,
// another match started, the lobby letting them go) ends it.
//
// The lobby's side is tools/lobby/src/index.js (/v2/room/*, r2_* and Room.v2): every state change pushed on the
// member's WebSocket ({state}), or a poll's answer every 10 s without one. What the game does with a state:
//   at the table with an opponent and a token it hasn't played: a fresh session with them (seat), watching stopped;
//   in the line and not watching: the room's match watched (its watch id, one for the room's life);
//   getting ready (the next READY one after a match, ready.you): GET READY and its 3 s, the chime; then seated by the
//   lobby itself (Start: now; Esc or B: AWAY, kept in the line, passed over); an empty seat is taken at once, no countdown;
//   in the line again after playing (the match lost): the session ended, back to watching.
// A seat's match decided (one player out of the game, as its RAM has it) is reported once (result: a, b or draw);
// a session that ended otherwise reports how (disconnect, desync). With someone READY in the line the out player's
// Start is taken out of the input before it's sent (step), so there's no rematch and the next comes on; with nobody
// READY in it the two play on, the loser's Start a rematch as ever.
//
// Kept for a game that crashed or was restarted: each room's code, member id and key, in memory and in the data
// folder's room2.txt, so entering the same room within the lobby's 30 s takes back the same place (enter with mid and
// mkey). Shared with VF1 and VF2 (phase 3): this file and online_room.cpp have nothing of VF3's but Online's names.
#pragma once
#include "../net/http.h"
#include "../net/json.h"
#include "../net/room_link.h"
#include <memory>
#include <string>
#include <vector>

struct RoomMember
{
	// who and where
	std::string code, mid, mkey, owner_key, wid;   // (owner_key: the creator's, for settings and close; "" others)
	bool creating = false;          // (create, not enter: the first answer's)
	bool back = false;              // (the lobby took back a membership from before: a rejoin)
	// The room as the lobby last said (its state)
	struct Who { std::string name, country; bool ready = true; };   // (ready: in the line, READY (played when it's their turn) or AWAY (passed over))
	struct Seat
	{
		std::string role, side, token, as, them, wkey;
		Who peer;
		std::vector<std::string> cands;
		std::string relay;
		int n = 0;
	};
	std::string room_name;
	bool listed = false, locked = false, owner = false;
	int ready_ms = 15000;
	Who a, b;                       // (the table: side a is the session's host)
	bool has_a = false, has_b = false;
	std::string phase = "EMPTY", holder;   // (holder: the streak's side, "a" or "b", "" none)
	int streak = 0, n = 0;
	std::string last_result, last_why, last_winner;   // (how the last match ended: a, b, draw, forfeit, void)
	int last_n = -1;
	std::vector<Who> line;
	int pos = -1;                   // (0 at the table, 1.. in the line, -1 not known yet or a watcher)
	bool me_ready = true;           // (this member READY, or AWAY)
	double away_asked = -1e9;       // (when this player last set READY or AWAY: a change the lobby made itself is said)
	bool watcher = false;           // (in the room watching, no place in the line: Leave line; Join the line puts them back)
	int watchers = 0;               // (the room's members watching without a place)
	// The match on and the streak it's played for (the lobby's table.match), and what's shown over the match on screen:
	// the streak its pair's match began with, taken when the table's pair is the one on screen (a watcher behind the
	// lobby keeps the last match's while it's still on screen)
	int match_n = -1, match_streak = 0;
	std::string match_holder;
	std::string shown_pair, shown_holder;
	int shown_n = -1, shown_streak = 0;
	std::string side;               // ("a" or "b" at the table)
	bool seated = false;            // (a seat: an opponent, a token)
	Seat seat;
	int misses = 0;
	bool ready_on = false, ready_you = false;
	int ready_pos = 0;
	double ready_until = 0;         // (the ready check's end, on Online's clock)
	int version = 0;                // (counted up at every state: the room page made again)
	bool have_state = false;

	// What became of it ("" still a member): left, removed (ready, gone, version), closed
	std::string ended;

	// The membership's own link (STUN and TURN gathered once), handed to each seat's session and back
	std::unique_ptr<vfnet::RoomLink> link;
	double gathered_at = 0, kept_at = 0;   // (kept_at: room2.txt last written, its time the membership's last sign of life)

	// The lobby: its socket (pushed states; ping every 15 s), or polling every 10 s over HTTPS
	std::shared_ptr<vfnet::http::Socket> ws;
	bool ws_failed = false;
	int ws_tries = 0;
	double ws_at = 0, ping_at = 0, poll_at = 0, heard_at = 0;
	std::vector<std::pair<std::string, std::string>> outbox;   // (op, its JSON: sent on the socket, or POSTed)

	// The activity's bookkeeping
	int step = 0;                   // (0 getting ready: the link's addresses; 1 asking the lobby: create or enter; 2 a member)
	bool in_seat = false;
	bool test = false;              // (menutest's made-up state: shown, nothing done about it)           // (a seat's session on (connecting or playing): the link is Online's room meanwhile)
	std::string played_token;       // (the seat's token whose session was started: one session a seat)
	std::string retry_token;        // (a seat's session lost (cut off, out of step): the lobby playing on with the same pair and token
	                                // (the holder kept, nobody next) has it started again, fresh)
	std::string failed_token;       // (a seat whose session ended before it began: not tried again; the lobby moves on)
	bool started_said = false;      // (started sent for played_token)
	int reported_n = -1;            // (the match number a result was sent for)
	bool result_armed = false;      // (both in the game since the last report: the next decided match is a new one)
	bool want_watch = true;         // (in the line: the room's match watched; Stop watching turns it off)
	double watch_retry_at = 0;
	std::string seen_peer;          // (the opponent at the table last time: a new one is a new challenger)
	int last_pos = -1;
	bool was_called = false;
	double banner_until = 0;        // (a moment's banner: you lost, a new challenger, you're next)
	int banner = 0;                 // (0 none, 1 you lost, 2 new challenger, 3 you're next, 4 you won, 5 forfeit)
	std::string banner_name;
	bool ready_sent = false;
	double ready_auto_at = 0;       // (tests: VF3_ROOM2_READY's press)
	bool test_away_done = false;    // (tests: VF3_ROOM2_READY=away, AWAY once a member)

	// The first READY one in the line (the next to play), or null
	const Who *next_ready() const { for (const Who &w : line) if (w.ready) return &w; return nullptr; }
	// This member's place among the READY ones in the line (1: the next to play), 0 not in it or AWAY
	int ready_place() const { if (pos <= 0 || !me_ready) return 0; int k = 0; for (int i = 0; i < pos && i < int(line.size()); i++) if (line[size_t(i)].ready) k++; return k; }
};
