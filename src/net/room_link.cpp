#include "room_link.h"
#include "stun.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static const sock_t NO_SOCKET = INVALID_SOCKET;
static void close_socket(sock_t s) { closesocket(s); }
static std::string last_error() { return "error " + std::to_string(WSAGetLastError()); }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
static const sock_t NO_SOCKET = -1;
static void close_socket(sock_t s) { close(s); }
static std::string last_error() { return strerror(errno); }
#endif

namespace vfnet {

namespace {

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// "1.2.3.4:5678" / "[v6]:5678": its address part
std::string ip_of(const std::string &a)
{
	if (!a.empty() && a[0] == '[') return a.substr(1, a.find(']') - 1);
	const size_t c = a.rfind(':');
	return c == std::string::npos ? a : a.substr(0, c);
}

// A socket address as text, IPv4 mapped into IPv6 shown as IPv4
std::string text_of(const sockaddr_storage &ss)
{
	char host[INET6_ADDRSTRLEN] = {};
	if (ss.ss_family == AF_INET6) {
		const sockaddr_in6 &a = reinterpret_cast<const sockaddr_in6 &>(ss);
		const uint8_t *b = reinterpret_cast<const uint8_t *>(&a.sin6_addr);
		static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
		if (!memcmp(b, mapped, 12)) {
			inet_ntop(AF_INET, b + 12, host, sizeof host);
			return std::string(host) + ":" + std::to_string(ntohs(a.sin6_port));
		}
		inet_ntop(AF_INET6, &a.sin6_addr, host, sizeof host);
		return "[" + std::string(host) + "]:" + std::to_string(ntohs(a.sin6_port));
	}
	const sockaddr_in &a = reinterpret_cast<const sockaddr_in &>(ss);
	inet_ntop(AF_INET, &a.sin_addr, host, sizeof host);
	return std::string(host) + ":" + std::to_string(ntohs(a.sin_port));
}

// One way of writing an address, whoever wrote it (the STUN parser writes IPv6 in full, the socket's in short, a
// lobby in either case): compared as text, they must be written the same. "" if it isn't one.
bool sockaddr_of(const std::string &a, bool v6_socket, sockaddr_storage &ss, socklen_t &len);
std::string canon(const std::string &a)
{
	sockaddr_storage ss{};
	socklen_t len;
	return sockaddr_of(a, true, ss, len) ? text_of(ss) : std::string();
}

// "ip:port" (numbers only) as a socket address for a socket of this family (IPv4 mapped on an IPv6 one)
bool sockaddr_of(const std::string &a, bool v6_socket, sockaddr_storage &ss, socklen_t &len)
{
	std::string host = ip_of(a);
	const size_t c = a.rfind(':');
	if (c == std::string::npos) return false;
	const int port = atoi(a.c_str() + c + 1);
	if (port <= 0 || port > 65535) return false;
	memset(&ss, 0, sizeof ss);
	in_addr v4{};
	in6_addr v6{};
	if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
		if (v6_socket) {
			sockaddr_in6 &s6 = reinterpret_cast<sockaddr_in6 &>(ss);
			s6.sin6_family = AF_INET6;
#if defined(__APPLE__) || defined(__FreeBSD__)
			s6.sin6_len = sizeof s6;
#endif
			s6.sin6_port = htons(uint16_t(port));
			uint8_t *b = reinterpret_cast<uint8_t *>(&s6.sin6_addr);
			b[10] = b[11] = 0xFF;
			memcpy(b + 12, &v4, 4);
			len = sizeof s6;
		} else {
			sockaddr_in &s4 = reinterpret_cast<sockaddr_in &>(ss);
			s4.sin_family = AF_INET;
#if defined(__APPLE__) || defined(__FreeBSD__)
			s4.sin_len = sizeof s4;
#endif
			s4.sin_port = htons(uint16_t(port));
			s4.sin_addr = v4;
			len = sizeof s4;
		}
		return true;
	}
	if (v6_socket && inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
		sockaddr_in6 &s6 = reinterpret_cast<sockaddr_in6 &>(ss);
		s6.sin6_family = AF_INET6;
#if defined(__APPLE__) || defined(__FreeBSD__)
		s6.sin6_len = sizeof s6;
#endif
		s6.sin6_port = htons(uint16_t(port));
		s6.sin6_addr = v6;
		len = sizeof s6;
		return true;
	}
	return false;
}

// "ip:port" as STUN carries it
bool stun_of(const std::string &a, stun::Address &x)
{
	sockaddr_storage ss;
	socklen_t len = 0;
	if (!sockaddr_of(a, false, ss, len)) {   // (IPv6)
		if (!sockaddr_of(a, true, ss, len)) return false;
		const sockaddr_in6 &s6 = reinterpret_cast<const sockaddr_in6 &>(ss);
		x.v6 = true;
		memcpy(x.ip, &s6.sin6_addr, 16);
		x.port = ntohs(s6.sin6_port);
		return true;
	}
	const sockaddr_in &s4 = reinterpret_cast<const sockaddr_in &>(ss);
	x.v6 = false;
	memcpy(x.ip, &s4.sin_addr, 4);
	x.port = ntohs(s4.sin_port);
	return true;
}

// A private network's address (192.168.x.x and the like, IPv6's fc00::/7 and fe80::/10): a relay on the internet
// can't reach it, so nothing's sent to it through one
bool private_address(const std::string &a)
{
	const std::string ip = ip_of(a);
	unsigned b[4];
	if (sscanf(ip.c_str(), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4)
		return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
	std::string l = ip.substr(0, 2);
	for (char &c : l) c = char(tolower(uint8_t(c)));
	return !ip.empty() && (l == "fc" || l == "fd" || l == "fe");
}

// An address a player's game can have, as the other side gives it (through the lobby): not this machine's own
// (loopback: unless testing on one machine), nor none, multicast or broadcast, nor port 0. A game is never made to
// send to anything else on the other side's say-so.
bool player_address(const std::string &a, bool loopback_ok)
{
	const std::string ip = ip_of(a);
	const size_t colon = a.rfind(':');
	if (ip.empty() || colon == std::string::npos || atoi(a.c_str() + colon + 1) <= 0 || atoi(a.c_str() + colon + 1) > 65535) return false;
	unsigned b[4];
	if (sscanf(ip.c_str(), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4)
		return b[0] <= 255 && b[1] <= 255 && b[2] <= 255 && b[3] <= 255 && b[0] != 0 && b[0] < 224 && (b[0] != 127 || loopback_ok);
	std::string l = ip;
	for (char &c : l) c = char(tolower(c));
	if (l == "::" || l.rfind("ff", 0) == 0 || l.rfind("::ffff:", 0) == 0) return false;   // (mapped IPv4: given as IPv4, or not at all)
	return l != "::1" || loopback_ok;
}

std::string tid_key(const uint8_t tid[12]) { return std::string(reinterpret_cast<const char *>(tid), 12); }

} // namespace

struct RoomLink::Impl
{
	sock_t s = NO_SOCKET;
	bool v6 = true;
#ifdef _WIN32
	bool wsa = false;
#endif
	double started = now_ms();   // (gathering's start: when the servers were given)

	// STUN: this side's public address
	std::string stun_server, pub;
	int stun_tries = 0;
	double stun_next = 0;
	bool stun_done = true;

	// TURN: a relay address, kept alive
	enum Turn { OFF, ASKING, READY, FAILED } turn = OFF;
	std::string turn_server, user, password, realm, nonce, relay;
	std::vector<uint8_t> key;
	int turn_tries = 0;
	std::string alloc_tid;            // the latest ALLOCATE asked: its id, and its bytes (sent again as they are)
	std::vector<uint8_t> alloc_bytes;
	bool alloc_keyed = false;          // (with the login: a 401 to it is the login refused)
	double turn_first = 0;             // (when the first ALLOCATE went: the relay's time to answer counts from it)
	double turn_next = 0, refresh_at = 0, perm_at = 0;
	bool perm_dirty = false;
	double lifetime_s = 600;
	std::map<std::string, uint16_t> pending;   // a request's transaction id: its kind (to ask again on a stale nonce)
	std::map<std::string, std::string> perm_of;   // a permission request's transaction id: the address it's for

	// for the log, when a connection can't be made: what went each way, and what the relay said
	struct Count { int sent = 0, failed = 0, got = 0, knocks = 0; int error = 0; };
	std::map<std::string, Count> counts;   // by way: "d " or "r " and the address
	struct Said { std::string text, addr, after; bool operator==(const Said &o) const { return text == o.text && addr == o.addr && after == o.after; } };
	std::vector<Said> said;                 // what the relay said ("the relay refused " 1.2.3.4 ": 403")
	int strangers = 0;                      // datagrams from no one expected
	std::vector<std::string> stranger_from; // (the first few's addresses)
	void note(const std::string &text, const std::string &addr = "", const std::string &after = "")
	{
		const Said w{ text, addr, after };
		if (said.size() < 12 && std::find(said.begin(), said.end(), w) == said.end()) said.push_back(w);
	}

	// the other player
	bool host = false, have_peer = false, relay_only = false, loopback_ok = false;
	std::vector<std::string> peer;   // their addresses, the relay's among them
	std::string peer_relay;
	struct Way { std::string to; bool relay = false; bool operator==(const Way &o) const { return to == o.to && relay == o.relay; } };
	bool have_last = false, is_locked = false;
	Way last, way;
	double punch_until = 0, punch_next = 0;

	~Impl()
	{
		if (s != NO_SOCKET) {
			if (turn == READY) refresh(0);   // (the relay let go at once, not left to time out)
			close_socket(s);
		}
#ifdef _WIN32
		if (wsa) WSACleanup();
#endif
	}

	void send_to(const std::string &to, const uint8_t *p, size_t n)
	{
		sockaddr_storage ss;
		socklen_t len = 0;
		if (s == NO_SOCKET || !sockaddr_of(to, v6, ss, len)) return;
		sendto(s, reinterpret_cast<const char *>(p), int(n), 0, reinterpret_cast<const sockaddr *>(&ss), len);
	}
	// (the other player's ways: counted, and why a send failed kept)
	void send_counted(const std::string &to, const uint8_t *p, size_t n)
	{
		sockaddr_storage ss;
		socklen_t len = 0;
		Count &c = counts["d " + to];
		c.sent++;
		if (s == NO_SOCKET || !sockaddr_of(to, v6, ss, len)) { c.failed++; return; }
		if (sendto(s, reinterpret_cast<const char *>(p), int(n), 0, reinterpret_cast<const sockaddr *>(&ss), len) < 0) {
			c.failed++;
#ifdef _WIN32
			c.error = WSAGetLastError();
#else
			c.error = errno;
#endif
		}
	}
	void send_to(const std::string &to, const std::vector<uint8_t> &b) { send_to(to, b.data(), b.size()); }

	// ---- STUN ----
	void binding()
	{
		stun::Message q;
		q.type = stun::BINDING;
		stun::random_tid(q.tid);
		pending[tid_key(q.tid)] = stun::BINDING;
		send_to(stun_server, q.encode());
	}

	// ---- TURN ----
	std::vector<uint8_t> turn_send(stun::Message &q)
	{
		stun::random_tid(q.tid);
		if (!key.empty()) {
			q.add_string(stun::A_USERNAME, user);
			q.add_string(stun::A_REALM, realm);
			q.add_string(stun::A_NONCE, nonce);
		}
		pending[tid_key(q.tid)] = q.type;
		std::vector<uint8_t> bytes = q.encode(key);
		send_to(turn_server, bytes);
		if (pending.size() > 64) pending.erase(pending.begin());
		return bytes;
	}
	// A new ALLOCATE (at the start, or with a new login or nonce); one not answered is sent again as it was, the same
	// id: a new id for the same thing is another request to the relay, and the second of two is refused (437)
	void allocate()
	{
		stun::Message q;
		q.type = stun::ALLOCATE;
		q.add_u32(stun::A_TRANSPORT, 17u << 24);   // (UDP)
		q.add_u32(stun::A_LIFETIME, 600);
		alloc_keyed = !key.empty();
		alloc_bytes = turn_send(q);
		alloc_tid = tid_key(q.tid);
		turn_next = now_ms() + 500;
	}
	void refresh(uint32_t lifetime)
	{
		stun::Message q;
		q.type = stun::REFRESH;
		q.add_u32(stun::A_LIFETIME, lifetime);
		turn_send(q);
	}
	// One request for each of the other player's addresses a relay can reach: a relay refuses a request with any
	// address it won't relay to (a private network's, as Cloudflare's does), and one refused mustn't take the rest
	void permit()
	{
		for (const std::string &a : peer) permit_one(a);
	}
	void permit_one(const std::string &a)
	{
		stun::Address x;
		if (private_address(a) || !stun_of(a, x)) return;
		x.port = 0;
		stun::Message q;
		q.type = stun::PERMISSION;
		q.add_address(stun::A_PEER, x);
		turn_send(q);
		perm_of[tid_key(q.tid)] = a;
		if (perm_of.size() > 64) perm_of.erase(perm_of.begin());
	}
	void turn_reply(const stun::Message &r)
	{
		auto it = pending.find(tid_key(r.tid));
		if (it == pending.end()) return;   // (not a reply to anything asked: a stranger's, or one too late)
		const uint16_t asked = it->second;
		pending.erase(it);
		const int code = r.error_code();
		std::string perm_for;
		if (auto p = perm_of.find(tid_key(r.tid)); p != perm_of.end()) { perm_for = p->second; perm_of.erase(p); }
		// an ALLOCATE's answer counts only for the latest asked, and only while it's being asked (a late one's
		// answer, or one to a copy, says nothing of the allocation there is)
		if (asked == stun::ALLOCATE && (turn != ASKING || tid_key(r.tid) != alloc_tid)) return;
		if (code == 401 || code == 438) {   // (a login wanted, or a nonce gone stale: again with the new one)
			const std::string rl = r.get_string(stun::A_REALM), nc = r.get_string(stun::A_NONCE);
			if (nc.empty()) { if (asked == stun::ALLOCATE) { turn = FAILED; note("the relay refused this side: " + std::to_string(code)); } return; }
			if (code == 401 && asked == stun::ALLOCATE && alloc_keyed && rl == realm) { turn = FAILED; note("the relay refused the login"); return; }
			if (!rl.empty()) realm = rl;
			nonce = nc;
			key = stun::long_term_key(user, realm, password);
			if (asked == stun::ALLOCATE) { turn_tries = std::min(turn_tries, 4); allocate(); }   // (the login's own tries)
			else if (asked == stun::REFRESH) refresh(uint32_t(lifetime_s));
			else if (asked == stun::PERMISSION && !perm_for.empty()) permit_one(perm_for);   // (that one again: not all of them)
			return;
		}
		if (r.type == stun::ALLOCATE_OK) {
			stun::Address a;
			if (r.get_address(stun::A_RELAYED, a)) relay = canon(a.text());
			if (pub.empty() && r.get_address(stun::A_XOR_MAPPED, a)) pub = canon(a.text());
			if (const auto *v = r.get(stun::A_LIFETIME); v && v->size() == 4)
				lifetime_s = std::max(60.0, double(uint32_t((*v)[0]) << 24 | uint32_t((*v)[1]) << 16 | uint32_t((*v)[2]) << 8 | (*v)[3]));
			turn = relay.empty() ? FAILED : READY;
			refresh_at = now_ms() + lifetime_s * 500;
			perm_dirty = have_peer;
		} else if (r.type == stun::ALLOCATE_ERR) { turn = FAILED; note("the relay refused this side: " + std::to_string(code)); }
		else if (r.type == stun::REFRESH_OK) refresh_at = now_ms() + lifetime_s * 500;
		else if (r.type == stun::REFRESH_ERR) {   // (the allocation gone: a sleep, the relay restarted; nothing more through it)
			note("the relay refused to keep it: " + std::to_string(code));
			relay.clear();
			turn = FAILED;
		}
		else if (r.type == stun::PERMISSION_OK) { perm_at = now_ms(); note("the relay let through ", perm_for); }
		else if (r.type == stun::PERMISSION_ERR) note("the relay refused ", perm_for, ": " + std::to_string(code));
	}

	// ---- the ways to the other player ----
	std::vector<Way> ways() const
	{
		std::vector<Way> w;
		if (!relay_only)
			for (const std::string &a : peer) w.push_back({ a, false });
		if (turn == READY)
			for (const std::string &a : peer)
				if (!private_address(a)) w.push_back({ a, true });
		return w;
	}
	void send_way(const Way &w, const uint8_t *p, size_t n)
	{
		if (!w.relay) { send_counted(w.to, p, n); return; }
		counts["r " + w.to].sent++;
		stun::Message q;   // (a send indication: through this side's relay)
		q.type = stun::SEND;
		stun::random_tid(q.tid);
		stun::Address x;
		if (!stun_of(w.to, x)) return;
		q.add_address(stun::A_PEER, x);
		q.add(stun::A_DATA, p, n);
		send_to(turn_server, q.encode());
	}
	bool peer_ip(const std::string &from) const
	{
		const std::string ip = ip_of(from);
		for (const std::string &a : peer)
			if (ip_of(a) == ip) return true;
		return false;
	}

	void tick()
	{
		const double t = now_ms();
		if (!stun_done && t >= stun_next) {
			if (stun_tries++ < 12) { binding(); stun_next = t + 250; }
			else stun_done = true;
		}
		if (turn == ASKING && t >= turn_next) {
			if (turn_tries == 0) turn_first = t;
			if (turn_tries++ < 8) {
				if (alloc_bytes.empty()) allocate();
				else { pending[alloc_tid] = stun::ALLOCATE; send_to(turn_server, alloc_bytes); turn_next = t + 500; }   // (the same request again, still asked)
			}
			else { turn = FAILED; note("the relay didn't answer"); }
		}
		if (turn == READY) {
			if (t >= refresh_at) { refresh(uint32_t(lifetime_s)); refresh_at = t + 20000; }   // (asked again in 20 s if no answer)
			if (have_peer && (perm_dirty || t - perm_at > 240000)) { permit(); perm_dirty = false; perm_at = t; }
		}
		if (t - started > 4000) {   // (gathering's time is up: what's found is what there is)
			stun_done = true;
			if (turn == ASKING && relay.empty() && turn_tries > 0 && t - turn_first > 6000) { turn = FAILED; note("the relay didn't answer in time"); }
		}
		if (host && have_peer && !is_locked && t < punch_until && t >= punch_next) {
			static const uint8_t knock[8] = { 'V', 'F', 'N', 'Q', 0, 0, 0, 0 };
			for (const Way &w : ways()) send_way(w, knock, sizeof knock);
			punch_next = t + 150;
		}
	}
};

RoomLink::RoomLink() : m(std::make_unique<Impl>()) {}
RoomLink::~RoomLink() = default;

bool RoomLink::open(uint16_t port, std::string &error)
{
#ifdef _WIN32
	if (!m->wsa) {
		WSADATA d;
		if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { error = "Couldn't start the network (Winsock)."; return false; }
		m->wsa = true;
	}
#endif
	m->s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	m->v6 = m->s != NO_SOCKET;
	if (!m->v6) m->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m->s == NO_SOCKET) { error = "Couldn't open a UDP socket: " + last_error(); return false; }
	if (m->v6) {
		int off = 0;
		setsockopt(m->s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off), sizeof off);
	}
	int buf = 256 * 1024;
	setsockopt(m->s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&buf), sizeof buf);
	setsockopt(m->s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buf), sizeof buf);
#ifdef _WIN32
	u_long nb = 1;
	ioctlsocket(m->s, FIONBIO, &nb);
#else
	fcntl(m->s, F_SETFL, fcntl(m->s, F_GETFL, 0) | O_NONBLOCK);
#endif
	sockaddr_storage ss{};
	socklen_t len;
	if (m->v6) {
		sockaddr_in6 &a = reinterpret_cast<sockaddr_in6 &>(ss);
		a.sin6_family = AF_INET6;
		a.sin6_addr = in6addr_any;
		a.sin6_port = htons(port);
		len = sizeof a;
	} else {
		sockaddr_in &a = reinterpret_cast<sockaddr_in &>(ss);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons(port);
		len = sizeof a;
	}
	if (::bind(m->s, reinterpret_cast<const sockaddr *>(&ss), len) != 0) {
		if (port == 0) { error = "Couldn't open a port: " + last_error(); return false; }
		return open(0, error);   // (that port's taken: any other does, a room's peers are told which)
	}
	return true;
}

std::string RoomLink::resolve(const std::string &host, int port)
{
#ifdef _WIN32
	WSADATA d;
	if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return "";
#endif
	std::string out;
	for (int family : { AF_INET, AF_INET6 }) {   // (IPv4 first: every network has it)
		addrinfo hints{}, *res = nullptr;
		hints.ai_family = family;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
			sockaddr_storage ss{};
			memcpy(&ss, res->ai_addr, std::min<size_t>(res->ai_addrlen, sizeof ss));
			out = text_of(ss);
			freeaddrinfo(res);
			break;
		}
	}
#ifdef _WIN32
	WSACleanup();
#endif
	return out;
}

void RoomLink::use_stun(const std::string &server)
{
	if (server.empty()) return;
	m->stun_server = server;
	m->stun_done = false;
	m->started = now_ms();   // (gathering's time from now: not from when the link was made, the lobby asked meanwhile)
	m->stun_tries = 0;
	m->stun_next = 0;
}

void RoomLink::use_turn(const std::string &server, const std::string &user, const std::string &password)
{
	if (server.empty()) return;
	m->turn_server = server;
	m->user = user;
	m->password = password;
	m->turn = Impl::ASKING;
	m->started = now_ms();
	m->turn_tries = 0;
	m->turn_next = 0;
	m->alloc_tid.clear();
	m->alloc_bytes.clear();
	m->key.clear();
}

bool RoomLink::gathered() const { return m->stun_done && m->turn != Impl::ASKING; }

void RoomLink::set_relay_only(bool on) { m->relay_only = on; }
void RoomLink::allow_loopback(bool on) { m->loopback_ok = on; }

std::vector<std::string> RoomLink::candidates() const
{
	std::vector<std::string> out;
	if (m->relay_only) return out;   // (none given out: the relay's is the only way to this side)
	const std::string lan = lan_address();
	if (!lan.empty()) out.push_back(lan + ":" + std::to_string(local_port()));
	if (!m->pub.empty() && std::find(out.begin(), out.end(), m->pub) == out.end()) out.push_back(m->pub);
	return out;
}

std::string RoomLink::relay_address() const { return m->turn == Impl::READY ? m->relay : ""; }
std::string RoomLink::public_address() const { return m->pub; }

void RoomLink::set_peer(const std::vector<std::string> &cands, const std::string &relay, bool host)
{
	m->peer.clear();
	for (const std::string &given : cands) {
		sockaddr_storage ss;
		socklen_t len;
		const std::string a = canon(given);
		if (m->peer.size() < 8 && !a.empty() && player_address(a, m->loopback_ok) && sockaddr_of(a, m->v6, ss, len) && std::find(m->peer.begin(), m->peer.end(), a) == m->peer.end())
			m->peer.push_back(a);
	}
	sockaddr_storage ss;
	socklen_t len;
	m->peer_relay.clear();
	const std::string relay_c = canon(relay);
	if (!relay_c.empty() && player_address(relay_c, m->loopback_ok) && sockaddr_of(relay_c, m->v6, ss, len) &&
	    std::find(m->peer.begin(), m->peer.end(), relay_c) == m->peer.end()) { m->peer.push_back(relay_c); m->peer_relay = relay_c; }
	m->host = host;
	m->have_peer = !m->peer.empty();
	m->is_locked = false;
	m->have_last = false;
	m->perm_dirty = true;
	m->punch_until = now_ms() + 30000;
	m->punch_next = 0;
	tick();
}

void RoomLink::clear_peer()
{
	m->peer.clear();
	m->have_peer = m->is_locked = m->have_last = false;
}

void RoomLink::tick() { m->tick(); }

void RoomLink::send(const uint8_t *p, size_t n)
{
	if (m->is_locked) m->send_way(m->way, p, n);
	else if (m->host) { if (m->have_last) m->send_way(m->last, p, n); }
	else
		for (const Impl::Way &w : m->ways()) m->send_way(w, p, n);   // (a guest's hello: every way at once)
}

bool RoomLink::recv(std::vector<uint8_t> &out)
{
	if (m->s == NO_SOCKET) return false;
	m->tick();
	for (int skipped = 0; skipped < 256; skipped++) {
		uint8_t buf[2048];
		sockaddr_storage from{};
		socklen_t from_len = sizeof from;
		const auto n = recvfrom(m->s, reinterpret_cast<char *>(buf), sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
		if (n < 0) {
#ifdef _WIN32
			const int e = WSAGetLastError();
			if (e == WSAEMSGSIZE || e == WSAECONNRESET) continue;
#else
			if (errno == ECONNREFUSED || errno == EINTR) continue;
#endif
			return false;
		}
		const std::string who = text_of(from);
		const uint8_t *data = buf;
		size_t len = size_t(n);
		Impl::Way came{ who, false };
		if (stun::Message::is_stun(buf, len)) {
			stun::Message r;
			if (!stun::Message::decode(buf, len, r)) continue;
			if (who == m->stun_server && r.type == stun::BINDING_OK && m->pending.erase(tid_key(r.tid))) {
				stun::Address a;
				if (r.get_address(stun::A_XOR_MAPPED, a) || r.get_address(stun::A_MAPPED, a)) m->pub = canon(a.text());
				m->stun_done = true;
				continue;
			}
			if (who != m->turn_server) continue;
			if (r.type != stun::DATA_IND) { m->turn_reply(r); continue; }
			// a datagram through the relay: from whom, and what
			stun::Address peer;
			const std::vector<uint8_t> *d = r.get(stun::A_DATA);
			if (!d || !r.get_address(stun::A_PEER, peer) || d->size() > sizeof buf) continue;
			memcpy(buf, d->data(), d->size());
			len = d->size();
			came = { canon(peer.text()), true };
		}
		if (len >= 4 && memcmp(data, "VFN", 3) == 0 && m->peer_ip(came.to)) {
			Impl::Count &c = m->counts[(came.relay ? "r " : "d ") + came.to];
			if (data[3] == 'Q') c.knocks++;
			else c.got++;
		} else {
			m->strangers++;
			if (m->stranger_from.size() < 3 && std::find(m->stranger_from.begin(), m->stranger_from.end(), came.to) == m->stranger_from.end())
				m->stranger_from.push_back((came.relay ? "relayed " : "") + came.to);
		}
		if (m->relay_only && !came.relay) continue;   // (only the relay's way: anything straight to this socket, dropped)
		if (len < 4 || memcmp(data, "VFN", 3) != 0) continue;
		if (data[3] == 'Q') continue;   // (a knock: it made the hole, nothing more)
		if (data[3] != 'P') continue;
		if (m->is_locked) {
			if (!(came == m->way)) continue;   // (another way's, or a stranger's)
		} else if (m->host) {
			m->last = came;   // (the session locks onto it if its hello is good)
			m->have_last = true;
		} else {
			// (the session locks onto it once it's the host's welcome, with this game's token: an address alone
			// isn't enough, a relay's being shared by everyone on it)
			if (!m->peer_ip(came.to)) continue;
			m->last = came;
			m->have_last = true;
		}
		out.assign(data, data + len);
		return true;
	}
	return false;
}

void RoomLink::lock()
{
	if (m->is_locked || !m->have_last) return;
	m->way = m->last;
	m->is_locked = true;
}

bool RoomLink::locked() const { return m->is_locked; }
bool RoomLink::via_relay() const { return m->is_locked && (m->way.relay || (!m->peer_relay.empty() && m->way.to == m->peer_relay)); }

std::string RoomLink::report(const std::function<std::string(const std::string &)> &mask) const
{
	std::string out = "relay " + std::string(m->turn == Impl::READY ? "ready" : m->turn == Impl::FAILED ? "failed" : m->turn == Impl::ASKING ? "still asking" : "none");
	for (const Impl::Said &w : m->said) out += "; " + w.text + (w.addr.empty() ? "" : mask(w.addr)) + w.after;
	for (const auto &[k, c] : m->counts) {
		out += "; " + std::string(k[0] == 'r' ? "through this side's relay to " : "to ") + mask(k.substr(2)) + ": sent " + std::to_string(c.sent);
		if (c.failed) out += " (" + std::to_string(c.failed) + " failed, error " + std::to_string(c.error) + ")";
		out += ", got " + std::to_string(c.got) + " and " + std::to_string(c.knocks) + " knocks";
	}
	// (what came in on ways this side doesn't send by: the other side's relay to this side's own address)
	if (m->strangers) {
		out += "; " + std::to_string(m->strangers) + " from no one expected (";
		for (size_t i = 0; i < m->stranger_from.size(); i++) out += (i ? ", " : "") + mask(m->stranger_from[i]);
		out += ")";
	}
	return out;
}

bool RoomLink::local_blocked() const
{
#ifdef __APPLE__   // (macOS's Local Network permission, off: sends to the network's addresses refused, "no route to host")
	for (const auto &[k, c] : m->counts)
		if (k[0] == 'd' && private_address(k.substr(2)) && c.failed && c.error == EHOSTUNREACH) return true;
#endif
	return false;
}

std::string RoomLink::way() const
{
	if (!m->is_locked) return "no way yet";
	return std::string(m->way.relay ? "through this side's relay to " : "to ") + m->way.to;
}

uint16_t RoomLink::local_port() const
{
	sockaddr_storage a{};
	socklen_t n = sizeof a;
	if (m->s == NO_SOCKET || getsockname(m->s, reinterpret_cast<sockaddr *>(&a), &n) != 0) return 0;
	return ntohs(a.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6 *>(&a)->sin6_port : reinterpret_cast<sockaddr_in *>(&a)->sin_port);
}

} // namespace vfnet
