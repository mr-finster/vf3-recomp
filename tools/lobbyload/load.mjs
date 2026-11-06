// lobbyload: many simulated players against a lobby (tools/lobby: the live one, staging, wrangler dev or
// test/serve.mjs), from one machine, cheaply: hosts with rooms that stay and their guests, Quick match pairs, each
// match sent as the game sends it (its WebSocket, 5 uploads a second while watched and one every 30 s while not, or
// HTTPS POSTs), watchers on WebSockets and the fallbacks (the HTTPS stream, asking for pieces), browsers reading the
// rooms list. It counts what the lobby answered (by route and status, the time it took) and every error.
//   node tools/lobbyload/load.mjs --lobby URL [--rooms 50] [--quick 20] [--watchers 200] [--browsers 50]
//        [--seconds 120] [--https 0.1] [--game vf3] [--json]
// (--https: the share of hosts and watchers on the fallbacks, as on networks that carry no WebSocket.) Node 22 or
// later (its fetch and WebSocket), nothing installed. Every player is from this machine's one address: a lobby's
// per-address limits answer 429 past them (staging's are set high for this; counted, not an error of the lobby's).
const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : d; };
const LOBBY = String(arg('lobby', '')).replace(/\/+$/, '');
if (!/^https?:\/\/[^/]+$/.test(LOBBY)) { console.log('lobbyload --lobby URL [--rooms N] [--quick N] [--watchers N] [--browsers N] [--seconds S] [--https share] [--game vf3] [--json]'); process.exit(2); }
const ROOMS = +arg('rooms', 50), QUICK = +arg('quick', 20), WATCHERS = +arg('watchers', 200), BROWSERS = +arg('browsers', 50);
const SECONDS = +arg('seconds', 120), HTTPS = Math.min(Math.max(+arg('https', 0.1), 0), 1), GAME = arg('game', 'vf3'), AS_JSON = process.argv.includes('--json');
const WS_URL = LOBBY.replace(/^http/, 'ws');
const t0 = Date.now(), end = t0 + SECONDS * 1000;
const live = () => Date.now() < end;
const sleep = (ms) => new Promise((ok) => setTimeout(ok, Math.max(0, Math.min(ms, end - Date.now() + 50))));   // (none past the run's end)
let seq = 0;
const rnd = (n) => Math.floor(Math.random() * n);
const hex = (n) => [...Array(n)].map(() => rnd(16).toString(16)).join('');
const name = (p) => (p + (++seq).toString(36)).toUpperCase().slice(0, 10);

// ---- what came back: by route and status, the time, the errors; the sockets' messages ----
const stats = { routes: {}, errors: {}, ws: { opened: 0, refused: {}, closed_by_lobby: 0, in: 0, out: 0, bytes_in: 0 }, matches: { rooms: 0, quick: 0, sent: 0, off: 0, woken: 0 }, watchers: { ws: 0, stream: 0, poll: 0, frames: 0, told: {} } };
function note(route, status, ms) {
	const r = (stats.routes[route] = stats.routes[route] || { n: 0, status: {}, ms: [] });
	r.n++;
	r.status[status] = (r.status[status] || 0) + 1;
	if (r.ms.length < 20000) r.ms.push(ms);
}
const err = (what, e) => { const k = what + ': ' + String((e && (e.cause && e.cause.code)) || (e && e.message) || e).slice(0, 80); stats.errors[k] = (stats.errors[k] || 0) + 1; };
async function call(method, path, body, binary) {
	const route = path.split('?')[0], at = Date.now();
	try {
		const r = await fetch(LOBBY + path, { signal: AbortSignal.timeout(30000), method, body: body === undefined ? undefined : binary ? body : JSON.stringify(body), headers: binary ? { 'content-type': 'application/octet-stream' } : { 'content-type': 'application/json' } });
		const buf = new Uint8Array(await r.arrayBuffer());
		note(route, r.status, Date.now() - at);
		let json = null;
		if ((r.headers.get('content-type') || '').includes('json')) try { json = JSON.parse(new TextDecoder().decode(buf)); } catch (e) {}
		return { status: r.status, json, buf };
	} catch (e) {
		note(route, 'failed', Date.now() - at);
		err(route, e);
		return { status: 0, json: null, buf: null };
	}
}
// (a WebSocket: resolves open or refused; its messages to on_msg)
function socket(path, on_msg) {
	return new Promise((done) => {
		const route = path.split('?')[0], at = Date.now();
		let ws;
		try { ws = new WebSocket(WS_URL + path); } catch (e) { err(route, e); done(null); return; }
		ws.binaryType = 'arraybuffer';
		let opened = false, settled = false;
		const give_up = setTimeout(() => { if (!opened) { settled = true; note(route + ' (ws)', 'timeout', Date.now() - at); err(route + ' (ws)', 'no answer in 10 s'); try { ws.close(); } catch (e) {} done(null); } }, 10000);
		ws.onopen = () => { clearTimeout(give_up); if (settled) return; opened = true; stats.ws.opened++; note(route + ' (ws)', 101, Date.now() - at); done(ws); };
		ws.onmessage = (e) => { stats.ws.in++; stats.ws.bytes_in += typeof e.data === 'string' ? e.data.length : e.data.byteLength; if (e.data !== 'pong') on_msg(e.data); };
		const refused = () => { clearTimeout(give_up); if (settled || opened) return; settled = true; note(route + ' (ws)', 'refused', Date.now() - at); stats.ws.refused[route] = (stats.ws.refused[route] || 0) + 1; done(null); };
		ws.onerror = refused;   // (refused: as the game, on to the HTTPS way)
		ws.onclose = () => {
			if (!opened) return refused();
			if (live()) stats.ws.closed_by_lobby++;
		};
		const ping = setInterval(() => { if (ws.readyState === 1) { ws.send('ping'); stats.ws.out++; } else if (ws.readyState > 1) clearInterval(ping); }, 15000);
	});
}
const send = (ws, b) => { if (ws && ws.readyState === 1) { ws.send(b); stats.ws.out++; return true; } return false; };

// ---- a match's bytes, as src/net/watch.cpp makes them ----
function head(kind, match, extra) {
	const b = new Uint8Array(14 + extra), v = new DataView(b.buffer);
	v.setUint32(0, 0x31574656, true); b[4] = kind; b[5] = 1;
	v.setUint32(6, Number(match & 0xffffffffn), true); v.setUint32(10, Number(match >> 32n), true);
	return { b, v };
}
function start_bytes(match, p1, p2) {
	const m = new TextEncoder().encode(JSON.stringify({ game: GAME, p1, p2, c1: 'KR', c2: 'US' })), blob = new Uint8Array(2000).map((_, i) => i * 7);
	const { b, v } = head(1, match, 2 + m.length + 4 + blob.length);
	v.setUint16(14, m.length, true); b.set(m, 16); v.setUint32(16 + m.length, blob.length, true); b.set(blob, 20 + m.length);
	return b;
}
function push_bytes(match, first, n, over) {
	const { b, v } = head(2, match, 8 + n * 4 + 2 + 5);
	v.setUint32(14, first, true); v.setUint32(18, n, true);
	for (let i = 0; i < n * 2; i++) v.setUint16(22 + i * 2, (first * 2 + i) & 0x3ff, true);
	b[22 + n * 4 + 2 + 4] = over ? 1 : 0;
	return b;
}

// ---- a host: its match sent as the game sends it, for as long as the run ----
const matches = [];   // ({wid, has}: the watchers pick from these)
async function feed(wid, wkey, p1, p2, https) {
	const match = BigInt('0x' + hex(15));
	const m = { wid, match, has: 0 };
	matches.push(m);
	let every = 200, sent = 0, ws = null, pending = null;
	const answer = (j) => {
		if (!j) return;
		if (j.wake) { every = Math.min(Math.max(+j.every || 200, 200), 30000); stats.matches.woken++; return; }
		if (j.off) { stats.matches.off++; every = -1; }
		if (Number.isFinite(j.has) && j.has >= 0) m.has = j.has;
		if (Number.isFinite(j.every)) every = Math.min(Math.max(j.every, 200), 30000); else if (j.has !== undefined && every > 0) every = 200;
		if (pending) { pending(); pending = null; }
	};
	if (!https) ws = await socket(`/v1/watch/feed?w=${wid}&k=${wkey}`, (d) => { try { answer(JSON.parse(d)); } catch (e) {} });
	const up = async (b, start) => {
		if (ws && ws.readyState === 1) {
			const got = new Promise((ok) => { pending = ok; });
			send(ws, b);
			await Promise.race([got, sleep(3000)]);
			return;
		}
		const r = await call('POST', `/v1/watch/${start ? 'start' : 'push'}?w=${wid}&k=${wkey}`, b, true);
		answer(r.json);
	};
	await up(start_bytes(match, p1, p2), true);
	const began = Date.now();
	let last = Date.now();
	while (live() && every > 0) {
		await sleep(100);   // (a wake heard at once, not at the end of a 30 s wait)
		if (Date.now() - last < every) continue;
		last = Date.now();
		const frames = Math.floor((Date.now() - began) * 0.0575);   // (57.5 frames a second)
		const first = Math.min(m.has, frames), n = Math.min(frames - first, 4096);
		if (n <= 0) continue;
		await up(push_bytes(match, first, n, !live()), false);
		sent++;
		stats.matches.sent++;
	}
	if (ws) ws.close();
}
const me = (n, extra = {}) => ({ game: GAME, version: 'lobbyload', content: '0123456789abcdef', name: n, cands: ['203.0.113.7:7700'], relay: '', pool: 'lobbyload', accepts: ['stage'], watchable: true, watch: true, qm_listed: true, ...extra });   // (qm_listed: its Quick matches listed, as a game with the setting On)
async function room_pair(i) {
	await sleep(rnd(5000));
	const https = Math.random() < HTTPS, hn = name('H'), gn = name('G');
	const h = await call('POST', '/v1/host', me(hn, { listed: true, persist: true, room: 'LOAD ' + i }));
	if (h.status !== 200 || !h.json) return;
	const { code, key, wid, wkey } = h.json;
	let held = https ? null : await socket(`/v1/room/ws?code=${code}&key=${key}`, () => {});
	const wait = held ? null : call('POST', '/v1/wait', { code, key });
	await sleep(200 + rnd(2000));
	const j = await call('POST', '/v1/join', me(gn, { code, queue: true }));
	if (wait) await wait;
	if (j.status !== 200) return;
	stats.matches.rooms++;
	const hold = held ? null : setInterval(() => { if (live()) call('POST', '/v1/hold', { code, key }); }, 15000);
	const count = setInterval(() => { if (live() && wid) call('GET', `/v1/watch/count?w=${wid}`); }, 60000);   // (the guest's, while its figures show)
	if (wid && wkey) await feed(wid, wkey, hn, gn, https);
	else while (live()) await sleep(1000);
	clearInterval(hold); clearInterval(count);
	if (held) held.close();
	await call('POST', '/v1/close', { code, key });
}
async function quick_pair() {
	await sleep(rnd(5000));
	const a = me(name('QA')), b = me(name('QB'));
	const ra = await call('POST', '/v1/random', a);
	if (!ra.json || !ra.json.ticket) return;
	const wa = call('POST', '/v1/random/wait', { ...a, ticket: ra.json.ticket });
	await sleep(300);
	const rb = await call('POST', '/v1/random', b);
	const x = (await wa).json || {}, y = rb.json || {};
	const host = [x, y].find((r) => r.role === 'host');
	if (!host) { if (rb.json && rb.json.ticket) await call('POST', '/v1/random/leave', { ...b, ticket: rb.json.ticket }); return; }
	stats.matches.quick++;
	if (host.wid && host.wkey) await feed(host.wid, host.wkey, a.name, b.name, Math.random() < HTTPS);
}
// ---- a watcher: a match picked, counted, its frames as they come (or the fallbacks) ----
async function watcher() {
	await sleep(3000 + rnd(Math.max(1, SECONDS * 500)));   // (they come over the first half of the run)
	if (!matches.length || !live()) return;
	const m = matches[rnd(matches.length)], sid = hex(16), mh = m.match.toString(16).padStart(16, '0');
	const s = await call('POST', `/v1/watch/seen?w=${m.wid}&s=${sid}`);
	if (s.status !== 200) { const e = (s.json && s.json.error) || s.status; stats.watchers.told[e] = (stats.watchers.told[e] || 0) + 1; return; }
	await call('GET', `/v1/watch/start?w=${m.wid}&m=${mh}`);
	const way = Math.random() < HTTPS ? (Math.random() < 0.5 ? 'stream' : 'poll') : 'ws';
	let ws = null;
	if (way === 'ws') ws = await socket(`/v1/watch/ws?w=${m.wid}&m=${mh}&from=0&s=${sid}`, (d) => { if (typeof d !== 'string') stats.watchers.frames++; else try { const j = JSON.parse(d); if (j.error) stats.watchers.told[j.error] = (stats.watchers.told[j.error] || 0) + 1; } catch (e) {} });
	if (ws) {
		stats.watchers.ws++;
		while (live() && ws.readyState === 1) await sleep(1000);
		ws.close();
		return;
	}
	if (way === 'stream' || way === 'ws') {   // (the HTTPS stream: one long answer)
		stats.watchers.stream++;
		const at = Date.now();
		try {
			const ctl = new AbortController();
			const stop = setTimeout(() => ctl.abort(), Math.max(0, end - Date.now()));
			const r = await fetch(LOBBY + `/v1/watch/live?w=${m.wid}&m=${mh}&from=0&s=${sid}`, { signal: ctl.signal });
			note('/v1/watch/live', r.status, Date.now() - at);
			if (r.body) for await (const c of r.body) stats.watchers.frames += c.length > 4 ? 1 : 0;
			clearTimeout(stop);
		} catch (e) { if (live()) err('/v1/watch/live', e); }
		return;
	}
	stats.watchers.poll++;   // (asking for pieces, once a second, counted every 15 s)
	let from = 0, seen_at = Date.now();
	while (live()) {
		const r = await call('GET', `/v1/watch/frames?w=${m.wid}&m=${mh}&from=${from}`);
		if (r.status === 200 && r.buf && r.buf.length >= 40) { const v = new DataView(r.buf.buffer); from = v.getUint32(32, true) + v.getUint32(36, true); stats.watchers.frames++; }
		if (Date.now() - seen_at > 15000) { seen_at = Date.now(); await call('POST', `/v1/watch/seen?w=${m.wid}&s=${sid}`); }
		await sleep(1000);
	}
}
async function browser() {
	await sleep(rnd(15000));
	while (live()) { await call('GET', '/v1/online'); await sleep(15000); }
}

console.log(`lobbyload: ${ROOMS} rooms (a host and a guest each), ${QUICK} Quick match pairs, ${WATCHERS} watchers, ${BROWSERS} browsers, ${SECONDS} s, ${Math.round(HTTPS * 100)}% on the HTTPS fallbacks, against ${LOBBY}`);
const all = [];
for (let i = 0; i < ROOMS; i++) all.push(room_pair(i));
for (let i = 0; i < QUICK; i++) all.push(quick_pair());
for (let i = 0; i < WATCHERS; i++) all.push(watcher());
for (let i = 0; i < BROWSERS; i++) all.push(browser());
const tick = setInterval(() => process.stdout.write(`  ${Math.round((Date.now() - t0) / 1000)} s: ${Object.values(stats.routes).reduce((a, r) => a + r.n, 0)} requests, ${stats.ws.opened} sockets, ${stats.ws.in} messages in, ${Object.values(stats.errors).reduce((a, n) => a + n, 0)} errors\n`), 10000);
// (every player done, or 40 s past the end at most: one still waiting then is counted as hung, not waited for)
const hung = await Promise.race([Promise.allSettled(all).then(() => 0), new Promise((ok) => setTimeout(() => ok(1), end - Date.now() + 40000))]);
if (hung) stats.errors['players still waiting 40 s after the end'] = 1;
clearInterval(tick);

// ---- what came back ----
const pct = (a, p) => { if (!a.length) return 0; const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
const routes = Object.fromEntries(Object.entries(stats.routes).sort().map(([k, r]) => [k, { n: r.n, status: r.status, p50_ms: pct(r.ms, 0.5), p95_ms: pct(r.ms, 0.95) }]));
const out = { lobby: LOBBY, seconds: SECONDS, routes, sockets: stats.ws, matches: stats.matches, watchers: stats.watchers, errors: stats.errors };
if (AS_JSON) console.log(JSON.stringify(out, null, 1));
else {
	console.log('route                          requests  statuses                                   p50 ms  p95 ms');
	for (const [k, r] of Object.entries(routes)) console.log(k.padEnd(30) + String(r.n).padStart(9) + '  ' + JSON.stringify(r.status).padEnd(42) + String(r.p50_ms).padStart(7) + String(r.p95_ms).padStart(8));
	console.log(`sockets: ${stats.ws.opened} opened, refused ${JSON.stringify(stats.ws.refused)}, ${stats.ws.closed_by_lobby} closed by the lobby mid-run; ${stats.ws.in} messages in (${stats.ws.bytes_in} bytes), ${stats.ws.out} out`);
	console.log(`matches: ${stats.matches.rooms} rooms and ${stats.matches.quick} Quick matches played; ${stats.matches.sent} uploads; told off ${stats.matches.off}; woken by a watcher ${stats.matches.woken}`);
	console.log(`watchers: ${stats.watchers.ws} on WebSockets, ${stats.watchers.stream} on the stream, ${stats.watchers.poll} asking for pieces; ${stats.watchers.frames} replies of frames; told ${JSON.stringify(stats.watchers.told)}`);
	const n_err = Object.values(stats.errors).reduce((a, n) => a + n, 0);
	console.log(n_err ? `errors (${n_err}): ${JSON.stringify(stats.errors)}` : 'errors: none');
}
const server_errors = Object.values(stats.routes).reduce((a, r) => a + Object.entries(r.status).filter(([s]) => +s >= 500 && +s !== 503).reduce((x, [, n]) => x + n, 0), 0);
process.exit(server_errors || Object.keys(stats.errors).length ? 1 : 0);   // (any open socket or request let go)
