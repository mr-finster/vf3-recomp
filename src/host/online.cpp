#include "online.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

// ---- the match ----

vfnet::Config Online::config(const OnlineSettings &s) const
{
	vfnet::Config c;
	c.delay = s.delay < 0 ? 2 : std::clamp(s.delay, 0, 6);   // (Auto starts at 2, then follows the ping)
	c.soft_sync = true;   // (main's frame clock slows a little while ahead: no frame held back, no hitch)
	return c;
}

bool Online::open_host(std::string &error)
{
	session.reset();
	link = std::make_unique<vfnet::UdpLink>();
	if (!link->bind(uint16_t(port), error)) { link.reset(); return false; }
	if (adapter.settle) adapter.settle();
	identity.name = started_with.name;
	session = std::make_unique<vfnet::Session>(*link, adapter, identity, config(started_with), true, vf3net::start_blob(board));
	session->trace = [](const std::string &t) { fprintf(stderr, "online: %s\n", t.c_str()); };
	ph = Phase::hosting;
	return true;
}

void Online::ended(const std::string &why)
{
	last.any = true;
	last.was_host = is_host;
	last.why = why;
	last.opponent = session ? session->opponent_name() : "";
	last.seconds = int((now - began_at) / 1000);
	last.ping = ping();
	fprintf(stderr, "online: the match with %s ended after %d s: %s\n", last.opponent.c_str(), last.seconds, why.c_str());
}

bool Online::host(const OnlineSettings &s, std::string &error)
{
	stop();
	last = {};
	started_with = s;
	is_host = true;
	auto_delay = s.delay < 0;
	auto_done = false;
	port = std::clamp(s.port, 1024, 65535);
	lan = vfnet::lan_address();
	if (!open_host(error)) return false;
	fprintf(stderr, "online: hosting on UDP port %d (%s)\n", port, lan.empty() ? "no network address" : lan.c_str());
	return true;
}

bool Online::join(const OnlineSettings &s, std::string &error)
{
	stop();
	last = {};
	started_with = s;
	is_host = false;
	auto_delay = s.delay < 0;
	auto_done = false;
	// "host", "host:port", "[v6]:port" or a bare IPv6 address
	std::string host = s.address;
	int p = 7700;
	const size_t colons = size_t(std::count(host.begin(), host.end(), ':'));
	if (!host.empty() && host[0] == '[') {
		const size_t close = host.find(']');
		if (close == std::string::npos) { error = "That address isn't complete: a ] is missing."; return false; }
		if (close + 2 < host.size() && host[close + 1] == ':') p = atoi(host.c_str() + close + 2);
		host = host.substr(1, close - 1);
	} else if (colons == 1) {
		p = atoi(host.c_str() + host.find(':') + 1);
		host = host.substr(0, host.find(':'));
	}
	if (host.empty()) { error = "Type the host's address first."; return false; }
	if (p < 1 || p > 65535) { error = "The port after the : is a number from 1 to 65535."; return false; }
	link = std::make_unique<vfnet::UdpLink>();
	if (!link->connect(host, uint16_t(p), error)) { link.reset(); return false; }
	identity.name = s.name;
	session = std::make_unique<vfnet::Session>(*link, adapter, identity, config(s), false);
	session->trace = [](const std::string &t) { fprintf(stderr, "online: %s\n", t.c_str()); };
	joining_to = host + (p != 7700 ? ":" + std::to_string(p) : "");
	ph = Phase::joining;
	fprintf(stderr, "online: joining %s, port %d\n", host.c_str(), p);
	return true;
}

void Online::stop()
{
	if (session && ph == Phase::playing) ended("You left the match.");
	if (session && session->state() != vfnet::Session::State::ended) session->quit();
	session.reset();
	link.reset();
	ph = Phase::off;
}

void Online::poll(double t)
{
	now = t;
	if (!session) return;
	session->poll(t);
	vfnet::Session::Event e;
	while (session && session->next_event(e)) {
		switch (e.kind) {
		case vfnet::Session::Event::connected:
			ph = Phase::playing;
			began_at = now;
			notices.push_back({ Notice::joined, "Playing " + e.text + ": press Start" });
			break;
		case vfnet::Session::Event::ended: {
			const auto why = session->end_reason();
			const bool was_playing = ph == Phase::playing;
			// a host goes on waiting after a player who couldn't join (another version, another ROM, gone quiet
			// before the game started); anything else ends it
			if (!was_playing && is_host && (why == vfnet::Session::End::rejected || why == vfnet::Session::End::timeout || why == vfnet::Session::End::opponent_quit)) {
				notices.push_back({ Notice::refused, "A player couldn't join: " + e.text });
				std::string err;
				if (!open_host(err)) { notices.push_back({ Notice::ended, err }); session.reset(); link.reset(); ph = Phase::off; }
				return;
			}
			if (was_playing && why != vfnet::Session::End::quit) {
				const std::string who = session->opponent_name().empty() ? std::string("your opponent") : session->opponent_name();
				ended(why == vfnet::Session::End::opponent_quit ? who + " left the match."
				      : why == vfnet::Session::End::disconnected ? "Lost the connection to " + who + "."
				      : why == vfnet::Session::End::desync ? "The match went out of step." : e.text);
			}
			if (why != vfnet::Session::End::quit)
				notices.push_back({ was_playing ? Notice::ended : Notice::refused, was_playing ? last.why : e.text });
			session.reset();
			link.reset();
			ph = Phase::off;
			return;
		}
		default:
			break;
		}
	}
	// the connection in the log every 10 s of a match (what a report needs: the ping, the rollbacks, the waits)
	if (session && ph == Phase::playing && now - logged_at >= 10000) {
		const auto &st = session->stats();
		if (logged_at > 0)
			fprintf(stderr, "online: frame %u, ping %.0f ms, delay %d, %llu rollbacks (%.1f frames on average, %d the most in the last second), "
			        "%llu stalls for the other side, %llu waits to keep in time, %.1f frames ahead\n", session->frame(), st.rtt_ms, session->delay(),
			        (unsigned long long)(st.rollbacks - logged_rb), st.rollbacks > logged_rb ? double(st.resim_frames - logged_resim) / double(st.rollbacks - logged_rb) : 0.0,
			        st.rollback_max, (unsigned long long)(st.stalls - logged_stalls), (unsigned long long)(st.waits - logged_waits), double(st.ahead));
		logged_at = now;
		logged_rb = st.rollbacks;
		logged_resim = st.resim_frames;
		logged_stalls = st.stalls;
		logged_waits = st.waits;
	}
	// Auto's input delay, once the ping is known (the character select): half the round trip in frames, 1 to 4
	if (session && ph == Phase::playing && auto_delay && !auto_done && session->frame() > 120 && session->stats().rtt_ms > 0) {
		const double frame_ms = 1000.0 / 57.524160;
		session->set_delay(std::clamp(int(std::ceil(session->stats().rtt_ms / 2 / frame_ms)), 1, 4));
		auto_done = true;
	}
}

bool Online::step(uint16_t input)
{
	return ph == Phase::playing && session && session->step(input);
}

bool Online::next_notice(Notice &n)
{
	if (notices.empty()) return false;
	n = notices.front();
	notices.pop_front();
	return true;
}

void Online::set_delay(int setting)
{
	auto_delay = setting < 0;
	auto_done = false;
	if (session && !auto_delay) session->set_delay(setting);
}

std::string Online::status() const
{
	if (!session) return "";
	const std::string who = session->opponent_name();
	switch (ph) {
	case Phase::hosting:
		if (who.empty()) return "Waiting for a player to join...";
		return who + " is joining...";
	case Phase::joining:
		if (session->game_started()) return "Starting the game...";
		if (!who.empty()) return "Getting " + who + "'s settings...";
		return "Connecting to " + joining_to + "...";
	case Phase::playing:
		return "Playing " + who;
	default:
		return "";
	}
}

std::string Online::connection(int &level) const
{
	level = 0;
	if (!session || ph != Phase::playing) return "";
	const auto &st = session->stats();
	if (session->interrupted()) { level = 2; return session->opponent_name() + "   connection interrupted"; }
	const int ms = int(st.rtt_ms + 0.5), rb = st.rollback_max;
	level = ms < 60 && rb <= 2 ? 0 : ms < 120 && rb <= 4 ? 1 : 2;
	return session->opponent_name() + "   " + std::to_string(ms) + " ms   rollback " + std::to_string(rb);
}

// ---- the menu ----

namespace online_menu {

int first_row(const Online &o);

namespace {

const std::string NAME_LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-!?";
const std::string ADDRESS_LETTERS = "0123456789.:abcdefghijklmnopqrstuvwxyz-[]";

// What's being typed into a row, and the mistake to show under it
struct Typing
{
	bool on = false;
	std::string text, before, error;
};

// Typing into a row. A keyboard types (Backspace takes a letter off, Enter is done, Esc puts it back as it was);
// a pad: Up / Down change the last letter, Right adds one, Left takes one off, Y clears, A done, B as it was.
// done: true if what's typed will do (else it says why in error, and typing goes on).
void start_typing(game_menu &menu, std::shared_ptr<Typing> t, const std::string &letters, size_t max, bool upper,
                  std::function<bool(const std::string &, std::string &)> done)
{
	t->on = true;
	t->before = t->text;
	t->error.clear();
	SDL_Window *win = menu.window();
	if (win) SDL_StartTextInput(win);
	auto finish = [t, win] { t->on = false; if (win) SDL_StopTextInput(win); };
	menu.capture("Type, or on a pad: Up / Down a letter, Right adds one, Left takes one off. A: done, B: as it was.",
		[&menu, t, letters, max, upper, done, finish](const SDL_Event &e) -> bool {
			bool enter = false;
			auto add = [&](char ch) {
				if (upper) ch = char(toupper(uint8_t(ch)));
				else ch = char(tolower(uint8_t(ch)));
				if (letters.find(ch) != std::string::npos && t->text.size() < max) t->text += ch;
			};
			auto turn = [&](int d) {
				if (t->text.empty()) { t->text.push_back(letters[0]); return; }
				const size_t at = letters.find(t->text.back());
				const int n = int(letters.size());
				t->text.back() = letters[size_t(((at == std::string::npos ? 0 : int(at)) + d + n) % n)];
			};
			if (e.type == SDL_EVENT_TEXT_INPUT) {
				for (const char *p = e.text.text; p && *p; p++) add(*p);
			} else if (e.type == SDL_EVENT_KEY_DOWN) {
				if (e.key.scancode == SDL_SCANCODE_BACKSPACE) { if (!t->text.empty()) t->text.pop_back(); }
				else if (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER) enter = true;
				else return false;   // (a letter: its text comes as text input)
			} else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				switch (e.gbutton.button) {
				case SDL_GAMEPAD_BUTTON_SOUTH: enter = true; break;
				case SDL_GAMEPAD_BUTTON_EAST: t->text = t->before; finish(); return true;
				case SDL_GAMEPAD_BUTTON_NORTH: t->text.clear(); break;
				case SDL_GAMEPAD_BUTTON_DPAD_UP: turn(+1); break;
				case SDL_GAMEPAD_BUTTON_DPAD_DOWN: turn(-1); break;
				case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (t->text.size() < max) t->text.push_back(t->text.empty() ? letters[0] : t->text.back()); break;
				case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (!t->text.empty()) t->text.pop_back(); break;
				default: return false;
				}
			} else
				return false;
			t->error.clear();
			if (enter) {
				std::string why;
				if (done(t->text, why)) { finish(); return true; }
				t->error = why;
			}
			menu.refresh();
			return false;
		},
		[t, win] { t->text = t->before; t->on = false; t->error.clear(); if (win) SDL_StopTextInput(win); });
	menu.refresh();
}

menu_item delay_row(Online &o, OnlineSettings &s, std::function<void()> changed)
{
	return { "Input delay",
		"The frames between pressing a button and it happening, on your side only. Fewer: the game answers sooner, "
		"and the other player's moves can jump a little more (rollback). Auto picks 1 to 4 from the ping.",
		[&o, &s] {
			if (s.delay >= 0) return std::to_string(s.delay) + (s.delay == 1 ? " frame" : " frames");
			return o.playing() ? "Auto (" + std::to_string(o.delay_now()) + ")" : std::string("Auto");
		},
		[&o, &s, changed](int d) {
			s.delay = std::clamp(s.delay + d, -1, 6);
			if (o.playing()) o.set_delay(s.delay);
			changed();
		} };
}

menu_item connection_row(OnlineSettings &s, std::function<void()> changed)
{
	return { "Connection display", "In a match, the other player's name, the ping and the rollback, small at the top of the screen; "
		"the dot green, yellow or red for how good the connection is.",
		[&s] { return std::string(s.show_connection ? "On" : "Off"); },
		[&s, changed](int) { s.show_connection = !s.show_connection; changed(); } };
}

menu_page join_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Join a match", "", {} };
	auto addr = std::make_shared<Typing>();
	addr->text = s.address;
	auto problem = std::make_shared<std::string>();
	menu_item a{ "Address",
		"The host's address, as their Online page shows it: four numbers with dots (192.168.1.20), a name, or either with :port "
		"if they changed it from 7700. A: type it.",
		[addr] { return addr->on ? addr->text + "_" : addr->text.empty() ? std::string("Not set") : addr->text; },
		{},
		[&menu, &s, addr, changed] {
			start_typing(menu, addr, ADDRESS_LETTERS, 64, false, [&s, changed](const std::string &v, std::string &why) {
				if (v.empty()) { why = "Type the host's address."; return false; }
				s.address = v;
				changed();
				return true;
			});
		} };
	a.below = [addr] { return addr->error; };
	p.items.push_back(a);
	menu_item c{ "Connect", "Joins the host's match. Once you're both in, the game starts at its title screen: both press Start.", {}, {},
		[&o, &s, problem] {
			problem->clear();
			if (s.address.empty()) { *problem = "Type the host's address first."; return; }
			std::string err;
			if (!o.join(s, err)) *problem = err;
		} };
	c.below = [&o, problem] { return !problem->empty() ? *problem : o.phase() == Online::Phase::joining ? o.status() : std::string(); };
	c.good = [problem] { return problem->empty(); };
	p.items.push_back(c);
	p.items.push_back({ "Stop", "Stops trying to join.", {}, {}, [&o, problem] { o.stop(); problem->clear(); } });
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	return p;
}

} // namespace

std::string clean_name(const std::string &s)
{
	std::string out;
	for (char ch : s) {
		ch = char(toupper(uint8_t(ch)));
		if (NAME_LETTERS.find(ch) != std::string::npos && out.size() < size_t(vfnet::MAX_NAME)) out += ch;
	}
	while (!out.empty() && out.back() == ' ') out.pop_back();
	while (!out.empty() && out.front() == ' ') out.erase(out.begin());
	return out;
}

menu_page page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Online", "", {} };
	auto name = std::make_shared<Typing>();
	name->text = s.name;
	auto type_name = [&menu, &s, name, changed](std::function<void()> then) {
		menu.select_row(0);   // (the name's row: it's what's being typed into)
		start_typing(menu, name, NAME_LETTERS, size_t(vfnet::MAX_NAME), true, [&s, changed, then](const std::string &v, std::string &why) {
			const std::string n = clean_name(v);
			if (n.empty()) { why = "Your name needs a letter or a number."; return false; }
			s.name = n;
			changed();
			if (then) then();
			return true;
		});
	};
	menu_item n{ "Your name", "Up to 10 letters, shown under your energy bar in a match, and to the player you play. A: type it.",
		[name, &s] { return name->on ? name->text + "_" : s.name.empty() ? std::string("Not set") : s.name; },
		{}, [type_name] { type_name(nullptr); } };
	n.below = [name] { return name->error; };
	p.items.push_back(n);

	auto problem = std::make_shared<std::string>();
	if (o.phase() == Online::Phase::hosting) {
		p.items.push_back({ "WAITING FOR A PLAYER", "", {}, {}, {}, true });
		p.items.back().heading = true;
		const std::string here = o.local_address().empty() ? std::string("No network") : o.local_address() + (o.local_port() != 7700 ? ":" + std::to_string(o.local_port()) : "");
		p.items.push_back({ here, "", {}, {}, {}, true });
		p.items.back().big = true;
		p.items.push_back({ "Players on your network join with this address.", "", {}, {}, {}, true });
		p.items.push_back({ "Over the internet: forward UDP port " + std::to_string(o.local_port()) + " to this computer, and", "", {}, {}, {}, true });
		p.items.push_back({ "give them your public address. (The lobby, coming next, does this for you.)", "", {}, {}, {}, true });
		menu_item w{ "Stop waiting", "Stops hosting. You can play on your own meanwhile: close the menu, and the match starts when someone joins.", {}, {},
			[&o, &s, &menu, changed] { o.stop(); menu.replace(page(o, s, menu, changed), first_row(o)); } };
		w.below = [&o] { return o.status(); };
		w.good = [] { return true; };
		p.items.push_back(w);
	} else if (o.phase() == Online::Phase::joining) {
		p.items.push_back({ "JOINING", "", {}, {}, {}, true });
		p.items.back().heading = true;
		menu_item to{ s.address, "", {}, {}, {}, true };
		to.big = true;
		p.items.push_back(to);
		menu_item w{ "Stop joining", "Stops trying to join.", {}, {}, [&o, &s, &menu, changed] { o.stop(); menu.replace(page(o, s, menu, changed), first_row(o)); } };
		w.below = [&o] { return o.status(); };
		w.good = [] { return true; };
		p.items.push_back(w);
	} else {
		if (const Online::LastMatch &m = o.last_match(); m.any) {   // (just after a match: what happened, and the same again)
			p.items.push_back({ "MATCH OVER", "", {}, {}, {}, true });
			p.items.back().heading = true;
			menu_item why{ m.why, "", {}, {}, {}, true };
			why.big = m.why.size() <= 28;
			p.items.push_back(why);
			char line[96];
			snprintf(line, sizeof line, "%s%s  \u00b7  %d:%02d played  \u00b7  %d ms", m.opponent.empty() ? "" : "Against ", m.opponent.c_str(), m.seconds / 60, m.seconds % 60, m.ping);
			p.items.push_back({ line, "", {}, {}, {}, true });
			if (m.was_host)
				p.items.push_back({ "Host again", "Waits for a player to join you again, as before (" + m.opponent + " can join with the same address).", {}, {},
					[&o, &s, &menu, problem, changed] {
						std::string err;
						if (!o.host(s, err)) { *problem = err; menu.refresh(); return; }
						menu.replace(page(o, s, menu, changed), first_row(o));
					} });
			else
				p.items.push_back({ "Join " + (m.opponent.empty() ? std::string("again") : m.opponent + " again"),
					"Joins " + s.address + " again: ask " + (m.opponent.empty() ? std::string("the host") : m.opponent) + " to host again first.", {}, {},
					[&o, &s, &menu, problem, changed] {
						std::string err;
						if (!o.join(s, err)) { *problem = err; menu.refresh(); return; }
						menu.replace(page(o, s, menu, changed), first_row(o));
					} });
			p.items.back().below = [&o, problem] { return o.waiting() ? o.status() : *problem; };
		}
		menu_item h{ "Host a match", "Waits for a player to join you (play on your own meanwhile). Matches always play the PC way, "
			"free play, with your arcade settings (difficulty, rounds, time). You're player 1, with your own controls.", {}, {},
			[&o, &s, &menu, problem, changed, type_name] {
				auto go = [&o, &s, &menu, problem, changed] {
					std::string err;
					if (!o.host(s, err)) { *problem = err; menu.refresh(); return; }
					menu.replace(page(o, s, menu, changed), first_row(o));
				};
				if (s.name.empty()) { *problem = "Your name first: type it above."; type_name(go); return; }
				go();
			} };
		h.below = [problem] { return *problem; };
		p.items.push_back(h);
		p.items.push_back({ "Join a match", "Joins a player who's hosting, by their address. Matches always play the PC way, free play, "
			"with the host's arcade settings. You're player 2, with your own controls: Player 1's keys or pad work.", {}, {},
			[&o, &s, &menu, problem, changed, type_name] {
				auto go = [&o, &s, &menu, changed] { menu.push(join_page(o, s, menu, changed)); };
				if (s.name.empty()) { *problem = "Your name first: type it above."; type_name(go); return; }
				go();
			} });
	}
	p.items.push_back({ "SETTINGS", "", {}, {}, {}, true });
	p.items.back().heading = true;
	p.items.push_back(delay_row(o, s, changed));
	p.items.push_back(connection_row(s, changed));
	p.items.push_back({ "Port", "The UDP port a host waits on (7700 unless another program has it). Forward this one on your router "
		"to host over the internet.", [&s] { return std::to_string(s.port); },
		[&s, changed](int d) { s.port = std::clamp(s.port + d, 1024, 65535); changed(); } });
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	return p;
}

int first_row(const Online &o)
{
	switch (o.phase()) {
	case Online::Phase::hosting: return 6;   // (Stop waiting)
	case Online::Phase::joining: return 3;   // (Stop joining)
	default: return o.last_match().any ? 4 : 1;   // (Host / Join again, else Host a match)
	}
}

void match_rows(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::vector<menu_item> &rows)
{
	menu_item who{ "Playing", "", [&o] {
		return o.opponent() + "   " + std::to_string(o.ping()) + " ms" + (o.interrupted() ? "   interrupted" : "");
	}, {}, {}, true };
	rows.push_back(who);
	rows.push_back(delay_row(o, s, changed));
	rows.push_back(connection_row(s, changed));
	rows.push_back({ "Leave the match", "Ends the match for both of you. Your game goes back to playing on its own.", {}, {},
		[&o, &menu] {
			menu_page q{ "Leave the match?", "Your opponent is told, and your game goes back to playing on its own.", {} };
			q.items.push_back({ "Stay", "", {}, {}, [&menu] { menu.pop(); } });   // (first: a slip of the thumb stays)
			q.items.push_back({ "Leave", "", {}, {}, [&o, &menu] { o.stop(); menu.close(); } });
			menu.push(q);
		} });
}

} // namespace online_menu
