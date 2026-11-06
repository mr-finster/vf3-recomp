#include "online.h"
#include "online_text.h"
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
#include <set>

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
std::string lobby_says(const vfnet::http::Reply &r, const std::string &code, const std::string &mine);   // (below: the lobby's answer in words)

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
	for (int waited = 0; vfnet::http::active() > 0 && waited < 1200; waited += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
	use_terms("");
	session = std::make_unique<vfnet::Session>(lagged(*link), adapter, match_identity(), config(started_with), true, adapter.host_blob());
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
	feed_last();
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
	use_terms("");
	session = std::make_unique<vfnet::Session>(lagged(*link), adapter, match_identity(), config(s), false);
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
	start_job(gather(lobby, turn_json()));
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
	start_job(gather(lobby, turn_json()));
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
	queue_at = queue_len = 0;
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
	start_job(gather(lobby, turn_json()));
	return true;
}

// Getting ready for a room's match: the STUN server's address, and a relay's login (with this side's country)
std::function<void(Online::Job &)> Online::gather(const std::string &base, const std::string &body)
{
	return [base, body](Online::Job &j) {
		j.stun = vfnet::RoomLink::resolve(STUN_HOST, STUN_PORT);
		j.reply = vfnet::http::request("POST", base + "/v1/turn", body, 8000);
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
	case Try::watch: { const std::string w = watch_tried; return watch(s, w, error); }
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
		// (back in as the RAM has it this frame: the held reading's half a second behind, and the banner stayed over
		// the challenger's own way back in)
		if (score.players_now() >> p & 1) { out[p] = false; continue; }
		// (from the frame the RAM had them out, the held reading seconds later at a match's end, its round counts
		// changing then too: a Start pressed meanwhile took them to the character select, and was missed, so the
		// banner asked again there. A press, not a Start held through the match's end: up from not pressed.)
		// (and a Start pressed in the 3 s before they went out counts too: from the KO on, a Start takes the loser out
		// 2 frames later, straight to New Challenger and the character select (checked on the ROM), so a quick tap's
		// up again by the time they're out (and out_from can be the input delay later still). Missed before: reno's
		// log, a Korean opponent back in a second after losing, twice, with no press seen; reno mashing Start after
		// a loss, the banner up over his own way back in)
		if (out_scan[p] == SIZE_MAX) {
			const size_t from = std::min(out_from[p], inputs.size());
			out_scan[p] = from;
			for (size_t j = from; j > 0 && from - j < 172 && !out_pressed[p]; j--)   // (the last press first)
				if ((inputs[j - 1] & vf3net::START_BUTTON) && (j == 1 || !(inputs[j - 2] & vf3net::START_BUTTON))) {
					out_pressed[p] = true;
					fprintf(stderr, "online: player %d pressed Start at frame %zu, %zu frames before they went out at %zu\n", p + 1, j - 1, from - (j - 1), from);
				}
			out_held_at[p] = inputs.size();
			fprintf(stderr, "online: player %d out of the game at frame %zu (seen at %zu)\n", p + 1, out_scan[p], out_held_at[p]);
		}
		for (; out_scan[p] < inputs.size() && !out_pressed[p]; out_scan[p]++)
			if ((inputs[out_scan[p]] & vf3net::START_BUTTON) && (out_scan[p] == 0 || !(inputs[out_scan[p] - 1] & vf3net::START_BUTTON))) {
				out_pressed[p] = true;
				fprintf(stderr, "online: player %d pressed Start at frame %zu%s\n", p + 1, out_scan[p], out_scan[p] < out_held_at[p] ? " (before the reading caught up: missed before)" : "");
			}
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
				vfnet::json::quote(identity.build) + ",\"content\":" + vfnet::json::quote(hex64(identity.content)) +
				(pool.empty() ? std::string() : ",\"pool\":" + vfnet::json::quote(pool) + ",\"accepts\":" + vfnet::json::list(accepts)) + "}";   // (the count: those this side can meet)
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

// What a relay's login is asked with: which game, its version, its ROM's hash (the lobby gives none to an empty ask)
std::string Online::turn_json() const
{
	return "{\"game\":" + vfnet::json::quote(identity.game) + ",\"version\":" + vfnet::json::quote(identity.build) + ",\"content\":" +
	       vfnet::json::quote(hex64(identity.content)) + "}";
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
	       ",\"relay\":" + vfnet::json::quote(room ? room->relay_address() : "") +
	       (pool.empty() ? std::string() : ",\"pool\":" + vfnet::json::quote(pool) + ",\"accepts\":" + vfnet::json::list(accepts)) + ",\"watchable\":true" +
	       (random ? std::string(",\"qm_listed\":") + (started_with.quick_listed ? "true" : "false") : std::string()) + extra + "}";
}

vfnet::Link &Online::lagged(vfnet::Link &l)
{
	session.reset();   // (the session before, on the old lag link, gone first)
	lag = vfnet::LagLink::from_env(l);   // (tests: VF3_NET_LAG)
	if (!lag) return l;
	log_line(lag->describe());
	return *lag;
}

void Online::room_session(bool host)
{
	if (adapter.settle) adapter.settle();
	vfnet::Config c = config(started_with);
	c.token = room_token;
	session = std::make_unique<vfnet::Session>(lagged(*room), adapter, match_identity(), c, host, host ? adapter.host_blob() : std::vector<uint8_t>());
	session->trace = log_line;
}

bool Online::use_terms(const std::string &as, const std::string &them)
{
	// (the other player's own Dural, from its version as the lobby sends it, "them": only for what the match says)
	// (any build's Dural flag: this one's, or an older version's, legacy_dural_flag)
	their_dural = -1;
	if (!them.empty()) {
		their_dural = 0;
		for (const std::string &flag : { std::string(vf3net::DURAL_FLAG), std::string(vf3net::legacy_dural_flag(tb)) })
			for (size_t at = them.find('+'); at != std::string::npos; at = them.find('+', at + 1))
				if (them.compare(at, flag.size(), flag) == 0 && (at + flag.size() == them.size() || them[at + flag.size()] == '+')) their_dural = 1;
	}
	const std::string t = as.empty() ? identity.build : as;
	vf3net::Terms m;
	if (!vf3net::terms_of(t, identity.build, pool, tb, m)) return false;
	terms = t;
	adapter.host_terms = m.host_terms;
	adapter.team_one = m.team_one;
	adapter.dural = m.dural;   // (the terms': a pool's marked when either player has Dural on; a guest's from the host's blob, at the start)
	adapter.dural_legacy = m.dural_legacy;   // (an older version's Dural flag: its patches, as it plays them)
	adapter.stage_select_on = m.host_terms ? own_stage : m.stage;
	if (t != identity.build) log_line("the match's terms: " + t + (m.host_terms ? " (the host's settings)" : " (the other player's version)"));
	return true;
}

std::string Online::match_said() const
{
	const std::string whose = adapter.host_terms ? (is_host ? " (yours, as host)" : " (the host's)") : "";
	std::string s = std::string("Stage select ") + (adapter.stage_select_on ? "on" : "off");
	if (adapter.dural) {   // (whose: a pool's Dural is on when either player has her on)
		const char *by = own_dural && their_dural == 1 ? " (both)" : own_dural ? " (yours)" : their_dural == 1 ? " (theirs)" : "";
		s += std::string(", Dural on") + (adapter.dural_legacy ? " (older version's)" : "") + by;
	} else if (own_dural) s += ", Dural off";
	if (tb) {
		const size_t o = size_t(board.profile->settings.team_mode), at = board.profile->settings.at + o;
		const int one = adapter.team_one >= 0 ? adapter.team_one : o && board.backup.size() > at && board.backup[at] <= 1 ? board.backup[at] : -1;
		if (one >= 0) s += one ? ", 1 vs 1" : ", 3 vs 3";
	}
	return s + whose;
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
	room_ws_drop();
	if (!rooms || !is_host || key.empty()) return;
	farewell(lobby_url("/v1/close"), "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}");
	kept_code = code;   // (a Play again opens it again: the lobby keeps the code for its host, by this key)
	kept_key = key;
	key.clear();
}


// ---- watching (src/net/watch.h): this match's watchers, and watching another ----

struct Online::Fetch
{
	std::atomic<bool> done{ false };
	vfnet::http::Reply reply;
};

std::shared_ptr<Online::Fetch> Online::fetch(const std::string &method, const std::string &url, std::string body, int timeout_ms, size_t most)
{
	auto f = std::make_shared<Fetch>();
	std::thread([f, method, url, body = std::move(body), timeout_ms, most] {   // (its own copy: a stop meanwhile leaves it to finish and be forgotten)
		f->reply = vfnet::http::request(method, url, body, timeout_ms, most);
		f->done = true;
	}).detach();
	return f;
}

std::string Online::watch_code() const
{
	if (wid.size() != 6) return "";
	return wid.substr(0, 3) + "-" + wid.substr(3);
}

std::string Online::player_name(int p) const
{
	if (ph == Phase::watching) return online_menu::clean_name(watch_meta.name[p & 1]);
	if (!session) return "";
	return p == local_player() ? online_menu::clean_name(identity.name) : opponent();
}

std::string Online::player_country(int p) const
{
	if (ph == Phase::watching) return country(watch_meta.country[p & 1]);
	return p == local_player() ? country_me : country_peer;
}

int Online::watchers() const
{
	if (ph == Phase::watching) return watch_watchers;
	if (ph != Phase::playing || wid.empty()) return -1;
	if (is_host) return feed.on() ? feed.watchers() : -1;
	return std::max(guest_watchers, 0);
}

// The host's: the match's start (the session's blob, who plays, the terms), then its frames once a second
void Online::feed_begin()
{
	vfnet::watch::Start s;
	s.match = vfnet::watch::new_match_id();
	s.meta.game = identity.game;
	s.meta.terms = terms;
	s.meta.content = hex64(identity.content);
	s.meta.build = identity.build;
	for (int p = 0; p < 2; p++) {
		s.meta.name[p] = player_name(p);
		s.meta.country[p] = player_country(p);
	}
	s.blob = session->blob();
	feed.begin(s, now);
	log_line("this match may be watched: watch code " + watch_code());
}

// (how often the lobby asks the host to send: 5 a second while anyone watches, slower under load, every 30 s while
// nobody does)
static double feed_every(const vfnet::json::Value &v)
{
	return v["every"].kind == vfnet::json::Value::number ? std::clamp(v["every"].n, 200.0, 30000.0) : 200;
}

// (the lobby's month of watching used up: this match sent no more, quietly; it goes on as ever)
bool Online::lobby_off(const vfnet::json::Value &v)
{
	if (v["off"].kind != vfnet::json::Value::boolean || !v["off"].b) return false;
	log_line("watching: the lobby takes no more matches this month: this one isn't sent (it goes on)");
	feed = {};
	if (feed_ws) std::thread([w = std::move(feed_ws)] {}).detach();
	feed_ws_failed = true;
	feed_ws_waiting = false;
	return true;
}

void Online::feed_poll()
{
	for (FeedSlot &f : feed_slots)
		if (f.job && f.job->done) {   // (the lobby's answer: what it has, how many watch)
			vfnet::json::Value v;
			const vfnet::http::Reply &r = f.job->reply;
			const bool ok = r.status == 200 && vfnet::json::parse(r.body, v) && (f.up.start ? v["ok"].kind == vfnet::json::Value::boolean : v["has"].kind == vfnet::json::Value::number);
			const double has = v["has"].n;
			feed.done(f.up, ok, has < 0 ? vfnet::watch::NONE : uint32_t(std::clamp(has, 0.0, 1e9)), int(std::clamp(v["watchers"].n, 0.0, 1e7)), now);
			if (ok) feed.interval_ms = feed_every(v);   // (nobody watching, or the lobby busy: sending less often)
			if (lobby_off(v)) { f.job.reset(); return; }
			if (!ok && feed.failed()) log_line("watching: the lobby wasn't reached for a minute: this match can't be watched any more (it goes on)");
			f.job.reset();
		}
	if (persist && is_host && ph == Phase::playing && !key.empty()) {   // (a room that stays: still there, its match on)
		vfnet::http::Reply ignored;
		if (!random && !room_ws_failed) while (room_ws_poll(ignored)) {}   // (its socket kept: it holds the room; what comes on it means nothing now)
		if (!room_ws_open() && now >= hold_at) {   // (no socket: held over HTTPS, every 15 s)
			hold_at = now + 15000;
			farewell(lobby_url("/v1/hold"), "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}");
		}
	}
	if (feed.on() && !wid.empty() && !feed_ws && !feed_ws_failed)   // (the host's WebSocket: the uploads on it, from now on)
		feed_ws = std::make_shared<vfnet::http::Socket>(lobby_url("/v1/watch/feed?w=" + wid + "&k=" + wkey));
	if (feed_ws) {
		const vfnet::http::Socket::State fs = feed_ws->state();
		vfnet::http::Socket::Message msg;
		while (feed_ws->take(msg)) {   // (the answer to the last upload, as a POST's; or the lobby's word that a watcher came)
			if (msg.binary) continue;
			vfnet::json::Value v;
			const bool parsed = vfnet::json::parse(msg.data, v);
			if (parsed && v["wake"].kind == vfnet::json::Value::boolean && v["wake"].b) {   // (sent at once, not at the next idle upload)
				feed.interval_ms = feed_every(v);
				continue;
			}
			if (!feed_ws_waiting) continue;   // (nothing asked)
			const bool ok = parsed && (feed_ws_up.start ? v["ok"].kind == vfnet::json::Value::boolean : v["has"].kind == vfnet::json::Value::number);
			const double has = v["has"].n;
			feed.done(feed_ws_up, ok, has < 0 ? vfnet::watch::NONE : uint32_t(std::clamp(has, 0.0, 1e9)), int(std::clamp(v["watchers"].n, 0.0, 1e7)), now);
			if (ok) feed.interval_ms = feed_every(v);
			feed_ws_waiting = false;
			if (lobby_off(v)) return;
		}
		if (fs == vfnet::http::Socket::State::closed || (feed_ws_waiting && now - feed_ws_sent > 3000)) {   // (refused, gone, or too slow: the HTTPS way)
			log_line("watching: the host's WebSocket " + (fs == vfnet::http::Socket::State::closed ? "closed (" + feed_ws->error() + ")" : std::string("too slow")) + ": sending over HTTPS");
			if (feed_ws_waiting) feed.done(feed_ws_up, false, 0, 0, now);
			feed_ws_waiting = false;
			std::thread([w = std::move(feed_ws)] {}).detach();   // (let go on a thread: never a wait here)
			feed_ws_failed = true;
		} else {
			if (fs == vfnet::http::Socket::State::open && !feed_ws_waiting && feed.next(now, feed_ws_up)) {
				feed_ws->send(std::string(feed_ws_up.body.begin(), feed_ws_up.body.end()));
				feed_ws_waiting = true;
				feed_ws_sent = now;
			}
			return;   // (connecting, or sending on it: not over HTTPS as well)
		}
	}
	for (FeedSlot &f : feed_slots) {   // (each upload on a connection of its own, kept open: no new one, and its TLS, ten times a second)
		if (f.job || !feed.next(now, f.up)) continue;
		if (!f.client) f.client = std::make_shared<vfnet::http::Client>();
		auto job = std::make_shared<Fetch>();
		const std::string url = lobby_url(std::string("/v1/watch/") + (f.up.start ? "start" : "push") + "?w=" + wid + "&k=" + wkey), body(f.up.body.begin(), f.up.body.end());
		std::thread([job, client = f.client, url, body] { job->reply = client->request("POST", url, body, 3000, 4096); job->done = true; }).detach();
		f.job = job;
		break;   // (one a pass)
	}
	// (the guest: how many watch, asked once a minute while the figures at the top are shown; only shown)
	if (!is_host && !wid.empty() && ph == Phase::playing) {
		if (count_job && count_job->done) {
			vfnet::json::Value v;
			if (count_job->reply.status == 200 && vfnet::json::parse(count_job->reply.body, v)) guest_watchers = int(std::clamp(v["watchers"].n, 0.0, 1e7));
			count_job.reset();
		}
		if (!count_job && now >= count_at && now - cells_at < 2000) {
			count_at = now + 60000;
			count_job = fetch("GET", lobby_url("/v1/watch/count?w=" + wid), "", 5000, 4096);
		}
	}
}

void Online::feed_last()
{
	for (const FeedSlot &f : feed_slots) if (f.job) return;   // (one on its way: the lobby takes the match for over once it's quiet)
	if (!feed.on()) return;
	feed.over();
	feed.interval_ms = 0;
	vfnet::watch::Feed::Upload u;
	if (feed.next(now, u)) farewell(lobby_url("/v1/watch/push?w=" + wid + "&k=" + wkey), std::string(u.body.begin(), u.body.end()));
}

bool Online::watch(const OnlineSettings &s, const std::string &c, std::string &error)
{
	const std::string w = online_menu::clean_code(c);
	if (w.size() != 6) { error = tr::WATCH_CODE_BAD; return false; }
	if (identity.game.empty()) { error = "Online play isn't ready yet."; return false; }
	stop();
	last = {};
	failure.clear();
	fail_step = Step::none;
	note.clear();
	random = false;
	tried = Try::watch;
	watch_tried = w;
	started_with = s;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = "Online play isn't set up in this build (no lobby)."; return false; }
	if (lobby.rfind("https://", 0) != 0 && !lobby_on_this_machine(lobby)) { error = "The lobby's address (settings.ini's online_lobby) must start with https://."; return false; }
	wid = w;
	watch_sid = hex64(vfnet::watch::new_match_id());
	watch_match = watch_next = 0;
	seen_at = -1e18;
	watch_since = watch_heard = now;
	watch_live_ok = true;
	over_at = 0;
	watch_shown = watch_quick = false;
	watch_watchers = watch_restarts = 0;
	watch_meta = {};
	score = {};
	is_host = false;
	rooms = false;
	ph = Phase::watching;
	log_line("watching " + watch_code());
	return true;
}

void Online::watch_failed(const std::string &why)
{
	log_line("watching " + watch_code() + ": " + why);
	const std::string tried_code = wid;
	stop();
	failure = why;
	fail_step = Step::none;
	tried = Try::watch;
	watch_tried = tried_code;
	notices.push_back({ Notice::refused, why });
}

void Online::watch_over(const std::string &why)
{
	LastMatch m;
	m.any = m.watched = true;
	m.why = why;
	for (int p = 0; p < 2; p++) {
		m.name[p] = player_name(p);
		m.country[p] = player_country(p);
		m.wins[p] = spec ? spec->info().wins[p] : 0;
	}
	m.seconds = int((now - watch_since) / 1000);
	m.watch = wid;
	log_line("watching " + watch_code() + ": " + why);
	stop();
	last = m;
	notices.push_back({ Notice::ended, why });
}

// The match's start, if this game can run it as the players do: the same game, ROM and terms (as a guest's)
bool Online::watch_start(const vfnet::watch::Start &st, std::string &why)
{
	if (st.meta.game != identity.game) { why = st.meta.game == "vf3tb" ? tr::WATCH_IS_TEAM_BATTLE : tr::WATCH_IS_REVISION_D; return false; }
	if (st.meta.content != hex64(identity.content)) { why = tr::WATCH_OTHER_ROM; return false; }
	if (!use_terms(st.meta.terms)) { why = tr::fill(tr::WATCH_OTHER_VERSION, { { "version", online_menu::clean_name(st.meta.build) } }); return false; }
	watch_meta = st.meta;
	score = {};
	adapter.mute = true;   // (catching up unseen: its sound dropped)
	if (adapter.settle) adapter.settle();
	if (!spec->start(st)) { why = tr::WATCH_START_BAD; return false; }
	log_line("watching " + online_menu::clean_name(st.meta.name[0]) + " vs " + online_menu::clean_name(st.meta.name[1]) + " (" + st.meta.terms + ")");
	return true;
}

std::string Online::watch_status() const
{
	if (ph != Phase::watching) return "";
	if (!spec) return watch_match ? tr::WATCH_GETTING : tr::fill(tr::WATCH_LOOKING, { { "code", watch_code() } });
	switch (spec->state()) {
	case vfnet::watch::Spectator::State::waiting:
	case vfnet::watch::Spectator::State::starting: return tr::WATCH_STARTING;
	case vfnet::watch::Spectator::State::catching_up: return tr::fill(tr::WATCH_CATCHING_UP, { { "pct", std::to_string(watch_progress()) } });
	case vfnet::watch::Spectator::State::buffering: return tr::WATCH_BUFFERING;
	case vfnet::watch::Spectator::State::over: return tr::WATCH_BETWEEN;
	default: return tr::WATCH_ON;
	}
}

int Online::watch_step() const
{
	if (!spec) return 0;
	switch (spec->state()) {
	case vfnet::watch::Spectator::State::waiting:
	case vfnet::watch::Spectator::State::starting: return 1;
	case vfnet::watch::Spectator::State::catching_up: return watch_shown ? 3 : 2;
	default: return watch_shown ? 3 : 2;
	}
}

int Online::watch_progress() const
{
	if (!spec || !spec->total()) return 0;
	return int(std::clamp(100.0 * double(spec->frame()) / double(spec->total()), 0.0, 100.0));
}

void Online::watch_poll()
{
	using vfnet::watch::Spectator;
	// (here every 15 s, every 2 till a match is on: the lobby counts the watchers, says which match is on and
	// whether it's a Quick match's)
	if (seen_job && seen_job->done) {
		vfnet::json::Value v;
		const vfnet::http::Reply r = seen_job->reply;
		seen_job.reset();
		const bool said = r.status >= 400 && vfnet::json::parse(r.body, v);
		// (watching switched off at the lobby, or an older lobby without it: said plainly)
		if (said && ((r.status == 503 && v["error"].str() == "off") || (r.status == 404 && v["error"].str() == "bad"))) { watch_failed(tr::WATCH_OFF); return; }
		if (r.status == 404 && !watch_match) { watch_failed(tr::fill(tr::WATCH_NOTHING, { { "code", watch_code() } })); return; }
		if (said && r.status == 429 && v["error"].str() == "full") { watch_failed(tr::WATCH_FULL); return; }   // (400 watching already)
		if (said && r.status == 429 && v["error"].str() == "busy") { watch_failed(tr::WATCH_BUSY); return; }   // (too many watching, all matches together)
		if (r.status != 0) watch_heard = now;
		else if (now - watch_heard > 20000) { watch_failed(tr::WATCH_LOBBY_GONE); return; }   // (the lobby gone: said once, plainly; the players go on)
		if (r.status == 200 && vfnet::json::parse(r.body, v)) {
			// (the lobby busy: no stream for this one, the frames asked for less often)
			watch_live_ok = v["live"].kind != vfnet::json::Value::boolean || v["live"].b;
			if (v["poll_ms"].kind == vfnet::json::Value::number && spec) spec->ask_every(std::clamp(v["poll_ms"].n, 500.0, 10000.0));
			if (!watch_live_ok) stream_stop();
			watch_watchers = int(std::clamp(v["watchers"].n, 0.0, 1e7));
			watch_quick = v["random"].kind == vfnet::json::Value::boolean && v["random"].b;
			const std::string m = v["match"].str();
			if (m.size() == 16) watch_next = strtoull(m.c_str(), nullptr, 16);
		}
	}
	if (!seen_job && now >= seen_at) {
		seen_at = now + (spec && spec->state() != Spectator::State::over ? 15000 : 2000);
		seen_job = fetch("POST", lobby_url("/v1/watch/seen?w=" + wid + "&s=" + watch_sid), "", 5000, 4096);
	}
	// a match on: the first, or the next once one's over (a Play again: the same watch code)
	if (watch_next && watch_next != watch_match && (!spec || spec->state() == Spectator::State::over)) {
		if (spec) log_line("watching " + watch_code() + ": their next match");
		watch_match = watch_next;
		Spectator::Config c;
		stream_stop();
		spec = std::make_unique<Spectator>(adapter, c);
		spec->on_catch_up = [this](bool on) { adapter.mute = on || !watch_shown; };   // (catching up, unseen: no sound, then none late)
		watch_job.reset();
		watch_shown = false;
		over_at = 0;
	}
	if (!spec) {
		if (now - watch_since > 120000) watch_failed(tr::fill(tr::WATCH_NONE_STARTED, { { "code", watch_code() } }));
		return;
	}
	switch (spec->state()) {
	case Spectator::State::out_of_step:   // (once: from the match's start again, quietly; a second time, said)
		if (watch_restarts++ == 0) { log_line("watching: " + spec->why + " From the start again"); stream_stop(); spec.reset(); watch_match = 0; return; }
		watch_failed(tr::WATCH_OUT_OF_STEP); return;
	case Spectator::State::failed: watch_failed(spec->why); return;
	case Spectator::State::over:
		if (!over_at) over_at = now;
		if (now - over_at > 45000) { watch_over(tr::WATCH_MATCH_OVER); return; }   // (no next one: back to the Online screen)
		break;
	default: break;
	}
	stream_poll();   // (the frames as they come, once its start's in)
	if (ph != Phase::watching) return;   // (told watching's off on its socket: stopped)
	// the lobby asked for what's next: the start, the frames from where this one is
	if (watch_job && watch_job->done) {
		const std::shared_ptr<Fetch> j = watch_job;
		watch_job.reset();
		const bool ok = j->reply.status == 200;
		if (j->reply.status != 0) watch_heard = now;
		const std::vector<uint8_t> b(j->reply.body.begin(), j->reply.body.end());
		if (watch_want == "start") {
			vfnet::watch::Start st;
			if (ok && vfnet::watch::decode(b, st)) {
				std::string why;
				if (!watch_start(st, why)) { watch_failed(why); return; }
			}
		} else if (watch_want == "frames") {
			vfnet::watch::Reply r;
			if (ok && vfnet::watch::decode_reply(b, r)) {
				spec->reply(r, now);
				watch_watchers = int(r.watchers);
			}
		}
		spec->answered(now);
	}
	if (!watch_job) {
		const std::string w = spec->want(now);
		if (!w.empty()) {
			spec->asked(now);
			watch_want = w;
			const std::string q = "?w=" + wid + "&m=" + hex64(watch_match);
			if (w == "start") watch_job = fetch("GET", lobby_url("/v1/watch/start" + q), "", 10000, vfnet::watch::MAX_START + 8192);
			else if (w == "frames") watch_job = fetch("GET", lobby_url("/v1/watch/frames" + q + "&from=" + std::to_string(spec->ask_from())), "", 3000, 64 << 10);
		}
	}
	// (the log every 10 s: how it's going, what a report needs)
	if (watch_shown && now - logged_at >= 10000) {
		logged_at = now;
		char b[200];
		snprintf(b, sizeof b, "watching: frame %u of the lobby's %u, %.1f s behind, %u of the players' hashes matched, %d watching, %llu frames run unseen",
		         spec->frame(), spec->total(), spec->behind_s(), spec->hashes_checked, watch_watchers, (unsigned long long)spec->frames_unseen);
		log_line(b);
	}
}

// The frames as they come (a watcher's): one response from the lobby that goes on as long as the match, read on a
// thread of its own, each message given to the watcher here as it's taken (its time the buffer's measure)
struct Online::Stream
{
	std::mutex lock;
	std::deque<std::vector<uint8_t>> messages;
	std::atomic<bool> stop{ false }, done{ false };
	int status = 0;
};

void Online::stream_stop()
{
	if (watch_ws) { std::thread([w = std::move(watch_ws)] {}).detach(); if (spec) spec->live = false; }
	if (!stream) return;
	stream->stop = true;   // (its thread lets go at its next piece, or the lobby's next "still here")
	stream.reset();
	if (spec) spec->live = false;
}

void Online::stream_poll()
{
	using vfnet::http::Socket;
	if (watch_ws) {
		Socket::Message msg;
		bool any = false;
		while (watch_ws->take(msg)) {
			any = true;
			if (msg.binary) {
				vfnet::watch::Reply r;
				if (spec && vfnet::watch::decode_reply(std::vector<uint8_t>(msg.data.begin(), msg.data.end()), r)) { spec->reply(r, now); watch_watchers = int(r.watchers); watch_ws_frames_at = now; }
			} else {   // (the lobby's word, as /seen's answer: how many watch, the match on, whether it's a Quick match's)
				vfnet::json::Value v;
				if (!vfnet::json::parse(msg.data, v)) continue;
				if (v["error"].str() == "off") { watch_failed(tr::WATCH_OFF); return; }   // (watching switched off at the lobby)
				watch_watchers = int(std::clamp(v["watchers"].n, 0.0, 1e7));
				watch_quick = v["random"].kind == vfnet::json::Value::boolean && v["random"].b;
				const std::string mm = v["match"].str();
				if (mm.size() == 16) watch_next = strtoull(mm.c_str(), nullptr, 16);
			}
		}
		if (any) watch_heard = now;
		const Socket::State ws = watch_ws->state();
		if (ws == Socket::State::closed) {   // (refused: the lobby busy, the edge's pieces asked; gone: again in a moment)
			const int st = watch_ws->status();
			log_line("watching: the WebSocket " + std::string(st == 429 ? "wasn't taken (busy)" : "ended (" + watch_ws->error() + ")") + ": asking over HTTPS meanwhile");
			std::thread([w = std::move(watch_ws)] {}).detach();
			if (st != 101) watch_ws_failed = true;   // (never taken: refused, or no WebSocket through this network: the HTTPS stream)
			else stream_retry = now + 3000;          // (it was open and ended: a new one in a moment)
			if (spec) spec->live = false;
			return;
		}
		if (ws == Socket::State::open) {
			// (no frames on it for 45 s, longer than the host's slowest pace: asked for over HTTPS meanwhile, once a
			// second, whose answer says if the match is over; the lobby tells a socket only of new frames, so a match
			// that ended with none, or whose host's game closed, would otherwise hold this watcher on its last picture)
			if (spec) spec->live = now - watch_ws_frames_at < 45000;
			seen_at = std::max(seen_at, now + 15000);   // (counted by its socket: no /seen while it's open)
		}
		return;
	}
	if (stream) {
		std::deque<std::vector<uint8_t>> got;
		{
			std::lock_guard<std::mutex> l(stream->lock);
			got.swap(stream->messages);
		}
		for (const std::vector<uint8_t> &m : got) {
			vfnet::watch::Reply r;
			if (!m.empty() && vfnet::watch::decode_reply(m, r) && spec) { spec->reply(r, now); watch_watchers = int(r.watchers); }
		}
		if (!got.empty()) watch_heard = now;
		if (stream->done) {   // (ended: the lobby's too busy, or the connection went: asked for by the frames meanwhile, a stream again soon)
			stream_retry = now + (stream->status == 429 ? 60000 : 3000);
			log_line(std::string("watching: the stream ended (") + std::to_string(stream->status) + "): asking for the frames meanwhile");
			stream_stop();
		}
		return;
	}
	if (!spec || !watch_live_ok || now < stream_retry || spec->state() == vfnet::watch::Spectator::State::waiting || spec->state() == vfnet::watch::Spectator::State::starting ||
	    spec->state() == vfnet::watch::Spectator::State::over)
		return;
	if (!watch_ws_failed && watch_live_ok) {   // (a WebSocket first: hibernating at the lobby, the cheapest way; the HTTPS stream if it's refused)
		watch_ws = std::make_shared<Socket>(lobby_url("/v1/watch/ws?w=" + wid + "&m=" + hex64(watch_match) + "&from=" + std::to_string(spec->ask_from()) + "&s=" + watch_sid));
		watch_ws_frames_at = now;
		return;
	}
	auto box = std::make_shared<Stream>();
	const std::string url = lobby_url("/v1/watch/live?w=" + wid + "&m=" + hex64(watch_match) + "&from=" + std::to_string(spec->ask_from()) + "&s=" + watch_sid);
	std::thread([box, url] {
		std::vector<uint8_t> buf;
		const vfnet::http::Reply r = vfnet::http::stream(url, [box, &buf](const char *p, size_t n) {
			if (box->stop) return false;
			buf.insert(buf.end(), p, p + n);
			size_t at = 0;   // (each message: its length, then it; a length of 0, the lobby still there)
			while (buf.size() - at >= 4) {
				uint32_t len = 0;
				memcpy(&len, buf.data() + at, 4);
				if (len > (8u << 20)) return false;
				if (buf.size() - at - 4 < len) break;
				if (len) {
					std::lock_guard<std::mutex> l(box->lock);
					if (box->messages.size() > 4096) return false;   // (not being taken: let go)
					box->messages.emplace_back(buf.begin() + ptrdiff_t(at) + 4, buf.begin() + ptrdiff_t(at + 4 + len));
				}
				at += 4 + len;
			}
			buf.erase(buf.begin(), buf.begin() + ptrdiff_t(at));
			return true;
		}, 8000, 15000);
		box->status = r.status;
		box->done = true;
	}).detach();
	stream = box;
	spec->live = true;
}

// Enter code: what it is first (the lobby's peek), then a room open joined, a match on watched
bool Online::enter_code(const OnlineSettings &s, const std::string &c, std::string &error)
{
	const std::string want = online_menu::clean_code(c);
	if (want.size() != 6) { error = "A code is 6 letters and numbers (like KXQ-4M7)."; return false; }
	stop();
	last = {};
	failure.clear();
	fail_step = Step::none;
	started_with = s;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = "Online play isn't set up in this build (no lobby)."; return false; }
	if (lobby.rfind("https://", 0) != 0 && !lobby_on_this_machine(lobby)) { error = "The lobby's address (settings.ini's online_lobby) must start with https://."; return false; }
	peek_code = want;
	code = want;
	tried = Try::join_room;
	rooms = true;   // (the Online screen's: joining a code, its room being found)
	ph = Phase::joining;
	st = Step::finding;
	peek_job = fetch("POST", lobby_url("/v1/peek"), "{\"code\":" + vfnet::json::quote(want) + "}", 8000, 4096);
	log_line("the code " + room_code() + ": asking the lobby what it is");
	return true;
}

void Online::peek_poll()
{
	if (!peek_job || !peek_job->done) return;
	const vfnet::http::Reply r = peek_job->reply;
	peek_job.reset();
	vfnet::json::Value v;
	const std::string c = peek_code;
	std::string err;
	ph = Phase::off;
	st = Step::none;
	rooms = false;
	if (r.status == 200 && vfnet::json::parse(r.body, v) && v["state"].str() == "playing" && v["watch"].kind == vfnet::json::Value::boolean && v["watch"].b) {
		if (!watch(started_with, c, err)) { failure = err; notices.push_back({ Notice::refused, err }); }
		return;
	}
	if (r.status == 200 || r.status == 404) {   // (open, or not known to the lobby: joined, as the code's always been)
		if (!join_room(started_with, c, err)) { failure = err; notices.push_back({ Notice::refused, err }); }
		return;
	}
	failure = lobby_says(r, room_code(), identity.build);
	fail_step = Step::finding;
	tried = Try::join_room;
	notices.push_back({ Notice::refused, failure });
}

// ---- the rooms list ----

const std::vector<Online::RoomRow> &Online::rooms_list(const OnlineSettings &s, bool again)
{
	using namespace std::chrono;
	const double t = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
	if (now == 0) now = t;
	if (rooms_job && rooms_job->done) {
		vfnet::json::Value v;
		if (rooms_job->reply.status == 200 && vfnet::json::parse(rooms_job->reply.body, v)) {
			rooms_rows.clear();
			// (what the lobby has on: an older one says nothing, and has neither rooms nor watching)
			lobby_watch_on = v["features"]["watch"].kind == vfnet::json::Value::boolean && v["features"]["watch"].b;
			for (const vfnet::json::Value &r : v["rooms"].items) {
				if (r["game"].str() != identity.game) continue;   // (this game's: Revision D's or Team Battle's)
				RoomRow row;
				row.playing = r["state"].str() == "playing";
				row.code = online_menu::clean_code(r["code"].str());
				row.watch = online_menu::clean_code(r["watch"].str());
				row.locked = r["locked"].kind == vfnet::json::Value::boolean && r["locked"].b;
				row.quick = r["random"].kind == vfnet::json::Value::boolean && r["random"].b;
				if (row.code.size() != 6 || row.quick) row.code.clear();
				if (row.watch.size() != 6) row.watch.clear();
				if (row.quick && (!row.playing || row.watch.empty())) continue;
				if (row.code.empty() && row.watch.empty() && !row.locked) continue;
				if (!row.quick) row.room = online_menu::clean_room(r["name"].str());   // (the room's name: a room name's letters, 16 at most)
				row.queue = int(std::clamp(r["queue"].n, 0.0, 99.0));
				for (int p = 0; p < 2; p++) {
					const vfnet::json::Value &who = r[p ? "p2" : "p1"];
					row.name[p] = online_menu::clean_name(who["name"].str());
					row.country[p] = country(who["country"].str());
				}
				if (row.name[0].empty() || (row.playing && row.name[1].empty())) continue;
				for (int p = 0; p < 2 && r["score"].items.size() == 2; p++) row.wins[p] = int(std::clamp(r["score"].items[size_t(p)].n, 0.0, 99.0));
				row.round = int(std::clamp(r["round"].n, 0.0, 99.0));
				row.watchers = int(std::clamp(r["watchers"].n, 0.0, 1e7));
				row.since = int(std::clamp(r["since"].n, 0.0, 1e7));
				const std::string reg = r["region"].str();
				row.region = reg.size() == 2 ? reg : "";
				row.older = r["older"].kind == vfnet::json::Value::boolean && r["older"].b;
				if (rooms_rows.size() < 100) rooms_rows.push_back(row);
			}
			rooms_said = v["rooms"].kind == vfnet::json::Value::list ? 1 : 3;   // (no list: rooms off, or an older lobby)
			rooms_region = v["you"].str().size() == 2 ? v["you"].str() : rooms_region;
			rooms_seen++;
		} else rooms_said = 2;
		rooms_job.reset();
	}
	const std::string where = s.lobby.empty() ? lobby_default() : s.lobby;
	if (!rooms_job && (again || t - rooms_at > 15000) && !where.empty() && where != "none" && !identity.game.empty()) {
		rooms_at = t;
		if (rooms_said < 0) rooms_said = 0;
		rooms_job = fetch("GET", where + "/v1/online", "", 8000, 1 << 20);
	}
	return rooms_rows;
}

namespace {

// The lobby's no, in words: what happened and what to do
// Two versions apart only by what's added on the online version: Dural on the select (Cheats: "+dural"), Team
// Battle 1 vs 1 (Game > Team Battle mode: "+1v1") or the stage select (Online > Settings: "+stage"): said so
std::string options_apart(const std::string &theirs, const std::string &mine)
{
	const auto plain = [](std::string v) { const size_t at = v.find('+'); return at == std::string::npos ? v : v.substr(0, at); };
	const auto has = [](const std::string &v, const char *what) { return v.find(what) != std::string::npos; };
	if (plain(theirs) != plain(mine)) return "";
	if (has(theirs, "+dural") != has(mine, "+dural"))
		return has(theirs, "+dural") ? "Your opponent has Dural on the select (Cheats): turn it on too to play them, or ask them to turn it off."
		                             : "Your opponent has Dural on the select off, and you have it on (Cheats): turn it off to play them.";
	if (has(theirs, "+dural") && has(mine, "+dural") && has(theirs, vf3net::DURAL_FLAG) != has(mine, vf3net::DURAL_FLAG))
		return has(theirs, vf3net::DURAL_FLAG) ? "Your opponent's Dural on the select is newer than yours (her patches changed after v0.1.16): update to play them with it on, or both turn it off."
		                                       : "Your opponent's Dural on the select is older than yours (her patches changed after v0.1.16): they need to update to play you with it on, or both turn it off.";
	if (has(theirs, "+1v1") != has(mine, "+1v1"))
		return has(theirs, "+1v1") ? "Your opponent plays Team Battle 1 vs 1: switch to it in Game > Team Battle mode to play them."
		                           : "Your opponent plays Team Battle 3 vs 3: switch to it in Game > Team Battle mode to play them.";
	if (has(theirs, "+stage") != has(mine, "+stage"))
		return has(theirs, "+stage") ? "Your opponent has stage select on (Online > Settings): turn it on too to play them, or ask them to turn it off."
		                             : "Your opponent has stage select off, and you have it on (Online > Settings): turn it off to play them.";
	return "";
}

// A refusal's "Your opponent has X, you have Y" or "The host has X, you have Y": the options' one if that's all it is
std::string options_said(const std::string &text, const std::string &mine)
{
	for (const char *lead : { "Your opponent has ", "The host has " }) {
		const size_t at = text.find(lead), comma = text.find(", you have ");
		if (at == std::string::npos || comma == std::string::npos || comma < at) continue;
		const size_t from = at + strlen(lead);
		if (const std::string d = options_apart(text.substr(from, comma - from), mine); !d.empty()) return d;
	}
	return text;
}

std::string lobby_says(const vfnet::http::Reply &r, const std::string &code, const std::string &mine)
{
	if (r.status == 0) return tr::ONLINE_DOWN;   // (what went wrong, in the log: this, plainly)
	vfnet::json::Value v;
	vfnet::json::parse(r.body, v);
	const std::string e = v["error"].str();
	if (e == "no_room") return "There's no room " + code + ". Check the code, and that the host is still on their Host page.";
	if (e == "full") return "Someone's already joining room " + code + ": ask the host for a new code.";
	if (e == "version") {
		std::string theirs;
		for (char ch : v["host_version"].str())
			if (isalnum(uint8_t(ch)) || ch == '.' || ch == '-' || ch == '+' || ch == '_') theirs += ch;
		if (const std::string d = options_apart(v["host_version"].str(), mine); !d.empty()) return d;
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

// ---- a room code's host's WebSocket to its room (the lobby's, hibernating): its player told at once, the room held
// while it's open; refused, or no WebSocket through this network: the HTTPS wait every 20 s, the hold every 15 s ----

bool Online::room_ws_open() const
{
	return room_ws && room_ws->state() == vfnet::http::Socket::State::open;
}

void Online::room_ws_drop()
{
	if (room_ws) std::thread([w = std::move(room_ws)] {}).detach();   // (let go on a thread: never a wait here)
	room_ws.reset();
	room_ws_for.clear();
}

bool Online::room_ws_poll(vfnet::http::Reply &r)
{
	using vfnet::http::Socket;
	const std::string want = code + "|" + key;
	if (room_ws && room_ws_for != want) room_ws_drop();   // (another room, or a new key: a Play again's)
	if (!room_ws && now >= room_ws_at && !key.empty()) {
		room_ws = std::make_shared<Socket>(lobby_url("/v1/room/ws?code=" + code + "&key=" + key));
		room_ws_for = want;
		room_ws_made = now;
	}
	if (!room_ws) return false;
	Socket::Message msg;
	while (room_ws->take(msg)) {   // (its player, as a wait's answer; or the room gone)
		vfnet::json::Value v;
		if (msg.binary || !vfnet::json::parse(msg.data, v)) continue;
		r = {};
		r.status = v["error"].str() == "no_room" ? 404 : 200;
		r.body = msg.data;
		return true;
	}
	if (room_ws->state() != Socket::State::closed) return false;
	const int status = room_ws->status();
	const std::string why = room_ws->error();
	room_ws_drop();
	room_ws_at = now + 2000;
	// (refused: the room gone, or a lobby without this, either way the HTTPS wait says which; only its own message on
	// the socket, {error: no_room}, is taken for the room gone)
	if (status == 101 && now - room_ws_made > 60000) room_ws_tries = 0;   // (open a good while, then lost: as new)
	if (status == 101 && ++room_ws_tries <= 5) {
		log_line("the room's WebSocket closed (" + why + "): again in a moment");
		return false;
	}
	room_ws_failed = true;
	log_line("no WebSocket to the room (" + (status ? "the lobby said " + std::to_string(status) : why) + "): waiting over HTTPS");
	return false;
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
			relay_server = job->turn; relay_user = job->user; relay_pass = job->password; relay_stun = job->stun; relay_asks = 0;
			if (!job->turn.empty()) room->use_turn(job->turn, job->user, job->password);
			else log_line(job->reply.status == 0 ? "no relay: the online service wasn't reached (" + job->reply.error + ")"
			                                    : "no relay: the online service gave none (" + std::to_string(job->reply.status) + ")");
			if (job->reply.status == 0) { room_failed(random ? std::string(tr::ONLINE_UNAVAILABLE) : lobby_says(job->reply, room_code(), identity.build)); return; }
			country_me = country(job->country);
			job.reset();
		}
		if (!job && room->gathered() && started_with.hide_ip && room->relay_address().empty()) {
			// (the relay asked again, twice, before Hide my IP gives up; reno's, 25 Oct: it didn't answer, and the next
			// try went through. Each time from a new port: a router's or firewall's state for the old one may be what
			// lost the answers, and a relay that made the first allocation but whose answer was lost refuses a second
			// from the same port (437). The last time on the relay's other port, 53: some networks drop UDP to 3478.)
			if (!relay_server.empty() && relay_asks < 2) {
				relay_asks++;
				log_line("the relay didn't answer (" + room->report(mask) + "): asking it again from a new port" + (relay_asks == 2 ? ", on its port 53" : "") +
				         " (" + std::to_string(relay_asks) + " of 2)");
				auto fresh = std::make_unique<vfnet::RoomLink>();
				fresh->allow_loopback(lobby_on_this_machine(lobby));
				std::string err;
				if (fresh->open(0, err)) {
					fresh->set_relay_only(true);
					if (!relay_stun.empty()) fresh->use_stun(relay_stun);
					room = std::move(fresh);
				}
				std::string server = relay_server;
				if (relay_asks == 2) server = server.substr(0, server.rfind(':') + 1) + "53";
				room->use_turn(server, relay_user, relay_pass);
				return;
			}
			log_line("the relay didn't answer (" + room->report(mask) + ")");
			room_failed("Hide my IP address is on, but the relay couldn't be reached. A firewall may be blocking the game (allow it), or try again, or turn Hide my IP address off (Online > Settings).");
			return;
		}
		if (!job && room->gathered()) {
			log_line("this side: on its network and public " + masked(room->candidates()) + ", relay " + mask(room->relay_address()));
			const std::string base = lobby;
			if (random) {
				search();
			} else if (is_host) {
				st = Step::opening;
				// (a public room, or one whose matches may be watched: said to the lobby; a private room, as ever, nothing)
				// (every match may be watched, by its code; a room stays for the next in line; listed, in Rooms, if the host chose it)
				const OnlineSettings &w = started_with;
				const std::string watching = std::string(w.room_who >= 1 ? ",\"listed\":true" : "") + ",\"persist\":true" +
				                             (w.room_name.empty() ? "" : ",\"room\":" + vfnet::json::quote(w.room_name));
				const std::string body = me_json((want_code.empty() ? "" : ",\"want\":" + vfnet::json::quote(want_code) +
				                                 (want_code == kept_code ? ",\"key\":" + vfnet::json::quote(kept_key) : std::string())) + watching);
				start_job([base, body](Job &j) { j.reply = vfnet::http::request("POST", base + "/v1/host", body, 10000); });
			} else {
				st = Step::finding;
				const std::string body = me_json(",\"code\":" + vfnet::json::quote(code) + ",\"queue\":true");   // (a room whose match is on: in its line)
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
			// (a room stays, kept through its match and open again after it for the next in line, only when the lobby says
			// so: an older lobby, or one with rooms switched off, makes a code-only room, as ever)
			persist = !random && v["persist"].kind == vfnet::json::Value::boolean && v["persist"].b;
			listed_as.clear();   // (how the lobby lists it: its name, or its host's if the lobby wouldn't show the one typed)
			if (v["listed_as"].kind == vfnet::json::Value::object) {
				const std::string rn = online_menu::clean_room(v["listed_as"]["room"].str()), hn = online_menu::clean_name(v["listed_as"]["name"].str());
				listed_as = !rn.empty() ? rn : !hn.empty() ? online_menu::clean_room(hn + "'S ROOM") : "";
				if (!listed_as.empty()) log_line("listed in Rooms as " + listed_as + (rn.empty() && !started_with.room_name.empty() ? " (the name typed isn't shown: the lobby's word filter)" : ""));
			}
			if (v["wid"].str().size() == 6 && v["wkey"].str().size() == 32) {   // (its matches may be watched: the lobby's watch code, the key to send them with)
				wid = online_menu::clean_code(v["wid"].str());
				wkey = v["wkey"].str();
				log_line(std::string(started_with.room_who >= 1 ? "a listed room" : "a room") + ": its matches may be watched with its code");
			}
			job.reset();
			use_terms("");   // (its own till a player comes: then the lobby's terms for the two)
			room_session(true);
			st = Step::waiting;
			wait_errors = 0;
			retry_at = 0;
			log_line("room " + room_code() + " open: waiting for a player");
		}
		break;
	case Step::waiting: {
		vfnet::http::Reply r;
		bool got = false;
		if (!random && !room_ws_failed) {   // (a room code's host: told on the room's WebSocket, not by a wait every 20 s)
			got = room_ws_poll(r);
			if (!got && !room_ws_failed) break;
			if (!got) retry_at = now;   // (no WebSocket: the HTTPS wait from now)
		}
		if (!got && !job && now >= retry_at) {
			const std::string url = lobby_url("/v1/wait"), body = "{\"code\":" + vfnet::json::quote(code) + ",\"key\":" + vfnet::json::quote(key) + "}";
			start_job([url, body](Job &j) { j.reply = vfnet::http::request("POST", url, body, 30000); });
		}
		if (!got && job && job->done) {
			r = job->reply;
			job.reset();
			got = true;
		}
		if (got) {
			vfnet::json::Value v;
			if (r.status == 200 && vfnet::json::parse(r.body, v)) {
				if (wait_errors >= 3) note.clear();   // (reached again)
				wait_errors = 0;
				const vfnet::json::Value &g = v["guest"];
				if (g.kind == vfnet::json::Value::object) {
					peer_name = g["name"].str();
					country_peer = country(g["country"].str());   // (the guest's flag: the lobby sends it, as it does the host's to the guest)
					const std::string was = terms;
					if (!use_terms(g["as"].str(), g["them"].str())) { host_again("their version's settings can't be played here."); return; }
					if (terms != was) room_session(true);   // (the session for the two's terms: the pool's, or the player's version)
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
	}
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
			std::string extra = ticket.empty() ? "" : ",\"ticket\":" + vfnet::json::quote(ticket);
			if (ticket.empty()) {   // (the rooms of those too far lately, for the lobby to keep apart: ten minutes)
				avoid.erase(std::remove_if(avoid.begin(), avoid.end(), [this](const auto &a) { return now - a.second > 600000; }), avoid.end());
				if (avoid.size() > 8) avoid.erase(avoid.begin(), avoid.end() - 8);
				std::vector<std::string> codes;
				for (const auto &a : avoid) codes.push_back(a.first);
				if (!codes.empty()) extra += ",\"avoid\":" + vfnet::json::list(codes);
			}
			const std::string body = me_json(extra);
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
				if (never || (r.status == 0 && ++wait_errors >= 3)) { room_failed(r.status == 0 ? std::string(tr::ONLINE_UNAVAILABLE) : lobby_says(r, "", identity.build)); return; }
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
				if (v["wid"].str().size() == 6 && v["wkey"].str().size() == 32) { wid = online_menu::clean_code(v["wid"].str()); wkey = v["wkey"].str(); }   // (watchable by its code)
				ticket.clear();
				if (!use_terms(v["as"].str(), v["them"].str())) { room_failed("The online service matched you with a version this one can't play: update the game."); return; }
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
			const std::string base = lobby, body = me_json(",\"code\":" + vfnet::json::quote(code) + ",\"queue\":true");
			start_job([base, body](Job &j) { j.reply = vfnet::http::request("POST", base + "/v1/join", body, 10000); });
		}
		if (job && job->done) {
			vfnet::json::Value v;
			const vfnet::http::Reply r = job->reply;
			job.reset();
			// (Play again: the host's room not open again yet. Every 4 s: the lobby's limit is 20 joins a minute)
			if ((r.status == 404 || r.status == 429) && patient()) { retry_at = now + 4000; break; }
			// (a room whose match is on, that keeps a line: this player's place in it, asked again every 4 s till it's theirs)
			if (r.status == 409 && vfnet::json::parse(r.body, v) && v["queued"].kind == vfnet::json::Value::number) {
				const int was = queue_at;
				queue_at = int(std::clamp(v["queued"].n, 1.0, 99.0));
				queue_len = int(std::clamp(v["of"].n, double(queue_at), 99.0));
				if (queue_at != was) log_line("in line for room " + room_code() + ": " + std::to_string(queue_at) + " of " + std::to_string(queue_len));
				patient_until = now + 20000;
				retry_at = now + 4000;
				break;
			}
			if (r.status != 200 || !vfnet::json::parse(r.body, v) || v["token"].str().empty()) { room_failed(lobby_says(r, room_code(), identity.build)); return; }
			patient_until = 0;
			queue_at = queue_len = 0;
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
	if (v["wid"].str().size() == 6) wid = online_menu::clean_code(v["wid"].str());   // (the host's room may be watched: the count's asked by it)
	if (!use_terms(v["as"].str(), v["them"].str())) { room_failed("The online service matched you with a version this one can't play: update the game."); return; }
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

// A random opponent connected but too far (the ping over one side's limit): the room let go, into the queue again
// (the lobby told to keep these two apart a while: they'd only be matched again)
void Online::search_again(const std::string &why)
{
	close_room();
	session.reset();
	room->clear_peer();
	checking = false;
	is_host = false;
	ph = Phase::joining;
	peer_name.clear();
	country_peer.clear();
	code.clear();
	note = why;
	search();
}

// Two letters, or nothing
std::string Online::country(const std::string &c)
{
	return c.size() == 2 && isupper(uint8_t(c[0])) && isupper(uint8_t(c[1])) ? c : "";
}

void Online::stop()
{
	listed_as.clear();
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
	if (ph == Phase::watching) log_line("stopped watching " + watch_code());
	spec.reset();
	seen_job.reset();
	watch_job.reset();
	if (stream) stream_stop();
	feed = {};
	for (FeedSlot &f : feed_slots) {   // (an upload still on its way keeps its connection on its thread: the next match a new one, never two threads on one)
		f.job.reset();
		if (f.client) std::thread([c = std::move(f.client)] {}).detach();
		f.client.reset();
	}
	if (feed_ws) std::thread([w = std::move(feed_ws)] {}).detach();
	if (watch_ws) std::thread([w = std::move(watch_ws)] {}).detach();
	feed_ws_failed = feed_ws_waiting = watch_ws_failed = false;
	room_ws_drop();
	room_ws_failed = false;
	room_ws_tries = 0;
	room_ws_at = 0;
	peek_job.reset();
	count_job.reset();
	wid.clear();
	wkey.clear();
	guest_watchers = -1;
	persist = false;
	queue_at = queue_len = 0;
	adapter.mute = false;
	session.reset();
	link.reset();
	room.reset();
	job.reset();
	ph = Phase::off;
	st = Step::none;
	rooms = false;
	checking = false;
}

// Connected (and the ping within the limit, if it's measured first): the match begins
void Online::begin_match(const std::string &who)
{
	ph = Phase::playing;
	began_at = now;
	// (the log's figures from this session's own: the last match's would wrap the first line's differences)
	logged_at = now;
	logged_rb = logged_resim = logged_stalls = logged_waits = logged_steps = 0;
	logged_save = logged_run = logged_resim_ms = logged_hash = 0;
	if (rooms) {
		log_line(std::string("connected ") + (room->via_relay() ? "through a relay" : "directly") + ", " + mask(room->way()) + ": " + room->report(mask));
		if (is_host && persist) hold_at = now;   // (a room that stays: held while its match is on, its line waiting)
		else close_room();   // (the code's done its job)
	}
	notices.push_back({ Notice::joined, "Playing " + who + ": press Start" });
	notices.push_back({ Notice::tip, "This match: " + match_said() });   // (both players: the settings it plays, the host's in a pool match)
	log_line("this match: " + match_said());
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
	out_from[0] = out_from[1] = SIZE_MAX;
	in_before = 0;
	select_off_from = UINT32_MAX;
	select_was = -1;
	start_scan[0] = start_scan[1] = 0;
	start_seen[0] = start_seen[1] = false;
	note.clear();
	ticket.clear();   // (out of the queue: the lobby's done with it)
	guest_watchers = -1;
	count_at = now + 3000;
	if (is_host && !wkey.empty()) feed_begin();   // (its matches may be watched: sent to the lobby as it's played)
}

void Online::poll(double t)
{
	now = t;
	if (ph == Phase::watching) { watch_poll(); return; }
	if (peek_job) { peek_poll(); return; }
	if (rooms) room_poll();
	if (!session) return;
	feed_poll();
	session->poll(t);
	vfnet::Session::Event e;
	while (session && session->next_event(e)) {
		switch (e.kind) {
		case vfnet::Session::Event::connected:
			// a random opponent with a limit to the ping (Online settings): measured first, the game held meanwhile
			// (the session's inputs go back and forth all the same, the ping from them), a second or so
			if (rooms && random && started_with.max_ping > 0) {
				checking = true;
				check_from = now;
				check_who = e.text;
				log_line(std::string("connected ") + (room->via_relay() ? "through a relay" : "directly") + ": the ping measured first (at most " +
				         std::to_string(started_with.max_ping) + " ms)");
				break;
			}
			begin_match(e.text);
			break;
		case vfnet::Session::Event::ended: {
			const auto why = session->end_reason();
			const bool was_playing = ph == Phase::playing;
			checking = false;
			// a random opponent who found this one too far for their limit (Online settings): someone else, at once
			if (rooms && random && room && why == vfnet::Session::End::opponent_quit && session->opponent_quit_reason() == vfnet::QUIT_TOO_FAR) {
				const std::string who = opponent();
				log_line(who + " left: the ping over their limit; searching again");
				if (!code.empty()) avoid.push_back({ code, now });
				search_again(who + " was too far away for their ping limit: looking for someone else...");
				return;
			}
			// a host goes on waiting after a player who couldn't join (another version, another ROM, gone quiet
			// before the game started); anything else ends it
			if (!was_playing && is_host && (why == vfnet::Session::End::rejected || why == vfnet::Session::End::timeout || why == vfnet::Session::End::opponent_quit)) {
				if (rooms) { host_again(e.text); return; }
				const std::string said = why == vfnet::Session::End::rejected ? options_said(e.text, identity.build) : e.text;
				notices.push_back({ Notice::refused, "A player couldn't join: " + said });
				note = "A player couldn't join: " + said;
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
				if (why == vfnet::Session::End::rejected) failure = options_said(failure, identity.build);
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
			// a room that stays: open again at once, the same code, for the next in its line (the host stays)
			const bool again_room = was_playing && rooms && is_host && persist && !random && !key.empty() && why != vfnet::Session::End::desync;
			const std::string again_code = code;
			if (again_room) { kept_code = code; kept_key = key; key.clear(); }
			close_room();
			session.reset();
			link.reset();
			room.reset();
			job.reset();
			ph = Phase::off;
			st = Step::none;
			rooms = false;
			if (again_room) {
				const LastMatch m = last;
				std::string err;
				log_line("the room stays: open again for the next in line");
				if (!host_room(started_with, err, again_code)) log_line("the room couldn't open again: " + err);
				last = m;   // (shown when the hosting's over)
			}
			return;
		}
		default:
			break;
		}
	}
	// a random match's ping, measured before it begins: 30 round trips (its average settled: a second or so once both
	// games are going, the other's first frames can take seconds), at most 10 s if it's not known by then (let
	// through: not held up for it)
	if (checking && session && session->state() == vfnet::Session::State::running) {
		const double ms = session->stats().rtt_ms;
		if (session->stats().rtt_samples >= 30 || now - check_from >= 10000) {
			checking = false;
			const std::string who = opponent();
			if (ms > started_with.max_ping) {
				const std::string n = std::to_string(int(ms + 0.5)), limit = std::to_string(started_with.max_ping);
				log_line("the ping to " + who + " " + n + " ms, over the " + limit + " ms allowed (Online settings): searching again");
				session->quit(vfnet::QUIT_TOO_FAR);
				if (!code.empty()) avoid.push_back({ code, now });
				search_again(who + "'s ping was " + n + " ms, over your limit (" + limit + " ms): looking for someone else...");
				return;
			}
			log_line("the ping to " + who + " " + std::to_string(int(ms + 0.5)) + " ms: within the " + std::to_string(started_with.max_ping) + " ms allowed");
			begin_match(check_who);
		}
	}
	// the connection in the log every 10 s of a match (what a report needs: the ping, the rollbacks, the waits)
	if (session && ph == Phase::playing && now - logged_at >= 10000) {
		const auto &stt = session->stats();
		if (logged_at > 0)
			fprintf(stderr, "online: frame %u, ping %.0f ms, delay %d, %llu rollbacks (%.1f frames on average, %d the most in the last second), "
			        "%llu stalls for the other side, %llu waits to keep in time, %.1f frames ahead%s%s\n", session->frame(), stt.rtt_ms, session->delay_now(),
			        (unsigned long long)(stt.rollbacks - logged_rb), stt.rollbacks > logged_rb ? double(stt.resim_frames - logged_resim) / double(stt.rollbacks - logged_rb) : 0.0,
			        stt.rollback_max, (unsigned long long)(stt.stalls - logged_stalls), (unsigned long long)(stt.waits - logged_waits), double(stt.ahead),
			        via_relay() ? ", through a relay" : "",
			        stt.lockstep_holds ? (", " + std::to_string(stt.lockstep_holds) + " waits for the other side's input in the select, all match").c_str() : "");
		if (logged_at > 0 && stt.steps > logged_steps) {   // (where a frame's time went: what's slow on a slow machine)
			const double n = double(stt.steps - logged_steps);
			fprintf(stderr, "online: a frame's step %.2f ms on average, %.1f the worst: its save %.2f, its frame %.2f, frames run again %.2f, "
			        "the desync check %.2f; of late a step %.2f, a frame run again %.2f: %d frames can be run again here, %d there\n",
			        (stt.save_ms - logged_save + stt.run_ms - logged_run + stt.resim_ms - logged_resim_ms + stt.hash_ms - logged_hash) / n,
			        stt.step_max_ms, (stt.save_ms - logged_save) / n, (stt.run_ms - logged_run) / n, (stt.resim_ms - logged_resim_ms) / n, (stt.hash_ms - logged_hash) / n,
			        stt.step_recent_ms, stt.resim_recent_ms, adelay.rollback_room(stt), stt.peer_room);
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
	if (session && ph == Phase::playing) session->set_room(adelay.rollback_room(session->stats()));   // (told the other side, Auto or not)
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
	if (ph == Phase::watching) {   // (a watched match: the frames due from what the lobby's given, no input)
		if (!spec) return false;
		spec->time(now);
		bool shown = false;
		const int ran = spec->tick(&shown, 10.0);
		if (ran) score.frame(board);
		if (shown && !watch_shown) {   // (caught up: its first picture, the sound from now on)
			watch_shown = true;
			adapter.mute = false;
			notices.push_back({ Notice::watching, tr::fill(tr::WATCHING_P1_VS_P2, { { "p1", player_name(0) }, { "p2", player_name(1) } }) });
		}
		return shown;
	}
	if (ph == Phase::playing && session) {   // (the character select in lockstep: a guess put right shows there, a face flashing)
		// (over only once the select's byte has been off SELECT_LINGER frames: it drops for ~50 mid-select at times,
		// as the faces load, and each way out and back moved the input delay under the player's cursor)
		const bool sel = vf3net::in_select(board);
		const uint32_t f = session->frame();
		if (vf3net::scene_byte(board) != select_was) {
			select_was = vf3net::scene_byte(board);
			if (session->in_lockstep() || sel) log_line("the select's byte " + std::to_string(select_was) + " at frame " + std::to_string(f));
		}
		if (sel) select_off_from = UINT32_MAX;
		else if (select_off_from == UINT32_MAX) select_off_from = f;
		const bool want = vf3net::SELECT_LOCKSTEP && (sel || (session->in_lockstep() && f - select_off_from < SELECT_LINGER));
		if (want != session->in_lockstep()) {
			session->set_lockstep(want);
			log_line((want ? "the character select: in lockstep, input delay " + std::to_string(session->delay_now()) : "the character select over: rollback again, input delay " + std::to_string(session->delay())) +
			         " (frame " + std::to_string(f) + ", " + std::to_string(session->confirmed()) + " confirmed)");
		}
	}
	if (!(ph == Phase::playing && session && session->step(input))) return false;
	score.frame(board);   // (the score between the two, from the game's RAM: vf3net::Score)
	if (feed.on()) {   // (the frames confirmed since, to go to the lobby with the score: nothing waits for it)
		feed.take(*session);
		vfnet::watch::Info i;
		for (int p = 0; p < 2; p++) {
			i.wins[p] = uint8_t(std::clamp(score.matches[p], 0, 255));
			i.rounds[p] = uint8_t(std::clamp(score.rounds[p], 0, 255));
		}
		feed.set_info(i);
	}
	if (score.players() == 3) both_in = true;
	const uint8_t in_now = score.players_now();
	for (int p = 0; p < 2; p++)
		if ((in_before >> p & 1) && !(in_now >> p & 1)) out_from[p] = session->inputs(local_player()).size() - 1;   // (this frame's)
	in_before = in_now;
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
	if (ph == Phase::watching) return watch_status();   // (finding it, its start, catching up: under STARTING THE MATCH with the menu closed)
	const std::string who = session && !session->opponent_name().empty() ? opponent() : online_menu::clean_name(peer_name);
	if (rooms) {
		const std::string relay = via_relay() ? " (through the relay)" : "";
		if (checking) return "Connected to " + (who.empty() ? std::string("your opponent") : who) + relay + ": checking the ping...";
		switch (st) {
		case Step::getting_ready: return "Getting ready...";
		case Step::opening: return "Opening a room...";
		case Step::waiting:   // (a random match's host waits too: for the opponent the lobby found, no code to send)
			if (random) return "Connecting to " + (who.empty() ? std::string("your opponent") : who) + "...";
			return tr::SEND_THE_CODE;
		case Step::searching:
			if (!note.empty()) return note;   // (an opponent passed over, the service hard to reach)
			return players > 1 ? "Looking for an opponent near you (" + std::to_string(players - 1) + " more searching)..." : "Looking for an opponent near you...";
		case Step::finding:
			if (queue_at > 0) {
				return tr::fill(tr::IN_LINE, { { "code", room_code() }, { "n", std::to_string(queue_at) }, { "of", std::to_string(queue_len) } });
			}
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
	cells_at = now;
	if (ph == Phase::watching) {   // (a watched match: who plays, the score, how far behind, how many watch)
		if (!spec || !watch_shown) return false;
		c = {};
		c.watching = true;
		c.name = tr::WATCHING;
		const vfnet::watch::Info &i = spec->info();
		c.score = player_name(0) + "  " + std::to_string(i.wins[0]) + "\xE2\x80\x93" + std::to_string(i.wins[1]) + "  " + player_name(1);
		c.watchers = watch_watchers;
		c.level = spec->state() == vfnet::watch::Spectator::State::buffering ? 1 : 0;
		return true;
	}
	if (!session || ph != Phase::playing) return false;
	const auto &stt = session->stats();
	c = {};
	c.watchers = watchers();
	c.name = opponent();
	c.relay = via_relay();
	c.interrupted = session->interrupted();
	c.ping = int(stt.rtt_ms + 0.5);
	c.delay = session->delay();
	c.rollback = stt.rollback_max;
	// How good, by what the player feels rather than by the ping: Auto's input delay and rollback hide a long way
	// (a 310 ms match at delay 7 with rollbacks of up to 7 frames played well), so it's the delay, the rollbacks' size
	// and frames held back for the other side (the game slowing: a few a second unseen) that count. Good up to 7 frames'
	// delay and rollback and 3 frames held a second; fair up to 10 and 10; poor past that, or the connection dropped.
	const double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
	// (the time frames were held back, a second at a time: counting the tries was one a pass, so 4 at 240 Hz for one
	// frame, and counting frames made a 400 ms hold one)
	// (and the hold going on now, so the colour turns during a long one, not after it)
	const double stalled = session->stalled_now_ms();
	// (a new count when it wasn't looked at for a while, the versus screen or between matches: a hold in a match's first
	// second otherwise spread over all that time)
	if (stalls_mark_at < 0 || stalled < stalls_mark || now - stalls_mark_at > 3000) { stalls_mark = stalled; stalls_mark_at = now; stalls_per_s = 0; }
	else if (now - stalls_mark_at >= 1000) {
		stalls_per_s = (stalled - stalls_mark) / (now - stalls_mark_at) * 1000 / (1000.0 / 57.5);   // (in frames' time a second)
		stalls_mark = stalled;
		stalls_mark_at = now;
	}
	const int worst = std::max(c.delay, c.rollback);
	c.level = c.interrupted || worst > 10 || stalls_per_s > 10 ? 2 : worst > 7 || stalls_per_s > 3 ? 1 : 0;
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
		"and the other player's moves can jump a little more (rollback). Auto uses as little as it can (none on a LAN, 1 almost "
		"anywhere else), never more rollback than this machine runs without a hitch, and keeps the game at full speed against a "
		"far opponent with more delay (up to 15); it only ever lowers it between rounds.",
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
	return { "Connection display", "In a match, small at the bottom of the screen: the other player's name, the ping, the input delay, "
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
		return r + (pad ? foot(true, { { "A", "", tr::HINT_TYPE }, { "X", "", tr::HINT_DELETE }, { "Start", "", tr::HINT_SAVE }, { "B", "", tr::HINT_CANCEL } })
		                : foot(false, { { "", "Enter", tr::HINT_SAVE }, { "", "Backspace", tr::HINT_DELETE }, { "", "Esc", tr::HINT_CANCEL } })) + "</div>";
	};
	return p;
}

// Joining with a code: typed or picked, and joined at its 6th letter
menu_page code_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Enter code", "", {} };
	auto t = std::make_shared<Entry>();
	auto join = [&o, &s, &menu, t, changed] {
		if (t->text.size() != 6) { t->problem = "A code has 6 letters and numbers."; return; }
		std::string err;
		if (!o.enter_code(s, t->text, err)) { t->problem = err; return; }   // (a room open: joined; its match on: watched)
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
		                "<div class=\"sub\">A room's code: you join it, or watch the match on in it</div>"
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
	p.items.push_back({ "Random: max ping", "Random only passes over an opponent whose ping is higher than this: it's measured as you connect, "
		"before the match starts, and if it's over, the game leaves and looks for someone else (the two of you aren't matched again "
		"for a while). Any: whoever's nearest. Room codes and addresses play whatever the ping.",
		[&s] { return s.max_ping > 0 ? std::to_string(s.max_ping) + " ms" : std::string("Any"); },
		[&s, changed](int d) {
			static const int steps[] = { 0, 80, 100, 150, 200, 250 };
			int i = 0;
			while (i < 5 && steps[i] != s.max_ping) i++;
			if (steps[i] != s.max_ping) i = 0;
			s.max_ping = steps[(i + (d < 0 ? 5 : 1)) % 6];
			changed();
		} });
	p.items.push_back({ "Stage select", "On: the arcade's stage screen, as in Japan's arcades: the challenger picks the stage, so on every rematch "
		"the loser does. Off: the game's next stage each match. A match plays the host's choice (said as it starts), so you meet everyone; against an older version, the same as theirs. Not while hosting, joining or playing.",
		[&s] { return std::string(s.stage_select ? "On" : "Off"); },
		[&o, &s, changed](int) {
			if (o.phase() != Online::Phase::off) return;   // (a room open or a match on: its version stays as it began)
			s.stage_select = !s.stage_select;
			changed();
		} });
	p.items.push_back({ "Match found chime", "A chime when an online match is found (and the game's window flashes if it isn't the one in front), so "
		"you can wait in another window.",
		[&s] { return std::string(s.chime ? "On" : "Off"); }, [&s, changed](int) { s.chime = !s.chime; changed(); } });
	p.items.push_back({ tr::QUICK_LISTED, tr::QUICK_LISTED_HELP,
		[&s] { return std::string(s.quick_listed ? tr::ON : tr::OFF); }, [&s, changed](int) { s.quick_listed = !s.quick_listed; changed(); } });
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	return p;
}

// Playing by address: on the same network (Wi-Fi, a cable) or both on a VPN like Tailscale, no online service. The
// host's address typed (a keyboard) or picked (a pad): as the host's screen shows it, four numbers with dots, a name,
// either with :port, or an IPv6 address in [ ]. (Typed in capitals, as the other screens: clean_address lowers them.)
const std::string ADDRESS_PAD = "0123456789.:ABCDEFGHIJKLMNOPQRSTUVWXYZ-[]";
menu_page join_address_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Join by address", "", {} };
	auto t = std::make_shared<Entry>();
	t->text = s.address;
	for (char &ch : t->text) ch = char(toupper(uint8_t(ch)));
	const size_t max = 64;
	auto join = [&o, &s, &menu, t, changed] {
		const std::string a = clean_address(t->text);
		if (a.empty()) { t->problem = "Type the host's address."; return; }
		s.address = a;
		changed();
		std::string err;
		if (!o.join(s, err)) { t->problem = err; return; }
		menu.pop();   // (this page, and Play by address: the Online screen, joining)
		menu.pop();
		again(o, s, menu, changed);
	};
	for (char ch : ADDRESS_PAD)
		p.items.push_back({ std::string(1, ch), "", {}, {}, [t, ch, max] { add_to(*t, ADDRESS_PAD, max, std::string(1, ch)); } });
	p.items.push_back({ "Delete", "", {}, {}, [t] { if (!t->text.empty()) t->text.pop_back(); t->problem.clear(); } });
	p.items.push_back({ "OK", "", {}, {}, join });
	p.start = 0;
	p.text = true;
	p.on_event = [&menu, t, join, max](const SDL_Event &e) {
		return entry_event(menu, *t, ADDRESS_PAD, max, e, join, [&menu] { menu.pop(); }, [] {});
	};
	p.screen = [t](int sel, bool pad) {
		std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">JOIN BY ADDRESS</div>"
		                "<div class=\"sub\">The address the host's screen shows (192.168.1.20, or with :port if it's not 7700)</div>"
		                "<div class=\"address\">" + (t->text.empty() ? std::string("&nbsp;") : E(t->text)) + "</div>";
		if (pad) r += keypad(ADDRESS_PAD, 14, 0, sel, true, "name");
		else r += "<div class=\"typed\"><div>Just type it</div><div class=\"small\">Numbers, dots, a name, :port</div></div>";
		if (!t->problem.empty()) r += "<div class=\"problem\">" + E(t->problem) + "</div>";
		r += "</div>";
		return r + (pad ? foot(true, { { "A", "", "type" }, { "X", "", "delete" }, { "Start", "", "join" }, { "B", "", "cancel" } })
		                : foot(false, { { "", "Enter", "join" }, { "", "Backspace", "delete" }, { "", "Esc", "cancel" } })) + "</div>";
	};
	return p;
}

menu_page address_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Play by address", "On the same network (Wi-Fi, a cable), or both on a VPN like Tailscale: no online service needed. "
	             "(Room codes connect directly too, whenever the routers allow it.)", {} };
	auto problem = std::make_shared<std::string>();
	menu_item h{ "Host by address", "Waits for a player on your network (or your VPN) to join with your address, which your screen shows.", {}, {},
		[&o, &s, &menu, problem, changed] {
			named(s, menu, changed, [&o, &s, &menu, problem, changed] {
				std::string err;
				if (!o.host(s, err)) { *problem = err; return; }
				menu.pop();   // (this page: the Online screen, waiting)
				again(o, s, menu, changed);
			});
		} };
	h.below = [problem] { return *problem; };
	p.items.push_back(h);
	p.items.push_back({ "Join by address", "Joins a player hosting by address: type the address their screen shows.", {}, {},
		[&o, &s, &menu, changed] { named(s, menu, changed, [&o, &s, &menu, changed] { menu.push(join_address_page(o, s, menu, changed)); }); } });
	p.items.push_back({ "Port", "The UDP port Host by address waits on (7700 unless another program has it). Room codes need no port.",
		[&s] { return std::to_string(s.port); },
		[&s, changed](int d) { s.port = std::clamp(s.port + d, 1024, 65535); changed(); } });
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

// B or Esc: back a page
bool backs(game_menu &menu, const SDL_Event &e)
{
	if ((e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) ||
	    (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_BACKSPACE))) {
		menu.pop();
		return true;
	}
	return false;
}

// A room's name, typed as a player's is (and '): up to 16
const std::string ROOM_LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-!?'";
const std::string ROOM_PAD = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!.0123456789-'";
std::string room_clean(const std::string &s)
{
	std::string out;
	for (char ch : s) {
		ch = char(toupper(uint8_t(ch)));
		if (ROOM_LETTERS.find(ch) != std::string::npos && out.size() < 16) out += ch;
	}
	while (!out.empty() && out.back() == ' ') out.pop_back();
	while (!out.empty() && out.front() == ' ') out.erase(out.begin());
	return out;
}
// (the name filled in: the room's data, sent to the lobby and shown to everyone as it is: not translated)
std::string room_name_of(const OnlineSettings &s) { return s.room_name.empty() ? room_clean(s.name + "'S ROOM") : s.room_name; }

menu_page room_name_page(OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Room name", "", {} };
	auto t = std::make_shared<Entry>();
	t->text = room_name_of(s);
	auto save = [&s, &menu, t, changed] {
		const std::string n = room_clean(t->text);
		if (n.empty()) { t->problem = tr::ROOM_NAME_NEEDS; return; }
		s.room_name = n;
		changed();
		menu.pop();
	};
	for (char ch : ROOM_PAD)
		p.items.push_back({ std::string(1, ch), "", {}, {}, [t, ch] { add_to(*t, ROOM_LETTERS, 16, std::string(1, ch)); } });
	p.items.push_back({ "Delete", "", {}, {}, [t] { if (!t->text.empty()) t->text.pop_back(); } });
	p.items.push_back({ "OK", "", {}, {}, save });
	p.start = 0;
	p.text = true;
	p.on_event = [&menu, t, save](const SDL_Event &e) { return entry_event(menu, *t, ROOM_LETTERS, 16, e, save, [&menu] { menu.pop(); }, [] {}); };
	p.screen = [t](int sel, bool pad) {
		std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">" + E(tr::ROOM_NAME_TITLE) + "</div>"
		                "<div class=\"sub\">" + E(tr::ROOM_NAME_SUB) + "</div>"
		                "<div class=\"roomname\">" + E(t->text) + "<span class=\"caret\"></span></div>";
		if (pad) r += keypad(ROOM_PAD, 14, 0, sel, true, "name");
		else r += "<div class=\"typed\"><div>" + E(tr::JUST_TYPE_IT) + "</div><div class=\"small\">" + E(tr::ROOM_NAME_LETTERS) + "</div></div>";
		if (!t->problem.empty()) r += "<div class=\"problem\">" + E(t->problem) + "</div>";
		r += "</div>";
		return r + (pad ? foot(true, { { "A", "", tr::HINT_TYPE }, { "X", "", tr::HINT_DELETE }, { "Start", "", tr::HINT_SAVE }, { "B", "", tr::HINT_CANCEL } })
		                : foot(false, { { "", "Enter", tr::HINT_SAVE }, { "", "Backspace", tr::HINT_DELETE }, { "", "Esc", tr::HINT_CANCEL } })) + "</div>";
	};
	return p;
}

// Create room: its name (filled in) and whether it's listed in Rooms (not, by default: the code only, as every room
// before). Every match in it may be watched by anyone with the code; after each, the next in line plays the host.
menu_page create_room_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "Create room", "", {} };
	auto problem = std::make_shared<std::string>();
	const int name = 0, listed = 1, go = 2;
	p.items.push_back({ "Name", "", {}, {}, [&s, &menu, changed] { menu.push(room_name_page(s, menu, changed)); } });
	p.items.push_back({ "Listed in Rooms", "", {}, {}, [&s, changed] { s.room_who = s.room_who ? 0 : 2; changed(); } });
	p.items.push_back({ "Create", "", {}, {}, [&o, &s, &menu, problem, changed] {
		if (s.room_name.empty()) s.room_name = room_name_of(s);
		std::string err;
		if (!o.host_room(s, err)) { *problem = err; return; }
		menu.pop();
		again(o, s, menu, changed);   // (the Online screen: the room's code, step by step)
	} });
	p.start = go;
	p.on_event = [&menu](const SDL_Event &e) { return backs(menu, e); };
	p.screen = [&s, problem](int sel, bool pad) {
		std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">" + E(tr::CREATE_ROOM_TITLE) + "</div><div class=\"form\">";
		r += "<div " + menu_row(name, sel, "fr") + "><div class=\"fl\">" + E(tr::ROOM_NAME_FIELD) + "</div><div class=\"fv\"><span class=\"fbox\">" + E(room_name_of(s)) + "</span></div></div>";
		r += "<div " + menu_row(listed, sel, "fr") + "><div class=\"fl\">" + E(tr::LISTED_IN_ROOMS) + "</div><div class=\"fv\">" + E(s.room_who ? tr::ON : tr::OFF) + "</div>"
		     "<div class=\"fs fr2\">" + E(s.room_who ? tr::LISTED_ON : tr::LISTED_OFF) + "</div></div></div>";
		r += "<div class=\"note\">" + E(tr::CREATE_ROOM_NOTE) + "</div>";
		if (!problem->empty()) r += "<div class=\"problem\">" + E(*problem) + "</div>";
		r += "<div class=\"btns\"><div " + menu_row(go, sel, "btn") + ">" + E(tr::BUTTON_CREATE) + "</div></div></div>";
		return r + foot(pad, { { "A", "Enter", sel == name ? tr::HINT_CHANGE_NAME : sel == listed ? tr::HINT_CHANGE : tr::HINT_CREATE }, { "B", "Esc", tr::HINT_BACK } }) + "</div>";
	};
	return p;
}


} // namespace

// ---- the room browser (the Online screen): the lobby's rooms, this game's, nearest first; a window of them on screen ----

namespace {

constexpr int B_ROWS = 10;   // (rows on screen: the rest scroll)
// the screen's items: the name, the actions, the filter, then a row a room
enum { B_CHIP, B_QUICK, B_CREATE, B_CODE, B_MORE, B_ALL, B_OPEN, B_PLAYING, B_ROW0 };
struct Browser
{
	int first = 0, offset = 0;     // (the window's first row; the selected row's place in it)
	std::string sel_key;           // (the room selected: kept as the list comes again)
	int sel_item = B_QUICK;        // (or the item, not a room)
	std::set<std::string> known;   // (the rooms seen: new ones counted)
	int fresh = 0, version = -1;
};
Browser &browser() { static Browser b; return b; }
std::string room_key(const Online::RoomRow &r) { return r.code + "|" + r.watch + "|" + r.name[0]; }

// This game's rooms that this version can join or watch (an older version's not shown), by the filter: the open ones
// first, the nearest first, the newest; then the matches, the most watched first
std::vector<Online::RoomRow> shown_rooms(Online &o, const OnlineSettings &s)
{
	std::vector<Online::RoomRow> v;
	for (const Online::RoomRow &r : o.rooms_list(s))
		if (!r.older && (s.rooms_show == 0 || (s.rooms_show == 1) != r.playing)) v.push_back(r);
	const std::string you = o.rooms_you();
	std::stable_sort(v.begin(), v.end(), [&](const Online::RoomRow &a, const Online::RoomRow &b) {
		if (a.playing != b.playing) return !a.playing;
		if (!a.playing && (a.region == you) != (b.region == you)) return a.region == you;
		if (a.playing && a.watchers != b.watchers) return a.watchers > b.watchers;
		return a.since < b.since;
	});
	return v;
}

std::string room_title(const Online::RoomRow &r) { return r.quick ? tr::QUICK_MATCH_TAG : r.room.empty() ? tr::fill(tr::HOSTS_ROOM, { { "name", r.name[0] } }) : r.room; }
std::string fit(const std::string &s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "\xE2\x80\xA6"; }   // (a name cut to its column, with ...)

// A on a room: join it if it's open, watch it if a match is on (its line: from the room's own page, or Play next)
void room_act(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, const Online::RoomRow &r, std::shared_ptr<std::string> problem, bool join)
{
	const std::string code = r.code.empty() ? r.watch : r.code;
	if (code.empty()) {   // (a locked room: listed with no code, its host gives it: typed in Enter code)
		named(s, menu, changed, [&o, &s, &menu, changed] { menu.push(code_page(o, s, menu, changed)); });
		return;
	}
	if (r.playing && !join) {
		std::string err;
		if (!o.watch(s, r.watch.empty() ? code : r.watch, err)) { *problem = err; return; }
		while (menu.depth() > 1 && menu.top_name() != "Online") menu.pop();
		again(o, s, menu, changed);
		return;
	}
	named(s, menu, changed, [&o, &s, &menu, code, problem, changed] {
		std::string err;
		if (!o.join_room(s, code, err)) { *problem = err; return; }
		while (menu.depth() > 1 && menu.top_name() != "Online") menu.pop();
		again(o, s, menu, changed);
	});
}

// A room's own page (Y on it): who plays, how many watch, who's next; watch, play next, back
menu_page room_view(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, const Online::RoomRow &r)
{
	menu_page p{ "Room", "", {} };
	auto problem = std::make_shared<std::string>();
	int watch = -1;
	if (r.playing) {
		watch = int(p.items.size());
		p.items.push_back({ "Watch", "", {}, {}, [&o, &s, &menu, r, problem, changed] { room_act(o, s, menu, changed, r, problem, false); } });
	}
	const int join = r.quick ? -1 : int(p.items.size());   // (a Quick match's: to watch only)
	if (!r.quick) p.items.push_back({ "Join", "", {}, {}, [&o, &s, &menu, r, problem, changed] { room_act(o, s, menu, changed, r, problem, true); } });
	const int back = int(p.items.size());
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	p.start = watch >= 0 ? watch : join;
	p.on_event = [&menu](const SDL_Event &e) { return backs(menu, e); };
	p.screen = [r, watch, join, back, problem](int sel, bool pad) {
		const std::string c = r.code.empty() ? r.watch : r.code, dashed = c.size() == 6 ? c.substr(0, 3) + "-" + c.substr(3) : std::string("\xE2\x80\x94");   // (a locked room: none)
		std::string out = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">" + E(room_title(r)) + "</div><div class=\"sub small\">" +
		                  (r.quick ? E(tr::fill(tr::ROOM_QUICK_SUB, { { "code", dashed } })) : tr::fill(E(tr::ROOM_HOSTED_BY).c_str(), { { "flag", flag(r.country[0], "flag s hb") }, { "name", E(r.name[0]) }, { "code", E(dashed) } })) +
		                  "</div><div class=\"panes\"><div class=\"pane\">";
		if (r.playing)
			out += "<div class=\"ph\">" + E(tr::ROOM_PLAYING) + "</div><div class=\"pvs\">" + flag(r.country[0]) + E(r.name[0]) + "<span class=\"psc\">" + std::to_string(r.wins[0]) + " \xE2\x80\x93 " + std::to_string(r.wins[1]) +
			       "</span>" + E(r.name[1]) + flag(r.country[1], "flag r") + "</div><div class=\"pq\">" + E(tr::fill(tr::N_WATCHING, { { "n", std::to_string(r.watchers) } })) + "</div>";
		else out += "<div class=\"ph\">" + E(tr::ROOM_OPEN) + "</div><div class=\"pvs\">" + flag(r.country[0]) + E(r.name[0]) + "</div><div class=\"pq\">" + E(tr::WAITING_FOR_PLAYER) + "</div>";
		if (!r.quick)
			out += "</div><div class=\"pane\"><div class=\"ph\">" + E(tr::ROOM_NEXT) + "</div><div class=\"pq\">" +
			       E(!r.playing ? std::string(tr::JOIN_AND_PLAY) : r.queue > 0 ? tr::fill(tr::N_WAITING_TO_PLAY, { { "n", std::to_string(r.queue) } }) : std::string(tr::NOBODY_WAITING)) + "</div>";
		out += "</div></div>";
		if (!problem->empty()) out += "<div class=\"problem\">" + E(*problem) + "</div>";
		out += "<div class=\"btns\">";
		if (watch >= 0) out += "<div " + menu_row(watch, sel, "btn") + ">" + E(tr::BUTTON_WATCH) + "</div>";
		if (join >= 0) out += "<div " + menu_row(join, sel, "btn") + ">" + E(r.playing ? tr::BUTTON_PLAY_NEXT : tr::BUTTON_JOIN) + "</div>";
		out += "<div " + menu_row(back, sel, "btn") + ">" + E(tr::BUTTON_BACK) + "</div></div></div>";
		return out + foot(pad, { { "A", "Enter", tr::HINT_SELECT }, { "B", "Esc", tr::HINT_BACK } }) + "</div>";
	};
	return p;
}

// More: what's seldom wanted (on the same network, by address; the settings)
menu_page more_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p{ "More", "", {} };
	p.items.push_back({ tr::PLAY_BY_ADDRESS, tr::PLAY_BY_ADDRESS_HELP, {}, {}, [&o, &s, &menu, changed] { menu.push(address_page(o, s, menu, changed)); } });
	p.items.push_back({ tr::SETTINGS, tr::SETTINGS_HELP, {}, {}, [&o, &s, &menu, changed] { menu.push(settings_page(o, s, menu, changed)); } });
	p.items.push_back({ "Back", "", {}, {}, [&menu] { menu.pop(); } });
	return p;
}

// Enter code: a room's code, to join it, or watch the match on in it
menu_page enter_code_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed)
{
	menu_page p = code_page(o, s, menu, changed);
	p.name = "Enter code";
	return p;
}

// The browser itself: the Online screen when nothing's on
void browser_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, menu_page &p, std::shared_ptr<std::string> problem)
{
	Browser &b = browser();
	const std::vector<Online::RoomRow> rows = shown_rooms(o, s);
	if (o.rooms_version() != b.version) {   // (rooms new since the list was last seen: counted, not moving the one selected)
		const bool first_time = b.version < 0 || b.known.empty();
		for (const Online::RoomRow &r : rows)
			if (b.known.insert(room_key(r)).second && !first_time) b.fresh++;
		b.version = o.rooms_version();
	}
	int sel_row = -1;
	for (size_t i = 0; i < rows.size() && !b.sel_key.empty(); i++) if (room_key(rows[i]) == b.sel_key) sel_row = int(i);
	if (sel_row >= 0) b.first = std::clamp(sel_row - b.offset, 0, std::max(0, int(rows.size()) - B_ROWS));
	else if (!b.sel_key.empty()) b.sel_key.clear();
	b.first = std::clamp(b.first, 0, std::max(0, int(rows.size()) - B_ROWS));

	auto change_name = [&s, &menu, changed] { menu.push(name_page(s, menu, changed, {})); };
	p.items.push_back({ "Your name", "", {}, {}, change_name });
	p.items.push_back({ "Quick match", "", {}, {}, [&o, &s, &menu, problem, changed] {
		named(s, menu, changed, [&o, &s, &menu, problem, changed] {
			std::string err;
			if (!o.find_random(s, err)) { *problem = err; return; }
			again(o, s, menu, changed);
		});
	} });
	p.items.push_back({ "Create room", "", {}, {}, [&o, &s, &menu, changed] { named(s, menu, changed, [&o, &s, &menu, changed] { menu.push(create_room_page(o, s, menu, changed)); }); } });
	p.items.push_back({ "Enter code", "", {}, {}, [&o, &s, &menu, changed] { named(s, menu, changed, [&o, &s, &menu, changed] { menu.push(enter_code_page(o, s, menu, changed)); }); } });
	p.items.push_back({ "More", "", {}, {}, [&o, &s, &menu, changed] { menu.push(more_page(o, s, menu, changed)); } });
	auto filter = [&o, &s, &menu, changed](int show) {
		return [&o, &s, &menu, changed, show] { s.rooms_show = show; changed(); browser().first = 0; browser().sel_key.clear(); menu.replace(page(o, s, menu, changed), -1); };
	};
	p.items.push_back({ "All", "", {}, {}, filter(0) });
	p.items.push_back({ "Open", "", {}, {}, filter(1) });
	p.items.push_back({ "Playing", "", {}, {}, filter(2) });
	for (const Online::RoomRow &r : rows)
		p.items.push_back({ room_title(r), "", {}, {}, [&o, &s, &menu, r, problem, changed] { room_act(o, s, menu, changed, r, problem, false); } });
	p.start = sel_row >= 0 ? B_ROW0 + sel_row : std::min(b.sel_item, B_ROW0 - 1);
	const int n = int(rows.size());
	auto to_row = [&menu, n](int row) {   // (a room selected, the window following it)
		if (n == 0) return;
		row = std::clamp(row, 0, n - 1);
		Browser &b = browser();
		if (row < b.first) b.first = row;
		if (row >= b.first + B_ROWS) b.first = row - B_ROWS + 1;
		menu.select_row(B_ROW0 + row);
	};
	p.on_event = [&o, &s, &menu, changed, rows, n, to_row, change_name](const SDL_Event &e) {
		const int sel = menu.selected(), row = sel - B_ROW0;
		const bool key = e.type == SDL_EVENT_KEY_DOWN, padb = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
		const SDL_Scancode sc = key ? e.key.scancode : SDL_SCANCODE_UNKNOWN;
		const int button = padb ? e.gbutton.button : -1;
		if ((key && sc == SDL_SCANCODE_F5) || button == SDL_GAMEPAD_BUTTON_WEST) { o.rooms_list(s, true); return true; }
		if (button == SDL_GAMEPAD_BUTTON_NORTH || (key && sc == SDL_SCANCODE_TAB)) {   // (Y: the room's own page; elsewhere, the name)
			if (row >= 0 && row < n) menu.push(room_view(o, s, menu, changed, rows[size_t(row)]));
			else change_name();
			return true;
		}
		const bool up = (key && sc == SDL_SCANCODE_UP) || button == SDL_GAMEPAD_BUTTON_DPAD_UP, down = (key && sc == SDL_SCANCODE_DOWN) || button == SDL_GAMEPAD_BUTTON_DPAD_DOWN;
		const bool pgup = (key && sc == SDL_SCANCODE_PAGEUP) || button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, pgdn = (key && sc == SDL_SCANCODE_PAGEDOWN) || button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
		if (e.type == SDL_EVENT_MOUSE_WHEEL && n > 0) {   // (the wheel: the list, three rooms a notch)
			to_row((row >= 0 ? row : browser().first) + (e.wheel.y > 0 ? -3 : 3));
			return true;
		}
		if ((pgup || pgdn) && n > 0) { to_row((row >= 0 ? row : 0) + (pgup ? -B_ROWS : B_ROWS)); return true; }
		// (Left / Right along the buttons and along the filter, one by one: never off to the other line)
		const bool left = (key && sc == SDL_SCANCODE_LEFT) || button == SDL_GAMEPAD_BUTTON_DPAD_LEFT, right = (key && sc == SDL_SCANCODE_RIGHT) || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
		if ((left || right) && sel >= B_QUICK && sel < B_ROW0) {
			const int lo = sel <= B_MORE ? B_QUICK : B_ALL, hi = sel <= B_MORE ? B_MORE : B_PLAYING;
			menu.select_row(std::clamp(sel + (right ? 1 : -1), lo, hi));
			return true;
		}
		if (row >= 0 && row < n && (up || down)) {
			if (up && row == 0) { browser().first = 0; menu.select_row(B_ALL + s.rooms_show); return true; }   // (out of the list: to the filter)
			to_row(row + (up ? -1 : 1));
			return true;
		}
		if (down && sel >= B_ALL && sel < B_ROW0 && n > 0) { to_row(browser().first); return true; }   // (into the list where it's scrolled to)
		if (up && sel >= B_ALL && sel < B_ROW0) { menu.select_row(B_QUICK); return true; }
		return false;
	};
	p.screen = [&o, &s, rows, n, problem](int sel, bool pad) {
		Browser &b = browser();
		const int row = sel - B_ROW0;
		if (row >= 0 && row < n) {
			if (row < b.first) b.first = row;
			if (row >= b.first + B_ROWS) b.first = row - B_ROWS + 1;
			b.sel_key = room_key(rows[size_t(row)]);
			b.offset = row - b.first;
		} else { b.sel_key.clear(); b.sel_item = sel; }
		if (b.first == 0) b.fresh = 0;
		const int players = o.players_online(s);
		std::string r = "<div class=\"scr\"><div class=\"bhead\"><div class=\"btitle\">" + E(tr::ONLINE) + "</div><div class=\"bgame\">" + E(o.identity.game == "vf3tb" ? tr::GAME_VF3TB : tr::GAME_VF3) + "</div>"
		                "<div " + menu_row(B_CHIP, sel, "chip bchip") + ">";
		if (s.name.empty()) r += "<div class=\"me\">" + E(tr::SET_YOUR_NAME) + "</div>";
		else r += "<div class=\"who\">" + E(tr::PLAYING_AS) + "</div>" + flag(o.my_country()) + "<div class=\"me\">" + E(s.name) + "</div>";
		r += "</div></div><div class=\"bacts\">"
		     "<div " + menu_row(B_QUICK, sel, "ab") + ">" + E(tr::QUICK_MATCH) + (players > 0 ? "<span class=\"abs\"><span class=\"live\"></span>" + E(tr::fill(tr::N_ONLINE, { { "n", std::to_string(players) } })) + "</span>" : std::string()) + "</div>"
		     "<div " + menu_row(B_CREATE, sel, "ab") + ">" + E(tr::CREATE_ROOM) + "</div><div " + menu_row(B_CODE, sel, "ab") + ">" + E(tr::ENTER_CODE) + "</div>"
		     "<div class=\"abgap\"></div><div " + menu_row(B_MORE, sel, "ab abmore") + ">" + E(tr::MORE) + "</div></div>";
		auto f = [&](int i, const char *label, bool on) { return "<div " + menu_row(i, sel, std::string("bf") + (on ? " on" : "")) + ">" + label + "</div>"; };
		r += "<div class=\"bfilt\">" + f(B_ALL, tr::FILTER_ALL, s.rooms_show == 0) + f(B_OPEN, tr::FILTER_OPEN, s.rooms_show == 1) + f(B_PLAYING, tr::FILTER_PLAYING, s.rooms_show == 2);
		r += "<div class=\"bcount\">" + E(n > B_ROWS ? tr::fill(tr::ROWS_RANGE, { { "from", std::to_string(b.first + 1) }, { "to", std::to_string(std::min(n, b.first + B_ROWS)) }, { "n", std::to_string(n) } })
		                                          : n == 1 ? std::string(tr::ONE_ROOM) : n ? tr::fill(tr::N_ROOMS, { { "n", std::to_string(n) } }) : std::string()) +
		     (b.fresh > 0 && b.first > 0 ? "<span class=\"bnewc\">" + E(tr::fill(tr::N_NEW_ABOVE, { { "n", std::to_string(b.fresh) } })) + "</span>" : std::string()) + "</div></div>";
		r += "<div class=\"blist\"><div class=\"bcols\"><div class=\"c1\">" + E(tr::COL_ROOM) + "</div><div class=\"c2\">" + E(tr::COL_HOST) + "</div><div class=\"c3\">" + E(tr::COL_STATUS) +
		     "</div><div class=\"c5\">" + E(tr::COL_WATCHING) + "</div><div class=\"c7\"></div></div>";
		if (!n) {
			const int st = o.rooms_state();
			const int all = int(o.rooms_list(s).size());
			std::string big, small;
			if (st <= 0) big = tr::LOOKING_FOR_ROOMS;
			else if (st == 2) { big = tr::ONLINE_UNREACHABLE; small = tr::ONLINE_UNREACHABLE_MORE; }
			else if (st == 3) { big = tr::ROOMS_OFF; small = tr::ROOMS_OFF_MORE; }
			else if (all > 0 && s.rooms_show != 0) { big = s.rooms_show == 1 ? tr::NO_OPEN_ROOMS : tr::NO_MATCHES_LISTED; small = tr::ALL_SHOWS_EVERY_ROOM; }
			else { big = tr::NO_ROOMS; small = tr::NO_ROOMS_MORE; }
			r += "<div class=\"bicon" + std::string(st == 2 ? " bad" : "") + "\"><div style=\"top: 9dp;\"></div><div style=\"top: 19dp; right: 22dp;\"></div></div>";
			r += "<div class=\"bempty" + std::string(st == 2 ? " bad" : "") + "\">" + E(big) + "</div>" + (small.empty() ? "" : "<div class=\"bempty2\">" + E(small) + "</div>");
		}
		for (int i = b.first; i < n && i < b.first + B_ROWS; i++) {
			const Online::RoomRow &w = rows[size_t(i)];
			r += "<div " + menu_row(B_ROW0 + i, sel, "brow") + "><div class=\"c1\">" + (w.quick ? "<span class=\"qm\">" + E(tr::QUICK_MATCH_TAG) + "</span>" : E(fit(room_title(w), 16))) + "</div><div class=\"c2\">" +
			     (w.quick ? std::string("<span class=\"nohost\">\xE2\x80\x94</span>") : flag(w.country[0], "flag s") + E(w.name[0])) + "</div><div class=\"c3\">";
			if (w.playing)   // (both players, each with their flag, and the score between)
				r += "<span class=\"mark\">\xE2\x97\x8F</span><span class=\"pn\">" + flag(w.country[0], "flag s") + E(w.name[0]) + "</span><span class=\"sc\">" + std::to_string(w.wins[0]) + "\xE2\x80\x93" + std::to_string(w.wins[1]) +
				     "</span><span class=\"pn\">" + flag(w.country[1], "flag s") + E(w.name[1]) + "</span>";
			else r += "<span class=\"mark open\">\xE2\x97\x8B</span><span class=\"wt\">" + E(tr::ROW_OPEN) + "</span>";
			r += "</div><div class=\"c5\">" + (w.watchers ? std::to_string(w.watchers) : std::string()) + "</div><div class=\"c7\">" + E(w.playing ? tr::ROW_WATCH : tr::ROW_JOIN) + "</div></div>";
		}
		r += "</div>";
		if (n > B_ROWS) {   // (where the window is: a bar and its thumb)
			const int top = 194 + 30, h = 720 - 92 - top, th = std::max(24, h * B_ROWS / n), y = top + (h - th) * b.first / std::max(1, n - B_ROWS);
			r += "<div class=\"bsb\" style=\"top: " + std::to_string(top) + "dp; height: " + std::to_string(h) + "dp;\"></div><div class=\"bsbt\" style=\"top: " + std::to_string(y) + "dp; height: " + std::to_string(th) + "dp;\"></div>";
		}
		if (o.lobby_watches()) r += "<div class=\"bnote\">" + E(tr::BROWSER_NOTE) + "</div>";   // (not said where watching's off)
		if (!problem->empty()) r += "<div class=\"bproblem\">" + E(*problem) + "</div>";
		const bool on_row = row >= 0 && row < n;
		const char *act = on_row ? (rows[size_t(row)].playing ? tr::HINT_WATCH : tr::HINT_JOIN) : tr::HINT_SELECT;
		if (pad) return r + foot(true, { { "A", "", act }, { "Y", "", on_row ? tr::HINT_ROOM : tr::HINT_NAME }, { "LB/RB", "", tr::HINT_PAGE }, { "B", "", tr::HINT_BACK } }) + "</div>";
		return r + foot(false, { { "", "Enter", act }, { "", "Tab", on_row ? tr::HINT_ROOM : tr::HINT_NAME }, { "", "PgUp/PgDn", tr::HINT_PAGE }, { "", "Esc", tr::HINT_BACK } }) + "</div>";
	};
}

} // namespace

// (online.h: the browser is the Online screen itself; this is it, for the host program's "made again" as the list comes)
menu_page rooms_page(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed) { return page(o, s, menu, changed); }

namespace {

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

std::string clean_room(const std::string &s) { return room_clean(s); }

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

	if (o.watching()) {
		// ---- watching a match: found, its start, catching up with the players (then the menu closes on its picture) ----
		const int stop = int(p.items.size());
		p.items.push_back({ "Stop", "", {}, {}, [&o, &s, &menu, changed] { o.stop(); again(o, s, menu, changed); } });
		p.start = stop;
		p.on_event = [&menu](const SDL_Event &e) { return closes(menu, e); };
		p.screen = [&o, stop](int sel, bool pad) {
			const int at = o.watch_step();
			const std::string a = o.player_name(0), b = o.player_name(1);
			std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">" + E(tr::fill(tr::WATCHING_CODE, { { "code", o.watch_code() } })) + "</div><div class=\"sub\">" +
			                (a.empty() ? std::string("&nbsp;") : flag(o.player_country(0)) + E(a) + " &nbsp;vs&nbsp; " + E(b) + flag(o.player_country(1), "flag r")) + "</div>";
			const std::vector<std::pair<std::string, Mark>> v = { { tr::STEP_FIND, at > 0 ? done : now }, { tr::STEP_START, at > 1 ? done : at == 1 ? now : todo },
				{ tr::STEP_CATCH_UP, at > 2 ? done : at == 2 ? now : todo }, { tr::STEP_WATCH, at > 2 ? now : todo } };
			r += "<div class=\"stepsat\">" + steps(v) + "</div><div class=\"status\">" + E(o.watch_status()) + "</div>";
			r += "<div class=\"btns\"><div " + menu_row(stop, sel, "btn") + ">" + E(tr::BUTTON_STOP) + "</div></div></div>";
			return r + foot(pad, { { "A", "Enter", tr::HINT_SELECT }, { "B", "Esc", tr::HINT_CLOSE_MENU } }) + "</div>";
		};
		return p;
	}

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
			r += "<div class=\"btns\"><div " + menu_row(stop, sel, "btn") + ">STOP</div></div>";
			r += (o.lobby_watches() ? "<div class=\"note quiet\">" + E(tr::QUICK_WATCHABLE) + "</div>" : std::string()) + "</div>";   // (the one line about watching: no prompt; none where watching's off)
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
				if (!o.listed_as_name().empty()) r += "<div class=\"sub\">" + E(tr::fill(tr::LISTED_AS, { { "room", o.listed_as_name() } })) + "</div>";
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
				r += "<div class=\"title\">WAITING FOR A PLAYER</div><div class=\"sub\">On your network, or a VPN: they choose Play by address, then Join by address, and type this</div>"
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
			if (t == Online::Try::watch) {
				sub = tr::WATCHING_A_MATCH;
				v = { { tr::STEP_FIND, bad }, { tr::STEP_START, todo }, { tr::STEP_CATCH_UP, todo }, { tr::STEP_WATCH, todo } };
			} else if (t == Online::Try::random) {
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

	if (const Online::LastMatch &m = o.last_match(); m.any && m.watched) {
		// ---- a match watched, over: who, the score, and watching that code again ----
		int again_i = -1;
		if (!m.watch.empty()) {
			again_i = int(p.items.size());
			p.items.push_back({ "Watch again", "", {}, {}, [&o, &s, &menu, problem, changed] {
				std::string err;
				if (!o.watch(s, o.last_match().watch, err)) { *problem = err; return; }
				again(o, s, menu, changed);
			} });
		}
		const int back = int(p.items.size());
		p.items.push_back({ "Back", "", {}, {}, [&o, &s, &menu, changed] { o.set_last_match({}); again(o, s, menu, changed); } });
		p.start = back;
		p.on_event = [&menu](const SDL_Event &e) { return closes(menu, e); };
		p.screen = [&o, again_i, back, problem](int sel, bool pad) {
			const Online::LastMatch &m = o.last_match();
			std::string r = "<div class=\"scr\"><div class=\"col\"><div class=\"title\">" + E(tr::MATCH_OVER) + "</div>";
			const std::string mid = m.wins[0] + m.wins[1] > 0 ? "<div class=\"score\">" + std::to_string(m.wins[0]) + " \xE2\x80\x93 " + std::to_string(m.wins[1]) + "</div>"
			                                                 : std::string("<div class=\"vs\">VS</div>");
			r += "<div class=\"mini\"><div class=\"n\">" + flag(m.country[0]) + E(m.name[0]) + "</div>" + mid + "<div class=\"n\">" + E(m.name[1]) + flag(m.country[1]) + "</div></div>";
			r += "<div class=\"sub\">" + E(m.why) + " " + E(tr::fill(tr::WATCHED_FOR, { { "time", clock(m.seconds) } })) + "</div>";
			if (!problem->empty()) r += "<div class=\"problem\">" + E(*problem) + "</div>";
			r += "<div class=\"btns\">" + (again_i >= 0 ? "<div " + menu_row(again_i, sel, "btn") + ">" + E(tr::BUTTON_WATCH_AGAIN) + "</div>" : std::string()) + "<div " + menu_row(back, sel, "btn") + ">" + E(tr::BUTTON_BACK) + "</div></div>";
			r += "<div class=\"note\">" + E(tr::WATCH_AGAIN_NOTE) + "</div></div>";
			return r + foot(pad, { { "A", "Enter", tr::HINT_SELECT }, { "B", "Esc", tr::HINT_PLAY_SOLO } }) + "</div>";
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

	// ---- the room browser: the rooms, Quick match, Create room, by code or address ----
	browser_page(o, s, menu, changed, p, problem);
	return p;
}

void match_rows(Online &o, OnlineSettings &s, game_menu &menu, std::function<void()> changed, std::vector<menu_item> &rows)
{
	if (o.watching()) {   // (a match watched: stop, or play the winner next: a room's match, not a Quick match's, which has no room)
		rows.push_back({ tr::STOP_WATCHING, tr::STOP_WATCHING_HELP, {}, {}, [&o, &menu] { o.stop(); menu.close(); } });
		if (!o.watching_quick())
			rows.push_back({ tr::PLAY_NEXT, tr::PLAY_NEXT_HELP, {}, {}, [&o, &s, &menu] {
				const std::string c = o.watch_code();
				std::string err;
				if (!o.join_room(s, c, err)) o.tell_failure(err);   // (the watch stopped: why, on the Online page it goes back to)
				menu.close();
			} });
		return;
	}
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
