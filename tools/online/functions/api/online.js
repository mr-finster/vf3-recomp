// GET /api/online: the lobby's who's-online counts (its GET /v1/online), for the page beside it. The lobby's address
// is the Pages project's LOBBY_URL variable (Settings > Variables and Secrets), never in the site's files; and nothing
// from the lobby's answer passes through as it came: only the counts, read and written out again here, so its
// address can't reach a browser in a header, an error or a field. Kept while the counts are under FRESH_S old (this
// place's cache, and this copy's memory: by the counts' own time, so the lobby's copies and these never add up).
const GAMES = ['vf1', 'vf2', 'vf3', 'vf3tb'];
const STATES = ['total', 'searching', 'random', 'hosting', 'room'];
const FRESH_S = 15, MAX_AGE = 10;
let kept = null;   // ({t, body}: this copy's last answer, t when the counts were made)
const fresh = (t) => Date.now() - t < FRESH_S * 1000;
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
function clean(d) {
	const games = {};
	for (const g of GAMES) {
		const src = (d && d.games && d.games[g]) || {};
		games[g] = { ...counts(src), older: count(src.older), countries: lands(src.countries) };   // (one count a game: who one can meet; older: of them, on an older version)
	}
	const t = d && d.updated;
	return { updated: Number.isInteger(t) && t > 0 && t < 1e11 ? t : 0, games, countries: lands(d && d.countries) };
}

export async function onRequestGet({ request, env, waitUntil }) {
	const ok = (body) => out(body, 200, `public, max-age=${MAX_AGE}`);
	if (kept && fresh(kept.t)) return ok(kept.body);
	const key = new Request(new URL('/api/online', request.url).toString());
	const cache = caches.default;
	const hit = await cache.match(key).catch(() => null);
	if (hit) {
		const body = await hit.text(), t = made_at(body);
		if (fresh(t)) { kept = { t, body }; return ok(body); }
	}
	const base = typeof env.LOBBY_URL === 'string' ? env.LOBBY_URL.trim().replace(/\/+$/, '') : '';
	if (!/^https?:\/\/[^/?#\s]+$/.test(base)) return unavailable();
	let body;
	try {
		const r = await fetch(base + '/v1/online', { headers: { accept: 'application/json' }, cf: { cacheTtl: 0 } });
		if (r.status !== 200) return unavailable();
		body = JSON.stringify(clean(await r.json()));
	} catch (e) {
		return unavailable();   // (never the error's text: it could name the lobby)
	}
	kept = { t: made_at(body) || Date.now(), body };
	waitUntil(cache.put(key, out(body, 200, 'public, max-age=30')).catch(() => {}));
	return ok(body);
}
