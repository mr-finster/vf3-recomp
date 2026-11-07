// The menus' languages: English and 12 more (docs/i18n-design.md). Shared with VF1 and VF2 Recomp as it is:
// each game has its own strings (src/host/lang/en.inc and one file a language; their IDs in text.h), this is how they're
// looked up, filled in and switched.
//
// A string is a whole sentence with {name} places, looked up by its ID (tr::ONLINE, a tr::text) in the language on
// now, English where a translation is missing. The game's own text (the ROM's) is never translated, and the language
// never goes into a match, a save or the game's state: it's only what the menus show.
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace tr {

// The languages (each a table, lang/<code>.inc). pseudo: English lengthened about 40% with accents, for finding what's
// cut off (tests, VF_LANG=pseudo). Text that needs shaping (Arabic, Hindi, Thai) isn't drawn by the menus' fonts yet.
enum class lang : int { en, ko, ja, zh_hans, zh_hant, es, pt_br, fr, ru, de, it, pl, tr, pseudo };
constexpr int LANGS = 14;      // (pseudo's included)
constexpr int REAL_LANGS = 13; // (the ones a player can choose)
struct lang_info
{
	lang l;
	const char *code;   // settings.ini's and VF_LANG's: en, ko, ja, zh-Hans, zh-Hant, es, pt-BR, fr, ru, de, it, pl, tr (pseudo)
	const char *name;   // in its own script, as the setting lists it: English, 한국어, 日本語, 简体中文, ...
	bool cjk;           // (Chinese, Japanese, Korean: the menus' CJK styles, no italics for its glyphs)
};
const lang_info &info(lang l);

// A string's ID (text.h names each one): its text in the language on now, or in the one asked for
struct text
{
	uint16_t id;
	operator std::string() const;   // (so a tr::text goes wherever a std::string does)
	const char *c_str() const;
	bool operator==(const text &o) const { return id == o.id; }
	bool operator!=(const text &o) const { return id != o.id; }
};
const char *get(text t);
const char *get(text t, lang l);

using with = std::initializer_list<std::pair<const char *, std::string>>;
// fill(tr::WATCH_LOOKING, {{"code", "QX7-KMP"}}): the {name} places filled (each everywhere it's found)
std::string fill(text t, with w);
std::string fill(const std::string &tmpl, with w);   // (a string looked up already: markup put round it first)

// A line that's both shown and logged: kept as its ID and what fills it, so it's shown in the language on now (also
// after a switch) and logged in English. A raw one is a string from elsewhere (src/net's English, the system's),
// shown as it is unless it's one of src/net's known sentences (from_net).
struct line
{
	int id = -1;                                        // (-1: raw)
	std::string raw;
	std::vector<std::pair<std::string, line>> args;     // (each filled in where {its name} is; a raw one for a name or a number)
	line() = default;
	line(text t) : id(t.id) {}
	line(text t, std::initializer_list<std::pair<const char *, line>> a);
	line(const std::string &s) : raw(s) {}               // (raw)
	line(const char *s) : raw(s) {}
	bool empty() const { return id < 0 && raw.empty(); }
	void clear() { *this = line(); }
	std::string str() const;                            // in the language on now
	std::string en() const;                             // in English (the log)
	std::string in(lang l) const;
	bool operator==(text t) const { return id == t.id; }
};
// src/net's sentences (the session's ends and refusals, the sockets' errors), English there for every game: the known
// ones as lines of this game's strings (NET_*), the rest raw
line from_net(const std::string &english);

// ---- the language ----
// The setting (settings.ini's language): "auto" (the system's), or a language's code. Stepped through in the order of
// lang (Auto first, then English, Korean, Japanese, ...).
struct choice
{
	int i = 0;   // (0 auto; else 1 + the lang)
	bool automatic() const { return i == 0; }
	lang l() const { return lang(i - 1); }
	static choice of(lang l) { return { int(l) + 1 }; }
	bool operator==(const choice &o) const { return i == o.i; }
};
constexpr int CHOICES = 1 + REAL_LANGS;
const char *choice_key(choice c);                 // "auto", or the language's code (settings.ini)
bool choice_from(const std::string &key, choice &c);   // (any case, - or _, spaces round it: JA, zh_hans)
std::string choice_name(choice c);                // as the setting shows it: Auto (in the language on), or its own name
choice step(choice c, int d);                     // (Left / Right on the setting)
lang system();                                    // the system's preferred language, if one of these (else English)
// (one locale's: zh with Hant (zh-Hant, zh-Hant-TW, or zh and Hant as SDL's macOS gives a bare zh-Hant) Traditional,
// with Hans Simplified, else zh-TW, zh-HK, zh-MO Traditional, other zh Simplified; pt-* Brazilian; the rest by
// language; English for none of these)
lang from_locale(const char *language, const char *country);
bool from_locale_known(const char *language);
lang resolve(choice c);                           // (VF_LANG in the environment over it: a code, or pseudo)
void set(lang l);
lang current();
const char *code(lang l);
bool cjk();                                       // (Chinese, Japanese or Korean on)
// the documents' body class: "lang-<code>", and "cjk" for Chinese, Japanese, Korean (for assets/ui's styles by
// language: none has any yet, the fallback fonts doing what's needed)
std::string css_class();

// ---- the tables (text.cpp, the game's own) and checking them (tools/langtest) ----
struct entry { uint16_t id; const char *text; };
namespace data {
extern const char *const KEY[];      // each ID's name, in order
extern const char *const EN[];       // English, in the same order
extern const size_t COUNT;
extern const entry *const TABLE[];   // each language's translations (by lang; en's none), each an ID and its text, in any order
extern const size_t TABLE_COUNT[];
}
int find(const char *key);                        // an ID by its name (-1: none)
std::vector<std::string> check();                 // what's wrong with the tables: an ID missing or twice, extra, {places} not the same
std::vector<std::string> places(const char *t);   // a string's {names}, in order
std::string pseudo(const std::string &english);   // (lang::pseudo's text)

} // namespace tr
