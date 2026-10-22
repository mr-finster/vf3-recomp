#include "stun.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

namespace vfnet::stun {

// ---- the hashes ----

namespace {

uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

} // namespace

void md5(const uint8_t *p, size_t n, uint8_t out[16])
{
	static const uint32_t K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
	static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
		5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
	uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	std::vector<uint8_t> m(p, p + n);
	m.push_back(0x80);
	while (m.size() % 64 != 56) m.push_back(0);
	const uint64_t bits = uint64_t(n) * 8;
	for (int i = 0; i < 8; i++) m.push_back(uint8_t(bits >> (8 * i)));
	for (size_t off = 0; off < m.size(); off += 64) {
		uint32_t w[16];
		for (int i = 0; i < 16; i++) w[i] = uint32_t(m[off + 4 * i]) | uint32_t(m[off + 4 * i + 1]) << 8 | uint32_t(m[off + 4 * i + 2]) << 16 | uint32_t(m[off + 4 * i + 3]) << 24;
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
		for (int i = 0; i < 64; i++) {
			uint32_t f;
			int g;
			if (i < 16) { f = (b & c) | (~b & d); g = i; }
			else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
			else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
			else { f = c ^ (b | ~d); g = (7 * i) % 16; }
			const uint32_t t = d;
			d = c;
			c = b;
			b = b + rol(a + f + K[i] + w[g], R[i]);
			a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	}
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) out[4 * i + j] = uint8_t(h[i] >> (8 * j));
}

void sha1(const uint8_t *p, size_t n, uint8_t out[20])
{
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	std::vector<uint8_t> m(p, p + n);
	m.push_back(0x80);
	while (m.size() % 64 != 56) m.push_back(0);
	const uint64_t bits = uint64_t(n) * 8;
	for (int i = 7; i >= 0; i--) m.push_back(uint8_t(bits >> (8 * i)));
	for (size_t off = 0; off < m.size(); off += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++) w[i] = uint32_t(m[off + 4 * i]) << 24 | uint32_t(m[off + 4 * i + 1]) << 16 | uint32_t(m[off + 4 * i + 2]) << 8 | m[off + 4 * i + 3];
		for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			const uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol(b, 30); b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 4; j++) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
}

void hmac_sha1(const uint8_t *key, size_t kn, const uint8_t *p, size_t n, uint8_t out[20])
{
	uint8_t k[64] = {};
	if (kn > 64) sha1(key, kn, k);
	else memcpy(k, key, kn);
	std::vector<uint8_t> inner(64 + n), outer(64 + 20);
	for (int i = 0; i < 64; i++) { inner[i] = k[i] ^ 0x36; outer[i] = k[i] ^ 0x5c; }
	memcpy(inner.data() + 64, p, n);
	sha1(inner.data(), inner.size(), outer.data() + 64);
	sha1(outer.data(), outer.size(), out);
}

std::vector<uint8_t> long_term_key(const std::string &user, const std::string &realm, const std::string &pass)
{
	const std::string s = user + ":" + realm + ":" + pass;
	std::vector<uint8_t> k(16);
	md5(reinterpret_cast<const uint8_t *>(s.data()), s.size(), k.data());
	return k;
}

// ---- messages ----

std::string Address::text() const
{
	char b[64];
	if (!v6) snprintf(b, sizeof b, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
	else {
		std::string s = "[";
		for (int i = 0; i < 16; i += 2) {
			char h[8];
			snprintf(h, sizeof h, "%x", unsigned(ip[i] << 8 | ip[i + 1]));
			s += h;
			if (i < 14) s += ":";
		}
		snprintf(b, sizeof b, "%s]:%u", s.c_str(), port);
	}
	return b;
}

bool Address::operator==(const Address &o) const { return v6 == o.v6 && port == o.port && memcmp(ip, o.ip, v6 ? 16 : 4) == 0; }

void random_tid(uint8_t tid[12])
{
	static std::mt19937_64 rng(std::random_device{}() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
	for (int i = 0; i < 12; i++) tid[i] = uint8_t(rng());
}

void Message::add(uint16_t a, const void *p, size_t n)
{
	const uint8_t *b = static_cast<const uint8_t *>(p);
	attrs.push_back({ a, std::vector<uint8_t>(b, b + n) });
}

void Message::add_u32(uint16_t a, uint32_t v)
{
	const uint8_t b[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
	add(a, b, 4);
}

void Message::add_address(uint16_t a, const Address &x)
{
	std::vector<uint8_t> v = { 0, uint8_t(x.v6 ? 2 : 1), uint8_t((x.port ^ (MAGIC >> 16)) >> 8), uint8_t(x.port ^ (MAGIC >> 16)) };
	uint8_t pad[16] = { uint8_t(MAGIC >> 24), uint8_t(MAGIC >> 16), uint8_t(MAGIC >> 8), uint8_t(MAGIC) };
	memcpy(pad + 4, tid, 12);
	for (int i = 0; i < (x.v6 ? 16 : 4); i++) v.push_back(x.ip[i] ^ pad[i]);
	attrs.push_back({ a, v });
}

const std::vector<uint8_t> *Message::get(uint16_t a) const
{
	for (auto &[t, v] : attrs)
		if (t == a) return &v;
	return nullptr;
}

bool Message::get_address(uint16_t a, Address &x) const
{
	const std::vector<uint8_t> *v = get(a);
	if (!v || v->size() < 8) return false;
	x.v6 = (*v)[1] == 2;
	if (x.v6 && v->size() < 20) return false;
	const bool xored = a != A_MAPPED;
	uint8_t pad[16] = { uint8_t(MAGIC >> 24), uint8_t(MAGIC >> 16), uint8_t(MAGIC >> 8), uint8_t(MAGIC) };
	memcpy(pad + 4, tid, 12);
	x.port = uint16_t((*v)[2] << 8 | (*v)[3]);
	if (xored) x.port ^= uint16_t(MAGIC >> 16);
	for (int i = 0; i < (x.v6 ? 16 : 4); i++) x.ip[i] = (*v)[4 + i] ^ (xored ? pad[i] : 0);
	return true;
}

int Message::error_code() const
{
	const std::vector<uint8_t> *v = get(A_ERROR);
	if (!v || v->size() < 4) return 0;
	return ((*v)[2] & 7) * 100 + (*v)[3];
}

std::string Message::get_string(uint16_t a) const
{
	const std::vector<uint8_t> *v = get(a);
	return v ? std::string(v->begin(), v->end()) : std::string();
}

std::vector<uint8_t> Message::encode(const std::vector<uint8_t> &key) const
{
	std::vector<uint8_t> b = { uint8_t(type >> 8), uint8_t(type), 0, 0,
	                           uint8_t(MAGIC >> 24), uint8_t(MAGIC >> 16), uint8_t(MAGIC >> 8), uint8_t(MAGIC) };
	b.insert(b.end(), tid, tid + 12);
	for (auto &[t, v] : attrs) {
		b.push_back(uint8_t(t >> 8)); b.push_back(uint8_t(t));
		b.push_back(uint8_t(v.size() >> 8)); b.push_back(uint8_t(v.size()));
		b.insert(b.end(), v.begin(), v.end());
		while (b.size() % 4) b.push_back(0);
	}
	if (!key.empty()) {
		// (the length as if MESSAGE-INTEGRITY, 24 bytes, were there already: its HMAC covers the header with it)
		const size_t len = b.size() - 20 + 24;
		b[2] = uint8_t(len >> 8); b[3] = uint8_t(len);
		uint8_t mac[20];
		hmac_sha1(key.data(), key.size(), b.data(), b.size(), mac);
		b.push_back(uint8_t(A_INTEGRITY >> 8)); b.push_back(uint8_t(A_INTEGRITY));
		b.push_back(0); b.push_back(20);
		b.insert(b.end(), mac, mac + 20);
	}
	const size_t len = b.size() - 20;
	b[2] = uint8_t(len >> 8); b[3] = uint8_t(len);
	return b;
}

bool Message::is_stun(const uint8_t *p, size_t n)
{
	if (n < 20 || (p[0] & 0xC0) != 0) return false;
	const uint32_t magic = uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7];
	const size_t len = size_t(p[2]) << 8 | p[3];
	return magic == MAGIC && len % 4 == 0 && 20 + len == n;
}

bool Message::decode(const uint8_t *p, size_t n, Message &out)
{
	if (!is_stun(p, n)) return false;
	out = Message();
	out.type = uint16_t(p[0] << 8 | p[1]);
	memcpy(out.tid, p + 8, 12);
	size_t i = 20;
	while (i + 4 <= n) {
		const uint16_t t = uint16_t(p[i] << 8 | p[i + 1]), len = uint16_t(p[i + 2] << 8 | p[i + 3]);
		if (i + 4 + len > n) return false;
		out.attrs.push_back({ t, std::vector<uint8_t>(p + i + 4, p + i + 4 + len) });
		i += 4 + ((len + 3u) & ~3u);
	}
	return true;
}

} // namespace vfnet::stun
