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
#include <algorithm>
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
};

class Online
{
public:
	enum class Phase { off, hosting, joining, playing };
	// Where a room code's match has got to (none: by address)
	enum class Step { none, getting_ready, opening, waiting, finding, connecting };
	struct Notice { enum Kind { joined, ended, refused, tip } kind; std::string text; };   // (tip: a toast, nothing more)
	// The match that ended last: what ended it, who and how long, for the Online page to show after it (and offer
	// the same again)
	struct LastMatch
	{
		bool any = false, was_host = false, by_code = false;
		bool lost = false;            // the connection lost (else left, or out of step)
		bool relay = false;           // it went through a relay
		std::string why, opponent;
		std::string code, address;    // the room's code, or the address joined (for the same again)
		int seconds = 0, ping = 0;
	};
	// What was tried last (for Try again): hosting or joining, with a code or by address
	enum class Try { none, host_room, join_room, host, join };

	explicit Online(m3::Model3 &board) : adapter(board), board(board) {}
	~Online();
	vf3net::Adapter adapter;      // (its run, settle and program_changed set by main)
	vfnet::Identity identity;     // the game, build and ROM hash (main's); the name from the settings

	// With a room code: open a room (the code comes in a moment: room_code()), or join one
	// (want: the room's code from before, kept if it's free: a match's Play again; patient: no room yet is waited for,
	// a minute, the host opening it again)
	bool host_room(const OnlineSettings &s, std::string &error, const std::string &want = "");
	bool join_room(const OnlineSettings &s, const std::string &code, std::string &error, bool patient = false);
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
	bool is_hosting() const { return is_host; }
	std::string room_code() const;   // "ABC-DEF" ("" until the lobby's given one)
	bool playing() const { return ph == Phase::playing; }
	bool waiting() const { return ph == Phase::hosting || ph == Phase::joining; }
	// the match's game is set up and waits for the other side: no frame may run on the board meanwhile
	bool starting() const { return waiting() && session && session->game_started(); }
	bool interrupted() const { return session && session->interrupted(); }
	bool via_relay() const { return room && room->via_relay(); }
	std::string status() const;   // a line for the menu: what's happening now
	// The connection display: "MRFINSTER  42 ms  rollback 2", and how good (0 good, 1 fair, 2 poor)
	std::string connection(int &level) const;
	// The details under it: the input delay, the jitter, the rollbacks (average and the most), how far ahead, and
	// what a frame's step costs here
	std::string connection_detail() const;
	// the other player's name, as the HUD can show it (only its letters: whatever else the other side sent, dropped)
	std::string opponent() const;
	static bool local_address(const std::string &address);   // (a private network's: 192.168.x.x and the like)
	int local_player() const { return is_host ? 0 : 1; }   // (the host is player 1)
	int delay_now() const { return session ? session->delay() : 0; }
	void set_delay(int setting);  // the menu's, in a match (Auto: from the ping)
	int ping() const { return session ? int(session->stats().rtt_ms + 0.5) : 0; }
	// In a match: how much slower to run the frames to keep in time with the other side (0, or up to 0.02: 2%)
	// (gently: much more and the sound's queue ran dry, and its catching up ran frames in a burst)
	double slow_down() const { return playing() && session ? std::clamp(session->stats().lead * 0.01, 0.0, 0.02) : 0.0; }
	const LastMatch &last_match() const { return last; }
	// Why the last try didn't get going ("" if it did, or none yet): shown on the page it was made from
	const std::string &join_failure() const { return failure; }
	Step failed_at() const { return fail_step; }   // (how far the failed try got)
	Try last_try() const { return tried; }
	bool patient() const { return now < patient_until; }   // (joining a room that isn't open yet: waiting for it)
	std::string peer() const;     // the other player's name, as the lobby or the session has it ("" not yet)
	// In a match whose connection's gone quiet: the seconds left before it's given up
	int hold_left() const { return session ? session->quiet_left_s() : 0; }
	void set_last_match(const LastMatch &m) { last = m; }   // ({}: forgotten, the menu closed after it; tests: one made up)
	void forget_failure() { failure.clear(); }              // (the menu closed after it)
	const std::string &local_address() const { return lan; }
	int local_port() const { return port; }
	static std::string lobby_default();   // the built-in lobby's address

private:
	m3::Model3 &board;
	std::unique_ptr<vfnet::UdpLink> link;
	std::unique_ptr<vfnet::RoomLink> room;
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
	Try tried = Try::none;
	std::string want_code;           // (host_room's want)
	double patient_until = 0;
	std::string patient_for;         // (whose room's waited for)
	uint64_t logged_rb = 0, logged_resim = 0, logged_stalls = 0, logged_waits = 0, logged_steps = 0;
	double logged_save = 0, logged_run = 0, logged_resim_ms = 0, logged_hash = 0;
	std::deque<Notice> notices;
	vfnet::Config config(const OnlineSettings &s) const;
	bool open_host(std::string &error);
	OnlineSettings started_with;
	vfnet::Link &the_link() { return room ? static_cast<vfnet::Link &>(*room) : static_cast<vfnet::Link &>(*link); }

	// ---- a room code's match ----
	struct Job;                         // a request to the lobby, on a thread of its own
	std::shared_ptr<Job> job;
	std::string lobby, code, key, peer_name;
	uint64_t room_token = 0;
	double step_at = 0, retry_at = 0;
	int wait_errors = 0;
	std::string lobby_url(const std::string &path) const { return lobby + path; }
	std::string me_json(const std::string &extra) const;   // this side, as the lobby takes it
	void start_job(std::function<void(Job &)> work);
	void room_poll();
	void room_session(bool host);       // the session, with the lobby's token, on the room's link
	void room_failed(const std::string &why);
	void close_room();                  // (the host's: the lobby told, on a thread, nothing waited for)
};

namespace online_menu {

// The Online page (out of a match). changed: a setting changed (settings.ini to be saved).
menu_page page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed);
// The menu's rows in a match (the game goes on under it): who and how the connection is, input delay, the
// connection display, leaving the match
void match_rows(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::vector<menu_item> &rows);
// The name as typed: upper case, the letters the game's HUD can show, at most vfnet::MAX_NAME
std::string clean_name(const std::string &s);
// An address as settings.ini has it: the letters one is typed with, lower case, at most 64
std::string clean_address(const std::string &s);
// A room code as typed: its letters only (no 0/O, 1/I/L), upper case, at most 6
std::string clean_code(const std::string &s);

} // namespace online_menu
