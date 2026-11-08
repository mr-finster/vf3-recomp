// fntest: functions/api/online.js against a stand-in lobby (fetch and the edge cache replaced): what it passes on,
// and how it asks: given 4 s, its last answer kept up to 2 min while the lobby fails, a failure remembered 5 s.
//   node tools/online/fntest.mjs
import { fileURLToPath } from "node:url";

let lobby = { mode: "ok", body: null, fetches: 0, delay: 0 };
const store = new Map();
globalThis.caches = { default: {
	match: async (k) => { const e = store.get(k.url); return e ? new Response(e) : undefined; },
	put: async (k, r) => { store.set(k.url, await r.text()); },
} };
globalThis.fetch = (url, init) => {
	lobby.fetches++;
	return new Promise((ok, no) => {
		const signal = init && init.signal;
		if (signal) signal.addEventListener("abort", () => no(signal.reason));
		if (lobby.mode === "hang") return;
		if (lobby.mode === "down") return setTimeout(() => no(new TypeError("fetch failed: lobby.example.com")), 1);
		setTimeout(() => ok(lobby.mode === "500" ? new Response("error at lobby.example.com", { status: 500 }) : new Response(JSON.stringify(lobby.body()), { status: 200 })), lobby.delay);
	});
};
const now_s = () => Math.floor(Date.now() / 1000);
const room = (state, extra) => ({ game: "vf3", state, name: "KOREA FINALS", code: "ABCDEF", p1: { name: "HOST", country: "KR" }, ...(state === "playing" ? { watch: "GHJKMN", p2: { name: "GUEST", country: "JP" }, score: [1, 0] } : {}), ...extra });
const answer = (extra) => () => ({ updated: now_s(), games: { vf3: { total: 3, queued: 1, watching: 5 } }, countries: { KR: 2 }, rooms: [room("waiting"), room("playing", { code: "PQRSTU" })], features: { rooms: true, watch: true }, ...extra });

let bad = 0, n = 0;
const check = (ok, what, got) => { n++; if (!ok) bad++; console.log((ok ? "ok   " : "FAIL ") + what + (got !== undefined ? ": " + JSON.stringify(got) : "")); };
// (a new copy of the function: its memory and the cache emptied)
const fresh_copy = async () => { store.clear(); return import(fileURLToPath(new URL("functions/api/online.js", import.meta.url)) + "?" + Math.random()); };
let f = null;
const run = async (continent) => {
	const request = new Request("https://vf-online.example/api/online");
	Object.defineProperty(request, "cf", { value: continent ? { continent } : {} });
	const r = await Promise.race([f.onRequestGet({ request, env: { LOBBY_URL: "https://lobby.example.com" }, waitUntil: (p) => p }),
		new Promise((ok) => setTimeout(() => ok(new Response("still waiting at 8 s", { status: 599 })), 8000))]);
	const text = await r.text();
	let json = null;
	try { json = JSON.parse(text); } catch (e) {}
	return { status: r.status, cc: r.headers.get("cache-control"), text, json };
};
const reset = async (mode, body) => { f = await fresh_copy(); lobby = { mode, body: body || answer(), fetches: 0, delay: 0 }; };

const alive = setInterval(() => {}, 1000);   // (AbortSignal.timeout's timer keeps no Node process up by itself)

// what's passed on
await reset("ok");
let r = await run("EU");
check(r.status === 200 && r.json.rooms.length === 2, "rooms and a match passed on", r.json && r.json.rooms.length);
check(r.json.games.vf3.total === 3 && r.json.games.vf3.queued === 1 && r.json.games.vf3.watching === 5 && r.json.games.vf1.total === 0 && r.json.games.vf1.watching === 0, "the counts passed on: players (a room's line among them), watchers apart", r.json.games.vf3);
check(r.json.you === "EU" && /^private/.test(r.cc), "the viewer's own answer (with you) kept private", r.cc);
check(!/workers\.dev/.test(r.text), "nothing of the lobby's address");
r = await run("");
check(/^public/.test(r.cc) && !("you" in r.json), "without a continent, public", r.cc);
await reset("ok", answer({ features: undefined }));
r = await run();
check(r.json.features.rooms === false && !("rooms" in r.json), "no features (an older lobby): rooms off", r.json.features);
await reset("ok", answer({ features: { watch: true } }));
r = await run();
check(r.json.features.rooms === false && !("rooms" in r.json), "features without rooms: true: rooms off", r.json.features);
await reset("ok", answer({ features: { rooms: true, watch: false } }));
r = await run();
check(r.json.features.watch === false && r.json.rooms.length === 1 && r.json.rooms[0].state === "waiting", "watching off: open rooms only", r.json.rooms.map((x) => x.state));
await reset("ok", answer({ updated: now_s() + 86400 * 365 }));
r = await run();
check(r.json.updated <= now_s(), "a time from the future taken as now", r.json.updated - now_s());

// 30 at once with nothing kept: each asks for itself (no promise shared between requests), all answered
await reset("ok");
lobby.delay = 50;
let all = await Promise.all(Array.from({ length: 30 }, () => run("EU")));
check(all.every((x) => x.status === 200), "30 requests at once with nothing kept: all answered", lobby.fetches);
lobby.fetches = 0;
// (kept: the next 10 none)
for (let i = 0; i < 10; i++) await run("EU");
check(lobby.fetches === 0, "while fresh: no fetch", lobby.fetches);

// a hung lobby: given up at 4 s
await reset("hang");
let t0 = Date.now();
r = await run();
check(r.status === 502 && Date.now() - t0 >= 3900 && Date.now() - t0 < 5000, "a hung lobby given up after 4 s (unavailable)", [r.status, Date.now() - t0]);
// (while it's down: asked again only after 5 s, everyone else told at once)
t0 = Date.now();
all = await Promise.all(Array.from({ length: 10 }, () => run()));
check(lobby.fetches === 1 && all.every((x) => x.status === 502) && Date.now() - t0 < 100, "10 more within 5 s of the failure: no fetch, answered at once", [lobby.fetches, Date.now() - t0]);

// the lobby down after an answer: the last one passed on, stale, up to 2 min
await reset("ok");
r = await run("EU");
lobby.mode = "down";
const realNow = Date.now;
const shift = (ms) => { Date.now = () => realNow() + ms; };   // (the clock moved on)
shift(20000);   // (the answer 20 s old: not fresh)
r = await run("EU");
check(r.status === 200 && r.json.stale === true && r.json.you === "EU" && r.cc === "no-store" && r.json.rooms.length === 2, "lobby down, answer 20 s old: passed on stale", [r.status, r.json && r.json.stale, r.cc]);
const fetches = lobby.fetches;
for (let i = 0; i < 10; i++) await run();
check(lobby.fetches === fetches, "10 more during the outage within 5 s: no fetch", lobby.fetches - fetches);
shift(30000);
r = await run();
check(lobby.fetches === fetches + 1 && r.json && r.json.stale === true, "after 5 s: asked again, still stale", lobby.fetches - fetches);
lobby.mode = "500";
shift(130000);   // (2 min 10 s: too old to pass on)
r = await run();
check(r.status === 502 && r.text === '{"error":"unavailable"}', "lobby down, answer over 2 min old: unavailable", [r.status, r.text]);
check(!/workers\.dev/.test(r.text), "the lobby's error text not passed on");
lobby.mode = "ok";
shift(140000);
r = await run();
check(r.status === 200 && !r.json.stale, "the lobby back: fresh again", r.json && r.json.stale);
Date.now = realNow;
clearInterval(alive);

console.log(bad ? `fntest: ${bad} failed of ${n}` : `fntest: all ${n} passed`);
process.exit(bad ? 1 : 0);
