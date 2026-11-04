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
	['new Dural on vs new Dural off: the pool', neu('A', B + '+dural+stage'), neu('B', B), B + '+host1'],
	['old plain vs old plain: as ever', old('A', B), old('B', B), B],
	['old +stage vs old plain: apart, as ever', old('A', B + '+stage'), old('B', B), null],
	['old +1v1+stage vs new (own plain): the old one\'s', old('A', B + '+1v1+stage'), neu('B', B), B + '+1v1+stage'],
	['new vs old +dural: the old one\'s', neu('A', B + '+stage'), old('B', B + '+dural'), B + '+dural'],
	['old plain vs new +stage: the old one\'s', old('A', B), neu('B', B + '+stage'), B],
	['Rev D old +dural (her old patches) vs new (accepts dural2): apart', old('A', B + '+dural', 'vf3'), neu('B', B, 'vf3', ['stage', 'dural2']), null],
	['Rev D old +dural2+stage vs new: the old one\'s', old('A', B + '+dural2+stage', 'vf3'), neu('B', B, 'vf3', ['stage', 'dural2']), B + '+dural2+stage'],
	['another base (a test build) vs new: apart', old('A', 'v0.1.8-alpha-3-gabc1234'), neu('B', B), null],
	['a new pool vs another pool (a later +host2): either one\'s version, the same both ways', { ...neu('A', B + '+stage'), pool: B + '+host2' }, neu('B', B), B],
	['vf2 new Random vs new Normal: the pool', neu('A', 'v0.1.16+random', 'vf2', ['random']), neu('B', 'v0.1.16', 'vf2', ['random']), 'v0.1.16+host1'],
	['vf2 old +random vs new Normal: the old one\'s', old('A', 'v0.1.16+random', 'vf2'), neu('B', 'v0.1.16', 'vf2', ['random']), 'v0.1.16+random'],
];
cases[11][1].pool = cases[11][2].pool = cases[12][2].pool = 'v0.1.16+mix1';
cases[11][3] = 'v0.1.16+mix1';
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
console.log(fails ? fails + ' FAILED' : 'all passed');
process.exit(fails ? 1 : 0);
