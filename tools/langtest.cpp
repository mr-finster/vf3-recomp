// langtest: the menus' strings (src/host/lang/*.inc) checked, with no ROM and no window.
//   langtest [assets folder]   (default ./assets)
// - every ID in every language exactly once, none extra, each with the English's {places} (tr::check);
// - every letter of every language in the menus' fonts: Barlow Condensed, else the language's fallback subset
//   (assets/fonts), so nothing's drawn as an empty box; the language names Game > Language lists, in every font set;
// - a line's English and its translation rendered both ways (tr::line: shown in the language on, logged in English),
//   src/net's sentences mapped (tr::from_net), the fill of places;
// - the settings' language codes both ways (settings.ini's language = ...), the system's locales picked as they should
//   be (zh-TW/zh-HK Traditional, zh-CN/zh-SG Simplified, a script given by it (zh-Hant, and zh and Hant as SDL's macOS
//   gives that), pt-PT Brazilian, the first known one), pseudo's length.
// Exit 1 if anything's wrong. With --list, each language's strings that are the same as the English (to look at).
#include "../src/host/text.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int fails = 0;
void check(bool ok, const std::string &what)
{
	if (!ok) { fprintf(stderr, "langtest: FAIL %s\n", what.c_str()); fails++; }
	else printf("ok   %s\n", what.c_str());
}

// UTF-8 to code points (a bad byte: U+FFFD)
std::vector<char32_t> decode(const char *s)
{
	std::vector<char32_t> out;
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		char32_t c = *p;
		int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (n < 0) { out.push_back(0xFFFD); p++; continue; }
		if (n) c &= 0x3F >> n;
		p++;
		for (int i = 0; i < n; i++, p++) {
			if ((*p & 0xC0) != 0x80) { c = 0xFFFD; break; }
			c = c << 6 | (*p & 0x3F);
		}
		out.push_back(c);
	}
	return out;
}

struct font
{
	std::string file;
	FT_Face face = nullptr;
	bool has(char32_t c) const { return face && FT_Get_Char_Index(face, c) != 0; }
};

} // namespace

int main(int argc, char **argv)
{
	std::string assets = "assets";
	bool list = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--list")) list = true;
		else assets = argv[i];
	}

	// ---- the tables ----
	const std::vector<std::string> problems = tr::check();
	for (const std::string &p : problems) fprintf(stderr, "langtest: %s\n", p.c_str());
	check(problems.empty(), "every ID in every language once, none extra, the same {places} (" + std::to_string(tr::data::COUNT) + " strings, " +
	      std::to_string(tr::REAL_LANGS - 1) + " translations)");

	// a place's name: letters, digits and _ only (a typo'd brace shows up here)
	{
		int bad = 0;
		for (size_t i = 0; i < tr::data::COUNT; i++)
			for (const std::string &p : tr::places(tr::data::EN[i]))
				if (p.empty() || p.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") != std::string::npos) {
					fprintf(stderr, "langtest: %s's place {%s}\n", tr::data::KEY[i], p.c_str());
					bad++;
				}
		check(bad == 0, "every {place}'s name is plain");
	}
	// none starts or ends with a space (an ordinary one, a no-break one, an ideographic one): what the layout doesn't
	// collapse puts a row's label out of line with the others
	{
		auto spaced = [](const char *t) {
			const std::string s = t;
			for (const char *sp : { " ", "\xC2\xA0", "\xE3\x80\x80", "\t", "\n" }) {
				const size_t n = strlen(sp);
				if (s.size() >= n && (s.compare(0, n, sp) == 0 || s.compare(s.size() - n, n, sp) == 0)) return true;
			}
			return false;
		};
		int bad = 0;
		for (size_t i = 0; i < tr::data::COUNT; i++)
			if (spaced(tr::data::EN[i])) { fprintf(stderr, "langtest: en %s starts or ends with a space\n", tr::data::KEY[i]); bad++; }
		for (int l = 1; l < tr::REAL_LANGS; l++)
			for (size_t i = 0; i < tr::data::TABLE_COUNT[l]; i++)
				if (spaced(tr::data::TABLE[l][i].text)) {
					fprintf(stderr, "langtest: %s %s starts or ends with a space\n", tr::code(tr::lang(l)), tr::data::KEY[tr::data::TABLE[l][i].id]);
					bad++;
				}
		check(bad == 0, "no string starts or ends with a space");
	}
	// the same as the English: listed (a name of a thing, or not translated yet)
	if (list)
		for (int l = 1; l < tr::REAL_LANGS; l++)
			for (size_t i = 0; i < tr::data::COUNT; i++)
				if (!strcmp(tr::get(tr::text{ uint16_t(i) }, tr::lang(l)), tr::data::EN[i]))
					printf("same as English: %s %s \"%s\"\n", tr::code(tr::lang(l)), tr::data::KEY[i], tr::data::EN[i]);

	// ---- the fonts: every letter drawn ----
	FT_Library ft;
	if (FT_Init_FreeType(&ft)) { fprintf(stderr, "langtest: FreeType\n"); return 1; }
	auto load = [&](const std::string &name) {
		font f;
		f.file = name;
		if (FT_New_Face(ft, (assets + "/fonts/" + name).c_str(), 0, &f.face)) f.face = nullptr;
		check(f.face != nullptr, "the font " + name + " is there");
		return f;
	};
	// (Barlow's three are the same letters; the fallbacks as menu.cpp's FALLBACK_FONTS, each language's own first)
	const font barlow = load("BarlowCondensed-SemiBold.ttf"), barlow_i = load("BarlowCondensed-BoldItalic.ttf");
	std::map<std::string, std::vector<font>> fallback;
	for (const char *script : { "KR", "JP", "SC", "TC" })
		for (const char *w : { "Medium", "Bold" }) fallback[script].push_back(load(std::string("NotoSans") + script + "-" + w + ".ttf"));
	for (const char *w : { "Medium", "SemiBold", "BoldItalic" }) fallback["Cyr"].push_back(load(std::string("NotoSansCyr-") + w + ".ttf"));
	auto drawn = [&](char32_t c, const std::vector<std::string> &scripts) {
		if (c < 0x20 || c == 0xA0) return true;   // (spaces: no glyph needed)
		if (barlow.has(c) && barlow_i.has(c)) return true;
		for (const std::string &s : scripts) {   // (in every weight of a fallback that has it: bold text, italic text)
			bool all = true, any = false;
			for (const font &f : fallback[s]) { any = any || f.has(c); all = all && f.has(c); }
			if (any) return all;
		}
		return false;
	};
	const std::map<tr::lang, std::vector<std::string>> scripts = {
		{ tr::lang::ko, { "KR" } }, { tr::lang::ja, { "JP" } }, { tr::lang::zh_hans, { "SC" } }, { tr::lang::zh_hant, { "TC" } }, { tr::lang::ru, { "Cyr" } } };
	for (int l = 0; l < tr::LANGS; l++) {
		const tr::lang lg = tr::lang(l);
		std::vector<std::string> own = scripts.count(lg) ? scripts.at(lg) : std::vector<std::string>();
		std::set<char32_t> missing;
		for (size_t i = 0; i < tr::data::COUNT; i++)
			for (char32_t c : decode(tr::get(tr::text{ uint16_t(i) }, lg)))
				if (!drawn(c, own)) missing.insert(c);
		std::string m;
		for (char32_t c : missing) { char b[16]; snprintf(b, sizeof b, " U+%04X", unsigned(c)); m += b; }
		check(missing.empty(), std::string(tr::code(lg)) + ": every letter in its fonts" + (m.empty() ? "" : " (missing:" + m + ")"));
	}
	{   // the language names (Game > Language), each in its own script, in whatever language's on
		std::set<char32_t> missing;
		for (int l = 0; l < tr::REAL_LANGS; l++) {
			std::vector<std::string> own = scripts.count(tr::lang(l)) ? scripts.at(tr::lang(l)) : std::vector<std::string>();
			for (char32_t c : decode(tr::info(tr::lang(l)).name)) if (!drawn(c, own)) missing.insert(c);
		}
		check(missing.empty(), "the languages' names (Game > Language) in their fonts");
	}

	// ---- lines: shown in the language on, logged in English ----
	tr::set(tr::lang::ko);
	const tr::line said(tr::PLAYER_COULDNT_JOIN, { { "why", tr::line(tr::OPPONENT_LEFT, { { "name", "AKIRA" } }) } });
	check(said.en() == "A player couldn't join: AKIRA left the match.", "a line in English for the log (" + said.en() + ")");
	check(said.str() != said.en() && said.str().find("AKIRA") != std::string::npos, "the same line in Korean, the name in it (" + said.str() + ")");
	const tr::line net("The host has v0.1.9-alpha, you have v0.2.0-alpha: you both need the same version.");
	check(tr::from_net(net.raw).id == tr::NET_HOST_VERSION.id, "src/net's version refusal known");
	check(net.str().find("v0.1.9-alpha") != std::string::npos && net.str().find("v0.2.0-alpha") != std::string::npos && net.str() != net.raw,
	      "...and shown in Korean with both versions (" + net.str() + ")");
	check(tr::from_net("The match went out of step (a desync) at frame 1234.").id == tr::NET_DESYNC_AT.id, "src/net's desync known, its frame filled in");
	check(tr::line("Something the system said").str() == "Something the system said", "an unknown raw line shown as it is");
	tr::set(tr::lang::en);
	check(tr::fill(tr::IN_LINE, { { "code", "KXQ-4M7" }, { "n", "2" }, { "of", "3" } }) == "In line for room KXQ-4M7: 2 of 3. You play the host when it's your turn.", "places filled");
	check(tr::fill(std::string("{a} {a} {b}"), { { "a", "{b}" }, { "b", "x" } }) == "{b} {b} x", "what's filled in is never filled again");

	// ---- the setting ----
	for (int i = 0; i < tr::CHOICES; i++) {
		tr::choice c{ i }, back;
		const bool ok = tr::choice_from(tr::choice_key(c), back) && back == c;
		check(ok, std::string("language = ") + tr::choice_key(c) + " read back as itself");
	}
	tr::choice junk{ 3 };
	check(!tr::choice_from("klingon", junk) && junk.i == 3, "an unknown language code isn't taken");
	{   // (as a person might write it: any case, _ for -, spaces round it; written back as its code)
		struct { const char *written; tr::lang want; } as[] = { { "JA", tr::lang::ja }, { "zh-hans", tr::lang::zh_hans }, { " zh_HANT ", tr::lang::zh_hant },
			{ "pt-br", tr::lang::pt_br }, { "ko ", tr::lang::ko }, { "\tDE\r", tr::lang::de } };
		for (const auto &a : as) {
			tr::choice c;
			check(tr::choice_from(a.written, c) && c == tr::choice::of(a.want) && !strcmp(tr::choice_key(c), tr::code(a.want)),
			      std::string("language = \"") + a.written + "\" read as " + tr::code(a.want));
		}
		tr::choice c{ 3 };
		check(tr::choice_from(" AUTO", c) && c.automatic(), "language = AUTO read as auto");
		check(!tr::choice_from("ja-", junk) && !tr::choice_from("", junk) && junk.i == 3, "nor a code with more to it, nor none");
	}
	check(tr::step(tr::choice{ 0 }, -1).i == tr::CHOICES - 1 && tr::step(tr::choice{ tr::CHOICES - 1 }, 1).i == 0, "Left and Right go round the list");
	struct { const char *lang, *country; tr::lang want; } locales[] = {
		{ "ko", "KR", tr::lang::ko }, { "ja", "JP", tr::lang::ja }, { "zh", "CN", tr::lang::zh_hans }, { "zh", "SG", tr::lang::zh_hans },
		{ "zh", "TW", tr::lang::zh_hant }, { "zh", "HK", tr::lang::zh_hant }, { "zh", "MO", tr::lang::zh_hant }, { "zh", nullptr, tr::lang::zh_hans },
		{ "zh-Hant", nullptr, tr::lang::zh_hant }, { "zh", "Hant", tr::lang::zh_hant }, { "zh", "Hant_TW", tr::lang::zh_hant },
		{ "zh-Hant", "TW", tr::lang::zh_hant }, { "zh-Hant", "HK", tr::lang::zh_hant }, { "zh", "Hans", tr::lang::zh_hans }, { "zh", "Hans_CN", tr::lang::zh_hans },
		{ "zh-Hans", "HK", tr::lang::zh_hans }, { "zh-Hans", nullptr, tr::lang::zh_hans }, { "zh_TW", nullptr, tr::lang::zh_hant }, { "zh", "hant", tr::lang::zh_hant },
		{ "pt", "PT", tr::lang::pt_br }, { "pt", "BR", tr::lang::pt_br }, { "es", "MX", tr::lang::es },
		{ "fr", "CA", tr::lang::fr }, { "ru", "RU", tr::lang::ru }, { "de", "AT", tr::lang::de }, { "it", "IT", tr::lang::it }, { "pl", "PL", tr::lang::pl },
		{ "tr", "TR", tr::lang::tr }, { "en", "GB", tr::lang::en }, { "nl", "NL", tr::lang::en } };
	for (const auto &l : locales)
		check(tr::from_locale(l.lang, l.country) == l.want, std::string("the locale ") + l.lang + (l.country ? std::string(", ") + l.country : "") + ": " + tr::code(l.want));

	// ---- pseudo: about 40% longer, its places kept ----
	{
		const std::string p = tr::pseudo("Connecting to {name}...");
		check(p.find("{name}") != std::string::npos && p.front() == '[' && p.back() == ']', "pseudo keeps its places, in brackets (" + p + ")");
		size_t longer = 0, n = 0;
		for (size_t i = 0; i < tr::data::COUNT; i++) { longer += decode(tr::get(tr::text{ uint16_t(i) }, tr::lang::pseudo)).size(); n += decode(tr::data::EN[i]).size(); }
		check(longer * 10 >= n * 13, "pseudo is about 40% longer (" + std::to_string(longer * 100 / std::max<size_t>(n, 1)) + "% of the English)");
	}
	printf("%s\n", fails ? "langtest: FAILED" : "langtest: all passed");
	return fails ? 1 : 0;
}
