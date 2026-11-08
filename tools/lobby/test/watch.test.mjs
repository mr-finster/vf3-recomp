// The lobby's rooms list and watching (public rooms, a match sent by its host, watchers asking for it), in Node with
// stand-ins for Cloudflare (test/standin.mjs):  cd tools/lobby && npm test
import { lobby, sockets, settle, queued } from './standin.mjs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
const { Watch } = await import(pathToFileURL(resolve(process.argv[2])).href);
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

// ---- a private room, as every game before this: nothing listed, nothing to watch ----
const priv = await call('POST', '/v1/host', me('PRIV'), '198.51.100.2');
check('private room: no watch code', [priv.status, 'wid' in priv.body, 'wkey' in priv.body], [200, false, false]);
let d = await online();
check('private room: counted, not listed', [d.games.vf3.hosting >= 1, (d.rooms || []).filter((r) => r.code === priv.body.code).length], [true, 0]);

// ---- a public room: listed while it waits, by its host's name, with its region ----
const pub = await call('POST', '/v1/host', me('MRFINSTER', { listed: true }), '198.51.100.3', 'FR');
check('public room: a watch code and its key', [pub.status, /^[A-HJKMNP-Z2-9]{6}$/.test(pub.body.wid), /^[0-9a-f]{32}$/.test(pub.body.wkey)], [200, true, true]);
await settle();
d = await online();
const listed = (d.rooms || []).find((r) => r.code === pub.body.code);
check('public room: listed waiting, name, flag, region', listed && [listed.state, listed.p1.name, listed.p1.country, listed.region, listed.game], ['waiting', 'MRFINSTER', 'FR', 'AS', 'vf3']);
const joined = await call('POST', '/v1/join', me('DENVERDAN', { code: pub.body.code }), '198.51.100.4', 'US');
check('join a public room: the watch code in the answer', [joined.status, joined.body.wid], [200, pub.body.wid]);
await settle();
d = await online();
check('joined: no longer listed as waiting', (d.rooms || []).filter((r) => r.code === pub.body.code).length, 0);

// ---- the host sends the match; watchers ask for it ----
const W = pub.body.wid, K = pub.body.wkey, M = 0x1234abcd5678ef01n, MH = '1234abcd5678ef01';
const blob = new Uint8Array(3000).map((_, i) => i & 255);
let r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 0, [1, 2], [], [0, 0, 0, 0, 0]));
check('a push before the start: has -1 (send it all)', r.body.has, -1);
r = await call('POST', `/v1/watch/start?w=${W}&k=bad`, start_bytes(M, { p1: 'MRFINSTER', p2: 'DENVERDAN', c1: 'FR', c2: 'US' }, blob));
check('a start with the wrong key: refused', r.status, 404);
r = await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M, { game: 'vf3', p1: 'MRFINSTER', p2: 'DENVERDAN', c1: 'FR', c2: 'US' }, blob));
check('the start', r.body, { ok: true });
const pairs = [];
for (let f = 0; f < 3000; f++) pairs.push(f & 0x3ff, (f * 7) & 0x3ff);
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 0, pairs.slice(0, 2400), [[0, 11n], [120, 0xfedcba9876543210n], [1080, 33n]], [0, 0, 1, 0, 0]));
check('frames 0-1199 (nobody watching: the host told to send every 30 s)', r.body, { has: 1200, watchers: 0, every: 30000 });
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 1500, pairs.slice(3000, 3200), [], [0, 0, 1, 0, 0]));
check('a gap: not taken, what it has said', r.body.has, 1200);
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 1100, pairs.slice(2200, 6000), [[1200, 44n], [2400, 55n]], [1, 0, 2, 1, 0]));
check('an overlap: the new frames taken', r.body.has, 3000);
const sb = await call('GET', `/v1/watch/start?w=${W}&m=${MH}`);
check('a watcher gets the start, byte for byte', [sb.status, sb.body.length, sb.body[20 + 0] !== undefined, sb.cache], [200, 14 + 2 + JSON.stringify({ game: 'vf3', p1: 'MRFINSTER', p2: 'DENVERDAN', c1: 'FR', c2: 'US' }).length + 4 + 3000, true, 'public, max-age=86400']);
let rep = await call('GET', `/v1/watch/frames?w=${W}&m=${MH}&from=0`);
let rr = read_reply(rep.body);
check('frames from 0: a whole 2048 (8 pieces), kept a day', [rr.first, rr.pairs.length / 2, rr.total, rr.state, rep.cache], [0, 2048, 3000, 1, 'public, max-age=86400']);
check('frames from 0: the inputs as sent', rr.pairs.slice(0, 6).concat(rr.pairs.slice(4094)), pairs.slice(0, 6).concat(pairs.slice(4094, 4096)));
check('frames from 0: the hashes in it', rr.hashes.map(([f, h]) => f + ':' + h.toString(16)), ['0:b', '120:fedcba9876543210', '1080:21', '1200:2c']);
rep = await call('GET', `/v1/watch/frames?w=${W}&m=${MH}&from=2100`);
rr = read_reply(rep.body);
check('frames from 2100: from its piece (2048), the rest, kept a second', [rr.first, rr.pairs.length / 2, rep.cache, rr.info], [2048, 952, 'public, max-age=1', [1, 0, 2, 1, 0]]);
check('frames from 2100: the inputs as sent', rr.pairs.slice(0, 4), pairs.slice(4096, 4100));
const other = await call('GET', `/v1/watch/frames?w=${W}&m=0000000000000001&from=0`);
check('another match: none', other.status, 404);

// ---- watchers: counted, the count to the host and on the list ----
const s1 = await call('POST', `/v1/watch/seen?w=${W}&s=aaaaaaaaaaaaaaaa`);
const s2 = await call('POST', `/v1/watch/seen?w=${W}&s=bbbbbbbbbbbbbbbb`);
check('watchers: two, from the start (no keeper)', [s2.body.watchers, s1.body.keeper, s2.body.keeper, s2.body.match], [2, false, false, MH]);
check('a room\'s match: not a Quick match\'s (its watchers may play next)', [s1.body.random, s2.body.random], [false, false]);
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 3000, [], [], [1, 0, 2, 1, 0]));
check('the host told how many watch (and 5 a second again)', r.body, { has: 3000, watchers: 2 });
check('the guest asks the count', (await call('GET', `/v1/watch/count?w=${W}`)).body, { watchers: 2 });
await settle();
d = await online();
const on = (d.rooms || []).find((x) => x.watch === W);
check('the rooms list: the match, both names, the score, round, watchers', on && [on.state, on.p1.name, on.p2.name, on.p2.country, on.score, on.round, on.watchers], ['playing', 'MRFINSTER', 'DENVERDAN', 'US', [1, 0], 4, 2]);
// a watcher come late (past a minute in): no checkpoint asked of anyone (checkpoints, a watcher's state handed to others,
// are no more: one could be forged to the players' hash); it runs from the match's start
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 3000, pairs.slice(0, 2000), [], [1, 0, 2, 1, 0]));
await call('POST', `/v1/watch/seen?w=${W}&s=cccccccccccccccc`);
const s1b = await call('POST', `/v1/watch/seen?w=${W}&s=aaaaaaaaaaaaaaaa`);
const s3b = await call('POST', `/v1/watch/seen?w=${W}&s=cccccccccccccccc`);
check('a watcher come late: no keeper, no checkpoint, for anyone', [r.body.has, s1b.body.keeper, s3b.body.keeper, s1b.body.checkpoint, s3b.body.checkpoint], [4000, false, false, -1, -1]);
const cp = head(5, M, 4 + 2 + 3 + 4 + 10);
cp.v.setUint32(14, 2400, true); cp.v.setUint16(18, 3, true); cp.b.set(new TextEncoder().encode('fmt'), 20); cp.v.setUint32(23, 10, true);
const up1 = await call('POST', `/v1/watch/cp?w=${W}&s=aaaaaaaaaaaaaaaa`, cp.b), up2 = await call('POST', `/v1/watch/cp?w=${W}&m=${MH}&s=bbbbbbbbbbbbbbbb`, cp.b);
check('a checkpoint upload, from the longest watcher or anyone: refused (gone)', [up1.status, up1.body.error, up2.status], [410, 'gone', 410]);
rr = read_reply((await call('GET', `/v1/watch/frames?w=${W}&m=${MH}&from=2900`)).body);
check('a reply says no checkpoint', rr.checkpoint, 0xffffffff);
const got = await call('GET', `/v1/watch/cp?w=${W}&m=${MH}&f=2400`);
check('none given out', [got.status, got.body.error], [410, 'gone']);
// the match over: off the list
await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 3000, [], [], [3, 1, 0, 0, 1]));
await settle();
d = await online();
check('the match over: off the rooms list', (d.rooms || []).filter((x) => x.watch === W).length, 0);
rr = read_reply((await call('GET', `/v1/watch/frames?w=${W}&m=${MH}&from=2900`)).body);
check('a watcher told it\'s over', [rr.state, rr.info], [2, [3, 1, 0, 0, 1]]);
// Play again: the same watch code kept for the host's next room
await call('POST', '/v1/close', { code: pub.body.code, key: pub.body.key }, '198.51.100.3');
const again = await call('POST', '/v1/host', me('MRFINSTER', { listed: true, want: pub.body.code, key: pub.body.key }), '198.51.100.3', 'FR');
check('Play again: the same room code and watch code, a new key', [again.body.code, again.body.wid, again.body.wkey !== K], [pub.body.code, W, true]);
r = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M, 3000, [], [], [3, 1, 0, 0, 1]));
check('the old key no longer sends', r.status, 404);
// a private room whose matches may be watched: not listed while it waits, watchable once played
const tw = await call('POST', '/v1/host', me('TOURNEY', { watchable: true }), '198.51.100.5');
await settle();
d = await online();
check('private, watchable: a watch code, not listed waiting', [!!tw.body.wid, (d.rooms || []).filter((x) => x.code === tw.body.code).length], [true, 0]);
// ---- a persistent room (Create room: listed, a name, the host stays): listed while it plays, a queue for the next ----
const kh = await call('POST', '/v1/host', me('SEOULKICK', { listed: true, persist: true, room: 'korea finals!' }), '198.51.100.20', 'KR');
await settle();
let room_now = (await online()).rooms.find((x) => x.code === kh.body.code);
check('a named room: listed waiting, its name upper case', room_now && [room_now.state, room_now.name, room_now.locked], ['waiting', 'KOREA FINALS!', false]);
check('the lobby says where the asker is (near me)', (await online()).you, 'AS');
const j1 = await call('POST', '/v1/join', me('DENVERDAN', { code: kh.body.code }), '198.51.100.21', 'US');
check('the first player joins it', [j1.status, !!j1.body.token], [200, true]);
await settle();
room_now = (await online()).rooms.find((x) => x.code === kh.body.code);
check('playing: still listed, both names, its code (anyone may queue)', room_now && [room_now.state, room_now.p2.name, room_now.code], ['playing', 'DENVERDAN', kh.body.code]);
const old_join = await call('POST', '/v1/join', me('OLDGAME', { code: kh.body.code }), '198.51.100.22');
check('an old game joining it now: full, as ever', [old_join.status, old_join.body.error, 'queued' in old_join.body], [409, 'full', false]);
const q1 = await call('POST', '/v1/join', me('SARAH.B', { code: kh.body.code, queue: true }), '198.51.100.23', 'JP');
const q2 = await call('POST', '/v1/join', me('AOI', { code: kh.body.code, queue: true }), '198.51.100.24', 'CA');
check('in line: first and second', [q1.body.queued, q2.body.queued, q2.body.of], [1, 2, 2]);
check('the host holds the room while it plays', (await call('POST', '/v1/hold', { code: kh.body.code, key: kh.body.key }, '198.51.100.20')).body, { ok: true, queue: 2 });
await settle();
check('the list says how many are next', (await online()).rooms.find((x) => x.code === kh.body.code).queue, 2);
// the match over: the host opens it again (its code and key): the queue's first gets it, the second waits on
const re = await call('POST', '/v1/host', me('SEOULKICK', { listed: true, persist: true, room: 'korea finals!', want: kh.body.code, key: kh.body.key }), '198.51.100.20', 'KR');
check('open again in place: the same code, key and watch code', [re.body.code, re.body.key, re.body.wid], [kh.body.code, kh.body.key, kh.body.wid]);
const jump = await call('POST', '/v1/join', me('AOI', { code: kh.body.code, queue: true }), '198.51.100.24', 'CA');
check('the second in line can\'t jump it', [jump.status, jump.body.queued], [409, 2]);
const turn = await call('POST', '/v1/join', me('SARAH.B', { code: kh.body.code, queue: true }), '198.51.100.23', 'JP');
check('the first in line gets the room', [turn.status, !!turn.body.token, turn.body.name], [200, true, 'SEOULKICK']);
const after = await call('POST', '/v1/join', me('AOI', { code: kh.body.code, queue: true }), '198.51.100.24', 'CA');
check('and the second is now first', after.body.queued, 1);
// a locked room: listed, its code not
const lk = await call('POST', '/v1/host', me('KAGE', { listed: true, locked: true, room: 'team kr' }), '198.51.100.25', 'KR');
await settle();
const lr = (await online()).rooms.find((x) => x.name === 'TEAM KR');
check('a locked room: listed, no code', lr && [lr.state, lr.locked, 'code' in lr], ['waiting', true, false]);
check('its code still joins', (await call('POST', '/v1/join', me('PAL', { code: lk.body.code }), '198.51.100.26')).status, 200);

// ---- Enter code: what a code is ----
check('peek: a room open', (await call('POST', '/v1/peek', { code: lk.body.code })).body.state, 'playing');
const op = await call('POST', '/v1/host', me('OPENER', { watchable: true }), '198.51.100.27');
check('peek: a room waiting is open, its matches watchable by its code', [(await call('POST', '/v1/peek', { code: op.body.code })).body.state, op.body.wid === op.body.code], ['open', true]);
check('peek: no such room', (await call('POST', '/v1/peek', { code: 'ZZZ222' })).status, 404);

// ---- the frames as they come (a stream), to a watcher ----
{
	const sh = await call('POST', '/v1/host', me('STREAMER', { watchable: true }), '198.51.100.30');
	const W2 = sh.body.wid, K2 = sh.body.wkey, M2 = 0x77n, MH2 = '0000000000000077';
	await call('POST', `/v1/watch/start?w=${W2}&k=${K2}`, start_bytes(M2, { p1: 'STREAMER', p2: 'OTHER' }, new Uint8Array(10)));
	const ps = [];
	for (let f = 0; f < 300; f++) ps.push(f, f + 1);
	await call('POST', `/v1/watch/push?w=${W2}&k=${K2}`, push_bytes(M2, 0, ps.slice(0, 200), [], [0, 0, 0, 0, 0]));
	const req = new Request(`http://lobby/v1/watch/live?w=${W2}&m=${MH2}&from=0&s=cccccccccccccccc`, { headers: { 'cf-connecting-ip': '198.51.100.31' } });
	const res = await worker.fetch(req, env);
	const reader = res.body.getReader();
	let buf = new Uint8Array(0);
	async function next_message() {
		for (;;) {
			if (buf.length >= 4) {
				const len = new DataView(buf.buffer, buf.byteOffset).getUint32(0, true);
				if (buf.length >= 4 + len) { const m = buf.slice(4, 4 + len); buf = buf.slice(4 + len); return m; }
			}
			const { done, value } = await reader.read();
			if (done) return null;
			const both = new Uint8Array(buf.length + value.length); both.set(buf); both.set(value, buf.length); buf = both;
		}
	}
	let m1 = read_reply(await next_message());
	check('live: what\'s there at once', [m1.first, m1.pairs.length / 2, m1.total], [0, 100, 100]);
	await call('POST', `/v1/watch/push?w=${W2}&k=${K2}`, push_bytes(M2, 100, ps.slice(200, 260), [[120, 9n]], [0, 0, 1, 0, 0]));
	const m2 = read_reply(await next_message());
	check('live: the next frames as soon as they come, their hash with them', [m2.first, m2.pairs.length / 2, m2.hashes.length, m2.info[2]], [100, 30, 1, 1]);
	check('live: a watcher on a stream counted', (await call('GET', `/v1/watch/count?w=${W2}`)).body.watchers, 1);
	reader.cancel();
}

// ---- 400 watching a match at most: one more is told "full" (its game: Too many watching right now) ----
{
	// (from many addresses, as a crowd is: one address has WATCHERS_PER_IP at most, below)
	for (let i = 2; i < 400; i++) await call('POST', `/v1/watch/seen?w=${W}&s=${(0x1000000000000000n + BigInt(i)).toString(16)}`, undefined, `192.0.2.${i >> 4}`);
	const full = await call('POST', `/v1/watch/seen?w=${W}&s=dddddddddddddddd`, undefined, '192.0.2.200');
	check('the 401st watcher: full', [full.status, full.body.error], [429, 'full']);
	const still = await call('POST', `/v1/watch/seen?w=${W}&s=aaaaaaaaaaaaaaaa`);
	check('one of the 400 still counted', [still.status, still.body.watchers], [200, 400]);
	const live = await call('GET', `/v1/watch/live?w=${W}&m=${MH}&from=0&s=dddddddddddddddd`);
	check('the 401st on a stream: full too', [live.status, live.body.error], [429, 'full']);
}

// ---- Quick match: listed to watch once its host's game sends it (a new build); never an older build's ----
{
	const at = { cf: { latitude: '35.6', longitude: '139.7', continent: 'AS' } };
	async function post(path, body, from, country) {
		const req = new Request('http://lobby' + path, { method: 'POST', body: JSON.stringify(body), headers: { 'cf-connecting-ip': from, 'x-test-country': country } });
		Object.defineProperty(req, 'cf', { value: at.cf });
		const r = await worker.fetch(req, env);
		return { status: r.status, body: await r.json() };
	}
	async function pair(a, b) {
		const ra = await post('/v1/random', a, a.from, a.country);
		const wait = post('/v1/random/wait', { ...a, ticket: ra.body.ticket }, a.from, a.country);
		await queued(env, ra.body.ticket);
		const rb = await post('/v1/random', b, b.from, b.country);
		const wa = (await wait).body;
		return wa.role === 'host' ? { host: wa, guest: rb.body } : { host: rb.body, guest: wa };
	}
	const q = (name, from, country, extra) => ({ ...me(name, extra), from, country });
	const m = await pair(q('AKIRA', '198.51.100.60', 'JP', { watchable: true, qm_listed: true }), q('PAI', '198.51.100.61', 'KR', { watchable: true, qm_listed: true }));
	check('a Quick match (new builds): the host has a watch code and key', [m.host.role, /^[A-HJKMNP-Z2-9]{6}$/.test(m.host.wid || ''), /^[0-9a-f]{32}$/.test(m.host.wkey || '')], ['host', true, true]);
	await settle();
	d = await online();
	check('not listed before its host sends the match', (d.rooms || []).filter((x) => x.watch === m.host.wid).length, 0);
	const QM = 0x5151n;
	const hostname = m.guest.name === 'AKIRA' ? 'AKIRA' : 'PAI', guestname = hostname === 'AKIRA' ? 'PAI' : 'AKIRA';
	await call('POST', `/v1/watch/start?w=${m.host.wid}&k=${m.host.wkey}`, start_bytes(QM, { p1: hostname, p2: guestname, c1: hostname === 'AKIRA' ? 'JP' : 'KR', c2: hostname === 'AKIRA' ? 'KR' : 'JP' }, new Uint8Array(10)));
	await call('POST', `/v1/watch/push?w=${m.host.wid}&k=${m.host.wkey}`, push_bytes(QM, 0, [1, 2, 3, 4], [], [2, 1, 1, 1, 0]));
	await settle();
	d = await online();
	const row = (d.rooms || []).find((x) => x.watch === m.host.wid);
	check('listed: a Quick match, both players and flags, the score, to watch only (no code)',
		row && [row.state, !!row.random, row.p1.name, row.p1.country, row.p2.name, row.p2.country, row.score, 'code' in row, row.name, row.region],
		['playing', true, hostname, hostname === 'AKIRA' ? 'JP' : 'KR', guestname, hostname === 'AKIRA' ? 'KR' : 'JP', [2, 1], false, '', 'AS']);
	check('its players asked nothing: their answers as ever', [m.guest.role, typeof m.guest.token, 'listed' in m.host], ['guest', 'string', false]);
	await call('POST', `/v1/watch/push?w=${m.host.wid}&k=${m.host.wkey}`, push_bytes(QM, 2, [], [], [2, 1, 1, 1, 1]));
	await settle();
	d = await online();
	check('over: off the list', (d.rooms || []).filter((x) => x.watch === m.host.wid).length, 0);
	const o = await pair(q('OLDHOST', '198.51.100.62', 'US'), q('OLDGUEST', '198.51.100.63', 'US'));
	check('an older build\'s Quick match: nothing to watch, never listed', [o.host.role, 'wid' in o.host], ['host', false]);
}

// ---- every game the same: VF1, VF2 and Team Battle's rooms and Quick matches listed under their own game ----
{
	const as_game = { vf1: { version: 'v1.5.0', pool: undefined }, vf2: { version: 'v0.1.16+mix1', pool: 'v0.1.16+mix1' }, vf3tb: { version: V, pool: V } };
	let ipn = 70;
	for (const game of ['vf1', 'vf2', 'vf3tb']) {
		const g = (name, extra = {}) => ({ ...me(name, extra), game, version: as_game[game].version, pool: as_game[game].pool, from: '198.51.100.' + ipn++, country: 'JP' });
		const host = g('H' + game.toUpperCase(), { listed: true, room: game + ' night' });
		const h = await call('POST', '/v1/host', host, host.from, 'JP');
		await settle();
		d = await online();
		const open = (d.rooms || []).find((x) => x.code === h.body.code);
		check(game + ': a listed room, under its game, not older', open && [open.game, open.state, open.name, open.older], [game, 'waiting', game.toUpperCase() + ' NIGHT', false]);
		// a Quick match of this game: listed to watch once its host sends it
		const a = g('A' + game.toUpperCase(), { watchable: true, qm_listed: true }), b = g('B' + game.toUpperCase(), { watchable: true, qm_listed: true });
		const post = async (path, body) => {
			const req = new Request('http://lobby' + path, { method: 'POST', body: JSON.stringify(body), headers: { 'cf-connecting-ip': body.from, 'x-test-country': 'JP' } });
			Object.defineProperty(req, 'cf', { value: { latitude: '35.6', longitude: '139.7', continent: 'AS' } });
			return (await worker.fetch(req, env)).json();
		};
		const ra = await post('/v1/random', a);
		const wait = post('/v1/random/wait', { ...a, ticket: ra.ticket });
		await queued(env, ra.ticket);
		const rb = await post('/v1/random', b);
		const wa = await wait;
		const hs = wa.role === 'host' ? wa : rb;
		check(game + ': a Quick match has a watch code', /^[A-HJKMNP-Z2-9]{6}$/.test(hs.wid || ''), true);
		await call('POST', `/v1/watch/start?w=${hs.wid}&k=${hs.wkey}`, start_bytes(0x99n + BigInt(ipn), { p1: 'P1' + game.toUpperCase(), p2: 'P2' + game.toUpperCase(), c1: 'JP', c2: 'KR' }, new Uint8Array(10)));
		await settle();
		d = await online();
		const q = (d.rooms || []).find((x) => x.watch === hs.wid);
		check(game + ': its Quick match listed under its game, to watch only', q && [q.game, !!q.random, 'code' in q, q.p1.name, q.p2.country], [game, true, false, 'P1' + game.toUpperCase(), 'KR']);
	}
}

// ---- names: the word filter (src/words.js); a blocked room name is its host's, a blocked player PLAYER ----
{
	const { name_ok: ok_fn } = await import(pathToFileURL(resolve(process.argv[2])).href);
	const blocked = ['FUCK', 'FUUUCK', 'SH1THEAD', 'F*CK YOU', 'F.CK OFF', 'F U C K', 'F.U.C.K', 'PHUCK FUCKER', 'XFUCKX', 'SH1T', 'SH!T HEAD', 'A$$', 'ASS', 'ASSSS', '5H1T', 'BITCH3S', 'N1GGER', 'FAGGOT',
		'SIBAL', 'SSIBAL', 'GAESAEKKI', 'KICHIGAI', 'MANKO', 'PUTA', 'MIERDA', 'CARALHO', 'MERDE', 'PUTAIN', 'C0CK', 'D1CK', 'TITS', 'KOREA CUNT',
		// (plurals, -ING and -ER forms, spellings round it, | for I)
		'SHITS', 'DICKS', 'COCKS', 'ASSES', 'SHITTING', 'SHITTER', 'SHIIIT', 'CUNTS', 'TWATS', 'WANKER', 'WANKING', 'SLUTS', 'FUCKING', 'FUCKER', 'FCUK', 'PHUCK', 'N|GGER', 'SH|T', 'FUK', 'FUKIN', 'MERDAS', 'SIBALS', 'S H I T HEAD',
		// (a space or a mark inside a word, V for U, and one of them run into an insult)
		'NI GGER', 'FU CK', 'BIT CH', 'BIT CHES', 'SH IT', 'FU-CK', 'FVCK', 'FVCKER', 'CUNTFACE', 'SHITFACE', 'COCKSUCK', 'DICKHEAD', 'ASSHAT', 'TWATFACE', 'FU CKFACE'];
	const fine = ['CLASSIC', 'PASSION', 'SCUNTHORPE', 'ASSASSIN', 'SUSHI', 'SHIITAKE', 'COCKPIT', 'DICKENS', 'TITANIC', 'ESSEX', 'GRAPE', 'NIGERIA', 'MASSAGE',
		'KOREA FINALS', 'AS ONE', 'BASS', 'SEOULKICK', 'KAGE-MARU', 'SARAH.B', "AKIRA'S DOJO", 'COMPUTADORA', 'SPICY', 'PEDRO', 'CONOR', 'MANKIND', 'MATSUTAKE',
		// (words that hold a short one, and two real words that would make one run together)
		'MERDEKA', 'MERDAN', 'AAS', 'SIBALD', 'SUMMER DAY', 'ORBIT CHAMP', 'WHO REALLY', 'HAMMER DEN', 'TOP ORNATE', 'SUMMERDAYS', 'CUMIN', 'MIDDLESEX', 'CUMBERLAND', 'HANCOCK', 'SHITAKE',
		// (words that would be one of them with a space taken out, or a V read as U, or with something after it)
		'MISTER', 'SUSSEX', 'TITAN', 'PUT A COIN', 'AS SOON', 'FUKUOKA', 'WANKEL', 'DAVE', 'VIVA', 'EVIL KNIGHT', 'SPICE HEAD', 'BASSHEAD'];
	const hit = blocked.filter((x) => ok_fn(x));
	check('the filter: every blocked name blocked (substitutions, spaces, runs, endings)', hit, []);
	const miss = fine.filter((x) => !ok_fn(x));
	check('the filter: no false positives (CLASSIC, PASSION, SCUNTHORPE, ASSASSIN, SUMMER DAY, MERDEKA and the like)', miss, []);
	console.log(`     (the filter: ${blocked.length - hit.length} of ${blocked.length} to block blocked, ${fine.length - miss.length} of ${fine.length} to pass passed)`);
	// a room named with one: listed as its host's; the host told how it's listed
	const bad = await call('POST', '/v1/host', me('GOODHOST', { listed: true, room: 'FUCK THIS' }), '198.51.100.90', 'FR');
	check('a blocked room name: the host told it\'s listed as its own', bad.body.listed_as, { room: '', name: 'GOODHOST' });
	const good = await call('POST', '/v1/host', me('NICEHOST', { listed: true, persist: true, room: 'classic night' }), '198.51.100.91', 'FR');
	check('a fine one: as typed', good.body.listed_as, { room: 'CLASSIC NIGHT', name: 'NICEHOST' });
	const badname = await call('POST', '/v1/host', me('SH1THEAD', { listed: true, room: 'SUSHI BAR' }), '198.51.100.92', 'FR');
	await settle();
	d = await online();
	const rb = (d.rooms || []).find((x) => x.code === bad.body.code), rg = (d.rooms || []).find((x) => x.code === good.body.code), rn = (d.rooms || []).find((x) => x.code === badname.body.code);
	check('listed: the blocked room under its host\'s name, the fine one as typed', [rb && rb.name, rb && rb.p1.name, rg && rg.name], ['', 'GOODHOST', 'CLASSIC NIGHT']);
	check('a blocked player name: PLAYER, the room\'s own name kept', [rn && rn.p1.name, rn && rn.name, badname.body.listed_as], ['PLAYER', 'SUSHI BAR', { room: 'SUSHI BAR', name: 'PLAYER' }]);
	// a player joining with one: PLAYER to the host; a match's names sent to watchers the same
	const j = await call('POST', '/v1/join', me('A$$HOLE', { code: good.body.code }), '198.51.100.93', 'US');
	await settle();
	d = await online();
	const rj = (d.rooms || []).find((x) => x.code === good.body.code);
	check('a guest with a blocked name: PLAYER in the list (and to the host)', [j.status, rj && rj.state, rj && rj.p2 && rj.p2.name], [200, 'playing', 'PLAYER']);
	await call('POST', `/v1/watch/start?w=${good.body.wid}&k=${good.body.wkey}`, start_bytes(0x4242n, { p1: 'NICEHOST', p2: 'F*CK YOU', c1: 'FR', c2: 'US' }, new Uint8Array(10)));
	await settle();
	d = await online();
	const rw = (d.rooms || []).find((x) => x.watch === good.body.wid);
	check('a watched match\'s names as the host sent them, a blocked one PLAYER', rw && [rw.p1.name, rw.p2.name], ['NICEHOST', 'PLAYER']);
}

// ---- Quick match: listed only if both players let it be (Show my Quick Match games in Rooms), watchable by its code either way ----
{
	async function post(path, body) {
		const req = new Request('http://lobby' + path, { method: 'POST', body: JSON.stringify(body), headers: { 'cf-connecting-ip': body.from, 'x-test-country': 'JP' } });
		Object.defineProperty(req, 'cf', { value: { latitude: '35.6', longitude: '139.7', continent: 'AS' } });
		return (await worker.fetch(req, env)).json();
	}
	async function pair(a, b) {
		const ra = await post('/v1/random', a);
		const wait = post('/v1/random/wait', { ...a, ticket: ra.ticket });
		await queued(env, ra.ticket);
		const rb = await post('/v1/random', b);
		const wa = await wait;
		return wa.role === 'host' ? wa : rb;
	}
	const show = async (hx, gx, M, from) => {   // (a Quick match: listed? watchable by its code?)
		const h = await pair({ ...me('QHOST' + (M & 15), { watchable: true, ...hx }), from: '198.51.100.' + from }, { ...me('QGUEST' + (M & 15), { watchable: true, ...gx }), from: '198.51.100.' + (from + 1) });
		await call('POST', `/v1/watch/start?w=${h.wid}&k=${h.wkey}`, start_bytes(BigInt(M), { p1: 'QHOST' + (M & 15), p2: 'QGUEST' + (M & 15) }, new Uint8Array(10)));
		await settle();
		const listed = ((await online()).rooms || []).some((x) => x.watch === h.wid);
		const seen = await call('POST', `/v1/watch/seen?w=${h.wid}&s=eeeeeeeeeeee${(0x1000 + M).toString(16).slice(-4)}`);
		return [listed, seen.status, seen.body.match === BigInt(M).toString(16).padStart(16, '0'), seen.body.random];
	};
	// (and its watchers told it's a Quick match's, listed or not: no room, so no Play next)
	check('a Quick match, one player\'s setting Off: not listed, still watchable by its code', await show({ qm_listed: false }, { qm_listed: true }, 0x5353, 110), [false, 200, true, true]);
	check('a Quick match, both On: listed', await show({ qm_listed: true }, { qm_listed: true }, 0x5354, 112), [true, 200, true, true]);
	check('a new host On, an older build\'s guest (says nothing): not listed, still watchable by its code', await show({ qm_listed: true }, { qm_listed: undefined }, 0x5355, 114), [false, 200, true, true]);
	check('neither says (a game that sends nothing of it): not listed', await show({}, {}, 0x5356, 116), [false, 200, true, true]);
}

// ---- the load on watching, all matches together: the Stats say it back; past its marks new watchers poll, slower,
// then are told busy; the hosts send less often; one address gets 8 streams a match ----
{
	const host = await call('POST', '/v1/host', me('LOADHOST', { watchable: true }), '198.51.100.120');
	const W3 = host.body.wid, K3 = host.body.wkey, M3 = 0x6161n, MH3 = '0000000000006161';
	await call('POST', `/v1/watch/start?w=${W3}&k=${K3}`, start_bytes(M3, { p1: 'LOADHOST', p2: 'X' }, new Uint8Array(10)));
	await settle();
	const wobj = env.WATCHES.objs.get('w|' + W3);
	check('the Stats tell a Watch the load on all of them', [wobj.load.matches >= 1, Number.isFinite(wobj.load.watchers)], [true, true]);
	let r = await call('POST', `/v1/watch/seen?w=${W3}&s=1111111111111111`);
	check('light load: a stream, the edge asked every second', [r.body.live, r.body.poll_ms], [true, 1000]);
	r = await call('POST', `/v1/watch/push?w=${W3}&k=${K3}`, push_bytes(M3, 0, [1, 2], [], [0, 0, 0, 0, 0]));
	check('light load: the host sends as ever (no slower pace)', 'every' in r.body, false);
	// one address: 8 streams to a match, not 9
	const opened = [];
	for (let i = 0; i < 9; i++) {
		const res = await worker.fetch(new Request(`http://lobby/v1/watch/live?w=${W3}&m=${MH3}&from=0&s=${(0x2000000000000000n + BigInt(i)).toString(16)}`, { headers: { 'cf-connecting-ip': '198.51.100.121' } }), env);
		opened.push(res);
	}
	check('one address: 8 streams to a match, the 9th asks the edge', opened.map((x) => x.status).filter((x) => x === 200).length + '/' + opened[8].status, '8/429');
	for (const x of opened) if (x.status === 200) await x.body.cancel();
	// the soft mark: new watchers poll, less often; the host slower
	await settle();
	wobj.load = { matches: 600, watchers: 4500 };
	r = await call('POST', `/v1/watch/seen?w=${W3}&s=2222222222222222`);
	const live = await call('GET', `/v1/watch/live?w=${W3}&m=${MH3}&from=0&s=2222222222222222`);
	check('past the soft mark: no stream, the edge asked every 3 s', [r.status, r.body.live, r.body.poll_ms, live.status], [200, false, 3000, 429]);
	r = await call('POST', `/v1/watch/push?w=${W3}&k=${K3}`, push_bytes(M3, 2, [], [], [0, 0, 0, 0, 0]));
	check('the host sends every 400 ms (not 200)', r.body.every, 400);
	await settle();
	wobj.load = { matches: 2500, watchers: 9000 };
	r = await call('POST', `/v1/watch/seen?w=${W3}&s=3333333333333333`);
	const old = await call('POST', `/v1/watch/seen?w=${W3}&s=1111111111111111`);
	check('past the hard mark: a new watcher told busy, one watching already carries on', [r.status, r.body.error, old.status], [429, 'busy', 200]);
	await settle();   // (the Stats' own answer to the last telling in first: the load set after it)
	wobj.load = { matches: 2500, watchers: 9000 };
	r = await call('POST', `/v1/watch/push?w=${W3}&k=${K3}`, push_bytes(M3, 2, [], [], [0, 0, 0, 0, 0]));
	check('and the host every second', r.body.every, 1000);
	wobj.load = { matches: 0, watchers: 0 };
}

// ---- WebSockets (the Watch's own, hibernating): the host's feed, a watcher's frames as they come ----
{
	const host = await call('POST', '/v1/host', me('WSHOST', { watchable: true }), '198.51.100.130');
	const W4 = host.body.wid, K4 = host.body.wkey, M4 = 0x7171n, MH4 = '0000000000007171';
	const upgrade = async (path, from = '198.51.100.131') => worker.fetch(new Request('http://lobby' + path, { headers: { upgrade: 'websocket', 'cf-connecting-ip': from } }), env);
	const listen = (ws) => { const got = []; ws.accept(); ws.addEventListener('message', (e) => got.push(e.data)); return got; };
	const tick = settle;   // (everything the objects left running, done: no sleeping on it)
	const bad = await upgrade(`/v1/watch/feed?w=${W4}&k=wrong`);
	check('the host\'s feed with the wrong key: refused', bad.status, 404);
	const feed = await upgrade(`/v1/watch/feed?w=${W4}&k=${K4}`, '198.51.100.130');
	check('the host\'s feed: a WebSocket', [feed.status, !!feed.webSocket], [101, true]);
	const answers = listen(feed.webSocket);
	feed.webSocket.send(start_bytes(M4, { p1: 'WSHOST', p2: 'WSGUEST' }, new Uint8Array(10)));
	const ps = [];
	for (let f = 0; f < 400; f++) ps.push(f, f + 1);
	feed.webSocket.send(push_bytes(M4, 0, ps.slice(0, 200), [[50, 7n]], [0, 0, 0, 0, 0]));
	await tick();
	check('the start and a push as messages, each answered', answers.map((x) => JSON.parse(x)), [{ ok: true }, { has: 100, watchers: 0, every: 30000 }]);
	const seen = await call('POST', `/v1/watch/seen?w=${W4}&s=4444444444444444`);
	const wsr = await upgrade(`/v1/watch/ws?w=${W4}&m=${MH4}&from=0&s=4444444444444444`);
	check('a watcher\'s WebSocket', [seen.body.match, wsr.status, !!wsr.webSocket], [MH4, 101, true]);
	await tick();
	check('the first watcher: the host woken at once, down its socket (5 a second)', JSON.parse(answers[answers.length - 1]), { wake: true, every: 200 });
	const got = listen(wsr.webSocket);
	await tick();
	const bin = got.filter((x) => typeof x !== 'string'), txt = got.filter((x) => typeof x === 'string').map((x) => JSON.parse(x));
	const first = read_reply(new Uint8Array(bin[0]));
	check('at once: the frames from where it is, and the status (counted by its socket; no keeper)', [first.first, first.pairs.length / 2, txt.length > 0 && txt[txt.length - 1].watchers, txt.length > 0 && txt[txt.length - 1].keeper],
		[0, 100, 1, false]);
	feed.webSocket.send(push_bytes(M4, 100, ps.slice(200, 260), [], [1, 0, 1, 0, 0]));
	await tick();
	const bin2 = got.filter((x) => typeof x !== 'string');
	const next = read_reply(new Uint8Array(bin2[bin2.length - 1]));
	check('the next push to it as it comes', [next.first, next.pairs.length / 2, next.info[0]], [100, 30, 1]);
	check('the count with it: 1 (the socket, not twice)', (await call('GET', `/v1/watch/count?w=${W4}`)).body.watchers, 1);
	const before = sockets.in;
	wsr.webSocket.send('ping');
	await tick();
	check('its ping answered without waking the Watch', [got[got.length - 1], sockets.in - before], ['pong', 0]);
	const wobj = env.WATCHES.objs.get('w|' + W4);
	await tick();
	wobj.load = { matches: 0, watchers: 4500 };
	const shed = await upgrade(`/v1/watch/ws?w=${W4}&m=${MH4}&from=0&s=5555555555555555`);
	check('past the soft mark: no new WebSocket (it asks the edge, as without one)', shed.status, 429);
	wobj.load = { matches: 0, watchers: 0 };
	wsr.webSocket.close(1000, 'bye');
	await tick();
	check('closed: not counted', (await call('GET', `/v1/watch/count?w=${W4}`)).body.watchers, 0);
	// (the object woken without its match, as after hibernating: the host's next push told to send it all again)
	wobj.m = null;
	feed.webSocket.send(push_bytes(M4, 130, [], [], [1, 0, 1, 0, 0]));
	await tick();
	check('woken without the match: the host told to send it all again', JSON.parse(answers[answers.length - 1]).has, -1);
}

// ---- the month's spend ceiling: the Stats add up the watched match-hours; past it, the hosts told off, new watchers busy ----
{
	const host = await call('POST', '/v1/host', me('BUDGET', { watchable: true }), '198.51.100.140');
	const W5 = host.body.wid, K5 = host.body.wkey, M5 = 0x8181n;
	await call('POST', `/v1/watch/start?w=${W5}&k=${K5}`, start_bytes(M5, { p1: 'BUDGET', p2: 'X' }, new Uint8Array(10)));
	await settle();
	const wobj = env.WATCHES.objs.get('w|' + W5);
	check('under the ceiling: the host sends on', 'off' in (await call('POST', `/v1/watch/push?w=${W5}&k=${K5}`, push_bytes(M5, 0, [1, 2], [], [0, 0, 0, 0, 0]))).body, false);
	env.MONTH_MATCH_HOURS = '0.000001';   // (a ceiling already reached, as at the end of a busy month)
	await settle();
	await call('POST', `/v1/watch/push?w=${W5}&k=${K5}`, push_bytes(M5, 1, [3, 4], [], [1, 0, 0, 0, 0]));   // (a new score: the Stats told)
	await settle();
	check('the Stats say the month is over its ceiling', wobj.load.over, true);
	const r = await call('POST', `/v1/watch/push?w=${W5}&k=${K5}`, push_bytes(M5, 2, [5, 6], [], [0, 0, 0, 0, 0]));
	const sn = await call('POST', `/v1/watch/seen?w=${W5}&s=6666666666666666`);
	check('past it: the host told off (it sends no more), a new watcher told busy', [r.body.off, sn.status, sn.body.error], [true, 429, 'busy']);
	delete env.MONTH_MATCH_HOURS;
	wobj.load = { matches: 0, watchers: 0 };
}

// ---- the lobby's own reading of the host's messages (by hand: the frames themselves Cloudflare's): anything at all
// on the host's WebSocket and its POSTs, random bytes and real ones spoilt, answered and never thrown; the match whole ----
{
	const host = await call('POST', '/v1/host', me('FUZZHOST', { watchable: true }), '198.51.100.150');
	const W6 = host.body.wid, K6 = host.body.wkey, M6 = 0x9191n;
	const feed = await worker.fetch(new Request(`http://lobby/v1/watch/feed?w=${W6}&k=${K6}`, { headers: { upgrade: 'websocket', 'cf-connecting-ip': '198.51.100.150' } }), env);
	const answers = [];
	feed.webSocket.accept();
	feed.webSocket.addEventListener('message', (e) => answers.push(e.data));
	let seed = 12345;
	const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
	const good = [start_bytes(M6, { p1: 'FUZZHOST', p2: 'X' }, new Uint8Array(40)), push_bytes(M6, 0, [1, 2, 3, 4], [[0, 5n]], [0, 0, 0, 0, 0])];
	let thrown = 0, sent = 0;
	for (let i = 0; i < 2000; i++) {
		let b;
		if (i % 4 === 0) b = new Uint8Array(Math.floor(rnd() * 64)).map(() => Math.floor(rnd() * 256));
		else {
			b = good[i % 2].slice();
			const cut = rnd() < 0.5 ? Math.floor(rnd() * b.length) : b.length;
			b = b.slice(0, cut);
			for (let k = 0; k < 1 + Math.floor(rnd() * 4) && b.length; k++) b[Math.floor(rnd() * b.length)] = Math.floor(rnd() * 256);
			if (rnd() < 0.3 && b.length > 6) { b[4] = rnd() < 0.5 ? 1 : 2; b[5] = 1; b.set([0x56, 0x46, 0x57, 0x31], 0); }   // (its head right: deeper in)
		}
		feed.webSocket.send(b);
		sent++;
		try {
			const r = await call('POST', `/v1/watch/${i % 2 ? 'push' : 'start'}?w=${W6}&k=${K6}`, b);
			if (r.status >= 500) thrown++;
		} catch (e) { thrown++; }
	}
	await settle();
	check('2,000 spoilt messages on the host\'s WebSocket and as POSTs: each answered, none thrown', [answers.length, thrown], [sent, 0]);
	const short = start_bytes(M6, {}, new Uint8Array(0)).slice(0, 15);   // (a start's head and no more: its meta's length not there)
	check('a start cut after its head: refused, not thrown', (await call('POST', `/v1/watch/start?w=${W6}&k=${K6}`, short)).status, 400);
	feed.webSocket.send(good[0]);
	feed.webSocket.send(push_bytes(M6, 0, [1, 2, 3, 4], [], [0, 0, 0, 0, 0]));
	await settle();
	check('and the match taken whole after it', JSON.parse(answers[answers.length - 1]).has, 2);
}

// ---- the month's ceiling counts an hour nobody watched as 0.4 (its Watch asleep between uploads every 30 s) ----
{
	const st = env.STATS.objs.get('all');
	const tell = (watchers) => st.fetch(new Request('https://stats/tell', { method: 'POST', body: JSON.stringify({ op: 'watch', wid: 'MNTH22', game: 'vf3', version: V, p: [['A', 'KR'], ['B', 'US']], wins: [0, 0], rounds: [0, 0], watchers }) }));
	await tell(0);
	const m0 = st.s.month.ms;
	st.s.watches.MNTH22.seen -= 30000;
	await tell(2);
	const unwatched = st.s.month.ms - m0;
	st.s.watches.MNTH22.seen -= 30000;
	await tell(2);
	const watched = st.s.month.ms - m0 - unwatched;
	check('the month\'s hours: 30 s nobody watched counts 12 s, 30 s watched 30 s', [Math.round(unwatched / 1000), Math.round(watched / 1000)], [12, 30]);
	await st.fetch(new Request('https://stats/tell', { method: 'POST', body: JSON.stringify({ op: 'unwatch', wid: 'MNTH22' }) }));
}

// ---- a room's host on a WebSocket (hibernating): no wait every 20 s, no hold every 15 s; its player told at once ----
{
	const up = async (path, from = '198.51.100.160') => worker.fetch(new Request('http://lobby' + path, { headers: { upgrade: 'websocket', 'cf-connecting-ip': from } }), env);
	const tick = settle;   // (everything the objects left running, done: no sleeping on it)
	const h = await call('POST', '/v1/host', me('SOCKHOST', { persist: true, listed: true, room: 'socket room' }), '198.51.100.160');
	check('the room\'s WebSocket with the wrong key: refused', (await up(`/v1/room/ws?code=${h.body.code}&key=wrong`)).status, 404);
	check('no such room: refused', (await up(`/v1/room/ws?code=ZZZ222&key=${h.body.key}`)).status, 404);
	check('not an upgrade: told so', (await call('GET', `/v1/room/ws?code=${h.body.code}&key=${h.body.key}`)).status, 426);
	const ws = await up(`/v1/room/ws?code=${h.body.code}&key=${h.body.key}`);
	check('the room\'s WebSocket', [ws.status, !!ws.webSocket], [101, true]);
	const got = [];
	ws.webSocket.accept();
	ws.webSocket.addEventListener('message', (e) => { if (e.data !== 'pong') got.push(JSON.parse(e.data)); });
	const robj = env.ROOMS.objs.get(h.body.code);
	robj.r.seen -= 10 * 60e3;   // (no wait for ten minutes: as if the host had gone, but its socket's open)
	check('held by its socket alone: still there (a peek), no wait asked', (await call('POST', '/v1/peek', { code: h.body.code })).body.state, 'open');
	const before = sockets.in;
	ws.webSocket.send('ping');
	await tick();
	check('its ping answered without waking the room', sockets.in - before, 0);
	const j = await call('POST', '/v1/join', me('SOCKGUEST', { code: h.body.code }), '198.51.100.161', 'US');
	await tick();
	check('a player joins: the host told at once, as a wait\'s answer', [j.status, got.length, got[0] && got[0].guest && got[0].guest.name], [200, 1, 'SOCKGUEST']);
	await tick();
	const listed = (await online()).rooms.find((x) => x.code === h.body.code);
	check('its match on: listed playing (no hold asked: the socket holds it)', listed && listed.state, 'playing');
	await call('POST', '/v1/close', { code: h.body.code, key: h.body.key }, '198.51.100.160');
	await tick();
	check('the room gone: the host told, its socket closed', [got[got.length - 1], ws.webSocket.open], [{ error: 'no_room' }, false]);
	// a socket open already: a second one replaces it (the game's reconnect)
	const h2 = await call('POST', '/v1/host', me('SOCKHOST2'), '198.51.100.162');
	const a = await up(`/v1/room/ws?code=${h2.body.code}&key=${h2.body.key}`, '198.51.100.162');
	a.webSocket.accept();
	const b = await up(`/v1/room/ws?code=${h2.body.code}&key=${h2.body.key}`, '198.51.100.162');
	b.webSocket.accept();
	await tick();
	check('a second socket: the first closed, one open', [a.webSocket.open, env.ROOMS.objs.get(h2.body.code).ctx.getWebSockets('host').length], [false, 1]);
	// closed: the room kept HOST_GONE_MS from then, as after its last wait (the game waits over HTTPS meanwhile), however
	// long it was held by the socket alone (found by tools/lobbyload: a room held a minute was dropped as its socket closed)
	env.ROOMS.objs.get(h2.body.code).r.seen -= 10 * 60e3;
	b.webSocket.close(1000, 'bye');
	await tick();
	check('its socket closed: the room still there for now', (await call('POST', '/v1/peek', { code: h2.body.code })).body.state, 'open');
}

// ---- a locked room: its code never in the list, its match's watch code (the same code) included ----
{
	const lk = await call('POST', '/v1/host', me('LOCKED1', { listed: true, locked: true, persist: true, room: 'BY CODE' }), '198.51.100.170');
	await call('POST', '/v1/join', me('LOCKGUEST', { code: lk.body.code }), '198.51.100.171');
	await call('POST', `/v1/watch/start?w=${lk.body.wid}&k=${lk.body.wkey}`, start_bytes(0xa1a1n, { p1: 'LOCKED1', p2: 'LOCKGUEST' }, new Uint8Array(8)));
	const lk2 = await call('POST', '/v1/host', me('LOCKED2', { listed: true, locked: true, room: 'BY CODE 2' }), '198.51.100.172');
	await call('POST', '/v1/join', me('LOCKGUEST2', { code: lk2.body.code }), '198.51.100.173');
	await call('POST', `/v1/watch/start?w=${lk2.body.wid}&k=${lk2.body.wkey}`, start_bytes(0xa2a2n, { p1: 'LOCKED2', p2: 'LOCKGUEST2' }, new Uint8Array(8)));
	await settle();
	const all = JSON.stringify((await online()).rooms || []);
	const mine = ((await online()).rooms || []).filter((x) => x.p1 && x.p1.name === 'LOCKED1');
	check('a locked room playing: listed (locked, playing), its code nowhere in the list (not as a watch code either)', [mine.length && mine[0].state, mine.length && mine[0].locked, all.includes(lk.body.code), all.includes(lk2.body.code)], ['playing', true, false, false]);
	check('a locked room\'s match still watched by its code (from the host)', (await call('POST', `/v1/watch/seen?w=${lk.body.wid}&s=a1a1a1a1a1a1a1a1`)).body.match, '000000000000a1a1');
}

// ---- one address can't fill a match with made-up watchers (any s it likes): WATCHERS_PER_IP from it at most ----
{
	const h = await call('POST', '/v1/host', me('CROWDED', { watchable: true }), '198.51.100.180');
	await call('POST', `/v1/watch/start?w=${h.body.wid}&k=${h.body.wkey}`, start_bytes(0xb1b1n, { p1: 'CROWDED', p2: 'X' }, new Uint8Array(8)));
	let taken = 0;
	for (let i = 0; i < 400; i++) if ((await call('POST', `/v1/watch/seen?w=${h.body.wid}&s=${(0x2000000000000000n + BigInt(i)).toString(16)}`, undefined, '203.0.113.66')).status === 200) taken++;
	const more = await call('POST', `/v1/watch/seen?w=${h.body.wid}&s=${(0x2000000000000000n + 5n).toString(16)}`, undefined, '203.0.113.66');
	const real = await call('POST', `/v1/watch/seen?w=${h.body.wid}&s=feedfacefeedface`, undefined, '198.51.100.181');
	check('one address, 400 made-up watchers: 32 taken (one already in carries on); a real watcher from elsewhere then let in', [taken, more.status, real.status, real.body.watchers], [32, 200, 200, 33]);
}

// ---- watching behind a shared address (a phone network's, a school's) never uses up its players' 600 a minute ----
{
	const saved = { ANY_LIMIT: env.ANY_LIMIT, WATCHING_LIMIT: env.WATCHING_LIMIT }, used = {};
	for (const [k, n] of [['ANY_LIMIT', 600], ['WATCHING_LIMIT', 1800]]) env[k] = { limit: async ({ key }) => ({ success: (used[k + key] = (used[k + key] || 0) + 1) <= n }) };   // (a minute's window, counted)
	const h = await call('POST', '/v1/host', me('WATCHED', { watchable: true }), '198.51.100.190');
	await call('POST', `/v1/watch/start?w=${h.body.wid}&k=${h.body.wkey}`, start_bytes(0xc1c1n, { p1: 'WATCHED', p2: 'X' }, new Uint8Array(8)));
	const NAT = '203.0.113.210';
	for (let w = 0; w < 10; w++) {   // (ten watching from it, a minute each on the pieces: no stream for them)
		await call('POST', `/v1/watch/seen?w=${h.body.wid}&s=${(0x3000000000000000n + BigInt(w)).toString(16)}`, undefined, NAT);
		for (let i = 0; i < 59; i++) await call('GET', `/v1/watch/frames?w=${h.body.wid}&m=000000000000c1c1&from=0`, undefined, NAT);
	}
	const host = await call('POST', '/v1/host', me('NATHOST'), NAT), search = await call('POST', '/v1/random', me('NATSEEK'), NAT);
	check('ten watching for a minute behind one address: its players still host and search', [host.status, search.status], [200, 200]);
	Object.assign(env, saved);
}

// ---- the Watch out of memory between a match's idle uploads (Cloudflare's hibernation: 10 s idle, only hibernating
// sockets open): woken by the next upload, the match read back from its storage, the host sending on from where it was ----
{
	const h = await call('POST', '/v1/host', me('SLEEPER', { watchable: true }), '198.51.100.200');
	const W = h.body.wid, K = h.body.wkey, MS = 0xd1d1n, MSH = '000000000000d1d1';
	const feed = await worker.fetch(new Request(`http://lobby/v1/watch/feed?w=${W}&k=${K}`, { headers: { upgrade: 'websocket', 'cf-connecting-ip': '198.51.100.200' } }), env);
	const answers = [];
	feed.webSocket.accept();
	feed.webSocket.addEventListener('message', (e) => answers.push(JSON.parse(e.data)));
	const blob = new Uint8Array(1536 * 1024).map((_, i) => (i * 13) & 255);   // (a start over a MiB: kept in two pieces)
	const ps = (from, n) => { const o = []; for (let f = from; f < from + n; f++) o.push(f & 0xffff, (f * 3) & 0xffff); return o; };
	feed.webSocket.send(start_bytes(MS, { p1: 'SLEEPER', p2: 'X' }, blob));
	for (let f = 0; f < 20000; f += 4000) feed.webSocket.send(push_bytes(MS, f, ps(f, 4000), [[f, BigInt(f + 1)]], [0, 0, 1, 0, 0]));
	await settle();
	const hibernate = () => {   // (out of memory: a new object from the same storage and sockets, as Cloudflare makes it)
		const old = env.WATCHES.objs.get('w|' + W), fresh = new Watch(old.ctx, env);
		for (const ws of old.ctx.getWebSockets()) ws.owner.obj = fresh;
		env.WATCHES.objs.set('w|' + W, fresh);
	};
	hibernate();
	feed.webSocket.send(push_bytes(MS, 20000, ps(20000, 600), [], [1, 0, 2, 0, 0]));
	await settle();
	check('out of memory between idle uploads: the next one taken on from where it was (not "send it all again")', answers[answers.length - 1], { has: 20600, watchers: 0, every: 30000 });
	hibernate();
	const sb = await call('GET', `/v1/watch/start?w=${W}&m=${MSH}`);
	const rr = read_reply((await call('GET', `/v1/watch/frames?w=${W}&m=${MSH}&from=14336`)).body);   // (across two blocks kept)
	const want = ps(14336, 2048), at = 20 + JSON.stringify({ p1: 'SLEEPER', p2: 'X' }).length;
	check('and a watcher come then: the start whole, the frames as sent, the hashes and score', [sb.status, sb.body.length === 14 + 2 + JSON.stringify({ p1: 'SLEEPER', p2: 'X' }).length + 4 + blob.length && sb.body[at + 1000] === blob[1000] && sb.body[at + blob.length - 1] === blob[blob.length - 1], rr.first, rr.total, JSON.stringify(rr.pairs) === JSON.stringify(want), rr.hashes.map((x) => x[0]), rr.info],
		[200, true, 14336, 20600, true, [16000], [1, 0, 2, 0, 0]]);
	hibernate();
	feed.webSocket.send(push_bytes(MS, 21000, ps(21000, 10), [], [1, 0, 2, 0, 0]));   // (frames it never had: a gap)
	await settle();
	check('woken with less than the host thinks it has (a gap): all of it again', answers[answers.length - 1].has, -1);
	feed.webSocket.send(start_bytes(MS + 1n, { p1: 'SLEEPER', p2: 'Y' }, new Uint8Array(10)));
	await settle();
	hibernate();
	feed.webSocket.send(push_bytes(MS + 1n, 0, ps(0, 5), [], [0, 0, 0, 0, 0]));
	await settle();
	check('a new match: the last one let go, the new one kept from its start', answers[answers.length - 1], { has: 5, watchers: 0, every: 30000 });
}

// ---- no room-code oracle in watching: an unknown code, a room with no Watch (an old game's) and an armed one answer a
// watcher's asks alike; a code new to the asker counts against the new-code limit (a join's: 20 a minute an address, an
// IPv6 one by its /64, and 200 its /48), one already watched asked again as often as it likes ----
{
	const unknown = 'QQQ222', old = await call('POST', '/v1/host', { ...me('OLDGAME'), watchable: undefined, pool: '', accepts: [] }, '198.51.100.210');
	const armed = await call('POST', '/v1/host', me('ARMED', { watchable: true }), '198.51.100.211');
	check('the three: unknown, an old game\'s room (no watch code), a new one\'s (armed)', [!!old.body.code && !('wid' in old.body), !!armed.body.wid], [true, true]);
	const ask = async (code, from) => {
		const s = await call('POST', `/v1/watch/seen?w=${code}&s=${(0x4000000000000000n + BigInt(Math.floor(Math.random() * 1e9))).toString(16)}`, undefined, from);
		const c = await call('GET', `/v1/watch/count?w=${code}`, undefined, from);
		const f = await call('GET', `/v1/watch/frames?w=${code}&m=00000000000000aa&from=0`, undefined, from);
		const st = await call('GET', `/v1/watch/start?w=${code}&m=00000000000000aa`, undefined, from);
		return JSON.stringify([s.status, s.body, c.status, c.body, f.status, f.body, st.status, st.body]);
	};
	const a1 = await ask(unknown, '198.51.100.212'), a2 = await ask(old.body.code, '198.51.100.213'), a3 = await ask(armed.body.wid, '198.51.100.214');
	check('unknown, unarmed and armed codes (no match on yet): the same answers to seen, count, frames and start', [a1 === a2, a2 === a3], [true, true]);
	const saved = { JOIN_LIMIT: env.JOIN_LIMIT, CODE48_LIMIT: env.CODE48_LIMIT }, used = {};
	for (const [k, n] of [['JOIN_LIMIT', 20], ['CODE48_LIMIT', 200]]) env[k] = { limit: async ({ key }) => ({ success: (used[k + key] = (used[k + key] || 0) + 1) <= n }) };   // (a minute's window, counted)
	const G = '203.0.113.220';
	const statuses = [];
	for (let i = 0; i < 25; i++) statuses.push((await call('POST', `/v1/watch/seen?w=${['QQQ' + 'ABCDEFGHJK'[i % 10] + '22', old.body.code, armed.body.wid][i % 3]}&s=${(0x5000000000000000n + BigInt(i)).toString(16)}`, undefined, G)).status);
	check('guessing codes on seen: 20 a minute from an address, then busy for unknown and armed alike', [statuses.slice(0, 20).every((x) => x === 200), statuses.slice(20).every((x) => x === 429)], [true, true]);
	const cs = [];
	for (let i = 0; i < 3; i++) cs.push((await call('GET', `/v1/watch/count?w=${['QQQ322', armed.body.wid, old.body.code][i]}`, undefined, G)).status);
	check('and on count', cs, [429, 429, 429]);
	const W2 = '203.0.113.221', mine = '6000000000000001';
	let first = await call('POST', `/v1/watch/seen?w=${armed.body.wid}&s=${mine}`, undefined, W2);
	for (let i = 0; i < 19; i++) await call('POST', `/v1/watch/seen?w=QQQ${'ABCDEFGHJKMNPQRSTUV'[i]}33&s=${mine}`, undefined, W2);   // (its address's 20 used up)
	const again = [];
	for (let i = 0; i < 50; i++) again.push((await call('POST', `/v1/watch/seen?w=${armed.body.wid}&s=${mine}`, undefined, W2)).status);
	check('a watcher asking again about the code it watches: never limited (50 more past its address\'s 20 new ones)', [first.status, again.every((x) => x === 200)], [200, true]);
	const C2 = '203.0.113.222', counts = [];
	for (let i = 0; i < 40; i++) counts.push((await call('GET', `/v1/watch/count?w=${armed.body.wid}`, undefined, C2)).status);
	check('the guest\'s count asked again: one new code, never limited', counts.every((x) => x === 200), true);
	const v6 = [];   // (one /48: its /64s 20 each, the /48 200 all told)
	for (let k = 0; k < 12; k++) for (let i = 0; i < 20; i++) v6.push((await call('POST', `/v1/watch/seen?w=QQQ${'ABCDEFGHJKMN'[k]}${'23456789ABCDEFGHJKMN'[i]}4&s=${(0x7000000000000000n + BigInt(k * 100 + i)).toString(16)}`, undefined, `2001:db8:77:${k.toString(16)}::5`)).status);
	check('an IPv6 /48: 20 a /64, 200 for all its /64s together', [v6.filter((x) => x === 200).length, v6.filter((x) => x === 429).length], [200, 40]);
	const j = await call('POST', '/v1/join', me('V6JOIN', { code: armed.body.code }), '2001:db8:77:ff::9');
	check('and the /48\'s joins then: busy too (a code guessed at by join)', j.status, 429);
	Object.assign(env, saved);
}

// ---- one address can't fill the rooms list: ROOMS_PER_ADDRESS (6) listed from it, its newest; others unaffected ----
{
	const FLOOD = '203.0.113.230', codes = [];
	for (let i = 0; i < 10; i++) {
		codes.push((await call('POST', '/v1/host', me('FLOOD' + i, { listed: true, persist: true, room: 'FREE STUFF ' + i }), FLOOD)).body.code);
		await new Promise((ok) => setTimeout(ok, 3));   // (each its own moment: the newest known)
	}
	const other = await call('POST', '/v1/host', me('NEIGHBOUR', { listed: true, room: 'REAL ROOM' }), '198.51.100.231');
	await settle();
	const listed = ((await online()).rooms || []).map((x) => x.code);
	check('one address\'s 10 listed rooms: its 6 newest listed, the 4 oldest not; another address\'s listed', [codes.filter((c) => listed.includes(c)).length, codes.slice(4).every((c) => listed.includes(c)), listed.includes(other.body.code)], [6, true, true]);
}

// ---- a room's host socket gone quiet (no ping for 90 s: its game gone without a close) closed, the room let go and
// delisted; one still pinging kept; the pings answered without waking the object, set as each object is made ----
{
	const { Room } = await import(pathToFileURL(resolve(process.argv[2])).href);
	let set = null;
	new Room({ setWebSocketAutoResponse: (p) => { set = p; }, storage: {}, waitUntil: () => {} }, env);
	check('a Room made (as after hibernating): its pings answered by Cloudflare from the start', set && [set.request, set.response], ['ping', 'pong']);
	const up = async (path, from) => worker.fetch(new Request('http://lobby' + path, { headers: { upgrade: 'websocket', 'cf-connecting-ip': from } }), env);
	const dead = await call('POST', '/v1/host', me('DEADHOST', { listed: true, persist: true, room: 'DEAD ROOM' }), '198.51.100.240');
	const live = await call('POST', '/v1/host', me('LIVEHOST', { listed: true, persist: true, room: 'LIVE ROOM' }), '198.51.100.241');
	const dws = await up(`/v1/room/ws?code=${dead.body.code}&key=${dead.body.key}`, '198.51.100.240'), lws = await up(`/v1/room/ws?code=${live.body.code}&key=${live.body.key}`, '198.51.100.241');
	dws.webSocket.accept(); lws.webSocket.accept();
	const old = Date.now() - 120000;
	for (const code of [dead.body.code, live.body.code]) { const o = env.ROOMS.objs.get(code); o.ctx.getWebSockets('host')[0].serializeAttachment({ at: old }); o.r.seen = old; }
	lws.webSocket.send('ping');   // (the live one pings; the dead one's game is gone)
	await settle();
	await env.ROOMS.objs.get(dead.body.code).alarm();
	await env.ROOMS.objs.get(live.body.code).alarm();
	await settle();
	const listed = ((await online()).rooms || []).map((x) => x.code);
	const pd = await call('POST', '/v1/peek', { code: dead.body.code }), pl = await call('POST', '/v1/peek', { code: live.body.code });
	check('no ping for 2 minutes: its socket closed, the room gone and delisted; the one pinging still there and listed', [dws.webSocket.open, pd.status, listed.includes(dead.body.code), lws.webSocket.open, pl.body.state, listed.includes(live.body.code)], [false, 404, false, true, 'open', true]);
}

// ---- the Stats kept an entry a key: no value near the 128 KiB a value may be, however many play; the month's ceiling
// kept through a restart; what was kept as one value before taken apart ----
{
	const { Stats } = await import(pathToFileURL(resolve(process.argv[2])).href);
	const { serialize } = await import('node:v8');
	const st = env.STATS.objs.get('all'), store = st.ctx.storage.__store;
	const tell = (ev) => st.fetch(new Request('https://stats/tell', { method: 'POST', body: JSON.stringify({ game: 'vf3', version: V, ...ev }) }));
	const sid = (i) => (0x1000000000000000n + BigInt(i)).toString(16);
	for (let i = 0; i < 5000; i++) await tell({ op: 'host', code: 'H' + i.toString(36).toUpperCase().padStart(5, '0'), sids: [sid(i)], countries: ['KR'], listed: true, name: 'HOSTNAME' + (i % 99), room: 'A LONG ROOM NAME', region: 'AS', aid: sid(i % 50) });
	const many = Array.from({ length: 2000 }, (_, i) => sid(100000 + i));
	await tell({ op: 'queue', key: 'qbig|' + V, sids: many, countries: many.map(() => 'JP'), events: [] });
	for (let b = 0; b < 20; b++) await tell({ op: 'queue', key: 'qev|' + V, sids: [], countries: [], events: Array.from({ length: 1000 }, (_, i) => ({ op: 'match', code: 'M' + (b * 1000 + i).toString(36).toUpperCase().padStart(5, '0'), sids: [sid(200000 + b * 1000 + i), sid(300000 + b * 1000 + i)], countries: ['US', 'FR'], t: Date.now() })) });
	await tell({ op: 'watch', wid: 'MNTH33', p: [['A', 'KR'], ['B', 'US']], wins: [0, 0], rounds: [0, 0], watchers: 3 });
	st.s.watches.MNTH33.seen -= 60000;
	await tell({ op: 'watch', wid: 'MNTH33', p: [['A', 'KR'], ['B', 'US']], wins: [0, 0], rounds: [0, 0], watchers: 3 });
	let biggest = 0;
	for (const v of store.values()) biggest = Math.max(biggest, serialize(v).length);
	check('5,000 hosts, a queue of 2,000, 20,000 matches: kept a key each, the largest value under 128 KiB (none as one value)', [biggest < 128 * 1024, store.has('s'), store.size > 25000], [true, false, true]);
	const month = st.s.month.ms, again = new Stats(st.ctx, env);
	await new Promise((ok) => setTimeout(ok, 0));
	check('a restart: the month\'s watched time kept, and the hosts, matches and queues', [again.s.month.ms === month && month > 0, Object.keys(again.s.hosts).length, again.s.matches.length >= 20000, again.s.queues['qbig|' + V].sids.length], [true, Object.keys(st.s.hosts).length, true, 2000]);
	const ctx2 = { storage: { ...st.ctx.storage } }, legacy = new Map([['s', { queues: {}, hosts: { OLDONE: { game: 'vf3', version: V, sid: sid(1), country: 'KR', seen: Date.now(), born: Date.now() } }, matches: [], watches: {}, month: { key: new Date().toISOString().slice(0, 7), ms: 12345 } }]]);
	const fake = { get: async (k) => (Array.isArray(k) ? new Map() : legacy.get(k)), put: async (k, v) => { if (typeof k === 'object') for (const [a, b] of Object.entries(k)) legacy.set(a, b); else legacy.set(k, v); }, delete: async (k) => { for (const x of [].concat(k)) legacy.delete(x); }, list: async () => new Map(legacy) };
	const mig = new Stats({ ...ctx2, storage: fake, blockConcurrencyWhile: (f) => f(), waitUntil: () => {} }, env);
	await new Promise((ok) => setTimeout(ok, 0));
	await mig.fetch(new Request('https://stats/tell', { method: 'POST', body: JSON.stringify({ op: 'gone', game: 'vf3', version: V, code: 'NOSUCH' }) }));
	check('kept as one value before: taken apart at the next write, the month kept', [legacy.has('s'), legacy.has('h:OLDONE'), legacy.get('month') && legacy.get('month').ms], [false, true, 12345]);
	for (const k of Object.keys(st.s.hosts)) if (/^H/.test(k)) await tell({ op: 'gone', code: k });
	await tell({ op: 'queue', key: 'qbig|' + V, sids: [], countries: [], events: [] });
	st.drop_matches((m) => /^M/.test(m.code)); await st.flush();
}

// ---- an old game's pages: the counts as they were (the rooms a field more, which they don't read) ----
check('online: the counts\' shape unchanged', Object.keys(d.games.vf3).sort(), ['countries', 'hosting', 'older', 'queued', 'random', 'room', 'searching', 'total', 'watching']);
console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
