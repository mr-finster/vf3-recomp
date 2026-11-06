// sitelang: the Online page's languages checked as the games' langtest checks theirs: every line of the English in
// every language, none extra, the same {places}, none empty, and every language in the corner's list.
//   node tools/online/sitelang.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const page = readFileSync(fileURLToPath(new URL("site/online.html", import.meta.url)), "utf8");
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
console.log(bad ? "sitelang: " + bad + " problems" : "sitelang: " + (LANGS.length - 1) + " languages, " + Object.keys(EN).length + " lines each, all there");
process.exit(bad ? 1 : 0);
