// The menus' languages (i18n.h). Shared with VF1 and VF2 Recomp as it is.
#include "i18n.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>

namespace tr {

namespace {

lang now = lang::en;

// Each language's table, by ID: the translations where they are, English where not (made once, at the first look)
struct tables
{
	std::vector<const char *> by[LANGS];
	std::vector<std::string> pseudo_text;   // (pseudo's own strings: by[pseudo] points into them)
	tables()
	{
		const size_t n = data::COUNT;
		by[int(lang::en)].assign(data::EN, data::EN + n);
		for (int l = 1; l < REAL_LANGS; l++) {
			by[l] = by[int(lang::en)];
			for (size_t i = 0; i < data::TABLE_COUNT[l]; i++)
				if (data::TABLE[l][i].id < n && data::TABLE[l][i].text && *data::TABLE[l][i].text) by[l][data::TABLE[l][i].id] = data::TABLE[l][i].text;
		}
		pseudo_text.resize(n);
		by[int(lang::pseudo)].resize(n);
		for (size_t i = 0; i < n; i++) {
			pseudo_text[i] = pseudo(data::EN[i]);
			by[int(lang::pseudo)][i] = pseudo_text[i].c_str();
		}
	}
};
const tables &all() { static const tables t; return t; }

std::string fill_in(std::string s, const std::vector<std::pair<std::string, std::string>> &with)
{
	// (each place filled once, left to right: what's put in is never looked in again)
	std::string out;
	size_t at = 0;
	while (at < s.size()) {
		const size_t open = s.find('{', at);
		if (open == std::string::npos) break;
		const size_t close = s.find('}', open);
		if (close == std::string::npos) break;
		const std::string name = s.substr(open + 1, close - open - 1);
		const auto it = std::find_if(with.begin(), with.end(), [&](const auto &w) { return w.first == name; });
		out += s.substr(at, open - at);
		if (it != with.end()) out += it->second;
		else out += s.substr(open, close - open + 1);   // (not given: left as it is)
		at = close + 1;
	}
	return out + s.substr(std::min(at, s.size()));
}

// One letter's accented look-alike (pseudo), UTF-8; others as they are
const char *accented(char c)
{
	static const char *const lower[26] = { "\xC3\xA1", "b", "\xC3\xA7", "\xC4\x91", "\xC3\xA9", "\xC6\x92", "\xC4\x9F", "\xC4\xA5", "\xC3\xAD", "\xC4\xB5",
		"\xC4\xB7", "\xC4\xBA", "m", "\xC3\xB1", "\xC3\xB6", "p", "q", "\xC5\x95", "\xC5\xA1", "\xC5\xA3", "\xC3\xBC", "v", "\xC5\xB5", "x", "\xC3\xBD", "\xC5\xBE" };
	static const char *const upper[26] = { "\xC3\x81", "B", "\xC3\x87", "\xC4\x90", "\xC3\x89", "F", "\xC4\x9E", "\xC4\xA4", "\xC3\x8D", "\xC4\xB4",
		"\xC4\xB6", "\xC4\xB9", "M", "\xC3\x91", "\xC3\x96", "P", "Q", "\xC5\x94", "\xC5\xA0", "\xC5\xA2", "\xC3\x9C", "V", "\xC5\xB4", "X", "\xC3\x9D", "\xC5\xBD" };
	if (c >= 'a' && c <= 'z') return lower[c - 'a'];
	if (c >= 'A' && c <= 'Z') return upper[c - 'A'];
	return nullptr;
}

} // namespace

text::operator std::string() const { return get(*this); }
const char *text::c_str() const { return get(*this); }

const char *get(text t) { return get(t, now); }

const char *get(text t, lang l)
{
	const auto &v = all().by[int(l)];
	return t.id < v.size() ? v[t.id] : "";
}

std::string fill(text t, with w) { return fill(std::string(get(t)), w); }

std::string fill(const std::string &tmpl, with w)
{
	std::vector<std::pair<std::string, std::string>> v;
	for (const auto &p : w) v.push_back({ p.first, p.second });
	return fill_in(tmpl, v);
}

line::line(text t, std::initializer_list<std::pair<const char *, line>> a) : id(t.id)
{
	for (const auto &p : a) args.push_back({ p.first, p.second });
}

std::string line::in(lang l) const
{
	if (id < 0) {
		if (l == lang::en || raw.empty()) return raw;
		const line known = from_net(raw);   // (src/net's English: its own string if it's a known one)
		return known.id >= 0 ? known.in(l) : raw;
	}
	std::vector<std::pair<std::string, std::string>> v;
	for (const auto &a : args) v.push_back({ a.first, a.second.in(l) });
	return fill_in(get(text{ uint16_t(id) }, l), v);
}

std::string line::str() const { return in(now); }
std::string line::en() const { return in(lang::en); }

// src/net's sentences (session.cpp, link.cpp, room_link.cpp: the same in every game), as this game's NET_ strings
line from_net(const std::string &s)
{
	auto id = [](const char *key) { return find(key); };
	auto known = [&](const char *key) -> line {
		const int i = id(key);
		return i < 0 ? line(s) : line(text{ uint16_t(i) });
	};
	auto with = [&](const char *key, std::initializer_list<std::pair<const char *, line>> a) -> line {
		const int i = id(key);
		return i < 0 ? line(s) : line(text{ uint16_t(i) }, a);
	};
	auto starts = [&](const char *p) { return s.compare(0, strlen(p), p) == 0; };
	auto ends = [&](const char *p) { const size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; };
	static const std::pair<const char *, const char *> whole[] = {
		{ "The game couldn't start from the host's settings.", "NET_HOST_SETTINGS" },
		{ "Your opponent couldn't finish connecting.", "NET_OPPONENT_UNFINISHED" },
		{ "Couldn't reach the host: check the address, and that the host's game is waiting.", "NET_HOST_UNREACHED" },
		{ "The connection to your opponent was lost.", "NET_LOST" },
		{ "Connection interrupted", "NET_INTERRUPTED" },
		{ "Your opponent is playing another game.", "NET_OPPONENT_OTHER_GAME" },
		{ "Your game files (the ROM) aren't the same as your opponent's.", "NET_OPPONENT_OTHER_ROM" },
		{ "The host is playing another game.", "NET_HOST_OTHER_GAME" },
		{ "Your game files (the ROM) aren't the same as the host's.", "NET_HOST_OTHER_ROM" },
		{ "The host couldn't take you.", "NET_HOST_REFUSED" },
		{ "The game couldn't start.", "NET_GAME_NOT_STARTED" },
		{ "Your opponent left the match.", "NET_OPPONENT_LEFT" },
		{ "The game couldn't go back to a frame (rollback).", "NET_ROLLBACK_FAILED" },
		{ "You left the match.", "NET_YOU_LEFT" },
		{ "Couldn't start the network (Winsock).", "NET_WINSOCK" },
		{ "The match's start couldn't be loaded.", "NET_WATCH_START" },
	};
	for (const auto &w : whole) if (s == w.first) return known(w.second);
	// "Your opponent has X, you have Y: you both need the same version." / "The host has X, you have Y: ..."
	for (const auto &[lead, key] : { std::pair<const char *, const char *>{ "Your opponent has ", "NET_OPPONENT_VERSION" }, { "The host has ", "NET_HOST_VERSION" } }) {
		const char *tail = ": you both need the same version.";
		const size_t comma = s.find(", you have ");
		if (starts(lead) && ends(tail) && comma != std::string::npos && comma > strlen(lead)) {
			const size_t from = strlen(lead), mine = comma + strlen(", you have ");
			return with(key, { { "theirs", line(s.substr(from, comma - from)) }, { "mine", line(s.substr(mine, s.size() - strlen(tail) - mine)) } });
		}
	}
	if (starts("The match went out of step (a desync) at frame ") && ends(".")) {
		const size_t from = strlen("The match went out of step (a desync) at frame ");
		return with("NET_DESYNC_AT", { { "frame", line(s.substr(from, s.size() - 1 - from)) } });
	}
	if (starts("Couldn't open a UDP socket: ")) return with("NET_NO_SOCKET", { { "why", line(s.substr(strlen("Couldn't open a UDP socket: "))) } });
	if (starts("Couldn't open a port: ")) return with("NET_NO_PORT", { { "why", line(s.substr(strlen("Couldn't open a port: "))) } });
	if (starts("Couldn't use port ")) {
		const char *mid = " (another program may have it): ";
		const size_t m = s.find(mid);
		if (m != std::string::npos)
			return with("NET_PORT_TAKEN", { { "port", line(s.substr(strlen("Couldn't use port "), m - strlen("Couldn't use port "))) }, { "why", line(s.substr(m + strlen(mid))) } });
	}
	if (starts("Couldn't find ") && ends(".")) return with("NET_NO_HOST", { { "host", line(s.substr(strlen("Couldn't find "), s.size() - 1 - strlen("Couldn't find "))) } });
	return line(s);
}

// ---- the language ----

const lang_info &info(lang l)
{
	static const lang_info all[LANGS] = {
		{ lang::en, "en", "English", false },
		{ lang::ko, "ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", true },                                   // 한국어
		{ lang::ja, "ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", true },                                   // 日本語
		{ lang::zh_hans, "zh-Hans", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", true },             // 简体中文
		{ lang::zh_hant, "zh-Hant", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87", true },             // 繁體中文
		{ lang::es, "es", "Espa\xC3\xB1ol", false },
		{ lang::pt_br, "pt-BR", "Portugu\xC3\xAAs (Brasil)", false },
		{ lang::fr, "fr", "Fran\xC3\xA7" "ais", false },
		{ lang::ru, "ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", false },             // Русский
		{ lang::de, "de", "Deutsch", false },
		{ lang::it, "it", "Italiano", false },
		{ lang::pl, "pl", "Polski", false },
		{ lang::tr, "tr", "T\xC3\xBCrk\xC3\xA7" "e", false },
		{ lang::pseudo, "pseudo", "Pseudo", false },
	};
	const int i = int(l);
	return all[i >= 0 && i < LANGS ? i : 0];
}

const char *choice_key(choice c) { return c.automatic() ? "auto" : info(c.l()).code; }

bool choice_from(const std::string &key, choice &c)
{
	// (as a person might write it in settings.ini: JA, zh-hans, pt_br, spaces round it)
	const size_t a = key.find_first_not_of(" \t\r\n"), b = key.find_last_not_of(" \t\r\n");
	const std::string k = a == std::string::npos ? "" : key.substr(a, b - a + 1);
	auto same = [&](const char *code) {
		if (k.size() != strlen(code)) return false;
		for (size_t i = 0; i < k.size(); i++) {
			const char x = k[i] == '_' ? '-' : char(tolower((unsigned char)k[i]));
			if (x != tolower((unsigned char)code[i])) return false;
		}
		return true;
	};
	for (int i = 0; i < CHOICES; i++)
		if (same(choice_key(choice{ i }))) { c = choice{ i }; return true; }
	return false;
}

std::string choice_name(choice c)
{
	if (!c.automatic()) return info(c.l()).name;
	const int i = find("LANG_AUTO");
	return i < 0 ? "Auto" : get(text{ uint16_t(i) });
}

choice step(choice c, int d) { return choice{ ((c.i + d) % CHOICES + CHOICES) % CHOICES }; }

bool from_locale_known(const char *language)
{
	static const char *const known[] = { "en", "ko", "ja", "zh", "es", "pt", "fr", "ru", "de", "it", "pl", "tr" };
	for (const char *k : known) if (language && !strcmp(language, k)) return true;
	return false;
}

lang from_locale(const char *language, const char *country)
{
	const std::string l = language ? language : "", c = country ? country : "";
	if (l == "zh" || l.rfind("zh-", 0) == 0 || l.rfind("zh_", 0) == 0) {   // (a script given, by it; else by the region)
		// (its parts after zh, in the language and the country both: SDL's macOS locale is cut at its last '-', so
		// a bare zh-Hant comes as zh and Hant, zh-Hant-TW as zh-Hant and TW)
		std::vector<std::string> parts;
		std::string part;
		for (const char ch : l.substr(2) + "-" + c) {
			if (ch == '-' || ch == '_') { if (!part.empty()) parts.push_back(part); part.clear(); }
			else part += char(tolower((unsigned char)ch));
		}
		if (!part.empty()) parts.push_back(part);
		for (const std::string &p : parts) {
			if (p == "hant") return lang::zh_hant;
			if (p == "hans") return lang::zh_hans;
		}
		for (const std::string &p : parts)
			if (p == "tw" || p == "hk" || p == "mo") return lang::zh_hant;
		return lang::zh_hans;   // (CN, SG, none)
	}
	if (l == "pt") return lang::pt_br;   // (Portugal's Portuguese reads Brazil's well enough: the only one there is)
	for (int i = 1; i < REAL_LANGS; i++)
		if (l == info(lang(i)).code) return lang(i);
	return lang::en;
}

lang system()
{
	int n = 0;
	SDL_Locale **v = SDL_GetPreferredLocales(&n);
	lang l = lang::en;
	for (int i = 0; v && i < n; i++) {   // (the first of these the system prefers: Korean first and Japanese second gets
		// Korean; French first, then Japanese, French; one not here at all, e.g. Dutch, then German, German)
		if (!v[i] || !v[i]->language) continue;
		std::string lg = v[i]->language;
		const size_t cut = lg.find_first_of("-_");
		const std::string base = cut == std::string::npos ? lg : lg.substr(0, cut);
		if (!from_locale_known(base.c_str())) continue;
		l = from_locale(lg.c_str(), v[i]->country);
		break;
	}
	SDL_free(v);
	return l;
}

lang resolve(choice c)
{
	if (const char *e = getenv("VF_LANG"); e && *e) {
		for (int i = 0; i < LANGS; i++)
			if (!strcmp(e, info(lang(i)).code)) return lang(i);
	}
	return c.automatic() ? system() : c.l();
}

void set(lang l) { now = l; }
lang current() { return now; }
const char *code(lang l) { return info(l).code; }
bool cjk() { return info(now).cjk; }

std::string css_class()
{
	std::string c = info(now).code;
	for (char &ch : c) if (ch == '-') ch = '_';   // (lang-zh_Hans, lang-pt_BR)
	return (info(now).cjk ? "cjk lang-" : "lang-") + c;
}

// ---- the tables ----

int find(const char *key)
{
	for (size_t i = 0; i < data::COUNT; i++)
		if (!strcmp(data::KEY[i], key)) return int(i);
	return -1;
}

std::vector<std::string> places(const char *t)
{
	std::vector<std::string> v;
	for (const char *p = t; (p = strchr(p, '{')); ) {
		const char *q = strchr(p, '}');
		if (!q) break;
		v.emplace_back(p + 1, q);
		p = q + 1;
	}
	return v;
}

std::vector<std::string> check()
{
	std::vector<std::string> out;
	const size_t n = data::COUNT;
	std::set<std::string> keys;
	for (size_t i = 0; i < n; i++) {
		if (!keys.insert(data::KEY[i]).second) out.push_back(std::string("the ID ") + data::KEY[i] + " is in en.inc twice");
		if (!data::EN[i] || !*data::EN[i]) out.push_back(std::string(data::KEY[i]) + ": no English");
	}
	auto one = [&](const char *name, const entry *e, size_t count) {
		std::vector<int> seen(n, 0);
		for (size_t i = 0; i < count; i++) {
			if (e[i].id >= n) { out.push_back(std::string(name) + ": an entry for an ID that isn't in en.inc"); continue; }
			const char *key = data::KEY[e[i].id];
			if (seen[e[i].id]++) out.push_back(std::string(name) + ": " + key + " is translated twice");
			if (!e[i].text || !*e[i].text) { out.push_back(std::string(name) + ": " + key + " is empty"); continue; }
			std::vector<std::string> a = places(data::EN[e[i].id]), b = places(e[i].text);
			std::sort(a.begin(), a.end());
			std::sort(b.begin(), b.end());
			if (a != b) {
				std::string want, got;
				for (const auto &s : a) want += "{" + s + "}";
				for (const auto &s : b) got += "{" + s + "}";
				out.push_back(std::string(name) + ": " + key + " has " + (got.empty() ? "no places" : got) + ", English " + (want.empty() ? "none" : want));
			}
		}
		for (size_t i = 0; i < n; i++) if (!seen[i]) out.push_back(std::string(name) + ": " + data::KEY[i] + " isn't translated");
	};
	for (int l = 1; l < REAL_LANGS; l++) one((std::string(info(lang(l)).code) + ".inc").c_str(), data::TABLE[l], data::TABLE_COUNT[l]);
	return out;
}

std::string pseudo(const std::string &s)
{
	// every letter accented (the {places} as they are), then about 40% more of it, in brackets: what's cut off shows
	std::string out = "[";
	size_t letters = 0;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '{') {
			const size_t close = s.find('}', i);
			if (close != std::string::npos) { out += s.substr(i, close - i + 1); i = close; continue; }
		}
		const char *a = accented(s[i]);
		out += a ? a : std::string(1, s[i]);
		if ((uint8_t(s[i]) >= 0x20 && uint8_t(s[i]) < 0x80) || (uint8_t(s[i]) & 0xC0) == 0xC0) letters++;
	}
	const size_t more = (letters * 4 + 9) / 10;
	if (more) out += " ";
	for (size_t i = 1; i < more; i++) out += (i % 5 == 0) ? " " : "~";
	return out + "]";
}

} // namespace tr
