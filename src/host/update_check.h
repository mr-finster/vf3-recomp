// "Version X is out": at the start, at most once a day, the game asks GitHub for its own repository's latest release
// and, if it's newer than this build, says so in the menu (a line whose Get it opens the release's page in the
// browser) and once in a short note. It only asks and tells: nothing's ever downloaded or installed. Game > Check for
// updates Off: no request at all. Not on Android or iOS (their stores and sideloads have their own ways).
//
// Shared with VF1, VF2 and VF3 Recomp as it is (each game names its repository, its version and its env hook). Its own
// HTTPS, not src/net's (which is the lobby's and stays as it is): the same system libcurl (macOS, Linux: loaded as the
// game runs, its certificates and proxy settings the system's), WinHTTP on Windows, with a User-Agent naming the game
// and its version and GitHub's Accept header. Header only: no build file changes for it.
//
// A test points it elsewhere (the game's VFn_UPDATE_URL): http(s)://... (a tiny server), or file://... or a path (a
// JSON file read as a 200). The reply: one release (/releases/latest's object) or a list (/releases' array, the
// newest non-draft taken; a pre-release only for a build that is one itself).
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "../net/json.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace update_check {

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr bool AVAILABLE = false;   // (the setting not shown, never a request)
#else
constexpr bool AVAILABLE = true;
#endif

constexpr int64_t DAY = 24 * 60 * 60;
constexpr const char *PAGE_PREFIX = "https://github.com/mr-finster/";   // (the only pages Get it opens)

// ---- versions ----
// A release's tag or this build's version: v1.8.0, v0.1.22-alpha, 0.1.7-online-preview. Numbers compared as numbers
// (0.1.9 < 0.1.10), a missing one 0; a pre-release suffix older than none (0.1.22-alpha < 0.1.22), two suffixes by
// semver's rules (dot-separated parts, numeric ones as numbers). Not a version: git describe's builds between releases
// (v0.1.22-alpha-15-g8613e4c), -dirty, a bare commit, "dev", anything else.
struct version
{
	std::vector<long> num;
	std::string pre;   // (after the first -; empty: a release proper)
};

inline bool parse(const std::string &tag, version &v)
{
	v = version();
	size_t i = 0;
	const size_t n = tag.size();
	if (n > 64) return false;
	if (i < n && (tag[i] == 'v' || tag[i] == 'V')) i++;
	for (;;) {
		if (i >= n || !isdigit(uint8_t(tag[i]))) return false;
		long x = 0;
		int digits = 0;
		while (i < n && isdigit(uint8_t(tag[i]))) {
			if (++digits > 9) return false;
			x = x * 10 + (tag[i++] - '0');
		}
		v.num.push_back(x);
		if (i < n && tag[i] == '.') { i++; continue; }
		break;
	}
	if (v.num.size() > 4) return false;
	if (i == n) return true;
	if (tag[i] != '-') return false;
	v.pre = tag.substr(i + 1);
	if (v.pre.empty() || v.pre.size() > 40 || !isalpha(uint8_t(v.pre[0]))) return false;
	for (char c : v.pre)
		if (!isalnum(uint8_t(c)) && c != '.' && c != '-') return false;
	// (git describe between tags: ...-<commits>-g<hash>; a tree with changes: -dirty)
	std::vector<std::string> parts;
	for (size_t a = 0;;) {
		const size_t b = v.pre.find('-', a);
		parts.push_back(v.pre.substr(a, b == std::string::npos ? std::string::npos : b - a));
		if (b == std::string::npos) break;
		a = b + 1;
	}
	for (const std::string &p : parts)
		if (p.empty() || p == "dirty") return false;
	if (parts.size() >= 2) {
		const std::string &h = parts.back(), &c = parts[parts.size() - 2];
		const bool hash = h.size() >= 5 && h[0] == 'g' && std::all_of(h.begin() + 1, h.end(), [](char x) { return isxdigit(uint8_t(x)) != 0; });
		const bool count = std::all_of(c.begin(), c.end(), [](char x) { return isdigit(uint8_t(x)) != 0; });
		if (hash && count) return false;
	}
	return true;
}

// <0, 0, >0 as a is older than, the same as, newer than b
inline int compare(const version &a, const version &b)
{
	for (size_t i = 0; i < std::max(a.num.size(), b.num.size()); i++) {
		const long x = i < a.num.size() ? a.num[i] : 0, y = i < b.num.size() ? b.num[i] : 0;
		if (x != y) return x < y ? -1 : 1;
	}
	if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;
	auto split = [](const std::string &s) {
		std::vector<std::string> out;
		for (size_t p = 0;;) {
			const size_t q = s.find('.', p);
			out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
			if (q == std::string::npos) break;
			p = q + 1;
		}
		return out;
	};
	const auto pa = split(a.pre), pb = split(b.pre);
	auto numeric = [](const std::string &s) { return !s.empty() && s.size() < 10 && std::all_of(s.begin(), s.end(), [](char c) { return isdigit(uint8_t(c)) != 0; }); };
	for (size_t i = 0; i < std::min(pa.size(), pb.size()); i++) {
		const bool na = numeric(pa[i]), nb = numeric(pb[i]);
		if (na && nb) {
			const long x = atol(pa[i].c_str()), y = atol(pb[i].c_str());
			if (x != y) return x < y ? -1 : 1;
		} else if (na != nb) return na ? -1 : 1;   // (a number before a word)
		else if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
	}
	return pa.size() == pb.size() ? 0 : pa.size() < pb.size() ? -1 : 1;
}

// true only if both are versions and tag's strictly the newer (the same tag again, a hotfix's, is not)
inline bool newer(const std::string &tag, const std::string &running)
{
	version a, b;
	return parse(tag, a) && parse(running, b) && compare(a, b) > 0;
}

// "v0.1.23-alpha" as the menu shows it: "0.1.23-alpha"
inline std::string shown(const std::string &tag) { return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag; }

// a release's page fit to open: on GitHub under mr-finster, nothing odd in it
inline bool page_ok(const std::string &url)
{
	if (url.size() > 300 || url.compare(0, strlen(PAGE_PREFIX), PAGE_PREFIX) != 0) return false;
	for (char c : url)
		if (uint8_t(c) <= ' ' || uint8_t(c) >= 0x7f || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\') return false;
	return true;
}

// ---- the reply ----
struct result
{
	enum class what { newer, none, failed } is = what::failed;
	std::string tag, url;   // (newer: the release's)
	std::string why;        // (for the log: what was found, or why nothing was)
};

inline result read_reply(int status, const std::string &body, const std::string &running)
{
	result r;
	if (status != 200) {
		r.why = status ? "GitHub answered " + std::to_string(status) + (status == 403 || status == 429 ? " (its rate limit?)" : "") : "no answer";
		return r;
	}
	vfnet::json::Value v;
	if (!vfnet::json::parse(body, v) || (v.kind != vfnet::json::Value::object && v.kind != vfnet::json::Value::list)) {
		r.why = "the answer wasn't JSON";
		return r;
	}
	version mine;
	const bool proper = parse(running, mine);
	std::vector<const vfnet::json::Value *> all;
	if (v.kind == vfnet::json::Value::object) all.push_back(&v);
	else for (const auto &x : v.items) all.push_back(&x);
	const vfnet::json::Value *best = nullptr;
	version best_v;
	for (const vfnet::json::Value *x : all) {
		if (x->kind != vfnet::json::Value::object) continue;
		const auto &d = (*x)["draft"], &pre = (*x)["prerelease"];
		if (d.kind == vfnet::json::Value::boolean && d.b) continue;
		if (pre.kind == vfnet::json::Value::boolean && pre.b && !(proper && !mine.pre.empty())) continue;   // (pre-releases: only for one)
		version t;
		if (!parse((*x)["tag_name"].str(), t) || !page_ok((*x)["html_url"].str())) continue;
		if (!best || compare(t, best_v) > 0) { best = x; best_v = t; }
	}
	if (!best) {
		r.why = "no release in the answer";
		return r;
	}
	r.tag = (*best)["tag_name"].str();
	r.url = (*best)["html_url"].str();
	if (!proper) {
		r.is = result::what::none;
		r.why = "the latest is " + r.tag + "; this build (" + running + ") has no release version, so it never says";
	} else if (compare(best_v, mine) > 0) {
		r.is = result::what::newer;
		r.why = r.tag + " is out (this is " + running + ")";
	} else {
		r.is = result::what::none;
		r.why = "the latest is " + r.tag + ": this one (" + running + ") is up to date";
	}
	return r;
}

// due: never checked, a day since the last, or a clock put back (a check "from the future")
inline bool due(int64_t last, int64_t now) { return last <= 0 || now - last >= DAY || last - now > DAY; }

// ---- the request ----
struct reply { int status = 0; std::string body, error; };
constexpr size_t MOST = 1u << 20;   // (/releases/latest is some 10 KB; a list of 10 with their notes about 110 KB)
inline std::atomic<bool> quitting{ false };

inline reply read_file(const std::string &path)
{
	reply r;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) { r.error = "couldn't read " + path; return r; }
	char buf[16384];
	size_t got;
	while ((got = fread(buf, 1, sizeof buf, f)) > 0 && r.body.size() < MOST) r.body.append(buf, got);
	fclose(f);
	r.status = 200;
	return r;
}

#ifdef _WIN32
inline std::wstring wide(const std::string &s)
{
	if (s.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

inline reply get(const std::string &url, const std::string &agent, int timeout_ms)
{
	if (url.rfind("file://", 0) == 0) return read_file(url.substr(7));
	if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return read_file(url);
	reply r;
	struct H { HINTERNET h; ~H() { if (h) WinHttpCloseHandle(h); } };
	const std::wstring wurl = wide(url);
	URL_COMPONENTS u{};
	u.dwStructSize = sizeof u;
	wchar_t host[256] = {}, path[2048] = {};
	u.lpszHostName = host;
	u.dwHostNameLength = 256;
	u.lpszUrlPath = path;
	u.dwUrlPathLength = 2048;
	u.dwSchemeLength = 1;
	u.dwExtraInfoLength = 1;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) { r.error = "a bad address"; return r; }
	std::wstring target(u.lpszUrlPath, u.dwUrlPathLength);
	if (u.lpszExtraInfo && u.dwExtraInfoLength) target += std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
	const std::wstring wagent = wide(agent);
	H session{ WinHttpOpen(wagent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
	if (!session.h) session.h = WinHttpOpen(wagent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session.h) { r.error = "Windows' HTTP couldn't start"; return r; }
	WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
	H conn{ WinHttpConnect(session.h, host, u.nPort, 0) };
	if (!conn.h) { r.error = "couldn't connect"; return r; }
	H req{ WinHttpOpenRequest(conn.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                          u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) };
	if (!req.h) { r.error = "couldn't connect"; return r; }
	if (!WinHttpSendRequest(req.h, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(req.h, nullptr)) {
		r.error = "couldn't connect (error " + std::to_string(GetLastError()) + ")";
		return r;
	}
	DWORD status = 0, size = sizeof status;
	WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
	r.status = int(status);
	for (;;) {
		DWORD avail = 0;
		if (quitting || !WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
		if (r.body.size() + avail > MOST) { r.error = "the answer too long"; r.status = 0; break; }
		std::string chunk(avail, '\0');
		DWORD got = 0;
		if (!WinHttpReadData(req.h, chunk.data(), avail, &got)) break;
		r.body.append(chunk.data(), got);
	}
	return r;
}
#else
// libcurl, as src/net/http.cpp loads it (the same libraries, the same ABI numbers)
struct curl_fns
{
	void *(*easy_init)() = nullptr;
	int (*easy_setopt)(void *, int, ...) = nullptr;
	int (*easy_perform)(void *) = nullptr;
	void (*easy_cleanup)(void *) = nullptr;
	int (*easy_getinfo)(void *, int, ...) = nullptr;
	const char *(*easy_strerror)(int) = nullptr;
	void *(*slist_append)(void *, const char *) = nullptr;
	void (*slist_free_all)(void *) = nullptr;
	bool ok = false;
	enum {
		URL = 10002, HTTPHEADER = 10023, WRITEFUNCTION = 20011, WRITEDATA = 10001, TIMEOUT_MS = 155, CONNECTTIMEOUT_MS = 156,
		NOSIGNAL = 99, USERAGENT = 10018, HTTPGET = 80, NOPROGRESS = 43, XFERINFOFUNCTION = 20219, FOLLOWLOCATION = 52,
		MAXREDIRS = 68, RESPONSE_CODE = 0x200002,
	};
};

inline curl_fns &curl()
{
	static curl_fns c;
	static std::once_flag once;
	std::call_once(once, [] {
		void *lib = nullptr;
		for (const char *name : {
#ifdef __APPLE__
				 "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib",
#else
				 "libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", "libcurl-nss.so.4",
#endif
			 })
			if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
		if (!lib) return;
		auto sym = [&](auto &f, const char *n) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, n)); return f != nullptr; };
		c.ok = sym(c.easy_init, "curl_easy_init") && sym(c.easy_setopt, "curl_easy_setopt") && sym(c.easy_perform, "curl_easy_perform") &&
		       sym(c.easy_cleanup, "curl_easy_cleanup") && sym(c.easy_getinfo, "curl_easy_getinfo") && sym(c.easy_strerror, "curl_easy_strerror") &&
		       sym(c.slist_append, "curl_slist_append") && sym(c.slist_free_all, "curl_slist_free_all");
		// (curl_global_init: src/net's, when the lobby's used; curl_easy_init does it itself otherwise)
	});
	return c;
}

inline size_t curl_body(char *p, size_t size, size_t n, void *user)
{
	std::string &b = *static_cast<std::string *>(user);
	if (b.size() + size * n > MOST) return 0;
	b.append(p, size * n);
	return size * n;
}
inline int curl_progress(void *, int64_t, int64_t, int64_t, int64_t) { return quitting ? 1 : 0; }   // (the game quitting: given up)

inline reply get(const std::string &url, const std::string &agent, int timeout_ms)
{
	if (url.rfind("file://", 0) == 0) return read_file(url.substr(7));
	if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return read_file(url);
	reply r;
	curl_fns &c = curl();
	if (!c.ok) { r.error = "this system has no libcurl"; return r; }
	void *h = c.easy_init();
	if (!h) { r.error = "libcurl couldn't start"; return r; }
	void *headers = c.slist_append(nullptr, "Accept: application/vnd.github+json");
	c.easy_setopt(h, curl_fns::URL, url.c_str());
	c.easy_setopt(h, curl_fns::NOSIGNAL, 1L);
	c.easy_setopt(h, curl_fns::TIMEOUT_MS, long(timeout_ms));
	c.easy_setopt(h, curl_fns::CONNECTTIMEOUT_MS, long(std::min(timeout_ms, 5000)));
	c.easy_setopt(h, curl_fns::USERAGENT, agent.c_str());
	c.easy_setopt(h, curl_fns::HTTPHEADER, headers);
	c.easy_setopt(h, curl_fns::FOLLOWLOCATION, 1L);   // (a repository renamed: GitHub's API moves it with a redirect)
	c.easy_setopt(h, curl_fns::MAXREDIRS, 3L);
	c.easy_setopt(h, curl_fns::WRITEFUNCTION, &curl_body);
	c.easy_setopt(h, curl_fns::WRITEDATA, &r.body);
	c.easy_setopt(h, curl_fns::NOPROGRESS, 0L);
	c.easy_setopt(h, curl_fns::XFERINFOFUNCTION, &curl_progress);
	c.easy_setopt(h, curl_fns::HTTPGET, 1L);
	const int rc = c.easy_perform(h);
	if (rc != 0) r.error = std::string("couldn't connect (") + c.easy_strerror(rc) + ")";
	else {
		long status = 0;
		c.easy_getinfo(h, curl_fns::RESPONSE_CODE, &status);
		r.status = int(status);
	}
	c.slist_free_all(headers);
	c.easy_cleanup(h);
	return r;
}
#endif

// ---- the check, on a thread of its own ----
// start() once; the game's loop asks take() each pass (cheap) until it gives the result. Let go as the game quits:
// a request still on is given up (curl), or left to end on its own (Windows: the process ends under it).
class checker
{
public:
	void start(const std::string &url, const std::string &agent, const std::string &running)
	{
		auto s = st;
		t = std::thread([s, url, agent, running] {
			const reply rp = get(url, agent, 8000);
			result r;
			if (!rp.error.empty()) r.why = rp.error;
			else r = read_reply(rp.status, rp.body, running);
			std::lock_guard<std::mutex> lock(s->m);
			s->r = r;
			s->done = true;
		});
	}
	bool take(result &out)
	{
		if (!st->done.load() || taken) return false;
		std::lock_guard<std::mutex> lock(st->m);
		out = st->r;
		taken = true;
		return true;
	}
	~checker()
	{
		quitting = true;
		if (!t.joinable()) return;
#ifdef _WIN32
		if (!st->done) { t.detach(); return; }
#endif
		t.join();
	}
private:
	struct state { std::mutex m; std::atomic<bool> done{ false }; result r; };
	std::shared_ptr<state> st = std::make_shared<state>();
	std::thread t;
	bool taken = false;
};

} // namespace update_check
