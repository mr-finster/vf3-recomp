// Rooms 2: a room the lobby owns (online_room.h; docs/ROOMS2-DESIGN.md). The membership above what Online does: entering
// the room, its socket to the lobby (or polling), its state, and what the game does about it: watching the room's
// match while in its line, a fresh session at the table for each new pair (the room's own link kept for all of them),
// the match's result told, the ready check. Nothing here changes src/net: a seat is a room code's session as any other.
#include "online.h"
#include "online_room.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

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
constexpr double PING_MS = 15000;        // (the socket pinged: the lobby closes one quiet for 90 s)
constexpr double BANNER_MS = 4000;       // (a moment's banner over the game)
constexpr double REJOIN_MS = 30000;      // (a membership kept in room2.txt this long: the lobby's 30 s)

bool truthy(const Value &v) { return v.kind == Value::boolean && v.b; }
int num(const Value &v, int lo, int hi, int none = 0) { return v.kind == Value::number ? int(std::clamp(v.n, double(lo), double(hi))) : none; }
RoomMember::Who who_of(const Value &v) { return { online_menu::clean_name(v["name"].str()), v["country"].str().size() == 2 ? v["country"].str() : "", !(v["ready"].kind == Value::boolean && !v["ready"].b) }; }
double wall_ms() { return double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

} // namespace

// The room page's line: the rows shown of a line of n, this player at pos (1.., else none), at most rows of them: each
// an index into the line, or -k for a row that says "... k more". All of a line that fits; past that the first ones
// and this player's own row, "... k more" between and after (a row that would only say 1 more: that one shown instead)
std::vector<int> room2_line_rows(int n, int pos, int rows)
{
	std::vector<int> v;
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

bool Online::room2_enter(const OnlineSettings &s, const std::string &c, tr::line &error)
{
	const std::string want = online_menu::clean_code(c);
	if (want.size() != 6) { error = tr::CODE_BAD; return false; }
	return room2_begin(s, want, false, error);
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
	tried = create ? Try::host_room : Try::join_room;
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
	r2->step = 0;
	code = code_in;
	r2_join.reset();
	r2_asks.clear();
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

// room2.txt: "code mid mkey owner_key wall-ms", the last room's, for a game that comes back within the lobby's 30 s
void Online::room2_keep()
{
	if (r2_file.empty() || !r2) return;
	if (FILE *f = fopen(r2_file.c_str(), "w")) {
		fprintf(f, "%s %s %s %s %.0f\n", r2->code.c_str(), r2->mid.c_str(), r2->mkey.c_str(), r2->owner_key.empty() ? "-" : r2->owner_key.c_str(), wall_ms());
		fclose(f);
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
		const bool ok = create ? host_room(s, err) : enter_code(s, c, err);
		if (!ok) { failure = err; notices.push_back({ Notice::refused, err }); }
		return;
	}
	if (r.status != 200 || !parsed || !online_detail::lobby_key(v["mid"].str(), 0) || !online_detail::lobby_key(v["mkey"].str(), 32) || !online_detail::lobby_room_code(v["code"].str())) {
		const tr::line why = m.creating ? tr::line(tr::COULDNT_OPEN_ROOM_WHY, { { "why", online_detail::lobby_answer(r, room_code(), identity.build) } })
		                                : online_detail::lobby_answer(r, room_code(), identity.build);
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
	m.heard_at = m.poll_at = now;
	m.poll_at = now + POLL_MS;
	room2_keep();
	log("Rooms 2: " + std::string(m.creating ? "created room " : m.back ? "back in room " : "in room ") + room_code() + " (member " + m.mid + ")" +
	    (m.back ? ": its place kept from before" : "") + (m.wid.empty() ? ", its matches not watchable" : ""));
	room2_state(v["state"]);
}

// ---- the lobby's word ----

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
	m.watcher = truthy(you["watcher"]);
	m.watchers = num(st["watchers"], 0, 99);
	const Value &mt = t["match"];
	m.match_n = mt.kind == Value::object ? num(mt["n"], 0, 1 << 30, -1) : -1;
	m.match_streak = mt.kind == Value::object ? num(mt["streak"], 0, 9999) : 0;
	m.match_holder = mt.kind == Value::object ? online_menu::clean_name(mt["holder"].str()) : "";
	m.side = you["side"].str();
	m.misses = num(you["misses"], 0, 9);
	const bool was_ready = m.me_ready && m.have_state;   // (the first state, entering or back after a crash: AWAY as it was, not set by the lobby now)
	m.me_ready = !(you["ready"].kind == Value::boolean && !you["ready"].b);
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
	const bool was_on = m.ready_on && m.ready_you;
	m.ready_on = rd.kind == Value::object;
	m.ready_you = m.ready_on && truthy(rd["you"]);
	m.ready_pos = m.ready_on ? num(rd["pos"], 0, 99) : 0;
	if (m.ready_on) m.ready_until = now + num(rd["deadline_ms"], 0, 600000);
	if (st["watch"].kind == Value::string) m.wid = online_detail::lobby_room_code(st["watch"].str()) ? st["watch"].str() : "";
	m.have_state = true;
	m.version++;
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
	if (last_n >= 0 && last_n != m.last_n) {
		m.last_n = last_n;
		log("Rooms 2: match " + std::to_string(last_n) + " ended: " + m.last_result + (m.last_why.empty() ? "" : " (" + m.last_why + ")") + (m.last_winner.empty() ? "" : ", side " + m.last_winner));
		// (the other left mid-match: a forfeit said to the one who stays)
		if (m.last_result == "forfeit" && m.pos == 0 && !m.seen_peer.empty()) { m.banner = 5; m.banner_name = m.seen_peer; m.banner_until = now + BANNER_MS; }
	}
	// AWAY by the lobby itself (seated, its session never started): said, with how to play again
	(void)was_on; (void)was_pos;
	if (was_ready && !m.me_ready && now - m.away_asked > 5000 && !m.test) {
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
	const tr::line said = why == "ready" ? tr::line(tr::ROOM2_REMOVED_READY) : why == "version" ? tr::line(tr::ROOM2_NEEDS_NEWER)
	                    : why == "closed" ? tr::line(tr::ROOM2_CLOSED) : why == "gone" ? tr::line(tr::ROOM2_REMOVED_GONE) : tr::line(tr::ROOM2_LEFT);
	log("Rooms 2: out of room " + room_code() + ": " + why + " (" + said.en() + ")");
	stop_activity();
	r2.reset();
	last = {};   // (a seat's match isn't one to Play again: that's a room as before's, by this room's code (a new room, or "another version"))
	if (!r2_file.empty()) remove(r2_file.c_str());
	// (not a try that failed: no "Couldn't connect" page with Try again (a closed room's code, or one this player was
	// removed from), just what happened, said over whatever's on; the Online page goes back to the rooms)
	failure.clear();
	fail_step = Step::none;
	notices.push_back({ Notice::left_room, said });
}

// An op to the lobby: on the socket if it's open, else POSTed (its answer a state)
void Online::room2_send(const std::string &op, const std::string &extra)
{
	if (!r2 || r2->mid.empty()) return;
	RoomMember &m = *r2;
	if (m.ws && m.ws->state() == vfnet::http::Socket::State::open) {
		m.ws->send("{\"op\":" + quote(op) + extra + "}", false);
		return;
	}
	r2_asks.push_back({ op, fetch("POST", lobby_url("/v2/room/" + op), room2_body(extra), 8000, 64 << 10) });
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
			if (m.creating)
				extra += std::string(",\"listed\":") + (w.room_who >= 1 ? "true" : "false") + (w.room_name.empty() ? "" : ",\"room\":" + quote(w.room_name));
			else {
				extra += ",\"code\":" + quote(m.code);
				// (a membership from before, this room's: a crash or a restart, its place taken back if it's still there)
				if (!r2_file.empty())
					if (FILE *f = fopen(r2_file.c_str(), "r")) {
						char c[16] = "", mid[40] = "", mkey[80] = "", ok[80] = "";
						double at = 0;
						if (fscanf(f, "%15s %39s %79s %79s %lf", c, mid, mkey, ok, &at) == 5 && m.code == c && wall_ms() - at < REJOIN_MS &&
						    online_detail::lobby_key(mid, 0) && online_detail::lobby_key(mkey, 32)) {
							extra += ",\"mid\":" + quote(mid) + ",\"mkey\":" + quote(mkey);
							if (online_detail::lobby_key(ok, 32)) m.owner_key = ok;
							log("Rooms 2: a membership of this room from " + std::to_string(int((wall_ms() - at) / 1000)) + " s ago: asking for it back");
						}
						fclose(f);
					}
			}
			const std::string body = me_json(extra);
			std::swap(room, m.link);
			m.step = 1;
			r2_join = fetch("POST", lobby_url(m.creating ? "/v2/room/create" : "/v2/room/enter"), body, 10000, 64 << 10);
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
	// a member: room2.txt's time kept fresh (a crash's restart takes the place back only within the lobby's 30 s)
	if (now - m.kept_at >= 5000) { m.kept_at = now; room2_keep(); }
	// the socket, else polling
	using vfnet::http::Socket;
	if (!m.ws && !m.ws_failed && now >= m.ws_at)
		m.ws = std::make_shared<Socket>(lobby_url("/v2/room/ws?code=" + m.code + "&mid=" + m.mid + "&mkey=" + m.mkey));
	if (m.ws) {
		Socket::Message msg;
		while (r2 && m.ws && m.ws->take(msg)) {
			if (msg.data == "pong") continue;
			Value v;
			if (!vfnet::json::parse(msg.data, v)) continue;
			m.heard_at = now;
			if (v["state"].kind == Value::object) room2_state(v["state"]);
			else if (v["removed"].kind == Value::string) { room2_end(v["removed"].str()); return; }
			else if (truthy(v["closed"])) { room2_end("closed"); return; }
			else if (v["error"].kind == Value::string) log("Rooms 2: the lobby said " + v["error"].str() + " to " + v["op"].str());
		}
		if (!r2) return;
		if (m.ws && m.ws->state() == Socket::State::closed) {
			const int status = m.ws->status();
			log("Rooms 2: the room's socket closed (" + m.ws->error() + (status ? ", " + std::to_string(status) : "") + ")" + (status == 101 && m.ws_tries < 5 ? ": again in a moment" : ": polling"));
			std::thread([w = std::move(m.ws)] {}).detach();
			m.ws.reset();
			if (status == 101 && ++m.ws_tries <= 5) m.ws_at = now + 2000;
			else m.ws_failed = true;
			m.poll_at = std::min(m.poll_at, now);
		} else if (m.ws && m.ws->state() == Socket::State::open && now >= m.ping_at) {
			m.ws->send("ping", false);
			m.ping_at = now + PING_MS;
		}
	}
	const bool socket_open = m.ws && m.ws->state() == vfnet::http::Socket::State::open;
	if (!socket_open && now >= m.poll_at) {   // (no socket: asked every 10 s, the lobby's word that this member's still here)
		m.poll_at = now + POLL_MS;
		r2_asks.push_back({ "poll", fetch("POST", lobby_url("/v2/room/poll"), room2_body(), 8000, 64 << 10) });
	}
	for (size_t i = 0; i < r2_asks.size();) {
		if (!r2_asks[i].second->done) { i++; continue; }
		const std::string op = r2_asks[i].first;
		const vfnet::http::Reply r = r2_asks[i].second->reply;
		r2_asks.erase(r2_asks.begin() + long(i));
		Value v;
		const bool parsed = vfnet::json::parse(r.body, v);
		if (r.status == 410 && parsed) { room2_end(v["why"].str()); return; }
		if (r.status == 404 && parsed && (v["error"].str() == "no_room" || v["error"].str() == "not_member")) { room2_end(v["error"].str() == "no_room" ? "closed" : "gone"); return; }
		if (r.status == 200 && parsed) {
			m.heard_at = now;
			if (v["state"].kind == Value::object) room2_state(v["state"]);
			if (!r2) return;
			if (v["ok"].kind == Value::boolean && !v["ok"].b) log("Rooms 2: the lobby said " + v["error"].str() + " to " + op);
		} else if (r.status != 200)
			log("Rooms 2: " + op + ": " + (r.status ? "the lobby said " + std::to_string(r.status) : r.error));
	}
	room2_react();
	if (!r2) return;
	// a seat's match: connected (started said), decided (its result)
	if (m.in_seat && ph == Phase::playing && session && !m.started_said) {
		m.started_said = true;
		room2_send("started");
		log("Rooms 2: connected with " + opponent() + ": started");
	}
	if (m.in_seat) room2_decided();
	room2_shown();
	// tests: VF3_ROOM2_READY's press (now: Start while getting ready; esc: AWAY the first time; away: AWAY once in)
	if (m.ready_you && !m.ready_sent && m.ready_auto_at > 0 && now >= m.ready_auto_at) {
		m.ready_auto_at = 0;
		log("Rooms 2: (test) " + std::string(room2_test_ready == "esc" ? "Esc" : "Start") + " pressed while getting ready");
		if (room2_test_ready == "esc") { room2_set_ready(false); room2_test_ready.clear(); } else room2_ready();   // (esc: once)
	}
	if (room2_test_ready == "away" && !m.test_away_done && m.have_state) { m.test_away_done = true; log("Rooms 2: (test) READY: off"); room2_set_ready(false); }
}

// What to do now, by the lobby's last word and what's on
void Online::room2_react()
{
	if (!r2 || r2->step != 2 || !r2->have_state || r2->test) return;
	RoomMember &m = *r2;
	// at the table with an opponent: their session, if it's not the one on (a new pair; seated again cold)
	if (m.pos == 0 && m.seated && m.seat.token != m.played_token && m.seat.token != m.failed_token) { room2_seat(); return; }
	// (the session lost, the lobby's word since: the same two play on, its token kept (a lost match given to the holder,
	// nobody next): a fresh session with it, both sides alike)
	if (m.pos == 0 && m.seated && !m.in_seat && !m.retry_token.empty() && m.seat.token == m.retry_token && m.phase == "PLAYING" && m.n != m.reported_n) {
		log("Rooms 2: the same two play on (match " + std::to_string(m.n) + "): their session started again");
		m.retry_token.clear();
		m.played_token.clear();
		room2_seat();
		return;
	}
	// at the table alone (the other left, or lost and gone): a session still on with them ended
	if (m.pos == 0 && !m.seated && m.in_seat) {
		log("Rooms 2: the other player's no longer at the table: the session ended");
		if (ph == Phase::playing) ended(tr::line(tr::ROOM2_FORFEIT, { { "name", opponent() } }));
		stop_activity(true, QUIT_NEXT_IN_LINE);
	}
	// in the line: back from the table (lost, or a void match), then the room's match watched
	if ((m.pos > 0 || m.watcher) && m.in_seat) {
		const std::string mine = m.seat.side;
		room2_unseat(!m.watcher && (((m.last_result == "a" || m.last_result == "b") && m.last_result != mine) || (m.last_result == "forfeit" && m.last_winner != mine)));
	}
	if (m.in_seat && (m.banner == 1 || m.banner == 4) && !m.next_ready()) m.banner = 0;   // (nobody READY next after all: the two play on)
	if (!r2) return;
	// (a match there to watch: two at the table; a lone one waiting for the first in line has none yet)
	if ((m.pos > 0 || m.watcher) && m.want_watch && !m.wid.empty() && m.has_a && m.has_b && ph == Phase::off && now >= m.watch_retry_at) watch_begin(m.wid);
	// called: the chime and the window flashed (main), the banner's countdown; tests press Start by VF3_ROOM2_READY
	if (m.ready_you && !m.was_called) {
		m.was_called = true;
		m.ready_sent = false;
		log("Rooms 2: getting ready: seated in " + std::to_string(int((m.ready_until - now) / 1000 + 0.5)) + " s (Esc: AWAY)");
		notices.push_back({ Notice::called, tr::line(tr::ROOM2_YOURE_NEXT) });
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
	const bool stayed = m.in_seat;   // (at the table already: a new challenger)
	const std::string was_peer = m.seen_peer;
	if (m.in_seat) {
		if (ph == Phase::playing) ended(tr::line(tr::ROOM2_WON_STAYS));
		stop_activity(true, QUIT_NEXT_IN_LINE);
	} else if (ph != Phase::off || session)
		stop_activity();
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
	m.started_said = false;
	m.result_armed = false;
	m.in_seat = true;
	m.seen_peer = m.seat.peer.name;
	if (!use_terms(m.seat.as, m.seat.them)) {   // (can't play this one: the lobby moves on when neither says started)
		log("Rooms 2: the opponent's terms can't be played (" + m.seat.as + ")");
		m.failed_token = m.seat.token;
		stop_activity(true);
		return;
	}
	room->set_peer(m.seat.cands, m.seat.relay, is_host);
	room_session(is_host);
	ph = is_host ? Phase::hosting : Phase::joining;
	st = Step::connecting;
	step_at = now;
	if (m.banner == 4) m.banner = 0;   // (the win's banner, held till the line moved: it has)
	if (stayed && !was_peer.empty() && was_peer != m.seat.peer.name) { m.banner = 2; m.banner_name = m.seat.peer.name; m.banner_until = now + BANNER_MS; }
	log("Rooms 2: at the table with " + online_menu::clean_name(peer_name) + " (match " + std::to_string(m.seat.n) + ", side " + m.seat.side + ", " +
	    (is_host ? "the session's host" : "its guest") + (stayed ? ", staying on: a new challenger" : "") + "): their addresses " +
	    online_detail::mask_list(m.seat.cands) + "; connecting");
}

// Back in the line from the table: the session ended (the other side told the line moved), the room's match watched
void Online::room2_unseat(bool lost)
{
	RoomMember &m = *r2;
	// (who's next: the one at the table now who isn't the one this player played, the winner; else the first in line)
	const std::string next = m.has_a && m.a.name != m.seen_peer ? m.a.name : m.has_b && m.b.name != m.seen_peer ? m.b.name : m.next_ready() ? m.next_ready()->name : "";
	if (lost) { m.banner = 1; m.banner_name = next; m.banner_until = now + BANNER_MS; }
	if (ph == Phase::playing) ended(tr::line(tr::ROOM2_LOST_NEXT, { { "name", next } }));
	log("Rooms 2: back in the line (" + std::to_string(m.pos) + " of " + std::to_string(m.line.size()) + ")" + (next.empty() ? "" : ": next is " + next) + "; watching the room");
	stop_activity(true, QUIT_NEXT_IN_LINE);
	m.seen_peer.clear();
}

// A seat's session ended of itself: the other quit (the line moved on their side, or they left), the connection lost,
// out of step, or it never got going. Its result told as the session says; the lobby says what's next
void Online::room2_session_ended(const vfnet::Session::Event &e)
{
	RoomMember &m = *r2;
	const auto why = session->end_reason();
	const bool was_playing = ph == Phase::playing;
	const bool moved = why == vfnet::Session::End::opponent_quit && session->opponent_quit_reason() == QUIT_NEXT_IN_LINE;
	const std::string who = opponent();
	if (was_playing) {
		const char *res = why == vfnet::Session::End::desync ? "desync" : moved ? nullptr : why == vfnet::Session::End::quit ? nullptr : "disconnect";
		if (res && m.reported_n != m.n) {
			m.reported_n = m.n;
			room2_send("result", ",\"winner\":\"" + std::string(res) + "\",\"n\":" + std::to_string(m.n));
		}
		log("Rooms 2: the session with " + who + " ended: " + e.text + (res ? std::string(": result ") + res : moved ? ": the room's line moved on" : ""));
		if (res) m.retry_token = m.played_token;
		ended(moved ? tr::line(tr::OPPONENT_LEFT, { { "name", who } })
		      : why == vfnet::Session::End::opponent_quit ? tr::line(tr::OPPONENT_LEFT, { { "name", who } })
		      : why == vfnet::Session::End::disconnected ? tr::line(tr::OPPONENT_STOPPED, { { "name", who } })
		      : why == vfnet::Session::End::desync ? tr::line(tr::MATCH_OUT_OF_STEP) : tr::line(e.text),
		      why == vfnet::Session::End::disconnected);
	} else {
		m.failed_token = m.played_token;
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
	const int in = score.players();
	if (in == 3) m.result_armed = true;
	if (!both_in || in == 3 || !m.result_armed || m.reported_n == m.n || (m.phase != "PLAYING" && m.phase != "DECIDED")) return;
	const char *winner = in == 1 ? "a" : in == 2 ? "b" : "draw";
	m.reported_n = m.n;
	m.result_armed = false;
	room2_send("result", ",\"winner\":\"" + std::string(winner) + "\",\"n\":" + std::to_string(m.n));
	const bool won = (in >> local_player()) & 1;
	log("Rooms 2: match " + std::to_string(m.n) + " decided: " + (in == 0 ? std::string("a draw") : std::string(won ? "this player" : opponent()) + " won") +
	    " (side " + winner + "); " + (!m.next_ready() ? "nobody READY in the line: the two play on" : m.next_ready()->name + " is next"));
	if (!won && in != 0 && m.next_ready()) { m.banner = 1; m.banner_name = m.next_ready()->name; m.banner_until = now + 600000; }   // (till the line moves)
	if (won && m.next_ready()) { m.banner = 4; m.banner_until = now + 600000; }   // (till the line moves: not "waiting for the loser to press Start for a rematch", which can't come)
}

// ---- what the player does ----

bool Online::room2_ready()
{
	if (!r2 || !r2->ready_you || r2->ready_sent) return false;
	const int left = room2_ready_left();
	r2->ready_sent = true;
	room2_send("ready");
	log("Rooms 2: ready (Start pressed, " + std::to_string(left) + " s left)");
	return true;
}

// READY: on (played in turn) or off (AWAY: kept in the line, passed over)
void Online::room2_set_ready(bool on)
{
	if (!r2 || r2->mid.empty()) return;
	r2->me_ready = on;
	r2->away_asked = now;
	if (!on && r2->ready_you) r2->ready_sent = true;   // (the countdown taken down at once)
	room2_send(on ? "ready" : "away");
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

// The streak shown over the match on screen: the one its match began with (table.match), taken while the table's two are
// the two on screen; a watcher still on the last match (the lobby on to the next pair) keeps that match's
void Online::room2_shown()
{
	RoomMember &m = *r2;
	if (!playing()) return;
	const std::string p0 = player_name(0), p1 = player_name(1), pair = p0 + "|" + p1;
	if (m.match_n >= 0 && m.has_a && m.has_b && m.a.name == p0 && m.b.name == p1 && (m.shown_n != m.match_n || m.shown_pair != pair)) {
		m.shown_n = m.match_n;
		m.shown_pair = pair;
		m.shown_streak = m.match_streak;
		m.shown_holder = m.match_holder;
	}
}

int Online::room2_streak(int p) const
{
	if (!r2 || !r2->have_state || r2->shown_streak <= 0 || r2->shown_holder.empty()) return 0;
	if (r2->shown_pair != player_name(0) + "|" + player_name(1)) return 0;   // (another match on screen than the one taken)
	return player_name(p) == r2->shown_holder ? r2->shown_streak : 0;
}

void Online::room2_line(bool join)
{
	if (!r2 || r2->mid.empty()) return;
	room2_send(join ? "join_line" : "leave_line");
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
	r2_join.reset();
	r2_gather.reset();
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
	r2_asks.push_back({ "settings", fetch("POST", lobby_url("/v2/room/settings"), body, 8000, 64 << 10) });
	log("Rooms 2: the room's settings: " + std::string(listed ? "listed" : "not listed") + ", the ready check " + std::to_string(ready_ms / 1000) + " s");
	return true;
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
