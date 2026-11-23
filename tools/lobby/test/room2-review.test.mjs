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
// and the last review before the public beta (R1-R9, R0):
//   R1 a game's close waking a hibernated object taken for a restart: the crashed side alive for a result (its grace)
//   R2 a holder who never reports: never unseated ("the holder stays"); then (the review of the fix) a forged early win
//      taken at 25 s from a live opponent mid-match, and a challenger's claimed win against the holder's disconnect
//   R3 a lone desync forfeited the silent side
//   R4 SWITCH_WEBSOCKETS off left Rooms 2 sockets open; R5 messages that aren't one escaped the member's rate
//   R6 a re-entry waiting on the Watch's arming, the member leaving meanwhile: 500 (now: no answer waits on arming)
//   R7 a new seat told under the last seq while the Watch was armed (no timeout on arming); R4b an arming given up
//      landing late, after a newer pair's, put the older key back on the Watch
//   R8 the other side's report 10 s late voided an honest result; R9 a play-on nobody started, someone READY then: the
//      table stuck 15 minutes (its games hold the loser's Start for the next in line)
//   R0 a member's first call after a restart taken for after its socket's close in the same ms: not alive, a forfeit
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
	await act(A, 'started', { n: 2 }); await act(B, 'started', { n: 2 });   // (they play on: its match begun)
	const G = await enter(A.code, 'GRF');
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
	advance(25500); await alarm(A.code);
	check('F4 A says B won, B\'s game gone: B\'s win', last(A.code), '{"result":"b","n":1}');
	const A2 = await create('AAA'), B2 = await enter(A2.code, 'BBB');
	await act(A2, 'started'); await act(B2, 'started');
	await act(A2, 'result', { winner: 'b', n: 1 });
	advance(2000); remake(A2.code, [A2, B2], true);
	advance(23500); await alarm(A2.code);
	check('F4 A says B won, then a restart: B\'s win', last(A2.code), '{"result":"b","n":1}');
	const A3 = await create('AAA'), B3 = await enter(A3.code, 'BBB');
	await act(A3, 'started'); await act(B3, 'started');
	await act(A3, 'result', { winner: 'disconnect', n: 1 });
	advance(2000); remake(A3.code, [A3, B3], true);
	advance(3000); await act(B3, 'result', { winner: 'disconnect', n: 1 });   // (B back after the restart, polling: says it too, its first call since)
	check('F4 A says disconnect, a restart, then B: A within its 30 s to come back is alive (seated again, not a forfeit)', [rec(A3.code).t.phase, rec(A3.code).t.last.why], ['CONNECTING', 'disconnect']);
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

// ---- The last review before the public beta (17 Nov), R1-R9, each failing on the code before its fix ----
// (the Watch's arming held up till released: hold_arm(true) holds the next ones, release() lets them go)
const watch_get = env.WATCHES.get.bind(env.WATCHES);
let arm_held = false, arm_waiting = [];
env.WATCHES.get = (id) => { const w = watch_get(id); return { fetch: async (u, i) => { if (arm_held && String(u).includes('/arm')) await new Promise((ok) => arm_waiting.push(ok)); return w.fetch(u, i); } }; };
const hold_arm = (on) => { arm_held = on; };
const release = () => { arm_held = false; for (const f of arm_waiting.splice(0)) f(); };
const turns = async (n = 50) => { for (let i = 0; i < n; i++) await new Promise((ok) => setImmediate(ok)); };
// (what a test waits for, as it is, not a number of turns: a call through the Worker hashes its sender (crypto.subtle, the
// thread pool's), so how many turns it takes to reach its room depends on the machine and how busy it is)
async function until(cond, what) {
	const t0 = real_now();
	while (!cond()) {
		if (real_now() - t0 > 10000) throw new Error('until: never ' + what);
		await new Promise((ok) => setImmediate(ok));
	}
}
const side_of = (code, m) => (rec(code).t.a === m.mid ? 'a' : rec(code).t.b === m.mid ? 'b' : null);
{   // R1: a game crashing mid-match, its close waking a hibernated object: no restart's grace, it isn't alive for a result
	const A = await create('HLD'), B = await enter(A.code, 'CHL'), Cm = await enter(A.code, 'NXT');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'disconnect', n: 1 });   // (its connection lost; then its game crashes)
	advance(5000);
	remake(A.code, [A, B, Cm], false);   // (hibernated: its sockets still there)
	A.ws.close(1001); await settle();
	const alive = obj(A.code).v2_alive(rec(A.code), A.mid, Date.now());
	await act(B, 'result', { winner: 'disconnect', n: 1 });
	check('R1 a crash\'s close waking a hibernated object: not alive; both saying disconnect, the crashed side forfeits', [alive, last(A.code), show(A.code)],
		[false, JSON.stringify({ result: 'forfeit', winner: side_of(A.code, B), n: 1 }), 'READY HLD/CHL [NXT] s0 r:NXT']);
	const X = await create('HLD'), Y = await enter(X.code, 'CHL');
	await act(X, 'started'); await act(Y, 'started');
	advance(20000);
	remake(X.code, [X, Y], false);
	X.ws.close(1001); await settle();
	check('R1 ... its seen kept (30 s to come back), not its grace', [!!rec(X.code).members[X.mid], rec(X.code).members[X.mid].grace], [true, 0]);
}
{   // R2: one report, the other side silent: a forged early win isn't taken while the other's alive (its match waited out);
	// taken when the other's gone, or at the match's own end; a holder's disconnect against a claimed win, neutral
	const lone = async (claim) => {   // (a pair seated and started, side b's report alone; a third waiting in line)
		const A = await create('HLD'), B = await enter(A.code, 'CHL'), Cm = await enter(A.code, 'THD');
		await act(A, 'started'); await act(B, 'started');
		await act(B, 'result', { winner: claim(side_of(A.code, B)), n: 1 });
		return [A, B, Cm];
	};
	{
		const [A, B] = await lone((s) => s);   // (B claims its win at once: forged, a VF match is minutes)
		advance(25500); await act(A, 'poll'); await alarm(A.code);
		const waiting = show(A.code);
		advance(60000); await act(A, 'poll'); await alarm(A.code);
		await act(A, 'result', { winner: side_of(A.code, A), n: 1 });   // (A's honest report at its match's end: they disagree)
		check('R2 a forged early win, the other alive: still waited for at 25 s and a minute on; the honest report then: void, both to the back',
			[waiting, show(A.code).startsWith('DECIDED'), JSON.stringify(rec(A.code).t.last), rec(A.code).t.streak], ['DECIDED HLD/CHL [THD] s0', false, JSON.stringify({ result: 'void', why: 'disagree', n: 1 }), 0]);
	}
	{
		const [A, B] = await lone((s) => s);
		advance(25500); await act(A, 'poll'); await alarm(A.code);
		rec(A.code).t.deadline = Date.now() - 1; await alarm(A.code);   // (the match's own end (PLAYING_MAX_MS) reached, A never saying)
		check('R2 a holder that never reports, alive: the challenger\'s word stands at the match\'s end', [last(A.code), rec(A.code).t.holder === B.mid, rec(A.code).t.streak], [JSON.stringify({ result: side_of(A.code, B), n: 1 }), true, 1]);
	}
	{
		const [A, B] = await lone((s) => s);
		A.ws.close(1000); await settle();   // (the holder's game truly gone: no socket, no poll)
		advance(25500); await alarm(A.code);
		check('R2 one report, the other\'s game gone: taken at 25 s', [last(A.code), rec(A.code).t.holder === B.mid], [JSON.stringify({ result: side_of(A.code, B), n: 1 }), true]);
	}
	{
		const [A, B] = await lone(() => 'disconnect');
		advance(10000); await act(A, 'poll'); await alarm(A.code);
		const waiting = show(A.code);
		advance(15500); await act(A, 'poll'); await alarm(A.code);
		// (R-71, round 4: a lone disconnect claims no win: settled at DECIDE_MS as a disconnect, both alive: the two seated
		// again once (then the holder stays), never waited out to PLAYING_MAX_MS while the other side sits connecting)
		check('R2 a lone disconnect, the other alive and silent: at 25 s the two seated again, not 15 minutes in DECIDED', [waiting, show(A.code), last(A.code)],
			['DECIDED HLD/CHL [THD] s0', 'CONNECTING HLD/CHL [THD] s0', JSON.stringify({ result: 'void', why: 'disconnect', n: 1 })]);
	}
	{
		const [A, B] = await lone(() => 'desync');
		A.ws.close(1000); await settle();
		advance(25500); await alarm(A.code);
		const gone = [last(A.code), rec(A.code).t.phase];
		const [D, E] = await lone(() => 'desync');
		advance(25500); await act(D, 'poll'); await alarm(D.code);
		rec(D.code).t.deadline = Date.now() - 1; await alarm(D.code);
		check('R3 a lone desync: void (nobody\'s fault), the two seated again, never a forfeit (the other gone, or alive to the match\'s end)', [gone, [last(D.code), rec(D.code).t.phase, rec(D.code).t.holder === D.mid]],
			[[JSON.stringify({ result: 'void', why: 'desync', n: 1 }), 'CONNECTING'], [JSON.stringify({ result: 'void', why: 'desync', n: 1 }), 'CONNECTING', true]]);
	}
	{
		const A = await create('HLD'), B = await enter(A.code, 'CHL'), Cm = await enter(A.code, 'THD');
		await act(A, 'started'); await act(B, 'started');
		for (const m of [A, B]) await act(m, 'result', { winner: 'a', n: 1 });   // (A holds, streak 1; C seated against it)
		advance(3100); await alarm(A.code);
		const st = async () => { await act(A, 'started'); await act(Cm, 'started'); };
		await st();
		await act(A, 'result', { winner: 'disconnect', n: 2 });
		await act(Cm, 'result', { winner: side_of(A.code, Cm), n: 2 });   // (C, losing, cut the link and claims the win)
		const once = [last(A.code), show(A.code)];
		await st();
		await act(A, 'result', { winner: 'disconnect', n: 3 });
		await act(Cm, 'result', { winner: side_of(A.code, Cm), n: 3 });
		check('R2 the holder\'s disconnect against a claimed win: void, seated again once; again: the holder stays, its streak not raised, the challenger to the back',
			[once, last(A.code), show(A.code)], [[JSON.stringify({ result: 'void', why: 'disconnect', n: 2 }), 'CONNECTING AAA/THD [CHL] s1'.replace('AAA', 'HLD')], JSON.stringify({ result: 'void', why: 'disconnect', n: 3 }), 'READY HLD/THD [CHL] s1 r:CHL']);
	}
	{
		const J = await create('HLD'), K = await enter(J.code, 'CHL'), L = await enter(J.code, 'THD');
		await act(J, 'started'); await act(K, 'started');
		await act(J, 'result', { winner: 'disconnect', n: 1 });
		K.ws.close(1000); await settle();   // (the challenger's game truly gone: no socket, no poll)
		advance(25500); await alarm(J.code);
		check('R2 an honest disconnect (the other side dead): the live side\'s forfeit win at 25 s', [last(J.code), rec(J.code).t.holder === J.mid], [JSON.stringify({ result: 'forfeit', winner: side_of(J.code, J), n: 1 }), true]);
	}
	{
		const M = await create('HLD'), N = await enter(M.code, 'CHL'), O = await enter(M.code, 'THD');
		await act(M, 'started'); await act(N, 'started');
		await act(M, 'result', { winner: side_of(M.code, N), n: 1 });   // (a concession: the other won, by this player's own word)
		advance(25500); await act(N, 'poll'); await alarm(M.code);
		check('R2 a lone concession, the other alive: taken at 25 s', [last(M.code), rec(M.code).t.holder === N.mid], [JSON.stringify({ result: side_of(M.code, N), n: 1 }), true]);
	}
}
{   // R4: SWITCH_WEBSOCKETS off: a Rooms 2 room's sockets closed at its next alarm or message; its members poll on, alive meanwhile
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	A.ws.send(JSON.stringify({ op: 'result', winner: 'disconnect', n: 1 })); await settle();
	env.SWITCH_WEBSOCKETS = 'off';
	advance(1000); await alarm(A.code);
	const closed = [A.closed, B.closed];
	const ws_again = await socket_for({ ...A });
	await act(B, 'result', { winner: 'disconnect', n: 1 });   // (B over HTTPS now; A not heard from since: alive, the lobby closed its socket)
	check('R4 off: both sockets closed (1012) at the alarm, a new one refused; A alive meanwhile (seated again, not a forfeit)', [closed, ws_again, rec(A.code).t.phase, rec(A.code).t.last.why], [[1012, 1012], 503, 'CONNECTING', 'disconnect']);
	advance(20000); await alarm(A.code);
	check('R4 off: its members still there, polling', [said(await act(A, 'poll')), Object.keys(rec(A.code).members).length], ['ok', 2]);
	delete env.SWITCH_WEBSOCKETS;
	const C = await create('CCC'), D = await enter(C.code, 'DDD');
	env.SWITCH_WEBSOCKETS = 'off';
	C.ws.send(JSON.stringify({ op: 'poll' })); await settle();
	check('R4 off: closed at a member\'s message too', [C.closed, D.closed], [1012, 1012]);
	delete env.SWITCH_WEBSOCKETS;
}
{   // R5: messages that aren't one (over 1 KB, binary, not JSON, an op it doesn't know) count against the member's rate
	const A = await create('CCC');
	let answers = 0;
	A.ws.addEventListener('message', () => answers++);
	for (let i = 0; i < 20; i++) A.ws.send('{"op":"nope"}');
	for (let i = 0; i < 20; i++) A.ws.send('x');
	for (let i = 0; i < 20; i++) A.ws.send('{"op":"poll","pad":"' + 'x'.repeat(1100) + '"}');
	for (let i = 0; i < 20; i++) if (A.ws.open) A.ws.send(new Uint8Array(8));
	await settle();
	check('R5 80 messages that aren\'t one: none answered, the socket closed as a flood (1008)', [answers, A.closed], [0, 1008]);
	const B = await create('DDD');
	for (let i = 0; i < 5; i++) B.ws.send('x');
	B.ws.send(JSON.stringify({ op: 'poll' })); await settle();
	check('R5 a few: nothing more (the next message answered)', [B.closed, B.said.filter((d) => d.state).length >= 2], [null, true]);
}
// (a call answered without waiting for the objects' work after it (settle() would wait for an arming held up))
async function raw(method, path, body, from) {
	const req = new Request('http://lobby' + path, { method, headers: { 'cf-connecting-ip': from, 'x-test-country': 'KR' }, body: JSON.stringify(body) });
	Object.defineProperty(req, 'cf', { value: { latitude: '37.5', longitude: '127.0', continent: 'AS' } });
	try { const r = await worker.fetch(req, env); const t = await r.text(); let j = null; try { j = JSON.parse(t); } catch (e) {} return { status: r.status, body: j }; } catch (e) { return { status: 'threw ' + e.message }; }
}
const raw_act = (m, op, extra = {}) => raw('POST', '/v2/room/' + op, { code: m.code, mid: m.mid, mkey: m.mkey, ...extra }, m.ip);
{   // R6: a re-entry that seats the two again answered at once (not after the Watch's arming), the member leaving after it: no 500
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	B.ws.close(1000); await settle();
	advance(5000);   // (B's game back after a crash: not its link gathered again)
	hold_arm(true);
	const back = raw('POST', '/v2/room/enter', { ...me('BBB'), code: A.code, mid: B.mid, mkey: B.mkey }, B.ip);
	await until(() => arm_waiting.length > 0, 'the re-entry\'s arming');   // (the enter taken by the room, the two seated again: the leave after it)
	const left = await raw_act(B, 'leave');
	release();
	const r = await back;
	await settle();
	check('R6 re-entered (its arming held up), then left: the enter answered, back, not a 500; the leave taken', [r.status, r.body && r.body.back, said(left), !!rec(A.code).members[B.mid]], [200, true, 'ok', false]);
}
{   // R7: a new seat told at once with its own seq (never under the last one's), the Watch armed after it in the background
	const A = await create('AAA'), Cm = await enter(A.code, 'CCC'), Bm = await enter(A.code, 'BBB');
	await act(A, 'started');
	const before = Bm.state.seq;
	hold_arm(true);
	advance(31000);
	const t0 = real_now();
	await obj(A.code).alarm();
	const took = real_now() - t0;
	await turns();
	const poll = await raw_act(Bm, 'poll');
	const st = poll.body.state;
	check('R7 a poll while the Watch is armed: the new seat under a new seq, the same as its socket was told', [show(A.code), st.seq > before, st.you.pos, !!st.you.seat, Bm.state.seq === st.seq, Bm.state.you.pos], ['CONNECTING AAA/BBB [CCC*] s0', true, 0, true, true, 0]);
	check('R7 the alarm (and any call) not waiting for the arming', took < 1000, true);
	release();
	await settle();
	check('R7 armed: told again, the host its watch key', [rec(A.code).t.armed, A.state.seq > st.seq, !!(A.state.you.seat && A.state.you.seat.wkey)], [true, true, true]);
}
{   // R7/R4b: an arming that never answers given up after 5 s (the match unwatchable); landing late, after a newer pair's
	// arming, refused by the Watch: its key stays the newer pair's
	const X = await create('XXX'), Y = await enter(X.code, 'YYY'), Z = await enter(X.code, 'ZZZ');
	await act(X, 'started');
	hold_arm(true);
	advance(31000);
	await obj(X.code).alarm();
	hold_arm(false);   // (only that one held)
	await new Promise((ok) => setTimeout(ok, 5500)); await settle();
	const given_up = [show(X.code), rec(X.code).t.armed];
	const W = await enter(X.code, 'WWW');
	await act(Z, 'away');   // (Z, not started: unseated; W seated, its arming answered at once)
	await settle();
	const armed2 = [show(X.code), rec(X.code).t.armed];
	release(); await new Promise((ok) => setTimeout(ok, 20)); await settle();   // (the first, late, reaches the Watch now)
	const w = env.WATCHES.objs.get('w|' + X.code);
	check('R7 a Watch that never answers: given up after 5 s, the seat as it is; R4b the newer pair armed; the late one refused, the Watch on the newer key',
		[given_up, armed2, w.arm.wkey === rec(X.code).t.wkey, rec(X.code).t.armed], [['CONNECTING XXX/ZZZ [YYY*] s0', false], ['CONNECTING XXX/WWW [ZZZ*,YYY*] s0', true], true, true]);
	const wt = env.WATCHES.get(env.WATCHES.idFromName('w|ARMNUM'));
	const arm_at = (an, wkey) => wt.fetch('https://watch/arm', { method: 'POST', body: JSON.stringify({ wid: 'ARMNUM', wkey, game: 'vf3', version: V, an }) }).then((r) => r.status);
	check('R4b the Watch: a newer arming taken, an older refused (409), one with no number (an older lobby\'s) taken',
		[await arm_at(2000, 'k2'), await arm_at(1000, 'k1'), env.WATCHES.objs.get('w|ARMNUM').arm.wkey, await arm_at(undefined, 'k0'), env.WATCHES.objs.get('w|ARMNUM').arm.wkey], [200, 409, 'k2', 200, 'k0']);
}
{   // R8: the other side's report late (its game says it again 3 s, 9, 21 s after): still the match's
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a', n: 1 });
	advance(21500); await alarm(A.code);
	const waiting = show(A.code);
	await act(B, 'result', { winner: 'a', n: 1 });
	check('R8 the second report 21 s after the first: still waited for, the two agreeing (A\'s win, not void)', [waiting, last(A.code), show(A.code)], ['DECIDED AAA/BBB [] s0', JSON.stringify({ result: 'a', n: 1 }), 'PLAYING AAA/BBB [] s1']);
}
{   // R9: a play-on (nobody READY in line) neither has started, someone READY then: the line moves at once, not after 15 min
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	for (const m of [A, B]) await act(m, 'result', { winner: 'a', n: 1 });
	const playon = show(A.code);
	const Cm = await enter(A.code, 'CCC');
	check('R9 a play-on, then a newcomer: the last result stands, the newcomer got ready at once', [playon, show(A.code), Cm.state.ready && Cm.state.ready.you], ['PLAYING AAA/BBB [] s1', 'READY AAA/BBB [CCC] s1 r:CCC', true]);
	advance(3100); await alarm(A.code);
	check('R9 ... and seated 3 s later', show(A.code), 'CONNECTING AAA/CCC [BBB] s1');
	const D = await create('DDD'), E = await enter(D.code, 'EEE'), F = await enter(D.code, 'FFF');
	await act(F, 'away');
	await act(D, 'started'); await act(E, 'started');
	for (const m of [D, E]) await act(m, 'result', { winner: 'a', n: 1 });
	await act(F, 'ready');
	check('R9 a play-on, then READY: ON in line: the same', show(D.code), 'READY DDD/EEE [FFF] s1 r:FFF');
	await act(F, 'away');
	check('R9 ... the one called AWAY after all: the two play on again', show(D.code), 'PLAYING DDD/EEE [FFF*] s1');
	const G = await create('GGG'), H = await enter(G.code, 'HHH'), I = await enter(G.code, 'III');
	await act(I, 'leave_line');
	await act(G, 'started'); await act(H, 'started');
	for (const m of [G, H]) await act(m, 'result', { winner: 'a', n: 1 });
	for (const m of [G, H]) await act(m, 'started', { n: 2 });   // (the play-on's match begun)
	await act(I, 'join_line');
	check('R9 a play-on begun (both said started), then a newcomer: it waits for that match', show(G.code), 'PLAYING GGG/HHH [III] s1');
	for (const m of [G, H]) await act(m, 'result', { winner: 'a', n: 2 });
	check('R9 ... then it\'s next', show(G.code), 'READY GGG/HHH [III] s2 r:III');
}
{   // (a member's first call after a restart, in the same moment as the close the restart's wake took for its socket's: polling, alive)
	const A = await create('AAA'), B = await enter(A.code, 'BBB');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'disconnect', n: 1 });
	remake(A.code, [A, B], true);
	freeze();
	await act(B, 'result', { winner: 'disconnect', n: 1 });
	thaw();
	check('R0 a restart, then the other\'s report its first call: both alive, seated again (no forfeit)', [rec(A.code).t.phase, rec(A.code).t.last], ['CONNECTING', { result: 'void', why: 'disconnect', n: 1 }]);
}
env.WATCHES.get = watch_get;

console.log(`\nroom2-review: ${checks - fails} of ${checks} checks passed`);
Date.now = real_now;
process.exit(fails ? 1 : 0);
