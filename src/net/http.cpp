#include "http.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <type_traits>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace vfnet::http {

#ifdef _WIN32

namespace {

std::wstring wide(const std::string &s)
{
	if (s.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

struct Handle
{
	HINTERNET h = nullptr;
	explicit Handle(HINTERNET x) : h(x) {}
	~Handle() { if (h) WinHttpCloseHandle(h); }
};

} // namespace

Reply request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms)
{
	Reply r;
	std::wstring wurl = wide(url);
	URL_COMPONENTS u{};
	u.dwStructSize = sizeof u;
	wchar_t host[256] = {}, path[2048] = {};
	u.lpszHostName = host;
	u.dwHostNameLength = 256;
	u.lpszUrlPath = path;
	u.dwUrlPathLength = 2048;
	u.dwSchemeLength = 1;
	u.dwExtraInfoLength = 1;   // (the query: asked for, so it comes with the path below)
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) { r.error = "a bad address for the online service"; return r; }
	std::wstring target(u.lpszUrlPath, u.dwUrlPathLength);
	if (u.lpszExtraInfo && u.dwExtraInfoLength) target += std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
	Handle session(WinHttpOpen(L"VFRecomp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.h) session.h = WinHttpOpen(L"VFRecomp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session.h) { r.error = "Windows' HTTP couldn't start"; return r; }
	WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
	Handle conn(WinHttpConnect(session.h, host, u.nPort, 0));
	if (!conn.h) { r.error = "couldn't reach the online service"; return r; }
	const bool secure = u.nScheme == INTERNET_SCHEME_HTTPS;
	Handle req(WinHttpOpenRequest(conn.h, wide(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                              secure ? WINHTTP_FLAG_SECURE : 0));
	if (!req.h) { r.error = "couldn't reach the online service"; return r; }
	DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;   // (as curl: a redirect isn't followed, nor the request sent elsewhere)
	WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
	const wchar_t *headers = L"Content-Type: application/json\r\n";
	if (!WinHttpSendRequest(req.h, headers, DWORD(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()), DWORD(body.size()),
	                        DWORD(body.size()), 0) ||
	    !WinHttpReceiveResponse(req.h, nullptr)) {
		r.error = "couldn't reach the online service (error " + std::to_string(GetLastError()) + ")";
		return r;
	}
	DWORD status = 0, size = sizeof status;
	WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
	r.status = int(status);
	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
		if (r.body.size() + avail > 65536) break;   // (the lobby's replies are small)
		std::string chunk(avail, '\0');
		DWORD got = 0;
		if (!WinHttpReadData(req.h, chunk.data(), avail, &got)) break;
		r.body.append(chunk.data(), got);
	}
	return r;
}

#else

namespace {

// libcurl's functions and the options used (its ABI's numbers, the same on every version since 7.x)
struct Curl
{
	void *(*easy_init)() = nullptr;
	int (*easy_setopt)(void *, int, ...) = nullptr;
	int (*easy_perform)(void *) = nullptr;
	void (*easy_cleanup)(void *) = nullptr;
	int (*easy_getinfo)(void *, int, ...) = nullptr;
	const char *(*easy_strerror)(int) = nullptr;
	void *(*slist_append)(void *, const char *) = nullptr;
	void (*slist_free_all)(void *) = nullptr;
	int (*global_init)(long) = nullptr;
	bool ok = false;
	enum {
		URL = 10002, POSTFIELDS = 10015, POSTFIELDSIZE = 60, HTTPHEADER = 10023, WRITEFUNCTION = 20011, WRITEDATA = 10001,
		TIMEOUT_MS = 155, CONNECTTIMEOUT_MS = 156, NOSIGNAL = 99, USERAGENT = 10018, CUSTOMREQUEST = 10036, HTTPGET = 80,
		RESPONSE_CODE = 0x200002, GLOBAL_ALL = 3,
	};
};

Curl &curl()
{
	static Curl c;
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
		auto get = [&](auto &f, const char *n) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, n)); return f != nullptr; };
		c.ok = get(c.easy_init, "curl_easy_init") && get(c.easy_setopt, "curl_easy_setopt") && get(c.easy_perform, "curl_easy_perform") &&
		       get(c.easy_cleanup, "curl_easy_cleanup") && get(c.easy_getinfo, "curl_easy_getinfo") && get(c.easy_strerror, "curl_easy_strerror") &&
		       get(c.slist_append, "curl_slist_append") && get(c.slist_free_all, "curl_slist_free_all") && get(c.global_init, "curl_global_init");
		if (c.ok) c.global_init(Curl::GLOBAL_ALL);
	});
	return c;
}

size_t write_body(char *p, size_t size, size_t n, void *user)
{
	std::string &out = *static_cast<std::string *>(user);
	if (out.size() + size * n > 65536) return 0;   // (the lobby's replies are small)
	out.append(p, size * n);
	return size * n;
}

} // namespace

Reply request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms)
{
	Reply r;
	Curl &c = curl();
	if (!c.ok) { r.error = "this system has no libcurl (install it, e.g. the libcurl4 package)"; return r; }
	void *h = c.easy_init();
	if (!h) { r.error = "libcurl couldn't start"; return r; }
	void *headers = c.slist_append(nullptr, "Content-Type: application/json");
	c.easy_setopt(h, Curl::URL, url.c_str());
	c.easy_setopt(h, Curl::NOSIGNAL, 1L);
	c.easy_setopt(h, Curl::TIMEOUT_MS, long(timeout_ms));
	c.easy_setopt(h, Curl::CONNECTTIMEOUT_MS, long(std::min(timeout_ms, 8000)));
	c.easy_setopt(h, Curl::USERAGENT, "VFRecomp");
	c.easy_setopt(h, Curl::HTTPHEADER, headers);
	c.easy_setopt(h, Curl::WRITEFUNCTION, &write_body);
	c.easy_setopt(h, Curl::WRITEDATA, &r.body);
	if (method == "GET") c.easy_setopt(h, Curl::HTTPGET, 1L);
	else {
		c.easy_setopt(h, Curl::POSTFIELDS, body.c_str());
		c.easy_setopt(h, Curl::POSTFIELDSIZE, long(body.size()));
		if (method != "POST") c.easy_setopt(h, Curl::CUSTOMREQUEST, method.c_str());
	}
	const int rc = c.easy_perform(h);
	if (rc != 0) r.error = std::string("couldn't reach the online service (") + c.easy_strerror(rc) + ")";
	else {
		long status = 0;
		c.easy_getinfo(h, Curl::RESPONSE_CODE, &status);
		r.status = int(status);
	}
	c.slist_free_all(headers);
	c.easy_cleanup(h);
	return r;
}

#endif

} // namespace vfnet::http
