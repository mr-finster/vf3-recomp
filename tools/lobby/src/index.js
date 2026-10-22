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
//   POST /v1/turn   {game}  -> {servers: [{host, port, username, credential}], ttl, country}   (TURN_PER_DAY a day, all told)
//   POST /v1/random {game, version, content, name, cands, relay} -> {ticket, waiting, players} or a match
//   POST /v1/random/wait  {ticket}  (waits up to 20 s) -> the same
//   POST /v1/random/leave {ticket}
//   POST /v1/random/count {game, version, content} -> {players}: searching, or in a random match made in the last 10 minutes
//   A match: {role: "host", code, key, token, name, country} (the room's, opened as theirs, the other already in it)
//   or {role: "guest", code, name, cands, relay, token, country} (as /v1/join's). Players meet the nearest first.
//   GET  /v1/ping   -> {ok, protocol}
//
// Errors are {error: "<what>"} with a status: no_room 404, full 409, game / version / rom 412 (with the host's
// version), busy 429, bad 400, no_turn 503. The games word them; nothing here is shown as it comes.

const PROTOCOL = 1;
const LETTERS = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';   // (no 0/O, 1/I/L: read out over voice chat, typed on a pad)
const CODE_LENGTH = 6;
const HOST_GONE_MS = 30000;      // a host not heard from this long has gone (its wait comes every 20 s)
const ROOM_LIFE_MS = 2 * 3600e3; // and no room lasts longer
const WAIT_MS = 20000;
// The relay's logins a day, all players together: what the relay can cost is bounded by its logins (a login works
// a day), so past this none are given out until the next day (UTC) and the games connect directly or not at all.
// About two a match: room for a busy day; raise it as the players come.
const TURN_PER_DAY = 10000;
// And from one address a day (an IPv4 one, or an IPv6 /48): room for a school's or a phone network's shared
// address, but one address alone can't take the day's
const TURN_PER_ADDRESS_DAY = 500;
const GAMES = /^vf[123]$/;   // (the games there are: no queue or count made for anything else)

const json = (body, status = 200) =>
	new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });

// What a game says of itself, checked: short printable text, a few addresses
function clean(s, max) {
	return typeof s === 'string' ? s.replace(/[^\x20-\x7e]/g, '').slice(0, max) : '';
}
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
// Where a request comes from, as Cloudflare has it: the country (two letters) and roughly where
function country_of(req) {
	const c = req.cf && typeof req.cf.country === 'string' ? req.cf.country.toUpperCase() : '';
	return /^[A-Z]{2}$/.test(c) && c !== 'XX' && c !== 'T1' ? c : '';
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
		name: clean(b.name, 10),
		cands: addrs(b.cands),
		relay: relay && addr_ok(relay) ? relay : '',   // (not checked against Cloudflare's ranges: its TURN relays from others too, 104.30.x)
	};
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
	const g = [];
	for (let i = 0; i < groups; i++) g.push(((b[2 * i] << 8) | b[2 * i + 1]).toString(16));
	return g.join(':') + '::/' + groups * 16;
}
async function limited(env, req, what) {
	const r = await env[what].limit({ key: sender(req) });
	return !r.success;
}

export default {
	async fetch(req, env) {
		const url = new URL(req.url);
		const path = url.pathname;
		loopback_ok = !!env.TURN_STATIC;
		try {
			if (path === '/v1/ping') return json({ ok: true, protocol: PROTOCOL });
			if (req.method !== 'POST') return json({ error: 'bad' }, 404);
			if (await limited(env, req, 'ANY_LIMIT')) return json({ error: 'busy' }, 429);

			if (path === '/v1/host') {
				if (await limited(env, req, 'HOST_LIMIT')) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const me = who(b, req);
				if (!GAMES.test(me.game) || !me.version) return json({ error: 'bad' }, 400);
				// (the host's code from before, a match's Play again: kept if no one's taken it meanwhile)
				// (and only by the host it was: its key from then, so no one else can take the code a guest is waiting on)
				const want = normal_code(b.want), old_key = clean(b.key, 64);
				for (let tries = 0; tries < 8; tries++) {   // (a code already in use: another)
					const code = tries === 0 && code_ok(want) ? want : random_code();
					const r = await room(env, code).fetch('https://room/open', { method: 'POST', body: JSON.stringify({ code, host: me, old_key: tries === 0 ? old_key : '' }) });
					if (r.status === 200) return r;
				}
				return json({ error: 'busy' }, 429);
			}

			if (path === '/v1/join') {
				if (await limited(env, req, 'JOIN_LIMIT')) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const code = normal_code(b.code);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room/join', { method: 'POST', body: JSON.stringify({ guest: who(b, req) }) });
			}

			if (path === '/v1/wait' || path === '/v1/reopen' || path === '/v1/close') {
				if (path === '/v1/wait' && (await limited(env, req, 'WAIT_LIMIT'))) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const code = normal_code(b.code);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room' + path.slice(3), { method: 'POST', body: JSON.stringify({ key: clean(b.key, 64) }) });
			}

			if (path === '/v1/turn') {
				if (await limited(env, req, 'TURN_LIMIT')) return json({ error: 'busy' }, 429);
				const limits = env.LIMITS.get(env.LIMITS.idFromName('turn')), from = sender(req, 3);
				const day = await limits.fetch('https://limits/', { method: 'POST', body: JSON.stringify({ per_day: TURN_PER_DAY, who: from, per_who: TURN_PER_ADDRESS_DAY }) });
				if (day.status === 429) return json({ error: 'no_turn' }, 503);
				if (day.status === 409) return json({ error: 'busy' }, 429);   // (this address's day's used)
				const t = await turn(env, country_of(req));
				if (t.status !== 200) await limits.fetch('https://limits/', { method: 'POST', body: JSON.stringify({ refund: true, who: from }) });
				return t;
			}

			// a random opponent: the nearest of those searching, the same game, version and ROM
			if (path === '/v1/random' || path === '/v1/random/wait' || path === '/v1/random/leave' || path === '/v1/random/count') {
				if (path === '/v1/random' && (await limited(env, req, 'HOST_LIMIT'))) return json({ error: 'busy' }, 429);
				if (path === '/v1/random/wait' && (await limited(env, req, 'WAIT_LIMIT'))) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const me = who(b, req);
				if (!GAMES.test(me.game) || !me.version) return json({ error: 'bad' }, 400);
				const q = env.QUEUES.get(env.QUEUES.idFromName('q|' + me.game + '|' + me.version + '|' + me.content));
				if (path === '/v1/random/count') {   // (asked by everyone on the Online screen: kept 30 s at the edge, the queue asked once a place)
					const key = new Request('https://count.vf-lobby/' + encodeURIComponent(me.game + '|' + me.version + '|' + me.content));
					const edge = !env.TURN_STATIC;   // (not on a test's own lobby: its count's checked as it changes)
					const kept = edge && (await caches.default.match(key));
					if (kept) return kept;
					const r = await q.fetch('https://queue/count', { method: 'POST', body: '{}' });
					const out = new Response(await r.text(), { status: r.status, headers: { 'content-type': 'application/json', 'cache-control': 'max-age=30' } });
					if (edge && r.status === 200) await caches.default.put(key, out.clone());
					return out;
				}
				return q.fetch('https://queue' + (path === '/v1/random' ? '/enter' : path.slice(10)), {
					method: 'POST',
					body: JSON.stringify({ me, geo: geo_of(req), from: sender(req), ticket: clean(b.ticket, 32) }),
				});
			}

			return json({ error: 'bad' }, 404);
		} catch (e) {
			return json({ error: 'server' }, 500);
		}
	},
};

function room(env, code) {
	return env.ROOMS.get(env.ROOMS.idFromName(code));
}

// Cloudflare's TURN: a login for a day, from the TURN key (kept as this Worker's secrets)
async function turn(env, country) {
	// (testing on one's own computer: a TURN server of one's own, as a .dev.vars line TURN_STATIC='[{"host":...}]')
	if (env.TURN_STATIC) return json({ servers: JSON.parse(env.TURN_STATIC), ttl: 86400, country: env.DEV_COUNTRY || country });   // (never set on the real one)
	if (!env.TURN_KEY_ID || !env.TURN_KEY_API_TOKEN) return json({ error: 'no_turn' }, 503);
	const r = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${env.TURN_KEY_ID}/credentials/generate-ice-servers`, {
		method: 'POST',
		headers: { authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`, 'content-type': 'application/json' },
		body: JSON.stringify({ ttl: 86400 }),
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
	return json({ servers, ttl: 86400, country });
}

// One room: the host's details and key, the player who joined, and the host's waits
export class Room {
	constructor(ctx) {
		this.ctx = ctx;
		this.waiters = [];
	}
	async load() {
		if (this.r === undefined) this.r = (await this.ctx.storage.get('r')) || null;
		if (this.gone === undefined) this.gone = (await this.ctx.storage.get('gone')) || null;
		if (this.gone && Date.now() > this.gone.until) {
			this.gone = null;
			await this.ctx.storage.delete('gone');
		}
		const r = this.r;
		if (r && (Date.now() - r.seen > HOST_GONE_MS || Date.now() - r.born > ROOM_LIFE_MS)) await this.drop();
		return this.r;
	}
	async save() {
		await this.ctx.storage.put('r', this.r);
		await this.ctx.storage.setAlarm(Date.now() + HOST_GONE_MS + 1000);
	}
	// Gone: the room, and for ROOM_LIFE_MS its code kept for its host (by the key it had) to open again, a Play
	// again's guest waiting on it; no one else can take it from under them meanwhile
	async drop() {
		const r = this.r;
		this.r = null;
		await this.ctx.storage.deleteAll();
		if (r) {
			this.gone = { key: r.key, until: Date.now() + ROOM_LIFE_MS };
			await this.ctx.storage.put('gone', this.gone);
			await this.ctx.storage.setAlarm(this.gone.until + 1000);
		}
		for (const w of this.waiters.splice(0)) w(null);
	}
	async alarm() {
		const r = await this.load();
		if (r) await this.ctx.storage.setAlarm(Date.now() + HOST_GONE_MS + 1000);
		else if (this.gone) await this.ctx.storage.setAlarm(this.gone.until + 1000);
	}
	async fetch(req) {
		const op = new URL(req.url).pathname;
		const b = await req.json();
		const r = await this.load();
		if (op === '/open') {
			if (r) return json({ error: 'taken' }, 409);
			if (this.gone && (!b.old_key || b.old_key !== this.gone.key)) return json({ error: 'taken' }, 409);
			if (this.gone) {
				this.gone = null;
				await this.ctx.storage.delete('gone');
			}
			this.r = { code: b.code, host: b.host, key: random_hex(16), token: random_hex(8), guest: null, born: Date.now(), seen: Date.now() };
			await this.save();
			return json({ code: b.code, key: this.r.key, token: this.r.token });
		}
		if (!r) return json({ error: 'no_room' }, 404);
		if (op === '/join') {
			const g = b.guest, h = r.host;
			if (g.game !== h.game) return json({ error: 'game' }, 412);
			if (g.version !== h.version) return json({ error: 'version', host_version: h.version }, 412);
			if (g.content !== h.content) return json({ error: 'rom' }, 412);
			if (r.guest) return json({ error: 'full' }, 409);
			r.guest = g;
			await this.save();
			for (const w of this.waiters.splice(0)) w(g);
			return json({ name: h.name, cands: h.cands, relay: h.relay, token: r.token, country: h.country });
		}
		// the host's own: its key
		if (b.key !== r.key) return json({ error: 'no_room' }, 404);
		r.seen = Date.now();
		if (op === '/wait') {
			await this.save();
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
			r.token = random_hex(8);   // (a new session: the last player's token no good for it)
			await this.save();
			return json({ code: r.code, key: r.key, token: r.token });
		}
		if (op === '/close') {
			await this.drop();
			return json({ ok: true });
		}
		return json({ error: 'bad' }, 400);
	}
}

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

// Those searching for a random opponent (one queue per game, version and ROM: only those who can play each other
// meet). Each is matched with the nearest other, by where Cloudflare has them: close at first, further the longer
// either has waited, anyone after 40 s. The one who waited longer hosts: a room's opened as theirs, the other
// joined into it, and from there it's a room's match like any other (its code kept for Play again). Kept in memory
// only: those searching ask every 20 s, and one gone quiet 25 s is dropped.
const RANDOM_WAIT_MS = 20000, RANDOM_GONE_MS = 25000, RANDOM_KEPT_MS = 60000, RANDOM_PER_ADDRESS = 6, RANDOM_PLAYING_MS = 600000, RANDOM_MATCHES_PER_ADDRESS = 30;
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
		ctx.blockConcurrencyWhile(async () => { this.matched = (await ctx.storage.get('matched')) || []; });
		this.made = new Map();      // address -> when its matches were made, the last RANDOM_PLAYING_MS
	}
	prune() {
		const now = Date.now();
		for (const [t, w] of this.waiting)
			if (now - w.seen > (w.result ? RANDOM_KEPT_MS : RANDOM_GONE_MS)) this.waiting.delete(t);
	}
	searching() {
		let n = 0;
		for (const w of this.waiting.values()) if (!w.result) n++;
		return n;
	}
	// The nearest other one searching that this one may be matched with now: only one waiting on an answer this
	// moment (a game gone without saying so has none open)
	nearest(ticket, w) {
		const now = Date.now();
		let pick = null, best = Infinity;
		for (const [t, o] of this.waiting) {
			if (t === ticket || o.result || o.waiters.length === 0) continue;
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
		host.partner = guest;
		guest.partner = host;
		host.room = guest.room = { code: m.host.code, key: m.host.key };
		this.settle(host, m.host);
		this.settle(guest, m.guest);
	}
	async open(host, guest) {
		for (let tries = 0; tries < 8; tries++) {
			const code = random_code();
			const r = await room(this.env, code).fetch('https://room/open', { method: 'POST', body: JSON.stringify({ code, host: host.me }) });
			if (r.status !== 200) continue;
			const h = await r.json();
			const j = await room(this.env, code).fetch('https://room/join', { method: 'POST', body: JSON.stringify({ guest: guest.me }) });
			if (j.status !== 200) { await this.close(code, h.key); return null; }
			const g = await j.json();
			return {
				host: { role: 'host', code, key: h.key, token: h.token, name: guest.me.name, country: guest.me.country },
				guest: { role: 'guest', code, name: g.name, cands: g.cands, relay: g.relay, token: g.token, country: g.country },
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
		return json({ ticket, waiting: true, players: this.searching() });
	}
	async fetch(req) {
		const op = new URL(req.url).pathname;
		const b = await req.json();
		this.prune();
		const now = Date.now();
		if (op === '/count') {
			this.matched = this.matched.filter((t) => now - t < RANDOM_PLAYING_MS);
			return json({ players: this.searching() + 2 * this.matched.length });
		}
		if (op === '/enter') {
			let mine = 0;   // (a few tickets an address at most: no one place soaks up everyone searching)
			for (const o of this.waiting.values()) if (o.from === b.from && !o.result) mine++;
			if (mine >= RANDOM_PER_ADDRESS) return json({ error: 'busy' }, 429);
			// (and so many matches an address lately: no one place collects every player searching)
			const made = (this.made.get(b.from) || []).filter((y) => now - y < RANDOM_PLAYING_MS);
			if (made.length) this.made.set(b.from, made); else this.made.delete(b.from);
			if (made.length >= RANDOM_MATCHES_PER_ADDRESS) return json({ error: 'busy' }, 429);
			const ticket = random_hex(12);
			const w = { me: b.me, geo: b.geo, from: b.from, since: now, seen: now, result: null, waiters: [], partner: null, room: null, left: false };
			this.waiting.set(ticket, w);
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
				await this.close(w.room.code, w.room.key);
				o.result = null;
				o.partner = o.room = null;
				o.since = Date.now();
				for (const done of o.waiters.splice(0)) done();
			}
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
