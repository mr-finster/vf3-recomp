// GET /api/online: the lobby's who's-online counts (its GET /v1/online), for the page beside it. The lobby's address
// is the Pages project's LOBBY_URL variable (Settings > Variables and Secrets), never in the site's files; and nothing
// from the lobby's answer passes through as it came: only the counts, read and written out again here, so its
// address can't reach a browser in a header, an error or a field. Kept 30 s (this place's cache, and this copy's memory).
const GAMES = ['vf1', 'vf2', 'vf3', 'vf3tb'];
const STATES = ['total', 'searching', 'random', 'hosting', 'room'];
const KEPT_S = 30;
let kept = null;   // ({until, body}: this copy's last answer)

const out = (body, status, cache) =>
	new Response(body, { status, headers: { 'content-type': 'application/json', 'cache-control': cache } });
const unavailable = () => out('{"error":"unavailable"}', 502, 'no-store');

const count = (n) => (Number.isInteger(n) && n >= 0 && n < 1e7 ? n : 0);
function counts(c) {
	const o = {};
	for (const k of STATES) o[k] = count(c && c[k]);
	return o;
}
function clean(d) {
	const games = {};
	for (const g of GAMES) {
		const src = (d && d.games && d.games[g]) || {}, variants = {};
		const list = src.variants && typeof src.variants === 'object' ? Object.entries(src.variants).slice(0, 32) : [];
		for (const [name, c] of list) if (/^[a-z0-9]+(\+[a-z0-9]+)*$/.test(name) && name.length <= 32) variants[name] = counts(c);
		games[g] = { ...counts(src), variants };
	}
	const t = d && d.updated;
	return { updated: Number.isInteger(t) && t > 0 && t < 1e11 ? t : 0, games };
}

export async function onRequestGet({ request, env, waitUntil }) {
	if (kept && Date.now() < kept.until) return out(kept.body, 200, `public, max-age=${KEPT_S}`);
	const key = new Request(new URL('/api/online', request.url).toString());
	const cache = caches.default;
	const hit = await cache.match(key).catch(() => null);
	if (hit) return out(await hit.text(), 200, `public, max-age=${KEPT_S}`);
	const base = typeof env.LOBBY_URL === 'string' ? env.LOBBY_URL.trim().replace(/\/+$/, '') : '';
	if (!/^https?:\/\/[^/?#\s]+$/.test(base)) return unavailable();
	let body;
	try {
		const r = await fetch(base + '/v1/online', { headers: { accept: 'application/json' } });
		if (r.status !== 200) return unavailable();
		body = JSON.stringify(clean(await r.json()));
	} catch (e) {
		return unavailable();   // (never the error's text: it could name the lobby)
	}
	kept = { until: Date.now() + KEPT_S * 1000, body };
	const res = out(body, 200, `public, max-age=${KEPT_S}`);
	waitUntil(cache.put(key, res.clone()).catch(() => {}));
	return res;
}
