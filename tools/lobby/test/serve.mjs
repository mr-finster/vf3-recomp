// The lobby on this computer, without wrangler or Cloudflare: its Worker and stand-ins for its Durable Objects (in
// memory), for the games and tools/watchsim to talk to over HTTP.
//   node test/serve.mjs src/index.js [port] [NAME=value ...]   (the Worker's variables, e.g. SWITCH_WATCH=off; also
//   POST /__vars?SWITCH_WATCH=off&SWITCH_ROOMS= to change them while it runs, an empty value unsetting one)
import http from 'node:http';
import { createHash } from 'node:crypto';
import { lobby, sockets } from './standin.mjs';
const { worker, env } = await lobby(process.argv[2]);
env.__alarms = true;   // (the objects' alarms run when due, as Cloudflare's: a room's minute, a Watch's look at a quiet host)
const port = +(process.argv[3] || 8787);
for (const kv of process.argv.slice(4)) { const at = kv.indexOf('='); if (at > 0) env[kv.slice(0, at)] = kv.slice(at + 1); }
// (what Cloudflare would bill by request, counted: the Worker's requests by route, each Durable Object class's requests;
// GET /__counts, POST /__counts to start again; this test server's only. No edge cache here: every ask reaches its object)
let counts = { since: Date.now(), worker: {}, objects: {} };
for (const [name, ns] of Object.entries(env)) {
	if (!ns || typeof ns.get !== 'function') continue;
	const get = ns.get.bind(ns);
	ns.get = (id) => {
		const o = get(id);
		return { fetch: (u, init) => {
			counts.objects[name] = (counts.objects[name] || 0) + 1;
			return o.fetch(u, init);
		} };
	};
}
const server = http.createServer(async (q, res) => {
	if (q.url.startsWith('/__vars') && q.method === 'POST') {   // (the switches flipped as the dashboard would)
		for (const [k, v] of new URL(q.url, 'http://x').searchParams) if (/^[A-Z_]+$/.test(k) && typeof env[k] !== 'object') { if (v) env[k] = v; else delete env[k]; }
		res.writeHead(200, { 'content-type': 'application/json' });
		return res.end(JSON.stringify(Object.fromEntries(Object.entries(env).filter(([k, v]) => typeof v === 'string'))));
	}
	if (q.url === '/__counts') {
		if (q.method === 'POST') { counts = { since: Date.now(), worker: {}, objects: {} }; sockets.in = sockets.auto = sockets.out = 0; }
		res.writeHead(200, { 'content-type': 'application/json' });
		return res.end(JSON.stringify({ ...counts, websocket_messages: { ...sockets } }));
	}
	const route = q.url.split('?')[0];
	counts.worker[route] = (counts.worker[route] || 0) + 1;
	const parts = [];
	for await (const c of q) parts.push(c);
	const body = Buffer.concat(parts);
	const req = new Request('http://127.0.0.1:' + port + q.url, { method: q.method, headers: { ...q.headers, 'cf-connecting-ip': q.socket.remoteAddress || 'local', 'x-test-country': q.headers['x-test-country'] || 'KR' }, body: q.method === 'GET' || q.method === 'HEAD' ? undefined : body });
	Object.defineProperty(req, 'cf', { value: { latitude: '37.5', longitude: '127.0', continent: 'AS' } });
	try {
		const r = await worker.fetch(req, env);
		res.writeHead(r.status, Object.fromEntries(r.headers));
		if (!r.body) return res.end();
		const reader = r.body.getReader();   // (as it comes: a response that streams goes out a piece at a time)
		res.on('close', () => reader.cancel().catch(() => {}));
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			res.write(Buffer.from(value));
		}
		res.end();
	} catch (e) {
		res.writeHead(500);
		res.end(String(e));
	}
});
// A WebSocket (the game's feed and watchers): the upgrade to the Worker, then its frames to and from the object's socket
server.on('upgrade', async (q, sock, head) => {
	const route = q.url.split('?')[0];
	counts.worker[route] = (counts.worker[route] || 0) + 1;
	const req = new Request('http://127.0.0.1:' + port + q.url, { headers: { ...q.headers, 'cf-connecting-ip': sock.remoteAddress || 'local' } });
	Object.defineProperty(req, 'cf', { value: { latitude: '37.5', longitude: '127.0', continent: 'AS' } });
	let r;
	try { r = await worker.fetch(req, env); } catch (e) { sock.end('HTTP/1.1 500 Error\r\n\r\n'); return; }
	if (r.status !== 101 || !r.webSocket) {
		const body = await r.text();
		sock.end('HTTP/1.1 ' + r.status + ' No\r\nContent-Type: application/json\r\nContent-Length: ' + Buffer.byteLength(body) + '\r\nConnection: close\r\n\r\n' + body);
		return;
	}
	const accept = createHash('sha1').update(q.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
	sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
	const ws = r.webSocket;
	ws.accept();
	const frame = (op, data) => {
		const n = data.length;
		let h;
		if (n < 126) h = Buffer.from([0x80 | op, n]);
		else if (n < 65536) h = Buffer.from([0x80 | op, 126, n >> 8, n & 255]);
		else { h = Buffer.alloc(10); h[0] = 0x80 | op; h[1] = 127; h.writeBigUInt64BE(BigInt(n), 2); }
		if (!sock.destroyed) sock.write(Buffer.concat([h, data]));
	};
	ws.addEventListener('message', (e) => (typeof e.data === 'string' ? frame(1, Buffer.from(e.data)) : frame(2, Buffer.from(e.data))));
	ws.addEventListener('close', () => { frame(8, Buffer.alloc(0)); sock.end(); });
	let buf = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
	const take = () => {
		for (;;) {
			if (buf.length < 2) return;
			const op = buf[0] & 15, masked = buf[1] & 128;
			let n = buf[1] & 127, hl = 2;
			if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); hl = 4; }
			else if (n === 127) { if (buf.length < 10) return; n = Number(buf.readBigUInt64BE(2)); hl = 10; }
			if (masked) hl += 4;
			if (buf.length < hl + n) return;
			const data = Buffer.from(buf.subarray(hl, hl + n));
			if (masked) for (let i = 0; i < n; i++) data[i] ^= buf[hl - 4 + (i & 3)];
			buf = buf.subarray(hl + n);
			if (op === 8) { try { ws.close(1000, 'bye'); } catch (e) {} sock.end(); return; }
			if (op === 9) { frame(10, data); continue; }
			if (op === 1) ws.send(data.toString());
			else if (op === 2) ws.send(new Uint8Array(data));
		}
	};
	sock.on('data', (d) => { buf = Buffer.concat([buf, d]); try { take(); } catch (e) {} });
	sock.on('close', () => { try { ws.close(1001, 'gone'); } catch (e) {} });
	sock.on('error', () => {});
	take();
});
server.listen(port, '127.0.0.1', () => console.log('lobby on http://127.0.0.1:' + port));
