#include "online.h"
#include "../net/http.h"
#include "../net/json.h"
#include "version.h"
#include "flags.h"   // VF_LOBBY_URL (made by the Makefile)
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
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

// A lobby on this machine (tests): plain http allowed for it, and the other game's addresses its own
bool lobby_on_this_machine(const std::string &url)
{
	for (const char *p : { "http://127.0.0.1", "http://localhost", "http://[::1]" })
		if (url.rfind(p, 0) == 0 && (url.size() == strlen(p) || url[strlen(p)] == ':' || url[strlen(p)] == '/')) return true;
	return false;
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

// Every address in a line for the log hidden as mask hides one, wherever it is in the line (an IPv4's last part,
// an IPv6's all but its first two groups): log.txt is what players post with a report
std::string masked_text(const std::string &t)
{
	std::string out;
	for (size_t i = 0; i < t.size();) {
		// an IPv4: four numbers, dots between, not inside a longer run of them
		size_t j = i, parts = 0, last = 0;
		if (isdigit(uint8_t(t[i])) && (i == 0 || (!isdigit(uint8_t(t[i - 1])) && t[i - 1] != '.'))) {
			while (parts < 4) {
				size_t k = j;
				while (k < t.size() && k - j < 3 && isdigit(uint8_t(t[k]))) k++;
				if (k == j) break;
				parts++;
				if (parts == 4) { last = j; j = k; break; }
				if (k >= t.size() || t[k] != '.') break;
				j = k + 1;
			}
		}
		if (parts == 4 && (j >= t.size() || (!isdigit(uint8_t(t[j])) && t[j] != '.'))) {
			out.append(t, i, last - i);
			out += 'x';
			i = j;
			continue;
		}
		// an IPv6 in brackets: its first two groups kept
		if (t[i] == '[') {
			const size_t close = t.find(']', i);
			if (close != std::string::npos && close - i > 3 && t.find_first_not_of("0123456789abcdefABCDEF:.", i + 1) == close) {
				const size_t c1 = t.find(':', i + 1), c2 = c1 == std::string::npos ? c1 : t.find(':', c1 + 1);
				out.append(t, i, (c2 != std::string::npos && c2 < close ? c2 : i + 1) - i);
				out += ":...]";
				i = close + 1;
				continue;
			}
		}
		out += t[i++];
	}
	return out;
}

void log_line(const std::string &t) { fprintf(stderr, "online: %s\n", masked_text(t).c_str()); }

} // namespace

std::string Online::log_masked(const std::string &t) { return masked_text(t); }

struct Online::Job
{
	std::atomic<bool> done{ false };
	vfnet::http::Reply reply;
	std::string stun, turn, user, password;   // (getting ready's: the servers' addresses, the relay's login)
	std::string country;                       // (and this side's, as the lobby has it)
};

Online::~Online() = default;

// Goodbyes to the lobby (a room closed, a search left): on threads of their own, counted, so a quit can wait a
// moment for them (a game closed mid-search would otherwise be left in the queue, matched with someone)
static std::atomic<int> farewells{ 0 };
static std::atomic<bool> quitting{ false };   // (flush's time up: the waits for a reply given up)
static void farewell(const std::string &url, const std::string &body)
{
	farewells++;
	std::thread([url, body] { vfnet::http::request("POST", url, body, 5000); farewells--; }).detach();
}

void Online::flush(int ms)
{
	for (int waited = 0; farewells.load() > 0 && waited < ms; waited += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
	quitting = true;
	// (and any request still on its way given up, a moment for them to let go: no thread inside libcurl's or
	// OpenSSL's as the program's ending, a crash at the quit otherwise)
	vfnet::http::cancel_all();
	for (int waited = 0; vfnet::http::active() > 0 && waited < 500; waited += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

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
	c.delay = s.delay < 0 ? 2 : std::clamp(s.delay, 0, 10);   // (Auto starts at 2, then follows the ping)
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

void Online::ended(const std::string &why, bool lost)
{
	last.any = true;
	last.was_host = is_host;
	last.by_code = rooms;
	last.lost = lost;
	last.relay = via_relay();
	last.code = rooms ? code : "";
	last.address = rooms ? "" : joining_to;
	last.why = why;
	last.random = random;
	last.country[is_host ? 0 : 1] = country_me;
	last.country[is_host ? 1 : 0] = country_peer;
	last.wins[0] = score.matches[0];
	last.wins[1] = score.matches[1];
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
	fail_step = Step::none;
	peer_name.clear();   // (the last try's other player: not this one's)
	note.clear();
	random = false;
	ticket.clear();
	country_peer.clear();
	country_me.clear();   // (by address: no lobby to say where either is)
	tried = Try::host;
	patient_until = 0;
	started_with = s;
	is_host = true;
	auto_delay = s.delay < 0;
	auto_done = false;
	port = std::clamp(s.port, 1024, 65535);
	lan = vfnet::lan_address();
	if (!open_host(error)) return false;
	log_line("hosting on UDP port " + std::to_string(port) + " (" + (lan.empty() ? std::string("no network address") : lan) + ")");
	return true;
}

bool Online::join(const OnlineSettings &s, std::string &error)
{
	stop();
	last = {};
	failure.clear();
	fail_step = Step::none;
	peer_name.clear();   // (the last try's other player: not this one's)
	note.clear();
	random = false;
	ticket.clear();
	country_peer.clear();
	country_me.clear();   // (by address: no lobby to say where either is)
	tried = Try::join;
	patient_until = 0;
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
	log_line("joining " + host + ", port " + std::to_string(p));
	return true;
}

// ---- a room code's match ----

bool Online::find_random(const OnlineSettings &s, std::string &error)
{
	stop();
	last = {};
	failure.clear();
	fail_step = Step::none;
	peer_name.clear();
	note.clear();
	ticket.clear();
	country_peer.clear();
	tried = Try::random;
	want_code.clear();
	patient_until = 0;
	started_with = s;
	is_host = false;   // (until the lobby says: the one who waited longer hosts)
	auto_delay = s.delay < 0;
	auto_done = false;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = "Online play isn't set up in this build (no lobby)."; return false; }
	if (lobby.rfind("https://", 0) != 0 && !lobby_on_this_machine(lobby)) { error = "The lobby's address (settings.ini's online_lobby) must start with https://."; return false; }
	room = std::make_unique<vfnet::RoomLink>();
	room->allow_loopback(lobby_on_this_machine(lobby));
	if (!room->open(0, error)) { room.reset(); return false; }
	room->set_relay_only(s.hide_ip);
	identity.name = s.name;
	rooms = true;
	random = true;
	random_misses = 0;
	code.clear();
	ph = Phase::joining;
	st = Step::getting_ready;
	step_at = now;
	players = 0;
	log_line("finding a random opponent: getting ready (" + identity.build + ", game files " + hex64(identity.content) + ")");
	start_job(gather(lobby));
	return true;
}

bool Online::host_room(const OnlineSettings &s, std::string &error, const std::string &want)
{
	stop();
	last = {};
	failure.clear();
	fail_step = Step::none;
	peer_name.clear();   // (the last try's other player: not this one's)
	note.clear();
	random = false;
	ticket.clear();
	country_peer.clear();
	tried = Try::host_room;
	want_code = online_menu::clean_code(want);
	reopening = false;
	patient_until = 0;
	started_with = s;
	is_host = true;
	auto_delay = s.delay < 0;
	auto_done = false;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = "Online play isn't set up in this build (no lobby)."; return false; }
	if (lobby.rfind("https://", 0) != 0 && !lobby_on_this_machine(lobby)) { error = "The lobby's address (settings.ini's online_lobby) must start with https://."; return false; }
	room = std::make_unique<vfnet::RoomLink>();
	room->allow_loopback(lobby_on_this_machine(lobby));   // (two games on one machine: a test)
	if (!room->open(0, error)) { room.reset(); return false; }
	room->set_relay_only(s.hide_ip);
	identity.name = s.name;
	rooms = true;
	ph = Phase::hosting;
	st = Step::getting_ready;
	step_at = now;
	log_line("hosting with a room code: getting ready (" + identity.build + ", game files " + hex64(identity.content) + ")");
	start_job(gather(lobby));
	return true;
}

bool Online::join_room(const OnlineSettings &s, const std::string &c, std::string &error, bool patient)
{
	const std::string want = online_menu::clean_code(c);
	if (want.size() != 6) { error = "A room code is 6 letters and numbers (like KXQ-4M7): the host's Online page shows it."; return false; }
	const std::string waited_for = patient ? last.opponent : "";   // (kept: stop() and the rest forget the last match)
	stop();
	last = {};
	failure.clear();
	fail_step = Step::none;
	peer_name.clear();   // (the last try's other player: not this one's)
	note.clear();
	random = false;
	ticket.clear();
	country_peer.clear();
	tried = Try::join_room;
	patient_until = patient ? now + 60000 : 0;
	patient_for = waited_for;
	retry_at = 0;
	started_with = s;
	is_host = false;
	auto_delay = s.delay < 0;
	auto_done = false;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = "Online play isn't set up in this build (no lobby)."; return false; }
	if (lobby.rfind("https://", 0) != 0 && !lobby_on_this_machine(lobby)) { error = "The lobby's address (settings.ini's online_lobby) must start with https://."; return false; }
	room = std::make_unique<vfnet::RoomLink>();
	room->allow_loopback(lobby_on_this_machine(lobby));   // (two games on one machine: a test)
	if (!room->open(0, error)) { room.reset(); return false; }
	room->set_relay_only(s.hide_ip);
	identity.name = s.name;
	rooms = true;
	code = want;
	ph = Phase::joining;
	st = Step::getting_ready;
	step_at = now;
	log_line("joining room " + room_code() + ": getting ready (" + identity.build + ", game files " + hex64(identity.content) + ")");
	start_job(gather(lobby));
	return true;
}

// Getting ready for a room's match: the STUN server's address, and a relay's login (with this side's country)
std::function<void(Online::Job &)> Online::gather(const std::string &base)
{
	return [base](Online::Job &j) {
		j.stun = vfnet::RoomLink::resolve(STUN_HOST, STUN_PORT);
		j.reply = vfnet::http::request("POST", base + "/v1/turn", "{}", 8000);
		vfnet::json::Value v;
		if (j.reply.status == 200 && vfnet::json::parse(j.reply.body, v)) {
			j.country = v["country"].str();
			for (const vfnet::json::Value &sv : v["servers"].items) {
				const std::string ip = vfnet::RoomLink::resolve(sv["host"].str(), int(std::clamp(sv["port"].n, 0.0, 65535.0)));
				if (!ip.empty()) { j.turn = ip; j.user = sv["username"].str(); j.password = sv["credential"].str(); break; }
			}
		}
	};
}

bool Online::try_again(const OnlineSettings &s, std::string &error)
{
	switch (tried) {
	case Try::host_room: { const std::string w = want_code; return host_room(s, error, w); }   // (a Play again's code still wanted)
	case Try::join_room: { const std::string c = code; return join_room(s, c, error); }
	case Try::host: return host(s, error);
	case Try::join: return join(s, error);
	case Try::random: return find_random(s, error);
	default: error = "Nothing to try again."; return false;
	}
}

bool Online::play_again(const OnlineSettings &s, std::string &error)
{
	const LastMatch m = last;   // (a copy: hosting or joining forgets it)
	if (!m.any) { error = "No match to play again."; return false; }
	if (m.by_code) return m.was_host ? host_room(s, error, m.code) : join_room(s, m.code, error, true);
	return m.was_host ? host(s, error) : join(s, error);
}

int Online::start_wait() const
{
	if (!session || ph != Phase::playing) return 0;
	for (int p = 0; p < 2; p++) {
		const std::vector<uint16_t> &in = session->inputs(p);
		for (; start_scan[p] < in.size() && !start_seen[p]; start_scan[p]++)
			if (in[start_scan[p]] & vf3net::START_BUTTON) start_seen[p] = true;
	}
	const int me = local_player();
	if (!start_seen[me] || !start_seen[me ^ 1]) return !start_seen[me] ? 1 : 2;
	// after both have been in: one out of the game (a match lost; the winner plays on against the computer), until
	// they press Start (the game's back in only once a fighter's chosen: nothing to say over its character select)
	const int in = score.players();
	if (!both_in) return 0;
	bool out[2];
	for (int p = 0; p < 2; p++) {
		const std::vector<uint16_t> &inputs = session->inputs(p);
		if (in >> p & 1) { out_scan[p] = SIZE_MAX; out_pressed[p] = false; out[p] = false; continue; }
		if (out_scan[p] == SIZE_MAX) out_scan[p] = inputs.size();   // (from now: a Start held through the match's end isn't one)
		for (; out_scan[p] < inputs.size() && !out_pressed[p]; out_scan[p]++)
			if (inputs[out_scan[p]] & vf3net::START_BUTTON) out_pressed[p] = true;
		out[p] = !out_pressed[p];
	}
	return out[me] ? 3 : out[me ^ 1] ? 4 : 0;
}

int Online::players_online(const OnlineSettings &s)
{
	using namespace std::chrono;
	const double now = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
	if (ph == Phase::off && !online_asking->load() && now - online_asked > 30000 && !identity.game.empty()) {
		const std::string where = s.lobby.empty() ? lobby_default() : s.lobby;
		if (!where.empty() && where != "none") {
			online_asked = now;
			online_asking->store(true);
			const std::string url = where + "/v1/random/count", body = "{\"game\":" + vfnet::json::quote(identity.game) + ",\"version\":" +
				vfnet::json::quote(identity.build) + ",\"content\":" + vfnet::json::quote(hex64(identity.content)) + "}";
			std::thread([url, body, count = online_count, asking = online_asking] {
				const vfnet::http::Reply r = vfnet::http::request("POST", url, body, 5000);
				vfnet::json::Value v;
				if (r.status == 200 && vfnet::json::parse(r.body, v) && v["players"].kind == vfnet::json::Value::number) count->store(int(std::clamp(v["players"].n, 0.0, 1e6)));
				asking->store(false);
			}).detach();
		}
	}
	return online_count->load();
}

std::string Online::peer() const
{
	if (session && !session->opponent_name().empty()) return opponent();
	return online_menu::clean_name(peer_name);
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
	if (random) {   // (a stranger isn't told this side's own network's address: only a friend by code, who may share it)
		auto lan = [](const std::string &a) {
			unsigned b[4];
			if (sscanf(a.c_str(), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4)
				return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
			return a.size() > 3 && a[0] == '[' && (tolower(uint8_t(a[1])) == 'f') && strchr("cdeCDE", a[2]) != nullptr;
		};
		cands.erase(std::remove_if(cands.begin(), cands.end(), lan), cands.end());
	}
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
	fail_step = st;
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
	farewell(lobby_url("/v1/close"), "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}");
	kept_code = code;   // (a Play again opens it again: the lobby keeps the code for its host, by this key)
	kept_key = key;
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

// A room's host after a player who couldn't join: a random match's, another opponent (its code known to no one
// else); one with a code, the same code open again with a new token (the last player's no good for it)
void Online::host_again(const std::string &why)
{
	notices.push_back({ Notice::refused, "A player couldn't join: " + why });
	note = "A player couldn't join: " + why;
	session.reset();
	room->clear_peer();
	if (random) {
		close_room();
		is_host = false;
		ph = Phase::joining;
		peer_name.clear();
		country_peer.clear();
		search();
		return;
	}
	st = Step::opening;
	reopening = true;
	const std::string url = lobby_url("/v1/reopen"), body = "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}";
	start_job([url, body](Job &j) { j.reply = vfnet::http::request("POST", url, body, 10000); });
}

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
			country_me = country(job->country);
			job.reset();
		}
		if (!job && room->gathered() && started_with.hide_ip && room->relay_address().empty()) {
			room_failed("Hide my IP address is on, but the relay couldn't be reached: try again, or turn it off (Online > Settings).");
			return;
		}
		if (!job && room->gathered()) {
			log_line("this side: on its network and public " + masked(room->candidates()) + ", relay " + mask(room->relay_address()));
			const std::string base = lobby;
			if (random) {
				search();
			} else if (is_host) {
				st = Step::opening;
				const std::string body = me_json(want_code.empty() ? "" : ",\"want\":" + vfnet::json::quote(want_code) +
				                                 (want_code == kept_code ? ",\"key\":" + vfnet::json::quote(kept_key) : std::string()));
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
			if (!ok && reopening && job->reply.status == 404) {
				// (the room let go meanwhile, its host not asking while it connected: the same code opened anew, as its
				// host's, by its key; the lobby keeps it for that)
				reopening = false;
				const std::string base = lobby, body = me_json(",\"want\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key));
				start_job([base, body](Job &j) { j.reply = vfnet::http::request("POST", base + "/v1/host", body, 10000); });
				return;
			}
			reopening = false;
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
			const std::string url = lobby_url("/v1/wait"), body = "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}";
			start_job([url, body](Job &j) { j.reply = vfnet::http::request("POST", url, body, 30000); });
		}
		if (job && job->done) {
			vfnet::json::Value v;
			const vfnet::http::Reply r = job->reply;
			job.reset();
			if (r.status == 200 && vfnet::json::parse(r.body, v)) {
				if (wait_errors >= 3) note.clear();   // (reached again)
				wait_errors = 0;
				const vfnet::json::Value &g = v["guest"];
				if (g.kind == vfnet::json::Value::object) {
					peer_name = g["name"].str();
					room->set_peer(g["cands"].strings(), g["relay"].str(), true);
					st = Step::connecting;
					step_at = now;
					log_line("a player joined the room (" + online_menu::clean_name(peer_name) + "): their addresses " + masked(g["cands"].strings()) +
					         ", relay " + mask(g["relay"].str()) + "; connecting");
				}
			} else if (r.status == 404 && random) {   // (a random match's room lost: another opponent)
				session.reset();
				room->clear_peer();
				key.clear();
				is_host = false;
				ph = Phase::joining;
				peer_name.clear();
				country_peer.clear();
				search();
				return;
			} else if (r.status == 404) {
				room_failed("The room closed (the online service lost it): host again for a new code.");
				return;
			} else if (r.status >= 400 && r.status < 500 && r.status != 429) {   // (an answer that won't change)
				room_failed(lobby_says(r, code, identity.build));
				return;
			} else {   // (the service unreached for a moment: tried again in a while, the room kept)
				retry_at = now + 2000;
				if (++wait_errors == 3) note = "Having trouble reaching the online service: still trying...";
				log_line("waiting: " + (r.status ? "the service said " + std::to_string(r.status) : r.error));
			}
		}
		break;
	case Step::connecting:   // (a host told a player's coming who never makes a sound: gone meanwhile; the session waits
		// for its first hello before it counts the time, so it's counted here)
		if (is_host && session && session->opponent_name().empty() && !session->game_started() && now - step_at > 30000) {
			log_line("no word from the player the room was given: " + room->report(mask));
			host_again("they never reached this side.");
			return;
		}
		break;
	case Step::searching:   // (a random opponent: asked for, then waited for, 20 s at a time)
		if (!job && retry_at > 0 && now >= retry_at) {
			retry_at = 0;
			const std::string url = lobby_url(ticket.empty() ? "/v1/random" : "/v1/random/wait");
			const std::string body = me_json(ticket.empty() ? "" : ",\"ticket\":" + vfnet::json::quote(ticket));
			start_job([url, body](Job &j) { j.reply = vfnet::http::request("POST", url, body, 30000); });
		}
		if (job && job->done) {
			vfnet::json::Value v;
			const vfnet::http::Reply r = job->reply;
			job.reset();
			if (r.status == 404 && !ticket.empty()) { ticket.clear(); retry_at = now; break; }   // (the lobby forgot it: in again)
			if (r.status != 200 || !vfnet::json::parse(r.body, v)) {
				// (no answer three times, or one that won't change: an old lobby, another without random matches)
				const bool never = r.status >= 400 && r.status < 500 && r.status != 429;
				if (never || (r.status == 0 && ++wait_errors >= 3)) { room_failed(lobby_says(r, "", identity.build)); return; }
				retry_at = now + (r.status == 429 ? 10000 : 3000);   // (busy, or a moment's trouble: again soon; busy, less often)
				break;
			}
			wait_errors = 0;
			const std::string role = v["role"].str();
			if (role == "host" && v["code"].str().size() == 6 && !v["key"].str().empty()) {
				// the longer waiting: hosting the room the lobby opened, the other player in it already
				is_host = true;
				ph = Phase::hosting;
				code = v["code"].str();
				key = v["key"].str();
				room_token = strtoull(v["token"].str().c_str(), nullptr, 16);
				peer_name = v["name"].str();
				country_peer = country(v["country"].str());
				ticket.clear();
				room_session(true);
				st = Step::waiting;
				retry_at = 0;
				log_line("a random opponent (" + online_menu::clean_name(peer_name) + ", " + (country_peer.empty() ? "?" : country_peer) + "): hosting room " + room_code());
			} else if (role == "guest" && v["code"].str().size() == 6 && !v["token"].str().empty()) {
				code = v["code"].str();
				ticket.clear();
				joined(v);
				log_line("a random opponent (" + online_menu::clean_name(peer_name) + ", " + (country_peer.empty() ? "?" : country_peer) + "): joining room " + room_code());
			} else {
				ticket = v["ticket"].str();
				players = int(std::clamp(v["players"].n, 0.0, 1e6));
				retry_at = now;   // (the next wait at once: it's the lobby that waits)
			}
		}
		break;
	case Step::finding:
		if (!job && retry_at > 0 && now >= retry_at) {   // (the room not open yet, and waited for: asked again)
			retry_at = 0;
			const std::string base = lobby, body = me_json(",\"code\":" + vfnet::json::quote(code));
			start_job([base, body](Job &j) { j.reply = vfnet::http::request("POST", base + "/v1/join", body, 10000); });
		}
		if (job && job->done) {
			vfnet::json::Value v;
			const vfnet::http::Reply r = job->reply;
			job.reset();
			// (Play again: the host's room not open again yet. Every 4 s: the lobby's limit is 20 joins a minute)
			if ((r.status == 404 || r.status == 429) && patient()) { retry_at = now + 4000; break; }
			if (r.status != 200 || !vfnet::json::parse(r.body, v) || v["token"].str().empty()) { room_failed(lobby_says(r, room_code(), identity.build)); return; }
			patient_until = 0;
			joined(v);
		}
		break;
	default:
		break;
	}
}

// The room found (a code's, or a random opponent's): the host's addresses and the session's token, and connecting
void Online::joined(const vfnet::json::Value &v)
{
	room_token = strtoull(v["token"].str().c_str(), nullptr, 16);
	peer_name = v["name"].str();
	country_peer = country(v["country"].str());
	room->set_peer(v["cands"].strings(), v["relay"].str(), false);
	room_session(false);
	st = Step::connecting;
	log_line("found room " + room_code() + " (" + online_menu::clean_name(peer_name) + "): their addresses " + masked(v["cands"].strings()) + ", relay " +
	         mask(v["relay"].str()) + "; connecting");
}

// Into the lobby's queue for a random opponent (again: one who didn't get going)
void Online::search()
{
	st = Step::searching;
	ticket.clear();
	players = 0;
	wait_errors = 0;
	retry_at = now;
	log_line("searching for a random opponent");
}

// Two letters, or nothing
std::string Online::country(const std::string &c)
{
	return c.size() == 2 && isupper(uint8_t(c[0])) && isupper(uint8_t(c[1])) ? c : "";
}

void Online::stop()
{
	if (room && ph != Phase::playing && st == Step::connecting) log_line("stopped while connecting: " + room->report(mask));
	if (rooms && st == Step::searching && !ticket.empty()) {   // (out of the queue: no one's matched with a game gone)
		farewell(lobby_url("/v1/random/leave"), me_json(",\"ticket\":" + vfnet::json::quote(ticket)));
		ticket.clear();
	}
	// a request to the lobby still on its way: what it brings back (a room opened, a place in the queue, a match
	// made) closed or left as soon as it's here, not left for the lobby's timeouts with someone matched to it
	if (rooms && job && !job->done && (st == Step::opening || st == Step::searching)) {
		const std::string base = lobby, me = me_json("");
		farewells++;
		std::thread([j = job, base, me] {
			for (int i = 0; i < 400 && !j->done && !quitting; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
			vfnet::json::Value v;
			if (j->done && j->reply.status == 200 && vfnet::json::parse(j->reply.body, v)) {
				const std::string code = v["code"].str(), key = v["key"].str(), ticket = v["ticket"].str();
				if (!code.empty() && !key.empty())
					vfnet::http::request("POST", base + "/v1/close", "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}", 5000);
				else if (!ticket.empty() && !me.empty())
					vfnet::http::request("POST", base + "/v1/random/leave", me.substr(0, me.size() - 1) + ",\"ticket\":" + vfnet::json::quote(ticket) + "}", 5000);
			}
			farewells--;
		}).detach();
	}
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
			// (the log's figures from this session's own: the last match's would wrap the first line's differences)
			logged_at = now;
			logged_rb = logged_resim = logged_stalls = logged_waits = logged_steps = 0;
			logged_save = logged_run = logged_resim_ms = logged_hash = 0;
			if (rooms) {
				log_line(std::string("connected ") + (room->via_relay() ? "through a relay" : "directly") + ", " + mask(room->way()) + ": " + room->report(mask));
				close_room();   // (the code's done its job)
			}
			notices.push_back({ Notice::joined, "Playing " + e.text + ": press Start" });
			// (on one network through the relay, only because this Mac may not reach it: slower than need be, said on the
			// versus screen)
			local_slow = rooms && room && room->via_relay() && room->local_blocked();
			score = {};
			adelay = {};
			round_ends_seen = 0;
			round_end_frame = 0;
			both_in = false;
			random_misses = 0;
			out_scan[0] = out_scan[1] = SIZE_MAX;
			out_pressed[0] = out_pressed[1] = false;
			start_scan[0] = start_scan[1] = 0;
			start_seen[0] = start_seen[1] = false;
			note.clear();
			ticket.clear();   // (out of the queue: the lobby's done with it)
			break;
		case vfnet::Session::Event::ended: {
			const auto why = session->end_reason();
			const bool was_playing = ph == Phase::playing;
			// a host goes on waiting after a player who couldn't join (another version, another ROM, gone quiet
			// before the game started); anything else ends it
			if (!was_playing && is_host && (why == vfnet::Session::End::rejected || why == vfnet::Session::End::timeout || why == vfnet::Session::End::opponent_quit)) {
				if (rooms) { host_again(e.text); return; }
				notices.push_back({ Notice::refused, "A player couldn't join: " + e.text });
				note = "A player couldn't join: " + e.text;
				std::string err;
				if (!open_host(err)) { notices.push_back({ Notice::ended, err }); session.reset(); link.reset(); ph = Phase::off; }
				return;
			}
			if (was_playing && why != vfnet::Session::End::quit) {
				const std::string who = opponent();
				ended(why == vfnet::Session::End::opponent_quit ? who + " left the match."
				      : why == vfnet::Session::End::disconnected ? who + "'s game stopped answering."
				      : why == vfnet::Session::End::desync ? "The match went out of step." : e.text,
				      why == vfnet::Session::End::disconnected);
			}
			if (room && !was_playing) log_line("didn't connect: " + room->report(mask));
			// a random opponent never reached (gone meanwhile, or no way between the two): another, twice more at most
			if (!was_playing && !is_host && rooms && random && room && why != vfnet::Session::End::quit && ++random_misses < 3) {
				session.reset();
				room->clear_peer();
				ph = Phase::joining;
				peer_name.clear();
				country_peer.clear();
				search();
				return;
			}
			if (!was_playing && !is_host && why != vfnet::Session::End::quit) {
				failure = e.text;
				fail_step = rooms ? st : Step::connecting;
				if (rooms && room && room->local_blocked())   // (only a Mac's refuses like this)
					failure = "This Mac blocked the game from the local network: System Settings > Privacy & Security > Local Network, turn on "
					          "Virtua Fighter 3 Recomp (off and on again if it's on), then try again.";
				else if (rooms && why == vfnet::Session::End::timeout)
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
	// Auto's input delay (vfnet::AutoDelay): from the ping, enough that the game runs at full speed at any ping, up
	// at once when it must, down between rounds; where a frame costs this machine much of its time (a Steam Deck's
	// 11 ms of 17), shorter rollbacks and a frame's delay more, each frame run again costing it as much again
	if (session && ph == Phase::playing && auto_delay) {
		const auto &stt = session->stats();
		const double step_ms = stt.steps ? (stt.save_ms + stt.run_ms) / double(stt.steps) : 0.0;
		if (!auto_done) adelay.slow = step_ms > 7.0;   // (judged once, as the match starts: not changed under it)
		// (a round's just ended: its end's seen ~half a second after it, and the game shows who won for seconds more,
		// no one in control; a delay lowered then drops nothing anyone pressed)
		if (score.round_ends != round_ends_seen) { round_ends_seen = score.round_ends; round_end_frame = session->frame(); }
		const bool between_rounds = round_ends_seen > 0 && session->frame() - round_end_frame < 90;
		const int was = session->delay(), d = adelay.update(*session, now, between_rounds);
		if (d >= 0 && d != was) {
			session->set_delay(d);
			log_line((auto_done ? "input delay " + std::to_string(was) + " to " : "Auto input delay ") + std::to_string(d) + " (" + adelay.why + ")");
		} else if (d >= 0 && !auto_done)
			log_line("Auto input delay " + std::to_string(d) + " (" + adelay.why + ")");
		if (d >= 0) auto_done = true;
	}
}

bool Online::step(uint16_t input)
{
	if (!(ph == Phase::playing && session && session->step(input))) return false;
	score.frame(board);   // (the score between the two, from the game's RAM: vf3net::Score)
	if (score.players() == 3) both_in = true;
	return true;
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
	adelay = {};   // (Auto again: from the ping as it is now)
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
		case Step::waiting: return "Send it to your opponent: they choose Join and type it";
		case Step::searching:
			return players > 1 ? "Looking for an opponent near you (" + std::to_string(players - 1) + " more searching)..." : "Looking for an opponent near you...";
		case Step::finding:
			if (patient()) return "Waiting for " + (patient_for.empty() ? std::string("the host") : patient_for) + " to open room " + room_code() + " again...";
			return "Finding room " + room_code() + "...";
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

bool Online::connection(game_menu::net_cells &c, bool detailed) const
{
	if (!session || ph != Phase::playing) return false;
	const auto &stt = session->stats();
	c = {};
	c.name = opponent();
	c.relay = via_relay();
	c.interrupted = session->interrupted();
	c.ping = int(stt.rtt_ms + 0.5);
	c.delay = session->delay();
	c.rollback = stt.rollback_max;
	c.level = c.interrupted ? 2 : c.ping < 60 && c.rollback <= 2 ? 0 : c.ping < 120 && c.rollback <= 4 ? 1 : 2;
	if ((c.detailed = detailed)) {
		c.jitter = stt.jitter_ms;
		c.rb_avg = stt.rollbacks ? double(stt.resim_frames) / double(stt.rollbacks) : 0.0;
		c.ahead = stt.lead;
		c.frame_ms = stt.steps ? (stt.save_ms + stt.run_ms + stt.resim_ms + stt.hash_ms) / double(stt.steps) : 0.0;
	}
	return true;
}

// ---- the menu ----

namespace online_menu {

namespace {

const std::string NAME_LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-!?";
const std::string ADDRESS_LETTERS = "0123456789.:abcdefghijklmnopqrstuvwxyz-[]";

menu_item delay_row(Online &o, OnlineSettings &s, std::function<void()> changed)
{
	return { "Input delay",
		"The frames between pressing a button and it happening, on your side only. Fewer: the game answers sooner, "
		"and the other player's moves can jump a little more (rollback). Auto picks from the ping and this machine's speed, "
		"and keeps the game at full speed against a far opponent with more delay (up to 15); it only ever lowers it between rounds.",
		[&o, &s] {
			if (s.delay >= 0) return std::to_string(s.delay) + (s.delay == 1 ? " frame" : " frames");
			return o.playing() ? "Auto (" + std::to_string(o.delay_now()) + ")" : std::string("Auto");
		},
		[&o, &s, changed](int d) {
			s.delay = std::clamp(s.delay + d, -1, 10);
			if (o.playing()) o.set_delay(s.delay);
			changed();
		} };
}

menu_item connection_row(OnlineSettings &s, std::function<void()> changed)
{
	return { "Connection display", "In a match, small at the top of the screen: the other player's name, the ping, the input delay, "
		"the rollback and whether it's direct or through the relay, the dot green, yellow or red for how good the connection is. "
		"Detailed adds a row: the jitter, the rollbacks' average, how far ahead this side is, and what each frame costs here.",
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

// ---- the screens (assets/ui/screen.rcss) ----

std::string E(const std::string &s) { return menu_escape(s); }

// A country's flag (assets/ui/flags.rcss), "" if there's none for it; and one before a name
std::string flag(const std::string &country, const std::string &cls = "flag")
{
	if (flags::index_of(country) < 0) return "";
	return "<img class=\"" + cls + "\" sprite=\"flag-" + std::string{ char(country[0] | 0x20), char(country[1] | 0x20) } + "\"/>";
}
std::string flag_and(const std::string &country, const std::string &name) { return flag(country) + E(name); }

// The button hints at the bottom: each a pad's button and a keyboard's key (none: not shown that way)
struct Hint { const char *pad, *key, *what; };
std::string foot(bool pad, std::initializer_list<Hint> hints)
{
	std::string r = "<div class=\"foot\">";
	for (const Hint &h : hints) {
		const char *k = pad ? h.pad : h.key;
		if (k && *k) r += "<div class=\"hint\"><div class=\"key\">" + E(k) + "</div><div>" + E(h.what) + "</div></div>";   // (text alone in a flex row isn't drawn: in its own div)
	}
	return r + "</div>";
}

#ifdef __APPLE__
const char *const PASTE_KEY = "Cmd";
#else
const char *const PASTE_KEY = "Ctrl";
#endif

// Letters in boxes: what's typed, the next box waiting (cursor), the rest empty; a dash after the third (a code)
std::string boxes(const std::string &t, size_t n, const std::string &kind, bool cursor, bool dash, const std::string &filled = "set")
{
	std::string r = "<div class=\"codes" + std::string(kind == "nm" ? " name" : "") + "\">";
	for (size_t i = 0; i < n; i++) {
		if (dash && i == 3) r += "<div class=\"dash" + std::string(kind == "big" ? " y" : "") + "\"></div>";
		std::string cls = "box" + (kind.empty() ? "" : " " + kind), in;
		if (i < t.size()) { if (!filled.empty()) cls += " " + filled; in = E(std::string(1, t[i])); }
		else if (cursor && i == t.size()) { cls += " on"; in = "<div class=\"caret\"></div>"; }
		r += "<div class=\"" + cls + "\">" + in + "</div>";
	}
	return r + "</div>";
}

// The steps, left to right: each to do, under way, done or where it failed
enum Mark { todo, now, done, bad };
std::string steps(const std::vector<std::pair<std::string, Mark>> &v)
{
	std::string r = "<div class=\"steps\">";
	for (size_t i = 0; i < v.size(); i++) {
		if (i) r += "<div class=\"ln" + std::string(v[i - 1].second == done ? " done" : "") + "\"></div>";
		static const char *const cls[] = { "", " now", " done", " bad" };
		const std::string in = v[i].second == done ? "<img src=\"check.bmp\"/>" : v[i].second == bad ? "\xC3\x97" : v[i].second == now ? "<div class=\"pip\"></div>" : "";   // (×)
		r += "<div class=\"st" + std::string(cls[v[i].second]) + "\"><div class=\"dot\">" + in + "</div><div class=\"lbl\">" + E(v[i].first) + "</div></div>";
	}
	return r + "</div>";
}

// A pad's letters: one item each (first: the first's index), in rows; then Delete (and OK) on the last row
std::string keypad(const std::string &letters, size_t per_row, int first, int sel, bool ok, const std::string &cls)
{
	std::string r = "<div class=\"kb " + cls + "\">";
	const size_t n = letters.size();
	const size_t rows = (n + 2 + (ok ? 1 : 0) + per_row - 1) / per_row;
	size_t at = 0;
	for (size_t row = 0; row < rows; row++) {
		r += "<div class=\"kbrow\">";
		for (size_t c = 0; c < per_row && at < n; c++, at++)
			r += "<div " + menu_row(first + int(at), sel, "k") + ">" + E(std::string(1, letters[at])) + "</div>";
		if (row + 1 == rows) {
			r += "<div " + menu_row(first + int(n), sel, "k wide") + ">DELETE</div>";
			if (ok) r += "<div " + menu_row(first + int(n) + 1, sel, "k ok") + ">OK</div>";
		}
		r += "</div>";
	}
	return r + "</div>";
}

// Typing into a screen (a name, a code): a keyboard types, Backspace takes one off, Enter's done, Esc goes back, and
// Ctrl/Cmd+V pastes; a pad picks letters on the screen with A, X takes one off, Y pastes, Start's done, B goes back
struct Entry
{
	std::string text, problem;
};

bool add_to(Entry &t, const std::string &letters, size_t max, const std::string &what)
{
	bool any = false;
	for (char ch : what) {
		ch = char(toupper(uint8_t(ch)));
		if (letters.find(ch) != std::string::npos && t.text.size() < max) { t.text += ch; any = true; }
	}
	if (any) t.problem.clear();
	return any;
}

void paste_to(Entry &t, const std::string &letters, size_t max)
{
	if (char *clip = SDL_GetClipboardText()) {
		add_to(t, letters, max, clip);
		SDL_free(clip);
	}
}

bool entry_event(game_menu &menu, Entry &t, const std::string &letters, size_t max, const SDL_Event &e, const std::function<void()> &done,
                 const std::function<void()> &cancel, const std::function<void()> &typed)
{
	if (e.type == SDL_EVENT_TEXT_INPUT) {
		if (add_to(t, letters, max, e.text.text ? e.text.text : "")) typed();
		return true;
	}
	if (e.type == SDL_EVENT_KEY_DOWN) {
		const SDL_Keycode k = e.key.key;
		if (e.key.scancode == SDL_SCANCODE_BACKSPACE) { if (!t.text.empty()) t.text.pop_back(); t.problem.clear(); return true; }
		if (e.key.repeat) return true;
		if (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER) { done(); return true; }
		if (e.key.scancode == SDL_SCANCODE_ESCAPE) { cancel(); return true; }
		if (e.key.scancode == SDL_SCANCODE_V && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) { paste_to(t, letters, max); typed(); return true; }
		// (this key turned typing on: its letter comes only as this key)
		if (menu.text_fresh() && ((k >= 'a' && k <= 'z') || (k >= '0' && k <= '9')) && !(e.key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
			if (add_to(t, letters, max, std::string(1, char(k)))) typed();
			return true;
		}
		return k >= 32 && k < 127 && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI));   // (a letter's key: its text comes as text; Space types, it doesn't press)
	}
	if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
		switch (e.gbutton.button) {
		case SDL_GAMEPAD_BUTTON_WEST: if (!t.text.empty()) t.text.pop_back(); t.problem.clear(); return true;
		case SDL_GAMEPAD_BUTTON_NORTH: paste_to(t, letters, max); typed(); return true;
		case SDL_GAMEPAD_BUTTON_START: done(); return true;
		case SDL_GAMEPAD_BUTTON_EAST: cancel(); return true;
		default: return false;   // (A presses the letter selected; the d-pad moves)
		}
	}
	return false;
}

// The pad's letters for a name and a code
const std::string NAME_PAD = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!.0123456789-";

// Your name: typed (a keyboard) or picked (a pad). then: what was going on before it was asked for (none: just set)
menu_page name_page(OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::function<void()> then)
{
	menu_page p{ "Your name", "", {} };
	auto t = std::make_shared<Entry>();
	t->text = s.name;
	const size_t max = size_t(vfnet::MAX_NAME);
	auto save = [&s, &menu, t, changed, then] {
		const std::string n = clean_name(t->text);
		if (n.empty()) { t->problem = "Your name needs a letter or a number."; return; }
		s.name = n;
		changed();
		menu.pop();
		if (then) then();
	};
	for (char ch : NAME_PAD)
		p.items.push_back({ std::string(1, ch), "", {}, {}, [t, ch, max] { add_to(*t, NAME_LETTERS, max, std::string(1, ch)); } });
	p.items.push_back({ "Delete", "", {}, {}, [t] { if (!t->text.empty()) t->text.pop_back(); } });
	p.items.push_back({ "OK", "", {}, {}, save });
	p.start = 0;
	p.text = true;
	p.on_event = [&menu, t, save, max](const SDL_Event &e) {
		return entry_event(menu, *t, NAME_LETTERS, max, e, save, [&menu] { menu.pop(); }, [] {});
	};
	p.screen = [t, max](int sel, bool pad) {
		std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">YOUR NAME</div>"
		                "<div class=\"sub\">Shown under your energy bar \xC2\xB7 up to 10 letters</div>"
		                "<div style=\"margin-top: " + std::string(pad ? "36dp" : "64dp") + "\">" + boxes(t->text, max, "nm", true, false) + "</div>";
		if (pad) r += keypad(NAME_PAD, 14, 0, sel, true, "name");
		else r += "<div class=\"typed\"><div>Just type it</div><div class=\"small\">Letters, numbers and  !  .  -</div></div>";
		if (!t->problem.empty()) r += "<div class=\"problem\">" + E(t->problem) + "</div>";
		r += "</div>";
		return r + (pad ? foot(true, { { "A", "", "type" }, { "X", "", "delete" }, { "Start", "", "save" }, { "B", "", "cancel" } })
		                : foot(false, { { "", "Enter", "save" }, { "", "Backspace", "delete" }, { "", "Esc", "cancel" } })) + "</div>";
	};
	return p;
}

// Joining with a code: typed or picked, and joined at its 6th letter
menu_page code_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Join with a code", "", {} };
	auto t = std::make_shared<Entry>();
	auto join = [&o, &s, &menu, t, changed] {
		if (t->text.size() != 6) { t->problem = "A code has 6 letters and numbers."; return; }
		std::string err;
		if (!o.join_room(s, t->text, err)) { t->problem = err; return; }
		menu.pop();
		again(o, s, menu, changed);   // (the Online screen: joining, step by step)
	};
	auto typed = [t, join] { if (t->text.size() == 6) join(); };
	for (char ch : CODE_LETTERS)
		p.items.push_back({ std::string(1, ch), "", {}, {}, [t, ch, typed] { if (add_to(*t, CODE_LETTERS, 6, std::string(1, ch))) typed(); } });
	p.items.push_back({ "Delete", "", {}, {}, [t] { if (!t->text.empty()) t->text.pop_back(); t->problem.clear(); } });
	p.start = 0;
	p.text = true;
	p.on_event = [&menu, t, join, typed](const SDL_Event &e) {
		return entry_event(menu, *t, CODE_LETTERS, 6, e, join, [&menu] { menu.pop(); }, typed);
	};
	p.screen = [t](int sel, bool pad) {
		std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">ENTER THE CODE</div>"
		                "<div class=\"sub\">The 6 letters your opponent sent you</div>"
		                "<div style=\"margin-top: " + std::string(pad ? "36dp" : "64dp") + "\">" + boxes(t->text, 6, "", true, true) + "</div>";
		if (pad) r += keypad(CODE_LETTERS, 11, 0, sel, false, "code");
		else r += "<div class=\"typed\"><div class=\"line\"><div>Type it, or paste with</div><div class=\"key\">" + std::string(PASTE_KEY) + "</div><div>+</div><div class=\"key\">V</div></div>"
		          "<div class=\"small\">It joins by itself at the 6th letter</div></div>";
		if (!t->problem.empty()) r += "<div class=\"problem\">" + E(t->problem) + "</div>";
		r += "</div>";
		return r + (pad ? foot(true, { { "A", "", "type" }, { "X", "", "delete" }, { "Y", "", "paste" }, { "B", "", "back" } })
		                : foot(false, { { "", "Backspace", "delete" }, { "", "Esc", "back" } })) + "</div>";
	};
	return p;
}

// Before playing: a name. None yet: it's asked for, then what was chosen goes on.
void named(OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::function<void()> go)
{
	if (!s.name.empty()) { go(); return; }
	menu.push(name_page(s, menu, changed, go));
}

menu_page settings_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Settings", "", {} };
	p.items.push_back(delay_row(o, s, changed));
	p.items.push_back(connection_row(s, changed));
	p.items.push_back({ "Hide my IP address", "On: every online match (a code's or a random one) goes through the relay (Cloudflare's), so the other "
		"player never sees your address. A little more ping. Off: directly when the routers allow it, which is quickest.",
		[&s] { return std::string(s.hide_ip ? "On" : "Off"); }, [&s, changed](int) { s.hide_ip = !s.hide_ip; changed(); } });
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	return p;
}

// A screen page: B / Esc closes the menu (back to the game, what's going on going on), not back a page
bool closes(game_menu &menu, const SDL_Event &e)
{
	if ((e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) ||
	    (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE)) {
		menu.close();
		return true;
	}
	return false;
}

std::string clock(int seconds) { char b[16]; snprintf(b, sizeof b, "%d:%02d", seconds / 60, seconds % 60); return b; }

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

// The Online screen, as things are: hosting, joining, what came of a try that didn't get going, the match just
// over, or the two choices
menu_page page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Online", "", {} };
	const bool hosting = o.phase() == Online::Phase::hosting, joining = o.phase() == Online::Phase::joining;

	if ((hosting || joining) && o.random_match()) {
		// ---- a random opponent: searching, found, connecting ----
		const int stop = int(p.items.size());
		p.items.push_back({ "Stop", "", {}, {}, [&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } });
		p.start = stop;
		p.on_event = [&menu](const SDL_Event &e) { return closes(menu, e); };
		p.screen = [&o, stop](int sel, bool pad) {
			const Online::Step st = o.step_now();
			const bool started = o.starting(), found = st != Online::Step::getting_ready && st != Online::Step::searching;
			const std::string who = o.peer();
			std::string r = "<div class=\"scr\"><div class=\"col\">";
			if (!found)
				r += "<div class=\"title\">FINDING AN OPPONENT</div><div class=\"sub\">&nbsp;</div>";
			else
				r += "<div class=\"title\">OPPONENT FOUND</div><div class=\"sub\">" + flag_and(o.peer_country(), who.empty() ? std::string("Your opponent") : who) + "</div>";
			const std::vector<std::pair<std::string, Mark>> v = { { "SEARCH", found ? done : now }, { "FOUND", found ? done : todo },
				{ "CONNECT", started ? done : found ? now : todo }, { "FIGHT", started ? now : todo } };
			r += "<div class=\"stepsat\">" + steps(v) + "</div><div class=\"status\">" + E(o.status()) + "</div>";
			r += "<div class=\"btns\"><div " + menu_row(stop, sel, "btn") + ">STOP</div></div></div>";
			return r + foot(pad, { { "A", "Enter", "select" }, { "B", "Esc", "play solo while you wait" } }) + "</div>";
		};
		return p;
	}

	if ((hosting || joining) && o.by_code()) {
		// ---- a room: its code, and how far it's got ----
		auto copied = std::make_shared<bool>(false);
		int copy = -1;
		if (hosting) {
			copy = int(p.items.size());
			p.items.push_back({ "Copy code", "", {}, {}, [&o, copied] {
				if (o.room_code().empty()) return;
				SDL_SetClipboardText(o.room_code().c_str());
				*copied = true;
			} });
		}
		const int stop = int(p.items.size());
		p.items.push_back({ "Stop", "", {}, {}, [&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } });
		p.start = copy >= 0 ? copy : stop;
		p.on_event = [&menu](const SDL_Event &e) { return closes(menu, e); };
		p.screen = [&o, hosting, copy, stop, copied](int sel, bool pad) {
			const Online::Step st = o.step_now();
			const bool started = o.starting();
			std::string r = "<div class=\"scr\"><div class=\"col\">";
			std::vector<std::pair<std::string, Mark>> v;
			if (hosting) {
				r += "<div class=\"title\">YOUR ROOM CODE</div>";
				const std::string c = o.room_code();
				r += "<div style=\"margin-top: 28dp\">" + boxes(c.empty() ? "" : c.substr(0, 3) + c.substr(4), 6, "big", false, true, "") + "</div>";
				const bool open = st == Online::Step::waiting || st == Online::Step::connecting;
				v = { { "ROOM OPEN", open ? done : now }, { "OPPONENT", st == Online::Step::connecting ? done : open ? now : todo },
				      { "CONNECT", started ? done : st == Online::Step::connecting ? now : todo }, { "FIGHT", started ? now : todo } };
			} else {
				const std::string who = o.peer();
				r += "<div class=\"title\">JOINING " + E(o.room_code()) + "</div><div class=\"sub\">" + (who.empty() ? std::string("&nbsp;") : flag_and(o.peer_country(), who + "'s room")) + "</div>";
				const bool found = st == Online::Step::connecting;
				v = { { "CODE", done }, { "ROOM FOUND", found ? done : now }, { "CONNECT", started ? done : found ? now : todo }, { "FIGHT", started ? now : todo } };
			}
			r += "<div class=\"stepsat\">" + steps(v) + "</div>";
			// the state under the steps, as on every screen with them (a host's: the code copied, or a player who couldn't join)
			if (hosting && *copied) r += "<div class=\"status good\">Copied: paste it to your opponent</div>";
			else if (hosting && !o.host_note().empty()) r += "<div class=\"status bad\">" + E(o.host_note()) + "</div>";
			else {
				std::string why = !hosting && st == Online::Step::connecting && !started ? "Directly first, then through the relay. Usually a few seconds." : "";
				r += "<div class=\"status\">" + E(o.status()) + (why.empty() ? "" : "<span class=\"why\">" + E(why) + "</span>") + "</div>";
			}
			r += "<div class=\"btns\">";
			if (copy >= 0) r += "<div " + menu_row(copy, sel, "btn") + ">COPY CODE</div>";
			r += "<div " + menu_row(stop, sel, "btn") + ">STOP</div></div>";
			r += "</div>";
			return r + foot(pad, { { "A", "Enter", "select" }, { "B", "Esc", hosting ? "play solo while you wait" : "back to the game" } }) + "</div>";
		};
		return p;
	}

	if (hosting || joining) {
		// ---- by address: where to join, or where it's joining ----
		const int stop = int(p.items.size());
		p.items.push_back({ "Stop", "", {}, {}, [&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } });
		p.start = stop;
		p.on_event = [&menu](const SDL_Event &e) { return closes(menu, e); };
		p.screen = [&o, &s, hosting, stop](int sel, bool pad) {
			const bool started = o.starting(), talking = !o.peer().empty();
			std::string r = "<div class=\"scr\"><div class=\"col\">";
			std::vector<std::pair<std::string, Mark>> v;
			if (hosting) {
				const std::string here = o.local_address().empty() ? std::string("No network") : o.local_address() + (o.local_port() != 7700 ? ":" + std::to_string(o.local_port()) : "");
				r += "<div class=\"title\">WAITING FOR A PLAYER</div><div class=\"sub\">On your network, or a VPN: they choose Play by address, then Join, and type this</div>"
				     "<div class=\"address\">" + E(here) + "</div>";
				v = { { "OPPONENT", talking ? done : now }, { "CONNECT", started ? done : talking ? now : todo }, { "FIGHT", started ? now : todo } };
			} else {
				r += "<div class=\"title\">JOINING " + E(s.address) + "</div><div class=\"sub\">By address</div>";
				v = { { "CONNECT", started ? done : now }, { "FIGHT", started ? now : todo } };
			}
			r += "<div class=\"stepsat\">" + steps(v) + "</div><div class=\"status\">" + E(o.status()) + "</div>";
			r += "<div class=\"btns\"><div " + menu_row(stop, sel, "btn") + ">STOP</div></div></div>";
			return r + foot(pad, { { "A", "Enter", "select" }, { "B", "Esc", hosting ? "play solo while you wait" : "back to the game" } }) + "</div>";
		};
		return p;
	}

	auto problem = std::make_shared<std::string>();

	if (!o.join_failure().empty()) {
		// ---- a try that didn't get going: where it stopped, why, and again ----
		const int again_i = int(p.items.size());
		p.items.push_back({ "Try again", "", {}, {}, [&o, &s, &menu, problem, changed] {
			std::string err;
			if (!o.try_again(s, err)) { *problem = err; return; }
			again(o, s, menu, changed);
		} });
		const int back = int(p.items.size());
		p.items.push_back({ "Back", "", {}, {}, [&o, &s, &menu, changed] { o.forget_failure(); again(o, s, menu, changed); } });
		p.start = again_i;
		p.on_event = [&o, &s, &menu, changed](const SDL_Event &e) {
			if ((e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) ||
			    (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_BACKSPACE))) {
				o.forget_failure();
				again(o, s, menu, changed);
				return true;
			}
			return false;
		};
		p.screen = [&o, &s, again_i, back, problem](int sel, bool pad) {
			const Online::Try t = o.last_try();
			const Online::Step f = o.failed_at();
			std::string sub, r = "<div class=\"scr\"><div class=\"col\"><div class=\"title red\">COULDN'T CONNECT</div>";
			std::vector<std::pair<std::string, Mark>> v;
			if (t == Online::Try::random) {
				sub = "Finding a random opponent";
				const bool found = f == Online::Step::connecting || f == Online::Step::waiting;
				v = { { "SEARCH", found ? done : bad }, { "FOUND", found ? done : todo }, { "CONNECT", f == Online::Step::connecting ? bad : todo }, { "FIGHT", todo } };
			} else if (t == Online::Try::join_room) {
				const std::string who = o.peer();
				sub = "to " + (who.empty() ? std::string("room ") : who + "'s room ") + o.room_code();
				const bool found = f == Online::Step::connecting;
				v = { { "CODE", done }, { "ROOM FOUND", found ? done : bad }, { "CONNECT", found ? bad : todo }, { "FIGHT", todo } };
			} else if (t == Online::Try::host_room) {
				sub = "Couldn't open a room";
				const bool open = f == Online::Step::waiting || f == Online::Step::connecting;
				v = { { "ROOM OPEN", open ? done : bad }, { "OPPONENT", f == Online::Step::connecting ? done : open ? bad : todo },
				      { "CONNECT", f == Online::Step::connecting ? bad : todo }, { "FIGHT", todo } };
			} else {
				sub = t == Online::Try::join ? "to " + s.address : "Hosting by address";
				v = { { "CONNECT", bad }, { "FIGHT", todo } };
			}
			r += "<div class=\"sub\">" + E(sub) + "</div><div class=\"stepsat\">" + steps(v) + "</div>"
			     "<div class=\"status plain\">" + E(o.join_failure()) + "</div>";
			if (!problem->empty()) r += "<div class=\"problem\">" + E(*problem) + "</div>";
			r += "<div class=\"btns\"><div " + menu_row(again_i, sel, "btn") + ">TRY AGAIN</div><div " + menu_row(back, sel, "btn") + ">BACK</div></div></div>";
			return r + foot(pad, { { "A", "Enter", "select" }, { "B", "Esc", "back" } }) + "</div>";
		};
		return p;
	}

	if (const Online::LastMatch &m = o.last_match(); m.any) {
		// ---- the match just over: who, how long, how it was connected, and the same again ----
		const int again_i = int(p.items.size());
		p.items.push_back({ "Play again", "", {}, {}, [&o, &s, &menu, problem, changed] {
			std::string err;
			if (!o.play_again(s, err)) { *problem = err; return; }
			again(o, s, menu, changed);
		} });
		int another = -1;
		if (o.last_match().random) {
			another = int(p.items.size());
			p.items.push_back({ "New opponent", "", {}, {}, [&o, &s, &menu, problem, changed] {
				std::string err;
				if (!o.find_random(s, err)) { *problem = err; return; }
				again(o, s, menu, changed);
			} });
		}
		const int back = int(p.items.size());
		p.items.push_back({ "Back", "", {}, {}, [&o, &s, &menu, changed] { o.set_last_match({}); again(o, s, menu, changed); } });
		p.start = again_i;
		p.on_event = [&menu](const SDL_Event &e) { return closes(menu, e); };
		p.screen = [&o, &s, again_i, another, back, problem](int sel, bool pad) {
			const Online::LastMatch &m = o.last_match();
			const std::string who = m.opponent.empty() ? std::string("your opponent") : m.opponent;
			std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title" + std::string(m.lost ? " red\">CONNECTION LOST" : "\">MATCH OVER") + "</div>";
			// (each name with its flag, out from the middle: player 1's on its left, player 2's on its right)
			const std::string p1 = m.was_host ? E(s.name) : E(m.opponent.empty() ? "?" : m.opponent), p2 = m.was_host ? E(m.opponent.empty() ? "?" : m.opponent) : E(s.name);
			const std::string me = m.was_host ? flag(m.country[0]) + p1 : p2 + flag(m.country[1]), them = m.was_host ? p2 + flag(m.country[1]) : flag(m.country[0]) + p1;
			// (between the names: the matches won, read from the game, player 1's first; VS if none was finished)
			const std::string mid = m.wins[0] + m.wins[1] > 0 ? "<div class=\"score\">" + std::to_string(m.wins[0]) + " \xE2\x80\x93 " + std::to_string(m.wins[1]) + "</div>"
			                                                 : std::string("<div class=\"vs\">VS</div>");
			r += "<div class=\"mini\"><div class=\"n" + std::string(m.was_host ? " me" : "") + "\">" + (m.was_host ? me : them) + "</div>" + mid +
			     "<div class=\"n" + std::string(m.was_host ? "" : " me") + "\">" + (m.was_host ? them : me) + "</div></div>";
			r += "<div class=\"sub\">" + E(m.why) + "</div>";
			r += "<div class=\"stats\"><div class=\"stat\"><div class=\"v\">" + clock(m.seconds) + "</div><div class=\"l\">PLAYED</div></div>"
			     "<div class=\"stat\"><div class=\"v\">" + std::to_string(m.ping) + " ms</div><div class=\"l\">" + (m.lost ? "LAST PING" : "PING") + "</div></div>"
			     "<div class=\"stat\"><div class=\"v " + (m.relay ? "relay\">RELAY" : "good\">DIRECT") + "</div><div class=\"l\">CONNECTION</div></div></div>";
			if (!problem->empty()) r += "<div class=\"problem\">" + E(*problem) + "</div>";
			r += "<div class=\"btns\"><div " + menu_row(again_i, sel, "btn") + ">PLAY AGAIN</div>" +
			     (another >= 0 ? "<div " + menu_row(another, sel, "btn") + ">NEW OPPONENT</div>" : std::string()) + "<div " + menu_row(back, sel, "btn") + ">BACK</div></div>";
			std::string note;
			if (m.by_code && m.was_host) note = "Play again keeps your room code: " + who + " joins it again";
			else if (m.by_code) note = "Play again joins " + (m.code.size() == 6 ? m.code.substr(0, 3) + "-" + m.code.substr(3) : std::string("the room")) + " as soon as " + who + " opens it";
			else if (m.was_host) note = "Play again waits for " + who + " on the same address";
			else note = "Play again joins " + s.address + " again: " + who + " hosts first";
			if (m.lost) note = "Usually a Wi-Fi drop or a game closed. " + note;
			r += "<div class=\"note\">" + E(note) + "</div></div>";
			return r + foot(pad, { { "A", "Enter", "select" }, { "B", "Esc", "play solo" } }) + "</div>";
		};
		return p;
	}

	// ---- the two choices ----
	const int chip = 0, rnd = 1, host = 2, join = 3, settings = 4;
	auto change_name = [&s, &menu, changed] { menu.push(name_page(s, menu, changed, {})); };
	p.items.push_back({ "Your name", "", {}, {}, change_name });
	p.items.push_back({ "Random", "", {}, {}, [&o, &s, &menu, problem, changed] {
		named(s, menu, changed, [&o, &s, &menu, problem, changed] {
			std::string err;
			if (!o.find_random(s, err)) { *problem = err; return; }
			again(o, s, menu, changed);
		});
	} });
	p.items.push_back({ "Host", "", {}, {}, [&o, &s, &menu, problem, changed] {
		named(s, menu, changed, [&o, &s, &menu, problem, changed] {
			std::string err;
			if (!o.host_room(s, err)) { *problem = err; return; }
			again(o, s, menu, changed);
		});
	} });
	p.items.push_back({ "Join", "", {}, {}, [&o, &s, &menu, changed] {
		named(s, menu, changed, [&o, &s, &menu, changed] { menu.push(code_page(o, s, menu, changed)); });
	} });
	p.items.push_back({ "Settings", "", {}, {}, [&o, &s, &menu, changed] { menu.push(settings_page(o, s, menu, changed)); } });
	p.start = rnd;
	p.on_event = [change_name](const SDL_Event &e) {
		if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) { change_name(); return true; }
		return false;
	};
	p.screen = [&o, &s, problem](int sel, bool pad) {
		std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">ONLINE</div>";
		r += "<div " + menu_row(chip, sel, "chip") + ">";
		if (s.name.empty()) r += "<div class=\"me\">Set your name</div>";
		else r += "<div class=\"who\">Playing as</div>" + flag(o.my_country()) + "<div class=\"me\">" + E(s.name) + "</div>";
		if (pad) r += "<div class=\"key\">Y</div><div class=\"who\">change</div>";
		const int players = o.players_online(s);   // (playing at random now: on the Random card)
		r += "</div><div class=\"tiles\">"
		     "<div " + menu_row(rnd, sel, "tile") + "><div class=\"t\">RANDOM</div><div class=\"s\">" + (players > 0 ? "<span class=\"live\"></span>" + std::to_string(players) + " online" : std::string("Anyone near you")) + "</div></div>"
		     "<div " + menu_row(host, sel, "tile") + "><div class=\"t\">HOST</div><div class=\"s\">Get a code for a friend</div></div>"
		     "<div " + menu_row(join, sel, "tile") + "><div class=\"t\">JOIN</div><div class=\"s\">Type a friend's code</div></div></div>";
		if (!problem->empty()) r += "<div class=\"problem\">" + E(*problem) + "</div>";
		r += "<div class=\"more\"><div " + menu_row(settings, sel, "link") + ">Settings</div></div></div>";
		return r + foot(pad, { { "A", "Enter", "select" }, { "B", "Esc", "back" } }) + "</div>";
	};
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
