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
// none, multicast or broadcast, nor port 0 (what a game would otherwise be made to send to by the other player)
let loopback_ok = false;   // (testing on one's own computer, TURN_STATIC set: the games' addresses are its own)
function addr_ok(a) {
	const m = /^(?:(\d{1,3}(?:\.\d{1,3}){3})|\[([0-9a-fA-F:.]+)\]):(\d{1,5})$/.exec(a);
	if (!m || +m[3] < 1 || +m[3] > 65535) return false;
	if (m[1]) {
		const b = m[1].split('.').map(Number);
		return b.every((x) => x <= 255) && b[0] !== 0 && (b[0] !== 127 || loopback_ok) && b[0] < 224;
	}
	const v6 = m[2].toLowerCase();
	return v6 !== '::' && v6 !== '::1' && !v6.startsWith('ff') && !v6.startsWith('::ffff:127.') && !v6.startsWith('::ffff:0.');
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
	return {
		country: country_of(req),   // (the lobby's own: never what a game says of itself)
		game: clean(b.game, 16),
		version: clean(b.version, 64),
		content: clean(b.content, 32),
		name: clean(b.name, 10),
		cands: addrs(b.cands),
		relay: typeof b.relay === 'string' && addrs([b.relay]).length ? addrs([b.relay])[0] : '',
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
	if ((req.headers.get('content-length') | 0) > 4096) return null;
	try {
		const t = await req.text();
		if (t.length > 4096) return null;
		const b = JSON.parse(t);
		return b && typeof b === 'object' ? b : null;
	} catch {
		return null;
	}
}

// Too many tries from one address (an IPv6 one by its /64, as one home's are): a room code guessed at, the relay's
// logins farmed, the lobby flooded. Cloudflare's rate limiting (per place, roughly: a limit, not a count).
function sender(req) {
	const ip = req.headers.get('cf-connecting-ip') || 'local';
	return ip.includes(':') ? ip.split(':').slice(0, 4).join(':') : ip;
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
				if (!me.game || !me.version) return json({ error: 'bad' }, 400);
				// (the host's code from before, a match's Play again: kept if no one's taken it meanwhile)
				const want = normal_code(b.want);
				for (let tries = 0; tries < 8; tries++) {   // (a code already in use: another)
					const code = tries === 0 && code_ok(want) ? want : random_code();
					const r = await room(env, code).fetch('https://room/open', { method: 'POST', body: JSON.stringify({ code, host: me }) });
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
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const code = normal_code(b.code);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room' + path.slice(3), { method: 'POST', body: JSON.stringify({ key: clean(b.key, 64) }) });
			}

			if (path === '/v1/turn') {
				if (await limited(env, req, 'TURN_LIMIT')) return json({ error: 'busy' }, 429);
				const day = await env.LIMITS.get(env.LIMITS.idFromName('turn')).fetch('https://limits/', { method: 'POST', body: JSON.stringify({ per_day: TURN_PER_DAY }) });
				if (day.status === 429) return json({ error: 'no_turn' }, 503);
				return turn(env, country_of(req));
			}

			// a random opponent: the nearest of those searching, the same game, version and ROM
			if (path === '/v1/random' || path === '/v1/random/wait' || path === '/v1/random/leave') {
				if (path === '/v1/random' && (await limited(env, req, 'HOST_LIMIT'))) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const me = who(b, req);
				if (!me.game || !me.version) return json({ error: 'bad' }, 400);
				const q = env.QUEUES.get(env.QUEUES.idFromName('q|' + me.game + '|' + me.version + '|' + me.content));
				return q.fetch('https://queue' + (path === '/v1/random' ? '/enter' : path.slice(10)), {
					method: 'POST',
					body: JSON.stringify({ me, geo: geo_of(req), ticket: clean(b.ticket, 32) }),
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
		const r = this.r;
		if (r && (Date.now() - r.seen > HOST_GONE_MS || Date.now() - r.born > ROOM_LIFE_MS)) await this.drop();
		return this.r;
	}
	async save() {
		await this.ctx.storage.put('r', this.r);
		await this.ctx.storage.setAlarm(Date.now() + HOST_GONE_MS + 1000);
	}
	async drop() {
		this.r = null;
		await this.ctx.storage.deleteAll();
		for (const w of this.waiters.splice(0)) w(null);
	}
	async alarm() {
		const r = await this.load();
		if (r) await this.ctx.storage.setAlarm(Date.now() + HOST_GONE_MS + 1000);
	}
	async fetch(req) {
		const op = new URL(req.url).pathname;
		const b = await req.json();
		const r = await this.load();
		if (op === '/open') {
			if (r) return json({ error: 'taken' }, 409);
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
				this.waiters.push(done);
				setTimeout(() => {
					this.waiters = this.waiters.filter((w) => w !== done);
					done(undefined);
				}, WAIT_MS);
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
export class Limits {
	constructor(ctx) {
		this.ctx = ctx;
	}
	async fetch(req) {
		const { per_day } = await req.json();
		const day = Math.floor(Date.now() / 86400e3);
		const c = (await this.ctx.storage.get('c')) || { day, n: 0 };
		if (c.day !== day) { c.day = day; c.n = 0; }
		if (c.n >= per_day) return new Response(null, { status: 429 });
		c.n++;
		await this.ctx.storage.put('c', c);
		return new Response(null, { status: 204 });
	}
}

// Those searching for a random opponent (one queue per game, version and ROM: only those who can play each other
// meet). Each is matched with the nearest other, by where Cloudflare has them: close at first, further the longer
// either has waited, anyone after 40 s. The one who waited longer hosts: a room's opened as theirs, the other
// joined into it, and from there it's a room's match like any other (its code kept for Play again). Kept in memory
// only: those searching ask every 20 s, and one gone quiet 25 s is dropped.
const RANDOM_WAIT_MS = 20000, RANDOM_GONE_MS = 25000, RANDOM_KEPT_MS = 60000;
function reach_km(seconds) {
	return seconds < 8 ? 1500 : seconds < 20 ? 4000 : seconds < 40 ? 9000 : Infinity;
}
function km(a, b) {
	if (!a || !b) return 0;   // (not known: as near as can be)
	const r = Math.PI / 180, dlat = (b.lat - a.lat) * r, dlon = (b.lon - a.lon) * r;
	const h = Math.sin(dlat / 2) ** 2 + Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(dlon / 2) ** 2;
	return 12742 * Math.asin(Math.min(1, Math.sqrt(h)));
}

export class Queue {
	constructor(ctx, env) {
		this.ctx = ctx;
		this.env = env;
		this.waiting = new Map();   // ticket -> { me, geo, since, seen, result, waiters }
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
	// The nearest other one searching that this one may be matched with now
	nearest(ticket, w) {
		const now = Date.now();
		let pick = null, best = Infinity;
		for (const [t, o] of this.waiting) {
			if (t === ticket || o.result) continue;
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
		const m = await this.open(host, guest);
		if (!m) { w.result = o.result = null; return; }
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
			if (j.status !== 200) return null;
			const g = await j.json();
			return {
				host: { role: 'host', code, key: h.key, token: h.token, name: guest.me.name, country: guest.me.country },
				guest: { role: 'guest', code, name: g.name, cands: g.cands, relay: g.relay, token: g.token, country: g.country },
			};
		}
		return null;
	}
	settle(w, result) {
		w.result = result;
		w.seen = Date.now();
		for (const done of w.waiters.splice(0)) done();
	}
	answer(ticket, w) {
		if (w.result && w.result !== 'pairing') {
			this.waiting.delete(ticket);
			return json(w.result);
		}
		return json({ ticket, waiting: true, players: this.searching() });
	}
	async fetch(req) {
		const op = new URL(req.url).pathname;
		const b = await req.json();
		this.prune();
		const now = Date.now();
		if (op === '/enter') {
			const ticket = random_hex(12);
			const w = { me: b.me, geo: b.geo, since: now, seen: now, result: null, waiters: [] };
			this.waiting.set(ticket, w);
			await this.try_match(ticket, w);
			return this.answer(ticket, w);
		}
		const w = this.waiting.get(b.ticket);
		if (!w) return json({ error: 'gone' }, 404);
		w.seen = now;
		if (op === '/leave') {
			this.waiting.delete(b.ticket);
			return json({ ok: true });
		}
		if (op === '/wait') {
			if (!w.result) await this.try_match(b.ticket, w);
			if (!w.result || w.result === 'pairing')
				await new Promise((done) => {
					w.waiters.push(done);
					setTimeout(() => {
						w.waiters = w.waiters.filter((x) => x !== done);
						done();
					}, RANDOM_WAIT_MS);
				});
			w.seen = Date.now();
			return this.answer(b.ticket, w);
		}
		return json({ error: 'bad' }, 400);
	}
}
