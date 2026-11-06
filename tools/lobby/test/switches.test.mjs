// The lobby's off switches (SWITCH_ROOMS, SWITCH_LISTING, SWITCH_WATCH, SWITCH_WEBSOCKETS: Worker variables, "off"),
// every combination of them: room codes and Quick match work as ever in each, and what's off says so plainly.
//   cd tools/lobby && npm test   (node test/switches.test.mjs src/index.js)
import { lobby, settle, queued } from './standin.mjs';
const { worker, env } = await lobby(process.argv[2]);
let fails = 0, checks = 0;
function check(what, got, want, quiet) {
	const ok = JSON.stringify(got) === JSON.stringify(want);
	checks++;
	if (!ok) fails++;
	if (!ok || !quiet) console.log((ok ? 'ok   ' : 'FAIL ') + what + ': ' + JSON.stringify(got) + (ok ? '' : ' (wanted ' + JSON.stringify(want) + ')'));
}
let ip = 10;
async function call(method, path, body, from = '198.51.100.' + ip) {
	const init = { method, headers: { 'cf-connecting-ip': from, 'x-test-country': 'KR' } };
	if (body !== undefined) init.body = body instanceof Uint8Array ? body : JSON.stringify(body);
	const req = new Request('http://lobby' + path, init);
	Object.defineProperty(req, 'cf', { value: { latitude: '37.5', longitude: '127.0', continent: 'AS' } });
	const r = await worker.fetch(req, env);
	const type = r.headers.get('content-type') || '';
	return { status: r.status, body: type.includes('json') ? await r.json() : null };
}
const upgrade = (path) => worker.fetch(new Request('http://lobby' + path, { headers: { upgrade: 'websocket', 'cf-connecting-ip': '198.51.100.' + ip } }), env);
const tick = settle;   // (everything the objects left running, done: no sleeping on it)
const C = '0123456789abcdef', V = 'v0.1.7-online-preview+host1';
const me = (name, extra = {}) => ({ game: 'vf3', version: V, content: C, name, cands: ['203.0.113.7:7700'], relay: '', pool: V, accepts: ['stage'], ...extra });
const old_game = (name, extra = {}) => ({ game: 'vf3', version: 'v0.1.7-online-preview', content: C, name, cands: ['203.0.113.8:7700'], relay: '', ...extra });
// (a push's bytes, as src/net/watch.cpp makes them: no frames, its info)
function push_bytes(match) {
	const b = new Uint8Array(22 + 2 + 5), v = new DataView(b.buffer);
	v.setUint32(0, 0x31574656, true); b[4] = 2; b[5] = 1;
	v.setUint32(6, Number(match & 0xffffffffn), true); v.setUint32(10, Number(match >> 32n), true);
	return b;
}
function start_bytes(match) {
	const m = new TextEncoder().encode(JSON.stringify({ p1: 'A', p2: 'B' }));
	const b = new Uint8Array(20 + m.length + 4), v = new DataView(b.buffer);
	v.setUint32(0, 0x31574656, true); b[4] = 1; b[5] = 1;
	v.setUint32(6, Number(match & 0xffffffffn), true); v.setUint32(10, Number(match >> 32n), true);
	v.setUint16(14, m.length, true); b.set(m, 16); v.setUint32(16 + m.length, 4, true);
	return b;
}
const NAMES = ['SWITCH_ROOMS', 'SWITCH_LISTING', 'SWITCH_WATCH', 'SWITCH_WEBSOCKETS'];
for (let mask = 0; mask < 16; mask++) {
	ip++;
	const off = {};
	NAMES.forEach((k, i) => { off[k] = !!(mask & (1 << i)); if (off[k]) env[k] = 'off'; else delete env[k]; });
	const label = NAMES.filter((k) => off[k]).map((k) => k.slice(7).toLowerCase()).join('+') || 'all on';
	const rooms = !off.SWITCH_ROOMS, listing = rooms && !off.SWITCH_LISTING, watch = !off.SWITCH_WATCH, ws = !off.SWITCH_WEBSOCKETS;
	const q = mask !== 0 && mask !== 15;   // (all on and all off said line by line; the rest only if one fails)
	// a room code, as every game has made one: hosted, joined, waited for (an old game's and a new one's)
	const h = await call('POST', '/v1/host', old_game('OLDHOST'));
	const w = call('POST', '/v1/wait', { code: h.body.code, key: h.body.key });
	await tick();
	const j = await call('POST', '/v1/join', old_game('OLDGUEST', { code: h.body.code }), '198.51.200.' + ip);
	const wr = await w;
	check(`${label}: an old game's room code: hosted, joined, its guest to the host`, [h.status, Object.keys(h.body).sort(), j.status, !!j.body.token, wr.body.guest && wr.body.guest.name], [200, ['code', 'key', 'token'], 200, true, 'OLDGUEST'], q);
	// a new game's Create room: listed, a room that stays, watchable; what it got back says which are on
	const n = await call('POST', '/v1/host', me('NEWHOST', { listed: true, persist: true, room: 'my room', watchable: true }));
	check(`${label}: Create room: a code; stays only with rooms on, listed only with listing, watchable only with watching`,
		[n.status, n.body.code && n.body.code.length, !!n.body.persist, !!n.body.listed_as, !!n.body.wid], [200, 6, rooms, listing, watch], q);
	const sock = await upgrade(`/v1/room/ws?code=${n.body.code}&key=${n.body.key}`);
	check(`${label}: the room's WebSocket: taken, or refused for the HTTPS wait`, sock.status, ws ? 101 : 503, q);
	let pushed = null;
	if (sock.webSocket) { sock.webSocket.accept(); sock.webSocket.addEventListener('message', (e) => { if (e.data !== 'pong') pushed = JSON.parse(e.data); }); }
	const nj = await call('POST', '/v1/join', me('NEWGUEST', { code: n.body.code, queue: true }), '198.51.201.' + ip);
	await tick();
	const nw = await call('POST', '/v1/wait', { code: n.body.code, key: n.body.key });
	check(`${label}: its player joins; the host told (on its socket, or its wait)`, [nj.status, (pushed || nw.body).guest && (pushed || nw.body).guest.name], [200, 'NEWGUEST'], q);
	const second = await call('POST', '/v1/join', me('THIRD', { code: n.body.code, queue: true }), '198.51.202.' + ip);
	check(`${label}: a third: in the line if the room stays, else full as ever`, [second.status, second.body.error, 'queued' in second.body], [409, 'full', rooms], q);
	// the rooms list: there with rooms and listing on, the playing ones only with watching too; what's on, said
	const d = (await call('GET', '/v1/online')).body;
	check(`${label}: /v1/online says what's on; the rooms list only with it`, [d.features, 'rooms' in d, !!d.games], [{ rooms: listing, watch }, listing, true], q);
	if (listing) check(`${label}: the list's matches only with watching on`, (d.rooms || []).some((x) => x.state === 'playing'), watch, q);
	// watching: a match sent and watched only with watching on; otherwise told off, plainly
	const peek = await call('POST', '/v1/peek', { code: n.body.code });
	check(`${label}: Enter code on a match: watchable only with watching`, [peek.status, peek.body.state, peek.body.watch], [200, 'playing', watch], q);
	const seen = await call('POST', `/v1/watch/seen?w=${n.body.code}&s=00000000000000aa`);
	check(`${label}: a watcher: counted, or told off`, [seen.status, seen.body.error || null], watch ? [200, null] : [503, 'off'], q);
	// Quick match: two matched, as ever; watchable only with watching
	const qa = await call('POST', '/v1/random', me('QA', { watchable: true, watch: true }), '198.51.203.' + ip);
	const qwait = call('POST', '/v1/random/wait', me('QA', { watchable: true, watch: true, ticket: qa.body.ticket }), '198.51.203.' + ip);
	await queued(env, qa.body.ticket);
	const qb = await call('POST', '/v1/random', me('QB', { watchable: true, watch: true }), '198.51.204.' + ip);
	const qw = await qwait;
	const qhost = [qw.body, qb.body].find((x) => x.role === 'host');
	check(`${label}: Quick match: two matched, a host and a guest; a watch code only with watching`, [!!qhost, [qw.body.role, qb.body.role].sort(), !!(qhost && qhost.wid)], [true, ['guest', 'host'], watch], q);
	await call('POST', '/v1/close', { code: n.body.code, key: n.body.key });
	await call('POST', '/v1/close', { code: h.body.code, key: h.body.key });
}
NAMES.forEach((k) => delete env[k]);

// switched off while a match is sent and watched: the host told off at its next upload, its watcher's socket told and
// ended; switched back on, the next match sent and watched as before
{
	ip++;
	const h = await call('POST', '/v1/host', me('LIVEHOST', { watchable: true }));
	const W = h.body.wid, K = h.body.wkey, M = 0x4242n;
	await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M));
	await call('POST', `/v1/watch/seen?w=${W}&s=00000000000000bb`);
	const wsr = await upgrade(`/v1/watch/ws?w=${W}&m=0000000000004242&from=0&s=00000000000000bb`);
	const got = [];
	let closed = false;
	wsr.webSocket.accept();
	wsr.webSocket.addEventListener('message', (e) => { if (typeof e.data === 'string') got.push(JSON.parse(e.data)); });
	wsr.webSocket.addEventListener('close', () => { closed = true; });
	env.SWITCH_WATCH = 'off';
	const p = await call('POST', `/v1/watch/push?w=${W}&k=${K}`, push_bytes(M));
	await tick();
	check('watching switched off mid-match: the host told off, its watcher told and its socket ended', [p.body.off, got.some((x) => x.error === 'off'), closed], [true, true, true]);
	delete env.SWITCH_WATCH;
	const p2 = await call('POST', `/v1/watch/start?w=${W}&k=${K}`, start_bytes(M + 1n));
	check('switched back on: the next match taken', p2.body, { ok: true });
	// WebSockets switched off: a host's socket closed at its next message (it sends over HTTPS from then)
	const feed = await upgrade(`/v1/watch/feed?w=${W}&k=${K}`);
	let fclosed = false;
	feed.webSocket.accept();
	feed.webSocket.addEventListener('close', () => { fclosed = true; });
	env.SWITCH_WEBSOCKETS = 'off';
	feed.webSocket.send(push_bytes(M + 1n));
	await tick();
	check('WebSockets switched off: the host\'s socket closed at its next message, a new one refused', [fclosed, (await upgrade(`/v1/watch/feed?w=${W}&k=${K}`)).status], [true, 503]);
	delete env.SWITCH_WEBSOCKETS;
}
// a value other than "off" leaves a switch on (a typo can't turn anything off)
{
	env.SWITCH_ROOMS = 'yes';
	check('SWITCH_ROOMS "yes": on', (await call('GET', '/v1/online')).body.features.rooms, true);
	env.SWITCH_ROOMS = ' OFF ';
	check('SWITCH_ROOMS " OFF ": off', (await call('GET', '/v1/online')).body.features.rooms, false);
	delete env.SWITCH_ROOMS;
}
console.log(fails ? fails + ' FAILED' : `all passed (${checks} checks, the 16 combinations of the switches)`);
process.exit(fails ? 1 : 0);
