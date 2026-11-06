// Stand-ins for Cloudflare's Durable Objects and rate limits, to run the lobby in Node (the tests here, and serve.mjs)
import { pathToFileURL } from 'node:url';

// WebSockets as Cloudflare gives a Durable Object them (WebSocketPair, acceptWebSocket and its hibernation API, an
// automatic answer to "ping"), in memory: one end the object's, the other a test's or serve.mjs's
export const sockets = { in: 0, auto: 0, out: 0 };   // (messages to the objects, those answered without waking them, from them)
// The work the objects leave running after an answer (ctx.waitUntil: the online count told, the Stats' answer; a socket's
// message or close being handled), kept so a test can wait for exactly that instead of sleeping and hoping it's done:
// settle() returns once none is left (a Queue's own 2 s pause between tellings included)
const pending = new Set();
const track = (p) => { if (p && typeof p.then === 'function') { pending.add(p); p.then(() => pending.delete(p), () => pending.delete(p)); } };
const turn = () => new Promise((ok) => setImmediate(ok));
export async function settle() {
	for (let i = 0; i < 10000; i++) {
		await turn();   // (what's queued to start: a socket's delivery, a handler's next step)
		if (!pending.size) { await turn(); if (!pending.size) return; }
		await Promise.allSettled([...pending]);
	}
	throw new Error('settle: still busy');
}
// A search's wait taken in by its queue (a long-poll the queue holds open: not background work, so settle() can't see
// it): what a test waits for before the second player comes, so the two meet in the order it means, on any machine
export async function queued(env, ticket) {
	for (let i = 0; i < 100000; i++) {
		for (const q of env.QUEUES.objs.values()) { const w = q.waiting && q.waiting.get(ticket); if (w && w.waiters && w.waiters.length) return; }
		await turn();
	}
	throw new Error('queued: the wait never came in');
}
class FakeSocket {
	constructor() { this.peer = null; this.listeners = { message: [], close: [] }; this.open = true; this.owner = null; this.tags = []; this.att = null; }
	accept() {}
	addEventListener(type, f) {   // (what came before anyone listened: kept, as a real socket's would be)
		(this.listeners[type] = this.listeners[type] || []).push(f);
		if (type === 'message' && this.early) for (const d of this.early.splice(0)) f({ data: d });
	}
	serializeAttachment(v) { this.att = structuredClone(v); }
	deserializeAttachment() { return this.att == null ? null : structuredClone(this.att); }
	send(data) {
		if (!this.open) throw new Error('closed');
		const to = this.peer;
		if (!to || !to.open) return;
		if (data instanceof Uint8Array) data = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
		if (to.owner) {   // (to the object: its automatic answer, or its handler)
			const auto = to.owner.__auto;
			if (auto && data === auto.request) { sockets.auto++; to.auto_at = new Date(); queueMicrotask(() => this.deliver(auto.response)); return; }
			sockets.in++;
			track(new Promise((ok) => queueMicrotask(() => ok(to.owner.obj.webSocketMessage(to, data)))));
		} else {
			if (this.owner) sockets.out++;
			queueMicrotask(() => to.deliver(data));
		}
	}
	deliver(data) {
		if (!this.listeners.message.length) { (this.early = this.early || []).push(data); return; }
		for (const f of this.listeners.message) f({ data });
	}
	close(code = 1000, reason = '') {
		if (!this.open) return;
		this.open = false;
		const to = this.peer;
		if (!to || !to.open) return;
		if (to.owner) track(new Promise((ok) => queueMicrotask(() => ok(to.owner.obj.webSocketClose && to.owner.obj.webSocketClose(to, code, reason, true)))));
		else { to.open = false; queueMicrotask(() => { for (const f of to.listeners.close) f({ code, reason }); }); }
	}
}
globalThis.WebSocketPair = class { constructor() { const a = new FakeSocket(), b = new FakeSocket(); a.peer = b; b.peer = a; this[0] = a; this[1] = b; } };
globalThis.WebSocketRequestResponsePair = class { constructor(request, response) { this.request = request; this.response = response; } };
const RealResponse = globalThis.Response;
globalThis.Response = class extends RealResponse {   // (a 101 with its socket, as Workers allow and Node doesn't)
	constructor(body, init) {
		if (init && init.status === 101) {
			super(null, { ...init, status: 200 });
			Object.defineProperty(this, 'status', { value: 101 });
			this.webSocket = init.webSocket;
		} else super(body, init);
	}
};

export async function lobby(path) {
	const mod = await import(pathToFileURL(path).href);
	const { Room, Queue, Stats, Limits, Watch } = mod;
	function ns(Cls, env) {
		const objs = new Map();
		return {
			idFromName: (n) => ({ n, toString: () => n }),
			get(id) {
				if (!objs.has(id.n)) {
					const store = new Map();
					const owner = { obj: null, __auto: null, sockets: new Set() };
					const ctx = { id, storage: { get: async (k) => (Array.isArray(k) ? new Map(k.filter((x) => store.has(x)).map((x) => [x, structuredClone(store.get(x))])) : structuredClone(store.get(k))), put: async (k, v) => { if (typeof k === 'object') for (const [a, b] of Object.entries(k)) store.set(a, structuredClone(b)); else store.set(k, structuredClone(v)); }, delete: async (k) => (Array.isArray(k) ? k.filter((x) => store.delete(x)).length : store.delete(k)), deleteAll: async () => store.clear(), list: async ({ prefix = '' } = {}) => new Map([...store].filter(([k]) => k.startsWith(prefix)).sort(([a], [b]) => (a < b ? -1 : 1)).map(([k, v]) => [k, structuredClone(v)])), __store: store,
						// (alarms: kept, and run when due only where env.__alarms says so, serve.mjs's, as Cloudflare runs them;
						// the tests call an object's alarm() themselves)
						setAlarm: async (t) => {
							owner.alarm_at = t;
							if (!env.__alarms) return;
							clearTimeout(owner.alarm_timer);
							owner.alarm_timer = setTimeout(() => { owner.alarm_at = null; track(Promise.resolve().then(() => owner.obj.alarm())); }, Math.max(0, t - Date.now()));
							if (owner.alarm_timer.unref) owner.alarm_timer.unref();
						},
						getAlarm: async () => owner.alarm_at ?? null }, blockConcurrencyWhile: (f) => f(), waitUntil: track,
						acceptWebSocket: (ws, tags = []) => { ws.owner = owner; ws.tags = tags; owner.sockets.add(ws); },
						getWebSockets: (tag) => [...owner.sockets].filter((w) => w.open && w.peer.open && (!tag || w.tags.includes(tag))),
						getTags: (ws) => ws.tags || [],
						setWebSocketAutoResponse: (pair) => { owner.__auto = pair; },
						getWebSocketAutoResponseTimestamp: (ws) => ws.auto_at || null };
					owner.obj = new Cls(ctx, env);
					objs.set(id.n, owner.obj);
				}
				const o = objs.get(id.n);
				return { fetch: (u, init) => o.fetch(u instanceof Request ? u : new Request(u, init)) };
			},
			objs,
		};
	}
	const env = { TURN_STATIC: '[]' };
	for (const k of ['ANY_LIMIT', 'HOST_LIMIT', 'JOIN_LIMIT', 'WAIT_LIMIT', 'TURN_LIMIT', 'WATCH_LIMIT', 'WATCHING_LIMIT', 'CODE48_LIMIT']) env[k] = { limit: async () => ({ success: true }) };
	env.ROOMS = ns(Room, env); env.QUEUES = ns(Queue, env); env.STATS = ns(Stats, env); env.LIMITS = ns(Limits, env); env.WATCHES = ns(Watch, env);
	return { worker: mod.default, env };
}
