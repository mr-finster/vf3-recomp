#include "http.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <poll.h>
#endif

namespace vfnet::http {

namespace {
std::atomic<bool> cancelled{ false };
std::atomic<int> on_way{ 0 };
struct Counted { Counted() { on_way++; } ~Counted() { on_way--; } };
} // namespace

void cancel_all() { cancelled = true; }
int active() { return on_way.load(); }

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

Reply request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms, size_t max_reply)
{
	Reply r;
	if (cancelled) { r.error = "quitting"; return r; }
	Counted counted;
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
		if (cancelled || !WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
		if (r.body.size() + avail > max_reply) break;   // (the lobby's replies are small)
		std::string chunk(avail, '\0');
		DWORD got = 0;
		if (!WinHttpReadData(req.h, chunk.data(), avail, &got)) break;
		r.body.append(chunk.data(), got);
	}
	return r;
}

// (Windows: one session and one connection to the host kept, each request a new one on them: WinHTTP keeps the
// connection alive between them)
struct Client::Impl
{
	HINTERNET session = nullptr, conn = nullptr;
	std::wstring host;
	INTERNET_PORT port = 0;
	~Impl() { if (conn) WinHttpCloseHandle(conn); if (session) WinHttpCloseHandle(session); }
};
Client::Client() : m(new Impl) {}
Client::~Client() { delete m; }

namespace {
// One request on an open connection: its body read to max_reply (or to on_data, a stream's, while it says go on)
Reply over(HINTERNET conn, bool secure, const std::wstring &target, const std::string &method, const std::string &body, size_t max_reply,
           const std::function<bool(const char *, size_t)> *on_data)
{
	Reply r;
	Handle req(WinHttpOpenRequest(conn, wide(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
	if (!req.h) { r.error = "couldn't reach the online service"; return r; }
	DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
	const wchar_t *headers = L"Content-Type: application/json\r\n";
	if (!WinHttpSendRequest(req.h, headers, DWORD(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) ||
	    !WinHttpReceiveResponse(req.h, nullptr)) {
		r.error = "couldn't reach the online service (error " + std::to_string(GetLastError()) + ")";
		return r;
	}
	DWORD status = 0, size = sizeof status;
	WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
	r.status = int(status);
	for (;;) {
		DWORD avail = 0;
		if (cancelled || !WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
		std::string chunk(avail, '\0');
		DWORD got = 0;
		if (!WinHttpReadData(req.h, chunk.data(), avail, &got)) break;
		if (on_data) { if (!(*on_data)(chunk.data(), got)) break; continue; }
		if (r.body.size() + got > max_reply) break;
		r.body.append(chunk.data(), got);
	}
	return r;
}
bool crack(const std::string &url, std::wstring &host, INTERNET_PORT &port, bool &secure, std::wstring &target)
{
	const std::wstring wurl = wide(url);
	URL_COMPONENTS u{};
	u.dwStructSize = sizeof u;
	wchar_t h[256] = {}, path[2048] = {};
	u.lpszHostName = h;
	u.dwHostNameLength = 256;
	u.lpszUrlPath = path;
	u.dwUrlPathLength = 2048;
	u.dwSchemeLength = 1;
	u.dwExtraInfoLength = 1;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) return false;
	host.assign(h);
	port = u.nPort;
	secure = u.nScheme == INTERNET_SCHEME_HTTPS;
	target.assign(u.lpszUrlPath, u.dwUrlPathLength);
	if (u.lpszExtraInfo && u.dwExtraInfoLength) target += std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
	return true;
}
HINTERNET open_session(int timeout_ms, int receive_ms)
{
	HINTERNET s = WinHttpOpen(L"VFRecomp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!s) s = WinHttpOpen(L"VFRecomp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (s) WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, receive_ms);
	return s;
}
} // namespace

Reply Client::request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms, size_t max_reply)
{
	Reply r;
	if (cancelled) { r.error = "quitting"; return r; }
	Counted counted;
	std::wstring host, target;
	INTERNET_PORT port = 0;
	bool secure = false;
	if (!crack(url, host, port, secure, target)) { r.error = "a bad address for the online service"; return r; }
	if (!m->conn || host != m->host || port != m->port) {
		if (m->conn) WinHttpCloseHandle(m->conn);
		if (!m->session) m->session = open_session(timeout_ms, timeout_ms);
		m->conn = m->session ? WinHttpConnect(m->session, host.c_str(), port, 0) : nullptr;
		m->host = host;
		m->port = port;
	}
	if (!m->conn) { r.error = "couldn't reach the online service"; return r; }
	return over(m->conn, secure, target, method, body, max_reply, nullptr);
}

Reply stream(const std::string &url, const std::function<bool(const char *, size_t)> &on_data, int connect_ms, int idle_ms)
{
	Reply r;
	if (cancelled) { r.error = "quitting"; return r; }
	Counted counted;
	std::wstring host, target;
	INTERNET_PORT port = 0;
	bool secure = false;
	if (!crack(url, host, port, secure, target)) { r.error = "a bad address for the online service"; return r; }
	Handle session(open_session(connect_ms, idle_ms));
	if (!session.h) { r.error = "Windows' HTTP couldn't start"; return r; }
	Handle conn(WinHttpConnect(session.h, host.c_str(), port, 0));
	if (!conn.h) { r.error = "couldn't reach the online service"; return r; }
	return over(conn.h, secure, target, "GET", "", 0, &on_data);
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
		NOPROGRESS = 43, XFERINFOFUNCTION = 20219, XFERINFODATA = 10057, LOW_SPEED_LIMIT = 19, LOW_SPEED_TIME = 20,
		RESPONSE_CODE = 0x200002, GLOBAL_ALL = 3, HTTP_VERSION = 84, HTTP_VERSION_1_1 = 2,
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

struct Body { std::string *out; size_t max; };
size_t write_body(char *p, size_t size, size_t n, void *user)
{
	Body &b = *static_cast<Body *>(user);
	if (b.out->size() + size * n > b.max) return 0;   // (the lobby's replies are small)
	b.out->append(p, size * n);
	return size * n;
}

} // namespace

int progress(void *, int64_t, int64_t, int64_t, int64_t) { return cancelled ? 1 : 0; }   // (non-zero: curl gives it up)

Reply request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms, size_t max_reply)
{
	Reply r;
	if (cancelled) { r.error = "quitting"; return r; }
	Counted counted;
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
	Body into{ &r.body, max_reply };
	c.easy_setopt(h, Curl::WRITEDATA, &into);
	c.easy_setopt(h, Curl::NOPROGRESS, 0L);
	c.easy_setopt(h, Curl::XFERINFOFUNCTION, &progress);
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

// (curl: the one easy handle kept: its connection kept alive between the requests on it)
struct Client::Impl { void *h = nullptr; };
Client::Client() : m(new Impl) {}
Client::~Client()
{
	if (m->h && curl().ok) curl().easy_cleanup(m->h);
	delete m;
}

Reply Client::request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms, size_t max_reply)
{
	Reply r;
	if (cancelled) { r.error = "quitting"; return r; }
	Counted counted;
	Curl &c = curl();
	if (!c.ok) { r.error = "this system has no libcurl (install it, e.g. the libcurl4 package)"; return r; }
	if (!m->h && !(m->h = c.easy_init())) { r.error = "libcurl couldn't start"; return r; }
	void *h = m->h;
	void *headers = c.slist_append(nullptr, "Content-Type: application/json");
	Body into{ &r.body, max_reply };
	c.easy_setopt(h, Curl::URL, url.c_str());
	c.easy_setopt(h, Curl::NOSIGNAL, 1L);
	c.easy_setopt(h, Curl::TIMEOUT_MS, long(timeout_ms));
	c.easy_setopt(h, Curl::CONNECTTIMEOUT_MS, long(std::min(timeout_ms, 8000)));
	c.easy_setopt(h, Curl::USERAGENT, "VFRecomp");
	c.easy_setopt(h, Curl::HTTPHEADER, headers);
	c.easy_setopt(h, Curl::WRITEFUNCTION, &write_body);
	c.easy_setopt(h, Curl::WRITEDATA, &into);
	c.easy_setopt(h, Curl::NOPROGRESS, 0L);
	c.easy_setopt(h, Curl::XFERINFOFUNCTION, &progress);
	if (method == "GET") c.easy_setopt(h, Curl::HTTPGET, 1L);
	else {
		c.easy_setopt(h, Curl::POSTFIELDS, body.c_str());
		c.easy_setopt(h, Curl::POSTFIELDSIZE, long(body.size()));
		c.easy_setopt(h, Curl::CUSTOMREQUEST, method == "POST" ? nullptr : method.c_str());
	}
	const int rc = c.easy_perform(h);
	if (rc != 0) r.error = std::string("couldn't reach the online service (") + c.easy_strerror(rc) + ")";
	else {
		long status = 0;
		c.easy_getinfo(h, Curl::RESPONSE_CODE, &status);
		r.status = int(status);
	}
	c.easy_setopt(h, Curl::HTTPHEADER, nullptr);   // (the list's freed: not left in the handle)
	c.slist_free_all(headers);
	return r;
}

namespace {
struct Streamed { const std::function<bool(const char *, size_t)> *on_data; };
size_t stream_body(char *p, size_t size, size_t n, void *user)
{
	return (*static_cast<Streamed *>(user)->on_data)(p, size * n) ? size * n : 0;
}
} // namespace

Reply stream(const std::string &url, const std::function<bool(const char *, size_t)> &on_data, int connect_ms, int idle_ms)
{
	Reply r;
	if (cancelled) { r.error = "quitting"; return r; }
	Counted counted;
	Curl &c = curl();
	if (!c.ok) { r.error = "this system has no libcurl (install it, e.g. the libcurl4 package)"; return r; }
	void *h = c.easy_init();
	if (!h) { r.error = "libcurl couldn't start"; return r; }
	Streamed s{ &on_data };
	c.easy_setopt(h, Curl::URL, url.c_str());
	c.easy_setopt(h, Curl::NOSIGNAL, 1L);
	c.easy_setopt(h, Curl::CONNECTTIMEOUT_MS, long(connect_ms));
	c.easy_setopt(h, Curl::LOW_SPEED_LIMIT, 1L);   // (nothing at all for idle_ms: given up)
	c.easy_setopt(h, Curl::LOW_SPEED_TIME, long(std::max(1, idle_ms / 1000)));
	c.easy_setopt(h, Curl::USERAGENT, "VFRecomp");
	c.easy_setopt(h, Curl::WRITEFUNCTION, &stream_body);
	c.easy_setopt(h, Curl::WRITEDATA, &s);
	c.easy_setopt(h, Curl::NOPROGRESS, 0L);
	c.easy_setopt(h, Curl::XFERINFOFUNCTION, &progress);
	c.easy_setopt(h, Curl::HTTPGET, 1L);
	const int rc = c.easy_perform(h);
	long status = 0;
	c.easy_getinfo(h, Curl::RESPONSE_CODE, &status);
	r.status = int(status);
	if (rc != 0) r.error = std::string("the stream ended (") + c.easy_strerror(rc) + ")";
	c.easy_cleanup(h);
	return r;
}

#endif

// ---- a WebSocket (Socket) ----
FrameReader::Got FrameReader::take(std::string &buf, const std::function<void(std::string &&, bool)> &on_message, const std::function<void(const std::string &)> &on_ping)
{
	// (the frames taken off the front all at once as it returns, not each as it's read: thousands of tiny ones in the
	// buffer, each moving the rest down, would take ages)
	size_t at = 0;
	struct Eat { std::string &buf; const size_t &at; ~Eat() { buf.erase(0, at); } } eat{ buf, at };
	for (;;) {
		const size_t left = buf.size() - at;
		const char *f = buf.data() + at;
		if (left < 2) return Got::more;
		const uint8_t b0 = uint8_t(f[0]), b1 = uint8_t(f[1]);
		const bool fin = b0 & 0x80, masked = b1 & 0x80;
		const uint8_t op = b0 & 15;
		if (b0 & 0x70) { why = "a reserved bit set"; return Got::closed; }
		if (masked) { why = "a frame masked by the server"; return Got::closed; }
		if ((op > 2 && op < 8) || op > 10) { why = "an opcode the protocol doesn't have"; return Got::closed; }
		uint64_t n = b1 & 127;
		size_t hl = 2;
		if (n == 126) {
			if (left < 4) return Got::more;
			n = (uint64_t(uint8_t(f[2])) << 8) | uint8_t(f[3]);
			hl = 4;
		} else if (n == 127) {
			if (left < 10) return Got::more;
			n = 0;
			for (int i = 0; i < 8; i++) n = (n << 8) | uint8_t(f[2 + i]);
			hl = 10;
		}
		if (op >= 8 && (!fin || n > 125)) { why = "a control frame fragmented or too long"; return Got::closed; }
		if (n > max || (op == 0 && msg.size() + n > max)) { why = "a message too long"; return Got::closed; }
		if (left - hl < n) return Got::more;
		std::string data(f + hl, size_t(n));
		at += hl + size_t(n);
		if (op == 8) { why = "the lobby closed it"; return Got::closed; }
		if (op == 9) { on_ping(data); continue; }
		if (op == 10) continue;
		if (op == 0) {
			if (!in_msg) { why = "a continuation with no message begun"; return Got::closed; }
			msg += data;
		} else {
			if (in_msg) { why = "a new message in the middle of one"; return Got::closed; }
			msg = std::move(data);
			msg_binary = op == 2;
			in_msg = true;
		}
		if (fin) {
			in_msg = false;
			std::string whole = std::move(msg);
			msg.clear();
			on_message(std::move(whole), msg_binary);
		}
	}
}

// The state both platforms share: the queue out, what came in, the connection's state; the thread that runs it.
struct Socket::Impl
{
	std::string url;
	mutable std::mutex lock;
	std::condition_variable wake;
	std::deque<Message> out, in;
	size_t in_bytes = 0;   // (what's come and not been taken, its bytes: a bound, as its count)
	State st = State::connecting;
	int status = 0;
	std::string error;
	std::atomic<bool> stop{ false };
	std::thread worker;
#ifdef _WIN32
	// (WinHTTP's: the request the connecting waits on, closed by the thread letting it go so the wait ends at once, not
	// at its timeout; cut once it has been)
	std::mutex wait_lock;
	HINTERNET waiting_on = nullptr;
	bool cut = false;
	void cut_short()
	{
		std::lock_guard<std::mutex> l(wait_lock);
		if (waiting_on) { WinHttpCloseHandle(waiting_on); waiting_on = nullptr; cut = true; }
	}
#endif
	void closed(const std::string &why)
	{
		std::lock_guard<std::mutex> l(lock);
		if (st != State::closed && error.empty()) error = why;
		st = State::closed;
	}
	void got(std::string data, bool binary)
	{
		if (!binary && (data == "pong" || data == "ping")) return;   // (the keeping alive)
		std::lock_guard<std::mutex> l(lock);
		if (in.size() >= 4096 || in_bytes + data.size() > (16u << 20)) return;
		in_bytes += data.size();
		in.push_back({ std::move(data), binary });
	}
	bool next_out(Message &m, int wait_ms)
	{
		std::unique_lock<std::mutex> l(lock);
		if (out.empty() && wait_ms > 0) wake.wait_for(l, std::chrono::milliseconds(wait_ms), [&] { return !out.empty() || stop; });
		if (out.empty()) return false;
		m = std::move(out.front());
		out.pop_front();
		return true;
	}
	void run();   // (each platform's: connect, upgrade, then send and receive till it's closed or stopped)
};

Socket::Socket(const std::string &url) : m(new Impl)
{
	m->url = url;
	m->worker = std::thread([i = m] { i->run(); });
}
Socket::~Socket()
{
	m->stop = true;
	m->wake.notify_all();
#ifdef _WIN32
	m->cut_short();
#endif
	if (m->worker.joinable()) m->worker.join();
	delete m;
}
void Socket::send(std::string data, bool binary)
{
	{
		std::lock_guard<std::mutex> l(m->lock);
		if (m->st == State::closed || m->out.size() >= 256) return;
		m->out.push_back({ std::move(data), binary });
	}
	m->wake.notify_all();
}
bool Socket::take(Message &msg)
{
	std::lock_guard<std::mutex> l(m->lock);
	if (m->in.empty()) return false;
	msg = std::move(m->in.front());
	m->in.pop_front();
	m->in_bytes -= std::min(m->in_bytes, msg.data.size());
	return true;
}
Socket::State Socket::state() const { std::lock_guard<std::mutex> l(m->lock); return m->st; }
int Socket::status() const { std::lock_guard<std::mutex> l(m->lock); return m->status; }
std::string Socket::error() const { std::lock_guard<std::mutex> l(m->lock); return m->error; }

namespace {
constexpr int PING_MS = 15000, IDLE_MS = 45000;
}

#ifdef _WIN32

void Socket::Impl::run()
{
	Counted counted;
	std::wstring host, target;
	INTERNET_PORT port = 0;
	bool secure = false;
	if (cancelled || !crack(url, host, port, secure, target)) { closed("a bad address for the online service"); return; }
	if (stop) { closed(""); return; }
	Handle session(open_session(8000, IDLE_MS));
	if (!session.h) { closed("Windows' HTTP couldn't start"); return; }
	Handle conn(WinHttpConnect(session.h, host.c_str(), port, 0));
	if (!conn.h) { closed("couldn't reach the online service"); return; }
	Handle req(WinHttpOpenRequest(conn.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
	if (!req.h) { closed("couldn't reach the online service"); return; }
	WinHttpSetOption(req.h, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
	// (the connecting, its TLS and the upgrade's answer: let go meanwhile, its request closed under it, the wait ends)
	{
		std::lock_guard<std::mutex> l(wait_lock);
		if (stop) { closed(""); return; }
		waiting_on = req.h;
	}
	const bool answered = WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req.h, nullptr);
	{
		std::lock_guard<std::mutex> l(wait_lock);
		waiting_on = nullptr;
		if (cut) req.h = nullptr;   // (closed already)
	}
	if (stop) { closed(""); return; }
	if (!answered) {
		closed("couldn't reach the online service");
		return;
	}
	DWORD code = 0, size = sizeof code;
	WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
	{ std::lock_guard<std::mutex> l(lock); status = int(code); }
	if (code != 101) { closed("the lobby didn't take the WebSocket (" + std::to_string(code) + ")"); return; }
	Handle ws(WinHttpWebSocketCompleteUpgrade(req.h, 0));
	if (!ws.h) { closed("the WebSocket couldn't start"); return; }
	{ std::lock_guard<std::mutex> l(lock); st = State::open; }
	// (a receive waits on its own thread; WinHTTP lets one send go on meanwhile)
	std::atomic<bool> reading{ true };
	const HINTERNET wsh = ws.h;
	std::thread reader([&, wsh] {
		std::string msg;
		std::vector<char> buf(65536);
		for (;;) {
			DWORD n_read = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
			if (WinHttpWebSocketReceive(wsh, buf.data(), DWORD(buf.size()), &n_read, &type) != NO_ERROR) break;
			if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
			msg.append(buf.data(), n_read);
			if (msg.size() > FrameReader::MOST) break;
			if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
				got(std::move(msg), type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE);
				msg.clear();
			}
		}
		reading = false;
	});
	auto last_ping = std::chrono::steady_clock::now();
	while (!stop && !cancelled && reading) {
		Message mm;
		if (next_out(mm, 100)) {
			if (WinHttpWebSocketSend(ws.h, mm.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
			                         mm.data.empty() ? nullptr : mm.data.data(), DWORD(mm.data.size())) != NO_ERROR)
				break;
		}
		if (std::chrono::steady_clock::now() - last_ping > std::chrono::milliseconds(PING_MS)) {
			last_ping = std::chrono::steady_clock::now();
			char ping[] = "ping";
			WinHttpWebSocketSend(ws.h, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, ping, 4);
		}
	}
	// (the close said, not waited for; then the handle closed under the reader's receive, which ends with it at once:
	// WinHttpWebSocketClose would wait for the lobby's close, and with a receive on its way may never return)
	WinHttpWebSocketShutdown(wsh, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
	WinHttpCloseHandle(wsh);
	ws.h = nullptr;
	reader.join();
	closed(stop ? "" : "the connection ended");
}

#else

namespace {
// curl's own, for a connection it only makes (the WebSocket's bytes written here), each optional: an old curl, or
// one without them, and the game asks over HTTPS as before
struct CurlRaw
{
	int (*send)(void *, const void *, size_t, size_t *) = nullptr;
	int (*recv)(void *, void *, size_t, size_t *) = nullptr;
	bool ok = false;
	enum { CONNECT_ONLY = 141, ACTIVESOCKET = 0x500000 + 44, AGAIN = 81 };
};
CurlRaw &raw()
{
	static CurlRaw r;
	static std::once_flag once;
	std::call_once(once, [] {
		if (!curl().ok) return;
		for (const char *name : {
#ifdef __APPLE__
				 "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib",
#else
				 "libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", "libcurl-nss.so.4",
#endif
			 }) {
			void *lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
			if (!lib) continue;
			r.send = reinterpret_cast<decltype(r.send)>(dlsym(lib, "curl_easy_send"));
			r.recv = reinterpret_cast<decltype(r.recv)>(dlsym(lib, "curl_easy_recv"));
			r.ok = r.send && r.recv;
			break;
		}
	});
	return r;
}
int socket_progress(void *stop, int64_t, int64_t, int64_t, int64_t) { return cancelled || static_cast<std::atomic<bool> *>(stop)->load() ? 1 : 0; }
std::string base64(const uint8_t *p, size_t n)
{
	static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string s;
	for (size_t i = 0; i < n; i += 3) {
		const uint32_t v = (uint32_t(p[i]) << 16) | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		s += t[v >> 18]; s += t[(v >> 12) & 63];
		s += i + 1 < n ? t[(v >> 6) & 63] : '=';
		s += i + 2 < n ? t[v & 63] : '=';
	}
	return s;
}
} // namespace

void Socket::Impl::run()
{
	Counted counted;
	Curl &c = curl();
	CurlRaw &rw = raw();
	if (!c.ok || !rw.ok) { closed("this system's libcurl can't keep a connection of its own"); return; }
	// (the address: its host and port for the Host line, its path and query for the request; ws/wss as http/https)
	const bool secure = url.rfind("https://", 0) == 0;
	const size_t at = url.find("://");
	if (at == std::string::npos) { closed("a bad address for the online service"); return; }
	const size_t slash = url.find('/', at + 3);
	const std::string hostport = url.substr(at + 3, slash == std::string::npos ? std::string::npos : slash - at - 3);
	const std::string target = slash == std::string::npos ? "/" : url.substr(slash);
	void *h = c.easy_init();
	if (!h) { closed("libcurl couldn't start"); return; }
	struct Done { Curl &c; void *h; ~Done() { c.easy_cleanup(h); } } done{ c, h };
	c.easy_setopt(h, Curl::URL, (std::string(secure ? "https://" : "http://") + hostport + "/").c_str());
	c.easy_setopt(h, CurlRaw::CONNECT_ONLY, 1L);
	// (HTTP/1.1 offered, nothing else: the upgrade below is written here as HTTP/1.1, and a curl with HTTP/2 (macOS's,
	// most Linux's) would offer h2 too, the lobby's TLS take it, and the connection be HTTP/2's, the upgrade refused)
	c.easy_setopt(h, Curl::HTTP_VERSION, long(Curl::HTTP_VERSION_1_1));
	c.easy_setopt(h, Curl::NOSIGNAL, 1L);
	c.easy_setopt(h, Curl::CONNECTTIMEOUT_MS, 8000L);
	// (its connecting and TLS given up at a quit or once it's let go, as a request's: not up to 8 s later, a thread still
	// in curl's as the program ends)
	c.easy_setopt(h, Curl::NOPROGRESS, 0L);
	c.easy_setopt(h, Curl::XFERINFOFUNCTION, &socket_progress);
	c.easy_setopt(h, Curl::XFERINFODATA, static_cast<void *>(&stop));
	if (cancelled) { closed("quitting"); return; }
	if (const int rc = c.easy_perform(h); rc != 0) { closed(std::string("couldn't reach the online service (") + c.easy_strerror(rc) + ")"); return; }
	int fd = -1;   // (a curl_socket_t: an int, on these systems)
	c.easy_getinfo(h, CurlRaw::ACTIVESOCKET, &fd);
	if (fd < 0) { closed("couldn't reach the online service"); return; }
	// the bytes, both ways: what's ready now, else a wait on the socket for at most wait_ms
	auto send_all = [&](const std::string &b) {
		size_t off = 0;
		for (int spins = 0; off < b.size() && spins < 2000 && !stop && !cancelled; spins++) {
			size_t n = 0;
			const int rc = rw.send(h, b.data() + off, b.size() - off, &n);
			if (rc == 0) { off += n; continue; }
			if (rc != CurlRaw::AGAIN) return false;
			pollfd p{ int(fd), POLLOUT, 0 };
			poll(&p, 1, 10);
		}
		return off == b.size();
	};
	std::string buf;   // (what's come and not yet made a message)
	FrameReader reader;
	auto fill = [&](int wait_ms) {   // false: closed
		char tmp[16384];
		for (bool waited = false;;) {
			// (no more than a frame at its longest held: the rest left with the connection till this is taken; a frame
			// longer than that, or a head that never ends, is refused below)
			const size_t room = reader.room(buf);
			if (room == 0) return true;
			size_t n = 0;
			const int rc = rw.recv(h, tmp, std::min(sizeof tmp, room), &n);
			if (rc == 0 && n == 0) return false;   // (the other end closed it)
			if (rc == 0) { buf.append(tmp, n); waited = true; continue; }
			if (rc != CurlRaw::AGAIN) return false;
			if (waited || wait_ms <= 0) return true;
			pollfd p{ int(fd), POLLIN, 0 };
			poll(&p, 1, wait_ms);
			waited = true;
		}
	};
	// the upgrade: a GET with the WebSocket's headers; its answer's head read to its blank line
	std::mt19937 rng(uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ uint32_t(reinterpret_cast<uintptr_t>(this)));
	uint8_t key[16];
	for (uint8_t &k : key) k = uint8_t(rng());
	const std::string ask = "GET " + target + " HTTP/1.1\r\nHost: " + hostport + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(key, 16) +
	                        "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: VFRecomp\r\n\r\n";
	if (!send_all(ask)) { closed("couldn't reach the online service"); return; }
	const auto t0 = std::chrono::steady_clock::now();
	size_t head_end = std::string::npos;
	while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
		// (a refusal's whole answer and its close can come together: its head read before the close is taken for none)
		const bool more = !stop && !cancelled && std::chrono::steady_clock::now() - t0 <= std::chrono::seconds(10) && buf.size() <= 16384 && fill(200);
		if (!more && (head_end = buf.find("\r\n\r\n")) == std::string::npos) {
			closed("the lobby didn't answer the WebSocket");
			return;
		}
		if (!more) break;
	}
	const int code = buf.size() > 12 && buf.rfind("HTTP/1.1 ", 0) == 0 ? atoi(buf.c_str() + 9) : 0;
	{ std::lock_guard<std::mutex> l(lock); status = code; }
	if (code != 101) { closed("the lobby didn't take the WebSocket (" + std::to_string(code) + ")"); return; }
	buf.erase(0, head_end + 4);
	{ std::lock_guard<std::mutex> l(lock); st = State::open; }
	// frames: ours masked (as a client's must be), FIN set, one frame a message; theirs read whole, a ping answered
	auto frame = [&](uint8_t op, const std::string &data) {
		std::string f;
		f += char(0x80 | op);
		const size_t n = data.size();
		if (n < 126) f += char(0x80 | n);
		else if (n < 65536) { f += char(0x80 | 126); f += char(n >> 8); f += char(n & 255); }
		else { f += char(0x80 | 127); for (int i = 7; i >= 0; i--) f += char((uint64_t(n) >> (8 * i)) & 255); }
		uint8_t mask[4];
		for (uint8_t &k : mask) k = uint8_t(rng());
		f.append(reinterpret_cast<const char *>(mask), 4);
		for (size_t i = 0; i < n; i++) f += char(uint8_t(data[i]) ^ mask[i & 3]);
		return send_all(f);
	};
	auto last_in = std::chrono::steady_clock::now(), last_ping = last_in;
	std::string why = "the connection ended";
	while (!stop && !cancelled) {
		Message mm;
		bool sent_ok = true;
		while (sent_ok && next_out(mm, 0)) sent_ok = frame(mm.binary ? 2 : 1, mm.data);
		if (!sent_ok) break;
		const auto now = std::chrono::steady_clock::now();
		if (now - last_ping > std::chrono::milliseconds(PING_MS)) { last_ping = now; if (!frame(1, "ping")) break; }
		if (now - last_in > std::chrono::milliseconds(IDLE_MS)) { why = "nothing from the lobby for 45 s"; break; }
		const size_t had = buf.size();
		const bool open = fill(20);   // (a wait for more even with a frame's start held: what's left after take is never a whole frame)
		if (buf.size() != had) last_in = std::chrono::steady_clock::now();   // (anything at all from the lobby: it's there)
		// (each whole frame in the buffer)
		bool pong_ok = true;
		const FrameReader::Got g = reader.take(buf, [&](std::string &&m, bool binary) { got(std::move(m), binary); },
		                                       [&](const std::string &p) { pong_ok = pong_ok && frame(10, p); });
		if (g == FrameReader::Got::closed) { why = reader.why; break; }
		if (!pong_ok) break;
		if (!open) break;   // (closed: what came with its close taken first, the lobby's last word)
	}
	if (!stop) { frame(8, ""); }
	closed(stop ? "" : why);
}

#endif

} // namespace vfnet::http
