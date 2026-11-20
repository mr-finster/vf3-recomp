// Rooms 2 (/v2/room/*, docs/ROOMS2-DESIGN.md): the table's state machine as a truth table, run: every phase of the table
// (EMPTY, SOLO, SOLO with a line all AWAY, CONNECTING, PLAYING, DECIDED, READY after a match with both still seated: the next
// one getting ready) set up
// fresh, then every event the design names applied to it, the state after it and what the lobby answered checked against
// the table below. Then a six-member room's rotation (winner stays, loser to the back, the streak, an AWAY member passed
// over, READY again), nobody ready (the two play on), watching across seat changes, the limits, an older build's join, the
// rooms list.
//   cd tools/lobby && npm test   (node test/room2.test.mjs src/index.js; --print: the table as it comes out, to read)
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

// ---- the table's phases, each set up fresh: the room's creator A, then B, C, D ----
async function setup(phase) {
	const x = { members: {} };
	const add = (m) => { x.members[m.name] = m; return m; };
	const A = add(await create('A', { listed: true, room: 'TRUTH' }));
	x.code = A.code;
	x.owner_key = A.owner_key;
	if (phase === 'EMPTY') { await act(A, 'leave'); return x; }
	if (phase === 'SOLO') return x;
	const B = add(await enter(x.code, 'B'));
	if (phase === 'SOLO+AWAY') {   // (each seated as it enters, then AWAY: back to the front of the line)
		await act(B, 'away');
		for (const n of ['C', 'D']) await act(add(await enter(x.code, n)), 'away');
		return x;
	}
	add(await enter(x.code, 'C'));
	add(await enter(x.code, 'D'));
	if (phase === 'CONNECTING') return x;
	await act(A, 'started');
	await act(B, 'started');
	if (phase === 'PLAYING') return x;
	await act(A, 'result', { winner: 'a' });
	if (phase === 'DECIDED') return x;
	await act(B, 'result', { winner: 'a' });
	return x;   // (READY+: A won, both still seated, C getting ready)
}
const PHASES = ['EMPTY', 'SOLO', 'SOLO+AWAY', 'CONNECTING', 'PLAYING', 'DECIDED', 'READY+'];
const START = { EMPTY: 'EMPTY -/- [] s0', SOLO: 'SOLO A/- [] s0', 'SOLO+AWAY': 'SOLO A/- [D*,C*,B*] s0', CONNECTING: 'CONNECTING A/B [C,D] s0', PLAYING: 'PLAYING A/B [C,D] s0',
	DECIDED: 'DECIDED A/B [C,D] s0', 'READY+': 'READY A/B [C,D] s1 r:C' };
// (the one an event's done by: the challenger, B, if there is one, else A (in EMPTY, A who left: a member no more))
const actor = (x) => x.members.B || x.members.A;
const called = (x) => { const r = rec(x.code); return r && r.ready ? Object.values(x.members).find((m) => m.mid === r.ready.mid) : null; };
// (both seated ones' reports, A's first; in DECIDED A's is in already ('a'), so only B's; elsewhere A's alone, refused)
async function results(x, ra, rb) {
	const { A, B } = x.members, ph = rec(x.code) ? rec(x.code).t.phase : '';
	if (ph === 'PLAYING') return said(await act(A, 'result', { winner: ra })) + ',' + said(await act(B, 'result', { winner: rb }));
	if (ph === 'DECIDED') return said(await act(B, 'result', { winner: rb }));
	return said(await act(A, 'result', { winner: ra }));
}
const EVENTS = {
	'enter': async (x) => said(await enter(x.code, 'E')),
	'leave': async (x) => said(await act(actor(x), 'leave')),
	'socket close': async (x) => { const m = actor(x); if (m.ws) m.ws.close(1000); await settle(); return rec(x.code) && rec(x.code).members[m.mid] ? 'member' : 'out'; },
	'poll timeout': async (x) => {   // (its socket closed, its last word 31 s ago, the others on theirs: the alarm)
		const m = actor(x);
		if (m.ws) m.ws.close(1000);
		await settle();
		const r = rec(x.code);
		if (r && r.members[m.mid]) r.members[m.mid].seen = Date.now() - 31000;
		await alarm(x.code);
		return said(await act(m, 'poll'));
	},
	'ready': async (x) => { const m = called(x) || actor(x); return said(await act(m, 'ready')); },
	'ready, not called': async (x) => { const r = rec(x.code), m = Object.values(x.members).reverse().find((y) => r && r.members[y.mid] && (!r.ready || r.ready.mid !== y.mid)) || actor(x); return said(await act(m, 'ready')); },
	'away': async (x) => said(await act(actor(x), 'away')),
	'away, the one getting ready': async (x) => { const m = called(x); return m ? said(await act(m, 'away')) : 'none'; },
	'away then ready': async (x) => { const m = actor(x); return said(await act(m, 'away')) + ',' + said(await act(m, 'ready')); },
	'get ready timeout': async (x) => {   // (GET READY's 3 s up: the alarm seats them)
		const r = rec(x.code);
		if (!r || !r.ready) { await alarm(x.code); return 'none'; }
		r.ready.deadline = Date.now() - 1;
		await alarm(x.code);
		return 'seated';
	},
	'result agree': (x) => results(x, 'a', 'a'),
	'result disagree': (x) => results(x, 'a', 'b'),
	'result one-sided': async (x) => {   // (A's report (A won), B's never: the window up, B still there on its socket: A's word taken)
		const r = rec(x.code), ph = r ? r.t.phase : '';
		let out = ph === 'PLAYING' ? said(await act(x.members.A, 'result', { winner: 'a' })) : ph === 'DECIDED' ? 'in' : said(await act(actor(x), 'result', { winner: 'a' }));
		if (rec(x.code) && rec(x.code).t.phase === 'DECIDED') { rec(x.code).t.deadline = Date.now() - 1; await alarm(x.code); }
		return out;
	},
	'result one-sided, other dead': async (x) => {   // (the same, B's socket closed and B not polling: A's word taken as well)
		const r = rec(x.code), ph = r ? r.t.phase : '', B = x.members.B;
		if (B && B.ws) { B.ws.close(1000); await settle(); }
		let out = ph === 'PLAYING' ? said(await act(x.members.A, 'result', { winner: 'a' })) : ph === 'DECIDED' ? 'in' : said(await act(x.members.A, 'result', { winner: 'a' }));
		if (rec(x.code) && rec(x.code).t.phase === 'DECIDED') { rec(x.code).t.deadline = Date.now() - 1; await alarm(x.code); }
		return out;
	},
	'result draw': (x) => results(x, 'draw', 'draw'),
	'result desync': (x) => results(x, 'desync', 'desync'),
	'result disconnect': (x) => results(x, 'disconnect', 'disconnect'),
	'connect fail': async (x) => connect_fail(x, false),
	'connect fail, newcomer started': async (x) => connect_fail(x, true),
	'owner leave': async (x) => said(await act(x.members.A, 'leave')),
	'settings': async (x) => {
		const wrong = await call('POST', '/v2/room/settings', { code: x.code, owner_key: 'f'.repeat(32), room: 'NOPE' });
		const r = await call('POST', '/v2/room/settings', { code: x.code, owner_key: x.owner_key, room: 'new name', ready_ms: 20000 });
		return said(wrong) + ',' + said(r) + (r.status === 200 ? ':' + r.body.room.name + '/' + r.body.room.ready_ms : '');
	},
	'close': async (x) => {
		const wrong = await call('POST', '/v2/room/close', { code: x.code, owner_key: 'f'.repeat(32) });
		const r = await call('POST', '/v2/room/close', { code: x.code, owner_key: x.owner_key });
		const told = Object.values(x.members).some((m) => m.said && m.said.some((d) => d.closed === true));
		return said(wrong) + ',' + said(r) + ',' + said(await act(x.members.A, 'poll')) + (told ? ',told' : '');
	},
	'switch off': async (x) => {
		env.SWITCH_ROOMS2 = 'off';
		const p = await act(actor(x), 'poll'), e = await call('POST', '/v2/room/enter', { ...me('Z'), code: x.code }, '198.51.99.1');
		const listed = ((await call('GET', '/v1/online')).body.rooms || []).some((y) => y.v === 2 && y.room2 && y.room2.code === x.code);
		env.SWITCH_ROOMS2 = 'on';
		const back = await act(actor(x), 'poll');
		const told = Object.values(x.members).some((m) => m.said && m.said.some((d) => d.closed === true));   // (its members told it's closed)
		return said(p) + ',' + said(e) + ',' + (listed ? 'listed' : 'unlisted') + ',' + said(back) + (told ? ',told' : '');
	},
	'old /v1/join': async (x) => {
		const j = await call('POST', '/v1/join', old_game('OLD', { code: x.code }), '198.51.98.1');
		const q = await call('POST', '/v1/join', old_game('OLDQ', { code: x.code, queue: true }), '198.51.98.2');
		const p = await call('POST', '/v1/peek', { code: x.code }, '198.51.98.3');
		const h = await call('POST', '/v1/host', old_game('OLDH', { want: x.code }), '198.51.98.4');
		return [j, q, p].map((y) => y.status + ' ' + y.body.error + ('host_version' in y.body ? ' +hv' : '')).join(',') + ',host:' + (h.status === 200 && h.body.code !== x.code ? 'other code' : h.status);
	},
	'leave line': async (x) => {   // (out of the line or off the table, still in the room watching)
		const m = actor(x), res = await act(m, 'leave_line');
		const st = res.body && res.body.state;
		return said(res) + (st ? ' pos' + st.you.pos + (st.you.watcher ? ' watcher' : '') + ' w' + st.watchers : '');
	},
	'join line': async (x) => {   // (a watcher back into the line, at its back; one in it already: refused)
		const m = actor(x), first = said(await act(m, 'join_line'));
		await act(m, 'leave_line');
		const res = await act(m, 'join_line'), st = res.body && res.body.state;
		return first + ',' + said(res) + (st ? ' pos' + st.you.pos : '');
	},
	'rejoin': async (x) => {   // (its game crashed and came back within its 30 s: its socket gone, enter with its mid and mkey)
		const m = actor(x);
		if (m.ws) m.ws.close(1006);
		await settle();
		const r = await call('POST', '/v2/room/enter', { ...me(m.name, { cands: ['203.0.113.250:7701'] }), code: x.code, mid: m.mid, mkey: m.mkey }, m.ip);
		if (r.status !== 200) return said(r);
		return (r.body.mid === m.mid && r.body.mkey === m.mkey && r.body.back ? 'back' : 'new') + ' pos' + r.body.state.you.pos;
	},
	'/v1/online listing': async (x) => {
		await told(x.code);
		const d = (await call('GET', '/v1/online')).body;
		const row = (d.rooms || []).find((y) => y.v === 2 && y.room2 && y.room2.code === x.code);
		if (!row) return 'unlisted';
		const old_keys = ['p1', 'p2', 'code', 'watch', 'random'].filter((k) => k in row);
		return `${row.state} ${row.table.map((t) => (t ? t.name : '-')).join('/')} s${row.streak} line${row.line}` + (old_keys.length ? ' OLD:' + old_keys : '') + (d.features.rooms2 ? '' : ' nofeature');
	},
};
async function connect_fail(x, newcomer_started) {   // (a new pair's 30 s up; the newcomer said started, or neither did)
	const r = rec(x.code);
	if (!r || r.t.phase !== 'CONNECTING') { await alarm(x.code); return 'not connecting'; }
	const side = r.t.holder === r.t.a ? 'b' : 'a', newcomer = r.members[r.t[side]];
	if (newcomer_started) r.t.started[side] = true;
	r.t.deadline = Date.now() - 1;
	await alarm(x.code);
	const r2 = rec(x.code), out = r2 && r2.members[newcomer.mid] && r2.members[newcomer.mid].away ? newcomer.who.name : Object.values(r2 ? r2.members : {}).filter((m) => m.away).map((m) => m.who.name).join('') || '-';
	return out + ' away';
}

// The truth table: [state after, what the lobby answered], each phase's column in PHASES' order. What it says, as the
// design and the owner's decisions have it (13-14 Nov: READY / AWAY, no ready check to press): every member READY from
// entering; an empty seat taken at once by the first READY member in line (entering a waiting table: seated straight
// away); after a match the first READY one gets ready (GET READY, 3 s; Start: now) and is seated by itself, AWAY ones
// passed over in their places, nobody READY: the two play on; AWAY while getting ready, or seated before the session
// started, back to the front of the line; a new pair's 30 s without started: the newcomer back to the front, AWAY (one
// side's started alone proves nothing: the holder isn't unseated on a challenger's word), never let go for it; a leave, a poll timeout, the owner's leave mid-match is a
// forfeit (the one who stays keeps the streak if it was theirs, not raised); results agreeing go to the winner (the
// streak +1); disagreeing, void, both to the back, the streak gone; a draw, the holder stays; a desync or a disconnect
// with both alive, the two seated again cold; one report and the other's socket dead, that one loses; an older build's
// join, line and peek 412 version without host_version, its host never given the code
const T = {
	'enter': [
		["SOLO E/- [] s0","ok"],
		["CONNECTING A/E [] s0","ok"],
		["CONNECTING A/E [D*,C*,B*] s0","ok"],
		["CONNECTING A/B [C,D,E] s0","ok"],
		["PLAYING A/B [C,D,E] s0","ok"],
		["DECIDED A/B [C,D,E] s0","ok"],
		["READY A/B [C,D,E] s1 r:C","ok"],
	],
	'leave': [
		["EMPTY -/- [] s0","410 removed"],
		["EMPTY -/- [] s0","ok"],
		["SOLO A/- [D*,C*] s0","ok"],
		["CONNECTING A/C [D] s0","ok"],
		["CONNECTING A/C [D] s0","ok"],
		["CONNECTING A/C [D] s1","ok"],   // (beta2: B, who conceded A's win, off the table: the win counted)
		["CONNECTING A/C [D] s1","ok"],
	],
	'socket close': [
		["EMPTY -/- [] s0","out"],
		["SOLO A/- [] s0","member"],
		["SOLO A/- [D*,C*,B*] s0","member"],
		["CONNECTING A/B [C,D] s0","member"],
		["PLAYING A/B [C,D] s0","member"],
		["DECIDED A/B [C,D] s0","member"],
		["READY A/B [C,D] s1 r:C","member"],
	],
	'poll timeout': [
		["EMPTY -/- [] s0","410 removed"],
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [D*,C*] s0","410 removed"],
		["CONNECTING A/C [D] s0","410 removed"],
		["CONNECTING A/C [D] s0","410 removed"],
		["CONNECTING A/C [D] s1","410 removed"],   // (beta2: B, who conceded A's win, off the table: the win counted)
		["CONNECTING A/C [D] s1","410 removed"],
	],
	'ready': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","ok"],
		["CONNECTING A/B [D*,C*] s0","ok"],
		["CONNECTING A/B [C,D] s0","ok"],
		["PLAYING A/B [C,D] s0","ok"],
		["DECIDED A/B [C,D] s0","ok"],
		["CONNECTING A/C [D,B] s1","ok"],
	],
	'ready, not called': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","ok"],
		["CONNECTING A/D [C*,B*] s0","ok"],
		["CONNECTING A/B [C,D] s0","ok"],
		["PLAYING A/B [C,D] s0","ok"],
		["DECIDED A/B [C,D] s0","ok"],
		["READY A/B [C,D] s1 r:C","ok"],
	],
	'away': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A*/- [] s0","ok"],
		["SOLO A/- [D*,C*,B*] s0","ok"],
		["CONNECTING A/C [B*,D] s0","ok"],
		["PLAYING A/B* [C,D] s0","ok"],
		["DECIDED A/B* [C,D] s0","ok"],
		["READY A/B* [C,D] s1 r:C","ok"],
	],
	'away, the one getting ready': [
		["EMPTY -/- [] s0","none"],
		["SOLO A/- [] s0","none"],
		["SOLO A/- [D*,C*,B*] s0","none"],
		["CONNECTING A/B [C,D] s0","none"],
		["PLAYING A/B [C,D] s0","none"],
		["DECIDED A/B [C,D] s0","none"],
		["READY A/B [C*,D] s1 r:D","ok"],
	],
	'away then ready': [
		["EMPTY -/- [] s0","410 removed,410 removed"],
		["SOLO A/- [] s0","ok,ok"],
		["CONNECTING A/B [D*,C*] s0","ok,ok"],
		["CONNECTING A/C [B,D] s0","ok,ok"],
		["PLAYING A/B [C,D] s0","ok,ok"],
		["DECIDED A/B [C,D] s0","ok,ok"],
		["READY A/B [C,D] s1 r:C","ok,ok"],
	],
	'get ready timeout': [
		["EMPTY -/- [] s0","none"],
		["SOLO A/- [] s0","none"],
		["SOLO A/- [D*,C*,B*] s0","none"],
		["CONNECTING A/B [C,D] s0","none"],
		["PLAYING A/B [C,D] s0","none"],
		["DECIDED A/B [C,D] s0","none"],
		["CONNECTING A/C [D,B] s1","seated"],
	],
	'result agree': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","not_now"],
		["SOLO A/- [D*,C*,B*] s0","not_now"],
		["CONNECTING A/B [C,D] s0","not_now"],
		["READY A/B [C,D] s1 r:C","ok,ok"],
		["READY A/B [C,D] s1 r:C","ok"],
		["READY A/B [C,D] s1 r:C","not_now"],
	],
	'result disagree': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","not_now"],
		["SOLO A/- [D*,C*,B*] s0","not_now"],
		["CONNECTING A/B [C,D] s0","not_now"],
		["CONNECTING C/D [A,B] s0","ok,ok"],
		["CONNECTING C/D [A,B] s0","ok"],
		["READY A/B [C,D] s1 r:C","not_now"],
	],
	'result one-sided': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","not_now"],
		["SOLO A/- [D*,C*,B*] s0","not_now"],
		["CONNECTING A/B [C,D] s0","not_now"],
		["READY A/B [C,D] s1 r:C","ok"],
		["READY A/B [C,D] s1 r:C","in"],
		["READY A/B [C,D] s1 r:C","not_now"],
	],
	'result one-sided, other dead': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","not_now"],
		["SOLO A/- [D*,C*,B*] s0","not_now"],
		["CONNECTING A/B [C,D] s0","not_now"],
		["READY A/B [C,D] s1 r:C","ok"],
		["READY A/B [C,D] s1 r:C","in"],
		["READY A/B [C,D] s1 r:C","not_now"],
	],
	'result draw': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","not_now"],
		["SOLO A/- [D*,C*,B*] s0","not_now"],
		["CONNECTING A/B [C,D] s0","not_now"],
		["READY A/B [C,D] s0 r:C","ok,ok"],
		["CONNECTING C/D [A,B] s0","ok"],
		["READY A/B [C,D] s1 r:C","not_now"],
	],
	'result desync': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","not_now"],
		["SOLO A/- [D*,C*,B*] s0","not_now"],
		["CONNECTING A/B [C,D] s0","not_now"],
		["CONNECTING A/B [C,D] s0","ok,ok"],
		["CONNECTING C/D [A,B] s0","ok"],
		["READY A/B [C,D] s1 r:C","not_now"],
	],
	'result disconnect': [
		["EMPTY -/- [] s0","410 removed"],
		["SOLO A/- [] s0","not_now"],
		["SOLO A/- [D*,C*,B*] s0","not_now"],
		["CONNECTING A/B [C,D] s0","not_now"],
		["CONNECTING A/B [C,D] s0","ok,ok"],
		["CONNECTING A/B [C,D] s0","ok"],
		["READY A/B [C,D] s1 r:C","not_now"],
	],
	'connect fail': [
		["EMPTY -/- [] s0","not connecting"],
		["SOLO A/- [] s0","not connecting"],
		["SOLO A/- [D*,C*,B*] s0","not connecting"],
		["CONNECTING A/C [B*,D] s0","B away"],
		["PLAYING A/B [C,D] s0","not connecting"],
		["DECIDED A/B [C,D] s0","not connecting"],
		["READY A/B [C,D] s1 r:C","not connecting"],
	],
	'connect fail, newcomer started': [
		["EMPTY -/- [] s0","not connecting"],
		["SOLO A/- [] s0","not connecting"],
		["SOLO A/- [D*,C*,B*] s0","not connecting"],
		["CONNECTING A/C [B*,D] s0","B away"],
		["PLAYING A/B [C,D] s0","not connecting"],
		["DECIDED A/B [C,D] s0","not connecting"],
		["READY A/B [C,D] s1 r:C","not connecting"],
	],
	'owner leave': [
		["EMPTY -/- [] s0","410 removed"],
		["EMPTY -/- [] s0","ok"],
		["EMPTY -/- [D*,C*,B*] s0","ok"],
		["CONNECTING C/B [D] s0","ok"],
		["CONNECTING C/B [D] s0","ok"],
		["CONNECTING C/B [D] s0","ok"],
		["CONNECTING C/B [D] s0","ok"],
	],
	'settings': [
		["EMPTY -/- [] s0","404 no_room,ok:NEW NAME/20000"],
		["SOLO A/- [] s0","404 no_room,ok:NEW NAME/20000"],
		["SOLO A/- [D*,C*,B*] s0","404 no_room,ok:NEW NAME/20000"],
		["CONNECTING A/B [C,D] s0","404 no_room,ok:NEW NAME/20000"],
		["PLAYING A/B [C,D] s0","404 no_room,ok:NEW NAME/20000"],
		["DECIDED A/B [C,D] s0","404 no_room,ok:NEW NAME/20000"],
		["READY A/B [C,D] s1 r:C","404 no_room,ok:NEW NAME/20000"],
	],
	'close': [
		["GONE","404 no_room,ok,404 no_room"],
		["GONE","404 no_room,ok,404 no_room,told"],
		["GONE","404 no_room,ok,404 no_room,told"],
		["GONE","404 no_room,ok,404 no_room,told"],
		["GONE","404 no_room,ok,404 no_room,told"],
		["GONE","404 no_room,ok,404 no_room,told"],
		["GONE","404 no_room,ok,404 no_room,told"],
	],
	'switch off': [
		["GONE","404 no_room,404 off,unlisted,404 no_room"],
		["GONE","404 no_room,404 off,unlisted,404 no_room,told"],
		["GONE","404 no_room,404 off,unlisted,404 no_room,told"],
		["GONE","404 no_room,404 off,unlisted,404 no_room,told"],
		["GONE","404 no_room,404 off,unlisted,404 no_room,told"],
		["GONE","404 no_room,404 off,unlisted,404 no_room,told"],
		["GONE","404 no_room,404 off,unlisted,404 no_room,told"],
	],
	'old /v1/join': [
		["EMPTY -/- [] s0","412 version,412 version,412 version,host:other code"],
		["SOLO A/- [] s0","412 version,412 version,412 version,host:other code"],
		["SOLO A/- [D*,C*,B*] s0","412 version,412 version,412 version,host:other code"],
		["CONNECTING A/B [C,D] s0","412 version,412 version,412 version,host:other code"],
		["PLAYING A/B [C,D] s0","412 version,412 version,412 version,host:other code"],
		["DECIDED A/B [C,D] s0","412 version,412 version,412 version,host:other code"],
		["READY A/B [C,D] s1 r:C","412 version,412 version,412 version,host:other code"],
	],
	'leave line': [
		["EMPTY -/- [] s0","410 removed"],
		["EMPTY -/- [] s0","ok pos-1 watcher w1"],
		["SOLO A/- [D*,C*] s0","ok pos-1 watcher w1"],
		["CONNECTING A/C [D] s0","ok pos-1 watcher w1"],
		["CONNECTING A/C [D] s0","ok pos-1 watcher w1"],
		["CONNECTING A/C [D] s1","ok pos-1 watcher w1"],   // (beta2: B, who conceded A's win, off the table: the win counted)
		["CONNECTING A/C [D] s1","ok pos-1 watcher w1"],
	],
	'join line': [
		["EMPTY -/- [] s0","410 removed,410 removed"],
		["SOLO A/- [] s0","not_now,ok pos0"],
		["SOLO A/- [D*,C*,B*] s0","not_now,ok pos3"],
		["CONNECTING A/C [D,B] s0","not_now,ok pos2"],
		["CONNECTING A/C [D,B] s0","not_now,ok pos2"],
		["CONNECTING A/C [D,B] s1","not_now,ok pos2"],   // (beta2: B, who conceded A's win, off the table: the win counted)
		["CONNECTING A/C [D,B] s1","not_now,ok pos2"],
	],
	'rejoin': [
		["SOLO A/- [] s0","new pos0"],
		["SOLO A/- [] s0","back pos0"],
		["SOLO A/- [D*,C*,B*] s0","back pos3"],
		["CONNECTING A/B [C,D] s0","back pos0"],
		["CONNECTING A/B [C,D] s0","back pos0"],
		["READY A/B [C,D] s1 r:C","back pos0"],
		["READY A/B [C,D] s1 r:C","back pos0"],
	],
	'/v1/online listing': [
		["EMPTY -/- [] s0","unlisted"],
		["SOLO A/- [] s0","waiting A/- s0 line0"],
		["SOLO A/- [D*,C*,B*] s0","waiting A/- s0 line3"],
		["CONNECTING A/B [C,D] s0","playing A/B s0 line2"],
		["PLAYING A/B [C,D] s0","playing A/B s0 line2"],
		["DECIDED A/B [C,D] s0","playing A/B s0 line2"],
		["READY A/B [C,D] s1 r:C","waiting A/B s1 line2"],
	],
};

for (const phase of PHASES) {
	const x = await setup(phase);
	check(`${phase}: set up`, show(x.code), START[phase]);
}
const got = {};
for (const [ev, run] of Object.entries(EVENTS)) {
	got[ev] = [];
	for (const phase of PHASES) {
		const x = await setup(phase);
		const answer = await run(x);
		got[ev].push([show(x.code), answer]);
		check(`${phase} x ${ev}`, [show(x.code), answer], T[ev] ? T[ev][PHASES.indexOf(phase)] : 'no row in the table');
	}
}
if (PRINT) for (const [ev, rows] of Object.entries(got)) { console.log(`\t'${ev}': [`); for (const r of rows) console.log('\t\t' + JSON.stringify(r) + ','); console.log('\t],'); }


// ---- six members, several matches in a row: the winner stays, the loser to the back, the streak counts, F AWAY passed over ----
{
	const A = await create('A', { listed: true });
	const code = A.code, M = { A };
	for (const n of ['B', 'C', 'D', 'E', 'F']) M[n] = await enter(code, n);
	const by = (mid) => Object.values(M).find((m) => m.mid === mid);
	// (a match between the two seated: both started, both report the side that won)
	async function match(win) {
		const r = rec(code);
		for (const side of ['a', 'b']) await act(by(r.t[side]), 'started');
		for (const side of ['a', 'b']) await act(by(r.t[side]), 'result', { winner: win, n: rec(code).t.n });
	}
	async function seated() { const r = rec(code); r.ready.deadline = Date.now() - 1; await alarm(code); }   // (GET READY's 3 s up)
	check('rotation: six in: B seated as it entered (an empty seat: at once), the rest in line', show(code), 'CONNECTING A/B [C,D,E,F] s0', false);
	check('rotation: every member READY from entering', M.F.state.you.ready, true, false);
	await act(M.F, 'away');
	check('rotation: F AWAY: kept in its place, marked', [show(code), M.C.state.line.map((p) => p.name + (p.ready ? '' : '*'))], ['CONNECTING A/B [C,D,E,F*] s0', ['C', 'D', 'E', 'F*']], false);
	await match('a');
	check('rotation: A beat B: A stays, streak 1, C getting ready (GET READY, 3 s)', [show(code), M.C.state.ready.you, M.C.state.ready.deadline_ms <= 3000], ['READY A/B [C,D,E,F*] s1 r:C', true, true], false);
	await seated();
	check('rotation: C seated by itself: B to the back, C in B\'s seat', show(code), 'CONNECTING A/C [D,E,F*,B] s1', false);
	await match('b');
	check('rotation: C beat A: C holds the streak, 1; D getting ready', show(code), 'READY A/C [D,E,F*,B] s1 r:D', false);
	await act(M.D, 'ready');
	check('rotation: D presses Start while getting ready: seated now', show(code), 'CONNECTING D/C [E,F*,B,A] s1', false);
	await match('b');
	check('rotation: C beat D: streak 2; E next', show(code), 'READY D/C [E,F*,B,A] s2 r:E', false);
	await seated();
	await match('b');
	check('rotation: C beat E: streak 3; F AWAY passed over, B getting ready', show(code), 'READY E/C [F*,B,A,D] s3 r:B', false);
	await act(M.B, 'away');
	check('rotation: B goes AWAY while getting ready: the next READY one (A) instead, B keeps its place', show(code), 'READY E/C [F*,B*,A,D] s3 r:A', false);
	await seated();
	await match('draw');
	check('rotation: a draw: C (the holder) stays, the streak as it was; D next', show(code), 'READY A/C [F*,B*,D,E] s3 r:D', false);
	await act(M.F, 'ready');
	check('rotation: F READY again: not before D, who was already getting ready', show(code), 'READY A/C [F,B*,D,E] s3 r:D', false);
	await seated();
	await match('a');
	check('rotation: D beat C: D holds the streak, 1 (C\'s 3 gone); F, first in line and READY, next', show(code), 'READY D/C [F,B*,E,A] s1 r:F', false);
	await seated();
	check('rotation: F seated', show(code), 'CONNECTING D/F [B*,E,A,C] s1', false);
	await act(M.F, 'away');
	check('rotation: F AWAY before its session started: back to the front of the line, AWAY; E seated', [show(code), rec(code).t.last.why], ['CONNECTING D/E [F*,B*,A,C] s1', 'away'], false);
	const r = rec(code);
	r.t.started = { a: true }; r.t.deadline = Date.now() - 1;
	await alarm(code);
	check('rotation: E never started (30 s): back to the front, AWAY, not let go; A seated', [show(code), !!rec(code).members[M.E.mid], rec(code).t.last.why], ['CONNECTING D/A [E*,F*,B*,C] s1', true, 'connect'], false);
	check('rotation: E told: AWAY', M.E.state.you.ready, false, false);
	await match('a');
	check('rotation: D beat A: streak 2; C, the only READY one, next', show(code), 'READY D/A [E*,F*,B*,C] s2 r:C', false);
	await seated();
	for (const m of [M.D, M.C]) await act(m, 'started');
	await act(M.A, 'away');
	await act(M.C, 'away');   // (at the table, playing: AWAY from when it's back in the line)
	await match('a');
	check('rotation: nobody READY in line: the two play on (a rematch), as with an empty line', [show(code), rec(code).ready], ['PLAYING D/C* [E*,F*,B*,A*] s3', null], false);
	check('rotation: what each member sees: the table, the line in order with READY, their own place', [M.E.state.table.a.name, M.E.state.table.b.name, M.E.state.table.streak, M.E.state.line.map((p) => p.name + (p.ready ? '' : '*')), M.E.state.you.pos, M.E.state.you.ready],
		['D', 'C', 3, ['E*', 'F*', 'B*', 'A*'], 1, false], false);
	check('rotation: the matches counted (n)', rec(code).t.n, 11, false);
	for (const m of [M.D, M.C]) await act(m, 'started', { n: 11 });   // (the play-on's match begun: both games say so)
	await act(M.A, 'leave');
	await act(M.E, 'ready');
	await match('a');
	check('rotation: E READY again: it plays next', show(code), 'READY D/C* [E,F*,B*] s4 r:E', false);
}

// ---- both saying disconnect twice in a row (both alive): the two seated again once, then the holder stays; one-sided
// (the other alive and silent): its word taken ----
{
	const A = await create('A'), B = await enter(A.code, 'B'), C = await enter(A.code, 'C');
	await act(B, 'ready');
	for (const m of [A, B]) await act(m, 'started');
	for (const m of [A, B]) await act(m, 'result', { winner: 'disconnect' });
	check('disconnect once: the two seated again, cold (a new token)', [show(A.code), rec(A.code).t.last], ['CONNECTING A/B [C] s0', { result: 'void', why: 'disconnect', n: 1 }]);
	for (const m of [A, B]) await act(m, 'started');
	for (const m of [A, B]) await act(m, 'result', { winner: 'disconnect' });
	check('disconnect twice: the holder (A) stays, B out when C says ready', [show(A.code), rec(A.code).t.last.result], ['READY A/B [C] s0 r:C', 'void']);
	rec(A.code).ready.deadline = Date.now() - 1;
	await alarm(A.code);
	for (const m of [A, C]) await act(m, 'started');
	await act(C, 'result', { winner: 'b' });   // (C wins; A, the holder, never reports)
	rec(A.code).t.deadline = Date.now() - 1;
	await alarm(A.code);
	check('one-sided (the holder silent, alive): the challenger\'s win taken', [show(A.code), rec(A.code).t.last], ['READY A/C [B] s1 r:B', { result: 'b', n: 3 }]);
	rec(A.code).ready.deadline = Date.now() - 1;
	await alarm(A.code);
	const stale = await act(C, 'result', { winner: 'b', n: 3 });   // (C, seated again with B: its last match's report again)
	{   // (a desync seen by one side, the other's game cut off: a desync, void, the two seated again cold, not a disconnect)
		const X = await create('X'), Y = await enter(X.code, 'Y');
		await act(Y, 'ready');
		for (const m of [X, Y]) await act(m, 'started');
		await act(X, 'result', { winner: 'desync' });
		await act(Y, 'result', { winner: 'disconnect' });
		check('desync on one side, the connection lost on the other: void desync, seated again cold', [show(X.code), rec(X.code).t.last, rec(X.code).t.reseated], ['CONNECTING X/Y [] s0', { result: 'void', why: 'desync', n: 1 }, false]);
	}
	check('a late report for an earlier match: refused as stale', said(stale), 'stale');
}

// ---- the seat: the session's host (side a) gets the watch key, the guest the host's addresses; one watch id for the
// room's life, the Watch taking a new match on it after every seat change; seamless only when both have "cut1" ----
function start_bytes(match) {
	const m = new TextEncoder().encode(JSON.stringify({ p1: 'A', p2: 'B' }));
	const b = new Uint8Array(20 + m.length + 4), v = new DataView(b.buffer);
	v.setUint32(0, 0x31574656, true); b[4] = 1; b[5] = 1;
	v.setUint32(6, Number(match & 0xffffffffn), true); v.setUint32(10, Number(match >> 32n), true);
	v.setUint16(14, m.length, true); b.set(m, 16); v.setUint32(16 + m.length, 4, true);
	return b;
}
{
	const A = await create('A', { caps: ['cut1'] }), B = await enter(A.code, 'B', { caps: ['cut1', 'x'] }), C = await enter(A.code, 'C', { caps: [] });
	check('watch: the room\'s watch id is its code, said to every member', [A.wid, B.state.watch], [A.code, A.code]);
	await act(B, 'ready');
	const sa = A.state.you.seat, sb = B.state.you.seat;
	check('seat: A is side a, the session\'s host, with the watch key; B the guest, without', [sa.role, sa.side, !!sa.wkey, sb.role, sb.side, 'wkey' in sb], ['host', 'a', true, 'guest', 'b', false]);
	check('seat: each has the other\'s name and addresses, the same token and terms', [sa.peer.name, sb.peer.name, sb.peer.cands.length > 0, sa.token === sb.token, sa.as, sb.them], ['B', 'A', true, true, V, V]);
	check('seat: seamless, both have cut1', [sa.seamless, sb.seamless], [true, true]);
	check('seat: C in line has no seat, sees its place', [C.state.you.seat, C.state.you.pos], [null, 1]);
	const s1 = await call('POST', `/v1/watch/start?w=${A.wid}&k=${sa.wkey}`, start_bytes(0x1111n));
	check('watch: the host\'s match taken by the Watch on the room\'s watch id', s1.status, 200);
	for (const m of [A, B]) await act(m, 'started');
	for (const m of [A, B]) await act(m, 'result', { winner: 'a' });
	await act(C, 'ready');
	const s2 = A.state.you.seat;
	check('seat change: A still the host, a new watch key and token; seamless off (C has no cut1)', [s2.role, s2.peer.name, s2.wkey !== sa.wkey, s2.token !== sa.token, s2.seamless], ['host', 'C', true, true, false]);
	const old_key = await call('POST', `/v1/watch/start?w=${A.wid}&k=${sa.wkey}`, start_bytes(0x2222n));
	const s3 = await call('POST', `/v1/watch/start?w=${A.wid}&k=${s2.wkey}`, start_bytes(0x3333n));
	check('watch: the last seat\'s key refused, the new one\'s match taken on the same watch id', [old_key.status, s3.status], [404, 200]);
	const seen = await call('POST', `/v1/watch/seen?w=${A.wid}&s=00000000000000ab`, undefined, '198.51.97.1');
	check('watch: a watcher on the watch id gets the new match', [seen.status, seen.body.match], [200, '0000000000003333']);
	// (C beats A: C (side b) holds the streak; next a newcomer sits side a and is the session's host)
	for (const m of [A, C]) await act(m, 'started');
	for (const m of [A, C]) await act(m, 'result', { winner: 'b' });
	await act(B, 'ready');
	check('seat change: B in A\'s seat (side a) is now the session\'s host, with the key', [show(A.code), B.state.you.seat.role, !!B.state.you.seat.wkey, 'wkey' in C.state.you.seat], ['CONNECTING B/C [A] s1', 'host', true, false]);
	const s4 = await call('POST', `/v1/watch/start?w=${A.wid}&k=${B.state.you.seat.wkey}`, start_bytes(0x4444n));
	check('watch: B\'s match taken on the same watch id', s4.status, 200);
	env.SWITCH_WATCH = 'off';
	for (const m of [B, C]) await act(m, 'started');
	for (const m of [B, C]) await act(m, 'result', { winner: 'b' });
	await act(A, 'ready');
	check('watching switched off: no watch key at the next seat, no watch id said', [A.state.you.seat.role, 'wkey' in A.state.you.seat, A.state.watch], ['host', false, '']);
	delete env.SWITCH_WATCH;
}

// ---- the socket: what a member sends on it, what it's told ----
{
	const A = await create('A'), B = await enter(A.code, 'B');
	B.ws.send('ready');
	await settle();
	check('socket: "ready" from the one called seats them; both told', [show(A.code), A.state.table.phase, B.state.table.phase], ['CONNECTING A/B [] s0', 'CONNECTING', 'CONNECTING']);
	A.ws.send(JSON.stringify({ op: 'started' }));
	B.ws.send(JSON.stringify({ op: 'started' }));
	await settle();
	A.ws.send(JSON.stringify({ op: 'result', winner: 'b' }));
	B.ws.send(JSON.stringify({ op: 'result', winner: 'b' }));
	await settle();
	check('socket: started and results on it: B won, no line: they play on (a rematch)', [show(A.code), A.state.table.streak, A.state.table.holder, A.state.table.last], ['PLAYING A/B [] s1', 1, 'b', { result: 'b', n: 1 }]);
	A.ws.send(JSON.stringify({ op: 'join_line' }));
	await settle();
	check('socket: a refusal said on it', A.said[A.said.length - 1], { error: 'not_now', op: 'join_line' });
	A.ws.send('x'.repeat(5000));
	A.ws.send('{"op":"nothing"}');
	await settle();
	check('socket: junk ignored', show(A.code), 'PLAYING A/B [] s1');
	const bad = await socket_for({ ...B, mkey: 'f'.repeat(32) });
	check('socket: a wrong key refused', bad, 404);
	env.SWITCH_WEBSOCKETS = 'off';
	const off_ws = await socket_for({ ...B });
	delete env.SWITCH_WEBSOCKETS;
	check('socket: WebSockets switched off: refused (the game polls)', off_ws, 503);
	B.ws.send(JSON.stringify({ op: 'leave' }));
	await settle();
	check('socket: leave on it: forfeit, A alone (the streak was B\'s: A starts from 0)', [show(A.code), B.ws.open], ['SOLO A/- [] s0', false]);
}

// ---- liveness: polling members stay; one quiet 30 s goes; a socket quiet 90 s goes; an empty room closes a minute later ----
{
	const A = await create('A', {}, false), B = await enter(A.code, 'B', {}, false);
	await act(B, 'ready');
	for (const m of [A, B]) await act(m, 'started');
	for (let i = 0; i < 9; i++) { advance(10000); await act(A, 'poll'); await act(B, 'poll'); await alarm(A.code); }
	check('liveness: two polling every 10 s, 90 s on: both there, playing', [show(A.code), Object.keys(rec(A.code).members).length], ['PLAYING A/B [] s0', 2]);
	for (let i = 0; i < 3; i++) { advance(10500); await act(A, 'poll'); await alarm(A.code); }
	check('liveness: B quiet 31 s: gone (a forfeit); A, polling, stays', [show(A.code), said(await act(B, 'poll'))], ['SOLO A/- [] s0', '410 removed']);
	const S = await enter(A.code, 'S');
	await act(S, 'ready');
	for (const m of [A, S]) await act(m, 'started');
	for (let i = 0; i < 8; i++) { advance(10000); await act(A, 'poll'); await alarm(A.code); }
	check('liveness: a socket with no ping for 80 s: still there', [show(A.code), S.ws.open], ['PLAYING A/S [] s0', true]);
	for (let i = 0; i < 2; i++) { advance(10000); await act(A, 'poll'); await alarm(A.code); }
	check('liveness: ... 100 s: closed, its member gone', [show(A.code), S.ws.open], ['SOLO A/- [] s0', false]);
	await act(A, 'leave');
	check('empty: the last member gone, the room still there', show(A.code), 'EMPTY -/- [] s0');
	advance(30000); await alarm(A.code);
	const back = await enter(A.code, 'Z', {}, false);
	advance(20000); await act(back, 'poll'); advance(25000); await act(back, 'poll'); await alarm(A.code);
	check('empty: someone in within the minute: the room stays', show(A.code), 'SOLO Z/- [] s0');
	await act(back, 'leave');
	advance(59000); await alarm(A.code);
	check('empty: 59 s empty: still there', show(A.code), 'EMPTY -/- [] s0');
	advance(2000); await alarm(A.code);
	check('empty: 61 s empty: closed', [show(A.code), said(await call('POST', '/v2/room/enter', { ...me('Y'), code: A.code }, '198.51.96.1'))], ['GONE', '404 no_room']);
	const again = await call('POST', '/v1/host', me('H', { want: A.code }), '198.51.96.2');
	check('empty: its code not handed to a /v1 host for a while', again.body.code !== A.code, true);
}

// ---- rejoin: a game that crashed (or was restarted) enters again with its mid and mkey within its 30 s: its place kept;
// at the table mid-match, the two seated again cold with its new addresses; later, or with a wrong key, a new member ----
{
	const A = await create('A'), B = await enter(A.code, 'B'), C = await enter(A.code, 'C'), D = await enter(A.code, 'D');
	await act(B, 'ready');
	for (const m of [A, B]) await act(m, 'started');
	C.ws.close(1006);
	await settle();
	advance(20000);
	await alarm(A.code);
	const back = await call('POST', '/v2/room/enter', { ...me('C', { cands: ['203.0.113.251:7702'] }), code: A.code, mid: C.mid, mkey: C.mkey }, C.ip);
	check('rejoin: C back 20 s after its crash: the same member, its place (1st in line)', [back.body.mid === C.mid, back.body.back, back.body.state.you.pos, show(A.code)], [true, true, 1, 'PLAYING A/B [C,D] s0']);
	check('rejoin: no new member made', Object.keys(rec(A.code).members).length, 4);
	await socket_for(C);
	const n0 = rec(A.code).t.n, token0 = rec(A.code).t.token;
	B.ws.close(1006);
	await settle();
	const bb = await call('POST', '/v2/room/enter', { ...me('B', { cands: ['203.0.113.252:7703'] }), code: A.code, mid: B.mid, mkey: B.mkey }, B.ip);
	await socket_for(B);
	const seat = A.state.you.seat;
	check('rejoin: B back mid-match: the two seated again cold (a new token, n+1), A told B\'s new address', [bb.body.back, show(A.code), rec(A.code).t.n === n0 + 1, rec(A.code).t.token !== token0, seat && seat.peer.cands[0], rec(A.code).t.last],
		[true, 'CONNECTING A/B [C,D] s0', true, true, '203.0.113.252:7703', { result: 'void', why: 'rejoin', n: n0 }]);
	const wrong = await call('POST', '/v2/room/enter', { ...me('D'), code: A.code, mid: D.mid, mkey: 'f'.repeat(32) }, D.ip);
	check('rejoin: a wrong key: a new member (at the back), D still in its place', [wrong.body.mid !== D.mid, !!wrong.body.back, wrong.body.state.you.pos, rec(A.code).line.length], [true, false, 3, 3]);
	D.ws.close(1006);
	await settle();
	advance(31000);
	await alarm(A.code);
	const late = await call('POST', '/v2/room/enter', { ...me('D'), code: A.code, mid: D.mid, mkey: D.mkey }, '198.51.94.9');
	check('rejoin: D 31 s after its crash: gone, its place lost: a new member at the back', [late.body.mid !== D.mid, !!late.body.back, late.body.state.you.pos], [true, false, 2]);
}

// ---- a watcher member (Leave line): mid-match, a forfeit; still told every change, counted in the room, never called;
// the streak each match is played for (table.match), as it was when the match began ----
{
	const A = await create('A'), B = await enter(A.code, 'B'), C = await enter(A.code, 'C');
	await act(B, 'ready');
	for (const m of [A, B]) await act(m, 'started');
	await act(A, 'result', { winner: 'a' });
	await act(B, 'result', { winner: 'a' });
	check('match: the next pair\'s streak not yet; the last match\'s as it began', [rec(A.code).t.streak, A.state.table.match], [1, { n: 1, streak: 0, holder: 'A' }]);
	await act(C, 'ready');
	check('match: the new pair\'s match is played for the holder\'s 1', A.state.table.match, { n: 2, streak: 1, holder: 'A' });
	for (const m of [A, C]) await act(m, 'started');
	const left = await act(C, 'leave_line');
	check('watcher: C leaves mid-match: a forfeit, A stays, C still a member', [show(A.code), rec(A.code).t.last.result, !!rec(A.code).members[C.mid], left.body.state.you.pos], ['CONNECTING A/B [] s1', 'forfeit', true, -1]);
	check('watcher: never got ready nor seated', [C.state.you.pos, !!(C.state.ready && C.state.ready.you)], [-1, false]);
	const before = C.said.length;
	for (const m of [A, B]) await act(m, 'started');
	check('watcher: told of every change on its socket', C.said.length > before, true);
	advance(16000); await alarm(A.code);
	check('watcher: still a member, passed by', [!!rec(A.code).members[C.mid], show(A.code)], [true, 'PLAYING A/B [] s1']);
	await act(C, 'join_line');
	check('watcher: Join the line: at its back', [C.state.you.pos, C.state.line.map((p) => p.name)], [1, ['C']]);
}

// ---- the limits: a room's members, from one address, names, who acts for whom, games and versions ----
{
	const A = await create('A');
	const one = '198.51.95.1';
	const same = [];
	for (let i = 0; i < 9; i++) same.push(await enter(A.code, 'P' + i, {}, false, one));
	check('limits: 8 members from one address, the 9th refused full', same.map((m) => m.status), [200, 200, 200, 200, 200, 200, 200, 200, 409]);
	const many = [];
	for (let i = 0; i < 27; i++) many.push(await enter(A.code, 'Q' + i, {}, false));
	check('limits: 34 members at most (the table\'s 2 and a line of 32), the 35th full', [Object.keys(rec(A.code).members).length, many.slice(-3).map((m) => m.status)], [34, [200, 409, 409]]);
	const R = await create('A');
	const named = await enter(R.code, 'SHITHEAD');
	check('limits: a blocked name shown as PLAYER', [named.state.table.b.name, rec(R.code).members[named.mid].who.name], ['PLAYER', 'PLAYER']);
	const long = await enter(R.code, 'ABCDEFGHIJKLMNOP');
	check('limits: a name cut to 10', rec(R.code).members[long.mid].who.name, 'ABCDEFGHIJ');
	const forged = await call('POST', '/v2/room/ready', { code: R.code, mid: named.mid, mkey: long.mkey }, long.ip);
	const leave_other = await call('POST', '/v2/room/leave', { code: R.code, mid: named.mid }, long.ip);
	check('limits: no member acts for another (another\'s id, one\'s own key; no key): as a code with no room', [said(forged), said(leave_other), !!rec(R.code).members[named.mid]], ['404 no_room', '404 no_room', true]);
	const not_owner = await call('POST', '/v2/room/close', { code: R.code, mid: R.mid, mkey: R.mkey });
	check('limits: close without the owner key refused, even the owner\'s member key', [said(not_owner), show(R.code) !== 'GONE'], ['403 not_owner', true]);
	const game = await call('POST', '/v2/room/enter', { ...me('G', { game: 'vf2' }), code: R.code }, '198.51.95.2');
	const rom = await call('POST', '/v2/room/enter', { ...me('G', { content: 'fedcba9876543210' }), code: R.code }, '198.51.95.3');
	const ver = await call('POST', '/v2/room/enter', { ...me('G', { version: 'v9', pool: 'v9' }), code: R.code }, '198.51.95.4');
	check('limits: another game, ROM, version: refused as a /v1 join is', [said(game), said(rom), said(ver), ver.body.host_version], ['412 game', '412 rom', '412 version', V]);
	const caps = await enter(R.code, 'K', { caps: ['cut1', 'BAD CAP', 7, 'a'.repeat(17), 'x2', 'y', 'z', 'w', 'v', 'u', 't'] });
	check('limits: caps: 8 at most, short lower-case words only', rec(R.code).members[caps.mid].caps, ['cut1', 'x2', 'y', 'z', 'w']);
	const v1 = await call('POST', '/v1/host', me('H1'), '198.51.95.5');
	const into_v1 = await call('POST', '/v2/room/enter', { ...me('N'), code: v1.body.code }, '198.51.95.6');
	const poll_v1 = await call('POST', '/v2/room/poll', { code: v1.body.code, mid: 'aaaaaaaa', mkey: 'b'.repeat(32) }, '198.51.95.6');
	check('a /v1 room\'s code entered by a new build: 412 room_version (it joins with /v1/join); its other calls no_room', [said(into_v1), into_v1.body.room_version, said(poll_v1)], ['412 room_version', 1, '404 no_room']);
	const none = await call('POST', '/v2/room/enter', { ...me('N'), code: 'ZZZZZZ' }, '198.51.95.7');
	const junk = await call('POST', '/v2/room/nothing', {}, '198.51.95.7');
	const get = await call('GET', '/v2/room/poll', undefined, '198.51.95.7');
	check('no room by that code; an unknown op; a GET', [said(none), said(junk), said(get)], ['404 no_room', '404 bad', '404 bad']);
	const ready_ms = await call('POST', '/v2/room/settings', { code: R.code, owner_key: R.owner_key, ready_ms: 999999, listed: true, locked: true });
	check('settings: ready_ms kept within 5-60 s; locked only when listed', [ready_ms.body.room.ready_ms, ready_ms.body.room.locked], [60000, true]);
	const unlist = await call('POST', '/v2/room/settings', { code: R.code, owner_key: R.owner_key, listed: false });
	check('settings: unlisted, no longer locked', [unlist.body.room.listed, unlist.body.room.locked], [false, false]);
	const made = await call('POST', '/v2/room/create', me('M', { ready_ms: 100 }), '198.51.95.8');
	check('create: ready_ms at least 5 s; the owner key and the member\'s', [made.body.state.room.ready_ms, made.body.owner_key.length, made.body.mkey.length, made.body.state.room.owner], [5000, 32, 32, true]);
}

// ---- the rooms list: Rooms 2 rows as an older build's list reads them (online.cpp's rooms_list, the online page): skipped ----
{
	const A = await create('LIST', { listed: true, room: 'two rooms' }), B = await enter(A.code, 'MATE');
	await act(B, 'ready');
	const L = await create('LOCKED', { listed: true, locked: true });
	await told(A.code, L.code);
	const d = (await call('GET', '/v1/online')).body;
	const row = d.rooms.find((y) => y.v === 2 && y.room2 && y.room2.code === A.code), lrow = d.rooms.find((y) => y.v === 2 && y.table[0] && y.table[0].name === 'LOCKED');
	check('list: a Rooms 2 row: v 2, its name, the table\'s two, the streak, the line, its code and watch id under room2', [row.v, row.name, row.table, row.streak, row.line, row.state, row.room2.code],
		[2, 'TWO ROOMS', [{ name: 'LIST', country: 'KR' }, { name: 'MATE', country: 'KR' }], 0, 0, 'playing', A.code]);
	check('list: a locked one: no code', [lrow.locked, 'room2' in lrow], [true, false]);
	check('list: features say rooms2', d.features.rooms2, true);
	// (online.cpp: rows of this game; the code (6 letters, not a Quick match's) and the watch code; none of the three nor
	// locked: skipped; then p1's name empty, or playing without p2's: skipped)
	const old_reads = (r) => {
		const code = typeof r.code === 'string' && r.code.length === 6 && !r.random ? r.code : '', watch = typeof r.watch === 'string' && r.watch.length === 6 ? r.watch : '';
		if (r.random && (r.state !== 'playing' || !watch)) return false;
		if (!code && !watch && r.locked !== true) return false;
		const n1 = (r.p1 && r.p1.name) || '', n2 = (r.p2 && r.p2.name) || '';
		return !(!n1 || (r.state === 'playing' && !n2));
	};
	const v2rows = d.rooms.filter((y) => y.v === 2);
	check('list: an older build reads no Rooms 2 row (each skipped), the others as before', [v2rows.length >= 2, v2rows.filter(old_reads).length], [true, 0]);
	const count = d.games.vf3;
	check('count: the members counted among the game\'s players (in a room\'s match, or waiting at the table)', count.total >= 3 && count.room >= 2, true);
	env.SWITCH_ROOMS = 'off';
	const off = await call('POST', '/v2/room/create', me('X'), '198.51.94.1');
	delete env.SWITCH_ROOMS;
	check('SWITCH_ROOMS off turns Rooms 2 off too', said(off), '404 off');
	delete env.SWITCH_ROOMS2;
	const unset = await call('POST', '/v2/room/create', me('X'), '198.51.94.1');
	const d2 = (await call('GET', '/v1/online')).body;
	check('SWITCH_ROOMS2 unset: off (it\'s on only when "on"); none listed, no rooms2 feature', [said(unset), d2.rooms.some((y) => y.v === 2), 'rooms2' in d2.features], ['404 off', false, false]);
	env.SWITCH_ROOMS2 = 'On ';
	check('SWITCH_ROOMS2 "On ": on', said(await call('POST', '/v2/room/poll', { code: A.code, mid: A.mid, mkey: A.mkey }, A.ip)), 'ok');
}

// ---- a beta and the release online together (the Rooms 2 beta: the live lobby with SWITCH_ROOMS2 on, release builds
// and beta builds on it at once): a room made by a beta names its version to an older build given its code ----
{
	env.SWITCH_ROOMS2 = 'on';
	const B = await create('BETA', { build: 'v0.1.23-alpha-beta1', listed: true });
	const j = await call('POST', '/v1/join', old_game('REL', { code: B.code }), '198.51.97.1');
	const q = await call('POST', '/v1/join', old_game('RELQ', { code: B.code, queue: true }), '198.51.97.2');
	const p = await call('POST', '/v1/peek', { code: B.code }, '198.51.97.3');
	check('beta: a beta\'s room\'s code, an older build\'s join, line and peek: 412 version naming the room\'s version (its "The host has v0.1.23-alpha-beta1")',
		[j, q, p].map((y) => [y.status, y.body.error, y.body.host_version, y.body.room_version]), [0, 1, 2].map(() => [412, 'version', 'v0.1.23-alpha-beta1', 2]));
	for (const [build, want] of [['v1.9.0-beta1', 'v1.9.0-beta1'], ['v0.1.24', 'v0.1.24'], ['v0.1.23-alpha', 'v0.1.23-alpha'], ['v1.9.0 beta', undefined], ['<b>v1</b>', undefined],
		['v1.9.0-BETA1', undefined], [123, undefined], ['v1.9.0-' + 'a'.repeat(41), undefined], ['v0.1.7-online-preview+host1', undefined], ['', undefined]]) {
		const x = await create('BUILD', { build });
		const y = await call('POST', '/v1/join', old_game('REL', { code: x.code }), '198.51.97.4');
		check('beta: the creator\'s build ' + JSON.stringify(build) + ': told an older build as ' + (want || 'nothing (another version)'), [y.status, y.body.error, y.body.host_version], [412, 'version', want]);
		await act(x, 'leave');
	}
	// (a beta's other members, entered: nothing of theirs said; a release build's own /v1 room entered by a beta: joined as one)
	const M = await enter(B.code, 'MATE', { build: 'v9.9.9' });
	const p2 = await call('POST', '/v1/peek', { code: B.code }, '198.51.97.5');
	check('beta: the room\'s version is its creator\'s, not a later member\'s', [M.status, p2.body.host_version], [200, 'v0.1.23-alpha-beta1']);
	const rel = await call('POST', '/v1/host', old_game('RELH'), '198.51.97.6');
	const into = await call('POST', '/v2/room/enter', { ...me('BETA2', { build: 'v0.1.23-alpha-beta1' }), code: rel.body.code }, '198.51.97.7');
	const joined = await call('POST', '/v1/join', me('BETA2', { code: rel.body.code }), '198.51.97.7');
	check('beta: a release build\'s /v1 room entered by a beta: 412 room_version, then its /v1/join plays it (the release\'s version, the same online version)',
		[said(into), into.body.room_version, joined.status, joined.body.as], ['412 room_version', 1, 200, 'v0.1.7-online-preview']);
	// (the rooms list a release build reads: the beta's Rooms 2 room isn't in it; the count has the beta's members)
	const d = (await call('GET', '/v1/online')).body;
	check('beta: the list as a release build reads it: no Rooms 2 row; the beta\'s players counted among the game\'s', [d.rooms.some((y) => y.v === 2 && y.p1), d.games.vf3.total >= 2], [false, true]);
}

console.log(`\nroom2: ${checks - fails} of ${checks} checks passed`);
Date.now = real_now;
process.exit(fails ? 1 : 0);
