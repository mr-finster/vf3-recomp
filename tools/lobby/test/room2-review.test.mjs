// Rooms 2 before it goes live: the review's findings (17 Nov), a test each, each failing on the code before its fix.
//   1 a restart (a deploy, a variable changed) let every member go whose last call was 30 s ago (sockets gone, no close)
//   2 a member's flood (READY / AWAY toggled): a write, an alarm, a push to every member and a telling of the count each
//   3 the rooms list with Rooms 2 off while a Rooms 2 room still reported: every /v1 match ("playing") left out
//   4 a member's calls told a room's code from a made-up one (not_member / not_owner against no_room): codes guessed past
//     the code-guess limits
//   5 a member back after a crash with a version its opponent can't play: the table stuck in DECIDED, an alarm every 100 ms
//   6 entering a table whose one seated player can't play the newcomer: 500
//   7 SWITCH_ROOMS2 off didn't end the rooms open (their members polled into "off", not an end)
//   8 PLAYING forever: a session lost twice (held) and nobody READY in line, or a pair that never reports; duplicate
//     started and result (the games will say them again till heard) harmless
//   9 the design's "the 2nd time to the back" for a newcomer that doesn't start; a holder blaming every challenger
//  11 Rooms 2 rows pushing an older build's rows off the list (ROOMS_MAX), and one address's Rooms 2 rooms its /v1 rows
//   cd tools/lobby && node test/room2-review.test.mjs src/index.js   (in npm test)
import { lobby, settle } from './standin.mjs';
const { worker, env } = await lobby(process.argv[2]);
const mod = await import(new URL('../' + (process.argv[2] || 'src/index.js'), import.meta.url).href);
env.SWITCH_ROOMS2 = 'on';
const real_now = Date.now;
let skew = 0, frozen = null;
Date.now = () => (frozen ?? real_now()) + skew;
const advance = (ms) => { skew += ms; };
const freeze = () => { frozen = real_now(); }, thaw = () => { skew += frozen - real_now(); frozen = null; };   // (the clock stopped: what's "at once" is, on any machine)
let fails = 0, checks = 0;
function check(what, got, want) {
	const ok = JSON.stringify(got) === JSON.stringify(want);
	checks++;
	if (!ok) fails++;
	console.log((ok ? 'ok   ' : 'FAIL ') + what + ': ' + JSON.stringify(got) + (ok ? '' : ' (wanted ' + JSON.stringify(want) + ')'));
}
let ipn = 1;
const next_ip = () => '198.18.' + (ipn >> 8) + '.' + (ipn++ & 255);
async function call(method, path, body, from = '198.51.100.1') {
	const init = { method, headers: { 'cf-connecting-ip': from, 'x-test-country': 'KR' } };
	if (body !== undefined) init.body = JSON.stringify(body);
	const req = new Request('http://lobby' + path, init);
	Object.defineProperty(req, 'cf', { value: { latitude: '37.5', longitude: '127.0', continent: 'AS' } });
	const r = await worker.fetch(req, env);
	const t = await r.text();
	let j = null;
	try { j = JSON.parse(t); } catch (e) {}
	await settle();
	return { status: r.status, body: j };
}
const C = '0123456789abcdef', V = 'v0.1.7-online-preview+host1';
const me = (name, extra = {}) => ({ game: 'vf3', version: V, content: C, name, cands: ['203.0.113.' + (ipn % 200 + 1) + ':7700'], relay: '', pool: V, accepts: ['stage'], ...extra });
const obj = (code) => env.ROOMS.objs.get(code);
const rec = (code) => (obj(code) ? obj(code).r : null);
async function socket_for(m) {
	const r = await worker.fetch(new Request(`http://lobby/v2/room/ws?code=${m.code}&mid=${m.mid}&mkey=${m.mkey}`, { headers: { upgrade: 'websocket', 'cf-connecting-ip': m.ip } }), env);
	if (r.status !== 101) return r.status;
	m.ws = r.webSocket;
	m.ws.accept();
	m.said = [];
	m.closed = null;
	m.ws.addEventListener('message', (e) => { if (e.data === 'pong') return; const d = JSON.parse(e.data); m.said.push(d); if (d.state) m.state = d.state; });
	m.ws.addEventListener('close', (e) => { m.closed = e.code; });
	await settle();
	return 101;
}
async function create(name, extra = {}, sock = true) {
	const ip = next_ip();
	const r = await call('POST', '/v2/room/create', me(name, extra), ip);
	if (r.status !== 200) return { status: r.status, body: r.body };
	const m = { name, ip, code: r.body.code, mid: r.body.mid, mkey: r.body.mkey, owner_key: r.body.owner_key, state: r.body.state, status: 200 };
	if (sock) await socket_for(m);
	return m;
}
async function enter(code, name, extra = {}, sock = true) {
	const ip = next_ip();
	const r = await call('POST', '/v2/room/enter', { ...me(name, extra), code }, ip);
	if (r.status !== 200) return { status: r.status, body: r.body };
	const m = { name, ip, code, mid: r.body.mid, mkey: r.body.mkey, state: r.body.state, status: 200 };
	if (sock) await socket_for(m);
	return m;
}
const act = (m, op, extra = {}) => call('POST', '/v2/room/' + op, { code: m.code, mid: m.mid, mkey: m.mkey, ...extra }, m.ip);
const alarm = async (code) => { if (obj(code)) await obj(code).alarm(); await settle(); };
const alarm_in = async (code) => { const a = await obj(code).ctx.storage.getAlarm(); return a == null ? null : a - Date.now(); };
function show(code) {
	const r = rec(code);
	if (!r) return 'GONE';
	const n = (mid) => (mid && r.members[mid] ? r.members[mid].who.name + (r.members[mid].away ? '*' : '') : '-');
	return `${r.t.phase} ${n(r.t.a)}/${n(r.t.b)} [${r.line.map(n).join(',')}] s${r.t.streak}` + (r.ready ? ' r:' + n(r.ready.mid) : '');
}
const said = (res) => (res.status === 200 ? (res.body && res.body.ok === false ? res.body.error : 'ok') : res.status + ' ' + (res.body && res.body.error));
// (the online count's tellings, counted)
let tells = 0;
{
	const get = env.STATS.get.bind(env.STATS);
	env.STATS.get = (id) => { const s = get(id); return { fetch: (u, i) => { if (String(u).includes('/tell')) tells++; return s.fetch(u, i); } }; };
}
const stats = () => env.STATS.objs.get('all');
// (an object made anew on the same storage, as Cloudflare does after hibernating or a restart; restart: its sockets gone
// with it, no close delivered)
function remake(code, members, restart) {
	const o = obj(code);
	if (restart) for (const m of members) if (m.ws) { m.ws.open = false; m.ws.peer.open = false; }
	const fresh = new mod.Room(o.ctx, env);
	env.ROOMS.objs.set(code, fresh);
	for (const m of members) if (m.ws && m.ws.peer.owner) m.ws.peer.owner.obj = fresh;   // (its sockets' messages to the new one)
	return fresh;
}

// ---- 1: a restart: every member given 30 s to come back; a hibernated object woken as it was ----
{
	const A = await create('AAA'), ms = [A];
	for (const n of ['BBB', 'CCC', 'DDD', 'EEE']) ms.push(await enter(A.code, n));
	for (const m of ms.slice(0, 2)) await act(m, 'started');
	advance(45000);   // (45 s with only pings, answered without waking the object)
	remake(A.code, ms, true);
	check('1 restart: the first game back on its socket: nobody let go, the match on', [await socket_for(ms[2]), show(A.code), Object.keys(rec(A.code).members).length], [101, 'PLAYING AAA/BBB [CCC,DDD,EEE] s0', 5]);
	const back = await call('POST', '/v2/room/enter', { ...me('DDD'), code: A.code, mid: ms[3].mid, mkey: ms[3].mkey }, ms[3].ip);
	check('1 restart: another back by its membership: its place kept', [back.status, back.body.back, back.body.state && back.body.state.you.pos], [200, true, 2]);
	await socket_for(ms[3]);
	advance(31000); await alarm(A.code);
	const seated = [rec(A.code).t.a, rec(A.code).t.b].map((x) => (x && rec(A.code).members[x] ? rec(A.code).members[x].who.name : '-')).sort();   // (which side's whose: by the order the three went)
	check('1 restart: those not back in 30 s: gone then (the match forfeited; the two back play)', [rec(A.code).t.phase, seated, Object.keys(rec(A.code).members).length], ['CONNECTING', ['CCC', 'DDD'], 2]);
}
{   // (a restart with the alarm first: the same)
	const A = await create('AAA'), ms = [A];
	for (const n of ['BBB', 'CCC']) ms.push(await enter(A.code, n));
	for (const m of ms.slice(0, 2)) await act(m, 'started');
	advance(45000);
	remake(A.code, ms, true);
	await alarm(A.code);
	check('1 restart, the alarm first: nobody let go', [show(A.code), Object.keys(rec(A.code).members).length], ['PLAYING AAA/BBB [CCC] s0', 3]);
	check('1 restart: kept so (a second restart before anyone\'s back gives no more time)', Object.values(obj(A.code).ctx.storage.__store.get('r').members).every((m) => m.ws === false), true);
}
{   // (hibernated: its sockets still there; a member polling, quiet 31 s, is let go as ever; those on sockets stay)
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), P = await enter(A.code, 'POL', {}, false);
	for (const m of [A, B]) await act(m, 'started');
	advance(31000);
	remake(A.code, [A, B], false);
	await alarm(A.code);
	check('1 hibernated, woken: sockets kept, the quiet poller let go, the others there', [show(A.code), said(await act(P, 'poll'))], ['PLAYING AAA/BBB [] s0', '410 removed']);
	B.ws.send(JSON.stringify({ op: 'result', winner: 'a', n: 1 })); await settle();
	check('1 hibernated, woken: its sockets\' messages taken', show(A.code), 'DECIDED AAA/BBB [] s0');
}
{   // (a socket closed, then the object hibernated: its member's 30 s from its close, not from the wake)
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), Cm = await enter(A.code, 'CCC');
	for (const m of [A, B]) await act(m, 'started');
	advance(5000);
	Cm.ws.close(1000); await settle();
	advance(20000);
	remake(A.code, [A, B], false);
	advance(11000); await alarm(A.code);
	check('1 a socket closed, then hibernated: gone 30 s after its close', [show(A.code), said(await act(Cm, 'poll'))], ['PLAYING AAA/BBB [] s0', '410 removed']);
}

// ---- 2: a member's flood: the count told at most once a second (the last always), the writes and pushes bounded ----
freeze();
{
	const A = await create('AAA'), ms = [A];
	for (let i = 0; i < 6; i++) ms.push(await enter(A.code, 'P' + i));
	advance(2000);
	const st = obj(A.code).ctx.storage, put = st.put;
	let puts = 0;
	st.put = (...a) => { puts++; return put(...a); };
	tells = 0;
	const F = ms[ms.length - 1], heard = ms[2].said.length;
	for (let i = 0; i < 1000; i++) F.ws.send(i % 2 ? 'ready' : 'away');
	await settle();
	check('2 flood: 1,000 messages from one member: writes, pushes to another member and tellings bounded', [puts <= 12, ms[2].said.length - heard <= 12, tells <= 1], [true, true, true]);
	check('2 flood: its socket closed for it (busy), still a member', [F.closed, !!rec(A.code).members[F.mid]], [1008, true]);
	const away = rec(A.code).members[F.mid].away;
	advance(10000); await alarm(A.code);
	const row = stats().s.hosts[A.code];
	check('2 flood: the last state told (10 s on, by the alarm)', [row.ready, tells >= 1], [away ? 4 : 5, true]);
	st.put = put;
}
{   // (a change told at once; changes within 10 s told by the alarm, as one; one that changes it back, not told)
	const A = await create('TTT', { listed: true }), B = await enter(A.code, 'UUU');
	advance(10000);
	tells = 0;
	await act(B, 'away');   // (SOLO, B AWAY in line: told at once)
	const first = tells;
	await act(B, 'ready');   // (B seated again)
	const Cm = await enter(A.code, 'VVV');   // (and one more in line)
	check('2 throttle: the first change told at once, the next two within 10 s not yet', [first, tells], [1, 1]);
	check('2 throttle: the alarm set for the 10 s\' end', (await alarm_in(A.code)) <= 10000, true);
	advance(10000); await alarm(A.code);
	check('2 throttle: then told once, as it is', [tells, stats().s.hosts[A.code].phase, stats().s.hosts[A.code].line], [2, 'CONNECTING', 1]);
	await act(Cm, 'away'); await act(Cm, 'ready');   // (AWAY and back to READY within the 10 s: as told)
	advance(10000); await alarm(A.code);
	check('2 throttle: changed and changed back: nothing new told', tells, 2);
	await act(A, 'started'); await act(B, 'started');
	advance(10000); await alarm(A.code);
	check('2 throttle: a change after it: told', [tells, stats().s.hosts[A.code].phase], [3, 'PLAYING']);
	advance(25000); for (const m of [A, B, Cm]) m.ws.send(JSON.stringify({ op: 'poll' })); await settle();
	advance(25000); await alarm(A.code);
	check('2 throttle: nothing changed, still told it\'s there every 50 s', tells, 4);
}
{   // (HTTPS: a member's calls past its rate answered busy; polls that change nothing not written each)
	const A = await create('HHH', {}, false);
	advance(20000);
	const st = obj(A.code).ctx.storage, put = st.put;
	let puts = 0;
	st.put = (...a) => { puts++; return put(...a); };
	await act(A, 'poll');
	const before = puts;
	for (let i = 0; i < 5; i++) await act(A, 'poll');
	check('2 polls: five more that change nothing in a second: no write each', puts - before, 0);
	const many = [];
	for (let i = 0; i < 20; i++) many.push(said(await act(A, 'poll')));
	check('2 rate: a member\'s calls past 10 at once: busy', [many.filter((x) => x === 'ok').length <= 10, many.includes('429 busy')], [true, true]);
	advance(4000);
	check('2 rate: a few seconds on, answered again', said(await act(A, 'poll')), 'ok');
	st.put = put;
}

thaw();

// ---- 3: the rooms list with Rooms 2 off, a Rooms 2 room still reporting: /v1's rows as they are ----
{
	const st = env.STATS.get(env.STATS.idFromName('all'));
	await st.fetch('https://stats/tell', { method: 'POST', body: JSON.stringify({ op: 'watch', wid: 'WTCHAB', game: 'vf3', version: V, listed: true, p: [['P1', 'KR'], ['P2', 'JP']], wins: [1, 0], rounds: [1, 1], watchers: 3, aid: '' }) });
	const rows = async () => ((await call('GET', '/v1/online')).body.rooms || []).filter((x) => x.v === 2 || x.watch === 'WTCHAB').map((x) => (x.v === 2 ? 'v2' : x.state + ':' + x.watch));
	const A = await create('LST', { listed: true });
	const on = await rows();
	env.SWITCH_ROOMS2 = 'off';
	const off = await rows();
	env.SWITCH_WATCH = 'off';
	const off_nowatch = await rows();
	delete env.SWITCH_WATCH;
	env.SWITCH_ROOMS2 = 'on';
	check('3 list: Rooms 2 on: both', [on.includes('v2'), on.includes('playing:WTCHAB')], [true, true]);
	check('3 list: Rooms 2 off, a room of it still reporting: no Rooms 2 row, the /v1 match as it was', off, ['playing:WTCHAB']);
	check('3 list: watching off as well: its matches left out (as ever)', off_nowatch, []);
	await act(A, 'leave');
}

// ---- 4: a member's calls on a room's code and on a made-up one answer alike ----
{
	const A = await create('OWN'), B = await enter(A.code, 'MEM');
	const fake = { mid: '0a0b0c0d', mkey: 'f'.repeat(32) };
	const pair = async (path, body) => [said(await call('POST', path, { code: A.code, ...body })), said(await call('POST', path, { code: 'ZZZZZZ', ...body }))];
	for (const op of ['poll', 'ready', 'away', 'started', 'result', 'leave', 'leave_line', 'join_line'])
		check('4 guess: ' + op + ' by a non-member: the same as a code with no room', await pair('/v2/room/' + op, { ...fake, winner: 'a' }), ['404 no_room', '404 no_room']);
	for (const op of ['settings', 'close']) check('4 guess: ' + op + ' with a wrong owner key: the same as no room', await pair('/v2/room/' + op, { owner_key: 'e'.repeat(32) }), ['404 no_room', '404 no_room']);
	const ws = await worker.fetch(new Request(`http://lobby/v2/room/ws?code=${A.code}&mid=${fake.mid}&mkey=${fake.mkey}`, { headers: { upgrade: 'websocket' } }), env);
	check('4 guess: the socket by a non-member: no room', [ws.status, (await ws.json()).error], [404, 'no_room']);
	check('4 guess: settings by a member that isn\'t the owner: not_owner (to it alone)', said(await act(B, 'settings', { owner_key: 'e'.repeat(32) })), '403 not_owner');
	await act(B, 'leave');
	check('4 guess: a member let go, by its own key: told why, as before', said(await act(B, 'poll')), '410 removed');
}

// ---- 5: back after a crash with a version the opponent can't play; a seat that fails in a result: the table moves on ----
{
	const A = await create('AAA', { version: 'X', pool: '', accepts: [] });
	const B = await enter(A.code, 'BBB', { version: 'X+f', pool: 'P', accepts: [] });
	await act(A, 'leave');
	const Cm = await enter(A.code, 'CCC', { version: 'X+f', pool: 'P', accepts: [] });
	await act(B, 'started'); await act(Cm, 'started');
	const back = await call('POST', '/v2/room/enter', { ...me('CCC', { version: 'X+g', pool: 'Q', accepts: [] }), code: A.code, mid: Cm.mid, mkey: Cm.mkey }, Cm.ip);
	check('5 rejoin, another version: refused naming the opponent\'s; out, its match forfeited, the other stays', [back.status, back.body.error, back.body.host_version, show(A.code)], [412, 'version', 'X+f', 'SOLO -/BBB [] s0']);
	advance(2000); await alarm(A.code);   // (its last change told)
	tells = 0;
	const at = await alarm_in(A.code);
	for (let i = 0; i < 5; i++) { advance(100); await alarm(A.code); }
	check('5 no alarm storm: the next alarm seconds away, the count not told at every alarm', [at >= 1000, tells], [true, 0]);
}
{   // (a seat failing where a result seats the two again (a desync): void, the newcomer unseated, the table on)
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), D = await enter(A.code, 'DDD');
	await act(A, 'started'); await act(B, 'started');
	rec(A.code).members[B.mid].who.version = 'Y';   // (B's terms with A gone (as a record from a game back with another version))
	rec(A.code).members[B.mid].who.pool = '';
	await act(A, 'result', { winner: 'desync' });
	const r = await act(B, 'result', { winner: 'desync' });
	check('5 desync, the two can\'t be seated again: void, the newcomer back to the front AWAY, the next seated', [said(r), show(A.code), (rec(A.code) && rec(A.code).t.last || {}).why], ['ok', 'CONNECTING AAA/DDD [BBB*] s0', 'version']);
	advance(11000); await alarm(A.code);   // (past the first result's 10 s)
	tells = 0;
	const at = await alarm_in(A.code);
	for (let i = 0; i < 5; i++) { advance(100); await alarm(A.code); }
	check('5 nothing past due left: the next alarm not at once, nothing told at every alarm', [at >= 1000, tells, show(A.code)], [true, 0, 'CONNECTING AAA/DDD [BBB*] s0']);
}

// ---- 6: entering a table whose one seated player can't play the newcomer: told so, not a 500 ----
{
	const A = await create('AAA', { version: 'X', pool: '', accepts: [] });
	const B = await enter(A.code, 'BBB', { version: 'X+f', pool: 'P', accepts: [] });
	await act(A, 'leave');
	const r = await call('POST', '/v2/room/enter', { ...me('CCC', { version: 'X+g', pool: 'Q', accepts: [] }), code: A.code }, next_ip());
	check('6 enter: the one at the table can\'t play it: 412 version naming theirs, the room as it was', [r.status, r.body.error, r.body.host_version, show(A.code)], [412, 'version', 'X+f', 'SOLO -/BBB [] s0']);
}

// ---- 7: switched off: rooms open end (members told closed, the count told they're gone); /v1 as it was ----
{
	const A = await create('AAA', { listed: true }), B = await enter(A.code, 'BBB'), P = await enter(A.code, 'POL', {}, false);
	const v1 = await call('POST', '/v1/host', { ...me('V1H'), listed: true, persist: true }, '198.51.77.1');
	const v1rec = JSON.stringify(rec(v1.body.code));
	obj(A.code).told2 = 0; await alarm(A.code);
	env.SWITCH_ROOMS2 = 'off';
	check('7 off: create and enter: off (a new game makes a /v1 room)', [said(await call('POST', '/v2/room/create', me('NEW'), next_ip())), said(await call('POST', '/v2/room/enter', { ...me('NEW'), code: A.code }, next_ip()))], ['404 off', '404 off']);
	check('7 off: a member polling: no_room (its game ends the room)', said(await act(P, 'poll')), '404 no_room');
	check('7 off: the room closed, its members on sockets told closed, the count told it\'s gone', [show(A.code), [A, B].every((m) => m.said.some((d) => d.closed === true) && m.closed === 1000), A.code in stats().s.hosts], ['GONE', true, false]);
	const pv1 = await call('POST', '/v2/room/poll', { code: v1.body.code, mid: 'x', mkey: 'y' });
	check('7 off: a member\'s call on a /v1 room\'s code: no_room, its object left as it was', [said(pv1), JSON.stringify(rec(v1.body.code)) === v1rec], ['404 no_room', true]);
	const j = await call('POST', '/v1/join', { ...me('V1G'), code: v1.body.code }, '198.51.77.2');
	check('7 off: /v1 rooms as ever (a join)', [j.status, j.body.name], [200, 'V1H']);
	env.SWITCH_ROOMS2 = 'on';
	// (by its alarm, and by a socket's message)
	const E = await create('EEE'), F = await enter(E.code, 'FFF');
	env.SWITCH_ROOMS2 = 'off';
	await alarm(E.code);
	check('7 off: a room\'s next alarm closes it', [show(E.code), F.said.some((d) => d.closed === true)], ['GONE', true]);
	env.SWITCH_ROOMS2 = 'on';
	const G = await create('GGG'), H = await enter(G.code, 'HHH');
	env.SWITCH_ROOMS2 = 'off';
	H.ws.send('ready'); await settle();
	check('7 off: a member\'s socket message closes it', [show(G.code), G.said.some((d) => d.closed === true)], ['GONE', true]);
	env.SWITCH_ROOMS2 = 'on';
	check('7 on again: a new room as before', said(await call('POST', '/v2/room/create', me('NEW'), next_ip())), 'ok');
}

// ---- 8: PLAYING not forever; started and result said again: the same ----
{   // (a session lost twice, both alive (held), nobody in line: they play on with the same token; neither starts it: void)
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'disconnect', n: 1 }); await act(B, 'result', { winner: 'disconnect', n: 1 });
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'disconnect', n: 2 }); await act(B, 'result', { winner: 'disconnect', n: 2 });
	check('8 held: the two play on, their session to start again', [show(A.code), rec(A.code).t.restart, (await alarm_in(A.code)) <= 30000], ['PLAYING AAA/BBB [] s0', true, true]);
	advance(31000); await alarm(A.code);
	check('8 held, neither started in 30 s: void as a new pair (the newcomer to the front, AWAY)', [show(A.code), (rec(A.code) && rec(A.code).t.last || {}).why], ['SOLO AAA/- [BBB*] s0', 'connect']);
}
{   // (held, then both start it: on, with PLAYING's own limit)
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	for (const n of [1, 2]) { await act(A, 'result', { winner: 'disconnect', n }); await act(B, 'result', { winner: 'disconnect', n }); if (n === 1) { await act(A, 'started'); await act(B, 'started'); } }
	await act(A, 'started', { n: 3 }); await act(B, 'started', { n: 3 });
	advance(31000); await alarm(A.code);
	check('8 held, both started again: the match on past 30 s', show(A.code), 'PLAYING AAA/BBB [] s0');
}
{   // (a pair that never reports, someone READY in line: void after 15 minutes, the next got ready)
	const A = await create('AAA', {}, false), B = await enter(A.code, 'BBB', {}, false), Cm = await enter(A.code, 'CCC', {}, false);
	await act(A, 'started'); await act(B, 'started');
	const polls = async (k) => { for (let i = 0; i < k; i++) { advance(20000); for (const m of [A, B, Cm]) await act(m, 'poll'); await alarm(A.code); } };
	await polls(44);   // (14:40)
	const at14 = show(A.code);
	advance(20500); for (const m of [A, B, Cm]) await act(m, 'poll'); await alarm(A.code);   // (15:00.5)
	check('8 no result in 15 minutes: void (the holder stays), the next READY one got ready', [at14, show(A.code), (rec(A.code) && rec(A.code).t.last || {}).why], ['PLAYING AAA/BBB [CCC] s0', 'READY AAA/BBB [CCC] s0 r:CCC', 'timeout']);
}
{   // (said again: started in CONNECTING and PLAYING, a result in DECIDED and after it: nothing more happens)
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), Cm = await enter(A.code, 'CCC');
	const s1 = [said(await act(A, 'started', { n: 1 })), said(await act(A, 'started', { n: 1 }))];
	check('8 started twice in CONNECTING: ok, still waiting for the other', [s1, show(A.code)], [['ok', 'ok'], 'CONNECTING AAA/BBB [CCC] s0']);
	await act(B, 'started', { n: 1 });
	B.ws.send(JSON.stringify({ op: 'started', n: 1 })); await settle();
	check('8 started again once on: ok, the match on', [said(await act(A, 'started', { n: 1 })), show(A.code)], ['ok', 'PLAYING AAA/BBB [CCC] s0']);
	check('8 started for another match (an earlier seat\'s, late): stale', said(await act(A, 'started', { n: 0 })), 'stale');
	const r1 = [said(await act(A, 'result', { winner: 'a', n: 1 })), said(await act(A, 'result', { winner: 'a', n: 1 }))];
	A.ws.send(JSON.stringify({ op: 'result', winner: 'a', n: 1 })); await settle();
	check('8 a result said three times in DECIDED: ok, one result', [r1, show(A.code)], [['ok', 'ok'], 'DECIDED AAA/BBB [CCC] s0']);
	await act(B, 'result', { winner: 'a', n: 1 });
	const after = [said(await act(B, 'result', { winner: 'a', n: 1 })), said(await act(A, 'result', { winner: 'a', n: 1 }))];
	check('8 results said again once it\'s applied: refused, the streak counted once', [after, show(A.code)], [['not_now', 'not_now'], 'READY AAA/BBB [CCC] s1 r:CCC']);
}

// ---- 9: a newcomer that doesn't start: the front once, then the back; a holder whose every new pair fails: unseated ----
{
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), Cm = await enter(A.code, 'CCC');
	await act(Cm, 'away');
	const fail = async () => { rec(A.code).t.deadline = Date.now() - 1; await alarm(A.code); };
	await fail();
	const once = show(A.code);
	await act(B, 'ready');
	await fail();
	check('9 a newcomer not started: the front, AWAY; the 2nd time in a row, the back', [once, show(A.code)], ['SOLO AAA/- [BBB*,CCC*] s0', 'SOLO AAA/- [CCC*,BBB*] s0']);
}
{
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), Cm = await enter(A.code, 'CCC'), D = await enter(A.code, 'DDD'), E = await enter(A.code, 'EEE');
	const fail = async () => { await act(A, 'started'); rec(A.code).t.deadline = Date.now() - 1; await alarm(A.code); };   // (the holder says started; the newcomer never connects)
	await fail(); await fail();
	const two = show(A.code);
	await fail();
	check('9 a holder whose new pairs fail, each blamed on the newcomer: the 3rd time the holder\'s unseated', [two, show(A.code)], ['CONNECTING AAA/DDD [CCC*,BBB*,EEE] s0', 'CONNECTING EEE/DDD [AAA*,CCC*,BBB*] s0']);
}

// ---- 11: Rooms 2 rows never push an older build's rows off the list ----
{
	const st = env.STATS.get(env.STATS.idFromName('all'));
	const L = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789', code = (p, i) => p + L[Math.floor(i / 31) % 31] + L[i % 31];
	const sid = (i) => (0x1000000000000000n + BigInt(i)).toString(16);
	const tell = (b) => st.fetch('https://stats/tell', { method: 'POST', body: JSON.stringify({ game: 'vf2', version: 'v0.1.16+mix1', ...b }) });
	for (let i = 0; i < 60; i++) await tell({ op: 'host', code: code('HOST', i), listed: true, name: 'H' + i, sids: [sid(i)], countries: ['KR'], aid: (0x2000000000000000n + BigInt(i)).toString(16) });
	for (let i = 0; i < 40; i++) await tell({ op: 'watch', wid: code('WTCH', i), listed: true, p: [['A' + i, 'KR'], ['B' + i, 'JP']], wins: [0, 0], rounds: [0, 0], watchers: 1, aid: (0x3000000000000000n + BigInt(i)).toString(16) });
	advance(1000);   // (the Rooms 2 rooms newer)
	for (let i = 0; i < 50; i++) await tell({ op: 'room2', code: code('RMRM', i), listed: true, table: [['R' + i, 'KR'], null], sids: [sid(500 + i)], countries: ['KR'], aid: (0x4000000000000000n + BigInt(i)).toString(16) });
	const rooms = stats().rooms().filter((x) => x.game === 'vf2');
	check('11 list: a game\'s 100 rows of rooms and matches all kept beside its 50 Rooms 2 rows (newer)', [rooms.filter((x) => x.v !== 2).length, rooms.filter((x) => x.v === 2).length], [100, 50]);
	const aid = (0x5000000000000000n).toString(16);
	for (let i = 0; i < 6; i++) await tell({ op: 'host', code: code('SAME', i), listed: true, name: 'S' + i, sids: [sid(900 + i)], countries: ['KR'], aid, game: 'vf1', version: 'v1.5.0' });
	advance(1000);
	for (let i = 0; i < 3; i++) await tell({ op: 'room2', code: code('SAMR', i), listed: true, table: [['T' + i, 'KR'], null], sids: [sid(950 + i)], countries: ['KR'], aid, game: 'vf1', version: 'v1.5.0' });
	const v1rows = stats().rooms().filter((x) => x.game === 'vf1');
	check('11 list: one address\'s 6 rooms all kept, its newer Rooms 2 rooms over its 6 left out', [v1rows.filter((x) => x.v !== 2).length, v1rows.filter((x) => x.v === 2).length], [6, 0]);
}

// ---- The final review (17 Nov), F1-F8 ----
const last = (code) => JSON.stringify(rec(code).t.last);
{   // F1: a challenger's started alone doesn't unseat the holder
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a', n: 1 }); await act(B, 'result', { winner: 'a', n: 1 });
	const G = await enter(A.code, 'GRF');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a', n: 2 }); await act(B, 'result', { winner: 'a', n: 2 });
	advance(3100); await alarm(A.code);
	const seated = show(A.code);
	await act(G, 'started');   // (said, never connected)
	advance(30500); await alarm(A.code);
	check('F1 a challenger says started alone: it goes back to the front, the holder keeps the table and its streak', [seated, show(A.code), rec(A.code).t.last.why], ['CONNECTING AAA/GRF [BBB] s2', 'CONNECTING AAA/BBB [GRF*] s2', 'connect']);
}
{   // F1: a challenger's started alone isn't counted against the holder (its 3 in a row)
	const A = await create('AAA'), ms = [];
	for (const n of ['BBB', 'CCC', 'DDD', 'EEE']) ms.push(await enter(A.code, n));
	const fail = async (who) => { if (who) await act(who, 'started'); rec(A.code).t.deadline = Date.now() - 1; await alarm(A.code); };
	await fail(A);   // (B: the holder said started)
	await fail(ms[1]);   // (C: only C said it)
	await fail(A);   // (D)
	check('F1 a challenger\'s started alone in between: the holder\'s 3 in a row not reached', show(A.code), 'CONNECTING AAA/EEE [DDD*,CCC*,BBB*] s0');
}
{   // F2: a loser coming back by its own key (its game gone: its socket closed): one re-seat, then a forfeit
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), Cm = await enter(A.code, 'CCC');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a', n: 1 }); await act(B, 'result', { winner: 'a', n: 1 });
	advance(3100); await alarm(A.code);
	await act(A, 'started'); await act(Cm, 'started');
	const back = async (m) => { if (m.ws && m.ws.open) { m.ws.close(1000); await settle(); } advance(5000); return call('POST', '/v2/room/enter', { ...me(m.name), code: A.code, mid: m.mid, mkey: m.mkey }, m.ip); };
	const b1 = await back(Cm);
	const once = [b1.status, b1.body.back, show(A.code), rec(A.code).t.last.why, rec(A.code).t.reseated];
	await act(A, 'started'); await act(Cm, 'started');
	const b2 = await back(Cm);
	check('F2 back mid-match: the two seated again once', once, [200, true, 'CONNECTING AAA/CCC [BBB] s1', 'rejoin', true]);
	check('F2 back mid-match again: forfeited (the other stays, its streak kept)', [b2.status, show(A.code), last(A.code)], [200, 'READY AAA/CCC [BBB] s1 r:BBB', '{"result":"forfeit","winner":"a","n":3}']);
}
{   // F2: decided by the other side's report, the loser back: settled by it, not voided
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a', n: 1 });
	B.ws.close(1000); await settle(); advance(5000);
	const b = await call('POST', '/v2/room/enter', { ...me('BBB'), code: A.code, mid: B.mid, mkey: B.mkey }, B.ip);
	check('F2 back with the other\'s win in: the win stands', [b.status, show(A.code), last(A.code)], [200, 'PLAYING AAA/BBB [] s1', '{"result":"a","n":1}']);
	check('F2 ... and the two playing on start their session again', rec(A.code).t.restart, true);
	const A2 = await create('AAA'), B2 = await enter(A2.code, 'BBB');
	await act(A2, 'started'); await act(B2, 'started');
	await act(A2, 'result', { winner: 'disconnect', n: 1 });
	B2.ws.close(1000); await settle(); advance(5000);
	await call('POST', '/v2/room/enter', { ...me('BBB'), code: A2.code, mid: B2.mid, mkey: B2.mkey }, B2.ip);
	check('F2 back with the other\'s "disconnect" in: forfeited by the one back', last(A2.code), '{"result":"forfeit","winner":"a","n":1}');
}
{   // (b) the same game gathering its link again (its socket open, or polling and just heard from), maybe just as a ready
	// seated it: its new addresses taken, nothing else (no void, no re-seat, its socket kept); a loser doing it voids nothing
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(B, 'away'); await act(B, 'ready');   // (seated again by its ready: a new pair)
	const before = JSON.stringify([rec(A.code).t.token, rec(A.code).t.n, rec(A.code).t.reseated, rec(A.code).t.last]);
	const g = await call('POST', '/v2/room/enter', { ...me('BBB', { cands: ['203.0.113.77:7777'] }), code: A.code, mid: B.mid, mkey: B.mkey }, B.ip);
	check('b re-gather while seated, socket open: back, the pair as it was, its socket kept, the new addresses the other\'s peer', [g.status, g.body.back, JSON.stringify([rec(A.code).t.token, rec(A.code).t.n, rec(A.code).t.reseated, rec(A.code).t.last]) === before, B.ws.open, A.state.you.seat.peer.cands], [200, true, true, true, ['203.0.113.77:7777']]);
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a', n: rec(A.code).t.n });
	const l = await call('POST', '/v2/room/enter', { ...me('BBB'), code: A.code, mid: B.mid, mkey: B.mkey }, B.ip);
	check('b a loser entering again with its socket open: nothing voided, the match still decided', [l.status, show(A.code)], [200, 'DECIDED AAA/BBB [] s0']);
	const P = await create('PPP', {}, false), Q = await enter(P.code, 'QQQ', {}, false);
	await act(P, 'started'); await act(Q, 'started');
	await act(Q, 'poll');
	const pq = await call('POST', '/v2/room/enter', { ...me('QQQ'), code: P.code, mid: Q.mid, mkey: Q.mkey }, Q.ip);
	check('b polling, just heard from: its link only', [pq.status, show(P.code), rec(P.code).t.last], [200, 'PLAYING PPP/QQQ [] s0', null]);
	advance(5000); await act(P, 'poll');
	const pq2 = await call('POST', '/v2/room/enter', { ...me('QQQ'), code: P.code, mid: Q.mid, mkey: Q.mkey }, Q.ip);
	check('b polling, 5 s quiet: a game come back (the two seated again, once)', [pq2.status, show(P.code), rec(P.code).t.last.why], [200, 'CONNECTING PPP/QQQ [] s0', 'rejoin']);
}
freeze();
{   // F3: one member toggling at its allowed rate: the Stats told at most every 10 s
	const A = await create('AAA');
	advance(20000); await alarm(A.code);
	tells = 0;
	for (let i = 0; i < 120; i++) {   // (4 minutes, a toggle every 2 s)
		A.ws.send(i % 2 ? 'ready' : 'away'); await settle();
		for (let k = 0; k < 2; k++) { advance(1000); if ((await alarm_in(A.code)) <= 0) await alarm(A.code); }
	}
	check('F3 a member toggling READY / AWAY for 4 minutes: the count told at most every 10 s', [tells <= 25, show(A.code).startsWith('SOLO AAA')], [true, true]);
}
thaw();
{   // F3: Rooms 2 rooms a day from one address
	env.ROOMS2_CREATES_PER_DAY = '3';
	const ip = '198.18.250.1', made = [];
	for (let i = 0; i < 4; i++) made.push(said(await call('POST', '/v2/room/create', me('CAP' + i), ip)));
	const other = said(await call('POST', '/v2/room/create', me('CAP9'), '198.18.250.2'));
	delete env.ROOMS2_CREATES_PER_DAY;
	check('F3 rooms made from one address a day: past its cap, busy; another address not held up', [made, other], [['ok', 'ok', 'ok', '429 busy'], 'ok']);
}
{   // F4: a one-sided concession, the winner gone or a restart: the concession taken
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'b', n: 1 });
	B.ws.close(1000); await settle();
	advance(10500); await alarm(A.code);
	check('F4 A says B won, B\'s game gone: B\'s win', last(A.code), '{"result":"b","n":1}');
	const A2 = await create('AAA'), B2 = await enter(A2.code, 'BBB');
	await act(A2, 'started'); await act(B2, 'started');
	await act(A2, 'result', { winner: 'b', n: 1 });
	advance(2000); remake(A2.code, [A2, B2], true);
	advance(8500); await alarm(A2.code);
	check('F4 A says B won, then a restart: B\'s win', last(A2.code), '{"result":"b","n":1}');
	const A3 = await create('AAA'), B3 = await enter(A3.code, 'BBB');
	await act(A3, 'started'); await act(B3, 'started');
	await act(A3, 'result', { winner: 'a', n: 1 });
	advance(2000); remake(A3.code, [A3, B3], true);
	advance(8500); await alarm(A3.code);
	check('F4 A says A won, then a restart: B within its 30 s to come back is alive (seated again, not a forfeit)', [rec(A3.code).t.phase, rec(A3.code).t.last.why], ['CONNECTING', 'disconnect']);
}
freeze();
{   // F5: a socket's messages over the rate: each answered busy
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	for (let i = 0; i < 12; i++) B.ws.send(i % 2 ? 'ready' : 'away');
	await settle();
	const busy = B.said.filter((x) => x.error === 'busy');
	check('F5 messages past the rate: each told busy with its op', [busy.length >= 1, busy.every((x) => x.op === 'ready' || x.op === 'away')], [true, true]);
}
thaw();
{   // F6: two new pairs seated within one arming's round trip: the Watch on the newer pair's key
	const A = await create('AAA'), B = await enter(A.code, 'BBB'), Cm = await enter(A.code, 'CCC'), D = await enter(A.code, 'DDD');
	const get = env.WATCHES.get.bind(env.WATCHES);
	let hold = null, held = 0;
	env.WATCHES.get = (id) => { const w = get(id); return { fetch: async (u, i) => { if (String(u).includes('/arm') && held++ === 0) await new Promise((ok) => { hold = ok; }); return w.fetch(u, i); } }; };
	const p1 = act(B, 'away');   // (C seated: its arming held up)
	for (let i = 0; i < 50 && !hold; i++) await new Promise((ok) => setImmediate(ok));
	const p2 = act(Cm, 'away');   // (D seated meanwhile)
	for (let i = 0; i < 50; i++) await new Promise((ok) => setImmediate(ok));
	hold();
	await p1; await p2; await settle();
	env.WATCHES.get = get;
	const w = env.WATCHES.objs.get('w|' + A.code);
	check('F6 the Watch armed with the pair seated now\'s key', [show(A.code), w.arm.wkey === rec(A.code).t.wkey, rec(A.code).t.armed], ['CONNECTING AAA/DDD [CCC*,BBB*] s0', true, true]);
}
{   // F7: members polling, a restart: the same 30 s to come back, once
	const A = await create('AAA', {}, false), B = await enter(A.code, 'BBB', {}, false);
	await act(A, 'started'); await act(B, 'started');
	advance(9000); await act(A, 'poll'); await act(B, 'poll');   // (heard from, not written yet: every 10 s)
	remake(A.code, [], true);
	advance(25000); await alarm(A.code);   // (34 s after what's kept)
	const kept = [show(A.code), Object.keys(rec(A.code).members).length];
	advance(31000); await alarm(A.code);
	check('F7 polling members over a restart: kept 30 s from it', kept, ['PLAYING AAA/BBB [] s0', 2]);
	check('F7 ... once: not back in 30 s, gone', show(A.code), 'EMPTY -/- [] s0');
}
{   // F8: the holder's failures counted only one straight after another (an AWAY between: counted again from 1)
	const A = await create('AAA'), ms = [];
	for (const n of ['BBB', 'CCC', 'DDD', 'EEE', 'FFF']) ms.push(await enter(A.code, n));
	const fail = async () => { await act(A, 'started'); rec(A.code).t.deadline = Date.now() - 1; await alarm(A.code); };
	await fail();   // (B)
	await act(ms[1], 'away');   // (C AWAY while seated: not a failure)
	await fail(); await fail();   // (D, E)
	check('F8 an AWAY between the holder\'s failures: counted from 1 again', show(A.code), 'CONNECTING AAA/FFF [EEE*,DDD*,CCC*,BBB*] s0');
}

// ---- seq: every state told carries the room's seq, one more at each change, never less (a restart, the object made again) ----
{
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	const seqs = (m) => m.said.filter((d) => d.state).map((d) => d.state.seq);
	const s0 = A.state.seq;
	const r1 = await act(B, 'away');
	const r2 = await act(A, 'poll');
	const pushed = seqs(A);
	check('seq: in every state (a push, a call\'s answer); a change: one more; a poll that changes nothing: the same', [typeof s0, r1.body.state.seq > s0, r2.body.state.seq === r1.body.state.seq, pushed.every((x, i) => i === 0 || x >= pushed[i - 1]), pushed[pushed.length - 1] === r1.body.state.seq], ['number', true, true, true, true]);
	check('seq: kept with the room', obj(A.code).ctx.storage.__store.get('r').seq, r1.body.state.seq);
	const before = r1.body.state.seq;
	remake(A.code, [A, B], true);
	const after = await act(A, 'poll');
	const changed = await act(B, 'ready');
	check('seq: after a restart, never less; the next change one more', [after.body.state.seq >= before, changed.body.state.seq > after.body.state.seq], [true, true]);
	remake(A.code, [], false);
	const again = await act(A, 'poll');
	check('seq: the object made again from its storage: the same', again.body.state.seq >= changed.body.state.seq, true);
	const C = await create('CCC');
	check('seq: a new room\'s own, from its start', C.state.seq >= 1 && C.state.seq <= 3, true);
}

console.log(`\nroom2-review: ${checks - fails} of ${checks} checks passed`);
Date.now = real_now;
process.exit(fails ? 1 : 0);
