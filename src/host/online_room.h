// Rooms 2 (docs/ROOMS2-DESIGN.md): a room the lobby owns, winner stays on, a visible line, READY or AWAY. The game
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
	bool rejoining = false;         // (entered with a membership from before (room2.txt): back at its table, perhaps)
	bool play_next = false;         // (entered by Play next: a room as before's code joined in its line, not as Enter code)
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
	// (the room's rules, the creator's, as the lobby has them: none for a room an older build made, each game its own settings then)
	bool has_rules = false, rule_side = false, rule_dural = false, rule_custom = false, rule_stage = true;
	int rule_max = 0;
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
	// the streak its match began with and its holder's side ("a": player 1, the session's host; "b": player 2), taken
	// while the table's match is the one on screen: at the table, this seat's (or a rematch's after it); watching, the
	// seat the watched match's start came in (a watcher behind the lobby keeps the last seat's while it's still on
	// screen). By the lobby's sides, never by names (a name the lobby blocks is "PLAYER" there; two can be the same):
	// names are only shown
	int match_n = -1, match_streak = 0;
	std::string match_holder;       // (its holder's name, as the lobby has it: only to find its side if the table's moved on)
	std::string match_side;         // (its holder's side, "" none or not known)
	int match_side_n = -1;          // (the match number match_side was taken for)
	int shown_n = -1, shown_streak = 0, shown_epoch = -1;
	std::string shown_side;
	// The seats as the lobby's states show them, counted (epoch): a new pair, or the same two seated again cold (each a
	// session of its own, a watched match's start of its own), by the table's CONNECTING and its two (the lobby's names
	// compared only with its own); the seat a watched match's start came in (watch_epoch)
	int epoch = 0, epoch_n = -1, watch_epoch = -1;
	std::string epoch_pair;
	double watch_started_at = -1e18, applied_at = -1e18;   // (a watched match's start; the last state's applying)
	std::string side;               // ("a" or "b" at the table)
	std::string stayer;             // (this seat's one who stays, "a" or "b": the streak's holder against a challenger; "" a cold pair)
	bool seated = false;            // (a seat: an opponent, a token)
	Seat seat;
	int misses = 0;
	bool ready_on = false, ready_you = false;
	int ready_pos = 0;
	double ready_until = 0;         // (GET READY's end, on Online's clock)
	int version = 0;                // (counted up when what the room page shows changes: the page made again)
	std::string shown_key;          // (what the room page shows, as the last state had it: a state that changes none of it isn't a new page)
	bool have_state = false;
	// The states' order: the lobby's seq if it sends one; else when each was known (a socket's push as it came, a POST's
	// answer as its request was sent) and the table's match number (table.n, which only grows): a state older than the
	// last applied one is not applied (a poll's answer that took longer than an op's after it, a push since)
	double state_at = -1e18;
	int state_n = -1;
	double state_seq = -1;
	bool lobby_ready = true;        // (READY or AWAY as the lobby last said, not as this player last asked)

	// What became of it ("" still a member): left, removed (ready, gone, version), closed
	std::string ended;

	// The membership's own link (STUN and TURN gathered once), handed to each seat's session and back
	std::unique_ptr<vfnet::RoomLink> link;
	double gathered_at = 0;
	bool turn_given = false;        // (the lobby gave a relay's login when it was gathered: a link without the relay then is gathered again soon)
	std::string kept;               // (what room2.txt last said: written again only when it changes)

	// The lobby: its socket (pushed states; ping every 15 s), or polling every 10 s over HTTPS
	std::shared_ptr<vfnet::http::Socket> ws;
	int ws_tries = 0;
	double ws_at = 0, ping_at = 0, poll_at = 0, polled_at = -1e18;
	double ws_heard_at = 0, ws_polled_at = 0;   // (the socket's last word, any; its last poll sent: a socket quiet too long is dead)
	double ws_open_at = 0;          // (when the socket last opened: open a minute, its tries counted afresh)
	bool ws_first = false;          // (the socket's first state not come yet: the order's marks start again from it)
	int ws_fails = 0;               // (handshakes refused or failed in a row: polling meanwhile, the socket tried again later and later)
	// The ops not yet seen taken (the socket gives no answer to an op, and the lobby drops a member's calls past its rate:
	// a burst of 10, then one per 2 s; a POST's can be lost or answered busy): each sent again 3 s, 6, 12, 24 after,
	// while the state doesn't show it (started: still CONNECTING, or a few times in PLAYING (a session started again
	// after a lost one must say it again); result: the match still on or DECIDED; ready, away: this player's READY not as
	// asked), given up after a few
	struct Op { std::string op, extra; int n = -1; double at = 0; int tries = 0; bool connecting = false, now = false; };   // (connecting: started first said while
	                                // CONNECTING; now: ready as READY NOW, seated at once while called, not READY: ON)
	std::vector<Op> outbox;

	// The activity's bookkeeping
	int step = 0;                   // (0 getting ready: the link's addresses; 1 asking the lobby: create or enter; 2 a member)
	bool in_seat = false;           // (a seat's session on (connecting or playing): the link is Online's room meanwhile)
	bool test = false;              // (menutest's made-up state: shown, nothing done about it)
	std::string played_token;       // (the seat's token whose session was started: one session a seat)
	std::string retry_token;        // (a seat's session lost (cut off, out of step): the lobby playing on with the same pair and token
	                                // (the holder kept, nobody next) has it started again, fresh)
	std::string failed_token;       // (a seat whose session ended before it began: not tried again; the lobby moves on, told
	                                // disconnect if it's PLAYING (a rematch's restart), never left seated with no session)
	int stuck_n = -1;               // (the match number a seat with no session was last told disconnect for)
	std::string stuck_token;        // (and its token: the same seat stuck again told only after STUCK_AGAIN_MS)
	double stuck_since = 0, stuck_told = 0;   // (seated, its token played, no session, the table PLAYING: since when; told when)
	int seat_n = -1;                // (the match number the seat's session was started for: no state older unseats it)
	bool settling = false;          // (a seat's start failed: not between seats till the lobby's word on it (re-gathering waits))
	bool started_said = false;      // (started sent for played_token)
	int started_n = -1;             // (the match number started was last said for: a rematch's PLAYING said too, harmless)
	int reported_n = -1;            // (the match number a result was sent for)
	bool result_armed = false;      // (both in the game since the last report: the next decided match is a new one)
	bool want_watch = true;         // (in the line: the room's match watched; Stop watching turns it off)
	double watch_retry_at = 0;
	std::string seen_peer;          // (the opponent at the table last time: a new one is a new challenger)
	std::string seen_token;         // (and that seat's token: a new seat but the same two seated again cold (a desync, a cut, a
	                                // rejoin) is no new challenger)
	int last_pos = -1;
	bool was_called = false;
	double banner_until = 0;        // (a moment's banner: you lost, a new challenger, you're next)
	int banner = 0;                 // (0 none, 1 you lost, 2 new challenger, 3 you're next, 4 you won, 5 forfeit)
	std::string banner_name;
	bool ready_sent = false;
	double ready_auto_at = 0;       // (tests: the game's ROOM2_READY hook's press, VF3_/VF1_/VF2_ROOM2_READY)
	bool test_away_done = false;    // (tests: ROOM2_READY=away, AWAY once a member)
	double regather_at = 0;         // (the link's addresses gathered again (the relay's login past its hours, or a seat that
	                                // didn't get going): not before this)
	int no_relay_tries = 0;         // (the link gathered again for a relay it hasn't got: each try the next one twice as late, capped)

	// The first READY one in the line (the next to play), or null
	const Who *next_ready() const { for (const Who &w : line) if (w.ready) return &w; return nullptr; }
	// Who's next to play: the one being called (GET READY), else the first READY one in the line; "" none
	std::string next_name() const
	{
		if (ready_on && ready_pos >= 1 && ready_pos <= int(line.size())) return line[size_t(ready_pos - 1)].name;
		return next_ready() ? next_ready()->name : std::string();
	}
	// An op of this kind still waiting to be seen taken
	bool pending(const std::string &op) const { for (const Op &o : outbox) if (o.op == op) return true; return false; }
	// This member's place among the READY ones in the line (1: the next to play), 0 not in it or AWAY
	// (this player's own place counted by its READY as asked, not the line's as the lobby last had it)
	int ready_place() const { if (pos <= 0 || !me_ready || pos > int(line.size())) return 0; int k = 0; for (int i = 0; i < pos; i++) if (i == pos - 1 || line[size_t(i)].ready) k++; return k; }
};
