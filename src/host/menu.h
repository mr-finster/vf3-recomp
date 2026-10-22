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
};

struct menu_page
{
	std::string name;
	std::string note;                     // under the items when the selected one has no help
	std::vector<menu_item> items;
};

class game_menu
{
public:
	game_menu();
	~game_menu();
	// Loads the fonts and the document from assets_dir; false if they aren't there.
	bool init(SDL_Window *win, SDL_Renderer *ren, const std::string &assets_dir);
	bool ready() const;
	void set_footer(const std::string &text);
	void set_title(const std::string &text);    // (PAUSED; a match goes on under the menu: ONLINE MATCH)
	void select_row(int i) { select(i); }
	void set_fps(const std::string &text);      // the small FPS at the top right; empty hides it
	// Over the game, the menu open or not: online play's connection at the top (empty hides it; level 0 good,
	// 1 fair, 2 poor), a notice for a few seconds near the bottom, and one that stays in the middle while it's
	// true (empty title: gone)
	void set_net(const std::string &text, int level, const std::string &detail = "");   // (detail: a smaller line under it)
	void toast(const std::string &text, double seconds = 4);
	void set_notice(const std::string &title, const std::string &sub = "");

	bool is_open() const { return m_open; }
	bool capturing() const { return bool(m_capture); }   // (a key or a name being typed: every key its)
	void open(menu_page root);            // opens at this page
	void close();
	void push(menu_page p);               // a sub-page
	void pop();                           // back a page; closes at the top
	void replace(menu_page p, int sel);   // the page on top made again (rows come and go), this row selected
	size_t depth() const { return m_stack.size(); }
	const std::string &top_name() const { static const std::string none; return m_stack.empty() ? none : m_stack.back().name; }
	SDL_Window *window() const;
	int selected() const { return m_sel; }
	void refresh();                       // redraw the values (changed from outside)
	// Waits for a key or pad button: on_event sees each input event until it returns true. Esc
	// cancels. The prompt shows under the items meanwhile.
	void capture(std::string prompt, std::function<bool(const SDL_Event &)> on_event, std::function<void()> on_cancel = {});

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
	std::string m_fps, m_net, m_net_detail, m_notice = "\n";   // (m_notice: its title and line, "\n" none)
	int m_net_level = -1;
	uint64_t m_toast_until = 0;   // (SDL_GetTicks)
	std::vector<menu_page> m_stack;
	std::vector<int> m_sel_stack;
	int m_sel = 0;
	int m_stick_x = 0, m_stick_y = 0;    // the left stick's last direction, for edges
	bool m_pointer_moved = false;        // (since the rows were last made: only then does hovering select)
	std::function<bool(const SDL_Event &)> m_capture;
	std::string m_capture_prompt;
	std::function<void()> m_capture_cancel;   // (Esc while capturing)

	void build();                         // the current page into the document
	void show_help();                     // the selected item's help under the items
	void scroll_to_selected();
	void move(int d);
	void select(int i);
	void change(int d);
	void click_change(int d);
	void activate();
	void clear();
	friend struct menu_listener;
};
