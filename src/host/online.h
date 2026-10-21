// Online play in the game: the menu's Online pages, and what runs a match (the socket, the netplay session and
// the board's adapter). main.cpp calls poll() every pass, and in a match step() for each frame instead of running
// it, and shows the notices. The lobby server (rooms by code, quick match, watching) comes later: for now a host
// waits on a port and a guest joins by address.
#pragma once
#include "menu.h"
#include "vf3_netplay.h"
#include "../net/link.h"
#include "../net/session.h"
#include <deque>
#include <functional>
#include <memory>
#include <string>

// The Online settings, kept in settings.ini
struct OnlineSettings
{
	std::string name;             // up to vfnet::MAX_NAME letters, shown under the energy bar
	int delay = -1;               // input delay: -1 Auto (from the ping), else 0-6 frames
	bool show_connection = true;  // the ping and rollback at the top in a match
	int port = 7700;              // the host's UDP port
	std::string address;          // the last address joined
};

class Online
{
public:
	enum class Phase { off, hosting, joining, playing };
	struct Notice { enum Kind { joined, ended, refused } kind; std::string text; };

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
	std::string opponent() const { return session ? session->opponent_name() : ""; }
	int delay_now() const { return session ? session->delay() : 0; }
	void set_delay(int setting);  // the menu's, in a match (Auto: from the ping)
	int ping() const { return session ? int(session->stats().rtt_ms + 0.5) : 0; }
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
	double now = 0;
	std::deque<Notice> notices;
	vfnet::Config config(const OnlineSettings &s) const;
	bool open_host(std::string &error);
	OnlineSettings started_with;
};

namespace online_menu {

// The Online page (out of a match). changed: a setting changed (settings.ini to be saved).
menu_page page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed);
// The menu's rows in a match (the game goes on under it): who and how the connection is, input delay, the
// connection display, leaving the match
void match_rows(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::vector<menu_item> &rows);
// The name as typed: upper case, the letters the game's HUD can show, at most vfnet::MAX_NAME
std::string clean_name(const std::string &s);

} // namespace online_menu
