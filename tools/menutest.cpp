// menutest: the menu's Online pages and the notices over the game, drawn as the game draws them (RmlUi on SDL's
// renderer) in a window nobody sees, and written as pictures: for looking at them without the ROM or a screen.
//   SDL_VIDEO_DRIVER=offscreen menutest <out folder> [width height]
// Writes the Online screens (home, name and code entry on a keyboard and a pad, hosting and joining by address, and
// with VF_LOBBY set a room's: tools/lobby/test/serve.mjs with WATCHERS_PER_IP=100000, as staging has it, since its 400
// watchers all come from this machine), the match over, the menu in a match, the notices, and the versus screen; and checks
// what they do (where the keys go, what's typed, what each button starts). Exit 1 if any check fails.
#include "../src/host/menu.h"
#include "../src/host/online.h"
#include "../src/host/links.h"
#include "../src/host/text.h"
#include "../src/host/png.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // (a Mac's: SIGPIPE ignored instead, FakeLobby::open)
#endif
#endif

namespace {

SDL_Renderer *ren = nullptr;
game_menu *menu = nullptr;
std::string out;
SDL_Texture *backdrop = nullptr;   // MENUTEST_BACKDROP: a picture of the game instead of the stand-in
std::vector<std::string> overflows;   // (each picture's text past its box: game_menu::overflow)
std::vector<uint32_t> last_px;   // (the last picture written, its pixels)
std::vector<std::string> misaligned;   // (a page's rows whose labels don't start where the others' do)
std::vector<std::string> unfit;   // (a page of rows that doesn't fit whole: game_menu::page_fit)

// a stand-in for the game's picture: a dark stage, two energy bars and a timer, so the notices are seen over something
void draw_game(int w, int h)
{
	if (backdrop) { SDL_RenderTexture(ren, backdrop, nullptr, nullptr); return; }
	for (int y = 0; y < h; y += 4) {
		const float t = float(y) / float(h);
		SDL_SetRenderDrawColor(ren, Uint8(30 + 40 * t), Uint8(40 + 30 * t), Uint8(70 - 20 * t), 255);
		SDL_FRect r{ 0, float(y), float(w), 4 };
		SDL_RenderFillRect(ren, &r);
	}
	const float u = float(h) / 720.0f;
	SDL_SetRenderDrawColor(ren, 255, 210, 63, 255);
	SDL_FRect a{ float(w) * 0.08f, 34 * u, float(w) * 0.36f, 18 * u }, b{ float(w) * 0.56f, 34 * u, float(w) * 0.36f, 18 * u };
	SDL_RenderFillRect(ren, &a);
	SDL_RenderFillRect(ren, &b);
	SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
	SDL_FRect f1{ float(w) * 0.3f, float(h) * 0.45f, 60 * u, 220 * u }, f2{ float(w) * 0.62f, float(h) * 0.45f, 60 * u, 220 * u };
	SDL_RenderFillRect(ren, &f1);
	SDL_RenderFillRect(ren, &f2);
}

void shot(const char *name, int w, int h)
{
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	draw_game(w, h);
	menu->render(false);
	if (!*name) return;   // (a frame only)
	for (const std::string &o : menu->overflow()) overflows.push_back(std::string(name) + ": " + o);
	if (const std::string f = menu->page_fit(); !f.empty()) unfit.push_back(std::string(name) + ": " + f);
	SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr);
	SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : nullptr;
	if (s) SDL_DestroySurface(s);
	if (!c) { fprintf(stderr, "menutest: couldn't read the picture back\n"); return; }
	std::vector<uint32_t> px(size_t(c->w) * size_t(c->h));
	for (int y = 0; y < c->h; y++) memcpy(&px[size_t(y) * size_t(c->w)], static_cast<uint8_t *>(c->pixels) + size_t(y) * size_t(c->pitch), size_t(c->w) * 4);
	const std::string path = out + "/" + name;
	write_png(path.c_str(), px.data(), c->w, c->h);
	last_px = px;
	SDL_DestroySurface(c);
	printf("%s\n", path.c_str());
}

// After a picture: the rows let finish sliding, then checked for starting in line (the picture's as it was).
void settle_check(const char *name, int w, int h)
{
	for (int i = 0; i < 8; i++) { shot("", w, h); SDL_Delay(40); }
	for (const std::string &o : menu->misaligned()) misaligned.push_back(std::string(name) + ": " + o);
}

#ifndef _WIN32
// A lobby of the test's own on this machine (plain HTTP, one request a connection): each request's first line kept, and
// answered as answer() says (a status and a JSON body). For what the real one does only now and then (a watched match
// replaced by the next before it was over, a start that never comes), each time.
struct FakeLobby
{
	int fd = -1, port = 0;
	std::mutex lock;
	std::vector<std::string> asked;
	std::function<std::pair<int, std::string>(const std::string &line)> answer;
	std::atomic<bool> stop{ false };
	std::thread thread;
	bool open()
	{
		signal(SIGPIPE, SIG_IGN);   // (a watcher that let go before its answer: not this test's end)
		fd = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t n = sizeof a;
		if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(fd, 16) != 0 || getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n) != 0) return false;
		port = ntohs(a.sin_port);
		thread = std::thread([this] {
			while (!stop) {
				pollfd p{ fd, POLLIN, 0 };
				if (poll(&p, 1, 50) <= 0) continue;
				const int c = accept(fd, nullptr, nullptr);
				if (c < 0) continue;
				std::string req;
				char buf[4096];
				size_t want = std::string::npos;
				for (int i = 0; i < 100 && (want == std::string::npos || req.size() < want); i++) {
					pollfd q{ c, POLLIN, 0 };
					if (poll(&q, 1, 20) <= 0) continue;
					const ssize_t got = recv(c, buf, sizeof buf, 0);
					if (got <= 0) break;
					req.append(buf, size_t(got));
					if (const size_t end = req.find("\r\n\r\n"); end != std::string::npos && want == std::string::npos) {
						size_t len = 0;
						for (const char *h : { "Content-Length:", "content-length:" })
							if (const size_t at = req.find(h); at != std::string::npos && at < end) len = size_t(atol(req.c_str() + at + strlen(h)));
						want = end + 4 + len;
					}
				}
				const std::string line = req.substr(0, req.find("\r\n"));
				{ std::lock_guard<std::mutex> l(lock); asked.push_back(line); }
				const auto [status, body] = answer ? answer(line) : std::pair<int, std::string>{ 404, "{\"error\":\"bad\"}" };
				const std::string out = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
				                        "\r\nConnection: close\r\n\r\n" + body;
				send(c, out.data(), out.size(), MSG_NOSIGNAL);
				close(c);
			}
		});
		return true;
	}
	bool was_asked(const std::string &part)
	{
		std::lock_guard<std::mutex> l(lock);
		for (const std::string &a : asked) if (a.find(part) != std::string::npos) return true;
		return false;
	}
	~FakeLobby()
	{
		stop = true;
		if (thread.joinable()) thread.join();
		if (fd >= 0) close(fd);
	}
};
#endif

void key(SDL_Scancode sc)
{
	SDL_Event e{};
	e.type = SDL_EVENT_KEY_DOWN;
	e.key.scancode = sc;
	e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);   // (as a real key press has both)
	e.key.down = true;
	menu->handle_event(e);
}

void pad(SDL_GamepadButton b)
{
	SDL_Event e{};
	e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
	e.gbutton.button = Uint8(b);
	e.gbutton.down = true;
	menu->handle_event(e);
}

void mouse(float x, float y)
{
	SDL_Event e{};
	e.type = SDL_EVENT_MOUSE_MOTION;
	e.motion.x = x;
	e.motion.y = y;
	menu->handle_event(e);
}

// the left button pressed or let go at (x, y), as SDL gives it (the pointer moved there first, as a real press)
void button(bool down, float x, float y)
{
	mouse(x, y);
	SDL_Event e{};
	e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	e.button.button = SDL_BUTTON_LEFT;
	e.button.down = down;
	e.button.x = x;
	e.button.y = y;
	menu->handle_event(e);
}

void text(const char *t)
{
	SDL_Event e{};
	e.type = SDL_EVENT_TEXT_INPUT;
	e.text.text = t;
	menu->handle_event(e);
}

// The settings pages (main.cpp's: they need the game to be made, so here their rows as they are there, by the same
// strings, each with its longest value): each page drawn, each row selected (its help under the rows), and every
// line checked against its box. One picture a page: pages_<name>.png, its longest help showing.
struct row { std::string label, help, value; };
void page_shots(game_menu &gm, int w, int h, const char *file, tr::text title, const std::vector<row> &rows, const std::string &note = "")
{
	const bool top = title.id == tr::M_RESUME.id;   // (the main page: the menu's first, no name over its rows)
	menu_page root{ "", "", {} };
	root.items.push_back({ tr::M_RESUME, "", {}, {}, {} });
	if (!top) gm.open(root);
	menu_page p{ top ? std::string() : std::string(title), note, {} };
	size_t longest = 0;
	for (size_t i = 0; i < rows.size(); i++) {
		const row &r = rows[i];
		menu_item it{ r.label, r.help, {}, {}, {} };
		if (!r.value.empty()) { const std::string v = r.value; it.value = [v] { return v; }; it.change = [](int) {}; }
		p.items.push_back(it);
		if (r.help.size() > rows[longest].help.size()) longest = i;
	}
	if (!top) p.items.push_back({ tr::BACK, tr::BACK_HELP, {}, {}, {} });
	if (top) gm.open(p); else gm.push(p);
	for (size_t i = 0; i < rows.size(); i++) {
		gm.select_row(int(i));
		shot("", w, h);
		for (const std::string &o : gm.overflow()) overflows.push_back(std::string(file) + " row " + std::to_string(i) + ": " + o);
		// (every row's help within the panel, not only the pictured one's)
		if (const std::string f = gm.page_fit(); !f.empty()) unfit.push_back(std::string(file) + " row " + std::to_string(i) + ": " + f);
	}
	gm.select_row(int(longest));
	for (int i = 0; i < 8; i++) { shot("", w, h); SDL_Delay(40); }   // (the rows' slide, 0.12 s, over before the picture)
	shot(file, w, h);
	for (const std::string &o : gm.misaligned()) misaligned.push_back(std::string(file) + ": " + o);
	gm.close();
}

void pages(game_menu &gm, int w, int h)
{
	using namespace tr;
	auto F = [](tr::text t, tr::with v) { return tr::fill(t, v); };
	const std::string on = ON, off = OFF;
	page_shots(gm, w, h, "pages_main.png", M_RESUME, {   // (the main page: its rows' helps)
		{ M_RESUME, M_RESUME_HELP, "" }, { P_DISPLAY, M_DISPLAY_HELP, "" }, { P_SOUND, M_SOUND_HELP, "" }, { P_GAME, M_GAME_HELP, "" },
		{ M_ARCADE, M_ARCADE_HELP, "" }, { P_ONLINE, F(M_ONLINE_NOT_READY, { { "game", "Virtua Fighter 3 Team Battle" } }), "" },
		{ P_CONTROLS, M_CONTROLS_MATCH_HELP, "" }, { P_CHEATS, M_CHEATS_ON_HELP, "" },
		{ F(UPD_OUT, { { "v", "0.1.23-alpha" } }), UPD_OUT_HELP, UPD_GET },   // (a newer release out: update_check.h)
		{ M_QUIT, M_QUIT_HELP, "" } });
	// Display: its three pages' rows, each with what it's set to, and the free camera's; then each page (as main.cpp's
	// display_page, picture_page, quality_page and screen_page)
	auto listed = [&](std::initializer_list<std::string> parts) {
		std::string out;
		for (const std::string &x : parts) out = out.empty() ? x : F(LIST_PAIR, { { "a", out }, { "b", x } });
		return out;
	};
	page_shots(gm, w, h, "pages_display.png", P_DISPLAY, {
		{ D_PICTURE, listed({ D_SHAPE, D_WINDOW_SIZE, D_FULLSCREEN, D_FS_MODE, D_HUD, D_SHOW_FPS }), listed({ D_SUM_FIT, F(D_SUM_WINDOW_N, { { "n", "4" } }) }) },
		{ D_QUALITY, listed({ D_SCALE, D_AA, D_RENDERER, D_DRIVER, D_BG_FILTER, D_TEXTURES, D_SMOOTH_MOTION, D_VSYNC, D_FRAME_LIMIT }),
		  listed({ F(N_X, { { "n", "6" } }), F(D_SUM_AA, { { "n", F(N_X, { { "n", "3" } }) } }), R3D_GPU }) },
		{ D_SCREEN, listed({ D_SCREEN_FILTER, D_CRT_COLOURS, D_BRIGHTNESS, D_GAMMA }), listed({ D_SCANLINES, D_CRT_COLOURS }) },
		{ D_FREECAM, D_FREECAM_HELP, D_FREECAM_NOT_ONLINE } });
	page_shots(gm, w, h, "pages_display_more.png", P_DISPLAY, {   // (the other values of its rows)
		{ D_PICTURE, "", listed({ "21:9", D_FULLSCREEN }) }, { D_PICTURE, "", listed({ "4:3", D_SUM_WINDOW }) },
		{ D_QUALITY, "", listed({ F(N_X, { { "n", "1" } }), R3D_CPU }) }, { D_SCREEN, "", OFF }, { D_SCREEN, "", listed({ D_CRT, D_CRT_COLOURS }) },
		{ D_FREECAM, D_FREECAM_HELP, on } });
	page_shots(gm, w, h, "pages_picture.png", D_PICTURE, {
		{ D_SHAPE, D_SHAPE_HELP, D_SHAPE_FIT },
		{ D_WINDOW_SIZE, D_WINDOW_SIZE_HELP, F(D_WINDOW_N_IN_WINDOW, { { "n", "3" }, { "w", "1536" }, { "h", "1152" } }) },
		{ D_FULLSCREEN, D_FULLSCREEN_HELP, on },
		{ D_FS_MODE, D_FS_MODE_HELP, D_BORDERLESS },
		{ D_HUD, D_HUD_HELP, on },
		{ D_SHOW_FPS, D_SHOW_FPS_HELP, off },
		{ D_WINDOW_SIZE, D_WINDOW_SIZE_HELP, D_FREE }, { D_FS_MODE, D_FS_MODE_HELP, D_STANDARD } });
	page_shots(gm, w, h, "pages_quality.png", D_QUALITY, {
		{ D_SCALE, D_SCALE_HELP, F(D_SCALE_LOWERED, { { "n", "6" }, { "now", "2" } }) },
		{ D_AA, D_AA_HELP, F(D_AA_NOW_CPU, { { "n", "3" }, { "now", OFF_LOWER } }) },
		{ D_RENDERER, D_RENDERER_HELP, D_R3D_CRASHED },
		{ D_DRIVER, D_DRIVER_HELP, D_DRIVER_AUTO },
		{ D_BG_FILTER, D_BG_FILTER_HELP, D_SMOOTH },
		{ D_TEXTURES, D_TEXTURES_HELP, D_ARCADE },
		{ D_SMOOTH_MOTION, D_SMOOTH_MOTION_HELP, D_SMOOTH_BUSY },
		{ D_VSYNC, D_VSYNC_HELP, on },
		{ D_FRAME_LIMIT, D_FRAME_LIMIT_HELP, F(D_FRAME_LIMIT_SCREEN, { { "n", "144" } }) } });
	page_shots(gm, w, h, "pages_quality_more.png", D_QUALITY, {   // (the other values of its rows)
		{ D_RENDERER, D_RENDERER_HELP, F(D_R3D_NEXT_START, { { "what", AUTO } }) }, { D_RENDERER, D_RENDERER_HELP, F(AUTO_IS, { { "what", R3D_GPU } }) },
		{ D_RENDERER, D_RENDERER_HELP, D_R3D_FAILED }, { D_AA, D_AA_HELP, F(D_AA_NOW_BUSY, { { "n", "3" }, { "now", F(N_X, { { "n", "2" } }) } }) },
		{ D_AA, D_AA_HELP, F(D_AA_NOW_SCALE, { { "n", "3" }, { "now", F(N_X, { { "n", "2" } }) } }) }, { D_TEXTURES, D_TEXTURES_HELP, D_SHARP } });
	page_shots(gm, w, h, "pages_screen.png", D_SCREEN, {
		{ D_SCREEN_FILTER, D_SCREEN_FILTER_HELP, D_SCANLINES },
		{ D_CRT_COLOURS, D_CRT_COLOURS_HELP, CRT_PAL },
		{ D_BRIGHTNESS, D_BRIGHTNESS_HELP, F(N_PERCENT, { { "n", "100" } }) },
		{ D_GAMMA, D_GAMMA_HELP, F(N_PERCENT, { { "n", "100" } }) },
		{ D_CRT_COLOURS, D_CRT_COLOURS_HELP, CRT_SEGA }, { D_CRT_COLOURS, D_CRT_COLOURS_HELP, CRT_NTSC_JP }, { D_SCREEN_FILTER, D_SCREEN_FILTER_HELP, D_CRT } });
	page_shots(gm, w, h, "pages_sound.png", P_SOUND, {
		{ S_VOLUME, S_VOLUME_HELP, F(N_PERCENT, { { "n", "100" } }) }, { S_MUSIC, S_MUSIC_HELP, F(N_PERCENT, { { "n", "80" } }) },
		{ S_EFFECTS, S_EFFECTS_HELP, F(N_PERCENT, { { "n", "100" } }) } });
	page_shots(gm, w, h, "pages_game.png", P_GAME, {
		{ G_LANGUAGE, G_LANGUAGE_HELP, F(LANG_AUTO_IS, { { "lang", info(tr::current() == lang::en || tr::current() == lang::pseudo ? lang::pt_br : tr::current()).name } }) },
		{ G_PLAY_STYLE, G_PLAY_STYLE_HELP, G_ARCADE }, { G_RESET, G_RESET_HELP, "" },
		{ G_VERSION, G_VERSION_HELP, "Virtua Fighter 3" }, { G_ADD_TB, G_ADD_TB_HELP, G_CHOOSING },
		{ G_TB_MODE, G_TB_MODE_HELP, G_3V3 }, { G_LOG, G_LOG_HELP, on }, { G_UPDATES, G_UPDATES_HELP, on } });
	{   // (Game > Language: each of its choices, its name in its own letters)
		std::vector<row> v;
		for (int i = 0; i < CHOICES; i++) v.push_back({ G_LANGUAGE, G_LANGUAGE_HELP, choice_name(tr::choice{ i }) });
		// (in two pictures: a page is 14 rows at most)
		const size_t half = v.size() / 2;
		page_shots(gm, w, h, "pages_languages.png", P_GAME, std::vector<row>(v.begin(), v.begin() + std::ptrdiff_t(half)));
		page_shots(gm, w, h, "pages_languages2.png", P_GAME, std::vector<row>(v.begin() + std::ptrdiff_t(half), v.end()));
	}
	{   // (Game > Version: the question asked first)
		menu_page root{ "", "", {} };
		root.items.push_back({ M_RESUME, "", {}, {}, {} });
		gm.open(root);
		menu_page q{ fill(G_SWITCH_Q, { { "game", "Virtua Fighter 3 Team Battle" } }), "", {} };
		q.items.push_back({ G_SWITCH_RESTART, fill(G_SWITCH_RESTART_HELP, { { "game", "Virtua Fighter 3 Team Battle" } }), {}, {}, {} });
		q.items.push_back({ CANCEL, fill(G_STAY_IN, { { "game", "Virtua Fighter 3" } }), {}, {}, {} });
		gm.push(q);
		shot("pages_switch.png", w, h);
		gm.close();
	}
	page_shots(gm, w, h, "pages_arcade.png", P_ARCADE, {
		{ A_DIFFICULTY, A_DIFFICULTY_HELP, A_HARDEST }, { A_ROUNDS_CPU, A_ROUNDS_CPU_HELP, "3" }, { A_ROUNDS_VS, A_ROUNDS_VS_HELP, "3" },
		{ A_ENERGY_CPU, A_ENERGY_CPU_HELP, "200" }, { A_ENERGY_VS, A_ENERGY_VS_HELP, "200" }, { A_STAGE_SELECT, A_STAGE_SELECT_HELP_TB, A_CHALLENGER },
		{ A_CPU_ORDER, A_CPU_ORDER_HELP, A_THE_GAMES }, { A_DEMO_SOUND, A_DEMO_SOUND_HELP, on }, { A_COUNTRY, A_COUNTRY_HELP, A_EXPORT },
		{ A_DEBUG, A_DEBUG_HELP, off }, { A_SERVICE, A_SERVICE_HELP, "" } });
	page_shots(gm, w, h, "pages_arcade_more.png", P_ARCADE, {
		{ A_DIFFICULTY, A_DIFFICULTY_HELP, A_EASY }, { A_DIFFICULTY, A_DIFFICULTY_HELP, A_NORMAL }, { A_DIFFICULTY, A_DIFFICULTY_HELP, A_HARD },
		{ A_STAGE_SELECT, A_STAGE_SELECT_HELP, A_VARIABLE }, { A_STAGE_SELECT, A_STAGE_SELECT_HELP, A_FULL }, { A_CPU_ORDER, A_CPU_ORDER_HELP, A_RANDOM },
		{ A_COUNTRY, A_COUNTRY_HELP, A_JAPAN }, { A_COUNTRY, A_COUNTRY_HELP, A_USA }, { A_COUNTRY, A_COUNTRY_HELP, A_ASIA } });
	page_shots(gm, w, h, "pages_cheats.png", P_CHEATS, { { C_DURAL, C_DURAL_HELP, on }, { C_DURAL_STAGE, C_DURAL_STAGE_HELP, C_DURAL_STAGE_START } });
	page_shots(gm, w, h, "pages_cheats_ways.png", P_CHEATS, {   // (Dural's stage: each way's value and help)
		{ C_DURAL_STAGE, C_DURAL_STAGE_OFF_HELP, off }, { C_DURAL_STAGE, F(C_DURAL_STAGE_BUTTON_HELP, { { "button", std::string(CTL_GUARD) } }), CTL_GUARD },
		{ C_DURAL_STAGE, F(C_DURAL_STAGE_BUTTON_HELP, { { "button", std::string(CTL_ESCAPE) } }), CTL_ESCAPE },
		{ C_DURAL_STAGE, C_DURAL_STAGE_GP_HELP, C_DURAL_STAGE_START_GP } });
	page_shots(gm, w, h, "pages_controls.png", P_CONTROLS, {
		{ P_PLAYER_1, CTL_P1_HELP, F(CTL_ANY_PAD, { { "pad", "Xbox Series X Controller" } }) }, { P_PLAYER_2, CTL_P2_HELP, CTL_ANY_PAD_NONE },
		{ CTL_SWAP, CTL_SWAP_HELP, "" } });
	page_shots(gm, w, h, "pages_player.png", P_PLAYER_1, {
		{ CTL_PAD, CTL_PAD_HELP, CTL_CHOSEN_PAD_GONE }, { CTL_KEYBOARD, CTL_KEYBOARD_HELP, "" }, { CTL_KEYBOARD2, CTL_KEYBOARD2_HELP, "" }, { CTL_PAD_BUTTONS, CTL_PAD_BUTTONS_HELP, "" },
		{ CTL_PAD_BUTTONS2, CTL_PAD_BUTTONS2_HELP, "" },
		{ CTL_SETUP, CTL_SETUP_HELP, "" }, { CTL_RESET, CTL_RESET_HELP, "" } });
	{
		const tr::text names[] = { CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT, CTL_GUARD, CTL_PUNCH, CTL_KICK, CTL_ESCAPE, CTL_GUARD_PUNCH, CTL_PUNCH_KICK, CTL_GUARD_KICK,
			CTL_ALL_THREE, CTL_START, CTL_COIN, CTL_MENU };
		// (a player's keys: the four combinations on a page of their own, Button combinations, as main.cpp's keys_page)
		std::vector<row> keys, pads, combos;
		for (int a = 0; a < 15; a++) {
			if (a == 8) keys.push_back({ CTL_COMBOS, F(CTL_COMBOS_HELP, { { "list", listed({ CTL_GUARD_PUNCH, CTL_PUNCH_KICK, CTL_GUARD_KICK, CTL_ALL_THREE }) } }), "" });
			(a >= 8 && a <= 11 ? combos : keys).push_back({ names[a], a == 13 ? CTL_KEY_HELP_COIN : a == 14 ? CTL_KEY_HELP_MENU : CTL_KEY_HELP, a == 14 ? F(CTL_KEY_N, { { "n", "231" } }) : "Left Shift" });
			if (a >= 4) pads.push_back({ names[a], a == 13 ? CTL_PADB_HELP_COIN : a == 14 ? CTL_PADB_HELP_MENU : CTL_PADB_HELP, a == 5 ? PAD_RIGHT_STICK : a == 6 ? PAD_TOUCHPAD : a == 7 ? std::string(PAD_DPAD_RIGHT) : std::string("LB") });
		}
		page_shots(gm, w, h, "pages_keys.png", P_P2_KEYBOARD, keys);
		page_shots(gm, w, h, "pages_combos.png", CTL_COMBOS, combos);
		page_shots(gm, w, h, "pages_pad.png", P_P2_PAD, pads);
		page_shots(gm, w, h, "pages_keys2.png", P_P2_KEYBOARD2, keys);   // (the second keys' and buttons': the same rows)
		page_shots(gm, w, h, "pages_pad2.png", P_P2_PAD2, pads);
	}
	// the prompts that take the help's place while a key, a button or a set-up step is waited for
	{
		menu_page root{ "", "", {} };
		root.items.push_back({ M_RESUME, "", {}, {}, {} });
		gm.open(root);
		menu_page p{ P_P1_PAD, "", {} };
		p.items.push_back({ CTL_GUARD_PUNCH, "", [] { return std::string("-"); }, {}, {} });
		gm.push(p);
		for (const std::string &prompt : { fill(CTL_PRESS_PAD_VIEW_P2, { { "control", CTL_GUARD_PUNCH } }), fill(CTL_PRESS_KEY_P1, { { "control", CTL_ALL_THREE } }),
		                                   fill(SETUP_PRESS, { { "button", SETUP_MENU_STEP }, { "player", "2" }, { "n", "10" }, { "of", "10" } }) }) {
			gm.capture(prompt, [](const SDL_Event &) { return false; });
			shot("", w, h);
			for (const std::string &o : gm.overflow()) overflows.push_back("a prompt: " + o);
		}
		shot("pages_prompt.png", w, h);
		gm.end_capture();
		gm.close();
	}
	// the toasts (bottom right): each of the longest
	for (const std::string &t : { fill(CPU_NOTICE_CUT, { { "why", CPU_WHY_NO_DRIVER }, { "todo", CPU_TODO_OTHER_DRIVER } }), std::string(SETUP_HELD),
	                              fill(DURAL_STAGE_WAIT, { { "n", "12" } }), std::string(DURAL_STAGE_NOT_NOW), fill(SETUP_DONE_BOTH, { { "player", "1" }, { "other", "2" } }),
	                              std::string(CTL_RESET_FORGOTTEN), std::string(SETUP_STEAM), std::string(LINK_IS_TEAM_BATTLE),
	                              fill(UPD_TOAST, { { "v", "0.1.23-alpha" } }) }) {
		gm.toast(t);
		shot("", w, h);
		for (const std::string &o : gm.overflow()) overflows.push_back("a toast: " + o);
	}
	gm.toast(fill(CPU_NOTICE_CUT, { { "why", CPU_WHY_NO_DRIVER }, { "todo", CPU_TODO_OTHER_DRIVER } }));
	shot("pages_toast.png", w, h);
	gm.toast("");
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: SDL_VIDEO_DRIVER=offscreen [MENUTEST_RENDERER=gpu] menutest <out folder> [width height]\n"); return 2; }
	out = argv[1];
	const int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
	if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "menutest: %s\n", SDL_GetError()); return 1; }
	SDL_Window *win = SDL_CreateWindow("menutest", w, h, 0);
	// (MENUTEST_RENDERER=gpu: as the game draws on a GPU, e.g. Mesa's lavapipe without one; else SDL's software one)
	const char *rd = getenv("MENUTEST_RENDERER");
	ren = win ? SDL_CreateRenderer(win, rd && *rd ? rd : "software") : nullptr;
	if (!ren) { fprintf(stderr, "menutest: %s\n", SDL_GetError()); return 1; }
	// the language: VF_LANG (en, ko, ja, zh-Hans, ..., pseudo), else English
	tr::set(getenv("VF_LANG") && *getenv("VF_LANG") ? tr::resolve(tr::choice{}) : tr::lang::en);
	printf("language: %s\n", tr::code(tr::current()));
	game_menu gm;
	menu = &gm;
	if (!gm.init(win, ren, "assets")) { fprintf(stderr, "menutest: the menu's files aren't in ./assets\n"); return 1; }
	gm.set_footer("v0.1.3-alpha   Oct 21 2026");
	// MENUTEST_BACKDROP=game.bmp MENUTEST_NET="NAME,ping,delay,rollback,relay(0/1),level": only the connection display,
	// over that picture of the game (as the game draws it, on MENUTEST_RENDERER's renderer), to net_over.png
	if (const char *bd = getenv("MENUTEST_BACKDROP"); bd && *bd) {
		SDL_Surface *img = SDL_LoadBMP(bd);
		backdrop = img ? SDL_CreateTextureFromSurface(ren, img) : nullptr;
		if (img) SDL_DestroySurface(img);
		if (!backdrop) { fprintf(stderr, "menutest: %s: %s\n", bd, SDL_GetError()); return 1; }
		game_menu::net_cells c;
		char name[32] = "";
		int relay = 0;
		if (const char *n = getenv("MENUTEST_NET"))
			sscanf(n, "%31[^,],%d,%d,%d,%d,%d", name, &c.ping, &c.delay, &c.rollback, &relay, &c.level);
		c.name = name;
		c.relay = relay != 0;
		gm.set_net(c);
		shot("", w, h);
		shot("net_over.png", w, h);
		return 0;
	}

	static m3::Model3 board;   // (no ROM: the pages only read the settings and the match's state)
	board.backup.assign(0x20000, 0);
	Online o(board);
	o.identity = { "vf3", "test", 1, "" };
	OnlineSettings s;
	s.name = "MRFINSTER";
	auto changed = [] {};
	auto root = [&] {
		menu_page p{ "", "", {} };
		for (tr::text r : { tr::M_RESUME, tr::P_DISPLAY, tr::P_SOUND, tr::P_GAME, tr::M_ARCADE, tr::P_ONLINE, tr::P_CONTROLS, tr::M_QUIT })
			p.items.push_back({ r, "", {}, {}, {} });
		return p;
	};

	gm.open(root());
	shot("root.png", w, h);
	// a pointer resting over the rows: moving it selects, the rows made again under it (a value's change) don't
	for (int x = w / 8; x < w && gm.selected() == 0; x += w / 8)   // (where the rows are: across, then down)
		for (int y = 0; y < h && gm.selected() == 0; y += 4) { mouse(float(x), float(y)); shot("", w, h); }
	const int hovered = gm.selected();
	key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_DOWN);
	const int chosen = gm.selected();
	for (int i = 0; i < 3; i++) { gm.refresh(); shot("", w, h); }
	if (hovered == 0 || chosen == hovered || gm.selected() != chosen) {
		fprintf(stderr, "menutest: the pointer: hovered %d, then %d chosen, %d after the rows were made again\n", hovered, chosen, gm.selected());
		return 1;
	}
	printf("pointer: hovering selected row %d; row %d stayed selected as the rows were made again\n", hovered, chosen);
	gm.close();
	gm.open(root());
	int fails = 0;
	auto check = [&](bool ok, const char *what) { if (!ok) { fprintf(stderr, "menutest: FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); };
	auto top = [&] { return gm.top_name(); };
	// (a picture's words not drawn over each other: the page's, the note's, a banner's, the versus screen's)
	auto room2_clear = [&](const char *name) {
		const std::vector<std::string> c = gm.clashes();
		for (const std::string &x : c) check(false, (std::string(name) + ": " + x).c_str());
		if (c.empty()) printf("ok   %s: no words over others\n", name);
	};
	auto poll_for = [&](double ms, std::function<bool()> until) {   // (the Online's work, in real time)
		const uint64_t t0 = SDL_GetTicks();
		while (SDL_GetTicks() - t0 < ms && !until()) { o.poll(double(SDL_GetTicks())); SDL_Delay(10); }
	};
	auto again = [&] { gm.replace(online_menu::page(o, s, gm, changed), -1); };
	gm.push(online_menu::page(o, s, gm, changed));
	shot("home.png", w, h);
	check(gm.selected() == 1, "the Online screen (the room browser) opens on Quick match");
	key(SDL_SCANCODE_RIGHT);
	check(gm.selected() == 2, "Right goes to Create room");
	key(SDL_SCANCODE_DOWN);
	check(gm.selected() >= 5 && gm.selected() <= 7, ("Down goes to the filters (" + std::to_string(gm.selected()) + ")").c_str());
	key(SDL_SCANCODE_UP);
	key(SDL_SCANCODE_UP);
	check(gm.selected() == 0, "Up from the actions goes to the name");
	pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);   // (a pad now: its hints)
	shot("home_pad.png", w, h);
	check(gm.pad_mode(), "a pad's button: pad mode");

	// your name, on a pad: Y from the home screen
	pad(SDL_GAMEPAD_BUTTON_NORTH);
	check(top() == tr::P_YOUR_NAME.c_str(), "Y opens the name");
	for (int i = 0; i < 3; i++) pad(SDL_GAMEPAD_BUTTON_WEST);   // (X takes letters off)
	for (int i = 0; i < 5; i++) pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
	shot("name_pad.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_SOUTH);   // (A: the letter selected, T)
	pad(SDL_GAMEPAD_BUTTON_START);   // (saved)
	check(s.name == "MRFINST", ("the pad typed the name (" + s.name + ")").c_str());
	check(top() == tr::P_ONLINE.c_str(), "Start saves and goes back");
	// on a keyboard
	gm.select_row(0);   // (the name: Up from wherever the pad left it)
	key(SDL_SCANCODE_RETURN);
	check(top() == tr::P_YOUR_NAME.c_str(), "Enter on the name opens it");
	key(SDL_SCANCODE_BACKSPACE);
	text("ER");
	shot("name_key.png", w, h);
	key(SDL_SCANCODE_SPACE);   // (types a space: doesn't press anything)
	check(top() == tr::P_YOUR_NAME.c_str(), "Space types, it doesn't press");
	key(SDL_SCANCODE_RETURN);
	check(s.name == "MRFINSER", ("the keyboard typed the name (" + s.name + ")").c_str());
	s.name = "MRFINSTER";
	again();

	// joining with a code: a keyboard, then a pad
	gm.select_row(1);   // (Quick match: the browser keeps what was selected last)
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);
	check(top() == tr::P_ENTER_CODE.c_str(), "Enter code opens the code");
	text("zz");
	key(SDL_SCANCODE_Z);   // (a letter's key: its letter comes as text, not twice)
	text("z");
	shot("join_key.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	for (int i = 0; i < 7; i++) pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	shot("join_pad.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_EAST);   // (B: back)
	check(top() == tr::P_ONLINE.c_str(), "B goes back from the code");

	// no name yet: asked for first, then on to the code
	s.name.clear();
	again();
	gm.select_row(1);
	key(SDL_SCANCODE_RIGHT);   // (a keyboard again: Quick match to Join by code)
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);
	check(top() == tr::P_YOUR_NAME.c_str(), "Join with no name asks for it");
	shot("name_first.png", w, h);
	text("AKIRA");
	key(SDL_SCANCODE_RETURN);
	check(s.name == "AKIRA" && top() == tr::P_ENTER_CODE.c_str(), "then goes on to the code");
	key(SDL_SCANCODE_ESCAPE);
	s.name = "MRFINSTER";

	// Play by address, from the browser's More: its page, then the address typed on a keyboard and a pad
	again();
	gm.select_row(1);
	for (int i = 0; i < 3; i++) key(SDL_SCANCODE_RIGHT);   // (Quick match to More)
	key(SDL_SCANCODE_RETURN);
	check(top() == tr::P_MORE.c_str(), "More: what's seldom wanted");
	shot("more.png", w, h);
	settle_check("more.png", w, h);
	key(SDL_SCANCODE_RETURN);
	check(top() == tr::P_PLAY_ADDRESS.c_str(), "the link opens Play by address");
	shot("address_page.png", w, h);
	key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_RETURN);
	check(top() == tr::P_JOIN_ADDRESS.c_str(), "Join by address opens the address");
	text("192.168.1.20");
	shot("address_key.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
	shot("address_pad.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_EAST);   // (B: back to Play by address)
	check(top() == tr::P_PLAY_ADDRESS.c_str(), "B goes back from the address");
	key(SDL_SCANCODE_ESCAPE);
	key(SDL_SCANCODE_ESCAPE);   // (More: back to the browser)
	s.address.clear();
	again();

	// Settings, in More under Play by address: Random's max ping, a step at a time (Any, 80, 100, ...), and the chime
	gm.select_row(1);
	for (int i = 0; i < 3; i++) key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);
	key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_RETURN);
	check(top() == tr::P_SETTINGS.c_str(), "Settings is in More, under Play by address");
	for (int i = 0; i < 3; i++) key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RIGHT);
	check(s.max_ping == 100, ("Random: max ping steps to 100 ms (" + std::to_string(s.max_ping) + ")").c_str());
	key(SDL_SCANCODE_LEFT);
	key(SDL_SCANCODE_LEFT);
	check(s.max_ping == 0, "and back to Any");
	key(SDL_SCANCODE_LEFT);
	check(s.max_ping == 250, "Any's left is the most, 250 ms");
	key(SDL_SCANCODE_DOWN);
	check(s.stage_select, "Stage select on by default (issue #32)");
	key(SDL_SCANCODE_RIGHT);
	check(!s.stage_select, "Stage select turns off");
	key(SDL_SCANCODE_RIGHT);
	check(s.stage_select, "and on");
	key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_RIGHT);
	check(!s.chime, "the chime turns off");
	key(SDL_SCANCODE_RIGHT);
	check(s.chime, "and on");
	shot("settings.png", w, h);
	settle_check("settings.png", w, h);
	s.max_ping = 0;
	key(SDL_SCANCODE_ESCAPE);
	key(SDL_SCANCODE_ESCAPE);   // (More: back to the browser)
	again();

	// by address: hosting, then joining a host that isn't there
	tr::line err;
	setenv("VF3_TEST_LAN", "192.168.1.20=eth0", 1);   // (the addresses shown: these, not this machine's)
	if (!o.host(s, err)) fprintf(stderr, "menutest: hosting: %s\n", err.en().c_str());
	again();
	shot("address_hosting.png", w, h);
	o.stop();
	// on several networks (a Wi-Fi Direct hotspot without internet beside the home's: issue #46): each, with its
	// adapter; loopback never, link-local (169.254) only when it's all there is
	setenv("VF3_TEST_LAN", "127.0.0.1=lo,192.168.1.20=Wi-Fi,169.254.10.3=Ethernet 2,192.168.137.1=Local Area Connection* 10,10.8.0.2=", 1);
	if (!o.host(s, err)) fprintf(stderr, "menutest: hosting: %s\n", err.en().c_str());
	check(o.local_addresses().size() == 3 && o.local_addresses()[1].ip == "192.168.137.1" && o.local_addresses()[1].adapter == "Local Area Connection* 10" &&
	          o.local_address() == "192.168.1.20",
	      "hosting on several networks: every address, loopback and link-local left out");
	again();
	shot("address_hosting_many.png", w, h);
	o.stop();
	setenv("VF3_TEST_LAN", "127.0.0.1=lo,169.254.10.3=Ethernet 2", 1);
	check(lan_list::addresses("").size() == 1 && lan_list::addresses("")[0].ip == "169.254.10.3", "link-local when it's the only one");
	setenv("VF3_TEST_LAN", "10.0.0.5=a,192.168.1.9=b", 1);
	check(lan_list::addresses("192.168.1.9")[0].ip == "192.168.1.9", "the internet's address first");
	// the most shown (6, in two rows), virtual machines' adapters after the real ones: the hotspot's not pushed off
	setenv("VF3_TEST_LAN", "172.29.16.1=vEthernet (WSL),192.168.56.1=VirtualBox Host-Only Network,192.168.1.20=Wi-Fi,172.17.0.1=docker0,"
	       "192.168.122.1=virbr0,192.168.137.1=Local Area Connection* 10,10.8.0.2=Ethernet 3", 1);
	{
		const auto l = lan_list::addresses("192.168.1.20");
		check(l.size() == 7 && l[0].ip == "192.168.1.20" && l[1].ip == "192.168.137.1" && l[2].ip == "10.8.0.2" && l[3].virt && l[6].virt,
		      "virtual adapters listed after the real ones");
	}
	if (!o.host(s, err)) fprintf(stderr, "menutest: hosting: %s\n", err.en().c_str());
	again();
	shot("address_hosting_most.png", w, h);
	o.stop();
	unsetenv("VF3_TEST_LAN");
	s.address = "192.168.1.20";
	if (!o.join(s, err)) fprintf(stderr, "menutest: joining: %s\n", err.en().c_str());
	o.poll(1000);
	again();
	shot("address_joining.png", w, h);
	for (double t = 1000; t < 20000 && o.phase() == Online::Phase::joining; t += 100) o.poll(t);   // (no host: it gives up)
	again();
	shot("address_failed.png", w, h);
	check(!o.join_failure().empty() && o.last_try() == Online::Try::join, "a join by address that failed: Couldn't connect, to try again");
	key(SDL_SCANCODE_ESCAPE);
	check(o.join_failure().empty() && top() == tr::P_ONLINE.c_str() && gm.selected() < 8, "Esc on Couldn't connect: back to the browser");

	// a room code's (the lobby: VF_LOBBY, e.g. tools/lobby's npx wrangler dev, or test/serve.mjs with its switches; an
	// older lobby too: what it has on, from its /v1/online, says what's expected)
	if (!Online::lobby_default().empty()) {
		bool has_rooms = false, has_watch = false;
		{
			vfnet::json::Value f;
			const vfnet::http::Reply fr = vfnet::http::request("GET", Online::lobby_default() + "/v1/online", "", 5000, 1 << 20);
			if (fr.status == 200 && vfnet::json::parse(fr.body, f)) {
				has_rooms = f["rooms"].kind == vfnet::json::Value::list;
				has_watch = f["features"]["watch"].kind == vfnet::json::Value::boolean && f["features"]["watch"].b;
			}
			printf("the lobby: rooms %s, watching %s%s\n", has_rooms ? "on" : "off", has_watch ? "on" : "off", f["features"].kind == vfnet::json::Value::object ? "" : " (an older lobby: it says neither)");
		}
		// a random opponent: this one searching, and another (a second game's, here) found
		again();
		gm.select_row(1);   // (Quick match: the browser keeps what was selected last)
		key(SDL_SCANCODE_RETURN);   // (Random)
		poll_for(8000, [&] { return o.step_now() == Online::Step::searching || o.phase() == Online::Phase::off; });
		poll_for(1500, [] { return false; });
		again();
		shot("random_searching.png", w, h);
		check(o.step_now() == Online::Step::searching, ("Random: searching (" + o.status() + ")").c_str());
		static m3::Model3 board2;
		board2.backup.assign(0x20000, 0);
		Online o2(board2);
		o2.identity = o.identity;
		OnlineSettings s2 = s;
		s2.name = "AKIRAFAN";
		if (!o2.find_random(s2, err)) fprintf(stderr, "menutest: the other's random: %s\n", err.en().c_str());
		const uint64_t r0 = SDL_GetTicks();
		while (SDL_GetTicks() - r0 < 15000 && !(o.step_now() == Online::Step::connecting && o2.step_now() == Online::Step::connecting)) {
			o.poll(double(SDL_GetTicks()));
			o2.poll(double(SDL_GetTicks()));
			SDL_Delay(10);
		}
		again();
		shot("random_found.png", w, h);
		check(o.room_code() == o2.room_code() && !o.room_code().empty() && o.is_hosting() != o2.is_hosting(),
		      ("two searching are matched: one room, a host and a guest (" + o.room_code() + ", " + o2.room_code() + ")").c_str());
		check(o.peer() == "AKIRAFAN" && o2.peer() == "MRFINSTER", ("each has the other's name (" + o.peer() + ", " + o2.peer() + ")").c_str());
		check(!o.peer_country().empty() || !o.my_country().empty(), ("countries from the lobby (" + o.my_country() + ", " + o.peer_country() + ")").c_str());
		o2.stop();
		o.stop();
		// the Random card: how many play at random (the two just matched, counted for a while), asked again every 30 s
		poll_for(40000, [&] { return o.players_online(s) > 0; });
		again();
		shot("home_online.png", w, h);
		check(o.players_online(s) >= 2, ("the Random card's count (" + std::to_string(o.players_online(s)) + ")").c_str());
		gm.select_row(1);
		key(SDL_SCANCODE_RIGHT);
		key(SDL_SCANCODE_RETURN);   // (Create room: on Create, a room with the code only, as ever)
		key(SDL_SCANCODE_RETURN);
		poll_for(15000, [&] { return !o.room_code().empty() || o.phase() == Online::Phase::off; });
		again();
		shot("room_host.png", w, h);
		printf("room: hosting, code %s (%s)\n", o.room_code().c_str(), o.status().c_str());
		check(o.room_code().size() == 7, "hosting: a code");
		const std::string kept = o.room_code();
		key(SDL_SCANCODE_RETURN);   // (Copy code)
		shot("room_copied.png", w, h);
		char *clip = SDL_GetClipboardText();
		check(clip && kept == clip, "Copy code puts it on the clipboard");
		SDL_free(clip);
		// the room's code kept by Play again (as after a match)
		Online::LastMatch m;
		m.any = m.was_host = m.by_code = true;
		m.opponent = "AKIRAFAN";
		m.code = kept.size() == 7 ? kept.substr(0, 3) + kept.substr(4) : "";
		o.stop();
		o.set_last_match(m);
		again();
		key(SDL_SCANCODE_RETURN);   // (Play again)
		poll_for(15000, [&] { return !o.room_code().empty() || o.phase() == Online::Phase::off; });
		check(o.room_code() == kept, ("Play again keeps the code (" + o.room_code() + " for " + kept + ")").c_str());
		o.stop();
		// joining a code nobody has: it says so, where it stopped
		again();
		gm.select_row(1);
		key(SDL_SCANCODE_RIGHT);
		key(SDL_SCANCODE_RIGHT);
		key(SDL_SCANCODE_RETURN);
		text("zzz222");   // (the 6th letter: it joins)
		check(top() == tr::P_ONLINE.c_str() && o.phase() == Online::Phase::joining, "the 6th letter joins");
		shot("room_joining.png", w, h);
		poll_for(15000, [&] { return o.phase() == Online::Phase::off; });
		again();
		shot("room_failed.png", w, h);
		printf("room: joining ZZZ-222: %s\n", o.join_failure().c_str());
		check(o.failed_at() == Online::Step::finding, "the failure is at finding the room");
		// a guest's Play again: waits for the host's room
		Online::LastMatch g;
		g.any = g.by_code = true;
		g.opponent = "AKIRAFAN";
		g.code = "ZZZ222";
		o.forget_failure();
		o.set_last_match(g);
		again();
		key(SDL_SCANCODE_RETURN);   // (Play again: joins ZZZ-222, waiting for it)
		poll_for(6000, [&] { return o.phase() == Online::Phase::off; });
		again();
		shot("room_patient.png", w, h);
		check(o.phase() == Online::Phase::joining && o.patient(), ("a guest's Play again waits for the room (" + o.status() + ")").c_str());
		o.stop();
		// a public room: in the rooms list (the lobby's), its host by name; Rooms shows it, A on it joins
		again();
		s.room_who = 2;
		const std::string named_was = s.room_name;
		s.room_name = "SH1T ROOM";   // (a name the lobby's word filter won't show: listed as its host's)
		if (!o.host_room(s, err)) fprintf(stderr, "menutest: a public room: %s\n", err.en().c_str());
		poll_for(15000, [&] { return o.step_now() == Online::Step::waiting || o.phase() == Online::Phase::off; });
		const std::string pub = o.room_code();
		check(pub.size() == 7 && o.watch_code().empty() != has_watch, ("a public room: a code, and a watch code only with watching (" + pub + ", " + o.watch_code() + ")").c_str());
		if (has_rooms) check(o.listed_as_name() == "MRFINSTER'S ROOM", ("a blocked room name: listed as its host's, and the host told (" + o.listed_as_name() + ")").c_str());
		else check(o.listed_as_name().empty(), "rooms off: the host not told it's listed");
		again();
		shot("room_host_listed.png", w, h);
		s.room_name = named_was;
		o2.identity = o.identity;
		bool listed = false;
		for (int i = 0; i < 40 && !listed; i++) {
			o2.rooms_list(s2, true);
			poll_for(500, [] { return false; });
			for (const Online::RoomRow &r : o2.rooms_list(s2)) listed = listed || (!r.playing && r.code == pub.substr(0, 3) + pub.substr(4) && r.name[0] == "MRFINSTER");
		}
		check(listed == has_rooms, has_rooms ? "the public room is in another game's Rooms, by its host's name" : "rooms off: no list, so not in it");
		if (!has_rooms) {   // (another game's browser: its one plain line)
			o2.rooms_list(s2, true);
			for (const uint64_t t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 3000 && o2.rooms_state() != 3;) { o2.rooms_list(s2); o.poll(double(SDL_GetTicks())); SDL_Delay(10); }
			gm.replace(online_menu::page(o2, s2, gm, changed), -1);
			shot("browser_rooms_off.png", w, h);
			check(o2.rooms_state() == 3, "rooms off: the browser says Rooms aren't available right now");
		}
		if (!has_watch) {   // (a watcher told plainly, the host's room as it was)
			if (!o2.watch(s2, pub, err)) fprintf(stderr, "menutest: watch: %s\n", err.en().c_str());
			for (const uint64_t t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 8000 && o2.phase() != Online::Phase::off;) { o.poll(double(SDL_GetTicks())); o2.poll(double(SDL_GetTicks())); SDL_Delay(10); }
			printf("watching with watching off: %s\n", o2.join_failure().c_str());
			check(o2.phase() == Online::Phase::off && o2.join_failure() == std::string(tr::WATCH_OFF), "watching off: a watcher told Watching isn't available right now");
			check(o.step_now() == Online::Step::waiting, "and the host's room as it was");
			o2.forget_failure();
		}
		// 400 watching a match (the lobby's most a match): one more is told plainly, and nothing else happens (the
		// watchers counted only while a match is on: one sent as a host's game sends it, its own room's, by hand)
		if (has_watch) {
			const std::string lob = s.lobby.empty() ? Online::lobby_default() : s.lobby;
			vfnet::json::Value hv;
			const vfnet::http::Reply hr = vfnet::http::request("POST", lob + "/v1/host", "{\"game\":" + vfnet::json::quote(o.identity.game) + ",\"version\":" +
				vfnet::json::quote(o.identity.build) + ",\"content\":" + vfnet::json::quote(vfnet::watch::hex(o.identity.content)) + ",\"name\":\"FULLHOST\",\"cands\":[],\"relay\":\"\",\"watchable\":true}", 5000);
			const std::string wc = vfnet::json::parse(hr.body, hv) ? hv["wid"].str() : "";
			vfnet::watch::Start st;
			st.match = vfnet::watch::new_match_id();
			st.meta.game = o.identity.game;
			st.meta.content = vfnet::watch::hex(o.identity.content);
			st.meta.name[0] = "FULLHOST";
			st.meta.name[1] = "FULLGUEST";
			st.blob.assign(16, 1);
			const std::vector<uint8_t> sb = vfnet::watch::encode(st);
			const bool on = wc.size() == 6 && vfnet::http::request("POST", lob + "/v1/watch/start?w=" + wc + "&k=" + hv["wkey"].str(), std::string(sb.begin(), sb.end()), 5000).status == 200;
			int counted = 0;
			for (int i = 0; i < 400 && on; i++) {
				char sid[17];
				snprintf(sid, sizeof sid, "%016x", 0x5000 + i);
				vfnet::json::Value sv;
				const vfnet::http::Reply sr = vfnet::http::request("POST", lob + "/v1/watch/seen?w=" + wc + "&s=" + sid, "", 3000, 4096);
				counted += sr.status == 200 && vfnet::json::parse(sr.body, sv) && sv["watchers"].n == i + 1;
			}
			check(counted == 400, ("400 watching a match, counted (" + std::to_string(counted) + ")").c_str());
			if (!o2.watch(s2, wc, err)) fprintf(stderr, "menutest: watch: %s\n", err.en().c_str());
			for (const uint64_t t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 8000 && o2.phase() != Online::Phase::off;) { o.poll(double(SDL_GetTicks())); o2.poll(double(SDL_GetTicks())); SDL_Delay(10); }
			printf("watching the 401st: %s\n", o2.join_failure().c_str());
			check(o2.phase() == Online::Phase::off && o2.join_failure() == std::string(tr::WATCH_FULL), "the 401st watcher: Too many watching right now");
			check(o.step_now() == Online::Step::waiting, "and the host's room as it was");
			o2.forget_failure();
		}
		o.stop();
		s.room_who = 0;
		// watching a code nobody's match has: a lobby with watching answers it as a room whose match hasn't begun (so
		// watching is no way to find room codes): the game looks for its match (and after 2 minutes says none began);
		// without watching (switched off, an older lobby) it says so at once
		again();
		if (!o.watch(s, "QQQ222", err)) fprintf(stderr, "menutest: watch: %s\n", err.en().c_str());
		if (has_watch) {
			poll_for(4000, [&] { return o.phase() == Online::Phase::off; });
			again();
			shot("watch_looking.png", w, h);
			check(o.watching() && o.watch_status() == tr::fill(tr::WATCH_LOOKING, { { "code", "QQQ-222" } }), ("watching a code with no match: looking for it (" + o.watch_status() + ")").c_str());
			o.stop();
		} else {
			poll_for(8000, [&] { return o.phase() == Online::Phase::off; });
			again();
			shot("watch_failed.png", w, h);
			check(o.last_try() == Online::Try::watch && !o.join_failure().empty(), ("watching a code with no match: said (" + o.join_failure() + ")").c_str());
		}
		o.forget_failure();
	}

	// Create room: on Create (unlisted, the code only, as ever), its name filled in, Listed in Rooms a switch; with Rooms
	// off at the lobby (or an older lobby, without them), no Listed in Rooms: a room with its code only
	{
		again();
		// (the lobby's word on Rooms had first: the page's rows follow it, and so do these checks)
		for (const uint64_t t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 8000 && o.rooms_state() == 0;) { o.rooms_list(s); SDL_Delay(10); }
		const std::string lobby_was = s.lobby;
		s.lobby = "none";   // (no room opened by these pages' presses)
		gm.select_row(2);
		key(SDL_SCANCODE_RETURN);   // (Create room: the name's set)
		const bool listing = o.rooms_state() != 3;   // (as create_room_page has it: Rooms off, no row to be listed by)
		const int c_listed = listing ? 1 : -1, c_rule = listing ? 2 : 1, c_go = c_rule + 4;   // (the room's rules between Listed and Create)
		printf("Create room: Rooms %s at the lobby, so %s\n", listing ? "on (or not known)" : "off", listing ? "Listed in Rooms offered" : "no Listed in Rooms");
		check(top() == tr::P_CREATE_ROOM.c_str() && gm.selected() == c_go && s.room_who == 0, "Create room: on Create, not listed (the code only) as ever");
		gm.select_row(c_go + 1);
		check(gm.selected() == c_go, listing ? "Create room: its name, Listed in Rooms, the room's rules and Create" : "Rooms off: Create room has its name, the room's rules and Create, no Listed in Rooms");
		settle_check("create_room.png", w, h);
		shot("create_room.png", w, h);
		room2_clear("create_room.png");
		{   // the room's rules, set before the room is made (the creator's saved ones), sent with its create
			const bool side0 = s.rule_side, dural0 = s.rule_dural, stage0 = s.rule_stage;
			const int max0 = s.rule_max;
			gm.select_row(c_rule);
			key(SDL_SCANCODE_RETURN);   // (Winner may change side)
			pad(SDL_GAMEPAD_BUTTON_SOUTH);   // (and back, by the pad)
			gm.select_row(c_rule + 1);
			key(SDL_SCANCODE_RETURN);   // (Dural)
			gm.select_row(c_rule + 2);
			key(SDL_SCANCODE_RETURN);   // (Players in the room: the next value)
			check(s.rule_side == side0 && s.rule_dural == !dural0 && s.rule_max == (max0 ? s.rule_max : 2) && s.rule_stage == stage0,
			      "Create room: the room's rules on the form, each row its next value (Enter, the pad's A)");
			gm.select_row(c_rule + 3);
			for (int i = 0; i < 8; i++) { shot("", w, h); SDL_Delay(40); }
			shot("create_room_rule.png", w, h);   // (a rule selected: its help under the rows)
			room2_clear("create_room_rule.png");
			{   // a click on a rule's row: its next value
				std::vector<game_menu::box> rows = gm.boxes("row");
				if (int(rows.size()) > c_rule + 3) {
					const auto &b = rows[size_t(c_rule + 3)];
					button(true, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2);
					button(false, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2);
				}
				check(s.rule_stage == !stage0, "Create room: a click on Challenger picks the stage changes it");
			}
			s.rule_side = side0; s.rule_dural = dural0; s.rule_max = max0; s.rule_stage = stage0;
			gm.select_row(c_go);
		}
		if (c_listed >= 0) {
			gm.select_row(c_listed);
			key(SDL_SCANCODE_RETURN);
			check(s.room_who == 2, "Listed in Rooms: on");
			shot("create_listed.png", w, h);
			key(SDL_SCANCODE_RETURN);
			check(s.room_who == 0, "and off");
		} else {
			gm.select_row(0);
			gm.select_row(c_go);
			check(s.room_who == 0 && gm.selected() == c_go, "Rooms off: nothing switches listing on, the room has its code only");
		}
		gm.select_row(0);
		key(SDL_SCANCODE_RETURN);
		check(top() == tr::P_ROOM_NAME.c_str(), "Name: typed");
		for (int i = 0; i < 20; i++) key(SDL_SCANCODE_BACKSPACE);
		text("korea finals");
		shot("room_name.png", w, h);
		key(SDL_SCANCODE_RETURN);
		check(s.room_name == "KOREA FINALS" && top() == tr::P_CREATE_ROOM.c_str(), ("the room's name (" + s.room_name + ")").c_str());
		key(SDL_SCANCODE_ESCAPE);
		check(top() == tr::P_ONLINE.c_str(), "Esc: back to the browser");
		// the browser with 60 rooms (the lobby's, made up): scrolled with the selection, a page at a time, filtered
		std::vector<Online::RoomRow> rows;
		// (every host once, every opponent once, a room's name once: as a real list would be)
		const char *names[] = { "SEOULKICK", "DENVERDAN", "SARAH.B", "KAGE-MARU", "AOI", "SHUN", "JACKY", "LION", "WOLF", "TAKA", "CHIBITA", "BUSAN!",
			"MOKUJIN", "TEKKAMAN", "RYUSEI", "HAYATE", "JUNGKOOK", "MINSU", "HYUNWOO", "DAEJEON", "OSAKA-K", "NAGOYA", "SAPPORO", "KYOTO99", "LYONNAIS",
			"MARSEIL", "BERLINER", "HAMBURG", "LONDONER", "LEEDS", "TORONTO", "VANCOUV", "RIO-SP", "SAOPAULO", "TAIPEI", "KAOHSIUNG", "HKBOY", "KOWLOON",
			"NYC-VF", "TEXAS", "CHICAGO", "SEATTLE", "MADRID", "LISBOA", "AKIRA99", "DURALFAN", "BEAT-KID", "OHMYGOD", "BLUE", "KOREA1", "SEOULMAN",
			"TOKYO-JO", "PARISVF", "NEOGEO", "ARCADIA", "SPIKE", "JEFFRY", "PAI", "LAU", "MRFINSTER" };
		const char *guests[] = { "ZERO", "ROOKIE", "SENSEI", "CHAMP", "NOVA", "ECHO", "RAVEN", "TIGER", "PANDA", "COMET", "GHOST", "FALCON", "VIPER",
			"ORCA", "MANTIS", "COBRA", "BISON", "LYNX", "OTTER", "HERON", "SWIFT", "EMBER", "FROST", "STORM", "BLAZE", "DRIFT", "PIXEL", "TURBO", "RETRO",
			"NEON", "LASER", "ROCKET", "JET", "ACE", "KING", "QUEEN", "BISHOP", "KNIGHT", "ROOK", "PAWN", "SAMURAI", "NINJA", "RONIN", "SHOGUN", "DAIMYO",
			"MONK", "SAGE", "ORACLE", "WIZARD", "BARD", "RANGER", "SCOUT", "PILOT", "CAPTAIN", "MAJOR", "CORPORAL", "MEDIC", "SNIPER", "TANK", "BRAWLER" };
		const char *lands[] = { "KR", "US", "JP", "JP", "CA", "TW", "US", "FR", "CA", "JP", "JP", "KR", "JP", "JP", "JP", "JP", "KR", "KR", "KR", "KR", "JP", "JP",
			"JP", "JP", "FR", "FR", "DE", "DE", "GB", "GB", "CA", "CA", "BR", "BR", "TW", "TW", "HK", "HK", "US", "US", "US", "US", "ES", "PT", "KR", "DE",
			"BR", "KR", "US", "KR", "KR", "JP", "FR", "DE", "GB", "BR", "GB", "HK", "HK", "FR" };
		const char *regions_of[][2] = { { "KR", "AS" }, { "JP", "AS" }, { "TW", "AS" }, { "HK", "AS" }, { "US", "NA" }, { "CA", "NA" }, { "FR", "EU" }, { "DE", "EU" },
			{ "GB", "EU" }, { "ES", "EU" }, { "PT", "EU" }, { "BR", "SA" } };
		const char *rooms_named[] = { "KOREA FINALS", "FT5 ONLY", "BEGINNERS OK", "NIGHT SET", "AKIRA'S DOJO", "TEAM KR", "PRACTICE", "LAU MIRROR", "CASUAL", "WINNER STAYS" };
		for (int i = 0; i < 60; i++) {   // (a few open, the rest playing)
			Online::RoomRow r;
			r.playing = i % 9 != 0;
			r.room = i < 10 ? rooms_named[i] : "";
			r.name[0] = names[i]; r.country[0] = lands[i];
			for (auto &rg : regions_of) if (r.country[0] == rg[0]) r.region = rg[1];
			r.code = std::string("KXQ4M") + char('A' + i % 20);
			if (r.playing) { r.name[1] = guests[i]; r.country[1] = lands[(i * 7 + 3) % 60]; r.watch = std::string("QX7KM") + char('A' + i % 20); r.wins[0] = i % 3; r.wins[1] = i % 2; r.round = 1 + i % 5; r.watchers = (i * 13) % 50; r.queue = i % 4; }
			r.older = i == 17;
			r.since = i * 40;
			rows.push_back(r);
		}
		for (int i : { 20, 41 }) {   // (two Quick match games: their players, to watch only; one the most watched)
			rows[size_t(i)].quick = true;
			rows[size_t(i)].room.clear();
			rows[size_t(i)].code.clear();
			rows[size_t(i)].queue = 0;
		}
		rows[20].watchers = 52;
		o.set_rooms(rows);
		again();
		pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT);   // (a pad's: its hints)
		shot("browser.png", w, h);
		gm.select_row(8);   // (the first room)
		for (int i = 0; i < 14; i++) pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
		check(gm.selected() == 8 + 14, ("Down through the rooms: the 15th selected (" + std::to_string(gm.selected()) + ")").c_str());
		shot("browser_scrolled.png", w, h);
		pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
		check(gm.selected() == 8 + 24, ("RB: a page on (" + std::to_string(gm.selected()) + ")").c_str());
		shot("browser_page.png", w, h);
		pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		check(gm.selected() == 8, "LB: back to the first room");
		// the list coming again with a new room at the top: the one selected stays selected, where it was
		for (int i = 0; i < 6; i++) pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
		const std::string was = rows[6].name[0] + "/" + rows[6].room;
		Online::RoomRow fresh = rows[0];
		fresh.room = "NEW ROOM"; fresh.code = "NEWAAA"; fresh.since = 0; fresh.playing = false; fresh.name[0] = "NEWCOMER"; fresh.country[0] = "KR";
		rows.insert(rows.begin(), fresh);
		o.set_rooms(rows);
		gm.replace(online_menu::page(o, s, gm, changed), -1);
		const int now_row = gm.selected() - 8;
		check(now_row >= 0 && now_row < int(rows.size()) && rows[size_t(now_row)].name[0] + "/" + rows[size_t(now_row)].room == was,
		      ("a new list: the room selected still selected (" + std::to_string(now_row) + ")").c_str());
		shot("browser_new.png", w, h);
		pad(SDL_GAMEPAD_BUTTON_NORTH);   // (Y: the room's own page)
		check(top() == tr::P_ROOM.c_str(), "Y: the room's own page");
		shot("room_view.png", w, h);
		pad(SDL_GAMEPAD_BUTTON_EAST);
		{   // (a Quick match: the most watched, the first match in the list; its own page to watch only, no Join)
			int open = 0;
			for (const Online::RoomRow &r : o.rooms_list(s)) open += !r.playing && !r.older;
			gm.select_row(8 + open - 1);
			key(SDL_SCANCODE_DOWN);   // (by a key: the hints the selected row's)
			check(gm.selected() == 8 + open, "the Quick match selected");
			shot("browser_quick.png", w, h);
			pad(SDL_GAMEPAD_BUTTON_NORTH);
			check(top() == tr::P_ROOM.c_str(), "Y on a Quick match: its page");
			shot("room_view_quick.png", w, h);
			gm.select_row(1);
			gm.select_row(2);   // (no third: Watch and Back only)
			check(gm.selected() == 1, "a Quick match's page: Watch and Back, no Join");
			key(SDL_SCANCODE_RETURN);
			check(top() == tr::P_ONLINE.c_str() && o.phase() == Online::Phase::off, "and Back goes back");
		}
		// the filter: open rooms; then playing, with none
		gm.select_row(6);   // (Open)
		key(SDL_SCANCODE_RETURN);
		check(s.rooms_show == 1, "the filter: Open");
		shot("browser_open.png", w, h);
		s.rooms_show = 2;
		std::vector<Online::RoomRow> none(rows.begin(), rows.begin() + 5);
		for (Online::RoomRow &r : none) r.playing = false;
		o.set_rooms(none);
		again();
		shot("browser_filtered_empty.png", w, h);
		s.rooms_show = 0;
		o.set_rooms({});
		again();
		shot("browser_empty.png", w, h);
		{   // (a listed room whose host locked it: the lobby lists it with no code and no watch code; its page, and A on it)
			Online::RoomRow lk;
			lk.locked = true; lk.room = "PRIVATE"; lk.name[0] = "LOCKER"; lk.country[0] = "KR";
			Online::RoomRow lkp = lk;
			lkp.playing = true; lkp.name[1] = "GUEST";
			o.set_rooms({ lk, lkp });
			again();
			for (int r : { 0, 1 }) {
				gm.select_row(8 + r);
				pad(SDL_GAMEPAD_BUTTON_NORTH);   // (Y: the room's own page)
				shot("room_view_locked.png", w, h);
				check(top() == tr::P_ROOM.c_str(), "a locked room's page: drawn");
				pad(SDL_GAMEPAD_BUTTON_EAST);
				gm.select_row(8 + r);
				key(SDL_SCANCODE_RETURN);   // (A: join it, or watch it)
				check(o.phase() == Online::Phase::off, "a locked room: nothing started without its code");
				while (top() != tr::P_ONLINE.c_str()) key(SDL_SCANCODE_ESCAPE);
			}
			o.set_rooms({});
			again();
		}
		gm.select_row(1);
		key(SDL_SCANCODE_ESCAPE);
		gm.push(online_menu::page(o, s, gm, changed));
		// watching: a match's code typed; the screen while it's found and caught up with
		s.lobby = "http://127.0.0.1:9";   // (nobody there: it goes on looking)
		{   // links (main.cpp's, through online_menu): refused while a match is on its way, followed otherwise; with no name
			// yet, the name asked for first, as every join (the host, the rooms list and the watchers show it)
			auto show = [&] { gm.close(); gm.open(root()); gm.push(online_menu::page(o, s, gm, changed)); };
			check(online_menu::link_refused(o).empty(), "a link with nothing on: followed");
			// (a port of this run's own, and another if it's taken: 14 menutests side by side, langcheck's, all hosted on
			// the settings' 7700, and all but one failed this check now and then)
			const int port_was = s.port;
			s.port = 20000 + int(SDL_GetTicksNS() / 1000 % 20000);
			for (int tries = 0; tries < 20 && !o.host(s, err); tries++) {
				fprintf(stderr, "menutest: hosting on port %d: %s: another port\n", s.port, err.en().c_str());
				s.port = 20000 + (s.port - 20000 + 7919) % 20000;
			}
			check(!online_menu::link_refused(o).empty() && o.waiting(), "a link while hosting: refused, the room kept (as in a match)");
			o.stop();
			s.port = port_was;
			if (!o.join_room(s, "KXQ4M7", err)) fprintf(stderr, "menutest: join: %s\n", err.en().c_str());
			check(!online_menu::link_refused(o).empty(), "a link while joining a room: refused");
			o.stop();
			if (!o.watch(s, "QX7KMP", err)) fprintf(stderr, "menutest: watch: %s\n", err.en().c_str());
			check(online_menu::link_refused(o).empty(), "a link while watching: followed (the watch left for it)");
			o.stop();
			s.name.clear();
			tr::line lerr;
			const bool ok = online_menu::follow_link(o, s, gm, changed, "join", "KXQ4M7", show, lerr);
			shot("link_name_first.png", w, h);
			check(ok && top() == tr::P_YOUR_NAME.c_str() && o.phase() == Online::Phase::off, "a link's join with no name yet: the name asked for first");
			text("AKIRA");
			key(SDL_SCANCODE_RETURN);
			check(s.name == "AKIRA" && top() == tr::P_ONLINE.c_str() && gm.depth() == 2 && (o.phase() == Online::Phase::joining || o.in_room2()) && o.room_code() == "KXQ-4M7",
			      ("then on into the link's room, one Online page (" + o.room_code() + ", " + std::to_string(gm.depth()) + " deep)").c_str());
			o.stop();
			// the watcher's Play next, with no name yet: the same
			s.name.clear();
			if (!o.watch(s, "QX7KMP", err)) fprintf(stderr, "menutest: watch: %s\n", err.en().c_str());
			menu_page mm{ "", "", {} };
			online_menu::match_rows(o, s, gm, changed, mm.items);
			gm.close();
			gm.open(mm);
			gm.select_row(1);   // (Play next)
			key(SDL_SCANCODE_RETURN);
			check(top() == tr::P_YOUR_NAME.c_str() && o.watching(), "Play next with no name yet: the name asked for first, still watching");
			text("AKIRA");
			key(SDL_SCANCODE_RETURN);
			check(s.name == "AKIRA" && !gm.is_open() && (o.phase() == Online::Phase::joining || o.in_room2()) && o.room_code() == "QX7-KMP",
			      ("then the room joined to play next (" + o.room_code() + ": a Rooms 2 room's line asked first, a room as before's joined if the lobby says so)").c_str());
			o.stop();
			o.forget_failure();
			s.name = "MRFINSTER";
			show();
		}
		{   // watching off at the lobby: a match on joined to play next, not watched; Rooms off: Create room lists nothing
			Online::RoomRow pr;
			pr.playing = true; pr.room = "NIGHT SET"; pr.name[0] = "SEOULKICK"; pr.name[1] = "DENVERDAN"; pr.country[0] = "KR"; pr.country[1] = "US";
			pr.code = pr.watch = "KXQ4MD";
			Online::RoomRow qr = pr;
			qr.quick = true; qr.room.clear(); qr.code.clear(); qr.name[0] = "QUICKA"; qr.watch = "KXQ4ME";
			o.set_rooms({ pr, qr }, false);
			again();
			gm.select_row(8);
			pad(SDL_GAMEPAD_BUTTON_NORTH);   // (Y: the room's own page)
			shot("room_view_no_watch.png", w, h);
			check(top() == tr::P_ROOM.c_str() && gm.selected() == 0, "watching off: a match's own page opens on Play next, no Watch");
			gm.select_row(2);
			check(gm.selected() == 0, "Play next and Back only");
			key(SDL_SCANCODE_RETURN);
			check(o.phase() == Online::Phase::joining && !o.watching() && top() == tr::P_ONLINE.c_str(), "and Play next joins its room");
			o.stop();
			again();
			gm.select_row(8);
			key(SDL_SCANCODE_RETURN);
			check(o.phase() == Online::Phase::joining && !o.watching(), "A on a match with watching off: its room joined to play next, not watched");
			o.stop();
			again();
			gm.select_row(9);
			key(SDL_SCANCODE_RETURN);
			check(o.phase() == Online::Phase::off, "A on a Quick match with watching off: nothing started (it says watching's off)");
			const std::string lobby_before = s.lobby;
			s.lobby = "none";   // (no answer from the lobby taking the place of these rooms meanwhile)
			o.set_rooms({}, false, 3);
			again();
			gm.select_row(2);
			key(SDL_SCANCODE_RETURN);   // (Create room)
			shot("create_room_unlisted.png", w, h);
			check(top() == tr::P_CREATE_ROOM.c_str() && gm.selected() == 1 + 4, "Rooms off at the lobby: Create room has no Listed in Rooms (on Create)");
			gm.select_row(2 + 4);
			check(gm.selected() == 1 + 4, "its name, the room's rules and Create only");
			key(SDL_SCANCODE_ESCAPE);
			s.lobby = lobby_before;
			o.set_rooms({});
			again();
		}
		{   // a locked room joined from its own page: its code typed, then one Online page under it, not two
			Online::RoomRow lk;
			lk.locked = true; lk.room = "PRIVATE"; lk.name[0] = "LOCKER"; lk.country[0] = "KR";
			o.set_rooms({ lk });
			again();
			const size_t depth = gm.depth();
			gm.select_row(8);
			pad(SDL_GAMEPAD_BUTTON_NORTH);
			key(SDL_SCANCODE_RETURN);   // (Join: an open room's first)
			check(top() == tr::P_ENTER_CODE.c_str(), "Join on a locked room's page: its code asked for");
			text("KXQ4M7");
			check(top() == tr::P_ONLINE.c_str() && gm.depth() == depth && (o.phase() == Online::Phase::joining || o.in_room2()),
			      ("the code typed: joining, one Online page (" + std::to_string(gm.depth()) + " deep, " + std::to_string(depth) + " before)").c_str());
			o.stop();
			o.set_rooms({});
			again();
		}
		if (!o.watch(s, "QX7KMP", err)) fprintf(stderr, "menutest: watch: %s\n", err.en().c_str());
		again();
		check(o.watching() && o.playing(), "watching: a match on, as the host program sees it (its frames through step)");
		shot("watch_finding.png", w, h);
		o.stop();
		// the lobby down (nobody at its address): each says so in one plain line; the browser its unreachable state
		{
			auto until_off = [&](double ms) { poll_for(ms, [&] { return o.phase() == Online::Phase::off; }); };
			if (!o.enter_code(s, "KXQ4M7", err)) fprintf(stderr, "menutest: enter code: %s\n", err.en().c_str());
			until_off(10000);
			check(o.join_failure() == std::string(tr::ONLINE_DOWN), ("the lobby down: Enter code says so plainly (" + o.join_failure() + ")").c_str());
			o.forget_failure();
			if (!o.find_random(s, err)) fprintf(stderr, "menutest: quick match: %s\n", err.en().c_str());
			until_off(10000);
			again();
			shot("quick_unavailable.png", w, h);
			check(o.join_failure() == std::string(tr::ONLINE_UNAVAILABLE), ("the lobby down: Quick match says Online is unavailable (" + o.join_failure() + ")").c_str());
			o.forget_failure();
			// (as the game keeps it up to date, nothing pressed: keep_up, main.cpp's, made again only when it's changed)
			auto make = [&] { return online_menu::page(o, s, gm, changed); };
			const int v0 = o.rooms_version();
			bool remade = false;
			o.rooms_list(s, true);
			poll_for(15000, [&] { remade = online_menu::keep_up(o, s, gm, make) || remade; return o.rooms_state() == 2 && o.rooms_version() != v0; });
			remade = online_menu::keep_up(o, s, gm, make) || remade;
			shot("browser_unreachable.png", w, h);
			check(o.rooms_state() == 2 && remade, "the lobby down: the browser made again by itself to say it can't reach it (nothing pressed)");
			check(!online_menu::keep_up(o, s, gm, make), "and not again while nothing's changed");
			{   // (a list had, then the lobby gone: kept through one miss, then gone, not shown as if it were there still)
				std::vector<Online::RoomRow> had(2);
				had[0].name[0] = "OLDHOST"; had[0].code = "KXQ4MA";
				had[1].name[0] = "OLDHOST2"; had[1].code = "KXQ4MB";
				o.set_rooms(had);
				online_menu::keep_up(o, s, gm, make);
				size_t after[2] = {};
				bool made[2] = {};
				for (int miss = 0; miss < 2; miss++) {
					const int v = o.rooms_version();
					o.rooms_list(s, true);
					poll_for(5000, [&] { made[miss] = online_menu::keep_up(o, s, gm, make) || made[miss]; return o.rooms_version() != v; });
					made[miss] = online_menu::keep_up(o, s, gm, make) || made[miss];
					after[miss] = o.rooms_list(s).size();
				}
				shot("browser_unreachable_after.png", w, h);
				check(after[0] == 2 && after[1] == 0 && made[0] && made[1] && o.rooms_state() == 2,
				      ("the lobby gone after a list: its rooms kept through one miss, then the browser says it can't reach it (" + std::to_string(after[0]) + ", " + std::to_string(after[1]) + ")").c_str());
			}
			if (!o.watch(s, "QX7KMP", err)) fprintf(stderr, "menutest: watch: %s\n", err.en().c_str());
			until_off(40000);
			check(o.join_failure() == std::string(tr::WATCH_LOBBY_GONE), ("the lobby down: a watcher told once, plainly (" + o.join_failure() + ")").c_str());
			o.forget_failure();
		}
#ifndef _WIN32
		{   // watching, against a lobby of the test's own (what the real one does now and then, each time), on a clock of
			// the test's (minutes in seconds)
			FakeLobby fl;
			std::atomic<int> mode{ 0 }, seens{ 0 };
			const std::string A = "00000000000000a1", B = "00000000000000b2";
			fl.answer = [&](const std::string &line) -> std::pair<int, std::string> {
				const auto seen = [](const std::string &m) {
					return std::pair<int, std::string>{ 200, "{\"watchers\":1,\"keeper\":false,\"match\":\"" + m + "\",\"checkpoint\":-1,\"random\":false,\"live\":true,\"poll_ms\":1000}" };
				};
				if (line.find("/v1/watch/seen") != std::string::npos) return seen(mode == 1 && seens++ > 0 ? B : A);
				if (mode >= 1 && line.find("/v1/watch/start") != std::string::npos && line.find("m=" + A) != std::string::npos) return { 404, "{\"error\":\"no_match\"}" };
				return { 503, "{\"error\":\"x\"}" };   // (its start never had: 0)
			};
			check(fl.open(), "a lobby of the test's own");
			static m3::Model3 board3;
			board3.backup.assign(0x20000, 0);
			// (a watcher of its own each time, on the test's clock: how long it took, in its seconds)
			double t = 0;
			auto watch_until = [&](Online &w3, double most_s, const std::function<bool()> &until) {
				OnlineSettings s3 = s;
				s3.lobby = "http://127.0.0.1:" + std::to_string(fl.port);
				t = 1e6;
				w3.identity = o.identity;
				w3.poll(t);
				tr::line e3;
				if (!w3.watch(s3, "QX7KMP", e3)) fprintf(stderr, "menutest: watch: %s\n", e3.en().c_str());
				for (; t - 1e6 < most_s * 1000 && !until(); t += 250) { w3.poll(t); SDL_Delay(4); }
				return (t - 1e6) / 1000;
			};
			{   // its start never had: after two minutes it says no match started, not "Starting the match" for ever
				mode = 0;
				Online w3(board3);
				const double took = watch_until(w3, 200, [&] { return w3.phase() == Online::Phase::off; });
				printf("watching, its start never had: %s after %.0f s\n", w3.join_failure().c_str(), took);
				check(w3.phase() == Online::Phase::off && w3.join_failure() == tr::fill(tr::WATCH_NONE_STARTED, { { "code", "QX7-KMP" } }) && took > 110 && took < 135,
				      "a watched match whose start never comes: given up after two minutes, said");
			}
			{   // the match watched gone at the lobby ("no_match"), the next in its place: on to the next
				mode = 1;
				seens = 0;
				Online w3(board3);
				const double took = watch_until(w3, 60, [&] { return fl.was_asked("/v1/watch/start?w=QX7KMP&m=" + B); });
				printf("watching, the match replaced by the next: its start asked for after %.0f s\n", took);
				check(fl.was_asked("/v1/watch/start?w=QX7KMP&m=" + B) && w3.watching(), "a watched match replaced by the next before it was over: on to the next one");
			}
			{   // the match watched gone, none after it: said over (the notice), then back to the Online page
				mode = 2;
				Online w3(board3);
				watch_until(w3, 20, [&] { return w3.watch_between(); });
				check(w3.watch_between() && w3.watch_status() == std::string(tr::WATCH_BETWEEN), ("a watched match gone at the lobby: over, waiting for their next (" + w3.watch_status() + ")").c_str());
				const double from = t;
				for (; t - from < 60000 && w3.phase() != Online::Phase::off; t += 250) { w3.poll(t); SDL_Delay(4); }
				const Online::LastMatch &lm = w3.last_match();
				printf("watching, the match gone: back to the Online page after %.0f s more (%s)\n", (t - from) / 1000, lm.why.en().c_str());
				check(w3.phase() == Online::Phase::off && lm.any && lm.watched && lm.why.str() == std::string(tr::WATCH_MATCH_OVER) && t - from > 40000,
				      "and with no next one, its end said once the while for one is up");
			}
		}
#endif
		s.lobby = lobby_was;
		// a watched match over
		Online::LastMatch wm;
		wm.any = wm.watched = true;
		wm.why = tr::WATCH_MATCH_OVER;
		wm.name[0] = "SEOULKICK"; wm.name[1] = "DENVERDAN"; wm.country[0] = "KR"; wm.country[1] = "US";
		wm.wins[0] = 3; wm.wins[1] = 1; wm.seconds = 412; wm.watch = "QX7KMP";
		o.set_last_match(wm);
		again();
		shot("watch_over.png", w, h);
		o.set_last_match({});
		again();
	}

	{
	Online::LastMatch lm;
	Online::LastMatch &m = lm;
	m.any = m.was_host = m.by_code = true;
	m.why = tr::line(tr::OPPONENT_LEFT, { { "name", "AKIRAFAN" } });
	m.opponent = "AKIRAFAN";
	m.code = "FR7W25";
	m.seconds = 760;
	m.ping = 19;
	m.wins[0] = 3;
	m.wins[1] = 2;
	m.country[0] = "AU";
	m.country[1] = "JP";
	o.set_last_match(lm);
	again();
	shot("match_over.png", w, h);
	{   // (the rooms list changing meanwhile: this page left as it is, Back still selected, not Play again)
		gm.select_row(1);
		Online::RoomRow r;
		r.name[0] = "NEWHOST"; r.code = "KXQ4MC";
		o.set_rooms({ r });
		const bool made = online_menu::keep_up(o, s, gm, [&] { return online_menu::page(o, s, gm, changed); });
		check(!made && gm.selected() == 1, "a match over: a new rooms list doesn't make its page again (Back stays selected)");
		o.tell_failure(tr::ONLINE_DOWN);
		again();
		gm.select_row(1);
		o.set_rooms({});
		check(!online_menu::keep_up(o, s, gm, [&] { return online_menu::page(o, s, gm, changed); }) && gm.selected() == 1, "nor a try that failed's (Back stays selected)");
		o.forget_failure();
		again();
	}
	m.wins[0] = m.wins[1] = 0;   // (no match finished: VS in its circle between the names)
	o.set_last_match(lm);
	again();
	shot("match_over_vs.png", w, h);
	m.was_host = false;
	m.lost = m.relay = true;
	m.why = tr::line(tr::OPPONENT_STOPPED, { { "name", "AKIRAFAN" } });
	m.seconds = 195;
	m.ping = 25;
	o.set_last_match(m);
	again();
	shot("match_lost.png", w, h);
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);   // (Back: the choices)
	check(!o.last_match().any && top() == tr::P_ONLINE.c_str(), "Back after a match: the browser");
	pad(SDL_GAMEPAD_BUTTON_DPAD_UP);
	key(SDL_SCANCODE_ESCAPE);   // (B / Esc: back a page, the home screen's)
	check(top() != tr::P_ONLINE.c_str(), "Esc on the choices: back a page");
	}

	gm.close();
	menu_page m{ "", "", {} };
	m.items.push_back({ tr::M_RESUME, tr::M_RESUME_MATCH_HELP, {}, {}, {} });
	online_menu::match_rows(o, s, gm, changed, m.items);
	for (tr::text r : { tr::P_DISPLAY, tr::P_SOUND, tr::P_CONTROLS }) m.items.push_back({ r, "", {}, {}, {} });
	gm.open(m);
	gm.set_title(tr::TITLE_ONLINE_MATCH);
	game_menu::net_cells near;
	near.name = "MRFINSTER"; near.ping = 21; near.delay = 1; near.rollback = 0;
	gm.set_net(near);
	shot("in_match.png", w, h);
	for (int i = 0; i < 3; i++) key(SDL_SCANCODE_DOWN);   // (to Leave the match: the Playing line is passed over)
	key(SDL_SCANCODE_RETURN);
	shot("leave.png", w, h);
	gm.close();
	gm.set_title(tr::TITLE_PAUSED);

	gm.toast(tr::fill(tr::ST_PLAYING, { { "name", "MRFINSTER" } }));
	game_menu::net_cells far;   // (a far one through the relay, Detailed, its name too long)
	far.name = "GUESTPLAYER12"; far.level = 2; far.ping = 412; far.delay = 11; far.rollback = 6; far.relay = true;
	far.detailed = true; far.jitter = 12; far.rb_avg = 3.1; far.ahead = 0.4; far.frame_ms = 9.8;
	gm.set_net(far);
	shot("overlay.png", w, h);
	far.ping = 1234; far.delay = 15; far.ahead = -12.5; far.frame_ms = 17.4; far.name = "G";
	gm.set_net(far);
	shot("net_wide.png", w, h);
	gm.toast(tr::line(tr::PLAYER_COULDNT_JOIN, { { "why", tr::line("Your opponent has v0.1.6-alpha-13-gf9440e8, you have v0.1.7-netplay-test3: you both need the same version.") } }).str());
	shot("toast_long.png", w, h);
	gm.toast("");
	near.interrupted = true;
	gm.set_net(near);
	gm.set_notice(tr::fill(tr::BANNER_WAITING_FOR, { { "name", "AKIRAFAN" } }), tr::fill(tr::NOTICE_DROPPED, { { "n", "7" } }), tr::NOTICE_CARRIES_ON);
	shot("interrupted.png", w, h);
	SDL_Delay(250);
	shot("interrupted_later.png", w, h);   // (its spinner a quarter turn on)
	gm.set_notice("");
	gm.set_net("", 0);
	gm.show_versus({ tr::VS_PLAYER_1_HOST, "MRFINSTER", tr::VS_PLAYER_2, "AKIRAFAN", tr::fill(tr::VS_DIRECT_MS, { { "ms", "19" } }), tr::VS_BOTH_PRESS_START, false, "AU", "JP", "RANDOM", "SECRET: SIBA" });
	for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(20); }   // (its fade in)
	shot("versus.png", w, h);
	// (the longest names, 10 wide letters each, with flags: each kept to its side of the badge)
	gm.show_versus({ tr::VS_PLAYER_1_HOST, "WWWWWWWWWW", tr::VS_PLAYER_2, "AAAAAAAAAA", tr::current() == tr::lang::en ? std::string("Connected through a relay \xC2\xB7 61 ms") : tr::fill(tr::VS_RELAY_MS, { { "ms", "61" } }), tr::VS_BOTH_PRESS_START, true, "AU", "JP" });
	for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(20); }
	shot("versus_long.png", w, h);
	for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(200); }   // (gone after its 3.5 s)
	{   // a note as a match starts (a Rooms 2 room's "This match: ...", the room's banner HERE COMES A NEW CHALLENGER): held
		// back while the versus screen is up, its words not over "Connected directly / Both press Start"; then over the game
		gm.show_versus({ tr::VS_PLAYER_1_HOST, "MRFINSTER", tr::VS_PLAYER_2, "AKIRAFAN", tr::fill(tr::VS_DIRECT_MS, { { "ms", "19" } }), tr::VS_BOTH_PRESS_START, false, "AU", "JP" });
		gm.set_banner(tr::ROOM2_NEW_CHALLENGER, "AKIRAFAN");
		gm.toast(tr::line(tr::THIS_MATCH, { { "terms", tr::line(tr::TERMS_2, { { "a", tr::line(tr::TERM_STAGE_ON) }, { "b", tr::line(tr::TERM_DURAL_OLD_BOTH) } }) } }).str(), 6);
		for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(20); }
		shot("versus_note.png", w, h);
		room2_clear("versus_note.png");
		check(gm.toast_showing(), "a note while the versus screen is up: held, not gone");
		for (int i = 0; i < 20 && gm.versus_showing(); i++) { shot("", w, h); SDL_Delay(200); }
		gm.set_banner("");
		for (int i = 0; i < 3; i++) { shot("", w, h); SDL_Delay(20); }
		shot("versus_note_after.png", w, h);
		room2_clear("versus_note_after.png");
		check(gm.toast_showing() && !gm.versus_showing(), "the note after the versus screen: over the game, its time from then");
		// a notice's card up (Match over, a watched match between two) with a room's banner (GET READY) and a note: the
		// banner under the card, the note held
		gm.set_notice(tr::MATCH_OVER, tr::fill(tr::BANNER_WAITING_FOR, { { "name", "WWWWWWWWWW" } }));
		gm.set_banner(tr::fill(tr::ROOM2_GET_READY_N, { { "n", "3" } }), tr::fill(tr::ROOM2_YOU_PLAY, { { "name", "WWWWWWWWWW" } }) + "   " + tr::fill(tr::ROOM2_NOT_NOW, { { "key", "Esc" } }));
		for (int i = 0; i < 3; i++) { shot("", w, h); SDL_Delay(20); }
		shot("notice_banner_note.png", w, h);
		room2_clear("notice_banner_note.png");
		gm.set_notice("");
		gm.set_banner("");
		gm.toast("");
	}
	gm.set_banner(tr::BANNER_PRESS, "", "OPTIONS");
	shot("press_start.png", w, h);
	gm.set_banner(tr::fill(tr::BANNER_WAITING_FOR, { { "name", "AKIRAFAN" } }), tr::BANNER_TO_PRESS_START);
	shot("waiting_start.png", w, h);
	gm.set_banner(tr::BANNER_PRESS, tr::BANNER_FOR_REMATCH, "OPTIONS");
	shot("rematch.png", w, h);
	gm.set_banner(tr::BANNER_PRESS, tr::BANNER_FOR_REMATCH, "+");   // (a pad's Start as the Deck names it: one sign, in the middle of its box)
	shot("rematch_plus.png", w, h);
	// Rooms 2's banners over the game: the ready check's countdown, you're next, a new challenger, the loser's
	gm.set_banner(tr::fill(tr::ROOM2_GET_READY_N, { { "n", "3" } }), tr::fill(tr::ROOM2_YOU_PLAY, { { "name", "AKIRAFAN" } }) + "   " + tr::fill(tr::ROOM2_NOT_NOW, { { "key", "Esc" } }));
	shot("room2_ready_banner.png", w, h);
	gm.set_banner(tr::ROOM2_NEW_CHALLENGER, "WWWWWWWWWW");
	shot("room2_challenger.png", w, h);
	gm.set_banner(tr::fill(tr::ROOM2_LOST_NEXT, { { "name", "WWWWWWWWWW" } }), tr::ROOM2_TO_LINE);
	shot("room2_lost.png", w, h);
	gm.set_banner(room2_streak_text(12), tr::fill(tr::ROOM2_FORFEIT, { { "name", "WWWWWWWWWW" } }));
	shot("room2_forfeit.png", w, h);
	gm.set_banner("");
	{   // (the connection bar in a match: each cell's words in the middle of its box)
		game_menu::net_cells c;
		c.name = "MRFINSTER"; c.ping = 25; c.delay = 1; c.rollback = 0; c.relay = true;
		gm.set_net(c);
		shot("net_cells.png", w, h);
		c.watchers = 37;   // (a match that may be watched: how many do)
		gm.set_net(c);
		shot("net_watched.png", w, h);
		game_menu::net_cells v;   // (a watcher's: who plays and the score, how far behind, how many watch)
		v.watching = true; v.name = "WATCHING"; v.score = "SEOULKICK  2\xE2\x80\x93" "1  DENVERDAN"; v.watchers = 37;
		gm.set_net(v);
		shot("net_watching.png", w, h);
		gm.show_versus({ tr::VS_PLAYER_1, "SEOULKICK", tr::VS_PLAYER_2, "DENVERDAN", tr::WATCHING_BEHIND, tr::WATCHING_HOW_TO_STOP, false, "KR", "US" });
		for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(20); }
		shot("versus_watching.png", w, h);
		gm.hide_versus();
		gm.set_net("", 0);
	}
	{   // the game's links (links.h): only exactly their form, the code checked letter by letter
		links::Link l;
		check(links::parse("vf3recomp://vf3/join/KXQ4M7", l) && l.game == "vf3" && l.action == "join" && l.code == "KXQ4M7", "a link: vf3recomp://vf3/join/KXQ4M7");
		check(links::parse("VF3RECOMP://vf3tb/watch/kxq-4m7/", l) && l.game == "vf3tb" && l.action == "watch" && l.code == "KXQ4M7", "a link: any case, the code's dash, a / at the end");
		for (const char *bad : { "vf3recomp://vf3/join/KXQ4M", "vf3recomp://vf3/join/KXQ4M70", "vf3recomp://vf3/join/KXQ4M0", "vf3recomp://vf2/join/KXQ4M7",
		                         "vf3recomp://vf3/host/KXQ4M7", "vf3recomp://vf3/join/KXQ4M7?x=1", "vf3recomp://vf3/join/KXQ4M7/x", "vf3recomp:vf3/join/KXQ4M7",
		                         "http://vf3/join/KXQ4M7", "vf3recomp://vf3/join/../../KXQ4M7", "vf3recomp://vf3//join/KXQ4M7", "vf3recomp://vf3/join/KXQ--4M7", "vf3recomp://vf3/join/KX-Q4M7" })
			check(!links::parse(bad, l), (std::string("not a link: ") + bad).c_str());
	}
#if !defined(_WIN32) && !defined(__APPLE__)   // (a Mac's links come from the system to the running app: no pid file)
	{   // two copies on one data folder: the one running first stays the one links are handed to (a link never starts a
		// third); once it's gone, the other takes its place
		namespace fs = std::filesystem;
		const fs::path dir = fs::path(out) / "links-data";
		std::error_code ec;
		fs::create_directories(dir, ec);
		fs::remove(dir / "running.pid", ec);
		fs::remove(dir / "link.txt", ec);
		auto pid_in = [&] { std::ifstream f(dir / "running.pid"); long p = 0; f >> p; return p; };
		const pid_t first = fork();   // (a copy of this program, running)
		if (first == 0) { pause(); _exit(0); }
		{ std::ofstream f(dir / "running.pid"); f << long(first); }
		links::running(dir.string());
		check(pid_in() == long(first), "a second copy on the data folder: the first left as the one links go to");
		check(links::hand_over(dir.string(), "vf3recomp://vf3/join/KXQ4M7"), "a link handed to the first");
		kill(first, SIGKILL);
		waitpid(first, nullptr, 0);
		std::string url;
		links::take(dir.string(), url);
		check(pid_in() == long(getpid()), "the first gone: the second takes its place");
		fs::remove_all(dir, ec);
	}
#endif
	{   // a Rooms 2 room's own page (online_room.h): the table and its streak, the line of 3 in order with this player's
		// place marked; called, the ready check's countdown; the creator's settings
		o.stop();
		gm.close();
		gm.open(root());
		const std::string table = R"("table":{"a":{"name":"AKIRAFAN","country":"JP"},"b":{"name":"LAUCHAN","country":"KR"},"phase":"PLAYING","streak":3,"holder":"a","n":5,"last":null})";
		o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":true},)" + table +
		                 R"(,"line":[{"name":"PAIPAI","country":"US","ready":false},{"name":"MRFINSTER","country":"GB","ready":true},{"name":"JACKYB","country":"FR","ready":true}],"you":{"mid":"x","pos":2,"ready":true},"ready":null,"watch":"KXQ4M7"})");
		gm.push(online_menu::page(o, s, gm, changed));
		check(o.room2_member() && o.room2()->line.size() == 3 && o.room2()->pos == 2, "Rooms 2: a member, 2nd of 3 in the line");
		settle_check("room2_page.png", w, h);
		shot("room2_page.png", w, h);
		check(gm.depth() == 2 && top() == tr::P_ONLINE.c_str(), "Rooms 2: the room's own page is the Online page");
		o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},)" + table +
		                 R"(,"line":[{"name":"MRFINSTER","country":"GB"},{"name":"PAIPAI","country":"US"},{"name":"JACKYB","country":"FR"},{"name":"WWWWWWWWWW","country":"AU"},{"name":"E","country":""},{"name":"F","country":""},{"name":"G","country":""},{"name":"H","country":""},{"name":"I","country":""}],"you":{"mid":"x","pos":1,"misses":0},"ready":{"pos":1,"deadline_ms":12000,"you":true},"watch":"KXQ4M7"})");
		again();
		check(o.room2_ready_left() >= 11 && o.room2_ready_left() <= 12, ("Rooms 2: called: the ready check's seconds (" + std::to_string(o.room2_ready_left()) + ")").c_str());
		shot("room2_called.png", w, h);
		{   // called: the first button READY NOW (seated at once), NOT NOW under it (AWAY), never a READY that turns to AWAY
			check(gm.selected() == 0, "Rooms 2: called: the first button selected");
			key(SDL_SCANCODE_RETURN);
			check(o.room2_ready_left() < 0 && o.room2()->me_ready && o.room2()->pending("ready") && !o.room2()->pending("away"),
			      "Rooms 2: called: the first button is READY NOW (ready said, seated at once), not AWAY");
			o.set_room2_test("");
		}
		{   // the page made again as the room changes keeps the button selected (by its id), wherever it is now; a state that
			// changes nothing shown doesn't make the page again
			const std::string head = R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},)" + table;
			const std::string line3 = R"(,"line":[{"name":"PAIPAI","country":"US","ready":true},{"name":"MRFINSTER","country":"GB","ready":true},{"name":"JACKYB","country":"FR","ready":true}],)";
			o.set_room2_test(head + line3 + R"("you":{"mid":"x","pos":2,"ready":true},"ready":null,"watch":"KXQ4M7"})");
			again();
			key(SDL_SCANCODE_DOWN);
			key(SDL_SCANCODE_DOWN);
			const int was = gm.selected();
			check(was == 2, ("Rooms 2: in the line, LEAVE LINE selected (" + std::to_string(was) + ")").c_str());
			const int v0 = o.room2_version();
			o.set_room2_test(head + line3 + R"("you":{"mid":"x","pos":2,"ready":true},"ready":null,"watch":"KXQ4M7"})");
			check(o.room2_version() == v0, "Rooms 2: the same state again: the page not made again");
			o.set_room2_test(head + R"(,"line":[{"name":"MRFINSTER","country":"GB","ready":true},{"name":"JACKYB","country":"FR","ready":true}],"you":{"mid":"x","pos":1,"ready":true},"ready":{"pos":1,"deadline_ms":3000,"you":true},"watch":"KXQ4M7"})");
			check(o.room2_version() != v0, "Rooms 2: called: the page made again");
			again();
			check(gm.selected() == 3, ("Rooms 2: called, READY NOW and NOT NOW over it: LEAVE LINE still selected (" + std::to_string(gm.selected()) + ")").c_str());
			shot("room2_called_kept.png", w, h);
			room2_clear("room2_called_kept.png");
			key(SDL_SCANCODE_UP);
			key(SDL_SCANCODE_UP);
			key(SDL_SCANCODE_UP);
			check(gm.selected() == 0, "Rooms 2: called: READY NOW at the top");
			o.set_room2_test(head + line3 + R"("you":{"mid":"x","pos":2,"ready":true},"ready":null,"watch":"KXQ4M7"})");
			again();
			check(gm.selected() == 0, "Rooms 2: the call over: READY NOW's place, READY: ON, still selected");
			key(SDL_SCANCODE_DOWN);
			key(SDL_SCANCODE_DOWN);
			key(SDL_SCANCODE_DOWN);   // (LEAVE ROOM)
			o.set_room2_test(head + line3 + R"("you":{"mid":"x","pos":3,"ready":false},"ready":null,"watch":"KXQ4M7"})");
			again();
			check(gm.selected() == 3, "Rooms 2: the line moved: LEAVE ROOM still selected");
			// (a place past the line's end, as a lobby's state might say for a moment: no row past it)
			o.set_room2_test(head + R"(,"line":[{"name":"PAIPAI","country":"US"}],"you":{"mid":"x","pos":30,"ready":true},"ready":null,"watch":"KXQ4M7"})");
			again();
			check(room2_line_rows(1, 30, ROOM2_LINE_ROWS) == std::vector<int>{ 0 } && room2_line_rows(20, 30, ROOM2_LINE_ROWS).back() == -6,
			      "Rooms 2's line: a place past its end, no row past it");
			shot("room2_pos_past.png", w, h);
			o.set_room2_test("");
		}
		{   // the mouse: the room's page made again while the left button is down (the room changed: called meanwhile, READY
			// NOW and SKIP THIS TURN over the row pressed; the table down to one) waits for its release, and the click is the
			// row's that was pressed, by its id, wherever it is now (LEAVE LINE stays LEAVE LINE, never the row now in its
			// place); a row gone by then: no click at all, never a row past the page's end
			const std::string head = R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},)";
			const std::string line3 = R"(,"line":[{"name":"PAIPAI","country":"US","ready":true},{"name":"MRFINSTER","country":"GB","ready":true},{"name":"JACKYB","country":"FR","ready":true}],)";
			const std::string in_line = head + table + line3 + R"("you":{"mid":"x","pos":2,"ready":true},"ready":null,"watch":"KXQ4M7"})";
			const std::string called = head + table + R"(,"line":[{"name":"MRFINSTER","country":"GB","ready":true},{"name":"JACKYB","country":"FR","ready":true}],"you":{"mid":"x","pos":1,"ready":true},"ready":{"pos":1,"deadline_ms":9000,"you":true},"watch":"KXQ4M7"})";
			const std::string alone = head + R"("table":{"a":{"name":"AKIRAFAN","country":"JP"},"b":null,"phase":"SOLO","streak":0,"holder":"a","n":5,"last":null},"line":[{"name":"PAIPAI","country":"US"}],"you":{"mid":"x","pos":-1,"watcher":true,"misses":0},"watchers":1,"ready":null,"watch":"KXQ4M7"})";
			auto mid = [](const game_menu::box &b) { return std::make_pair((b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2); };
			o.set_room2_test(in_line);
			again();
			std::vector<game_menu::box> rows = gm.boxes("row");
			check(rows.size() == 4, ("Rooms 2's mouse: in the line, 4 buttons (" + std::to_string(rows.size()) + ")").c_str());
			if (rows.size() == 4) {
				// LEAVE LINE (the 3rd) pressed; called before it's let go: the page made again only at the release
				auto [x, y] = mid(rows[2]);
				gm.set_test_held(1);
				button(true, x, y);
				o.set_room2_test(called);
				again();
				button(false, x, y);
				gm.set_test_held(-1);
				check(o.room2() && o.room2()->pending("leave_line") && gm.top_name() == tr::P_ONLINE.c_str(),
				      "Rooms 2's mouse: LEAVE LINE pressed, called before the release: LEAVE LINE's click (not WATCH, now in its place)");
			}
			o.set_room2_test("");
			o.set_room2_test(called);
			again();
			rows = gm.boxes("row");
			check(rows.size() == 5, ("Rooms 2's mouse: called, 5 buttons (" + std::to_string(rows.size()) + ")").c_str());
			if (rows.size() == 5) {
				// WATCH (the 3rd) pressed; meanwhile out of the line and the table down to one: WATCH gone, and the page's 2 rows
				// short of the one pressed: no click
				auto [x, y] = mid(rows[2]);
				const int dropped = gm.dropped_clicks();
				gm.set_test_held(1);
				button(true, x, y);
				o.set_room2_test(alone);
				again();
				button(false, x, y);
				gm.set_test_held(-1);
				check(o.room2() && o.room2()->outbox.empty() && gm.top_name() == tr::P_ONLINE.c_str() && o.in_room2() && gm.dropped_clicks() == dropped + 1,
				      "Rooms 2's mouse: WATCH pressed, gone by the release (2 rows left): no click, nothing past the rows");
				// and a click with nothing changed under it takes as ever
				rows = gm.boxes("row");
				if (!rows.empty()) {
					auto [x2, y2] = mid(rows[0]);
					button(true, x2, y2);
					button(false, x2, y2);
					check(o.room2() && o.room2()->pending("join_line"), "Rooms 2's mouse: a click with the page as it was: JOIN THE LINE");
				}
			}
			o.set_room2_test("");
		}
		{   // COPY CODE at the table during a match: the room's page made again (a match's, every half second) between the
			// press and the release: the code copied all the same, and "Copied" kept for its seconds as the page is made again
			o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":true},"table":{"a":{"name":"MRFINSTER","country":"GB"},"b":{"name":"LAUCHAN","country":"KR"},"phase":"PLAYING","streak":0,"holder":"a","n":1,"last":null},"line":[],"you":{"mid":"x","pos":0,"side":"a","ready":true},"watchers":0,"ready":null,"watch":"KXQ4M7"})");
			again();
			std::vector<game_menu::box> rows = gm.boxes("row");
			check(rows.size() == 4, ("Rooms 2: the creator at the table, 4 buttons, COPY CODE the 2nd (" + std::to_string(rows.size()) + ")").c_str());
			if (rows.size() == 4) {
				const float x = (rows[1].x0 + rows[1].x1) / 2, y = (rows[1].y0 + rows[1].y1) / 2;
				auto clip = [] { char *c = SDL_GetClipboardText(); std::string t = c ? c : ""; SDL_free(c); return t; };
				SDL_SetClipboardText("none");
				gm.set_test_held(1);
				button(true, x, y);
				gm.refresh();
				gm.render(false);
				gm.set_test_held(0);
				button(false, x, y);
				gm.set_test_held(-1);
				check(clip() == o.room_code() && !o.room_code().empty(), ("Rooms 2: COPY CODE pressed, the page made again before the release: the code copied (" + clip() + ")").c_str());
				again();
				shot("", w, h);
				check(!gm.boxes("good").empty(), "Rooms 2: \"Copied\" still shown after the page is made again");
			}
			o.set_room2_test("");
		}
		{   // the lobby's states in order: one older than the last applied (a poll's answer sent before it, the table's match
			// number lower) not applied; the lobby's seq when it sends one; a state that says the room's closed ends it
			const std::string head = R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},)";
			auto st = [&](int n, int pos, const std::string &more = "") {
				return head + R"("table":{"a":{"name":"AKIRAFAN","country":"JP"},"b":{"name":"LAUCHAN","country":"KR"},"phase":"PLAYING","streak":1,"holder":"a","n":)" +
				       std::to_string(n) + R"(,"last":null},"line":[{"name":"PAIPAI","country":"US"},{"name":"MRFINSTER","country":"GB"},{"name":"JACKYB","country":"FR"}],"you":{"mid":"x","pos":)" +
				       std::to_string(pos) + R"(,"ready":true},"ready":null,"watch":"KXQ4M7")" + more + "}";
			};
			o.set_room2_test(st(5, 2));
			check(o.room2_test_state(st(6, 1), 1000) && o.room2()->pos == 1, "Rooms 2's states: a newer one applied");
			check(!o.room2_test_state(st(5, 3), 2000) && o.room2()->pos == 1, "Rooms 2's states: an older match number not applied");
			check(!o.room2_test_state(st(6, 3), 500) && o.room2()->pos == 1, "Rooms 2's states: a poll's answer sent before the last applied not applied");
			check(o.room2_test_state(st(6, 2), 1000) && o.room2()->pos == 2, "Rooms 2's states: the same age again applied");
			check(o.room2_test_state(st(7, 1), 100) && o.room2()->pos == 1, "Rooms 2's states: a higher match number wins over an older time");
			check(o.room2_test_state(st(7, 3, R"(,"seq":10)"), 900) && o.room2()->pos == 3 && !o.room2_test_state(st(7, 1, R"(,"seq":9)"), 5000) && o.room2()->pos == 3,
			      "Rooms 2's states: the lobby's seq, when it sends one, decides");
			check(o.room2_test_state(st(8, 2, R"(,"seq":4)"), 5000) && o.room2()->pos == 2 && o.room2_test_state(st(8, 1, R"(,"seq":5)"), 5000) && o.room2()->pos == 1,
			      "Rooms 2's states: a lower seq with a higher match number applied (a lobby's seq that went back: never frozen on an old view)");
			check(!o.room2_test_state(R"({"closed":true})", 6000) && !o.in_room2(), "Rooms 2's states: {closed:true} ends the membership");
			o.set_room2_test("");
			// (this player's own place among the READY ones by its READY as asked, not the line's as the lobby last had it)
			o.set_room2_test(head + R"("table":{"a":{"name":"AKIRAFAN","country":"JP"},"b":{"name":"LAUCHAN","country":"KR"},"phase":"PLAYING","streak":1,"holder":"a","n":3,"last":null},)"
			                 R"("line":[{"name":"PAIPAI","country":"US","ready":false},{"name":"MRFINSTER","country":"GB","ready":false}],"you":{"mid":"x","pos":2,"ready":false},"ready":null,"watch":"KXQ4M7"})");
			check(o.room2()->ready_place() == 0, "Rooms 2: AWAY: no place among the READY ones");
			o.room2_set_ready(true);
			check(o.room2()->me_ready && o.room2()->ready_place() == 1, "Rooms 2: READY asked, the one before AWAY: next to play by its own READY");
			o.set_room2_test("");
		}
		o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},)" + table +
		                 R"(,"line":[{"name":"PAIPAI","country":"US"}],"you":{"mid":"x","pos":-1,"watcher":true,"misses":0},"watchers":1,"ready":null,"watch":"KXQ4M7"})");
		again();
		check(o.room2()->watcher && o.room2()->pos == -1, "Rooms 2: Leave line: watching, no place in the line");
		shot("room2_watcher.png", w, h);
		o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"","listed":false,"locked":false,"ready_ms":15000,"owner":true},"table":{"a":{"name":"MRFINSTER","country":"GB"},"b":null,"phase":"SOLO","streak":0,"holder":"a","n":0,"last":null},"line":[],"you":{"mid":"x","pos":0,"side":"a","misses":0},"ready":null,"watch":"KXQ4M7"})");
		again();
		shot("room2_alone.png", w, h);
		room2_clear("room2_alone.png");
		{   // the line as one long column: which rows a line shows (all that fit; past that the first ones, "... n more",
			// this player's own row and a count after it)
			auto rows = [](int n, int pos) { std::string t; for (int i : room2_line_rows(n, pos, ROOM2_LINE_ROWS)) t += (t.empty() ? "" : ",") + std::to_string(i < 0 ? i : i + 1); return t; };
			check(rows(2, 1) == "1,2" && rows(15, 15) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15", "Rooms 2's line: all of a line that fits");
			check(rows(32, 0) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,-18", ("Rooms 2's line: 32, at the table: the first 14, 18 more (" + rows(32, 0) + ")").c_str());
			check(rows(20, 3) == "1,2,3,4,5,6,7,8,9,10,11,12,13,14,-6", ("Rooms 2's line: 3rd of 20: the first 14 (" + rows(20, 3) + ")").c_str());
			check(rows(20, 17) == "1,2,3,4,5,6,7,8,9,10,11,12,-4,17,-3", ("Rooms 2's line: 17th of 20: the first 12, 4 more, this player's, 3 more (" + rows(20, 17) + ")").c_str());
			check(rows(32, 32) == "1,2,3,4,5,6,7,8,9,10,11,12,13,-18,32", ("Rooms 2's line: last of 32 (" + rows(32, 32) + ")").c_str());
			check(rows(20, 19) == "1,2,3,4,5,6,7,8,9,10,11,12,-6,19,20", ("Rooms 2's line: 19th of 20: the last shown, not \"1 more\" (" + rows(20, 19) + ")").c_str());
			bool all_ok = true;
			for (int fit = ROOM2_LINE_ROWS; fit <= 34 && all_ok; fit++)   // (15 rows at 720 dp to more than a 32-line in a tall window)
				for (int n = 1; n <= 32 && all_ok; n++)
					for (int pos = 0; pos <= n; pos++) {
						const std::vector<int> v = room2_line_rows(n, pos, fit);
						int seen = 0, at = -1;
						bool mine = pos == 0, fits = int(v.size()) <= fit && (n <= fit ? int(v.size()) == n : int(v.size()) == fit), order = true;
						for (int i : v) { seen += i < 0 ? -i : 1; mine = mine || i == pos - 1; fits = fits && i != -1; if (i >= 0) { order = order && i > at; at = i; } }
						if (seen != n || !mine || !fits || !order) {
							std::string t;
							for (int i : v) t += (t.empty() ? "" : ",") + std::to_string(i < 0 ? i : i + 1);
							check(false, ("Rooms 2's line: " + std::to_string(n) + ", at " + std::to_string(pos) + ", " + std::to_string(fit) + " rows: " + t).c_str());
							all_ok = false;
							break;
						}
					}
			check(all_ok, "Rooms 2's line: every length to 32, every place, 15 to 34 rows: each one shown or counted, in order, this player's row shown, the rows all used, no \"1 more\"");
			check(room2_line_fit(720) == 15 && room2_line_fit(800) == 17 && room2_line_fit(960) == 23 && room2_line_fit(1024) == 25,
			      "Rooms 2's line: 15 rows at 720 dp tall, 17 at 800 (16:10), 23 at 960 (4:3), 25 at 1024 (5:4)");
			// the line fills the page's height: as many rows as fit (room2_line_fit), the last one and "n watching" over the
			// help, the help in the page; under the line no more space than the help's place and two rows' (fixed rows
			// left a 4:3 window's bottom third empty)
			auto fills = [&](const char *name, int n) {
				const float dp = std::min(float(h) / 720.0f, float(w) / 1280.0f);
				const std::vector<game_menu::box> rows = gm.boxes("r2row"), watch = gm.boxes("r2watch"), help = gm.boxes("r2help"), right = gm.boxes("r2right");
				const int want = std::min(n, room2_line_fit(gm.height_dp()));
				std::string why;
				if (int(rows.size()) != want) why = std::to_string(rows.size()) + " rows, not " + std::to_string(want);
				else if (right.size() != 1 || help.size() != 1) why = "no line's column or no help";
				else {
					const float bottom = watch.empty() ? rows.back().y1 : watch.back().y1;
					if (bottom > help[0].y0 + 1) why = "the line over its help";
					else if (n > want && help[0].y0 - bottom > 2 * 28 * dp + 60 * dp) why = "space left under the line (" + std::to_string(int((help[0].y0 - bottom) / dp)) + " dp)";
					else if (help[0].y1 > right[0].y1 + 1) why = "the help past the page's bottom";
				}
				check(why.empty(), (std::string(name) + ": the line fills the page, " + std::to_string(rows.size()) + " rows at " + std::to_string(w) + "x" + std::to_string(h) + (why.empty() ? "" : ": " + why)).c_str());
			};
			// the pages: a long line (20, READY and AWAY mixed, this player 17th, below what fits), the longest the lobby
			// has (32, this player at the table, the creator), a short one (1, 2), a watcher with others watching
			const char *names[] = { "SEOULKICK", "DENVERDAN", "PAIPAI", "JACKYB", "WWWWWWWWWW", "SARAHB", "KAGEMARU", "LIONR", "WOLFH", "JEFFRY", "SHUNDI", "LAUCHAN",
			                        "AOI", "TAKAKI", "VANESSA", "BRADBURN", "MRFINSTER", "GOH", "LEIFEI", "ELBLAZE", "JANET", "TAKAYUKI", "ROOKIE", "AKIRA2", "KUMACHAN",
			                        "DURALFAN", "SIBA", "KAGE2", "PAI2", "LAU2", "OHSUMI", "LASTONE" };
			const char *countries[] = { "KR", "US", "HK", "FR", "AU", "GB", "JP", "DE", "CA", "", "CN", "TW", "JP", "JP", "BR", "US", "GB", "JP", "CN", "ES", "US", "JP",
			                            "IT", "PL", "JP", "RU", "TR", "JP", "HK", "CN", "JP", "NZ" };
			auto line = [&](int n, int me, const std::string &you) {
				std::string l = "\"line\":[";
				for (int i = 0; i < n; i++) {
					const std::string nm = i + 1 == me ? "MRFINSTER" : names[i] == std::string("MRFINSTER") ? "TEKKEN" : names[i];
					l += std::string(i ? "," : "") + "{\"name\":\"" + nm + "\",\"country\":\"" + countries[i] + "\",\"ready\":" + ((i + 1 == me ? you.find("\"ready\":false") != std::string::npos : i % 4 == 1 || i % 7 == 5) ? "false" : "true") + "}";
				}
				return l + "]," + you;
			};
			const std::string room = R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},)";
			o.set_room2_test(room + table + "," + line(20, 17, R"("you":{"mid":"x","pos":17,"ready":true},"watchers":4,"ready":null,"watch":"KXQ4M7"})"));
			again();
			check(o.room2()->line.size() == 20 && o.room2()->pos == 17, "Rooms 2: a long line, this player 17th of 20");
			settle_check("room2_line_long.png", w, h);
			shot("room2_line_long.png", w, h);
			room2_clear("room2_line_long.png");
			fills("room2_line_long.png", 20);
			o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"WWWWWWWWWWWWWWWW","listed":true,"locked":false,"ready_ms":15000,"owner":true},"table":{"a":{"name":"MRFINSTER","country":"GB"},"b":{"name":"WWWWWWWWWW","country":"AU"},"phase":"PLAYING","streak":12,"holder":"b","n":40,"last":null},)" +
			                 line(32, 0, R"("you":{"mid":"x","pos":0,"side":"a","ready":true},"watchers":2,"ready":null,"watch":"KXQ4M7"})"));
			again();
			settle_check("room2_line_32.png", w, h);
			shot("room2_line_32.png", w, h);
			room2_clear("room2_line_32.png");
			fills("room2_line_32.png", 32);
			{   // the streak beside its holder's name, short, the longest name: on the name's line, in the card, after the name
				const std::vector<game_menu::box> st = gm.boxes("r2st"), sn = gm.boxes("r2sn"), card = gm.boxes("r2card");
				bool ok = st.size() == 1 && sn.size() == 2 && card.size() == 1;
				if (ok) ok = st[0].x0 >= sn[1].x1 - 1 && st[0].x1 <= card[0].x1 - 1 && st[0].y0 >= sn[1].y0 - 1 && st[0].y1 <= sn[1].y1 + 1;
				check(ok, "Rooms 2's table: the streak (12) beside the longest name, on its line, inside the card");
				// the longest streak (the lobby's 9999 at most) beside the longest name: the streak whole in the card, after the
				// name; a language too long for both, the name cut short ("..."), never under it or over it
				o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},"table":{"a":{"name":"MRFINSTER","country":"GB"},"b":{"name":"WWWWWWWWWW","country":"AU"},"phase":"PLAYING","streak":9999,"holder":"b","n":40,"last":null},)" +
				                 line(3, 0, R"("you":{"mid":"x","pos":0,"side":"a","ready":true},"watchers":0,"ready":null,"watch":"KXQ4M7"})"));
				again();
				shot("room2_streak_long.png", w, h);
				room2_clear("room2_streak_long.png");
				const std::vector<game_menu::box> st2 = gm.boxes("r2st"), sn2 = gm.boxes("r2sn"), card2 = gm.boxes("r2card");
				ok = st2.size() == 1 && sn2.size() == 2 && card2.size() == 1 && st2[0].x0 >= sn2[1].x1 - 1 && st2[0].x1 <= card2[0].x1 - 1 && st2[0].y0 >= sn2[1].y0 - 1 && st2[0].y1 <= sn2[1].y1 + 1;
				check(ok, "Rooms 2's table: the longest streak (9999) beside the longest name, on its line, inside the card");
			}
			// a line of 32, this player 27th, below the fold: the first ones, "... n more", this player's row, "... n more"; at
			// the table the longest name with its 3 WINS beside it
			const std::string long_table = R"("table":{"a":{"name":"AKIRAFAN","country":"JP"},"b":{"name":"WWWWWWWWWW","country":"AU"},"phase":"PLAYING","streak":3,"holder":"b","n":5,"last":null})";
			o.set_room2_test(room + long_table + "," + line(32, 27, R"("you":{"mid":"x","pos":27,"ready":true},"watchers":3,"ready":null,"watch":"KXQ4M7"})"));
			again();
			settle_check("room2_line_32_fold.png", w, h);
			shot("room2_line_32_fold.png", w, h);
			room2_clear("room2_line_32_fold.png");
			fills("room2_line_32_fold.png", 32);
			{
				const std::vector<int> v = room2_line_rows(32, 27, room2_line_fit(gm.height_dp()));
				check(room2_line_fit(gm.height_dp()) >= 32 || (std::find(v.begin(), v.end(), 26) != v.end() && v.back() == -5), "Rooms 2's line: 27th of 32 below the fold: this player's row, then 5 more");
			}
			if (w * 3 <= h * 4) check(true, "Rooms 2's line: (a window 4:3 or taller already: not made taller)");
			else {   // the window made 4:3 with the page open (a corner dragged): the page made again for it, more rows
				const int was = int(gm.boxes("r2row").size());
				SDL_SetWindowSize(win, w, w * 3 / 4);
				for (int i = 0; i < 4; i++) { SDL_PumpEvents(); shot("", w, w * 3 / 4); SDL_Delay(20); }
				const int now = int(gm.boxes("r2row").size()), want = std::min(32, room2_line_fit(gm.height_dp()));
				check(now == want && now > was, ("Rooms 2's line: the window made 4:3 with the page open: " + std::to_string(was) + " rows, then " + std::to_string(now) + " (" + std::to_string(want) + " fit)").c_str());
				room2_clear("room2_line_32_fold (made 4:3)");
				SDL_SetWindowSize(win, w, h);
				for (int i = 0; i < 4; i++) { SDL_PumpEvents(); shot("", w, h); SDL_Delay(20); }
				check(int(gm.boxes("r2row").size()) == was, "Rooms 2's line: and back: as many rows as before");
			}
			o.set_room2_test(room + table + "," + line(24, 3, R"("you":{"mid":"x","pos":3,"ready":false},"watchers":0,"ready":null,"watch":"KXQ4M7"})"));
			again();
			shot("room2_line_top.png", w, h);
			room2_clear("room2_line_top.png");
			o.set_room2_test(room + table + "," + line(1, 1, R"("you":{"mid":"x","pos":1,"ready":true},"watchers":0,"ready":null,"watch":"KXQ4M7"})"));
			again();
			shot("room2_line_short1.png", w, h);
			room2_clear("room2_line_short1.png");
			o.set_room2_test(room + table + "," + line(2, 2, R"("you":{"mid":"x","pos":2,"ready":true},"watchers":1,"ready":{"pos":1,"deadline_ms":3000,"you":false},"watch":"KXQ4M7"})"));
			again();
			shot("room2_line_short2.png", w, h);
			room2_clear("room2_line_short2.png");
			o.set_room2_test(room + table + "," + line(6, 0, R"("you":{"mid":"x","pos":-1,"watcher":true},"watchers":4,"ready":null,"watch":"KXQ4M7"})"));
			again();
			shot("room2_line_watcher.png", w, h);
			room2_clear("room2_line_watcher.png");
			// GET READY (this player next, its 3 s) with the longest status chip
			o.set_room2_test(room + table + "," + line(9, 1, R"("you":{"mid":"x","pos":1,"ready":true},"watchers":0,"ready":{"pos":1,"deadline_ms":3000,"you":true},"watch":"KXQ4M7"})"));
			again();
			shot("room2_line_turn.png", w, h);
			room2_clear("room2_line_turn.png");
			// a note with the page open: in the line's column where its help is, not over the page; the longest note
			o.set_room2_test(room + table + "," + line(20, 17, R"("you":{"mid":"x","pos":17,"ready":true},"watchers":4,"ready":null,"watch":"KXQ4M7"})"));
			again();
			gm.toast(tr::line(tr::THIS_MATCH, { { "terms", tr::line(tr::TERMS_2, { { "a", tr::line(tr::TERM_STAGE_ON) }, { "b", tr::line(tr::TERM_DURAL_OLD_BOTH) } }) } }).str(), 6);
			for (int i = 0; i < 6; i++) { shot("", w, h); SDL_Delay(40); }
			shot("room2_line_note.png", w, h);
			room2_clear("room2_line_note.png");
			gm.toast(tr::line(tr::ROOM2_AWAY_SAID).str(), 6);
			shot("room2_line_note2.png", w, h);
			room2_clear("room2_line_note2.png");
			gm.toast("");
			// a notice's card (Starting the match: a watcher catching up) with the page open: its words in the same place, not
			// a card over the page
			gm.set_notice(tr::NOTICE_STARTING, tr::fill(tr::WATCH_CATCHING_UP, { { "pct", "54" } }));
			for (int i = 0; i < 3; i++) { shot("", w, h); SDL_Delay(20); }
			shot("room2_line_notice.png", w, h);
			room2_clear("room2_line_notice.png");
			gm.set_notice("");
			// entering (the lobby not answered yet)
			o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"FRIDAY FIGHTS","listed":true,"locked":false,"ready_ms":15000,"owner":false},"entering":true})");
			again();
			shot("room2_entering.png", w, h);
			room2_clear("room2_entering.png");
		}
		o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"","listed":false,"locked":false,"ready_ms":15000,"owner":true},"table":{"a":{"name":"MRFINSTER","country":"GB"},"b":null,"phase":"SOLO","streak":0,"holder":"a","n":0,"last":null},"line":[],"you":{"mid":"x","pos":0,"side":"a","misses":0},"ready":null,"watch":"KXQ4M7"})");
		again();
		const int settings_row = 2;   // (READY, COPY CODE, then ROOM SETTINGS; the selection kept from the page before, by id)
		gm.select_row(settings_row);
		key(SDL_SCANCODE_RETURN);
		check(top() == tr::ROOM2_SETTINGS.c_str(), "Rooms 2: the creator's Room settings");
		{   // the creator's settings and rules not taken by the lobby: only the newest of each kind sent again (OwnerAsks)
			OwnerAsks q;
			OwnerAsks::Ask a;
			const int a1 = q.sent("rules"), b1 = q.sent("rules");   // (pressed twice, the lobby unreachable: both fail)
			const bool old_again = q.failed("rules", "A", a1, 1, 0), new_again = q.failed("rules", "B", b1, 1, 0);
			check(!old_again && new_again && q.again.size() == 1 && !q.due(2999, a) && q.due(3000, a) && a.body == "B",
			      "Rooms 2's settings: pressed twice, both failing: only the newer sent again");
			OwnerAsks r;
			const int a2 = r.sent("rules");
			r.sent("rules");   // (B taken by the lobby; A's failure comes after it)
			check(!r.failed("rules", "A", a2, 1, 0) && r.again.empty(), "Rooms 2's settings: A failing after B was taken: never sent again over B");
			OwnerAsks t;
			const int s1 = t.sent("settings"), r1 = t.sent("rules");
			check(t.failed("settings", "S", s1, 1, 0) && t.failed("rules", "R", r1, 1, 0) && t.again.size() == 2, "Rooms 2's settings: settings and rules each their own newest");
			int tries = 1, sends = 1;
			OwnerAsks u;
			int seq = u.sent("rules");
			while (u.failed("rules", "R", seq, tries, 0) && u.due(1e9, a)) { seq = u.sent("rules"); tries = a.tries + 1; sends++; }
			check(sends == OwnerAsks::TRIES, ("Rooms 2's settings: a change that keeps failing sent " + std::to_string(sends) + " times in all, then given up").c_str());
		}

		shot("room2_settings.png", w, h);
		{   // the rows: the room's rules as the lobby has them, not this player's saved ones (a change the lobby didn't take, or
			// a room made with others); no Random and custom fighters row (nothing plays it yet). A change: the room's with that
			// one changed, kept as this player's
			check(gm.row_count() == 7 && gm.row_value(1) == std::string(tr::OFF) && gm.row_value(4) == std::string(tr::ON),
			      "Rooms 2's settings: 7 rows (no Random and custom fighters), the side and the stage as the room's");
			const bool side0 = s.rule_side;
			o.set_room2_test(R"({"code":"KXQ4M7","room":{"name":"","listed":false,"locked":false,"ready_ms":15000,"owner":true,"rules":{"side":true,"dural":true,"custom":false,"max":4,"stage":false}},"table":{"a":{"name":"MRFINSTER","country":"GB"},"b":null,"phase":"SOLO","streak":0,"holder":"a","n":0,"last":null},"line":[],"you":{"mid":"x","pos":0,"side":"a","misses":0},"ready":null,"watch":"KXQ4M7"})");
			gm.refresh();
			check(gm.row_value(1) == std::string(tr::ON) && gm.row_value(2) == std::string(tr::ON) && gm.row_value(3) == "4" && gm.row_value(4) == std::string(tr::OFF) && s.rule_side == side0,
			      "Rooms 2's settings: the room's rules shown (side On, Dural On, 4 players, stage Off), the saved ones left alone");
			gm.select_row(3);
			key(SDL_SCANCODE_RIGHT);
			check(gm.row_value(3) == "5" && s.rule_max == 5 && s.rule_side && s.rule_dural && !s.rule_stage && o.room2()->rule_max == 5,
			      "Rooms 2's settings: Players 4 to 5: the room's others kept, all kept as this player's");
			s.rule_side = false; s.rule_dural = false; s.rule_max = 0; s.rule_stage = true;
		}
		settle_check("room2_settings.png", w, h);
		for (int row : { 0, 1 }) {   // (each row's help, Close the room's the longest)
			gm.select_row(row);
			for (int i = 0; i < 8; i++) { shot("", w, h); SDL_Delay(40); }
			shot(row == 0 ? "room2_settings_listed.png" : "room2_settings_close.png", w, h);
		}
		gm.pop();
		o.set_room2_test("");
		check(!o.in_room2(), "Rooms 2: out of the room");
		gm.close();
	}
	pages(gm, w, h);
	// Game > Language changed with a page open (as main.cpp's set_language: tr::set, then the menu's set_language): the
	// page at once in the other language, its letters' fonts first; and back, exactly the picture it was
	if (tr::current() == tr::lang::en) {
		menu_page root{ "", "", {} };
		for (tr::text t : { tr::M_RESUME, tr::P_DISPLAY, tr::P_SOUND, tr::P_GAME, tr::M_ARCADE, tr::P_ONLINE, tr::M_QUIT }) root.items.push_back({ t, "", {}, {}, {} });
		gm.open(root);
		gm.select_row(3);
		auto drawn = [&](const char *name) { for (int i = 0; i < 8; i++) { shot("", w, h); SDL_Delay(40); } shot(name, w, h); return last_px; };
		const std::vector<uint32_t> en = drawn("switch_en.png");
		std::vector<uint32_t> other;
		for (tr::lang l : { tr::lang::ko, tr::lang::ja, tr::lang::ru, tr::lang::tr }) {
			tr::set(l);
			gm.set_language();
			other = drawn((std::string("switch_") + tr::code(l) + ".png").c_str());
			check(other != en, (std::string("Game > Language: the page drawn in ") + tr::code(l) + " at once").c_str());
			for (const std::string &o : gm.misaligned()) misaligned.push_back(std::string("switch_") + tr::code(l) + ": " + o);
		}
		tr::set(tr::lang::en);
		gm.set_language();
		check(drawn("switch_back.png") == en, "and back in English, the picture it was");
		gm.close();
	}
	// text past its box, anywhere: listed (tools/langcheck.py runs this in every language and fails a language on a
	// box its text overflows where the English doesn't; pseudo, 40% longer than any, finds the tight places)
	for (const std::string &o : overflows) printf("overflow: %s\n", o.c_str());
	printf("%zu lines past their box\n", overflows.size());
	for (const std::string &o : misaligned) check(false, ("a row out of line: " + o).c_str());
	check(misaligned.empty(), "every page's rows start in line");
	// every page of rows whole, none scrolling (a scroll can't be seen: the rows below the panel's foot looked missing)
	for (const std::string &o : unfit) check(false, ("a page that doesn't fit: " + o).c_str());
	check(unfit.empty(), "every page of rows fits whole: none scrolls");
	if (fails) { fprintf(stderr, "menutest: %d failed\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
