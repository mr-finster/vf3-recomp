// Every lobby test file, one after another, each to its end even if one fails; at the end, every failed check by file,
// what it got and what it wanted (each test prints its own "FAIL what: got (wanted ...)"), so a failure that happens
// once in a while, here or in CI, says exactly which.   node test/all.mjs src/index.js   (npm test runs it)
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const lobby = process.argv[2] || 'src/index.js';
const files = ['pairing.test.mjs', 'watch.test.mjs', 'ws-over.test.mjs', 'switches.test.mjs', 'pairing-switched.mjs', 'review.test.mjs', 'counts.test.mjs'];
const quiet = process.argv.includes('--quiet');   // (only the failures and the summary)
const failed = [];
for (const f of files) {
	const code = await new Promise((done) => {
		const p = spawn(process.execPath, [fileURLToPath(new URL('./' + f, import.meta.url)), lobby], { stdio: ['ignore', 'pipe', 'pipe'] });
		let out = '';
		p.stdout.on('data', (d) => { out += d; if (!quiet) process.stdout.write(d); });
		p.stderr.on('data', (d) => { out += d; });
		p.on('close', (c) => {
			const lines = out.split('\n');
			for (const l of lines) if (/^\s*FAIL /.test(l)) failed.push(f + ': ' + l.trim());
			if (c !== 0 && !lines.some((l) => /^\s*FAIL /.test(l))) failed.push(f + ': exited ' + c + ' with no FAIL line: ' + lines.filter((l) => l.trim()).slice(-5).join(' | '));
			done(c);
		});
	});
	if (quiet && code === 0) console.log('ok   ' + f);
}
if (failed.length) { console.log('\n' + failed.length + ' FAILED:'); for (const l of failed) console.log('  ' + l); }
else console.log('\nall passed (' + files.join(', ') + ')');
process.exit(failed.length ? 1 : 0);
