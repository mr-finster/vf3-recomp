// A listed room's score on the rooms list (the Online page's, the games' own Rooms): VF2's report of 8 Nov, a room that
// stays, its host losing match after match to the player who joined, the line saying 0-0. Its score is its Watch's,
// which only its host's uploads bring: here the guest wins every match, and (1) the host's uploads stop while the room
// is still held (its line said 0-0 once the Stats let the Watch's telling go, 75 s on), (2) the match ends and the
// same two play the room's next (Play again, or back from its line: a new match, its Watch's score from 0; the line
// said 0-0 again), (3) the match's last push came within 5 s of the Watch's last telling (never told), (4) a start
// sent again for the same match (a Watch woken without it) isn't a new match, (5) someone else joins: from 0-0.
//   cd tools/lobby && node test/score.test.mjs src/index.js   (npm test runs it)
import { lobby, settle } from './standin.mjs';
const real_now = Date.now;
let skew = 0;
Date.now = () => real_now() + skew;
const advance = (ms) => { skew += ms; };
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
	Object.defineProperty(req, 'cf', { value: { latitude: '55.7', longitude: '37.6', continent: 'EU' } });
	const r = await worker.fetch(req, env);
	const type = r.headers.get('content-type') || '';
	return { status: r.status, body: type.includes('json') ? await r.json() : new Uint8Array(await r.arrayBuffer()) };
}
const V = 'v0.1.19', C = '0123456789abcdef';
const me = (name, extra = {}) => ({ game: 'vf2', version: V, content: C, name, cands: ['203.0.113.7:7700'], relay: '', pool: V, accepts: [], ...extra });
// (the bytes, as src/net/watch.cpp makes them)
function head(kind, match, extra) {
	const b = new Uint8Array(14 + extra), v = new DataView(b.buffer);
	v.setUint32(0, 0x31574656, true); b[4] = kind; b[5] = 1;
	v.setUint32(6, Number(match & 0xffffffffn), true); v.setUint32(10, Number(match >> 32n), true);
	return { b, v };
}
function start_bytes(match, meta) {
	const m = new TextEncoder().encode(JSON.stringify(meta)), blob = new Uint8Array(16600);
	const { b, v } = head(1, match, 2 + m.length + 4 + blob.length);
	v.setUint16(14, m.length, true); b.set(m, 16); v.setUint32(16 + m.length, blob.length, true); b.set(blob, 20 + m.length);
	return b;
}
function push_bytes(match, first, n, info) {
	const { b, v } = head(2, match, 8 + n * 4 + 2 + 5);
	v.setUint32(14, first, true); v.setUint32(18, n, true);
	b.set(info, 22 + n * 4 + 2);
	return b;
}
// (time on, its host holding the room every 15 s as a game does while its match is on: by its socket, or /v1/hold)
let held = null;
const later = async (ms) => { for (let t = 0; t < ms; t += 15000) { advance(Math.min(15000, ms - t)); if (held) await call('POST', '/v1/hold', held, HOST); } };
const line = async (code) => { await settle(); return ((await call('GET', '/v1/online')).body.rooms || []).find((x) => x.code === code); };
const said = (r) => r && [r.state, r.p1.name, r.p2 && r.p2.name, r.score];

// a listed room that stays (Create room: Listed), its host in Russia; a player from Australia joins it
const HOST = '198.51.100.40', GUEST = '198.51.100.41';
const h = await call('POST', '/v1/host', me('KOSMONAVT', { listed: true, persist: true, room: "kosmonavt's room" }), HOST, 'RU');
const W = h.body.wid, code = h.body.code;
check('the room: listed, its matches sent by its code', [h.status, W === code, !!h.body.wkey], [200, true, true]);
const j = await call('POST', '/v1/join', me('SURF-KICK', { code }), GUEST, 'AU');
held = { code, key: h.body.key };
check('the guest joins', j.status, 200);
const meta = { game: 'vf2', p1: 'KOSMONAVT', p2: 'SURF-KICK', c1: 'RU', c2: 'AU' };
let M = 0x1111222233334444n, n = 0, K = h.body.wkey;
const start = async () => (await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, meta), HOST)).body;
const push = async (info, frames = 1725) => { const r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, n, frames, info), HOST); n += frames; return r.body; };
check('the match\'s start', await start(), { ok: true });
await push([0, 1, 0, 3, 0]);
check('the guest won one: 0-1', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [0, 1]]);
await later(30000); await push([0, 2, 1, 6, 0]);
check('and another: 0-2', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [0, 2]]);

// (1) the host's uploads stop (the lobby not reached from it for a minute: its game sends no more this match), the room
// still held by its host every 15 s: the line kept its last score, not 0-0
await later(120000);
let r = await line(code);
check('no word from the Watch for 2 minutes, the room held: still playing, its score as last known', [said(r), r && r.round], [['playing', 'KOSMONAVT', 'SURF-KICK', [0, 2]], 0]);

// (2) the uploads back (a new match: the session over and the same two again), (3) its last push 2 s after the one
// before, the match over (said by the host's last push)
const re = await call('POST', '/v1/host', me('KOSMONAVT', { listed: true, persist: true, room: "kosmonavt's room", want: code, key: h.body.key }), HOST, 'RU');
check('the room open again in place (the match over)', [re.body.code, re.body.wid], [code, W]);
check('waiting again: no score', said(await line(code)), ['waiting', 'KOSMONAVT', null, undefined]);
const j2 = await call('POST', '/v1/join', me('SURF-KICK', { code }), GUEST, 'AU');
check('the same player joins again', j2.status, 200);
check('the same two again, before its match is sent: the room\'s score so far', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [0, 2]]);
M = 0x5555666677778888n; n = 0; K = re.body.wkey;
check('the next match\'s start', await start(), { ok: true });
check('its start (0-0 in it): the room\'s score, 0-2', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [0, 2]]);
await later(30000); await push([0, 1, 0, 3, 0]);
check('the guest wins its first: 0-3', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [0, 3]]);
// (4) the Watch woke without the match (a deploy, a restart): the host sends its start again, the same match
check('the same match\'s start again', await start(), { ok: true });
check('not a new match: still 0-3', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [0, 3]]);
// (as the real lobby's Watch: told at most every 5 s, not every change as a test's own lobby is)
const turn_static = env.TURN_STATIC;
delete env.TURN_STATIC;
await later(30000); await push([0, 2, 1, 6, 0]);
await settle();
env.TURN_STATIC = turn_static;
check('0-4', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [0, 4]]);
delete env.TURN_STATIC;
await later(2000); await push([1, 3, 3, 9, 1], 100);   // (the host won one, the guest one more, the match over: 2 s after the last)
await settle();
env.TURN_STATIC = turn_static;
const re2 = await call('POST', '/v1/host', me('KOSMONAVT', { listed: true, persist: true, room: "kosmonavt's room", want: code, key: h.body.key }), HOST, 'RU');
await call('POST', '/v1/join', me('SURF-KICK', { code }), GUEST, 'AU');
check('the over, 2 s after the last push, told all the same: 1-5 when the two are back (0-2, then 1-3)', said(await line(code)), ['playing', 'KOSMONAVT', 'SURF-KICK', [1, 5]]);

// (5) someone else's turn: from 0-0
const re3 = await call('POST', '/v1/host', me('KOSMONAVT', { listed: true, persist: true, room: "kosmonavt's room", want: code, key: h.body.key }), HOST, 'RU');
await call('POST', '/v1/join', me('NEONKID', { code }), '198.51.100.42', 'KR');
M = 0x9999aaaabbbbccccn; n = 0; K = re3.body.wkey;
await start();
check('another player: from 0-0', said(await line(code)), ['playing', 'KOSMONAVT', 'NEONKID', [0, 0]]);
await later(30000); await push([1, 0, 3, 1, 0]);
check('the host wins one: 1-0', said(await line(code)), ['playing', 'KOSMONAVT', 'NEONKID', [1, 0]]);

// (6) the Stats' storage: a Watch's telling writes the room's entry only when its tally changed (a Watch tells every
// 5-20 s while its match is watched; its entry holds the line's queue and the tally)
await settle();
const st = env.STATS.objs.get('all'), touch0 = st.touch.bind(st), touched = [];
st.touch = (kind, id) => { if (kind === 'hosts' && id === W) touched.push(id); touch0(kind, id); };
const tl = JSON.parse(JSON.stringify(st.s.hosts[W].tally)), two = tl.pair.split('|').map((name) => ({ name }));
st.tally(W, two, tl.cur, tl.mid);
st.tally(W, two, [0, 0], tl.mid);   // (a start sent again: 0-0 for a moment, not taken)
check('the same tally told again, or less: the room\'s entry not written', [touched.length, st.s.hosts[W].tally], [0, tl]);
st.tally(W, two, [tl.cur[0], tl.cur[1] + 1], tl.mid);
check('a win more: written', touched.length, 1);
st.tally(W, two, [tl.cur[0], tl.cur[1] + 1], 'ffffffffffffffff');
check('a new match: written', touched.length, 2);
st.touch = touch0;

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
