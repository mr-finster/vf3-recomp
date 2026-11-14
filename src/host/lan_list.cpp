#include "lan_list.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>   // (GetAdaptersAddresses: -liphlpapi, tools/win/build.sh)
#include <windows.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace lan_list {

namespace {

// (an address's four bytes, in the network's order, as text: no inet_ntop, which Windows wants Winsock started for)
std::string dotted(const uint8_t b[4])
{
	char s[16];
	snprintf(s, sizeof s, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	return s;
}

struct Found
{
	std::vector<Address> v;
	std::vector<Address> link_local;   // (169.254.x.x: kept only for when there's nothing else)
	void add(const uint8_t b[4], const std::string &adapter)
	{
		if (b[0] == 127 || (b[0] | b[1] | b[2] | b[3]) == 0) return;   // (loopback; an adapter without one yet)
		std::vector<Address> &to = b[0] == 169 && b[1] == 254 ? link_local : v;
		const std::string ip = dotted(b);
		for (const Address &a : to)
			if (a.ip == ip) return;
		to.push_back({ ip, adapter });
	}
};

void system_addresses(Found &f)
{
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<uint8_t> buf;
	ULONG r = ERROR_BUFFER_OVERFLOW;
	for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
		buf.resize(size);
		r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
		                         reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
	}
	if (r != NO_ERROR) return;
	for (const IP_ADAPTER_ADDRESSES *a = reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(buf.data()); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		std::string name;   // (the name the user sees, "Wi-Fi", "Local Area Connection* 10": UTF-16, made UTF-8)
		if (a->FriendlyName && a->FriendlyName[0]) {
			const int n = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
			if (n > 1) {
				name.resize(size_t(n));
				WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, &name[0], n, nullptr, nullptr);
				name.resize(size_t(n - 1));
			}
		}
		for (const IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
			const sockaddr *sa = u->Address.lpSockaddr;
			if (!sa || sa->sa_family != AF_INET) continue;
			f.add(reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in *>(sa)->sin_addr), name);
		}
	}
#else
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0) return;
	for (const ifaddrs *i = list; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
		if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
		f.add(reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in *>(i->ifa_addr)->sin_addr), i->ifa_name ? i->ifa_name : "");
	}
	freeifaddrs(list);
#endif
}

// (VF3_TEST_LAN: "ip=adapter,..." in place of the system's)
bool test_addresses(Found &f)
{
	const char *e = getenv("VF3_TEST_LAN");
	if (!e || !*e) return false;
	std::string s = e;
	if (s == "none") return true;
	size_t at = 0;
	while (at <= s.size()) {
		size_t end = s.find(',', at);
		if (end == std::string::npos) end = s.size();
		const std::string item = s.substr(at, end - at);
		const size_t eq = item.find('=');
		unsigned b[4];
		char tail = 0;
		if (sscanf(item.substr(0, eq).c_str(), "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) == 4 && b[0] < 256 && b[1] < 256 &&
		    b[2] < 256 && b[3] < 256) {
			const uint8_t u[4] = { uint8_t(b[0]), uint8_t(b[1]), uint8_t(b[2]), uint8_t(b[3]) };
			f.add(u, eq == std::string::npos ? "" : item.substr(eq + 1));
		}
		at = end + 1;
	}
	return true;
}

}

std::vector<Address> addresses(const std::string &preferred)
{
	Found f;
	const bool test = test_addresses(f);
	if (!test) system_addresses(f);
	std::vector<Address> out = f.v.empty() ? f.link_local : f.v;
	if (out.empty() && !test && !preferred.empty()) out.push_back({ preferred, "" });   // (the system's list refused: the route's)
	for (size_t i = 1; i < out.size(); i++)
		if (out[i].ip == preferred) {
			const Address p = out[i];
			out.erase(out.begin() + long(i));
			out.insert(out.begin(), p);
			break;
		}
	return out;
}

}
