// GET /api/online: the lobby's who's-online counts and rooms list (its GET /v1/online), for the page beside it. The lobby's address
// is the Pages project's LOBBY_URL variable (Settings > Variables and Secrets), never in the site's files; and nothing
// from the lobby's answer passes through as it came: only the counts, read and written out again here, so its
// address can't reach a browser in a header, an error or a field. Kept while the counts are under FRESH_S old (this
// place's cache, and this copy's memory: by the counts' own time, so the lobby's copies and these never add up).
// The lobby given TIMEOUT_MS; when it fails the last answer is passed on up to STALE_S old marked "stale", and it isn't
// asked again for FAIL_S. (Only plain data is kept between requests, never a promise: on Workers one request can't
// safely wait on another's fetch, so each request that misses asks for itself.)
const GAMES = ['vf1', 'vf2', 'vf3', 'vf3tb'];
const STATES = ['total', 'searching', 'random', 'hosting', 'queued', 'room'];   // (total: the players, each once; queued: in a room's line)
const FRESH_S = 15, MAX_AGE = 10, STALE_S = 120, FAIL_S = 5, TIMEOUT_MS = 4000;
let kept = null;   // ({t, body}: this copy's last answer, t when the counts were made)
let failed_at = 0;   // (when the lobby last couldn't be reached: not asked again for FAIL_S)
const age = (t) => Date.now() - t;
const fresh = (t) => age(t) < FRESH_S * 1000;
const made_at = (body) => { try { return JSON.parse(body).updated * 1000; } catch (e) { return 0; } };

const out = (body, status, cache) =>
	new Response(body, { status, headers: { 'content-type': 'application/json', 'cache-control': cache, 'access-control-allow-origin': '*' } });   // (any page may read the counts: the download sites' Online now button)
const unavailable = () => out('{"error":"unavailable"}', 502, 'no-store');

const count = (n) => (Number.isInteger(n) && n >= 0 && n < 1e7 ? n : 0);
function counts(c) {
	const o = {};
	for (const k of STATES) o[k] = count(c && c[k]);
	return o;
}
// (players by country: a country's two letters, or "" not known; at most MAX_COUNTRIES, most first)
const MAX_COUNTRIES = 250;
function lands(c) {
	const list = c && typeof c === 'object' && !Array.isArray(c) ? Object.entries(c) : [];
	const ok = list.filter(([k, n]) => (k === '' || /^[A-Z]{2}$/.test(k)) && count(n) > 0).map(([k, n]) => [k, count(n)]);
	return Object.fromEntries(ok.sort((x, y) => y[1] - x[1] || (x[0] < y[0] ? -1 : 1)).slice(0, MAX_COUNTRIES));
}
// (the rooms list: each field checked and written out again, nothing else of the lobby's passed on; a player's name
// only the letters a game's names have)
const NAME = /^[A-Z0-9 .!?-]{1,10}$/, ROOM = /^[A-Z0-9 .!?'-]{1,16}$/, CODE = /^[A-HJKMNP-Z2-9]{6}$/, REGION = /^(AF|AN|AS|EU|NA|OC|SA)$/, MAX_ROOMS = 400;
function who(p) {
	return p && typeof p === 'object' && typeof p.name === 'string' && NAME.test(p.name) ? { name: p.name, country: typeof p.country === 'string' && /^[A-Z]{2}$/.test(p.country) ? p.country : '' } : null;
}
function rooms(list, watch) {
	const out = [];
	for (const r of Array.isArray(list) ? list : []) {
		if (out.length >= MAX_ROOMS || !r || typeof r !== 'object' || !GAMES.includes(r.game)) continue;
		const p1 = who(r.p1), locked = r.locked === true;
		const base = { game: r.game, name: typeof r.name === 'string' && ROOM.test(r.name) ? r.name : '', locked, region: REGION.test(r.region) ? r.region : '', since: count(r.since),
			older: r.older === true, queue: Math.min(count(r.queue), 99), watchers: count(r.watchers), ...(!locked && CODE.test(r.code) ? { code: r.code } : {}) };
		if (!p1) continue;
		if (r.state === 'waiting' && (base.code || locked)) out.push({ ...base, state: 'waiting', p1 });
		else if (watch && r.state === 'playing' && who(r.p2) && (CODE.test(r.watch) || base.code)) {
			const score = Array.isArray(r.score) && r.score.length === 2 ? r.score.map((x) => Math.min(count(x), 99)) : [0, 0];
			const { code: _, ...watch_only } = base;   // (a Quick match: to watch only, no code to join, no room name)
			const quick = r.random === true && CODE.test(r.watch);
			out.push({ ...(quick ? { ...watch_only, name: '', queue: 0, random: true } : base), state: 'playing', ...(CODE.test(r.watch) ? { watch: r.watch } : {}), p1, p2: who(r.p2), score, round: Math.min(count(r.round), 99) });
		}
	}
	return out;
}
function clean(d) {
	const games = {};
	for (const g of GAMES) {
		const src = (d && d.games && d.games[g]) || {};
		// (one count a game: who one can meet; older: of them, on an older version; watching: those watching its matches,
		// apart from total, 0 from a lobby that doesn't say)
		games[g] = { ...counts(src), older: count(src.older), countries: lands(src.countries), watching: count(src.watching) };
	}
	const t = d && d.updated;
	// (what the lobby has on, its switches': each on only when it says so; no features, as from an older lobby, is
	// both off; watching off, its matches aren't listed: open rooms only)
	const f = d && d.features && typeof d.features === 'object' ? d.features : null;
	const has_rooms = !!f && f.rooms === true && Array.isArray(d.rooms), watch = !!f && f.watch === true;
	// (the counts' time no later than now: one from the future would keep them fresh for ever)
	return { updated: Number.isInteger(t) && t > 0 ? Math.min(t, Math.floor(Date.now() / 1000)) : 0, games, countries: lands(d && d.countries), features: { rooms: has_rooms, watch },
		...(has_rooms ? { rooms: rooms(d.rooms, watch) } : {}) };
}

export async function onRequestGet(ctx) {
	const r = await counts_kept(ctx);
	// (the viewer's own continent, as Cloudflare places them, beside the kept answer: the rooms near them first; so this
	// answer is the viewer's own, kept by no one else)
	const c = ctx.request.cf && typeof ctx.request.cf.continent === 'string' ? ctx.request.cf.continent : '';
	if (r.status !== 200 || !REGION.test(c)) return r;
	const body = await r.text();
	return out(body.slice(0, -1) + ',"you":"' + c + '"}', 200, r.headers.get('cache-control') === 'no-store' ? 'no-store' : `private, max-age=${MAX_AGE}`);
}
// (the lobby's answer, cleaned; null if it can't be had in TIMEOUT_MS)
async function ask(base) {
	try {
		const r = await fetch(base + '/v1/online', { headers: { accept: 'application/json' }, cf: { cacheTtl: 0 }, signal: AbortSignal.timeout(TIMEOUT_MS) });
		if (r.status !== 200) return null;
		return JSON.stringify(clean(await r.json()));
	} catch (e) {
		return null;   // (never the error's text: it could name the lobby)
	}
}
async function counts_kept({ request, env, waitUntil }) {
	const ok = (body) => out(body, 200, `public, max-age=${MAX_AGE}`);
	if (kept && fresh(kept.t)) return ok(kept.body);
	const key = new Request(new URL('/api/online', request.url).toString());
	const cache = caches.default;
	const hit = await cache.match(key).catch(() => null);
	if (hit) {
		const body = await hit.text(), t = made_at(body);
		if (!kept || t > kept.t) kept = { t, body };   // (another copy's, newer: kept here too, if only to pass on stale)
		if (fresh(t)) return ok(body);
	}
	// (the last answer, up to STALE_S old, marked stale, while the lobby can't be reached; else unavailable)
	const stale = () => (kept && age(kept.t) < STALE_S * 1000 ? out(kept.body.slice(0, -1) + ',"stale":true}', 200, 'no-store') : unavailable());
	if (Date.now() - failed_at < FAIL_S * 1000) return stale();
	const base = typeof env.LOBBY_URL === 'string' ? env.LOBBY_URL.trim().replace(/\/+$/, '') : '';
	if (!/^https?:\/\/[^/?#\s]+$/.test(base)) return unavailable();
	const body = await ask(base);
	if (body === null) { failed_at = Date.now(); return stale(); }
	failed_at = 0;
	kept = { t: made_at(body) || Date.now(), body };
	waitUntil(cache.put(key, out(body, 200, `public, max-age=${STALE_S}`)).catch(() => {}));   // (kept there as long as it may be passed on stale; fresh by its own time)
	return ok(body);
}
