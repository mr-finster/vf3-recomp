// The lobby: room codes for online play (VF1, VF2 and VF3 Recomp). A host opens a room and gets a code; a player
// joins with the code; the lobby hands each the other's addresses (as the game found them: its own network's, its
// public one by STUN, its relay's by TURN) and a token for the session, and the games connect to each other
// themselves. No game traffic comes through here. It also gives out short-lived logins for Cloudflare's TURN relay
// (for the players a direct connection can't reach), so the TURN key never leaves this Worker.
//
//   POST /v1/host   {game, version, content, name, cands, relay, want?} -> {code, key, token}  (want: a code from before)
//   POST /v1/join   {code, game, version, content, name, cands, relay} -> {name, cands, relay, token}  (or {error})
//   POST /v1/wait   {code, key}   (the host's: waits up to 20 s for a player) -> {guest: null | {name, cands, relay}}
//   POST /v1/reopen {code, key}   (the host's, after a player who didn't get going: the same code, open again)
//   POST /v1/close  {code, key}
//   POST /v1/turn   {game, version, content} -> {servers: [{host, port, username, credential}], ttl, country}   (TURN_PER_DAY a day, all told)
//   POST /v1/random {game, version, content, name, cands, relay, avoid?} -> {ticket, waiting, players} or a match
//        (avoid: room codes of random matches lately, their other player not matched with this one again)
//   POST /v1/random/wait  {ticket}  (waits up to 20 s) -> the same
//   POST /v1/random/leave {ticket}
//   POST /v1/random/count {game, version, content} -> {players}: the game's players online, as /v1/online counts them
//        (its games[game].total: each player once, searching, hosting, in a room's line or in a match; no watchers).
//        Stats not there or not answering: the queue's own count, as before (searching, or in a Quick match made in the
//        last 10 minutes)
//   A match: {role: "host", code, key, token, name, country} (the room's, opened as theirs, the other already in it)
//   or {role: "guest", code, name, cands, relay, token, country} (as /v1/join's). Players meet the nearest first.
//   Any of these may carry {pool, accepts} as well (a game that plays across settings: its version's base and a flag,
//   "v0.1.7-online-preview+host1", and the flags of an older version's it can play that version's way, ["stage", ...]).
//   Two players meet when terms() finds what their match plays as: the same pool (the host's settings), the same
//   version (as ever), or one's version the other can play; that's said to both as "as" (the join's answer, the guest
//   in the host's wait, a random match's either side). A game sending no pool is matched as before, by its version.
//   GET  /v1/ping   -> {ok, protocol}
//   GET  /v1/online -> who's online now: each game's count (everyone one can meet) and those on an older version (README),
//        and the rooms: public rooms waiting and the matches anyone can watch (rooms[], below)
//
// The rooms list and watching (spectating; src/net/watch.h has the bytes). /v1/host may carry:
//   {listed: true}    the room's in the rooms list (/v1/online's rooms) by its host's name, and {room}, its name
//   {locked: true}    (listed) shown with a lock: its code isn't listed, a player needs it from the host
//   {watchable: true} its matches may be watched (every new game's: by the room's own code, which is its watch code;
//                     a listed room's in the list), and a Random match's too when its host's game sends it: a Quick
//                     match's is listed once its start comes ({random: true}: its players, to watch, no code to join),
//                     by a watch code of its own, never its room's (the code its Play again opens for the other player)
//   {persist: true}   the room stays while its match is played (the host says it's there with /v1/hold), and holds a
//                     queue: a player joining it then ({queue: true} in /v1/join) waits in line, and the first in line
//                     gets it when the host opens it again after the match (/v1/host with its code and key: the host stays)
// The answer then has {wid, wkey} (a room that may be watched): the watch code and the key the host sends its match
// with; /v1/join's has {wid}, or {error: "full", queued: n} (n-th in line). A game that sends none of these is a
// private room as ever: nothing listed, nothing to watch, nothing kept.
//   POST /v1/hold   {code, key}   (a persistent room's host, every 15 s while it plays)
//   POST /v1/watch/start?w=WID&k=WKEY   (the host's: a match's start, binary) -> {ok}
//   POST /v1/watch/push?w=WID&k=WKEY    (the host's, once a second: the frames confirmed since, binary) -> {has, watchers}
//        (has: the frames the lobby has of the match, -1 it has no start: the host sends it all again)
//   GET  /v1/watch/start?w=WID&m=MATCH  -> the match's start (binary; kept at the edge: a match's never changes)
//   GET  /v1/watch/frames?w=WID&m=MATCH&from=F -> a reply (binary): the frames from F's piece of PIECE on, up to
//        PIECES of them (a whole one kept at the edge for good, the last one a second)
//   POST /v1/watch/seen?w=WID&s=SID     (a watcher's, every 15 s) -> {watchers, keeper: false, match, checkpoint: -1,
//        random}, or {error: "full"} 429 for one more than WATCH_MAX (400) watching (its game: "Too many watching right
//        now"); random: a Quick match's (no room: nobody plays its next match, so the game offers no Play next)
//   POST /v1/watch/cp, GET /v1/watch/cp: no more (a watcher's checkpoint, a state made by a watcher and handed to others,
//        could be forged to the players' hash: refused, 410 {error: "gone"}; a late watcher runs from the match's start)
//   GET  /v1/watch/count?w=WID          -> {watchers} (the guest's: the count shown in the match)
//   GET  /v1/watch/snap?w=WID&m=MATCH   -> {frame, size}: the match's latest snapshot (the host's state at a confirmed
//        frame, watch.h's SNAP), if it's within SNAP_FRESH frames of the latest; else {frame (-1 none), pending: true}
//        and the host asked for one (its next push's answer, or at once on its socket: {snap: true}), or {frame: -1} if
//        its host's game makes none (an older build's: no "snap" in its start's meta). A watcher come late loads it and
//        runs only the frames after it; without, the match from its start, as before. In memory only, the latest one.
//   GET  /v1/watch/snap?w=WID&m=MATCH&f=F -> that snapshot (binary; kept at the edge: a snapshot never changes)
//   POST /v1/watch/snap?w=WID&k=WKEY&m=MATCH&f=F&i=I&n=N (the host's, binary) -> {ok, got}: part I of N (SNAP_PART
//        bytes each, the last less) of its snapshot at frame F, paced by the host (watch.h's Snapper); whole, it's the
//        match's latest, checked: its head the match's, its frame F
//   GET  /v1/watch/live?w=WID&m=MATCH&from=F&s=SID -> the frames as they come (a response that streams: each message a
//        u32 length then a reply as /frames' (length 0: still here), from F on; a few hundred at most a match, after
//        which watchers ask /frames (Cloudflare's edge) as before)
//   POST /v1/peek   {code}   -> {state: "open" | "playing", name, host, watch} or {error: no_room}: what a code is (Enter code;
//        watch: its match may be watched by this code)
//
// Errors are {error: "<what>"} with a status: no_room 404, full 409, game / version / rom 412 (with the host's
// version), busy 429, bad 400, no_turn 503. The games word them; nothing here is shown as it comes.
//
// Rooms 2 (docs/ROOMS2-DESIGN.md; off unless SWITCH_ROOMS2 is "on"): a room the lobby owns, not its creator's game. Its
// members are in line (watching or not) or at the table (the two playing); the winner stays on, the loser goes to the
// back of the line, the first READY member in line plays next (AWAY ones keep their place and are passed over: no ready
// check to press). Every call but create and enter carries the
// member's {code, mid, mkey}; settings and close the creator's {owner_key}. Each answers {ok, state} (state: below), or,
// a member's calls past its rate (R2.MSG_BURST at once, then one every 2 s), 429 busy (a socket's dropped). Switched off,
// create, enter and the socket answer "off" 404; a member's call closes a room still open and answers no_room.
//   POST /v2/room/create  {game, version, content, name, cands, relay, pool?, accepts?, caps?, room?, listed?, locked?,
//        ready_ms?} -> {code, owner_key, mid, mkey, wid, state}: the room, its creator its first member, at the table
//   POST /v2/room/enter   {code, ...as create's player, mid?, mkey?} -> {code, mid, mkey, wid, state, back?}, or {error: "room_version"} 412
//        (mid, mkey: a membership from before, a game crashed or restarted: still a member (within its 30 s), it's taken
//        back with its place, back: true; at the table in a match, the two seated again cold. Otherwise a new member)
//        (a /v1 room's code: the game joins it with /v1/join), no_room 404, full 409, game / version / rom 412
//   GET  /v2/room/ws?code=&mid=&mkey=   a member's WebSocket (hibernating): {state} pushed at every change, {removed: why}
//        or {closed: true} at its end; the member sends "ping", "ready", "away", or {op: "ready" | "away" | "started" | "result" |
//        "leave" | "leave_line" | "join_line", winner?, n?} (as the POSTs below; a refusal comes back as {error})
//   POST /v2/room/poll    (a member without a socket: every 10 s; gone after 30 s without one)
//   POST /v2/room/ready   (READY: on; every member is, from entering. The one being got ready (GET READY, 3 s) seated now)
//   POST /v2/room/away    (AWAY: kept in the line, passed over; the one being got ready, or seated and not started yet,
//        back to the front of the line, AWAY)
//   POST /v2/room/started {n?} (a seated member: its session connected; both within 30 s, or the newcomer (whoever said
//        it: one side's word alone proves nothing) back to the front of the line, AWAY (the 2nd time in a row, the back).
//        Said again, the same; n another match's: stale)
//   POST /v2/room/result  {winner: "a" | "b" | "draw" | "desync" | "disconnect", n?} (each seated member, at the match's end)
//   POST /v2/room/leave
//   POST /v2/room/leave_line   (out of the line, or off the table (a match on: forfeited), still in the room watching: you.pos -1,
//        you.watcher; counted as the room's; told every change)    POST /v2/room/join_line   (a watcher: to the back of the line)
//   POST /v2/room/settings {owner_key, room?, listed?, locked?, ready_ms?}   POST /v2/room/close {owner_key}
// state: {code, seq, room: {name, listed, locked, ready_ms, owner}, table: {a, b, phase, streak, holder, n, last, match: {n, streak,
// holder (a name)}: the match on and the streak it's played for (what's shown over it)}, watchers: n, line: [{name,
// country, ready}], you: {mid, pos (0 at the table, 1.. in line, -1 a watcher: Leave line), side?, watcher?, seat?, ready}, ready
// (GET READY: the next to play, seated when its 3 s are up, after a match with both still seated): {pos, deadline_ms, you} | null,
// watch: wid}. seat (a member at the table with an opponent): {role: "host" (side a: the session's host, who sends its
// match to be watched with wkey) | "guest", side, peer: {name, cands, relay, country}, token, as, them, n, seamless, wkey?}.
// seq: the room's state's number, one more at every change any member's state shows, kept with the room (a restart, the
// object let go: never less), the same in every state told between two changes (a push, a call's answer): a game applies
// none older than the last it applied (a poll's answer that took long, a push before it).
// An older build's /v1/join or /v1/peek on a Rooms 2 code: {error: "version", room_version: 2} 412, with host_version the
// room's creator's own version (its create's build, a release's tag: "The host has v1.9.0-beta1, you have ..."), or, a
// creator that sent none, no host_version ("The host has another version, you have ...: you both need the same version").
// A member's call on a code with no room, or with a mid and mkey that aren't a member's, answers no_room 404 alike (so a
// code can't be found by them: only enter is charged as a code tried); a member let go, by its own key, 410 removed.

import { ANYWHERE, STRONG, ENDINGS, WHOLE, COMPOUNDS } from './words.js';

const PROTOCOL = 1;
const LETTERS = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';   // (no 0/O, 1/I/L: read out over voice chat, typed on a pad)
const CODE_LENGTH = 6;
const HOST_GONE_MS = 30000;      // a host not heard from this long has gone (its wait comes every 20 s)
const ROOM_LIFE_MS = 2 * 3600e3; // and no room lasts longer
const WAIT_MS = 20000;
const HOLD_TELL_MS = 60000;   // (a room whose host is on a socket: the online count told it's there this often, by an alarm)
const HOST_PING_GONE_MS = 90000;   // (a host's socket with no ping this long (its game pings every 15 s): gone, closed, its room let go)
// (sockets' pings answered by Cloudflare without waking the object: set as each object is made, so after hibernating too)
function auto_pong(ctx) {
	if (ctx.setWebSocketAutoResponse) try { ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong')); } catch (e) {}
}
// The off switches: Worker variables (Settings > Variables in the dashboard; plain text, "off" turns one off, anything
// else or none leaves it on), read at every request, so a change is live in seconds with no game release. Each leaves
// the games working as the builds before rooms and watching did: Quick match and room codes untouched.
//   SWITCH_ROOMS       off: no rooms list, no listing, no room that stays; Create room makes a code-only room, as ever
//   SWITCH_LISTING     off: rooms work (a room that stays, its line), but none listed, nor any Quick match to watch
//   SWITCH_WATCH       off: no match is sent or watched; a host sending is told off, a watcher "Watching isn't available"
//   SWITCH_WEBSOCKETS  off: no WebSocket taken (open ones closed as they're next used); everything over HTTPS, as before
// (ROOMS and WATCHES are the Durable Objects' bindings, so the switches are SWITCH_ names.)
const switch_on = (env, k) => String((env && env[k]) ?? 'on').trim().toLowerCase() !== 'off';
const sockets_on = (env) => switch_on(env, 'SWITCH_WEBSOCKETS');
// (snapshots, a watcher come late dropping straight in: on only when SWITCH_SNAP is "on". Off, as by default, no host is
// asked for one and a watcher is told there's none: it runs the match from its start, as before)
const snaps_on = (env) => String((env && env.SWITCH_SNAP) ?? '').trim().toLowerCase() === 'on';
const off = () => json({ error: 'off' }, 503);
// (Rooms 2, the lobby's own rooms: on only when SWITCH_ROOMS2 is "on", the opposite of the switches above on purpose: it
// stays off on the live lobby until a game that uses it is released. Off, /v2/room/* answers 404 {error: "off"} (a new
// game makes a /v1 room, as before) and the rooms list has none of them; nothing else changes. SWITCH_ROOMS off turns it
// off too)
const rooms2_on = (env) => String((env && env.SWITCH_ROOMS2) ?? '').trim().toLowerCase() === 'on' && switch_on(env, 'SWITCH_ROOMS');
// The relay's logins a day, all players together, past which none are given out until the next day (UTC) and the
// games connect directly or not at all. A game asks for one at every try (direct or not), so it's about two a
// match; Cloudflare bills the relay by its traffic (the first 1000 GB a month free), not its logins, and a relayed
// hour is ~10 MB: this bounds a runaway, the per-address cap below bounds any one address.
const TURN_PER_DAY = 100000;
// And from one address a day (an IPv4 one, or an IPv6 /48): room for a school's or a phone network's shared
// address, but one address alone can't take the day's
const TURN_PER_ADDRESS_DAY = 200;
// How long a relay login works: longer than a long night's session (a login ending may end a match through the relay
// with it), short enough that one taken for something else is soon worthless
const TURN_TTL = 6 * 3600;
const GAMES = /^(vf[123]|vf3tb)$/;   // (the games there are: no queue or count made for anything else)

const json = (body, status = 200) =>
	new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });

// What a game says of itself, checked: short printable text, a few addresses
function clean(s, max) {
	return typeof s === 'string' ? s.replace(/[^\x20-\x7e]/g, '').slice(0, max) : '';
}
// The names shown to others (a room's, a player's): none with a word of src/words.js in it (its head says how it's matched:
// word by word on the name made plain, never across two real words)
const SUBS = { 0: 'O', 1: 'I', '!': 'I', '|': 'I', 3: 'E', 4: 'A', 5: 'S', 7: 'T', $: 'S', '@': 'A' };
const runs = (s, n) => s.replace(/(.)\1+/g, (m, c) => c.repeat(Math.min(m.length, n)));
const doubled = (w) => runs(w, 1) !== w;   // (a word with a double letter: matched with runs cut to 2, else to 1)
const plain = (word, root) => runs(word, doubled(root) ? 2 : 1);
const forms = (roots, endings) => roots.map((r) => [r, ['', ...endings].map((e) => runs(r + e, doubled(r) ? 2 : 1))]);
const ANY = ANYWHERE.map((w) => [w, runs(w, doubled(w) ? 2 : 1)]);
const ALL_FORMS = [...forms(STRONG, ENDINGS), ...forms(WHOLE, ['S', 'ES'])];
// (across a space: only the whole run of words spelling a strong word or one of ANYWHERE's, with an ending or none, or one
// of them with a COMPOUNDS word after it; never one inside two real words, nor WHOLE's short ones: PUT A, AS SOON)
const SPAN_ENDINGS = ['S', 'ES', 'ER', 'ERS', 'ING', 'ED'];   // (ENDINGS but the rarer, which two real words make more often: BIT CHIN)
const SPAN_FORMS = [...forms(ANYWHERE, SPAN_ENDINGS), ...forms(STRONG, SPAN_ENDINGS)];
// (a word that's one of STRONG's, WHOLE's or ANYWHERE's then a COMPOUNDS word, plural or not: SHITFACE, COCKSUCK; never one
// with anything else after it, COCKPIT, SHITAKE, nor before it, HANCOCK, SCUNTHORPE)
const TAILS = COMPOUNDS.flatMap((c) => [c, c + 'S']);
const compound = (word, roots) => roots.some((r) => { const w = plain(word, r), p = runs(r, doubled(r) ? 2 : 1); return w.startsWith(p) && TAILS.includes(w.slice(p.length)); });
const WORD_ROOTS = [...STRONG, ...WHOLE, ...ANYWHERE], SPAN_ROOTS = [...STRONG, ...ANYWHERE];
function name_words(s, vu = false) {   // (made plain, its words; one-letter words in a row joined; vu: V read as U too, FVCK)
	let t = String(s || '').toUpperCase().replace(/[013457!|$@]/g, (c) => SUBS[c] || c);
	if (vu) t = t.replace(/V/g, 'U');
	const words = [];
	let single = false;   // (the last word one letter: the next one-letter word goes with it)
	for (const w of t.split(/\s+/).map((x) => x.replace(/[^A-Z]/g, '')).filter(Boolean)) {
		if (w.length === 1 && single) words[words.length - 1] += w;
		else words.push(w);
		single = w.length === 1;
	}
	return words;
}
export function name_ok(s) {   // (exported for test/watch.test.mjs)
	for (const vu of /V/i.test(String(s || '')) ? [false, true] : [false]) {
		const words = name_words(s, vu);
		for (const word of words) {
			if (ANY.some(([w, p]) => plain(word, w).includes(p))) return false;
			if (ALL_FORMS.some(([r, fs]) => fs.includes(plain(word, r)))) return false;
			if (compound(word, WORD_ROOTS)) return false;
		}
		// (a word with a space in it: NI GGER, FU CK, BIT CH; two to four words in a row, run together)
		for (let i = 0; i < words.length; i++)
			for (let j = i + 2; j <= Math.min(words.length, i + 4); j++) {
				const run = words.slice(i, j).join('');
				if (SPAN_FORMS.some(([r, fs]) => fs.includes(plain(run, r))) || compound(run, SPAN_ROOTS)) return false;
			}
	}
	return true;
}
const player_name = (s) => (name_ok(s) ? s : s ? 'PLAYER' : '');
function addrs(list) {
	if (!Array.isArray(list)) return [];
	return list.slice(0, 8).map((a) => clean(a, 64)).filter(addr_ok);
}
// "1.2.3.4:5678" or "[v6]:5678", and an address a player's game can have: not this machine's own (loopback), nor
// none, multicast or broadcast, nor port 0 (what a game would otherwise be made to send to by the other player).
// IPv6 is read whole (every way of writing one: :: anywhere, an IPv4 inside), not by how it starts.
let loopback_ok = false;   // (testing on one's own computer, TURN_STATIC set: the games' addresses are its own)
function v4_bytes(t) {
	const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(t);
	if (!m) return null;
	const b = m.slice(1).map(Number);
	return b.every((x) => x <= 255) ? b : null;
}
function v6_bytes(t) {   // 16 bytes, or null
	if (!/^[0-9a-fA-F:.]+$/.test(t) || (t.match(/::/g) || []).length > 1) return null;
	let tail4 = null;
	const dot = t.lastIndexOf(':');
	if (t.includes('.')) {   // (an IPv4 at the end: its two groups)
		tail4 = v4_bytes(t.slice(dot + 1));
		if (!tail4) return null;
		t = t.slice(0, dot + 1) + '0:0';
	}
	const [head, rest] = t.includes('::') ? t.split('::') : [t, null];
	const hs = head ? head.split(':') : [], rs = rest ? rest.split(':') : [];
	if (rest === null && hs.length !== 8) return null;
	if (rest !== null && hs.length + rs.length > 7) return null;
	const groups = rest === null ? hs : [...hs, ...Array(8 - hs.length - rs.length).fill('0'), ...rs];
	const out = [];
	for (const g of groups) {
		if (!/^[0-9a-fA-F]{1,4}$/.test(g)) return null;
		const v = parseInt(g, 16);
		out.push(v >> 8, v & 255);
	}
	if (tail4) out.splice(12, 4, ...tail4);
	return out;
}
function v4_ok(b) {
	return b[0] !== 0 && (b[0] !== 127 || loopback_ok) && b[0] < 224 && !(b[0] === 169 && b[1] === 254 && b[2] === 169 && b[3] === 254);
}
function addr_ok(a) {
	const m = /^(?:(\d{1,3}(?:\.\d{1,3}){3})|\[([0-9a-fA-F:.]+)\]):(\d{1,5})$/.exec(a);
	if (!m || +m[3] < 1 || +m[3] > 65535) return false;
	if (m[1]) {
		const b = v4_bytes(m[1]);
		return !!b && v4_ok(b);
	}
	const b = v6_bytes(m[2]);
	if (!b) return false;
	const zero = (from, to) => b.slice(from, to).every((x) => x === 0);
	if (zero(0, 10) && b[10] === 0xff && b[11] === 0xff) return v4_ok(b.slice(12));   // (an IPv4, mapped)
	if (zero(0, 12)) return loopback_ok && b[15] === 1 && zero(12, 15);              // (::, ::1, and the old ::a.b.c.d)
	if (b[0] === 0x00 && b[1] === 0x64 && b[2] === 0xff && b[3] === 0x9b && zero(4, 12)) return v4_ok(b.slice(12));   // (NAT64)
	if (b[0] === 0xff) return false;                                                    // (multicast)
	if (b[0] === 0xfe && (b[1] & 0xc0) === 0x80) return false;                          // (link-local: no use without its interface)
	return true;
}
// Where a request comes from, as Cloudflare has it: the country (two letters) and roughly where. (Testing on one's own
// computer, TURN_STATIC set: an x-test-country header stands in for Cloudflare's; never read on the real one.)
function country_ok(c) {   // (a country's two letters, else "": not known)
	return typeof c === 'string' && /^[A-Z]{2}$/.test(c) && c !== 'XX' && c !== 'T1' ? c : '';
}
function country_of(req) {
	const t = loopback_ok ? req.headers.get('x-test-country') : null;
	if (t !== null) return country_ok(t.toUpperCase());
	return country_ok(req.cf && typeof req.cf.country === 'string' ? req.cf.country.toUpperCase() : '');
}
function region_of(req) {   // (the continent Cloudflare places a request on: a room's "where", for the rooms list)
	const c = req.cf && typeof req.cf.continent === 'string' ? req.cf.continent : '';
	return /^(AF|AN|AS|EU|NA|OC|SA)$/.test(c) ? c : '';
}
function geo_of(req) {
	const lat = req.cf ? parseFloat(req.cf.latitude) : NaN, lon = req.cf ? parseFloat(req.cf.longitude) : NaN;
	return Number.isFinite(lat) && Number.isFinite(lon) ? { lat, lon } : null;
}
function who(b, req) {
	const relay = typeof b.relay === 'string' ? clean(b.relay, 64) : '';
	return {
		country: country_of(req),   // (the lobby's own: never what a game says of itself)
		game: clean(b.game, 16),
		version: clean(b.version, 64),
		content: clean(b.content, 32),
		name: player_name(clean(b.name, 10)),   // (a blocked name: PLAYER, for everyone who sees it)
		cands: addrs(b.cands),
		relay: relay && addr_ok(relay) ? relay : '',   // (not checked against Cloudflare's ranges: its TURN relays from others too, 104.30.x)
		pool: clean(b.pool, 64),
		accepts: Array.isArray(b.accepts) ? b.accepts.slice(0, 8).filter((f) => typeof f === 'string' && /^[a-z0-9]{1,16}$/.test(f)) : [],
		watch: b.watchable === true,   // (a game that sends its matches to be watched: a random match's host's)
		qm_listed: b.qm_listed === true,   // (Show my Quick Match games in Rooms: On, said; listed only if both players say so: an older
		                                   // build, which never says, never listed, nor a game that says nothing)
	};
}
// What a match between two players plays as (the game and the ROM checked apart), or null: they can't meet.
// The same pool: the pool's (the host's settings). The same version: it (every game before pools). Else one's
// version that the other can play its way (the same base, every flag of it one the other accepts): an older build's
// terms, so it meets the newer as it meets its own.
function base_of(v) {
	const at = v.indexOf('+');
	return at < 0 ? v : v.slice(0, at);
}
function adopts(n, v) {   // (player n can play version v's match)
	if (!n.pool || !Array.isArray(n.accepts) || base_of(v) !== base_of(n.version)) return false;
	const flags = v.slice(base_of(v).length).split('+').slice(1);
	return new Set(flags).size === flags.length && flags.every((f) => n.accepts.includes(f));
}
// Two in the same pool: the pool's, with Dural on the select when either player has her on (VF3's issue: either
// side's Play as Dural unlocks her for both; the other only sees her spot), marked "+<flag>" with the newest Dural
// flag both accept: a game's accepts hold its own Dural flag ("dural3" now) and, from v0.1.17-alpha, the older
// version's it plays with that version's patches (Revision D's "dural2", Team Battle's "dural": v0.1.16-alpha's), so
// a new build and a v0.1.16 one play her the older way, two new ones "dural3". Both sides load the flag's patches,
// the same words; no flag both accept: the pool's without her. (v0.1.16's builds accept one Dural flag each, so
// between them this is the rule as it was: either's version carrying it, both accepting it.)
const DURAL = /^dural\d*$/;
function dural_rank(f) {
	return f === 'dural' ? 1 : parseInt(f.slice(5), 10);
}
function dural_on(n) {   // (a Dural flag of its own, one it accepts, in its version)
	return n.version.slice(base_of(n.version).length).split('+').slice(1).some((f) => DURAL.test(f) && n.accepts.includes(f));
}
function pool_terms(a, b) {
	if (!dural_on(a) && !dural_on(b)) return a.pool;
	const both = a.accepts.filter((f) => DURAL.test(f) && b.accepts.includes(f)).sort((x, y) => dural_rank(y) - dural_rank(x));
	return both.length ? a.pool + '+' + both[0] : a.pool;
}
function terms(a, b) {
	if (a.pool && a.pool === b.pool) return pool_terms(a, b);
	if (a.version === b.version) return a.version;
	if (!b.pool && adopts(a, b.version)) return b.version;
	if (!a.pool && adopts(b, a.version)) return a.version;
	const ab = adopts(a, b.version), ba = adopts(b, a.version);   // (two pools apart: either's version, the same whichever asks)
	if (ab && ba) return a.version < b.version ? a.version : b.version;
	return ab ? b.version : ba ? a.version : null;
}
// a room's name as the game lets it be typed: the letters of a player's name, and ' (16 at most), upper case
function room_name(s) {   // (a blocked one: none, so it shows as its host's: "<HOST>'S ROOM")
	const raw = clean(s, 16).toUpperCase(), n = raw.replace(/[^A-Z0-9 .!?'-]/g, '').trim();
	return name_ok(raw) && name_ok(n) ? n : '';   // (as typed too, before the marks a name can't have go: N|GGER)
}
function code_ok(c) {
	return typeof c === 'string' && c.length === CODE_LENGTH && [...c].every((ch) => LETTERS.includes(ch));
}
function normal_code(c) {
	return clean(c, 16).toUpperCase().replace(/[^A-Z0-9]/g, '');
}
function random_code() {
	const b = new Uint8Array(CODE_LENGTH);
	crypto.getRandomValues(b);
	return [...b].map((x) => LETTERS[x % LETTERS.length]).join('');
}
function random_hex(n) {
	const b = new Uint8Array(n);
	crypto.getRandomValues(b);
	return [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
}

async function body_of(req) {
	if ((req.headers.get('content-length') | 0) > 4096 || !req.body) return null;
	try {
		// (read as it comes, up to 4 KB: a body sent without its length isn't read whole first)
		const reader = req.body.getReader(), parts = [];
		let size = 0;
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			size += value.length;
			if (size > 4096) { reader.cancel(); return null; }
			parts.push(value);
		}
		const all = new Uint8Array(size);
		let at = 0;
		for (const p of parts) { all.set(p, at); at += p.length; }
		const t = new TextDecoder().decode(all);
		const b = JSON.parse(t);
		return b && typeof b === 'object' ? b : null;
	} catch {
		return null;
	}
}

// Too many tries from one address (an IPv6 one by its /64, as one home's are): a room code guessed at, the relay's
// logins farmed, the lobby flooded. Cloudflare's rate limiting (per place, roughly: a limit, not a count).
function sender(req, groups = 4) {   // (groups: an IPv6 one by its first so many: 4 its /64, 3 its /48)
	const ip = req.headers.get('cf-connecting-ip') || 'local';
	if (!ip.includes(':')) return ip;
	const b = v6_bytes(ip);
	if (!b) return ip;
	if (b.slice(0, 10).every((x) => x === 0) && b[10] === 0xff && b[11] === 0xff) return b.slice(12).join('.');   // (an IPv4, mapped)
	const g = [];
	for (let i = 0; i < groups; i++) g.push(((b[2 * i] << 8) | b[2 * i + 1]).toString(16));
	return g.join(':') + '::/' + groups * 16;
}
async function limited(env, req, what, groups = 4) {   // (groups: an IPv6 address by its /64, or 3: its /48)
	if (!env[what] && what === 'CODE48_LIMIT') return false;   // (a wrangler.toml from before it: no /48 limit)
	const r = await (env[what] || env.ANY_LIMIT).limit({ key: sender(req, groups) });   // (a binding not there, as a wrangler.toml from before it: the general one)
	return !r.success;
}
// A player for the online count: their address (an IPv6 one by its /64) and name, hashed (two tickets, rooms or
// searches of the same player are one; the address itself is never kept by the count). groups 3: an IPv6 one by its
// /48, for what one address may have of something shared (listed rooms, a match's watchers), as CODE48_LIMIT counts:
// a /48 of one's own is 65,536 /64s, not that many places
async function player_id(req, name, groups = 4) {
	const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('vf-online|' + sender(req, groups) + '|' + name));
	return [...new Uint8Array(d).slice(0, 8)].map((x) => x.toString(16).padStart(2, '0')).join('');
}

export default {
	async fetch(req, env) {
		const url = new URL(req.url);
		const path = url.pathname;
		loopback_ok = !!env.TURN_STATIC;
		try {
			if (path === '/v1/ping') return json({ ok: true, protocol: PROTOCOL });
			if (path === '/v1/online' && (req.method === 'GET' || req.method === 'HEAD')) return await online(env, req);
			if (path.startsWith('/v1/watch/')) return await watch_route(env, req, url);
			// a room's host's WebSocket (hibernating): told of its player the moment one comes, and, while it's open, the
			// room held (no /v1/wait every 20 s, no /v1/hold every 15 s)
			if (path === '/v1/room/ws') {
				if ((req.headers.get('upgrade') || '').toLowerCase() !== 'websocket') return json({ error: 'bad' }, 426);
				if (!sockets_on(env)) return off();   // (the game waits over HTTPS, as before)
				if (await limited(env, req, 'WAIT_LIMIT')) return json({ error: 'busy' }, 429);
				const code = normal_code(url.searchParams.get('code'));
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch(new Request('https://room/ws?key=' + encodeURIComponent(clean(url.searchParams.get('key') || '', 64)), req));
			}
			if (path.startsWith('/v2/room/')) return await room2_route(env, req, url, path.slice(9));
			if (req.method !== 'POST') return json({ error: 'bad' }, 404);
			if (await limited(env, req, 'ANY_LIMIT')) return json({ error: 'busy' }, 429);

			if (path === '/v1/host') {
				if (await limited(env, req, 'HOST_LIMIT')) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const me = who(b, req);
				if (!GAMES.test(me.game) || !me.version) return json({ error: 'bad' }, 400);
				const sid = await player_id(req, me.name);
				// (the host's code from before, a match's Play again: kept if no one's taken it meanwhile)
				// (and only by the host it was: its key from then, so no one else can take the code a guest is waiting on)
				const want = normal_code(b.want), old_key = clean(b.key, 64);
				for (let tries = 0; tries < 8; tries++) {   // (a code already in use: another)
					const code = tries === 0 && code_ok(want) ? want : random_code();
					const r = await room(env, code).fetch('https://room/open', { method: 'POST', body: JSON.stringify({ code, host: me, old_key: tries === 0 ? old_key : '', sid, aid: await player_id(req, '#room', 3), listed: b.listed === true, locked: b.listed === true && b.locked === true, watchable: b.watchable === true || b.listed === true, persist: b.persist === true, room: room_name(b.room), region: region_of(req) }) });
					if (r.status === 200) return r;
				}
				return json({ error: 'busy' }, 429);
			}

			if (path === '/v1/join') {
				const b = await body_of(req), code = normal_code(b && b.code);
				const guest = b && code_ok(code) ? who(b, req) : null;
				const ask = guest && JSON.stringify({ guest, sid: await player_id(req, guest.name), queue: b.queue === true });
				// (a player in a room's line asks again every 4 s: not a new code, so not charged as one (two in line behind
				// one address used up its 20 a minute and fell out of it). The room asked first answers only for one in its
				// line; anyone else, and any code with no room, gets the same "new" and is charged as before: no oracle)
				if (ask && b.queue === true) {
					const r = await room(env, code).fetch('https://room/join?inline=1', { method: 'POST', body: ask });
					if (r.headers.get('x-new') !== '1') return r;
					await r.body?.cancel();
				}
				if (await new_code_limited(env, req)) return json({ error: 'busy' }, 429);
				if (!b) return json({ error: 'bad' }, 400);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room/join', { method: 'POST', body: ask });
			}

			if (path === '/v1/peek') {   // (Enter code: a room open to join, or its match on to watch)
				if (await new_code_limited(env, req)) return json({ error: 'busy' }, 429);
				const b = await body_of(req), code = normal_code(b && b.code);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room/peek', { method: 'POST', body: '{}' });
			}
			if (path === '/v1/wait' || path === '/v1/reopen' || path === '/v1/close' || path === '/v1/hold') {
				if (path === '/v1/wait' && (await limited(env, req, 'WAIT_LIMIT'))) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const code = normal_code(b.code);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room' + path.slice(3), { method: 'POST', body: JSON.stringify({ key: clean(b.key, 64) }) });
			}

			if (path === '/v1/turn') {
				if (await limited(env, req, 'TURN_LIMIT')) return json({ error: 'busy' }, 429);
				// (only for one of the games, as it asks: which game, its version, its ROM's hash; not an empty ask)
				const b = await body_of(req), me = b ? who(b, req) : null;
				if (!me || !GAMES.test(me.game) || !me.version || !/^[0-9a-f]{16}$/.test(me.content)) return json({ error: 'bad' }, 400);
				const limits = env.LIMITS.get(env.LIMITS.idFromName('turn')), from = sender(req, 3);
				const day = await limits.fetch('https://limits/', { method: 'POST', body: JSON.stringify({ per_day: TURN_PER_DAY, who: from, per_who: TURN_PER_ADDRESS_DAY }) });
				if (day.status === 429) return json({ error: 'no_turn' }, 503);
				if (day.status === 409) return json({ error: 'busy' }, 429);   // (this address's day's used)
				let t = null;
				try {
					t = await turn(env, country_of(req));
				} finally {   // (a login not given, however it failed: not counted)
					if (!t || t.status !== 200) await limits.fetch('https://limits/', { method: 'POST', body: JSON.stringify({ refund: true, who: from }) });
				}
				return t;
			}

			// a random opponent: the nearest of those searching, the same game, ROM and version's base, whose match terms() finds
			if (path === '/v1/random' || path === '/v1/random/wait' || path === '/v1/random/leave' || path === '/v1/random/count') {
				if (path === '/v1/random' && (await limited(env, req, 'HOST_LIMIT'))) return json({ error: 'busy' }, 429);
				if (path === '/v1/random/wait' && (await limited(env, req, 'WAIT_LIMIT'))) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const me = who(b, req);
				if (!GAMES.test(me.game) || !me.version) return json({ error: 'bad' }, 400);
				const q = env.QUEUES.get(env.QUEUES.idFromName('q|' + me.game + '|' + base_of(me.version) + '|' + me.content));
				if (path === '/v1/random/count') return await random_count(env, me, q);
				return q.fetch('https://queue' + (path === '/v1/random' ? '/enter' : path.slice(10)), {
					method: 'POST',
					body: JSON.stringify({ me, geo: geo_of(req), region: region_of(req), from: sender(req), sid: path === '/v1/random' ? await player_id(req, me.name) : '', aid: path === '/v1/random' ? await player_id(req, '#room', 3) : '', ticket: clean(b.ticket, 32), avoid: Array.isArray(b.avoid) ? b.avoid.slice(0, 8).map((c) => clean(c, 8)) : [] }),
				});
			}

			return json({ error: 'bad' }, 404);
		} catch (e) {
			return json({ error: 'server' }, 500);
		}
	},
};

// The "N online" a game shows by Quick match: the game's players online, the same number the Online page and the
// sites show for it (/v1/online's games[game].total, as online_counts gets it: each Worker copy's memory and the edge
// first, the Stats asked about once a copy every ONLINE_FRESH_MS). Kept 30 s at the edge a game (it no longer depends
// on the version, ROM or pool asking), so it's worked out at most once a game a place every 30 s. The Stats not
// there or not answering: the queue's own count, as before (searching it can meet, or in a Quick match lately), kept
// as before by everything it depends on; never an error. Always {players: number}, as every released game reads it.
// (Someone on the Online screen and not searching isn't counted, as on the page: the lobby doesn't know they're there.)
async function random_count(env, me, q) {
	const edge = !env.TURN_STATIC;   // (not on a test's own lobby: its count's checked as it changes)
	const headers = { 'content-type': 'application/json', 'cache-control': 'max-age=30' };
	const key = new Request('https://count.vf-lobby/v2/' + encodeURIComponent(me.game));
	const kept = edge && (await caches.default.match(key));
	if (kept) return kept;
	let players = -1;
	try {
		if (env.STATS) {
			const r = await online_counts(env);
			if (r.status === 200) {
				const g = (await r.json()).games?.[me.game];
				if (g && Number.isInteger(g.total) && g.total >= 0) players = g.total;
			} else await r.body?.cancel();
		}
	} catch (e) {}
	if (players >= 0) {
		const out = new Response(JSON.stringify({ players }), { headers });
		if (edge) await caches.default.put(key, out.clone());
		return out;
	}
	// (the queue's count, as the lobby gave it before: by game, version, ROM, pool and what it accepts)
	const old = new Request('https://count.vf-lobby/' + encodeURIComponent(me.game + '|' + me.version + '|' + me.content + '|' + me.pool + '|' + me.accepts.join(',')));
	const was = edge && (await caches.default.match(old));
	if (was) return was;
	const r = await q.fetch('https://queue/count', { method: 'POST', body: JSON.stringify({ me }) });
	const out = new Response(await r.text(), { status: r.status, headers });
	if (edge && r.status === 200) await caches.default.put(old, out.clone());
	return out;
}
function room(env, code) {
	return env.ROOMS.get(env.ROOMS.idFromName(code));
}
function watches(env, wid) {
	return env.WATCHES.get(env.WATCHES.idFromName('w|' + wid));
}
const ARM_MS = 5000;   // (a Watch's arming not answered in this long: taken as not armed, the match only unwatchable)
// (each arming numbered, rising (from the clock, in microseconds, and past the last in this isolate): a Watch refuses one
// older than the one it has, so an arming given up here (ARM_MS) that lands late never puts back an older pair's key.
// A watch id is armed by its one room object, so its armings are numbered in the order they're sent)
let arm_last = 0;
async function arm(env, wid, b) {   // (a Watch armed for a room's matches: true; its object not reached, refusing or too slow: false)
	let timer = null;
	arm_last = Math.max(Date.now() * 1000, arm_last + 1);
	const n = arm_last;
	try {
		const late = new Promise((ok) => { timer = setTimeout(() => ok(null), ARM_MS); });
		const a = await Promise.race([watches(env, wid).fetch('https://watch/arm', { method: 'POST', body: JSON.stringify({ ...b, an: n }) }), late]);
		if (!a) return false;
		await a.body?.cancel();
		return a.status === 200;
	} catch (e) {
		return false;
	} finally {
		clearTimeout(timer);
	}
}

// Cloudflare's TURN: a login for TURN_TTL, from the TURN key (kept as this Worker's secrets)
async function turn(env, country) {
	// (testing on one's own computer: a TURN server of one's own, as a .dev.vars line TURN_STATIC='[{"host":...}]')
	if (env.TURN_STATIC) return json({ servers: JSON.parse(env.TURN_STATIC), ttl: TURN_TTL, country: env.DEV_COUNTRY || country });   // (never set on the real one)
	if (!env.TURN_KEY_ID || !env.TURN_KEY_API_TOKEN) return json({ error: 'no_turn' }, 503);
	const r = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${env.TURN_KEY_ID}/credentials/generate-ice-servers`, {
		method: 'POST',
		headers: { authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`, 'content-type': 'application/json' },
		body: JSON.stringify({ ttl: TURN_TTL }),
	});
	if (!r.ok) return json({ error: 'no_turn' }, 503);
	const d = await r.json();
	// (either form Cloudflare has used: iceServers a list of servers, or one)
	const list = Array.isArray(d.iceServers) ? d.iceServers : d.iceServers ? [d.iceServers] : [];
	const servers = [];
	for (const s of list) {
		if (!s.username || !s.credential) continue;
		for (const u of Array.isArray(s.urls) ? s.urls : [s.urls]) {
			// turn:host:port?transport=udp (the games speak TURN over UDP only)
			const m = /^turn:([^:?]+):(\d+)(\?transport=udp)?$/.exec(u || '');
			if (m && (m[3] || !/transport=/.test(u))) servers.push({ host: m[1], port: +m[2], username: s.username, credential: s.credential });
		}
	}
	if (!servers.length) return json({ error: 'no_turn' }, 503);
	return json({ servers, ttl: TURN_TTL, country });
}

// One room: the host's details and key, the player who joined, and the host's waits
export class Room {
	constructor(ctx, env) {
		this.ctx = ctx;
		this.env = env;
		this.waiters = [];
		this.told = 0;   // (when the online count last heard this room's host is waiting)
		this.quiet = new WeakSet();   // (host sockets closed for silence: not counted while Cloudflare lets go of them)
		auto_pong(ctx);
	}
	// (the online count told of a room code's host waiting, a player joining, the room gone: not a random match's room,
	// counted by its queue)
	tell(ev) {
		const r = this.r;
		if (!r || r.random || r.v === 2) return;
		if (ev.op === 'host' || ev.op === 'reopen') this.told = Date.now();
		// (a public room's host by name, for the rooms list: what they chose to show; a private room's, never)
		const g = r.guest;
		const listed = r.listed ? { listed: true, aid: r.aid || '', name: r.host.name, room: r.name || '', locked: !!r.locked, region: r.region || '', persist: !!r.persist, playing: !!r.playing,
			p2: r.playing && g ? [g.name, g.country] : null, queue: (r.queue || []).length, wid: r.wid || '' } : {};
		// (a room that stays, any (listed or not): its line's players still asking, and its match's other player while it's
		// on, so the count has them as long as the room says it's there, not only the 10 minutes after the match began)
		const now = Date.now(), line = r.persist ? (r.queue || []).filter((q) => now - q.seen < QUEUE_GONE_MS && q.sid) : [];
		const stays = r.persist ? { persist: true, qsids: line.map((q) => q.sid), qcountries: line.map((q) => q.country || ''),
			...(r.playing && r.gsid ? { gsid: r.gsid, gcountry: (g && g.country) || '' } : {}) } : {};
		tell(this, { code: r.code, game: r.host.game, version: r.host.pool || r.host.version, playing: !!r.playing, ...listed, ...stays, ...ev });   // (a pool's player counted as the pool's)
	}
	host_socket() { return this.ctx.getWebSockets ? this.ctx.getWebSockets('host').some((ws) => !this.quiet.has(ws)) : false; }
	// (a host's socket with no ping for HOST_PING_GONE_MS: its game gone without a close (a cable pulled, a sleep): closed;
	// when its last ping was, or 0)
	quiet_hosts() {
		let last_of_quiet = 0;
		const now = Date.now();
		for (const ws of this.ctx.getWebSockets ? this.ctx.getWebSockets('host') : []) {
			if (this.quiet.has(ws)) continue;
			const at = (ws.deserializeAttachment() || {}).at || 0, pong = this.ctx.getWebSocketAutoResponseTimestamp ? this.ctx.getWebSocketAutoResponseTimestamp(ws) : null;
			const last = Math.max(at, pong ? pong.getTime() : 0);
			if (now - last <= HOST_PING_GONE_MS) continue;
			this.quiet.add(ws);
			try { ws.close(1001, 'quiet'); } catch (e) {}
			last_of_quiet = Math.max(last_of_quiet, last);
		}
		return last_of_quiet;
	}
	async load(held = false) {   // (held: its host's socket open till this moment, as when it's just closed)
		const woke = this.r === undefined;
		if (woke) this.r = (await this.ctx.storage.get('r')) || null;
		if (this.gone === undefined) this.gone = (await this.ctx.storage.get('gone')) || null;
		if (this.gone && Date.now() > this.gone.until) {
			this.gone = null;
			await this.ctx.storage.delete('gone');
		}
		const r = this.r;
		if (r && r.v === 2) {   // (a Rooms 2 room: its members' liveness is its own, v2_sweep; a restart's lost sockets, v2_woke)
			if (woke && this.v2_woke(r)) await this.ctx.storage.put('r', r);
			return r;
		}
		if (r && (held || this.host_socket())) r.seen = Date.now();   // (its host's socket open: it's there)
		else if (r && woke && r.ws !== false) {
			// (held by its host's socket when this object last ran, the socket gone with it: a deploy, a restart, a variable
			// changed. Its seen is from when the socket came, maybe long ago: the host given HOST_GONE_MS from now to come
			// back, on a socket or a wait, as after its socket's close; once: kept so, a wake after this one takes it as it is)
			r.seen = Math.max(r.seen, Date.now());
			r.ws = false;
			await this.ctx.storage.put('r', r);
		}
		if (r && (Date.now() - r.seen > HOST_GONE_MS || Date.now() - r.born > ROOM_LIFE_MS)) await this.drop();
		return this.r;
	}
	async save() {
		await this.ctx.storage.put('r', this.r);
		await this.ctx.storage.setAlarm(Date.now() + (this.host_socket() ? HOLD_TELL_MS : HOST_GONE_MS + 1000));
	}
	// Gone: the room, and for ROOM_LIFE_MS its code kept for its host (by the key it had) to open again, a Play
	// again's guest waiting on it; no one else can take it from under them meanwhile
	async drop() {
		const r = this.r;
		if (r && r.v === 2) tell(this, { op: 'gone', code: r.code, game: r.game, version: r.version });
		else if (r) this.tell({ op: 'gone' });
		this.r = null;
		await this.ctx.storage.deleteAll();
		if (r) {
			this.gone = { key: r.key || random_hex(16), until: Date.now() + ROOM_LIFE_MS };
			await this.ctx.storage.put('gone', this.gone);
			await this.ctx.storage.setAlarm(this.gone.until + 1000);
		}
		for (const w of this.waiters.splice(0)) w(null);
		if (this.ctx.getWebSockets) for (const ws of this.ctx.getWebSockets('host')) try { ws.send(JSON.stringify({ error: 'no_room' })); ws.close(1000, 'gone'); } catch (e) {}
		if (this.ctx.getWebSockets) for (const ws of this.ctx.getWebSockets('m')) try { ws.send(JSON.stringify({ closed: true, error: 'no_room' })); ws.close(1000, 'gone'); } catch (e) {}
	}
	async alarm() {
		const woke = this.r === undefined, was = !woke ? this.r : (await this.ctx.storage.get('r')) || null;
		if (was && was.v === 2) {
			this.r = was;
			if (woke && this.v2_woke(was)) await this.ctx.storage.put('r', was);
			return this.v2_alarm();
		}
		const quiet = this.quiet_hosts();
		let r = await this.load();
		if (r && quiet && !this.host_socket()) {   // (held by a socket gone quiet: there till its last ping, not till now)
			r.seen = Math.min(r.seen, quiet);
			r.ws = false;
			if (Date.now() - r.seen > HOST_GONE_MS) { await this.drop(); r = null; }
			else await this.ctx.storage.put('r', r);
		}
		// (its host on a socket: the online count told the room's still there, as a wait or a hold would have, every
		// HOLD_TELL_MS; the alarm the only thing that wakes it meanwhile)
		if (r && this.host_socket() && !sockets_on(this.env)) {   // (switched off: its host waits and holds over HTTPS)
			for (const ws of this.ctx.getWebSockets('host')) try { ws.close(1012, 'off'); } catch (e) {}
			r.seen = Date.now();
			r.ws = false;
			await this.save();
		} else if (r && this.host_socket()) {
			if (Date.now() - this.told > HOLD_TELL_MS - 5000) { this.tell({ op: r.playing ? 'hold' : 'host', sids: [r.sid], countries: [r.host.country] }); this.told = Date.now(); }
			await this.ctx.storage.setAlarm(Date.now() + HOLD_TELL_MS);
		} else if (r) await this.ctx.storage.setAlarm(Date.now() + HOST_GONE_MS + 1000);
		else if (this.gone) await this.ctx.storage.setAlarm(this.gone.until + 1000);
	}
	async webSocketMessage(ws, msg) {   // (its host sends nothing but pings, answered without waking this; one that wakes it all the same, noted)
		const att = ws.deserializeAttachment() || {};
		if (att.mid) return this.v2_message(ws, att, msg);   // (a Rooms 2 member's)
		if (msg === 'ping') try { ws.serializeAttachment({ at: Date.now() }); ws.send('pong'); } catch (e) {}
	}
	async webSocketClose(ws, code) {
		try { ws.close(code === 1005 ? 1000 : code, 'closed'); } catch (e) {}
		const att = ws.deserializeAttachment() || {};
		if (att.mid) { const quiet = this.quiet.has(ws); (this.closed2 = this.closed2 || new WeakSet()).add(ws); return this.v2_closed(att, quiet); }
		const r = await this.load(!this.quiet.has(ws));   // (there till now, however long since its last request: not taken for gone; one closed for silence, not)
		if (r && !this.host_socket()) r.ws = false;
		if (r) await this.save();   // (gone HOST_GONE_MS from now if no wait or new socket comes, as after its last wait)
	}
	async webSocketError(ws) { try { ws.close(1011, 'error'); } catch (e) {} }
	async fetch(req) {
		const op = new URL(req.url).pathname;
		if (op.startsWith('/v2/')) return this.v2(op.slice(4), req);   // (Rooms 2: below)
		if (op === '/ws') {   // (its host's WebSocket: by its key; told of its player at once, the room held while it's open)
			const r = await this.load();
			if (!r || new URL(req.url).searchParams.get('key') !== r.key) return json({ error: 'no_room' }, 404);
			for (const old of this.ctx.getWebSockets('host')) try { this.quiet.add(old); old.close(1000, 'replaced'); } catch (e) {}
			const [client, server] = Object.values(new WebSocketPair());
			this.ctx.acceptWebSocket(server, ['host']);
			server.serializeAttachment({ at: Date.now() });   // (its pings counted from now)
			r.seen = Date.now();
			r.ws = true;   // (held by its socket: a restart meanwhile gives its host time to come back on it, load())
			await this.ctx.storage.put('r', r);
			await this.ctx.storage.setAlarm(Date.now() + HOLD_TELL_MS);
			if (r.guest) server.send(JSON.stringify({ guest: r.guest }));
			return new Response(null, { status: 101, webSocket: client });
		}
		const b = await req.json();
		const r = await this.load();
		// (a Rooms 2 room's code: an older build's join or peek told it needs a newer version: the room's creator's own, its
		// release's tag (host_version: "The host has v1.9.0-beta1, you have ..."), or, a creator that didn't say, none ("The
		// host has another version"); a /v1 host's own calls find no room by their key; a /v1/join in line asking again is "new")
		if (r && r.v === 2 && op !== '/open' && !(op === '/join' && new URL(req.url).searchParams.get('inline') === '1')) {
			if (op === '/join' || op === '/peek') return json({ error: 'version', room_version: 2, ...(r.build ? { host_version: r.build } : {}) }, 412);
			return json({ error: 'no_room' }, 404);
		}
		if (op === '/open') {
			// (a persistent room's own host after a match: the room open again in place, its queue and watch code kept)
			const rooms_on = switch_on(this.env, 'SWITCH_ROOMS'), listing_on = switch_on(this.env, 'SWITCH_LISTING'), watch_on = switch_on(this.env, 'SWITCH_WATCH');
			if (!rooms_on) { b.listed = false; b.persist = false; b.locked = false; b.room = ''; }   // (a code-only room, as ever)
			if (r && r.persist && rooms_on && b.old_key && b.old_key === r.key) {
				r.host = b.host;
				r.guest = null;
				r.gsid = '';
				r.playing = false;
				r.token = random_hex(8);
				r.seen = Date.now();
				r.born = Date.now();   // (a room that stays lives ROOM_LIFE_MS from its last match, not its first: one in use goes on)
				r.queue_open = Date.now();
				const out = { code: r.code, key: r.key, token: r.token, persist: true, ...(r.wid && watch_on ? { wid: r.wid } : {}), ...(r.listed && listing_on ? { listed_as: { room: r.name || '', name: b.host.name } } : {}) };
				if (r.wid && watch_on) {
					const wkey = random_hex(16);
					const a = await arm(this.env, r.wid, { wid: r.wid, wkey, game: b.host.game, version: b.host.pool || b.host.version, listed: !!r.listed && !r.locked, region: r.region || '', aid: r.aid || '' });   // (a locked room's match never listed: its watch code is its code)
					if (a) out.wkey = wkey;
					else delete out.wid;   // (its Watch not reached: the room open again all the same, its match not sent)
				}
				await this.save();
				this.tell({ op: 'reopen', sids: [r.sid], countries: [r.host.country] });
				return json(out);
			}
			if (r && b.old_key && b.old_key === r.key) await this.drop();   // (its own host, again before its close arrived: a Play again)
			if (this.r) return json({ error: 'taken' }, 409);
			if (this.gone && (!b.old_key || b.old_key !== this.gone.key)) return json({ error: 'taken' }, 409);
			if (this.gone) {
				this.gone = null;
				await this.ctx.storage.delete('gone');
			}
			this.r = { code: b.code, host: b.host, key: random_hex(16), token: random_hex(8), guest: null, born: Date.now(), seen: Date.now(), random: !!b.random, sid: clean(b.sid, 16), aid: hex16(b.aid), ws: false };
			// a room whose matches may be watched: its watch code (the room's own from before, a Play again's) armed with a
			// new key for its host to send with
			const out = { code: b.code, key: this.r.key, token: this.r.token };
			if (!b.random && (b.listed || b.watchable || b.persist)) {
				this.r.persist = !!b.persist;
				this.r.queue = [];
				this.r.name = typeof b.room === 'string' ? room_name(b.room) : '';
				this.r.locked = !!b.locked;
				this.r.listed = !!b.listed;   // (listed whether or not its matches may be watched: watching can be switched off)
				this.r.region = typeof b.region === 'string' ? b.region.slice(0, 2) : '';
				if (b.persist) out.persist = true;   // (the game keeps it through its match only when told: an older lobby doesn't)
				if (b.listed && listing_on) out.listed_as = { room: this.r.name, name: b.host.name };   // (as the list shows it: a blocked name, its host's)
			}
			// a room whose matches may be watched (every new game's): watched by its own code, armed with a new key for its host.
			// A Quick match's by a watch code of its own, never its room's: its watch code is in the rooms list for anyone, and
			// its room's code is what its Play again opens, by its host's key, for the other player to join (by the room's code
			// a stranger could take that room, and the host's addresses with it). Its own: one no Watch holds now (fresh), so
			// it's no room's code in use; the room's own code never said to anyone but the two.
			// (watching is extra: its Watch not reached, the room opens all the same, its matches not sent)
			if (watch_on && (b.listed || b.watchable || (b.random && b.host && b.host.watch))) {
				this.r.listed = !b.random && !!b.listed;
				this.r.region = typeof b.region === 'string' ? b.region.slice(0, 2) : '';
				for (let tries = 0; tries < (b.random ? 4 : 1); tries++) {
					const wid = b.random ? random_code() : b.code, wkey = random_hex(16);
					if (await arm(this.env, wid, { wid, wkey, game: b.host.game, version: b.host.pool || b.host.version, listed: !b.random && !!b.listed && !b.locked, random: !!b.random && b.quick_listed === true, quick: !!b.random, fresh: !!b.random, region: this.r.region, aid: this.r.aid || '' })) {
						this.r.wid = wid; out.wid = wid; out.wkey = wkey;
						break;
					}
				}
			}
			await this.save();
			this.tell({ op: 'host', sids: [this.r.sid], countries: [b.host.country] });
			return json(out);
		}
		if (op === '/peek') {   // (what this code is now: watchable by it whenever a match is on, a room or a Random one's)
			if (!r) return json({ error: 'no_room' }, 404);
			return json({ state: r.guest ? 'playing' : 'open', name: r.name || '', host: r.host.name, country: r.host.country, game: r.host.game, watch: !!r.wid && r.wid === r.code && switch_on(this.env, 'SWITCH_WATCH'), queue: (r.queue || []).length, random: !!r.random });
		}
		if (op === '/join' && new URL(req.url).searchParams.get('inline') === '1') {   // (the Worker's: one in this room's line asking again?)
			const sid = clean(b.sid, 16), now = Date.now();
			const inline = !!r && !!r.persist && sid.length > 0 && (r.queue || []).some((q) => q.sid === sid && now - q.seen < QUEUE_GONE_MS);
			if (!inline) return new Response('{}', { headers: { 'content-type': 'application/json', 'x-new': '1' } });   // (nothing done: the same for any code)
		}
		if (!r) return json({ error: 'no_room' }, 404);
		if (op === '/join') {
			const g = b.guest, h = r.host;
			if (g.game !== h.game) return json({ error: 'game' }, 412);
			const as = terms(g, h);
			if (as === null) return json({ error: 'version', host_version: h.version }, 412);
			if (g.content !== h.content) return json({ error: 'rom' }, 412);
			// a persistent room's line: a player asking with {queue} waits in it while a match is on, and after it the
			// first in line gets the room (each in it asks every few seconds: one not heard from for QUEUE_GONE_MS is let go)
			const sid = clean(b.sid, 16), now = Date.now();
			if (r.persist) {
				const was = r.queue.length;
				r.queue = r.queue.filter((q) => now - q.seen < QUEUE_GONE_MS);
				const at = r.queue.findIndex((q) => q.sid === sid);
				if (at >= 0) r.queue[at].seen = now;
				const first_free = !r.guest && (at === 0 || r.queue.length === 0);
				if (!first_free && (r.guest || r.queue.length)) {
					if (!b.queue) return json({ error: 'full' }, 409);
					if (at < 0 && r.queue.length < QUEUE_MAX) r.queue.push({ sid, name: g.name, country: g.country, seen: now });
					const pos = r.queue.findIndex((q) => q.sid === sid);
					if (pos < 0) return json({ error: 'full' }, 409);
					await this.save();
					if (r.queue.length !== was) this.tell({ op: 'hold', sids: [r.sid], countries: [h.country] });
					return json({ error: 'full', queued: pos + 1, of: r.queue.length }, 409);
				}
				if (at === 0) r.queue.shift();
			}
			if (r.guest) return json({ error: 'full' }, 409);
			r.guest = { ...g, as, them: g.version };   // (them: the other player's own version, for what the match says: whose Dural)
			r.gsid = sid;   // (for the count: who's playing the host while the room says so)
			if (r.persist) r.playing = true;
			await this.save();
			this.tell({ op: 'join', sids: [r.sid, clean(b.sid, 16)], countries: [h.country, g.country] });
			for (const w of this.waiters.splice(0)) w(r.guest);   // (with the match's terms)
			for (const ws of this.ctx.getWebSockets ? this.ctx.getWebSockets('host') : []) try { ws.send(JSON.stringify({ guest: r.guest })); } catch (e) {}   // (and its socket, at once)
			return json({ name: h.name, cands: h.cands, relay: h.relay, token: r.token, country: h.country, as, them: h.version, ...(r.wid ? { wid: r.wid } : {}) });
		}
		// the host's own: its key
		if (b.key !== r.key) return json({ error: 'no_room' }, 404);
		r.seen = Date.now();
		if (op === '/wait') {
			await this.save();
			if (!r.guest && Date.now() - this.told > 60000) this.tell({ op: 'host', sids: [r.sid], countries: [r.host.country] });   // (still waiting: a minute at most between)
			if (r.guest) return json({ guest: r.guest });
			const g = await new Promise((done) => {
				for (const w of this.waiters.splice(0)) w(undefined);   // (one wait at a time: an older one answered now)
				let timer = 0;
				const finish = (v) => {
					clearTimeout(timer);
					this.waiters = this.waiters.filter((w) => w !== finish);
					done(v);
				};
				this.waiters.push(finish);
				timer = setTimeout(() => finish(undefined), WAIT_MS);
			});
			const now = await this.load();
			if (!now) return json({ error: 'no_room' }, 404);
			now.seen = Date.now();
			await this.save();
			return json({ guest: g || now.guest || null });
		}
		if (op === '/reopen') {
			r.guest = null;
			r.gsid = '';
			r.playing = false;   // (open again: listed waiting, not as a match on)
			r.token = random_hex(8);   // (a new session: the last player's token no good for it)
			await this.save();
			this.tell({ op: 'reopen', sids: [r.sid], countries: [r.host.country] });   // (the player who joined didn't get going: no match after all)
			return json({ code: r.code, key: r.key, token: r.token });
		}
		if (op === '/close') {
			await this.drop();
			return json({ ok: true });
		}
		if (op === '/hold') {   // (a persistent room's host, playing: still there; the rooms list told now and then)
			r.playing = true;
			await this.save();
			if (Date.now() - this.told > 60000) { this.tell({ op: 'hold', sids: [r.sid], countries: [r.host.country] }); this.told = Date.now(); }
			// (its line as it is: those still asking, not one who stopped and wasn't let go yet, no /v1/join since. A game
			// whose guest makes way for the next in line after each match (vf3-recomp issue #42) goes by this: a line
			// left would have moved its guest on for nobody)
			const now = Date.now();
			return json({ ok: true, queue: (r.queue || []).filter((q) => now - q.seen < QUEUE_GONE_MS).length });
		}
		return json({ error: 'bad' }, 400);
	}
}

// ---- Rooms 2 (docs/ROOMS2-DESIGN.md): the lobby's own rooms, winner stays on, a visible line, READY / AWAY ----
// Held by the same Room object as a /v1 room (one object a code, so a code is never both), its record {v: 2, ...} in
// the same 'r', every /v1 path of the object leaving it alone (load() returns it as it is, but for a restart's lost
// sockets, v2_woke; /v1/join and /v1/peek answer 412 version). The table: EMPTY (nobody), SOLO (one, nobody READY in
// line), READY (after a match, both still seated: the next READY member getting ready, GET READY's 3 s), CONNECTING (a
// new pair: both say started within 30 s), PLAYING (a result within 15 minutes), DECIDED (one result in, 25 s for the
// other). The state machine is plain functions on the record (r2_*), the object's methods (v2*) do the rest:
// the members' sockets, storage, the alarm, the Watch, the online count.
const R2 = {
	MEMBERS_MAX: 34,          // (the table's two and a line of QUEUE_MAX)
	PER_ADDRESS: 8,           // (members of one room from one address, an IPv6 one by its /64: a LAN party's, not a script's line)
	CONNECT_MS: 30000,        // (a new pair's both "started" within this)
	DECIDE_MS: 25000,         // (the other result within this of the first: a game says its own again 3 s, 9 and 21 s after the first time
	                          // till it's taken, so one lost on the way still counts; under GONE_MS, so a decided match is settled by its
	                          // report before a game that closed as it was decided is let go)
	READY_MS: 15000, READY_MIN_MS: 5000, READY_MAX_MS: 60000,   // (ready_ms: kept and answered for older test builds; nothing waits on it now)
	GET_READY_MS: 3000,       // (GET READY: the next to play, after a match, seated this long after it's said; AWAY first to stay out)
	GONE_MS: 30000,           // (a member with no socket not heard from this long has gone; its game polls every 10 s)
	PING_GONE_MS: 90000,      // (a member's socket with no ping this long (its game pings every 15 s): closed, gone)
	EMPTY_MS: 60000,          // (a room with no members closes this long after its last left)
	TELL_MS: 50000,           // (the online count told it's there at least this often: it forgets a room in 100 s)
	LIFE_MS: 2 * 3600e3,      // (a room with no new match nor member this long closes)
	REMOVED_KEPT: 40,         // (members let go, kept so their next call says why: the last so many)
	PLAYING_MAX_MS: 15 * 60e3,   // (a match with no result this long (a VF match is minutes): void, the table moves on, so a pair that
	                          // never reports can't hold it for the room's two hours)
	TELL_GAP_MS: 10000,       // (the online count told of a room's changes at most this often (the list is kept 15 s at the edge), the last
	                          // always told (by the alarm), one the same as the last not told: one Stats for every room, kept light)
	SAVE_MS: 10000,           // (a room's record written at every change, and at least this often while only its members' liveness moves)
	MSG_BURST: 10, MSG_EVERY_MS: 2000, MSG_CLOSE: 50,   // (a member's calls, its socket's and HTTPS's together: 10 at once, then one every 2 s;
	                          // past that each dropped (HTTPS: 429 busy), 50 dropped in a row and its socket closed)
	LIVE_MS: 4000,            // (an enter by a membership heard from this recently (or on an open socket): its link gathered again, not a game back)
	CREATES_PER_DAY: 100,     // (Rooms 2 rooms made from one address (an IPv6 one by its /48) a day, UTC)
	CONNECT_BACK: 2,          // (a member whose new pair didn't start this many times in a row (blamed each time): to the back, not the front)
	HOLDER_FAILS: 3,          // (a streak's holder whose new pairs didn't start this many times in a row, each blamed on the newcomer:
	                          // the holder's the one who can't play, unseated itself the next time)
};
const R2_MATCH = new Set(['CONNECTING', 'PLAYING', 'DECIDED']);
const R2_RESULTS = new Set(['a', 'b', 'draw', 'desync', 'disconnect']);
// (a Rooms 2 room's creator's own version, its release's tag, as a game says it: v1.9.0, v0.1.23-alpha, a beta's
// v0.1.23-alpha-beta1; else none. Told an older build that's given the room's code: "The host has v1.9.0-beta1")
function r2_build(v) {
	return typeof v === 'string' && /^v\d{1,4}\.\d{1,4}\.\d{1,4}(-[a-z][a-z0-9-]{0,39})?$/.test(v) ? v : '';
}
const R2_OPS = new Set(['create', 'enter', 'poll', 'ready', 'away', 'started', 'result', 'leave', 'leave_line', 'join_line', 'settings', 'close']);
const r2_other = (side) => (side === 'a' ? 'b' : 'a');
function r2_side(r, mid) {
	return mid && r.t.a === mid ? 'a' : mid && r.t.b === mid ? 'b' : null;
}
function r2_table() {
	return { a: null, b: null, phase: 'EMPTY', streak: 0, holder: null, n: 0, last: null, match: null, token: '', as: '', started: {}, reports: {}, deadline: 0, reseated: false, wkey: '', armed: false, arm: false, lost: false, restart: false, playon: false, hf: null, decide_at: 0 };
}
function r2_clear_match(t) {
	t.token = ''; t.as = ''; t.started = {}; t.reports = {}; t.deadline = 0; t.reseated = false; t.wkey = ''; t.armed = false; t.arm = false; t.lost = false; t.restart = false; t.playon = false; t.decide_at = 0;
}
// (the two seated, a new session between them, cold: its token and terms, a new watch key for side a's game; false: they
// can't play each other)
function r2_seat(r, now, reseat = false) {
	const t = r.t, A = r.members[t.a], B = r.members[t.b];
	const as = A && B ? terms(B.who, A.who) : null;
	if (as === null) return false;
	t.token = random_hex(8); t.as = as; t.n++;
	t.match = { n: t.n, streak: t.streak, holder: t.holder };   // (the streak this match is played for: what's shown over it)
	t.phase = 'CONNECTING'; t.deadline = now + R2.CONNECT_MS; t.started = {}; t.reports = {}; t.reseated = reseat; t.lost = false; t.restart = false; t.playon = false;
	t.wkey = random_hex(16); t.armed = false; t.arm = true;   // (armed by the object: v2_commit)
	r.active = now;
	return true;
}
// (the two seated again cold (a desync, a disconnect, a member back after a crash); if they can't play each other now (one
// back with another version), the match void and the newcomer (not the streak's holder) unseated: the table moves on)
function r2_reseat(r, now, reseat) {
	if (r2_seat(r, now, reseat)) return;
	const t = r.t;
	r2_unseat(r, t.holder === t.a ? 'b' : 'a', 'version', now);
}
// (the line empty, or nobody in it READY: the two play on in their session, as a room's match always has. Their session
// lost (held: a disconnect seated again once already): the same token, the two start it again, both to say started within
// CONNECT_MS as a new pair, else void as one (r2_connect_failed). Any match: a result within PLAYING_MAX_MS, else void.
// t.playon: a play-on whose match neither has said started for yet: someone READY in line meanwhile (its games then hold
// the loser's Start), the table moves on at once instead (r2_advance))
function r2_rematch(r, now) {
	const t = r.t, restart = !!t.lost;
	t.n++; t.phase = 'PLAYING'; t.reports = {}; t.started = {}; t.lost = false; t.restart = restart; t.playon = true;
	t.deadline = now + (restart ? R2.CONNECT_MS : R2.PLAYING_MAX_MS);
	t.match = { n: t.n, streak: t.streak, holder: t.holder };
	r.ready = null;
	r.active = now;
}
// The first READY member in line (AWAY ones passed over, their places kept): the next to play, or null
function r2_next(r) {
	return r.line.find((x) => r.members[x] && !r.members[x].away) || null;
}
function r2_call(r, now, mid) {   // (GET READY: the next to play, unless one is already: a READY member ahead of them who just came back waits their turn)
	if (!r.ready) r.ready = { mid, deadline: now + R2.GET_READY_MS };
}
// (what members are told of: the table, the line, who's getting ready, the members, READY or AWAY, watching or not)
const r2_sig = (r) => JSON.stringify([r.t, r.line, r.ready, Object.entries(r.members).map(([k, m]) => k + (m.away ? '*' : '') + (m.watcher ? 'w' : ''))]);
// Where the table goes from here, nothing pending: a match on stays on; after one (READY, both still seated) the next
// READY member got ready (3 s, then seated), or the two play on (nobody ready in line: as an empty line); one seated: the
// next READY member seated at once, or SOLO; none: the first READY member in line takes the table, or EMPTY
function r2_advance(r, now) {
	const t = r.t;
	if (t.a && t.b && R2_MATCH.has(t.phase)) {
		// (a play-on (r2_rematch) neither of the two has said started for, and a READY member in line now (entered, READY
		// again, joined the line): its match never began (its games hold the loser's Start for the next in line), so the
		// last match's result stands and the next is got ready at once, not after PLAYING_MAX_MS. Their session as it was:
		// lost, if it was, for a play-on after all)
		if (!(t.phase === 'PLAYING' && t.playon && !t.started.a && !t.started.b && r2_next(r))) return;
		t.lost = t.restart; t.restart = false; t.playon = false; t.reports = {}; t.started = {}; t.deadline = 0;
	}
	const next = r2_next(r);
	if (t.a && t.b) {   // (READY: the match decided, its loser still seated till the next is)
		t.phase = 'READY';
		if (next) r2_call(r, now, next);
		else r2_rematch(r, now);
		return;
	}
	r.ready = null;
	if (!t.a && !t.b && next) { r.line = r.line.filter((x) => x !== next); t.a = next; }
	const one = t.a || t.b;
	r2_clear_match(t);
	if (!one) { t.phase = 'EMPTY'; t.holder = null; t.streak = 0; return; }
	if (t.holder !== one) { t.holder = one; t.streak = 0; }   // (left alone at the table: the streak kept only by the one who held it)
	t.phase = 'SOLO';
	const two = r2_next(r);
	if (two) r2_take(r, two, now);   // (the table waiting: the first ready one plays it at once, no countdown)
}
function r2_enter(r, m, now) {
	r.members[m.mid] = m;
	r.line.push(m.mid);
	r.empty_since = 0;
	r.active = now;
	r2_advance(r, now);
}
// A member gone (left, quiet, can't play): out of the line, off the table (a match on: forfeited, the other stays, the
// streak kept, not raised), the next seated
function r2_remove(r, mid, why, now) {
	const t = r.t, side = r2_side(r, mid), m = r.members[mid];
	if (!m) return;
	delete r.members[mid];
	r.line = r.line.filter((x) => x !== mid);
	if (r.ready && r.ready.mid === mid) r.ready = null;
	r.removed = [...(r.removed || []).filter((x) => x.mid !== mid), { mid, mkey: m.mkey, why }].slice(-R2.REMOVED_KEPT);
	if (side) {
		t[side] = null;
		if (R2_MATCH.has(t.phase)) t.last = { result: 'forfeit', n: t.n, winner: t[r2_other(side)] ? r2_other(side) : null };
		if (t.holder === mid) t.holder = null;
		t.phase = 'SOLO';   // (no match without two: r2_advance says what it is)
	}
	if (!Object.keys(r.members).length) r.empty_since = now;
	r2_advance(r, now);
}
// A member out of the line and off the table but still in the room, watching (Leave line): a match on is forfeited as
// by leaving; Join the line puts them at its back
function r2_leave_line(r, mid, now) {
	const t = r.t, side = r2_side(r, mid), m = r.members[mid];
	if (!m || m.watcher) return m ? null : 'not_now';
	m.watcher = true;
	r.line = r.line.filter((x) => x !== mid);
	if (r.ready && r.ready.mid === mid) r.ready = null;
	if (side) {
		t[side] = null;
		if (R2_MATCH.has(t.phase)) t.last = { result: 'forfeit', n: t.n, winner: t[r2_other(side)] ? r2_other(side) : null };
		if (t.holder === mid) t.holder = null;
		t.phase = 'SOLO';
	}
	r2_advance(r, now);
	return null;
}
function r2_join_line(r, mid, now) {
	const m = r.members[mid];
	if (!m || !m.watcher) return 'not_now';
	m.watcher = false;
	r.line.push(mid);
	r.active = now;
	r2_advance(r, now);
	return null;
}
// mid (in line) seated: against the one who stays (the streak's holder; the one alone at the table), the loser (the
// other) to the back of the line, the newcomer in the seat left, a new session between them
function r2_take(r, mid, now) {
	const t = r.t, m = r.members[mid];
	const stays = t.a && t.b ? t.holder : t.a || t.b;
	if (stays && terms(m.who, r.members[stays].who) === null) { r2_remove(r, mid, 'version', now); return; }
	r.line = r.line.filter((x) => x !== mid);
	r.ready = null;
	if (t.a && t.b) {
		const out = t.a === stays ? 'b' : 'a';
		r.line.push(t[out]);
		t[out] = null;
	}
	t[t.a ? 'b' : 'a'] = mid;
	if (!t.holder) t.holder = stays;
	r2_seat(r, now);
}
// READY on (every member from entering): passed over no more; the one being got ready seated now (Start pressed)
function r2_ready(r, mid, now) {
	const m = r.members[mid];
	if (!m || m.watcher) return 'not_now';
	const was = m.away;
	m.away = false;
	if (r.ready && r.ready.mid === mid) { r2_take(r, mid, now); return null; }
	if (was && !r2_side(r, mid)) r2_advance(r, now);
	return null;
}
// AWAY: kept in the line, passed over. The one being got ready, or seated and not started yet (the session not going:
// CONNECTING), back to the front of the line; at the table in a match, AWAY from when they're back in the line
function r2_away(r, mid, now) {
	const t = r.t, m = r.members[mid], side = r2_side(r, mid);
	if (!m || m.watcher) return 'not_now';
	m.away = true;
	if (r.ready && r.ready.mid === mid) { r.ready = null; r2_advance(r, now); return null; }
	if (side && t.phase === 'CONNECTING' && t.holder !== mid) { r2_unseat(r, side, 'away', now); return null; }
	return null;
}
// The seated side that can't play this pair (AWAY, or its session never started) back to the front of the line, AWAY
function r2_unseat(r, side, why, now) {
	const t = r.t, mid = t[side], m = r.members[mid];
	t.last = { result: 'void', why, n: t.n };
	t[side] = null;
	if (t.holder === mid) { t.holder = null; t.streak = 0; }
	t.phase = 'SOLO';
	if (m) {
		m.away = true;
		// (the design's "the newcomer back to the front once, then the back": the CONNECT_BACK-th time in a row, to the back)
		if (why === 'connect' && (m.connect_fails = (m.connect_fails || 0) + 1) >= R2.CONNECT_BACK) { m.connect_fails = 0; r.line.push(mid); }
		else r.line.unshift(mid);
	}
	r.ready = null;
	r2_advance(r, now);
}
// A new pair that didn't both say started in 30 s: the newcomer (not the streak's holder) back to the front of the line,
// AWAY (not let go: READY: on puts them back in turn; gone or not is the 30 s rule's). Whoever said started: a session
// connects both ends at once, so one side's word alone proves nothing, and the holder isn't unseated on a challenger's
// say-so. A holder whose new pairs fail HOLDER_FAILS times in a row (seat after seat, nothing else between), the holder
// saying started or neither, is the one unseated (a holder who says started and never connects can't keep the table by
// blaming every challenger); a newcomer's started alone isn't counted against it (t.hf: {mid, n, count})
function r2_connect_failed(r, now) {
	const t = r.t, newcomer = t.holder === t.a ? 'b' : 'a', holder = r2_other(newcomer), H = t[holder];
	let side = newcomer;
	if (!(t.started[newcomer] && !t.started[holder]) && H) {
		const count = t.hf && t.hf.mid === H && t.hf.n === t.n - 1 ? t.hf.count + 1 : 1;
		t.hf = { mid: H, n: t.n, count };
		if (count >= R2.HOLDER_FAILS) { side = holder; t.hf = null; }
	}
	r2_unseat(r, side, 'connect', now);
}
// (both said started: the match on, a result within PLAYING_MAX_MS; their failures to start forgotten)
function r2_on(r, now) {
	const t = r.t;
	t.phase = 'PLAYING'; t.restart = false; t.playon = false; t.deadline = now + R2.PLAYING_MAX_MS; t.hf = null;
	for (const x of [t.a, t.b]) if (r.members[x]) r.members[x].connect_fails = 0;
	r.active = now;
}
// (a seated member's session connected: as often as it's said (a game says it again till it's heard), the same; n, if
// sent, the match it's for: an earlier one's, late, refused as stale)
function r2_started(r, mid, now, n) {
	const t = r.t, side = r2_side(r, mid);
	if (!side || !t.token) return 'not_now';
	if (Number.isInteger(n) && n !== t.n) return 'stale';
	if (t.phase === 'PLAYING' && t.restart) {   // (the two starting their lost session again: both, as a new pair)
		t.started[side] = true;
		if (t.started.a && t.started.b) r2_on(r, now);
		return null;
	}
	if (t.phase === 'PLAYING' && t.playon) { t.started[side] = true; return null; }   // (a play-on's match begun: kept on for a READY one in line)
	if (t.phase !== 'CONNECTING') return R2_MATCH.has(t.phase) ? null : 'not_now';
	t.started[side] = true;
	if (t.started.a && t.started.b) r2_on(r, now);
	return null;
}
// (a match with no result in PLAYING_MAX_MS: void, the holder stays (as a disconnect held), the table moves on: the next
// READY one in line got ready, or the two play on)
function r2_timeout(r, now) {
	const t = r.t;
	return r2_won(r, r2_side(r, t.holder) || 'a', false, { result: 'void', why: 'timeout', n: t.n }, now);
}
function r2_result(r, mid, winner, n, now, alive) {
	const t = r.t, side = r2_side(r, mid);
	if (!side || !R2_RESULTS.has(winner)) return 'not_now';
	if (Number.isInteger(n) && n !== t.n) return 'stale';   // (an earlier match's, late)
	if (t.phase === 'PLAYING') {   // (DECIDE_MS for the other's report; a lone one settled then only as r2_lone says, else at the match's own end)
		t.reports = { [side]: winner }; t.phase = 'DECIDED'; t.decide_at = now + R2.DECIDE_MS;
		t.deadline = Math.max(t.restart ? now + R2.PLAYING_MAX_MS : t.deadline || 0, t.decide_at); t.restart = false;
		return null;
	}
	if (t.phase !== 'DECIDED') return 'not_now';
	if (t.reports[side]) return t.reports[side] === winner ? null : 'already';
	t.reports[side] = winner;
	return r2_resolve(r, now, alive);
}
// A lone report settles the match (r2_due) once DECIDE_MS is up only if it concedes (the other side won, by its own
// player's word) or the silent side's game is gone (not alive: no socket, not polling, past a restart's grace); while the
// silent side's alive, the match is waited out to its own end (PLAYING_MAX_MS from its start: t.deadline), so a report
// sent early (a forged win) is contradicted by the honest side's when its match ends (disagreeing: void). At that end
// the lone report stands, so a side that never reports can't keep a table for it
function r2_lone(r, now, alive) {
	const t = r.t, ra = t.reports.a, rb = t.reports.b;
	if (!(ra || rb) || (ra && rb)) return false;
	const silent = ra ? 'b' : 'a';
	return now >= t.deadline || (now >= (t.decide_at || 0) && ((ra || rb) === silent || !alive(t[silent])));
}
// The match's result, from the two reports (or one, as r2_lone says): agreed, applied; a draw, the streak's holder
// stays; a desync, void and the two seated again, cold; disagreeing, void, both to the back, the streak gone; a
// disconnect (both saying it, or one, the other claiming a win), the side whose lobby socket is dead loses, both alive the
// two seated again once, then the holder stays, its streak not raised (held: a holder that says disconnect whenever it
// loses keeps the table but never grows its streak; a challenger that cuts the link while losing and claims the win gains
// nothing). One report: a win, a loss or a draw as it says; a desync, void (nobody's fault), the two seated again; a
// disconnect, the silent side's forfeit. alive(mid): its socket open, or its game polling
function r2_resolve(r, now, alive) {
	const t = r.t, ra = t.reports.a, rb = t.reports.b;
	let kind;
	// (one side out of step, the other only cut off: a desync ends the session on the side that sees it, and the other
	// side's game sees only the connection lost)
	if (ra && rb) kind = ra === rb ? ra : (ra === 'desync' && rb === 'disconnect') || (ra === 'disconnect' && rb === 'desync') ? 'desync'
		: ra === 'disconnect' || rb === 'disconnect' ? 'disconnect' : 'disagree';
	else { const said = ra || rb; kind = said === 'disconnect' ? 'lost_' + (ra ? 'b' : 'a') : said; }
	if (kind === 'disconnect') {
		const da = !alive(t.a), db = !alive(t.b);
		kind = da && db ? 'disagree' : da ? 'lost_a' : db ? 'lost_b' : t.reseated ? 'held' : 'reseat';
	}
	const n = t.n;
	if (kind === 'a' || kind === 'b') return r2_won(r, kind, true, { result: kind, n }, now);
	if (kind === 'held') t.lost = true;   // (their session's gone: the two start it again if they play on, r2_rematch)
	if (kind === 'draw' || kind === 'held') return r2_won(r, r2_side(r, t.holder) || 'a', false, { result: kind === 'draw' ? 'draw' : 'void', why: 'disconnect', n }, now);
	if (kind === 'lost_a' || kind === 'lost_b') return r2_won(r, kind === 'lost_a' ? 'b' : 'a', false, { result: 'forfeit', winner: kind === 'lost_a' ? 'b' : 'a', n }, now);
	if (kind === 'desync' || kind === 'reseat') {
		t.last = { result: 'void', why: kind === 'desync' ? 'desync' : 'disconnect', n };
		r2_reseat(r, now, kind === 'reseat' || t.reseated);
		return null;
	}
	// (disagreeing, or both gone: void, both to the back, the streak gone)
	t.last = { result: 'void', why: kind === 'disagree' && ra && rb ? 'disagree' : 'gone', n };
	r.line.push(t.a, t.b);
	t.a = t.b = t.holder = null;
	t.streak = 0;
	t.phase = 'EMPTY';
	r.ready = null;
	r2_advance(r, now);
	return null;
}
// side won (raise: a match won, the streak counts it; else a draw or a forfeit: the one who stays keeps the streak if
// it was theirs, not raised). The loser stays seated till the next in line says ready (or they play on: no line)
function r2_won(r, side, raise, last, now) {
	const t = r.t, w = t[side];
	if (raise) { if (t.holder === w) t.streak++; else { t.holder = w; t.streak = 1; } }
	else if (t.holder !== w) { t.holder = w; t.streak = 0; }
	t.last = last;
	t.reports = {}; t.deadline = 0; t.decide_at = 0;
	t.phase = 'READY';
	r2_advance(r, now);
	return null;
}
// Every deadline passed, in order (a member gone first: their leaving can end what the deadline was for). A step that
// moved nothing (a deadline passed and still there: never meant to happen) ends the match on the table, void, both to the
// back (r2_stuck), so the room can't spin on it (an alarm every 100 ms, the count told each time)
const r2_moved = (r) => JSON.stringify([r.t, r.ready, r.line]);
function r2_due(r, now, alive) {
	let changed = false;
	for (let i = 0; i < 200; i++) {
		const t = r.t, was = r2_moved(r);
		if (r.ready && now >= r.ready.deadline) { if (r.members[r.ready.mid]) r2_take(r, r.ready.mid, now); else { r.ready = null; r2_advance(r, now); } }   // (GET READY's 3 s up: seated)
		else if (t.phase === 'CONNECTING' && now >= t.deadline) r2_connect_failed(r, now);
		else if (t.phase === 'DECIDED' && (now >= t.deadline || r2_lone(r, now, alive))) r2_resolve(r, now, alive);
		else if (t.phase === 'PLAYING' && t.deadline && now >= t.deadline) { if (t.restart) r2_connect_failed(r, now); else r2_timeout(r, now); }
		else break;
		changed = true;
		if (r2_moved(r) === was) r2_stuck(r, now);
	}
	return changed;
}
function r2_stuck(r, now) {
	const t = r.t;
	t.last = { result: 'void', why: 'stuck', n: t.n };
	for (const x of [t.a, t.b]) if (x && r.members[x]) r.line.push(x);
	t.a = t.b = t.holder = null;
	t.streak = 0;
	t.phase = 'EMPTY';
	r2_clear_match(t);
	r.ready = null;
	r2_advance(r, now);
}
function r2_view(r, mid, now, watch_on) {
	const t = r.t, m = r.members[mid], side = r2_side(r, mid);
	const pub = (x) => { const p = x && r.members[x]; return p ? { name: p.who.name, country: p.who.country } : null; };
	const inl = (x) => { const p = r.members[x]; return p ? { name: p.who.name, country: p.who.country, ready: !p.away } : null; };
	let seat = null;
	if (side && t.a && t.b && t.token) {
		const p = r.members[t[r2_other(side)]];
		seat = { role: side === 'a' ? 'host' : 'guest', side, peer: { name: p.who.name, cands: p.who.cands, relay: p.who.relay, country: p.who.country }, token: t.token, as: t.as, them: p.who.version, n: t.n,
			seamless: m.caps.includes('cut1') && p.caps.includes('cut1'), ...(side === 'a' && t.armed && t.wkey ? { wkey: t.wkey } : {}) };
	}
	return { code: r.code, seq: r.seq || 0, room: { name: r.name, listed: r.listed, locked: r.locked, ready_ms: r.ready_ms, owner: r.owner === mid },
		table: { a: pub(t.a), b: pub(t.b), phase: t.phase, streak: t.streak, holder: r2_side(r, t.holder), n: t.n, last: t.last,
			match: t.match ? { n: t.match.n, streak: t.match.streak, holder: t.match.holder && r.members[t.match.holder] ? r.members[t.match.holder].who.name : '' } : null },
		line: r.line.map(inl),
		you: { mid, pos: side ? 0 : m.watcher ? -1 : r.line.indexOf(mid) + 1, ...(side ? { side } : {}), ...(m.watcher ? { watcher: true } : {}), seat, ready: !m.away },
		watchers: Object.values(r.members).filter((x) => x.watcher).length,
		ready: r.ready ? { pos: r.line.indexOf(r.ready.mid) + 1, deadline_ms: Math.max(0, r.ready.deadline - now), you: r.ready.mid === mid } : null,
		watch: watch_on ? r.wid : '' };
}
const r2_caps = (l) => (Array.isArray(l) ? l.slice(0, 8).filter((c) => typeof c === 'string' && /^[a-z0-9]{1,16}$/.test(c)) : []);
const r2_ready_ms = (x, was) => (Number.isFinite(x) ? Math.min(Math.max(Math.round(x), R2.READY_MIN_MS), R2.READY_MAX_MS) : was);

// The Worker's side: checked, limited, passed to the code's object
async function room2_route(env, req, url, op) {
	// (off: create, enter and the socket answer "off" (a new game makes a /v1 room, as before); a member's call goes on to
	// its room, which, still open, closes itself (its members told closed) and answers no_room: the game's room ended)
	if (!rooms2_on(env) && (op === 'ws' || op === 'create' || op === 'enter')) return json({ error: 'off' }, 404);
	if (op === 'ws') {
		if ((req.headers.get('upgrade') || '').toLowerCase() !== 'websocket') return json({ error: 'bad' }, 426);
		if (!sockets_on(env)) return off();   // (the game polls over HTTPS)
		if (await limited(env, req, 'WAIT_LIMIT')) return json({ error: 'busy' }, 429);
		const code = normal_code(url.searchParams.get('code'));
		if (!code_ok(code)) return json({ error: 'no_room' }, 404);
		const q = (k) => encodeURIComponent(clean(url.searchParams.get(k) || '', 64));
		return room(env, code).fetch(new Request('https://room/v2/ws?mid=' + q('mid') + '&mkey=' + q('mkey'), req));
	}
	if (req.method !== 'POST' || !R2_OPS.has(op)) return json({ error: 'bad' }, 404);
	if (await limited(env, req, 'ANY_LIMIT')) return json({ error: 'busy' }, 429);
	if (op === 'create') {
		if (await limited(env, req, 'HOST_LIMIT')) return json({ error: 'busy' }, 429);
		const b = await body_of(req);
		if (!b) return json({ error: 'bad' }, 400);
		const me = who(b, req);
		if (!GAMES.test(me.game) || !me.version) return json({ error: 'bad' }, 400);
		const ask = { me, caps: r2_caps(b.caps), sid: await player_id(req, me.name), pid: await player_id(req, '#member'), aid: await player_id(req, '#room', 3),
			listed: b.listed === true, locked: b.listed === true && b.locked === true, room: room_name(b.room), ready_ms: r2_ready_ms(+b.ready_ms, R2.READY_MS), region: region_of(req), build: r2_build(b.build) };
		// (Rooms 2 rooms made from one address a day (an IPv6 one by its /48), CREATES_PER_DAY (ROOMS2_CREATES_PER_DAY): each
		// one is the online count's to keep, so one address can't make them without end; the count not reached: not held up)
		const limits = env.LIMITS && env.LIMITS.get(env.LIMITS.idFromName('rooms2')), from = sender(req, 3);
		let counted = false;
		if (limits) try {
			const day = await limits.fetch('https://limits/', { method: 'POST', body: JSON.stringify({ per_day: 1e9, who: from, per_who: +env.ROOMS2_CREATES_PER_DAY || R2.CREATES_PER_DAY }) });
			if (day.status === 409) return json({ error: 'busy' }, 429);
			counted = day.status === 204;
		} catch (e) {}
		for (let tries = 0; tries < 8; tries++) {
			const code = random_code();
			const r = await room(env, code).fetch('https://room/v2/create', { method: 'POST', body: JSON.stringify({ ...ask, code }) });
			if (r.status === 200) return r;
			await r.body?.cancel();
		}
		if (counted) try { await limits.fetch('https://limits/', { method: 'POST', body: JSON.stringify({ refund: true, who: from }) }); } catch (e) {}
		return json({ error: 'busy' }, 429);
	}
	if (op === 'enter') {
		if (await new_code_limited(env, req)) return json({ error: 'busy' }, 429);   // (a code tried: as a /v1/join)
		const b = await body_of(req), code = normal_code(b && b.code);
		if (!b) return json({ error: 'bad' }, 400);
		if (!code_ok(code)) return json({ error: 'no_room' }, 404);
		const me = who(b, req);
		if (!GAMES.test(me.game) || !me.version) return json({ error: 'bad' }, 400);
		// (mid and mkey: the game's membership from before, a crash or a restart: taken back with its place if it's still a member)
		const back = typeof b.mid === 'string' && typeof b.mkey === 'string' ? { mid: clean(b.mid, 16), mkey: clean(b.mkey, 64) } : {};
		return room(env, code).fetch('https://room/v2/enter', { method: 'POST', body: JSON.stringify({ me, caps: r2_caps(b.caps), sid: await player_id(req, me.name), pid: await player_id(req, '#member'), ...back }) });
	}
	const b = await body_of(req), code = normal_code(b && b.code);
	if (!b) return json({ error: 'bad' }, 400);
	if (!code_ok(code)) return json({ error: 'no_room' }, 404);
	const ask = { mid: clean(b.mid, 16), mkey: clean(b.mkey, 64), owner_key: clean(b.owner_key, 64), winner: R2_RESULTS.has(b.winner) ? b.winner : '', n: Number.isInteger(b.n) ? b.n : null };
	if (op === 'settings') Object.assign(ask, { ...(typeof b.room === 'string' ? { room: room_name(b.room) } : {}), ...(typeof b.listed === 'boolean' ? { listed: b.listed } : {}),
		...(typeof b.locked === 'boolean' ? { locked: b.locked } : {}), ...(Number.isFinite(b.ready_ms) ? { ready_ms: b.ready_ms } : {}) });
	return room(env, code).fetch('https://room/v2/' + op, { method: 'POST', body: JSON.stringify(ask) });
}

// The object's side of a Rooms 2 room
Object.assign(Room.prototype, {
	v2_socket(mid) {   // (the member's open socket, not one closed for silence)
		return (this.ctx.getWebSockets ? this.ctx.getWebSockets('m:' + mid) : []).find((ws) => !this.quiet.has(ws) && !(this.closed2 && this.closed2.has(ws))) || null;
	},
	v2_alive(r, mid, now) {   // (for a result: its socket open, or its game polling since its socket's close)
		const m = r.members[mid];
		if (!m) return false;
		if (this.v2_socket(mid)) return true;
		if (m.grace && now <= m.grace) return true;   // (a restart's 30 s to come back, v2_woke: not taken for gone meanwhile)
		return m.polled > (m.ws_closed || 0) && now - m.polled <= R2.GONE_MS;
	},
	// The record read again (the object made anew). After hibernating, its members' sockets are still there: nothing to do.
	// After a restart (a deploy, a variable changed) they're gone with it, no close delivered, and a member's seen is from
	// its last call, maybe long ago (pings never wake the object): each member that was on a socket (m.ws; a record from
	// before it, any member) given R2.GONE_MS from now to come back, on a socket or polling, as after its socket's close;
	// once (m.ws false). True if any was
	// A member polling (no socket) has its seen written at most every SAVE_MS: on a wake with no socket at all (not a
	// hibernation's, which needs one: a restart, or the object let go and made again), it's given the same 30 s, once
	// till it's heard from again (m.graced)
	v2_woke(r) {
		const now = Date.now(), none = !(this.ctx.getWebSockets ? this.ctx.getWebSockets('m') : []).length;
		let any = false;
		for (const m of Object.values(r.members || {})) {
			if (this.v2_socket(m.mid)) continue;
			if (m.ws !== false) { m.ws = false; m.ws_closed = now; }
			else if (!none || m.graced) continue;
			m.graced = true;
			m.seen = Math.max(m.seen || 0, now);
			m.grace = now + R2.GONE_MS;
			any = true;
		}
		return any;
	},
	// Rooms 2 switched off (SWITCH_ROOMS2, or SWITCH_ROOMS): a room still open closes itself at its next alarm, call or
	// message (its members told {closed: true}, the online count told it's gone): true if so
	async v2_off() {
		if (rooms2_on(this.env)) return false;
		if (this.r && this.r.v === 2) await this.drop();
		return true;
	},
	// A member's calls (its socket's messages and its HTTPS calls together, by its id, in memory): MSG_BURST at once, then
	// one every MSG_EVERY_MS (ROOMS2_MSG_BURST, a Worker variable, raises the burst for a load test). True: over, dropped
	v2_over(mid, now) {
		const rates = this.rates2 || (this.rates2 = new Map()), burst = +this.env.ROOMS2_MSG_BURST || R2.MSG_BURST;
		let b = rates.get(mid);
		if (!b) rates.set(mid, (b = { n: burst, at: now, dropped: 0 }));
		b.n = Math.min(burst, b.n + Math.max(0, now - b.at) / R2.MSG_EVERY_MS);
		b.at = now;
		if (b.n >= 1) { b.n -= 1; b.dropped = 0; return false; }
		b.dropped++;
		return true;
	},
	// Members gone (a socket quiet 90 s: closed; no socket and no word for 30 s), then every deadline passed, then an
	// empty room's minute or an idle room's life: true if anything changed (the room dropped: this.r null)
	async v2_sweep(r, now) {
		let changed = false;
		for (const ws of this.ctx.getWebSockets ? this.ctx.getWebSockets('m') : []) {
			if (this.quiet.has(ws)) continue;
			const att = ws.deserializeAttachment() || {}, pong = this.ctx.getWebSocketAutoResponseTimestamp ? this.ctx.getWebSocketAutoResponseTimestamp(ws) : null;
			if (now - Math.max(att.at || 0, pong ? pong.getTime() : 0) <= R2.PING_GONE_MS) continue;
			this.quiet.add(ws);
			try { ws.close(1001, 'quiet'); } catch (e) {}
			if (r.members[att.mid] && !this.v2_socket(att.mid)) { r.members[att.mid].ws_closed = now; r.members[att.mid].ws = false; this.dirty2 = true; }
		}
		for (const [mid, m] of Object.entries(r.members))
			if (!this.v2_socket(mid) && now - m.seen > R2.GONE_MS) { r2_remove(r, mid, 'gone', now); changed = true; }
		if (r2_due(r, now, (mid) => this.v2_alive(r, mid, now))) changed = true;
		if ((!Object.keys(r.members).length && r.empty_since && now - r.empty_since >= R2.EMPTY_MS) || now - r.active > R2.LIFE_MS) {
			await this.drop();
			return true;
		}
		return changed;
	},
	// Kept (at every change; otherwise, only its members' liveness moved, every SAVE_MS), the Watch armed for a new pair,
	// every member's socket told, the online count told (at most every TELL_GAP_MS), the next alarm set (the soonest
	// deadline)
	// A new pair's seat (t.arm) is told at once, with the change's seq, as any change; the Watch armed after it, in the
	// background (never between a change and its seq: a poll meanwhile would get the new seat under the old seq; nor
	// before the call's answer: an op or an enter never waits on it), and told again once armed (side a's wkey)
	async v2_commit(changed, save = false) {
		const r = this.r;
		if (!r) return;
		const t = r.t, arm = t.arm;
		if (arm) { t.arm = false; changed = true; }
		const now = Date.now();
		if (changed) r.seq = (r.seq || 0) + 1;   // (a change: the states told from now on carry the next seq, kept with the record)
		// (dirty2: a member's socket gone, kept at once: else a hibernation's wake would take it for a restart's, v2_woke)
		const write = changed || save || this.dirty2 || !this.saved2 || now - this.saved2 >= R2.SAVE_MS;
		if (write) { this.dirty2 = false; await this.ctx.storage.put('r', r); this.saved2 = now; }
		if (changed) {
			const watch_on = switch_on(this.env, 'SWITCH_WATCH');
			for (const ws of this.ctx.getWebSockets ? this.ctx.getWebSockets('m') : []) {
				const att = ws.deserializeAttachment() || {};
				if (r.members[att.mid]) try { ws.send(JSON.stringify({ state: r2_view(r, att.mid, now, watch_on) })); } catch (e) {}
				else {   // (a member let go (gone, can't play): told why, its socket closed)
					const x = (r.removed || []).find((y) => y.mid === att.mid);
					try { ws.send(JSON.stringify({ removed: x ? x.why : 'gone' })); ws.close(1000, 'removed'); } catch (e) {}
				}
			}
			if (this.rates2) for (const mid of this.rates2.keys()) if (!r.members[mid]) this.rates2.delete(mid);
		}
		this.v2_tell(changed);   // (a change; or one held back, its second up; or the TELL_MS that it's there)
		let next = now + 30000;   // (a socket's silence looked at every 30 s)
		for (const [mid, m] of Object.entries(r.members)) if (!this.v2_socket(mid)) next = Math.min(next, m.seen + R2.GONE_MS + 500);
		if (r.ready) next = Math.min(next, r.ready.deadline);
		if ((r.t.phase === 'CONNECTING' || r.t.phase === 'DECIDED' || r.t.phase === 'PLAYING') && r.t.deadline) next = Math.min(next, r.t.deadline);
		if (r.t.phase === 'DECIDED' && r.t.decide_at > now) next = Math.min(next, r.t.decide_at);   // (a lone report: looked at then, and every 30 s after)
		if (r.empty_since) next = Math.min(next, r.empty_since + R2.EMPTY_MS);
		if (this.tell2_due) next = Math.min(next, (this.told2 || 0) + R2.TELL_GAP_MS);
		next = Math.min(next, r.active + R2.LIFE_MS + 1000);
		// (a deadline already passed, after the sweep: never meant to be (r2_due moves every one on); a second, not 100 ms)
		const at = Math.max(next, now + (next <= now ? 1000 : 100));
		if (write || !this.alarm2 || this.alarm2 <= now || at < this.alarm2) { await this.ctx.storage.setAlarm(at); this.alarm2 = at; }
		if (arm) {   // (one arming at a time, each with the pair's key as it is when it runs: never the Watch left on an older pair's)
			const armed = (this.arming = (this.arming || Promise.resolve()).then(() => this.v2_arm(), () => this.v2_arm()))
				.then(() => { if (this.r && this.r.t.armed && this.r.t.wkey) return this.v2_commit(true); })   // (armed: side a told its wkey, a change)
				.catch(() => {});
			if (this.ctx.waitUntil) this.ctx.waitUntil(armed);
		}
	},
	// The room's Watch armed with the pair seated now's key (t.wkey), again while it's changed meanwhile; armed: the Watch
	// took it (t.armed_key, the key it has)
	async v2_arm() {
		for (let i = 0; i < 4; i++) {
			const r = this.r;
			if (!r || !r.t.wkey) return;
			const wkey = r.t.wkey;
			if (r.t.armed_key === wkey) { r.t.armed = true; return; }
			const ok = switch_on(this.env, 'SWITCH_WATCH') && (await arm(this.env, r.wid, { wid: r.wid, wkey, game: r.game, version: r.version, listed: false, region: r.region, aid: r.aid }));
			const now = this.r;
			if (!now) return;
			if (ok) now.t.armed_key = wkey;
			if (now.t.wkey === wkey) { now.t.armed = ok; return; }
		}
	},
	// The online count: who's in it, the table and the line, the room for the list if it's listed. A change (force) told
	// at once, or, told within the last TELL_GAP_MS, by the alarm then (changes in between told as one, the last as it
	// is); otherwise every TELL_MS that it's there
	v2_tell(force) {
		const r = this.r, now = Date.now();
		if (!r) return;
		if (force) this.tell2_due = true;
		const since = now - (this.told2 || 0);
		if ((!this.tell2_due && since < R2.TELL_MS) || since < R2.TELL_GAP_MS) return;
		this.tell2_due = false;
		const t = r.t, order = [t.a, t.b, ...r.line, ...Object.keys(r.members).filter((x) => r.members[x].watcher)].filter((x) => x && r.members[x]).map((x) => r.members[x]);
		const seat = (x) => (x && r.members[x] ? [r.members[x].who.name, r.members[x].who.country] : null);
		const ev = { op: 'room2', code: r.code, game: r.game, version: r.version, playing: !!(t.a && t.b && R2_MATCH.has(t.phase)), listed: r.listed, locked: r.locked, room: r.name, region: r.region,
			aid: r.aid, wid: switch_on(this.env, 'SWITCH_WATCH') ? r.wid : '', table: [seat(t.a), seat(t.b)], streak: t.streak, line: r.line.length,
			ready: r.line.filter((x) => r.members[x] && !r.members[x].away).length, phase: t.phase,
			sids: order.map((m) => m.sid), countries: order.map((m) => m.who.country) };
		const said = JSON.stringify(ev);
		if (said === this.told2_said && since < R2.TELL_MS) return;   // (what it last told, as it is: nothing new to tell (READY / AWAY toggled back))
		this.told2 = now;
		this.told2_said = said;
		tell(this, ev);
	},
	// SWITCH_WEBSOCKETS off: its members' sockets closed (at the next alarm or message, as a /v1 host's are), each member
	// then polling (its game's socket refused, 503 off): given GONE_MS from now to be heard from, and taken as alive for a
	// result meanwhile (the lobby closed it, not its game); true if any was
	v2_sockets_off(r, now) {
		if (sockets_on(this.env)) return false;
		let any = false;
		for (const ws of this.ctx.getWebSockets ? this.ctx.getWebSockets('m') : []) {
			if (this.quiet.has(ws) || (this.closed2 && this.closed2.has(ws))) continue;
			const att = ws.deserializeAttachment() || {}, m = r.members[att.mid];
			this.quiet.add(ws);
			try { ws.close(1012, 'off'); } catch (e) {}
			any = true;
			if (!m || this.v2_socket(att.mid)) continue;
			m.ws = false; m.ws_closed = now; m.seen = Math.max(m.seen || 0, now); m.grace = now + R2.GONE_MS;
		}
		if (any) this.dirty2 = true;
		return any;
	},
	async v2_alarm() {
		this.alarm2 = 0;
		if (await this.v2_off()) { if (this.gone) await this.ctx.storage.setAlarm(this.gone.until + 1000); return; }
		const r = this.r, now = Date.now();
		this.v2_sockets_off(r, now);
		const changed = await this.v2_sweep(r, now);
		if (!this.r) { if (this.gone) await this.ctx.storage.setAlarm(this.gone.until + 1000); return; }
		this.v2_tell(false);
		await this.v2_commit(changed);
	},
	v2_member(r, b) {   // (the member by its id and key, or null)
		const m = r.members[b.mid];
		return m && typeof b.mkey === 'string' && b.mkey.length === 32 && b.mkey === m.mkey ? m : null;
	},
	// (not a member: a member let go, by its own key, told why; anything else, as a code with no room (no_room 404), so
	// a member's calls don't tell a room's code from a made-up one: only enter is charged as a code tried)
	v2_gone_why(r, b) {
		const x = (r.removed || []).find((y) => y.mid === b.mid && y.mkey === b.mkey);
		return x ? json({ error: 'removed', why: x.why }, 410) : json({ error: 'no_room' }, 404);
	},
	v2_new_member(r, b, now) {
		let mid;
		do mid = random_hex(4); while (r.members[mid] || (r.removed || []).some((x) => x.mid === mid));
		return { mid, mkey: random_hex(16), who: b.me, caps: r2_caps(b.caps), sid: clean(b.sid, 16), pid: clean(b.pid, 16), seen: now, polled: now, ws_closed: 0, ws: false, away: false, joined: now, watcher: false };
	},
	// A member back (its game crashed or restarted, within the 30 s it's kept): its new addresses and caps, its old socket
	// let go, its place as it was. At the table its session is gone with its game, and the other's (its token) is no good
	// to the new one. A match starting or on: the two seated again cold, once (the pair's one re-seat, t.reseated, shared
	// with a disconnect's); after that, a match on is forfeited by the one back (else a loser could void every match it's
	// losing by coming back), one starting is its failure to start. Decided already by the other side's report: settled by
	// it (a win, a loss or a draw as they said; a desync or disconnect, forfeited by the one back), never voided. Its own
	// report alone in: the other's still waited for. After a match (READY), or decided: if the two play on, they start
	// their session again (t.lost)
	v2_reclaim(r, m, b, now) {
		for (const old of this.ctx.getWebSockets ? this.ctx.getWebSockets('m:' + m.mid) : []) try { this.quiet.add(old); old.close(1000, 'replaced'); } catch (e) {}
		m.who = b.me;
		m.caps = r2_caps(b.caps);
		this.v2_heard(m, now);
		m.polled = now;
		m.ws_closed = 0;
		const t = r.t, side = r2_side(r, m.mid);
		if (!side || !t.a || !t.b) return;
		const other = r2_other(side), n = t.n;
		if (t.phase === 'READY') { t.lost = true; return; }
		if (t.phase === 'DECIDED') {
			t.lost = true;
			const theirs = t.reports[other];
			if (!theirs) return;
			if (theirs === 'a' || theirs === 'b' || theirs === 'draw') { t.reports[side] = theirs; r2_resolve(r, now, () => true); return; }
			r2_won(r, other, false, { result: 'forfeit', winner: other, n }, now);
			return;
		}
		if (t.phase !== 'CONNECTING' && t.phase !== 'PLAYING') return;
		if (!t.reseated) {
			t.last = { result: 'void', why: 'rejoin', n };
			r2_reseat(r, now, true);
		} else if (t.phase === 'CONNECTING') r2_unseat(r, side, 'connect', now);
		else {
			t.lost = true;
			r2_won(r, other, false, { result: 'forfeit', winner: other, n }, now);
		}
	},
	// (a member heard from (a call, a message, its socket): its seen now; a restart's grace over. True if the grace's
	// flag was set (kept at once, so the next restart gives it the grace again))
	v2_heard(m, now) {
		m.seen = now;
		m.grace = 0;
		if (!m.graced) return false;
		m.graced = false;
		return true;
	},
	// What a member does: by HTTPS or its socket alike
	v2_act(r, m, op, b, now) {
		if (op === 'poll') return null;
		if (op === 'ready') return r2_ready(r, m.mid, now);
		if (op === 'away') return r2_away(r, m.mid, now);
		if (op === 'started') return r2_started(r, m.mid, now, Number.isInteger(b.n) ? b.n : null);
		if (op === 'result') {
			return r2_result(r, m.mid, b.winner, Number.isInteger(b.n) ? b.n : null, now, (x) => this.v2_alive(r, x, now));
		}
		if (op === 'leave') { r2_remove(r, m.mid, 'left', now); return null; }
		if (op === 'leave_line') return r2_leave_line(r, m.mid, now);
		if (op === 'join_line') return r2_join_line(r, m.mid, now);
		return 'bad';
	},
	async v2(op, req) {
		const now = Date.now(), watch_on = switch_on(this.env, 'SWITCH_WATCH');
		if (!rooms2_on(this.env)) {   // (switched off: a room still open closes now, and the call finds none; a /v1 room's object left as it is)
			const r = this.r !== undefined ? this.r : (await this.ctx.storage.get('r')) || null;
			if (r && r.v === 2) { if (this.gone === undefined) this.gone = (await this.ctx.storage.get('gone')) || null; this.r = r; await this.v2_off(); }
			await req.body?.cancel();
			return json({ error: 'no_room' }, 404);
		}
		if (op === 'ws') {
			const u = new URL(req.url), r = await this.load();
			if (!r || r.v !== 2) return json({ error: 'no_room' }, 404);
			const b = { mid: u.searchParams.get('mid') || '', mkey: u.searchParams.get('mkey') || '' }, m = this.v2_member(r, b);
			if (!m) return this.v2_gone_why(r, b);
			if (this.v2_over(m.mid, now)) return json({ error: 'busy' }, 429);
			for (const old of this.ctx.getWebSockets('m:' + m.mid)) try { this.quiet.add(old); old.close(1000, 'replaced'); } catch (e) {}
			const [client, server] = Object.values(new WebSocketPair());
			this.ctx.acceptWebSocket(server, ['m', 'm:' + m.mid]);
			server.serializeAttachment({ mid: m.mid, at: now });
			const graced = this.v2_heard(m, now);
			const fresh = m.ws !== true || graced;   // (on a socket now, kept: a restart before its close gives it time to come back, v2_woke)
			m.ws = true;
			const changed = await this.v2_sweep(r, now);
			if (!this.r) return json({ error: 'no_room' }, 404);
			await this.v2_commit(changed, fresh);
			if (this.r && this.r.members[m.mid]) server.send(JSON.stringify({ state: r2_view(this.r, m.mid, Date.now(), watch_on) }));
			return new Response(null, { status: 101, webSocket: client });
		}
		const b = await req.json();
		if (op === 'create') {
			const was = await this.load();
			if (was || this.gone) return json({ error: 'taken' }, 409);
			const me = b.me;
			this.r = { v: 2, code: b.code, game: me.game, content: me.content, version: me.pool || me.version, base: me, born: now, active: now, owner_key: random_hex(16), owner: '',
				name: room_name(b.room), listed: !!b.listed, locked: !!b.locked, ready_ms: r2_ready_ms(+b.ready_ms, R2.READY_MS), region: typeof b.region === 'string' ? b.region.slice(0, 2) : '',
				aid: hex16(b.aid), wid: b.code, members: {}, line: [], t: r2_table(), ready: null, empty_since: 0, removed: [], build: r2_build(b.build) };
			const r = this.r, m = this.v2_new_member(r, b, now);
			r.owner = m.mid;
			r2_enter(r, m, now);
			await this.v2_commit(true);
			return json({ code: r.code, owner_key: r.owner_key, mid: m.mid, mkey: m.mkey, wid: watch_on ? r.wid : '', state: r2_view(r, m.mid, now, watch_on) });
		}
		const r = await this.load();
		if (!r) return json({ error: 'no_room' }, 404);
		if (r.v !== 2) return op === 'enter' ? json({ error: 'room_version', room_version: 1 }, 412) : json({ error: 'no_room' }, 404);
		this.v2_sockets_off(r, now);   // (SWITCH_WEBSOCKETS off: its members' sockets closed now, as at its alarm)
		// (a member's calls over MSG_BURST at once, then one every MSG_EVERY_MS: busy, nothing done)
		if (op !== 'enter' && op !== 'settings' && op !== 'close' && this.v2_member(r, b) && this.v2_over(b.mid, now)) return json({ error: 'busy' }, 429);
		const swept = await this.v2_sweep(r, now);
		if (!this.r) return json({ error: 'no_room' }, 404);
		if (op === 'enter') {
			const me = b.me;
			if (me.game !== r.game) return json({ error: 'game' }, 412);
			if (terms(me, r.base) === null) return json({ error: 'version', host_version: r.base.version }, 412);
			if (me.content !== r.content) return json({ error: 'rom' }, 412);
			const was = b.mid && b.mkey ? this.v2_member(r, b) : null;
			if (was) {   // (its membership from before taken back, its place kept: a crash or a restart within its 30 s)
				// (at the table, back with a version its opponent can't play (another build): out, its match forfeited, as one
				// that can't play the one who stays is (r2_take); told so, as an older build is)
				const side = r2_side(r, was.mid), opp = side && r.members[r.t[r2_other(side)]];
				if (opp && terms(me, opp.who) === null) {
					r2_remove(r, was.mid, 'version', now);
					await this.v2_commit(true);
					return json({ error: 'version', host_version: opp.who.version }, 412);
				}
				// (still there: its socket open, or, polling, heard from in the last LIVE_MS and not since a socket's close: not a
				// game come back but the same one gathering its link again (its new addresses), maybe just as a ready seated it:
				// its addresses and caps taken, nothing else (no void, no re-seat, its socket kept))
				const live = !!this.v2_socket(was.mid) || (now - was.seen <= R2.LIVE_MS && (was.ws_closed || 0) < was.seen);
				if (live) {
					was.who = me;
					was.caps = r2_caps(b.caps);
					this.v2_heard(was, now);
					was.polled = Math.max(now, (was.ws_closed || 0) + 1);
				} else this.v2_reclaim(r, was, b, now);
				await this.v2_commit(true);
				// (the Watch's arming waited for (a re-seat): the room closed, or this member gone (it left, by its other link)
				// meanwhile: said as any call of its would be)
				if (!this.r) return json({ error: 'no_room' }, 404);
				if (!this.r.members[was.mid]) return this.v2_gone_why(this.r, b);
				return json({ code: r.code, mid: was.mid, mkey: was.mkey, wid: watch_on ? r.wid : '', back: true, state: r2_view(this.r, was.mid, Date.now(), watch_on) });
			}
			const members = Object.values(r.members);
			if (members.length >= R2.MEMBERS_MAX || members.filter((x) => x.pid && x.pid === clean(b.pid, 16)).length >= (+this.env.ROOMS2_PER_ADDRESS || R2.PER_ADDRESS)) {
				if (swept) await this.v2_commit(true);
				return json({ error: 'full' }, 409);
			}
			const m = this.v2_new_member(r, b, now);
			r2_enter(r, m, now);
			await this.v2_commit(true);
			if (!this.r) return json({ error: 'no_room' }, 404);   // (closed while its seat's Watch was armed)
			if (!r.members[m.mid]) {   // (seated at once against one it can't play (another build than the room's creator's): let go, r2_take)
				const x = r.members[r.t.a || r.t.b];
				return json({ error: 'version', ...(x ? { host_version: x.who.version } : {}) }, 412);
			}
			return json({ code: r.code, mid: m.mid, mkey: m.mkey, wid: watch_on ? r.wid : '', state: r2_view(r, m.mid, Date.now(), watch_on) });
		}
		if (op === 'settings' || op === 'close') {   // (the creator's: by its key, whether it's still in the room or not)
			if (typeof b.owner_key !== 'string' || b.owner_key.length !== 32 || b.owner_key !== r.owner_key) {
				if (swept) await this.v2_commit(true);
				return this.v2_member(r, b) ? json({ error: 'not_owner' }, 403) : json({ error: 'no_room' }, 404);   // (only a member's told it's not the owner)
			}
			if (op === 'close') { await this.drop(); return json({ ok: true }); }
			if (typeof b.room === 'string') r.name = room_name(b.room);
			if (typeof b.listed === 'boolean') r.listed = b.listed;
			if (typeof b.locked === 'boolean') r.locked = b.locked;
			if (!r.listed) r.locked = false;
			r.ready_ms = r2_ready_ms(+b.ready_ms, r.ready_ms);
			await this.v2_commit(true);
			return json({ ok: true, room: { name: r.name, listed: r.listed, locked: r.locked, ready_ms: r.ready_ms } });
		}
		const m = this.v2_member(r, b);
		if (!m) { if (swept) await this.v2_commit(true); return this.v2_gone_why(r, b); }
		const graced = this.v2_heard(m, now);
		m.polled = Math.max(now, (m.ws_closed || 0) + 1);   // (a call: polling since any close known (one a restart's wake took for its socket's, made by this call, v2_woke, in the same ms))
		const sig = r2_sig(r);
		const e = this.v2_act(r, m, op, b, now);
		const changed = swept || r2_sig(r) !== sig;
		await this.v2_commit(changed, graced);
		if (op === 'leave') return json({ ok: true });
		const still = this.r && this.r.members[m.mid];
		if (!still) return this.r ? this.v2_gone_why(this.r, b) : json({ error: 'no_room' }, 404);
		return json({ ok: !e, ...(e ? { error: e } : {}), state: r2_view(this.r, m.mid, Date.now(), watch_on) }, e === 'bad' ? 400 : 200);
	},
	async v2_message(ws, att, msg) {
		if (msg === 'ping') { try { ws.serializeAttachment({ ...att, at: Date.now() }); ws.send('pong'); } catch (e) {} return; }
		let b = null;
		if (msg === 'ready' || msg === 'away') b = { op: msg };
		else if (typeof msg === 'string' && msg.length <= 1024) try { b = JSON.parse(msg); } catch (e) {}
		if (!b || typeof b !== 'object' || !['poll', 'ready', 'away', 'started', 'result', 'leave', 'leave_line', 'join_line'].includes(b.op)) {
			// (one that isn't a message (over 1 KB, binary, not JSON, no op it knows): nothing done, nothing answered, but
			// counted against the member's rate as any (else a flood of them cost the object without end): MSG_CLOSE of them
			// past it in a row, its socket closed)
			if (this.v2_over(att.mid, Date.now()) && this.rates2.get(att.mid).dropped >= R2.MSG_CLOSE) try { ws.close(1008, 'busy'); } catch (e) {}
			return;
		}
		const r = await this.load(), now = Date.now();
		if (await this.v2_off()) { try { ws.close(1000, 'gone'); } catch (e) {} return; }
		if (!r || r.v !== 2) { try { ws.close(1000, 'gone'); } catch (e) {} return; }
		// (SWITCH_WEBSOCKETS off: every member's socket closed, this one's message not taken: its game says it again over HTTPS)
		if (this.v2_sockets_off(r, now)) { const changed = await this.v2_sweep(r, now); if (this.r) await this.v2_commit(changed); return; }
		// (a member's messages over its rate: dropped; MSG_CLOSE of them in a row, its socket closed (its game comes back on
		// a new one, or polls: still a member, 30 s to do so))
		if (r.members[att.mid] && this.v2_over(att.mid, now)) {   // (each said so: {error: "busy", op}, its game to say it again later)
			try { ws.send(JSON.stringify({ error: 'busy', op: b.op })); } catch (e) {}
			if (this.rates2.get(att.mid).dropped >= R2.MSG_CLOSE) try { ws.close(1008, 'busy'); } catch (e) {}
			return;
		}
		const swept = await this.v2_sweep(r, now);
		if (!this.r) return;
		const m = r.members[att.mid];
		if (!m) { try { ws.send(JSON.stringify({ removed: 'gone' })); ws.close(1000, 'gone'); } catch (e) {} return; }
		const graced = this.v2_heard(m, now);
		try { ws.serializeAttachment({ ...att, at: now }); } catch (e) {}
		const sig = r2_sig(r);
		const e = this.v2_act(r, m, b.op, { winner: R2_RESULTS.has(b.winner) ? b.winner : '', n: Number.isInteger(b.n) ? b.n : null }, now);
		await this.v2_commit(swept || r2_sig(r) !== sig, graced);
		if (e) try { ws.send(JSON.stringify({ error: e, op: b.op })); } catch (x) {}
		if (b.op === 'leave') try { ws.close(1000, 'left'); } catch (x) {}
		else if (b.op === 'poll' && this.r && this.r.members[att.mid]) try { ws.send(JSON.stringify({ state: r2_view(this.r, att.mid, Date.now(), switch_on(this.env, 'SWITCH_WATCH')) })); } catch (x) {}
	},
	// (a member's socket closed: 30 s to come back, on a socket or polling; one closed for silence, not. A close delivered is
	// no restart: a hibernated object woken by it (v2_woke gave the member a restart's grace, its socket closed by now) takes
	// it as it is, the member not alive for a result till it's heard from again (a game crashed mid-match forfeits it). One
	// the lobby closed itself (quiet: for silence, or SWITCH_WEBSOCKETS off) keeps what it was given then)
	async v2_closed(att, quiet) {
		const r = await this.load();
		if (!r || r.v !== 2) return;
		if (await this.v2_off()) return;
		const m = r.members[att.mid];
		if (m && !this.v2_socket(att.mid)) {
			m.ws_closed = Date.now(); m.ws = false; this.dirty2 = true;
			if (!quiet) { m.seen = Math.max(m.seen, Date.now()); m.grace = 0; m.graced = false; }
		}
		const changed = await this.v2_sweep(r, Date.now());
		if (this.r) await this.v2_commit(changed);
	},
});

// The relay's logins given out today (UTC), all players together: kept, so a restart doesn't forget them
// (and each address's: a new day, all forgotten)
export class Limits {
	constructor(ctx) {
		this.ctx = ctx;
	}
	async fetch(req) {
		const { per_day, who, per_who, refund } = await req.json();
		const day = Math.floor(Date.now() / 86400e3);
		let c = (await this.ctx.storage.get('c')) || { day, n: 0 };
		if (c.day !== day) {
			await this.ctx.storage.deleteAll();
			c = { day, n: 0 };
		}
		const k = 'a:' + String(who || '').slice(0, 64);
		const a = who ? (await this.ctx.storage.get(k)) || 0 : 0;
		if (refund) {   // (a login asked for that didn't come: not counted)
			c.n = Math.max(0, c.n - 1);
			await this.ctx.storage.put({ c, [k]: Math.max(0, a - 1) });
			return new Response(null, { status: 204 });
		}
		if (c.n >= per_day) return new Response(null, { status: 429 });
		if (who && per_who && a >= per_who) return new Response(null, { status: 409 });
		c.n++;
		await this.ctx.storage.put(who ? { c, [k]: a + 1 } : { c });
		return new Response(null, { status: 204 });
	}
}

// Those searching for a random opponent (one queue per game, version's base and ROM; in it, only those terms()
// finds a match for meet). Each is matched with the nearest other, by where Cloudflare has them: close at first, further the longer
// either has waited, anyone after 40 s. The one who waited longer hosts: a room's opened as theirs, the other
// joined into it, and from there it's a room's match like any other (its code kept for Play again). Kept in memory
// only: those searching ask every 20 s, and one gone quiet 25 s is dropped.
const RANDOM_WAIT_MS = 20000, RANDOM_GONE_MS = 25000, RANDOM_KEPT_MS = 60000, RANDOM_PER_ADDRESS = 6, RANDOM_PLAYING_MS = 600000, RANDOM_MATCHES_PER_ADDRESS = 120;   // (high: a phone network's address is many players')
function reach_km(seconds) {
	return seconds < 8 ? 1500 : seconds < 20 ? 4000 : seconds < 40 ? 9000 : Infinity;
}
function km(a, b) {
	if (!a || !b) return 5000;   // (not known: far, so met after a while, not before everyone)
	const r = Math.PI / 180, dlat = (b.lat - a.lat) * r, dlon = (b.lon - a.lon) * r;
	const h = Math.sin(dlat / 2) ** 2 + Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(dlon / 2) ** 2;
	return 12742 * Math.asin(Math.min(1, Math.sqrt(h)));
}

export class Queue {
	constructor(ctx, env) {
		this.ctx = ctx;
		this.env = env;
		this.waiting = new Map();   // ticket -> { me, geo, from, since, seen, result, waiters, partner, room, left }
		this.matched = [];          // (when each match was made, the last RANDOM_PLAYING_MS: for the count; kept in storage,
		                            // the queue being let go of by Cloudflare once no one's searching)
		this.made = new Map();      // address -> when its matches were made, the last RANDOM_PLAYING_MS
		this.told = { sig: '', at: 0, busy: false, again: false, events: [] };   // (what the online count last heard: sync)
		this.pairs = new Map();     // a match's room code -> its two players ({from, name}) and when, the last RANDOM_PLAYING_MS:
		                            // a player passing the other over (too far for their ping limit) names the room, and the
		                            // two aren't matched again meanwhile (kept in storage too: the two are connecting, no one
		                            // asking, when the queue's let go of)
		ctx.blockConcurrencyWhile(async () => {
			this.matched = (await ctx.storage.get('matched')) || [];
			this.pairs = new Map((await ctx.storage.get('pairs')) || []);
		});
	}
	prune() {
		const now = Date.now();
		for (const [t, w] of this.waiting)
			if (now - w.seen > (w.result ? RANDOM_KEPT_MS : RANDOM_GONE_MS)) this.waiting.delete(t);
		for (const [a, times] of this.made)
			if (!times.length || now - times[times.length - 1] > RANDOM_PLAYING_MS) this.made.delete(a);
		for (const [c, p] of this.pairs) if (now - p.t > RANDOM_PLAYING_MS) this.pairs.delete(c);
		this.sync();
	}
	// The online count told who's searching here (each player once) and the matches made, when that changes, and every
	// 15 s while anyone searches (its entry lapses a minute after it's last heard: a queue no one asks any more); one
	// telling at a time, 2 s apart at most often, so a busy queue tells it a few times a minute, not at every request
	sync() {
		const t = this.told;
		if (t.busy) { t.again = true; return; }
		if (!this.game) return;
		const now = Date.now();
		// (each searching once, with their country: the lobby's own; by version, a queue having all of a base's: the
		// count's variants are the versions')
		const by = new Map();
		for (const w of this.waiting.values()) {
			if (w.result || !w.sid) continue;
			const v = w.me.pool || w.me.version;   // (a pool's player counted as the pool's: who it meets, not its own settings)
			const of = by.get(v) || new Map();
			if (!of.get(w.sid)) of.set(w.sid, country_ok(w.me.country));
			by.set(v, of);
		}
		const lists = new Map();
		for (const [v, of] of by) { const sids = [...of.keys()].sort(); lists.set(v, { sids, countries: sids.map((x) => of.get(x)) }); }
		for (const v of t.versions || []) if (!lists.has(v)) lists.set(v, { sids: [], countries: [] });   // (one gone: its entry cleared)
		const sig = [...lists].map(([v, l]) => v + ':' + l.sids.map((x, i) => x + l.countries[i]).join(',')).sort().join(';');
		const anyone = [...by.keys()].length > 0;
		if (sig === t.sig && !t.events.length && !(anyone && now - t.at > 15000)) return;
		t.sig = sig;
		t.at = now;
		t.busy = true;
		t.versions = [...by.keys()];
		let events = t.events.splice(0);
		if (!lists.size) lists.set(this.version, { sids: [], countries: [] });
		let done = Promise.resolve();
		for (const [v, l] of lists) {   // (the events with the first: a match's own version beside it)
			done = tell(this, { op: 'queue', key: this.ctx.id.toString() + '|' + v, game: this.game, version: v, sids: l.sids.slice(0, 2000), countries: l.countries.slice(0, 2000), events });
			events = [];
		}
		done = done.then(() => new Promise((ok) => setTimeout(ok, this.env.TURN_STATIC ? 0 : 2000)))   // (a test's own lobby: no pause, its tellings checked as they come)
			.then(() => {
				t.busy = false;
				if (t.again) { t.again = false; this.sync(); }
			});
		try { this.ctx.waitUntil(done); } catch (e) {}
	}
	// (one of those this one passed over, or was passed over by)
	apart(w, o) {
		const is = (x, y) => x.from === y.from && x.me.name === y.name;
		return w.avoid.some((p) => is(o, p)) || o.avoid.some((p) => is(w, p));
	}
	searching(me) {   // (those me could be matched with, if said: else all)
		let n = 0;
		for (const w of this.waiting.values()) if (!w.result && (!me || terms(me, w.me) !== null)) n++;
		return n;
	}
	// The nearest other one searching that this one may be matched with now: only one waiting on an answer this
	// moment (a game gone without saying so has none open)
	nearest(ticket, w) {
		const now = Date.now();
		let pick = null, best = Infinity;
		for (const [t, o] of this.waiting) {
			if (t === ticket || o.result || o.waiters.length === 0 || this.apart(w, o) || terms(w.me, o.me) === null) continue;
			const d = km(w.geo, o.geo), waited = Math.max(now - w.since, now - o.since) / 1000;
			if (d <= reach_km(waited) && d < best) { pick = o; best = d; }
		}
		return pick;
	}
	async try_match(ticket, w) {
		const o = this.nearest(ticket, w);
		if (!o) return;
		const host = o.since <= w.since ? o : w, guest = host === o ? w : o;
		w.result = o.result = 'pairing';   // (both taken before anything's awaited: no one else picks them meanwhile)
		let m = null;
		try {
			m = await this.open(host, guest);
		} catch (e) {
			m = null;
		}
		if (m && (host.left || guest.left)) {   // (one gave up while the room was made: the other searches on)
			await this.close(m.host.code, m.host.key);
			m = null;
		}
		if (!m) {
			for (const x of [host, guest]) if (!x.left) { x.result = null; for (const done of x.waiters.splice(0)) done(); }
			return;
		}
		const t = Date.now();
		this.matched = this.matched.filter((x) => t - x < RANDOM_PLAYING_MS);
		this.matched.push(t);
		this.ctx.storage.put('matched', this.matched);   // (not awaited: nothing's to run between here and the settling; the write's kept before any answer goes)
		for (const x of [host, guest]) this.made.set(x.from, [...(this.made.get(x.from) || []).filter((y) => t - y < RANDOM_PLAYING_MS), t]);
		this.pairs.set(m.host.code, { t, players: [host, guest].map((x) => ({ from: x.from, name: x.me.name })) });
		for (const [c, p] of this.pairs) if (t - p.t > RANDOM_PLAYING_MS) this.pairs.delete(c);
		this.ctx.storage.put('pairs', [...this.pairs]);   // (not awaited, as matched's)
		this.told.events.push({ op: 'match', code: m.host.code, version: host.me.pool || host.me.version, sids: [host.sid, guest.sid], countries: [host.me.country, guest.me.country], t });
		host.partner = guest;
		guest.partner = host;
		host.room = guest.room = { code: m.host.code, key: m.host.key };
		this.settle(host, m.host);
		this.settle(guest, m.guest);
		this.sync();
	}
	async open(host, guest) {
		for (let tries = 0; tries < 8; tries++) {
			const code = random_code();
			const r = await room(this.env, code).fetch('https://room/open', { method: 'POST', body: JSON.stringify({ code, host: host.me, aid: host.aid || '', random: true, region: host.region || '', quick_listed: host.me.qm_listed === true && guest.me.qm_listed === true }) });
			if (r.status !== 200) continue;
			const h = await r.json();
			const j = await room(this.env, code).fetch('https://room/join', { method: 'POST', body: JSON.stringify({ guest: guest.me }) });
			if (j.status !== 200) { await this.close(code, h.key); return null; }
			const g = await j.json();
			return {
				host: { role: 'host', code, key: h.key, token: h.token, name: guest.me.name, country: guest.me.country, as: g.as, them: guest.me.version, ...(h.wid ? { wid: h.wid, wkey: h.wkey } : {}) },
				guest: { role: 'guest', code, name: g.name, cands: g.cands, relay: g.relay, token: g.token, country: g.country, as: g.as, them: g.them, ...(g.wid ? { wid: g.wid } : {}) },
			};
		}
		return null;
	}
	async close(code, key) {
		try {
			await room(this.env, code).fetch('https://room/close', { method: 'POST', body: JSON.stringify({ key }) });
		} catch (e) {}
	}
	settle(w, result) {
		w.result = result;
		w.seen = Date.now();
		for (const done of w.waiters.splice(0)) done();
	}
	// (a match is kept till RANDOM_KEPT_MS, not dropped once said: an answer lost on the way is asked for again)
	answer(ticket, w) {
		if (w.result && w.result !== 'pairing') return json(w.result);
		return json({ ticket, waiting: true, players: this.searching(w.me) });
	}
	async fetch(req) {
		const op = new URL(req.url).pathname;
		const b = await req.json();
		this.prune();
		const now = Date.now();
		if (op === '/count') {
			this.matched = this.matched.filter((t) => now - t < RANDOM_PLAYING_MS);
			return json({ players: this.searching(b.me) + 2 * this.matched.length });
		}
		if (op === '/enter') {
			let mine = 0;   // (a few tickets an address at most: no one place soaks up everyone searching)
			for (const o of this.waiting.values()) if (o.from === b.from && !o.result) mine++;
			if (mine >= (+this.env.RANDOM_PER_ADDRESS || RANDOM_PER_ADDRESS)) return json({ error: 'busy' }, 429);   // (a variable may raise it: staging's, for tools/lobbyload)
			// (and so many matches an address lately: no one place collects every player searching)
			const made = (this.made.get(b.from) || []).filter((y) => now - y < RANDOM_PLAYING_MS);
			if (made.length) this.made.set(b.from, made); else this.made.delete(b.from);
			if (made.length >= (+this.env.RANDOM_MATCHES_PER_ADDRESS || RANDOM_MATCHES_PER_ADDRESS)) return json({ error: 'busy' }, 429);
			const ticket = random_hex(12);
			// (the other player of each room it names: those it's kept apart from)
			const avoid = [];
			for (const c of Array.isArray(b.avoid) ? b.avoid : []) {
				const p = this.pairs.get(c);
				if (p) for (const x of p.players) if (x.from !== b.from || x.name !== b.me.name) avoid.push(x);
			}
			const w = { me: b.me, geo: b.geo, region: typeof b.region === 'string' ? b.region.slice(0, 2) : '', from: b.from, sid: clean(b.sid, 16), aid: hex16(b.aid), avoid, since: now, seen: now, result: null, waiters: [], partner: null, room: null, left: false };
			this.waiting.set(ticket, w);
			if (!this.game) { this.game = b.me.game; this.version = b.me.version; }   // (one game and version's base a queue: the version first seen, for the count)
			this.sync();
			await this.try_match(ticket, w);
			return this.answer(ticket, w);
		}
		const w = this.waiting.get(b.ticket);
		if (!w) return json({ error: 'gone' }, 404);
		w.seen = now;
		if (op === '/leave') {
			this.waiting.delete(b.ticket);
			w.left = true;   // (mid-pairing: try_match sees it)
			const o = w.partner;
			if (o && !o.left && w.room) {   // (matched already: the room's gone, the other back to searching)
				this.told.events.push({ op: 'unmatch', code: w.room.code });
				await this.close(w.room.code, w.room.key);
				o.result = null;
				o.partner = o.room = null;
				o.since = Date.now();
				for (const done of o.waiters.splice(0)) done();
			}
			this.sync();
			return json({ ok: true });
		}
		if (op === '/wait') {
			if (!w.result) await this.try_match(b.ticket, w);
			if (!w.result || w.result === 'pairing')
				await new Promise((done) => {
					for (const x of w.waiters.splice(0)) x();   // (one wait at a time)
					let timer = 0;
					const finish = () => {
						clearTimeout(timer);
						w.waiters = w.waiters.filter((x) => x !== finish);
						done();
					};
					w.waiters.push(finish);
					timer = setTimeout(finish, RANDOM_WAIT_MS);
				});
			w.seen = Date.now();
			return this.answer(b.ticket, w);
		}
		return json({ error: 'bad' }, 400);
	}
}

// Who's online (GET /v1/online): one Durable Object, told by the rooms and the random queues what they saw (a host
// waiting, a player joining, who's searching, a match made), so a page view costs one read of it, not one of every
// queue; and that read kept 30 s (Cloudflare's edge, and each copy of the Worker's memory). Only what the lobby saw:
// the games never say how many there are. Each player once (their address and name, hashed: player_id), in what
// they're doing now (searching, then hosting, then their latest match), one count a game (who one can
// meet: its pool, POOLS), and of them those on an older version ("older"; the version a game sends never shown).
const ROOMS_MAX = 100, ROOMS_PER_ADDRESS = 6, WATCH_LISTED_MS = 75000;   // (ROOMS_PER_ADDRESS: listed from one address, its newest: a phone
                                                                          // network's many hosts, not one script's whole list)   // (a watched match's host sends every 30 s while nobody watches)
const QUEUE_GONE_MS = 15000, QUEUE_MAX = 32;   // (a persistent room's line: one not asking this long let go; at most this many)   // (the rooms list: a game's most; a watched match not heard of this long is off it)
const ONLINE_FRESH_MS = 15000, ONLINE_PLAYING_MS = 600000, ONLINE_HOST_MS = 100000, ONLINE_QUEUE_MS = 60000;
// Each game's pool (its NET_VERSION's base with its pool flag): its players meet everyone of it, whatever their
// settings, and those older versions they can play; anyone else (an older version's player) counted as "older". VF1
// has no pool and nothing that splits: all its players are its current ones. A game's new NET_VERSION or pool: here.
const POOLS = { vf1: null, vf2: 'v0.1.16+mix1', vf3: 'v0.1.7-online-preview+host1', vf3tb: 'v0.1.7-online-preview+host1' };
const VF1_BASES = ['v1.5.0'];
function variant_of(game, version) {
	if (!(game in POOLS) || typeof version !== 'string') return 'older';
	if (game === 'vf1') return VF1_BASES.includes(version) ? 'current' : 'older';
	return version === POOLS[game] ? 'current' : 'older';
}

// (a room's or queue's telling, one after another: a room opened then closed is heard in that order; a telling that
// fails is let go, the games' play never waits on it)
function tell(from, ev) {
	from.telling = (from.telling || Promise.resolve())
		.then(() => from.env.STATS.get(from.env.STATS.idFromName('all')).fetch('https://stats/tell', { method: 'POST', body: JSON.stringify(ev) }))
		.then((r) => r.body && r.body.cancel())
		.catch(() => {});
	try { from.ctx.waitUntil(from.telling); } catch (e) {}
	return from.telling;
}

let online_kept = null;   // ({t, body}: this copy of the Worker's last answer, t when the counts were made)
// (every copy, this one's and the edge's, kept only while the counts themselves are under ONLINE_FRESH_MS old: one
// copy made from another never adds its age to it)
const fresh_at = (body) => { try { return JSON.parse(body).updated * 1000; } catch (e) { return 0; } };
async function online(env, req) {
	const r = await online_counts(env, req);
	if (r.status !== 200) return r;
	// (what's on, the switches': a game or the site shows "Rooms aren't available right now" without rooms, as with
	// an older lobby that has no such field; the rooms list left out, or only the open ones, as they say)
	const rooms_on = switch_on(env, 'SWITCH_ROOMS') && switch_on(env, 'SWITCH_LISTING'), watch_on = switch_on(env, 'SWITCH_WATCH'), r2 = rooms2_on(env);
	let body = await r.text();
	if (!rooms_on || !watch_on || (!r2 && body.includes('"v":2'))) {
		try {
			const d = JSON.parse(body);
			if (!r2 && d.rooms) d.rooms = d.rooms.filter((x) => x.v !== 2);   // (Rooms 2 switched off: none listed, every other row as it is)
			if (!rooms_on) delete d.rooms;
			else if (!watch_on) d.rooms = (d.rooms || []).filter((x) => x.state === 'waiting' || x.v === 2).map(({ watch, watchers, score, round, ...x }) => (x.room2 ? { ...x, room2: { code: x.room2.code } } : x));   // (nothing to watch)
			body = JSON.stringify(d);
		} catch (e) {}
	}
	// (the asker's own continent, as Cloudflare places them, beside the kept counts: "near me" in the rooms list)
	const you = region_of(req), add = ',"features":{"rooms":' + rooms_on + ',"watch":' + watch_on + (r2 && rooms_on ? ',"rooms2":true' : '') + '}' + (you ? ',"you":"' + you + '"' : '');
	return new Response(body.endsWith('}') ? body.slice(0, -1) + add + '}' : body, { status: 200, headers: r.headers });
}
// (not limited per address: the online page's function asks from Cloudflare's own addresses, many visitors' asks from
// one, and a game's from a phone network's shared one. Being asked often costs nothing more than a request each: each
// Worker copy's memory and the edge answer it, the Stats asked about once a copy every ONLINE_FRESH_MS)
async function online_counts(env, req) {
	const headers = { 'content-type': 'application/json', 'cache-control': 'public, max-age=10', 'access-control-allow-origin': '*' };
	const kept_ok = !env.TURN_STATIC;   // (not on a test's own lobby: its counts checked as they change)
	if (kept_ok && online_kept && Date.now() - online_kept.t < ONLINE_FRESH_MS) return new Response(online_kept.body, { headers });
	const key = new Request('https://online.vf-lobby/v1');
	const edge = kept_ok && (await caches.default.match(key));
	if (edge) {
		const body = await edge.text(), t = fresh_at(body);
		if (Date.now() - t < ONLINE_FRESH_MS) { online_kept = { t, body }; return new Response(body, { headers }); }
	}
	const r = await env.STATS.get(env.STATS.idFromName('all')).fetch('https://stats/online', { method: 'POST', body: '{}' });
	const body = await r.text();
	if (r.status !== 200) return new Response('{"error":"server"}', { status: 500, headers: { ...headers, 'cache-control': 'no-store' } });
	if (kept_ok) {
		online_kept = { t: fresh_at(body) || Date.now(), body };
		await caches.default.put(key, new Response(body, { headers: { ...headers, 'cache-control': 'public, max-age=30' } }));
	}
	return new Response(body, { headers });
}

export class Stats {
	constructor(ctx, env) {
		this.ctx = ctx;
		this.env = env;
		this.s = { queues: {}, hosts: {}, matches: [], watches: {} };
		// Kept in storage an entry a key (each well under the 128 KiB a value may be, however many play: a queue's 2,000
		// searching at most is the largest), written as each changes: 'q:' a queue's, 'h:' a host's, 'w:' a watched match's,
		// 'm:' a match made, 'month' the month's watched hours (so a restart keeps the month's ceiling where it was)
		this.dirty = new Map();   // (key -> its entry, to write)
		this.gone = new Set();    // (keys to delete)
		ctx.blockConcurrencyWhile(async () => {
			const old = await ctx.storage.get('s');   // (one value, as kept before: taken apart, the next write its keys)
			if (old) {
				const s = (this.s = { queues: {}, hosts: {}, matches: [], watches: {}, ...old });
				for (const [k, q] of Object.entries(s.queues)) this.dirty.set('q:' + k, q);
				for (const [k, h] of Object.entries(s.hosts)) this.dirty.set('h:' + k, h);
				for (const [k, w] of Object.entries(s.watches)) this.dirty.set('w:' + k, w);
				for (const m of s.matches) { m.k = 'm:' + random_hex(8); this.dirty.set(m.k, m); }
				if (s.month) this.dirty.set('month', s.month);
				this.gone.add('s');
				return;
			}
			const s = this.s;
			for (const [k, v] of await ctx.storage.list()) {
				if (k.startsWith('q:')) s.queues[k.slice(2)] = v;
				else if (k.startsWith('h:')) s.hosts[k.slice(2)] = v;
				else if (k.startsWith('w:')) s.watches[k.slice(2)] = v;
				else if (k.startsWith('m:')) s.matches.push(v);
				else if (k === 'month') s.month = v;
			}
			s.matches.sort((a, b) => a.t - b.t);
		});
	}
	set(kind, id, v) { const k = kind[0] + ':' + id; this.s[kind][id] = v; this.dirty.set(k, v); this.gone.delete(k); }
	del(kind, id) { if (!(id in this.s[kind])) return; const k = kind[0] + ':' + id; delete this.s[kind][id]; this.dirty.delete(k); this.gone.add(k); }
	touch(kind, id) { if (id in this.s[kind]) this.dirty.set(kind[0] + ':' + id, this.s[kind][id]); }
	add_match(m) { m.k = 'm:' + random_hex(8); this.s.matches.push(m); this.dirty.set(m.k, m); }
	drop_matches(gone) {
		const keep = [];
		for (const m of this.s.matches) if (gone(m)) { this.dirty.delete(m.k); if (m.k) this.gone.add(m.k); } else keep.push(m);
		this.s.matches = keep;
	}
	async flush() {   // (what changed written, what went deleted: 128 keys a call, as Cloudflare takes them)
		const dels = [...this.gone], puts = [...this.dirty];
		this.gone.clear();
		this.dirty.clear();
		for (let i = 0; i < dels.length; i += 128) await this.ctx.storage.delete(dels.slice(i, i + 128));
		for (let i = 0; i < puts.length; i += 128) await this.ctx.storage.put(Object.fromEntries(puts.slice(i, i + 128)));
	}
	prune() {
		const now = Date.now(), s = this.s;
		for (const [k, q] of Object.entries(s.queues)) if (now > q.until) this.del('queues', k);
		for (const [k, h] of Object.entries(s.hosts)) if (now - h.seen > ONLINE_HOST_MS) this.del('hosts', k);   // (a room that stays may outlive ROOM_LIFE_MS from its first match: its host's tellings keep it)
		const old = s.matches.filter((m) => now - m.t >= ONLINE_PLAYING_MS).length, over = Math.max(0, s.matches.length - old - 20000);
		if (old || over) { let i = 0; this.drop_matches((m) => now - m.t >= ONLINE_PLAYING_MS || (now - m.t < ONLINE_PLAYING_MS && i++ < over)); }
		for (const [k, w] of Object.entries(s.watches)) if (now - w.seen > WATCH_LISTED_MS) this.del('watches', k);
	}
	async fetch(req) {
		const op = new URL(req.url).pathname;
		const b = await req.json();
		this.prune();
		if (op === '/online') {
			const out = json(this.count());
			if (this.gone.size || this.dirty.size) await this.flush();   // (those let go meanwhile: deleted)
			return out;
		}
		const s = this.s, now = Date.now();
		// (each player told of, with their country beside them: [sid, country], a country not well formed "")
		const pairs = (l, cs) => (Array.isArray(l) ? l : []).map((x, i) => [x, country_ok(Array.isArray(cs) ? cs[i] : '')]).filter(([x]) => typeof x === 'string' && /^[0-9a-f]{16}$/.test(x));
		if (!GAMES.test(b.game) || typeof b.version !== 'string') return json({ error: 'bad' }, 400);
		const at = { game: b.game, version: b.version.slice(0, 64) };
		const code = typeof b.code === 'string' ? b.code.slice(0, 8) : '';
		const unmatch = (c, kind) => this.drop_matches((m) => m.code === c && m.kind === kind);
		if (b.op === 'queue') {
			// (its key: the queue's id, 64 hex digits on Cloudflare, then the version: kept whole, each version its own entry.
			// Cut to 64 as it was, every version of a queue was one entry, the last told: a queue with an older build's player
			// and new ones searching counted only one of the two lists)
			const key = String(b.key).slice(0, 160), sids = [], countries = [];
			if (key.length > 64) this.del('queues', key.slice(0, 64));   // (its entry as kept before this: gone at once, not counted beside it for its last minute)
			for (const [sid, c] of pairs(b.sids, b.countries)) if (!sids.includes(sid) && sids.length < 2000) { sids.push(sid); countries.push(c); }
			if (sids.length) this.set('queues', key, { ...at, sids, countries, until: now + ONLINE_QUEUE_MS });
			else this.del('queues', key);
			for (const e of Array.isArray(b.events) ? b.events.slice(0, 1000) : []) {
				const c = typeof e.code === 'string' ? e.code.slice(0, 8) : '';
				if (e.op === 'match') {
					unmatch(c, 'random');
					const p = pairs(e.sids, e.countries).slice(0, 2);
					// (its own version, what it plays as: told with the first list's, whatever version that is, before)
					const v = typeof e.version === 'string' && e.version ? e.version.slice(0, 64) : at.version;
					this.add_match({ ...at, version: v, kind: 'random', code: c, sids: p.map((x) => x[0]), countries: p.map((x) => x[1]), t: Math.min(now, +e.t || now) });
				} else if (e.op === 'unmatch') unmatch(c, 'random');
			}
		} else if (b.op === 'watch' || b.op === 'unwatch') {
			// (a match that may be watched: its players by name, the score, the watchers; gone once over)
			const wid = typeof b.wid === 'string' && code_ok(b.wid) ? b.wid : '';
			if (!wid) return json({ error: 'bad' }, 400);
			// (the month's watched time: each match's since its last telling, a minute at most between two; while nobody
			// watched it, 0.4 of it: its host sent every 30 s, its Watch asleep between)
			const month = new Date(now).toISOString().slice(0, 7);
			if (!s.month || s.month.key !== month) s.month = { key: month, ms: 0 };
			const was = s.watches[wid];
			if (was) s.month.ms += Math.min(Math.max(now - was.seen, 0), 60000) * (was.watchers > 0 ? 1 : UNWATCHED_WEIGHT);
			this.dirty.set('month', s.month);
			const p = (Array.isArray(b.p) ? b.p : []).slice(0, 2).map((x) => ({ name: Array.isArray(x) ? clean(x[0], 10) : '', country: Array.isArray(x) ? country_ok(x[1]) : '' }));
			const nums = (l) => (Array.isArray(l) ? l : []).slice(0, 2).map((x) => Math.min(Math.max(x | 0, 0), 255));
			this.tally(wid, p, nums(b.wins), hex16(b.mid));
			if (b.op === 'unwatch') this.del('watches', wid);
			else this.set('watches', wid, { ...at, listed: !!b.listed, random: !!b.random && !b.listed, region: String(b.region || '').slice(0, 2), aid: hex16(b.aid), p, wins: nums(b.wins), rounds: nums(b.rounds),
				watchers: Math.min(Math.max(b.watchers | 0, 0), 1e6), seen: now, born: Math.min(now, +b.born || now) });
			await this.flush();
			let watchers = 0;
			for (const w of Object.values(s.watches)) watchers += w.watchers | 0;
			const hours = s.month.ms / 3600e3;
			return json({ matches: Object.keys(s.watches).length, watchers, month_hours: Math.round(hours * 10) / 10, over: hours >= month_ceiling(this.env) });   // (the load on watching, everywhere: the Watches shed it by this)
		} else if (b.op === 'host' || b.op === 'reopen' || b.op === 'hold' || (b.op === 'join' && b.persist === true)) {   // (a room that stays, listed or not: its host kept, in its match)
			if (b.op === 'reopen') unmatch(code, 'room');
			const [p] = pairs(b.sids, b.countries);
			// (a public room's host by name and where, for the rooms list; anyone else's, only counted)
			const listed = b.listed === true ? { listed: true, aid: hex16(b.aid), name: clean(b.name, 10), room: room_name(b.room), locked: b.locked === true, region: String(b.region || '').slice(0, 2),
				playing: b.playing === true || b.op === 'join', p2: Array.isArray(b.p2) ? { name: clean(b.p2[0], 10), country: country_ok(b.p2[1]) } : null,
				queue: Math.min(Math.max(b.queue | 0, 0), 99), wid: typeof b.wid === 'string' && code_ok(b.wid) ? b.wid : '' } : {};
			if (b.op === 'join') {   // (a persistent room's match: counted as one, and listed as playing)
				unmatch(code, 'room');
				const two = pairs(b.sids, b.countries).slice(0, 2);
				this.add_match({ ...at, kind: 'room', code, sids: two.map((x) => x[0]), countries: two.map((x) => x[1]), t: now });
			}
			// (playing: a room that stays, its match on; its host counted as in that match, not as hosting)
			const playing = b.playing === true || b.op === 'join';
			// (a room that stays: its line's players, and its match's other player while the room says it's on; a room that
			// says nothing of them (not one that stays) has none)
			const line = pairs(b.qsids, b.qcountries).slice(0, QUEUE_MAX), [guest] = playing ? pairs([b.gsid], [b.gcountry]) : [];
			const stays = { queued: line, guest: guest || null };
			const was = s.hosts[code], tally = was && was.sid === (p && p[0]) && was.tally ? { tally: was.tally } : {};   // (a room that stays, open again: the score between its two kept)
			if (p && b.op !== 'join') this.set('hosts', code, { ...at, sid: p[0], country: p[1], seen: now, born: (was && was.born) || now, in_match: playing, ...listed, ...stays, ...tally });
			else if (s.hosts[code]) { Object.assign(s.hosts[code], { seen: now, in_match: playing }, listed, stays); this.touch('hosts', code); }
		} else if (b.op === 'join') {
			this.del('hosts', code);
			unmatch(code, 'room');
			const p = pairs(b.sids, b.countries).slice(0, 2);
			this.add_match({ ...at, kind: 'room', code, sids: p.map((x) => x[0]), countries: p.map((x) => x[1]), t: now });
		} else if (b.op === 'room2') {
			// (a Rooms 2 room: its members, the table's first then the line's, each counted once as a room's player (the two
			// at the table in a room's match while one's on); listed, the list's row: the table's two, the streak, the line)
			if (!code_ok(code)) return json({ error: 'bad' }, 400);
			const all = pairs(b.sids, b.countries).slice(0, R2.MEMBERS_MAX), playing = b.playing === true && all.length >= 2;
			const seat = (x) => (Array.isArray(x) ? { name: clean(x[0], 10), country: country_ok(x[1]) } : null);
			const num = (x, hi) => Math.min(Math.max(x | 0, 0), hi);
			if (!all.length) this.del('hosts', code);
			else this.set('hosts', code, { ...at, v: 2, sid: all[0][0], country: all[0][1], seen: now, born: (s.hosts[code] && s.hosts[code].born) || now, in_match: playing,
				guest: playing ? all[1] : null, queued: all.slice(playing ? 2 : 1), listed: b.listed === true, locked: b.listed === true && b.locked === true, room: room_name(b.room),
				region: String(b.region || '').slice(0, 2), aid: hex16(b.aid), wid: typeof b.wid === 'string' && code_ok(b.wid) ? b.wid : '',
				table: (Array.isArray(b.table) ? b.table : []).slice(0, 2).map(seat), streak: num(b.streak, 9999), line: num(b.line, 99), ready: num(b.ready, 99), phase: typeof b.phase === 'string' ? b.phase.slice(0, 12) : '' });
		} else if (b.op === 'gone') this.del('hosts', code);
		else return json({ error: 'bad' }, 400);
		await this.flush();
		return new Response(null, { status: 204 });
	}
	// A listed room's score: the matches won between its host and the player it has now, all its matches together (a room
	// that stays: each match a new one, its Watch's score from 0; the same two again, Play again or back from its line,
	// go on from where they were), and kept when its Watch is told no more while it plays (its host's uploads stopped:
	// the lobby not reached for a minute, the month's watching used up; its line said 0-0 till the room closed). From
	// its Watch's tellings: the match's id (a new one: the last one's score put by), the two players, the score now
	tally(wid, p, wins, mid) {
		const h = this.s.hosts[wid];
		if (!h || h.v === 2 || h.wid !== wid || p.length !== 2 || wins.length !== 2) return;
		const pair = p.map((x) => same_name(x.name)).join('|'), was = JSON.stringify(h.tally || null);
		let t = h.tally;
		if (!t || t.pair !== pair) t = { pair, done: [0, 0], cur: [0, 0], mid };
		else if (mid && t.mid && t.mid !== mid) t = { pair, done: [0, 1].map((i) => Math.min(t.done[i] + t.cur[i], 9999)), cur: [0, 0], mid };
		else if (mid) t.mid = mid;
		t.cur = [0, 1].map((i) => Math.max(t.cur[i], wins[i]));   // (a match's score only goes up: a start sent again, 0-0 for a moment, isn't taken)
		h.tally = t;
		if (JSON.stringify(t) !== was) this.touch('hosts', wid);   // (written only when it changed: a Watch tells every 5-20 s)
	}
	// The rooms list: public rooms waiting for an opponent (to join), and the matches anyone may watch, newest first,
	// ROOMS_MAX a game at most. A player's name only where they chose to be listed (Create room: Listed), or played a
	// Quick match on a build that sends its matches to be watched (every Quick match can be: listed as one, to watch)
	rooms() {
		const s = this.s, now = Date.now(), out = [], per = {}, per2 = {}, seen_wids = new Set();
		// (a game's ROOMS_MAX rows of rooms and matches as ever, and its Rooms 2 rows a ROOMS_MAX of their own: a Rooms 2 room
		// never pushes an older build's row off its list)
		const room = (r) => { const p = r.v === 2 ? per2 : per; p[r.game] = (p[r.game] || 0) + 1; if (p[r.game] <= ROOMS_MAX) out.push(r); };
		const older = (x) => variant_of(x.game, x.version) === 'older';
		const watch_of = (wid) => (wid && s.watches[wid]) || null;
		// listed rooms: waiting for a challenger, or (a persistent one) playing, its match's score and watchers beside it
		const hosts = Object.entries(s.hosts).filter(([, h]) => h.listed && h.name && h.v !== 2).sort((a, b) => b[1].born - a[1].born);
		const hosts2 = Object.entries(s.hosts).filter(([, h]) => h.v === 2 && h.listed && h.table && h.table.some((x) => x && x.name)).sort((a, b) => b[1].born - a[1].born);
		// (from one address, ROOMS_PER_ADDRESS at most, its newest, rooms and matches together: one address can't fill the list)
		// (the rows of rooms and matches counted first, by themselves as ever, then the Rooms 2 rows, newest first, in what's
		// left of the address's: its Rooms 2 rooms never take its older build's rows' places)
		const by_host = new Set([...hosts, ...hosts2].map(([, h]) => h.wid).filter(Boolean)), over = new Set(), from = {};
		const v1 = [...hosts.map(([code, h]) => ['h' + code, h]), ...Object.entries(s.watches).filter(([wid, w]) => (w.listed || w.random) && !by_host.has(wid)).map(([wid, w]) => ['w' + wid, w])];
		const per_address = +this.env?.ROOMS_PER_ADDRESS || ROOMS_PER_ADDRESS;
		for (const list of [v1, hosts2.map(([code, h]) => ['r' + code, h])])
			for (const [k, x] of list.sort((a, b) => b[1].born - a[1].born)) if (x.aid && (from[x.aid] = (from[x.aid] || 0) + 1) > per_address) over.add(k);
		for (const [code, h] of hosts) {
			if (over.has('h' + code)) continue;
			const w = watch_of(h.wid);
			if (w) seen_wids.add(h.wid);
			const base = { game: h.game, name: h.room || '', locked: !!h.locked, p1: { name: h.name, country: h.country || '' }, region: h.region || '', since: Math.round((now - h.born) / 1000),
				older: older(h), queue: h.queue | 0, watchers: w ? w.watchers : 0, ...(h.locked ? {} : { code }) };
			if (h.playing && (h.p2 || w)) {
				const p2 = h.p2 && h.p2.name ? h.p2 : w && w.p[1] && w.p[1].name ? w.p[1] : null;
				if (!p2) continue;
				// (a locked room's match: no watch code, which is the room's own code, the one its lock keeps from the list)
				// (the score: the room's tally between these two, its match's now with it; no Watch, its last known)
				const t = h.tally && h.tally.pair === [h.name, p2.name].map(same_name).join('|') ? h.tally : null;
				const score = t ? [0, 1].map((i) => t.done[i] + (w ? Math.max(t.cur[i], w.wins[i] | 0) : t.cur[i])) : w ? w.wins : [0, 0];
				room({ ...base, state: 'playing', p2, score, ...(w ? { ...(h.locked ? {} : { watch: h.wid }), round: (w.rounds[0] | 0) + (w.rounds[1] | 0) + 1 } : { round: 0 }) });
			} else if (!h.playing) room({ ...base, state: 'waiting' });
		}
		// Rooms 2 rooms (v: 2): the table's two, the streak, the line's length, its code and watch code (not a locked one's)
		// under room2. No p1, code or watch of their own: an older build's list skips a row without them (online.cpp's
		// rooms_list: no code, no watch code, not locked; then no p1's name), as the online page does (its p1)
		for (const [code, h] of hosts2) {
			if (over.has('r' + code)) continue;
			const w = h.in_match ? watch_of(h.wid) : null;
			room({ v: 2, game: h.game, state: h.in_match ? 'playing' : 'waiting', name: h.room || '', locked: !!h.locked, region: h.region || '', since: Math.round((now - h.born) / 1000), older: older(h),
				table: h.table, streak: h.streak | 0, line: h.line | 0, ready: h.ready | 0, phase: h.phase || '', watchers: w ? w.watchers : 0,
				...(w ? { score: w.wins, round: (w.rounds[0] | 0) + (w.rounds[1] | 0) + 1 } : {}), ...(h.locked ? {} : { room2: { code, ...(w ? { watch: h.wid } : {}) } }) });
		}
		// a listed room's match whose room isn't kept (no line after it): from its Watch (an unlisted room's never: by its code
		// only); and a Quick match's, once its host's game sends it (a new build: an old one's never here), to watch only (no code)
		const ws = Object.entries(s.watches).filter(([wid, w]) => (w.listed || w.random) && !seen_wids.has(wid) && w.p.length === 2 && w.p[0].name && w.p[1].name)
			.sort((a, b) => b[1].watchers - a[1].watchers || b[1].born - a[1].born);
		for (const [wid, w] of ws)
			if (!over.has('w' + wid)) room({ game: w.game, state: 'playing', name: '', locked: false, watch: wid, ...(w.random ? { random: true } : { code: wid }), p1: w.p[0], p2: w.p[1], score: w.wins,
				round: (w.rounds[0] | 0) + (w.rounds[1] | 0) + 1, watchers: w.watchers, queue: 0, region: w.region, since: Math.round((now - w.born) / 1000), older: older(w) });
		return out;
	}
	// Each game: everyone once, in what they're doing now, by variant
	count() {
		const s = this.s, now = Date.now();
		const best = {};   // game -> sid -> {rank, t, what, variant, country}
		const put = (game, version, sid, rank, t, what, country) => {
			const g = (best[game] = best[game] || new Map()), o = g.get(sid);
			country = country_ok(country) || (o ? o.country : '');   // (a player's country: what any of theirs had, if this one hasn't)
			if (!o || rank > o.rank || (rank === o.rank && t > o.t)) g.set(sid, { rank, t, what, variant: variant_of(game, version), country });
			else if (!o.country) o.country = country;
		};
		const c_at = (l, i) => (Array.isArray(l) ? l[i] : '');   // (kept before countries were: none, so "")
		for (const q of Object.values(s.queues)) q.sids.forEach((sid, i) => put(q.game, q.version, sid, 3, q.until, 'searching', c_at(q.countries, i)));
		for (const h of Object.values(s.hosts)) {   // (a room that stays, its match on: its host in a room's match, not hosting)
			if (h.in_match) put(h.game, h.version, h.sid, 1, h.seen, 'room', h.country);
			else put(h.game, h.version, h.sid, 2, h.seen, 'hosting', h.country);
			if (h.in_match && Array.isArray(h.guest)) put(h.game, h.version, h.guest[0], 1, h.seen, 'room', h.guest[1]);   // (its other player)
			for (const [sid, c] of Array.isArray(h.queued) ? h.queued : []) put(h.game, h.version, sid, 2, h.seen, 'queued', c);   // (in its line: waiting to play)
		}
		for (const m of s.matches) m.sids.forEach((sid, i) => put(m.game, m.version, sid, 1, m.t, m.kind === 'random' ? 'random' : 'room', c_at(m.countries, i)));
		const zero = () => ({ total: 0, searching: 0, random: 0, hosting: 0, queued: 0, room: 0 });
		// (watching: those watching a match of the game now, as each match's Watch counts them; apart from total, which is
		// the players: a watcher plays no one, and may be a player too, counted there as one)
		const watching = {};
		for (const w of Object.values(s.watches)) watching[w.game] = (watching[w.game] || 0) + (w.watchers | 0);
		// (players by country, most first: a country's two letters, "" not known)
		const by_country = (m) => Object.fromEntries([...m].sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1)));
		const games = {}, all = new Map();
		for (const game of Object.keys(POOLS)) {
			const g = { ...zero(), older: 0 }, lands = new Map();
			for (const p of (best[game] || new Map()).values()) {
				g.total++;
				lands.set(p.country, (lands.get(p.country) || 0) + 1);
				all.set(p.country, (all.get(p.country) || 0) + 1);
				g[p.what]++;
				if (p.variant === 'older') g.older++;
			}
			g.countries = by_country(lands);
			g.watching = watching[game] || 0;
			games[game] = g;
		}
		return { updated: Math.floor(now / 1000), games, countries: by_country(all), rooms: this.rooms() };
	}
}

// ---- Watching a match (src/net/watch.h has the bytes; docs/netplay.md, Watching) ----
// One Watch a watch code: armed by the room that made it (its host's key), it keeps the match on now in memory: its
// start, every frame's inputs from the first, the players' state hashes and the scoreboard, as the host sends them
// once a second; and the watchers, who ask for the frames from where they are. The players never wait on any of it:
// the host's sending is on a thread of its own, and a watcher only ever asks. A crowd of watchers costs the Watch
// little: the frames are answered in pieces of PIECE, and a whole piece (and a match's start) never changes, so
// Cloudflare's edge keeps it and answers everyone near it; the piece still filling is kept a second.
const PIECE = 256, PIECES = 8, WATCH_FRAMES = 500000, START_MAX = (4 << 20) + 8192;   // (a start at most: watch.h's MAX_START and its head)
const WATCH_MAX = 400;     // watching one match at most: one more is told "full" (its game: Too many watching right now)
const STREAMS_MAX = WATCH_MAX;   // (watchers sent the frames as they come by one Watch; any past that ask the edge's pieces)
const STREAMS_PER_IP = 8;  // (streams from one address to one match: a LAN party's, not a script's thousand)
const WATCHERS_PER_IP = 32;   // (watchers from one address to one match, however they watch: a phone network's crowd, not
                              // one script's 400 made-up ones keeping everyone else out as "full")
// Shedding load, all matches together (the Stats' count, told back every 20 s): past SOFT new watchers ask the edge's
// pieces, less often; past HARD they're told "busy" (their game: Watching is busy right now). The hosts send less often
// past MATCHES_SOFT/HARD (a watcher further behind; the players never notice: their sending is on a thread)
const WATCHERS_SOFT = 4000, WATCHERS_HARD = 8000, MATCHES_SOFT = 500, MATCHES_HARD = 2000;
// The month's spend ceiling: watched match-hours this calendar month (UTC), as the Stats add them up (an hour nobody
// watched counts 0.4: its Watch asleep between uploads every 30 s, it and its room cost about that of a watched one
// at worst). Past it, a new watcher is told "busy" and the hosts stop sending (answered off: their game sends no more;
// the match goes on). 2,200 keeps a month near $15 in all even if Cloudflare bills each object for all the time it's
// awake (the worst case: a watched match-hour 536 GB-s, its Watch and its room); if the dashboard shows it billed only
// while handling messages, it can be far higher.
const MONTH_MATCH_HOURS = 2200, UNWATCHED_WEIGHT = 0.4;
// (the ceiling, a MONTH_MATCH_HOURS variable on the Worker changing it: "0" no watching at all; empty or not a number, 2,200)
function month_ceiling(env) {
	const v = String((env && env.MONTH_MATCH_HOURS) ?? '').trim();
	return v !== '' && Number.isFinite(+v) && +v >= 0 ? +v : MONTH_MATCH_HOURS;
}
// Nobody watching: the host sends every IDLE_EVERY_MS (one message, the frames since), so the object hibernates
// between; the first watcher to come wakes it at once (a message down its socket, which costs nothing) and it's 5 a
// second from then. (A host on HTTPS hears it at its next upload.) Quiet that long is no sign the match is over: it's
// taken for over after WATCH_QUIET_IDLE_MS then
const IDLE_EVERY_MS = 30000, WATCH_QUIET_IDLE_MS = 75000;
// (and so the match is kept in the Watch's storage as well while nobody watches: Cloudflare takes a Durable Object with
// only hibernating WebSockets out of memory after some 10 s idle, every idle gap, and its memory with it. Kept: the start
// in pieces of KEPT_PART, the frames in blocks of FRAME_BLOCK with their hashes (each value well under the 128 KiB a
// value may be), written at each idle upload; read back when it wakes without the match. Without it each idle upload
// found nothing, and the host sent the start and every frame again.)
const KEPT_PART = 120 << 10, FRAME_BLOCK = 16384, START_PARTS = Math.ceil(START_MAX / KEPT_PART), FRAME_BLOCKS = Math.ceil(WATCH_FRAMES / FRAME_BLOCK);
const KEPT_KEYS = ['mm', ...Array.from({ length: START_PARTS }, (_, i) => 'ms' + i), ...Array.from({ length: FRAME_BLOCKS }, (_, i) => 'mf' + i)];   // (67: one get, one delete; 128 at most)
const WATCH_QUIET_MS = 30000;      // no word from the host this long: the match is over (its game closed, its connection gone)
const WATCHER_MS = 40000;          // a watcher not heard from this long has gone (they say they're there every 15 s)
const WATCHER_PING_GONE_MS = 90000;   // (a watcher's WebSocket with no ping this long (its game pings every 15 s): gone, closed)
const STREAM_STUCK_MS = 10000;     // (a stream that hasn't taken its last message this long: its watcher not reading, let go)
const WATCH_IDLE_MS = 3 * 3600e3;  // a watch code nothing's used this long is forgotten
const W_MAGIC = 0x31574656, W_START = 1, W_PUSH = 2, W_REPLY = 4, W_SNAP = 5;
const HASH_BACK = 120;   // (frames: a reply's hashes reach back this far before its first frame)
// Snapshots (watch.h's SNAP): the host's state, deflated, at most SNAP_MAX, in parts of SNAP_PART (watch.h's: small, so
// the host's upload never bursts long ahead of its players' packets); one counts as fresh within SNAP_FRESH frames of the
// match's latest (35 s: a watcher loading it runs that many at most, a few seconds), else the host's asked for a new one,
// no more than once every SNAP_ASK_MS while a watcher's asked within SNAP_WANT_MS. Memory: a Watch's objects may share
// an isolate's 128 MB with many others, so a Watch holds one snapshot at most, put together in place (one buffer, the
// last one let go as a new one's first part comes), and lets it go once nobody's asked for it for SNAP_IDLE_MS (the edge
// keeps serving it by its frame: it's stale by then anyway), and a part-sent one with no part for SNAP_STALL_MS
const SNAP_MAX = (16 << 20) + 64, SNAP_PART = 32 << 10, SNAP_FRESH = 2000, SNAP_ASK_MS = 15000, SNAP_WANT_MS = 30000, SNAP_HEAD = 42;
const SNAP_IDLE_MS = 60000, SNAP_STALL_MS = 30000;

async function bytes_of(req, max) {
	if ((req.headers.get('content-length') | 0) > max || !req.body) return null;
	try {
		const reader = req.body.getReader(), parts = [];
		let size = 0;
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			size += value.length;
			if (size > max) { reader.cancel(); return null; }
			parts.push(value);
		}
		const all = new Uint8Array(size);
		let at = 0;
		for (const p of parts) { all.set(p, at); at += p.length; }
		return all;
	} catch {
		return null;
	}
}
const binary = (body, keep) =>
	new Response(body, { headers: { 'content-type': 'application/octet-stream', 'cache-control': keep > 0 ? 'public, max-age=' + keep : 'no-store', 'x-keep': String(keep | 0) } });
// (a binary message's head: magic, kind, version 1, the match as 16 hex digits, as watch.cpp's hex())
function w_head(b, kind) {
	if (!b || b.length < 14) return null;
	const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
	if (v.getUint32(0, true) !== W_MAGIC || b[4] !== kind || b[5] !== 1) return null;
	const m = v.getUint32(10, true).toString(16).padStart(8, '0') + v.getUint32(6, true).toString(16).padStart(8, '0');
	return m === '0000000000000000' ? null : { v, m, at: 14 };
}
const hex16 = (s) => (typeof s === 'string' && /^[0-9a-f]{16}$/.test(s) ? s : '');   // (a match's id; an address's hash, aid)
const same_name = (x) => String(x || '').toUpperCase().replace(/[^A-Z0-9]/g, '');   // (a player's name as the room has it and as its Watch does, the same)

// (a watcher's seen, a guest's count: a code new to the asker counts against the limit for new codes, a join's, whatever
// the code is, so watching can't be used to find room codes faster than joining can; asked again, not)
async function looked(env, req, w, inner, method) {
	let r = await w.fetch(inner, { method });
	if (r.headers.get('x-new') === '1') {
		await r.body?.cancel();
		if (await new_code_limited(env, req)) return json({ error: 'busy' }, 429);
		r = await w.fetch(inner + '&admit=1', { method });
	}
	return new Response(await r.text(), { status: r.status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });
}
// (a watcher's address, hashed, for WATCHERS_PER_IP and STREAMS_PER_IP: an IPv6 one by its /48, so one /48's many /64s
// can't fill a match's 400 between them)
const watcher_ip = async (req) => (await player_id(req, 'watch', 3)).slice(0, 8);
async function new_code_limited(env, req) {   // (an address, an IPv6 one by its /64, 20 a minute; and its /48 all told, 200)
	return (await limited(env, req, 'JOIN_LIMIT')) || (await limited(env, req, 'CODE48_LIMIT', 3));
}
async function watch_route(env, req, url) {
	const op = url.pathname.slice('/v1/watch/'.length);
	const q0 = (k) => url.searchParams.get(k) || '';
	// (what a match's host and its watchers ask again and again: a push 5 a second, the pieces each second, the seen and the
	// count; limited apart from everything else, so watching from behind a shared address never uses up its players'
	// ANY_LIMIT, their hosting, joining and searching)
	const again = (req.method === 'POST' && (op === 'push' || op === 'seen' || op === 'snap')) || (req.method === 'GET' && (op === 'frames' || op === 'count' || (op === 'snap' && !q0('f'))));
	if (await limited(env, req, again ? 'WATCHING_LIMIT' : 'ANY_LIMIT')) return json({ error: 'busy' }, 429);
	const q = (k) => url.searchParams.get(k) || '';
	const wid = normal_code(q('w'));
	if (!code_ok(wid)) return json({ error: 'no_watch' }, 404);
	const w = watches(env, wid), edge = !env.TURN_STATIC;
	const sid = clean(q('s'), 32).replace(/[^0-9a-f]/g, '');
	// (watching switched off: a host's start or push answered off by its Watch, which ends its watchers' sockets; anything
	// else of a watcher's said plainly)
	if (!switch_on(env, 'SWITCH_WATCH') && !(req.method === 'POST' && (op === 'start' || op === 'push'))) return off();
	// (checkpoints, watchers' states handed to others, no more: refused before anything's read; none to give out)
	if (op === 'cp') return json({ error: 'gone' }, 410);
	if (req.method === 'POST' && (op === 'start' || op === 'push')) {
		const body = await bytes_of(req, op === 'start' ? START_MAX : 64 << 10);
		if (!body) return json({ error: 'bad' }, 400);
		return w.fetch('https://watch/' + op + '?k=' + encodeURIComponent(clean(q('k'), 64)) + '&s=' + sid, { method: 'POST', body });
	}
	if (req.method === 'POST' && op === 'snap') {   // (a part of the host's snapshot)
		const body = await bytes_of(req, SNAP_PART);
		const m = hex16(q('m')), f = parseInt(q('f'), 10), i = parseInt(q('i'), 10), n = parseInt(q('n'), 10);
		if (!body || !body.length || !m || !(f >= 0 && f <= WATCH_FRAMES) || !(n >= 1 && n <= Math.ceil(SNAP_MAX / SNAP_PART)) || !(i >= 0 && i < n)) return json({ error: 'bad' }, 400);
		return w.fetch('https://watch/snap?k=' + encodeURIComponent(clean(q('k'), 64)) + '&m=' + m + '&f=' + f + '&i=' + i + '&n=' + n, { method: 'POST', body });
	}
	if (req.method === 'GET' && op === 'snap') {   // (a watcher's: is there one, then the one, kept at the edge by its frame)
		const m = hex16(q('m'));
		if (!m) return json({ error: 'bad' }, 400);
		if (!q('f')) return w.fetch('https://watch/snap?m=' + m);
		if (!snaps_on(env)) return json({ error: 'no_snap' }, 404);   // (switched off: none, not even one the edge still keeps)
		const f = parseInt(q('f'), 10);
		if (!(f >= 0 && f <= WATCH_FRAMES)) return json({ error: 'bad' }, 400);
		if (await limited(env, req, 'WATCH_LIMIT')) return json({ error: 'busy' }, 429);
		const key = new Request('https://watch.vf-lobby/snap/' + wid + '/' + m + '/' + f);
		const kept = edge && (await caches.default.match(key));
		if (kept) return kept;
		const r = await w.fetch('https://watch/snap?m=' + m + '&f=' + f);
		const out = new Response(r.body, { status: r.status, headers: r.status === 200 ? { 'content-type': 'application/octet-stream', 'cache-control': 'public, max-age=86400' } : { 'content-type': 'application/json', 'cache-control': 'no-store' } });
		if (edge && r.status === 200) await caches.default.put(key, out.clone());
		return out;
	}
	if (req.method === 'POST' && op === 'seen') return looked(env, req, w, 'https://watch/seen?s=' + sid + '&ip=' + (await watcher_ip(req)), 'POST');
	if (req.method !== 'GET') return json({ error: 'bad' }, 404);
	// (WebSockets, the Watch's own and hibernating between messages: the host's feed, a watcher's frames as they come)
	if (op === 'feed' || op === 'ws') {
		if ((req.headers.get('upgrade') || '').toLowerCase() !== 'websocket') return json({ error: 'bad' }, 426);
		if (!sockets_on(env)) return off();   // (the host sends over HTTPS, a watcher takes the stream, as before)
		if (op === 'feed') return w.fetch(new Request('https://watch/feed?k=' + encodeURIComponent(clean(q('k'), 64)), req));
		if (await limited(env, req, 'WATCH_LIMIT')) return json({ error: 'busy' }, 429);
		const m = hex16(q('m'));
		if (!m) return json({ error: 'bad' }, 400);
		const from = Math.min(Math.max(parseInt(q('from'), 10) || 0, 0), WATCH_FRAMES);
		const ip = await watcher_ip(req);
		return w.fetch(new Request('https://watch/ws?m=' + m + '&from=' + from + '&s=' + sid + '&ip=' + ip, req));
	}
	if (op === 'count') return looked(env, req, w, 'https://watch/count?ip=' + (await watcher_ip(req)), 'GET');
	if (op === 'live') {   // (the frames as they come: a response that streams, straight from the Watch, never kept)
		if (await limited(env, req, 'WATCH_LIMIT')) return json({ error: 'busy' }, 429);
		const m = hex16(q('m'));
		if (!m) return json({ error: 'bad' }, 400);
		const from = Math.min(Math.max(parseInt(q('from'), 10) || 0, 0), WATCH_FRAMES);
		const ip = await watcher_ip(req);   // (the address, hashed: streams counted by it, never kept)
		return w.fetch('https://watch/live?m=' + m + '&from=' + from + '&s=' + sid + '&ip=' + ip);
	}
	if (op !== 'start' && op !== 'frames') return json({ error: 'bad' }, 404);
	if (op !== 'frames' && (await limited(env, req, 'WATCH_LIMIT'))) return json({ error: 'busy' }, 429);   // (a start: a watcher beginning)
	const m = hex16(q('m'));
	if (!m) return json({ error: 'bad' }, 400);
	// (the frames from the start of F's piece: everyone near the same frame asks the same, and the edge answers them)
	const from = Math.floor(Math.min(Math.max(parseInt(q('from'), 10) || 0, 0), WATCH_FRAMES) / PIECE) * PIECE;
	const inner = 'https://watch/' + op + '?m=' + m + '&from=' + from;
	const key = new Request('https://watch.vf-lobby/' + op + '/' + wid + '/' + m + '/' + (op === 'frames' ? from : 0));
	const kept = edge && (await caches.default.match(key));
	if (kept) return kept;
	const r = await w.fetch(inner);
	const keep = r.headers.get('x-keep') | 0;
	const out = new Response(r.body, { status: r.status, headers: r.status === 200 ? { 'content-type': 'application/octet-stream', 'cache-control': keep > 0 ? 'public, max-age=' + keep : 'no-store' } : { 'content-type': 'application/json', 'cache-control': 'no-store' } });
	if (edge && r.status === 200 && keep > 0) await caches.default.put(key, out.clone());
	return out;
}

export class Watch {
	constructor(ctx, env) {
		this.ctx = ctx;
		this.env = env;
		this.m = null;               // the match on now (in memory; and in storage while nobody watches: restore())
		this.watchers = new Map();   // sid -> {first, last, ip}
		this.askers = new Map();     // (the count's askers lately, by their address's hash: ip -> when)
		this.pending = new Map();    // (watchers waiting for a match, not counted till one's on: sid -> when)
		this.streams = new Set();    // the watchers the frames go to as they come: {writer, sent, sid}
		this.told = { at: 0, sig: '' };
		this.load = { matches: 0, watchers: 0 };   // (all matches', as the Stats last said)
		this.quiet = new WeakSet();  // (watchers' sockets closed for silence: not counted while Cloudflare lets go of them)
		auto_pong(ctx);
		ctx.blockConcurrencyWhile(async () => {
			this.arm = (await ctx.storage.get('arm')) || null;
		});
	}
	// (while anyone's on a stream: every 5 s, a length of 0 to those with nothing new; a match over or gone, the streams ended)
	keepalive() {
		if (this.ticking || !this.streams.size) return;
		this.ticking = true;
		setTimeout(() => {
			this.ticking = false;
			if (this.m && this.over() && Date.now() - this.m.seen > WATCH_QUIET_MS) for (const st of [...this.streams]) this.end_stream(st);
			this.flush(true);
			this.keepalive();
		}, 5000);
	}
	// The match kept while nobody watches (its Watch out of memory between the idle uploads): what's new since it was last
	// kept, at each idle upload; the start once, when it comes (the last match's let go)
	keep_start(b) {
		const puts = {};
		for (let i = 0; i * KEPT_PART < b.length; i++) puts['ms' + i] = b.slice(i * KEPT_PART, (i + 1) * KEPT_PART);
		this.ctx.storage.delete(KEPT_KEYS).catch(() => {});
		this.m.parts = Object.keys(puts).length;
		this.m.kept = 0;
		this.ctx.storage.put(puts).catch(() => {});
		this.keep_frames(true);
	}
	keep_frames(force) {
		const m = this.m;
		if (!m || !m.parts || (!force && m.kept >= m.n)) return;
		const puts = {};
		for (let k = Math.floor(m.kept / FRAME_BLOCK); k * FRAME_BLOCK < m.n; k++) {
			const from = k * FRAME_BLOCK, to = Math.min(m.n, from + FRAME_BLOCK);
			puts['mf' + k] = { f: new Uint8Array(m.frames.buffer.slice(from * 4, to * 4)), h: [...m.hashes].filter(([fr]) => fr >= from && fr < from + FRAME_BLOCK) };
		}
		m.kept = m.n;
		puts.mm = { id: m.id, meta: m.meta, born: m.born, seen: m.seen, info: [...m.info], n: m.n, parts: m.parts };
		if (this.arm) puts.arm = this.arm;   // (its last use too: the alarm doesn't take a match still sent for idle)
		this.ctx.storage.put(puts).catch(() => {});
	}
	// (quiet: the alarm's, looking whether the host's gone quiet: the host's last word as kept. Anyone else's: from now, a
	// restart (a deploy, a variable changed) or a sleep isn't the host gone quiet, it's over if it says so or nothing comes
	// from it from here; the alarm's own look isn't put off so, or one woken by each alarm would never find it over)
	restore(quiet = false) {   // (woken without the match: the one kept, if any; read once, everything after it waiting on that, in order)
		if (!this.restoring) this.restoring = this.m ? Promise.resolve() : this.read_kept(quiet);
		return this.restoring;
	}
	async read_kept(quiet) {
		const got = await this.ctx.storage.get(KEPT_KEYS);
		const mm = got.get('mm');
		if (!mm || !mm.parts || this.m) return;
		const parts = [];
		for (let i = 0; i < mm.parts; i++) { const p = got.get('ms' + i); if (!p) return; parts.push(p); }
		const start = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
		parts.reduce((at, p) => (start.set(p, at), at + p.length), 0);
		const frames = new Uint16Array(Math.max(4096, mm.n * 2)), hashes = new Map();
		let n = 0;
		while (n < mm.n) {   // (the blocks there, in order: as many frames as they hold)
			const blk = got.get('mf' + Math.floor(n / FRAME_BLOCK));
			if (!blk || !blk.f || !blk.f.length) break;
			new Uint8Array(frames.buffer).set(blk.f, n * 4);
			for (const [fr, hh] of blk.h || []) hashes.set(fr, hh);
			n += blk.f.length / 4;
		}
		n = Math.min(n, mm.n);
		this.m = { id: mm.id, head: start.slice(0, 14), start, meta: mm.meta, frames, n, hashes, info: new Uint8Array(mm.info),
			seen: quiet ? mm.seen : Math.max(mm.seen, Date.now()), born: mm.born, parts: mm.parts, kept: n, restored: true };
	}
	async alarm() {
		this.quiet_at = 0;
		this.quiet_watchers();
		if (this.arm && Date.now() - (this.arm.used || 0) > WATCH_IDLE_MS) {
			this.arm = null;
			this.m = null;
			this.restoring = Promise.resolve();
			for (const st of [...this.streams]) this.end_stream(st);
			await this.ctx.storage.deleteAll();
			return;
		}
		if (!this.arm) { for (const st of [...this.streams]) this.end_stream(st); return; }
		if (this.m && this.over() && Date.now() - this.m.seen > WATCH_QUIET_MS) for (const st of [...this.streams]) this.end_stream(st);
		// (watchers on WebSockets, the host quiet: told it's over; still sending, looked at again from its last word)
		if (this.w_sockets().length) {
			await this.restore(true);
			if (!this.m || this.over()) this.quiet_to_sockets();
			else { this.quiet_alarm(); if (this.quiet_at) return; }
		}
		await this.ctx.storage.setAlarm(Date.now() + WATCH_IDLE_MS);
	}
	count() {
		const now = Date.now();
		for (const st of this.streams) { const x = this.watchers.get(st.sid); if (x) x.last = now; }   // (one on a stream is here)
		for (const [s, x] of this.watchers) if (now - x.last > WATCHER_MS) this.watchers.delete(s);
		return this.watchers.size + this.w_sockets().length;   // (and those on a WebSocket, while it's open)
	}
	w_sockets(tag = 'w') { return this.ctx.getWebSockets(tag).filter((ws) => !this.quiet.has(ws)); }
	// (a watcher's socket with no ping for WATCHER_PING_GONE_MS: its game gone without a close (a cable pulled, a sleep),
	// closed, no longer counted; looked at by the alarm and every 15 s at most as the host sends)
	quiet_watchers() {
		const now = Date.now();
		this.reaped = now;
		for (const ws of this.ctx.getWebSockets('w')) {
			if (this.quiet.has(ws)) continue;
			const at = (ws.deserializeAttachment() || {}).t || 0, pong = this.ctx.getWebSocketAutoResponseTimestamp ? this.ctx.getWebSocketAutoResponseTimestamp(ws) : null;
			if (now - Math.max(at, pong ? pong.getTime() : 0) <= WATCHER_PING_GONE_MS) continue;
			this.quiet.add(ws);
			try { ws.close(1001, 'quiet'); } catch (e) {}
		}
	}
	// The streams: each sent what it hasn't had yet, as soon as it can take it (one slow to read gets more at once later,
	// never anything dropped, and holds no one else up); every 5 s a length of 0 if nothing else went (still here)
	flush(force) {
		const m = this.m, now = Date.now();
		for (const st of [...this.streams]) {
			if (!m || m.id !== st.m) { this.end_stream(st); continue; }   // (its match gone: ended, taking its last message or not)
			if (st.busy) {   // (not reading: let go after STREAM_STUCK_MS, its watcher asks the pieces if it's still there)
				if (now - st.at > STREAM_STUCK_MS) this.end_stream(st);
				continue;
			}
			let msg = null;
			if (st.sent < m.n || (force && st.told !== this.over())) {
				const r = new Uint8Array(this.reply(st.sent).body_bytes);
				const v = new DataView(r.buffer);
				st.sent = v.getUint32(32, true) + v.getUint32(36, true);   // (first + n: what's had now)
				st.told = this.over();
				msg = r;
			} else if (Date.now() - st.at > 5000) msg = new Uint8Array(0);
			if (!msg) continue;
			const out = new Uint8Array(4 + msg.length);
			new DataView(out.buffer).setUint32(0, msg.length, true);
			out.set(msg, 4);
			st.busy = true;
			st.at = Date.now();
			st.writer.write(out).then(() => { st.busy = false; if (st.sent < (this.m ? this.m.n : 0)) this.flush(false); }, () => this.end_stream(st));
		}
	}
	end_stream(st) {
		if (!this.streams.delete(st)) return;
		// (a stream its watcher let go of: closed already, said nothing; one still taking a message: aborted, as a close would
		// wait on that message for ever)
		try { (st.busy ? st.writer.abort() : st.writer.close()).catch(() => {}); } catch (e) {}
	}
	knows(s) {   // (a watcher counted here already: asking, on a stream, or on a WebSocket)
		if (!s || s.length < 8) return false;
		if (this.watchers.has(s) || this.pending.has(s)) return true;
		for (const ws of this.ctx.getWebSockets('w')) if ((ws.deserializeAttachment() || {}).sid === s) return true;
		return false;
	}
	from_ip(ip) {   // (watchers from one address: asking, on a stream, on a WebSocket)
		this.count();
		let n = this.w_sockets('ip:' + ip).length;
		for (const x of this.watchers.values()) if (x.ip === ip) n++;
		return n;
	}
	over() {
		const m = this.m;
		const idle = this.count() === 0 || Date.now() - (this.woke || 0) < WATCH_QUIET_IDLE_MS;   // (sent to idly, or lately)
		return !m || (m.info[4] & 1) === 1 || Date.now() - m.seen > (idle ? WATCH_QUIET_IDLE_MS : WATCH_QUIET_MS);
	}
	// The rooms list told what's on (the Stats): when it changes, or every 20 s while it's on; a few seconds apart at most
	tell_stats(force) {
		const a = this.arm, m = this.m;
		if (!a || !m || !m.meta) return;
		const now = Date.now(), over = this.over();
		const sig = [over, m.info.join(','), this.count()].join('|');
		const gap = this.env.TURN_STATIC ? 0 : 5000;   // (a test's own lobby: every change at once)
		// (the match over: told at once, its last score with it; within the gap of the last telling it was never told,
		// the host sending no more)
		if (!force && over === !!this.told.over) if (now - this.told.at < gap || (sig === this.told.sig && now - this.told.at < 20000)) return;
		this.told = { at: now, sig, over };
		const ev = { op: over ? 'unwatch' : 'watch', wid: a.wid, game: a.game, version: a.version, listed: a.listed, random: !!a.random, region: a.region, aid: a.aid || '',
			p: [[m.meta.p1, m.meta.c1], [m.meta.p2, m.meta.c2]], wins: [m.info[0], m.info[1]], rounds: [m.info[2], m.info[3]], watchers: this.count(), born: m.born, mid: m.id };
		const p = this.env.STATS.get(this.env.STATS.idFromName('all')).fetch('https://stats/tell', { method: 'POST', body: JSON.stringify(ev) })
			.then((r) => (r.ok ? r.json() : null))
			.then((l) => { if (l && Number.isFinite(l.watchers)) this.load = { matches: l.matches | 0, watchers: l.watchers | 0, over: l.over === true }; })
			.catch(() => {});
		try { this.ctx.waitUntil(p); } catch (e) {}
	}
	reply(from) {
		const r = this.reply_bytes(from);
		return { body_bytes: r.b.buffer, response: () => binary(r.b, r.keep) };
	}
	reply_bytes(from) {
		const m = this.m;
		const first = Math.min(from, m.n), n = Math.min(m.n - first, PIECE * PIECES);
		// (the hashes of these frames, and of the HASH_BACK before them: a hash is made a few frames after its frame, so it
		// often comes after its frame went to a watcher, who checks it if it hasn't run that frame yet)
		const hs = [...m.hashes].filter(([fr]) => fr + HASH_BACK >= first && fr <= first + n);
		const b = new Uint8Array(14 + 1 + 4 + 4 + 5 + 4 + 8 + n * 4 + 2 + hs.length * 12);
		const v = new DataView(b.buffer);
		b.set(m.head, 0);   // (magic, the kind made REPLY below, version, match: as the start had them)
		b[4] = W_REPLY;
		let at = 14;
		const over = this.over();
		b[at++] = over ? 2 : 1;
		v.setUint32(at, m.n, true); at += 4;
		v.setUint32(at, this.count(), true); at += 4;
		b.set(m.info, at); at += 5;
		v.setUint32(at, 0xffffffff, true); at += 4;   // (no checkpoint: none are kept)
		v.setUint32(at, first, true); at += 4;
		v.setUint32(at, n, true); at += 4;
		b.set(new Uint8Array(m.frames.buffer, first * 4, n * 4), at); at += n * 4;
		v.setUint16(at, hs.length, true); at += 2;
		for (const [fr, h] of hs) { v.setUint32(at, fr, true); b.set(h, at + 4); at += 12; }
		// (a whole piece, or the end of a match its host said is over, never changes: kept a day; the piece still filling, a
		// second; and so is an end taken for over only because the host's gone quiet: it may be a restart, the host sending on)
		return { b, keep: n === PIECE * PIECES || ((m.info[4] & 1) === 1 && first + n === m.n) ? 86400 : 1 };
	}
	// A start or a push (bytes as src/net/watch.h makes them), from a POST or the host's WebSocket: [answer, status]
	take(b, start) {
		const a = this.arm, now = Date.now();
		a.used = now;
		this.snap_tidy(now);
		if (!switch_on(this.env, 'SWITCH_WATCH')) {   // (switched off: the host sends no more; its watchers told, their sockets ended)
			for (const w of this.ctx.getWebSockets('w')) try { w.send(JSON.stringify({ error: 'off' })); w.close(1000, 'off'); } catch (e) {}
			for (const st of [...this.streams]) this.end_stream(st);
			return [{ off: true }, 200];
		}
		if (start) {
			const h = w_head(b, W_START);
			if (!h || b.length < 20 || b.length > START_MAX) return [{ error: 'bad' }, 400];   // (its meta's length and the blob's read below; on a WebSocket too, as a POST's)
			const ml = h.v.getUint16(14, true);
			if (b.length < 16 + ml + 4) return [{ error: 'bad' }, 400];
			let meta = null;
			try { meta = JSON.parse(new TextDecoder().decode(b.subarray(16, 16 + ml))); } catch (e) {}
			const bl = h.v.getUint32(16 + ml, true);
			if (!meta || typeof meta !== 'object' || b.length !== 20 + ml + bl) return [{ error: 'bad' }, 400];
			const name = (x) => { const raw = clean(x, 10).toUpperCase(), n = raw.replace(/[^A-Z0-9 .!?-]/g, ''); return n && !name_ok(raw) ? 'PLAYER' : player_name(n); };
			for (const st of [...this.streams]) this.end_stream(st);   // (a new match: the streams of the last one ended; theirs ask anew)
			this.m = { id: h.m, head: b.slice(0, 14), start: b, meta: { p1: name(meta.p1), p2: name(meta.p2), c1: country_ok(meta.c1), c2: country_ok(meta.c2), ...(meta.snap === 1 ? { snap: 1 } : {}) },
				frames: new Uint16Array(4096), n: 0, hashes: new Map(), info: new Uint8Array(5), seen: now, born: now };
			this.keep_start(b);
			this.tell_stats(true);
			this.status_to_sockets(true);
			return [{ ok: true }, 200];
		}
		const h = w_head(b, W_PUSH), m = this.m;
		if (!h || b.length < 22) return [{ error: 'bad' }, 400];
		if (!m || h.m !== m.id) return [{ has: -1, watchers: this.count() }, 200];   // (no start for it here, or this object woke without it: sent all again)
		const first = h.v.getUint32(14, true), n = h.v.getUint32(18, true);
		let at = 22 + n * 4;
		if (n > 4096 || b.length < at + 2) return [{ error: 'bad' }, 400];
		if (m.restored && first > m.n) return [{ has: -1, watchers: this.count() }, 200];   // (some of it not kept: all of it again)
		const nh = h.v.getUint16(at, true);
		if (b.length !== at + 2 + nh * 12 + 5) return [{ error: 'bad' }, 400];
		if (first <= m.n && first + n > m.n && first + n <= WATCH_FRAMES) {
			const need = (first + n) * 2;
			if (need > m.frames.length) {
				const bigger = new Uint16Array(Math.min(Math.max(need, m.frames.length * 2), WATCH_FRAMES * 2));
				bigger.set(m.frames.subarray(0, m.n * 2));
				m.frames = bigger;
			}
			const skip = m.n - first;
			new Uint8Array(m.frames.buffer).set(b.subarray(22 + skip * 4, 22 + n * 4), m.n * 4);
			m.n = first + n;
		}
		at += 2;
		for (let i = 0; i < nh; i++, at += 12) {
			const fr = h.v.getUint32(at, true);
			if (fr <= m.n && m.hashes.size < 20000) m.hashes.set(fr, b.slice(at + 4, at + 12));
		}
		m.info = b.slice(at, at + 5);
		m.seen = now;
		if (now - (this.reaped || 0) > 15000) this.quiet_watchers();
		this.flush(m.info[4] & 1);   // (the new frames to every stream at once)
		this.to_sockets();           // (and every watcher's WebSocket)
		const watchers = this.count();
		if (watchers === 0) this.keep_frames(false);   // (sent for idle from here: its Watch may sleep, the match kept)
		this.tell_stats(false);
		this.status_to_sockets(false);
		const every = watchers === 0 ? IDLE_EVERY_MS : this.load.matches > MATCHES_HARD ? 1000 : this.load.matches > MATCHES_SOFT ? 400 : 200;   // (5 a second while anyone watches; slower under load; idle with none)
		if (this.load.over) return [{ has: m.n, watchers, off: true }, 200];   // (the month's ceiling reached: the host sends no more)
		return [{ has: m.n, watchers, ...(every > 200 ? { every } : {}), ...(this.snap_ask() ? { snap: true } : {}) }, 200];
	}
	// The watchers' WebSockets: each sent what it hasn't had of this match, and whether it's over whenever that changes
	// (a last push with no new frames, or the host gone quiet: the alarm's), its place kept with it so it outlives the
	// object hibernating; a watcher of the last match told the new one by the status
	to_sockets() {
		const m = this.m;
		if (!m) return;
		if (!sockets_on(this.env)) {   // (switched off: each on to the HTTPS stream, from where it was)
			for (const ws of this.ctx.getWebSockets('w')) try { ws.close(1012, 'off'); } catch (e) {}
			return;
		}
		const over = this.over();
		for (const ws of this.ctx.getWebSockets('w')) {
			const at = ws.deserializeAttachment() || {};
			if (at.m !== m.id || (at.sent >= m.n && !!at.over === over)) continue;
			this.to_socket(ws, at, this.reply_bytes(Math.min(at.sent, m.n)).b, over);
		}
		this.quiet_alarm();
	}
	to_socket(ws, at, r, over) {   // (a reply sent, and what it had kept with the socket: up to where, over or not, the score)
		const v = new DataView(r.buffer);
		at.sent = Math.max(at.sent || 0, v.getUint32(32, true) + v.getUint32(36, true));
		at.over = over;
		at.info = [...r.subarray(23, 28)];
		try { ws.send(r); ws.serializeAttachment(at); } catch (e) {}
	}
	// (watchers on WebSockets hear only when the host sends: one whose game closed or whose connection went sends
	// nothing more, so an alarm looks in WATCH_QUIET_MS after the last word; each push resets nothing, it's set once and
	// looks again from the last word. Not while nobody's on a socket.)
	quiet_alarm() {
		if (this.quiet_at || !this.ctx.getWebSockets('w').length || !this.m) return;
		this.quiet_at = Math.max(Date.now() + 5000, this.m.seen + WATCH_QUIET_MS + 1000);
		this.ctx.storage.setAlarm(this.quiet_at).catch(() => {});
	}
	// The host quiet (the alarm), its match maybe gone from memory with it (the object out of memory while only
	// WebSockets wait: nothing kept while anyone watches): each watcher on a socket told the match is over, once, from
	// what its socket kept (the match's id, its frames, the score last sent)
	quiet_to_sockets() {
		const m = this.m;
		for (const ws of this.ctx.getWebSockets('w')) {
			const at = ws.deserializeAttachment() || {};
			if (at.over || !hex16(at.m)) continue;
			let r;
			if (m && m.id === at.m) r = this.reply_bytes(Math.min(at.sent || 0, m.n)).b;
			else {   // (the match not here any more: a reply with no frames, the match's own id, over)
				r = new Uint8Array(14 + 1 + 4 + 4 + 5 + 4 + 8 + 2);
				const v = new DataView(r.buffer);
				v.setUint32(0, W_MAGIC, true);
				r[4] = W_REPLY;
				r[5] = 1;
				v.setUint32(6, parseInt(at.m.slice(8), 16), true);
				v.setUint32(10, parseInt(at.m.slice(0, 8), 16), true);
				r[14] = 2;
				v.setUint32(15, at.sent || 0, true);
				v.setUint32(19, this.count(), true);
				if (Array.isArray(at.info) && at.info.length === 5) r.set(at.info.map((x) => x & 255), 23);
				v.setUint32(28, 0xffffffff, true);
				v.setUint32(32, at.sent || 0, true);
			}
			r[14] = 2;   // (over: the host's quiet, whatever this object knows of it)
			this.to_socket(ws, at, r, true);
		}
	}
	// (how many watch, the match on: to each watcher's WebSocket when it changes, its /seen's answer without the asking;
	// no keeper, no checkpoint any more)
	status_to_sockets(force) {
		const ws = this.ctx.getWebSockets('w');
		if (!ws.length) return;
		const m = this.m, sig = [this.count(), m ? m.id : ''].join('|');
		if (!force && sig === this.status_sig) return;
		this.status_sig = sig;
		const quick = !!(this.arm && this.arm.quick);
		for (const w of ws) try { w.send(JSON.stringify({ watchers: this.count(), keeper: false, match: m ? m.id : '', checkpoint: -1, random: quick })); } catch (e) {}
	}
	// (a watcher come while the host sends idly: told at once, on its socket, to send as it should; free)
	wake_host(snap = false) {
		this.woke = Date.now();
		for (const h of this.ctx.getWebSockets('host')) try { h.send(JSON.stringify({ wake: true, every: 200, ...(snap ? { snap: true } : {}) })); } catch (e) {}
	}
	// Snapshots: is one wanted of the host now (a watcher asked lately, none fresh, none coming, not asked just now):
	// asking marks it asked
	snap_fresh() {
		const m = this.m;
		return !!(m && m.snap && m.n - m.snap.frame <= SNAP_FRESH);
	}
	snap_ask() {
		const m = this.m, now = Date.now();
		if (!snaps_on(this.env) || !m || !m.meta || m.meta.snap !== 1 || !m.snap_want || now - m.snap_want > SNAP_WANT_MS || this.snap_fresh()) return false;
		if (now - (m.snap_asked || 0) < SNAP_ASK_MS) return false;
		if (m.snap_in && now - m.snap_in.at < SNAP_ASK_MS) return false;   // (one coming: its parts still arriving)
		m.snap_asked = now;
		return true;
	}
	// (memory: the snapshot nobody's asked for in SNAP_IDLE_MS let go, as is a part-sent one stalled SNAP_STALL_MS)
	snap_tidy(now = Date.now()) {
		const m = this.m;
		if (!m) return;
		if (m.snap && now - m.snap.used > SNAP_IDLE_MS) m.snap = null;
		if (m.snap_in && now - m.snap_in.at > SNAP_STALL_MS) m.snap_in = null;
	}
	// A part of the host's snapshot, put in its place in the one buffer it's put together in: whole, checked and kept as
	// the match's latest. A new one's first part lets the last one go (a new one's asked for only once that's stale; one
	// sent while it's fresh is refused), so a Watch never holds two
	snap_part(f, i, n, b) {
		const m = this.m, now = Date.now();
		if (!(n >= 1 && n * SNAP_PART <= SNAP_MAX + SNAP_PART && i >= 0 && i < n) || !b.length || b.length > SNAP_PART || (i < n - 1 && b.length !== SNAP_PART)) return { error: 'bad' };
		if (!m.snap_in || m.snap_in.f !== f || m.snap_in.n !== n) {
			if (m.snap && (f <= m.snap.frame || this.snap_fresh())) return { error: 'bad' };   // (not newer, or the one kept still fresh: none asked for)
			m.snap_in = null;   // (any part-sent one let go,
			m.snap = null;      // and the stale one kept, before the new one's buffer is made)
			m.snap_in = { f, n, buf: new Uint8Array(n * SNAP_PART), have: new Uint8Array(n), got: 0, size: (n - 1) * SNAP_PART, at: now };
		}
		const s = m.snap_in;
		s.at = now;
		if (!s.have[i]) {
			if (i === n - 1) { if (s.size + b.length > SNAP_MAX) { m.snap_in = null; return { error: 'bad' }; } s.size += b.length; }
			s.buf.set(b, i * SNAP_PART);
			s.have[i] = 1;
			s.got++;
		}
		if (s.got < n) return { ok: true, got: s.got };
		m.snap_in = null;
		const all = s.buf.subarray(0, s.size);
		const h = w_head(all, W_SNAP);
		if (!h || h.m !== m.id || all.length < SNAP_HEAD || h.v.getUint32(14, true) !== f || h.v.getUint32(38, true) !== all.length - SNAP_HEAD) return { error: 'bad' };
		m.snap = { frame: f, b: all, used: now };
		return { ok: true, got: s.got };
	}
	async webSocketMessage(ws, msg) {
		const tags = this.ctx.getTags ? this.ctx.getTags(ws) : [];
		if (!tags.includes('host')) {   // (a watcher sends nothing but its pings, answered without waking this: anything else, closed)
			if (msg === 'ping') try { const at = ws.deserializeAttachment() || {}; at.t = Date.now(); ws.serializeAttachment(at); ws.send('pong'); } catch (e) {}
			else try { ws.close(1008, 'no'); } catch (e) {}
			return;
		}
		if (msg === 'ping') { try { ws.send('pong'); } catch (e) {} return; }
		// (the feed of a key this Watch no longer has: a room armed it again since, a new key its host's now)
		if (!this.arm || (ws.deserializeAttachment() || {}).k !== this.arm.wkey) { try { ws.close(1008, 'replaced'); } catch (e) {} return; }
		if (typeof msg === 'string') return;
		await this.restore();
		if (!sockets_on(this.env)) { try { ws.close(1012, 'off'); } catch (e) {} return; }   // (switched off: the host on to HTTPS, its upload sent again there)
		const b = new Uint8Array(msg);
		let answer;
		try { [answer] = this.take(b, b.length > 4 && b[4] === W_START); } catch (e) { answer = { error: 'bad' }; }   // (anything the host's socket brings: answered, never thrown)
		try { ws.send(JSON.stringify(answer)); } catch (e) {}
	}
	async webSocketClose(ws, code) {
		try { ws.close(code === 1005 ? 1000 : code, 'closed'); } catch (e) {}
		this.status_to_sockets(false);
	}
	async webSocketError(ws) {
		try { ws.close(1011, 'error'); } catch (e) {}
	}
	async fetch(req) {
		const url = new URL(req.url), op = url.pathname, q = (k) => url.searchParams.get(k) || '';
		const now = Date.now();
		if (op === '/arm') {
			const b = await req.json();
			if (b.fresh === true && this.arm && now - (this.arm.used || 0) <= WATCH_IDLE_MS) return json({ error: 'taken' }, 409);   // (another's)
			// (an arming older than the one it has (its number lower: sent before it, come late): refused, the newer key kept)
			const an = Number.isSafeInteger(b.an) && b.an > 0 ? b.an : 0;
			if (an && this.arm && this.arm.an && an < this.arm.an) return json({ error: 'stale' }, 409);
			for (const old of this.ctx.getWebSockets('host')) try { old.close(1000, 'replaced'); } catch (e) {}   // (the last key's feed: no more)
			this.arm = { wid: String(b.wid).slice(0, 6), wkey: String(b.wkey), game: b.game, version: String(b.version).slice(0, 64), listed: !!b.listed, random: !!b.random, quick: !!b.quick, region: String(b.region || '').slice(0, 2), aid: hex16(b.aid), used: now, an };
			await this.ctx.storage.put('arm', this.arm);
			await this.ctx.storage.setAlarm(now + WATCH_IDLE_MS);
			return json({ ok: true });
		}
		// A code nobody's watched and one whose Watch is armed answer a watcher's asks alike (no oracle for room codes: every
		// new game's room is watchable by its code). A first ask, a watcher not yet counted or a count from an address that
		// hasn't asked lately, is said to the Worker first (x-new, nothing done), which lets it through again only within its
		// limit for new codes (JOIN_LIMIT, as a join's); those already watching ask on as often as they like.
		const a = this.arm, admit = q('admit') === '1';
		if (op === '/seen' || op === '/count') {
			const known = op === '/seen' ? !!a && this.knows(q('s')) : !!a && this.askers.has(q('ip'));
			if (!known && !admit) return new Response('{}', { headers: { 'content-type': 'application/json', 'x-new': '1' } });
			if (!a) return op === '/seen' ? json({ watchers: 0, keeper: false, match: '', checkpoint: -1, random: false, live: true, poll_ms: 1000 }) : json({ watchers: 0 });
		}
		if (!a) return op === '/feed' || req.method === 'POST' ? json({ error: 'no_watch' }, 404) : json({ error: 'no_match' }, 404);   // (a host's key: no_watch, as a wrong key's)
		if (op !== '/count' && op !== '/feed') await this.restore();
		if (req.method === 'POST' && (op === '/start' || op === '/push')) {
			if (q('k') !== a.wkey) return json({ error: 'no_watch' }, 404);
			const [answer, status] = this.take(new Uint8Array(await req.arrayBuffer()), op === '/start');
			return json(answer, status);
		}
		// the host's WebSocket: its start and pushes as messages, each answered as a POST's would be
		if (op === '/feed') {
			if (q('k') !== a.wkey) return json({ error: 'no_watch' }, 404);
			for (const old of this.ctx.getWebSockets('host')) try { old.close(1000, 'replaced'); } catch (e) {}
			const [client, server] = Object.values(new WebSocketPair());
			this.ctx.acceptWebSocket(server, ['host']);
			server.serializeAttachment({ k: a.wkey });   // (its messages taken only while this key's the Watch's)
			return new Response(null, { status: 101, webSocket: client });
		}
		if (op === '/seen') {
			const s = q('s');
			if (s.length >= 8 && !this.m) {   // (no match yet: waiting for one, known here, not counted: as a code with no Watch answers)
				for (const [x, t] of this.pending) if (now - t > WATCHER_MS) this.pending.delete(x);
				if (this.pending.size < 1000 || this.pending.has(s)) this.pending.set(s, now);
			} else if (s.length >= 8) {
				this.pending.delete(s);
				const x = this.watchers.get(s), ip = q('ip');
				if (x) x.last = now;
				else if (this.count() >= WATCH_MAX) return json({ error: 'full', watchers: this.watchers.size }, 429);
				else if (ip && this.from_ip(ip) >= (+this.env.WATCHERS_PER_IP || WATCHERS_PER_IP)) return json({ error: 'busy' }, 429);   // (a variable may raise it: staging's)
				else if (this.load.watchers >= WATCHERS_HARD || this.load.over) return json({ error: 'busy' }, 429);   // (everywhere: too many watching, or the month's ceiling)
				else {
					if (this.count() === 0) this.wake_host();
					this.watchers.set(s, { first: now, last: now, ip });
				}
			}
			const m = this.m, n = this.count(), soft = this.load.watchers >= WATCHERS_SOFT;
			// (live: may it have a stream; poll_ms: how often it asks for pieces without one)
			return json({ watchers: n, keeper: false, match: m ? m.id : '', checkpoint: -1, random: !!a.quick,
				live: !soft && this.streams.size < STREAMS_MAX, poll_ms: soft ? 3000 : 1000 });
		}
		if (op === '/count') {   // (the guest's, once a minute: its address known 3 minutes, so asked again without a new code's limit)
			for (const [ip, t] of this.askers) if (now - t > 180000) this.askers.delete(ip);
			if (q('ip') && this.askers.size < 1000) this.askers.set(q('ip'), now);
			return json({ watchers: this.count() });
		}
		if (op === '/snap') this.snap_tidy(now);
		if (req.method === 'POST' && op === '/snap') {
			if (q('k') !== a.wkey) return json({ error: 'no_watch' }, 404);
			if (!snaps_on(this.env)) return json({ error: 'off' }, 503);
			const m = this.m;
			if (!m || q('m') !== m.id) return json({ error: 'no_match' }, 404);
			const out = this.snap_part(parseInt(q('f'), 10), parseInt(q('i'), 10), parseInt(q('n'), 10), new Uint8Array(await req.arrayBuffer()));
			return json(out, out.error ? 400 : 200);
		}
		const m = this.m;
		if (!m || (req.method !== 'POST' && q('m') !== m.id)) return json({ error: 'no_match' }, 404);
		if (op === '/snap' && !q('f')) {   // (is there a snapshot: the latest if fresh, else one asked of the host if it makes them)
			if (!snaps_on(this.env)) return json({ frame: -1 });   // (switched off: none, none to come)
			if (m.snap) m.snap.used = now;   // (said: kept a while yet, to be fetched)
			if (this.snap_fresh()) return json({ frame: m.snap.frame, size: m.snap.b.length });
			if (!m.meta || m.meta.snap !== 1) return json({ frame: -1 });
			if (m.n <= SNAP_FRESH) return json({ frame: -1, n: m.n });   // (a match just begun: its start's as near)
			m.snap_want = now;
			if (this.ctx.getWebSockets('host').length && this.snap_ask()) this.wake_host(true);   // (on its socket at once; over HTTPS, its next push's answer)
			return json({ frame: m.snap ? m.snap.frame : -1, ...(m.snap ? { size: m.snap.b.length } : {}), pending: true });
		}
		if (op === '/snap') {
			const f = parseInt(q('f'), 10);
			if (!snaps_on(this.env) || !m.snap || m.snap.frame !== f) return json({ error: 'no_snap' }, 404);
			m.snap.used = now;
			return binary(m.snap.b, 86400);
		}
		if (op === '/start') return binary(m.start, 86400);
		if (op === '/frames') return this.reply(Math.max(0, parseInt(q('from'), 10) || 0)).response();
		if (op === '/ws') {
			const sid = q('s'), ip = q('ip');
			if (!this.watchers.has(sid) && this.count() >= WATCH_MAX) return json({ error: 'full' }, 429);
			if (this.load.over) return json({ error: 'busy' }, 429);
			if (this.w_sockets().length >= STREAMS_MAX || this.load.watchers >= WATCHERS_SOFT || this.w_sockets('ip:' + ip).length >= STREAMS_PER_IP)
				return json({ error: 'busy' }, 429);   // (as a stream's: the rest ask the edge's pieces)
			const x = this.watchers.get(sid);
			if (this.count() === 0) this.wake_host();
			this.watchers.delete(sid);   // (counted by its socket from now on)
			const [client, server] = Object.values(new WebSocketPair());
			this.ctx.acceptWebSocket(server, ['w', 'ip:' + ip]);
			const at = { sid, m: m.id, sent: Math.min(Math.max(parseInt(q('from'), 10) || 0, 0), m.n), first: x ? x.first : now, over: false, t: now };   // (t: its pings counted from now)
			server.serializeAttachment(at);
			const over = this.over();
			if (at.sent < m.n || over) this.to_socket(server, at, this.reply_bytes(at.sent).b, over);   // (what's there, or that it's over)
			this.status_to_sockets(true);
			this.quiet_alarm();
			return new Response(null, { status: 101, webSocket: client });
		}
		if (op === '/live') {
			if (q('s').length < 8) return json({ error: 'bad' }, 400);   // (a watcher's own number, as its game sends: counted toward the 400)
			if (!this.watchers.has(q('s')) && this.count() >= WATCH_MAX) return json({ error: 'full' }, 429);   // (one more than may watch)
			const ip = q('ip');
			if (this.load.over || this.streams.size >= STREAMS_MAX || this.load.watchers >= WATCHERS_SOFT || [...this.streams].filter((x) => x.ip === ip).length >= STREAMS_PER_IP)
				return json({ error: 'busy' }, 429);   // (so many: the rest ask /frames, the edge answering)
			const { readable, writable } = new TransformStream();
			const st = { writer: writable.getWriter(), sent: Math.min(Math.max(parseInt(q('from'), 10) || 0, 0), m.n), m: m.id, sid: q('s'), ip, busy: false, at: 0, told: null };
			const x = this.watchers.get(st.sid);
			if (x) x.last = now; else this.watchers.set(st.sid, { first: now, last: now, ip });
			this.streams.add(st);
			this.flush(true);
			this.keepalive();
			return new Response(readable, { headers: { 'content-type': 'application/octet-stream', 'cache-control': 'no-store' } });
		}
		return json({ error: 'bad' }, 400);
	}
}
