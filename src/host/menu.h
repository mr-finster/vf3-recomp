// The in-game menu, drawn with RmlUi over the paused game (assets/ui/menu.rml and menu.rcss).
// The host builds the pages; the menu handles the keyboard, pads and mouse.
#pragma once
#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// One line of a page.
struct menu_item
{
	std::string label;
	std::string help;                     // under the items while this one is selected
	std::function<std::string()> value;   // shown on the right (choices); none for buttons
	std::function<void(int)> change;      // Left / Right: -1 / +1 (choices)
	std::function<void()> activate;       // Enter / A / a click (buttons)
	bool info = false;                    // a line of text: not selectable
	std::function<void()> clear;          // Delete / Y: back to none (a key or button binding)
	bool heading = false;                 // a section's title (with info: not selectable)
	bool sub = false;                     // a row belonging to the one above it (indented)
	std::function<std::string()> below;   // a line under the row when not empty (a mistake to put right, a status)
	std::function<bool()> good;           // (below: true, shown as good news or how it's going, not a mistake)
	bool big = false;                     // (info: big, to be read out: a host's address)
	std::string id;                       // (a screen's button: who it is, kept selected when the page is made again (replace with
	                                      // no row given) though rows come and go before it; "" none)
};

struct menu_page
{
	std::string name;
	std::string note;                     // under the items when the selected one has no help
	std::vector<menu_item> items;
	int start = -1;                       // the row selected when it opens (or is made again with no row given)
	// A screen of its own instead of rows (the Online pages, assets/ui/screen.rml): its RML, made again at each refresh,
	// given the selected item and whether a pad is in use (else a keyboard). Each item that can be selected is an
	// element with menu_row(i, sel) as its attributes; Up / Down / Left / Right go to the nearest one that way.
	std::function<std::string(int sel, bool pad)> screen;
	std::function<bool(const SDL_Event &)> on_event;   // (a screen's) the input first: true if it used it (typing)
	bool text = false;                    // (a screen's) typed into: the keyboard's text on while it's shown
	bool own_note = false;                // (a screen's) a note (toast) shown in the page's own place for it, its element
	                                      // #pagenote (#pagehelp hidden meanwhile), not over the page; its time counted there
	bool by_height = false;               // (a screen's) made again when the window's height in dp changes (height_dp: what
	                                      // it shows depends on it, the room page's line)
};

// A screen's selectable element: its class (row, and selected) and id, with more classes
std::string menu_row(int i, int sel, const std::string &cls = "");
std::string menu_escape(const std::string &s);

class game_menu
{
public:
	game_menu();
	~game_menu();
	// Loads the fonts and the document from assets_dir; false if they aren't there.
	bool init(SDL_Window *win, SDL_Renderer *ren, const std::string &assets_dir);
	bool ready() const;
	void set_footer(const std::string &text);
	void set_language();                        // (tr::set's language into the menus: after a change, at once)
	// (tests: each line of text drawn past the box it's in, as laid out now: the menu's page or screen and the overlay)
	std::vector<std::string> overflow();
	std::vector<std::string> misaligned();   // (rows whose labels don't start where the others' do: for tests)
	std::vector<std::string> clashes();      // (lines of text drawn over each other, the page's and what's over it: for tests)
	struct box { float x0, y0, x1, y1; };
	std::vector<box> boxes(const std::string &cls);   // (each element of this class shown, in window pixels: for tests)
	float height_dp() const;                 // the window's height in dp (the menus' unit: 720 at least, more in a taller one)
	// (tests: the page of rows shown, if it doesn't fit whole: its rows past their space, where they'd scroll, or the
	// panel past the window's bottom, or the help under them wider than the panel; empty if it fits, or a screen is shown)
	std::string page_fit();
	void set_title(const std::string &text);    // (PAUSED; a match goes on under the menu: ONLINE MATCH)
	void select_row(int i) { select(i); }
	void set_test_held(int held) { m_test_held = held; }   // (tests: the left button down (1) or up (0) as SDL would say; -1 SDL's own)
	int dropped_clicks() const { return m_dropped; }       // (tests: clicks on a row gone by the button's release, not run)
	size_t row_count() const { return m_stack.empty() ? 0 : m_stack.back().items.size(); }   // (tests: the page's rows, and a row's value)
	std::string row_value(size_t i) const { return i < row_count() && m_stack.back().items[i].value ? m_stack.back().items[i].value() : std::string(); }
	void set_fps(const std::string &text);      // the small FPS at the top right; empty hides it
	// Over the game, the menu open or not: online play's connection at the bottom (empty hides it; level 0 good,
	// 1 fair, 2 poor), a notice for a few seconds near the bottom, and one that stays in the middle while it's
	// true (empty title: gone)
	void set_net(const std::string &text, int level);
	// In a match, the same place: the figures in cells of a fixed width, in a mono font (nothing moves as they
	// change); detailed, a second row of them
	struct net_cells { std::string name; int level = 0, ping = 0, delay = 0, rollback = 0; bool relay = false, interrupted = false, detailed = false;
	                   double jitter = 0, rb_avg = 0, ahead = 0, frame_ms = 0;
	                   int watchers = -1;          // (how many watch this match: -1 none can)
	                   bool watching = false;      // (a watched match's: the players and the score)
	                   std::string score;
	                   std::string next;           // (a Rooms 2 room's: who plays next, "" none; next_me: it's this player, in gold)
	                   bool next_me = false; };
	void set_net(const net_cells &c);
	void toast(const std::string &text, double seconds = 4);
	void set_notice(const std::string &title, const std::string &sub = "", const std::string &small = "");
	// A match found: the two players side by side, VS between, for a few seconds over the game's start (a line
	// under them: how it's connected, and what to do)
	struct versus { std::string left_label, left, right_label, right, line, sub; bool relay = false; std::string left_country, right_country, left_fighter = {}, right_fighter = {}; };
	void show_versus(const versus &v, double seconds = 3.5);
	bool versus_showing() const { return m_vs_until > SDL_GetTicks(); }
	bool toast_showing() const { return m_toast_until || m_toast_wait > 0; }   // (up, or waiting for the menu to close)
	bool m_vs_under = false;   // (the versus screen's names hidden under an open page)
	std::string m_vs_rml;      // (its words as last laid out)
	void hide_versus() { if (m_vs_until) m_vs_until = SDL_GetTicks(); }   // (gone at the next draw: a match over before its time)
	// At the bottom in the middle over the game, while it's set: what the players are waited for (Press Start)
	void set_banner(const std::string &title, const std::string &sub = "", const std::string &key = "");   // (key: a button, shown as one after the title)

	bool is_open() const { return m_open; }
	bool capturing() const { return bool(m_capture); }   // (a key or a name being typed: every key its)
	void open(menu_page root);            // opens at this page
	void close();
	void push(menu_page p);               // a sub-page
	void pop();                           // back a page; closes at the top
	void replace(menu_page p, int sel);   // the page on top made again (rows come and go), this row selected (-1: the one
	                                      // selected before if it has an id the new page has too, else the page's start)
	size_t depth() const { return m_stack.size(); }
	const std::string &top_name() const { static const std::string none; return m_stack.empty() ? none : m_stack.back().name; }
	SDL_Window *window() const;
	int selected() const { return m_sel; }
	bool pad_mode() const { return m_pad; }  // the last input a pad's (else the keyboard's or the mouse's)
	// (a screen's on_event) this key turned the keyboard's text on: its own letter won't come as text, take it from the key
	bool text_fresh() const { return m_text_fresh; }
	void refresh();                       // redraw the values (changed from outside)
	// Waits for a key or pad button: on_event sees each input event until it returns true. Esc
	// cancels. The prompt shows under the items meanwhile.
	void capture(std::string prompt, std::function<bool(const SDL_Event &)> on_event, std::function<void()> on_cancel = {});
	void end_capture();                   // a capture over from outside (done, or timed out): not its cancel

	// Takes the event if the menu is open; true when it was used.
	bool handle_event(const SDL_Event &e);
	// Draws the menu over what has been rendered so far (in window pixels).
	void render(bool fps = false);   // the menu if open; the FPS (fps: shown, and the menu closed)
	// The renderer lost its textures (a device reset): RmlUi's let go, made again as they're drawn.
	void textures_lost();

	struct impl;
private:
	std::unique_ptr<impl> m;
	bool m_open = false;
	std::string m_banner = "\n\n";
	std::string m_fps, m_net = "-", m_notice = "\n\n";   // (m_net: the connection's RML and class; m_notice: its title and lines, "\n\n" none)
	void show_net(const std::string &rml, const char *cls);
	uint64_t m_toast_until = 0;   // (SDL_GetTicks)
	double m_toast_wait = 0;      // (a notice's seconds, counted from the first picture drawn with the menu closed)
	uint64_t m_vs_from = 0, m_vs_until = 0;   // the versus screen's (SDL_GetTicks)
	void draw_versus(int w, int h);       // its two sides, under its names
	void draw_spinner();                  // a notice's spinner, over its card
	std::string m_toast_text;             // (the note up, or waiting)
	bool page_owns_note() const { return m_open && on_screen() && m_stack.back().own_note; }
	void place_note(bool force = false);  // (the note: over the game, in the page's own place, or held back)
	std::string m_note_was;               // (place_note's last: not set again each picture)
	long m_built_h = 0;                   // (the window's height in dp the page was made for: menu_page::by_height)
	std::vector<menu_page> m_stack;
	std::vector<int> m_sel_stack;
	int m_sel = 0;
	int m_stick_x = 0, m_stick_y = 0;    // the left stick's last direction, for edges
	bool m_pad = false;                  // (pad_mode)
	bool m_text_on = false;              // the keyboard's text turned on for a screen (and so to be turned off)
	bool m_text_fresh = false;           // (text_fresh)
	bool m_pointer_moved = false;        // (since the rows were last made: only then does hovering select)
	bool m_mouse_down = false, m_build_pending = false, m_mouse_seen = false;   // (the left button down: the page made again at its release)
	int m_mouse_x = 0, m_mouse_y = 0;    // (the pointer, in the context's pixels)
	int m_test_held = -1;                // (set_test_held)
	std::vector<std::pair<std::string, std::string>> m_built;   // (the page's rows as last drawn, their ids and labels: row_now)
	size_t m_built_depth = 0;
	std::string m_built_name;
	int m_dropped = 0;                   // (clicks on a row gone since it was drawn: dropped_clicks)
	int row_now(int i) const;
	void build_now();
	std::function<bool(const SDL_Event &)> m_capture;
	std::string m_capture_prompt;
	std::function<void()> m_capture_cancel;   // (Esc while capturing)

	void build();                         // the current page into the document
	void show_help();                     // the selected item's help under the items
	void scroll_to_selected();
	void move(int d);
	void move2d(int dx, int dy);          // (a screen's) to the nearest item that way
	void sync_text();                     // the keyboard's text on for a screen that's typed into, else off
	bool on_screen() const { return !m_stack.empty() && bool(m_stack.back().screen); }
	void select(int i);
	void change(int d);
	void click_change(int d);
	void activate();
	void clear();
	friend struct menu_listener;
};
