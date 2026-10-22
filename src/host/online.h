// Online play in the game: the menu's Online pages, and what runs a match (the socket, the netplay session and
// the board's adapter). main.cpp calls poll() every pass, and in a match step() for each frame instead of running
// it, and shows the notices. The lobby server (rooms by code, quick match, watching) comes later: for now a host
// waits on a port and a guest joins by address.
#pragma once
#include "menu.h"
#include "vf3_netplay.h"
#include "../net/link.h"
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
	int port = 7700;              // the host's UDP port
	std::string address;          // the last address joined
};

class Online
{
public:
	enum class Phase { off, hosting, joining, playing };
	struct Notice { enum Kind { joined, ended, refused } kind; std::string text; };
	// The match that ended last: what ended it, who and how long, for the Online page to show after it (and offer
	// the same again)
	struct LastMatch
	{
		bool any = false, was_host = false;
		std::string why, opponent;
		int seconds = 0, ping = 0;
	};

	explicit Online(m3::Model3 &board) : adapter(board), board(board) {}
	vf3net::Adapter adapter;      // (its run, settle and program_changed set by main)
	vfnet::Identity identity;     // the game, build and ROM hash (main's); the name from the settings

	bool host(const OnlineSettings &s, std::string &error);
	bool join(const OnlineSettings &s, std::string &error);
	void stop();                  // stop waiting, or leave the match (the other side is told)
	void poll(double now_ms);
	// One frame in a match with this player's input; false if it couldn't run yet (the picture stays)
	bool step(uint16_t input);
	bool next_notice(Notice &n);

	Phase phase() const { return ph; }
	bool playing() const { return ph == Phase::playing; }
	bool waiting() const { return ph == Phase::hosting || ph == Phase::joining; }
	// the match's game is set up and waits for the other side: no frame may run on the board meanwhile
	bool starting() const { return waiting() && session && session->game_started(); }
	bool interrupted() const { return session && session->interrupted(); }
	std::string status() const;   // a line for the menu: what's happening now
	// The connection display: "MRFINSTER  42 ms  rollback 2", and how good (0 good, 1 fair, 2 poor)
	std::string connection(int &level) const;
	// The details under it: the input delay, the jitter, the rollbacks (average and the most), how far ahead, and
	// what a frame's step costs here
	std::string connection_detail() const;
	// the other player's name, as the HUD can show it (only its letters: whatever else the other side sent, dropped)
	std::string opponent() const;
	int local_player() const { return is_host ? 0 : 1; }   // (the host is player 1)
	int delay_now() const { return session ? session->delay() : 0; }
	void set_delay(int setting);  // the menu's, in a match (Auto: from the ping)
	int ping() const { return session ? int(session->stats().rtt_ms + 0.5) : 0; }
	// In a match: how much slower to run the frames to keep in time with the other side (0, or up to 0.02: 2%)
	// (gently: much more and the sound's queue ran dry, and its catching up ran frames in a burst)
	double slow_down() const { return playing() && session ? std::clamp(session->stats().lead * 0.01, 0.0, 0.02) : 0.0; }
	const LastMatch &last_match() const { return last; }
	void set_last_match(const LastMatch &m) { last = m; }   // ({}: forgotten, the menu closed after it; tests: one made up)
	const std::string &local_address() const { return lan; }
	int local_port() const { return port; }

private:
	m3::Model3 &board;
	std::unique_ptr<vfnet::UdpLink> link;
	std::unique_ptr<vfnet::Session> session;
	Phase ph = Phase::off;
	bool is_host = false, auto_delay = true, auto_done = false;
	int port = 0;
	std::string lan, joining_to;
	double now = 0, logged_at = 0, began_at = 0;
	LastMatch last;
	void ended(const std::string &why);   // (the match over: kept as the last match)
	uint64_t logged_rb = 0, logged_resim = 0, logged_stalls = 0, logged_waits = 0, logged_steps = 0;
	double logged_save = 0, logged_run = 0, logged_resim_ms = 0, logged_hash = 0;
	std::deque<Notice> notices;
	vfnet::Config config(const OnlineSettings &s) const;
	bool open_host(std::string &error);
	OnlineSettings started_with;
};

namespace online_menu {

// The Online page (out of a match). changed: a setting changed (settings.ini to be saved).
menu_page page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed);
// The row the Online page starts on: waiting, its Stop; after a match, the one that plays again
int first_row(const Online &o);
// The menu's rows in a match (the game goes on under it): who and how the connection is, input delay, the
// connection display, leaving the match
void match_rows(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::vector<menu_item> &rows);
// The name as typed: upper case, the letters the game's HUD can show, at most vfnet::MAX_NAME
std::string clean_name(const std::string &s);
// An address as settings.ini has it: the letters one is typed with, lower case, at most 64
std::string clean_address(const std::string &s);

} // namespace online_menu
