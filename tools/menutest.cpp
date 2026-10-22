// menutest: the menu's Online pages and the notices over the game, drawn as the game draws them (RmlUi on SDL's
// renderer) in a window nobody sees, and written as pictures: for looking at them without the ROM or a screen.
//   SDL_VIDEO_DRIVER=offscreen menutest <out folder> [width height]
// Writes online.png, online_typing.png, online_no_name.png, hosting.png, join.png, join_typed.png, in_match.png,
// leave.png, overlay.png and interrupted.png.
#include "../src/host/menu.h"
#include "../src/host/online.h"
#include "../src/host/png.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

SDL_Renderer *ren = nullptr;
game_menu *menu = nullptr;
std::string out;

// a stand-in for the game's picture: a dark stage, two energy bars and a timer, so the notices are seen over something
void draw_game(int w, int h)
{
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
	SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr);
	SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : nullptr;
	if (s) SDL_DestroySurface(s);
	if (!c) { fprintf(stderr, "menutest: couldn't read the picture back\n"); return; }
	std::vector<uint32_t> px(size_t(c->w) * size_t(c->h));
	for (int y = 0; y < c->h; y++) memcpy(&px[size_t(y) * size_t(c->w)], static_cast<uint8_t *>(c->pixels) + size_t(y) * size_t(c->pitch), size_t(c->w) * 4);
	const std::string path = out + "/" + name;
	write_png(path.c_str(), px.data(), c->w, c->h);
	SDL_DestroySurface(c);
	printf("%s\n", path.c_str());
}

void key(SDL_Scancode sc)
{
	SDL_Event e{};
	e.type = SDL_EVENT_KEY_DOWN;
	e.key.scancode = sc;
	e.key.down = true;
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

void text(const char *t)
{
	SDL_Event e{};
	e.type = SDL_EVENT_TEXT_INPUT;
	e.text.text = t;
	menu->handle_event(e);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: SDL_VIDEO_DRIVER=offscreen menutest <out folder> [width height]\n"); return 2; }
	out = argv[1];
	const int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
	if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "menutest: %s\n", SDL_GetError()); return 1; }
	SDL_Window *win = SDL_CreateWindow("menutest", w, h, 0);
	ren = win ? SDL_CreateRenderer(win, "software") : nullptr;
	if (!ren) { fprintf(stderr, "menutest: %s\n", SDL_GetError()); return 1; }
	game_menu gm;
	menu = &gm;
	if (!gm.init(win, ren, "assets")) { fprintf(stderr, "menutest: the menu's files aren't in ./assets\n"); return 1; }
	gm.set_footer("v0.1.3-alpha   Oct 21 2026");

	static m3::Model3 board;   // (no ROM: the pages only read the settings and the match's state)
	board.backup.assign(0x20000, 0);
	Online o(board);
	o.identity = { "vf3", "test", 1, "" };
	OnlineSettings s;
	s.name = "MRFINSTER";
	auto changed = [] {};
	auto root = [&] {
		menu_page p{ "", "", {} };
		for (const char *r : { "Resume", "Display", "Sound", "Game", "Arcade settings", "Online", "Controls", "Quit" })
			p.items.push_back({ r, "", {}, {}, {} });
		return p;
	};

	gm.open(root());
	shot("root.png", w, h);
	// a pointer resting over the rows: moving it selects, the rows made again under it (a value's change) don't
	for (int y = h / 4; y < h * 3 / 4 && gm.selected() == 0; y += 4) { mouse(float(w) / 4, float(y)); shot("", w, h); }
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
	gm.push(online_menu::page(o, s, gm, changed));
	shot("online.png", w, h);

	key(SDL_SCANCODE_RETURN);   // (Your name: typing)
	for (int i = 0; i < 9; i++) key(SDL_SCANCODE_BACKSPACE);
	text("Akira");
	shot("online_typing.png", w, h);
	key(SDL_SCANCODE_ESCAPE);

	s.name.clear();
	gm.replace(online_menu::page(o, s, gm, changed), 1);
	key(SDL_SCANCODE_RETURN);   // (Host with no name: asked for it)
	shot("online_no_name.png", w, h);
	key(SDL_SCANCODE_ESCAPE);
	s.name = "MRFINSTER";

	std::string err;
	if (!o.host(s, err)) fprintf(stderr, "menutest: hosting: %s\n", err.c_str());
	gm.replace(online_menu::page(o, s, gm, changed), 6);
	shot("hosting.png", w, h);
	o.stop();

	gm.replace(online_menu::page(o, s, gm, changed), 2);
	key(SDL_SCANCODE_RETURN);   // (Join a match)
	key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_RETURN);   // (Connect with no address)
	shot("join.png", w, h);
	key(SDL_SCANCODE_UP);
	key(SDL_SCANCODE_RETURN);
	text("192.168.1.20");
	key(SDL_SCANCODE_RETURN);
	key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_RETURN);   // (Connect)
	o.poll(1000);
	gm.refresh();
	shot("join_typed.png", w, h);
	o.stop();

	o.set_last_match({ true, true, "RENODECK left the match.", "RENODECK", 194, 19 });
	gm.replace(online_menu::page(o, s, gm, changed), online_menu::first_row(o));
	shot("match_over_host.png", w, h);
	o.set_last_match({ true, false, "Lost the connection to MRFINSTER.", "MRFINSTER", 61, 140 });
	s.address = "192.168.1.20";
	gm.replace(online_menu::page(o, s, gm, changed), online_menu::first_row(o));
	shot("match_over_guest.png", w, h);
	key(SDL_SCANCODE_RETURN);   // (Join MRFINSTER again: the page while joining)
	o.poll(1000);
	gm.replace(online_menu::page(o, s, gm, changed), gm.selected());
	shot("joining.png", w, h);
	o.stop();

	gm.close();
	menu_page m{ "", "", {} };
	m.items.push_back({ "Resume", "Back to the match (it goes on while the menu's open).", {}, {}, {} });
	online_menu::match_rows(o, s, gm, changed, m.items);
	for (const char *r : { "Display", "Sound", "Controls" }) m.items.push_back({ r, "", {}, {}, {} });
	gm.open(m);
	gm.set_title("ONLINE MATCH");
	gm.set_net("MRFINSTER   42 ms   rollback 1", 0);
	shot("in_match.png", w, h);
	for (int i = 0; i < 3; i++) key(SDL_SCANCODE_DOWN);   // (to Leave the match: the Playing line is passed over)
	key(SDL_SCANCODE_RETURN);
	shot("leave.png", w, h);
	gm.close();
	gm.set_title("PAUSED");

	gm.toast("Playing MRFINSTER");
	gm.set_net("MRFINSTER   42 ms   rollback 1", 0);
	shot("overlay.png", w, h);
	gm.toast("");
	gm.set_net("MRFINSTER   connection interrupted", 2);
	gm.set_notice("Connection interrupted", "Waiting for MRFINSTER...  6");
	shot("interrupted.png", w, h);
	return 0;
}
