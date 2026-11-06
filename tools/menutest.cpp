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
#include "../src/host/online_text.h"
#include "../src/host/png.h"
#include <SDL3/SDL.h>
#include <algorithm>
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
	check(gm.selected() == 1, "the Online screen (the room browser) opens on Quick match");
	key(SDL_SCANCODE_RIGHT);
	check(gm.selected() == 2, "Right goes to Create room");
	key(SDL_SCANCODE_DOWN);
	check(gm.selected() >= 7 && gm.selected() <= 14, "Down goes to the filters");
	key(SDL_SCANCODE_UP);
	key(SDL_SCANCODE_UP);
	check(gm.selected() == 0, "Up from the actions goes to the name");
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
	gm.select_row(0);   // (the name: Up from wherever the pad left it)
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
	gm.select_row(1);   // (Quick match: the browser keeps what was selected last)
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);
	check(top() == "Enter code", "Enter code opens the code");
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
	gm.select_row(1);
	key(SDL_SCANCODE_RIGHT);   // (a keyboard again: Quick match to Join by code)
	key(SDL_SCANCODE_RIGHT);
	key(SDL_SCANCODE_RETURN);
	check(top() == "Your name", "Join with no name asks for it");
	shot("name_first.png", w, h);
	text("AKIRA");
	key(SDL_SCANCODE_RETURN);
	check(s.name == "AKIRA" && top() == "Enter code", "then goes on to the code");
	key(SDL_SCANCODE_ESCAPE);
	s.name = "MRFINSTER";

	// Play by address, from the browser's More: its page, then the address typed on a keyboard and a pad
	again();
	gm.select_row(1);
	for (int i = 0; i < 3; i++) key(SDL_SCANCODE_RIGHT);   // (Quick match to More)
	key(SDL_SCANCODE_RETURN);
	check(top() == "More", "More: what's seldom wanted");
	shot("more.png", w, h);
	key(SDL_SCANCODE_RETURN);
	check(top() == "Play by address", "the link opens Play by address");
	shot("address_page.png", w, h);
	key(SDL_SCANCODE_DOWN);
	key(SDL_SCANCODE_RETURN);
	check(top() == "Join by address", "Join by address opens the address");
	text("192.168.1.20");
	shot("address_key.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
	shot("address_pad.png", w, h);
	pad(SDL_GAMEPAD_BUTTON_EAST);   // (B: back to Play by address)
	check(top() == "Play by address", "B goes back from the address");
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
	check(top() == "Settings", "Settings is in More, under Play by address");
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
	s.max_ping = 0;
	key(SDL_SCANCODE_ESCAPE);
	key(SDL_SCANCODE_ESCAPE);   // (More: back to the browser)
	again();

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
	check(o.join_failure().empty() && top() == "Online" && gm.selected() < 8, "Esc on Couldn't connect: back to the browser");

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
		// a public room: in the rooms list (the lobby's), its host by name; Rooms shows it, A on it joins
		again();
		s.room_who = 2;
		const std::string named_was = s.room_name;
		s.room_name = "SH1T ROOM";   // (a name the lobby's word filter won't show: listed as its host's)
		if (!o.host_room(s, err)) fprintf(stderr, "menutest: a public room: %s\n", err.c_str());
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
			if (!o2.watch(s2, pub, err)) fprintf(stderr, "menutest: watch: %s\n", err.c_str());
			for (const uint64_t t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 8000 && o2.phase() != Online::Phase::off;) { o.poll(double(SDL_GetTicks())); o2.poll(double(SDL_GetTicks())); SDL_Delay(10); }
			printf("watching with watching off: %s\n", o2.join_failure().c_str());
			check(o2.phase() == Online::Phase::off && o2.join_failure() == tr::WATCH_OFF, "watching off: a watcher told Watching isn't available right now");
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
			if (!o2.watch(s2, wc, err)) fprintf(stderr, "menutest: watch: %s\n", err.c_str());
			for (const uint64_t t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 8000 && o2.phase() != Online::Phase::off;) { o.poll(double(SDL_GetTicks())); o2.poll(double(SDL_GetTicks())); SDL_Delay(10); }
			printf("watching the 401st: %s\n", o2.join_failure().c_str());
			check(o2.phase() == Online::Phase::off && o2.join_failure() == tr::WATCH_FULL, "the 401st watcher: Too many watching right now");
			check(o.step_now() == Online::Step::waiting, "and the host's room as it was");
			o2.forget_failure();
		}
		o.stop();
		s.room_who = 0;
		// watching a code nobody's match has: a lobby with watching answers it as a room whose match hasn't begun (so
		// watching is no way to find room codes): the game looks for its match (and after 2 minutes says none began);
		// without watching (switched off, an older lobby) it says so at once
		again();
		if (!o.watch(s, "QQQ222", err)) fprintf(stderr, "menutest: watch: %s\n", err.c_str());
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

	// Create room: on Create (unlisted, the code only, as ever), its name filled in, Listed in Rooms a switch
	{
		again();
		const std::string lobby_was = s.lobby;
		s.lobby = "none";   // (no room opened by these pages' presses)
		gm.select_row(2);
		key(SDL_SCANCODE_RETURN);   // (Create room: the name's set)
		check(top() == "Create room" && gm.selected() == 2 && s.room_who == 0, "Create room: on Create, not listed (the code only) as ever");
		shot("create_room.png", w, h);
		gm.select_row(1);
		key(SDL_SCANCODE_RETURN);
		check(s.room_who == 2, "Listed in Rooms: on");
		shot("create_listed.png", w, h);
		key(SDL_SCANCODE_RETURN);
		check(s.room_who == 0, "and off");
		gm.select_row(0);
		key(SDL_SCANCODE_RETURN);
		check(top() == "Room name", "Name: typed");
		for (int i = 0; i < 20; i++) key(SDL_SCANCODE_BACKSPACE);
		text("korea finals");
		shot("room_name.png", w, h);
		key(SDL_SCANCODE_RETURN);
		check(s.room_name == "KOREA FINALS" && top() == "Create room", ("the room's name (" + s.room_name + ")").c_str());
		key(SDL_SCANCODE_ESCAPE);
		check(top() == "Online", "Esc: back to the browser");
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
		check(top() == "Room", "Y: the room's own page");
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
			check(top() == "Room", "Y on a Quick match: its page");
			shot("room_view_quick.png", w, h);
			gm.select_row(1);
			gm.select_row(2);   // (no third: Watch and Back only)
			check(gm.selected() == 1, "a Quick match's page: Watch and Back, no Join");
			key(SDL_SCANCODE_RETURN);
			check(top() == "Online" && o.phase() == Online::Phase::off, "and Back goes back");
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
				check(top() == "Room", "a locked room's page: drawn");
				pad(SDL_GAMEPAD_BUTTON_EAST);
				gm.select_row(8 + r);
				key(SDL_SCANCODE_RETURN);   // (A: join it, or watch it)
				check(o.phase() == Online::Phase::off, "a locked room: nothing started without its code");
				while (top() != "Online") key(SDL_SCANCODE_ESCAPE);
			}
			o.set_rooms({});
			again();
		}
		gm.select_row(1);
		key(SDL_SCANCODE_ESCAPE);
		gm.push(online_menu::page(o, s, gm, changed));
		// watching: a match's code typed; the screen while it's found and caught up with
		s.lobby = "http://127.0.0.1:9";   // (nobody there: it goes on looking)
		if (!o.watch(s, "QX7KMP", err)) fprintf(stderr, "menutest: watch: %s\n", err.c_str());
		again();
		check(o.watching() && o.playing(), "watching: a match on, as the host program sees it (its frames through step)");
		shot("watch_finding.png", w, h);
		o.stop();
		// the lobby down (nobody at its address): each says so in one plain line; the browser its unreachable state
		{
			auto until_off = [&](double ms) { poll_for(ms, [&] { return o.phase() == Online::Phase::off; }); };
			if (!o.enter_code(s, "KXQ4M7", err)) fprintf(stderr, "menutest: enter code: %s\n", err.c_str());
			until_off(10000);
			check(o.join_failure() == tr::ONLINE_DOWN, ("the lobby down: Enter code says so plainly (" + o.join_failure() + ")").c_str());
			o.forget_failure();
			if (!o.find_random(s, err)) fprintf(stderr, "menutest: quick match: %s\n", err.c_str());
			until_off(10000);
			again();
			shot("quick_unavailable.png", w, h);
			check(o.join_failure() == tr::ONLINE_UNAVAILABLE, ("the lobby down: Quick match says Online is unavailable (" + o.join_failure() + ")").c_str());
			o.forget_failure();
			o.rooms_list(s, true);
			poll_for(5000, [&] { o.rooms_list(s); return o.rooms_state() == 2; });
			again();
			shot("browser_unreachable.png", w, h);
			check(o.rooms_state() == 2, "the lobby down: the browser shows it can't reach it");
			if (!o.watch(s, "QX7KMP", err)) fprintf(stderr, "menutest: watch: %s\n", err.c_str());
			until_off(40000);
			check(o.join_failure() == tr::WATCH_LOBBY_GONE, ("the lobby down: a watcher told once, plainly (" + o.join_failure() + ")").c_str());
			o.forget_failure();
		}
		s.lobby = lobby_was;
		// a watched match over
		Online::LastMatch wm;
		wm.any = wm.watched = true;
		wm.why = "The match is over.";
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
	check(!o.last_match().any && top() == "Online", "Back after a match: the browser");
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
	gm.set_banner("PRESS", "for a rematch", "+");   // (a pad's Start as the Deck names it: one sign, in the middle of its box)
	shot("rematch_plus.png", w, h);
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
		gm.show_versus({ "PLAYER 1", "SEOULKICK", "PLAYER 2", "DENVERDAN", "Watching \xC2\xB7 a few seconds behind the players",
		                 "The players never wait for you. Esc or Start: the menu, to stop watching", false, "KR", "US" });
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
	if (fails) { fprintf(stderr, "menutest: %d failed\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
