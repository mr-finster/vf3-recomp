// This machine's IPv4 addresses, as the Host page (play by address) shows them (issue #46): every network it's on, not
// only the one on the way to the internet (src/net's lan_address()), which on a Wi-Fi Direct hotspot or another network
// without internet is the wrong one while the PC is also online another way. Mac and Linux: getifaddrs; Windows:
// GetAdaptersAddresses (iphlpapi).
#pragma once
#include <string>
#include <vector>

namespace lan_list {

struct Address
{
	std::string ip;        // "192.168.1.20"
	std::string adapter;   // the network adapter's name as the system gives it ("Wi-Fi", "en0", "wlan0"; "" if none)
	bool virt = false;     // a virtual machine's or container's adapter (vEthernet, VirtualBox, docker0...): listed last
};

// The adapters that are up (and running), their IPv4 addresses: not loopback's (127.x) or a tunnel's, and link-local
// (169.254.x.x) only when it's all there is. preferred (the address the system reaches the internet from) first when
// it's among them, then the real adapters', then the virtual ones', each in the system's order; no address twice. Tests: VF3_TEST_LAN="ip=adapter,ip=adapter,..." in place of the system's
// ("none": no address).
std::vector<Address> addresses(const std::string &preferred);

}
