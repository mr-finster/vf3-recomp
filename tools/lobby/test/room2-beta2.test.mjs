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
// a loser who leaves just after the decision: counted only when the lobby saw the decision from both games, both reports
// agreeing, or the leaver's own report conceding; the winner's report alone then a leave (or going quiet) is a forfeit,
// the streak not raised (a win claimed at once against a rage-quit: beta 1's way); a forfeit mid-match as before
{
	const A = await create('A', { listed: true, room: 'QUIT' });
	const B = await enter(A.code, 'B');
	const C = await enter(A.code, 'C');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a' }); await act(B, 'result', { winner: 'a' });
	await act(B, 'leave');
	const r = rec(A.code);
	check('both reports, then the loser left: A\'s win counted', [r.t.last.result, r.t.streak, r.members[r.t.holder].who.name], ['a', 1, 'A']);
	check('the next seated', [r.t.phase, r.members[r.t.b] && r.members[r.t.b].who.name], ['CONNECTING', 'C']);
	// the loser's own report conceding it, then its Leave line before the winner's report: counted
	const D = await enter(A.code, 'D');
	await act(A, 'started'); await act(C, 'started');
	await act(C, 'result', { winner: 'a' });
	check('decided by the loser\'s report', rec(A.code).t.phase, 'DECIDED');
	await act(C, 'leave_line');
	const r2 = rec(A.code);
	check('the loser conceded and left the line: counted', [r2.t.last.result, r2.t.streak], ['a', 2]);
	// and a forfeit mid-match as before: not counted
	await act(A, 'started'); await act(D, 'started');
	await act(D, 'leave');
	const r3 = rec(A.code);
	check('a forfeit mid-match: kept, not raised', [r3.t.last.result, r3.t.streak], ['forfeit', 2]);
}
// (the review's P2 and P4) a win reported at once, no match played: the opponent leaving the line, leaving the room or going
// quiet is a forfeit, never a counted win
for (const how of ['leave_line', 'leave', 'quiet']) {
	const A = await create('A', { listed: true, room: 'FORGE' });
	const B = await enter(A.code, 'B', {}, how !== 'quiet');
	await enter(A.code, 'C');
	await act(A, 'started'); await act(B, 'started');
	await act(A, 'result', { winner: 'a' });
	if (how === 'quiet') { advance(31000); await alarm(A.code); } else await act(B, how);
	const r = rec(A.code);
	check('A\'s lone report, then B ' + how + ': a forfeit, the streak not raised', [r.t.last && r.t.last.result, r.t.streak], ['forfeit', 0]);
}
// (the review's P1) the room's rules on the seated terms whatever a game sent: a modified one with no pool and a version
// with the Dural flag, in a Dural Off room, seated without her; the stage rule on version terms as the room says; a room
// without rules, and an honest pair, as before
{
	const BASE = 'v0.1.7-online-preview', POOL = BASE + '+host1';
	const honest = { version: BASE + '+stage', pool: POOL, accepts: ['stage', 'dural3', 'dural2'] };
	const sneaky = { version: BASE + '+dural3', pool: '', accepts: [] };
	const seat_as = async (rules, b) => {
		const A = await create('A', { listed: true, room: 'TERMS', ...(rules ? { rules } : {}), ...honest });
		await enter(A.code, 'B', b);
		return rec(A.code).t.as;
	};
	check('Dural off, no pool and +dural3: no Dural, the stage on', await seat_as({ dural: false, stage: true }, sneaky), BASE + '+stage');
	check('Dural off, stage off, no pool and +dural3+stage: neither', await seat_as({ dural: false, stage: false }, { ...sneaky, version: BASE + '+dural3+stage' }), BASE);
	check('Dural on, stage on, no pool and +dural3: Dural kept, the stage added', await seat_as({ dural: true, stage: true }, sneaky), BASE + '+dural3+stage');
	check('no rules: the terms as before', await seat_as(null, sneaky), BASE + '+dural3');
	check('Dural off, an honest pair, B with her on: the pool\'s', await seat_as({ dural: false }, { ...honest, version: BASE + '+dural3+stage' }), POOL);
	check('Dural on, an honest pair, B with her on: the pool\'s and Dural', await seat_as({ dural: true }, { ...honest, version: BASE + '+dural3+stage' }), POOL + '+dural3');
	check('stage off, an honest pair: the pool\'s (its host applies the stage rule)', await seat_as({ stage: false }, honest), POOL);
}
// the name filter's HEADS: insults made with a word before, innocent names kept
check('names blocked', ['DICKMASTER', 'MASTERCOCK', 'BIGDICK', 'SUCKMYDICK'].map(name_ok), [false, false, false, false]);
check('names kept', ['HANCOCK', 'PEACOCK', 'SHERLOCK', 'DICKENS', 'COCKPIT', 'MASTER', 'BIGBOSS', 'KINGKONG'].map(name_ok), [true, true, true, true, true, true, true, true]);
// (the review's: real surnames a COMPOUNDS word makes pass (ALLOW), as in beta 1; WANKER and SHITTER inside a name blocked)
const kept = ['DICKMAN', 'COCKMAN', 'ASSMAN', 'D1CKMAN', 'ASSMANN', 'SWANKER', 'COCKBURN', 'HITMAN', 'BATMAN', 'KINGPIN', 'MASTERCHIEF', 'SCUNTHORPE'];
check('surnames and words kept', kept.filter((n) => !name_ok(n)), []);
const blocked = ['WANKER', 'SHITTER', 'BIGWANKER', 'FATSHITTER', 'WANKERBOY', 'MRSHITTER', 'W4NKER', 'SH1TTERS', 'WANK ER', 'BIGWANKING', 'FATSLUTS', 'BIGDICKMAN', 'DICKMANS'];
check('WANKER, SHITTER inside names blocked', blocked.filter((n) => name_ok(n)), []);

console.log((fails ? fails + ' FAILED' : 'all passed') + ' (' + checks + ' checks)');
process.exit(fails ? 1 : 0);
