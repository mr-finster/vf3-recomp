// The players' counts (GET /v1/online's games), checked against who is really there, in a mix of everything the
// games do: Quick match searchers (new builds and older ones), Quick matches, rooms waiting (by code, listed),
// matches in rooms, a room that stays with players in its line, watchers, VF1, VF2, VF3 and Team Battle, older
// builds that send none of the rooms fields. Then time: a match in a room that stays going past 10 minutes, and
// everyone gone. What counts as online: every player the lobby knows is there (searching, hosting, in a room's line,
// in a match), each once a game; watchers apart (games[g].watching: watching, not playing; never in total).
// And the games' "N online" by Quick match (POST /v1/random/count): that same total, the game's own, whatever build asks;
// the queue's own count when the Stats can't be reached.
//   cd tools/lobby && npm test   (node test/counts.test.mjs src/index.js; COUNTS_OUT=file writes the mix's /v1/online)
import { lobby, settle, queued } from './standin.mjs';
import { writeFileSync } from 'node:fs';
const { worker, env } = await lobby(process.argv[2]);
const real_now = Date.now;
let skew = 0;
Date.now = () => real_now() + skew;
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
	return { status: r.status, body: type.includes('json') ? await r.json() : null };
}
const online = async () => { await settle(); return (await call('GET', '/v1/online')).body; };
const C = '0123456789abcdef', V3 = 'v0.1.7-online-preview+host1', V2 = 'v0.1.16+mix1';
let ip = 1;
const addr = () => '198.51.' + (100 + (ip >> 8)) + '.' + (ip++ & 255);
// a player: a new build (pool, accepts, watchable), or an older one's (none of them: what a build before rooms sends)
const vf3 = (name, extra = {}) => ({ game: 'vf3', version: V3, content: C, name, cands: ['203.0.113.7:7700'], relay: '', pool: V3, accepts: ['stage', 'dural3', 'dural2'], watchable: true, from: addr(), ...extra });
const tb = (name, extra = {}) => vf3(name, { game: 'vf3tb', accepts: ['stage', '1v1', 'dural3', 'dural'], ...extra });
const vf3_old = (name) => ({ game: 'vf3', version: 'v0.1.7-online-preview', content: C, name, cands: ['203.0.113.7:7700'], relay: '', from: addr() });
const vf1 = (name, extra = {}) => ({ game: 'vf1', version: 'v1.5.0', content: C, name, cands: ['203.0.113.7:7700'], relay: '', from: addr(), ...extra });   // (v1.5.x and v1.6.0: the same NET_VERSION)
const vf2_old = (name) => ({ game: 'vf2', version: 'v0.1.16', content: C, name, cands: ['203.0.113.7:7700'], relay: '', from: addr() });
const vf2 = (name, extra = {}) => ({ game: 'vf2', version: V2, content: C, name, cands: ['203.0.113.7:7700'], relay: '', pool: V2, accepts: [], watchable: true, from: addr(), ...extra });

const MAGIC = 0x31574656;
function head(kind, match, extra) {
	const b = new Uint8Array(14 + extra), v = new DataView(b.buffer);
	v.setUint32(0, MAGIC, true); b[4] = kind; b[5] = 1;
	v.setUint32(6, Number(match & 0xffffffffn), true); v.setUint32(10, Number(match >> 32n), true);
	return { b, v };
}
function start_bytes(match, meta) {
	const m = new TextEncoder().encode(JSON.stringify(meta)), blob = new Uint8Array(64);
	const { b, v } = head(1, match, 2 + m.length + 4 + blob.length);
	v.setUint16(14, m.length, true); b.set(m, 16); v.setUint32(16 + m.length, blob.length, true); b.set(blob, 20 + m.length);
	return b;
}
function push_bytes(match, first, n, info) {
	const { b, v } = head(2, match, 8 + n * 4 + 2 + 5);
	v.setUint32(14, first, true); v.setUint32(18, n, true);
	let at = 22;
	for (let f = first; f < first + n; f++) { v.setUint16(at, f & 0xffff, true); v.setUint16(at + 2, f & 0xffff, true); at += 4; }
	v.setUint16(at, 0, true); at += 2;
	b.set(info, at);
	return b;
}
let match_no = 1n;
// a room's match sent to be watched (its host's game does this for every new build's room), and so many watching it
async function watched(wid, wkey, p1, p2, watchers) {
	const M = 0x5000000000000000n + match_no++;
	await call('POST', `/v1/watch/start?w=${wid}&k=${wkey}`, start_bytes(M, { p1, p2, c1: 'KR', c2: 'JP' }));
	await call('POST', `/v1/watch/push?w=${wid}&k=${wkey}`, push_bytes(M, 0, 600, [1, 0, 2, 1, 0]));
	for (let i = 0; i < watchers; i++) await call('POST', `/v1/watch/seen?w=${wid}&s=${(0xa000000000000000n + match_no * 100n + BigInt(i)).toString(16)}`, undefined, addr());
	await call('POST', `/v1/watch/push?w=${wid}&k=${wkey}`, push_bytes(M, 600, 60, [1, 0, 2, 1, 0]));   // (the host's next upload: the rooms list told who's watching)
}
const searching = async (p) => (await call('POST', '/v1/random', p, p.from)).body.ticket;
async function quick_pair(a, b) {
	const ra = await call('POST', '/v1/random', a, a.from);
	const wait = call('POST', '/v1/random/wait', { ...a, ticket: ra.body.ticket }, a.from);
	await queued(env, ra.body.ticket);
	const rb = await call('POST', '/v1/random', b, b.from);
	const wa = (await wait).body;
	return wa.role === 'host' ? { host: wa, hf: a.from } : { host: rb.body, hf: b.from };
}
const host = async (p, extra = {}) => ({ ...(await call('POST', '/v1/host', { ...p, ...extra }, p.from)).body, from: p.from });
const join = (code, p, extra = {}) => call('POST', '/v1/join', { ...p, code, ...extra }, p.from);

// ---- the mix ----
const truth = { vf1: 0, vf2: 0, vf3: 0, vf3tb: 0 }, watching = { vf1: 0, vf2: 0, vf3: 0, vf3tb: 0 }, listed = { vf1: 0, vf2: 0, vf3: 0, vf3tb: 0 };
const keep = [];   // (rooms whose hosts go on saying they're there, and the lines' players asking again)
// VF3 (Revision D): 2 new builds and an older one searching
for (const p of [vf3('SEEK1'), vf3('SEEK2', { pool: V3 }), vf3_old('OLDSEEK')]) { await searching(p); truth.vf3++; }
// a Quick match on
await quick_pair(vf3('QMA'), vf3('QMB')); truth.vf3 += 2;
// a room by code waiting, and one playing (an older build's: no rooms fields)
await host(vf3_old('PRIVWAIT')); truth.vf3++;
{ const h = await host(vf3_old('PRIVHOST')); await join(h.code, vf3_old('PRIVGUEST')); truth.vf3 += 2; }
// a listed room that stays, its match on, two in its line, three watching
{
	const h = await host(vf3('STAYHOST'), { listed: true, persist: true, room: 'FRIDAY' });
	await join(h.code, vf3('STAYGUEST'));
	const line = [vf3('LINE1'), vf3('LINE2')];
	for (const p of line) await join(h.code, p, { queue: true });
	await watched(h.wid, h.wkey, 'STAYHOST', 'STAYGUEST', 3);
	truth.vf3 += 4; watching.vf3 += 3; listed.vf3++;
	keep.push({ h, line });
}
// a listed room waiting
await host(vf3('OPEN1'), { listed: true }); truth.vf3++; listed.vf3++;
// an unlisted room that stays (Create room, not listed), its match on, one in its line
{
	const h = await host(vf3('UNLHOST'), { persist: true });
	await join(h.code, vf3('UNLGUEST'));
	const line = [vf3('UNLLINE')];
	for (const p of line) await join(h.code, p, { queue: true });
	truth.vf3 += 3;
	keep.push({ h, line });
}
// Team Battle: one searching, a listed room's match (one room, not kept: its match listed from its Watch) with one watching, a listed room waiting
await searching(tb('TBSEEK')); truth.vf3tb++;
{ const h = await host(tb('TBHOST'), { listed: true }); await join(h.code, tb('TBGUEST')); await watched(h.wid, h.wkey, 'TBHOST', 'TBGUEST', 1); truth.vf3tb += 2; watching.vf3tb += 1; listed.vf3tb++; }
await host(tb('TBOPEN'), { listed: true }); truth.vf3tb++; listed.vf3tb++;
// VF1: v1.5.x (no rooms fields) searching, a room waiting, a match by code; v1.6.0: a listed room that stays, its match on, one in line
await searching(vf1('V1SEEK')); truth.vf1++;
await host(vf1('V1WAIT')); truth.vf1++;
{ const h = await host(vf1('V1HOST')); await join(h.code, vf1('V1GUEST')); truth.vf1 += 2; }
{
	const h = await host(vf1('V16HOST', { watchable: true, pool: '', accepts: [] }), { listed: true, persist: true });
	await join(h.code, vf1('V16GUEST'));
	const line = [vf1('V16LINE')];
	for (const p of line) await join(h.code, p, { queue: true });
	truth.vf1 += 3; listed.vf1++;
	keep.push({ h, line });
}
// VF2: v0.1.16 (plain) searching, a Quick match of v0.1.19s, a listed room waiting
await searching(vf2_old('V2OLD')); truth.vf2++;
await quick_pair(vf2('V2QA'), vf2('V2QB')); truth.vf2 += 2;
await host(vf2('V2OPEN'), { listed: true }); truth.vf2++; listed.vf2++;

let d = await online();
if (process.env.COUNTS_OUT) writeFileSync(process.env.COUNTS_OUT, JSON.stringify({ online: d, truth, watching, listed }, null, 1));
const total = (x) => Object.fromEntries(Object.entries(x.games).map(([g, c]) => [g, c.total]));
check('the mix: each game\'s players (searching, hosting, in a line, in a match), each once', total(d), truth);
check('the mix: watchers apart, by game', Object.fromEntries(Object.entries(d.games).map(([g, c]) => [g, c.watching])), watching);
check('the mix: in a line', [d.games.vf3.queued, d.games.vf1.queued], [3, 1]);
check('the mix: on an older version (a Quick match counted as its own version, not the version told before it)', [d.games.vf2.older, d.games.vf3.older], [1, 4]);
check('the mix: each count\'s parts add up to its total', Object.values(d.games).every((c) => c.searching + c.random + c.hosting + c.queued + c.room === c.total), true);
check('the mix: VF3 site (VF3 + Team Battle)', d.games.vf3.total + d.games.vf3tb.total, truth.vf3 + truth.vf3tb);
const rooms_by = {};
for (const r of d.rooms || []) if (!r.older && !r.locked) rooms_by[r.game] = (rooms_by[r.game] || 0) + 1;
check('the mix: the rooms list, by game (the Online page\'s tabs)', { vf1: rooms_by.vf1 | 0, vf2: rooms_by.vf2 | 0, vf3: rooms_by.vf3 | 0, vf3tb: rooms_by.vf3tb | 0 }, listed);

// ---- the games' "N online" by Quick match (POST /v1/random/count): the game's players, as the Online page has them ----
// (asked as each game's builds ask: a new build and an older one of each; VF3 and Team Battle each their own)
const askers = { vf1: [vf1('ASK1'), vf1('ASK16', { watchable: true, pool: '', accepts: [] })], vf2: [vf2('ASK2'), vf2_old('ASK2OLD')],
	vf3: [vf3('ASK3'), vf3_old('ASK3OLD')], vf3tb: [tb('ASKTB'), tb('ASKTBOLD', { version: 'v0.1.7-online-preview', pool: undefined, accepts: undefined })] };
const random_count = async (p) => { const r = await call('POST', '/v1/random/count', p, p.from); return [r.status, r.body]; };
const shape = ([status, b]) => status === 200 && b && Object.keys(b).join() === 'players' && Number.isInteger(b.players) && b.players >= 0;
{
	const got = {}, page = {}, shapes = [];
	for (const [g, ps] of Object.entries(askers)) {
		got[g] = [];
		for (const p of ps) { const r = await random_count(p); shapes.push(shape(r)); got[g].push(r[1].players); }
		page[g] = [d.games[g].total, d.games[g].total];
	}
	check('"N online": each game\'s count is the Online page\'s total for that game, whatever build asks', got, page);
	check('"N online": the same as who is really there (no Quick match counted twice, the rooms in it)', Object.fromEntries(Object.entries(got).map(([g, l]) => [g, l[0]])), truth);
	check('"N online": Team Battle\'s its own, not VF3\'s', [got.vf3tb[0] === truth.vf3tb, got.vf3[0] === truth.vf3, truth.vf3tb !== truth.vf3], [true, true, true]);
	check('"N online": the answer is {players: number}, as every released game reads it', shapes.every(Boolean), true);
	// the Stats not answering (an error, a throw, no binding): the queue's own count, as before; never an error
	const stats = env.STATS, failing = [
		['an error', { idFromName: (n) => n, get: () => ({ fetch: async () => new Response('{"error":"server"}', { status: 500 }) }) }],
		['a throw', { idFromName: (n) => n, get: () => ({ fetch: async () => { throw new Error('gone'); } }) }],
		['none', undefined],
	];
	for (const [what, s] of failing) {
		env.STATS = s;
		const r3 = await random_count(askers.vf3[0]), rtb = await random_count(askers.vf3tb[0]);
		env.STATS = stats;
		// (VF3's queue: SEEK1, SEEK2 and OLDSEEK searching, QMA and QMB's match; Team Battle's: TBSEEK searching)
		check('"N online", the Stats ' + what + ': the queue\'s count, {players}, 200', [shape(r3), r3[1].players, shape(rtb), rtb[1].players], [true, 5, true, 1]);
	}
	check('"N online": the Stats back, the page\'s total again', (await random_count(askers.vf3[0]))[1].players, truth.vf3);
	// a Quick match played three times by the same two (a rematch each time, through the queue): still two players
	const a = vf3('AGAINA'), b = vf3('AGAINB');
	for (let i = 0; i < 3; i++) await quick_pair(a, b);
	truth.vf3 += 2;
	await settle();
	check('"N online": the same two in three Quick matches, counted once each', [(await random_count(askers.vf3[0]))[1].players, (await online()).games.vf3.total], [truth.vf3, truth.vf3]);
}

// ---- time: the rooms that stay play on, 12 minutes (holds every 15 s, their lines asking every 5 s); everyone else quiet ----
for (let s = 5; s <= 12 * 60; s += 5) {
	advance(5000);
	for (const { h, line } of keep) {
		if (s % 15 === 0) await call('POST', '/v1/hold', { code: h.code, key: h.key }, h.from);
		for (const p of line) await join(h.code, p, { queue: true });
	}
}
d = await online();
check('12 minutes on: only the rooms that stay (host, the one playing, the line); everyone else gone', total(d), { vf1: 3, vf2: 0, vf3: 7, vf3tb: 0 });
// their lines leave (stop asking): gone from the count within a minute and a half, the rooms' players still there
for (let s = 0; s < 90; s += 15) { advance(15000); for (const { h } of keep) await call('POST', '/v1/hold', { code: h.code, key: h.key }, h.from); }
d = await online();
check('the lines gone quiet: no longer counted', [total(d), d.games.vf3.queued], [{ vf1: 2, vf2: 0, vf3: 4, vf3tb: 0 }, 0]);
// and everyone gone without a word: nothing left after the longest any of it is kept
advance(11 * 60000);
d = await online();
check('everyone gone: 0 everywhere, no watchers', [total(d), Object.values(d.games).map((c) => c.watching)], [{ vf1: 0, vf2: 0, vf3: 0, vf3tb: 0 }, [0, 0, 0, 0]]);
console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
