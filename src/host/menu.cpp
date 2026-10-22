#include "menu.h"
#include <RmlUi/Core.h>
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

void game_menu::set_net(const std::string &text, int level, const std::string &detail)
{
	if (!m->overlay || (text == m_net && level == m_net_level && detail == m_net_detail)) return;
	m_net = text;
	m_net_level = level;
	m_net_detail = detail;
	if (Rml::Element *e = m->overlay->GetElementById("net")) {
		e->SetInnerRML(text.empty() ? "" : "<span class=\"dot\"></span>" + escape(text) + (detail.empty() ? "" : "<div class=\"detail\">" + escape(detail) + "</div>"));
		e->SetClassNames(level <= 0 ? "good" : level == 1 ? "fair" : "poor");
	}
}

void game_menu::toast(const std::string &text, double seconds)
{
	if (!m->overlay) return;
	if (Rml::Element *e = m->overlay->GetElementById("toast")) e->SetInnerRML(text.empty() ? "" : "<span>" + escape(text) + "</span>");
	m_toast_until = text.empty() ? 0 : SDL_GetTicks() + uint64_t(seconds * 1000);
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

void game_menu::show_versus(const versus &v, double seconds)
{
	if (!m->overlay) return;
	Rml::Element *e = m->overlay->GetElementById("vs");
	if (!e) return;
	e->SetInnerRML("<div class=\"pl left\">" + escape(v.left_label) + "</div><div class=\"nm left\">" + escape(v.left) + "</div>"
	               "<div class=\"pl right\">" + escape(v.right_label) + "</div><div class=\"nm right\">" + escape(v.right) + "</div>"
	               "<div class=\"badge\"><div>VS</div></div>"
	               "<div class=\"bar\"><div class=\"line" + std::string(v.relay ? " relay" : "") + "\"><span class=\"dot\"></span>" + escape(v.line) + "</div>"
	               "<div class=\"sub\">" + escape(v.sub) + "</div></div>");
	e->SetProperty("visibility", "visible");
	m_vs_from = SDL_GetTicks();
	m_vs_until = m_vs_from + uint64_t(seconds * 1000);
}

// The versus screen's two sides: one slant through the middle, the badge on it (drawn here, the SDL renderer's
// interface having no shapes but boxes: the names and the badge are the overlay's, over these)
void game_menu::draw_versus(int w, int h)
{
	const uint64_t now = SDL_GetTicks();
	const float dp = float(h) / 720.0f, mid = float(w) / 2, top_h = 560 * dp, lean = 60 * dp;
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
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(win, &w, &h);
	m->ctx = Rml::CreateContext("menu", Rml::Vector2i(w, h));
	if (!m->ctx) return false;
	m->doc = m->ctx->LoadDocument(assets + "/ui/menu.rml");
	if (!m->doc) return false;
	m->overlay = m->ctx->LoadDocument(assets + "/ui/overlay.rml");   // optional
	if (m->overlay) {
		m->overlay->Show();
		if (Rml::Element *e = m->overlay->GetElementById("notice")) e->SetProperty("visibility", "hidden");
		if (Rml::Element *e = m->overlay->GetElementById("vs")) e->SetProperty("visibility", "hidden");
	}
	m->screen = m->ctx->LoadDocument(assets + "/ui/screen.rml");   // (without it a screen page shows as rows)
	m->listener.menu = this;
	for (Rml::ElementDocument *d : { m->doc, m->screen })
		if (d) {
			d->AddEventListener(Rml::EventId::Mouseover, &m->listener);
			d->AddEventListener(Rml::EventId::Click, &m->listener);
		}
	m_pad = SDL_HasGamepad();   // (a pad plugged in: the screens start as a pad's, a Steam Deck's)
	return true;
}

void game_menu::open(menu_page root)
{
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

void game_menu::show_help()
{
	if (m_capture) {
		m->doc->GetElementById("note")->SetInnerRML("<span class=\"key\">" + escape(m_capture_prompt) + "</span>");
		return;
	}
	const menu_page &p = m_stack.back();
	const std::string &h = m_sel < int(p.items.size()) && !p.items[m_sel].help.empty() ? p.items[m_sel].help : p.note;
	m->doc->GetElementById("note")->SetInnerRML(escape(h));
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
	if (m_toast_until && SDL_GetTicks() >= m_toast_until) toast("");
	if (m_vs_until && SDL_GetTicks() >= m_vs_until) {
		m_vs_until = 0;
		if (Rml::Element *e = m->overlay ? m->overlay->GetElementById("vs") : nullptr) e->SetProperty("visibility", "hidden");
	}
	if (!m_open && (m_fps.empty() || !fps) && m_net.empty() && !m_toast_until && m_notice == "\n\n" && !m_vs_until) return;   // (nothing over the game)
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
	m->ctx->SetDensityIndependentPixelRatio(float(h) / 720.0f);
	m->ctx->Update();
	if (m_vs_until) draw_versus(w, h);
	m->gfx->BeginOverlay();
	m->ctx->Render();
	m->gfx->EndFrame();
	SDL_SetRenderLogicalPresentation(m->ren, lw, lh, mode);
}
