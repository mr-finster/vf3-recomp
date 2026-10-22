// STUN and TURN (RFC 5389, 8656), as much as finding a public address and using a relay need: a binding request
// (what address the internet sees this socket as), and TURN's allocate, refresh, permissions and send / data
// indications, with its long-term credentials (MD5 and HMAC-SHA1, here). Messages only: the socket is the caller's.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vfnet::stun {

constexpr uint32_t MAGIC = 0x2112A442;
enum : uint16_t {
	BINDING = 0x0001, BINDING_OK = 0x0101,
	ALLOCATE = 0x0003, ALLOCATE_OK = 0x0103, ALLOCATE_ERR = 0x0113,
	REFRESH = 0x0004, REFRESH_OK = 0x0104, REFRESH_ERR = 0x0114,
	PERMISSION = 0x0008, PERMISSION_OK = 0x0108, PERMISSION_ERR = 0x0118,
	SEND = 0x0016, DATA_IND = 0x0017,
};
enum : uint16_t {
	A_MAPPED = 0x0001, A_USERNAME = 0x0006, A_INTEGRITY = 0x0008, A_ERROR = 0x0009, A_LIFETIME = 0x000D,
	A_PEER = 0x0012, A_DATA = 0x0013, A_REALM = 0x0014, A_NONCE = 0x0015, A_RELAYED = 0x0016,
	A_TRANSPORT = 0x0019, A_XOR_MAPPED = 0x0020,
};

// An address as STUN carries it: IPv4 (4 bytes) or IPv6 (16), and a port
struct Address
{
	bool v6 = false;
	uint8_t ip[16] = {};
	uint16_t port = 0;
	std::string text() const;   // "1.2.3.4:5678", "[2001:db8::1]:5678"
	bool operator==(const Address &o) const;
};

struct Message
{
	uint16_t type = 0;
	uint8_t tid[12] = {};
	std::vector<std::pair<uint16_t, std::vector<uint8_t>>> attrs;

	void add(uint16_t a, const void *p, size_t n);
	void add_u32(uint16_t a, uint32_t v);
	void add_string(uint16_t a, const std::string &s) { add(a, s.data(), s.size()); }
	void add_address(uint16_t a, const Address &x);   // XOR'd (the XOR- attributes)
	const std::vector<uint8_t> *get(uint16_t a) const;
	bool get_address(uint16_t a, Address &x) const;    // XOR'd ones: A_XOR_MAPPED, A_RELAYED, A_PEER; A_MAPPED plain
	int error_code() const;                            // 0 if none
	std::string get_string(uint16_t a) const;

	// The bytes to send; key: MESSAGE-INTEGRITY with it (empty: none)
	std::vector<uint8_t> encode(const std::vector<uint8_t> &key = {}) const;
	// A datagram that's a STUN message (else false: anything else, a game's)
	static bool decode(const uint8_t *p, size_t n, Message &out);
	static bool is_stun(const uint8_t *p, size_t n);
};

void random_tid(uint8_t tid[12]);

// TURN's long-term credential key: MD5(username ":" realm ":" password)
std::vector<uint8_t> long_term_key(const std::string &user, const std::string &realm, const std::string &pass);

// (the hashes, exposed for tests)
void md5(const uint8_t *p, size_t n, uint8_t out[16]);
void sha1(const uint8_t *p, size_t n, uint8_t out[20]);
void hmac_sha1(const uint8_t *key, size_t kn, const uint8_t *p, size_t n, uint8_t out[20]);

} // namespace vfnet::stun
