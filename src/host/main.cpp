// Desktop host: an SDL3 window with VF3's picture in it (widescreen in the fights), keyboard and gamepads for
// both players, pacing at the Model 3's 57.52 Hz (by the sound queue when there is sound, else the clock), smooth
// motion (a picture for each refresh of the screen, up to 120 a second), the in-game menu (as VF1's and VF2's),
// and the board's NVRAM (backup RAM and EEPROM) and the menu's settings kept in a data folder.
//
//   vf3 [vf3.zip] [--arcade] [--fullscreen | --window] [--no-sound] [--volume N] [--music N] [--effects N]
//       [--scale N] [--aspect fit|4:3|16:10|16:9|21:9] [--show-boot] [--interp] [--roms <folder>]
//
// --volume, --music and --effects (0-100) as the menu's Sound page: the overall volume, and the music's and the
// effects' and voices' within it.
//
// The game's PowerPC code is recompiled to native code from the player's ROM as it starts (src/rec, about 0.2
// s); --interp (or VF3_INTERP=1) runs it on the interpreter instead, the reference it matches frame for frame.
//
// The game and the data folder (the saves, settings.ini, Screenshots, log.txt): a zip named on the command line,
// with vf3_data beside it; else the player's own, roms/vf3.zip in their data folder, which is the program's own
// folder in portable mode (a file named portable.txt beside the program or in the folder above, or beside a Mac's .app: nothing is read
// or written anywhere else), else a "roms" folder's beside the program (the Steam Deck package keeps everything
// in its folder), else the source tree's roms/vf3.zip (from the current folder: vf3_data beside it), else the
// system's (~/Library/Application Support, %APPDATA%, ~/.local/share: "Virtua Fighter 3 Recomp"). The player's
// own is asked for the first time: a file picker for MAME's vf3.zip (Revision D), checked, then copied into
// roms/. A ROM folder of the player's own (--roms <folder>, VF3_ROMS, or roms_folder = <folder> in settings.ini,
// in that order) is looked in first for vf3.zip, and never written to unless a set is picked while it has none.
//
// The menu (Esc, or on a pad View tapped, L3 + R3 or the touchpad click) pauses the game and its sound: Display
// (render scale, antialiasing, picture shape, 3D renderer, smooth motion, background and screen filters, FPS,
// fullscreen), Sound (volume, music, effects and voices), Game (PC or arcade play style, reset), Arcade settings
// (the arcade's own, and its service menu), Controls (each player's pad, keys and pad buttons) and Quit. Its
// choices are kept in settings.ini in the data folder ("key = value"), written when the menu closes (and within
// 3 s of a hotkey's change); the command line wins over them.
//
// The default is the PC way of playing: no boot to sit through (it runs unseen, about two seconds), and free play
// (the game's own coin setting FREE PLAY, the arcade's kept in settings.ini's arcade_coins and given back for the
// arcade way; its FREE PLAY notices hidden), so Start starts. --arcade (or Game > Play style: Arcade) shows the
// boot and takes coins (the 5 and 6 keys, or the Coin bindings).
//
// --scale N (1-6) draws the 3D at N times the arcade's 384 lines; the 2D (text, the HUD) is the arcade's,
// enlarged. Until one is chosen, the scale fits the window as it first opens. F5 steps through them.
// The player's settings are kept as set (as VF1's and VF2's): the governor that held smooth motion to 60 and lowered the
// scale when the computer couldn't keep up runs only in tests (VF3_GOVERN).
//
// --aspect sets the picture's shape; F6 steps through them. The default, fit, follows the window's shape (between
// 4:3 and 48:9). Wider than 4:3, the 3D's view opens out to the sides (the same vertical view) where it covers the
// whole screen (the fights); the 2D stays in the middle 4:3.
//
// Keys:  player 1: arrows, Z guard, X punch, C kick, V escape, 1 start
//        player 2: I J K L, B guard, N punch, M kick, comma escape, 2 start (both remapped in Controls)
//        5 / 6 coins, F2 test, 9 service, Esc the menu, P pause, F3 reset, F5 render scale, F6 picture shape,
//        F9 the timings in the title bar, F11 (or F, Alt+Enter) fullscreen, F12 a screenshot (the picture, into Screenshots in the
//        data folder), Tab held: as fast as it goes. In the arcade's test menu Down moves and Start selects.
// Pads:  d-pad or left stick, X guard, A punch, B kick, Y escape, LT guard+punch, RB punch+kick, LB guard+kick,
//        RT all three, Start; Menu (View tapped by default), L3 + R3 or the touchpad click: the menu (View
//        + Start too; View tapped is a coin only when it's set as Coin); View + LB: render scale; View + RB:
//        picture shape.
//
// Testing without a display:
//   VF3_HIDDEN=1         no window on screen, no sound out (the board still runs); never a dialog
//   VF3_INTERP=1         the PowerPC on the interpreter (as --interp)
//   VF3_QUIT_AFTER=N     quit after N game frames, writing build/host_last.png (the frame) and
//                        build/host_window.png (the window as drawn); such runs save nothing
//   VF3_SCRIPT=file      inputs from an input script (src/host/input_script.h), frame by frame from the boot;
//                        its shots go to build/host_<name>.png
//   VF3_WINDOW=WxH       the window's size in points
//   VF3_DATA=dir         the data folder
//   VF3_PACE_LOG=1       the pacing and the pictures' times every 2 s
//   VF3_KEYS=k@F,...     presses key k (an SDL key name) when game frame F is reached; the window 3 frames later
//                        goes to build/host_window_F.png
//   VF3_HOST_ROOM=1      hosts a room code's match at pass 60 (the code in log.txt), as Online > Host a match
//   VF3_JOIN_CODE=XXXXXX joins that room at pass 60, as Online > Join with a code (VF_LOBBY: another lobby)
//   VF3_RANDOM=1         looks for a random opponent at pass 60, as Online > Random
//   VF3_MENU_KEYS=k,...  presses these keys (esc, up, down, left, right, enter, or SDL key names) 6 passes of the
//                        loop apart from pass 30
//   VF3_SHOT_AT=n,...    the window as drawn at these passes of the loop, to build/host_window_at_N.png
//   VF3_ROMS=folder      the player's ROM folder (as --roms)
//   VF3_3D=cpu|gpu|auto  the 3D renderer for this run (as Display > 3D renderer)
//   VF3_GPU3D_NO_PROBE=1 no start-up probe of Direct3D 12 (a quad drawn and read back) before the GPU's check
//   VF3_GPU3D_PROBE_ANY=1 the probe on any driver, not only Direct3D 12
//   VF3_GPU_CHECK_DIR=d  the GPU's start-up check's pictures (gpu.png, cpu.png) into d
//   VF3_WIDE_SKY=0       the stages' 2D sky kept to the middle 4:3 (tests)
//   VF3_SKY_LAG=1        the stages' sky as the game scrolls it (3 frames behind the camera: sky_lead.h)
//   VF3_RECORD=dir       a recording (a trailer's footage): from game frame VF3_RECORD_FROM (default 0; before it as
//                        fast as it goes) to VF3_RECORD_TO, 60 pictures a second by the game's clock, not the
//                        real one (smooth motion's in-betweens), the window's whole picture to dir/video.mp4 (by
//                        ffmpeg) and the sound to dir/sound.wav, in step. Use with VF3_HIDDEN and VF3_WINDOW.
//   VF3_GOVERN=1         the governor on (off for players and tests: the settings as set)
//   VF3_THREADS=N        threads drawing the 3D (default: the cores less 1, 12 at most)
//   VF3_TEST_LOAD=ms:s   each picture ms longer (by the scale's square, from 1x) for the first s seconds after the
//                        start's grace (the governor's tests)
#include "version.h"      // VF3_VERSION, VF3_DATE (made by the Makefile from git)
#include "online.h"
#include "../video/hud_names.h"
#include "flags.h"
#include "../machine/model3.h"
#include "../machine/romset.h"
#include "../rec/ppc_rec.h"
#include "../sound/soundboard.h"
#include "../video/tilegen.h"
#include "../video/tile_worker.h"
#include "../video/sky_lead.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_raster.h"
#include "../video/parallel.h"
#include "png.h"
#include "input_script.h"
#include "menu.h"
#include "controls.h"
#include "sysinfo.h"
#include "d3d11_map.h"
#include "../gpu/r3d_gpu.h"
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <SDL3/SDL_main.h>   // WinMain, and argv as UTF-8
#endif
#include <algorithm>
#include <utility>
#include <cctype>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif
#include <vector>
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

// Every message box goes through here: a test run (VF3_HIDDEN) never shows one (nor a file picker); the message
// goes to stderr (log.txt without a console) instead.
static bool message_box(SDL_MessageBoxFlags flags, const char *title, const std::string &msg)
{
	if (getenv("VF3_HIDDEN")) { fprintf(stderr, "(hidden: no dialog) %s\n", msg.c_str()); return false; }
	return SDL_ShowSimpleMessageBox(flags, title, msg.c_str(), nullptr);
}

namespace {

constexpr double FRAME_HZ = 57.524160;       // the Model 3's frame rate (the reference's timing)
constexpr int SOUND_RATE = 44100;
constexpr float LEVEL = 4.0f;                 // --volume 100: the board's output raised 12 dB (it peaks well under full scale), limited
constexpr int MAX_SCALE = 6;
constexpr int BOOT_FRAMES = 960;              // the boot, up to the SEGA logo (the PC way: unseen)

std::string find_assets()
{
	const char *base = SDL_GetBasePath();
	const std::string b = base ? base : "";
	for (const std::string &d : { b + "assets", b + "../../assets", std::string("assets") })
		if (FILE *f = fopen((d + "/ui/menu.rml").c_str(), "rb")) { fclose(f); return d; }
	return "assets";
}

// A folder as the player writes it: "~" (or "~/...") is their home folder; quotes around it are dropped; a
// relative one is taken from `base` (settings.ini's folder) or the current folder.
std::string player_folder(std::string f, const std::string &base = std::string())
{
	while (!f.empty() && (f.back() == ' ' || f.back() == '\t')) f.pop_back();
	if (f.size() >= 2 && (f[0] == '"' || f[0] == '\'') && f.back() == f[0]) f = f.substr(1, f.size() - 2);
	if (f.empty()) return f;
	if (f[0] == '~' && (f.size() == 1 || f[1] == '/' || f[1] == '\\')) {
		const char *h = getenv("HOME");
		if (!h || !*h) h = getenv("USERPROFILE");
		if (h && *h) f = std::string(h) + f.substr(1);
	}
	std::filesystem::path p(f);
	std::error_code ec;
	if (p.is_relative()) p = base.empty() ? std::filesystem::absolute(p, ec) : std::filesystem::path(base) / p;
	p = p.lexically_normal();
	if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();
	return p.string();
}

// The program's folder, with a separator at the end: a Mac app's is the folder the .app is in
std::string program_folder()
{
	const char *base = SDL_GetBasePath();
	std::string b = base ? base : "";
	const std::string in_app = ".app/Contents/Resources/";
	if (b.size() > in_app.size() && b.compare(b.size() - in_app.size(), in_app.size(), in_app) == 0) {
		b.resize(b.size() - in_app.size());
		b.resize(b.find_last_of('/') + 1);
	}
	return b;
}

// Portable mode, as the player asks for it: a file named portable.txt beside the program, or in the folder above it
// (its name as found, else null). Any contents; "portable" and Windows' portable.txt.txt (a New Text Document renamed
// with its extension hidden) count too. The folder above: the download unzips into a folder of its own, and a
// portable.txt put in the folder it was unzipped to went unseen (VF2, a player's report). Looked for with SDL (UTF-8
// names, as on a drive whose folders have accents), not std::filesystem (Windows' code page there). As VF2's.
const char *portable_marker(const std::string &folder)
{
	if (folder.empty()) return nullptr;
	std::string up = folder;
	while (!up.empty() && (up.back() == '/' || up.back() == '\\')) up.pop_back();
	const size_t cut = up.find_last_of("/\\");
	up = cut == std::string::npos ? std::string() : up.substr(0, cut + 1);
	static const char *const NAMES[] = { "portable.txt", "portable", "portable.txt.txt" };
	static const char *const BESIDE[] = { "portable.txt beside the program", "portable beside the program", "portable.txt.txt beside the program" };
	static const char *const ABOVE[] = { "portable.txt in the folder above", "portable in the folder above", "portable.txt.txt in the folder above" };
	for (int above = 0; above < 2; above++) {
		const std::string &dir = above ? up : folder;
		if (dir.empty()) continue;
		for (int k = 0; k < 3; k++) {
			SDL_PathInfo info;
			if (SDL_GetPathInfo((dir + NAMES[k]).c_str(), &info) && info.type == SDL_PATHTYPE_FILE) return above ? ABOVE[k] : BESIDE[k];
		}
	}
	return nullptr;
}

const float ASPECTS[] = { 0.0f, 4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 64.0f / 27.0f };
const char *const ASPECT_NAMES[] = { "fit", "4:3", "16:10", "16:9", "21:9" };
constexpr int N_ASPECTS = 5;

bool g_pc_mode = true;

struct pad_state
{
	bool back_was = false, back_combo = false, start_was = false;
	bool menu_was = false, touch_was = false, sticks_was = false;   // the menu's other buttons (edges)
	int coin_frames = 0, start_seq = -1;
};

enum class hotkey { none, menu, scale, aspect };

// The cabinet's own buttons (the players' are in controls.h, set in the menu)
struct sys_key { SDL_Scancode key; int what; };
enum { SK_COIN1, SK_COIN2, SK_TEST, SK_SERVICE };
const sys_key SYS_KEYS[] = {
	{ SDL_SCANCODE_5, SK_COIN1 }, { SDL_SCANCODE_6, SK_COIN2 }, { SDL_SCANCODE_F2, SK_TEST }, { SDL_SCANCODE_9, SK_SERVICE },
};

// The controls for the next frame, from the keyboard and the pads, and the hotkeys pressed on a pad. The PC way's
// Start: two coins (a credit, as the game is set), then Start.
hotkey read_inputs(m3::Controls &c, SDL_Gamepad *pads[2], pad_state ps[2], const player_controls ctl[2])
{
	c = m3::Controls();
	const bool *ks = SDL_GetKeyboardState(nullptr);
	for (const auto &k : SYS_KEYS)
		if (ks[k.key]) {
			if (k.what == SK_COIN1) c.coin[0] = true;
			if (k.what == SK_COIN2) c.coin[1] = true;
			if (k.what == SK_TEST) c.test[0] = true;
			if (k.what == SK_SERVICE) c.service[0] = true;
		}
	auto key = [&](SDL_Scancode k) { return k != SDL_SCANCODE_UNKNOWN && ks[k]; };
	hotkey hk = hotkey::none;
	static bool key_start_was[2], key_coin_was[2], pad_coin_was[2], key_menu_was[2];
	for (int p = 0; p < 2; p++) {
		pad_state &st = ps[p];
		bool up = false, down = false, left = false, right = false, g = false, pu = false, k = false, e = false;
		auto act = [&](int a) {
			switch (a) {
			case C_UP: up = true; break;
			case C_DOWN: down = true; break;
			case C_LEFT: left = true; break;
			case C_RIGHT: right = true; break;
			case C_GUARD: g = true; break;
			case C_PUNCH: pu = true; break;
			case C_KICK: k = true; break;
			case C_ESCAPE: e = true; break;
			case C_GUARD_PUNCH: g = pu = true; break;
			case C_PUNCH_KICK: pu = k = true; break;
			case C_GUARD_KICK: g = k = true; break;
			case C_ALL_THREE: g = pu = k = true; break;
			default: break;
			}
		};
		auto press_start = [&] {
			st.start_seq = 0;
		};
		for (int a = 0; a < C_START; a++)
			if (key(ctl[p].key[a])) act(a);
		const bool kstart = key(ctl[p].key[C_START]);
		bool start_held = kstart;   // (held, as the cabinet's: a character chosen with Start held is in the 2P outfit)
		if (kstart && !key_start_was[p]) press_start();
		key_start_was[p] = kstart;
		const bool kcoin = key(ctl[p].key[C_COIN]);
		if (kcoin && !key_coin_was[p] && !g_pc_mode) st.coin_frames = 6;
		key_coin_was[p] = kcoin;
		// a key set as Menu opens it too (Esc always does)
		const bool kmenu = key(ctl[p].key[C_MENU]);
		if (kmenu && !key_menu_was[p]) hk = hotkey::menu;
		key_menu_was[p] = kmenu;
		if (SDL_Gamepad *gp = pads[p]) {
			auto btn = [&](SDL_GamepadButton x) { return SDL_GetGamepadButton(gp, x); };
			const int lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
			const int dead = 12000;
			const bool back = btn(SDL_GAMEPAD_BUTTON_BACK);
			const bool start = player_controls::pad_down(gp, ctl[p].pad[C_START]);
			// Coin and Menu on View are its tap (below), so View + LB and the like stay hotkeys
			const int coin_code = ctl[p].pad[C_COIN], menu_code = ctl[p].pad[C_MENU];
			const bool pcoin = coin_code != SDL_GAMEPAD_BUTTON_BACK && player_controls::pad_down(gp, coin_code);
			if (pcoin && !pad_coin_was[p] && !g_pc_mode) st.coin_frames = 6;
			pad_coin_was[p] = pcoin;
			const bool pmenu = menu_code != SDL_GAMEPAD_BUTTON_BACK && player_controls::pad_down(gp, menu_code);
			if (pmenu && !st.menu_was && !back) hk = hotkey::menu;   // (View held: LB and RB are its hotkeys)
			st.menu_was = pmenu;
			// L3 + R3 and the touchpad click (a PlayStation pad's) always open the menu as well, so a
			// pad without a View button, or with Menu moved, can't be locked out of it
			const bool sticks = btn(SDL_GAMEPAD_BUTTON_LEFT_STICK) && btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
			if (sticks && !st.sticks_was) hk = hotkey::menu;
			st.sticks_was = sticks;
			bool touch_bound = false;
			for (int a = C_GUARD; a < C_COUNT; a++) touch_bound |= ctl[p].pad[a] == SDL_GAMEPAD_BUTTON_TOUCHPAD;
			const bool touch = !touch_bound && btn(SDL_GAMEPAD_BUTTON_TOUCHPAD);
			if (touch && !st.touch_was) hk = hotkey::menu;
			st.touch_was = touch;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead) up = true;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead) down = true;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead) left = true;
			if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead) right = true;
			if (back) {
				const bool pstart = btn(SDL_GAMEPAD_BUTTON_START);
				const bool lb = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), rb = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
				if (pstart && !st.start_was) { hk = hotkey::menu; st.back_combo = true; }
				static bool lb_was[2], rb_was[2];
				if (lb && !lb_was[p]) { hk = hotkey::scale; st.back_combo = true; }
				if (rb && !rb_was[p]) { hk = hotkey::aspect; st.back_combo = true; }
				lb_was[p] = lb; rb_was[p] = rb;
			} else {
				for (int a = C_GUARD; a < C_START; a++)
					if (player_controls::pad_down(gp, ctl[p].pad[a])) act(a);
				if (start && !st.start_was) press_start();
				start_held |= start;
			}
			// View tapped on its own: whichever of Menu and Coin it's set as
			if (!back && st.back_was && !st.back_combo) {
				if (menu_code == SDL_GAMEPAD_BUTTON_BACK) hk = hotkey::menu;
				else if (coin_code == SDL_GAMEPAD_BUTTON_BACK && !g_pc_mode) st.coin_frames = 6;
			}
			if (!back) st.back_combo = false;
			st.back_was = back;
			st.start_was = back ? btn(SDL_GAMEPAD_BUTTON_START) : start;
		}
		c.up[p] = up; c.down[p] = down; c.left[p] = left; c.right[p] = right;
		c.guard[p] = g; c.punch[p] = pu; c.kick[p] = k; c.escape[p] = e;
		if (st.coin_frames > 0) { c.coin[p] = true; st.coin_frames--; }
		// Start, held 6 frames
		if (st.start_seq >= 0) {
			const int s = st.start_seq++;
			if (s < 6) c.start[p] = true;
			else st.start_seq = -1;
		}
		if (start_held) c.start[p] = true;
	}
	return hk;
}

// A save is used only if it's the right size.
bool load_blob(const std::string &path, void *dst, size_t n)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	std::vector<uint8_t> buf(n + 1);
	const size_t got = fread(buf.data(), 1, n + 1, f);
	fclose(f);
	if (got != n) return false;
	memcpy(dst, buf.data(), n);
	return true;
}

bool flush_to_disk(FILE *f)
{
	if (fflush(f) != 0) return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

bool replace_file(const std::string &path, const std::string &tmp)
{
	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
	if (ec) { std::filesystem::remove(tmp, ec); return false; }
	return true;
}

// Written beside the old file, flushed to the disk, then swapped in: a crash mid-save leaves the previous one whole.
bool save_blob(const std::string &path, const void *src, size_t n)
{
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	const bool ok = fwrite(src, 1, n, f) == n && flush_to_disk(f);
	if (fclose(f) != 0 || !ok) { std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
	return replace_file(path, tmp);
}

// Saves written off the game's thread (as VF1's and VF2's): each flushed to the disk took tens of milliseconds on a
// hard disk, a hitch every few seconds while the arcade's bookkeeping kept changing the backup RAM in play. put copies
// the bytes and returns; the writer thread writes the newest copy of each file; finish writes what's left and stops
// (a put after it is dropped: the caller writes the final copies itself).
struct bg_saver
{
	std::mutex m;
	std::condition_variable cv;
	std::map<std::string, std::vector<uint8_t>> pending;
	bool stop = false;
	std::thread writer{ [this] { run(); } };   // (last: started once the rest is made)

	void put(const std::string &path, const void *src, size_t n)
	{
		std::vector<uint8_t> bytes(static_cast<const uint8_t *>(src), static_cast<const uint8_t *>(src) + n);
		{
			std::lock_guard<std::mutex> lk(m);
			if (stop) return;
			pending[path] = std::move(bytes);
		}
		cv.notify_one();
	}
	void run()
	{
		for (;;) {
			std::map<std::string, std::vector<uint8_t>> now;
			{
				std::unique_lock<std::mutex> lk(m);
				cv.wait(lk, [&] { return stop || !pending.empty(); });
				if (pending.empty() && stop) return;
				now.swap(pending);
			}
			for (auto &[path, bytes] : now)
				if (!save_blob(path, bytes.data(), bytes.size())) fprintf(stderr, "couldn't save %s\n", path.c_str());
		}
	}
	void finish()
	{
		{
			std::lock_guard<std::mutex> lk(m);
			stop = true;
		}
		cv.notify_one();
		if (writer.joinable()) writer.join();
	}
	~bg_saver() { finish(); }
};

uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull)
{
	for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t *>(p)[i]) * 1099511628211ull;
	return h;
}

// The menu's choices and the controls, kept in settings.ini in the data folder ("key = value").
// scale 0: none chosen yet (the one that fits the window).
struct settings
{
	static constexpr int VERSION = 3;
	int version = VERSION;
	bool upgraded = false;   // (an upgrade made as it was read: saved at once)
	int scale = 0, aspect = 0, volume = 100, renderer3d = 2;   // renderer3d: 0 the CPU, 1 the GPU, 2 Auto
	int music = 100, effects = 100;   // within the volume
	int arcade_coin = -2;   // the arcade way's coin setting, while the PC way's FREE PLAY replaces it (-1 none to give back, -2 none kept yet)
	std::string roms_folder;
	static constexpr const char *BUILD = VF3_VERSION " " VF3_DATE " " VF3_COMMIT;   // (what gpu3d_failed was learnt by)
	std::string gpu3d_failed;   // SDL_GPU's drivers whose 3D failed here ("direct3d12 vulkan"): the next start tries the next
	bool fullscreen = false, arcade = false, show_fps = false, smooth = true;
	bool timings = false;    // F9: the timings in the title bar
	bool smooth_bg = true;   // the 2D backgrounds (the layers below the 3D) scaled smoothly, not as blocks
	bool sharp_textures = true;   // the 3D's textures' mipmap levels for the picture's size (false: the arcade's, video::Raster::arcade_lod)
	int filter = 0;          // the screen filter: 0 off, 1 scanlines, 2 CRT
	OnlineSettings online;   // the Online page's
	int crt_colours = 0;     // an arcade monitor's colours: 0 off, 1-5 as Supermodel's (CRT_COLOURS)
	int graphics = 0;        // Windows: the GPU renderer's driver, 0 Auto (Vulkan first, Direct3D 12 if it fails), 1 Direct3D 12 first
	int aa = 1;              // antialiasing: the 3D drawn aa times larger each way and averaged (1 off)
	player_controls ctl[2] = { player_controls::defaults(0), player_controls::defaults(1) };

	void load(const std::string &path)
	{
		FILE *f = fopen(path.c_str(), "r");
		if (!f) return;
		std::map<std::string, std::string> kv;
		char line[1100], key[64], val[1024];
		while (fgets(line, sizeof line, f))
			if (sscanf(line, " %63[a-z0-9_] = %1023[^\r\n]", key, val) == 2) kv[key] = val;
		fclose(f);
		auto num = [&](const char *k, int &v, int lo, int hi) {
			auto it = kv.find(k);
			if (it != kv.end()) v = std::clamp(atoi(it->second.c_str()), lo, hi);
		};
		auto flag = [&](const char *k, bool &v) { int i = v; num(k, i, 0, 1); v = i; };
		num("scale", scale, 0, MAX_SCALE);
		num("picture_shape", aspect, 0, N_ASPECTS - 1);
		num("volume", volume, 0, 100);
		num("music_volume", music, 0, 100);
		num("effects_volume", effects, 0, 100);
		num("arcade_coins", arcade_coin, -2, m3::Model3::COIN_FREE_PLAY - 1);
		num("renderer_3d", renderer3d, 0, 2);
		if (auto it = kv.find("roms_folder"); it != kv.end()) roms_folder = it->second;
		if (auto it = kv.find("online_name"); it != kv.end()) online.name = online_menu::clean_name(it->second);
		if (auto it = kv.find("online_address"); it != kv.end()) online.address = online_menu::clean_address(it->second);
		if (auto it = kv.find("online_lobby"); it != kv.end() && (it->second.rfind("https://", 0) == 0 || it->second.rfind("http://", 0) == 0))
			online.lobby = it->second.substr(0, 200);   // (another lobby than the built-in one: tests, a server of one's own)
		num("online_delay", online.delay, -1, 10);
		num("online_port", online.port, 1024, 65535);
		num("online_connection", online.show_connection, 0, 2);
		flag("online_hide_ip", online.hide_ip);
		if (auto it = kv.find("gpu_3d_failed"); it != kv.end()) gpu3d_failed = it->second;
		std::string failed_build;
		if (auto it = kv.find("gpu_3d_failed_build"); it != kv.end()) failed_build = it->second;
		flag("fullscreen", fullscreen);
		flag("arcade_mode", arcade);
		flag("frame_timings", timings);
		flag("show_fps", show_fps);
		flag("smooth_motion", smooth);
		flag("smooth_backgrounds", smooth_bg);
		flag("background_filter", smooth_bg);   // (VF1's and VF2's name)
		flag("sharp_textures", sharp_textures);
		num("screen_filter", filter, 0, 2);
		num("crt_colours", crt_colours, 0, 5);
		num("graphics", graphics, 0, 1);
		num("antialiasing", aa, 0, 3);
		if (aa == 0) aa = 1;   // (VF2's Low, edges smoothed after drawing: not in this game's renderer)
		for (int p = 0; p < 2; p++) ctl[p].load(kv, p);
		// Upgrades, by the version the settings were written with (saved at once when one is made)
		version = 0;
		num("settings_version", version, 0, 1000);
		// 1, before the Menu button: Arcade mode's View tapped was a coin. It stays one (as Coin, so the menu is
		// L3 + R3, the touchpad, View + Start or Esc), so a cabinet's coin button keeps working after the update
		if (arcade && !kv.count("p1_pad_menu"))
			for (auto &c : ctl)
				if (c.pad[C_COIN] == PAD_NONE && c.pad[C_MENU] == SDL_GAMEPAD_BUTTON_BACK) {
					c.pad[C_COIN] = SDL_GAMEPAD_BUTTON_BACK;
					c.pad[C_MENU] = PAD_NONE;
					upgraded = true;
					fprintf(stderr, "settings: View tapped stays the arcade way's coin (the menu: L3 + R3, the touchpad, View + Start or Esc)\n");
				}
		// 3, after the release review (as VF2's): a GPU failure remembered by an older build forgotten once, so the
		// next start tries the GPU again (the failures it remembers now are only its start-up's)
		if (version < 3 && !gpu3d_failed.empty()) {
			fprintf(stderr, "settings: the GPU's failures remembered by an older build forgotten (%s): the GPU tried again\n", gpu3d_failed.c_str());
			gpu3d_failed.clear();
			upgraded = true;
		}
		// A GPU failure remembered by another build (another version, or the same rebuilt) forgotten, so each new build
		// tries the GPU again: a driver updated since, or a fix here. (Remembered for ever, one bad start had kept a
		// Windows player's 3D on the CPU at every start after, its cost growing with the scale: slow above 1x.)
		if (!gpu3d_failed.empty() && failed_build != BUILD) {
			fprintf(stderr, "settings: the GPU's failures remembered by %s forgotten (%s): this build tries the GPU again\n",
			        failed_build.empty() ? "an older build" : failed_build.c_str(), gpu3d_failed.c_str());
			gpu3d_failed.clear();
			upgraded = true;
		}
		if (version < VERSION && !kv.empty()) upgraded = true;
		version = VERSION;
	}

	// false if it couldn't be written (said once in the log; the caller tries again later)
	bool save(const std::string &path) const
	{
		const std::string tmp = path + ".tmp";
		std::error_code dec;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), dec);   // (not made yet: a zip on the command line)
		FILE *f = fopen(tmp.c_str(), "w");
		static bool said = false;
		if (!f) {
			if (!said) fprintf(stderr, "couldn't save the settings in %s\n", path.c_str());
			said = true;
			return false;
		}
		fprintf(f, "# Virtua Fighter 3 settings (the in-game menu writes these)\n");
		fprintf(f, "settings_version = %d\n", VERSION);
		fprintf(f, "scale = %d\npicture_shape = %d\nfullscreen = %d\nvolume = %d\narcade_mode = %d\nshow_fps = %d\n"
				"smooth_motion = %d\nrenderer_3d = %d\nbackground_filter = %d\n", scale, aspect, int(fullscreen), volume, int(arcade), int(show_fps),
				int(smooth), renderer3d, int(smooth_bg));
		if (!roms_folder.empty()) fprintf(f, "roms_folder = %s\n", roms_folder.c_str());
		if (!gpu3d_failed.empty()) fprintf(f, "gpu_3d_failed = %s\ngpu_3d_failed_build = %s\n", gpu3d_failed.c_str(), BUILD);
		fprintf(f, "crt_colours = %d\nsharp_textures = %d\n", crt_colours, int(sharp_textures));
		if (graphics) fprintf(f, "graphics = %d\n", graphics);
		fprintf(f, "screen_filter = %d\nantialiasing = %d\nmusic_volume = %d\neffects_volume = %d\narcade_coins = %d\nframe_timings = %d\n", filter, aa, music, effects, arcade_coin, int(timings));
		fprintf(f, "online_name = %s\nonline_delay = %d\nonline_connection = %d\nonline_port = %d\n", online.name.c_str(), online.delay,
				online.show_connection, online.port);
		if (!online.address.empty()) fprintf(f, "online_address = %s\n", online.address.c_str());
		if (!online.lobby.empty()) fprintf(f, "online_lobby = %s\n", online.lobby.c_str());
		fprintf(f, "online_hide_ip = %d\n", int(online.hide_ip));
		for (int p = 0; p < 2; p++) ctl[p].save(f, p);
		const bool flushed = flush_to_disk(f);
		if (fclose(f) == 0 && flushed && replace_file(path, tmp)) return true;
		std::error_code ec;
		std::filesystem::remove(tmp, ec);
		if (!said) fprintf(stderr, "couldn't save the settings in %s\n", path.c_str());
		said = true;
		return false;
	}
};

// A file copied whole: to a temporary name, then renamed (as VF1's: std::filesystem::copy_file left an empty file
// in Valve's container on a shared folder, and a cut-short copy mustn't look like a set)
bool copy_whole(const std::filesystem::path &from, const std::filesystem::path &to, std::string &why)
{
	FILE *in = fopen(from.string().c_str(), "rb");
	if (!in) { why = "can't read " + from.string(); return false; }
	const std::filesystem::path tmp = to.string() + ".tmp";
	FILE *out = fopen(tmp.string().c_str(), "wb");
	if (!out) { fclose(in); why = "can't write " + tmp.string(); return false; }
	std::vector<char> buf(1 << 20);
	bool ok = true;
	for (size_t n; (n = fread(buf.data(), 1, buf.size(), in)) > 0;)
		if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
	ok = ok && !ferror(in);
	fclose(in);
	ok = (fclose(out) == 0) && ok;
	if (ok) ok = replace_file(to.string(), tmp.string());
	else { std::error_code ec; std::filesystem::remove(tmp, ec); }
	if (!ok) why = "couldn't write " + to.string();
	return ok;
}

// The player's own game, the first time: no complete set in their data folder's roms/ yet, so ask for MAME's
// vf3.zip with a file picker, check it, and copy it in. The player's ROM folder (`folder`) is looked in first and
// only read. `keep`: the player's data folder when the data folder is the program's own (the Steam Deck package):
// a copy of the zip is kept there too, so an update unpacked into a new folder doesn't ask for it again (as VF1's).
// The zip's path in `out`; false if they cancel.
bool first_launch(const std::string &data, const std::string &keep, const std::string &folder, std::string &out)
{
	namespace fs = std::filesystem;
	const fs::path own = fs::path(data) / "roms";
	const fs::path roms = folder.empty() ? own : fs::path(folder), zip = roms / "vf3.zip";
	const fs::path kept = !folder.empty() || keep.empty() ? own : fs::path(keep) / "roms";
	std::error_code ec;
	auto keep_copy = [&] {   // (when it isn't there already)
		if (kept == roms || !folder.empty() || !fs::is_regular_file(zip, ec)) return;
		if (fs::is_regular_file(kept / "vf3.zip", ec) && fs::file_size(kept / "vf3.zip", ec) == fs::file_size(zip, ec)) return;
		fs::create_directories(kept, ec);
		std::string why;
		if (copy_whole(zip, kept / "vf3.zip", why)) fprintf(stderr, "a copy of vf3.zip kept in %s (for the next update)\n", kept.string().c_str());
		else fprintf(stderr, "couldn't keep a copy of vf3.zip: %s\n", why.c_str());
	};
	std::string problem;
	auto check = [&](const fs::path &z) -> bool {
		romset::result r = romset::load(z.string());
		if (r.ok) { out = z.string(); return true; }
		problem = r.error + "\n\n";
		return false;
	};
	if (!folder.empty()) {
		if (fs::is_regular_file(zip, ec)) fprintf(stderr, "the game from the ROM folder %s\n", roms.string().c_str());
		else {
			fprintf(stderr, "ROM folder: no vf3.zip in %s%s: looking in %s next\n", roms.string().c_str(),
					fs::is_directory(roms, ec) ? "" : " (no such folder)", own.string().c_str());
			if (fs::is_regular_file(own / "vf3.zip", ec) && check(own / "vf3.zip")) {
				fprintf(stderr, "the game from %s\n", own.string().c_str());
				return true;
			}
		}
	}
	if (fs::is_regular_file(zip, ec) && check(zip)) {
		keep_copy();
		return true;
	}
	// an update in a new folder: the set kept from before
	if (kept != roms && fs::is_regular_file(kept / "vf3.zip", ec) && check(kept / "vf3.zip")) {
		fprintf(stderr, "the game from %s\n", kept.string().c_str());
		return true;
	}
	if (getenv("VF3_HIDDEN")) {
		fprintf(stderr, "%sno vf3.zip in %s\n", problem.c_str(), roms.string().c_str());
		return false;
	}
	SDL_Init(SDL_INIT_VIDEO);
	const char *title = "Virtua Fighter 3";
	// (the player's ROM folder holds a vf3.zip that isn't the set: the one chosen goes in the data folder's roms,
	// not over the player's own file)
	const fs::path into = !folder.empty() && fs::exists(zip, ec) ? own : roms, into_zip = into / "vf3.zip";
	const std::string where = "It's copied into " + into.string() + ", so you only do this once.";
	std::string note = problem + "Choose your copy of Virtua Fighter 3: MAME's \"vf3\" set (vf3.zip, Revision D).\n\n" + where;
	for (;;) {
		if (!message_box(SDL_MESSAGEBOX_INFORMATION, title, note)) return false;
		// (the callback may run on another thread: what it writes is read only after done, set last)
		struct pick { std::atomic<bool> done{ false }; bool failed = false; std::vector<std::string> files; } chosen;
		static const SDL_DialogFileFilter filters[] = { { "MAME ROM sets (zip)", "zip" } };
		SDL_ShowOpenFileDialog([](void *ud, const char *const *list, int) {
			auto *c = static_cast<pick *>(ud);
			if (!list) c->failed = true;
			else for (; *list; list++) c->files.push_back(*list);
			c->done.store(true, std::memory_order_release);
		}, &chosen, nullptr, filters, 1, nullptr, false);
		while (!chosen.done.load(std::memory_order_acquire)) { SDL_Event e; while (SDL_PollEvent(&e)) {} SDL_Delay(10); }
		if (chosen.failed) {
			fs::create_directories(roms, ec);
			message_box(SDL_MESSAGEBOX_INFORMATION, title,
				"There's no file picker here. Put your vf3.zip in this folder, then start the game again:\n\n" + roms.string());
			return false;
		}
		if (chosen.files.empty()) return false;
		const std::string f = chosen.files[0];
		romset::result r = romset::load(f);
		if (!r.ok) {
			note = fs::path(f).filename().string() + ": " + r.error + "\n\nChoose MAME's \"vf3\" set (vf3.zip).\n\n" + where;
			continue;
		}
		fs::create_directories(into, ec);
		std::string why;
		if (!fs::equivalent(f, into_zip, ec) && !copy_whole(f, into_zip, why)) {
			note = "Couldn't copy it into " + into.string() + ": " + why + "\n\nChoose your vf3.zip.";
			continue;
		}
		if (into == roms) keep_copy();
		out = into_zip.string();
		return true;
	}
}

// The board and what makes its pictures and sound
// The game's, the sound's and the pictures' threads: macOS runs a thread that doesn't say on whichever cores it likes,
// the efficiency cores among them (a frame that took 7 ms there took more: late for the picture that waits on it)
static void fast_thread()
{
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

struct Game
{
	m3::Model3 board;
	ppcrec::Rec rec;
	bool recompiled = false;
	SoundBoard sound;
	video::TileGen tiles;
	// the tile lines drawn on a thread of their own as the game reaches each (their registers copied): half a
	// fight frame's time on the game's own thread before (2.8 of 5.5 ms; a Steam Deck's ~4-5 of 11)
	video::TileWorker tile_worker{ tiles, board };
	video::SceneBuilder builder;
	// the last two frames' scenes (smooth motion goes between them) and their 2D layers
	video::Scene scene[2];
	int newest = 0;
	bool have_prev = false;
	std::vector<uint32_t> below = std::vector<uint32_t>(video::W * video::H), above = std::vector<uint32_t>(video::W * video::H);
	std::vector<uint32_t> below_wide = std::vector<uint32_t>(video::WW * video::H);   // (widescreen's: tilegen.h)
	bool full_3d = false;
	video::TileGen::Scroll prev_scroll;   // the frame before's (for bg_dx, bg_dy)
	std::vector<uint32_t> prev_below;
	bool bg_moved = false;                // the layer below the 3D scrolled this frame by bg_dx, bg_dy
	float bg_dx = 0, bg_dy = 0;   // the newest frame's 3D drawn full width (a stage)
	video::SkyLead sky;           // how far the stage's sky is behind the 3D camera (sky_lead.h)
	std::vector<int16_t> audio;
	uint64_t frame = 0;
	bool tiles_on = true;   // (off for the boot run unseen: the 2D layers drawn a line at a time for nothing)

	bool load(const std::string &zip, std::string &error, bool interp)
	{
		if (!board.load(zip, error)) return false;
		if (!interp) {
			ppcrec::Stats st;
			built_image = board.program_image();
			recompiled = rec.build(board.cpu, built_image, &st);
			if (recompiled) {
				board.cpu.rec = &rec;
				fprintf(stderr, "recompiled %zu instructions (%zu native) to %zu KB of native code in %.0f ms\n",
						st.instructions, st.native, st.code_bytes / 1024, st.ms);
			} else fprintf(stderr, "couldn't recompile the game (no executable memory?): running it on the slower interpreter instead\n");
		}
		sound.attach(&board.sound_rom, &board.samples);
		board.on_line = [this](unsigned y) {
			if (!tiles_on) return;
			tile_worker.line(y);
		};
		return true;
	}
	// The program changed since its native code was made (the PC way's FREE PLAY notices: an instruction in the
	// CROM the boot copies from): made again, or the copied program wouldn't match it and the interpreter would run
	// everything
	std::vector<uint32_t> built_image;
	void program_changed()
	{
		if (!recompiled) return;
		std::vector<uint32_t> img = board.program_image();
		if (img == built_image) return;
		built_image = std::move(img);
		ppcrec::Stats st;
		if (rec.build(board.cpu, built_image, &st))
			fprintf(stderr, "recompiled again (the program changed): %zu instructions in %.0f ms\n", st.instructions, st.ms);
		else fprintf(stderr, "couldn't recompile the changed program: the interpreter runs it\n");
	}
	// The sound board runs on a thread of its own, a frame behind the game (its ~3 ms a frame alongside the next
	// frame's work, not in a picture's time): it hears only the game's bytes, and its sound is queued a frame later
	// (the queue kept a frame shorter for it: QUEUED)
	std::thread sound_thread;
	std::mutex sound_m;
	std::condition_variable sound_cv;
	bool sound_busy = false, sound_quit = false, sound_job_on = false;
	std::vector<uint8_t> sound_midi;
	std::vector<int16_t> sound_out;
	// A netplay match's sound: the sound board run to the real clock instead, on its thread, the queue kept topped up
	// (free_target bytes) whatever the frames do. Its frames come when the session lets them: one held back for the
	// other side, a slow frame on a Steam Deck (a character select's 2D), and the sound made a frame at a time ran
	// dry (choppy). The sound board only hears the game's bytes, never sends any back: they're queued here as the
	// frames make them and played as it reaches them, the music and the voices in their own time and pitch.
	SDL_AudioStream *free_stream = nullptr;
	std::vector<uint8_t> free_midi;   // (a frame's bytes, taken)
	int free_target = 0;
	std::atomic<int> sound_dry{ 0 };   // (the queue found empty: a drop heard; the log's pace line)
	void sound_wait()
	{
		std::unique_lock<std::mutex> lk(sound_m);
		sound_cv.wait(lk, [this] { return !sound_busy; });
	}
	// A change to the sound board, between its runs (a match's sound thread makes none meanwhile)
	template <class F> void sound_do(F f)
	{
		std::unique_lock<std::mutex> lk(sound_m);
		sound_cv.wait(lk, [this] { return !sound_busy; });
		f();
	}
	// The match's sound to the real clock (a stream), or back to a frame's at a time (nullptr)
	void set_sound_free(SDL_AudioStream *stream, int target_bytes)
	{
		if (stream == free_stream) return;
		if (emu_thread.joinable()) job_wait();   // (no frame half done meanwhile)
		if (!sound_thread.joinable()) sound_start();
		sound_wait();
		{
			std::lock_guard<std::mutex> lk(sound_m);
			if (stream && !sound_out.empty())   // (the last frame's own, made meanwhile: played first)
				SDL_PutAudioStreamData(stream, sound_out.data(), int(sound_out.size() * sizeof(int16_t)));
			if (stream) sound_out.clear();
			sound_midi.clear();
			free_stream = stream;
			free_target = target_bytes;
		}
		sound_cv.notify_all();
	}
	void sound_start()
	{
		sound_thread = std::thread([this] {
			fast_thread();
			std::unique_lock<std::mutex> lk(sound_m);
			std::vector<uint8_t> midi;
			std::vector<int16_t> out;
			bool fed = false;   // (this match's sound begun)
			for (;;) {
				if (!free_stream) fed = false;
				if (free_stream) {
					// (a look every 4 ms: a frame's sound, 17 ms, made whenever the queue's under its mark)
					sound_cv.wait_for(lk, std::chrono::milliseconds(4));
					if (sound_quit) return;
					if (free_stream && fed && SDL_GetAudioStreamQueued(free_stream) == 0) sound_dry++;   // (not before its first)
					for (int n = 0; free_stream && n < 4 && SDL_GetAudioStreamQueued(free_stream) < free_target; n++) {
						SDL_AudioStream *to = free_stream;
						midi.swap(sound_midi);
						sound_midi.clear();
						sound_busy = true;   // (reset_host waits for it)
						lk.unlock();
						for (uint8_t b : midi) sound.midi_in(b);
						midi.clear();
						out.clear();
						sound.run(1.0 / FRAME_HZ, out);
						SDL_PutAudioStreamData(to, out.data(), int(out.size() * sizeof(int16_t)));
						fed = true;
						lk.lock();
						sound_busy = false;
						sound_cv.notify_all();
					}
					continue;
				}
				sound_cv.wait(lk, [this] { return sound_busy || sound_quit || free_stream; });
				if (sound_quit) return;
				if (free_stream) continue;
				lk.unlock();
				for (uint8_t b : sound_midi) sound.midi_in(b);
				sound_out.clear();
				sound.run(1.0 / FRAME_HZ, sound_out);
				if (!sound_job_on) sound_out.clear();
				lk.lock();
				sound_busy = false;
				sound_cv.notify_all();
			}
		});
	}
	~Game()
	{
		if (emu_thread.joinable()) {
			job_wait();
			{ std::lock_guard<std::mutex> lk(emu_m); emu_quit = true; }
			emu_cv.notify_all();
			emu_thread.join();
		}
		if (sound_thread.joinable()) {
			{ std::lock_guard<std::mutex> lk(sound_m); sound_quit = true; }
			sound_cv.notify_all();
			sound_thread.join();
		}
	}
	void reset()
	{
		reset_host();
		board.reset();
	}
	// all but the board: the sound board, the pictures (a netplay match starts its board itself)
	void reset_host()
	{
		if (emu_thread.joinable()) job_wait();
		{
			std::unique_lock<std::mutex> lk(sound_m);   // (held: a match's sound thread makes none meanwhile)
			sound_cv.wait(lk, [this] { return !sound_busy; });
			sound_out.clear();
			sound_midi.clear();
			sound.reset();
		}
		builder.clear_cache();
		have_prev = false;
		scene[0] = scene[1] = video::Scene();
		frame = 0;
	}
	std::function<void(std::vector<uint8_t> &)> take_midi;   // (a netplay match's: the adapter's)
	std::string hud_names[2];   // (a netplay match's: the players' names under the energy bars)
	video::HudFlag hud_flags[2];   // (and their countries' flags, beside them: none if not known)
	// One frame: the board, the sound board, the frame's scene and 2D layers. In two halves for smooth motion
	// (start_frame, finish_frame): the first half run a picture before the frame is due, so no picture's time holds
	// a whole frame (at 120 a second a picture has 8.3 ms, a frame took 7-10 on an M4 Max: every other picture missed
	// its refresh, about 80 a second)
	void run_frame(bool sound_on, bool picture)
	{
		start_frame();
		finish_frame(sound_on, picture);
		sysinfo::note_frame();   // (on the thread that ran it: the game's own, when it has one)
	}
	// what a frame's parts cost, ms in all (the pace line's: where a slow machine's time goes), and how many frames
	double part_ms[5] = {};   // the board, waiting for the sound, the scene, the 2D layers, the sky
	uint64_t part_n = 0;
	static double ms_since(uint64_t t) { return double(SDL_GetTicksNS() - t) / 1e6; }
	void start_frame()
	{
		const uint64_t t = SDL_GetTicksNS();
		board.run_frame_start();
		part_ms[0] += ms_since(t);
	}
	void finish_frame(bool sound_on, bool picture)
	{
		uint64_t t = SDL_GetTicksNS();
		board.run_frame_end();
		part_ms[0] += ms_since(t);
		t = SDL_GetTicksNS();
		if (!sound_thread.joinable()) sound_start();
		if (free_stream) {   // (a match's: the frame's bytes queued for the sound board, which runs on its own)
			free_midi.clear();
			if (take_midi) take_midi(free_midi);
			else {
				free_midi.assign(board.midi.begin(), board.midi.end());
				board.midi.clear();
			}
			std::lock_guard<std::mutex> lk(sound_m);
			sound_midi.insert(sound_midi.end(), free_midi.begin(), free_midi.end());
			part_n++;
		} else {
			sound_wait();   // (the frame before's sound, made meanwhile)
			part_ms[1] += ms_since(t);
			part_n++;
			audio.insert(audio.end(), sound_out.begin(), sound_out.end());
			sound_out.clear();
			{
				std::lock_guard<std::mutex> lk(sound_m);
				if (take_midi) take_midi(sound_midi);   // (a match: a frame run again sends only what it hadn't)
				else {
					sound_midi.assign(board.midi.begin(), board.midi.end());
					board.midi.clear();
				}
				sound_job_on = sound_on;
				sound_busy = true;
			}
			sound_cv.notify_all();
		}
		frame++;
		if (!picture) { tile_worker.finish(); have_prev = false; sky.reset(); return; }
		newest ^= 1;
		// the 2D layers put together on the tile worker (its last lines first) while the 3D's scene is built here
		tile_worker.then([this] {
			tiles.aligned(below, above);
			if (!hud_names[0].empty() || !hud_names[1].empty()) {   // (a netplay match's)
				const video::HudFlag *fl[2] = { &hud_flags[0], &hud_flags[1] };
				video::hud_names(board, above, hud_names, fl);
			}
			tiles.aligned_wide(below_wide);
		});
		t = SDL_GetTicksNS();
		builder.build(board, scene[newest]);
		part_ms[2] += ms_since(t);
		have_prev = frame > 1;
		t = SDL_GetTicksNS();
		tile_worker.finish();   // (the frame's layers its own, put together)
		part_ms[3] += ms_since(t);
		t = SDL_GetTicksNS();
		bg_moved = video::TileGen::scrolled(prev_scroll, tiles.scroll, prev_below, below, bg_dx, bg_dy) && have_prev;
		prev_scroll = tiles.scroll;
		prev_below = below;
		static const bool sky_lag = getenv("VF3_SKY_LAG") != nullptr;   // (tests: the game's own backdrop, 3 frames behind)
		if (have_prev && !sky_lag) sky.frame(prev(), cur(), bg_moved, bg_dx, bg_dy);
		else sky.reset();
		part_ms[4] += ms_since(t);
		bool full = false;   // (the 3D drawn full width: a stage; a close-up's or a title's 3D isn't)
		for (auto &v : scene[newest].viewports) full |= v.full_screen && !v.models.empty();
		full_3d = full;
	}
	const video::Scene &cur() const { return scene[newest]; }
	const video::Scene &prev() const { return scene[newest ^ 1]; }
	void forget_prev() { have_prev = false; view.have_prev = false; }

	// What the pictures are drawn from: the last two frames' scenes, their 2D layers, the sky's lead and a copy of the
	// texture RAM, taken from the game as each frame ends (publish). The game's frames run on a thread of their own
	// (job_start, job_wait) while the pictures are drawn: the pictures never read what the game is writing.
	struct View {
		video::Scene scene[2];
		int newest = 0;
		bool have_prev = false, full_3d = false, bg_moved = false;
		float bg_dx = 0, bg_dy = 0;
		std::vector<uint32_t> below = std::vector<uint32_t>(video::W * video::H), above = std::vector<uint32_t>(video::W * video::H);
		std::vector<uint32_t> below_wide = std::vector<uint32_t>(video::WW * video::H);
		video::SkyLead sky;
		std::vector<uint16_t> tex;
		std::vector<m3::Model3::TexUpload> uploads;   // (the areas changed since a renderer last took them)
		const video::Scene &cur() const { return scene[newest]; }
		const video::Scene &prev() const { return scene[newest ^ 1]; }
	} view;
	void publish()
	{
		view.newest ^= 1;
		view.scene[view.newest] = scene[newest];
		view.have_prev = have_prev;
		view.full_3d = full_3d;
		view.bg_moved = bg_moved;
		view.bg_dx = bg_dx;
		view.bg_dy = bg_dy;
		view.below = below;
		view.above = above;
		view.below_wide = below_wide;
		view.sky = sky;
		// the texture RAM: the rows written since the last (all of it the first time, or after many)
		auto &ups = board.tex_uploads;
		if (view.tex.size() != board.tex_ram.size() || ups.size() > 256) {
			view.tex = board.tex_ram;
			view.uploads.assign(1, m3::Model3::TexUpload{ 0, 0, 2048, 2048 });
		} else {
			for (auto &u : ups) {
				const size_t y0 = u.y & 2047, h = std::min<size_t>(u.h, 2048 - y0);
				memcpy(view.tex.data() + y0 * 2048, board.tex_ram.data() + y0 * 2048, h * 2048 * sizeof(uint16_t));
				view.uploads.push_back(u);
			}
			if (view.uploads.size() > 512) view.uploads.assign(1, m3::Model3::TexUpload{ 0, 0, 2048, 2048 });
		}
		ups.clear();
	}
	std::thread emu_thread;
	std::mutex emu_m;
	std::condition_variable emu_cv;
	bool emu_busy = false, emu_quit = false, emu_sound_on = true;
	std::function<void()> emu_job;   // (instead of a frame: a netplay match's step, its rollback with it)
	std::thread::id emu_id;
	void job_start(bool sound_on)
	{
		if (!emu_thread.joinable())
			emu_thread = std::thread([this] {
				fast_thread();
				std::unique_lock<std::mutex> lk(emu_m);
				emu_id = std::this_thread::get_id();
				for (;;) {
					emu_cv.wait(lk, [this] { return emu_busy || emu_quit; });
					if (emu_quit) return;
					const bool snd = emu_sound_on;
					std::function<void()> job = std::move(emu_job);
					emu_job = nullptr;
					lk.unlock();
					if (job) job();
					else run_frame(snd, true);
					lk.lock();
					emu_busy = false;
					emu_cv.notify_all();
				}
			});
		{
			std::lock_guard<std::mutex> lk(emu_m);
			emu_sound_on = sound_on;
			emu_busy = true;
		}
		emu_cv.notify_all();
	}
	// Anything on the game's thread: a match's step (the session's saves and loads, the frames it runs again)
	void job_run(std::function<void()> f)
	{
		{
			std::lock_guard<std::mutex> lk(emu_m);
			emu_job = std::move(f);
		}
		job_start(true);
	}
	void job_wait()
	{
		if (std::this_thread::get_id() == emu_id) return;   // (the job itself, settling before a save: nothing to wait for)
		std::unique_lock<std::mutex> lk(emu_m);
		emu_cv.wait(lk, [this] { return !emu_busy; });
	}
};

} // namespace

int main(int argc, char **argv)
{
	if (getenv("VF3_HIDDEN")) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
	const bool pace_log = getenv("VF3_PACE_LOG") != nullptr;
	std::string zip;
	bool arg_arcade = false, sound = true, show_boot = false, arg_interp = getenv("VF3_INTERP") != nullptr;
	int arg_volume = -1, arg_music = -1, arg_effects = -1, arg_scale = 0, arg_aspect = -1, arg_full = -1;
	std::string arg_roms;
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--arcade") arg_arcade = true;
		else if (a == "--roms" && i + 1 < argc) arg_roms = argv[++i];
		else if (a.rfind("--roms=", 0) == 0) arg_roms = a.substr(7);
		else if (a == "--interp") arg_interp = true;
		else if (a == "--fullscreen") arg_full = 1;
		else if (a == "--window") arg_full = 0;
		else if (a == "--no-sound") sound = false;
		else if (a == "--volume" && i + 1 < argc) arg_volume = std::clamp(atoi(argv[++i]), 0, 100);
		else if (a == "--music" && i + 1 < argc) arg_music = std::clamp(atoi(argv[++i]), 0, 100);
		else if (a == "--effects" && i + 1 < argc) arg_effects = std::clamp(atoi(argv[++i]), 0, 100);
		else if (a == "--scale" && i + 1 < argc) arg_scale = std::clamp(atoi(argv[++i]), 1, MAX_SCALE);
		else if (a == "--aspect" && i + 1 < argc) {
			const std::string v = argv[++i];
			for (int k = 0; k < N_ASPECTS; k++) if (v == ASPECT_NAMES[k] || (k == 0 && v == "auto")) arg_aspect = k;
			if (arg_aspect < 0) { fprintf(stderr, "--aspect: fit, 4:3, 16:10, 16:9 or 21:9\n"); return 2; }
		}
		else if (a == "--show-boot") show_boot = true;
		else if (a.rfind("--", 0) == 0) { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
		else if (a.rfind("-psn_", 0) == 0) {}
		else {
			std::error_code ec;
			if (!std::filesystem::is_regular_file(a, ec)) { fprintf(stderr, "%s: no such file (options start with --)\n", a.c_str()); return 2; }
			zip = a;
		}
	}
	const bool hidden = getenv("VF3_HIDDEN") != nullptr;
	const int64_t quit_after = getenv("VF3_QUIT_AFTER") ? atoll(getenv("VF3_QUIT_AFTER")) : -1;
	InputScript script;
	bool scripted = false;
	if (const char *sp = getenv("VF3_SCRIPT")) {
		std::string err;
		if (!script.load(sp, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
		scripted = true;
	}
	auto fail = [](const std::string &what) {
		fprintf(stderr, "%s\n", what.c_str());
		if (!getenv("VF3_HIDDEN")) message_box(SDL_MESSAGEBOX_ERROR, "Virtua Fighter 3", what);
		return 1;
	};

	// The game and the data folder (see the top)
	std::error_code ec;
	std::string data = getenv("VF3_DATA") ? getenv("VF3_DATA") : "";
	bool players = false;
	std::string keep_data;   // (the Steam Deck package's: where its zip is kept across updates)
	const std::string home = program_folder();
	const char *portable = portable_marker(home);
	if (zip.empty()) {
		const char *base = SDL_GetBasePath();
		const std::string here = base ? base : "";
		if (portable) {
			if (data.empty()) data = home;
			players = true;
		} else if (!here.empty() && std::filesystem::is_directory(here + "roms", ec)) {
			if (data.empty()) {
				data = here;
				// the zip kept in the player's data folder too, for the next update (not while testing: VF3_DATA)
				char *pref = SDL_GetPrefPath(nullptr, "Virtua Fighter 3 Recomp");
				if (pref) keep_data = pref;
				SDL_free(pref);
				while (!keep_data.empty() && keep_data.back() == '/') keep_data.pop_back();
			}
			players = true;
		} else if (arg_roms.empty() && !getenv("VF3_ROMS") && std::filesystem::is_regular_file("roms/vf3.zip", ec)) zip = "roms/vf3.zip";
		else {
			if (data.empty()) {
				char *pref = SDL_GetPrefPath(nullptr, "Virtua Fighter 3 Recomp");
				data = pref ? pref : ".";
				SDL_free(pref);
			}
			players = true;
		}
		while (data.size() > 1 && (data.back() == '/' || data.back() == '\\') && data[data.size() - 2] != ':') data.pop_back();
	}
	if (data.empty()) data = (std::filesystem::path(zip).parent_path() / "vf3_data").string();
	if (data.empty()) data = "vf3_data";
	if (!isatty(fileno(stderr))) {
		std::filesystem::create_directories(data, ec);
		// the last runs' logs kept (log.1.txt the one before, to log.3.txt): a match's report survives the game being
		// opened again before its log is sent
		for (int i = 3; i >= 1; i--) {
			const std::filesystem::path from = data + (i == 1 ? std::string("/log.txt") : "/log." + std::to_string(i - 1) + ".txt"),
			                            to = data + "/log." + std::to_string(i) + ".txt";
			std::filesystem::remove(to, ec);
			std::filesystem::rename(from, to, ec);
		}
		if (freopen((data + "/log.txt").c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
	}
	sysinfo::tune();
	fast_thread();
	const std::string system = sysinfo::describe();
	fprintf(stderr, "Virtua Fighter 3 Recomp %s (%s) on %s\n", VF3_VERSION, VF3_DATE, system.c_str());
	// a Steam Deck, with Steam (its SteamDeck variable) or not (its APU: the LCD model's 0405, the OLED's 0932)
	const bool deck = getenv("SteamDeck") || system.find("Custom APU 0405") != std::string::npos || system.find("Custom APU 0932") != std::string::npos;
	if (deck && !getenv("SteamDeck")) fprintf(stderr, "a Steam Deck (its APU), started without Steam\n");
	if (portable) fprintf(stderr, "portable (%s): everything in %s\n", portable, data.c_str());
	else if (home.find("/AppTranslocation/") != std::string::npos)   // (macOS runs a downloaded app from a hidden copy until it's moved)
		fprintf(stderr, "run from macOS's hidden copy of the app (App Translocation): a portable.txt beside it isn't seen until the app is moved with the Finder\n");
	const std::string settings_path = data + "/settings.ini";
	settings cfg;
	bool cfg_dirty = false;
	if (deck) { cfg.fullscreen = true; cfg.aspect = 2; cfg.scale = 2; }
	cfg.load(settings_path);
	if (cfg.upgraded && !getenv("VF3_QUIT_AFTER")) cfg.save(settings_path);
	if (arg_scale) cfg.scale = arg_scale;
	if (arg_aspect >= 0) cfg.aspect = arg_aspect;
	if (arg_full >= 0) cfg.fullscreen = arg_full;
	if (arg_arcade) cfg.arcade = true;
	if (arg_volume >= 0) cfg.volume = arg_volume;
	if (arg_music >= 0) cfg.music = arg_music;
	if (arg_effects >= 0) cfg.effects = arg_effects;
	g_pc_mode = !cfg.arcade;
	int aspect_i = cfg.aspect;

	if (players) {
		std::string roms_folder, from;
		if (!arg_roms.empty()) roms_folder = player_folder(arg_roms), from = "--roms";
		else if (const char *e = getenv("VF3_ROMS"); e && *e) roms_folder = player_folder(e), from = "VF3_ROMS";
		else if (!cfg.roms_folder.empty()) roms_folder = player_folder(cfg.roms_folder, data), from = "settings.ini";
		if (!roms_folder.empty()) fprintf(stderr, "ROM folder (%s): %s\n", from.c_str(), roms_folder.c_str());
		else fprintf(stderr, "ROM folder: %s\n", (std::filesystem::path(data) / "roms").string().c_str());
		if (!first_launch(data, keep_data, roms_folder, zip)) return hidden ? 1 : 0;
	}
	static Game game;   // (large: not on the stack)
	{
		std::string err;
		if (!game.load(zip, err, arg_interp)) return fail(err);
	}
	m3::Model3 &board = game.board;
	// Online play (online.h): in a match the board holds the host's NVRAM, and this player's own is kept here
	static Online online(board);
	std::vector<uint8_t> own_backup;
	uint16_t own_eeprom[64] = {};
	bool nvram_away = false;
	const std::string backup_path = data + "/backup", eeprom_path = data + "/eeprom";
	load_blob(backup_path, board.backup.data(), board.backup.size());
	load_blob(eeprom_path, board.eeprom.regs, sizeof(board.eeprom.regs));
	auto nvram_hash = [&] { return fnv(board.eeprom.regs, sizeof(board.eeprom.regs), fnv(board.backup.data(), board.backup.size())); };
	uint64_t saved_hash = nvram_hash();
	bg_saver saver;
	// (in play: copied for the writer thread, bg_saver; at the quit: written here, the writer finished first)
	auto save_nvram = [&](bool now = false) {
		if (game.emu_thread.joinable()) game.job_wait();   // (not while a frame runs alongside)
		if (quit_after >= 0) return;   // test runs leave the saves alone
		if (nvram_away) return;        // (a netplay match: the host's NVRAM, not this player's)
		const uint64_t h = nvram_hash();
		if (h == saved_hash) return;
		std::filesystem::create_directories(data, ec);
		if (!now) {
			saver.put(backup_path, board.backup.data(), board.backup.size());
			saver.put(eeprom_path, board.eeprom.regs, sizeof(board.eeprom.regs));
			saved_hash = h;
		} else if (save_blob(backup_path, board.backup.data(), board.backup.size()) &&
				   save_blob(eeprom_path, board.eeprom.regs, sizeof(board.eeprom.regs)))
			saved_hash = h;
		else fprintf(stderr, "couldn't save the NVRAM in %s\n", data.c_str());
	};

	// Boot: the PC way runs it unseen up to the SEGA logo
	auto skip_boot = [&] { return g_pc_mode && !show_boot && !scripted; };
	uint64_t game_frame = 0;   // frames since the boot (the script's frame numbers)
	// The PC way: the FREE PLAY notices hidden (always free play: nothing to say); the arcade way: shown
	auto apply_play_style = [&] {
		if (game.emu_thread.joinable()) game.job_wait();   // (not while a frame runs alongside)
		if (!g_pc_mode) board.show_free_play_text();
		else if (board.hide_free_play_text() != 2) fprintf(stderr, "not the data known: the FREE PLAY notices stay\n");
		game.program_changed();
	};
	auto boot = [&] {
		apply_play_style();
		game.reset();
		// The PC way plays free: the arcade's coin setting is kept (settings.ini's arcade_coins) before FREE PLAY
		// replaces it, and the arcade way gives it back (as VF1's and VF2's). A board that has never run writes its
		// settings in its first frames, so those run first. FREE PLAY the player chose in the test menu, with none
		// kept, stays.
		for (int i = 0; i < 300 && board.coin_setting() < 0; i++) board.run_frame();
		if (const int coin = board.coin_setting(); coin >= 0) {
			int keep = cfg.arcade_coin;
			if (g_pc_mode) {
				if (coin != m3::Model3::COIN_FREE_PLAY) keep = coin;
				else if (keep == -2) keep = -1;
				board.set_coin_setting(m3::Model3::COIN_FREE_PLAY);
			} else {
				if (coin == m3::Model3::COIN_FREE_PLAY && keep >= 0) board.set_coin_setting(keep);
				keep = -1;
			}
			if (keep != cfg.arcade_coin) { cfg.arcade_coin = keep; cfg_dirty = true; }
			if (g_pc_mode && !board.free_play()) fprintf(stderr, "the PC way: couldn't set FREE PLAY in the game's settings (coin setting %d)\n", board.coin_setting());
			game.reset();
		}
		game_frame = 0;
		if (!skip_boot()) return;
		const auto t0 = std::chrono::steady_clock::now();
		game.tiles_on = false;
		while (game_frame < BOOT_FRAMES) {
			game.run_frame(false, false);
			game.audio.clear();
			game_frame++;
		}
		game.tiles_on = true;
		if (!scripted) vf3net::set_versus_count(board, vf3net::random_versus_count());   // (vf3net::VERSUS_COUNT: the first match between two players' stage)
		else if (const char *vc = getenv("VF3_VERSUS_COUNT")) vf3net::set_versus_count(board, uint32_t(atoi(vc)));   // (tests: that stage)
		fprintf(stderr, "boot: %llu frames in %.2f s, unseen\n", (unsigned long long)game_frame,
				std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	};
	boot();

#ifdef _WIN32
	SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
#endif
	if (!SDL_Init(SDL_INIT_VIDEO)) return fail(std::string("Couldn't start the video: ") + SDL_GetError());
	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) fprintf(stderr, "no gamepads: %s\n", SDL_GetError());
	if (sound && !SDL_InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "no sound: %s\n", SDL_GetError());
	int win_w = 1366, win_h = 768;
	const bool fixed_window = getenv("VF3_WINDOW") && sscanf(getenv("VF3_WINDOW"), "%dx%d", &win_w, &win_h) == 2;
	if (!fixed_window) {
		SDL_Rect usable;
		if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
			const float k = std::min({ 1.0f, usable.w * 0.9f / win_w, usable.h * 0.9f / win_h });
			win_w = int(win_w * k); win_h = int(win_h * k);
		}
	}
	SDL_Window *win = SDL_CreateWindow("Virtua Fighter 3", win_w, win_h,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_HIDDEN);
	if (!win) return fail(std::string("Couldn't open the window: ") + SDL_GetError());
	SDL_HideCursor();
	// the window's icon (the Mac app and the Windows exe carry their own)
	if (SDL_Surface *icon = SDL_LoadPNG((find_assets() + "/icon/icon.png").c_str())) {
		SDL_SetWindowIcon(win, icon);
		SDL_DestroySurface(icon);
	}
	// The 3D on the GPU (Display > 3D renderer: Auto or GPU, or VF3_3D=cpu|gpu|auto): SDL's "gpu" renderer
	// (Vulkan, Direct3D 12 or Metal), whose device the 3D shares, its pictures the renderer's textures. Else, or if
	// it can't start here, SDL's usual one and the CPU's 3D.
	enum { R3D_CPU, R3D_GPU, R3D_AUTO };
	static const char *R3D_NAMES[] = { "CPU", "GPU", "Auto" };
	int r3d_mode = cfg.renderer3d;
	// Why the 3D is on the CPU when the setting isn't CPU: said on screen, not only in the log (a player whose GPU
	// couldn't be used just saw a slow game). cpu_why_new: not said yet.
	std::string cpu_why;
	bool cpu_why_new = false;
	// (why: what happened, in a player's words; then what to do. None with a renderer chosen by hand, SDL_RENDER_DRIVER)
#ifdef __APPLE__
	static const char *UPDATE = "Update macOS, then choose GPU in Display > 3D renderer to try it again.";
#else
	static const char *UPDATE = "Update the graphics driver, then choose GPU in Display > 3D renderer to try it again.";
#endif
	static const char *RETRY = "Choose GPU in Display > 3D renderer to try it again from the next start.";
	auto on_cpu_because = [&](const std::string &why, const char *what_to_do) {
		if (cpu_why.empty() && !getenv("SDL_RENDER_DRIVER")) { cpu_why = why + "." + (*what_to_do ? std::string(" ") + what_to_do : ""); cpu_why_new = true; }
	};
	if (const char *e = getenv("VF3_3D")) r3d_mode = !strcmp(e, "gpu") ? R3D_GPU : !strcmp(e, "auto") ? R3D_AUTO : R3D_CPU;
	SDL_Renderer *ren = nullptr;
	auto failed_before = [&](const char *d) { return (" " + cfg.gpu3d_failed + " ").find(std::string(" ") + d + " ") != std::string::npos; };
	if (r3d_mode != R3D_CPU && !getenv("SDL_RENDER_DRIVER")) {
		// (as VF2's: a marker left while the renderer starts, there next time if it crashed then, so SDL's usual
		// renderer and the CPU's 3D, said so; deleting gpu-start.crashed tries again. CRT Emudriver, the modified
		// AMD driver arcade cabinets use, crashes there: not tried at all.)
		const std::string marker = data + "/gpu-start.crashed";
		const bool known_bad = sysinfo::gpu_names().find("CRT Emudriver") != std::string::npos;
		// (the marker holds the build that left it: another build's, a driver or a fix since, the GPU tried again)
		std::string marker_build;
		bool marker_there = false;
		if (SDL_GetPathInfo(marker.c_str(), nullptr)) {
			marker_there = true;
			size_t n = 0;
			if (void *d = SDL_LoadFile(marker.c_str(), &n)) { marker_build.assign(static_cast<const char *>(d), n); SDL_free(d); }
		}
		const bool crashed = known_bad || (marker_there && marker_build == settings::BUILD);
		if (known_bad) { fprintf(stderr, "SDL's gpu renderer: not tried with CRT Emudriver (its start crashes there): SDL's usual renderer, the CPU's 3D\n"); on_cpu_because("CRT Emudriver can't run the GPU renderer", ""); }
		else if (crashed) {
			fprintf(stderr, "SDL's gpu renderer: the last start closed while starting it (%s): SDL's usual renderer, the CPU's 3D (a new build tries it again)\n", marker.c_str());
			on_cpu_because("the graphics driver closed the game the last time the GPU started", RETRY);
		}
		else if (marker_there) fprintf(stderr, "SDL's gpu renderer: %s closed while starting it: this build tries it again\n", marker_build.empty() ? "an older build" : marker_build.c_str());
		if (!crashed) {
			if (SDL_IOStream *io = SDL_IOFromFile(marker.c_str(), "w")) { SDL_WriteIO(io, settings::BUILD, strlen(settings::BUILD)); SDL_CloseIO(io); }
			// (a driver whose 3D failed its check here before is passed over: Direct3D 12's for Vulkan, SDL's other on
			// Windows: some older cards draw nothing on one and are fine on the other. SDL_GPU_DRIVER chooses over this.)
			// (as VF1's and VF2's: Vulkan first on Windows for every card, Direct3D 12 if Vulkan won't start or draws no
			// 3D, remembered. A Radeon's first (vf2-recomp #19: an RX 9070 on Direct3D 12 took ~29 ms a picture, 30 a
			// second at any scale), then NVIDIA's too: vf2-recomp #21 and #22 (an RTX 3060: ~100 ms a 3D picture on
			// Direct3D 12, 57 a second on Vulkan), vf1-recomp #28 (17 ms a 2D layer's upload), #29 and #16. The player's
			// Display > Graphics driver: Direct3D 12 first instead.)
#ifdef _WIN32
			const std::string cards = sysinfo::gpu_names();   // (the hardware's: software adapters left out)
			const bool any_card = !cards.empty() && cards.find("Microsoft Basic Render") == std::string::npos;
#else
			const bool any_card = false;
#endif
			const bool vk_first = any_card && cfg.graphics == 0 && !failed_before("vulkan") && !getenv("SDL_GPU_DRIVER");
			const bool try_vulkan = (failed_before("direct3d12") || vk_first) && !failed_before("vulkan") && !getenv("SDL_GPU_DRIVER");
			if (try_vulkan) {
				fprintf(stderr, "SDL's gpu renderer: Vulkan (%s)\n", failed_before("direct3d12") ? "the 3D failed on Direct3D 12 here before" : "Windows, first");
				SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
			}
			ren = SDL_CreateRenderer(win, "gpu");
			if (!ren && try_vulkan) {   // (no Vulkan here: Direct3D 12 again)
				fprintf(stderr, "SDL's gpu renderer: Vulkan won't start (%s): Direct3D 12\n", SDL_GetError());
				cfg.gpu3d_failed += " vulkan";
				cfg_dirty = true;
				SDL_ResetHint(SDL_HINT_GPU_DRIVER);
				ren = SDL_CreateRenderer(win, "gpu");
			}
			// A first look, before anything is drawn (as VF2's: vf2-recomp #3, a GTX 970 drew no 3D on Direct3D 12 and
			// was fine on Vulkan): a made-up scene, one white quad over the screen, on the renderer's device and on the
			// CPU; the GPU's empty where the CPU's is drawn, the renderer made again on Vulkan at once, not from the next
			// start. Only that case: the full check, pixel for pixel, comes later on the game's own scene. 1 drawn, 0
			// empty, -1 not looked at. (VF3_GPU3D_NO_PROBE: not done; VF3_GPU3D_PROBE_ANY: on any driver, for tests.)
			auto draws_3d = [&](SDL_Renderer *r) -> int {
				SDL_GPUDevice *gd = r ? SDL_GetGPURendererDevice(r) : nullptr;
				if (!gd) return -1;
				R3dGpu dev;
				if (!dev.init(gd)) return -1;
				video::Scene sc;
				video::Viewport v;
				v.full_screen = true;
				v.left = -1; v.right = 1; v.bottom = -0.75f; v.top = 0.75f; v.cota = 1;
				video::Mesh m;
				const float q[6][2] = { { -20, -20 }, { 20, -20 }, { 20, 20 }, { -20, -20 }, { 20, 20 }, { -20, 20 } };
				for (auto &p : q) {
					video::Vertex x = {};
					x.pos[0] = p[0]; x.pos[1] = p[1]; x.pos[2] = -10;
					x.normal[2] = x.face_normal[2] = 1;
					x.colour[0] = x.colour[1] = x.colour[2] = x.colour[3] = 255;
					m.verts.push_back(x);
				}
				video::Model md;
				md.meshes = std::make_shared<std::vector<video::Mesh>>(1, m);
				md.matrix = video::Mat4::identity();
				v.models.push_back(md);
				sc.viewports.push_back(v);
				video::Raster cpu;
				cpu.resize(video::W, video::H);
				cpu.prepare(sc);
				if (!dev.render(sc, cpu, board)) return 0;
				std::vector<uint32_t> gp;
				if (!dev.read_output(gp)) return -1;
				cpu.render(sc, board.tex_ram.data());
				std::vector<uint32_t> cp(size_t(cpu.width()) * cpu.height());
				cpu.to_premultiplied(reinterpret_cast<uint8_t *>(cp.data()), size_t(cpu.width()) * 4);
				if (gp.size() != cp.size()) return -1;
				long long cpu_drawn = 0, gpu_drawn = 0;
				for (size_t i = 0; i < cp.size(); i++) { cpu_drawn += (cp[i] & 0xFFFFFF) != 0; gpu_drawn += (gp[i] & 0xFFFFFF) != 0; }
				const long long px = (long long)cp.size();
				if (getenv("VF3_GPU3D_PROBE_ANY")) fprintf(stderr, "SDL's gpu renderer: the probe: %lld of %lld pixels drawn on the CPU, %lld on the GPU\n", cpu_drawn, px, gpu_drawn);
				return cpu_drawn > px / 2 && gpu_drawn < px / 100 ? 0 : 1;
			};
			if (ren && !getenv("SDL_GPU_DRIVER") && !getenv("VF3_GPU3D_NO_PROBE") && !(failed_before("direct3d12") && failed_before("vulkan"))) {
				SDL_GPUDevice *gd = SDL_GetGPURendererDevice(ren);
				const char *drv = gd ? SDL_GetGPUDeviceDriver(gd) : nullptr;
				if (drv && !strcmp(drv, "vulkan") && vk_first && !failed_before("direct3d12") && draws_3d(ren) == 0) {
					// (Vulkan chosen first draws nothing: Direct3D 12, as before)
					fprintf(stderr, "SDL's gpu renderer: Vulkan draws no 3D here: Direct3D 12 instead\n");
					cfg.gpu3d_failed += (cfg.gpu3d_failed.empty() ? "" : " ") + std::string("vulkan");
					cfg_dirty = true;
					SDL_DestroyRenderer(ren);
					SDL_ResetHint(SDL_HINT_GPU_DRIVER);
					ren = SDL_CreateRenderer(win, "gpu");
				} else if (drv && (!strcmp(drv, "direct3d12") || getenv("VF3_GPU3D_PROBE_ANY")) && draws_3d(ren) == 0 && !strcmp(drv, "direct3d12")) {
					fprintf(stderr, "SDL's gpu renderer: Direct3D 12 draws no 3D here: Vulkan instead\n");
					if (!failed_before("direct3d12")) { cfg.gpu3d_failed += (cfg.gpu3d_failed.empty() ? "" : " ") + std::string("direct3d12"); cfg_dirty = true; }
					SDL_DestroyRenderer(ren);
					SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
					ren = SDL_CreateRenderer(win, "gpu");
					const int vk = ren ? draws_3d(ren) : -1;
					if (!ren || vk == 0) {   // (no Vulkan, or no 3D on it either: Direct3D 12 for the 2D, the 3D on the CPU)
						fprintf(stderr, "SDL's gpu renderer: Vulkan %s: Direct3D 12, the 3D on the CPU\n", ren ? "draws no 3D either" : "won't start");
						if (ren) SDL_DestroyRenderer(ren);
						if (!failed_before("vulkan")) { cfg.gpu3d_failed += " vulkan"; cfg_dirty = true; }
						SDL_ResetHint(SDL_HINT_GPU_DRIVER);
						ren = SDL_CreateRenderer(win, "gpu");
					} else fprintf(stderr, "SDL's gpu renderer: Vulkan draws the 3D here\n");
				}
			}
			SDL_RemovePath(marker.c_str());
			if (!ren) { fprintf(stderr, "SDL's gpu renderer: %s (SDL's usual renderer, the CPU's 3D)\n", SDL_GetError()); on_cpu_because("the GPU renderer wouldn't start", UPDATE); }
		}
	}
	if (!ren) ren = SDL_CreateRenderer(win, nullptr);
	if (!ren) return fail(std::string("Couldn't start the graphics: ") + SDL_GetError());
	SDL_SetRenderVSync(ren, 1);
	fprintf(stderr, "renderer: %s\n", SDL_GetRendererName(ren));

	// ---- the picture ----
	// The picture's shape: the chosen one, or the window's (fit: between 4:3 and 48:9, three screens side by side)
	auto window_aspect = [&] {
		int w = 0, h = 0;
		SDL_GetWindowSizeInPixels(win, &w, &h);
		return h > 0 ? std::clamp(float(w) / float(h), 4.0f / 3.0f, 48.0f / 9.0f) : 16.0f / 9.0f;
	};
	auto picture_aspect = [&] { return ASPECTS[aspect_i] > 0 ? ASPECTS[aspect_i] : window_aspect(); };
	int scale = cfg.scale;       // in use (the governor may lower it for this run)
	if (!scale) {
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(win, &pw, &ph);
		if (cfg.fullscreen)
			if (const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
				pw = int(dm->w * dm->pixel_density); ph = int(dm->h * dm->pixel_density);
			}
		const int cores = int(std::thread::hardware_concurrency());
		const int most = cores >= 8 ? 4 : cores >= 6 ? 3 : 2;   // (the first start's: 5x and 6x only when chosen)
		scale = std::clamp(int(std::ceil(ph / 384.0 - 0.1)), 2, most);   // (at least 2x, as VF1's and VF2's)
	}
	const int auto_scale = scale;   // the player's scale while settings.ini has none (0: worked out above)
	auto chosen_scale = [&] { return cfg.scale ? cfg.scale : auto_scale; };
	static video::Raster raster;
	// ---- the 3D on the GPU ----
	static R3dGpu gpu3d;
	struct { bool on = false, failed = false, checked = false; SDL_Texture *tex = nullptr; int w = 0, h = 0, k = 1; double cpu_ms = 0, gpu_ms = 0; } g3;
	if (r3d_mode != R3D_CPU) {
		const char *rn = SDL_GetRendererName(ren);
		SDL_GPUDevice *gd = rn && !strcmp(rn, "gpu") ? SDL_GetGPURendererDevice(ren) : nullptr;
		if (gd && failed_before(SDL_GetGPUDeviceDriver(gd))) {
			g3.failed = true;
			fprintf(stderr, "the 3D on the GPU: failed on %s here before (the CPU's 3D; choosing a 3D renderer in the menu tries again)\n", SDL_GetGPUDeviceDriver(gd));
			on_cpu_because("the GPU drew the 3D wrongly here", RETRY);
		} else if (gd) {
			const char *name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(gd), SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
			fprintf(stderr, "SDL_GPU: %s, %s\n", SDL_GetGPUDeviceDriver(gd), name ? name : "?");
			// (as VF1's: a "GPU" that is itself drawn by the CPU, a machine with no graphics driver, draws the 3D about 5
			// times slower than the CPU's own renderer, so Auto keeps the CPU there; the GPU chosen by hand still uses it)
			std::string lower = name ? name : "";
			for (char &c : lower) c = char(std::tolower((unsigned char)c));
			bool software = false;
			for (const char *k : { "llvmpipe", "lavapipe", "softpipe", "swiftshader", "software" }) software |= lower.find(k) != std::string::npos;
			const auto t0 = std::chrono::steady_clock::now();
			if (software && r3d_mode == R3D_AUTO)
				{ fprintf(stderr, "the 3D on the GPU: %s draws on the CPU, so Auto uses the CPU's 3D (faster here)\n", name); on_cpu_because("this computer has no graphics driver for its GPU", "Install the graphics card's driver."); }
			else if (gpu3d.init(gd)) {
				g3.on = true;
				fprintf(stderr, "the 3D on the GPU: started in %.0f ms (checked against the CPU's picture before it's trusted)\n",
						std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
			} else {
				g3.failed = true;
				fprintf(stderr, "the 3D on the GPU: %s (the CPU's 3D)\n", gpu3d.error().c_str());
				on_cpu_because("the GPU's 3D wouldn't start", UPDATE);
			}
		} else { fprintf(stderr, "the 3D on the GPU: no GPU renderer here (the CPU's 3D)\n"); on_cpu_because("the GPU renderer couldn't start here", UPDATE); }
	}
	fprintf(stderr, "the 3D: %s%s\n", R3D_NAMES[r3d_mode], getenv("VF3_3D") ? " (VF3_3D)" : "");
	// remember: the driver kept as failed for the next start (its start-up check, its textures); a picture failing
	// in play (a command buffer refused around a sleep or a GPU reset, once) only for this run: kept, a Steam Deck's
	// one bad moment put the 3D on the CPU at every start after, until the player changed it back
	auto g3_fail = [&](const std::string &why, bool remember = true) {
		fprintf(stderr, "the 3D on the GPU: %s: on the CPU from now on%s\n", why.c_str(), remember ? "" : " (this run)");
		// (its start's check or its textures: kept for this build, the menu tries again; a failure in play, a sleep or a
		// GPU reset: for this run only)
		if (remember) on_cpu_because(why.find("aren't the CPU's") != std::string::npos ? "the GPU drew the 3D wrongly here" : "the GPU's 3D failed here", RETRY);
		else on_cpu_because("the GPU's 3D stopped (after a sleep, or the graphics driver restarting)", "The next start tries the GPU again.");
		g3.on = false;
		g3.failed = true;
		if (!remember) return;
		// the driver it failed on, kept: the next start tries SDL's other (Direct3D 12, then Vulkan)
		const char *rn = SDL_GetRendererName(ren);
		if (SDL_GPUDevice *gd = rn && !strcmp(rn, "gpu") ? SDL_GetGPURendererDevice(ren) : nullptr)
			if (const char *drv = SDL_GetGPUDeviceDriver(gd); drv && !failed_before(drv)) {
				cfg.gpu3d_failed += (cfg.gpu3d_failed.empty() ? "" : " ") + std::string(drv);
				cfg_dirty = true;
				if (!strcmp(drv, "direct3d12") && !failed_before("vulkan")) fprintf(stderr, "the 3D on the GPU: the next start tries Vulkan\n");
			}
	};
	static video::Scene between;
	SDL_Texture *tex3d = nullptr, *tex_below = nullptr, *tex_above = nullptr;
	bool g3_shown = false;   // the picture on show is the GPU's
	uint32_t side_colour[2] = { 0xFF000000u, 0xFF000000u };   // widescreen's bars beside the 2D (left, right)
	bool sky_wide = false;
	float bg_shift_x = 0, bg_shift_y = 0;   // the layers below the 3D moved back towards the frame before (draw_below)
	// the layers below the 3D across the picture's width (VF3_WIDE_SKY=0: never)
	const bool wide_sky = !getenv("VF3_WIDE_SKY") || atoi(getenv("VF3_WIDE_SKY")) != 0;
	// Widescreen's sides where the 2D over the 3D is opaque at the 4:3 edge (a close-up's frame, the title): that
	// layer's edge carried on across the side, over the 3D (the game draws its stage behind such screens, hidden in
	// 4:3); runs of rows of one colour
	struct SideRun { int y0, y1; uint32_t c; };
	std::vector<SideRun> side_cover[2];
	// what the pictures cost, for the log: a line every 10 s (2 s with VF3_PACE_LOG)
	struct { uint64_t t0 = 0; int pictures = 0, frames = 0; double draw_ms = 0, emu_ms = 0, upload_ms = 0, gpu_ms = 0, stopped_s = 0; int slow[4] = {}; double slow_work_ms = 0, prep_ms = 0, g3_wall_ms = 0, interp_ms = 0; } pace;   // (slow: passes over 1.5 refreshes, by what they ran: nothing but a picture, a frame's first half, its second, a whole frame)
	uint64_t between_n = 0, cut_n = 0;   // smooth motion's pictures between frames, and those shown as the frame (a cut)
	int tex_w = 0, tex_h = 0;
	auto make_2d = [&](SDL_Texture *&t, int w) {
		t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, video::H);
		SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
	};
	make_2d(tex_below, video::WW);
	SDL_SetTextureScaleMode(tex_below, cfg.smooth_bg ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
	video::Raster::arcade_lod = !cfg.sharp_textures;
	make_2d(tex_above, video::W);
	// A picture into a streaming texture: on Direct3D 11 into the texture itself, mapped (d3d11_map.h: SDL's
	// lock and update would make a new staging texture each time), else locked; write(pixels, pitch)
	auto send = [&](SDL_Texture *t, auto &&write) {
		void *p = nullptr;
		int pitch = 0;
		if (d3d11_map(t, &p, &pitch)) { write(static_cast<uint8_t *>(p), size_t(pitch)); d3d11_unmap(t); }
		else if (SDL_LockTexture(t, nullptr, &p, &pitch)) { write(static_cast<uint8_t *>(p), size_t(pitch)); SDL_UnlockTexture(t); }
	};
	auto send_2d = [&](SDL_Texture *t, const std::vector<uint32_t> &src, int w) {
		send(t, [&](uint8_t *p, size_t pitch) {
			for (int y = 0; y < video::H; y++) memcpy(p + size_t(y) * pitch, src.data() + size_t(y) * w, size_t(w) * 4);
		});
	};
	// the 3D's size: 384 lines a scale, as wide as the picture's shape
	// antialiasing in use: the player's, less where the picture drawn would be over 8 times the arcade's each way
	int gov_aa_cut = 0;   // the governor's: steps below the player's antialiasing (cut before the scale)
	auto aa_now = [&] {
		int k = std::clamp(cfg.aa - gov_aa_cut, 1, 3);
		while (k > 1 && scale * k > 8) k--;
		return k;
	};
	// the picture's size as shown (the 3D's drawn aa_now() times that each way)
	auto size_3d = [&](int &w, int &h, int &gw) {
		h = 384 * scale;
		w = int(std::lround(h * picture_aspect())) & ~1;
		gw = int(std::lround(h * 4.0 / 3.0));
		w = std::max(w, gw);
	};
	auto ensure_3d = [&]() -> bool {
		int w, h, gw;
		size_3d(w, h, gw);
		raster.resize(w * aa_now(), h * aa_now(), gw * aa_now());
		if (tex3d && tex_w == w && tex_h == h) return true;
		if (tex3d) SDL_DestroyTexture(tex3d);
		for (;;) {
			size_3d(w, h, gw);
			tex3d = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
			if (tex3d || scale <= 1) break;
			fprintf(stderr, "a %d x %d picture is too large here (%s): trying %dx\n", w, h, SDL_GetError(), scale - 1);
			scale--;
		}
		if (!tex3d) return false;
		raster.resize(w * aa_now(), h * aa_now(), gw * aa_now());
		SDL_SetTextureBlendMode(tex3d, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
		SDL_SetTextureScaleMode(tex3d, SDL_SCALEMODE_LINEAR);
		tex_w = w; tex_h = h;
		return true;
	};
	if (!ensure_3d()) { gpu3d.shutdown(); return fail("Couldn't make the picture's texture"); }   // (the GPU's objects let go before the renderer)
	// a picture drawn: the 3D (smooth motion: between the last two frames, t from the older) and the 2D layers
	double test_load_ms = 0, test_load_s = 0;
	if (const char *tl = getenv("VF3_TEST_LOAD")) sscanf(tl, "%lf:%lf", &test_load_ms, &test_load_s);
	// the GPU's output, as a texture of the renderer's (made again at a new size)
	auto g3_texture = [&]() -> bool {
		const int k = aa_now(), ow = raster.width() / k, oh = raster.height() / k;
		if (g3.tex && g3.w == ow && g3.h == oh && g3.k == k) return true;
		if (g3.tex) { SDL_DestroyTexture(g3.tex); g3.tex = nullptr; }
		SDL_GPUTexture *gt = gpu3d.output(raster.width(), raster.height(), k);
		if (!gt) { g3_fail(gpu3d.error()); return false; }
		SDL_PropertiesID pr = SDL_CreateProperties();
		SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
		SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
		SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, ow);
		SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, oh);
		SDL_SetPointerProperty(pr, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, gt);
		g3.tex = SDL_CreateTextureWithProperties(ren, pr);
		SDL_DestroyProperties(pr);
		if (!g3.tex) { g3_fail(std::string("the renderer won't show its pictures: ") + SDL_GetError()); return false; }
		SDL_SetTextureBlendMode(g3.tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
		SDL_SetTextureScaleMode(g3.tex, SDL_SCALEMODE_LINEAR);
		g3.w = ow; g3.h = oh; g3.k = k;
		return true;
	};
	// The start's check: the GPU's picture of the game's own scene against the CPU's. Judged by how wrong, not how
	// many (as VF2's): colours off by a little, or edges a hair apart, anywhere, are a driver's rounding (NVIDIA's on
	// Direct3D 12 puts pixels a hair off); only a picture plainly wrong goes to the CPU: over 1% of it with colours off
	// by more than GPU_ROUNDING_COLOUR, or over 5% drawn on one and not the other (a GPU drawing nothing). When the
	// player chose the GPU (not Auto): 10% and 25%. (A driver's rounding is a few pixels: well inside Auto's 1%;
	// looser, a GPU drawing the fighters' textures wrong would pass.)
	constexpr long long GPU_ROUNDING_PX = 64;
	constexpr int GPU_ROUNDING_COLOUR = 8;
	constexpr double GPU_WRONG_COLOUR = 0.01, GPU_WRONG_COVER = 0.05, GPU_WRONG_COLOUR_CHOSEN = 0.10, GPU_WRONG_COVER_CHOSEN = 0.25;
	auto g3_check = [&](const video::Scene &s) -> bool {
		std::vector<uint32_t> gp;
		if (!gpu3d.read_output(gp)) { g3_fail("its picture couldn't be read back"); return false; }
		raster.render(s, game.view.tex.data());
		const int k = aa_now(), cw = raster.width() / k, ch = raster.height() / k;
		std::vector<uint32_t> cp(size_t(cw) * ch);
		raster.to_premultiplied(reinterpret_cast<uint8_t *>(cp.data()), size_t(cw) * 4, k);
		long long n = 0, drawn = 0, cover = 0, big = 0;
		int most = 0;
		for (size_t i = 0; i < cp.size(); i++) {
			if (cp[i] >> 24) drawn++;
			if (cp[i] == gp[i]) continue;
			n++;
			if (((cp[i] >> 24) == 0) != ((gp[i] >> 24) == 0)) { cover++; continue; }
			int d = 0;
			for (int sh = 0; sh < 32; sh += 8) d = std::max(d, std::abs(int((cp[i] >> sh) & 255) - int((gp[i] >> sh) & 255)));
			if (d > GPU_ROUNDING_COLOUR) big++;
			most = std::max(most, d);
		}
		const long long px = (long long)cp.size();
		const bool chosen = r3d_mode == R3D_GPU;
		const long long ok = std::max(GPU_ROUNDING_PX, (long long)(px * (chosen ? GPU_WRONG_COLOUR_CHOSEN : GPU_WRONG_COLOUR)));
		const long long ok_cover = std::max(GPU_ROUNDING_PX, (long long)(px * (chosen ? GPU_WRONG_COVER_CHOSEN : GPU_WRONG_COVER)));
		const bool keep = big <= ok && cover <= ok_cover;
		fprintf(stderr, "the 3D on the GPU: its picture (%d x %d, %lld pixels of 3D) against the CPU's: %lld pixels differ (%lld drawn on one only, %lld by more than %d, colours by %d at most; up to %lld and %lld kept): %s\n",
				cw, ch, drawn, n, cover, big, GPU_ROUNDING_COLOUR, most, ok_cover, ok, keep ? "the GPU's" : "the CPU's");
		if (getenv("VF3_GPU_CHECK_DIR")) {
			const std::string d = getenv("VF3_GPU_CHECK_DIR");
			write_png((d + "/gpu.png").c_str(), gp.data(), cw, ch);
			write_png((d + "/cpu.png").c_str(), cp.data(), cw, ch);
		}
		if (!keep) { g3_fail("its pictures aren't the CPU's"); return false; }
		return true;
	};
	double picture_fixed_ms = 0;   // the last picture's in-between and triangles prepared (the governor's)
	auto draw_picture = [&](double t, bool new_2d) {
		ensure_3d();
		const double fixed_before = pace.interp_ms + pace.prep_ms;
		picture_fixed_ms = 0;
		// the stage's sky where the 3D camera is now, not where it was 3 frames before (sky_lead.h)
		bg_shift_x = -game.view.sky.at_x(1);
		bg_shift_y = -game.view.sky.at_y(1);
		const video::Scene *s = &game.view.cur();
		// (at t = 1 too with smooth motion: the frame itself, but for a flipbook's step still fading in)
		if ((t < 1.0 || cfg.smooth) && game.view.have_prev) {
			const auto ti = std::chrono::steady_clock::now();
			const bool blended = video::interpolate(game.view.prev(), game.view.cur(), float(t), between);
			pace.interp_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ti).count();
			picture_fixed_ms = pace.interp_ms - fixed_before;
			if (blended) {
				s = &between;
				between_n++;
				if (game.view.bg_moved) { bg_shift_x = float(1 - t) * game.view.bg_dx; bg_shift_y = float(1 - t) * game.view.bg_dy; }
				else bg_shift_x = bg_shift_y = 0;
				bg_shift_x -= game.view.sky.at_x(float(t));
				bg_shift_y -= game.view.sky.at_y(float(t));
			}
			else cut_n++;
		}
		bool on_gpu = false;
		// (not before the first frame's copy: with no sound to wait on, the first picture can come before any frame,
		// and the GPU's first upload is the whole texture memory, read from that copy)
		if (g3.on && g3_texture() && !game.view.tex.empty()) {
			const auto tg = std::chrono::steady_clock::now();
			raster.prepare(*s);
			const auto tp = std::chrono::steady_clock::now();
			pace.prep_ms += std::chrono::duration<double, std::milli>(tp - tg).count();
			picture_fixed_ms = pace.interp_ms + pace.prep_ms - fixed_before;
			const bool rendered = gpu3d.render(*s, raster, game.view.tex, game.view.uploads);
			pace.g3_wall_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
			if (!rendered) g3_fail(gpu3d.error(), false);
			else {
				on_gpu = true;
				g3.gpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tg).count();
				// checked once, on a scene with enough in it
				size_t polys = raster.tris.size();
				if (!g3.checked && polys > 500) { g3.checked = true; on_gpu = g3_check(*s); }
				// (checked: the CPU's drawing buffers let go, made again if it ever draws: at 2x with 3x antialiasing on
				// a Steam Deck they were near 500 MB of its shared memory, kept for nothing)
				if (on_gpu && g3.checked) raster.drop_buffers();
			}
		}
		if (!on_gpu) {
			game.view.uploads.clear();
			raster.render(*s, game.view.tex.data());
			const uint64_t tu = SDL_GetTicksNS();
			send(tex3d, [&](uint8_t *p, size_t pitch) { raster.to_premultiplied(p, pitch, aa_now()); });
			pace.upload_ms += double(SDL_GetTicksNS() - tu) / 1e6;
		} else pace.gpu_ms += gpu3d.last_cpu_ms;
		g3_shown = on_gpu;
		if (static const bool pic_log = getenv("VF3_PIC_LOG") != nullptr; pic_log) {   // (tests: each picture's makings)
			size_t models = 0;
			for (auto &v : s->viewports) models += v.models.size();
			fprintf(stderr, "pic f %llu t %.2f blended %d models %zu vps %zu full %d sky %d side %08x blank %d\n", (unsigned long long)game_frame, t,
			        int(s == &between), models, s->viewports.size(), int(game.view.full_3d), int(sky_wide), side_colour[0], int(s->blank));
		}
		if (new_2d) {
			// A stage: the 3D full width and the 2D over it clear of the 4:3 edges (a fight's HUD never reaches them)
			// covering under half of the rest (a fight's HUD and a replay's under 10%, the ranking's a third; the
			// close-ups' and the next match's pictures, 2D over a hidden stage, all of it). Rows opaque all the way
			// across are a replay's letterbox bands: they don't count.
			int edge_rows = 0, rows = 0;
			long covered = 0;
			bool band[video::H];
			for (int y = 0; y < video::H; y++) {
				const uint32_t *row = &game.view.above[size_t(y) * video::W];
				int c = 0;
				for (int x = 0; x < video::W; x++) c += (row[x] >> 24) != 0;
				band[y] = c == video::W;
				if (band[y]) continue;
				rows++;
				covered += c;
				edge_rows += (row[0] >> 24) != 0 || (row[video::W - 1] >> 24) != 0;
			}
			const bool seen = game.view.full_3d && rows > 0 && edge_rows * 20 < video::H && covered * 100 < long(rows) * video::W * 50;
			// a stage stays one through a round's opening wipe and the like (2D over part of it for under 20 frames);
			// a picture over all of it (the next match's, a close-up) or the 3D gone ends it at once. In a match a
			// stage shows from its second frame, and is held only once seen 8 frames running, more than a rollback
			// goes back: a frame guessed wrong (the character select's start, the other player not yet known to have
			// pressed Start) looked like a stage, the hold kept the sides on for 20 frames, and the select flashed
			// 16:9; waiting the 8 frames before showing it, a stage's sides came in visibly late
			static int stage_hold = 0, stage_seen = 0;
			stage_seen = seen ? stage_seen + 1 : 0;
			const bool match = online.playing();
			bool stage = seen && (stage_hold > 0 || stage_seen >= (match ? 2 : 1));
			if (stage && (!match || stage_seen >= 8)) stage_hold = 20;
			else if (stage_hold > 0 && game.view.full_3d && covered * 100 < long(std::max(rows, 1)) * video::W * 50) { stage_hold--; stage = true; }
			else stage_hold = 0;
			// On a stage the sky is stretched across the picture and the letterbox bands carried across the sides;
			// elsewhere the picture is 4:3 with bars the full height over whatever is beside it: the 2D's edge colour
			// where that is one colour all the way down (the SEGA logo's white, a fade), else black
			sky_wide = stage && wide_sky;
			for (int s = 0; s < 2; s++) {
				const int x = s ? video::W - 1 : 0;
				auto at = [&](int y) {
					const uint32_t a = game.view.above[size_t(y) * video::W + x], b = game.view.below[size_t(y) * video::W + x];
					return (a >> 24) ? a : (b >> 24) ? b : 0xFF000000u;
				};
				side_colour[s] = 0xFF000000u;
				side_cover[s].clear();
				if (stage) {
					for (int y = 0; y < video::H; y++) {
						if (!band[y]) continue;
						const uint32_t c = game.view.above[size_t(y) * video::W + x];
						if (!side_cover[s].empty() && side_cover[s].back().y1 == y && side_cover[s].back().c == c) side_cover[s].back().y1++;
						else side_cover[s].push_back({ y, y + 1, c });
					}
				} else {
					const uint32_t c0 = at(0);
					bool one = true;
					for (int y = 1; y < video::H && one; y++) one = at(y) == c0;
					side_colour[s] = one ? c0 : 0xFF000000u;
					side_cover[s].push_back({ 0, video::H, side_colour[s] });
				}
			}
			const uint64_t tu = SDL_GetTicksNS();
			send_2d(tex_below, game.view.below_wide, video::WW);
			send_2d(tex_above, game.view.above, video::W);
			pace.upload_ms += double(SDL_GetTicksNS() - tu) / 1e6;
		}
	};
	// Where the picture goes in the window: as large as fits, its shape kept
	auto picture_rect = [&](SDL_FRect &all, SDL_FRect &mid) {
		int pw = 0, ph = 0;
		SDL_GetRenderOutputSize(ren, &pw, &ph);
		const float a = picture_aspect();
		float w = float(pw), h = float(ph);
		if (w / h > a) w = h * a; else h = w / a;
		all = { (pw - w) / 2, (ph - h) / 2, w, h };
		const float mw = h * 4.0f / 3.0f;
		mid = { all.x + (w - mw) / 2, all.y, mw, h };
	};
	// The layers below the 3D (the sky, the city). Between frames (smooth motion) moved on from the frame before
	// as far as the picture is between them, to the fraction of a pixel, as the 3D is (bg_shift: in its pixels).
	// Widescreen, on a stage: across the whole picture, 1:1 (turning with the 3D), the panorama carried on past the
	// 4:3 screen's edges (tilegen.h); stretched only past the widened layer's own edges (wider than about 2.6:1).
	// Else 4:3.
	auto draw_below = [&](const SDL_FRect &all, const SDL_FRect &mid) {
		const float k = mid.w / video::W;
		// the picture's sides, in the layer's pixels from the 4:3 screen's left
		const float l = sky_wide ? (all.x - mid.x) / k : 0.0f, r = sky_wide ? (all.x + all.w - mid.x) / k : float(video::W);
		const float u0 = std::max(video::EXT + l - bg_shift_x, 0.0f), u1 = std::min(video::EXT + r - bg_shift_x, float(video::WW));
		const float x0 = mid.x + l * k, x1 = mid.x + r * k;
		// Moved down by bg_shift_y (or up): the band it leaves at the top (or bottom) its edge row's, carried on (the
		// sky's: a camera tilting fast, a jump attack's, moved it further than the 12 rows that were stretched to cover
		// it, and left a bar of black)
		const float sy = std::clamp(bg_shift_y, -float(video::H) + 1, float(video::H) - 1);
		auto band = [&](float v0, float v1, float d0, float d1) {
			if (v1 - v0 < 0.01f || d1 - d0 < 0.01f) return;
			const SDL_FRect src = { u0, v0, u1 - u0, v1 - v0 };
			const SDL_FRect dst = { x0, mid.y + d0 * k, x1 - x0, (d1 - d0) * k };
			SDL_RenderTexture(ren, tex_below, &src, &dst);
		};
		if (sy > 0) {
			band(0, 1, 0, sy);
			band(0, float(video::H) - sy, sy, float(video::H));
		} else {
			band(-sy, float(video::H), 0, float(video::H) + sy);
			band(float(video::H) - 1, float(video::H), float(video::H) + sy, float(video::H));
		}
	};
	// Screen filters (as VF2's): a picture of the monitor's glass multiplied over the game's, at the window's own
	// pixels (made again when the picture's size changes). Scanlines darkens between the arcade's 384 lines; CRT adds
	// an aperture grille (red, green and blue stripes) and slightly darker corners.
	static const char *FILTER_NAMES[] = { "Off", "Scanlines", "CRT" };
	SDL_Texture *glass = nullptr;
	int glass_w = 0, glass_h = 0, glass_kind = 0;
	auto draw_filter = [&](const SDL_FRect &r) {
		if (!cfg.filter) return;
		const int w = int(std::lround(r.w)), h = int(std::lround(r.h));
		if (w < 1 || h < 1) return;
		if (!glass || w != glass_w || h != glass_h || cfg.filter != glass_kind) {
			if (glass) SDL_DestroyTexture(glass);
			glass = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
			if (!glass) return;
			glass_w = w; glass_h = h; glass_kind = cfg.filter;
			const bool crt = cfg.filter == 2;
			// the lines want a couple of pixels each to show: they fade in from 1.5 to 3 per line
			const float per_line = h / 384.0f;
			const float depth = (crt ? 0.45f : 0.6f) * std::clamp((per_line - 1.5f) / 1.5f, 0.0f, 1.0f);
			// a whole number of screen pixels a line (the picture's lines to the nearest), so every line looks the
			// same: a fraction of a line apart (the Steam Deck's 800 lines: 2.08 a line) beats into uneven bands
			const int period = std::max(1, int(std::lround(per_line)));
			std::vector<float> row(static_cast<size_t>(h));
			for (int y = 0; y < h; y++) {
				const float f = period == 2 ? float(y % 2) * 0.5f : (float(y % period) + 0.5f) / float(period);
				row[size_t(y)] = 1 - depth * (1 - std::sin(f * 3.14159265f));   // 1 mid-line, darker at its edges
			}
			auto glass_at = [&](int x, int y, float rgb[3]) {
				rgb[0] = rgb[1] = rgb[2] = row[size_t(y)];
				if (!crt) return;
				for (int c = 0; c < 3; c++) if (c != x % 3) rgb[c] *= 0.78f;   // the grille: one colour per pixel column
				const float dx = (x + 0.5f) / w * 2 - 1, dy = (y + 0.5f) / h * 2 - 1;
				const float d = std::min(1.0f, (dx * dx + dy * dy) / 2);
				for (int c = 0; c < 3; c++) rgb[c] *= 1 - 0.22f * d * d;      // the corners
			};
			// Multiplying only darkens: drawn as twice the product (dst * glass * 2), the glass also brightens the
			// middle of each line, so the picture keeps about its brightness
			const SDL_BlendMode times2 = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_DST_COLOR, SDL_BLENDFACTOR_SRC_COLOR,
				SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
			const bool doubled = SDL_SetTextureBlendMode(glass, times2);
			if (!doubled) SDL_SetTextureBlendMode(glass, SDL_BLENDMODE_MOD);   // a renderer without it: darker
			float gain = 1;
			if (doubled) {
				double sum = 0;
				int n = 0;
				for (int y = 0; y < h; y += 3)
					for (int x = 0; x < w; x += 7) { float v[3]; glass_at(x, y, v); sum += v[0] + v[1] + v[2]; n += 3; }
				gain = std::min(1.4f, float(n / sum));
			}
			const float k = 255 * gain / (doubled ? 2 : 1);
			std::vector<uint32_t> px(size_t(w) * h);
			parallel_rows(h, [&](int y0, int y1) {
				for (int y = y0; y < y1; y++)
					for (int x = 0; x < w; x++) {
						float v[3];
						glass_at(x, y, v);
						uint32_t c[3];
						for (int i = 0; i < 3; i++) c[i] = uint32_t(std::min(255.0f, v[i] * k + 0.5f));
						px[size_t(y) * w + x] = 0xff000000u | c[0] << 16 | c[1] << 8 | c[2];
					}
			}, 4);
			SDL_UpdateTexture(glass, nullptr, px.data(), w * 4);
			SDL_SetTextureScaleMode(glass, SDL_SCALEMODE_NEAREST);
		}
		SDL_RenderTexture(ren, glass, nullptr, &r);   // over the picture, under the menu
	};
	// CRT colours (as Supermodel's CRT colours, issue #2): the picture as an arcade monitor showed its colours, its
	// phosphors' and its white's (a Japanese cabinet's 9300 K: bluer whites, deeper reds) on today's sRGB screen. The
	// picture is put together in a texture, then drawn into the window through r3d.hlsl's FS_COLOUR: the monitor's
	// gamma (or sRGB's curve) to linear light, the matrix (Dan Mons' colour_matrix_adaptations, as Supermodel's), and
	// sRGB's curve out. On SDL's GPU renderer only (its custom render state): not on SDL's other renderers.
	struct crt_colours { const char *name; float gamma; float m[9]; };   // (gamma 0: sRGB's curve)
	static const crt_colours CRT_COLOURS[] = {
		{ "Off", 0, {} },
		{ "Sega arcade (ARI, 9300 K)", 2.5f, { 1.3272128f, -0.3802109f, -0.1003608f, -0.0241849f, 0.9544506f, 0.0807359f, -0.0239586f, -0.0409736f, 1.4076564f } },
		{ "Sony PVM-20M2U (9300 K)", 2.25f, { 0.9241392f, -0.0690701f, -0.0084279f, 0.0231962f, 0.9729222f, 0.0148832f, -0.0054876f, 0.0098221f, 1.3383897f } },
		{ "BT.601 NTSC (9300 K, Japan)", 0, { 0.7822491f, 0.0506169f, 0.0137752f, 0.0147969f, 0.9741745f, 0.0220302f, -0.0013501f, -0.0044077f, 1.3484820f } },
		{ "BT.601 NTSC (6500 K, US)", 0, { 0.9395421f, 0.0501814f, 0.0102766f, 0.0177722f, 0.9657929f, 0.0164349f, -0.0016216f, -0.0043697f, 1.0059913f } },
		{ "BT.601 PAL (6500 K, Europe, Australia)", 0, { 1.0440432f, -0.0440432f, 0, 0, 1, 0, 0, 0.0117934f, 0.9882066f } },
	};
	const int N_CRT_COLOURS = int(sizeof CRT_COLOURS / sizeof CRT_COLOURS[0]);
	SDL_GPURenderState *crt_state = nullptr;
	SDL_GPUShader *crt_shader = nullptr;
	SDL_Texture *crt_tex = nullptr;
	int crt_tex_w = 0, crt_tex_h = 0, crt_set = -1;
	bool crt_failed = false;
	// the texture to put the picture together in, the CRT colours on (null: straight into the window)
	auto crt_target = [&]() -> SDL_Texture * {
		if (!cfg.crt_colours || crt_failed) return nullptr;
		if (!crt_state) {
			SDL_GPUDevice *gd = SDL_GetGPURendererDevice(ren);
			if (gd) crt_shader = colour_shader(gd);
			SDL_GPURenderStateCreateInfo ci = {};
			ci.fragment_shader = crt_shader;
			if (crt_shader) crt_state = SDL_CreateGPURenderState(ren, &ci);
			if (!crt_state) {
				fprintf(stderr, "CRT colours: not on this renderer (%s)\n", gd ? SDL_GetError() : "SDL's GPU renderer only");
				crt_failed = true;
				return nullptr;
			}
		}
		if (crt_set != cfg.crt_colours) {
			const crt_colours &c = CRT_COLOURS[cfg.crt_colours];
			const float u[12] = { c.m[0], c.m[1], c.m[2], c.gamma, c.m[3], c.m[4], c.m[5], 0, c.m[6], c.m[7], c.m[8], 0 };
			SDL_SetGPURenderStateFragmentUniforms(crt_state, 0, u, sizeof u);
			crt_set = cfg.crt_colours;
		}
		int pw = 0, ph = 0;
		SDL_GetRenderOutputSize(ren, &pw, &ph);
		if (pw < 1 || ph < 1) return nullptr;
		if (!crt_tex || pw != crt_tex_w || ph != crt_tex_h) {
			if (crt_tex) SDL_DestroyTexture(crt_tex);
			crt_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, pw, ph);
			if (!crt_tex) return nullptr;
			SDL_SetTextureScaleMode(crt_tex, SDL_SCALEMODE_NEAREST);
			SDL_SetTextureBlendMode(crt_tex, SDL_BLENDMODE_NONE);
			crt_tex_w = pw; crt_tex_h = ph;
		}
		return crt_tex;
	};
	auto compose = [&](bool filter = true) {
		SDL_Texture *crt = crt_target();
		if (crt) SDL_SetRenderTarget(ren, crt);
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		SDL_FRect all, mid;
		picture_rect(all, mid);
		for (int s = 0; s < 2; s++) {
			const uint32_t v = side_colour[s];
			SDL_SetRenderDrawColor(ren, uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), 255);
			const SDL_FRect bar = s ? SDL_FRect{ mid.x + mid.w, all.y, all.x + all.w - (mid.x + mid.w), all.h } : SDL_FRect{ all.x, all.y, mid.x - all.x, all.h };
			if (bar.w > 0) SDL_RenderFillRect(ren, &bar);
		}
		draw_below(all, mid);
		SDL_RenderTexture(ren, g3_shown && g3.tex ? g3.tex : tex3d, nullptr, &all);
		const float ky = mid.h / video::H;
		for (int s = 0; s < 2; s++)
			for (const SideRun &r : side_cover[s]) {
				SDL_SetRenderDrawColor(ren, uint8_t(r.c), uint8_t(r.c >> 8), uint8_t(r.c >> 16), 255);
				const float y0 = mid.y + r.y0 * ky, y1 = mid.y + r.y1 * ky;
				const SDL_FRect bar = s ? SDL_FRect{ mid.x + mid.w, y0, all.x + all.w - (mid.x + mid.w), y1 - y0 }
				                        : SDL_FRect{ all.x, y0, mid.x - all.x, y1 - y0 };
				if (bar.w > 0) SDL_RenderFillRect(ren, &bar);
			}
		SDL_RenderTexture(ren, tex_above, nullptr, &mid);
		if (crt) {
			SDL_SetRenderTarget(ren, nullptr);
			SDL_SetGPURenderState(ren, crt_state);
			SDL_RenderTexture(ren, crt, nullptr, nullptr);
			SDL_SetGPURenderState(ren, nullptr);
		}
		if (filter) draw_filter(all);
	};
	auto read_window = [&](const std::string &path, const SDL_Rect *area = nullptr) {
		SDL_Surface *s = SDL_RenderReadPixels(ren, area);
		if (!s) return;
		SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
		SDL_DestroySurface(s);
		if (!c) return;
		std::vector<uint32_t> px(size_t(c->w) * c->h);
		for (int y = 0; y < c->h; y++) memcpy(&px[size_t(y) * c->w], static_cast<uint8_t *>(c->pixels) + size_t(y) * c->pitch, size_t(c->w) * 4);
		write_png(path.c_str(), px.data(), c->w, c->h);
		SDL_DestroySurface(c);
	};
	// the frame as the game made it (1x, 4:3): the 2D with the 3D between, for shots and tests
	auto write_frame = [&](const std::string &path) {
		static video::Raster one;
		one.resize(video::W, video::H);
		one.render(game.cur(), board.tex_ram.data());
		std::vector<uint32_t> pic(video::W * video::H);
		for (int i = 0; i < video::W * video::H; i++) pic[i] = (game.below[i] >> 24) ? game.below[i] : 0xFF000000u;
		one.compose(pic.data(), pic.data());
		for (int i = 0; i < video::W * video::H; i++) if (game.above[i] >> 24) pic[i] = game.above[i];
		write_png(path.c_str(), pic.data(), video::W, video::H);
	};

	// ---- sound ----
	SDL_AudioStream *audio = nullptr;
	if (sound) {
		SDL_AudioSpec spec{ SDL_AUDIO_S16, 2, SOUND_RATE };
		audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (audio) {
			if (hidden) SDL_SetAudioStreamGain(audio, 0.0f);
			SDL_ResumeAudioStreamDevice(audio);
		} else fprintf(stderr, "no audio: %s\n", SDL_GetError());
	}
	game.sound_wait();
	game.sound.gain = LEVEL * cfg.volume / 100.0;
	game.sound.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f);
	const int frame_bytes = int(SOUND_RATE / FRAME_HZ) * 4;
	constexpr double MATCH_QUEUED = 4.0;   // (a match's: the sound thread keeps it there, to the real clock)
	constexpr double QUEUED = 3.0;   // (3.5 when the sound was made in the frame's own time: it's a frame later now; 2.5 kept it near
	                                 // the frame and a half that runs an extra frame, two frames in a pass)

	// ---- pads ----
	SDL_Gamepad *pads[2] = { nullptr, nullptr };
	bool pads_swapped = false;
	auto guid_of = [](SDL_JoystickID id) {
		char buf[64];
		SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), buf, sizeof buf);
		return std::string(buf);
	};
	auto open_pads = [&] {
		for (auto &p : pads) if (p) { SDL_CloseGamepad(p); p = nullptr; }
		int n = 0;
		SDL_JoystickID *ids = SDL_GetGamepads(&n);
		std::vector<bool> taken(size_t(std::max(n, 0)), false);
		for (int p = 0; p < 2; p++) {
			const std::string &c = cfg.ctl[p].pad_choice;
			if (c == "auto" || c == "none") continue;
			for (int i = 0; i < n; i++)
				if (!taken[i] && guid_of(ids[i]) == c) { pads[p] = SDL_OpenGamepad(ids[i]); taken[i] = true; break; }
		}
		// then "auto" players take the free ones in order, then a player whose chosen pad isn't there (or shows up
		// differently: a pad in another mode has another GUID) takes the next free one too (as VF1's and VF2's)
		for (int pass = 0; pass < 2; pass++)
			for (int p = 0; p < 2; p++) {
				const std::string &c = cfg.ctl[p].pad_choice;
				if (pads[p] || c == "none" || (pass == 0 && c != "auto")) continue;
				for (int i = 0; i < n; i++)
					if (!taken[i]) { pads[p] = SDL_OpenGamepad(ids[i]); taken[i] = true; break; }
			}
		SDL_free(ids);
		if (pads_swapped && pads[0] && pads[1]) std::swap(pads[0], pads[1]);
		for (int p = 0; p < 2; p++)
			if (pads[p]) fprintf(stderr, "player %d: %s\n", p + 1, SDL_GetGamepadName(pads[p]));
	};
	open_pads();

	// ---- the menu ----
	auto menu_owner = std::make_unique<game_menu>();   // (let go before the renderer it draws with)
	game_menu &menu = *menu_owner;
	// the countries' flags (assets/ui/flags.png), for the HUD's: each a 12 x 9 of its 48 x 36, a box of 4 x 4 for a pixel
	SDL_Surface *flag_atlas = nullptr;
	if (SDL_Surface *f = SDL_LoadSurface((find_assets() + "/ui/flags.png").c_str())) {
		flag_atlas = SDL_ConvertSurface(f, SDL_PIXELFORMAT_ABGR8888);
		SDL_DestroySurface(f);
	}
	auto hud_flag = [&](const std::string &country) {
		video::HudFlag fl;
		const int i = flags::index_of(country);
		if (!flag_atlas || i < 0) return fl;
		const int x0 = i % flags::COLS * flags::CELL_W + 1, y0 = i / flags::COLS * flags::CELL_H + 1;
		if (x0 + flags::W > flag_atlas->w || y0 + flags::H > flag_atlas->h) return fl;
		fl.w = flags::W / 4;
		fl.h = flags::H / 4;
		fl.px.resize(size_t(fl.w) * size_t(fl.h));
		for (int y = 0; y < fl.h; y++)
			for (int x = 0; x < fl.w; x++) {
				unsigned sum[4] = {};
				for (int dy = 0; dy < 4; dy++)
					for (int dx = 0; dx < 4; dx++) {
						const uint8_t *q = static_cast<const uint8_t *>(flag_atlas->pixels) + size_t(y0 + y * 4 + dy) * size_t(flag_atlas->pitch) + size_t(x0 + x * 4 + dx) * 4;
						for (int c = 0; c < 3; c++) sum[c] += q[c] * q[3];   // (by their cover: a flag not a rectangle, Nepal's)
						sum[3] += q[3];
					}
				const unsigned a = sum[3] / 16, d = sum[3] ? sum[3] : 1;
				fl.px[size_t(y) * size_t(fl.w) + size_t(x)] = a << 24 | (sum[2] / d) << 16 | (sum[1] / d) << 8 | (sum[0] / d);
			}
		return fl;
	};
	if (!menu.init(win, ren, find_assets()))
		fprintf(stderr, "no menu: its files (assets/ui) weren't found\n");
	menu.set_footer(std::string("Virtua Fighter 3 Recomp ") + VF3_VERSION + (*VF3_DATE ? std::string(" \u00b7 ") + VF3_DATE : std::string()));
	bool running = true, paused = false;
	pad_state pstate[2];

	// ---- the governor (tests only: VF3_GOVERN) ----
	// The pictures' cost: if they leave too little of the screen's refresh (smooth motion) or of the game's
	// frame, the in-between pictures go first, then the scale a step at a time. Each cut lifts again once the
	// pictures have been well within their time for a while; the first seconds after a start or a change don't
	// count (the GPU's and caches' warm-up).
	struct governor {
		bool smooth_off = false;           // in-between pictures held to 60 a second (as VF1's and VF2's: off, a
		                                   // 60 Hz screen's pictures would be uneven, the game's 57.5 a second)
		int scale_cut = 0;                 // steps below the player's scale
		double avg_ms = 0;                 // a picture's time, averaged
		double fixed_ms = 0;               // the part of it no scale changes (the in-between, the triangles prepared), averaged
		// Refreshes missed: a picture each refresh, passes more than a refresh and a half apart, counted over 3 s. A
		// picture's own time leaves out the GPU's (it's submitted, not waited on) and the wait at the present: a GPU
		// that can't keep up misses refreshes with the pictures' time well in budget (VF2's governor judges by the
		// pictures shown, for the same reason). Over 10% of a window's: taken as pictures over their time.
		uint64_t win_t0 = 0;
		int win_passes = 0, win_missed = 0;
		int was_passes = 0, was_missed = 0;   // (the last window's, closed: what gov.missing was judged on, for the log)
		bool missing = false;
		int missed_cuts = 0;   // cuts made for refreshes missed this run: each waits longer to lift (10 s, 20, 40, then
		                       // 60), never kept for the run (as VF2's since its #3). Kept from the third, the character
		                       // select's and the intros' loads (a stage, a fighter) held a fast Mac at 60 for good by the
		                       // second match; kept at once, a first start's warm-up (the new exe scanned, the driver's
		                       // shaders built) held a fast PC at 60 all run
		uint64_t grace_until = 0, calm_since = 0;
		double emu_ms = 3.0;   // a game frame's cost, smoothed over frames (not the pace line's window, which restarts)
		int pictures = 0;
	} gov;
	auto gov_grace = [&](double s) { gov.grace_until = SDL_GetTicksNS() + uint64_t(s * 1e9); gov.calm_since = 0; gov.avg_ms = gov.fixed_ms = 0; gov.pictures = 0;
	                              gov.win_t0 = SDL_GetTicksNS(); gov.win_passes = gov.win_missed = 0; gov.missing = false; };
	gov_grace(10);   // (the start's first seconds don't count: the GPU warming up, the sound queue filling; as VF2's)
	const uint64_t start_ns = SDL_GetTicksNS();

	// A graphics setting changed by the player: every cut the governor made lifted, the pictures judged afresh. (Only
	// the setting's own cut was: smooth motion held to 60 by a slow moment stayed held while the player lowered the
	// scale, the cuts then lifted only after 10 calm seconds by a measure of the old settings, if ever.)
	auto gov_fresh = [&] {
		if (gov.smooth_off || gov.scale_cut || gov_aa_cut) fprintf(stderr, "governor: a setting changed: its cuts lifted\n");
		gov.smooth_off = false;
		gov.missed_cuts = 0;
		gov.scale_cut = 0;
		scale = chosen_scale();   // (not cfg.scale: 0 on a first start, and the picture's size loop never ended)
		gov_aa_cut = 0;
		gov_grace(3);
	};
	auto set_scale = [&](int s) {
		cfg.scale = std::clamp(s, 1, MAX_SCALE);
		cfg_dirty = true;
		gov_fresh();
		fprintf(stderr, "render scale %dx\n", scale);
	};
	auto set_shape = [&](int a) { aspect_i = cfg.aspect = a; cfg_dirty = true; gov_fresh(); };
	auto set_fullscreen = [&](bool on) { SDL_SetWindowFullscreen(win, on); SDL_SyncWindow(win); cfg.fullscreen = on; cfg_dirty = true; gov_fresh(); };
	int test_frames = 0;        // the test button held for these frames (the menu's Service menu; Start in the test menu)
	int service_frames = 0, test_mode_frames = 0;   // (Down in the test menu: the service button)
	bool test_pending = false, in_test = false, test_down_was = false, test_start_was = false;
	bool clock_reset = false;   // (the boot took its time: the frame clock starts again from now)
	// Reset (the menu, F3, leaving the test menu the PC way): the menu closes, a paused game unpauses, and the screen
	// goes black at once while the boot runs unseen, not the last picture held (as VF1's and VF2's)
	// a frame started, its first half run (smooth motion: Game::start_frame), with the inputs it was started with
	bool half = false;
	// (the started frame run on the game's own thread meanwhile, unless VF3_NO_EMU_THREAD: then its first half when
	// started, the rest when due)
	const bool emu_threaded = getenv("VF3_NO_EMU_THREAD") == nullptr;
	hotkey half_hk = hotkey::none;
	InputScript::Actions half_act;
	bool half_left = false;
	uint64_t half_ns = 0;   // (its first half's time, counted with the frame's)
	auto reset_game = [&] {
		// (never on a match's board, from its start: the host's settings booted, the same on both sides; a reset
		// there, or a setting changed, and the match goes out of step. end_match gives this player's back first.)
		if (nvram_away) { fprintf(stderr, "(no restart: an online match's board)\n"); return; }
		half = false;
		menu.close();   // (back to the game, as VF1's and VF2's)
		paused = false;
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		SDL_RenderPresent(ren);
		save_nvram();
		boot();
		test_frames = service_frames = test_mode_frames = 0;
		test_pending = in_test = false;
		fprintf(stderr, "the game restarted\n");
		clock_reset = true;
	};
	auto on_off = [](bool b) { return std::string(b ? "On" : "Off"); };
	auto wrap = [](int v, int n) { return (v % n + n) % n; };
	auto back_item = [&] { return menu_item{ "Back", "Back to the previous page.", {}, {}, [&] { menu.pop(); } }; };
	auto display_page = [&] {
		menu_page p{ "Display", "", {} };
		p.items.push_back({ "Render scale",
			"How sharp the 3D is. 1x is the original arcade resolution; higher is sharper but needs a faster computer. If yours can't keep up, the game lowers it for you. (F5)",
			[&] { return std::to_string(chosen_scale()) + "x" + (gov.scale_cut ? " (lowered to " + std::to_string(scale) + "x)" : ""); },
			[&](int d) { set_scale(chosen_scale() + d); } });
		p.items.push_back({ "Antialiasing",
			"Smooths jagged edges on the 3D: 2x and 3x draw it twice or three times as large each way and average it down. Cleaner, but four or nine times the work.",
			[&] {
				const int k = aa_now();
				if (cfg.aa > 1 && k < cfg.aa) return std::to_string(cfg.aa) + "x (" + (k > 1 ? std::to_string(k) + "x" : std::string("off")) + (gov_aa_cut ? " for now: the computer is busy)" : " at this render scale)");
				return k > 1 ? std::to_string(k) + "x" : std::string("Off");
			},
			[&](int d) { cfg.aa = std::clamp(cfg.aa + d, 1, 3); cfg_dirty = true; gov_fresh(); } });
		p.items.push_back({ "3D renderer",
			"Which part of your computer draws the 3D. Auto uses the GPU when it draws the same picture as the CPU here (it checks as the game starts). A change takes effect at the next start.",
			[&] {
				std::string now = g3.on ? "GPU" : "CPU";
				if (cfg.renderer3d != r3d_mode) return std::string(R3D_NAMES[cfg.renderer3d]) + " (from the next start)";
				if (r3d_mode == R3D_AUTO) return "Auto (" + now + ")";
				if (r3d_mode == R3D_GPU && g3.failed) return std::string("GPU (failed: CPU)");
				return std::string(R3D_NAMES[r3d_mode]);
			},
			[&](int d) {
				static const int ORDER[] = { R3D_AUTO, R3D_CPU, R3D_GPU };
				int i = 0;
				while (i < 2 && ORDER[i] != cfg.renderer3d) i++;
				cfg.renderer3d = ORDER[wrap(i + d, 3)]; cfg_dirty = true;
				cfg.gpu3d_failed.clear();   // (the player's choice: every GPU driver tried again from the next start, a crashed start too)
				SDL_RemovePath((data + "/gpu-start.crashed").c_str());
			} });
		p.items.push_back({ "Picture shape",
			"Fit the window fills any screen shape. The other shapes add black bars to keep their shape; 4:3 is the original arcade screen. Wider, the fights see more to the sides. (F6)",
			[&] { return std::string(aspect_i ? ASPECT_NAMES[aspect_i] : "Fit the window"); },
			[&](int d) { set_shape(wrap(aspect_i + d, N_ASPECTS)); } });
		p.items.push_back({ "Background filter",
			"Smooth softens the stage backgrounds so they don't look blocky when enlarged. Off keeps their original pixels. The scores and text stay sharp either way.",
			[&] { return std::string(cfg.smooth_bg ? "Smooth" : "Off"); },
			[&](int) { cfg.smooth_bg = !cfg.smooth_bg; cfg_dirty = true; SDL_SetTextureScaleMode(tex_below, cfg.smooth_bg ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST); } });
		p.items.push_back({ "Distant textures",
			"Sharp picks the textures' detail for the picture's size, so things in the distance stay as crisp as the render scale allows. Arcade picks it as the arcade did for its own screen: softer in the distance at higher render scales. The same at 1x.",
			[&] { return std::string(cfg.sharp_textures ? "Sharp" : "Arcade"); },
			[&](int) { cfg.sharp_textures = !cfg.sharp_textures; video::Raster::arcade_lod = !cfg.sharp_textures; cfg_dirty = true; } });
		p.items.push_back({ "Smooth motion",
			"On: a picture for each refresh of your screen, placed between the arcade's 57.5 frames a second, a frame behind them. Off: only the arcade's own frames, as on the cabinet (uneven on most screens).",
			[&] { return on_off(cfg.smooth) + (cfg.smooth && gov.smooth_off ? " (60 a second for now: the computer is busy)" : ""); },
			[&](int) { cfg.smooth = !cfg.smooth; cfg_dirty = true; gov_fresh(); } });
		p.items.push_back({ "Screen filter",
			"Scanlines adds the dark lines between the arcade monitor's rows. CRT adds those, the monitor's red, green and blue stripes, and slightly darker corners. The lines need a large window or fullscreen to show.",
			[&] { return std::string(FILTER_NAMES[cfg.filter]); },
			[&](int d) { cfg.filter = wrap(cfg.filter + d, 3); cfg_dirty = true; } });
		p.items.push_back({ "CRT colours",
			"The colours as an arcade monitor showed them, as Supermodel's CRT colours. Sega arcade is a Japanese cabinet's: bluer whites and richer colours. The others are a Sony studio monitor and the TV standards of Japan, the US and Europe. Off: the game's colours as they are.",
			[&] { return std::string(CRT_COLOURS[cfg.crt_colours].name) + (cfg.crt_colours && crt_failed ? " (not with this renderer)" : ""); },
			[&](int d) { cfg.crt_colours = wrap(cfg.crt_colours + d, N_CRT_COLOURS); cfg_dirty = true; } });
#ifdef _WIN32
		p.items.push_back({ "Graphics driver",
			"Auto starts on Vulkan, and goes to Direct3D 12 by itself if Vulkan won't start or draws wrongly. Direct3D 12 starts on Direct3D 12 instead. Applies from the next start.",
			[&] { return std::string(cfg.graphics ? "Direct3D 12" : "Auto (Vulkan first)"); },
			[&](int) { cfg.graphics = !cfg.graphics; cfg_dirty = true; } });
#endif
		p.items.push_back({ "Show FPS", "Shows the frame rate in the top right corner.",
			[&] { return on_off(cfg.show_fps); },
			[&](int) { cfg.show_fps = !cfg.show_fps; cfg_dirty = true; if (!cfg.show_fps) menu.set_fps(""); } });
		p.items.push_back({ "Fullscreen", "Play fullscreen or in a window. (F11, F or Alt+Enter)",
			[&] { return on_off(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN); },
			[&](int) { set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); } });
		p.items.push_back(back_item());
		return p;
	};
	auto sound_page = [&] {
		menu_page p{ "Sound", "", {} };
		p.items.push_back({ "Volume", "The overall volume.",
			[&] { return std::to_string(cfg.volume) + "%"; },
			[&](int d) {
				cfg.volume = std::clamp(cfg.volume + 10 * d, 0, 100); cfg_dirty = true;
				game.sound_do([&] { game.sound.gain = LEVEL * cfg.volume / 100.0; });
			} });
		p.items.push_back({ "Music", "The music's volume, within the overall volume.",
			[&] { return std::to_string(cfg.music) + "%"; },
			[&](int d) {
				cfg.music = std::clamp(cfg.music + 10 * d, 0, 100); cfg_dirty = true;
				game.sound_do([&] { game.sound.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f); });
			} });
		p.items.push_back({ "Effects and voices", "Hits, voices and the announcer, within the overall volume.",
			[&] { return std::to_string(cfg.effects) + "%"; },
			[&](int d) {
				cfg.effects = std::clamp(cfg.effects + 10 * d, 0, 100); cfg_dirty = true;
				game.sound_do([&] { game.sound.set_volumes(cfg.music / 100.0f, cfg.effects / 100.0f); });
			} });
		p.items.push_back(back_item());
		return p;
	};
	auto game_page = [&] {
		menu_page p{ "Game", "", {} };
		p.items.push_back({ "Play style",
			"PC skips the boot screen and is always free play. Arcade has the boot screen, coins and credits, like the real cabinet: Start needs a credit, and Coin adds one (5 and 6, or set it in Controls: a key, or a pad button such as View); its coin settings are kept for it. Changing it restarts the game.",
			[&] { return std::string(cfg.arcade ? "Arcade" : "PC"); },
			[&](int) { if (nvram_away) return; cfg.arcade = !cfg.arcade; g_pc_mode = !cfg.arcade; cfg_dirty = true; reset_game(); } });
		p.items.push_back({ "Reset game", "Restarts the game, like switching the arcade off and on. Rankings and settings are kept. (F3)",
			{}, {}, [&] { reset_game(); } });
		p.items.push_back(back_item());
		return p;
	};
	// The arcade's own settings (its test menu's GAME ASSIGNMENTS), read and written where the game keeps them in the
	// backup RAM: a byte each at 0x1E000 on (found by changing each in the test menu), the energies 32-bit. The game
	// reads them as it starts: a change restarts it when the menu closes (as VF2's), if they differ from when their
	// page opened (a value stepped there and back, or at its limit, doesn't).
	std::vector<uint8_t> arcade_before;   // (empty: the page not opened since the menu last closed)
	auto arcade_bytes = [&] { if (game.emu_thread.joinable()) game.job_wait(); return std::vector<uint8_t>(board.backup.begin() + 0x1E000, board.backup.begin() + 0x1E040); };
	auto arcade_page = [&] {
		menu_page p{ "Arcade settings: a change restarts the game when the menu closes", "", {} };
		auto at = [&](int o) -> uint8_t & { return board.backup[0x1E000 + size_t(o)]; };
		auto choice = [&](const char *label, const char *help, int o, int lo, int hi, std::function<std::string(int)> name) {
			p.items.push_back({ label, help,
				[at, o, lo, hi, name] { const int v = at(o); return v < lo || v > hi ? std::string("-") : name(v); },
				[&, at, o, lo, hi](int d) {
					const int v = at(o);
					if (v < lo || v > hi || nvram_away) return;   // (not the host's settings, a match's)
					at(o) = uint8_t(std::clamp(v + d, lo, hi));
				} });
		};
		auto energy = [&](const char *label, const char *help, int o, int lo, int hi) {
			auto get = [at, o] { return int(at(o)) << 24 | int(at(o + 1)) << 16 | int(at(o + 2)) << 8 | int(at(o + 3)); };
			p.items.push_back({ label, help,
				[get, lo, hi] { const int v = get(); return v < lo || v > hi ? std::string("-") : std::to_string(v); },
				[&, at, get, o, lo, hi](int d) {
					const int v = get();
					if (v < lo || v > hi || nvram_away) return;
					const int n = std::clamp(v + 10 * d, lo, hi);
					for (int i = 0; i < 4; i++) at(o + i) = uint8_t(n >> (24 - 8 * i));
				} });
		};
		static const char *DIFFICULTY[] = { "Easy", "Normal", "Hard", "Hardest" };
		static const char *COUNTRY[] = { "Japan", "USA", "Export", "Asia" };
		choice("Difficulty", "How hard the computer fights.", 6, 0, 3, [](int v) { return std::string(DIFFICULTY[v]); });
		choice("Rounds to win (vs CPU)", "Rounds needed to win a match against the computer.", 2, 2, 5, [](int v) { return std::to_string(v); });
		choice("Rounds to win (vs player)", "Rounds needed to win a match between two players.", 3, 2, 5, [](int v) { return std::to_string(v); });
		energy("Energy (vs CPU)", "Each fighter's energy in a match against the computer (the arcade's ENERGY MAX).", 8, 160, 360);
		energy("Energy (vs player)", "Each fighter's energy in a match between two players.", 12, 180, 400);
		choice("Stage select (vs player)", "A match between two players: the challenger picks the stage (the arcade's own screen, 8 seconds; "
		       "hold Start on one for its Dural version). Off, as most arcades had it, each match goes to the game's next stage. Online matches have it off.",
		       0x17, 0, 1, [&](int v) { return on_off(v == 0); });
		choice("Demo sound", "Sound during the demo that plays between games.", 7, 0, 1, [&](int v) { return on_off(v == 0); });
		choice("Country", "The arcade's region: Japan, USA, Export or Asia. It changes the game's text and what it shows.", 0x1E, 0, 3,
			[](int v) { return std::string(COUNTRY[v]); });
		p.items.push_back({ "Service menu",
			"The arcade's own test menu: input and sound tests, the rest of its game settings, coin settings and bookkeeping. Down moves, Start selects (or 9 and F2 on a keyboard). Choose EXIT to come back.",
			{}, {}, [&] { menu.close(); paused = false; test_pending = true; } });
		p.items.push_back(back_item());
		return p;
	};
	auto key_name = [](SDL_Scancode k) {
		if (!k) return std::string("-");
		const char *n = SDL_GetScancodeName(k);
		return n && *n ? std::string(n) : "Key " + std::to_string(int(k));
	};
	auto pad_name = [&](int p) {
		const std::string &c = cfg.ctl[p].pad_choice;
		const char *nm = pads[p] ? SDL_GetGamepadName(pads[p]) : nullptr;
		std::string now = pads[p] ? (nm && *nm ? nm : "Gamepad") : "";
		if (c == "none") return std::string("None");
		if (c == "auto") return now.empty() ? std::string("Any pad (none connected)") : "Any pad: " + now;
		return now.empty() ? std::string("Chosen pad (not connected)") : now;
	};
	auto cycle_pad = [&](int p, int d) {
		std::vector<std::string> opts = { "auto" };
		int n = 0;
		SDL_JoystickID *ids = SDL_GetGamepads(&n);
		for (int i = 0; i < n; i++) opts.push_back(guid_of(ids[i]));
		SDL_free(ids);
		opts.push_back("none");
		auto it = std::find(opts.begin(), opts.end(), cfg.ctl[p].pad_choice);
		int i = it == opts.end() ? 0 : int(it - opts.begin());
		cfg.ctl[p].pad_choice = opts[wrap(i + d, int(opts.size()))];
		if (cfg.ctl[p].pad_choice != "auto" && cfg.ctl[p].pad_choice != "none" && cfg.ctl[1 - p].pad_choice == cfg.ctl[p].pad_choice)
			cfg.ctl[1 - p].pad_choice = "auto";
		cfg_dirty = true;
		open_pads();
	};
	auto keys_page = [&](int p) {
		menu_page pg{ p ? "Player 2 keyboard" : "Player 1 keyboard", "", {} };
		for (int a = 0; a < C_COUNT; a++)
			pg.items.push_back({ CONTROL_NAMES[a], std::string(a == C_COIN ? "A coin for this player in Arcade mode (a cabinet's coin slot). "
													: a == C_MENU ? "Opens this menu (as Esc always does): for a cabinet's button. " : "") +
					"Select, then press the key to use. Delete clears it.",
				[&, p, a] { return key_name(cfg.ctl[p].key[a]); }, {},
				[&, p, a] {
					menu.capture(std::string("Press a key for ") + CONTROL_NAMES[a] + (p ? " (player 2)" : " (player 1)") +
								 ". Backspace or Delete clears it, Esc keeps it.",
						[&, p, a](const SDL_Event &e) {
							if (e.type != SDL_EVENT_KEY_DOWN) return false;
							const SDL_Scancode k = e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE ? SDL_SCANCODE_UNKNOWN : e.key.scancode;
							static const SDL_Scancode RESERVED[] = { SDL_SCANCODE_P, SDL_SCANCODE_TAB, SDL_SCANCODE_F3,
								SDL_SCANCODE_F5, SDL_SCANCODE_F9, SDL_SCANCODE_F6, SDL_SCANCODE_F11, SDL_SCANCODE_F12 };
							for (SDL_Scancode r : RESERVED) if (k == r) return false;
							for (const auto &sk : SYS_KEYS) if (k == sk.key) return false;
							if (k) for (auto &c : cfg.ctl) for (auto &x : c.key) if (x == k) x = SDL_SCANCODE_UNKNOWN;
							cfg.ctl[p].key[a] = k;
							cfg_dirty = true;
							return true;
						});
				}, false, [&, p, a] { cfg.ctl[p].key[a] = SDL_SCANCODE_UNKNOWN; cfg_dirty = true; } });
		pg.items.push_back(back_item());
		return pg;
	};
	auto pad_buttons_page = [&](int p) {
		menu_page pg{ p ? "Player 2 pad buttons" : "Player 1 pad buttons", "", {} };
		for (int a = C_GUARD; a < C_COUNT; a++)
			pg.items.push_back({ CONTROL_NAMES[a], std::string(a == C_COIN ? "A coin for this player in Arcade mode (a cabinet's coin slot). "
												   : a == C_MENU ? "Opens this menu. L3 + R3 and the touchpad click (when it isn't set to something else) always do too, and Esc on a keyboard. " : "") +
												   "Select, then press the button (or trigger) to use. Y (or Delete) clears it. "
												   "The d-pad and left stick always move.",
				[&, p, a] { return player_controls::pad_label(cfg.ctl[p].pad[a]); }, {},
				[&, p, a] {
					menu.capture(std::string("Press a pad button for ") + CONTROL_NAMES[a] + (p ? " (player 2)" : " (player 1)") +
								 (a >= C_COIN ? ". Esc keeps it; Delete clears it." : ". View or Esc keeps it; Delete clears it."),
						[&, p, a](const SDL_Event &e) {
							int code = PAD_NONE;
							if (e.type == SDL_EVENT_KEY_DOWN) {
								if (e.key.scancode != SDL_SCANCODE_DELETE && e.key.scancode != SDL_SCANCODE_BACKSPACE) return false;
								cfg.ctl[p].pad[a] = PAD_NONE;
								cfg_dirty = true;
								return true;
							}
							if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
								// View: keep it (View is for the menu's hotkeys, not moves), unless it's for Coin or Menu
								if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK && a < C_COIN) return true;
								code = e.gbutton.button;
							} else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 16000 &&
									   (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
								code = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PAD_LT : PAD_RT;
							else return false;
							for (int b = C_GUARD; b < C_COUNT; b++) if (cfg.ctl[p].pad[b] == code) cfg.ctl[p].pad[b] = PAD_NONE;
							cfg.ctl[p].pad[a] = code;
							cfg_dirty = true;
							return true;
						});
				}, false, [&, p, a] { cfg.ctl[p].pad[a] = PAD_NONE; cfg_dirty = true; } });
		pg.items.push_back(back_item());
		return pg;
	};
	auto player_page = [&](int p) {
		menu_page pg{ p ? "Player 2" : "Player 1", "", {} };
		pg.items.push_back({ "Pad", "Which controller this player uses: any free one, a particular one, or none.",
							 [&, p] { return pad_name(p); }, [&, p](int d) { cycle_pad(p, d); } });
		pg.items.push_back({ "Keyboard", "Change this player's keys.", {}, {}, [&, p] { menu.push(keys_page(p)); } });
		pg.items.push_back({ "Pad buttons", "Change which buttons guard, punch, kick, escape, do the combinations, start, add a coin and open the menu.",
							 {}, {}, [&, p] { menu.push(pad_buttons_page(p)); } });
		pg.items.push_back({ "Reset to defaults", "Put this player's keys and buttons back as they were.", {}, {},
							 [&, p] {
								 cfg.ctl[p] = player_controls::defaults(p);
								 for (auto k : cfg.ctl[p].key)
									 if (k) for (auto &x : cfg.ctl[1 - p].key) if (x == k) x = SDL_SCANCODE_UNKNOWN;
								 cfg_dirty = true;
								 open_pads();
							 } });
		pg.items.push_back(back_item());
		return pg;
	};
	auto controls_page = [&] {
		menu_page pg{ "Controls", "", {} };
		pg.items.push_back({ "Player 1", "Player 1's controller, keys and buttons.", [&] { return pad_name(0); }, {},
							 [&] { menu.push(player_page(0)); } });
		pg.items.push_back({ "Player 2", "Player 2's controller, keys and buttons.", [&] { return pad_name(1); }, {},
							 [&] { menu.push(player_page(1)); } });
		pg.items.push_back({ "Swap pads", "Player 1 and player 2 swap controllers.", {}, {},
							 [&] {
								 auto choice = [&](int p) { return pads[p] ? guid_of(SDL_GetGamepadID(pads[p])) : std::string("none"); };
								 std::string a = choice(0), b = choice(1);
								 if (a == b) { pads_swapped = !pads_swapped; std::swap(pads[0], pads[1]); return; }
								 cfg.ctl[0].pad_choice = b;
								 cfg.ctl[1].pad_choice = a;
								 cfg_dirty = true;
								 open_pads();
							 } });
		pg.items.push_back(back_item());
		return pg;
	};
	bool online_ready = false;
	auto online_page = [&] {
		if (!online_ready) {   // (the ROM's hash for the handshake, once)
			if (game.emu_thread.joinable()) game.job_wait();
			online.identity = { "vf3", VF3_VERSION, vf3net::content_hash(board), "" };
			online_ready = true;
		}
		return online_menu::page(online, cfg.online, menu, [&] { cfg_dirty = true; });
	};
	auto main_page = [&] {
		menu_page p{ "", "", {} };
		if (online.playing() || nvram_away) {   // (the match goes on under the menu; or it's starting, on its board)
			p.items.push_back({ "Resume", "Back to the match (it goes on while the menu's open).", {}, {}, [&] { menu.close(); } });
			online_menu::match_rows(online, cfg.online, menu, [&] { cfg_dirty = true; }, p.items);
			p.items.push_back({ "Display", "Sharpness, antialiasing, screen shape, the 3D renderer, smooth motion, background and screen filters, frame rate and fullscreen.", {}, {}, [&] { menu.push(display_page()); } });
			p.items.push_back({ "Sound", "The overall, music and effects volumes.", {}, {}, [&] { menu.push(sound_page()); } });
			p.items.push_back({ "Controls", "Each player's controller, keys and buttons. Online, either player's controls play your fighter.", {}, {}, [&] { menu.push(controls_page()); } });
			return p;
		}
		p.items.push_back({ "Resume", "Back to the game.", {}, {}, [&] { menu.close(); } });
		p.items.push_back({ "Display", "Sharpness, antialiasing, screen shape, the 3D renderer, smooth motion, background and screen filters, frame rate and fullscreen.", {}, {}, [&] { menu.push(display_page()); } });
		p.items.push_back({ "Sound", "The overall, music and effects volumes.", {}, {}, [&] { menu.push(sound_page()); } });
		p.items.push_back({ "Game", "Play style and reset.", {}, {}, [&] { menu.push(game_page()); } });
		p.items.push_back({ "Arcade settings", "Difficulty, rounds, energy, demo sound, country and the service menu. Changing them restarts the game.", {}, {},
							[&] { if (arcade_before.empty()) arcade_before = arcade_bytes(); menu.push(arcade_page()); } });
		p.items.push_back({ "Online", "Play someone over the network: host a match, or join one.", {}, {}, [&] { menu.push(online_page()); } });
		p.items.push_back({ "Controls", "Each player's controller, keys and buttons.", {}, {}, [&] { menu.push(controls_page()); } });
		p.items.push_back({ "Quit", "Saves and closes the game.", {}, {}, [&] { running = false; } });
		return p;
	};
	bool sound_paused = false;
	auto sync_menu_sound = [&] {
		const bool pause = menu.is_open() && !online.playing();   // (a match goes on, its sound too)
		if (!audio || pause == sound_paused) return;
		sound_paused = pause;
		if (sound_paused) SDL_PauseAudioStreamDevice(audio); else SDL_ResumeAudioStreamDevice(audio);
	};
	auto open_menu = [&] {
		if (game.emu_thread.joinable()) game.job_wait();
		menu.set_title(online.playing() ? "ONLINE MATCH" : "PAUSED");
		if (menu.ready() && !menu.is_open()) menu.open(main_page());
		sync_menu_sound();
	};

	// ---- test hooks ----
	std::vector<std::pair<SDL_Scancode, uint64_t>> test_keys;   // VF3_KEYS
	std::vector<uint64_t> window_shots;
	if (const char *k = getenv("VF3_KEYS"))
		for (std::string all = std::string(k) + ","; !all.empty(); all.erase(0, all.find(',') + 1)) {
			const std::string t = all.substr(0, all.find(','));
			const size_t at = t.find('@');
			if (t.empty() || at == std::string::npos) continue;
			test_keys.push_back({ SDL_GetScancodeFromName(t.substr(0, at).c_str()), strtoull(t.c_str() + at + 1, nullptr, 10) });
		}
	std::vector<uint64_t> shot_at;   // VF3_SHOT_AT: the window at these passes, to build/host_window_at_N.png
	if (const char *k = getenv("VF3_SHOT_AT"))
		for (const char *q = k; *q;) { shot_at.push_back(strtoull(q, nullptr, 10)); while (*q && *q != ',') q++; if (*q) q++; }
	std::vector<std::string> menu_keys;   // VF3_MENU_KEYS
	if (const char *k = getenv("VF3_MENU_KEYS"))
		for (std::string all = std::string(k) + ","; !all.empty(); all.erase(0, all.find(',') + 1))
			if (all.find(',') > 0) menu_keys.push_back(all.substr(0, all.find(',')));
	auto send_key = [&](SDL_Scancode sc) {
		SDL_Event e{};
		e.type = SDL_EVENT_KEY_DOWN;
		e.key.scancode = sc;
		e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
		e.key.down = true;
		SDL_PushEvent(&e);
		e.type = SDL_EVENT_KEY_UP;
		e.key.down = false;
		SDL_PushEvent(&e);
	};

	// VF3_RECORD: the recording (see the top)
	struct {
		std::string dir;
		uint64_t from = 0, to = UINT64_MAX, n = 0, base = 0;   // game frames; n: the pictures recorded; base: frames_run then
		bool on = false;
		FILE *video = nullptr, *wav = nullptr;
		uint32_t wav_bytes = 0;
		int w = 0, h = 0;
	} rec;
	if (const char *d = getenv("VF3_RECORD")) {
		rec.dir = d;
		std::filesystem::create_directories(rec.dir, ec);
		if (const char *f = getenv("VF3_RECORD_FROM")) rec.from = strtoull(f, nullptr, 10);
		if (const char *f = getenv("VF3_RECORD_TO")) rec.to = strtoull(f, nullptr, 10);
	}
	auto wav_header = [&] {
		const uint32_t rate = SOUND_RATE, br = rate * 4, sz = 36 + rec.wav_bytes;
		const uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
		const uint32_t sixteen = 16;
		fseek(rec.wav, 0, SEEK_SET);
		fwrite("RIFF", 1, 4, rec.wav); fwrite(&sz, 4, 1, rec.wav); fwrite("WAVEfmt ", 1, 8, rec.wav);
		fwrite(&sixteen, 4, 1, rec.wav); fwrite(&fmt, 2, 1, rec.wav); fwrite(&ch, 2, 1, rec.wav);
		fwrite(&rate, 4, 1, rec.wav); fwrite(&br, 4, 1, rec.wav); fwrite(&align, 2, 1, rec.wav); fwrite(&bits, 2, 1, rec.wav);
		fwrite("data", 1, 4, rec.wav); fwrite(&rec.wav_bytes, 4, 1, rec.wav);
		fseek(rec.wav, 0, SEEK_END);
	};
	auto rec_stop = [&] {
		if (rec.video) pclose(rec.video);
		if (rec.wav) { wav_header(); fclose(rec.wav); }
		if (rec.on) fprintf(stderr, "record: %llu pictures, %.1f s, to %s\n", (unsigned long long)rec.n, rec.n / 60.0, rec.dir.c_str());
		rec.video = rec.wav = nullptr;
		rec.on = false;
	};

	// ---- the loop ----
	// Pacing (as VF2's): the game's frames by the clock, one every 1/57.52 s; the sound card's clock is the one
	// that counts: the frame clock is nudged (1% at most) to keep about QUEUED frames of sound queued, and a queue
	// running low gets its frames at once. A picture for each pass (each refresh of the screen, by vsync): with
	// smooth motion, between the last two frames by the frame clock; else the newest frame's, once.
	double frame_clock = 0, queue_avg = 0;
	double screen_hz = 60;   // (looked at once a second, below)
	bool vsync_on = true;    // (the renderer's vsync as it reports it, looked at with the refresh rate)
	// A picture each refresh with smooth motion: the frame clock moves a refresh's time each pass, pulled a tenth of
	// the way to the real clock (a real hitch, more than 2 refreshes off: taken whole). By the real clock, each
	// picture showed the game where it was when that pass happened to draw, early or late in the refresh as the
	// game's frame took its time (on a Steam Deck 4-8 ms in some passes, none in others), and shown a refresh apart
	// the motion wobbled: 60 pictures a second that didn't feel smooth.
	double even_real = 0, even_shown = 0, pass_avg = 0;   // (pass_avg: the passes' time apart, averaged)
	uint64_t frames_run = 0, clock_t = SDL_GetTicksNS(), pass = 0;
	bool shown = false, first_shown = false;
	bool new_frame = true;
	auto restart_clock_real = [&] { frame_clock = double(frames_run); clock_t = SDL_GetTicksNS(); even_real = even_shown = 0; };
	pace.t0 = SDL_GetTicksNS();
	int shot_n = 0;
	m3::Controls script_ctl;
	// The renderer's device or its targets reset (Direct3D's after a driver update, a display change; Direct3D 9's
	// render targets): every texture let go of and made again, the menu's too, and the newest frame drawn again, as
	// VF1's and VF2's. The device lost for good: the game closes (the log says so).
	auto textures_lost = [&](bool device) {
		fprintf(stderr, "the renderer's %s reset: its textures made again\n", device ? "device" : "targets");
		if (tex3d) { SDL_DestroyTexture(tex3d); tex3d = nullptr; tex_w = tex_h = 0; }
		if (g3.tex) { SDL_DestroyTexture(g3.tex); g3.tex = nullptr; g3.w = g3.h = 0; }
		SDL_DestroyTexture(tex_below);
		SDL_DestroyTexture(tex_above);
		make_2d(tex_below, video::WW);
		SDL_SetTextureScaleMode(tex_below, cfg.smooth_bg ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
		make_2d(tex_above, video::W);
		if (glass) { SDL_DestroyTexture(glass); glass = nullptr; glass_w = glass_h = 0; }
		menu.textures_lost();
		new_frame = true;   // (the next picture sends the 2D again)
	};
	// Started from a terminal, the window can open behind it or without the keyboard (the keys go to the terminal):
	// it asks for it over about 5 s, and the log says whether it got it (as VF1's and VF2's)
	int focus_checks = 300;
	// smooth motion's pictures between frames: at most 120 a second (as VF1's and VF2's: a 144 or 240 Hz screen
	// gets a picture at its refreshes up to that), none while the window can't be seen
	double smooth_budget = 0;
	uint64_t smooth_t = SDL_GetTicksNS();
	// the window's title: the scale and the picture's shape; F9, the timings every 2 s instead (and in the log)
	std::string title_now;
	struct { uint64_t t0 = 0; int frames = 0, pictures = 0; double emu_ms = 0, draw_ms = 0; } t9;
	t9.t0 = SDL_GetTicksNS();
	uint64_t save_t = SDL_GetTicksNS();   // the settings saved every 3 s when changed (the NVRAM once a minute: nvram_t)

	// ---- online play (online.h): the match's frames through the session, its notices over the game ----
	online.adapter.settle = [&] { if (game.emu_thread.joinable()) game.job_wait(); };
	online.adapter.program_changed = [&] { game.program_changed(); };
	online.adapter.booting = [] { SDL_PumpEvents(); };   // (the boot's 2 s: the system doesn't take the game for hung)
	// (tests: VF3_TEST_FRAME_MS="ms[,every s,its ms]" makes each frame of a match cost that much at least, a Steam
	// Deck's, now and then more, its character select's; a frame run again 40% of it)
	static const std::array<double, 3> test_ms = [] {
		double v[3] = { 0, 0, 0 };
		if (const char *e = getenv("VF3_TEST_FRAME_MS")) sscanf(e, "%lf,%lf,%lf", &v[0], &v[1], &v[2]);
		return std::array<double, 3>{ v[0], v[1], v[2] };
	}();
	online.adapter.run = [&](bool resim) {
		const uint64_t t0 = SDL_GetTicksNS();
		if (!resim) game.run_frame(true, true);
		else {
			const bool tiles = game.tiles_on;   // (a frame run again after a rollback: the board alone, no picture or sound)
			game.tiles_on = false;
			board.run_frame();
			game.tiles_on = tiles;
		}
		if (test_ms[0] > 0) {
			double ms = test_ms[0];
			if (test_ms[1] > 0 && std::fmod(double(t0) / 1e9, test_ms[1]) < 2.0) ms = test_ms[2];   // (2 s of it)
			if (resim) ms *= 0.4;
			while (double(SDL_GetTicksNS() - t0) / 1e6 < ms) {}
		}
	};
	online.adapter.on_begin = [&] {
		if (!nvram_away) {   // (this player's own, given back after the match)
			own_backup = board.backup;
			memcpy(own_eeprom, board.eeprom.regs, sizeof own_eeprom);
			nvram_away = true;
		}
		half = false;
		paused = false;
		game.reset_host();
		game.tiles_on = false;   // (the match's boot runs unseen)
		test_frames = service_frames = test_mode_frames = 0;
		test_pending = in_test = false;
	};
	std::vector<uint8_t> late_midi;   // (a frame run again that has sounds its first run hadn't: they go with the next frame's)
	online.adapter.midi_out = [&](const std::vector<uint8_t> &b) { late_midi.insert(late_midi.end(), b.begin(), b.end()); };
	bool match_on = false;
	Online::Phase online_was = Online::Phase::off;
	std::string online_code_was;   // (the room code: its page made again when the lobby gives it)
	uint64_t online_refresh_t = 0;
	bool menu_closed_elsewhere = false;   // (the menu closed by a match starting: its closing's bookkeeping still owed)
	// A step that couldn't run (the other side behind, or keeping in time) takes no input: what was pressed for it
	// goes with the next one tried, so a tap of Start or Coin then (read_inputs plays them out over a few frames,
	// each read once) isn't lost
	uint16_t step_carry = 0, match_job_bits = 0;
	constexpr uint16_t ONE_SHOT = vf3net::START_BUTTON | 512;   // (Start and Coin: the input word's, vf3net::pack)
	auto end_match = [&] {   // back to playing on its own, from the boot, with this player's NVRAM
		if (game.emu_thread.joinable()) game.job_wait();   // (nothing of a frame still running while it's all put back)
		half = false;
		const bool reopen = match_on || nvram_away;   // (a match, or one that started and never got going)
		if (match_on) fprintf(stderr, "online: the match ended at frame %llu; back to playing alone\n", (unsigned long long)game_frame);
		match_on = false;
		game.take_midi = nullptr;
		game.hud_names[0].clear();
		game.hud_flags[0] = game.hud_flags[1] = {};
		game.hud_names[1].clear();
		game.tiles_on = true;
		menu.set_notice("");
		menu.set_banner("");
		menu.hide_versus();
		step_carry = 0;   // (a press held over from this match's last step: not the next one's)
		menu.set_net("", 0);
		board.rtc_time = -1;
		if (nvram_away) {
			board.backup = own_backup;
			memcpy(board.eeprom.regs, own_eeprom, sizeof own_eeprom);
			nvram_away = false;
		}
		menu.set_title("PAUSED");
		reset_game();
		// back on the Online page: what ended the match, and the same again a press away
		if (reopen && menu.ready()) {
			if (menu.is_open()) menu.close();
			menu.open(main_page());
			menu.select_row(5);   // (Online)
			menu.push(online_page());
		}
		sync_menu_sound();
	};
	// The button this player presses for Start, as their pad or keyboard has it (online, Player 1's controls): a pad's
	// Start under its own name (OPTIONS on a PlayStation's, + on a Switch's, MENU on an Xbox One's and a Steam Deck),
	// a button it's set to otherwise, else the key
	auto start_button = [&]() -> std::string {
		const int b = cfg.ctl[0].pad[C_START];
		if (pads[0] && b != PAD_NONE) {
			if (b != SDL_GAMEPAD_BUTTON_START) {
				const char *bn = b == PAD_LT ? "LT" : b == PAD_RT ? "RT" : SDL_GetGamepadStringForButton(SDL_GamepadButton(b));
				std::string n = bn ? bn : "START";
				for (char &ch : n) ch = char(toupper(uint8_t(ch)));
				return n;
			}
			const char *nm = SDL_GetGamepadName(pads[0]);
			if (nm && strstr(nm, "Steam Deck")) return "MENU";
			switch (SDL_GetGamepadType(pads[0])) {
			case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return "OPTIONS";
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "+";
			case SDL_GAMEPAD_TYPE_XBOXONE: return "MENU";
			default: return "START";
			}
		}
		std::string k = key_name(cfg.ctl[0].key[C_START]);
		for (char &ch : k) ch = char(toupper(uint8_t(ch)));
		return k;
	};
	auto online_pass = [&] {
		online.poll(double(SDL_GetTicksNS()) / 1e6);
		Online::Notice n;
		while (online.next_notice(n)) {
			if (n.kind == Online::Notice::joined) {
				match_on = true;
				late_midi.clear();
				game.take_midi = [&](std::vector<uint8_t> &out) {
					online.adapter.take_midi(out);
					out.insert(out.begin(), late_midi.begin(), late_midi.end());
					late_midi.clear();
				};
				game.tiles_on = true;
				game_frame = vf3net::BOOT_FRAMES;
				game.hud_names[online.local_player()] = online.identity.name;   // (the name the other side was sent)
				game.hud_names[online.local_player() ^ 1] = online.opponent();
				game.hud_flags[online.local_player()] = hud_flag(online.my_country());
				game.hud_flags[online.local_player() ^ 1] = hud_flag(online.peer_country());
				arcade_before.clear();   // (the board's arcade settings are the host's now: no restart for them)
				if (menu.is_open()) { menu.close(); menu_closed_elsewhere = true; }
				clock_reset = true;
				sync_menu_sound();
				// who's playing who, over the title screen for a moment (the host is player 1)
				const std::string host = online.is_hosting() ? online.identity.name : online.opponent(), guest = online.is_hosting() ? online.opponent() : online.identity.name;
				const int ms = online.ping();
				menu.show_versus({ "PLAYER 1 \xC2\xB7 HOST", host, "PLAYER 2", guest,
				                   std::string(online.via_relay() ? "Connected through the relay" : "Connected directly") + (ms > 0 ? " \xC2\xB7 " + std::to_string(ms) + " ms" : ""),
				                   online.slow_local() ? "Through the relay: this Mac blocks your network. Turn on Local Network (System Settings) for a faster match"
				                                       : "Both press Start", online.via_relay(),
				                   online.is_hosting() ? online.my_country() : online.peer_country(), online.is_hosting() ? online.peer_country() : online.my_country() });
			}
			// (nothing else said over the game: the Online screen says what came of it, when it's next opened)
			fprintf(stderr, "online: %s\n", Online::log_masked(n.text).c_str());
		}
		if ((match_on && !online.playing()) || (nvram_away && !online.playing() && !online.starting())) end_match();   // (ended, left, or never got going)
		// (quiet a second is often only both games restarting their boards at the match's start: said after two, and
		// never over the versus screen; and Connection back only after it was said)
		static bool said_interrupted = false;
		const bool interrupted = online.playing() && online.interrupted() && (said_interrupted || (online.hold_left() <= 6 && !menu.versus_showing()));
		said_interrupted = interrupted;
		// who's to press Start: at the match's start until both have (not over the versus screen, which says it), and
		// after a match the loser, for a rematch (the winner told for 8 s: they're playing on, against the computer)
		int wait = online.playing() && !menu.is_open() && !menu.versus_showing() && !interrupted ? online.start_wait() : 0;
		static uint64_t other_out_since = 0;
		if (wait != 4) other_out_since = 0;
		else if (!other_out_since) other_out_since = SDL_GetTicks();
		else if (SDL_GetTicks() - other_out_since > 8000) wait = 0;
		static int said_wait = 0;
		if (wait != said_wait && online.playing()) {   // (in the log: who's to press Start, and why)
			static const char *what[5] = { "no one", "this player (the match's start)", "the other (the match's start)", "this player (a rematch)", "the other (a rematch)" };
			fprintf(stderr, "online: to press Start: %s\n", what[wait]);
		}
		said_wait = wait;
		// (the 3D on the CPU with the setting not CPU: why, on screen once, as soon as the menu can say it)
		if (cpu_why_new && !online.playing()) {   // (not over an online match: after it)
			cpu_why_new = false;
			menu.toast("The 3D is on the CPU (slower): " + cpu_why, 12);
		}
		const bool press = wait == 1 || wait == 3;
		menu.set_banner(press ? "PRESS" : wait ? "WAITING FOR " + online.opponent() : "",
		                wait == 2 ? "to press Start" : wait == 3 ? "for a rematch" : wait == 4 ? "to press Start for a rematch" : "", press ? start_button() : "");
		if (interrupted)
			menu.set_notice("WAITING FOR " + online.opponent(), "The connection dropped. Holding the match for " + std::to_string(online.hold_left()) + " s...",
			                "It carries on from right here if it comes back");
		else if (online.starting()) menu.set_notice("STARTING THE MATCH", online.status());
		else menu.set_notice("");
		std::string line;
		if (cfg.online.show_connection && online.waiting() && !online.starting())   // (playing alone meanwhile: still waiting, and where)
			line = online.phase() == Online::Phase::hosting && online.by_code()
				? "Hosting   " + (online.room_code().empty() ? std::string("a room") : "room " + online.room_code()) + "   " + online.status()
				: online.phase() == Online::Phase::hosting
				? "Hosting   " + (online.local_address().empty() ? std::string("no network") : online.local_address()) + "   " + online.status()
				: online.status();
		// (in a match: its figures, said 4 times a second, not laid out again each frame; the versus screen says who
		// and how, and the start's quiet isn't news)
		static game_menu::net_cells cells;
		static uint64_t cells_at = 0;
		static bool had_cells = false;
		if (!online.playing() || !cfg.online.show_connection || menu.versus_showing()) {
			had_cells = false;
			menu.set_net(menu.versus_showing() ? std::string() : line, 0);
		} else if (!had_cells || SDL_GetTicks() - cells_at >= 250 || online.interrupted() != cells.interrupted) {
			had_cells = online.connection(cells, cfg.online.show_connection == 2);
			cells_at = SDL_GetTicks();
			if (had_cells) menu.set_net(cells);
			else menu.set_net(std::string(), 0);
		}
		// the Online page as things change: its rows made again with the phase, its status lines a few times a second
		if (menu.is_open() && (online.phase() != online_was || online.room_code() != online_code_was) && menu.top_name() == "Online" && !menu.capturing())
			menu.replace(online_page(), -1);
		else if (menu.is_open() && online.phase() != online_was && !menu.capturing())
			menu.refresh();   // (the Join page: its line under Connect, joining no longer, and why)
		else if (menu.is_open() && online.playing() && !menu.capturing() && SDL_GetTicks() - online_refresh_t > 500) {
			online_refresh_t = SDL_GetTicks();   // (the menu in a match: its Playing row's ping)
			menu.refresh();
		}
		else if (menu.is_open() && online.waiting() && SDL_GetTicks() - online_refresh_t > 250) {
			online_refresh_t = SDL_GetTicks();
			static std::string said;
			if (const std::string now = online.status(); now != said) { said = now; menu.refresh(); }
		}
		else if (menu.is_open() && online.phase() == Online::Phase::off && menu.top_name() == "Online" && !menu.capturing()) {
			static int said = -2;   // (the Online screen: how many play at random, as the lobby says)
			if (const int n = online.players_online(cfg.online); n != said) { said = n; menu.refresh(); }
		}
		online_was = online.phase();
		if (!menu.capturing()) online_code_was = online.room_code();
	};

	// A match's step on the game's thread: started after the pass's inputs, run while the picture's drawn and the
	// screen's waited for, taken at the next pass's start, before anything else looks at the game (in a match every
	// frame ran here before, and its rollback's frames with it, the picture waiting on all of it: on a Steam Deck
	// passes went past their refresh)
	struct { bool on = false, ok = false; double ms = 0; } match_job;
	bool match_job_taken = false;
	auto take_match_job = [&] {
		if (!match_job.on) return;
		game.job_wait();
		match_job.on = false;
		step_carry = match_job.ok ? 0 : uint16_t(match_job_bits & ONE_SHOT);
		if (!match_job.ok) {   // (it couldn't run: still due)
			frames_run -= 1;
			frame_clock = std::min(frame_clock, double(frames_run) + 1.0);
			return;
		}
		game.publish();
		game_frame++;
		pace.frames++;
		pace.emu_ms += match_job.ms;
		match_job_taken = true;
		if (audio && !game.audio.empty())
			SDL_PutAudioStreamData(audio, game.audio.data(), int(game.audio.size() * sizeof(int16_t)));
		game.audio.clear();
		if (quit_after >= 0 && int64_t(game_frame) >= quit_after) running = false;   // (tests)
	};

	while (running) {
		pass++;
		// A match: the sound board runs to the real clock on its thread, keeping the queue topped up itself
		// (Game::set_sound_free): the frames come when the session lets them, and neither their waits nor a slow
		// frame touch the sound
		game.set_sound_free(audio && online.playing() ? audio : nullptr, int(frame_bytes * MATCH_QUEUED));
		const uint64_t pass_t0 = SDL_GetTicksNS();
		int pass_kind = 0;   // (the pace line's: 0 a picture only, 1 a frame's first half, 2 its second, 3 a whole frame)
		take_match_job();
		if (std::exchange(match_job_taken, false)) { pass_kind = 3; new_frame = true; }
		uint64_t pass_work_ns = 0;
		const bool menu_was_open = menu.is_open();
		// (tests: a room hosted, or joined with its code, as the menu would, once the game's past its start)
		static const char *test_host_room = getenv("VF3_HOST_ROOM"), *test_join_code = getenv("VF3_JOIN_CODE"), *test_random = getenv("VF3_RANDOM");
		if ((test_host_room || test_join_code || test_random) && pass == 60) {
			online_page();   // (the identity, as opening the Online page sets it)
			std::string err;
			if (!(test_random ? online.find_random(cfg.online, err)
			      : test_join_code ? online.join_room(cfg.online, test_join_code, err) : online.host_room(cfg.online, err)))
				fprintf(stderr, "online: (test) %s\n", err.c_str());
		}
		if (!menu_keys.empty() && pass >= 30 && (pass - 30) % 6 == 0 && (pass - 30) / 6 < menu_keys.size()) {
			const std::string &k = menu_keys[(pass - 30) / 6];
			SDL_Scancode sc = k == "esc" ? SDL_SCANCODE_ESCAPE : k == "up" ? SDL_SCANCODE_UP : k == "down" ? SDL_SCANCODE_DOWN
				: k == "left" ? SDL_SCANCODE_LEFT : k == "right" ? SDL_SCANCODE_RIGHT : k == "enter" ? SDL_SCANCODE_RETURN
				: SDL_GetScancodeFromName(k.c_str());
			send_key(sc);
		}
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT) running = false;
			if (e.type == SDL_EVENT_RENDER_DEVICE_RESET || e.type == SDL_EVENT_RENDER_TARGETS_RESET) textures_lost(e.type == SDL_EVENT_RENDER_DEVICE_RESET);
			if (e.type == SDL_EVENT_RENDER_DEVICE_LOST) { fprintf(stderr, "the graphics device was lost: closing\n"); running = false; }
			if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) { open_pads(); menu.refresh(); }
			if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
				menu.refresh();
				if (!hidden && SDL_GetKeyboardFocus() != win) SDL_RaiseWindow(win);   // (the move to a new Space can lose it)
			}
			if (menu.handle_event(e)) continue;
			// Fullscreen also with F and Alt+Enter, as VF1's and VF2's (after the menu's own events: typing in its
			// fields never does it; F not while a player has it as a control, nor with Ctrl, Alt or the command key)
			auto fullscreen_key = [&](const SDL_KeyboardEvent &k) {
				if (k.scancode == SDL_SCANCODE_RETURN && (k.mod & SDL_KMOD_ALT)) return true;
				if (k.scancode != SDL_SCANCODE_F || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) return false;
				for (const auto &c : cfg.ctl) for (SDL_Scancode sc : c.key) if (sc == SDL_SCANCODE_F) return false;
				return true;
			};
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && fullscreen_key(e.key)) {
				set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
				continue;
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
				switch (e.key.scancode) {
				case SDL_SCANCODE_ESCAPE: open_menu(); break;
				case SDL_SCANCODE_P: if (!online.playing() && !nvram_away) paused = !paused; break;   // (a match can't pause)
				case SDL_SCANCODE_F3: if (!online.playing() && !nvram_away) reset_game(); break;
				case SDL_SCANCODE_F9:   // the timings in the title (and the log) every 2 s
					cfg.timings = !cfg.timings; cfg_dirty = true; title_now.clear();
					t9 = {}; t9.t0 = SDL_GetTicksNS();
					break;
				case SDL_SCANCODE_F5: set_scale(scale % MAX_SCALE + 1); break;
				case SDL_SCANCODE_F6: set_shape((aspect_i + 1) % N_ASPECTS); break;
				case SDL_SCANCODE_F11: set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); break;
				case SDL_SCANCODE_F12: {
					// the picture as drawn (the render scale, antialiased, its shape), without the bars around it or
					// the screen filter, into Screenshots in the data folder, numbered, never overwritten (as VF2's)
					const std::string dir = data + "/Screenshots";
					std::filesystem::create_directories(dir, ec);
					char name[32];
					do snprintf(name, sizeof name, "/vf3_%04d.png", ++shot_n);
					while (std::filesystem::exists(dir + name, ec) && shot_n < 9999);
					SDL_FRect all, mid;
					picture_rect(all, mid);
					const SDL_Rect area = { int(std::lround(all.x)), int(std::lround(all.y)), int(std::lround(all.w)), int(std::lround(all.h)) };
					compose(false);
					read_window(dir + name, &area);
					fprintf(stderr, "screenshot %s\n", (dir + name).c_str());
					break;
				}
				default: break;
				}
			}
		}
		sync_menu_sound();
		if ((menu_was_open && !menu.is_open()) || std::exchange(menu_closed_elsewhere, false)) {
			if (online.phase() == Online::Phase::off) { online.set_last_match({}); online.forget_failure(); }   // (MATCH OVER, COULDN'T CONNECT: just after, not for ever)
			if (cfg_dirty && quit_after < 0 && cfg.save(settings_path)) cfg_dirty = false;
			if (!arcade_before.empty()) {
				const bool changed = arcade_before != arcade_bytes();
				arcade_before.clear();
				if (changed && !nvram_away) reset_game();   // (never in a match: that's a desync)
			}
			pstate[0] = pstate[1] = pad_state{};
			pstate[0].back_was = pstate[1].back_was = true;
			pstate[0].back_combo = pstate[1].back_combo = true;
			pstate[0].start_was = pstate[1].start_was = true;
			for (pad_state &s : pstate) s.menu_was = s.touch_was = s.sticks_was = true;
			restart_clock_real();
			gov_grace(2);
		}
		if (clock_reset) { clock_reset = false; frames_run = 0; restart_clock_real(); new_frame = true; }
		online_pass();
		const bool stopped = !online.playing() && (paused || menu.is_open() || online.starting());
		const bool ff = !stopped && !online.playing() && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB];
		int due = 0;
		if (stopped) {
			pace.stopped_s += double(SDL_GetTicksNS() - clock_t) / 1e9;   // (the menu's time isn't the game's)
			restart_clock_real();
		}
		else if (!rec.dir.empty()) {
			// the recording: before it starts, frames as fast as they go; then 60 pictures a second by the game's clock
			if (!rec.on && game_frame >= rec.from) {
				rec.on = true;
				rec.from = game_frame;
				rec.base = frames_run;
				int ww = 0, wh = 0;
				SDL_GetCurrentRenderOutputSize(ren, &ww, &wh);
				rec.w = ww & ~1; rec.h = wh & ~1;
				const std::string cmd = "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s " + std::to_string(rec.w) + "x" + std::to_string(rec.h) +
				                        " -r 60 -i - -c:v libx264 -preset slow -crf 12 -pix_fmt yuv420p '" + rec.dir + "/video.mp4'";
				rec.video = popen(cmd.c_str(), "w");
				rec.wav = fopen((rec.dir + "/sound.wav").c_str(), "wb");
				if (rec.wav) wav_header();
				fprintf(stderr, "record: from frame %llu, %dx%d\n", (unsigned long long)rec.from, rec.w, rec.h);
			}
			if (!rec.on) { due = 8; frame_clock = double(frames_run + due); }
			else {
				frame_clock = double(rec.base) + double(rec.n + 1) * FRAME_HZ / 60.0;
				due = std::max(0, int(int64_t(std::floor(frame_clock)) - int64_t(frames_run)));
			}
			frames_run += due;
		}
		else {
			const uint64_t now_ns = SDL_GetTicksNS();
			double dt = double(now_ns - clock_t) / 1e9;
			clock_t = now_ns;
			// (only while the passes keep to the screen's refreshes, a whole number of them apart on average: not with
			// vsync off or not honoured, a minimised or covered window, or a refresh rate that isn't the screen's; each
			// pass moves the clock by the whole refreshes it took, so a picture every other refresh steps evenly too,
			// and never back)
			static const bool no_even = getenv("VF3_NO_EVEN_PACE") != nullptr;   // (tests: by the real clock, as before)
			pass_avg += (dt - pass_avg) * 0.05;
			const double per = pass_avg * screen_hz, whole = std::round(per);
			const bool unseen_now = !hidden && (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED));
			if (!no_even && cfg.smooth && !hidden && !unseen_now && vsync_on && (!gov.smooth_off || screen_hz <= 60 * 1.05) &&
				whole >= 1 && std::fabs(per - whole) < 0.12) {
				const double unit = 1.0 / screen_hz, step = std::max(1.0, std::round(dt * screen_hz)) * unit;
				even_real += dt;
				const double behind = even_real - even_shown;
				dt = std::fabs(behind - step) > 2 * unit ? behind : step + (behind - step) * 0.1;
				dt = std::max(0.0, dt);
				even_shown += dt;
				if (even_shown > 1e6) { even_real -= even_shown; even_shown = 0; }   // (kept small)
			} else even_real = even_shown = 0;
			double rate = FRAME_HZ;
			int low = 0;
			bool sound_full = false;
			// (a match's sound: its own thread's, to the real clock, set_sound_free above; alone: a frame's at a
			// time, the frames kept in step with the queue)
			if (audio && !online.playing()) {
				const int q = SDL_GetAudioStreamQueued(audio);
				if (q > frame_bytes * int(FRAME_HZ * 0.3)) sound_full = true;
				queue_avg += (q - queue_avg) * std::min(1.0, dt / 0.5);
				rate *= 1 - std::clamp((queue_avg / frame_bytes - QUEUED) * 0.005, -0.01, 0.01);
				if (q < frame_bytes) low = int(std::ceil(QUEUED - double(q) / frame_bytes));   // (under a frame's: extra frames)
			}
			rate *= 1 - online.slow_down();   // (a match: ahead of the other side, a little slower till it's caught up)
			frame_clock += dt * rate;
			due = int(int64_t(std::floor(frame_clock)) - int64_t(frames_run));
			if (due > 8) due = 1, frame_clock = double(frames_run + 1);
			due = std::max(due, low);
			if (due > 0) frame_clock = std::max(frame_clock, double(frames_run + due));
			due = std::max(due, 0);
			if (!ff && due > 2 && due > low) { frame_clock -= due - std::max(2, low); due = std::max(2, low); }
			if (ff) { due = 8; frame_clock = double(frames_run + due); }
			else if (sound_full) { due = 0; frame_clock = std::min(frame_clock, double(frames_run) + 1); }
			frames_run += due;
		}
		// the inputs the game reads during a frame, set before it starts (or its first half)
		auto frame_inputs = [&](hotkey &hk, InputScript::Actions &act, bool &left_test) {
				// the inputs the game reads during the next frame: the script's, then the player's
				m3::Controls live;
				hk = read_inputs(live, pads, pstate, cfg.ctl);
				act = InputScript::Actions();
				if (scripted) act = script.step(script_ctl, game_frame);
				m3::Controls &c = board.controls;
				auto mix = [](bool *a, const bool *b, const bool *d) { a[0] = b[0] || d[0]; a[1] = b[1] || d[1]; };
				mix(c.coin, script_ctl.coin, live.coin); mix(c.start, script_ctl.start, live.start);
				mix(c.test, script_ctl.test, live.test); mix(c.service, script_ctl.service, live.service);
				mix(c.up, script_ctl.up, live.up); mix(c.down, script_ctl.down, live.down);
				mix(c.left, script_ctl.left, live.left); mix(c.right, script_ctl.right, live.right);
				mix(c.punch, script_ctl.punch, live.punch); mix(c.kick, script_ctl.kick, live.kick);
				mix(c.guard, script_ctl.guard, live.guard); mix(c.escape, script_ctl.escape, live.escape);
				// The arcade's test menu (RAM 0x102002 is 4 while it's up; 0 at the attract, a fight, the boot): Down
				// moves (the service button) and Start selects (the test button), any pad or key, as VF1's and VF2's;
				// the menu's Service menu presses test once the game is past its boot; leaving it, the PC way restarts
				// at once past the boot (the game reboots through its own, about 7 s)
				const bool test_mode = board.ram.size() > 0x102002 && board.ram[0x102002 ^ 3] == 4;
				test_mode_frames = test_mode ? test_mode_frames + 1 : 0;
				if (!in_test && test_mode_frames >= 30) in_test = true;
				left_test = false;
				if (in_test && !test_mode) { in_test = false; left_test = true; }
				if (in_test) {
					const bool down = c.down[0] || c.down[1], start = c.start[0] || c.start[1];
					if (down && !test_down_was) service_frames = 6;
					if (start && !test_start_was) test_frames = 6;
					test_down_was = down;
					test_start_was = start;
				}
				if (service_frames > 0) { c.service[0] = true; service_frames--; }
				if (test_pending && game_frame >= BOOT_FRAMES) { test_pending = false; test_frames = 10; }
				if (test_frames > 0) { c.test[0] = true; test_frames--; }
				game.builder.widen = std::max(1.0f, float(picture_aspect()) * 0.75f) * 1.05f;   // (a little more: smooth motion)
		};
		if (online.playing()) {
			// A match: each frame through the session, with this player's input (whichever player's controls they
			// use; nothing while the menu's open); a frame that can't run yet (the other side behind, or keeping in
			// time) stays due, and the picture stays
			for (int i = 0; i < due && running && online.playing(); i++) {
				m3::Controls live;
				const hotkey hk = read_inputs(live, pads, pstate, cfg.ctl);
				if (scripted) {   // (tests: an input script's player 1 as this player's, in a match too)
					script.step(script_ctl, game_frame);
					const uint16_t sb = vf3net::pack(script_ctl, 0);
					vf3net::unpack(uint16_t(vf3net::pack(live, 0) | sb), 0, live);
				}
				const uint16_t bits = menu.is_open() ? 0 : uint16_t(vf3net::pack(live, 0) | vf3net::pack(live, 1) | step_carry);
				game.builder.widen = std::max(1.0f, float(picture_aspect()) * 0.75f) * 1.05f;
				const uint64_t te = SDL_GetTicksNS();
				online.poll(double(te) / 1e6);
				if (hk == hotkey::menu) open_menu();
				if (emu_threaded && i + 1 == due) {   // (the pass's last: on the game's thread, taken next pass)
					match_job = { true, false, 0 };
					match_job_bits = bits;
					game.job_run([&, bits] {
						const uint64_t t0 = SDL_GetTicksNS();
						match_job.ok = online.step(bits);
						match_job.ms = double(SDL_GetTicksNS() - t0) / 1e6;
					});
					break;
				}
				if (!online.step(bits)) {
					step_carry = uint16_t(bits & ONE_SHOT);
					frames_run -= uint64_t(due - i);
					frame_clock = std::min(frame_clock, double(frames_run) + 1.0);
					break;
				}
				step_carry = 0;
				game.publish();
				game_frame++;
				pace.frames++;
				pace.emu_ms += double(SDL_GetTicksNS() - te) / 1e6;
				pass_kind = 3;
				new_frame = true;
				if (audio && !game.audio.empty())
					SDL_PutAudioStreamData(audio, game.audio.data(), int(game.audio.size() * sizeof(int16_t)));
				game.audio.clear();
				if (quit_after >= 0 && int64_t(game_frame) >= quit_after) running = false;   // (tests)
			}
			due = 0;
		}
		for (int i = 0; i < due && running; i++) {
			// A frame more than this pass's first (a slow pass let two fall due): started on the game's own thread, not
			// run here, and taken next pass (run here it made this pass slow too, and the next, one after another)
			if (emu_threaded && i > 0 && !half && !ff && cfg.smooth) {
				frames_run -= uint64_t(due - i);
				frame_clock = std::min(frame_clock, double(frames_run) + 1.0);
				const uint64_t th = SDL_GetTicksNS();
				frame_inputs(half_hk, half_act, half_left);
				game.job_start(!ff);
				half = true;
				half_ns = SDL_GetTicksNS() - th;
				break;
			}
			uint64_t te = SDL_GetTicksNS();
			if (half) te -= half_ns;
			hotkey hk = hotkey::none;
			InputScript::Actions act;
			bool left_test = false;
			if (half) { hk = half_hk; act = half_act; left_test = half_left; half_hk = hotkey::none; half_act = InputScript::Actions(); }
			else frame_inputs(hk, act, left_test);
			if (half) {
				if (emu_threaded) game.job_wait();
				else game.finish_frame(!ff, true);
				half = false;
				pass_kind = std::max(pass_kind, 2);
			} else { game.run_frame(!ff, true); pass_kind = 3; }
			game.publish();
			game_frame++;
			if (game_frame == BOOT_FRAMES && !scripted && !skip_boot()) vf3net::set_versus_count(board, vf3net::random_versus_count());   // (the boot seen: as after an unseen one)
			if (game_frame == BOOT_FRAMES && scripted) if (const char *vc = getenv("VF3_VERSUS_COUNT")) vf3net::set_versus_count(board, uint32_t(atoi(vc)));   // (tests: that stage)
			pace.frames++;
			pace.emu_ms += double(SDL_GetTicksNS() - te) / 1e6;
			t9.frames++;
			t9.emu_ms += double(SDL_GetTicksNS() - te) / 1e6;
			gov.emu_ms += (std::min(double(SDL_GetTicksNS() - te) / 1e6, 4 * gov.emu_ms + 1) - gov.emu_ms) * 0.02;   // (a lone slow frame barely counts)
			new_frame = true;
			for (auto &s : act.shots) write_frame("build/host_" + s + ".png");
			if (act.quit) running = false;
			if (rec.wav && game_frame > rec.from) {
				fwrite(game.audio.data(), sizeof(int16_t), game.audio.size(), rec.wav);
				rec.wav_bytes += uint32_t(game.audio.size() * sizeof(int16_t));
			}
			if (audio && !game.audio.empty() && !ff && rec.dir.empty())   // (recording: to its file only, not queued for hours)
				SDL_PutAudioStreamData(audio, game.audio.data(), int(game.audio.size() * sizeof(int16_t)));
			game.audio.clear();
			for (auto &k : test_keys) if (k.second == game_frame) { send_key(k.first); window_shots.push_back(game_frame + 3); }
			if (quit_after >= 0 && int64_t(game_frame) >= quit_after) running = false;
			switch (hk) {
			case hotkey::menu: open_menu(); break;
			case hotkey::scale: set_scale(scale % MAX_SCALE + 1); break;
			case hotkey::aspect: set_shape((aspect_i + 1) % N_ASPECTS); break;
			case hotkey::none: break;
			}
			if (left_test && g_pc_mode) { reset_game(); break; }
			if (menu.is_open()) break;
		}
		// The next frame's first half, when it's due within half a frame: smooth motion's pictures then each hold half
		// a frame's work at most (the pictures go between the last two frames finished: this one isn't yet)
		static const bool no_halves = getenv("VF3_NO_HALVES") != nullptr;   // (tests: whole frames, as before)
		if (!half && !no_halves && rec.dir.empty() && running && !stopped && !ff && cfg.smooth && (due == 0 || emu_threaded) && !menu.is_open() && first_shown &&
			online.phase() == Online::Phase::off && !match_on && !nvram_away &&   // (online: whole frames, each through the session; none as one ends)
			frame_clock - double(frames_run) >= 0.5) {
			const uint64_t th = SDL_GetTicksNS();
			frame_inputs(half_hk, half_act, half_left);
			if (emu_threaded) game.job_start(!ff);
			else game.start_frame();
			half = true;
			pass_kind = std::max(pass_kind, 1);
			half_ns = SDL_GetTicksNS() - th;
		}
		// The picture: smooth motion's, between the last two frames, at every pass; else the newest frame's
		// (in a match, each frame as it is: a picture between two frames the rollback may yet change showed the
		// change as a ghost; and the in-between's time is better spent on the frames)
		const bool smooth = cfg.smooth && !ff && !online.playing();
		const uint64_t bt = SDL_GetTicksNS();
		smooth_budget = std::min(2.0, smooth_budget + double(bt - smooth_t) * (gov.smooth_off ? 60 : 120) / 1e9);
		smooth_t = bt;
		const bool unseen = !hidden && (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED));
		// (a screen no faster than the pictures' limit: a picture each refresh, its vsync the pace. By the budget alone a
		// refresh a hair early found it a hair short of one, and that refresh's picture was lost: 108-118 a second on a
		// 120 Hz screen. The budget only holds a faster screen to the limit.)
		static uint64_t screen_hz_t = 0;
		if (bt - screen_hz_t > 1000000000ull) {
			const bool first_look = !screen_hz_t;
			screen_hz_t = bt;
			const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
			const double hz_now = dm && dm->refresh_rate > 0 ? dm->refresh_rate : 60.0;
			if (hz_now != screen_hz || first_look) fprintf(stderr, "screen: %.2f Hz%s\n", hz_now, !hidden ? " (a picture each refresh, with vsync)" : "");
			screen_hz = hz_now;
			int vs = 0;
			const bool vs_now = SDL_GetRenderVSync(ren, &vs) && vs != 0;
			if (vs_now != vsync_on || first_look) fprintf(stderr, "vsync: %s\n", vs_now ? "on" : "off (pictures by the budget, not a picture each refresh)");
			vsync_on = vs_now;
		}
		// (hidden, or vsync off: nothing paces the passes to the screen, a picture each pass was ~500 a second; as VF1's.
		// A faster screen than 120 Hz a picture each refresh too, as VF1's and VF2's: held to 120 a second, a 144 or 165 Hz
		// screen's pictures came 1 and 2 refreshes apart by turns, an uneven judder)
		// A screen over 180 Hz (240, 360): a picture every 2nd or 3rd refresh, about 120 a second as before, still even
		// (each one a whole number of refreshes after the last); every refresh there was 2-3 times the pictures' work.
		static uint64_t last_pic_ns = 0;
		const int refreshes_apart = screen_hz > 180 ? std::max(1, int(std::lround(screen_hz / 120.0))) : 1;
		const bool by_refresh = !hidden && vsync_on && (!gov.smooth_off || screen_hz <= 60 * 1.05);   // (the screen's the pace, not the budget)
		const bool every_refresh = by_refresh && (refreshes_apart == 1 || double(bt - last_pic_ns) >= (refreshes_apart - 0.5) * 1e9 / screen_hz);
		// (tests: VF3_TEST_NO_PICTURES, two games at once on a small machine, the frames' cost alone: a picture only now
		// and then)
		static const bool no_pictures = getenv("VF3_TEST_NO_PICTURES") != nullptr;
		const bool draw = no_pictures ? first_shown ? (pass % 120 == 0) : true
		                              : rec.on || new_frame || (smooth && !stopped && !unseen && ((!by_refresh && smooth_budget >= 1) || every_refresh)) || !first_shown;
		if (draw) {
			last_pic_ns = bt;
			smooth_budget = std::max(0.0, smooth_budget - 1);
			t9.pictures++;
			const uint64_t td = SDL_GetTicksNS();
			double t = smooth && !stopped ? std::clamp(frame_clock - double(frames_run), 0.0, 1.0) : 1.0;
			if (stopped) t = 1.0;
			draw_picture(t, new_frame);
			if (test_load_ms > 0 && double(SDL_GetTicksNS() - start_ns) / 1e9 < test_load_s + 5)
				SDL_DelayNS(uint64_t(test_load_ms * scale * scale * 1e6));
			const double ms = double(SDL_GetTicksNS() - td) / 1e6;
			pace.draw_ms += ms;
			t9.draw_ms += ms;
			pace.pictures++;
			new_frame = false;
			// the governor
			const uint64_t now = SDL_GetTicksNS();
			// (off for players, as VF1's and VF2's since its v0.1.10: the scale, the antialiasing and smooth motion
			// always the player's; its cuts held a fast Mac at 60 through the character select's loads. VF3_GOVERN:
			// on, for tests)
			static const bool governs = getenv("VF3_GOVERN") != nullptr;
			if (governs && now > gov.grace_until && !stopped && !ff && !unseen) {
				gov.fixed_ms = gov.pictures ? gov.fixed_ms + (std::min(picture_fixed_ms, ms) - gov.fixed_ms) * 0.05 : std::min(picture_fixed_ms, ms);
				gov.avg_ms = gov.pictures++ ? gov.avg_ms + (ms - gov.avg_ms) * 0.05 : ms;
				const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
				const double screen_hz = dm && dm->refresh_rate > 0 ? std::min(240.0f, dm->refresh_rate) : 60.0;
				const double hz = std::min(screen_hz, gov.smooth_off ? 60.0 : 120.0);   // (the pictures a second at most)
				const double frame_ms = 1000.0 / FRAME_HZ, emu = gov.emu_ms;
				// a picture each refresh (smooth) or each frame, with the game's frame: at 120 a second a game frame
				// comes with about every other picture, so each picture's share of it (it was taken whole from each,
				// and a 10 ms frame on an M4 Max left 120 a second no time at all: held to 60)
				const double budget = smooth ? 1000.0 / hz - emu * std::min(1.0, FRAME_HZ / hz) : frame_ms - emu;
				if (gov.pictures > 30) {
					if (gov.avg_ms > 0.9 * budget || gov.missing) {
						if (gov.missing) gov.missed_cuts++;
						if (gov.missing) fprintf(stderr, "governor: %d of %d refreshes missed (the GPU or the present, not the pictures' %.1f ms)\n", gov.was_missed, gov.was_passes, gov.avg_ms);
						if (smooth && !gov.smooth_off && hz > 65) {
							gov.smooth_off = true;
							fprintf(stderr, "governor: pictures take %.1f ms, over %.0f a second's time: smooth motion held to 60 pictures a second for now\n", gov.avg_ms, hz);
						} else if (!gov.missing && gov.avg_ms - gov.fixed_ms < 0.35 * budget) {
							// Most of a picture is the in-between and the triangles prepared, on the CPU at any scale: a lower
							// scale or less antialiasing would only make it look worse (a Steam Deck in Wolf's desert: 13 ms
							// pictures, the scale cut from 2x to 1x and still 13 ms). Said once.
							static bool said = false;
							if (!said) fprintf(stderr, "governor: pictures take %.1f ms, %.1f of it on the CPU at any scale: the scale and antialiasing kept\n", gov.avg_ms, gov.fixed_ms);
							said = true;
						} else if (aa_now() > 1) {
							gov_aa_cut = cfg.aa - (aa_now() - 1);
							fprintf(stderr, "governor: pictures take %.1f ms, over a frame's time: antialiasing lowered to %s for now\n", gov.avg_ms,
									aa_now() > 1 ? (std::to_string(aa_now()) + "x").c_str() : "off");
						} else if (scale > 1) {
							scale--;
							gov.scale_cut++;
							fprintf(stderr, "governor: pictures take %.1f ms, over a frame's time: the render scale lowered to %dx for now\n", gov.avg_ms, scale);
						}
						gov_grace(2);
						gov.calm_since = 0;
					} else if (gov.smooth_off || gov.scale_cut || gov_aa_cut) {
						// calm: the cut lifted after 10 s (longer for cuts again for refreshes missed: missed_cuts) well within the time the pictures would take then (the scale's
						// first, then the antialiasing's, then smooth motion)
						const int k = aa_now();
						const double want = gov.scale_cut ? gov.avg_ms * double((scale + 1) * (scale + 1)) / (scale * scale)
						                  : gov_aa_cut ? gov.avg_ms * double((k + 1) * (k + 1)) / (k * k) : gov.avg_ms;
						const double want_budget = gov.scale_cut || gov_aa_cut ? budget : 1000.0 / std::min(screen_hz, 120.0) - emu * std::min(1.0, FRAME_HZ / std::min(screen_hz, 120.0));
						if (want < 0.6 * want_budget) {
							if (!gov.calm_since) gov.calm_since = now;
							else if (now - gov.calm_since > (gov.missed_cuts > 3 ? 60 : 10ull << std::max(0, gov.missed_cuts - 1)) * 1000000000ull) {
								if (gov.scale_cut) { scale++; gov.scale_cut--; fprintf(stderr, "governor: calm again: the render scale back up to %dx\n", scale); }
								else if (gov_aa_cut) { gov_aa_cut--; fprintf(stderr, "governor: calm again: antialiasing back up to %dx\n", aa_now()); }
								else if (cfg.smooth) { gov.smooth_off = false; fprintf(stderr, "governor: calm again: smooth motion back to 120 pictures a second\n"); }
								gov_grace(2);
							}
						} else gov.calm_since = 0;
					}
				}
			}
		}
		compose();
		// the frame rate: the pictures shown a second, over the game (not the menu or a pause, where no new
		// pictures are made: hidden there, as VF1's and VF2's)
		const bool fps_shown = cfg.show_fps && !stopped;
		if (fps_shown) {
			static uint64_t fps_t = 0;
			static int fps_n = 0;
			if (draw) fps_n++;
			if (SDL_GetTicksNS() - fps_t > 500000000ull) {
				char b[32];
				snprintf(b, sizeof b, "%.0f FPS", fps_n * 1e9 / double(SDL_GetTicksNS() - fps_t));
				menu.set_fps(b);
				fps_t = SDL_GetTicksNS();
				fps_n = 0;
			}
		}
		menu.render(fps_shown);   // (the menu, or what's over the game: the FPS, a match's connection line, toasts, notices)
		for (auto it = window_shots.begin(); it != window_shots.end();)
			if (*it <= game_frame) { read_window("build/host_window_" + std::to_string(*it - 3) + ".png"); it = window_shots.erase(it); }
			else ++it;
		for (uint64_t s : shot_at) if (s == pass) read_window("build/host_window_at_" + std::to_string(pass) + ".png");
		if (!running && quit_after >= 0) {
			if (game.emu_thread.joinable()) game.job_wait();   // (a frame's half may still be running)
			write_frame("build/host_last.png");
			read_window("build/host_window.png");
		}
		if (rec.on && rec.video) {
			if (SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr)) {
				if (SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888)) {
					for (int y = 0; y < rec.h && y < c->h; y++)
						fwrite(static_cast<uint8_t *>(c->pixels) + size_t(y) * c->pitch, 4, size_t(rec.w), rec.video);
					SDL_DestroySurface(c);
				}
				SDL_DestroySurface(s);
			}
			rec.n++;
			if (rec.n % 600 == 0) fprintf(stderr, "record: %.0f s\n", rec.n / 60.0);
			if (game_frame >= rec.to) running = false;
		}
		pass_work_ns = SDL_GetTicksNS() - pass_t0;
		SDL_RenderPresent(ren);
		{   // the governor's count of refreshes missed (above)
			static uint64_t last_present = 0;
			const uint64_t now_p = SDL_GetTicksNS();
			// (only passes whose own work fit in a refresh: one that ran long on the CPU before its present, the game's
			// frame loading a stage or a fighter (the character select, the intros: 11 ms before the present), is no
			// sign of the GPU or the present falling behind, and fewer pictures wouldn't help it. Counted, those
			// loads alone held smooth motion at 60 on an M4 Max at 4x.)
			if (last_present && quit_after < 0 && rec.dir.empty() && every_refresh && draw && smooth && !stopped && !unseen && now_p > gov.grace_until &&
				double(pass_work_ns) < 1e9 / screen_hz) {
				gov.win_passes++;
				if (double(now_p - last_present) > 1.5e9 / screen_hz) gov.win_missed++;
			}
			last_present = now_p;
			if (now_p - gov.win_t0 > 3000000000ull) {
				gov.missing = gov.win_passes > 100 && gov.win_missed * 10 > gov.win_passes;
				gov.was_passes = gov.win_passes; gov.was_missed = gov.win_missed;
				gov.win_t0 = now_p;
				gov.win_passes = gov.win_missed = 0;
			}
		}
		if (!first_shown) {
			first_shown = true;
			if (!hidden) { SDL_ShowWindow(win); SDL_RaiseWindow(win); }
			fprintf(stderr, "first picture %.2f s after the start\n", double(SDL_GetTicksNS() - start_ns) / 1e9);
		}
		shown = true;
		if (focus_checks > 0 && !hidden && first_shown) {
			const bool ours = SDL_GetKeyboardFocus() == win;
			if (--focus_checks % 30 == 0 && !ours) SDL_RaiseWindow(win);
			if (focus_checks == 0 || ours) {
				fprintf(stderr, "keyboard: %s\n", ours ? "ours" : "not ours (click the window)");
				focus_checks = 0;
			}
		}
		if (hidden) SDL_DelayNS(1000000);   // (no vsync to wait for when hidden on some systems)
		else if (unseen) SDL_DelayNS(2000000);   // (a minimised window's present may not wait: not a core spun)
		{   // at most a pass every 2 ms, as VF1's and VF2's: with vsync off (forced so, or not taken) or a minimised
			// window, the present returns at once, and the passes ran flat out (a core and the GPU at full tilt)
			static uint64_t last_pass = SDL_GetTicksNS();
			const uint64_t tn = SDL_GetTicksNS();
			if (!hidden && tn - last_pass < 2000000) SDL_DelayNS(2000000 - (tn - last_pass));
			last_pass = SDL_GetTicksNS();
		}
		// the title: the scale and shape, or (F9) the timings
		if (cfg.timings && SDL_GetTicksNS() - t9.t0 >= 2000000000ull) {
			const double secs = double(SDL_GetTicksNS() - t9.t0) / 1e9;
			char t[200];
			snprintf(t, sizeof t, "Virtua Fighter 3  %dx  %.1f frames/s  %.1f pictures/s  game %.2f ms/frame  picture %.2f ms", scale, t9.frames / secs,
					 t9.pictures / secs, t9.frames ? t9.emu_ms / t9.frames : 0.0, t9.pictures ? t9.draw_ms / t9.pictures : 0.0);
			SDL_SetWindowTitle(win, t);
			fprintf(stderr, "%s\n", t);
			t9 = {};
			t9.t0 = SDL_GetTicksNS();
		} else if (!cfg.timings) {
			char t[96];
			if (cfg.aspect) snprintf(t, sizeof t, "Virtua Fighter 3  %dx  %s", scale, ASPECT_NAMES[cfg.aspect]);
			else snprintf(t, sizeof t, "Virtua Fighter 3  %dx  fit (%.2f:1)", scale, picture_aspect());
			if (title_now != t) { title_now = t; SDL_SetWindowTitle(win, t); }
		}
		if (SDL_GetTicksNS() - pace.t0 > (pace_log ? 2000000000ull : 10000000000ull)) {
			const double all_s = double(SDL_GetTicksNS() - pace.t0) / 1e9, s = std::max(0.001, all_s - pace.stopped_s);
			const double pics = std::max(1, pace.pictures);
			char stopped_note[64] = "";
			if (pace.stopped_s >= 0.05) snprintf(stopped_note, sizeof(stopped_note), " (of %.1f s: %.1f s in the menu or paused)", all_s, pace.stopped_s);
			fprintf(stderr, "pace: %.1f frames/s, %.1f pictures/s%s (%llu between frames, %llu at a cut); a frame %.2f ms; a picture %.2f ms (%s %.2f, uploads %.2f), %dx (%d x %d)%s; sound queued %d bytes\n",
					pace.frames / s, pace.pictures / all_s, stopped_note, (unsigned long long)between_n, (unsigned long long)cut_n, pace.frames ? pace.emu_ms / pace.frames : 0.0,
					pace.draw_ms / pics, g3_shown ? "the GPU's set-up" : "the CPU's 3D", g3_shown ? pace.gpu_ms / pics : (pace.draw_ms - pace.upload_ms) / pics,
					pace.upload_ms / pics, scale, tex_w, tex_h, gov.smooth_off ? ", smooth motion held to 60 a second" : "", audio ? SDL_GetAudioStreamQueued(audio) : -1);
			if (game.free_stream) fprintf(stderr, "  a match's sound, to the real clock: ran dry %d times\n", game.sound_dry.exchange(0));
			if (const std::string pl = sysinfo::placement(); !pl.empty()) fprintf(stderr, "  %s\n", pl.c_str());
			if (g3_shown && pics > 0)
				fprintf(stderr, "  a picture's 3D: the in-between %.2f ms, the triangles prepared %.2f ms, the GPU's renderer %.2f ms (its set-up %.2f, the rest waiting on the GPU)\n",
						pace.interp_ms / pics, pace.prep_ms / pics, pace.g3_wall_ms / pics, pace.gpu_ms / pics);
			if (const int ns = pace.slow[0] + pace.slow[1] + pace.slow[2] + pace.slow[3])
				fprintf(stderr, "  passes over a refresh and a half: %d (pictures only %d, first halves %d, second halves %d, whole frames %d), their work before the present %.2f ms\n",
						ns, pace.slow[0], pace.slow[1], pace.slow[2], pace.slow[3], pace.slow_work_ms / ns);
			if (game.emu_thread.joinable()) game.job_wait();   // (its counts: not while a frame's adding to them)
			if (game.part_n) {
				const double n = double(game.part_n);
				fprintf(stderr, "  a frame's parts: the board %.2f ms, waiting for the sound %.2f, the scene %.2f, the 2D layers %.2f, the sky %.2f\n",
						game.part_ms[0] / n, game.part_ms[1] / n, game.part_ms[2] / n, game.part_ms[3] / n, game.part_ms[4] / n);
			}
			for (double &v : game.part_ms) v = 0;
			game.part_n = 0;
			pace = {};
			pace.t0 = SDL_GetTicksNS();
			between_n = cut_n = 0;
		}
		// the settings (a hotkey's change: Steam can end the game without a quit) saved every 3 s when changed, and the
		// NVRAM (the game writes its rankings and settings as it goes: the arcade's bookkeeping changes it all through
		// play) once a minute, as VF1's and VF2's; at the quit, whatever's changed
		static uint64_t nvram_t = SDL_GetTicksNS();
		if (SDL_GetTicksNS() - nvram_t > 60000000000ull) {
			nvram_t = SDL_GetTicksNS();
			save_nvram();
		}
		if (SDL_GetTicksNS() - save_t > 3000000000ull) {
			save_t = SDL_GetTicksNS();
			if (cfg_dirty && !menu.is_open() && quit_after < 0 && cfg.save(settings_path)) cfg_dirty = false;
		}
		// (a refresh and a half of this screen's: at 60 Hz every pass waiting on vsync was over the 120 Hz one's)
		if (!stopped && double(SDL_GetTicksNS() - pass_t0) > 1.5e9 / std::max(60.0, screen_hz)) { pace.slow[pass_kind]++; pace.slow_work_ms += double(pass_work_ns) / 1e6; }
		(void)shown;
	}
	if (game.emu_thread.joinable()) game.job_wait();   // (a match's step still running)
	if (flag_atlas) SDL_DestroySurface(flag_atlas);
	flag_atlas = nullptr;
	online.stop();   // (closed in a match: the other side is told at once, not after a silence)
	Online::flush(1500);   // (and the lobby told: a room closed, a search left)
	if (nvram_away) {   // (closed in a match: this player's own NVRAM back, to be saved, not the host's)
		if (game.emu_thread.joinable()) game.job_wait();
		board.backup = own_backup;
		memcpy(board.eeprom.regs, own_eeprom, sizeof own_eeprom);
		nvram_away = false;
	}
	saver.finish();
	saved_hash = ~nvram_hash();   // (the last copies written here whatever the writer did)
	save_nvram(true);
	rec_stop();
	if (cfg_dirty && quit_after < 0) cfg.save(settings_path);
	game.set_sound_free(nullptr, 0);   // (the sound thread off the stream first)
	if (audio) SDL_DestroyAudioStream(audio);
	for (auto &p : pads) if (p) SDL_CloseGamepad(p);
	menu_owner.reset();
	if (g3.tex) SDL_DestroyTexture(g3.tex);
	gpu3d.shutdown();
	SDL_DestroyTexture(tex3d);
	if (glass) SDL_DestroyTexture(glass);
	if (crt_tex) SDL_DestroyTexture(crt_tex);
	if (crt_state) SDL_DestroyGPURenderState(crt_state);
	if (crt_shader) SDL_ReleaseGPUShader(SDL_GetGPURendererDevice(ren), crt_shader);
	SDL_DestroyTexture(tex_below);
	SDL_DestroyTexture(tex_above);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	fprintf(stderr, "quit after %llu frames\n", (unsigned long long)game_frame);
	return 0;
}
