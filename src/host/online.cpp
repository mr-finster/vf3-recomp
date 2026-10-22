#include "online.h"
#include "../net/http.h"
#include "../net/json.h"
#include "version.h"   // VF_LOBBY_URL (made by the Makefile)
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

// The lobby built in (tools/lobby, deployed: tools/lobby/README.md; the build's VF_LOBBY_URL); settings.ini's
// online_lobby, or VF_LOBBY in the environment, for another
const char *const LOBBY = VF_LOBBY_URL;
const char *const STUN_HOST = "stun.cloudflare.com";
const int STUN_PORT = 3478;
const std::string CODE_LETTERS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";   // (the lobby's: no 0/O, 1/I/L)

// An address for the log, which players post with their reports: the last part of it hidden
std::string mask(const std::string &a)
{
	if (a.empty()) return "none";
	if (a[0] == '[') return a.substr(0, std::min<size_t>(a.size(), 10)) + "...]" + a.substr(a.find(']') + 1);
	const size_t dot = a.rfind('.'), colon = a.rfind(':');
	if (dot == std::string::npos || colon == std::string::npos || colon < dot) return a;
	return a.substr(0, dot + 1) + "x" + a.substr(colon);
}

std::string masked(const std::vector<std::string> &v)
{
	std::string s;
	for (const std::string &a : v) s += (s.empty() ? "" : ", ") + mask(a);
	return s.empty() ? "none" : s;
}

std::string hex64(uint64_t v)
{
	char b[24];
	snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
	return b;
}

void log_line(const std::string &t) { fprintf(stderr, "online: %s\n", t.c_str()); }

} // namespace

struct Online::Job
{
	std::atomic<bool> done{ false };
	vfnet::http::Reply reply;
	std::string stun, turn, user, password;   // (getting ready's: the servers' addresses, the relay's login)
};

Online::~Online() = default;

std::string Online::lobby_default()
{
	const char *e = getenv("VF_LOBBY");
	std::string s = e && *e ? e : LOBBY;
	while (!s.empty() && s.back() == '/') s.pop_back();
	return s;
}

// ---- the match ----

vfnet::Config Online::config(const OnlineSettings &s) const
{
	vfnet::Config c;
	c.delay = s.delay < 0 ? 2 : std::clamp(s.delay, 0, 6);   // (Auto starts at 2, then follows the ping)
	c.checksum_every = 120;   // (the desync check's hash, 27 MB, every 2 s: every half second it was a frame's worst cost on a Steam Deck)
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
	session->trace = log_line;
	ph = Phase::hosting;
	return true;
}

void Online::ended(const std::string &why)
{
	last.any = true;
	last.was_host = is_host;
	last.by_code = rooms;
	last.why = why;
	last.opponent = opponent();
	last.seconds = int((now - began_at) / 1000);
	last.ping = ping();
	fprintf(stderr, "online: the match with %s ended after %d s: %s\n", last.opponent.c_str(), last.seconds, why.c_str());
}

// A private network's address (192.168.x.x, 10.x.x.x, 172.16-31.x.x: an iPhone's hotspot is 172.20.10.x; 169.254.x.x):
// reachable only from the same network. (Not 100.64-127.x.x: Tailscale's, which reach across the internet.)
bool Online::local_address(const std::string &a)
{
	unsigned b[4];
	char tail = 0;
	if (sscanf(a.c_str(), "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) < 4) return false;
	return b[0] == 10 || (b[0] == 192 && b[1] == 168) || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 169 && b[1] == 254);
}

std::string Online::opponent() const
{
	if (!session) return "";
	std::string n = online_menu::clean_name(session->opponent_name());
	if (n.empty()) n = online_menu::clean_name(peer_name);
	return n.empty() ? std::string(is_host ? "PLAYER 2" : "PLAYER 1") : n;
}

bool Online::host(const OnlineSettings &s, std::string &error)
{
	stop();
	last = {};
	failure.clear();
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
	failure.clear();
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
	session->trace = log_line;
	joining_to = host + (p != 7700 ? ":" + std::to_string(p) : "");
	ph = Phase::joining;
	fprintf(stderr, "online: joining %s, port %d\n", host.c_str(), p);
	return true;
}

// ---- a room code's match ----

bool Online::host_room(const OnlineSettings &s, std::string &error)
{
	stop();
	last = {};
	failure.clear();
	started_with = s;
	is_host = true;
	auto_delay = s.delay < 0;
	auto_done = false;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = "Room codes aren't set up in this build yet: use Host by address below, on the same network or a VPN."; return false; }
	room = std::make_unique<vfnet::RoomLink>();
	if (!room->open(0, error)) { room.reset(); return false; }
	room->set_relay_only(s.hide_ip);
	identity.name = s.name;
	rooms = true;
	ph = Phase::hosting;
	st = Step::getting_ready;
	step_at = now;
	log_line("hosting with a room code: getting ready");
	const std::string base = lobby;
	start_job([base](Job &j) {
		j.stun = vfnet::RoomLink::resolve(STUN_HOST, STUN_PORT);
		j.reply = vfnet::http::request("POST", base + "/v1/turn", "{}", 8000);
		vfnet::json::Value v;
		if (j.reply.status == 200 && vfnet::json::parse(j.reply.body, v))
			for (const vfnet::json::Value &sv : v["servers"].items) {
				const std::string ip = vfnet::RoomLink::resolve(sv["host"].str(), int(sv["port"].n));
				if (!ip.empty()) { j.turn = ip; j.user = sv["username"].str(); j.password = sv["credential"].str(); break; }
			}
	});
	return true;
}

bool Online::join_room(const OnlineSettings &s, const std::string &c, std::string &error)
{
	const std::string want = online_menu::clean_code(c);
	if (want.size() != 6) { error = "A room code is 6 letters and numbers (like KXQ-4M7): the host's Online page shows it."; return false; }
	stop();
	last = {};
	failure.clear();
	started_with = s;
	is_host = false;
	auto_delay = s.delay < 0;
	auto_done = false;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = "Room codes aren't set up in this build yet: use Join by address below."; return false; }
	room = std::make_unique<vfnet::RoomLink>();
	if (!room->open(0, error)) { room.reset(); return false; }
	room->set_relay_only(s.hide_ip);
	identity.name = s.name;
	rooms = true;
	code = want;
	ph = Phase::joining;
	st = Step::getting_ready;
	step_at = now;
	log_line("joining room " + room_code() + ": getting ready");
	const std::string base = lobby;
	start_job([base](Job &j) {
		j.stun = vfnet::RoomLink::resolve(STUN_HOST, STUN_PORT);
		j.reply = vfnet::http::request("POST", base + "/v1/turn", "{}", 8000);
		vfnet::json::Value v;
		if (j.reply.status == 200 && vfnet::json::parse(j.reply.body, v))
			for (const vfnet::json::Value &sv : v["servers"].items) {
				const std::string ip = vfnet::RoomLink::resolve(sv["host"].str(), int(sv["port"].n));
				if (!ip.empty()) { j.turn = ip; j.user = sv["username"].str(); j.password = sv["credential"].str(); break; }
			}
	});
	return true;
}

std::string Online::room_code() const
{
	if (code.size() != 6) return "";
	return code.substr(0, 3) + "-" + code.substr(3);
}

void Online::start_job(std::function<void(Job &)> work)
{
	auto j = std::make_shared<Job>();
	job = j;
	std::thread([j, work] {   // (it keeps its own copy: a stop meanwhile leaves it to finish and be forgotten)
		work(*j);
		j->done = true;
	}).detach();
}

std::string Online::me_json(const std::string &extra) const
{
	std::vector<std::string> cands = room ? room->candidates() : std::vector<std::string>();
	return "{\"game\":" + vfnet::json::quote(identity.game) + ",\"version\":" + vfnet::json::quote(identity.build) + ",\"content\":" +
	       vfnet::json::quote(hex64(identity.content)) + ",\"name\":" + vfnet::json::quote(identity.name) + ",\"cands\":" + vfnet::json::list(cands) +
	       ",\"relay\":" + vfnet::json::quote(room ? room->relay_address() : "") + extra + "}";
}

void Online::room_session(bool host)
{
	if (adapter.settle) adapter.settle();
	vfnet::Config c = config(started_with);
	c.token = room_token;
	session = std::make_unique<vfnet::Session>(*room, adapter, identity, c, host, host ? vf3net::start_blob(board) : std::vector<uint8_t>());
	session->trace = log_line;
}

void Online::room_failed(const std::string &why)
{
	log_line("room: " + why);
	failure = why;
	notices.push_back({ Notice::refused, why });
	close_room();
	session.reset();
	room.reset();
	job.reset();
	ph = Phase::off;
	st = Step::none;
	rooms = false;
}

void Online::close_room()
{
	if (!rooms || !is_host || key.empty()) return;
	const std::string url = lobby_url("/v1/close"), body = "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}";
	std::thread([url, body] { vfnet::http::request("POST", url, body, 5000); }).detach();
	key.clear();
}

namespace {

// The lobby's no, in words: what happened and what to do
std::string lobby_says(const vfnet::http::Reply &r, const std::string &code, const std::string &mine)
{
	if (r.status == 0)
		return "Couldn't reach the online service (" + r.error + "). Check the internet connection; on the same network you can "
		       "still play: Host by address / Join by address.";
	vfnet::json::Value v;
	vfnet::json::parse(r.body, v);
	const std::string e = v["error"].str();
	if (e == "no_room") return "There's no room " + code + ". Check the code, and that the host is still on their Host page.";
	if (e == "full") return "Someone's already joining room " + code + ": ask the host for a new code.";
	if (e == "version") {
		std::string theirs;
		for (char ch : v["host_version"].str())
			if (isalnum(uint8_t(ch)) || ch == '.' || ch == '-' || ch == '+' || ch == '_') theirs += ch;
		return "The host has " + (theirs.empty() ? std::string("another version") : theirs.substr(0, 40)) + ", you have " + mine +
		       ": you both need the same version (the newest, from the site).";
	}
	if (e == "rom") return "Your game files (the ROM) aren't the same as the host's: you both need the same vf3.zip.";
	if (e == "game") return "That room's for another game.";
	if (e == "busy") return "Too many tries just now: wait a minute, then try again.";
	return "The online service had a problem (" + std::to_string(r.status) + "): try again in a moment.";
}

} // namespace

void Online::room_poll()
{
	if (!room) return;
	if (!session) {   // (no session reading the link yet: its housekeeping, getting its addresses, kept going here)
		std::vector<uint8_t> p;
		while (room->recv(p)) {}
	}
	switch (st) {
	case Step::getting_ready:
		if (job && job->done) {
			if (!job->stun.empty()) room->use_stun(job->stun);
			if (!job->turn.empty()) room->use_turn(job->turn, job->user, job->password);
			else log_line(job->reply.status == 0 ? "no relay: the online service wasn't reached (" + job->reply.error + ")"
			                                    : "no relay: the online service gave none (" + std::to_string(job->reply.status) + ")");
			if (job->reply.status == 0) { room_failed(lobby_says(job->reply, room_code(), identity.build)); return; }
			job.reset();
		}
		if (!job && room->gathered() && started_with.hide_ip && room->relay_address().empty()) {
			room_failed("Hide my IP address is on, but the relay couldn't be reached: try again, or turn it off (Online > Settings).");
			return;
		}
		if (!job && room->gathered()) {
			log_line("this side: on its network and public " + masked(room->candidates()) + ", relay " + mask(room->relay_address()));
			const std::string base = lobby;
			if (is_host) {
				st = Step::opening;
				const std::string body = me_json("");
				start_job([base, body](Job &j) { j.reply = vfnet::http::request("POST", base + "/v1/host", body, 10000); });
			} else {
				st = Step::finding;
				const std::string body = me_json(",\"code\":" + vfnet::json::quote(code));
				start_job([base, body](Job &j) { j.reply = vfnet::http::request("POST", base + "/v1/join", body, 10000); });
			}
		}
		break;
	case Step::opening:   // (a room opened, or opened again after a player who didn't get going)
		if (job && job->done) {
			vfnet::json::Value v;
			const bool ok = job->reply.status == 200 && vfnet::json::parse(job->reply.body, v) && v["code"].str().size() == 6 && !v["key"].str().empty();
			if (!ok) { room_failed("Couldn't open a room: " + lobby_says(job->reply, room_code(), identity.build)); return; }
			code = v["code"].str();
			key = v["key"].str();
			room_token = strtoull(v["token"].str().c_str(), nullptr, 16);
			job.reset();
			room_session(true);
			st = Step::waiting;
			wait_errors = 0;
			retry_at = 0;
			log_line("room " + room_code() + " open: waiting for a player");
		}
		break;
	case Step::waiting:
		if (!job && now >= retry_at) {
			const std::string url = lobby_url("/v1/wait?code=" + code + "&key=" + key);
			start_job([url](Job &j) { j.reply = vfnet::http::request("GET", url, "", 30000); });
		}
		if (job && job->done) {
			vfnet::json::Value v;
			const vfnet::http::Reply r = job->reply;
			job.reset();
			if (r.status == 200 && vfnet::json::parse(r.body, v)) {
				wait_errors = 0;
				const vfnet::json::Value &g = v["guest"];
				if (g.kind == vfnet::json::Value::object) {
					peer_name = g["name"].str();
					room->set_peer(g["cands"].strings(), g["relay"].str(), true);
					st = Step::connecting;
					log_line("a player joined the room (" + online_menu::clean_name(peer_name) + "): their addresses " + masked(g["cands"].strings()) +
					         ", relay " + mask(g["relay"].str()) + "; connecting");
				}
			} else if (r.status == 404) {
				room_failed("The room closed (the online service lost it): host again for a new code.");
				return;
			} else {   // (the service unreached for a moment: tried again in a while, the room kept)
				retry_at = now + 2000;
				if (++wait_errors == 3) notices.push_back({ Notice::refused, "Having trouble reaching the online service: still trying..." });
				log_line("waiting: " + (r.status ? "the service said " + std::to_string(r.status) : r.error));
			}
		}
		break;
	case Step::finding:
		if (job && job->done) {
			vfnet::json::Value v;
			const vfnet::http::Reply r = job->reply;
			job.reset();
			if (r.status != 200 || !vfnet::json::parse(r.body, v) || v["token"].str().empty()) { room_failed(lobby_says(r, room_code(), identity.build)); return; }
			room_token = strtoull(v["token"].str().c_str(), nullptr, 16);
			peer_name = v["name"].str();
			room->set_peer(v["cands"].strings(), v["relay"].str(), false);
			room_session(false);
			st = Step::connecting;
			log_line("found room " + room_code() + " (" + online_menu::clean_name(peer_name) + "): their addresses " + masked(v["cands"].strings()) + ", relay " +
			         mask(v["relay"].str()) + "; connecting");
		}
		break;
	default:
		break;
	}
}

void Online::stop()
{
	if (session && ph == Phase::playing) ended("You left the match.");
	if (session && session->state() != vfnet::Session::State::ended) session->quit();
	close_room();
	session.reset();
	link.reset();
	room.reset();
	job.reset();
	ph = Phase::off;
	st = Step::none;
	rooms = false;
}

void Online::poll(double t)
{
	now = t;
	if (rooms) room_poll();
	if (!session) return;
	session->poll(t);
	vfnet::Session::Event e;
	while (session && session->next_event(e)) {
		switch (e.kind) {
		case vfnet::Session::Event::connected:
			ph = Phase::playing;
			began_at = now;
			if (rooms) {
				log_line(std::string("connected ") + (room->via_relay() ? "through a relay" : "directly"));
				close_room();   // (the code's done its job)
			}
			notices.push_back({ Notice::joined, "Playing " + e.text + ": press Start" });
			break;
		case vfnet::Session::Event::ended: {
			const auto why = session->end_reason();
			const bool was_playing = ph == Phase::playing;
			// a host goes on waiting after a player who couldn't join (another version, another ROM, gone quiet
			// before the game started); anything else ends it
			if (!was_playing && is_host && (why == vfnet::Session::End::rejected || why == vfnet::Session::End::timeout || why == vfnet::Session::End::opponent_quit)) {
				notices.push_back({ Notice::refused, "A player couldn't join: " + e.text });
				if (rooms) {   // (the same code, open again, a new token: the last player's no good for it)
					session.reset();
					room->clear_peer();
					st = Step::opening;
					const std::string url = lobby_url("/v1/reopen"), body = "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}";
					start_job([url, body](Job &j) { j.reply = vfnet::http::request("POST", url, body, 10000); });
					return;
				}
				std::string err;
				if (!open_host(err)) { notices.push_back({ Notice::ended, err }); session.reset(); link.reset(); ph = Phase::off; }
				return;
			}
			if (was_playing && why != vfnet::Session::End::quit) {
				const std::string who = opponent();
				ended(why == vfnet::Session::End::opponent_quit ? who + " left the match."
				      : why == vfnet::Session::End::disconnected ? "Lost the connection to " + who + "."
				      : why == vfnet::Session::End::desync ? "The match went out of step." : e.text);
			}
			if (!was_playing && !is_host && why != vfnet::Session::End::quit) {
				failure = e.text;
				if (rooms && why == vfnet::Session::End::timeout)
					failure = "Couldn't connect to " + opponent() + ", directly or through the relay. Try again; if it keeps failing, send both "
					          "players' log.txt (README: Online play).";
				// (a local address over the internet: the commonest mistake, worth saying what to do)
				else if (why == vfnet::Session::End::timeout && local_address(joining_to))
					failure += " " + joining_to + " is a local address: it works only on the same network (Wi-Fi, a hotspot). Over the "
					           "internet, use the host's public address or a VPN like Tailscale.";
			}
			if (why != vfnet::Session::End::quit)
				notices.push_back({ was_playing ? Notice::ended : Notice::refused, was_playing ? last.why : (failure.empty() ? e.text : failure) });
			close_room();
			session.reset();
			link.reset();
			room.reset();
			job.reset();
			ph = Phase::off;
			st = Step::none;
			rooms = false;
			return;
		}
		default:
			break;
		}
	}
	// the connection in the log every 10 s of a match (what a report needs: the ping, the rollbacks, the waits)
	if (session && ph == Phase::playing && now - logged_at >= 10000) {
		const auto &stt = session->stats();
		if (logged_at > 0)
			fprintf(stderr, "online: frame %u, ping %.0f ms, delay %d, %llu rollbacks (%.1f frames on average, %d the most in the last second), "
			        "%llu stalls for the other side, %llu waits to keep in time, %.1f frames ahead%s\n", session->frame(), stt.rtt_ms, session->delay(),
			        (unsigned long long)(stt.rollbacks - logged_rb), stt.rollbacks > logged_rb ? double(stt.resim_frames - logged_resim) / double(stt.rollbacks - logged_rb) : 0.0,
			        stt.rollback_max, (unsigned long long)(stt.stalls - logged_stalls), (unsigned long long)(stt.waits - logged_waits), double(stt.ahead),
			        via_relay() ? ", through a relay" : "");
		if (logged_at > 0 && stt.steps > logged_steps) {   // (where a frame's time went: what's slow on a slow machine)
			const double n = double(stt.steps - logged_steps);
			fprintf(stderr, "online: a frame's step %.2f ms on average, %.1f the worst: its save %.2f, its frame %.2f, frames run again %.2f, "
			        "the desync check %.2f\n", (stt.save_ms - logged_save + stt.run_ms - logged_run + stt.resim_ms - logged_resim_ms + stt.hash_ms - logged_hash) / n,
			        stt.step_max_ms, (stt.save_ms - logged_save) / n, (stt.run_ms - logged_run) / n, (stt.resim_ms - logged_resim_ms) / n, (stt.hash_ms - logged_hash) / n);
		}
		session->clear_step_max();
		logged_steps = stt.steps;
		logged_save = stt.save_ms;
		logged_run = stt.run_ms;
		logged_resim_ms = stt.resim_ms;
		logged_hash = stt.hash_ms;
		logged_at = now;
		logged_rb = stt.rollbacks;
		logged_resim = stt.resim_frames;
		logged_stalls = stt.stalls;
		logged_waits = stt.waits;
	}
	// Auto's input delay, once the ping is known (the character select): half the round trip in frames, 1 to 4;
	// and 2 at least where a frame costs this machine much of its time (a Steam Deck's 11 ms of 17: each frame a
	// rollback runs again costs it as much again, so fewer and shorter ones, for a frame's delay on this side only)
	if (session && ph == Phase::playing && auto_delay && !auto_done && session->frame() > 120 && session->stats().rtt_ms > 0) {
		const double frame_ms = 1000.0 / 57.524160;
		const auto &stt = session->stats();
		const double step_ms = stt.steps ? (stt.save_ms + stt.run_ms) / double(stt.steps) : 0.0;
		const int by_ping = std::clamp(int(std::ceil(stt.rtt_ms / 2 / frame_ms)), 1, 4);
		const int d = std::max(by_ping, step_ms > 7.0 ? 2 : 1);
		session->set_delay(d);
		fprintf(stderr, "online: Auto input delay %d (ping %.0f ms, a frame's step %.1f ms here)\n", d, stt.rtt_ms, step_ms);
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
	const std::string who = session && !session->opponent_name().empty() ? opponent() : online_menu::clean_name(peer_name);
	if (rooms) {
		const std::string relay = via_relay() ? " (through the relay)" : "";
		switch (st) {
		case Step::getting_ready: return "Getting ready...";
		case Step::opening: return "Opening a room...";
		case Step::waiting: return "Waiting for a player: give them the code above.";
		case Step::finding: return "Finding room " + room_code() + "...";
		case Step::connecting:
			if (session && session->game_started()) return "Starting the game" + relay + "...";
			if (session && room->locked()) return "Connected to " + (who.empty() ? std::string("the other player") : who) + relay + ": getting ready...";
			return "Connecting to " + (who.empty() ? std::string("the other player") : who) + "...";
		default: break;
		}
	}
	if (!session) return "";
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
	const auto &stt = session->stats();
	if (session->interrupted()) { level = 2; return opponent() + "   connection interrupted"; }
	const int ms = int(stt.rtt_ms + 0.5), rb = stt.rollback_max;
	level = ms < 60 && rb <= 2 ? 0 : ms < 120 && rb <= 4 ? 1 : 2;
	return opponent() + "   " + std::to_string(ms) + " ms   rollback " + std::to_string(rb) + (via_relay() ? "   relay" : "");
}

std::string Online::connection_detail() const
{
	if (!session || ph != Phase::playing) return "";
	const auto &stt = session->stats();
	char b[200];
	snprintf(b, sizeof b, "delay %d   jitter %.0f ms   rollback %.1f avg, %d peak   ahead %+.1f   step %.1f ms (save %.1f)",
	         session->delay(), stt.jitter_ms, stt.rollbacks ? double(stt.resim_frames) / double(stt.rollbacks) : 0.0, stt.rollback_max, stt.lead,
	         stt.steps ? (stt.save_ms + stt.run_ms + stt.resim_ms + stt.hash_ms) / double(stt.steps) : 0.0, stt.steps ? stt.save_ms / double(stt.steps) : 0.0);
	return b;
}

// ---- the menu ----

namespace online_menu {

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
				else if (e.key.scancode == SDL_SCANCODE_V && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {   // (a code pasted)
					if (char *clip = SDL_GetClipboardText()) { for (const char *p = clip; *p; p++) add(*p); SDL_free(clip); }
				} else return false;   // (a letter: its text comes as text input)
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
		"and the other player's moves can jump a little more (rollback). Auto picks from the ping and this machine's speed.",
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
	return { "Connection display", "In a match, small at the top of the screen: the other player's name, the ping and the rollback, "
		"the dot green, yellow or red for how good the connection is. Detailed adds a line: the input delay, the jitter, the "
		"rollbacks, how far ahead this side is, and what each frame costs here.",
		[&s] { return std::string(s.show_connection == 2 ? "Detailed" : s.show_connection ? "On" : "Off"); },
		[&s, changed](int d) { s.show_connection = (s.show_connection + (d < 0 ? 2 : 1)) % 3; changed(); } };
}

menu_item info(const std::string &text, bool big = false, bool heading = false)
{
	menu_item i{ text, "", {}, {}, {}, true };
	i.big = big;
	i.heading = heading;
	return i;
}

// The page made again as things are now, the row it opens on chosen by it
void again(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu.replace(page(o, s, menu, changed), -1);
}

menu_page join_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Join by address", "", {} };
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
	c.below = [&o, problem] {
		if (!problem->empty()) return *problem;
		if (o.phase() == Online::Phase::joining) return o.status();
		return o.join_failure();   // (why it didn't, once it's stopped trying)
	};
	c.good = [&o, problem] { return problem->empty() && (o.phase() == Online::Phase::joining || o.join_failure().empty()); };
	p.items.push_back(c);
	p.items.push_back({ "Stop", "Stops trying to join.", {}, {}, [&o, problem] { o.stop(); problem->clear(); } });
	// (the Online page under it made again: it was made before Connect, and shows joining now, or what came of it)
	p.items.push_back({ "Back", "", {}, {}, [&o, &s, &menu, changed] { menu.pop(); again(o, s, menu, changed); } });
	return p;
}

// Joining with a code: type it (a keyboard, or a pad's Up / Down), then Join
menu_page code_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Join with a code", "", {} };
	auto c = std::make_shared<Typing>();
	auto problem = std::make_shared<std::string>();
	auto shown = [](const std::string &t) { return t.size() > 3 ? t.substr(0, 3) + "-" + t.substr(3) : t; };
	auto join = [&o, &s, &menu, c, problem, changed] {
		std::string err;
		if (!o.join_room(s, c->text, err)) { *problem = err; menu.refresh(); return; }
		problem->clear();
		menu.pop();
		again(o, s, menu, changed);   // (the Online page: joining, step by step)
	};
	menu_item code{ "Room code", "The 6 letters and numbers on the host's Online page (KXQ-4M7: the dash doesn't matter). A: type it; "
		"a keyboard can paste it too (Ctrl+V / Cmd+V).",
		[c, shown] { return c->on ? shown(c->text) + "_" : c->text.empty() ? std::string("Type it") : shown(c->text); },
		{},
		[&menu, c, join] {
			start_typing(menu, c, CODE_LETTERS, 6, true, [join](const std::string &v, std::string &why) {
				if (clean_code(v).size() != 6) { why = "A code has 6 letters and numbers."; return false; }
				join();   // (done typing: straight in)
				return true;
			});
		} };
	code.below = [c] { return c->error; };
	p.items.push_back(code);
	menu_item j{ "Join", "Joins the host's room. Once you're both in, the game starts at its title screen: both press Start.", {}, {}, join };
	j.below = [problem] { return *problem; };
	p.items.push_back(j);
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	return p;
}

// Before playing: a name. If there's none, it's asked for, then what was chosen goes on.
std::function<void(std::function<void()>)> need_name(OnlineSettings &s, game_menu &menu, std::shared_ptr<Typing> name, std::function<void()> changed,
                                                     std::shared_ptr<std::string> problem)
{
	return [&s, &menu, name, changed, problem](std::function<void()> go) {
		if (!s.name.empty()) { go(); return; }
		*problem = "Your name first: type it above, then press A.";
		menu.select_row(0);
		start_typing(menu, name, NAME_LETTERS, size_t(vfnet::MAX_NAME), true, [&s, changed, go, problem](const std::string &v, std::string &why) {
			const std::string n = clean_name(v);
			if (n.empty()) { why = "Your name needs a letter or a number."; return false; }
			s.name = n;
			changed();
			problem->clear();
			go();
			return true;
		});
	};
}

} // namespace

std::string clean_address(const std::string &s)
{
	std::string out;
	for (char ch : s) {
		ch = char(tolower(uint8_t(ch)));
		if (ADDRESS_LETTERS.find(ch) != std::string::npos && out.size() < 64) out += ch;
	}
	return out;
}

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

std::string clean_code(const std::string &s)
{
	std::string out;
	for (char ch : s) {
		ch = char(toupper(uint8_t(ch)));
		if (CODE_LETTERS.find(ch) != std::string::npos && out.size() < 6) out += ch;
	}
	return out;
}

menu_page page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Online", "", {} };
	auto name = std::make_shared<Typing>();
	name->text = s.name;
	auto problem = std::make_shared<std::string>();
	auto named = need_name(s, menu, name, changed, problem);
	menu_item n{ "Your name", "Up to 10 letters, shown under your energy bar in a match, and to the player you play. A: type it.",
		[name, &s] { return name->on ? name->text + "_" : s.name.empty() ? std::string("Not set") : s.name; },
		{}, [&menu, &s, name, changed] {
			start_typing(menu, name, NAME_LETTERS, size_t(vfnet::MAX_NAME), true, [&s, changed](const std::string &v, std::string &why) {
				const std::string nn = clean_name(v);
				if (nn.empty()) { why = "Your name needs a letter or a number."; return false; }
				s.name = nn;
				changed();
				return true;
			});
		} };
	n.below = [name] { return name->error; };
	p.items.push_back(n);
	auto status = [&o](menu_item &it) {
		it.below = [&o] { return o.status(); };
		it.good = [] { return true; };
	};

	if (o.phase() == Online::Phase::hosting && o.by_code()) {
		p.items.push_back(info("YOUR ROOM CODE", false, true));
		p.items.push_back(info(o.room_code().empty() ? "..." : o.room_code(), true));
		p.items.push_back(info("Give this code to the other player: they choose Join with a code."));
		auto copied = std::make_shared<bool>(false);
		if (!o.room_code().empty()) {
			menu_item cp{ "Copy the code", "Copies it, to paste into a message to the other player.", {}, {},
				[&o, copied, &menu] { SDL_SetClipboardText(o.room_code().c_str()); *copied = true; menu.refresh(); } };
			cp.below = [copied] { return *copied ? std::string("Copied.") : std::string(); };
			cp.good = [] { return true; };
			p.items.push_back(cp);
		}
		menu_item w{ "Stop hosting", "Closes the room. You can play on your own meanwhile: close the menu, and the match starts when "
			"someone joins.", {}, {}, [&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } };
		status(w);
		p.start = int(p.items.size());
		p.items.push_back(w);
	} else if (o.phase() == Online::Phase::joining && o.by_code()) {
		p.items.push_back(info("JOINING ROOM", false, true));
		p.items.push_back(info(o.room_code(), true));
		menu_item w{ "Stop joining", "Stops trying to join.", {}, {}, [&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } };
		status(w);
		p.start = int(p.items.size());
		p.items.push_back(w);
	} else if (o.phase() == Online::Phase::hosting) {
		p.items.push_back(info("WAITING FOR A PLAYER", false, true));
		const std::string here = o.local_address().empty() ? std::string("No network") : o.local_address() + (o.local_port() != 7700 ? ":" + std::to_string(o.local_port()) : "");
		p.items.push_back(info(here, true));
		p.items.push_back(info("Players on your network join with this address."));
		p.items.push_back(info("Over the internet, use Host a match (a room code) instead,"));
		p.items.push_back(info("or both join a VPN like Tailscale and use its address."));
		menu_item w{ "Stop waiting", "Stops hosting. You can play on your own meanwhile: close the menu, and the match starts when someone joins.", {}, {},
			[&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } };
		status(w);
		p.start = int(p.items.size());
		p.items.push_back(w);
	} else if (o.phase() == Online::Phase::joining) {
		p.items.push_back(info("JOINING", false, true));
		p.items.push_back(info(s.address, true));
		menu_item w{ "Stop joining", "Stops trying to join.", {}, {}, [&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } };
		status(w);
		p.start = int(p.items.size());
		p.items.push_back(w);
	} else {
		// what came of the last try, if it didn't get going (the room's, the lobby's, the connection's)
		if (!o.join_failure().empty()) {
			menu_item h = info("COULDN'T CONNECT", false, true);
			h.below = [&o] { return o.join_failure(); };
			p.items.push_back(h);
		}
		if (const Online::LastMatch &m = o.last_match(); m.any) {   // (just after a match: what happened, and the same again)
			p.items.push_back(info("MATCH OVER", false, true));
			p.items.push_back(info(m.why, m.why.size() <= 28));
			char line[96];
			snprintf(line, sizeof line, "%s%s  ·  %d:%02d played  ·  %d ms", m.opponent.empty() ? "" : "Against ", m.opponent.c_str(), m.seconds / 60, m.seconds % 60, m.ping);
			p.items.push_back(info(line));
			p.start = int(p.items.size());
			if (m.was_host)
				p.items.push_back({ "Host again", m.by_code ? "Opens a new room, with a new code to give " + m.opponent + "." :
					"Waits for a player to join you again, as before (" + m.opponent + " can join with the same address).", {}, {},
					[&o, &s, &menu, problem, changed] {
						std::string err;
						if (!(o.last_match().by_code ? o.host_room(s, err) : o.host(s, err))) { *problem = err; menu.refresh(); return; }
						again(o, s, menu, changed);
					} });
			else if (m.by_code)
				p.items.push_back({ "Join with a code", "Ask " + (m.opponent.empty() ? std::string("the host") : m.opponent) + " to host again, and type their new code.", {}, {},
					[&o, &s, &menu, changed] { menu.push(code_page(o, s, menu, changed)); } });
			else
				p.items.push_back({ "Join " + (m.opponent.empty() ? std::string("again") : m.opponent + " again"),
					"Joins " + s.address + " again: ask " + (m.opponent.empty() ? std::string("the host") : m.opponent) + " to host again first.", {}, {},
					[&o, &s, &menu, problem, changed] {
						std::string err;
						if (!o.join(s, err)) { *problem = err; menu.refresh(); return; }
						again(o, s, menu, changed);
					} });
			p.items.back().below = [problem] { return *problem; };
		}
		p.items.push_back(info("PLAY SOMEONE ONLINE", false, true));
		menu_item h{ "Host a match", "Opens a room and gives you a code to send the other player: it works across the internet, no setup. "
			"Matches play the PC way, free play, with your arcade settings (rounds, difficulty). You're player 1.", {}, {},
			[&o, &s, &menu, problem, changed, named] {
				named([&o, &s, &menu, problem, changed] {
					std::string err;
					if (!o.host_room(s, err)) { *problem = err; menu.refresh(); return; }
					again(o, s, menu, changed);
				});
			} };
		h.below = [problem] { return *problem; };
		if (p.start < 0) p.start = int(p.items.size());
		p.items.push_back(h);
		p.items.push_back({ "Join with a code", "Type the code the host gives you. Matches play the PC way, free play, with the host's arcade "
			"settings. You're player 2, with your own controls: Player 1's keys or pad work.", {}, {},
			[&o, &s, &menu, changed, named] { named([&o, &s, &menu, changed] { menu.push(code_page(o, s, menu, changed)); }); } });
		p.items.push_back(info("ON THE SAME NETWORK, OR A VPN", false, true));
		p.items.push_back({ "Host by address", "Waits for a player on your network (or your VPN, like Tailscale) to join with your address. "
			"No online service needed.", {}, {},
			[&o, &s, &menu, problem, changed, named] {
				named([&o, &s, &menu, problem, changed] {
					std::string err;
					if (!o.host(s, err)) { *problem = err; menu.refresh(); return; }
					again(o, s, menu, changed);
				});
			} });
		p.items.push_back({ "Join by address", "Joins a player hosting by address: on your network, or over a VPN.", {}, {},
			[&o, &s, &menu, changed, named] { named([&o, &s, &menu, changed] { menu.push(join_page(o, s, menu, changed)); }); } });
	}
	p.items.push_back(info("SETTINGS", false, true));
	p.items.push_back(delay_row(o, s, changed));
	p.items.push_back(connection_row(s, changed));
	p.items.push_back({ "Hide my IP address", "On: matches made with a code always go through the relay (Cloudflare's), so the other player "
		"never sees your address. A little more ping. Off: directly when the routers allow it, which is quickest.",
		[&s] { return std::string(s.hide_ip ? "On" : "Off"); }, [&s, changed](int) { s.hide_ip = !s.hide_ip; changed(); } });
	p.items.push_back({ "Port", "The UDP port Host by address waits on (7700 unless another program has it). Room codes need no port.",
		[&s] { return std::to_string(s.port); },
		[&s, changed](int d) { s.port = std::clamp(s.port + d, 1024, 65535); changed(); } });
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	return p;
}

void match_rows(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::vector<menu_item> &rows)
{
	menu_item who{ "Playing", "", [&o] {
		return o.opponent() + "   " + std::to_string(o.ping()) + " ms" + (o.via_relay() ? "   relay" : "") + (o.interrupted() ? "   interrupted" : "");
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
