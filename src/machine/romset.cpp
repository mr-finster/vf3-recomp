#include "romset.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <zlib.h>

namespace romset {

namespace {

// ---- a minimal zip reader: the central directory, stored and deflated entries (as VF2's) ----

struct entry { std::string name; uint32_t crc = 0, csize = 0, usize = 0, local = 0; uint16_t method = 0; };

uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

struct zip
{
	std::vector<uint8_t> data;
	std::vector<entry> entries;

	bool open(const std::string &path)
	{
		// vf3.zip is about 112 MB: anything over 512 MB isn't the set, and isn't read
		std::error_code ec;
		const uintmax_t size = std::filesystem::file_size(path, ec);
		if (ec || size < 22 || size > (512u << 20)) return false;
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) return false;
		const long n = long(size);
		data.resize(size_t(n));
		const bool got = fread(data.data(), 1, data.size(), f) == data.size();
		fclose(f);
		if (!got) return false;
		long eocd = -1;
		for (long i = n - 22; i >= 0 && i >= n - 22 - 65535; i--)
			if (le32(&data[i]) == 0x06054b50) { eocd = i; break; }
		if (eocd < 0) return false;
		const uint16_t count = le16(&data[eocd + 10]);
		size_t at = le32(&data[eocd + 16]);
		for (int k = 0; k < count; k++) {
			if (at + 46 > data.size() || le32(&data[at]) != 0x02014b50) return false;
			entry e;
			e.method = le16(&data[at + 10]);
			e.crc = le32(&data[at + 16]);
			e.csize = le32(&data[at + 20]);
			e.usize = le32(&data[at + 24]);
			const uint16_t nlen = le16(&data[at + 28]), xlen = le16(&data[at + 30]), clen = le16(&data[at + 32]);
			e.local = le32(&data[at + 42]);
			if (at + 46 + nlen > data.size()) return false;
			e.name.assign(reinterpret_cast<const char *>(&data[at + 46]), nlen);
			const size_t slash = e.name.find_last_of('/');
			if (slash != std::string::npos) e.name = e.name.substr(slash + 1);
			entries.push_back(e);
			at += 46 + nlen + xlen + clen;
		}
		return true;
	}

	bool read(const entry &e, std::vector<uint8_t> &out) const
	{
		const size_t local = e.local;
		if (local + 30 > data.size() || le32(&data[local]) != 0x04034b50) return false;
		const size_t start = local + 30 + le16(&data[local + 26]) + le16(&data[local + 28]);
		if (start + size_t(e.csize) > data.size() || e.usize > (64u << 20)) return false;
		out.resize(e.usize);
		if (e.method == 0) {
			if (e.csize != e.usize) return false;
			memcpy(out.data(), &data[start], e.usize);
		} else if (e.method == 8) {
			z_stream z{};
			if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
			z.next_in = const_cast<Bytef *>(&data[start]);
			z.avail_in = e.csize;
			z.next_out = out.data();
			z.avail_out = e.usize;
			const int r = inflate(&z, Z_FINISH);
			inflateEnd(&z);
			if (r != Z_STREAM_END || z.total_out != e.usize) return false;
		} else {
			return false;
		}
		return uint32_t(crc32(0, out.data(), uInt(out.size()))) == e.crc;
	}
};

// ---- the set: each ROM goes into its region in 2-byte chunks every `stride` bytes, byte-swapped or not ----

enum region_id { CROM = m3::ROM_CROM, BANKED = m3::ROM_BANKED, VROM = m3::ROM_VROM, SOUND = m3::ROM_SOUND, SAMPLES = m3::ROM_SAMPLES,
	N_REGIONS = m3::ROM_REGIONS };
const size_t REGION_SIZE[N_REGIONS] = { 0x200000, 0x4000000, 0x2000000, 0x80000, 0x800000 };

using rom = m3::RomFile;

void place(std::vector<uint8_t> &r, const rom &ro, const std::vector<uint8_t> &d)
{
	for (size_t i = 0, at = ro.offset; i + ro.chunk <= d.size() && at + ro.chunk <= r.size(); i += ro.chunk, at += ro.stride)
		for (uint32_t j = 0; j < ro.chunk; j++)
			r[at + j] = d[i + (ro.swap ? ro.chunk - 1 - j : j)];
}

void to_host_words(std::vector<uint8_t> &v)
{
	for (size_t i = 0; i + 3 < v.size(); i += 4) {
		uint32_t w = uint32_t(v[i]) << 24 | uint32_t(v[i + 1]) << 16 | uint32_t(v[i + 2]) << 8 | v[i + 3];
		memcpy(&v[i], &w, 4);
	}
}

} // namespace

result load(const std::string &path, const m3::Profile &profile)
{
	result res;
	zip z;
	if (!z.open(path)) { res.error = "Couldn't read " + path + " as a zip file."; return res; }
	std::vector<uint8_t> region[N_REGIONS];
	for (int r = 0; r < N_REGIONS; r++) region[r].assign(REGION_SIZE[r], 0);
	int bad = 0;
	std::vector<uint8_t> d;
	for (size_t i = 0; i < profile.rom_count; i++) {
		const rom &ro = profile.roms[i];
		const entry *e = nullptr;
		for (const entry &x : z.entries) if (x.crc == ro.crc && x.name == ro.name) { e = &x; break; }
		if (!e) for (const entry &x : z.entries) if (x.crc == ro.crc) { e = &x; break; }
		if (!e || !z.read(*e, d)) {
			if (!bad++) res.error = std::string("This isn't a complete ") + profile.title + " set, \"" + profile.set + "\". Not usable:";
			res.error += std::string(" ") + ro.name;
			continue;
		}
		place(region[ro.region], ro, d);
	}
	if (bad) return res;

	// The fixed CROM: the program ROMs in its top 2 MB, the start of banked CROM below them
	res.crom.assign(0x800000, 0);
	memcpy(&res.crom[0x800000 - REGION_SIZE[CROM]], region[CROM].data(), REGION_SIZE[CROM]);
	memcpy(&res.crom[0], region[BANKED].data(), 0x800000 - REGION_SIZE[CROM]);
	res.banked.resize(0x8000000);
	memcpy(&res.banked[0], region[BANKED].data(), REGION_SIZE[BANKED]);
	memcpy(&res.banked[REGION_SIZE[BANKED]], region[BANKED].data(), REGION_SIZE[BANKED]);
	to_host_words(res.crom);
	to_host_words(res.banked);
	res.vrom.resize(0x4000000);
	memcpy(&res.vrom[0], region[VROM].data(), REGION_SIZE[VROM]);
	memcpy(&res.vrom[REGION_SIZE[VROM]], region[VROM].data(), REGION_SIZE[VROM]);
	res.sound = std::move(region[SOUND]);
	res.samples.resize(0x1000000);
	memcpy(&res.samples[0], region[SAMPLES].data(), REGION_SIZE[SAMPLES]);
	memcpy(&res.samples[REGION_SIZE[SAMPLES]], region[SAMPLES].data(), REGION_SIZE[SAMPLES]);
	res.ok = true;
	return res;
}

} // namespace romset
