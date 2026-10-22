// The lobby: room codes for online play (VF1, VF2 and VF3 Recomp). A host opens a room and gets a code; a player
// joins with the code; the lobby hands each the other's addresses (as the game found them: its own network's, its
// public one by STUN, its relay's by TURN) and a token for the session, and the games connect to each other
// themselves. No game traffic comes through here. It also gives out short-lived logins for Cloudflare's TURN relay
// (for the players a direct connection can't reach), so the TURN key never leaves this Worker.
//
//   POST /v1/host   {game, version, content, name, cands, relay}    -> {code, key, token}
//   POST /v1/join   {code, game, version, content, name, cands, relay} -> {name, cands, relay, token}  (or {error})
//   GET  /v1/wait?code=&key=   (the host's: waits up to 20 s for a player) -> {guest: null | {name, cands, relay}}
//   POST /v1/reopen {code, key}   (the host's, after a player who didn't get going: the same code, open again)
//   POST /v1/close  {code, key}
//   POST /v1/turn   {game}  -> {servers: [{host, port, username, credential}], ttl}
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

const json = (body, status = 200) =>
	new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });

// What a game says of itself, checked: short printable text, a few addresses
function clean(s, max) {
	return typeof s === 'string' ? s.replace(/[^\x20-\x7e]/g, '').slice(0, max) : '';
}
function addrs(list) {
	if (!Array.isArray(list)) return [];
	return list.slice(0, 8).map((a) => clean(a, 64)).filter((a) => /^[0-9a-fA-F:.\[\]]+:\d{1,5}$/.test(a));
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

// Too many tries from one address: a room code guessed at, the relay's logins farmed
async function limited(env, req, what, per_minute) {
	const ip = req.headers.get('cf-connecting-ip') || 'local';
	const stub = env.LIMITS.get(env.LIMITS.idFromName(ip));
	const r = await stub.fetch('https://limits/', { method: 'POST', body: JSON.stringify({ what, per_minute }) });
	return r.status === 429;
}

export default {
	async fetch(req, env) {
		const url = new URL(req.url);
		const path = url.pathname;
		try {
			if (path === '/v1/ping') return json({ ok: true, protocol: PROTOCOL });

			if (path === '/v1/host' && req.method === 'POST') {
				if (await limited(env, req, 'host', 20)) return json({ error: 'busy' }, 429);
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

			if (path === '/v1/join' && req.method === 'POST') {
				if (await limited(env, req, 'join', 30)) return json({ error: 'busy' }, 429);
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const code = normal_code(b.code);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room/join', { method: 'POST', body: JSON.stringify({ guest: who(b) }) });
			}

			if (path === '/v1/wait' && req.method === 'GET') {
				const code = normal_code(url.searchParams.get('code'));
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room/wait', { method: 'POST', body: JSON.stringify({ key: clean(url.searchParams.get('key'), 64) }) });
			}

			if ((path === '/v1/reopen' || path === '/v1/close') && req.method === 'POST') {
				const b = await body_of(req);
				if (!b) return json({ error: 'bad' }, 400);
				const code = normal_code(b.code);
				if (!code_ok(code)) return json({ error: 'no_room' }, 404);
				return room(env, code).fetch('https://room' + path.slice(3), { method: 'POST', body: JSON.stringify({ key: clean(b.key, 64) }) });
			}

			if (path === '/v1/turn' && req.method === 'POST') {
				if (await limited(env, req, 'turn', 10)) return json({ error: 'busy' }, 429);
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
	if (env.TURN_STATIC) return json({ servers: JSON.parse(env.TURN_STATIC), ttl: 86400 });
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
				setTimeout(() => done(undefined), WAIT_MS);
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

// Tries per address per minute (in memory: a limit, not a record; a restart forgets it, as it may)
export class Limits {
	constructor(ctx) {
		this.ctx = ctx;
		this.counts = new Map();
	}
	async fetch(req) {
		const { what, per_minute } = await req.json();
		const minute = Math.floor(Date.now() / 60000);
		const k = what + ':' + minute;
		const n = (this.counts.get(k) || 0) + 1;
		this.counts.set(k, n);
		for (const key of this.counts.keys()) if (!key.endsWith(':' + minute)) this.counts.delete(key);
		return new Response(null, { status: n > per_minute ? 429 : 204 });
	}
}
