// Online play in the game: the menu's Online pages, and what runs a match (the connection, the netplay session and
// the board's adapter). main.cpp calls poll() every pass, and in a match step() for each frame instead of running
// it, and shows the notices.
//
// Two ways to meet. With a room code (the usual): the host opens a room in the lobby (tools/lobby, a Cloudflare
// Worker) and gets a code; the other player types it; the lobby hands each the other's addresses, and the two games
// connect themselves, directly where the routers let them, else through Cloudflare's TURN relay (net/room_link.h).
// By address (on the same network, or over a VPN): the host waits on a port, the other types its address.
#pragma once
#include "menu.h"
#include "vf3_netplay.h"
#include "../net/link.h"
#include "../net/room_link.h"
#include "../net/session.h"
#include "../net/json.h"
#include "../net/watch.h"
#include "../net/http.h"
#include <mutex>
#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>

// The Online settings, kept in settings.ini
struct OnlineSettings
{
	std::string name;             // up to vfnet::MAX_NAME letters, shown under the energy bar
	int delay = -1;               // input delay: -1 Auto (from the ping), else 0-6 frames
	int show_connection = 1;      // at the top in a match: 0 nothing, 1 the name, ping and rollback, 2 those and the details
	int port = 7700;              // the host's UDP port (by address)
	std::string address;          // the last address joined
	std::string lobby;            // the lobby's address ("" the built-in one)
	bool hide_ip = false;         // a room code's matches always through the relay: the other player never sees this address
	int max_ping = 0;             // Random's: an opponent further than this (the ping, ms) passed over for another; 0 any
	bool chime = true;            // a chime when a match is found (and the window flashing, if it's not the one in front)
	bool stage_select = true;     // the arcade's stage select in a match (CHALLENGER: the loser picks on a rematch); "+stage" on the online version
	                              // (on by default since it no longer splits Random, issue #32: a match plays the host's)
	// Create room: its name (filled in from the player's), and whether it's listed in Rooms (2; 0 not: the code only, as
	// every room before). Every match may be watched by anyone with its code; a room stays for the next in line.
	std::string room_name;
	int room_who = 0;
	int rooms_show = 0;           // the rooms list's filter, kept: 0 all, 1 open, 2 playing
	bool quick_listed = true;     // Show my Quick Match games in Rooms (listed only if both players have it on)
};

class Online
{
public:
	enum class Phase { off, hosting, joining, playing, watching };   // (watching: another match, a few seconds behind; frames through step as a match's)
	// Where a room code's match has got to (none: by address)
	enum class Step { none, getting_ready, opening, waiting, finding, searching, connecting };   // (searching: a random opponent)
	struct Notice { enum Kind { joined, ended, refused, tip, watching } kind; std::string text; };   // (tip: a toast, nothing more; watching: a watched match's first picture)
	// The match that ended last: what ended it, who and how long, for the Online page to show after it (and offer
	// the same again)
	struct LastMatch
	{
		bool any = false, was_host = false, by_code = false;
		bool lost = false;            // the connection lost (else left, or out of step)
		bool random = false;          // a random opponent's (else a code's or an address's)
		bool relay = false;           // it went through a relay
		std::string why, opponent;
		std::string code, address;    // the room's code, or the address joined (for the same again)
		int seconds = 0, ping = 0;
		int wins[2] = {};             // matches won between the two: player 1's (the host's), player 2's
		std::string country[2];       // (and their countries: two letters, "" not known)
		bool watched = false;         // a match watched, not played: name[] the players
		std::string name[2];
		std::string watch;            // (its watch code: watched again)
	};
	// What was tried last (for Try again): hosting or joining, with a code or by address
	enum class Try { none, host_room, join_room, host, join, random, watch };

	explicit Online(m3::Model3 &board) : adapter(board), board(board) {}
	~Online();
	vf3net::Adapter adapter;      // (its run, settle and program_changed set by main)
	vfnet::Identity identity;     // the game, build and ROM hash (main's); the name from the settings
	// This side's own settings for a match (main's, with identity): Dural on the select, the stage select, Team Battle;
	// what it tells the lobby besides its own version (vf3_netplay.h's HOST_TERMS_FLAG): the pool, and the flags it
	// can play another's way. A match plays the lobby's terms (use_terms), by address its own.
	bool own_dural = false, own_stage = false, tb = false;
	int their_dural = -1;   // the other player's own Dural on the select (its version, the lobby's "them"): 1 on, 0 off, -1 not known
	std::string pool;
	std::vector<std::string> accepts;
	std::string terms;            // (the match's, as the session's identity has it)

	// With a room code: open a room (the code comes in a moment: room_code()), or join one
	// (want: the room's code from before, kept if it's free: a match's Play again; patient: no room yet is waited for,
	// a minute, the host opening it again)
	bool host_room(const OnlineSettings &s, std::string &error, const std::string &want = "");
	bool join_room(const OnlineSettings &s, const std::string &code, std::string &error, bool patient = false);
	// A random opponent: the nearest of those searching (the lobby's), then a room's match like any other
	bool find_random(const OnlineSettings &s, std::string &error);
	bool random_match() const { return random; }
	int searching_with() const { return players; }   // (others searching too, as the lobby last said)
	// The same again: the last try (Try again), or the last match's (Play again)
	bool try_again(const OnlineSettings &s, std::string &error);
	bool play_again(const OnlineSettings &s, std::string &error);
	// By address
	bool host(const OnlineSettings &s, std::string &error);
	bool join(const OnlineSettings &s, std::string &error);
	void stop();                  // stop waiting, or leave the match (the other side is told)
	void poll(double now_ms);
	// One frame in a match with this player's input; false if it couldn't run yet (the picture stays)
	bool step(uint16_t input);
	bool next_notice(Notice &n);

	Phase phase() const { return ph; }
	Step step_now() const { return st; }
	bool by_code() const { return rooms; }
	bool is_random() const { return random; }   // (a random opponent's match, not a code's)
	bool is_hosting() const { return is_host; }
	std::string room_code() const;   // "ABC-DEF" ("" until the lobby's given one)
	// a match on: played, or watched (its frames through step either way: the host program runs them as a match's)
	bool playing() const { return ph == Phase::playing || ph == Phase::watching; }
	bool waiting() const { return ph == Phase::hosting || ph == Phase::joining; }
	// the match's game is set up and waits for the other side: no frame may run on the board meanwhile
	bool starting() const { return (waiting() && session && session->game_started()) || (watching() && !watch_shown); }
	bool interrupted() const { return session && session->interrupted(); }
	bool via_relay() const { return room && room->via_relay(); }
	std::string status() const;   // a line for the menu: what's happening now
	// The connection display in a match: the other player, the ping, the input delay, the rollback, direct or
	// through the relay, and how good (level 0 good, 1 fair, 2 poor: by what the player feels, see connection); detailed, the jitter, the rollbacks' average,
	// how far ahead this side is and what a frame's step costs here too. False: not in a match.
	bool connection(game_menu::net_cells &c, bool detailed) const;
	// the other player's name, as the HUD can show it (only its letters: whatever else the other side sent, dropped)
	std::string opponent() const;
	static bool local_address(const std::string &address);   // (a private network's: 192.168.x.x and the like)
	int local_player() const { return is_host ? 0 : 1; }   // (the host is player 1)
	int delay_now() const { return session ? session->delay() : 0; }
	void set_delay(int setting);  // the menu's, in a match (Auto: from the ping)
	int ping() const { return session ? int(session->stats().rtt_ms + 0.5) : 0; }
	// In a match: how much slower to run the frames to keep in time with the other side (0, or up to 0.02: 2%)
	// (gently: much more and the sound's queue ran dry, and its catching up ran frames in a burst)
	// How much slower this side's frames run to stay level with the other side's, ahead of it (lead in frames): 2% a
	// frame ahead, up to 6%, so a slower machine (a Steam Deck a few percent short of full speed) is kept up with
	// smoothly; at 2% at most this side ran into the rollback's end and stopped dead, again and again (a stutter, and
	// the sound ran dry). The sound slows with it (main.cpp), not dropping out.
	double slow_down() const { return playing() && session ? std::clamp(session->stats().lead * 0.02, 0.0, 0.06) : 0.0; }
	const LastMatch &last_match() const { return last; }
	// Why the last try didn't get going ("" if it did, or none yet): shown on the page it was made from
	const std::string &join_failure() const { return failure; }
	Step failed_at() const { return fail_step; }   // (how far the failed try got)
	Try last_try() const { return tried; }
	bool patient() const { return now < patient_until; }   // (joining a room that isn't open yet: waiting for it)
	std::string peer() const;     // the other player's name, as the lobby or the session has it ("" not yet)
	// Countries (two letters, "" not known), as the lobby has them from where each connects
	const std::string &my_country() const { return country_me; }
	const std::string &peer_country() const { return country_peer; }
	// In a match whose connection's gone quiet: the seconds left before it's given up
	int hold_left() const { return session ? session->quiet_left_s() : 0; }
	// What the host's screen says besides how it's going: a player who couldn't join and why, the lobby hard to
	// reach ("" none)
	const std::string &host_note() const { return note; }
	// Who's to press Start: on the match's title screen 1 this player hasn't yet, 2 the other hasn't; after a match
	// (the loser out of the game, as its RAM has it) 3 this player's out (Start for a rematch), 4 the other is; 0 none
	int start_wait() const;
	// who's in the game, as the score holds it (bit 0 player 1, bit 1 player 2): a side the CPU has (a player out, or
	// one who never pressed Start) shows the game's own name on the HUD, not the player's
	// (in at once, as the RAM has them this frame; out once that holds)
	int players_in_game() const { return (session && ph == Phase::playing) || watch_showing() ? score.players() | score.players_now() : 0; }
	// How many play at random now (searching, or in a random match lately), as the lobby last said; -1 not known.
	// Asked again every 30 s while it's called (the Online screen open), never while a match is on or being made
	int players_online(const OnlineSettings &s);
	// At a quit: a moment (at most ms) for the goodbyes to the lobby still on their way (a room closed, a search left)
	static void flush(int ms);
	static std::string log_masked(const std::string &t);   // (a line for the log, its addresses' ends hidden)
	bool slow_local() const { return local_slow; }   // (this match through the relay only for a Mac's Local Network permission)
	void set_last_match(const LastMatch &m) { last = m; }   // ({}: forgotten, the menu closed after it; tests: one made up)
	void forget_failure() { failure.clear(); }              // (the menu closed after it)
	// (a try made from the match's menu that didn't get going, the match left already: said on the Online page)
	void tell_failure(const std::string &why) { failure = why; fail_step = Step::none; notices.push_back({ Notice::refused, why }); }
	const std::string &local_address() const { return lan; }

	// ---- watching (src/net/watch.h): a match's watchers, and watching one ----
	// Watch a match by its room's code (the browser's, Enter code's or a link's): the game starts from the match's start
	// a moment behind the players and runs their inputs; nothing goes to them
	bool watch(const OnlineSettings &s, const std::string &code, std::string &error);
	// Enter code: the lobby asked what the code is; a room open is joined, a match on is watched
	bool enter_code(const OnlineSettings &s, const std::string &code, std::string &error);
	bool watching() const { return ph == Phase::watching; }
	bool watching_quick() const { return watching() && watch_quick; }   // (a Quick match's: no room to play next in)
	bool watch_showing() const { return watching() && watch_shown; }   // (its picture up: caught up)
	std::string watch_status() const;   // (watching: how it's going, in words)
	int watch_progress() const;         // (catching up: 0-100)
	int watch_step() const;             // (0 finding the match, 1 its start, 2 catching up, 3 watching)
	// Who plays (player 1 the host): a match's or a watched one's, for the HUD and the screens
	std::string player_name(int p) const;
	std::string player_country(int p) const;
	// How many watch: this match's (the host told by the lobby with each upload, the guest asking now and then), or
	// the watched one's; -1 a match nobody can watch
	int watchers() const;
	std::string watch_code() const;     // "KXQ-4M7": the room's code this match is watched by, or the watched one's ("" none)
	const std::string &rooms_you() const { return rooms_region; }   // (the lobby's continent for this player: "near me")
	// The rooms list (the lobby's /v1/online: listed rooms, open or playing), this game's; asked again every 15 s while
	// it's called (the Online screen, the browser, open)
	struct RoomRow
	{
		bool playing = false, older = false, locked = false;   // (locked: listed, its code needed from its host: none here)
		bool quick = false;   // (a Quick match: its two players, to watch only: no code, no room name)
		std::string room, code, watch, name[2], country[2], region;   // (room: its name; code: to join, watch: to watch)
		int wins[2] = {}, round = 0, watchers = 0, since = 0, queue = 0;
	};
	const std::vector<RoomRow> &rooms_list(const OnlineSettings &s, bool again = false);
	// A listed room's name as the lobby lists it (its own, or "<HOST>'S ROOM" if the lobby's word filter wouldn't show it)
	const std::string &listed_as_name() const { return listed_as; }
	int rooms_state() const { return rooms_said; }   // (-1 not asked yet, 0 asking, 1 have, 2 couldn't reach the lobby, 3 no rooms there)
	bool lobby_watches() const { return lobby_watch_on; }   // (the lobby's last word: watching on; an older lobby's none)
	int rooms_version() const { return rooms_seen; }   // (a new list each time it comes: its page made again)
	void set_rooms(std::vector<RoomRow> r) { rooms_rows = std::move(r); rooms_said = 1; rooms_seen++; lobby_watch_on = true; }   // (tests: a list made up, as a lobby with watching gives)
	int local_port() const { return port; }
	static std::string lobby_default();   // the built-in lobby's address

private:
	m3::Model3 &board;
	std::unique_ptr<vfnet::UdpLink> link;
	vfnet::Link &lagged(vfnet::Link &l);
	std::unique_ptr<vfnet::RoomLink> room;
	std::unique_ptr<vfnet::LagLink> lag;   // (tests' worse network: VF3_NET_LAG)
	std::unique_ptr<vfnet::Session> session;
	Phase ph = Phase::off;
	Step st = Step::none;
	bool is_host = false, auto_delay = true, auto_done = false, rooms = false;
	int port = 0;
	std::string lan, joining_to;
	double now = 0, logged_at = 0, began_at = 0;
	LastMatch last;
	std::string failure;
	void ended(const std::string &why, bool lost = false);   // (the match over: kept as the last match)
	Step fail_step = Step::none;
	std::string note;
	bool local_slow = false;
	vf3net::Score score;
	mutable size_t start_scan[2] = {};   // (start_wait: how far each player's inputs have been looked through)
	mutable bool start_seen[2] = {};
	std::shared_ptr<std::atomic<int>> online_count = std::make_shared<std::atomic<int>>(-1);
	std::shared_ptr<std::atomic<bool>> online_asking = std::make_shared<std::atomic<bool>>(false);
	double online_asked = -1e18;   // (when, in ms)
	bool reopening = false;  // (opening: the same room again, after a player who couldn't join)
	int random_misses = 0;   // (random opponents in a row not reached)
	vfnet::AutoDelay adelay;   // (Auto's input delay through a match)
	int round_ends_seen = 0;
	uint32_t round_end_frame = 0;
	bool both_in = false;   // (both have been in the game at once, as its RAM has it: start_wait's rematches)
	mutable size_t out_scan[2] = { SIZE_MAX, SIZE_MAX };   // (a player out of the game: their inputs looked through from then)
	mutable bool out_pressed[2] = {};                     // (and they've pressed Start since: on their way back in)
	// (the frame each went out of the game as the RAM had it then, before the reading's held: out_scan from there, a
	// Start pressed in the moments before the held reading caught up counted; and who was in, the frame before)
	size_t out_from[2] = { SIZE_MAX, SIZE_MAX };
	mutable size_t out_held_at[2] = {};   // (and the frame the held reading had them out: the log's)
	uint8_t in_before = 0;
	uint32_t select_off_from = UINT32_MAX;   // (the frame the select's byte went off: lockstep kept SELECT_LINGER frames past it)
	int select_was = -1;   // (the select's byte last seen: its changes in the log)
	static constexpr uint32_t SELECT_LINGER = 120;
	mutable double stalls_mark = 0;                     // (connection: the frames held back, counted a second at a time)
	mutable double stalls_mark_at = -1, stalls_per_s = 0;
	Try tried = Try::none;
	std::string want_code;           // (host_room's want)
	std::string kept_code, kept_key;  // (the last room this side hosted: its code and key, for a Play again's)
	std::string listed_as;            // (a listed room's name as the lobby lists it: "" not listed)
	double patient_until = 0;
	std::string patient_for;         // (whose room's waited for)
	bool random = false;
	std::string ticket, country_me, country_peer;
	int players = 0;
	uint64_t logged_rb = 0, logged_resim = 0, logged_stalls = 0, logged_waits = 0, logged_steps = 0;
	double logged_save = 0, logged_run = 0, logged_resim_ms = 0, logged_hash = 0;
	std::deque<Notice> notices;
	vfnet::Config config(const OnlineSettings &s) const;
	bool open_host(std::string &error);
	OnlineSettings started_with;
	vfnet::Link &the_link() { return room ? static_cast<vfnet::Link &>(*room) : static_cast<vfnet::Link &>(*link); }

	// ---- a room code's match ----
	struct Job;                         // a request to the lobby, on a thread of its own
	static std::function<void(Job &)> gather(const std::string &base, const std::string &body);
	std::string turn_json() const;
	std::shared_ptr<Job> job;
	std::string lobby, code, key, peer_name;
	uint64_t room_token = 0;
	double step_at = 0, retry_at = 0;
	int wait_errors = 0;
	std::string relay_server, relay_user, relay_pass, relay_stun;   // (the relay's login, for asking again: Hide my IP's)
	int relay_asks = 0;
	std::string lobby_url(const std::string &path) const { return lobby + path; }
	std::string me_json(const std::string &extra) const;   // this side, as the lobby takes it
	void start_job(std::function<void(Job &)> work);
	void room_poll();
	void host_again(const std::string &why);
	void room_session(bool host);       // the session, with the lobby's token, on the room's link
	bool use_terms(const std::string &as, const std::string &them = "");   // the match's terms (the lobby's "as", or "": this side's own): the adapter set for them
	vfnet::Identity match_identity() const { vfnet::Identity i = identity; i.build = terms; return i; }
	std::string match_said() const;     // (what the match plays, said to both players: the stage select and the rest)
	void room_failed(const std::string &why);
	void joined(const vfnet::json::Value &v);   // (the room found: connecting to its host)
	void search();                      // (into the queue for a random opponent)
	void search_again(const std::string &why);   // (a random opponent connected but too far: another)
	void begin_match(const std::string &who);    // (connected, and the ping fine if it's measured first: playing)
	bool checking = false;              // (a random match connected, its ping measured before it begins: max_ping)
	double check_from = 0;
	std::string check_who;
	std::vector<std::pair<std::string, double>> avoid;   // (the rooms of random opponents too far, and when: the lobby keeps those apart)
	static std::string country(const std::string &c);
	void close_room();                  // (the host's: the lobby told, on a thread, nothing waited for)

	// ---- watching ----
	struct Fetch;                       // (a request about a watched match, on a thread of its own: the players never wait on one)
	static std::shared_ptr<Fetch> fetch(const std::string &method, const std::string &url, std::string body, int timeout_ms, size_t most);
	std::string wid, wkey;              // this match's watch code and the key the host sends it with (none: not watchable)
	vfnet::watch::Feed feed;            // (the host's: the match to the lobby)
	struct FeedSlot { std::shared_ptr<Fetch> job; vfnet::watch::Feed::Upload up; std::shared_ptr<vfnet::http::Client> client; };
	FeedSlot feed_slots[vfnet::watch::Feed::MOST_IN_FLIGHT];   // (the uploads on their way, each on a connection kept open)
	// (the host's WebSocket to its match's Watch, the uploads on it one at a time; none, or it failed: the HTTPS slots)
	std::shared_ptr<vfnet::http::Socket> feed_ws;
	bool feed_ws_failed = false, feed_ws_waiting = false;
	bool lobby_off(const vfnet::json::Value &v);   // (the lobby's answer says no more this month: true, the feed stopped)
	double feed_ws_sent = 0;
	vfnet::watch::Feed::Upload feed_ws_up;
	// (a watcher's WebSocket: the frames as they come and the lobby's word on who watches; refused or failed: the stream)
	std::shared_ptr<vfnet::http::Socket> watch_ws;
	bool watch_ws_failed = false;
	double watch_ws_frames_at = 0;      // (the last frames on it)
	std::shared_ptr<Fetch> count_job;
	double count_at = 0;
	int guest_watchers = -1;
	mutable double cells_at = -1e18;    // (when the figures at the top were last asked for: the guest's count asked only while they're shown)
	// (a room code's host's WebSocket to its room, hibernating at the lobby: told of its player at once, and the room
	// held while it's open, a persistent one's through its matches; refused or failed: the HTTPS wait and hold)
	std::shared_ptr<vfnet::http::Socket> room_ws;
	std::string room_ws_for;            // (the code and key it's for)
	bool room_ws_failed = false;
	int room_ws_tries = 0;
	double room_ws_at = 0, room_ws_made = 0;
	bool room_ws_open() const;
	void room_ws_drop();
	bool room_ws_poll(vfnet::http::Reply &r);   // (a message on it, as a wait's answer: true, r it)
	void feed_begin();
	void feed_poll();
	void feed_last();                   // (the match over: said to the lobby)
	// a watcher's
	std::unique_ptr<vfnet::watch::Spectator> spec;
	std::shared_ptr<Fetch> seen_job, watch_job;
	struct Stream;                      // (the frames as they come: a response read on a thread of its own)
	std::shared_ptr<Stream> stream;
	double stream_retry = 0;
	void stream_poll();
	void stream_stop();
	std::string watch_sid, watch_want, watch_tried;   // (watch_tried: the code a watch that failed had: Try again)
	uint64_t watch_match = 0, watch_next = 0;   // (the match watched; a newer one the lobby says is on)
	double seen_at = -1e18, watch_since = 0, over_at = 0;
	double watch_heard = 0;           // (the lobby's last answer to this watcher: none for 20 s, watching stops)
	bool watch_live_ok = true;        // (the lobby lets this watcher have a stream: not when it's busy)
	bool watch_shown = false, watch_quick = false;   // (watch_quick: the lobby says it's a Quick match's: no room, nobody plays next)
	int watch_watchers = 0, watch_restarts = 0;
	vfnet::watch::Meta watch_meta;
	void watch_poll();
	void watch_failed(const std::string &why);
	void watch_over(const std::string &why);
	bool watch_start(const vfnet::watch::Start &s, std::string &why);
	// the rooms list
	std::shared_ptr<Fetch> rooms_job;
	std::vector<RoomRow> rooms_rows;
	double rooms_at = -1e18;
	int rooms_said = -1, rooms_seen = 0;
	bool lobby_watch_on = false;
	std::shared_ptr<Fetch> peek_job;
	std::string peek_code;
	void peek_poll();
	std::string rooms_region;
	int queue_at = 0, queue_len = 0;
	bool persist = false;               // (the room's own: kept through its match, opened again after it)
	double hold_at = 0;
};

namespace online_menu {

// The Online page (out of a match; and a watched match's start). changed: a setting changed (settings.ini to be saved).
menu_page page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed);
// Online > Rooms: public rooms to join and matches to watch, this game's (made again as the list comes: rooms_version)
menu_page rooms_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed);
// The menu's rows in a match (the game goes on under it): who and how the connection is, input delay, the
// connection display, leaving the match
void match_rows(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::vector<menu_item> &rows);
// The name as typed: upper case, the letters the game's HUD can show, at most vfnet::MAX_NAME
std::string clean_name(const std::string &s);
// An address as settings.ini has it: the letters one is typed with, lower case, at most 64
std::string clean_address(const std::string &s);
// A room's name as typed: a name's letters and ', upper case, at most 16
std::string clean_room(const std::string &s);
// A room code as typed: its letters only (no 0/O, 1/I/L), upper case, at most 6
std::string clean_code(const std::string &s);

} // namespace online_menu
