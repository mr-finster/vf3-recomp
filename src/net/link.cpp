#include "link.h"
#include <algorithm>
#include <cstring>

#ifdef _WIN32
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

// ---- UDP ----

struct UdpLink::Impl
{
	sock_t s = NO_SOCKET;
	sockaddr_storage peer{};
	socklen_t peer_len = 0;
#ifdef _WIN32
	bool wsa = false;
#endif
	bool v6 = true;   // (a dual-stack IPv6 socket, IPv4 addresses mapped; IPv4 alone where IPv6 is off)
	bool open(std::string &error)
	{
#ifdef _WIN32
		if (!wsa) {
			WSADATA d;
			if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { error = "Couldn't start the network (Winsock)."; return false; }
			wsa = true;
		}
#endif
		s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		v6 = s != NO_SOCKET;
		if (!v6) s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == NO_SOCKET) { error = "Couldn't open a UDP socket: " + last_error(); return false; }
		if (v6) {
			int off = 0;
			setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off), sizeof off);
		}
#ifdef _WIN32
		u_long nb = 1;
		ioctlsocket(s, FIONBIO, &nb);
#else
		fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
		return true;
	}
	~Impl()
	{
		if (s != NO_SOCKET) close_socket(s);
#ifdef _WIN32
		if (wsa) WSACleanup();
#endif
	}
};

UdpLink::UdpLink() : m(std::make_unique<Impl>()) {}
UdpLink::~UdpLink() = default;

bool UdpLink::bind(uint16_t port, std::string &error)
{
	if (!m->open(error)) return false;
	int r;
	if (m->v6) {
		sockaddr_in6 a{};
		a.sin6_family = AF_INET6;
		a.sin6_addr = in6addr_any;
		a.sin6_port = htons(port);
		r = ::bind(m->s, reinterpret_cast<sockaddr *>(&a), sizeof a);
	} else {
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons(port);
		r = ::bind(m->s, reinterpret_cast<sockaddr *>(&a), sizeof a);
	}
	if (r != 0) {
		error = "Couldn't use port " + std::to_string(port) + " (another program may have it): " + last_error();
		return false;
	}
	return true;
}

bool UdpLink::connect(const std::string &host, uint16_t port, std::string &error)
{
	if (!m->open(error)) return false;
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = m->v6 ? AF_UNSPEC : AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
		error = "Couldn't find " + host + ".";
		return false;
	}
	if (!m->v6 || res->ai_family == AF_INET6) {
		memcpy(&m->peer, res->ai_addr, res->ai_addrlen);
		m->peer_len = socklen_t(res->ai_addrlen);
	} else {   // (an IPv4 address on the IPv6 socket: mapped, ::ffff:a.b.c.d)
		sockaddr_in6 to{};
		const sockaddr_in *v4 = reinterpret_cast<const sockaddr_in *>(res->ai_addr);
		to.sin6_family = AF_INET6;
		to.sin6_port = v4->sin_port;
		uint8_t *b = reinterpret_cast<uint8_t *>(&to.sin6_addr);
		b[10] = b[11] = 0xFF;
		memcpy(b + 12, &v4->sin_addr, 4);
		memcpy(&m->peer, &to, sizeof to);
		m->peer_len = sizeof to;
	}
	freeaddrinfo(res);
	peer_set = true;
	return true;
}

uint16_t UdpLink::local_port() const
{
	sockaddr_storage a{};
	socklen_t n = sizeof a;
	if (m->s == NO_SOCKET || getsockname(m->s, reinterpret_cast<sockaddr *>(&a), &n) != 0) return 0;
	return ntohs(a.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6 *>(&a)->sin6_port : reinterpret_cast<sockaddr_in *>(&a)->sin_port);
}

void UdpLink::send(const uint8_t *p, size_t n)
{
	if (m->s == NO_SOCKET || !peer_set) return;
	sendto(m->s, reinterpret_cast<const char *>(p), int(n), 0, reinterpret_cast<const sockaddr *>(&m->peer), m->peer_len);
}

bool UdpLink::recv(std::vector<uint8_t> &out)
{
	if (m->s == NO_SOCKET) return false;
	for (;;) {
		uint8_t buf[2048];
		sockaddr_storage from{};
		socklen_t from_len = sizeof from;
		const auto n = recvfrom(m->s, reinterpret_cast<char *>(buf), sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
		if (n < 0) return false;
		if (!peer_set) {   // a host's first datagram: its sender is the peer from now on
			m->peer = from;
			m->peer_len = from_len;
			peer_set = true;
		} else if (from_len != m->peer_len || memcmp(&from, &m->peer, size_t(from_len)) != 0)
			continue;   // (anyone else's: dropped)
		out.assign(buf, buf + n);
		return true;
	}
}

// ---- the simulated wire ----

struct SimWire::End : Link
{
	SimWire *w;
	int dir;   // the direction it sends in
	End(SimWire *w, int dir) : w(w), dir(dir) {}
	void send(const uint8_t *p, size_t n) override { w->push(dir, p, n); }
	bool recv(std::vector<uint8_t> &out) override { return w->pop(dir ^ 1, out); }
};

SimWire::SimWire(Params a_to_b, Params b_to_a, uint32_t seed) : rng(seed)
{
	params[0] = a_to_b;
	params[1] = b_to_a;
	ends[0] = std::make_unique<End>(this, 0);
	ends[1] = std::make_unique<End>(this, 1);
}

SimWire::~SimWire() = default;
Link &SimWire::a() { return *ends[0]; }
Link &SimWire::b() { return *ends[1]; }

void SimWire::push(int dir, const uint8_t *p, size_t n)
{
	sent++;
	const Params &q = params[dir];
	std::uniform_real_distribution<double> u(0, 1);
	if (cut_ || u(rng) < q.loss) { lost++; return; }
	Packet pk{ now + q.latency_ms + q.jitter_ms * u(rng), std::vector<uint8_t>(p, p + n) };
	auto &qu = queue[dir];
	qu.insert(std::upper_bound(qu.begin(), qu.end(), pk.at, [](double t, const Packet &x) { return t < x.at; }), std::move(pk));
}

bool SimWire::pop(int dir, std::vector<uint8_t> &out)
{
	auto &qu = queue[dir];
	if (qu.empty() || qu.front().at > now) return false;
	out = std::move(qu.front().data);
	qu.pop_front();
	return true;
}

} // namespace vfnet
