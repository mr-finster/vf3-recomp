#include "links.h"
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace links {

namespace {

const std::string CODE_LETTERS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";   // (the lobby's: no 0/O, 1/I/L)

std::string lower(std::string s)
{
	for (char &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

std::string exe_path()
{
#ifdef _WIN32
	wchar_t w[MAX_PATH * 2] = {};
	const DWORD n = GetModuleFileNameW(nullptr, w, DWORD(sizeof w / sizeof w[0]));
	if (!n || n >= sizeof w / sizeof w[0]) return "";
	const int len = WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr);
	std::string s(size_t(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, int(n), s.data(), len, nullptr, nullptr);
	return s;
#elif defined(__linux__)
	std::error_code ec;
	const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
	return ec ? "" : p.string();
#else
	return "";
#endif
}

std::string read_file(const std::filesystem::path &p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

std::string read_start(const std::filesystem::path &p, size_t most)   // (at most that much: a file in the data folder, whatever it is)
{
	std::ifstream f(p, std::ios::binary);
	std::string s(most, '\0');
	f.read(s.data(), std::streamsize(most));
	s.resize(size_t(f.gcount()));
	return s;
}

} // namespace

bool is_link(const std::string &arg)
{
	const std::string s = std::string(SCHEME) + ":";
	return arg.size() >= s.size() && lower(arg.substr(0, s.size())) == s;
}

// vf3recomp://GAME/ACTION/CODE, the code with or without its dash, a / at the end or not: that and nothing else
bool parse(const std::string &url, Link &out)
{
	if (url.size() > 64) return false;
	const std::string head = std::string(SCHEME) + "://";
	if (url.size() <= head.size() || lower(url.substr(0, head.size())) != head) return false;
	std::string rest = url.substr(head.size());
	if (!rest.empty() && rest.back() == '/') rest.pop_back();
	const size_t a = rest.find('/'), b = a == std::string::npos ? a : rest.find('/', a + 1);
	if (a == std::string::npos || b == std::string::npos || rest.find('/', b + 1) != std::string::npos) return false;
	const std::string game = lower(rest.substr(0, a)), action = lower(rest.substr(a + 1, b - a - 1));
	std::string code;
	bool dash = false;
	for (char c : rest.substr(b + 1)) {
		if (c == '-' && code.size() == 3 && !dash) { dash = true; continue; }   // (one dash, after the third letter)
		c = char(toupper(uint8_t(c)));
		if (CODE_LETTERS.find(c) == std::string::npos) return false;
		code += c;
	}
	if ((game != "vf3" && game != "vf3tb") || (action != "join" && action != "watch") || code.size() != 6) return false;
	out = { game, action, code };
	return true;
}

std::string register_scheme()
{
	const std::string exe = exe_path();
	if (exe.empty()) return "";
#ifdef _WIN32
	// HKCU\Software\Classes\vf3recomp: this user's, no administrator; written again only when the program's moved
	auto wide = [](const std::string &s) {
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
		std::wstring w(size_t(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
		return w;
	};
	const std::wstring key = L"Software\\Classes\\" + wide(SCHEME), command = L"\"" + wide(exe) + L"\" \"%1\"";
	wchar_t now[MAX_PATH * 2 + 16] = {};
	DWORD size = sizeof now;
	if (RegGetValueW(HKEY_CURRENT_USER, (key + L"\\shell\\open\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, now, &size) == ERROR_SUCCESS && command == now) return "";
	const std::wstring name = L"URL:Virtua Fighter 3 Recomp";
	auto set = [](const std::wstring &k, const wchar_t *value_name, const std::wstring &v) {
		return RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), value_name, REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
	};
	const bool ok = set(key, nullptr, name) && set(key, L"URL Protocol", L"") && set(key + L"\\shell\\open\\command", nullptr, command);
	return ok ? "links: vf3recomp:// opens this program (Windows, this user)" : "links: couldn't tell Windows this program opens vf3recomp://";
#elif defined(__linux__)
	// a .desktop file of this user's saying it opens x-scheme-handler/vf3recomp, and that as the default (mimeapps.list),
	// unless the player chose another
	const char *home = getenv("HOME");
	if (!home || !*home) return "";
	const char *xdg = getenv("XDG_DATA_HOME"), *cfg = getenv("XDG_CONFIG_HOME");
	const std::filesystem::path apps = std::filesystem::path(xdg && *xdg ? xdg : std::string(home) + "/.local/share") / "applications";
	const std::filesystem::path desktop = apps / (std::string(SCHEME) + "-links.desktop");
	// (the Steam Deck's package: its launcher beside the program, run from its own folder)
	const std::filesystem::path launcher = std::filesystem::path(exe).parent_path() / "VirtuaFighter3.sh";
	std::error_code e0;
	const std::string run = std::filesystem::is_regular_file(launcher, e0) ? launcher.string() : exe;
	// (the Desktop Entry spec's Exec: in its quotes " ` $ and \ each after a \, and every \ of that written \\ in the
	// file, as any string's; a % doubled, not taken for a %u. A line's end can't be in it at all)
	std::error_code ec;
	std::string exec = run;
	// (a % in the path, right by the spec doubled: GLib looks for the program by Exec's own text, %% and all, finds none
	// and won't open the link. Such a program run by a script of this user's, in the data folder, whose path has none)
	if (run.find('%') != std::string::npos) {
		const std::filesystem::path script = apps.parent_path() / (std::string(SCHEME) + "-links.sh");
		std::string text = "#!/bin/sh\nexec '";
		for (char c : run) text += c == '\'' ? std::string("'\\''") : std::string(1, c);
		text += "' \"$@\"\n";
		if (read_file(script) != text) {
			std::filesystem::create_directories(script.parent_path(), ec);
			std::ofstream f(script, std::ios::binary | std::ios::trunc);
			f << text;
			if (!f) return "links: couldn't write " + script.string();
		}
		using std::filesystem::perms;
		std::filesystem::permissions(script, perms::owner_exec | perms::group_exec | perms::others_exec, std::filesystem::perm_options::add, ec);
		exec = script.string();
	}
	std::string quoted;
	for (char c : exec) {
		if (uint8_t(c) < 0x20 || c == 0x7f) return "links: the program's folder has a character a .desktop file can't hold: not registered";
		if (c == '"' || c == '`' || c == '$') quoted += "\\\\";
		else if (c == '\\') quoted += "\\\\\\";
		else if (c == '%') quoted += '%';
		quoted += c;
	}
	const std::string entry = "[Desktop Entry]\nType=Application\nName=Virtua Fighter 3 Recomp\nExec=\"" + quoted + "\" %u\nNoDisplay=true\nMimeType=x-scheme-handler/" + SCHEME + ";\n";
	std::string done;
	if (read_file(desktop) != entry) {
		std::filesystem::create_directories(apps, ec);
		std::ofstream f(desktop, std::ios::binary | std::ios::trunc);
		f << entry;
		if (!f) return "links: couldn't write " + desktop.string();
		done = "links: vf3recomp:// opens this program (" + desktop.string() + ")";
	}
	const std::filesystem::path list = std::filesystem::path(cfg && *cfg ? cfg : std::string(home) + "/.config") / "mimeapps.list";
	std::string text = read_file(list);
	const std::string line = std::string("x-scheme-handler/") + SCHEME + "=";
	if (text.find(line) == std::string::npos) {   // (the default only if none's set: a player's own choice left alone)
		const std::string want = line + desktop.filename().string() + "\n";
		const size_t at = text.find("[Default Applications]");
		if (at == std::string::npos) text += (text.empty() || text.back() == '\n' ? "" : "\n") + std::string("[Default Applications]\n") + want;
		else if (text.find('\n', at) == std::string::npos) text += "\n" + want;   // (the header the file's last line, no line's end after it)
		else text.insert(text.find('\n', at) + 1, want);
		std::filesystem::create_directories(list.parent_path(), ec);
		std::ofstream f(list, std::ios::binary | std::ios::trunc);
		f << text;
		if (f && done.empty()) done = "links: vf3recomp:// opens this program (" + list.string() + ")";
	}
	return done;
#else
	return "";   // (a Mac: the app's Info.plist says so)
#endif
}

// The copy running: its process id in the data folder; a link for it in link.txt there
namespace {
std::filesystem::path pid_file(const std::string &data) { return std::filesystem::path(data) / "running.pid"; }
std::filesystem::path link_file(const std::string &data) { return std::filesystem::path(data) / "link.txt"; }
// (the process with that number is a copy of this program: running.pid outlives a crash, and its number is soon
// another program's; a link handed to that one would be lost, this copy gone too)
bool alive(long pid)
{
	if (pid <= 0) return false;
#ifdef _WIN32
	if (DWORD(pid) == GetCurrentProcessId()) return false;
	HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
	if (!h) return false;
	bool on = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
	wchar_t theirs[MAX_PATH * 2] = {}, mine[MAX_PATH * 2] = {};
	DWORD n = DWORD(sizeof theirs / sizeof theirs[0]);
	const DWORD m = GetModuleFileNameW(nullptr, mine, DWORD(sizeof mine / sizeof mine[0]));
	on = on && QueryFullProcessImageNameW(h, 0, theirs, &n) && m && m < sizeof mine / sizeof mine[0] && _wcsicmp(theirs, mine) == 0;
	CloseHandle(h);
	return on;
#else
	if (pid == long(getpid()) || kill(pid_t(pid), 0) != 0) return false;
#if defined(__linux__)
	std::error_code e1, e2;
	const std::filesystem::path theirs = std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", e1), mine = std::filesystem::read_symlink("/proc/self/exe", e2);
	return !e1 && !e2 && theirs == mine;
#else
	return true;
#endif
#endif
}
long own_pid()
{
#ifdef _WIN32
	return long(GetCurrentProcessId());
#else
	return long(getpid());
#endif
}
} // namespace

bool hand_over(const std::string &data, const std::string &url)
{
#ifdef __APPLE__
	(void)data; (void)url;
	return false;   // (the system gives the running app its links)
#else
	Link l;
	if (!parse(url, l)) return false;
	const long pid = atol(read_file(pid_file(data)).c_str());
	if (!alive(pid)) return false;
	const std::filesystem::path part = link_file(data).string() + "." + std::to_string(own_pid());
	bool ok = false;
	{
		std::ofstream f(part, std::ios::binary | std::ios::trunc);
		f << std::string(SCHEME) << "://" << l.game << "/" << l.action << "/" << l.code;   // (written again from what was read: only that)
		ok = bool(f.flush());
	}
	std::error_code ec;
	if (ok) std::filesystem::rename(part, link_file(data), ec);   // (in place whole: take never reads half of it)
	if (!ok || ec) { std::filesystem::remove(part, ec); return false; }
#ifdef _WIN32
	AllowSetForegroundWindow(DWORD(pid));   // (this copy has the foreground, the link's: the one running may come to the front with it)
#endif
	return true;
#endif
}

void running(const std::string &data)
{
	// (another copy on this data folder running already: it stays the one links are handed to, this one never
	// taking its place, so a link never starts a third; this one takes over once that's gone: take)
#ifndef __APPLE__   // (a Mac's links come from the system, to the app it has running: the file only its own)
	if (alive(atol(read_file(pid_file(data)).c_str()))) return;
#endif
	std::ofstream f(pid_file(data), std::ios::binary | std::ios::trunc);
	f << own_pid();
}

void stopped(const std::string &data)
{
	std::error_code ec;
	if (atol(read_file(pid_file(data)).c_str()) == own_pid()) std::filesystem::remove(pid_file(data), ec);
}

bool take(const std::string &data, std::string &url)
{
	if (atol(read_file(pid_file(data)).c_str()) != own_pid()) running(data);   // (the copy links went to gone: this one's now)
	std::error_code ec;
	if (!std::filesystem::exists(link_file(data), ec)) return false;
	const auto at = std::filesystem::last_write_time(link_file(data), ec);
	const bool old = !ec && std::filesystem::file_time_type::clock::now() - at > std::chrono::seconds(60);
	url = read_start(link_file(data), 64);
	std::filesystem::remove(link_file(data), ec);
	if (old) return false;   // (handed to a copy that closed before it took it: not acted on now, at another start)
	Link l;
	return parse(url, l);
}

} // namespace links
