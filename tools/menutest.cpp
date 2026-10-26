// menutest: the menu's Online pages and the notices over the game, drawn as the game draws them (RmlUi on SDL's
// renderer) in a window nobody sees, and written as pictures: for looking at them without the ROM or a screen.
//   SDL_VIDEO_DRIVER=offscreen menutest <out folder> [width height]
// Writes the Online screens (home, name and code entry on a keyboard and a pad, hosting and joining by address, and
// with VF_LOBBY set a room's), the match over, the menu in a match, the notices, and the versus screen; and checks
// what they do (where the keys go, what's typed, what each button starts). Exit 1 if any check fails.
#include "../src/host/menu.h"
#include "../src/host/online.h"
#include "../src/host/png.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

SDL_Renderer *ren = nullptr;
game_menu *menu = nullptr;
std::string out;
SDL_Texture *backdrop = nullptr;   // MENUTEST_BACKDROP: a picture of the game instead of the stand-in

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
	if (argc < 2) { fprintf(stderr, "usage: SDL_VIDEO_DRIVER=offscreen [MENUTEST_RENDERER=gpu] menutest <out folder> [width height]\n"); return 2; }
	out = argv[1];
	const int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
	if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "menutest: %s\n", SDL_GetError()); return 1; }
	SDL_Window *win = SDL_CreateWindow("menutest", w, h, 0);
	// (MENUTEST_RENDERER=gpu: as the game draws on a GPU, e.g. Mesa's lavapipe without one; else SDL's software one)
	const char *rd = getenv("MENUTEST_RENDERER");
	ren = win ? SDL_CreateRenderer(win, rd && *rd ? rd : "software") : nullptr;
	if (!ren) { fprintf(stderr, "menutest: %s\n", SDL_GetError()); return 1; }
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
		for (const char *r : { "Resume", "Display", "Sound", "Game", "Arcade settings", "Online", "Controls", "Quit" })
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
	auto poll_for = [&](double ms, std::function<bool()> until) {   // (the Online's work, in real time)
		const uint64_t t0 = SDL_GetTicks();
		while (SDL_GetTicks() - t0 < ms && !until()) { o.poll(double(SDL_GetTicks())); SDL_Delay(10); }
	};
	auto again = [&] { gm.replace(online_menu::page(o, s, gm, changed), -1); };
	gm.push(online_menu::page(o, s, gm, changed));
	shot("home.png", w, h);
	check(gm.selected() == 1, "the home screen opens on Random");
	key(SDL_SCANCODE_RIGHT);
	check(gm.selected() == 2, "Right goes to Host");
	key(SDL_SCANCODE_DOWN);
	check(gm.selected() == 4, "Down goes to Settings");
	key(SDL_SCANCODE_UP);
	key(SDL_SCANCODE_UP);
	check(gm.selected() == 0, "Up from the tiles goes to the name");
	pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);   // (a pad now: its hints)
	shot("home_pad.png", w, h);
	check(gm.pad_mode(), "a pad's button: pad mode");

	// your name, on a pad: Y from the home screen
	pad(SDL_GAMEPAD_BUTTON_NORTH);
	check(top() == "Your name", "Y opens the name");
	for (int i = 0; i < 3; i++) pad(SDL_GAMEPAD_BUTTON_WEST);   // (X takes letters off)
	for (int i = 0; i < 5; i++) pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
	shot("name_pad.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_SOUTH);   // (A: the letter selected, T)
	pad(SDL_GAMEPAD_BUTTON_START);   // (saved)
	check(s.name == "MRFINST", ("the pad typed the name (" + s.name + ")").c_str());
	check(top() == "Online", "Start saves and goes back");
	// on a keyboard
	key(SDL_SCANCODE_UP);
	key(SDL_SCANCODE_RETURN);
	check(top() == "Your name", "Enter on the name opens it");
	key(SDL_SCANCODE_BACKSPACE);
	text("ER");
	shot("name_key.png", w, h);
	key(SDL_SCANCODE_SPACE);   // (types a space: doesn't press anything)
	check(top() == "Your name", "Space types, it doesn't press");
	key(SDL_SCANCODE_RETURN);
	check(s.name == "MRFINSER", ("the keyboard typed the name (" + s.name + ")").c_str());
	s.name = "MRFINSTER";
	again();

	// joining with a code: a keyboard, then a pad
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);
	check(top() == "Join with a code", "Join opens the code");
	text("zz");
	key(SDL_SCANCODE_Z);   // (a letter's key: its letter comes as text, not twice)
	text("z");
	shot("join_key.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	for (int i = 0; i < 7; i++) pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	shot("join_pad.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_EAST);   // (B: back)
	check(top() == "Online", "B goes back from the code");

	// no name yet: asked for first, then on to the code
	s.name.clear();
	again();
	key(SDL_SCANCODE_RIGHT);   // (a keyboard again: Host to Join)
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);
	check(top() == "Your name", "Join with no name asks for it");
	shot("name_first.png", w, h);
	text("AKIRA");
	key(SDL_SCANCODE_RETURN);
	check(s.name == "AKIRA" && top() == "Join with a code", "then goes on to the code");
	key(SDL_SCANCODE_ESCAPE);
	s.name = "MRFINSTER";

	// by address: hosting, then joining a host that isn't there
	std::string err;
	if (!o.host(s, err)) fprintf(stderr, "menutest: hosting: %s\n", err.c_str());
	again();
	shot("address_hosting.png", w, h);
	o.stop();
	s.address = "192.168.1.20";
	if (!o.join(s, err)) fprintf(stderr, "menutest: joining: %s\n", err.c_str());
	o.poll(1000);
	again();
	shot("address_joining.png", w, h);
	for (double t = 1000; t < 20000 && o.phase() == Online::Phase::joining; t += 100) o.poll(t);   // (no host: it gives up)
	again();
	shot("address_failed.png", w, h);
	check(!o.join_failure().empty() && o.last_try() == Online::Try::join, "a join by address that failed: Couldn't connect, to try again");
	key(SDL_SCANCODE_ESCAPE);
	check(o.join_failure().empty() && gm.selected() == 1, "Esc on Couldn't connect: back to the choices");

	// a room code's (the lobby: VF_LOBBY, e.g. tools/lobby's npx wrangler dev)
	if (!Online::lobby_default().empty()) {
		// a random opponent: this one searching, and another (a second game's, here) found
		again();
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
		if (!o2.find_random(s2, err)) fprintf(stderr, "menutest: the other's random: %s\n", err.c_str());
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
		key(SDL_SCANCODE_RIGHT);
		key(SDL_SCANCODE_RETURN);   // (Host)
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
		key(SDL_SCANCODE_RIGHT);
		key(SDL_SCANCODE_RIGHT);
		key(SDL_SCANCODE_RETURN);
		text("zzz222");   // (the 6th letter: it joins)
		check(top() == "Online" && o.phase() == Online::Phase::joining, "the 6th letter joins");
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
	}

	{
	Online::LastMatch lm;
	Online::LastMatch &m = lm;
	m.any = m.was_host = m.by_code = true;
	m.why = "AKIRAFAN left the match.";
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
	m.wins[0] = m.wins[1] = 0;   // (no match finished: VS in its circle between the names)
	o.set_last_match(lm);
	again();
	shot("match_over_vs.png", w, h);
	m.was_host = false;
	m.lost = m.relay = true;
	m.why = "AKIRAFAN's game stopped answering.";
	m.seconds = 195;
	m.ping = 25;
	o.set_last_match(m);
	again();
	shot("match_lost.png", w, h);
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);   // (Back: the choices)
	check(!o.last_match().any && gm.selected() == 1, "Back after a match: the choices");
	pad(SDL_GAMEPAD_BUTTON_DPAD_UP);
	key(SDL_SCANCODE_ESCAPE);   // (B / Esc: back a page, the home screen's)
	check(top() != "Online", "Esc on the choices: back a page");
	}

	gm.close();
	menu_page m{ "", "", {} };
	m.items.push_back({ "Resume", "Back to the match (it goes on while the menu's open).", {}, {}, {} });
	online_menu::match_rows(o, s, gm, changed, m.items);
	for (const char *r : { "Display", "Sound", "Controls" }) m.items.push_back({ r, "", {}, {}, {} });
	gm.open(m);
	gm.set_title("ONLINE MATCH");
	game_menu::net_cells near;
	near.name = "MRFINSTER"; near.ping = 21; near.delay = 1; near.rollback = 0;
	gm.set_net(near);
	shot("in_match.png", w, h);
	for (int i = 0; i < 3; i++) key(SDL_SCANCODE_DOWN);   // (to Leave the match: the Playing line is passed over)
	key(SDL_SCANCODE_RETURN);
	shot("leave.png", w, h);
	gm.close();
	gm.set_title("PAUSED");

	gm.toast("Playing MRFINSTER");
	game_menu::net_cells far;   // (a far one through the relay, Detailed, its name too long)
	far.name = "GUESTPLAYER12"; far.level = 2; far.ping = 412; far.delay = 11; far.rollback = 6; far.relay = true;
	far.detailed = true; far.jitter = 12; far.rb_avg = 3.1; far.ahead = 0.4; far.frame_ms = 9.8;
	gm.set_net(far);
	shot("overlay.png", w, h);
	far.ping = 1234; far.delay = 15; far.ahead = -12.5; far.frame_ms = 17.4; far.name = "G";
	gm.set_net(far);
	shot("net_wide.png", w, h);
	gm.toast("A player couldn't join: Your opponent has v0.1.6-alpha-13-gf9440e8, you have v0.1.7-netplay-test3: you both need the same version.");
	shot("toast_long.png", w, h);
	gm.toast("");
	near.interrupted = true;
	gm.set_net(near);
	gm.set_notice("WAITING FOR AKIRAFAN", "The connection dropped. Holding the match for 7 s...", "It carries on from right here if it comes back");
	shot("interrupted.png", w, h);
	SDL_Delay(250);
	shot("interrupted_later.png", w, h);   // (its spinner a quarter turn on)
	gm.set_notice("");
	gm.set_net("", 0);
	gm.show_versus({ "PLAYER 1 \xC2\xB7 HOST", "MRFINSTER", "PLAYER 2", "AKIRAFAN", "Connected directly \xC2\xB7 19 ms", "Both press Start", false, "AU", "JP", "RANDOM", "SECRET: SIBA" });
	for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(20); }   // (its fade in)
	shot("versus.png", w, h);
	// (the longest names, 10 wide letters each, with flags: each kept to its side of the badge)
	gm.show_versus({ "PLAYER 1 \xC2\xB7 HOST", "WWWWWWWWWW", "PLAYER 2", "AAAAAAAAAA", "Connected through a relay \xC2\xB7 61 ms", "Both press Start", true, "AU", "JP" });
	for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(20); }
	shot("versus_long.png", w, h);
	for (int i = 0; i < 20; i++) { shot("", w, h); SDL_Delay(200); }   // (gone after its 3.5 s)
	gm.set_banner("PRESS", "", "OPTIONS");
	shot("press_start.png", w, h);
	gm.set_banner("WAITING FOR AKIRAFAN", "to press Start");
	shot("waiting_start.png", w, h);
	gm.set_banner("PRESS", "for a rematch", "OPTIONS");
	shot("rematch.png", w, h);
	gm.set_banner("");
	if (fails) { fprintf(stderr, "menutest: %d failed\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
