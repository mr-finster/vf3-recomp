// sitelang: the Online page's languages checked as the games' langtest checks theirs: every line of the English in
// every language, none extra, the same {places}, none empty, and every language in the corner's list. Then every
// letter each language written in Barlow shows (as written and in capitals, as CSS makes them in that language: a
// Turkish i is İ) in each of the site's three Barlow fonts; and the page's one script's hash the one in _headers'
// script-src (the page doesn't run without it: after changing the script, --csp writes the new one there).
//   node tools/online/sitelang.mjs [--csp]
import { readFileSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { brotliDecompressSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const here = (f) => fileURLToPath(new URL(f, import.meta.url));
const page = readFileSync(here("site/online.html"), "utf8");
const block = (name, close = "\n};\n") => {   // (the declaration, from "const NAME = " to its close)
	const at = page.indexOf("const " + name + " = ");
	if (at < 0) throw new Error("no " + name + " in online.html");
	const end = page.indexOf(close, at);
	return page.slice(at, end + close.length);
};
const box = {};
vm.runInNewContext(block("EN").replace("const EN", "this.EN") + block("TR").replace("const TR", "this.TR") + block("LANGS", "]];").replace("const LANGS", "this.LANGS"), box);
const { EN, TR, LANGS } = box;
const places = (s) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(",");
let bad = 0;
const fail = (what) => { console.error("sitelang: " + what); bad++; };
for (const [code] of LANGS) {
	if (code === "en") continue;
	const t = TR[code];
	if (!t) { fail(code + ": no lines"); continue; }
	for (const k of Object.keys(EN)) {
		if (!(k in t)) fail(code + ": " + k + " isn't translated");
		else if (typeof t[k] !== "string" || !t[k].trim()) fail(code + ": " + k + " is empty");
		else if (places(t[k]) !== places(EN[k])) fail(code + ": " + k + " has {" + places(t[k]) + "}, the English {" + places(EN[k]) + "}");
	}
	for (const k of Object.keys(t)) if (!(k in EN)) fail(code + ": " + k + " isn't one of the English's");
}
for (const code of Object.keys(TR)) if (!LANGS.some(([c]) => c === code)) fail(code + " isn't in the list of languages");

// ---- the fonts: a WOFF2's characters (its cmap: formats 4 and 12) ----
const KNOWN = ["cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"];
function woff2_chars(buf) {
	if (buf.toString("latin1", 0, 4) !== "wOF2") throw new Error("not a WOFF2");
	let o = 48;
	const base128 = () => { let v = 0; for (let i = 0; i < 5; i++) { const b = buf[o++]; v = v * 128 + (b & 127); if (!(b & 128)) return v; } throw new Error("bad UIntBase128"); };
	const tables = [];
	for (let i = buf.readUInt16BE(12); i > 0; i--) {
		const flags = buf[o++], tag = (flags & 63) === 63 ? buf.toString("latin1", o, (o += 4)) : KNOWN[flags & 63], xf = flags >> 6, orig = base128();
		tables.push({ tag, len: (tag === "glyf" || tag === "loca" ? xf === 3 : xf === 0) ? orig : base128() });
	}
	const data = brotliDecompressSync(buf.subarray(o, o + buf.readUInt32BE(20)));
	let at = 0;
	for (const t of tables) { if (t.tag === "cmap") return cmap(data.subarray(at, at + t.len)); at += t.len; }
	throw new Error("no cmap");
}
function cmap(c) {
	const set = new Set();
	for (let i = 0; i < c.readUInt16BE(2); i++) {
		const off = c.readUInt32BE(8 + i * 8), fmt = c.readUInt16BE(off);
		if (fmt === 4) {
			const seg2 = c.readUInt16BE(off + 6), ends = off + 14, starts = ends + seg2 + 2, deltas = starts + seg2, ranges = deltas + seg2;
			for (let k = 0; k < seg2; k += 2) {
				const end = c.readUInt16BE(ends + k), start = c.readUInt16BE(starts + k), delta = c.readUInt16BE(deltas + k), ro = c.readUInt16BE(ranges + k);
				for (let ch = start; ch <= end && ch !== 0xffff; ch++) {
					let g = ro ? c.readUInt16BE(ranges + k + ro + 2 * (ch - start)) : 1;
					if (g) g = ((ro ? g : ch) + delta) & 0xffff;
					if (g) set.add(ch);
				}
			}
		} else if (fmt === 12) {
			for (let k = 0, n = c.readUInt32BE(off + 12); k < n; k++)
				for (let ch = c.readUInt32BE(off + 16 + k * 12), last = c.readUInt32BE(off + 20 + k * 12); ch <= last; ch++) set.add(ch);
		}
	}
	return set;
}
const fonts = [...page.matchAll(/font-family: "Barlow Condensed";[^}]*url\((fonts\/[^)]+\.woff2)\)/g)].map((m) => m[1]);
if (fonts.length !== 3) fail("the page's Barlow faces: " + fonts.length + ", not 3");
// (the languages Barlow writes alone; the others' letters come from Noto after it, those not in Barlow)
const NOTO = ["ko", "ja", "zh-Hans", "zh-Hant", "ru"];
const fixed = [page.match(/const HOSTS_ROOM = "([^"]*)"/)[1], "QUICK MATCH", "VF1 VF2 VF3 TB Virtua Fighter Team Battle", "Twemoji by Twitter, Inc. and contributors, CC BY 4.0. 0123456789\u2013\u2014\u00b7\u2026"]
	.concat(LANGS.map(([, name]) => name).filter((x) => /^[\u0000-\u024f]+$/.test(x)));
let letters = 0;
for (const f of fonts) {
	const has = woff2_chars(readFileSync(here("site/" + f)));
	for (const [code] of LANGS) {
		if (NOTO.includes(code)) continue;
		const missing = new Set();
		for (const line of Object.values(Object.assign({}, EN, TR[code] || {})).concat(fixed))
			for (const text of [line, line.toLocaleUpperCase(code), line.toUpperCase()])
				for (const ch of text) if (!/\s/.test(ch) && !has.has(ch.codePointAt(0))) missing.add(ch);
		if (missing.size) fail(f + ": " + code + " needs " + [...missing].map((ch) => ch + " U+" + ch.codePointAt(0).toString(16).toUpperCase().padStart(4, "0")).join(", "));
	}
	letters = has.size;
}

// ---- the script's hash in the content security policy ----
const scripts = [...page.matchAll(/<script>([\s\S]*?)<\/script>/g)];
const headers_file = here("site/_headers"), headers = readFileSync(headers_file, "utf8");
if (scripts.length !== 1 || /<script[^>]/.test(page)) fail("the page has " + scripts.length + " inline scripts and should have only its one");
else {
	const hash = "'sha256-" + createHash("sha256").update(scripts[0][1], "utf8").digest("base64") + "'";
	const now = (headers.match(/script-src ([^;]*)/) || [])[1];
	if (now !== hash) {
		if (process.argv.includes("--csp") && now !== undefined) { writeFileSync(headers_file, headers.replace("script-src " + now, "script-src " + hash)); console.log("sitelang: _headers' script-src now " + hash); }
		else fail("_headers' script-src is " + now + ", the page's script " + hash + " (node tools/online/sitelang.mjs --csp writes it)");
	}
}
console.log(bad ? "sitelang: " + bad + " problems" : "sitelang: " + (LANGS.length - 1) + " languages, " + Object.keys(EN).length + " lines each, all there; " + fonts.length + " fonts (" + letters + " characters) with every letter; the script's hash in the CSP");
process.exit(bad ? 1 : 0);
