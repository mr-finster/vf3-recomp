// Watching's snapshots (a late watcher drops straight in): the host's parts taken with its key, put together and
// checked, the latest kept and handed out (kept at the edge by its frame), asked of the host when a watcher wants one
// and none is fresh, never asked of a host whose game makes none (an older build's), and nothing changed for one that
// doesn't know them. In Node with stand-ins for Cloudflare (test/standin.mjs):  cd tools/lobby && npm test
import { lobby, settle } from './standin.mjs';
const real_now = Date.now;
let skew = 0;
Date.now = () => real_now() + skew;   // (time moved on by later(): a snapshot nobody asks for, a stalled upload)
const later = (ms) => { skew += ms; };
const { worker, env } = await lobby(process.argv[2]);
let fails = 0;
function check(what, got, want) {
	const ok = JSON.stringify(got) === JSON.stringify(want);
	if (!ok) fails++;
	console.log((ok ? 'ok   ' : 'FAIL ') + what + ': ' + JSON.stringify(got) + (ok ? '' : ' (wanted ' + JSON.stringify(want) + ')'));
}
async function call(method, path, body, from = '198.51.100.1', country = 'KR') {
	const init = { method, headers: { 'cf-connecting-ip': from, 'x-test-country': country } };
	if (body !== undefined) init.body = body instanceof Uint8Array ? body : JSON.stringify(body);
	const req = new Request('http://lobby' + path, init);
	Object.defineProperty(req, 'cf', { value: { latitude: '37.5', longitude: '127.0', continent: 'AS' } });
	const r = await worker.fetch(req, env);
	const type = r.headers.get('content-type') || '';
	return { status: r.status, cache: r.headers.get('cache-control'), body: type.includes('json') ? await r.json() : new Uint8Array(await r.arrayBuffer()) };
}
const C = '0123456789abcdef', V = 'v0.1.7-online-preview+host1';
const me = (name, extra = {}) => ({ game: 'vf3', version: V, content: C, name, cands: ['203.0.113.7:7700'], relay: '', pool: V, accepts: ['stage'], ...extra });
const MAGIC = 0x31574656;
function head(kind, match, extra) {
	const b = new Uint8Array(14 + extra), v = new DataView(b.buffer);
	v.setUint32(0, MAGIC, true); b[4] = kind; b[5] = 1;
	v.setUint32(6, Number(match & 0xffffffffn), true); v.setUint32(10, Number(match >> 32n), true);
	return { b, v };
}
function start_bytes(match, meta, blob) {
	const m = new TextEncoder().encode(JSON.stringify(meta));
	const { b, v } = head(1, match, 2 + m.length + 4 + blob.length);
	v.setUint16(14, m.length, true); b.set(m, 16); v.setUint32(16 + m.length, blob.length, true); b.set(blob, 20 + m.length);
	return b;
}
function push_bytes(match, first, n) {
	const { b, v } = head(2, match, 8 + n * 4 + 2 + 5);
	v.setUint32(14, first, true); v.setUint32(18, n, true);
	for (let i = 0; i < n * 2; i++) v.setUint16(22 + i * 2, (first * 2 + i) & 0xffff, true);
	return b;
}
// (as watch.cpp's encode_snap: the head, then "packed" bytes, here made up: the lobby never unpacks them)
function snap_bytes(match, frame, packed) {
	const { b, v } = head(5, match, 28 + packed);
	v.setUint32(14, frame, true); v.setUint32(18, 77, true); v.setBigUint64(22, 0x1122334455667788n, true);
	v.setUint32(30, 28000000, true); v.setUint32(34, 0, true); v.setUint32(38, packed, true);
	for (let i = 0; i < packed; i++) b[42 + i] = (i * 7) & 255;
	return b;
}
const PART = 32 << 10;
async function send_snap(W, K, MH, bytes, frame, key = K) {
	const n = Math.ceil(bytes.length / PART);
	let r;
	for (let i = 0; i < n; i++) r = await call('POST', `/v1/watch/snap?w=${W}&k=${key}&m=${MH}&f=${frame}&i=${i}&n=${n}`, bytes.slice(i * PART, (i + 1) * PART), '198.51.100.3');
	return r;
}

async function room(name, ip) {
	const h = await call('POST', '/v1/host', me(name, { listed: true }), ip, 'FR');
	await call('POST', '/v1/join', me('GUEST' + name.slice(0, 3), { code: h.body.code }), '198.51.100.99', 'US');
	return h.body;
}

// ---- switched off (as by default): no snapshot asked for or handed out ----
const R0 = await room('OFFHOST', '198.51.100.6');
const M0 = 0x0234abcd5678ef01n, MH0 = '0234abcd5678ef01';
await call('POST', `/v1/watch/start?w=${R0.wid}&k=${R0.wkey}`, start_bytes(M0, { p1: 'OFFHOST', p2: 'GUESTOFF', c1: 'FR', c2: 'US', snap: 1 }, new Uint8Array(100)), '198.51.100.6');
for (let f = 0; f < 8000; f += 4000) await call('POST', `/v1/watch/push?w=${R0.wid}&k=${R0.wkey}`, push_bytes(M0, f, 4000), '198.51.100.6');
let r = await call('GET', `/v1/watch/snap?w=${R0.wid}&m=${MH0}`, undefined, '198.51.100.25');
check('SWITCH_SNAP not on: a late watcher told there\'s none', r.body, { frame: -1 });
r = await call('POST', `/v1/watch/push?w=${R0.wid}&k=${R0.wkey}`, push_bytes(M0, 8000, 10), '198.51.100.6');
check('... and its host never asked for one', 'snap' in r.body, false);
r = await call('POST', `/v1/watch/snap?w=${R0.wid}&k=${R0.wkey}&m=${MH0}&f=7000&i=0&n=1`, snap_bytes(M0, 7000, 100), '198.51.100.6');
check('... a part sent all the same: refused, off', [r.status, r.body.error], [503, 'off']);
env.SWITCH_SNAP = 'on';

// ---- a new build's host (its start says "snap": 1) ----
const R = await room('SNAPHOST', '198.51.100.3');
const W = R.wid, K = R.wkey, M = 0x1234abcd5678ef01n, MH = '1234abcd5678ef01';
r = await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, { p1: 'SNAPHOST', p2: 'GUESTSNA', c1: 'FR', c2: 'US', snap: 1 }, new Uint8Array(100)), '198.51.100.3');
check('the start taken', r.body, { ok: true });
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 0, 1000), '198.51.100.3');
check('a push: nobody wants a snapshot, none asked for', [r.body.has, 'snap' in r.body], [1000, false]);
r = await call('GET', `/v1/watch/snap?w=${W}&m=${MH}`, undefined, '198.51.100.20');
check('a watcher come early (1,000 frames in): none needed, none asked of the host', r.body, { frame: -1, n: 1000 });
for (let f = 1000; f < 9000; f += 4000) await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, f, 4000), '198.51.100.3');
r = await call('GET', `/v1/watch/snap?w=${W}&m=${MH}`, undefined, '198.51.100.20');
check('a watcher come late (9,000 frames): none yet, one asked for', r.body, { frame: -1, pending: true });
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 9000, 10), '198.51.100.3');
check('the host\'s next push: a snapshot wanted', r.body.snap, true);
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 9010, 10), '198.51.100.3');
check('and not asked again at once', 'snap' in r.body, false);
const S = snap_bytes(M, 8990, 300000);   // (ten parts)
r = await send_snap(W, K, MH, S, 8990, 'wrongkey');
check('parts with the wrong key: refused', r.status, 404);
r = await call('POST', `/v1/watch/snap?w=${W}&k=${K}&m=${MH}&f=8990&i=0&n=3`, S.slice(0, PART - 1), '198.51.100.3');
check('a part short of SNAP_PART (not the last): refused', r.status, 400);
r = await call('POST', `/v1/watch/snap?w=${W}&k=${K}&m=${MH}&f=8990&i=0&n=3`, new Uint8Array(PART + 1), '198.51.100.3');
check('a part over SNAP_PART: refused', r.status, 400);
r = await send_snap(W, K, MH, S, 8990);
check('the ten parts: taken, whole', r.body, { ok: true, got: 10 });
r = await call('GET', `/v1/watch/snap?w=${W}&m=${MH}`, undefined, '198.51.100.21');
check('a watcher: the snapshot there, fresh', r.body, { frame: 8990, size: S.length });
r = await call('GET', `/v1/watch/snap?w=${W}&m=${MH}&f=8990`, undefined, '198.51.100.21');
check('fetched: the bytes as sent, kept a day at the edge', [r.status, r.body.length, r.body.every((x, i) => x === S[i]), r.cache], [200, S.length, true, 'public, max-age=86400']);
r = await call('GET', `/v1/watch/snap?w=${W}&m=${MH}&f=8991`, undefined, '198.51.100.21');
check('another frame: none', [r.status, r.body.error], [404, 'no_snap']);
r = await call('GET', `/v1/watch/snap?w=${W}&m=0000000000000001`, undefined, '198.51.100.21');
check('another match: none', [r.status, r.body.error], [404, 'no_match']);
// (a snapshot whose head isn't its match's or its frame's: not kept)
const bad = snap_bytes(0x99n, 9000, 1000);
r = await send_snap(W, K, MH, bad, 9000);
check('a snapshot of another match: refused whole', r.status, 400);
const bad2 = snap_bytes(M, 9001, 1000);
r = await send_snap(W, K, MH, bad2, 9000);
check('a snapshot whose frame isn\'t the one said: refused', r.status, 400);
const cut = snap_bytes(M, 9000, 1000).slice(0, 900);
r = await send_snap(W, K, MH, cut, 9000);
check('a snapshot cut short: refused', r.status, 400);
r = await call('GET', `/v1/watch/snap?w=${W}&m=${MH}`, undefined, '198.51.100.21');
check('the good one still the latest', r.body.frame, 8990);
// (the match goes on: past SNAP_FRESH frames the one there is too old; a watcher asks, the host's asked again)
for (let f = 9020; f < 11100; f += 2080) await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, f, 2080), '198.51.100.3');
r = await call('GET', `/v1/watch/snap?w=${W}&m=${MH}`, undefined, '198.51.100.22');
check('2,000 frames on: too old, a new one asked for, the old one said', r.body, { frame: 8990, size: S.length, pending: true });
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 11100, 10), '198.51.100.3');
check('... not within 15 s of the last asking', 'snap' in r.body, false);

// ---- an older build's host: no "snap" in its start ----
const R2 = await room('OLDHOST', '198.51.100.4');
const M2 = 0x2234abcd5678ef01n, MH2 = '2234abcd5678ef01';
await call('POST', `/v1/watch/start?w=${R2.wid}&k=${R2.wkey}`, start_bytes(M2, { p1: 'OLDHOST', p2: 'GUESTOLD', c1: 'FR', c2: 'US' }, new Uint8Array(100)), '198.51.100.4');
for (let f = 0; f < 8000; f += 4000) await call('POST', `/v1/watch/push?w=${R2.wid}&k=${R2.wkey}`, push_bytes(M2, f, 4000), '198.51.100.4');
r = await call('GET', `/v1/watch/snap?w=${R2.wid}&m=${MH2}`, undefined, '198.51.100.23');
check('an older build\'s match: none, none to come', r.body, { frame: -1 });
r = await call('POST', `/v1/watch/push?w=${R2.wid}&k=${R2.wkey}`, push_bytes(M2, 8000, 10), '198.51.100.4');
check('its host never asked for one: its answer as ever', [r.body.has, 'snap' in r.body], [8010, false]);
// (the frames and the start as ever for a watcher who knows nothing of snapshots)
r = await call('GET', `/v1/watch/frames?w=${R2.wid}&m=${MH2}&from=0`, undefined, '198.51.100.23');
check('an older watcher: the frames as ever', [r.status, r.body.length > 2048 * 4], [200, true]);

// ---- the host on its WebSocket: asked at once ----
const R3 = await room('WSHOST', '198.51.100.5');
const M3 = 0x3234abcd5678ef01n, MH3 = '3234abcd5678ef01';
const req = new Request(`http://lobby/v1/watch/feed?w=${R3.wid}&k=${R3.wkey}`, { headers: { upgrade: 'websocket', 'cf-connecting-ip': '198.51.100.5' } });
Object.defineProperty(req, 'cf', { value: { continent: 'EU' } });
const up = await worker.fetch(req, env);
const ws = up.webSocket, heard = [];
if (ws) { ws.accept(); ws.addEventListener('message', (e) => heard.push(typeof e.data === 'string' ? JSON.parse(e.data) : e.data)); }
check('the host\'s socket taken', up.status, 101);
ws.send(start_bytes(M3, { p1: 'WSHOST', p2: 'GUESTWS', c1: 'FR', c2: 'US', snap: 1 }, new Uint8Array(10)));
await settle();
for (let f = 0; f < 8000; f += 4000) { ws.send(push_bytes(M3, f, 4000)); await settle(); }
heard.length = 0;
r = await call('GET', `/v1/watch/snap?w=${R3.wid}&m=${MH3}`, undefined, '198.51.100.24');
await settle();
check('a late watcher: the host told on its socket at once', heard.filter((m) => m.snap === true).length, 1);
check('... the watcher told one is coming', r.body, { frame: -1, pending: true });

// ---- memory: one snapshot at most in a Watch, put together in place, let go once nobody asks ----
const R4 = await room('MEMHOST', '198.51.100.7');
const W4 = R4.wid, K4 = R4.wkey, M4 = 0x4234abcd5678ef01n, MH4 = '4234abcd5678ef01', H4 = '198.51.100.7';
let n4 = 0;
const push4 = async (frames) => { const x = await call('POST', `/v1/watch/push?w=${W4}&k=${K4}`, push_bytes(M4, n4, frames), H4); n4 += frames; return x.body; };
const ask4 = async () => (await call('GET', `/v1/watch/snap?w=${W4}&m=${MH4}`, undefined, '198.51.100.30')).body;
const part4 = async (bytes, frame, i, n) => (await call('POST', `/v1/watch/snap?w=${W4}&k=${K4}&m=${MH4}&f=${frame}&i=${i}&n=${n}`, bytes.slice(i * PART, (i + 1) * PART), H4)).body;
await call('POST', `/v1/watch/start?w=${W4}&k=${K4}`, start_bytes(M4, { p1: 'MEMHOST', p2: 'GUESTMEM', c1: 'FR', c2: 'US', snap: 1 }, new Uint8Array(100)), H4);
for (let k = 0; k < 3; k++) await push4(3000);
check('a late watcher: one asked for', await ask4(), { frame: -1, pending: true });
const S4 = snap_bytes(M4, 8990, 300000);
r = await send_snap(W4, K4, MH4, S4, 8990);
check('a snapshot sent (ten parts): taken', r.body, { ok: true, got: 10 });
const wobj = env.WATCHES.objs.get('w|' + W4);
check('put together in one buffer (its parts not kept apart), nothing part-sent held', [wobj.m.snap.b.buffer.byteLength, wobj.m.snap.b.length, wobj.m.snap_in], [10 * PART, S4.length, null]);
r = await call('GET', `/v1/watch/snap?w=${W4}&m=${MH4}&f=8990`, undefined, '198.51.100.30');
check('fetched whole from that buffer', [r.status, r.body.length, r.body.every((x, i) => x === S4[i])], [200, S4.length, true]);
later(50000); await push4(10);
check('50 s on, asked for again: still there', (await ask4()).frame, 8990);
later(50000); await push4(10);
check('50 s after that (asked 50 s ago): still kept', (await ask4()).frame, 8990);
later(61000); await push4(10);
check('nobody asking for 61 s: let go (the edge keeps its copy), none said, a new one asked for', [wobj.m.snap, await ask4()], [null, { frame: -1, pending: true }]);
// (a part-sent snapshot whose host stopped: let go after 30 s with no part)
check('a new one\'s first two parts', [(await part4(snap_bytes(M4, 9000, 300000), 9000, 0, 10)).got, (await part4(snap_bytes(M4, 9000, 300000), 9000, 1, 10)).got], [1, 2]);
later(31000); await push4(10);
check('no part for 31 s: the part-sent one let go', wobj.m.snap_in, null);
check('... a later part starts it anew', (await part4(snap_bytes(M4, 9000, 300000), 9000, 2, 10)).got, 1);
const S5 = snap_bytes(M4, 9100, 300000);
r = await send_snap(W4, K4, MH4, S5, 9100);
check('a whole one, newer: taken', [r.body, (await ask4()).frame], [{ ok: true, got: 10 }, 9100]);
r = await call('POST', `/v1/watch/snap?w=${W4}&k=${K4}&m=${MH4}&f=9110&i=0&n=10`, snap_bytes(M4, 9110, 300000).slice(0, PART), H4);
check('another sent while that one\'s fresh (none asked for): refused, the fresh one kept', [r.status, (await ask4()).frame], [400, 9100]);
await push4(2100);
check('2,100 frames on: stale, said, a new one asked for', await ask4(), { frame: 9100, size: S5.length, pending: true });
const S6 = snap_bytes(M4, 11200, 300000), bad6 = snap_bytes(0x77n, 11200, 300000);   // (the second's head another match's)
await part4(bad6, 11200, 0, 10);
check('a new one\'s first part: the stale one let go first (a Watch holds one at most)', [wobj.m.snap, await ask4()], [null, { frame: -1, pending: true }]);
for (let i = 1; i < 10; i++) r = await call('POST', `/v1/watch/snap?w=${W4}&k=${K4}&m=${MH4}&f=11200&i=${i}&n=10`, bad6.slice(i * PART, (i + 1) * PART), H4);
check('its head another match\'s: refused whole, nothing kept', [r.status, wobj.m.snap, wobj.m.snap_in], [400, null, null]);
r = await send_snap(W4, K4, MH4, S6, 11200);
check('sent again, right: taken', [r.body, (await ask4()).frame], [{ ok: true, got: 10 }, 11200]);

// ---- switched off: none handed out, not even the edge's copy ----
const kept = new Map();
let hits = 0;
globalThis.caches = { default: { match: async (k) => { const x = kept.get(k.url); if (x) hits++; return x && x.clone(); }, put: async (k, v) => { kept.set(k.url, v); } } };
delete env.TURN_STATIC;   // (the edge's cache on, as on Cloudflare)
r = await call('GET', `/v1/watch/snap?w=${W4}&m=${MH4}&f=11200`, undefined, '198.51.100.31');
check('on: fetched, and kept at the edge', [r.status, r.body.length, kept.size], [200, S6.length, 1]);
r = await call('GET', `/v1/watch/snap?w=${W4}&m=${MH4}&f=11200`, undefined, '198.51.100.32');
check('... the next watcher\'s from the edge', [r.status, r.body.length, hits], [200, S6.length, 1]);
env.SWITCH_SNAP = 'off';
r = await call('GET', `/v1/watch/snap?w=${W4}&m=${MH4}&f=11200`, undefined, '198.51.100.33');
check('switched off: the edge\'s copy not handed out either', [r.status, r.body.error, hits], [404, 'no_snap', 1]);
check('... and none said to a watcher waiting on one', await ask4(), { frame: -1 });
env.SWITCH_SNAP = 'on';
env.TURN_STATIC = '[]';
delete globalThis.caches;

await settle();
console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
