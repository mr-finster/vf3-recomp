// The lobby: room codes for online play (VF1, VF2 and VF3 Recomp). A host opens a room and gets a code; a player
// joins with the code; the lobby hands each the other's addresses (as the game found them: its own network's, its
// public one by STUN, its relay's by TURN) and a token for the session, and the games connect to each other
// themselves. No game traffic comes through here. It also gives out short-lived logins for Cloudflare's TURN relay
// (for the players a direct connection can't reach), so the TURN key never leaves this Worker.
//
//   POST /v1/host   {game, version, content, name, cands, relay}    -> {code, key, token}
//   POST /v1/join   {code, game, version, content, name, cands, relay} -> {name, cands, relay, token}  (or {error})
//   POST /v1/wait   {code, key}   (the host's: waits up to 20 s for a player) -> {guest: null | {name, cands, relay}}
//   POST /v1/reopen {code, key}   (the host's, after a player who didn't get going: the same code, open again)
//   POST /v1/close  {code, key}
//   POST /v1/turn   {game}  -> {servers: [{host, port, username, credential}], ttl}   (TURN_PER_DAY a day, all told)
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
function who(b) {
	return {
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
				const me = who(b);
				if (!me.game || !me.version) return json({ error: 'bad' }, 400);
				for (let tries = 0; tries < 8; tries++) {   // (a code already in use: another)
					const code = random_code();
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
				return room(env, code).fetch('https://room/join', { method: 'POST', body: JSON.stringify({ guest: who(b) }) });
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
				return turn(env);
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
async function turn(env) {
	// (testing on one's own computer: a TURN server of one's own, as a .dev.vars line TURN_STATIC='[{"host":...}]')
	if (env.TURN_STATIC) return json({ servers: JSON.parse(env.TURN_STATIC), ttl: 86400 });   // (never set on the real one)
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
	return json({ servers, ttl: 86400 });
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
			return json({ name: h.name, cands: h.cands, relay: h.relay, token: r.token });
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
