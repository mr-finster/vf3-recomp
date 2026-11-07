#include "menu.h"
#include "text.h"
#include "flags.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_SDL.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace {

std::string escape(const std::string &s)
{
	std::string o;
	for (char c : s) {
		if (c == '&') o += "&amp;";
		else if (c == '<') o += "&lt;";
		else if (c == '>') o += "&gt;";
		else o += c;
	}
	return o;
}

// The fonts for what Barlow Condensed hasn't got (assets/fonts: each a subset of Noto Sans with only the letters the
// strings use, tools/fonts/subset.py): Korean, Japanese, Chinese (Simplified, Traditional) and Cyrillic (condensed, as
// Barlow; its italic for Barlow's). Fallback faces: a glyph is looked for in them only when Barlow has none.
struct fallback_font { const char *file, *family; int weight; bool italic; };
const fallback_font FALLBACK_FONTS[] = {
	{ "NotoSansKR-Medium.ttf", "Noto Sans KR", 500, false }, { "NotoSansKR-Bold.ttf", "Noto Sans KR", 700, false },
	{ "NotoSansJP-Medium.ttf", "Noto Sans JP", 500, false }, { "NotoSansJP-Bold.ttf", "Noto Sans JP", 700, false },
	{ "NotoSansSC-Medium.ttf", "Noto Sans SC", 500, false }, { "NotoSansSC-Bold.ttf", "Noto Sans SC", 700, false },
	{ "NotoSansTC-Medium.ttf", "Noto Sans TC", 500, false }, { "NotoSansTC-Bold.ttf", "Noto Sans TC", 700, false },
	{ "NotoSansCyr-Medium.ttf", "Noto Sans Cyr", 500, false }, { "NotoSansCyr-SemiBold.ttf", "Noto Sans Cyr", 600, false },
	{ "NotoSansCyr-BoldItalic.ttf", "Noto Sans Cyr", 700, true },
};
// (the language on's own first: Chinese, Japanese and Korean share characters each draws its own way)
Rml::StringList fallback_order(tr::lang l)
{
	switch (l) {
	case tr::lang::ko: return { "Noto Sans KR", "Noto Sans JP", "Noto Sans SC", "Noto Sans TC", "Noto Sans Cyr" };
	case tr::lang::ja: return { "Noto Sans JP", "Noto Sans SC", "Noto Sans TC", "Noto Sans KR", "Noto Sans Cyr" };
	case tr::lang::zh_hans: return { "Noto Sans SC", "Noto Sans TC", "Noto Sans JP", "Noto Sans KR", "Noto Sans Cyr" };
	case tr::lang::zh_hant: return { "Noto Sans TC", "Noto Sans SC", "Noto Sans JP", "Noto Sans KR", "Noto Sans Cyr" };
	default: return { "Noto Sans Cyr", "Noto Sans JP", "Noto Sans KR", "Noto Sans SC", "Noto Sans TC" };
	}
}

// The menu's key hints (under the rows), in the language on: each key's name, then what it does
std::string hint_rml()
{
	auto key = [](const std::string &k) { return "<span class=\"key\">" + escape(k) + "</span>"; };
	return key(tr::KEYS_ARROWS) + " / " + key(tr::KEYS_DPAD) + " " + escape(tr::HINT_CHOOSE) +
	       " &nbsp;&nbsp;" + key(tr::KEYS_LEFT_RIGHT) + " " + escape(tr::HINT_CHANGE) +
	       " &nbsp;&nbsp;" + key("Enter") + " / " + key("A") + " " + escape(tr::HINT_SELECT) +
	       " &nbsp;&nbsp;" + key("Esc") + " / " + key("B") + " " + escape(tr::HINT_BACK);
}

int first_selectable(const menu_page &p)
{
	if (p.start >= 0 && p.start < int(p.items.size()) && !p.items[size_t(p.start)].info) return p.start;
	for (size_t i = 0; i < p.items.size(); i++)
		if (!p.items[i].info) return int(i);
	return 0;
}

} // namespace

std::string menu_escape(const std::string &s) { return escape(s); }

std::string menu_row(int i, int sel, const std::string &cls)
{
	return "class=\"row" + (cls.empty() ? "" : " " + cls) + (i == sel ? " selected" : "") + "\" id=\"r" + std::to_string(i) + "\"";
}

// Mouse over a row selects it; a click selects and uses it.
struct menu_listener : Rml::EventListener
{
	game_menu *menu = nullptr;
	void ProcessEvent(Rml::Event &ev) override
	{
		Rml::Element *e = ev.GetTargetElement();
		while (e && !e->IsClassSet("row")) e = e->GetParentNode();
		if (!e || e->IsClassSet("info") || menu->capturing()) return;   // (typing into a row: the pointer doesn't move off it)
		// (a row comes under a still pointer when the rows are made again, a status line's change: that's no choice;
		// on a Steam Deck the selection kept going back to the row under the pointer)
		if (ev.GetId() == Rml::EventId::Mouseover && !menu->m_pointer_moved) return;
		const int i = atoi(e->GetId().c_str() + 1);
		menu->select(i);
		if (ev.GetId() == Rml::EventId::Click) {
			const menu_item &it = menu->m_stack.back().items[i];
			const Rml::Element *t = ev.GetTargetElement();
			if (it.activate) menu->activate();
			else if (it.change) menu->click_change(t && t->IsClassSet("back") ? -1 : +1);   // (the "<": down)
		}
	}
};

struct game_menu::impl
{
	SDL_Window *win = nullptr;
	SDL_Renderer *ren = nullptr;
	std::unique_ptr<SystemInterface_SDL> sys;
	std::unique_ptr<RenderInterface_SDL> gfx;
	Rml::Context *ctx = nullptr;
	Rml::ElementDocument *doc = nullptr;
	Rml::ElementDocument *overlay = nullptr;   // drawn over the game, menu or not (the FPS)
	bool mono = false;   // JetBrains Mono loaded (the connection display's; Barlow if not)
	Rml::ElementDocument *screen = nullptr;    // a page that's a screen of its own (menu_page::screen)
	Rml::ElementDocument *active(bool screen_page) const { return screen_page && screen ? screen : doc; }
	menu_listener listener;
};

game_menu::game_menu() : m(std::make_unique<impl>()) {}

game_menu::~game_menu()
{
	if (m->ctx) {
		Rml::RemoveContext(m->ctx->GetName());
		Rml::Shutdown();
	}
}

bool game_menu::ready() const { return m->doc != nullptr; }
SDL_Window *game_menu::window() const { return m->win; }

void game_menu::textures_lost() { if (ready()) Rml::ReleaseTextures(); }

void game_menu::set_fps(const std::string &text)
{
	if (!m->overlay || text == m_fps) return;
	m_fps = text;
	if (Rml::Element *e = m->overlay->GetElementById("fps")) e->SetInnerRML(escape(text));
}

void game_menu::show_net(const std::string &rml, const char *cls)
{
	const std::string key = rml + "\n" + cls;
	if (!m->overlay || key == m_net) return;
	m_net = key;
	if (Rml::Element *e = m->overlay->GetElementById("net")) {
		e->SetInnerRML(rml);
		e->SetClassNames(cls);
	}
}

void game_menu::set_net(const std::string &text, int level)
{
	show_net(text.empty() ? "" : "<span class=\"dot\"></span>" + escape(text), level <= 0 ? "plain good" : level == 1 ? "plain fair" : "plain poor");
}

void game_menu::set_net(const net_cells &c)
{
	// (each figure a cell of its own width: its label at the left, its figure at the right; the second row
	// narrower, centred under the first)
	auto cell = [](const char *label, const std::string &v, int w, const char *cls = "") {
		return std::string("<div class=\"c ") + cls + "\" style=\"width: " + std::to_string(w) + "dp;\">" +
		       (*label ? std::string("<span class=\"l\">") + escape(label) + "</span>" : "") + "<span class=\"v\">" + escape(v) + "</span></div>";
	};
	auto num = [](const char *f, double v) { char b[32]; snprintf(b, sizeof b, f, v); return std::string(b); };
	std::string name = c.name;   // (10 letters fit)
	if (name.size() > 10) name = name.substr(0, 9) + "\xE2\x80\xA6";
	std::string rml = "<div class=\"row\"><div class=\"c name\" style=\"width: 118dp;\"><span class=\"v\">" + escape(name) + "</span><span class=\"dot\">" + (m->mono ? "\xE2\x97\x8F" : "\xE2\x80\xA2") + "</span></div>";   // (a circle of the font: drawn round at any size; Barlow has only the bullet)
	const std::string watchers = c.watchers > 0 ? cell(tr::CELL_WATCHING.c_str(), std::to_string(std::min(c.watchers, 99999)), 112, "eye") : "";   // (how many watch, if any: drawn only)
	if (c.watching) {   // (a watched match: one cell, watching and how many, and the score)
		rml = "<div class=\"row\"><div class=\"c name\" style=\"width: 150dp;\"><span class=\"v\">" + escape(c.watchers > 0 ? tr::fill(tr::CELL_WATCHING_N, { { "n", std::to_string(std::min(c.watchers, 99999)) } }) : std::string(tr::CELL_WATCHING)) +
		      "</span><span class=\"dot\">" + (m->mono ? "\xE2\x97\x8F" : "\xE2\x80\xA2") + "</span></div>" + cell("", c.score, 236, "mid") + "</div>";
		show_net(rml, c.level <= 0 ? "cells good" : "cells fair");
		return;
	}
	if (c.interrupted) rml += cell("", tr::CELL_INTERRUPTED, 222, "lost");
	else rml += cell(tr::CELL_PING.c_str(), std::to_string(std::min(c.ping, 9999)), 86) + cell(tr::CELL_DELAY.c_str(), std::to_string(c.delay), 76) + cell(tr::CELL_RB.c_str(), std::to_string(c.rollback), 50);
	rml += cell("", c.relay ? tr::CELL_RELAY : tr::CELL_DIRECT, 66, c.relay ? "via relay" : "via direct") + watchers + "</div>";
	if (c.detailed && !c.interrupted)   // (above it: the first row stays where it is either way)
		rml = "<div class=\"row two\">" + cell(tr::CELL_JITTER.c_str(), num("%.0f", c.jitter), 92) + cell(tr::CELL_RB_AVG.c_str(), num("%.1f", c.rb_avg), 92) +
		      cell(tr::CELL_AHEAD.c_str(), num("%+.1f", c.ahead), 86) + cell(tr::CELL_FRAME.c_str(), num("%.1f", c.frame_ms), 92) + "</div>" + rml;
	show_net(rml, c.interrupted ? "cells poor" : c.level <= 0 ? "cells good" : c.level == 1 ? "cells fair" : "cells poor");
}

void game_menu::toast(const std::string &text, double seconds)
{
	if (!m->overlay) return;
	if (Rml::Element *e = m->overlay->GetElementById("toast")) e->SetInnerRML(text.empty() ? "" : "<span>" + escape(text) + "</span>");
	m_toast_until = 0;
	m_toast_wait = text.empty() ? 0 : seconds;   // (counted from when it can be seen: render)
}

void game_menu::set_notice(const std::string &title, const std::string &sub, const std::string &small)
{
	const std::string key = title + "\n" + sub + "\n" + small;
	if (!m->overlay || key == m_notice) return;
	m_notice = key;   // (an empty one too, "\n\n": else it was made again every pass)
	if (Rml::Element *e = m->overlay->GetElementById("notice")) {
		e->SetInnerRML(title.empty() ? "" : "<div class=\"card\"><div class=\"spin\"></div><div class=\"title\">" + escape(title) + "</div>" +
		               (sub.empty() ? "" : "<div class=\"sub\">" + escape(sub) + "</div>") +
		               (small.empty() ? "" : "<div class=\"small\">" + escape(small) + "</div>") + "</div>");
		e->SetProperty("visibility", title.empty() ? "hidden" : "visible");
	}
}

void game_menu::set_banner(const std::string &title, const std::string &sub, const std::string &button)
{
	const std::string key = title + "\n" + sub + "\n" + button;
	if (!m->overlay || key == m_banner) return;
	m_banner = key;
	// (the key's name in the middle of its box: Barlow sets capitals below a line's middle, and a sign like the
	// Deck's "+" 0.09 em, so each is lifted by its own: overlay.rml's .k and .k.sign)
	const bool sign = button.size() == 1 && !isalnum(uint8_t(button[0]));
	if (Rml::Element *e = m->overlay->GetElementById("banner")) {
		e->SetInnerRML(title.empty() ? "" : "<div class=\"in\"><div class=\"title\">" + escape(title) +
		               (button.empty() ? "" : "<span class=\"button\"><span class=\"k" + std::string(sign ? " sign" : "") + "\">" + escape(button) + "</span></span>") + "</div>" +
		               (sub.empty() ? "" : "<div class=\"sub\">" + escape(sub) + "</div>") + "</div>");
		e->SetProperty("visibility", title.empty() ? "hidden" : "visible");
	}
}

// The size of everything: 1dp a 720th of the window's height, or a 1280th of its width if that's less (a window
// narrower than 16:9, a Steam Deck's 16:10, 4:3: the screens laid out for 1280 x 720 fit across it whole)
static float dp_of(int w, int h) { return std::min(float(h) / 720.0f, float(w) / 1280.0f); }

void game_menu::show_versus(const versus &v, double seconds)
{
	if (!m->overlay) return;
	Rml::Element *e = m->overlay->GetElementById("vs");
	if (!e) return;
	// (each player's flag above their label, at its outer edge)
	auto fl = [](const std::string &c) {
		return flags::index_of(c) < 0 ? std::string() : "<img class=\"vflag\" sprite=\"flag-" + std::string{ char(c[0] | 0x20), char(c[1] | 0x20) } + "\"/>";
	};
	// (each player's fighter, in a game with a choice of them: under the name, smaller)
	auto ft = [](const char *side, const std::string &f) { return f.empty() ? std::string() : "<div class=\"ft " + std::string(side) + "\">" + escape(f) + "</div>"; };
	e->SetInnerRML("<div class=\"fl left\">" + fl(v.left_country) + "</div><div class=\"pl left\">" + escape(v.left_label) + "</div><div class=\"nm left\">" + escape(v.left) + "</div>" + ft("left", v.left_fighter) +
	               "<div class=\"fl right\">" + fl(v.right_country) + "</div><div class=\"pl right\">" + escape(v.right_label) + "</div><div class=\"nm right\">" + escape(v.right) + "</div>" + ft("right", v.right_fighter) +
	               "<div class=\"badge\"><div>VS</div></div>"
	               "<div class=\"bar\"><div class=\"line" + std::string(v.relay ? " relay" : "") + "\"><span class=\"dot\"></span>" + escape(v.line) + "</div>"
	               "<div class=\"sub\">" + escape(v.sub) + "</div></div>");
	// (a long name, 10 letters wide ones, made smaller to fit its side: from the edge's 72dp to the badge, 20dp short
	// of it)
	m->overlay->UpdateDocument();
	const float dp = m->ctx->GetDensityIndependentPixelRatio(), room = float(m->ctx->GetDimensions().x) / 2 - (72 + 70 + 20) * dp;
	for (int i = 0; i < e->GetNumChildren(); i++) {
		Rml::Element *n = e->GetChild(i);
		const float w = n->IsClassSet("nm") ? n->GetOffsetWidth() : 0;
		if (w > room) n->SetProperty("font-size", std::to_string(92 * dp * std::max(0.3f, room / w)) + "px");
	}
	e->SetProperty("visibility", "visible");
	m_vs_from = SDL_GetTicks();
	m_vs_until = m_vs_from + uint64_t(seconds * 1000);
}

// The versus screen's two sides: one slant through the middle, the badge on it (drawn here, the SDL renderer's
// interface having no shapes but boxes: the names and the badge are the overlay's, over these)
void game_menu::draw_versus(int w, int h)
{
	const uint64_t now = SDL_GetTicks();
	const float dp = dp_of(w, h), mid = float(w) / 2, top_h = 560 * dp, lean = 60 * dp;
	// (in, then out: a quarter of a second each)
	const float a = std::clamp(std::min(float(now - m_vs_from), float(m_vs_until - now)) / 250.0f, 0.0f, 1.0f);
	auto col = [a](uint32_t rgb) { return SDL_FColor{ float(rgb >> 16 & 255) / 255, float(rgb >> 8 & 255) / 255, float(rgb & 255) / 255, a }; };
	auto quad = [&](SDL_FPoint p0, SDL_FPoint p1, SDL_FPoint p2, SDL_FPoint p3, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
		const SDL_Vertex v[4] = { { p0, col(c0), {} }, { p1, col(c1), {} }, { p2, col(c2), {} }, { p3, col(c3), {} } };
		const int idx[6] = { 0, 1, 2, 0, 2, 3 };
		SDL_RenderGeometry(m->ren, nullptr, v, 4, idx, 6);
	};
	SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND);
	const float W = float(w), H = float(h);
	// the left (player 1's): yellow into orange; the right (player 2's): blue, darker down; the bar under them
	quad({ 0, 0 }, { mid + lean, 0 }, { mid - lean, top_h }, { 0, top_h }, 0xf4d24c, 0xf2b23e, 0xf0922e, 0xf1a535);
	quad({ mid + lean, 0 }, { W, 0 }, { W, top_h }, { mid - lean, top_h }, 0x3445a0, 0x3b4fb0, 0x24306b, 0x1a2250);
	quad({ 0, top_h }, { W, top_h }, { W, H }, { 0, H }, 0x0b1026, 0x0b1026, 0x0b1026, 0x0b1026);
	quad({ 0, top_h }, { W, top_h }, { W, top_h + 3 * dp }, { 0, top_h + 3 * dp }, 0xffffff, 0xffffff, 0xffffff, 0xffffff);
	// the badge VS sits on: a navy disc in a white ring, drawn here with soft edges (a pixel's feather out of each
	// edge: smooth at any size, where a rounded box's corners were jagged and its halves left a seam through it)
	{
		const float cx = mid, cy = 280 * dp, r_in = 65 * dp, r_out = 70 * dp, f = 0.75f;
		constexpr int N = 160;
		const uint32_t navy = 0x0b1026, white = 0xffffff;
		// the rings, from the middle out: (radius, colour, alpha)
		struct Ring { float r; uint32_t c; float a; };
		const Ring rings[] = { { 0, navy, 1 }, { r_in - f, navy, 1 }, { r_in + f, white, 1 }, { r_out - f, white, 1 }, { r_out + f, white, 0 } };
		constexpr int R = sizeof rings / sizeof rings[0];
		std::vector<SDL_Vertex> v;
		std::vector<int> idx;
		v.reserve(1 + (R - 1) * N);
		v.push_back({ { cx, cy }, col(navy), {} });
		for (int k = 1; k < R; k++)
			for (int i = 0; i < N; i++) {
				const float t = float(i) * 6.2831853f / N;
				SDL_FColor c = col(rings[k].c);
				c.a *= rings[k].a;
				v.push_back({ { cx + rings[k].r * std::cos(t), cy + rings[k].r * std::sin(t) }, c, {} });
			}
		for (int i = 0; i < N; i++) idx.insert(idx.end(), { 0, 1 + i, 1 + (i + 1) % N });
		for (int k = 1; k + 1 < R; k++) {
			const int a0 = 1 + (k - 1) * N, b0 = 1 + k * N;
			for (int i = 0; i < N; i++) {
				const int j = (i + 1) % N;
				idx.insert(idx.end(), { a0 + i, b0 + i, b0 + j, a0 + i, b0 + j, a0 + j });
			}
		}
		SDL_RenderGeometry(m->ren, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
	}
	if (Rml::Element *e = m->overlay ? m->overlay->GetElementById("vs") : nullptr) e->SetProperty("opacity", std::to_string(a));
}

void game_menu::set_title(const std::string &text)
{
	if (m->doc)
		if (Rml::Element *e = m->doc->GetElementById("title")) e->SetInnerRML(escape(text));
}

void game_menu::set_footer(const std::string &text)
{
	if (m->doc)
		if (Rml::Element *e = m->doc->GetElementById("ver")) e->SetInnerRML(escape(text));
}

// The language on (tr::current) into the documents: their body's class (tr::css_class: for styles by language, none
// yet), the key hints, and the fallback fonts' order (the glyphs taken from them looked for again)
void game_menu::set_language()
{
	if (!m->ctx) return;
	Rml::SetFallbackFontOrder(fallback_order(tr::current()));
	for (Rml::ElementDocument *d : { m->doc, m->overlay, m->screen })
		if (d) d->SetClassNames(tr::css_class());
	if (m->doc)
		if (Rml::Element *e = m->doc->GetElementById("hint")) e->SetInnerRML(hint_rml());
	Rml::ReleaseFontResources();
	if (m_open) build();
}

// Text past its box: each line of each text in the shown documents against the boxes it's in (each block it's inside,
// up to the first one placed absolutely: what its place is measured from), and the window. (menutest: every screen
// in every language, so a translation that doesn't fit is found without looking.)
std::vector<std::string> game_menu::overflow()
{
	std::vector<std::string> out;
	if (!m->ctx) return out;
	m->ctx->Update();
	const Rml::Vector2i win = m->ctx->GetDimensions();
	std::vector<Rml::ElementDocument *> docs;
	if (m_open) docs.push_back(on_screen() && m->screen ? m->screen : m->doc);
	if (m->overlay) docs.push_back(m->overlay);
	std::function<void(Rml::Element *)> walk = [&](Rml::Element *e) {
		if (!e->IsVisible()) return;
		if (auto *t = dynamic_cast<Rml::ElementText *>(e)) {
			const Rml::Vector2f at = t->GetAbsoluteOffset(Rml::BoxArea::Border);
			for (const auto &ln : t->GetLines()) {
				if (ln.text.find_first_not_of(" \t\n") == std::string::npos) continue;
				const float x0 = at.x + ln.position.x, x1 = x0 + float(ln.width);
				const float lh = t->GetLineHeight(), y1 = at.y + ln.position.y, y0 = y1 - lh * 0.8f;
				std::string where;
				if (x0 < -1 || x1 > float(win.x) + 1) where = "the window";
				for (Rml::Element *a = t->GetParentNode(); a && where.empty(); a = a->GetParentNode()) {
					if (a->GetDisplay() == Rml::Style::Display::Inline) continue;
					const Rml::Vector2f p = a->GetAbsoluteOffset(Rml::BoxArea::Border), sz = a->GetBox().GetSize(Rml::BoxArea::Border);
					if (sz.x <= 0) continue;
					// (a box that clips what's past its bottom, but not the menu's list of rows: it scrolls)
					const bool clips = a->GetComputedValues().overflow_y() != Rml::Style::Overflow::Visible && a->GetId() != "rows";
					if (x0 < p.x - 1.5f || x1 > p.x + sz.x + 1.5f || (clips && (y0 < p.y - 1.5f || y1 > p.y + sz.y + 1.5f)))
						where = "#" + a->GetId() + "." + a->GetClassNames();
					if (a->GetPosition() == Rml::Style::Position::Absolute || a->GetPosition() == Rml::Style::Position::Fixed) break;
				}
				if (!where.empty()) {
					size_t cut = std::min<size_t>(ln.text.size(), 60);
					while (cut < ln.text.size() && (uint8_t(ln.text[cut]) & 0xC0) == 0x80) cut--;   // (a whole letter)
					out.push_back("\"" + ln.text.substr(0, cut) + "\" past " + where);
				}
			}
		}
		for (int i = 0; i < e->GetNumChildren(); i++) walk(e->GetChild(i));
	};
	for (Rml::ElementDocument *d : docs) if (d && d->IsVisible()) walk(d);
	return out;
}

std::vector<std::string> game_menu::misaligned()
{
	// The rows' labels each start where the others do (but the selected one's, and a sub row's: indented on purpose),
	// and none starts with a space (a full-width one, a no-break one: what the layout doesn't collapse).
	std::vector<std::string> out;
	if (!m->ctx || !m_open || on_screen()) return out;
	m->ctx->Update();
	Rml::Element *rows = m->doc->GetElementById("rows");
	if (!rows) return out;
	std::vector<std::pair<float, std::string>> at;
	for (int i = 0; i < rows->GetNumChildren(); i++) {
		Rml::Element *r = rows->GetChild(i);
		if (!r->IsVisible() || !r->IsClassSet("row")) continue;
		bool plain = true;
		for (const char *c : { "selected", "sub", "heading", "info", "error", "good", "big" }) plain = plain && !r->IsClassSet(c);
		Rml::Element *label = nullptr;
		for (int j = 0; j < r->GetNumChildren() && !label; j++)
			if (r->GetChild(j)->IsClassSet("label")) label = r->GetChild(j);
		std::function<Rml::ElementText *(Rml::Element *)> first = [&](Rml::Element *e) -> Rml::ElementText * {
			if (auto *t = dynamic_cast<Rml::ElementText *>(e); t && !t->GetLines().empty()) return t;
			for (int j = 0; j < e->GetNumChildren(); j++) if (Rml::ElementText *t = first(e->GetChild(j))) return t;
			return nullptr;
		};
		Rml::ElementText *t = first(label ? label : r);
		if (!t) continue;
		const auto &ln = t->GetLines().front();
		const std::string &s = ln.text;
		size_t cut = std::min<size_t>(s.size(), 40);   // (the label's first 40 bytes, back to a whole letter)
		while (cut > 0 && cut < s.size() && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
		if (!s.empty() && (s[0] == ' ' || s.rfind("\xE3\x80\x80", 0) == 0 || s.rfind("\xC2\xA0", 0) == 0))
			out.push_back("\"" + s.substr(0, cut) + "\" starts with a space");
		if (plain) at.push_back({ t->GetAbsoluteOffset(Rml::BoxArea::Border).x + ln.position.x, s.substr(0, cut) });
	}
	if (at.size() < 2) return out;
	std::vector<float> xs;
	for (const auto &a : at) xs.push_back(a.first);
	std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
	const float mid = xs[xs.size() / 2];
	for (const auto &a : at)
		if (std::fabs(a.first - mid) > 1.0f)
			out.push_back("\"" + a.second + "\" starts at " + std::to_string(int(std::lround(a.first))) + ", the rows at " + std::to_string(int(std::lround(mid))));
	return out;
}

bool game_menu::init(SDL_Window *win, SDL_Renderer *ren, const std::string &assets)
{
	m->win = win;
	m->ren = ren;
	m->sys = std::make_unique<SystemInterface_SDL>(win);
	m->gfx = std::make_unique<RenderInterface_SDL>(ren);
	Rml::SetSystemInterface(m->sys.get());
	Rml::SetRenderInterface(m->gfx.get());
	if (!Rml::Initialise()) return false;
	for (const char *f : { "BarlowCondensed-Medium.ttf", "BarlowCondensed-SemiBold.ttf", "BarlowCondensed-BoldItalic.ttf" })
		if (!Rml::LoadFontFace(assets + "/fonts/" + f)) return false;
	// the other scripts' letters, where Barlow has none: each language's own subset (assets/fonts/README.txt), fallback
	// faces looked in only for a glyph Barlow hasn't got (English is Barlow's alone), in the order set_language gives
	for (const fallback_font &f : FALLBACK_FONTS)
		if (!Rml::LoadFontFace(assets + "/fonts/" + f.file, f.family, f.italic ? Rml::Style::FontStyle::Italic : Rml::Style::FontStyle::Normal, Rml::Style::FontWeight(f.weight), true))
			SDL_Log("menu: %s isn't there: its language's letters not drawn", f.file);
	// (online play's figures: without it, in Barlow, set below)
	const bool mono = m->mono = Rml::LoadFontFace(assets + "/fonts/JetBrainsMono-SemiBold.ttf", "JetBrains Mono", Rml::Style::FontStyle::Normal);
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(win, &w, &h);
	m->ctx = Rml::CreateContext("menu", Rml::Vector2i(w, h));
	if (!m->ctx) return false;
	m->doc = m->ctx->LoadDocument(assets + "/ui/menu.rml");
	if (!m->doc) return false;
	if (Rml::Element *e = m->doc->GetElementById("title")) e->SetInnerRML(escape(tr::TITLE_PAUSED));   // (the file's is English)
	m->overlay = m->ctx->LoadDocument(assets + "/ui/overlay.rml");   // optional
	if (m->overlay) {
		m->overlay->Show();
		if (Rml::Element *e = m->overlay->GetElementById("notice")) e->SetProperty("visibility", "hidden");
		if (Rml::Element *e = m->overlay->GetElementById("vs")) e->SetProperty("visibility", "hidden");
		if (Rml::Element *e = m->overlay->GetElementById("banner")) e->SetProperty("visibility", "hidden");
		if (Rml::Element *e = m->overlay->GetElementById("net"); e && !mono) e->SetProperty("font-family", "Barlow Condensed");
	}
	m->screen = m->ctx->LoadDocument(assets + "/ui/screen.rml");   // (without it a screen page shows as rows)
	m->listener.menu = this;
	for (Rml::ElementDocument *d : { m->doc, m->screen })
		if (d) {
			d->AddEventListener(Rml::EventId::Mouseover, &m->listener);
			d->AddEventListener(Rml::EventId::Click, &m->listener);
		}
	m_pad = SDL_HasGamepad();   // (a pad plugged in: the screens start as a pad's, a Steam Deck's)
	set_language();
	return true;
}

void game_menu::open(menu_page root)
{
	// (a notice up: kept up beside the menu while it is open, its advice followed there, and the rest of its time
	// counted from when the menu closes)
	if (m_toast_until) {
		const uint64_t now = SDL_GetTicks();
		if (m_toast_until > now) { m_toast_wait = double(m_toast_until - now) / 1000.0; m_toast_until = 0; }
		else toast("");
	}
	if (!ready()) return;
	m_stack = { std::move(root) };
	m_sel_stack.clear();
	m_sel = first_selectable(m_stack.back());
	m_open = true;
	build();
	if (m->overlay) m->overlay->PullToFront();   // (its notices over the menu; the FPS hidden while it's open)
	SDL_ShowCursor();
}

void game_menu::close()
{
	if (!ready()) return;
	m_capture = nullptr;
	if (auto c = std::exchange(m_capture_cancel, nullptr)) c();
	m->doc->Hide();
	if (m->screen) m->screen->Hide();
	m_open = false;
	m_stack.clear();
	sync_text();
	SDL_HideCursor();
}

void game_menu::push(menu_page p)
{
	m_sel_stack.push_back(m_sel);
	m_stack.push_back(std::move(p));
	m_sel = first_selectable(m_stack.back());
	build();
}

void game_menu::pop()
{
	m_capture = nullptr;
	if (auto c = std::exchange(m_capture_cancel, nullptr)) c();
	if (m_stack.size() <= 1) { close(); return; }
	m_stack.pop_back();
	m_sel = m_sel_stack.back();
	m_sel_stack.pop_back();
	build();
}

void game_menu::replace(menu_page p, int sel)
{
	if (m_stack.empty()) return;
	m_stack.back() = std::move(p);
	const menu_page &q = m_stack.back();
	m_sel = sel >= 0 && sel < int(q.items.size()) && !q.items[sel].info ? sel : first_selectable(q);
	if (m_open) build();
}

void game_menu::refresh()
{
	if (m_open) build();
}

void game_menu::build()
{
	const menu_page &p = m_stack.back();
	const bool scr = p.screen && m->screen;
	if (m_open) {   // (the page's own document shown, the other hidden)
		Rml::ElementDocument *show = scr ? m->screen : m->doc, *hide = scr ? m->doc : m->screen;
		show->Show();
		if (hide) hide->Hide();
		if (m->overlay) m->overlay->PullToFront();
	}
	sync_text();
	if (scr) {
		Rml::Element *root = m->screen->GetElementById("screen");
		if (!root) return;
		root->SetInnerRML(p.screen(m_sel, m_pad));
		m_pointer_moved = false;
		// the selected item not on the screen now (the letters, gone with the pad): the page's own first one
		m->screen->UpdateDocument();
		auto shown = [&](int i) {
			Rml::Element *e = m->screen->GetElementById("r" + std::to_string(i));
			return e && e->GetBox().GetSize(Rml::BoxArea::Border).x > 0;
		};
		if (!shown(m_sel)) {
			int pick = p.start >= 0 && shown(p.start) ? p.start : -1;
			for (int i = 0; pick < 0 && i < int(p.items.size()); i++)
				if (!p.items[i].info && shown(i)) pick = i;
			if (pick >= 0 && pick != m_sel) { m_sel = pick; root->SetInnerRML(p.screen(m_sel, m_pad)); }
		}
		return;
	}
	std::string crumb;
	for (size_t i = 1; i < m_stack.size(); i++) crumb += (i > 1 ? "  /  " : "") + m_stack[i].name;
	m->doc->GetElementById("crumb")->SetInnerRML(escape(crumb));
	std::string rml;
	for (size_t i = 0; i < p.items.size(); i++) {
		const menu_item &it = p.items[i];
		rml += "<div class=\"row" + std::string(it.info ? " info" : "") + (it.heading ? " heading" : "") + (it.sub ? " sub" : "") + (it.big ? " big" : "") + (int(i) == m_sel ? " selected" : "") +
			   "\" id=\"r" + std::to_string(i) + "\"><span class=\"label\">" + escape(it.label) + "</span>";
		if (it.value) {
			rml += "<span class=\"value\">";
			const bool arrows = it.change && !(m_capture && int(i) == m_sel);   // (not while it's being typed into)
			if (arrows) rml += "<span class=\"arrow back\">&lt;&nbsp;&nbsp;</span>";
			rml += escape(it.value());
			if (arrows) rml += "<span class=\"arrow\">&nbsp;&nbsp;&gt;</span>";
			rml += "</span>";
		}
		rml += "</div>";
		if (it.below)
			if (const std::string b = it.below(); !b.empty())
				rml += "<div class=\"row info " + std::string(it.good && it.good() ? "good" : "error") + "\"><span class=\"label\">" + escape(b) + "</span></div>";
	}
	m->doc->GetElementById("rows")->SetInnerRML(rml);
	m_pointer_moved = false;
	scroll_to_selected();
	show_help();
}

// Long pages scroll inside #rows: keep the selected row in view.
void game_menu::scroll_to_selected()
{
	Rml::Element *rows = m->doc->GetElementById("rows"), *e = m->doc->GetElementById("r" + std::to_string(m_sel));
	if (!rows || !e) return;
	m->doc->UpdateDocument();                      // the rows' layout, for their positions
	const float top = e->GetOffsetTop(), h = e->GetOffsetHeight(), view = rows->GetClientHeight();
	float st = rows->GetScrollTop();
	if (top < st) st = top;
	else if (top + h > st + view) st = top + h - view;
	rows->SetScrollTop(st);
}

void game_menu::capture(std::string prompt, std::function<bool(const SDL_Event &)> on_event, std::function<void()> on_cancel)
{
	m_capture_prompt = std::move(prompt);
	m_capture = std::move(on_event);
	m_capture_cancel = std::move(on_cancel);
	show_help();
}

void game_menu::end_capture()
{
	if (!m_capture) return;
	m_capture = nullptr;
	m_capture_cancel = nullptr;
	if (m_open) build();
}

void game_menu::show_help()
{
	Rml::Element *note = m->doc->GetElementById("note");
	if (m_capture) note->SetInnerRML("<span class=\"key\">" + escape(m_capture_prompt) + "</span>");
	else {
		const menu_page &p = m_stack.back();
		const std::string &h = m_sel < int(p.items.size()) && !p.items[m_sel].help.empty() ? p.items[m_sel].help : p.note;
		note->SetInnerRML(escape(h));
	}
	// (a translation longer than its four lines: a size smaller, then another, till it fits, 14dp at the least. Not
	// the English: its layout as it was)
	note->RemoveProperty("font-size");
	note->RemoveProperty("line-height");
	if (tr::current() == tr::lang::en) return;
	const float dp = m->ctx->GetDensityIndependentPixelRatio();
	m->doc->UpdateDocument();
	for (int size = 17; size >= 14 && note->GetScrollHeight() > note->GetClientHeight() + 1; size--) {
		note->SetProperty("font-size", std::to_string(float(size) * dp) + "px");
		note->SetProperty("line-height", std::to_string(float(size) * 20.0f / 18.0f * dp) + "px");
		m->doc->UpdateDocument();
	}
}

void game_menu::select(int i)
{
	const menu_page &p = m_stack.back();
	if (i < 0 || i >= int(p.items.size()) || p.items[i].info || i == m_sel) return;
	Rml::ElementDocument *d = m->active(on_screen());
	if (auto *old = d->GetElementById("r" + std::to_string(m_sel))) old->SetClass("selected", false);
	m_sel = i;
	if (auto *e = d->GetElementById("r" + std::to_string(m_sel))) e->SetClass("selected", true);
	if (on_screen()) return;
	scroll_to_selected();
	show_help();
}

// A screen's items are laid out as it likes (tiles side by side, a grid of letters): the next one is the nearest
// whose middle is that way, nearly in line counting for more than near
void game_menu::move2d(int dx, int dy)
{
	if (!m->screen) return;
	const menu_page &p = m_stack.back();
	m->screen->UpdateDocument();
	auto middle = [&](int i, Rml::Vector2f &c) {
		Rml::Element *e = m->screen->GetElementById("r" + std::to_string(i));
		if (!e) return false;
		const Rml::Vector2f sz = e->GetBox().GetSize(Rml::BoxArea::Border);
		if (sz.x <= 0 || sz.y <= 0) return false;
		c = e->GetAbsoluteOffset(Rml::BoxArea::Border) + sz * 0.5f;
		return true;
	};
	Rml::Vector2f at;
	if (!middle(m_sel, at)) return;
	int best = -1;
	float best_score = 0;
	for (int i = 0; i < int(p.items.size()); i++) {
		Rml::Vector2f c;
		if (i == m_sel || p.items[i].info || !middle(i, c)) continue;
		const float along = (c.x - at.x) * float(dx) + (c.y - at.y) * float(dy);
		const float across = std::abs((c.x - at.x) * float(dy)) + std::abs((c.y - at.y) * float(dx));
		if (along < 4) continue;
		const float score = along + across * 3;
		if (best < 0 || score < best_score) { best = i; best_score = score; }
	}
	if (best >= 0) select(best);
}

void game_menu::sync_text()
{
	const bool want = m_open && on_screen() && m_stack.back().text && !m_pad;
	if (want == m_text_on || !m->win) return;
	m_text_on = want;
	if (want) SDL_StartTextInput(m->win); else SDL_StopTextInput(m->win);
}

void game_menu::move(int d)
{
	if (on_screen()) { move2d(0, d); return; }
	const menu_page &p = m_stack.back();
	const int n = int(p.items.size());
	int i = m_sel;
	for (int k = 0; k < n; k++) {
		i = (i + d + n) % n;
		if (!p.items[i].info) { select(i); return; }
	}
}

void game_menu::change(int d)
{
	if (m_stack.empty() || size_t(m_sel) >= m_stack.back().items.size()) return;   // (closed by a change before)
	auto f = m_stack.back().items[m_sel].change;   // a copy: it may rebuild the pages
	if (!f && on_screen()) { move2d(d, 0); return; }   // (a screen: Left / Right go across it)
	if (!f) return;
	f(d);
	refresh();
}

// A click: a step (the "<" down, anywhere else up), and from either end round to the other. Some choices stop at
// their ends (the render scale at 6x, the difficulty at Hardest, the rounds), and with a click always up, a mouse
// couldn't come back down from them: the option looked stuck (#3).
void game_menu::click_change(int d)
{
	// (a change can close the menu: the play style restarts the game, the pages gone)
	auto value = [&] {
		if (m_stack.empty() || size_t(m_sel) >= m_stack.back().items.size()) return std::string("\x01gone");
		const menu_item &it = m_stack.back().items[m_sel];
		return it.value ? it.value() : std::string();
	};
	const std::string was = value();
	change(d);
	if (value() != was) return;
	for (int n = 0; n < 64; n++) {   // (at an end: to the other one)
		const std::string at = value();
		if (at == "\x01gone") break;
		change(-d);
		if (value() == at) break;
	}
}

// A binding back to none: Delete on a keyboard, Y on a pad, with its row selected (not Esc or
// Backspace, which go back a page, nor B)
void game_menu::clear()
{
	if (m_stack.empty() || size_t(m_sel) >= m_stack.back().items.size()) return;
	auto f = m_stack.back().items[m_sel].clear;
	if (!f) return;
	f();
	refresh();
}

void game_menu::activate()
{
	const menu_item &it = m_stack.back().items[m_sel];
	if (it.activate) {
		auto f = it.activate;                     // a copy: it may push, pop or close
		f();
		if (m_open) refresh();
	} else if (it.change)
		change(+1);
}

bool game_menu::handle_event(const SDL_Event &e)
{
	if (!m_open) return false;
	// which is in use, a pad or the keyboard: a screen shows its letters for a pad, and the buttons each names
	bool pad = m_pad;
	if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(e.gaxis.value)) > 16000)) pad = true;
	else if (e.type == SDL_EVENT_KEY_DOWN) pad = false;
	m_text_fresh = false;
	if (pad != m_pad) {
		m_pad = pad;
		const bool was = m_text_on;
		if (on_screen() && !m_capture) build();
		m_text_fresh = !was && m_text_on;
	}
	if (!m_capture && on_screen() && m_stack.back().on_event) {
		auto f = m_stack.back().on_event;   // (a copy: it may push, pop or close)
		if (f(e)) {
			if (m_open) refresh();
			return true;
		}
	}
	if (m_capture) {
		switch (e.type) {
		case SDL_EVENT_KEY_DOWN:
			if (e.key.repeat) return true;
			if (e.key.scancode == SDL_SCANCODE_ESCAPE) { m_capture = nullptr; if (auto c = std::exchange(m_capture_cancel, nullptr)) c(); build(); return true; }
			[[fallthrough]];
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_TEXT_INPUT: {
			auto f = m_capture;
			if (f(e)) { m_capture = nullptr; m_capture_cancel = nullptr; build(); }
			return true;
		}
		case SDL_EVENT_KEY_UP: case SDL_EVENT_GAMEPAD_BUTTON_UP:
		case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
			return true;
		default:
			break;
		}
	}
	switch (e.type) {
	case SDL_EVENT_KEY_DOWN:
		switch (e.key.scancode) {
		case SDL_SCANCODE_UP: move(-1); break;
		case SDL_SCANCODE_DOWN: move(+1); break;
		case SDL_SCANCODE_LEFT: change(-1); break;
		case SDL_SCANCODE_RIGHT: change(+1); break;
		case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
			if (!e.key.repeat) activate();
			break;
		case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
			if (!e.key.repeat) pop();
			break;
		case SDL_SCANCODE_DELETE:
			if (!e.key.repeat) clear();
			break;
		default: break;
		}
		return true;
	case SDL_EVENT_KEY_UP:
		return true;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		switch (e.gbutton.button) {
		case SDL_GAMEPAD_BUTTON_DPAD_UP: move(-1); break;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(+1); break;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: change(-1); break;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: change(+1); break;
		case SDL_GAMEPAD_BUTTON_SOUTH: activate(); break;
		case SDL_GAMEPAD_BUTTON_EAST: pop(); break;
		case SDL_GAMEPAD_BUTTON_NORTH: clear(); break;
		case SDL_GAMEPAD_BUTTON_START: case SDL_GAMEPAD_BUTTON_BACK: case SDL_GAMEPAD_BUTTON_GUIDE: case SDL_GAMEPAD_BUTTON_TOUCHPAD: close(); break;
		default: break;
		}
		return true;
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		return true;
	case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
		// the left stick moves like the d-pad, once per push
		const int dir = e.gaxis.value < -16000 ? -1 : e.gaxis.value > 16000 ? 1 : 0;
		if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
			if (dir && dir != m_stick_y) move(dir);
			m_stick_y = dir;
		} else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
			if (dir && dir != m_stick_x) change(dir);
			m_stick_x = dir;
		}
		return true;
	}
	case SDL_EVENT_MOUSE_MOTION: {
		m_pointer_moved = true;
		const float d = SDL_GetWindowPixelDensity(m->win);
		m->ctx->ProcessMouseMove(int(e.motion.x * d), int(e.motion.y * d), 0);
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (e.button.button == SDL_BUTTON_LEFT) m->ctx->ProcessMouseButtonDown(0, 0);
		else if (e.button.button == SDL_BUTTON_RIGHT) pop();
		return true;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (e.button.button == SDL_BUTTON_LEFT) m->ctx->ProcessMouseButtonUp(0, 0);
		return true;
	case SDL_EVENT_MOUSE_WHEEL:
		if (e.wheel.y) move(e.wheel.y > 0 ? -1 : +1);
		return true;
	default:
		return false;
	}
}

void game_menu::render(bool fps)
{
	if (!m->ctx) return;   // (no menu: its files weren't found)
	if (m_toast_wait > 0 && !m_open) { m_toast_until = SDL_GetTicks() + uint64_t(m_toast_wait * 1000); m_toast_wait = 0; }
	if (m_toast_until && SDL_GetTicks() >= m_toast_until) toast("");
	if (m_vs_until && SDL_GetTicks() >= m_vs_until) {
		m_vs_until = 0;
		if (Rml::Element *e = m->overlay ? m->overlay->GetElementById("vs") : nullptr) e->SetProperty("visibility", "hidden");
	}
	if (!m_open && (m_fps.empty() || !fps) && m_net.empty() && !m_toast_until && m_notice == "\n\n" && !m_vs_until && m_banner == "\n\n") return;   // (nothing over the game)
	if (Rml::Element *e = m->overlay ? m->overlay->GetElementById("fps") : nullptr)
		e->SetProperty("visibility", fps && !m_open && !m_fps.empty() ? "visible" : "hidden");
	// the menu is laid out in window pixels, not the game's letterboxed picture
	int lw = 0, lh = 0;
	SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
	SDL_GetRenderLogicalPresentation(m->ren, &lw, &lh, &mode);
	SDL_SetRenderLogicalPresentation(m->ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
	int w = 0, h = 0;
	SDL_GetCurrentRenderOutputSize(m->ren, &w, &h);
	m->ctx->SetDimensions(Rml::Vector2i(w, h));
	m->ctx->SetDensityIndependentPixelRatio(dp_of(w, h));
	m->ctx->Update();
	if (m_vs_until) draw_versus(w, h);
	m->gfx->BeginOverlay();
	m->ctx->Render();
	m->gfx->EndFrame();
	if (m_notice != "\n\n") draw_spinner();
	SDL_SetRenderLogicalPresentation(m->ren, lw, lh, mode);
}

// A notice's spinner: a ring's three quarters going round (a turn a second), drawn here with soft edges where its
// box is laid out (a box's rounded border, a quarter of it see-through, was lumpy and stood still)
void game_menu::draw_spinner()
{
	Rml::Element *n = m->overlay ? m->overlay->GetElementById("notice") : nullptr;
	Rml::Element *e = n ? n->QuerySelector(".spin") : nullptr;
	if (!e) return;
	const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border), size = e->GetBox().GetSize(Rml::BoxArea::Border);
	const float dp = m->ctx->GetDensityIndependentPixelRatio(), cx = at.x + size.x / 2, cy = at.y + size.y / 2;
	const float r_out = std::min(size.x, size.y) / 2, r_in = r_out - 5 * dp, f = 0.75f;
	const float start = float(SDL_GetTicks() % 1000) / 1000.0f * 6.2831853f, sweep = 6.2831853f * 0.75f;
	constexpr int N = 96;
	// the arc as rows of vertices across it, from the inside out: transparent, full, full, transparent (a pixel's
	// feather either side), its ends faded over a few degrees
	const float radii[4] = { r_in - f, r_in + f, r_out - f, r_out + f }, alpha[4] = { 0, 1, 1, 0 };
	std::vector<SDL_Vertex> v;
	std::vector<int> idx;
	v.reserve((N + 1) * 4);
	for (int i = 0; i <= N; i++) {
		const float u = float(i) / N, a = start + sweep * u, end = std::min(1.0f, std::min(u, 1 - u) * 20);
		for (int k = 0; k < 4; k++)
			v.push_back({ { cx + radii[k] * std::cos(a), cy + radii[k] * std::sin(a) }, { 0xf2 / 255.0f, 0xd1 / 255.0f, 0x4b / 255.0f, alpha[k] * end }, {} });
	}
	for (int i = 0; i < N; i++)
		for (int k = 0; k < 3; k++) {
			const int a = i * 4 + k, b = a + 4;
			idx.insert(idx.end(), { a, b, b + 1, a, b + 1, a + 1 });
		}
	SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(m->ren, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}
