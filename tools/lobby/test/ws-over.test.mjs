// A watcher on a WebSocket sent a hash made after its frame went to it (with the next frames), and told the match is
// over: when the host's last push brings no new frames (the game's feed_last after a lost connection or a player gone),
// and when the host goes quiet with no last push at all (its game closed, its connection gone: the Watch's alarm), even
// with the match gone from the object's memory meanwhile (out of memory while only WebSockets wait):  cd tools/lobby &&
// npm test
import { lobby, settle } from './standin.mjs';
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
const online = async () => (await call('GET', '/v1/online')).body;
const C = '0123456789abcdef', V = 'v0.1.7-online-preview+host1';
const me = (name, extra = {}) => ({ game: 'vf3', version: V, content: C, name, cands: ['203.0.113.7:7700'], relay: '', pool: V, accepts: ['stage'], ...extra });

// ---- the bytes (as src/net/watch.cpp makes them) ----
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
function push_bytes(match, first, pairs, hashes, info) {
	const { b, v } = head(2, match, 8 + pairs.length * 2 + 2 + hashes.length * 12 + 5);
	v.setUint32(14, first, true); v.setUint32(18, pairs.length / 2, true);
	let at = 22;
	for (const x of pairs) { v.setUint16(at, x, true); at += 2; }
	v.setUint16(at, hashes.length, true); at += 2;
	for (const [f, h] of hashes) { v.setUint32(at, f, true); v.setBigUint64(at + 4, h, true); at += 12; }
	b.set(info, at);
	return b;
}
function read_reply(b) {
	const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
	if (v.getUint32(0, true) !== MAGIC || b[4] !== 4) return null;
	let at = 14;
	const r = { state: b[at++], total: v.getUint32(at, true), watchers: v.getUint32(at + 4, true) };
	at += 8;
	r.info = [...b.slice(at, at + 5)]; at += 5;
	r.checkpoint = v.getUint32(at, true); at += 4;
	r.first = v.getUint32(at, true); const n = v.getUint32(at + 4, true); at += 8;
	r.pairs = []; for (let i = 0; i < n * 2; i++, at += 2) r.pairs.push(v.getUint16(at, true));
	const nh = v.getUint16(at, true); at += 2;
	r.hashes = []; for (let i = 0; i < nh; i++, at += 12) r.hashes.push([v.getUint32(at, true), v.getBigUint64(at + 4, true)]);
	return at === b.length ? r : null;
}

const host = await call('POST', '/v1/host', me('WSHOST', { watchable: true }), '198.51.100.130');
const W = host.body.wid, K = host.body.wkey, M = 0x7272n, MH = '0000000000007272';
const upgrade = async (path, from = '198.51.100.131') => worker.fetch(new Request('http://lobby' + path, { headers: { upgrade: 'websocket', 'cf-connecting-ip': from } }), env);
const listen = (ws) => { const got = []; ws.accept(); ws.addEventListener('message', (e) => got.push(e.data)); return got; };
const feed = await upgrade(`/v1/watch/feed?w=${W}&k=${K}`, '198.51.100.130');
const answers = listen(feed.webSocket);
feed.webSocket.send(start_bytes(M, { p1: 'WSHOST', p2: 'WSGUEST' }, new Uint8Array(10)));
const ps = [];
for (let f = 0; f < 300; f++) ps.push(f, f + 1);
feed.webSocket.send(push_bytes(M, 0, ps, [[50, 7n]], [0, 0, 0, 0, 0]));
await settle();
await call('POST', `/v1/watch/seen?w=${W}&s=5555555555555555`);
const wsr = await upgrade(`/v1/watch/ws?w=${W}&m=${MH}&from=0&s=5555555555555555`);
const got = listen(wsr.webSocket);
await settle();
const replies = () => got.filter((x) => typeof x !== 'string').map((x) => read_reply(new Uint8Array(x)));
const r1 = replies();
check('the watcher has every frame, the match on', [r1.length > 0, r1[r1.length - 1].first + r1[r1.length - 1].pairs.length / 2, r1[r1.length - 1].state], [true, 300, 1]);
// a hash made after its frame went to the watcher (as the players' are, a few frames late): in the next reply all the same
const more = [];
for (let f = 300; f < 310; f++) more.push(f, f + 1);
feed.webSocket.send(push_bytes(M, 300, more, [[295, 9n]], [0, 0, 0, 0, 0]));
await settle();
const rh = replies().pop();
check('a hash for a frame the watcher already has: sent with the next frames', [rh.first, rh.pairs.length / 2, rh.hashes.map(([f, h]) => f + ':' + h)], [300, 10, ['295:9']]);
// the host's last push: no new frames, the match over (Feed::over's flag)
feed.webSocket.send(push_bytes(M, 310, [], [], [0, 0, 0, 0, 1]));
await settle();
const r2 = replies();
check('the match over, no new frames: the watcher on its WebSocket told (a reply with state 2)', r2.some((r) => r.state === 2), true);
// and over HTTPS (the game's feed_last sends it so, by POST)
const host2 = await call('POST', '/v1/host', me('WSHOST2', { watchable: true }), '198.51.100.140');
const W2 = host2.body.wid, K2 = host2.body.wkey, M2 = 0x7373n, MH2 = '0000000000007373';
const feed2 = await upgrade(`/v1/watch/feed?w=${W2}&k=${K2}`, '198.51.100.140');
listen(feed2.webSocket);
feed2.webSocket.send(start_bytes(M2, { p1: 'A', p2: 'B' }, new Uint8Array(10)));
feed2.webSocket.send(push_bytes(M2, 0, ps, [], [0, 0, 0, 0, 0]));
await settle();
await call('POST', `/v1/watch/seen?w=${W2}&s=6666666666666666`);
const ws2 = await upgrade(`/v1/watch/ws?w=${W2}&m=${MH2}&from=0&s=6666666666666666`);
const got2 = listen(ws2.webSocket);
await settle();
const p = await call('POST', `/v1/watch/push?w=${W2}&k=${K2}`, push_bytes(M2, 300, [], [], [0, 0, 0, 0, 1]));
await settle();
const r3 = got2.filter((x) => typeof x !== 'string').map((x) => read_reply(new Uint8Array(x)));
check('the last push over HTTPS (feed_last\'s way): the watcher on its WebSocket told', [p.status, r3.some((r) => r.state === 2)], [200, true]);

// the host quiet, no last push (its game closed): the alarm tells the watcher on its WebSocket, once, the score kept
const host3 = await call('POST', '/v1/host', me('WSHOST3', { watchable: true }), '198.51.100.150');
const W3 = host3.body.wid, K3 = host3.body.wkey, M3 = 0x7474n, MH3 = '0000000000007474';
const feed3 = await upgrade(`/v1/watch/feed?w=${W3}&k=${K3}`, '198.51.100.150');
listen(feed3.webSocket);
feed3.webSocket.send(start_bytes(M3, { p1: 'A', p2: 'B' }, new Uint8Array(10)));
feed3.webSocket.send(push_bytes(M3, 0, ps, [], [1, 0, 2, 1, 0]));
await settle();
await call('POST', `/v1/watch/seen?w=${W3}&s=7777777777777777`);
const ws3 = await upgrade(`/v1/watch/ws?w=${W3}&m=${MH3}&from=0&s=7777777777777777`);
const got3 = listen(ws3.webSocket);
await settle();
const watch3 = env.WATCHES.objs.get('w|' + W3);
const r4 = () => got3.filter((x) => typeof x !== 'string').map((x) => read_reply(new Uint8Array(x)));
check('the alarm set for the host\'s quiet (the match on, a watcher on its WebSocket)', watch3.quiet_at > Date.now() + 25000, true);
await watch3.alarm();
await settle();
check('the alarm with the host still sending: nothing said, set again', [r4().some((r) => r.state === 2), watch3.quiet_at > 0], [false, true]);
watch3.m.seen -= 31000;   // (no word from the host for 31 s, the watcher there since long before)
watch3.woke -= 120000;
await watch3.alarm();
await settle();
const told = r4().filter((r) => r.state === 2);
check('the host quiet 31 s: the watcher told it\'s over, once, with its frames and the score', told.length === 1 && [told[0].total, told[0].info, told[0].first + told[0].pairs.length / 2], [300, [1, 0, 2, 1, 0], 300]);
await watch3.alarm();
await settle();
check('told once, not at every alarm', r4().filter((r) => r.state === 2).length, 1);

// the same with the object out of memory (only WebSockets waiting: the match not kept while anyone watches) and woken
// by its alarm: the watcher told from what its socket kept (the match's id, its frames, the score)
const host4 = await call('POST', '/v1/host', me('WSHOST4', { watchable: true }), '198.51.100.160');
const W4 = host4.body.wid, K4 = host4.body.wkey, M4 = 0x123456789abcdef0n, MH4 = '123456789abcdef0';
const feed4 = await upgrade(`/v1/watch/feed?w=${W4}&k=${K4}`, '198.51.100.160');
listen(feed4.webSocket);
feed4.webSocket.send(start_bytes(M4, { p1: 'A', p2: 'B' }, new Uint8Array(10)));
feed4.webSocket.send(push_bytes(M4, 0, ps, [], [0, 1, 1, 2, 0]));
await settle();
await call('POST', `/v1/watch/seen?w=${W4}&s=8888888888888888`);
const ws4 = await upgrade(`/v1/watch/ws?w=${W4}&m=${MH4}&from=0&s=8888888888888888`);
const got4 = listen(ws4.webSocket);
await settle();
const old = env.WATCHES.objs.get('w|' + W4), sock = old.ctx.getWebSockets('w')[0];
await old.ctx.storage.delete('mm');   // (nothing kept: watched from its first push)
const fresh = new old.constructor(old.ctx, env);   // (as Cloudflare makes it again: its storage and sockets, nothing in memory)
env.WATCHES.objs.set('w|' + W4, fresh);
sock.owner.obj = fresh;
await settle();   // (its constructor's read of what's kept: Cloudflare lets nothing in before it's done)
const real_now = Date.now;
Date.now = () => real_now() + 40000;   // (the alarm, 40 s on)
await fresh.alarm();
await settle();
Date.now = real_now;
const told4 = got4.filter((x) => typeof x !== 'string').map((x) => read_reply(new Uint8Array(x))).filter((r) => r && r.state === 2);
check('the object woken without the match: the watcher still told it\'s over (its match, its frames, the score)',
	told4.length === 1 && [told4[0].total, told4[0].first, told4[0].pairs.length, told4[0].info], [300, 300, 0, [0, 1, 1, 2, 0]]);
const head4 = got4.filter((x) => typeof x !== 'string').map((x) => new DataView(x)).pop();
check('... by the match\'s own id', head4 && head4.getUint32(6, true).toString(16) + '/' + head4.getUint32(10, true).toString(16), '9abcdef0/12345678');

// the same, the match kept from before anyone watched (read back stale when it wakes): told from it
const host6 = await call('POST', '/v1/host', me('WSHOST6', { watchable: true }), '198.51.100.180');
const W6 = host6.body.wid, K6 = host6.body.wkey, M6 = 0x7676n, MH6 = '0000000000007676';
await call('POST', `/v1/watch/start?w=${W6}&k=${K6}`, start_bytes(M6, { p1: 'A', p2: 'B' }, new Uint8Array(10)));
await call('POST', `/v1/watch/push?w=${W6}&k=${K6}`, push_bytes(M6, 0, ps.slice(0, 200), [], [1, 1, 0, 0, 0]));   // (kept: nobody watching)
await call('POST', `/v1/watch/seen?w=${W6}&s=6666666666666661`);
const ws6 = await upgrade(`/v1/watch/ws?w=${W6}&m=${MH6}&from=0&s=6666666666666661`);
const got6 = listen(ws6.webSocket);
await call('POST', `/v1/watch/push?w=${W6}&k=${K6}`, push_bytes(M6, 100, ps.slice(200), [], [2, 1, 0, 0, 0]));   // (watched: not kept)
await settle();
const old6 = env.WATCHES.objs.get('w|' + W6), sock6 = old6.ctx.getWebSockets('w')[0];
const fresh6 = new old6.constructor(old6.ctx, env);
env.WATCHES.objs.set('w|' + W6, fresh6);
sock6.owner.obj = fresh6;
await settle();
Date.now = () => real_now() + 40000;
await fresh6.alarm();
await settle();
Date.now = real_now;
const r6 = got6.filter((x) => typeof x !== 'string').map((x) => read_reply(new Uint8Array(x)));
const told6 = r6.filter((r) => r && r.state === 2);
check('woken with an older copy of the match: told it\'s over, its frames not taken back', told6.length === 1 && [Math.max(...r6.map((r) => r.first + r.pairs.length / 2)), told6[0].pairs.length], [300, 0]);

// a watcher come after the host went quiet: told it's over at once, on its socket
const host5 = await call('POST', '/v1/host', me('WSHOST5', { watchable: true }), '198.51.100.170');
const W5 = host5.body.wid, K5 = host5.body.wkey, M5 = 0x7575n, MH5 = '0000000000007575';
await call('POST', `/v1/watch/start?w=${W5}&k=${K5}`, start_bytes(M5, { p1: 'A', p2: 'B' }, new Uint8Array(10)));
await call('POST', `/v1/watch/push?w=${W5}&k=${K5}`, push_bytes(M5, 0, ps, [], [0, 0, 0, 0, 0]));
await call('POST', `/v1/watch/seen?w=${W5}&s=9999999999999999`);
env.WATCHES.objs.get('w|' + W5).m.seen -= 80000;   // (quiet past even the idle mark)
const ws5 = await upgrade(`/v1/watch/ws?w=${W5}&m=${MH5}&from=300&s=9999999999999999`);
const got5 = listen(ws5.webSocket);
await settle();
check('a watcher come once the host was quiet: told over at once', got5.filter((x) => typeof x !== 'string').map((x) => read_reply(new Uint8Array(x))).some((r) => r && r.state === 2), true);
console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
