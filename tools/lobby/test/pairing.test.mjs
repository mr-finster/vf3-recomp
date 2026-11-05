// The lobby's pairing (terms(), rooms and Random), run in Node with stand-ins for Cloudflare's Durable Objects:
//   cd tools/lobby && npm test   (node test/pairing.test.mjs src/index.js: no wrangler, nothing deployed)
import { pathToFileURL } from 'node:url';
const lobby = await import(pathToFileURL(process.argv[2]).href);
const { default: worker, Room, Queue, Stats, Limits } = lobby;
function ns(Cls, env) {
	const objs = new Map();
	return {
		idFromName: (n) => ({ n, toString: () => n }),
		get(id) {
			if (!objs.has(id.n)) {
				const store = new Map();
				const ctx = { id, storage: { get: async (k) => store.get(k), put: async (k, v) => { if (typeof k === 'object') for (const [a, b] of Object.entries(k)) store.set(a, b); else store.set(k, v); }, delete: async (k) => store.delete(k), deleteAll: async () => store.clear(), setAlarm: async () => {} }, blockConcurrencyWhile: (f) => f(), waitUntil: () => {} };
				objs.set(id.n, new Cls(ctx, env));
			}
			const o = objs.get(id.n);
			return { fetch: (u, init) => o.fetch(new Request(u, init)) };
		},
		objs,
	};
}
const env = { TURN_STATIC: '[]' };
for (const k of ['ANY_LIMIT', 'HOST_LIMIT', 'JOIN_LIMIT', 'WAIT_LIMIT', 'TURN_LIMIT']) env[k] = { limit: async () => ({ success: true }) };
env.ROOMS = ns(Room, env); env.QUEUES = ns(Queue, env); env.STATS = ns(Stats, env); env.LIMITS = ns(Limits, env);
let ip = 1;
async function post(path, body, from) {
	const req = new Request('http://lobby' + path, { method: 'POST', body: JSON.stringify(body), headers: { 'cf-connecting-ip': from, 'x-test-country': 'JP' } });
	Object.defineProperty(req, 'cf', { value: { latitude: '35.6', longitude: '139.7' } });   // (both in Tokyo: met at once)
	const r = await worker.fetch(req, env);
	return { status: r.status, body: await r.json() };
}
const C = '0123456789abcdef', B = 'v0.1.7-online-preview';
const neu = (name, version, game = 'vf3tb', accepts = ['stage', '1v1', 'dural']) => ({ game, version, content: C, name, cands: ['203.0.113.' + ip + ':7700'], relay: '', pool: B + '+host1', accepts, from: '198.51.100.' + ip++ });
const old = (name, version, game = 'vf3tb') => ({ game, version, content: C, name, cands: ['203.0.113.' + ip + ':7700'], relay: '', from: '198.51.100.' + ip++ });
// Random: a pair, the first in waits (its ticket), the second in matches: each side's answer
async function random_pair(a, b) {
	const ra = await post('/v1/random', a, a.from);
	if (!ra.body.ticket) return { error: 'a: ' + JSON.stringify(ra.body) };
	const wait = post('/v1/random/wait', { ...a, ticket: ra.body.ticket }, a.from);
	await new Promise((ok) => setTimeout(ok, 20));
	const rb = await post('/v1/random', b, b.from);
	if (rb.body.role) { const wa = await wait; return { a: wa.body, b: rb.body }; }
	await post('/v1/random/leave', { ...b, ticket: rb.body.ticket }, b.from);
	await post('/v1/random/leave', { ...a, ticket: ra.body.ticket }, a.from);   // (its wait answered: gone)
	return { none: true };
}
let fails = 0;
function check(what, got, want) {
	const ok = got === want;
	if (!ok) fails++;
	console.log((ok ? 'ok   ' : 'FAIL ') + what + ': ' + got + (ok ? '' : ' (wanted ' + want + ')'));
}
const cases = [
	['new 3v3 stage off vs new 1v1 stage on: the pool', neu('A', B), neu('B', B + '+1v1+stage'), B + '+host1'],
	['new Dural on vs new Dural off: the pool, Dural on (either\'s)', neu('A', B + '+dural+stage'), neu('B', B), B + '+host1+dural'],
	['new Dural off vs new Dural on: the same', neu('A', B), neu('B', B + '+dural'), B + '+host1+dural'],
	['new Dural on vs new Dural on: the pool, Dural on', neu('A', B + '+dural'), neu('B', B + '+dural+1v1'), B + '+host1+dural'],
	['Rev D new Dural off vs new Dural on: the pool, +dural2', neu('A', B, 'vf3', ['stage', 'dural2']), neu('B', B + '+dural2', 'vf3', ['stage', 'dural2']), B + '+host1+dural2'],
	['old plain vs old plain: as ever', old('A', B), old('B', B), B],
	['old +stage vs old plain: apart, as ever', old('A', B + '+stage'), old('B', B), null],
	['old +1v1+stage vs new (own plain): the old one\'s', old('A', B + '+1v1+stage'), neu('B', B), B + '+1v1+stage'],
	['new vs old +dural: the old one\'s', neu('A', B + '+stage'), old('B', B + '+dural'), B + '+dural'],
	['old plain vs new +stage: the old one\'s', old('A', B), neu('B', B + '+stage'), B],
	['Rev D old +dural (her old patches) vs new (accepts dural2): apart', old('A', B + '+dural', 'vf3'), neu('B', B, 'vf3', ['stage', 'dural2']), null],
	['Rev D old +dural2+stage vs new: the old one\'s', old('A', B + '+dural2+stage', 'vf3'), neu('B', B, 'vf3', ['stage', 'dural2']), B + '+dural2+stage'],
	['another base (a test build) vs new: apart', old('A', 'v0.1.8-alpha-3-gabc1234'), neu('B', B), null],
	['a new pool vs another pool (a later +host2): either one\'s version, the same both ways', { ...neu('A', B + '+stage'), pool: B + '+host2' }, neu('B', B), B],
	// v0.1.16's builds (a pool, their Dural flag "dural2", Team Battle's "dural") and this one's (its own "dural3", and
	// v0.1.16's, played with that version's patches): the same pool, Dural on when either has her on, on v0.1.16's flag
	// (both load v0.1.16's patches); two of this one's on "dural3"
	...(() => {
		const out = [];
		const v16 = (n, v, g) => neu(n, v, g, g === 'vf3' ? ['stage', 'dural2'] : ['stage', '1v1', 'dural']);
		const n3 = (n, v, g) => neu(n, v, g, g === 'vf3' ? ['stage', 'dural3', 'dural2'] : ['stage', '1v1', 'dural3', 'dural']);
		for (const g of ['vf3', 'vf3tb']) {
			const f = g === 'vf3' ? '+dural2' : '+dural';
			for (const od of [0, 1]) for (const nd of [0, 1]) {
				const want = B + '+host1' + (od || nd ? f : '');
				out.push([g + ' v0.1.16 Dural ' + (od ? 'on' : 'off') + ' vs new Dural ' + (nd ? 'on' : 'off') + ': the pool' + (od || nd ? ', ' + f + ' (v0.1.16\'s patches)' : ''), v16('A', B + (od ? f : ''), g), n3('B', B + (nd ? '+dural3' : ''), g), want]);
				out.push([g + ' new Dural ' + (nd ? 'on' : 'off') + ' vs v0.1.16 Dural ' + (od ? 'on' : 'off') + ': the same', n3('A', B + (nd ? '+dural3+stage' : '+stage'), g), v16('B', B + (od ? f : ''), g), want]);
			}
			out.push([g + ' new Dural on vs new Dural off: the pool, +dural3', n3('A', B + '+dural3', g), n3('B', B, g), B + '+host1+dural3']);
			out.push([g + ' new Dural off vs new Dural on: the pool, +dural3', n3('A', B, g), n3('B', B + '+dural3+stage', g), B + '+host1+dural3']);
			out.push([g + ' new Dural off vs new Dural off: the pool', n3('A', B, g), n3('B', B, g), B + '+host1']);
			out.push([g + ' pre-pool old ' + f + ' vs new: the old one\'s, as v0.1.16 plays it', old('A', B + f, g), n3('B', B, g), g === 'vf3' ? B + f : B + f]);
		}
		return out;
	})(),
	['vf2 new Random vs new Normal: the pool', neu('A', 'v0.1.16+random', 'vf2', ['random']), neu('B', 'v0.1.16', 'vf2', ['random']), 'v0.1.16+host1'],
	['vf2 old +random vs new Normal: the old one\'s', old('A', 'v0.1.16+random', 'vf2'), neu('B', 'v0.1.16', 'vf2', ['random']), 'v0.1.16+random'],
];
for (const c of cases) for (const x of [c[1], c[2]]) if (x.game === 'vf2' && x.pool) x.pool = 'v0.1.16+mix1';   // (VF2's pool flag)
for (const c of cases) if (c[3] === 'v0.1.16+host1') c[3] = 'v0.1.16+mix1';
for (const [what, a, b, want] of cases) {
	const r = await random_pair(a, b);
	if (r.error) { check('random, ' + what, r.error, want); continue; }
	if (r.none) { check('random, ' + what, null, want); continue; }
	const agree = r.a.as === r.b.as && (r.a.role === 'host') !== (r.b.role === 'host');
	check('random, ' + what, agree ? r.a.as : 'sides disagree ' + JSON.stringify(r), want);
	// and by room code, the same: b hosts, a joins
	const h = await post('/v1/host', b, b.from);
	const wait = post('/v1/wait', { code: h.body.code, key: h.body.key }, b.from);
	const j = await post('/v1/join', { ...a, code: h.body.code }, a.from);
	const w = await wait;
	const got = j.status === 200 ? (w.body.guest && w.body.guest.as === j.body.as ? j.body.as : 'host and guest disagree') : null;
	check('room,   ' + what, got, want);
	await post('/v1/close', { code: h.body.code, key: h.body.key }, b.from);
}
// Every combination (reno's ask): vf3 (Revision D) and vf3tb, each side new or old (the v0.1.15 builds' version
// strings, as they send them: base, then +dural2 / Team Battle's +dural, +1v1, +stage), stage on or off, Dural on or
// off, Team Battle's 1 vs 1 or 3 vs 3; Revision D's pre-v0.1.16 "+dural" too; and vf2's Normal, Random and Seed. Both
// orders, Random and room codes. What each pair should play, worked out here on its own (not with terms()):
//   new-new: the pool (+ the game's Dural flag when either has her on, vf3); new-old: the old one's version, if the
//   new build plays every flag of it (not Revision D's old "+dural"); old-old: only the same version, as today
{
	const games = {
		vf3: { flag: 'dural3', old: 'dural2', accepts: ['stage', 'dural3', 'dural2'], v16: ['stage', 'dural2'], tb: false },
		vf3tb: { flag: 'dural3', old: 'dural', accepts: ['stage', '1v1', 'dural3', 'dural'], v16: ['stage', '1v1', 'dural'], tb: true },
	};
	const sides = [];
	for (const [game, g] of Object.entries(games))
		for (const kind of ['new', 'v16', 'old'])
			for (const dural of [0, 1]) for (const one of g.tb ? [0, 1] : [0]) for (const stage of [0, 1]) {
				const v = B + (dural ? '+' + (kind === 'new' ? g.flag : g.old) : '') + (one ? '+1v1' : '') + (stage ? '+stage' : '');
				sides.push({ game, kind, v, dural, label: kind + (dural ? ' D' : ' -') + (g.tb ? (one ? ' 1v1' : ' 3v3') : '') + (stage ? ' S' : ' -') });
			}
	for (const stage of [0, 1]) sides.push({ game: 'vf3', kind: 'old', v: B + '+dural' + (stage ? '+stage' : ''), dural: 1, pre: true, label: 'old D(pre-0.1.16)' + (stage ? ' S' : ' -') });
	for (const kind of ['new', 'old'])
		for (const f of ['', 'R', 'S99']) sides.push({ game: 'vf2', kind, v: 'v0.1.16' + (f ? '+random' : ''), dural: 0, label: kind + ' ' + (f ? (f === 'R' ? 'Random' : 'Seed') : 'Normal') });
	const make = (x, name) => {
		if (x.game === 'vf2') return x.kind === 'new' ? { ...neu(name, x.v, 'vf2', ['random']), pool: 'v0.1.16+mix1' } : old(name, x.v, 'vf2');
		return x.kind === 'new' ? neu(name, x.v, x.game, games[x.game].accepts) : x.kind === 'v16' ? neu(name, x.v, x.game, games[x.game].v16) : old(name, x.v, x.game);
	};
	//   v16 (v0.1.16-alpha: a pool, its one Dural flag "dural2" / Team Battle's "dural") with new or v16: the pool, with
	//   v0.1.16's flag when either has her on (its patches on both sides); new-new: "dural3"; a pre-pool old one with a
	//   pool build: the old one's version, if the pool build plays every flag of it (not Revision D's pre-v0.1.16
	//   "+dural"), v0.1.16's Dural flag with its patches
	const want_of = (a, b) => {
		const pooled = (x) => x.kind !== 'old';
		if (pooled(a) && pooled(b)) {
			if (a.game === 'vf2') return 'v0.1.16+mix1';
			const g = games[a.game];
			if (!(a.dural || b.dural)) return B + '+host1';
			return B + '+host1+' + (a.kind === 'new' && b.kind === 'new' ? g.flag : g.old);
		}
		if (a.kind === 'old' && b.kind === 'old') return a.v === b.v ? a.v : null;
		const o = a.kind === 'old' ? a : b;
		return o.pre ? null : o.v;
	};
	const tally = { matched: 0, refused: 0, failed: 0 };
	const table = [];
	for (const game of ['vf3', 'vf3tb', 'vf2']) {
		const here = sides.filter((x) => x.game === game);
		for (const x of here) for (const y of here) {
			const want = want_of(x, y);
			const a = make(x, 'A'), b = make(y, 'B');
			const r = await random_pair(a, b);
			const rnd = r.error ? 'error' : r.none ? null : r.a.as === r.b.as && (r.a.role === 'host') !== (r.b.role === 'host') ? r.a.as : 'disagree';
			const h = await post('/v1/host', b, b.from);   // (b hosts, a joins)
			const wait = post('/v1/wait', { code: h.body.code, key: h.body.key }, b.from);
			const j = await post('/v1/join', { ...a, code: h.body.code }, a.from);
			let room_as = null;
			if (j.status === 200) { const w = await wait; room_as = w.body.guest && w.body.guest.as === j.body.as ? j.body.as : 'disagree'; }
			await post('/v1/close', { code: h.body.code, key: h.body.key }, b.from);
			const ok = rnd === want && room_as === want;
			if (!ok) { tally.failed++; fails++; console.log('FAIL matrix ' + game + ' ' + x.label + ' vs ' + y.label + ': random ' + rnd + ', room ' + room_as + ' (wanted ' + want + ')'); }
			else if (want === null) tally.refused++;
			else tally.matched++;
			table.push([game, x.label, y.label, want === null ? 'apart' : want.slice(want.indexOf('+') < 0 ? want.length : want.indexOf('+')) || '(base)', ok ? '' : 'FAIL']);
		}
	}
	// a different ROM: apart, whatever the settings
	{
		const a = neu('A', B + '+stage'), b = { ...neu('B', B + '+stage'), content: 'ffffffffffffffff' };
		const r = await random_pair(a, b);
		const h = await post('/v1/host', b, b.from);
		const j = await post('/v1/join', { ...a, code: h.body.code }, a.from);
		await post('/v1/close', { code: h.body.code, key: h.body.key }, b.from);
		check('matrix, a different ROM: apart (random, room)', (r.none ? 'apart' : 'met') + ', ' + j.body.error, 'apart, rom');
	}
	console.log('matrix (A vs B, both orders, Random and room codes the same): the terms (after the base)');
	let last = '';
	for (const [game, x, y, t, f] of table) {
		if (game + x !== last) { process.stdout.write((last ? '\n' : '') + (game + ' ' + x).padEnd(30) + ' | '); last = game + x; }
		process.stdout.write(y + ': ' + t + (f ? ' FAIL' : '') + '; ');
	}
	console.log('\nmatrix: ' + table.length + ' pairs: ' + tally.matched + ' matched, ' + tally.refused + ' apart as they should be, ' + tally.failed + ' failed');
}
// an old lobby's way, a new game: no pool sent at all is today's (version) rule: covered by the old/old cases
// the count: those one can be matched with
{
	const a = old('A', B + '+stage'), b = neu('B', B), c = old('C', B + '+dural+stage'), d = old('D', 'v0.1.7-online-preview+bogus');
	for (const x of [a, b, c, d]) x.content = 'fedcba9876543210';   // (a queue of its own: no matches made in it)
	const t = [];
	for (const x of [a, c, d]) t.push((await post('/v1/random', { ...x, cands: [] }, x.from)).body.ticket);
	const n = await post('/v1/random/count', b, b.from);
	check('count, a new player sees the old players it can play (not +bogus)', n.body.players, 2);
	const m = await post('/v1/random/count', a, a.from);
	check('count, an old +stage player sees only its own (none else)', m.body.players, 1);
}
// /v1/online: one count a game (who one can meet), the older versions' players among them, no variants
{
	const n = neu('N', B + '+dural+stage'), o = old('O', B + '+1v1'), v = { ...neu('V', 'v0.1.16+random', 'vf2', ['random']), pool: 'v0.1.16+mix1' };
	const get = async () => (await worker.fetch(new Request('http://lobby/v1/online', { headers: { 'cf-connecting-ip': '192.0.2.9' } }), env)).json();
	const d0 = await get();   // (the matrix's matches still counted, as playing: the change is what's checked)
	const h1 = await post('/v1/host', n, n.from), h2 = await post('/v1/host', o, o.from), h3 = await post('/v1/host', v, v.from);
	await new Promise((ok) => setTimeout(ok, 50));
	const d = await get();
	const tb = d.games.vf3tb, tb0 = d0.games.vf3tb, g2 = d.games.vf2, g20 = d0.games.vf2;
	check('online, the games counted', Object.keys(d.games || {}).sort().join(','), 'vf1,vf2,vf3,vf3tb');
	check('online, the shape: total, searching, random, hosting, room, older, countries; no variants', Object.keys(tb).sort().join(','), 'countries,hosting,older,random,room,searching,total');
	check('online, Team Battle: a new and an old host, the old one older', (tb.hosting - tb0.hosting) + ' hosting, ' + (tb.older - tb0.older) + ' older', '2 hosting, 1 older');
	check('online, VF2: a Random player is a current one', (g2.hosting - g20.hosting) + ' hosting, ' + (g2.older - g20.older) + ' older', '1 hosting, 0 older');
	for (const [h, x] of [[h1, n], [h2, o], [h3, v]]) await post('/v1/close', { code: h.body.code, key: h.body.key }, x.from);
}
console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
