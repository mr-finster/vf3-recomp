// The pairing tests (every route the games before rooms and watching use: room codes, Random, their answers) again
// with each off switch off, and all of them, at once in parallel: the old routes' expectations under every switch.
//   node test/pairing-switched.mjs src/index.js   (npm test runs it)
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const test = fileURLToPath(new URL('./pairing.test.mjs', import.meta.url));
const NAMES = ['SWITCH_ROOMS', 'SWITCH_LISTING', 'SWITCH_WATCH', 'SWITCH_WEBSOCKETS'];
const runs = [...NAMES.map((k) => [k]), NAMES];
const results = await Promise.all(runs.map((off) => new Promise((done) => {
	const vars = Object.fromEntries(off.map((k) => [k, 'off']));
	const p = spawn(process.execPath, [test, process.argv[2]], { env: { ...process.env, LOBBY_VARS: JSON.stringify(vars) }, stdio: ['ignore', 'pipe', 'ignore'] });
	let out = '';
	p.stdout.on('data', (d) => (out += d));
	p.on('close', (code) => done({ off, code, last: out.trim().split('\n').pop(), fails: out.split('\n').filter((l) => l.startsWith('FAIL')) }));
})));
let bad = 0;
for (const r of results) {
	console.log((r.code === 0 ? 'ok   ' : 'FAIL ') + 'the pairing tests with ' + r.off.join(', ') + ' off: ' + r.last);
	for (const f of r.fails) console.log('     ' + f);
	if (r.code !== 0) bad++;
}
console.log(bad ? bad + ' FAILED' : 'all passed');
process.exit(bad ? 1 : 0);
