// What a code review found, each as it was found and as it should be (each check here failed before its fix):
// a Quick match's room taken by its watch code, a room lost at a restart, a match told over at a restart, and the rest.
//   cd tools/lobby && npm test   (node test/review.test.mjs src/index.js)
import { lobby, settle, queued } from './standin.mjs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
const mod = await import(pathToFileURL(resolve(process.argv[2])).href);
const { worker, env } = await lobby(process.argv[2]);
const real_now = Date.now;
let skew = 0;
Date.now = () => real_now() + skew;   // (time moved on by advance(): a room's 30 s, a stream's 10 s, a Watch's 3 hours)
const advance = (ms) => { skew += ms; };
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
	return { status: r.status, cache: r.headers.get('cache-control'), raw: r, body: type.includes('json') ? await r.json() : null };
}
const upgrade = (path, from = '198.51.100.1') => worker.fetch(new Request('http://lobby' + path, { headers: { upgrade: 'websocket', 'cf-connecting-ip': from } }), env);
const online = async () => (await call('GET', '/v1/online')).body;
const C = '0123456789abcdef', V = 'v0.1.7-online-preview+host1';
const me = (name, extra = {}) => ({ game: 'vf3', version: V, content: C, name, cands: ['203.0.113.7:7700'], relay: '', pool: V, accepts: ['stage'], watchable: true, ...extra });
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
function push_bytes(match, first, pairs, info) {
	const { b, v } = head(2, match, 8 + pairs.length * 2 + 2 + 5);
	v.setUint32(14, first, true); v.setUint32(18, pairs.length / 2, true);
	let at = 22;
	for (const x of pairs) { v.setUint16(at, x, true); at += 2; }
	v.setUint16(at, 0, true); at += 2;
	b.set(info, at);
	return b;
}
const frames = (from, n) => { const o = []; for (let f = from; f < from + n; f++) o.push(f & 0xffff, f & 0xffff); return o; };
const sid = (i) => (0x1000000000000000n + BigInt(i)).toString(16);
async function quick_pair(a, b) {   // (a Quick match: the first in waits, the second matches; the host's and guest's answers)
	const ra = await call('POST', '/v1/random', a, a.from);
	const wait = call('POST', '/v1/random/wait', { ...a, ticket: ra.body.ticket }, a.from);
	await queued(env, ra.body.ticket);
	const rb = await call('POST', '/v1/random', b, b.from);
	const wa = (await wait).body;
	return wa.role === 'host' ? { host: wa, guest: rb.body, hf: a.from, gf: b.from } : { host: rb.body, guest: wa, hf: b.from, gf: a.from };
}

// ---- the players' counts: a room that stays, playing, unlisted ----
{
	const d0 = (await online()).games.vf3;
	const h = await call('POST', '/v1/host', me('UNLISTED', { persist: true }), '198.51.100.10');
	await call('POST', '/v1/join', me('UNGUEST', { code: h.body.code }), '198.51.100.11');
	await settle();
	for (let i = 0; i < 4; i++) { advance(20000); await call('POST', '/v1/hold', { code: h.body.code, key: h.body.key }, '198.51.100.10'); }   // (its host, playing: still there, a minute on)
	await settle();
	const d1 = (await online()).games.vf3;
	check('an unlisted room that stays, its match on: its host counted in the match, not hosting', [d1.hosting - d0.hosting, d1.room - d0.room], [0, 2]);
	await call('POST', '/v1/close', { code: h.body.code, key: h.body.key }, '198.51.100.10');
}

// ---- /v1/hold tells the online count once a minute, not at every hold after the first minute ----
{
	let tells = 0;
	const get = env.STATS.get.bind(env.STATS);
	env.STATS.get = (id) => { const o = get(id); return { fetch: (u, i) => { if (String(u.url || u).includes('/tell')) tells++; return o.fetch(u, i); } }; };
	const h = await call('POST', '/v1/host', me('HOLDER', { persist: true }), '198.51.100.12');
	await call('POST', '/v1/join', me('HGUEST', { code: h.body.code }), '198.51.100.13');
	await settle();
	tells = 0;
	for (let i = 0; i < 20; i++) { advance(15000); await call('POST', '/v1/hold', { code: h.body.code, key: h.body.key }, '198.51.100.12'); await settle(); }
	env.STATS.get = get;
	check('5 minutes of holds every 15 s: the online count told about once a minute', tells <= 6, true);
	await call('POST', '/v1/close', { code: h.body.code, key: h.body.key }, '198.51.100.12');
}

// ---- a Quick match's watch code (in the rooms list for anyone) isn't its room's code: its Play again can't be taken ----
{
	const m = await quick_pair({ ...me('ALICE', { qm_listed: true }), from: '198.51.100.20' }, { ...me('BOBBY', { qm_listed: true }), from: '198.51.100.21' });
	check('a Quick match: a watch code of its own, not the room\'s code', [/^[A-HJKMNP-Z2-9]{6}$/.test(m.host.wid || ''), m.host.wid !== m.host.code, m.guest.wid === m.host.wid], [true, true, true]);
	await call('POST', `/v1/watch/start?w=${m.host.wid}&k=${m.host.wkey}`, start_bytes(0x99n, { p1: 'ALICE', p2: 'BOBBY' }, new Uint8Array(4)), m.hf);
	await call('POST', `/v1/watch/push?w=${m.host.wid}&k=${m.host.wkey}`, push_bytes(0x99n, 0, [1, 2], [0, 0, 0, 0, 0]), m.hf);
	await settle();
	const row = ((await online()).rooms || []).find((r) => r.random && r.p1.name === 'ALICE' || r.random && r.p2.name === 'ALICE');
	check('listed to watch by its watch code; the room\'s code nowhere in the list', [!!row && row.watch === m.host.wid, JSON.stringify((await online()).rooms).includes(m.host.code)], [true, false]);
	check('a watcher by the listed code: the match', (await call('POST', `/v1/watch/seen?w=${row.watch}&s=${sid(1)}`, undefined, '203.0.113.99')).body.match, '0000000000000099');
	check('its players peek their room\'s code: not watchable by it (its watch code is its own)', (await call('POST', '/v1/peek', { code: m.host.code })).body.watch, false);
	// the match over, the host's Play again: the same room code opened again by its key (online.cpp's play_again)
	await call('POST', '/v1/close', { code: m.host.code, key: m.host.key }, m.hf);
	const again = await call('POST', '/v1/host', me('ALICE', { persist: true, want: m.host.code, key: m.host.key }), m.hf);
	check('Play again: the same room code', again.body.code, m.host.code);
	const thief = await call('POST', '/v1/join', me('MALLORY', { code: row.watch, queue: true }), '203.0.113.99');
	const nothing = await call('POST', '/v1/join', me('MALLORY', { code: 'QQQ222', queue: true }), '203.0.113.99');
	check('a stranger joining by the listed watch code: no room (as a code nobody has), no addresses', [thief.status, thief.body, 'cands' in thief.body], [nothing.status, nothing.body, false]);
	const orig = await call('POST', '/v1/join', me('BOBBY', { code: m.host.code, queue: true }), m.gf);
	check('the other player joins its Play again', [orig.status, orig.body.name], [200, 'ALICE']);
}

// ---- a room held by its host's socket survives the Room object's restart (a deploy, a variable changed) ----
{
	const restart = (code) => {   // (the object again from its storage; its sockets gone with it, as at a deploy)
		const old = env.ROOMS.objs.get(code);
		for (const s of old.ctx.getWebSockets()) { s.open = false; s.peer.open = false; }
		env.ROOMS.objs.set(code, new mod.Room(old.ctx, env));
		return env.ROOMS.objs.get(code);
	};
	const h = await call('POST', '/v1/host', me('SOCKET', { persist: true }), '198.51.100.30');
	const ws = await upgrade(`/v1/room/ws?code=${h.body.code}&key=${h.body.key}`, '198.51.100.30');
	ws.webSocket.accept();
	advance(45000);   // (its socket held it 45 s: no wait asked meanwhile)
	restart(h.body.code);
	const again = await upgrade(`/v1/room/ws?code=${h.body.code}&key=${h.body.key}`, '198.51.100.30');
	check('restarted: its host\'s socket back at once (not no_room)', again.status, 101);
	const w = await call('POST', '/v1/wait', { code: h.body.code, key: h.body.key }, '198.51.100.30');
	check('and its HTTPS wait too', [w.status, w.body.guest], [200, null]);
	// one whose host never comes back: let go HOST_GONE_MS after the restart, once (a wake after that one gives no more)
	const h2 = await call('POST', '/v1/host', me('GONEHOST', { persist: true }), '198.51.100.31');
	(await upgrade(`/v1/room/ws?code=${h2.body.code}&key=${h2.body.key}`, '198.51.100.31')).webSocket.accept();
	advance(45000);
	await restart(h2.body.code).alarm();
	check('a restart: the room kept for its host at first', (await call('POST', '/v1/peek', { code: h2.body.code })).status, 200);
	advance(31000);
	await restart(h2.body.code).alarm();
	check('its host never back: let go 30 s on, however many times it wakes', (await call('POST', '/v1/peek', { code: h2.body.code })).status, 404);
}

// ---- a watched match's Watch restarted: still on to its watchers, not "over", nothing kept a day at the edge ----
{
	const h = await call('POST', '/v1/host', me('RESTART'), '198.51.100.40');
	const W = h.body.wid, K = h.body.wkey, M = 0x77n, MH = '0000000000000077';
	await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, { p1: 'A', p2: 'B' }, new Uint8Array(4)), '198.51.100.40');
	await call('POST', `/v1/watch/seen?w=${W}&s=${sid(2)}`, undefined, '203.0.113.41');
	let f = 0;
	for (let t = 0; t < 600; t++) {   // (two minutes of pushes, 5 a second, watched: nothing kept in storage meanwhile)
		await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, f, frames(f, 4), [0, 0, 0, 0, 0]), '198.51.100.40');
		f += 4; advance(200);
		if (t % 50 === 0) await call('POST', `/v1/watch/seen?w=${W}&s=${sid(2)}`, undefined, '203.0.113.41');
	}
	const old = env.WATCHES.objs.get('w|' + W);
	env.WATCHES.objs.set('w|' + W, new mod.Watch(old.ctx, env));
	await settle();
	const r = await call('GET', `/v1/watch/frames?w=${W}&m=${MH}&from=1100`, undefined, '203.0.113.41');
	const b = new Uint8Array(await r.raw.arrayBuffer());
	check('restarted mid-match: a watcher not told it\'s over, its answer not kept a day', [b[14], r.cache], [1, 'public, max-age=1']);
	const ans = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, f, frames(f, 4), [0, 0, 0, 0, 0]), '198.51.100.40');
	check('the host\'s next push: told to send it all again', ans.body.has, -1);
	await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, { p1: 'A', p2: 'B' }, new Uint8Array(4)), '198.51.100.40');
	await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 0, frames(0, 100), [1, 0, 0, 0, 1]), '198.51.100.40');   // (the last: over)
	const end = await call('GET', `/v1/watch/frames?w=${W}&m=${MH}&from=0`, undefined, '203.0.113.41');
	check('a match its host said is over: its end kept a day (it never changes)', end.cache, 'public, max-age=86400');
	// the host gone quiet past 30 s with no last word: over to a watcher, but kept a second only
	const h2 = await call('POST', '/v1/host', me('QUIET'), '198.51.100.42');
	await call('POST', `/v1/watch/start?w=${h2.body.wid}&k=${h2.body.wkey}`, start_bytes(0x78n, { p1: 'A', p2: 'B' }, new Uint8Array(4)), '198.51.100.42');
	await call('POST', `/v1/watch/seen?w=${h2.body.wid}&s=${sid(3)}`, undefined, '203.0.113.43');
	await call('POST', `/v1/watch/push?w=${h2.body.wid}&k=${h2.body.wkey}`, push_bytes(0x78n, 0, frames(0, 10), [0, 0, 0, 0, 0]), '198.51.100.42');
	advance(90000);
	const q = await call('GET', `/v1/watch/frames?w=${h2.body.wid}&m=0000000000000078&from=0`, undefined, '203.0.113.43');
	const qb = new Uint8Array(await q.raw.arrayBuffer());
	check('the host quiet 90 s with no last word: over, kept a second, not a day', [qb[14], q.cache], [2, 'public, max-age=1']);
}

// ---- a listed room that stays, opened again after a player who didn't get going (/v1/reopen): listed waiting ----
{
	const h = await call('POST', '/v1/host', me('LISTHOST', { persist: true, listed: true, room: 'FRIDAY FIGHTS' }), '198.51.100.50');
	await call('POST', '/v1/join', me('NOSHOW', { code: h.body.code, queue: true }), '198.51.100.51');
	await call('POST', '/v1/reopen', { code: h.body.code, key: h.body.key }, '198.51.100.50');
	await settle();
	const row = ((await online()).rooms || []).find((r) => r.code === h.body.code);
	check('reopened: listed, waiting for a player', row && row.state, 'waiting');
	await call('POST', '/v1/close', { code: h.body.code, key: h.body.key }, '198.51.100.50');
}

// ---- its Watch not reached (an object restarting or overloaded): the room opens all the same, without watching ----
{
	const get = env.WATCHES.get;
	env.WATCHES.get = () => ({ fetch: async () => { throw new Error('Durable Object reset because its code was updated.'); } });
	const h = await call('POST', '/v1/host', me('NOWATCH', { persist: true, listed: true }), '198.51.100.60');
	check('a new game hosts: its room, no watch code', [h.status, !!h.body.code, 'wid' in h.body, 'wkey' in h.body], [200, true, false, false]);
	const m = await quick_pair({ ...me('QNOWATCH1'), from: '198.51.100.61' }, { ...me('QNOWATCH2'), from: '198.51.100.62' });
	check('a Quick match too: matched, nothing to watch', [m.host.role, m.guest.role, 'wid' in m.host], ['host', 'guest', false]);
	env.WATCHES.get = get;
	const w = await call('POST', '/v1/host', me('WATCHED', { persist: true }), '198.51.100.63');
	await call('POST', '/v1/join', me('WGUEST', { code: w.body.code }), '198.51.100.64');
	env.WATCHES.get = () => ({ fetch: async () => { throw new Error('reset'); } });
	const again = await call('POST', '/v1/host', me('WATCHED', { persist: true, want: w.body.code, key: w.body.key }), '198.51.100.63');
	check('a room that stays opened again for its next match: opened, no key to send with', [again.status, again.body.code === w.body.code, 'wkey' in again.body], [200, true, false]);
	env.WATCHES.get = get;
}

// ---- one IPv6 /48 (its many /64s) has what one address has: 32 watchers a match, 6 listed rooms ----
{
	const h = await call('POST', '/v1/host', me('CAPPED', { listed: true }), '198.51.100.70');
	await call('POST', `/v1/watch/start?w=${h.body.wid}&k=${h.body.wkey}`, start_bytes(0x66n, { p1: 'A', p2: 'B' }, new Uint8Array(4)), '198.51.100.70');
	let taken = 0;
	for (let i = 0; i < 420; i++) if ((await call('POST', `/v1/watch/seen?w=${h.body.wid}&s=${sid(1000 + i)}`, undefined, `2001:db8:77:${Math.floor(i / 32).toString(16)}::${(i % 32) + 1}`)).status === 200) taken++;
	const real = await call('POST', `/v1/watch/seen?w=${h.body.wid}&s=abcdefabcdefabcd`, undefined, '192.0.2.77');
	check('one /48\'s 13 /64s, 420 made-up watchers: 32 taken; a real watcher still let in', [taken, real.status], [32, 200]);
	const codes = [];
	for (let i = 0; i < 10; i++) {
		codes.push((await call('POST', '/v1/host', me('SIXFOUR' + i, { listed: true, persist: true, room: 'ROOM ' + i }), `2001:db8:78:${i.toString(16)}::1`)).body.code);
		advance(5);
	}
	await settle();
	const listed = ((await online()).rooms || []).map((x) => x.code);
	check('one /48\'s 10 listed rooms (a /64 each): 6 listed', codes.filter((c) => listed.includes(c)).length, 6);
}

// ---- a watcher's WebSocket: anything but a ping closes it; one with no ping for 90 s is closed and not counted ----
{
	const h = await call('POST', '/v1/host', me('WSWATCH'), '198.51.100.80');
	const W = h.body.wid, K = h.body.wkey, M = 0x81n, MH = '0000000000000081';
	await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, { p1: 'A', p2: 'B' }, new Uint8Array(4)), '198.51.100.80');
	await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 0, frames(0, 10), [0, 0, 0, 0, 0]), '198.51.100.80');
	const open = async (s, from) => { const r = await upgrade(`/v1/watch/ws?w=${W}&m=${MH}&from=0&s=${s}`, from); r.webSocket.accept(); return r.webSocket; };
	const chatty = await open(sid(4), '203.0.113.81'), pinger = await open(sid(5), '203.0.113.82'), dead = await open(sid(6), '203.0.113.83');
	await settle();
	pinger.send('ping');
	chatty.send('hello');
	chatty.send(new Uint8Array([1, 2, 3]));
	await settle();
	check('a watcher\'s message that isn\'t a ping: its socket closed; one pinging kept', [chatty.open, pinger.open], [false, true]);
	const wobj = env.WATCHES.objs.get('w|' + W);
	const ds = wobj.ctx.getWebSockets('w').find((x) => (x.deserializeAttachment() || {}).sid === sid(6));
	ds.serializeAttachment({ ...ds.deserializeAttachment(), t: Date.now() - 120000 });   // (its game gone 2 minutes, no close)
	wobj.reaped = 0;
	await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 10, frames(10, 10), [0, 0, 0, 0, 0]), '198.51.100.80');
	await settle();
	check('no ping for 2 minutes: closed at the next push, not counted; the one pinging still there', [dead.open, pinger.open, (await call('GET', `/v1/watch/count?w=${W}`)).body.watchers], [false, true, 1]);
}

// ---- a re-armed Watch (its room opened again: a new key) lets the last key's feed go ----
{
	const h = await call('POST', '/v1/host', me('REARM', { persist: true }), '198.51.100.90');
	const feed = await upgrade(`/v1/watch/feed?w=${h.body.wid}&k=${h.body.wkey}`, '198.51.100.90');
	const answers = [];
	feed.webSocket.accept();
	feed.webSocket.addEventListener('message', (e) => answers.push(e.data));
	await call('POST', '/v1/join', me('RGUEST', { code: h.body.code }), '198.51.100.91');
	const again = await call('POST', '/v1/host', me('REARM', { persist: true, want: h.body.code, key: h.body.key }), '198.51.100.90');
	await settle();
	check('opened again with a new key: the last key\'s feed closed', [again.body.wkey !== h.body.wkey, feed.webSocket.open], [true, false]);
	const wobj = env.WATCHES.objs.get('w|' + h.body.wid);
	const f2 = await upgrade(`/v1/watch/feed?w=${h.body.wid}&k=${again.body.wkey}`, '198.51.100.90');
	f2.webSocket.accept();
	const host = wobj.ctx.getWebSockets('host')[0];
	host.serializeAttachment({ k: h.body.wkey });   // (as one taken with the old key)
	f2.webSocket.send(start_bytes(0x91n, { p1: 'A', p2: 'B' }, new Uint8Array(4)));
	await settle();
	check('a feed of a key the Watch no longer has: its messages not taken', [f2.webSocket.open, !!wobj.m && wobj.m.id === '0000000000000091'], [false, false]);
}

// ---- a stream (/live) whose watcher stops reading is let go; the alarm forgetting the code ends any left ----
{
	const h = await call('POST', '/v1/host', me('STREAMS'), '198.51.100.100');
	const W = h.body.wid, K = h.body.wkey, M = 0x55n, MH = '0000000000000055';
	await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, { p1: 'A', p2: 'B' }, new Uint8Array(4)), '198.51.100.100');
	await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 0, frames(0, 2), [0, 0, 0, 0, 0]), '198.51.100.100');
	const nos = await call('GET', `/v1/watch/live?w=${W}&m=${MH}&from=0`, undefined, '203.0.113.101');
	check('a stream with no watcher\'s number: refused (every watcher counts toward the 400)', nos.status, 400);
	const stuck = await call('GET', `/v1/watch/live?w=${W}&m=${MH}&from=0&s=${sid(7)}`, undefined, '203.0.113.100');   // (never read)
	const wobj = env.WATCHES.objs.get('w|' + W);
	check('a stream taken', [stuck.status, wobj.streams.size], [200, 1]);
	advance(11000);
	await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 2, frames(2, 2), [0, 0, 0, 0, 0]), '198.51.100.100');
	check('not read for 11 s: let go', wobj.streams.size, 0);
	await call('GET', `/v1/watch/live?w=${W}&m=${MH}&from=0&s=${sid(8)}`, undefined, '203.0.113.102');   // (never read)
	advance(4 * 3600e3);
	await wobj.alarm();
	check('the code forgotten by the alarm (3 hours unused): its streams ended too', [wobj.arm, wobj.streams.size], [null, 0]);
}

// ---- the month's ceiling: "0" means no watching; a stream refused past it as a WebSocket is ----
{
	const h = await call('POST', '/v1/host', me('CEILING'), '198.51.100.110');
	const W = h.body.wid, K = h.body.wkey, M = 0x61n, MH = '0000000000000061';
	await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, { p1: 'A', p2: 'B' }, new Uint8Array(4)), '198.51.100.110');
	env.MONTH_MATCH_HOURS = '0';
	await settle();
	await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 0, frames(0, 2), [1, 0, 0, 0, 0]), '198.51.100.110');   // (a new score: the Stats told)
	await settle();
	const wobj = env.WATCHES.objs.get('w|' + W);
	check('MONTH_MATCH_HOURS "0": the month over its ceiling at once', wobj.load.over, true);
	const live = await call('GET', `/v1/watch/live?w=${W}&m=${MH}&from=0&s=${sid(9)}`, undefined, '203.0.113.111');
	check('past it: a new stream busy', [live.status, live.body && live.body.error], [429, 'busy']);
	delete env.MONTH_MATCH_HOURS;
	wobj.load = { matches: 0, watchers: 0 };
}

// ---- two players in a room's line behind one address, asking every 4 s: never charged as new codes, never out of it;
// anyone asking about a code they aren't in line for charged as ever (no oracle: an unknown code and a room alike) ----
{
	const used = new Map(), saved = { JOIN_LIMIT: env.JOIN_LIMIT, CODE48_LIMIT: env.CODE48_LIMIT };
	env.JOIN_LIMIT = { limit: async ({ key }) => { const k = key + '|' + Math.floor(Date.now() / 60000); used.set(k, (used.get(k) || 0) + 1); return { success: used.get(k) <= 20 }; } };   // (20 a minute)
	const h = await call('POST', '/v1/host', me('LINEHOST', { persist: true }), '198.51.100.120');
	await call('POST', '/v1/join', me('PLAYING', { code: h.body.code, queue: true }), '198.51.100.121');
	const NAT = '203.0.113.120', got = [];
	for (let t = 0; t < 45; t++) {   // (3 minutes)
		for (const name of ['FIRST', 'SECOND']) { const r = await call('POST', '/v1/join', me(name, { code: h.body.code, queue: true }), NAT); got.push(r.status === 409 ? r.body.queued : r.status); }
		await call('POST', '/v1/hold', { code: h.body.code, key: h.body.key }, '198.51.100.120');
		advance(4000);
	}
	check('two in line behind one address, 3 minutes: in line throughout, first and second', [got.every((x) => x === 1 || x === 2), got.slice(-2)], [true, [1, 2]]);
	const tries = [];
	for (let i = 0; i < 25; i++) tries.push((await call('POST', '/v1/join', me('GUESS' + i, { code: i % 2 ? h.body.code : 'QQQ' + 'ABCDEFGHJKMNPQRSTUVWXYZ'[i] + '22', queue: true }), '203.0.113.121')).status);
	check('new asks with a queue (a new name each, so never in line yet): 20 a minute, then busy, a room and an unknown code alike', [tries.slice(0, 20).every((x) => x === 404 || x === 409), tries.slice(20).every((x) => x === 429)], [true, true]);
	Object.assign(env, saved);
	await call('POST', '/v1/close', { code: h.body.code, key: h.body.key }, '198.51.100.120');
}

// ---- a room that stays, in use, lives on past ROOM_LIFE_MS (2 hours) from its first match ----
{
	const h = await call('POST', '/v1/host', me('LONGROOM', { persist: true, listed: true, room: 'ALL NIGHT' }), '198.51.100.130');
	const hold = async (minutes) => { for (let i = 0; i < minutes * 3; i++) { advance(20000); await call('POST', '/v1/hold', { code: h.body.code, key: h.body.key }, '198.51.100.130'); } };
	await call('POST', '/v1/join', me('NIGHT1', { code: h.body.code }), '198.51.100.131');
	await hold(90);
	const again = await call('POST', '/v1/host', me('LONGROOM', { persist: true, listed: true, room: 'ALL NIGHT', want: h.body.code, key: h.body.key }), '198.51.100.130');
	await call('POST', '/v1/join', me('NIGHT2', { code: h.body.code }), '198.51.100.132');
	await hold(60);
	const p = await call('POST', '/v1/peek', { code: h.body.code });
	await settle();
	const row = ((await online()).rooms || []).find((r) => r.code === h.body.code);
	check('2½ hours, a match after 1½: still there, playing, listed', [again.body.code === h.body.code, p.status, p.body.state, row && row.state], [true, 200, 'playing', 'playing']);
}

// ---- the online page's function asks from Cloudflare's own addresses: /v1/online not limited per address ----
{
	const saved = env.ANY_LIMIT;
	env.ANY_LIMIT = { limit: async () => ({ success: false }) };   // (its address's 600 a minute used up)
	const o = await call('GET', '/v1/online', undefined, '104.28.0.1'), other = await call('POST', '/v1/host', me('LIMITED'), '104.28.0.1');
	check('an address over its limit: /v1/online still answered (kept at the edge), anything else busy', [o.status, !!o.body.games, other.status], [200, true, 429]);
	env.ANY_LIMIT = saved;
}

// ---- /v1/hold: a room's line as it is, not one who left it (a game moves its guest on for the line, issue #42) ----
{
	const h = await call('POST', '/v1/host', me('LINEHOST', { listed: true, persist: true }), '198.51.100.140');
	await call('POST', '/v1/join', me('PLAYING', { code: h.body.code }), '198.51.100.141');
	const q = await call('POST', '/v1/join', me('INLINE', { code: h.body.code, queue: true }), '198.51.100.142');
	const held = async () => (await call('POST', '/v1/hold', { code: h.body.code, key: h.body.key }, '198.51.100.140')).body.queue;
	const asking = await held();
	advance(16000);   // (no ask from the one in line since: gone from it)
	check('/v1/hold: one in line while they ask, none once they stopped asking', [q.body.queued, asking, await held()], [1, 1, 0]);
}

Date.now = real_now;
console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
