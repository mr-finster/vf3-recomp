// Rooms 2: a room the lobby owns (online_room.h; docs/ROOMS2-DESIGN.md). The membership above what Online does: entering
// the room, its socket to the lobby (or polling), its state, and what the game does about it: watching the room's
// match while in its line, a fresh session at the table for each new pair (the room's own link kept for all of them),
// the match's result told, READY and AWAY. Nothing here changes src/net: a seat is a room code's session as any other.
#include "online.h"
#include "online_room.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

namespace online_detail {   // (online.cpp's helpers)
std::string mask_list(const std::vector<std::string> &v);
bool lobby_local(const std::string &url);
tr::line lobby_answer(const vfnet::http::Reply &r, const std::string &code, const std::string &mine);
std::string hex(uint64_t v);
void log(const std::string &t);
bool lobby_key(const std::string &s, size_t n);
bool lobby_room_code(const std::string &s);
void goodbye(const std::string &url, const std::string &body);
}

namespace {

using online_detail::log;
using vfnet::json::Value;
using vfnet::json::quote;

// Why a seat's session was quit: the room's line moved (a new pair at the table). online.cpp's QUIT_NEXT_IN_LINE: a
// /v1 room's guest made way for the next in line; here the same, said by either side. An older build reads a plain quit.
constexpr uint8_t QUIT_NEXT_IN_LINE = 2;
constexpr double POLL_MS = 10000;        // (no socket: the lobby asked every 10 s; a member not heard from for 30 s is gone)
constexpr double POLL_NEAR_MS = 4000;    // (the same, this player at the table, next or called, or an op not seen taken: GET READY seen in time)
constexpr double PING_MS = 15000;        // (the socket pinged: the lobby closes one quiet for 90 s)
constexpr double WS_POLL_MS = 45000;     // (an open socket the lobby's said nothing on this long: asked (a poll on it, answered with a state))
constexpr double WS_DEAD_MS = 2 * WS_POLL_MS;   // (nothing on it this long, the poll's answer neither: dead, closed, polling meanwhile)
constexpr double CONNECT_GIVEUP_MS = 30000;
constexpr double WIN_SEQUENCE_MS = 25000;
constexpr bool AUTO_AWAY = false;   // (reno, 26 Nov: no AWAY but the player's own (the menu's SKIP THIS TURN))   // (the arcade's end after a decision: the KO, the replay, the win poses, ~8-12 s)   // (a seated session not going while the match is on: disconnect told then)
constexpr double PLAYON_AWAY_MS = 20000;   // (a play-on's loser: their rematch's Start not pressed in this long, AWAY)
constexpr double BANNER_MS = 4000;       // (a moment's banner over the game)
constexpr double REJOIN_MS = 12 * 3600e3; // (a membership kept in room2.txt asked back this long after it was written (when it was made):
                                         // the lobby takes it back only if it still has it (its 30 s), else it's a new member)
constexpr double WS_AGAIN_MS = 2000;     // (the socket closed after it was open: again in a moment, 5 times)
constexpr double WS_LATER_MS = 15000;    // (refused or failed: polling meanwhile, the socket tried again after 15 s, 30, 60 ... 5 min)
constexpr double WS_STEADY_MS = 60000;   // (open this long: its tries counted afresh)
constexpr int OP_TRIES = 5;              // (an op not seen taken: sent this many times in all, 3 s, 6, 12, 24 apart: never past the lobby's
                                         // rate for a member, a burst of 10 then one per 2 s, with the polls)
constexpr double OP_AGAIN_MS = 3000;
constexpr double ROOM2_NO_RELAY_MS = 180000;   // (the room's link with no relay: gathered again this long after, between seats)
constexpr double STUCK_MS = 5000;        // (seated, the table PLAYING, no session: this long (the state that moves on may be coming), then disconnect told)
constexpr double STUCK_AGAIN_MS = 30000; // (the same seat stuck again (the lobby played on with it): told again only after this)

bool truthy(const Value &v) { return v.kind == Value::boolean && v.b; }
int num(const Value &v, int lo, int hi, int none = 0) { return v.kind == Value::number ? int(std::clamp(v.n, double(lo), double(hi))) : none; }
RoomMember::Who who_of(const Value &v) { return { online_menu::clean_name(v["name"].str()), v["country"].str().size() == 2 ? v["country"].str() : "", !(v["ready"].kind == Value::boolean && !v["ready"].b) }; }
double wall_ms() { return double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

// The room page made again only when something it shows has changed (its version counted up): a state that changes
// none of it (a poll's answer, the same again) leaves the page as it is, its selection with it
void bump_shown(RoomMember &m)
{
	std::string k = m.room_name + "|" + (m.owner_key.empty() ? "" : "o") + (m.listed ? "l" : "") + "|" + m.wid + "|";
	auto who = [&](const RoomMember::Who &w) { k += w.name + "/" + w.country + (w.ready ? "/r|" : "/a|"); };
	if (m.has_a) who(m.a); else k += "-|";
	if (m.has_b) who(m.b); else k += "-|";
	k += m.holder + "|" + std::to_string(m.streak) + "|" + std::to_string(m.pos) + (m.watcher ? "w" : "") + (m.me_ready ? "R" : "A") + "|" +
	     std::to_string(m.watchers) + "|" + (m.ready_on ? std::to_string(m.ready_pos) + (m.ready_you ? "y" : "") : "-") + "|";
	for (const RoomMember::Who &w : m.line) who(w);
	if (k != m.shown_key) { m.shown_key = k; m.version++; }
}

} // namespace

// The room page's line: the rows shown of a line of n, this player at pos (1.., else none), at most rows of them: each
// an index into the line, or -k for a row that says "... k more". All of a line that fits; past that the first ones
// and this player's own row, "... k more" between and after (a row that would only say 1 more: that one shown instead)
std::vector<int> room2_line_rows(int n, int pos, int rows)
{
	std::vector<int> v;
	if (pos > n) pos = 0;   // (a place past the line's end: none, never a row past it)
	if (n <= rows) { for (int i = 0; i < n; i++) v.push_back(i); return v; }
	if (pos <= 0 || pos <= rows - 1) {   // (this player among the first: those, then the rest as a count)
		for (int i = 0; i < rows - 1; i++) v.push_back(i);
		v.push_back(-(n - (rows - 1)));
		return v;
	}
	const int after = n - pos, top = rows - 2 - (after > 0 ? 1 : 0);   // (after this player's row: the last one, or a count)
	for (int i = 0; i < top; i++) v.push_back(i);
	v.push_back(-(pos - 1 - top));
	v.push_back(pos - 1);
	if (after == 1) v.push_back(n - 1);
	else if (after > 1) v.push_back(-after);
	return v;
}

std::string room2_streak_text(int n)
{
	if (n <= 0) return "";
	return n == 1 ? std::string(tr::ROOM2_STREAK_1) : tr::fill(tr::ROOM2_STREAK, { { "n", std::to_string(n) } });
}

std::string room2_wins_text(int n)
{
	if (n <= 0) return "";
	return n == 1 ? std::string(tr::ROOM2_STREAK_1) : tr::fill(tr::ROOM2_WINS_N, { { "n", std::to_string(n) } });
}

// The line's rows that fit a page this tall (the window's height in dp, 720 at least: game_menu::height_dp). screen.rcss:
// the room page from 30 dp under the top to 88 dp over the bottom (.r2room), a row 28 dp (.r2row); at 720 dp 15 rows,
// the head, "n watching" and the help or a note under them; past that each 28 dp more a row more, the rest kept as it is
int room2_line_fit(float page_h_dp)
{
	return ROOM2_LINE_ROWS + std::max(0, int((page_h_dp - 720.0f + 0.5f) / 28.0f));
}

// ---- entering ----

bool Online::room2_create(const OnlineSettings &s, tr::line &error) { return room2_begin(s, "", true, error); }

bool Online::room2_enter(const OnlineSettings &s, const std::string &c, tr::line &error, bool next)
{
	const std::string want = online_menu::clean_code(c);
	if (want.size() != 6) { error = tr::CODE_BAD; return false; }
	if (!room2_begin(s, want, false, error)) return false;
	if (r2) r2->play_next = next;
	return true;
}

bool Online::room2_begin(const OnlineSettings &s, const std::string &code_in, bool create, tr::line &error)
{
	if (identity.game.empty()) { error = tr::ONLINE_NOT_READY; return false; }
	stop();
	last = {};
	failure.clear();
	fail_step = Step::none;
	note.clear();
	random = false;
	ticket.clear();
	country_peer.clear();
	peer_name.clear();
	tried = create ? Try::room2_create : Try::room2_enter;   // (Try again: the same, a Rooms 2 room's, by the same code)
	r2_tried = create ? "" : code_in;
	r2_fresh.reset();
	r2_rejoin.reset();
	r2_fresh_told = false;
	want_code.clear();
	patient_until = 0;
	started_with = s;
	auto_delay = s.delay < 0;
	auto_done = false;
	lobby = s.lobby.empty() ? lobby_default() : s.lobby;
	if (lobby.empty()) { error = tr::NO_LOBBY; return false; }
	if (lobby.rfind("https://", 0) != 0 && !online_detail::lobby_local(lobby)) { error = tr::LOBBY_NOT_HTTPS; return false; }
	auto m = std::make_unique<RoomMember>();
	m->creating = create;
	m->code = code_in;
	m->link = std::make_unique<vfnet::RoomLink>();
	m->link->allow_loopback(online_detail::lobby_local(lobby));   // (games on one machine: a test)
	if (std::string e; !m->link->open(0, e)) { error = e; return false; }
	m->link->set_relay_only(s.hide_ip);
	identity.name = s.name;
	r2 = std::move(m);
	if (create) {   // (the rules it's made with, as the room's till the lobby's word: Room settings shows them, never this player's saved ones)
		r2->has_rules = true;
		r2->rule_change = s.rule_change;
		r2->rule_side = s.rule_side; r2->rule_dural = s.rule_dural; r2->rule_stage = s.rule_stage; r2->rule_max = s.rule_max; r2->rule_custom = false;
	}
	r2->step = 0;
	code = code_in;
	r2_join.reset();
	r2_asks.clear();
	r2_owner.clear();
	auto j = std::make_shared<Job>();
	r2_gather = j;
	std::thread([j, work = gather(lobby, turn_json())] { work(*j); j->done = true; }).detach();
	log(std::string("Rooms 2: ") + (create ? "creating a room" : "entering room " + room_code()) + ": getting ready (" + identity.build + ")");
	return true;
}

// The member's call's body: its code, id and key, and anything else
std::string Online::room2_body(const std::string &extra) const
{
	return "{\"code\":" + quote(r2->code) + ",\"mid\":" + quote(r2->mid) + ",\"mkey\":" + quote(r2->mkey) + extra + "}";
}

// room2.txt: "code mid mkey owner_key wall-ms", the last room's, for a game that comes back within the lobby's 30 s.
// Written when what it says changes (entering, a membership taken anew), never every few seconds, so never mid-match
void Online::room2_keep()
{
	if (r2_file.empty() || !r2) return;
	const std::string k = r2->code + " " + r2->mid + " " + r2->mkey + " " + (r2->owner_key.empty() ? "-" : r2->owner_key);
	if (k == r2->kept) return;
	if (FILE *f = fopen(r2_file.c_str(), "w")) {
		fprintf(f, "%s %.0f\n", k.c_str(), wall_ms());
		fclose(f);
		r2->kept = k;
	}
}

void Online::room2_joined(const vfnet::http::Reply &r)
{
	RoomMember &m = *r2;
	Value v;
	const bool parsed = vfnet::json::parse(r.body, v);
	const std::string e = parsed ? v["error"].str() : "";
	// (no Rooms 2 at this lobby: an older one (404, its own "bad"), or one with them switched off ("off"): a /v1 room,
	// as every build before; a /v1 room's code: joined as one, Enter code's way)
	const bool none_here = r.status == 404 && (e == "off" || e == "bad" || !parsed);
	if ((m.creating && (none_here || r.status == 400)) || (!m.creating && (none_here || (r.status == 412 && e == "room_version")))) {
		const bool create = m.creating;
		const std::string c = m.code;
		const OnlineSettings s = started_with;
		log(create ? "Rooms 2: not at this lobby (" + std::to_string(r.status) + (e.empty() ? "" : " " + e) + "): a room as before"
		           : "Rooms 2: " + room_code() + (e == "room_version" ? " is a room as before: joining it that way" : " not a Rooms 2 room here: as Enter code"));
		r2.reset();
		tr::line err;
		const bool next = m.play_next;
		const bool ok = create ? host_room(s, err) : next ? join_room(s, c, err) : enter_code(s, c, err);   // (Play next from a room as before's match: its line)
		if (!ok) { failure = err; notices.push_back({ Notice::refused, err }); }
		return;
	}
	if (r.status != 200 || !parsed || !online_detail::lobby_key(v["mid"].str(), 0) || !online_detail::lobby_key(v["mkey"].str(), 32) || !online_detail::lobby_room_code(v["code"].str())) {
		tr::line why = m.creating ? tr::line(tr::COULDNT_OPEN_ROOM_WHY, { { "why", online_detail::lobby_answer(r, room_code(), identity.build) } })
		                          : online_detail::lobby_answer(r, room_code(), identity.build);
		if (!m.creating && e == "full") why = tr::line(tr::ROOM2_ROOM_FULL);   // (the room at its players' limit: not a /v1 room's "someone's joining")
		if (m.rejoining && r.status == 412 && e == "version") {   // (taken back at its table: the version is the opponent's, not a host's)
			std::string theirs;
			for (char ch : v["host_version"].str())
				if (isalnum(uint8_t(ch)) || ch == '.' || ch == '-' || ch == '+' || ch == '_') theirs += ch;
			if (!theirs.empty()) why = tr::line(tr::ROOM2_OPPONENT_VERSION, { { "theirs", theirs.substr(0, 40) }, { "mine", identity.build } });
		}
		log("Rooms 2: " + why.en());
		r2.reset();
		code.clear();
		failure = why;
		fail_step = Step::finding;
		notices.push_back({ Notice::refused, why });
		return;
	}
	m.code = v["code"].str();
	code = m.code;
	m.mid = v["mid"].str();
	m.mkey = v["mkey"].str();
	if (online_detail::lobby_key(v["owner_key"].str(), 32)) m.owner_key = v["owner_key"].str();
	m.wid = online_detail::lobby_room_code(v["wid"].str()) ? v["wid"].str() : "";
	m.back = truthy(v["back"]);
	m.step = 2;
	m.polled_at = now;   // (the socket asked for first: polled only if it doesn't open)
	m.poll_at = now + POLL_MS;
	room2_keep();
	log("Rooms 2: " + std::string(m.creating ? "created room " : m.back ? "back in room " : "in room ") + room_code() + " (member " + m.mid + ")" +
	    (m.back ? ": its place kept from before" : "") + (m.wid.empty() ? ", its matches not watchable" : ""));
	room2_apply(v["state"], r2_join_sent);
}

// ---- the lobby's word ----

// A state from the lobby, applied unless it's older than the one applied last: by the lobby's seq if it sends one, else by
// when it was known (known_at: a socket's push as it came, a POST's answer as its request was sent) and the table's match
// number, which only grows. (A poll's answer that took longer than an op's sent after it, or a push meanwhile, would put
// back a state that's gone: a seat unseated, READY undone.) A state that says the room's closed ends the membership
bool Online::room2_apply(const Value &st, double known_at)
{
	if (!r2 || st.kind != Value::object) return false;
	RoomMember &m = *r2;
	if (truthy(st["closed"])) { room2_end("closed"); return false; }
	const int n = st["table"].kind == Value::object ? num(st["table"]["n"], 0, 1 << 30, -1) : -1;
	// (a table whose match number grew is newer whatever else says: a lobby's seq or a clock never freezes this player on
	// an old view)
	const bool grew = n >= 0 && n > m.state_n;
	if (st["seq"].kind == Value::number) {
		if (st["seq"].n < m.state_seq && !grew) { log("Rooms 2: a state older than the last (seq " + std::to_string(int64_t(st["seq"].n)) + "): not applied"); return false; }
		m.state_seq = st["seq"].n;
	} else if (!grew && (known_at < m.state_at || (n >= 0 && n < m.state_n))) {
		log("Rooms 2: a state older than the last (match " + std::to_string(n) + ", " + std::to_string(int(m.state_at - known_at)) + " ms): not applied");
		return false;
	}
	m.state_at = grew ? known_at : std::max(m.state_at, known_at);
	m.state_n = std::max(m.state_n, n);
	room2_state(st);
	return true;
}

void Online::room2_state(const Value &st)
{
	if (!r2 || st.kind != Value::object) return;
	RoomMember &m = *r2;
	const Value &rm = st["room"], &t = st["table"], &you = st["you"], &rd = st["ready"];
	m.room_name = online_menu::clean_room(rm["name"].str());
	m.listed = truthy(rm["listed"]);
	m.locked = truthy(rm["locked"]);
	m.owner = truthy(rm["owner"]);
	m.ready_ms = num(rm["ready_ms"], 1000, 120000, 15000);
	if ((m.has_rules = rm["rules"].kind == Value::object)) {
		const Value &ru = rm["rules"];
		m.rule_side = truthy(ru["side"]);
		m.rule_change = truthy(ru["change"]);
		m.rule_dural = truthy(ru["dural"]);
		m.rule_custom = truthy(ru["custom"]);
		m.rule_stage = ru["stage"].kind != Value::boolean || ru["stage"].b;
		m.rule_max = num(ru["max"], 0, 99, 0);
	}
	m.has_a = t["a"].kind == Value::object;
	m.has_b = t["b"].kind == Value::object;
	m.a = m.has_a ? who_of(t["a"]) : RoomMember::Who{};
	m.b = m.has_b ? who_of(t["b"]) : RoomMember::Who{};
	m.phase = t["phase"].str();
	m.streak = num(t["streak"], 0, 9999);
	m.holder = t["holder"].str();
	m.n = num(t["n"], 0, 1 << 30);
	const Value &last_v = t["last"];
	const int last_n = last_v.kind == Value::object ? num(last_v["n"], 0, 1 << 30, -1) : -1;
	m.last_result = last_v["result"].str();
	m.last_why = last_v["why"].str();
	m.last_winner = last_v["winner"].str();
	m.line.clear();
	for (const Value &w : st["line"].items) m.line.push_back(who_of(w));
	const int was_pos = m.pos;
	m.pos = num(you["pos"], -1, 99, -1);
	const bool was_watcher = m.watcher;
	m.watcher = truthy(you["watcher"]);
	if ((m.watcher && !was_watcher) || (was_pos == 0 && m.pos > 0)) m.want_watch = true;   // (hotfix 5: a watcher, or one who just played, watches on)
	m.watchers = num(st["watchers"], 0, 99);
	const Value &mt = t["match"];
	m.match_n = mt.kind == Value::object ? num(mt["n"], 0, 1 << 30, -1) : -1;
	m.match_streak = mt.kind == Value::object ? num(mt["streak"], 0, 9999) : 0;
	m.match_holder = mt.kind == Value::object ? online_menu::clean_name(mt["holder"].str()) : "";
	// (its holder's side: the table's holder while that match is on (CONNECTING to DECIDED: the holder changes only with its
	// result), kept for it after; a state come after the result (entering then, a poll's): the side whose name the
	// lobby gives the match's holder, if just one has it (the lobby's names, compared only with its own))
	if (m.match_n < 0) { m.match_side.clear(); m.match_side_n = -1; }
	else if (m.match_n == m.n && (m.phase == "CONNECTING" || m.phase == "PLAYING" || m.phase == "DECIDED")) {
		m.match_side = m.holder == "a" || m.holder == "b" ? m.holder : "";
		m.match_side_n = m.match_n;
	} else if (m.match_side_n != m.match_n) {
		const bool a = m.has_a && !m.match_holder.empty() && m.a.name == m.match_holder, b = m.has_b && !m.match_holder.empty() && m.b.name == m.match_holder;
		m.match_side = a != b ? (a ? "a" : "b") : "";
		m.match_side_n = m.match_n;
	}
	// (a new seat: CONNECTING with a new match number, or another two at the table than last time)
	if (m.has_a && m.has_b) {
		const std::string pair = m.a.name + "|" + m.b.name;
		if ((m.phase == "CONNECTING" && m.n != m.epoch_n) || pair != m.epoch_pair) {
			m.epoch++;
			if (m.phase == "CONNECTING") m.epoch_n = m.n;
			m.epoch_pair = pair;
			// (watching, its start come since the state before: a polled state late, the new seat's match on screen already)
			if (ph == Phase::watching && m.watch_started_at > m.applied_at) m.watch_epoch = m.epoch;
		}
	}
	m.applied_at = now;
	m.side = you["side"].str();
	m.misses = num(you["misses"], 0, 9);
	const bool had_state = m.have_state;   // (the first state, entering or back after a crash: AWAY as it was, not set by the lobby now)
	m.lobby_ready = !(you["ready"].kind == Value::boolean && !you["ready"].b);
	m.me_ready = m.lobby_ready;
	for (const RoomMember::Op &o : m.outbox)   // (READY or AWAY asked, not seen taken yet: shown as asked meanwhile)
		if (o.op == "ready" || o.op == "away") m.me_ready = o.op == "ready";
	const Value &seat = you["seat"];
	m.seated = seat.kind == Value::object && !seat["token"].str().empty();
	if (m.seated) {
		m.seat.role = seat["role"].str();
		m.seat.side = seat["side"].str();
		m.seat.token = seat["token"].str();
		m.seat.as = seat["as"].str();
		m.seat.them = seat["them"].str();
		m.seat.wkey = online_detail::lobby_key(seat["wkey"].str(), 32) ? seat["wkey"].str() : "";
		m.seat.peer = who_of(seat["peer"]);
		m.seat.cands = seat["peer"]["cands"].strings();
		m.seat.relay = seat["peer"]["relay"].str();
		m.seat.n = num(seat["n"], 0, 1 << 30);
	}
	if (m.settling && (!m.seated || m.seat.token != m.failed_token)) m.settling = false;   // (the failed seat over: back in the line, or another)
	const bool was_on = m.ready_on && m.ready_you;
	m.ready_on = rd.kind == Value::object;
	m.ready_you = m.ready_on && truthy(rd["you"]);
	m.ready_pos = m.ready_on ? num(rd["pos"], 0, 99) : 0;
	if (m.ready_on) m.ready_until = now + num(rd["deadline_ms"], 0, 600000);
	if (st["watch"].kind == Value::string) m.wid = online_detail::lobby_room_code(st["watch"].str()) ? st["watch"].str() : "";
	m.have_state = true;
	bump_shown(m);
	// (the log: the table and the line as they change, what a test reads)
	std::string line;
	for (const auto &w : m.line) line += (line.empty() ? "" : ",") + w.name + (w.ready ? "" : "*");
	char b[400];
	snprintf(b, sizeof b, "Rooms 2: room %s: %s %s vs %s, streak %d (%s), match %d; line [%s]; this player %s%s", room_code().c_str(), m.phase.c_str(),
	         m.has_a ? m.a.name.c_str() : "-", m.has_b ? m.b.name.c_str() : "-", m.streak, m.holder.empty() ? "-" : m.holder.c_str(), m.n, line.c_str(),
	         m.pos == 0 ? ("at the table, side " + m.side).c_str() : m.watcher ? "watching, not in line" : ("in line, " + std::to_string(m.pos) + " of " + std::to_string(m.line.size())).c_str(),
	         m.ready_on ? (m.ready_you ? ", getting ready" : (", " + std::to_string(m.ready_pos) + " in line getting ready").c_str()) : "");
	if (!m.me_ready) snprintf(b + strlen(b), sizeof b - strlen(b), " (AWAY)");
	static std::string said;
	if (said != b) { log(b); said = b; }
	const bool new_last = last_n >= 0 && last_n != m.last_n;
	if (new_last) {
		m.last_n = last_n;
		log("Rooms 2: match " + std::to_string(last_n) + " ended: " + m.last_result + (m.last_why.empty() ? "" : " (" + m.last_why + ")") + (m.last_winner.empty() ? "" : ", side " + m.last_winner));
		// (the other left mid-match: a forfeit said to the one who stays)
		if (m.last_result == "forfeit" && m.pos == 0 && !m.seen_peer.empty()) { m.banner = 5; m.banner_name = m.seen_peer; m.banner_until = now + BANNER_MS; }
	}
	// AWAY by the lobby itself (seated, its session never started: the match void, why "connect", this player out of the
	// seat and AWAY): said, with how to play again. Only then: never for a READY asked that the lobby hadn't taken yet
	(void)was_on;
	if (had_state && new_last && was_pos == 0 && m.pos != 0 && m.last_result == "void" && m.last_why == "connect" && !m.lobby_ready && !m.test) {
		log("Rooms 2: the lobby set this player AWAY (its session didn't start in time)");
		notices.push_back({ Notice::tip, tr::line(tr::ROOM2_SET_AWAY) });
	}
	// a seat's watch key, come after its seat (the lobby arms the Watch first): this side's match sent from now
	if (m.in_seat && m.seated && m.seat.token == m.played_token && is_host && wkey.empty() && !m.seat.wkey.empty()) {
		wkey = m.seat.wkey;
		wid = m.wid;
		if (ph == Phase::playing && session && !feed.on()) feed_begin();
	}
	room2_react();
}

void Online::room2_end(const std::string &why)
{
	if (!r2) return;
	room_turns++;
	const tr::line said = why == "ready" ? tr::line(tr::ROOM2_REMOVED_READY) : why == "version" ? tr::line(tr::ROOM2_NEEDS_NEWER)
	                    : why == "closed" ? tr::line(tr::ROOM2_CLOSED) : why == "gone" ? tr::line(tr::ROOM2_REMOVED_GONE) : tr::line(tr::ROOM2_LEFT);
	log("Rooms 2: out of room " + room_code() + ": " + why + " (" + said.en() + ")");
	if (session && ph == Phase::playing) ended(said);   // (a match on: its end said as what happened, not "You left the match")
	stop_activity(true);
	if (r2 && r2->ws) std::thread([w = std::move(r2->ws)] {}).detach();
	r2.reset();
	r2_asks.clear();
	r2_owner.clear();
	r2_join.reset();
	r2_gather.reset();
	r2_fresh.reset();
	r2_rejoin.reset();
	r2_fresh_told = false;
	last = {};   // (a seat's match isn't one to Play again: that's a room as before's, by this room's code (a new room, or "another version"))
	if (!r2_file.empty()) remove(r2_file.c_str());
	// (not a try that failed: no "Couldn't connect" page with Try again (a closed room's code, or one this player was
	// removed from), just what happened, said over whatever's on; the Online page goes back to the rooms)
	failure.clear();
	fail_step = Step::none;
	notices.push_back({ Notice::left_room, said });
}

// An op to the lobby: on the socket if it's open, else POSTed (its answer a state). keep: in the outbox till a state shows it
// taken (n: the match it's for), sent again meanwhile (room2_ops); READY and AWAY replace each other there
void Online::room2_send(const std::string &op, const std::string &extra, bool keep, int n)
{
	if (!r2 || r2->mid.empty()) return;
	RoomMember &m = *r2;
	if (keep) {
		const bool wish = op == "ready" || op == "away";
		m.outbox.erase(std::remove_if(m.outbox.begin(), m.outbox.end(), [&](const RoomMember::Op &o) {
			return o.op == op || (wish && (o.op == "ready" || o.op == "away")) || (op == "join_line" && o.op == "leave_line") || (op == "leave_line" && o.op == "join_line");
		}), m.outbox.end());
		RoomMember::Op o;
		o.op = op;
		o.extra = extra;
		o.n = n;
		o.tries = 1;
		o.at = now + OP_AGAIN_MS;
		o.connecting = m.phase == "CONNECTING";
		m.outbox.push_back(o);
	}
	if (m.ws && m.ws->state() == vfnet::http::Socket::State::open) {
		m.ws->send("{\"op\":" + quote(op) + extra + "}", false);
		return;
	}
	r2_asks.push_back({ op, fetch("POST", lobby_url("/v2/room/" + op), room2_body(extra), 8000, 64 << 10), now });
}

// The lobby's no to an op (its error, on the socket or in a POST's answer): busy (over the member's rate) sent again 3 s
// later at the soonest; not_now (a started after the seat's over), stale (another match's n), already, bad: not again
void Online::room2_refused(const std::string &op, const std::string &e)
{
	RoomMember &m = *r2;
	for (size_t i = 0; i < m.outbox.size();) {
		if (m.outbox[i].op != op) { i++; continue; }
		if (e == "busy") { m.outbox[i].at = std::max(m.outbox[i].at, now + OP_AGAIN_MS); i++; }
		else m.outbox.erase(m.outbox.begin() + long(i));
	}
}

// The ops not seen taken yet: each one done with once the state shows it (or the lobby said no to it), else sent again
// 3 s, 6, 12, 24 after (one POST of it on its way at a time; busy, 429, is only later), given up after OP_TRIES: the
// lobby's own deadlines then
void Online::room2_ops()
{
	RoomMember &m = *r2;
	for (size_t i = 0; i < m.outbox.size();) {
		RoomMember::Op &o = m.outbox[i];
		bool done = !m.have_state;
		if (o.op == "started")   // (CONNECTING: till both said it, PLAYING then; first said in PLAYING (a session started again after a lost one,
			                     // or a rematch): 3 times, nothing in the state says it was taken)
			done = done || !m.in_seat || m.n != o.n || (m.phase != "CONNECTING" && m.phase != "PLAYING") || (m.phase == "PLAYING" && (o.connecting || o.tries >= 3));
		else if (o.op == "result") done = done || m.n != o.n || (m.phase != "PLAYING" && m.phase != "DECIDED");   // (decided: the table's moved on)
		else if (o.op == "ready") done = done || m.watcher || (o.now ? !m.ready_you : m.lobby_ready);   // (READY NOW: till seated, the call over; READY: ON: till READY)
		else if (o.op == "away") done = done || m.watcher || !m.lobby_ready;
		else if (o.op == "join_line") done = done || !m.watcher;
		else if (o.op == "leave_line") done = done || m.watcher;
		else done = true;
		if (done) { m.outbox.erase(m.outbox.begin() + long(i)); continue; }
		bool on_way = false;   // (a POST of it still on its way: not sent again on top of it)
		for (const R2Ask &a : r2_asks) if (a.op == o.op && !a.f->done) on_way = true;
		if (now >= o.at && !on_way) {
			if (o.tries >= OP_TRIES) {
				log("Rooms 2: " + o.op + " not seen taken after " + std::to_string(o.tries) + " tries: given up (the lobby's word stands)");
				m.outbox.erase(m.outbox.begin() + long(i));
				bump_shown(m);
				continue;
			}
			o.at = now + OP_AGAIN_MS * double(1 << o.tries);
			o.tries++;
			log("Rooms 2: " + o.op + " sent again (" + std::to_string(o.tries) + ")");
			const std::string op = o.op, extra = o.extra;
			room2_send(op, extra);
		}
		i++;
	}
	if (m.have_state) {   // (READY or AWAY as the lobby has it once nothing's asked)
		bool r = m.lobby_ready;
		for (const RoomMember::Op &o : m.outbox) if (o.op == "ready" || o.op == "away") r = o.op == "ready";
		if (r != m.me_ready) { m.me_ready = r; bump_shown(m); }
	}
}

// ---- each pass ----

void Online::room2_poll()
{
	RoomMember &m = *r2;
	if (m.link && !m.in_seat) {   // (no session reading the room's link: its housekeeping, the relay kept)
		std::vector<uint8_t> p;
		while (m.link->recv(p)) {}
	}
	if (m.step == 0) {   // getting ready: the link's addresses (STUN, the relay's)
		if (r2_gather && r2_gather->done) {
			const std::shared_ptr<Job> j = r2_gather;
			r2_gather.reset();
			if (j->reply.status == 0) {
				const tr::line why = online_detail::lobby_answer(j->reply, room_code(), identity.build);
				log("Rooms 2: the lobby wasn't reached (" + j->reply.error + ")");
				r2.reset();
				failure = why;
				notices.push_back({ Notice::refused, why });
				return;
			}
			if (!j->stun.empty()) m.link->use_stun(j->stun);
			if (!j->turn.empty()) m.link->use_turn(j->turn, j->user, j->password);
			m.turn_given = !j->turn.empty();
			country_me = country(j->country);
		}
		if (!r2_gather && m.link->gathered()) {
			if (started_with.hide_ip && m.link->relay_address().empty()) {
				log("Rooms 2: Hide my IP: the relay didn't answer");
				r2.reset();
				failure = tr::HIDE_IP_NO_RELAY;
				notices.push_back({ Notice::refused, failure });
				return;
			}
			m.gathered_at = now;
			log("Rooms 2: this side: " + online_detail::mask_list(m.link->candidates()) + ", relay " + (m.link->relay_address().empty() ? "none" : "yes"));
			// (me_json's addresses are the room link's: the membership's link lent for it)
			std::swap(room, m.link);
			const OnlineSettings &w = started_with;
			std::string extra = ",\"caps\":[]";
			if (m.creating) {
				extra += std::string(",\"listed\":") + (w.room_who >= 1 ? "true" : "false") + (w.room_name.empty() ? "" : ",\"room\":" + quote(w.room_name));
				if (!release.empty()) extra += ",\"build\":" + quote(release);   // (an older build given this room's code is told "The host has <it>")
				extra += std::string(",\"rules\":{\"side\":") + (w.rule_side ? "true" : "false") + ",\"dural\":" + (w.rule_dural ? "true" : "false") +
				         ",\"custom\":false,\"max\":" + std::to_string(w.rule_max) + ",\"stage\":" + (w.rule_stage ? "true" : "false") + ",\"change\":" + (w.rule_change ? "true" : "false") + "}";
			} else {
				extra += ",\"code\":" + quote(m.code);
				// (a membership from before, this room's: a crash or a restart, its place taken back if it's still there)
				if (!r2_file.empty())
					if (FILE *f = fopen(r2_file.c_str(), "r")) {
						char c[16] = "", mid[40] = "", mkey[80] = "", ok[80] = "";
						double at = 0;
						if (fscanf(f, "%15s %39s %79s %79s %lf", c, mid, mkey, ok, &at) == 5 && m.code == c && wall_ms() - at < REJOIN_MS &&
						    online_detail::lobby_key(mid, 0) && online_detail::lobby_key(mkey, 32)) {
							extra += ",\"mid\":" + quote(mid) + ",\"mkey\":" + quote(mkey);
							m.rejoining = true;
							if (online_detail::lobby_key(ok, 32)) m.owner_key = ok;
							log("Rooms 2: a membership of this room from before (made " + std::to_string(int((wall_ms() - at) / 60000)) + " min ago): asking for it back");
						}
						fclose(f);
					}
			}
			const std::string body = me_json(extra);
			std::swap(room, m.link);
			m.step = 1;
			r2_join = fetch("POST", lobby_url(m.creating ? "/v2/room/create" : "/v2/room/enter"), body, 10000, 64 << 10);
		r2_join_sent = now;
		}
		return;
	}
	if (m.step == 1) {   // asked: create or enter
		if (!r2_join || !r2_join->done) return;
		const vfnet::http::Reply r = r2_join->reply;
		r2_join.reset();
		room2_joined(r);
		return;
	}
	// the socket, else polling: a socket closed after it was open tried again in a moment (5 times; open a minute, counted
	// afresh), one refused or failed again after 15 s, 30, 60 ... 5 min, polling meanwhile (never polling for good)
	using vfnet::http::Socket;
	if (!m.ws && now >= m.ws_at) {
		m.ws = std::make_shared<Socket>(lobby_url("/v2/room/ws?code=" + m.code + "&mid=" + m.mid + "&mkey=" + m.mkey));
		m.ws_first = true;
	}
	if (m.ws) {
		Socket::Message msg;
		while (r2 && m.ws && m.ws->take(msg)) {
			if (msg.data == "pong") continue;
			Value v;
			if (!vfnet::json::parse(msg.data, v)) continue;
			m.ws_heard_at = now;
			if (v["state"].kind == Value::object) {
				if (std::exchange(m.ws_first, false)) { m.state_seq = -1; m.state_at = -1e18; }   // (a new socket's first state: the lobby's view now, whatever came before)
				room2_apply(v["state"], now);
			}
			else if (v["removed"].kind == Value::string) { room2_end(v["removed"].str()); return; }
			else if (truthy(v["closed"])) { room2_end("closed"); return; }
			else if (v["error"].kind == Value::string) {
				const std::string e = v["error"].str(), op = v["op"].str();
				log("Rooms 2: the lobby said " + e + " to " + op);
				if (e == "no_room" || e == "off") { room2_end("closed"); return; }
				room2_refused(op, e);
			}
		}
		if (!r2) return;
		// (open but quiet: the lobby pushes a state at every change, and answers a poll on it; its pings' pongs never come
		// this far (src/net keeps them). Asked every WS_POLL_MS it's been quiet; nothing at all for WS_DEAD_MS, the socket's
		// dead though the network never said (a cut a router kept quiet): closed, polling, a new socket. A poll a minute is
		// well within the lobby's rate for a member, 10 at once then one per 2 s)
		bool dead = false;
		if (m.ws && m.ws->state() == Socket::State::open) {
			if (m.ws_open_at <= 0) { m.ws_open_at = now; m.ws_fails = 0; }   // (taken: the refusals in a row over)
			if (now - m.ws_open_at >= WS_STEADY_MS) m.ws_tries = m.ws_fails = 0;   // (a good while open: as new)
			const double heard = std::max(m.ws_heard_at, m.ws_open_at);
			if (now - heard >= WS_DEAD_MS) dead = true;
			else if (now - std::max(heard, m.ws_polled_at) >= WS_POLL_MS) {
				m.ws->send("{\"op\":\"poll\"}", false);
				m.ws_polled_at = now;
			}
			if (!dead && now >= m.ping_at) {
				m.ws->send("ping", false);
				m.ping_at = now + PING_MS;
			}
		}
		if (m.ws && (dead || m.ws->state() == Socket::State::closed)) {
			const int status = dead ? 101 : m.ws->status();
			const bool was_open = m.ws_open_at > 0;
			if (was_open && now - m.ws_open_at >= WS_STEADY_MS) m.ws_tries = m.ws_fails = 0;
			double again = 0;
			if (status == 101 && ++m.ws_tries <= 5) again = WS_AGAIN_MS;
			else again = std::min(300000.0, WS_LATER_MS * double(1 << std::min(m.ws_fails++, 5)));
			log("Rooms 2: the room's socket " + (dead ? "quiet for " + std::to_string(int((now - std::max(m.ws_heard_at, m.ws_open_at)) / 1000)) + " s, its poll unanswered: taken as closed"
			                                         : "closed (" + m.ws->error() + (status ? ", " + std::to_string(status) : "") + ")") +
			    ": polling, the socket again in " + std::to_string(int(again / 1000)) + " s");
			std::thread([w = std::move(m.ws)] {}).detach();
			m.ws.reset();
			m.ws_open_at = 0;
			m.ws_at = now + again;
			m.poll_at = std::min(m.poll_at, now);
		}
	}
	if (OwnerAsks::Ask a; r2_owner.due(now, a)) room2_owner_send(a.op, a.body, a.tries + 1);   // (the creator's settings or rules not taken: again, one a pass)
	const bool socket_open = m.ws && m.ws->state() == vfnet::http::Socket::State::open;
	// (no socket: asked every 10 s, the lobby's word that this member's still here; every 3 s at the table, next to play,
	// called, or an op not seen taken: GET READY, a new seat and the line's moves seen in time. One poll on its way at most)
	const bool near = m.pos == 0 || m.ready_you || m.ready_place() == 1 || !m.outbox.empty();
	bool polling = false;
	for (const R2Ask &a : r2_asks) if (a.op == "poll") polling = true;
	if (!socket_open && !polling && (now >= m.poll_at || now - m.polled_at >= (near ? POLL_NEAR_MS : POLL_MS))) {
		m.poll_at = now + POLL_MS;
		m.polled_at = now;
		r2_asks.push_back({ "poll", fetch("POST", lobby_url("/v2/room/poll"), room2_body(), 8000, 64 << 10), now });
	}
	for (size_t i = 0; i < r2_asks.size();) {
		if (!r2_asks[i].f->done) { i++; continue; }
		const std::string op = r2_asks[i].op;
		const double sent = r2_asks[i].sent;
		const vfnet::http::Reply r = r2_asks[i].f->reply;
		const R2Ask was = r2_asks[i];
		r2_asks.erase(r2_asks.begin() + long(i));
		Value v;
		const bool parsed = vfnet::json::parse(r.body, v);
		if ((op == "settings" || op == "rules") && !(r.status == 200 && parsed && !(v["ok"].kind == Value::boolean && !v["ok"].b)) && r.status != 410 &&
		    !(r.status == 404 && parsed)) {   // (the creator's change not taken: again later if it's still the newest of its kind)
			const bool again = r.status != 403 && r2_owner.failed(op, was.body, was.seq, was.tries, now);
			log("Rooms 2: the room's " + op + " not taken (" + (r.status ? "the lobby said " + std::to_string(r.status) : r.error) + "): " +
			    (again ? "sent again in " + std::to_string(int(OwnerAsks::AGAIN_MS / 1000) << (was.tries - 1)) + " s" : was.seq != r2_owner.newest_of(op) ? "a newer one went after it" : "given up"));
			continue;
		}
		if (r.status == 410 && parsed) { room2_end(v["why"].str()); return; }
		// (the room gone, closed, or Rooms 2 switched off at the lobby (its "off"); not a member any more: the lobby's
		// no_room or not_member): the membership over, said
		if (r.status == 404 && parsed && (v["error"].str() == "no_room" || v["error"].str() == "off" || v["error"].str() == "not_member")) {
			room2_end(v["error"].str() == "not_member" ? "gone" : "closed");
			return;
		}
		if (r.status == 200 && parsed) {
			if (truthy(v["closed"])) { room2_end("closed"); return; }
			if (v["state"].kind == Value::object) room2_apply(v["state"], sent);
			if (!r2) return;
			if (v["ok"].kind == Value::boolean && !v["ok"].b) {
				log("Rooms 2: the lobby said " + v["error"].str() + " to " + op);
				room2_refused(op, v["error"].str());
			} else if (op == "result" || op == "started")   // (taken: its answer says so, whatever the state's age)
				m.outbox.erase(std::remove_if(m.outbox.begin(), m.outbox.end(), [&](const RoomMember::Op &o) { return o.op == op; }), m.outbox.end());
		} else if (r.status != 200)   // (busy, 429, or not reached: the op sent again later, the poll at its time; never said to the player)
			log("Rooms 2: " + op + ": " + (r.status == 429 ? std::string("the lobby's busy: later") : r.status ? "the lobby said " + std::to_string(r.status) : r.error));
	}
	room2_ops();
	room2_regather();
	if (!r2) return;
	room2_react();
	if (!r2) return;
	// a seat's match: connected (started said, again while the lobby has it CONNECTING), decided (its result). A play-on
	// (the lobby's PLAYING n+1 after a match decided with nobody READY, the same session) said started only once its match
	// has begun: the out player back in, or their Start pressed. Till then the lobby, someone READY in line meanwhile,
	// lets the last result stand and calls them (GET READY: the loser's Start held then), never a table stuck in PLAYING
	const bool begun = !m.started_said || !(both_in && score.players() != 3) || rematch_coming();
	// (round 11: never for a new pair's match while this seat's session still plays the arcade's ending, its seat's token not
	// this session's: the new seat waits, room2_win_sequence)
	if (m.in_seat && ph == Phase::playing && session && m.started_n != m.n && begun && m.seat.token == m.played_token) {
		if (!m.started_said) log("Rooms 2: connected with " + opponent() + ": started");
		else log("Rooms 2: match " + std::to_string(m.n) + " begun (" + (score.players() == 3 ? std::string("both in") : "the out player's Start") +
		         ", in " + std::to_string(int(score.players())) + "): started");
		m.started_said = true;
		m.started_n = m.n;
		room2_send("started", ",\"n\":" + std::to_string(m.n), true, m.n);
	}
	if (m.in_seat) room2_decided();
	room2_shown();
	// tests: the ROOM2_READY hook's press (now: Start while getting ready; esc: AWAY the first time; away: AWAY once in)
	if (m.ready_you && !m.ready_sent && m.ready_auto_at > 0 && now >= m.ready_auto_at) {
		m.ready_auto_at = 0;
		log("Rooms 2: (test) " + std::string(room2_test_ready == "esc" ? "Esc" : "Start") + " pressed while getting ready");
		if (room2_test_ready == "esc") { room2_set_ready(false); room2_test_ready.clear(); } else room2_ready();   // (esc: once)
	}
	if (room2_test_ready == "away" && !m.test_away_done && m.have_state) { m.test_away_done = true; log("Rooms 2: (test) READY: off"); room2_set_ready(false); }
}

// The room's link gathered again (its STUN address and a new relay login: the relay's lasts the lobby's TURN TTL, 6 h;
// or a seat that didn't get going, the network perhaps changed) past room2_regather_ms, only between seats: in the line or
// watching, not next, not called. A new link, its addresses gathered, then told the lobby (enter again with this
// membership's id and key: its place kept), then it's the room's from the next seat (room2_seat). Never mid-match.
// A link with no relay (it didn't answer when gathered) is gathered again for one sooner, 3 min after, then 6, 12, 24 and
// every 24 min (each try a relay login from the lobby, counted against the address's day whether the relay answers this
// network or not), and between matches too: next in line (not called yet), or at the table between a decided match and
// the next seat (or alone at it), which in a room of 2 or 3 is nearly all the time
void Online::room2_regather()
{
	RoomMember &m = *r2;
	// (not between seats: READY or AWAY just asked and not seen taken (the lobby may seat this player any moment), READY in
	// the line with a seat free at the table (taken at once), or a start that failed and the lobby's word since not come)
	const bool asked = m.pending("ready") || m.pending("away") || m.me_ready != m.lobby_ready;
	const bool may_sit = !m.watcher && (m.me_ready || m.lobby_ready) && !(m.has_a && m.has_b);
	const bool between = !m.in_seat && m.pos != 0 && (m.pos > 0 || m.watcher) && !m.ready_you && m.ready_place() != 1 && !asked && !may_sit &&
	                     !m.settling && m.have_state && !m.test;
	const vfnet::RoomLink *now_link = m.link ? m.link.get() : m.in_seat ? room.get() : nullptr;   // (seated, the link's the session's)
	const bool no_relay = now_link && now_link->relay_address().empty() && m.turn_given;
	// (for a relay, between matches as well: next in line (its match not begun), or at the table, its match decided and this
	// side's result said, or no session on (alone, or a seat that didn't get going): never a match on)
	const bool quiet = !m.ready_you && !asked && m.have_state && !m.test;
	const bool next_up = !m.in_seat && m.pos > 0 && !m.watcher && m.ready_place() == 1 && !m.settling;
	const bool at_table = m.pos == 0 && (m.in_seat ? m.reported_n == m.n && (m.phase == "DECIDED" || m.phase == "READY")
	                                               : !(m.has_a && m.has_b) || m.seat.token == m.failed_token);
	const bool may = between || (no_relay && quiet && (next_up || at_table));
	if (!r2_fresh) {
		const double every = no_relay ? std::min(room2_regather_ms, ROOM2_NO_RELAY_MS) * double(1 << std::min(m.no_relay_tries, 3)) : room2_regather_ms;
		if (!may || now - m.gathered_at < every || now < m.regather_at || r2_gather) return;
		m.regather_at = now + 60000;   // (once a minute at most, whatever comes of it)
		auto l = std::make_unique<vfnet::RoomLink>();
		l->allow_loopback(online_detail::lobby_local(lobby));
		if (std::string e; !l->open(0, e)) { log("Rooms 2: the room's link not gathered again (" + e + ")"); return; }
		l->set_relay_only(started_with.hide_ip);
		r2_fresh = std::move(l);
		r2_fresh_told = false;
		auto j = std::make_shared<Job>();
		r2_gather = j;
		std::thread([j, work = gather(lobby, turn_json())] { work(*j); j->done = true; }).detach();
		if (no_relay) {   // (said once a try)
			m.no_relay_tries++;
			log("Rooms 2: the room's link has no relay: gathered again for one (try " + std::to_string(m.no_relay_tries) + ", " +
			    std::to_string(int((now - m.gathered_at) / 60000)) + " min since the last" + (m.in_seat ? ", at the table between matches" : next_up ? ", next in line" : "") +
			    "; if it has none, the next in " + std::to_string(int(std::min(room2_regather_ms, ROOM2_NO_RELAY_MS) * double(1 << std::min(m.no_relay_tries, 3)) / 60000)) + " min)");
		} else
			log("Rooms 2: the room's link gathered again (" + std::to_string(int((now - m.gathered_at) / 60000)) + " min since the last)");
		return;
	}
	std::vector<uint8_t> p;
	while (r2_fresh->recv(p)) {}   // (its housekeeping: the relay's login, kept alive)
	auto give_up = [&](const std::string &why) {
		log("Rooms 2: the room's link not gathered again (" + why + "): the one before kept, tried again in 10 min");
		r2_fresh.reset();
		r2_fresh_told = false;
		m.regather_at = now + 600000;
	};
	if (r2_gather) {
		if (!r2_gather->done) return;
		const std::shared_ptr<Job> j = r2_gather;
		r2_gather.reset();
		if (j->reply.status == 0) { give_up("the lobby wasn't reached"); return; }
		if (!j->stun.empty()) r2_fresh->use_stun(j->stun);
		if (!j->turn.empty()) r2_fresh->use_turn(j->turn, j->user, j->password);
		m.turn_given = !j->turn.empty();   // (no relay login this time (the address's day used up): no more tries for one soon)
	}
	if (r2_fresh_told) {   // (the lobby has it: the room's link at once if there's no seat on, else at the next seat)
		if (!m.in_seat && m.link) {
			m.link = std::move(r2_fresh);
			r2_fresh_told = false;
			m.gathered_at = now;
			log("Rooms 2: the room's link gathered again: in use");
			if (!m.link->relay_address().empty()) m.no_relay_tries = 0;   // (a relay at last: the next time without one from 3 min again)
		}
		return;
	}
	if (!r2_rejoin) {
		if (!r2_fresh->gathered()) return;
		if (started_with.hide_ip && r2_fresh->relay_address().empty()) { give_up("Hide my IP: the relay didn't answer"); return; }
		if (!may) return;   // (seated or called meanwhile: told once it's between seats again)
		std::swap(room, r2_fresh);   // (me_json's addresses are the room link's: the fresh one's lent for it)
		const std::string body = me_json(",\"caps\":[],\"code\":" + quote(m.code) + ",\"mid\":" + quote(m.mid) + ",\"mkey\":" + quote(m.mkey));
		std::swap(room, r2_fresh);
		r2_rejoin = fetch("POST", lobby_url("/v2/room/enter"), body, 10000, 64 << 10);
		r2_rejoin_sent = now;
		return;
	}
	if (!r2_rejoin->done) return;
	const vfnet::http::Reply r = r2_rejoin->reply;
	r2_rejoin.reset();
	Value v;
	const bool parsed = vfnet::json::parse(r.body, v);
	if (r.status == 410 && parsed) { room2_end(v["why"].str()); return; }
	if (r.status == 404 && parsed && (v["error"].str() == "no_room" || v["error"].str() == "off")) { room2_end("closed"); return; }
	if (r.status != 200 || !parsed || !online_detail::lobby_key(v["mid"].str(), 0) || !online_detail::lobby_key(v["mkey"].str(), 32)) {
		give_up(r.status ? "the lobby said " + std::to_string(r.status) : r.error);
		return;
	}
	if (v["mid"].str() != m.mid) {   // (this membership already gone at the lobby: the one its enter made taken as this player's, no ghost left in the line)
		log("Rooms 2: this membership was gone at the lobby: the new one (member " + v["mid"].str() + ", at the line's back) taken");
		m.mid = v["mid"].str();
		m.mkey = v["mkey"].str();
		m.outbox.clear();
		m.state_seq = -1;
		m.state_at = -1e18;
		if (m.ws) std::thread([w = std::move(m.ws)] {}).detach();
		m.ws.reset();
		m.ws_open_at = 0;
		m.ws_at = now;
		room2_keep();
	}
	r2_fresh_told = true;
	log("Rooms 2: the room's link gathered again: " + online_detail::mask_list(r2_fresh->candidates()) + ", relay " + (r2_fresh->relay_address().empty() ? "none" : "yes") + "; the lobby told");
	room2_apply(v["state"], r2_rejoin_sent);
	if (r2) room2_regather();   // (in use at once if between seats)
}

// What to do now, by the lobby's last word and what's on
void Online::room2_react()
{
	if (!r2 || r2->step != 2 || !r2->have_state || r2->test) return;
	RoomMember &m = *r2;
	// at the table with an opponent: their session, if it's not the one on (a new pair; seated again cold)
	if (m.pos == 0 && m.seated && m.seat.token != m.played_token && m.seat.token != m.failed_token) { if (!room2_win_sequence()) room2_seat(); return; }
	// (the session lost, the lobby's word since: the same two play on, its token kept (a lost match given to the holder,
	// nobody next): a fresh session with it, both sides alike)
	if (m.pos == 0 && m.seated && !m.in_seat && !m.retry_token.empty() && m.seat.token == m.retry_token && m.phase == "PLAYING" && m.n != m.reported_n) {
		log("Rooms 2: the same two play on (match " + std::to_string(m.n) + "): their session started again");
		m.retry_token.clear();
		m.played_token.clear();
		if (!room2_win_sequence()) room2_seat();
		return;
	}
	// (R-71) seated with a session that isn't running (connecting again, its link down) while the lobby has the match on or
	// decided and this side hasn't reported: after CONNECT_GIVEUP_MS, disconnect told and the session let go, never left
	// "connecting" for good (a 40 s drop: the other side said disconnect, this one sat connecting, the room in DECIDED)
	if (m.in_seat && session && session->state() != vfnet::Session::State::running && (m.phase == "PLAYING" || m.phase == "DECIDED") && m.reported_n != m.n) {
		if (m.connecting_since <= 0) m.connecting_since = now;
		else if (now - m.connecting_since >= CONNECT_GIVEUP_MS) {
			log("Rooms 2: the session not going for " + std::to_string(int(CONNECT_GIVEUP_MS / 1000)) + " s, the table " + m.phase + ": disconnect told");
			m.reported_n = m.n;
			m.connecting_since = 0;
			room2_send("result", ",\"winner\":\"disconnect\",\"n\":" + std::to_string(m.n), true, m.n);
			m.failed_token = m.seat.token;
			stop_activity(true);
			return;
		}
	} else m.connecting_since = 0;
	// seated, this seat's session played and none on now, the table PLAYING: a rematch's fresh start that never got going,
	// or a session the lobby hasn't heard ended. Never left seated with no session: disconnect told for the match (at
	// once for a start that failed, else after a moment for the state that moves on; the same seat stuck again told only
	// after a while, the lobby's own deadlines first)
	if (m.pos == 0 && m.seated && !m.in_seat && ph == Phase::off && m.phase == "PLAYING" && (m.seat.token == m.failed_token || m.seat.token == m.played_token) &&
	    m.retry_token != m.seat.token) {
		if (m.stuck_since <= 0) m.stuck_since = now;
		const bool again = m.stuck_token == m.seat.token;
		const double wait = again ? STUCK_AGAIN_MS : m.seat.token == m.failed_token ? 0 : STUCK_MS;
		if (m.stuck_n != m.n && m.reported_n != m.n && now - (again ? std::max(m.stuck_since, m.stuck_told) : m.stuck_since) >= wait) {
			log("Rooms 2: seated with no session, the table playing match " + std::to_string(m.n) + ": disconnect told");
			m.stuck_n = m.reported_n = m.n;
			m.stuck_token = m.seat.token;
			m.stuck_told = now;
			if (m.seat.token != m.failed_token) m.retry_token = m.seat.token;   // (a session that did play: the same two played on again fresh)
			room2_send("result", ",\"winner\":\"disconnect\",\"n\":" + std::to_string(m.n), true, m.n);
		}
	} else m.stuck_since = 0;
	// (a state older than the seat's start never unseats it: the lobby's match number only grows)
	const bool may_unseat = !m.in_seat || m.n >= m.seat_n;
	static int kept_said = -2;
	if (!may_unseat && kept_said != m.n) { kept_said = m.n; log("Rooms 2: a state from before this seat (match " + std::to_string(m.n) + " < " + std::to_string(m.seat_n) + "): the seat kept"); }
	// at the table alone (the other left, or lost and gone): a session still on with them ended
	if (may_unseat && m.pos == 0 && !m.seated && m.in_seat && !room2_win_sequence()) {
		log("Rooms 2: the other player's no longer at the table: the session ended");
		// (their match lost already, this player the winner (a play-on's loser gone AWAY): you stay on, no forfeit)
		const bool won_it = both_in && score.players() != 3 && ((score.players() >> local_player()) & 1);
		if (ph == Phase::playing) ended(won_it ? tr::line(tr::ROOM2_WON_STAYS) : tr::line(tr::ROOM2_FORFEIT, { { "name", opponent() } }));
		stop_activity(true, QUIT_NEXT_IN_LINE);
	}
	// in the line: back from the table (lost, or a void match), then the room's match watched
	if (may_unseat && (m.pos > 0 || m.watcher) && m.in_seat && !room2_win_sequence()) {
		const std::string mine = m.seat.side;
		room2_unseat(!m.watcher && (((m.last_result == "a" || m.last_result == "b") && m.last_result != mine) || (m.last_result == "forfeit" && m.last_winner != mine)));
	}
	if (m.pos != 0 && !m.in_seat) { m.seen_peer.clear(); m.seen_token.clear(); }   // (in the line or watching: the next seat isn't one stayed at)
	if (m.in_seat && (m.banner == 1 || m.banner == 4) && !room2_moving_on()) m.banner = 0;   // (the line not moving on after all: the two play on)
	if (!r2) return;
	// (a match there to watch: two at the table; a lone one waiting for the first in line has none yet)
	if ((m.pos > 0 || m.watcher) && m.want_watch && !m.wid.empty() && m.has_a && m.has_b && ph == Phase::off && now >= m.watch_retry_at) watch_begin(m.wid);
	// called: the chime and the window flashed (main), the banner's countdown; tests press Start by the ROOM2_READY hook
	if (m.ready_you && !m.was_called) {
		m.was_called = true;
		m.ready_sent = false;
		log("Rooms 2: getting ready: seated in " + std::to_string(int((m.ready_until - now) / 1000 + 0.5)) + " s (AWAY: the menu's SKIP THIS TURN)");
		notices.push_back({ Notice::called, tr::line(tr::ROOM2_CHIP_YOUR_TURN) });
		m.ready_auto_at = room2_test_ready == "now" || room2_test_ready == "esc" ? now + 1000 : 0;
	}
	if (!m.ready_you) m.was_called = false;
	// the next READY one (not getting ready yet, a match on): said once
	const int place = m.ready_place();
	if (place == 1 && m.last_pos != 1 && !m.ready_you && m.has_a && m.has_b) { m.banner = 3; m.banner_until = now + BANNER_MS; }
	m.last_pos = place;
}

// A new pair at the table: watching stopped, the session before (the loser's, this side the winner) quit, a fresh
// session with the opponent the lobby gives, on the room's link, with its token and terms
void Online::room2_seat()
{
	RoomMember &m = *r2;
	// (the room's link gathered again and the lobby being told its new addresses (the enter on its way): the opponent may
	// have been given them, so this seat waits for its answer and takes the link the lobby has (room2_react asks again
	// each pass; the enter's answer is 10 s at most, a seat's start 30 s). Never on the old link meanwhile)
	if (r2_rejoin) {
		static std::string said;
		if (said != m.seat.token) { said = m.seat.token; log("Rooms 2: seated while the lobby's being told the room's link's new addresses: its answer first"); }
		return;
	}
	// (at the table already: a new challenger. The opponent before still known (seen_peer, cleared only back in the line)
	// counts too: the loser's quit for the line moving can end this side's session before the lobby's word seats the next)
	const std::string was_peer = m.seen_peer, was_token = m.seen_token;
	const bool stayed = m.in_seat || !was_peer.empty();
	// (the same two seated again cold, a new token: the match before void for a desync, a cut or a rejoin, and the same
	// opponent. Else a new seat while staying on is a new challenger: another name always (a lost session held, then
	// another seated, leaves the same void), the same name too when it's no such reseat (two can be the same, or both
	// the lobby's PLAYER))
	const bool again = m.last_result == "void" && (m.last_why == "desync" || m.last_why == "disconnect" || m.last_why == "rejoin") && m.last_n + 1 == m.seat.n &&
	                   was_peer == m.seat.peer.name;
	const bool challenger = stayed && !was_token.empty() && was_token != m.seat.token && !again;
	if (m.in_seat) {
		if (ph == Phase::playing) ended(tr::line(tr::ROOM2_WON_STAYS));
		stop_activity(true, QUIT_NEXT_IN_LINE);
	} else if (ph != Phase::off || session)
		stop_activity();
	if (r2_fresh && !r2_gather && !r2_rejoin && r2_fresh_told) {   // (the link gathered again and the lobby told: this seat's on it)
		m.link = std::move(r2_fresh);
		r2_fresh_told = false;
		m.gathered_at = now;
		log("Rooms 2: the room's link gathered again: in use from this seat");
		if (!m.link->relay_address().empty()) m.no_relay_tries = 0;   // (a relay at last: the next time without one from 3 min again)
	}
	if (!m.link) {
		log("Rooms 2: the room's link is gone: leaving the room");
		room2_leave(false);
		return;
	}
	room = std::move(m.link);
	room->clear_peer();
	rooms = true;
	random = false;
	persist = false;
	key.clear();
	code = m.code;
	is_host = m.seat.role == "host";
	peer_name = m.seat.peer.name;
	country_peer = country(m.seat.peer.country);
	room_token = strtoull(m.seat.token.c_str(), nullptr, 16);
	wid = m.wid;
	wkey = is_host ? m.seat.wkey : "";
	m.played_token = m.seat.token;
	m.seat_n = m.seat.n;
	m.started_said = false;
	m.started_n = -1;
	m.result_armed = false;
	m.in_seat = true;
	sel_phase = 0;
	sel_base = 0;
	sel_now = {};
	sel_recording = sel_playing = false;
	m.touched = m.idle_away = m.decided_seen = m.playon_pressed = false;
	m.playon_out_at = 0;
	m.seen_peer = m.seat.peer.name;
	m.seen_token = m.seat.token;
	// (the one who stays: the table's holder, a winner on, against the newcomer; the stage select the newcomer's)
	m.stayer = m.holder == "a" || m.holder == "b" ? m.holder : "";
	if (!use_terms(m.seat.as, m.seat.them)) {   // (can't play this one: the lobby moves on when neither says started)
		log("Rooms 2: the opponent's terms can't be played (" + m.seat.as + ")");
		m.failed_token = m.seat.token;
		stop_activity(true);
		return;
	}
	// (the room's rules: the challenger's stage select as the creator set it, in the match's terms the host sends (a pool's
	// match plays the host's settings: an older build hosting plays its own))
	if (m.has_rules && adapter.host_terms) adapter.stage_select_on = m.rule_stage;
	adapter.dural_refused = false;   // (said for this seat's start only)
	room->set_peer(m.seat.cands, m.seat.relay, is_host);
	room_session(is_host);
	ph = is_host ? Phase::hosting : Phase::joining;
	st = Step::connecting;
	step_at = now;
	if (m.banner == 4) m.banner = 0;   // (the win's banner, held till the line moved: it has)
	if (challenger) { m.banner = 2; m.banner_name = m.seat.peer.name; m.banner_until = now + BANNER_MS; }
	log("Rooms 2: at the table with " + online_menu::clean_name(peer_name) + " (match " + std::to_string(m.seat.n) + ", side " + m.seat.side + ", " +
	    (is_host ? "the session's host" : "its guest") + (challenger ? ", staying on: a new challenger" : stayed ? ", staying on: the same two again" : "") + "): their addresses " +
	    online_detail::mask_list(m.seat.cands) + "; connecting");
}

// Back in the line from the table: the session ended (the other side told the line moved), the room's match watched
void Online::room2_unseat(bool lost)
{
	RoomMember &m = *r2;
	// (who's next: the one seated in this player's place at the table now (the winner keeps theirs); else the one called
	// or the first READY in line. By the side, not the names: two can be the same)
	const std::string &mine = m.seat.side;
	const std::string next = mine == "a" && m.has_a ? m.a.name : mine == "b" && m.has_b ? m.b.name : m.next_name();
	if ((lost || m.banner == 1) && !next.empty()) { m.banner = 1; m.banner_name = next; m.banner_until = now + BANNER_MS; }
	else if (next.empty() && m.banner == 1) m.banner = 0;   // (nobody next: no "NEXT IS" with no name)   // (or said at the table already: the line moving on)
	if (ph == Phase::playing) ended(next.empty() ? (m.me_ready ? tr::line(tr::ROOM2_TO_LINE) : tr::line(tr::ROOM2_AWAY_SAID)) : tr::line(tr::ROOM2_LOST_NEXT, { { "name", next } }));
	log("Rooms 2: back in the line (" + std::to_string(m.pos) + " of " + std::to_string(m.line.size()) + ")" + (next.empty() ? "" : ": next is " + next) + "; watching the room");
	stop_activity(true, QUIT_NEXT_IN_LINE);
	m.seen_peer.clear();
	m.seen_token.clear();
}

// A seat's session ended of itself: the other quit (the line moved on their side, or they left), the connection lost,
// out of step, or it never got going. Its result told as the session says; the lobby says what's next
void Online::room2_session_ended(const vfnet::Session::Event &e)
{
	RoomMember &m = *r2;
	const auto why = session->end_reason();
	const bool was_playing = ph == Phase::playing;
	// (or the match decided and reported, the line moving on: the winner's game made way at the loser-out on the frames run for
	// good, its quit lost or late (a lag's: the gate's VF1, "connection lost" and a disconnect told for a match over): the line
	// moved, never a disconnect, round 11b)
	const bool moved = (why == vfnet::Session::End::opponent_quit && session->opponent_quit_reason() == QUIT_NEXT_IN_LINE) ||
	                   ((why == vfnet::Session::End::opponent_quit || why == vfnet::Session::End::disconnected) && m.reported_n == m.n && m.reported_n >= 0 &&
	                    m.decided_seen && (room2_moving_on() || m.phase == "READY"));
	const std::string who = opponent();
	if (was_playing) {
		const char *res = why == vfnet::Session::End::desync ? "desync" : moved ? nullptr : why == vfnet::Session::End::quit ? nullptr : "disconnect";
		if (res && m.reported_n != m.n) {
			m.reported_n = m.n;
			room2_send("result", ",\"winner\":\"" + std::string(res) + "\",\"n\":" + std::to_string(m.n), true, m.n);
		}
		log("Rooms 2: the session with " + who + " ended: " + e.text + (res ? std::string(": result ") + res : moved ? ": the room's line moved on" : ""));
		if (res) m.retry_token = m.played_token;
		// (the line moved: the opponent made way, which is no "left the match": this side won and stays on, or lost and goes)
		const bool won = (score.players() >> local_player()) & 1;
		const tr::line moved_why = won ? tr::line(tr::ROOM2_WON_STAYS) : tr::line(tr::ROOM2_LOST_NEXT, { { "name", m.next_name() } });
		ended(moved ? moved_why
		      : why == vfnet::Session::End::opponent_quit ? tr::line(tr::OPPONENT_LEFT, { { "name", who } })
		      : why == vfnet::Session::End::disconnected ? tr::line(tr::OPPONENT_STOPPED, { { "name", who } })
		      : why == vfnet::Session::End::desync ? tr::line(tr::MATCH_OUT_OF_STEP) : tr::line(e.text),
		      why == vfnet::Session::End::disconnected);
	} else {
		m.failed_token = m.played_token;
		m.gathered_at = std::min(m.gathered_at, now - room2_regather_ms);   // (perhaps this side's network changed: its addresses gathered again, between seats)
		m.settling = true;   // (not before the lobby's word on this seat: still seated, or back in the line)
		if (adapter.dural_refused) log("Rooms 2: the host's start had Dural otherwise than the match's terms: it can't be played");
		log("Rooms 2: the session with " + online_menu::clean_name(peer_name) + " didn't get going (" + e.text + "): " + (room ? room->report([](const std::string &a) { return online_detail::mask_list({ a }); }) : std::string()) + "; the lobby moves on");
	}
	stop_activity(true);
	room2_react();
}

// The seat's match decided: one of the two out of the game, as its RAM has it (held, both sides alike): the winner the
// one still in (player 1 the session's host, side a); both out a draw. Told once a match: the next once both are back in
void Online::room2_decided()
{
	RoomMember &m = *r2;
	if (ph != Phase::playing || !session || session->state() != vfnet::Session::State::running) return;
	// (decided by the held reading of frames run for good only (Online::score_conf): one from guessed frames (the input
	// delay high, the loser's Start 24 frames after going out not here yet) reported a win the other game never saw, the
	// lobby left DECIDED while the two played a rematch. Both games decide on the same frames this way)
	// (by the rounds when the rounds to win are known (both armed at the match's start), else the held reading of who's in)
	const bool by_rounds = r2_to_win >= 1 && r2_to_win <= 5;
	const int in = by_rounds ? (conf_winner < 0 ? 3 : conf_winner) : score_conf.players();
	if (in == 3) m.result_armed = true;
	if ((by_rounds || both_in_conf) && in != 3 && m.result_armed && m.reported_n != m.n && (m.phase == "PLAYING" || m.phase == "DECIDED")) {
		const char *winner = in == 1 ? "a" : in == 2 ? "b" : "draw";
		m.reported_n = m.n;
		m.result_armed = false;
		room2_send("result", ",\"winner\":\"" + std::string(winner) + "\",\"n\":" + std::to_string(m.n), true, m.n);
		const bool won = (in >> local_player()) & 1;
		// (the fighter they won with kept for their next seat, the select's input it was chosen with; lost, none)
		sel_won_last = won && in != 3;
		if (sel_won_last && sel_last.col >= 0) { sel_keep = sel_last; sel_keep_side = local_player(); }
		else if (!sel_won_last) sel_keep = {};
		log("Rooms 2: match " + std::to_string(m.n) + " decided: " + (in == 0 ? std::string("a draw") : std::string(won ? "this player" : opponent()) + " won") +
		    " (side " + winner + "); " + (!m.next_ready() ? "nobody READY in the line: the two play on" : m.next_ready()->name + " is next"));
	}
	const bool decided = (by_rounds || both_in_conf) && in != 3 && !m.result_armed && m.reported_n >= 0;
	// the banners as it is: the line moving on (someone called, or about to be), "YOU LOST: NEXT IS ..." to the loser and
	// "n WINS IN A ROW: you stay on" to the winner, till the line moves; else (the two playing on) neither, the game's own
	// "Start for a rematch" said then
	if (decided && in != 0 && room2_moving_on()) {
		const bool won = (in >> local_player()) & 1;
		if (!won) {
			if (m.banner != 1) log("Rooms 2: the line moves on: " + m.next_name() + " next (this player's Start for a rematch held)");
			m.banner = 1;
			m.banner_name = m.next_name();
			m.banner_until = now + 600000;
		} else if (m.banner != 4) { m.banner = 4; m.banner_until = now + 600000; }
	}
	const int me = local_player();
	const bool lost_it = decided && in != 0 && !((in >> me) & 1);
	// (reno, 22 Nov) Lost with no input of this player's at all in the match (away: their Start pressed for them at the
	// seat's start): AWAY, passed over till READY again, so an empty chair isn't seated again and again
	// (reno, 26 Nov, the hotfix: never AWAY automatically, only from the menu's SKIP THIS TURN (D-19): this and the 20 s rule
	// below are off. A loser with someone waiting goes back to the line by the line's own move ("out", round 9); with no one,
	// the rematch waits)
	if (AUTO_AWAY && lost_it && !m.touched && !m.idle_away) {
		m.idle_away = true;
		log("Rooms 2: lost with no input of this player's in the match (away): AWAY");
		room2_set_ready(false, ",\"out\":" + std::to_string(m.reported_n));
	}
	// (reno, 22 Nov) A play-on's loser (nobody READY when it was decided) who doesn't press Start for the rematch in
	// PLAYON_AWAY_MS: AWAY, to the back of the line (the lobby takes them off the table); the winner waits for a challenger
	// (from when they're out of the game as its RAM has it, the win poses over: the decision comes at the deciding KO,
	// seconds before; and not once they've pressed Start since losing: their rematch on its way, a select or a continue
	// between. A VF3 loser who pressed Start 25 frames after going out was set AWAY by a timer counted from the KO)
	const bool out_now = !((score.players_now() >> me) & 1);
	if (!lost_it || room2_moving_on() || m.playon_pressed || !out_now) m.playon_out_at = 0;
	else if (!m.playon_out_at) m.playon_out_at = now;
	else if (AUTO_AWAY && now - m.playon_out_at >= PLAYON_AWAY_MS && !m.idle_away) {
		m.idle_away = true;
		log("Rooms 2: the rematch's Start not pressed in " + std::to_string(int(PLAYON_AWAY_MS / 1000)) + " s: AWAY, to the back of the line");
		// (out: the decided match they lost and are out of: the lobby takes a play-on's loser off the table though both
		// games said started (its session goes on from the decision), R-52, R-57)
		room2_set_ready(false, ",\"out\":" + std::to_string(m.reported_n));
	}
	// (round 9) The same loser out with their rematch's Start held back for someone READY in the line (room2_moving_on):
	// both games said started at the decision (the session goes on), so the lobby can't tell the rematch never began;
	// "out" (the match they lost) moves it on to the one READY, the winner's board then still: never a computer fight.
	// Said at most 3 times, 5 s apart (no answer kept: an old lobby without it says no)
	if (lost_it && out_now && room2_moving_on() && m.phase == "PLAYING" && (m.out_n != m.reported_n || (m.out_tries < 3 && now >= m.out_at))) {
		if (m.out_n != m.reported_n) { m.out_n = m.reported_n; m.out_tries = 0; log("Rooms 2: out of match " + std::to_string(m.reported_n) + ", " + m.next_name() + " READY: the lobby told to move on"); }
		room2_send("out", ",\"out\":" + std::to_string(m.reported_n), false);
		m.out_tries++;
		m.out_at = now + 5000;
	}
	if (decided && !m.decided_seen) m.decided_at = now;
	if (decided) m.decided_seen = true;
	else if (m.decided_seen) { m.decided_seen = false; m.touched = m.idle_away = m.playon_pressed = false; }   // (the next match on: afresh)
	// (reno, 22 Nov: no CPU fighting in a room seat) Decided with someone READY and the lobby's word in (READY: the result
	// applied, the next called): the winner's board stops here, its last picture held, "you stay on", till the next pair's
	// seated (the session ends as the line moving on: the loser's board still too, YOU LOST: NEXT IS ...)
	// (round 11b) The room DECIDED or READY on the other's report, this game's own decision not come (the gate's VF1 under
	// load): once the other's out of the game and this player alone in it, someone next, the board held all the same, never
	// a fight against the computer
	const bool alone_won = !decided && both_in && score.players_now() == (1 << me) && (m.phase == "DECIDED" || m.phase == "READY") &&
	                       m.next_ready() != nullptr;
	if (alone_won && ph == Phase::playing) {
		log("Rooms 2: the room decided, this game's own decision not come, this player alone in the game: its win told, the board still");
		if (m.reported_n != m.n) {
			m.reported_n = m.n;
			room2_send("result", ",\"winner\":\"" + std::string(me == 0 ? "a" : "b") + "\",\"n\":" + std::to_string(m.n), true, m.n);
		}
	}
	if (((decided && in != 0 && ((in >> me) & 1) && m.phase == "READY") || alone_won) && ph == Phase::playing && !room2_win_sequence()) {
		log("Rooms 2: won, " + m.next_name() + " called: the board still till the next pair's seated");
		ended(tr::line(tr::ROOM2_WON_STAYS));
		stop_activity(true, QUIT_NEXT_IN_LINE);
		return;
	}
}

// The lobby moving on from this seat's decided match to the next in the line: calling them (GET READY), or the match this
// side reported not resolved yet with someone READY in the line (it'll call them). Only then is the out player's Start
// held (no rematch: the next comes on); else the two play on, the loser's Start a rematch (a play-on's PLAYING n+1, someone
// READY since: the lobby calls them while neither has said started for it, room2_poll's; GET READY then)
// (a game polling the lobby, its socket refused or not open: its last state older than ms (a READY in the line since may not
// be known), and a poll asked now, one on its way at most; on a socket the lobby's word comes as it changes)
bool Online::room2_unsure(double ms)
{
	if (!r2 || r2->test) return false;
	RoomMember &m = *r2;
	if ((m.ws && m.ws->state() == vfnet::http::Socket::State::open) || now - m.state_at <= ms) return false;
	m.poll_at = std::min(m.poll_at, now);
	return true;
}

// (round 11, reno's staging test) The match decided (from the frames run for good, at the deciding KO) but the arcade's own
// end still playing: the KO, its replay and the win poses, both still in the game. The handover, the winner's board
// held, the loser back in the line and a new pair's seat all wait for it, the loser out (the continue, or the next
// challenger's point), WIN_SEQUENCE_MS at most: the two boards had stopped at the KO and the next versus screen come up
bool Online::room2_win_sequence() const
{
	if (!r2 || !r2->in_seat || !r2->decided_seen || ph != Phase::playing || !session || r2->decided_at <= 0) return false;
	// (VF3: and the fight's mode still on (the mode byte 7): Team Battle's loser stays in the game's byte through its continue,
	// the mode moving on is its end there)
	const bool fight = !board.profile->mode || board.ram[board.profile->mode ^ 3] == 7;
	// (the loser out on the frames run for good as well (the held reading): both games past it, the other's too, never the
	// winner's own frame alone, a lagging loser's ending cut and its session lost, round 11b)
	// (the frames run for good stop coming once the other's game has gone (its own ending over first): 2 s at most after this
	// game's own loser-out, never a fight against the computer meanwhile)
	if (score.players_now() != 3 && r2_local_out_at <= 0) r2_local_out_at = now;
	if (score.players_now() == 3) r2_local_out_at = 0;
	return (score.players_now() == 3 || (score_conf.players() == 3 && now - r2_local_out_at < 2000)) && fight && now - r2->decided_at < WIN_SEQUENCE_MS;
}

bool Online::room2_moving_on() const
{
	if (!r2 || !r2->in_seat) return false;
	const RoomMember &m = *r2;
	if (m.ready_on && !m.ready_you) return true;
	// (and a play-on's PLAYING n+1, someone READY in the line since: this side's report the match before, "out" moving the
	// lobby on once the loser's out, round 9)
	return m.next_ready() && (m.reported_n == m.n || (m.phase == "PLAYING" && m.reported_n >= 0 && m.n == m.reported_n + 1)) &&
	       (m.phase == "PLAYING" || m.phase == "DECIDED" || m.phase == "READY");
}

// ---- what the player does ----

bool Online::room2_ready()
{
	if (!r2 || !r2->ready_you || r2->ready_sent) return false;
	const int left = room2_ready_left();
	r2->ready_sent = true;
	r2->version++;   // (the room page's READY NOW gone: seated in a moment)
	room2_send("ready", "", true);
	if (r2 && !r2->outbox.empty() && r2->outbox.back().op == "ready") r2->outbox.back().now = true;   // (its own op: sent again only while still called)
	log("Rooms 2: ready (Start pressed, " + std::to_string(left) + " s left)");
	return true;
}

// READY: on (played in turn) or off (AWAY: kept in the line, passed over)
void Online::room2_set_ready(bool on, const std::string &extra)
{
	if (!r2 || r2->mid.empty()) return;
	r2->me_ready = on;
	r2->away_asked = now;
	if (!on && r2->ready_you) r2->ready_sent = true;   // (the countdown taken down at once)
	room2_send(on ? "ready" : "away", on ? std::string() : extra, true);
	r2->me_ready = on;   // (as asked, till the lobby's state shows it: room2_ops)
	r2->version++;
	log(std::string("Rooms 2: ") + (on ? "READY: on" : "READY: off (AWAY)"));
}

int Online::room2_ready_left() const
{
	if (!r2 || !r2->ready_you || r2->ready_sent) return -1;
	return std::max(0, int((r2->ready_until - now) / 1000 + 0.99));
}

int Online::room2_banner(std::string &name) const
{
	if (!r2 || !r2->banner || now >= r2->banner_until) return 0;
	name = r2->banner_name;
	return r2->banner;
}

// The room's next match come on screen (watched: its versus screen): "YOU LOST: NEXT IS ..." said over the match lost
// and back in the line, not left over the next one
void Online::room2_next_on()
{
	if (r2 && r2->banner == 1) r2->banner = 0;
}

// The streak shown over the match on screen: the one its match began with (table.match) and its holder's side, taken
// while the table's match is the one on screen: at the table, this seat's or a rematch's after it; watching, the seat
// the watched match's start came in (a watcher behind the lobby keeps the last seat's while it's still on screen)
void Online::room2_shown()
{
	RoomMember &m = *r2;
	if (!playing() || m.match_n < 0) return;
	const bool ours = ph == Phase::watching ? m.watch_epoch == m.epoch : m.in_seat && m.match_n >= m.seat_n;
	if (!ours) return;
	m.shown_n = m.match_n;
	m.shown_streak = m.match_streak;
	m.shown_side = m.match_side;
	m.shown_epoch = m.epoch;
}

int Online::room2_streak(int p) const
{
	if (!r2 || !r2->have_state || r2->shown_streak <= 0 || r2->shown_side.empty()) return 0;
	const RoomMember &m = *r2;
	// (taken for the match on screen: another one's never shown over it)
	const bool same = ph == Phase::watching ? m.shown_epoch == m.watch_epoch : m.in_seat && m.shown_n >= m.seat_n;
	if (!same) return 0;
	return m.shown_side == (p == 0 ? "a" : "b") ? m.shown_streak : 0;   // (side a player 1, the session's host)
}

void Online::room2_line(bool join)
{
	if (!r2 || r2->mid.empty()) return;
	room2_send(join ? "join_line" : "leave_line", "", true);
	log(std::string("Rooms 2: ") + (join ? "Join the line" : "Leave line: watching the room, no place in its line"));
}

void Online::room2_watch(bool on)
{
	if (!r2) return;
	r2->want_watch = on;
	if (!on && ph == Phase::watching) stop_activity();
	r2->watch_retry_at = 0;
	room2_react();
}

void Online::room2_leave(bool keep_watching)
{
	if (!r2) return;
	room_turns++;
	const std::string w = r2->wid;
	if (!r2->mid.empty()) {   // (the lobby told, not waited for: a match on is forfeited)
		online_detail::goodbye(lobby_url("/v2/room/leave"), room2_body());   // (a quit waits a moment for it: Online::flush)
		log("Rooms 2: left room " + room_code() + (keep_watching ? " (its matches still watched)" : ""));
	}
	const bool watching_now = ph == Phase::watching;
	if (r2->in_seat) stop_activity();
	if (r2->ws) std::thread([s = std::move(r2->ws)] {}).detach();
	r2.reset();
	r2_asks.clear();
	r2_owner.clear();
	r2_join.reset();
	r2_gather.reset();
	r2_fresh.reset();
	r2_rejoin.reset();
	r2_fresh_told = false;
	if (!r2_file.empty()) remove(r2_file.c_str());
	if (keep_watching && !w.empty() && !watching_now) watch_begin(w);
	else if (!keep_watching && watching_now) stop_activity();
	last = {};   // (as room2_end: no Play again of a seat's match, which would make a room as before)
}

bool Online::room2_settings(const std::string &name, bool listed, int ready_ms, tr::line &error)
{
	if (!r2 || r2->owner_key.empty()) { error = tr::NOTHING_TO_TRY; return false; }
	const std::string body = "{\"code\":" + quote(r2->code) + ",\"owner_key\":" + quote(r2->owner_key) + ",\"room\":" + quote(name) +
	                         ",\"listed\":" + (listed ? "true" : "false") + ",\"ready_ms\":" + std::to_string(std::clamp(ready_ms, 5000, 60000)) + "}";
	room2_owner_send("settings", body);
	log("Rooms 2: the room's settings: " + std::string(listed ? "listed" : "not listed") + ", the ready check " + std::to_string(ready_ms / 1000) + " s");
	return true;
}

// The creator's settings and rules, to the lobby's /v2/room/settings; one it didn't take (busy, not reached, its error) sent
// again 3 s later, 6, 12, 24, then given up (in the log): a change made once isn't lost to a busy lobby. Only the newest of
// each kind (OwnerAsks): an older one failing after a newer went out is dropped, never sent over it
void Online::room2_owner_send(const std::string &op, const std::string &body, int tries)
{
	const int seq = r2_owner.sent(op);   // (the newest of its kind: an older one failing now isn't sent again over it)
	r2_asks.push_back({ op, fetch("POST", lobby_url("/v2/room/settings"), body, 8000, 64 << 10), now, body, tries, seq });
}

void Online::room2_rules(const OnlineSettings &s)
{
	if (!r2 || r2->owner_key.empty()) return;
	const std::string body = "{\"code\":" + quote(r2->code) + ",\"owner_key\":" + quote(r2->owner_key) + ",\"rules\":{\"side\":" + (s.rule_side ? "true" : "false") +
	                         ",\"dural\":" + (s.rule_dural ? "true" : "false") + ",\"custom\":false,\"max\":" + std::to_string(s.rule_max) +
	                         ",\"stage\":" + (s.rule_stage ? "true" : "false") + ",\"change\":" + (s.rule_change ? "true" : "false") + "}}";   // (custom: none till Customize comes, whatever's kept)
	room2_owner_send("rules", body);
	r2->has_rules = true;   // (as asked, till the lobby's state says)
	r2->rule_change = s.rule_change;
	r2->rule_side = s.rule_side; r2->rule_dural = s.rule_dural; r2->rule_custom = false; r2->rule_stage = s.rule_stage; r2->rule_max = s.rule_max;
	r2->version++;
	log("Rooms 2: the room's rules: the winner may change side " + std::string(s.rule_side ? "yes" : "no") + ", character " + (s.rule_change ? "yes" : "no") + ", Dural " + (s.rule_dural ? "yes" : "no") + ", players at most " + (s.rule_max ? std::to_string(s.rule_max) : std::string("no limit")) + ", the challenger's stage select " + (s.rule_stage ? "yes" : "no"));
}

void Online::room2_close()
{
	if (!r2 || r2->owner_key.empty()) return;
	online_detail::goodbye(lobby_url("/v2/room/close"), "{\"code\":" + quote(r2->code) + ",\"owner_key\":" + quote(r2->owner_key) + "}");
	log("Rooms 2: closed room " + room_code());
	room2_end("closed");
}

void Online::set_room2_test(const std::string &state_json, const std::string &c)
{
	Value v;
	if (state_json.empty() || !vfnet::json::parse(state_json, v)) { r2.reset(); return; }
	if (!r2) r2 = std::make_unique<RoomMember>();
	r2->step = 2;
	r2->test = true;
	r2->code = code = c;
	r2->mid = "test";
	r2->want_watch = false;
	r2->owner_key = v["room"]["owner"].kind == Value::boolean && v["room"]["owner"].b ? std::string(32, '0') : "";
	if (v["entering"].kind == Value::boolean && v["entering"].b) {   // (the lobby not answered yet: Entering the room...)
		r2->mid.clear();
		r2->have_state = false;
		r2->room_name = v["room"]["name"].str();
		r2->version++;
		return;
	}
	room2_state(v);
}

bool Online::room2_test_state(const std::string &state_json, double known_at)
{
	Value v;
	if (!r2 || !vfnet::json::parse(state_json, v)) return false;
	return room2_apply(v, known_at);
}
