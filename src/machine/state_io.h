// Save states: the board's state written to and read back from a flat buffer, for netplay's rollback (save every
// frame, restore a few frames back and run them again), replays and desync checks.
//
// One function lists the fields (Model3::state and the devices' own state()) and is run with a StateWriter to save
// or a StateReader to restore, so the two can't drift apart. The buffer is for this build only: plain structs are
// copied as they are in memory, so a state from another build, or another platform, isn't meant to load (the
// version in the header catches a changed layout only when it's bumped). Sections name the parts, so two states
// that differ can say where (desync reports).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace m3 {

struct StateSection { const char *name; size_t offset; };

class StateWriter
{
public:
	// (append: after what the buffer holds already)
	explicit StateWriter(std::vector<uint8_t> &out, std::vector<StateSection> *sections = nullptr, bool append = false) : out(out), sections(sections) { if (!append) out.clear(); }
	static constexpr bool loading = false;
	void section(const char *name) { if (sections) sections->push_back({ name, out.size() }); }
	void bytes(const void *p, size_t n)
	{
		const size_t at = out.size();
		out.resize(at + n);   // (the caller's buffer keeps its capacity from the last save: no allocation)
		if (n) memcpy(out.data() + at, p, n);
	}
	// (only types without padding: padding bytes would make two equal states hash differently)
	template <class T> void pod(T &v)
	{
		static_assert(std::has_unique_object_representations_v<T>);
		bytes(&v, sizeof(T));
	}
	template <class T> void vec(std::vector<T> &v)
	{
		static_assert(std::has_unique_object_representations_v<T>);
		uint64_t n = v.size();
		pod(n);
		bytes(v.data(), n * sizeof(T));
	}
	void deq(std::deque<uint8_t> &d)
	{
		uint64_t n = d.size();
		pod(n);
		for (uint8_t b : d) out.push_back(b);
	}

private:
	std::vector<uint8_t> &out;
	std::vector<StateSection> *sections;
};

class StateReader
{
public:
	StateReader(const uint8_t *p, size_t n) : p(p), end(p + n) {}
	static constexpr bool loading = true;
	bool ok = true;
	void section(const char *) {}
	void bytes(void *q, size_t n)
	{
		if (!ok || size_t(end - p) < n) { ok = false; return; }
		if (n) memcpy(q, p, n);
		p += n;
	}
	template <class T> void pod(T &v)
	{
		static_assert(std::has_unique_object_representations_v<T>);
		bytes(&v, sizeof(T));
	}
	// (a vector of the same size as now, as the board's memories always are, is read in place: no allocation)
	template <class T> void vec(std::vector<T> &v)
	{
		static_assert(std::has_unique_object_representations_v<T>);
		uint64_t n = 0;
		pod(n);
		if (!ok || n > size_t(end - p) / sizeof(T)) { ok = false; return; }
		v.resize(size_t(n));
		bytes(v.data(), size_t(n) * sizeof(T));
	}
	void deq(std::deque<uint8_t> &d)
	{
		uint64_t n = 0;
		pod(n);
		if (!ok || n > size_t(end - p)) { ok = false; return; }
		d.assign(p, p + n);
		p += n;
	}
	// The next n bytes where they are, or null (a part the caller copies in its own way)
	const uint8_t *take(size_t n)
	{
		if (!ok || size_t(end - p) < n) { ok = false; return nullptr; }
		const uint8_t *q = p;
		p += n;
		return q;
	}
	bool done() const { return ok && p == end; }

private:
	const uint8_t *p, *end;
};

// A fast 64-bit hash of a state (desync checks: the same on every platform for the same bytes). Four lanes, each
// a word in four, so the multiplies don't wait on each other.
inline uint64_t state_hash(const uint8_t *p, size_t n)
{
	constexpr uint64_t K = 0xFF51AFD7ED558CCDull;
	uint64_t h[4] = { 0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
	size_t i = 0;
	for (; i + 32 <= n; i += 32)
		for (int l = 0; l < 4; l++) {
			uint64_t w;
			memcpy(&w, p + i + 8 * l, 8);
			h[l] = (h[l] ^ w) * K;
			h[l] ^= h[l] >> 29;
		}
	uint64_t x = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
	for (; i < n; i++)
		x = (x ^ p[i]) * 0x100000001B3ull;
	x ^= x >> 33;
	x *= 0xC4CEB9FE1A85EC53ull;
	return x ^ (x >> 33);
}

} // namespace m3
