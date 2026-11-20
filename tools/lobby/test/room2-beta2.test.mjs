// Rooms beta2: the room's rules (create and settings, the state's room.rules, max players, Dural on the select, the winner's
// side), a room without rules as before, and the name filter's HEADS.   node test/room2-beta2.test.mjs src/index.js
import { lobby, settle } from './standin.mjs';
const { worker, env } = await lobby(process.argv[2]);
const PRINT = process.argv.includes('--print');
env.SWITCH_ROOMS2 = 'on';
env.ROOMS2_MSG_BURST = '100000';   // (a member's calls' rate: test/room2-review.test.mjs has it; here each member's calls come all at once)
const real_now = Date.now;
let skew = 0;
Date.now = () => real_now() + skew;   // (time moved on by advance(): a ready check's 15 s, an empty room's minute)
const advance = (ms) => { skew += ms; };
let fails = 0, checks = 0;
function check(what, got, want, quiet = !PRINT) {
	const ok = JSON.stringify(got) === JSON.stringify(want);
	checks++;
	if (!ok) fails++;
	if (!ok || !quiet) console.log((ok ? 'ok   ' : 'FAIL ') + what + ': ' + JSON.stringify(got) + (ok ? '' : ' (wanted ' + JSON.stringify(want) + ')'));
}
let ipn = 1;
async function call(method, path, body, from = '198.51.100.1') {
	const init = { method, headers: { 'cf-connecting-ip': from, 'x-test-country': 'KR' } };
	if (body !== undefined) init.body = body instanceof Uint8Array ? body : JSON.stringify(body);
	const req = new Request('http://lobby' + path, init);
	Object.defineProperty(req, 'cf', { value: { latitude: '37.5', longitude: '127.0', continent: 'AS' } });
	const r = await worker.fetch(req, env);
	const type = r.headers.get('content-type') || '';
	const out = { status: r.status, body: type.includes('json') ? await r.json() : null };
	await settle();
	return out;
}
const C = '0123456789abcdef', V = 'v0.1.7-online-preview+host1';
const me = (name, extra = {}) => ({ game: 'vf3', version: V, content: C, name, cands: ['203.0.113.' + (ipn % 200 + 1) + ':7700'], relay: '', pool: V, accepts: ['stage'], ...extra });
const old_game = (name, extra = {}) => ({ game: 'vf3', version: 'v0.1.7-online-preview', content: C, name, cands: ['203.0.113.8:7700'], relay: '', ...extra });
const obj = (code) => env.ROOMS.objs.get(code);
const rec = (code) => (obj(code) ? obj(code).r : null);

// ---- members: each from an address of its own, on a socket (as a game is) unless asked not to be ----
async function socket_for(m) {
	const r = await worker.fetch(new Request(`http://lobby/v2/room/ws?code=${m.code}&mid=${m.mid}&mkey=${m.mkey}`, { headers: { upgrade: 'websocket', 'cf-connecting-ip': m.ip } }), env);
	if (r.status !== 101) return r.status;
	m.ws = r.webSocket;
	m.ws.accept();
	m.said = [];
	m.ws.addEventListener('message', (e) => { if (e.data === 'pong') return; const d = JSON.parse(e.data); m.said.push(d); if (d.state) m.state = d.state; });
	await settle();
	return 101;
}
async function create(name, extra = {}, sock = true) {
	const ip = '198.51.' + (100 + (ipn >> 8)) + '.' + (ipn++ & 255);
	const r = await call('POST', '/v2/room/create', me(name, extra), ip);
	if (r.status !== 200) return { status: r.status, body: r.body };
	const m = { name, ip, code: r.body.code, mid: r.body.mid, mkey: r.body.mkey, owner_key: r.body.owner_key, wid: r.body.wid, state: r.body.state, status: 200 };
	if (sock) await socket_for(m);
	return m;
}
async function enter(code, name, extra = {}, sock = true, ip) {
	ip = ip || '198.51.' + (100 + (ipn >> 8)) + '.' + (ipn++ & 255);
	const r = await call('POST', '/v2/room/enter', { ...me(name, extra), code }, ip);
	if (r.status !== 200) return { status: r.status, body: r.body };
	const m = { name, ip, code, mid: r.body.mid, mkey: r.body.mkey, state: r.body.state, status: 200 };
	if (sock) await socket_for(m);
	return m;
}
const act = (m, op, extra = {}) => call('POST', '/v2/room/' + op, { code: m.code, mid: m.mid, mkey: m.mkey, ...extra }, m.ip);
const alarm = async (code) => { if (obj(code)) await obj(code).alarm(); await settle(); };
// (the online count told of a room's last change now (told at most every 10 s, the last always, by its alarm): its last
// telling taken as long ago, its alarm run, the clock not moved)
const told = async (...codes) => { for (const c of codes) { if (obj(c)) obj(c).told2 = 0; await alarm(c); } };
// (the room as the lobby holds it, in a line: "PHASE A/B [line] sN", an AWAY member with a *, and while one's getting
// ready " r:NAME")
function show(code) {
	const r = rec(code);
	if (!r) return 'GONE';
	const n = (mid) => (mid && r.members[mid] ? r.members[mid].who.name + (r.members[mid].away ? '*' : '') : '-');
	return `${r.t.phase} ${n(r.t.a)}/${n(r.t.b)} [${r.line.map(n).join(',')}] s${r.t.streak}` + (r.ready ? ' r:' + n(r.ready.mid) : '');
}
const said = (res) => (res.status === 200 ? (res.body && res.body.ok === false ? res.body.error : 'ok') : res.status + ' ' + (res.body && res.body.error));
const { name_ok } = await import(new URL('../' + process.argv[2], import.meta.url).href);

// a room without rules (an older build's create): none in its state, as before; 8 can enter
{
	const A = await create('A', { listed: true, room: 'OLD' });
	check('no rules: none in the state', A.state.room.rules, undefined);
	let ok = 0;
	for (let i = 0; i < 6; i++) if ((await enter(A.code, 'P' + i)).status === 200) ok++;
	check('no rules: no limit (6 more entered)', ok, 6);
}
// rules at create: kept and in every member's state; max players enforced at enter; settings change them
{
	const A = await create('A', { listed: true, room: 'RULES', rules: { side: false, dural: false, custom: false, max: 3, stage: true } });
	check('rules at create', A.state.room.rules, { side: false, dural: false, custom: false, max: 3, stage: true });
	const B = await enter(A.code, 'B'), C = await enter(A.code, 'C'), D = await enter(A.code, 'D');
	check('max 3: two more entered', [B.status, C.status], [200, 200]);
	check('max 3: the 4th refused full', [D.status, D.body && D.body.error], [409, 'full']);
	check('a member sees the rules', B.state.room.rules.max, 3);
	const s = await call('POST', '/v2/room/settings', { code: A.code, owner_key: A.owner_key, rules: { max: 0, stage: false, bogus: 1 } }, A.ip);
	check('settings: rules changed, the rest kept', s.body.room.rules, { side: false, dural: false, custom: false, max: 0, stage: false });
	check('settings: then 4th enters', (await enter(A.code, 'D')).status, 200);
	const n = await call('POST', '/v2/room/settings', { code: A.code, mid: B.mid, mkey: B.mkey, owner_key: 'x'.repeat(32), rules: { max: 2 } }, B.ip);
	check('settings by a non-owner refused', n.status, 403);
	check('the room\'s rules unchanged', rec(A.code).rules.max, 0);
}
// Dural: the room's rule takes her off a pool match's terms (whoever has her on); with Dural allowed, as before
{
	const dv = V.replace('+host1', '+host1+dural3');
	const dme = { version: dv, pool: V, accepts: ['stage', 'dural3'] };
	const A = await create('A', { listed: true, room: 'NODURAL', rules: { dural: false }, ...dme });
	const B = await enter(A.code, 'B', dme);
	check('no Dural: the seat\'s terms the pool\'s', B.state.you.seat && B.state.you.seat.as, V);
	const A2 = await create('A', { listed: true, room: 'DURAL', rules: { dural: true }, ...dme });
	const B2 = await enter(A2.code, 'B', dme);
	check('Dural allowed: her flag in the terms', B2.state.you.seat && B2.state.you.seat.as, V + '+dural3');
}
// the winner's side: by default the one who stays keeps theirs; with side: true the newcomer is always side b
async function play(A, B, winner) {
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner }); await act(B, 'result', { winner });
}
for (const side of [false, true]) {
	const A = await create('A', { listed: true, room: 'SIDE', rules: { side } });
	const B = await enter(A.code, 'B');
	const C = await enter(A.code, 'C');
	await play(A, B, 'b');   // (B, side b, wins: stays)
	advance(4000); await alarm(A.code);
	const r = rec(A.code);
	const name = (mid) => r.members[mid].who.name;
	check('side ' + side + ': after B (b) won, C seated; sides', [name(r.t.a), name(r.t.b)], side ? ['B', 'C'] : ['C', 'B']);
}
// a loser who leaves just after the decision (one report in, the winner's): the win counted, the streak raised, the next seated
{
	const A = await create('A', { listed: true, room: 'QUIT' });
	const B = await enter(A.code, 'B');
	const C = await enter(A.code, 'C');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a' });
	check('decided, one report', rec(A.code).t.phase, 'DECIDED');
	await act(B, 'leave');
	const r = rec(A.code);
	check('the loser left: A\'s win counted', [r.t.last.result, r.t.streak, r.members[r.t.holder].who.name], ['a', 1, 'A']);
	check('the next seated', [r.t.phase, r.members[r.t.b] && r.members[r.t.b].who.name], ['CONNECTING', 'C']);
	// and a forfeit mid-match as before: not counted
	await act(A, 'started'); await act(C, 'started');
	await act(C, 'leave');
	const r2 = rec(A.code);
	check('a forfeit mid-match: kept, not raised', [r2.t.last.result, r2.t.streak], ['forfeit', 1]);
}
// the name filter's HEADS: insults made with a word before, innocent names kept
check('names blocked', ['DICKMASTER', 'MASTERCOCK', 'BIGDICK', 'SUCKMYDICK'].map(name_ok), [false, false, false, false]);
check('names kept', ['HANCOCK', 'PEACOCK', 'SHERLOCK', 'DICKENS', 'COCKPIT', 'MASTER', 'BIGBOSS', 'KINGKONG'].map(name_ok), [true, true, true, true, true, true, true, true]);

console.log((fails ? fails + ' FAILED' : 'all passed') + ' (' + checks + ' checks)');
process.exit(fails ? 1 : 0);
