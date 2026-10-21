// netfuzz: the netplay session (src/net) against an opponent who sends anything. Pairs of sessions play a small
// deterministic game over a simulated network, while a man in the middle spoils packets in flight (bits flipped,
// cut short, fields set to extremes, bytes spliced from other packets, packets replayed, duplicated, or made up),
// on the way to either side and in every phase (the handshake, the blob, the match). Built with AddressSanitizer
// and UndefinedBehaviorSanitizer (make build/bin/netfuzz), so any read or write out of bounds, any undefined
// behaviour, stops it with where. It also checks what a bad peer can make a session hold stays bounded.
//   netfuzz [--runs N] [--seed S]
#include "../src/net/session.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

struct Toy : vfnet::Game
{
	struct S { uint64_t mix; uint32_t frame; uint32_t pad; } s{};
	bool begin(const std::vector<uint8_t> &blob) override { s = {}; s.mix = vfnet::hash64(blob.data(), blob.size()); return true; }
	void save(std::vector<uint8_t> &st) override { st.resize(sizeof s); memcpy(st.data(), &s, sizeof s); }
	bool load(const std::vector<uint8_t> &st) override { if (st.size() != sizeof s) return false; memcpy(&s, st.data(), sizeof s); return true; }
	void advance(const uint16_t in[2], bool) override { s.mix = (s.mix ^ in[0] ^ (uint64_t(in[1]) << 16)) * 0x9E3779B97F4A7C15ull; s.frame++; }
};

// A link that spoils what passes through it
struct Spoiler : vfnet::Link
{
	vfnet::Link &inner;
	std::mt19937 &rng;
	double chance;
	std::vector<std::vector<uint8_t>> &seen;   // (every packet that's passed, both ways: for splicing and replays)
	Spoiler(vfnet::Link &l, std::mt19937 &r, double c, std::vector<std::vector<uint8_t>> &s) : inner(l), rng(r), chance(c), seen(s) {}
	void send(const uint8_t *p, size_t n) override { inner.send(p, n); }
	bool recv(std::vector<uint8_t> &out) override
	{
		std::uniform_real_distribution<double> u(0, 1);
		// a made-up packet, now and then, whether or not one came
		if (u(rng) < chance * 0.2) { make(out); return true; }
		if (!inner.recv(out)) return false;
		if (seen.size() < 4096) seen.push_back(out);
		if (u(rng) < chance) spoil(out);
		return true;
	}
	void make(std::vector<uint8_t> &out)
	{
		if (!seen.empty() && rng() % 2) { out = seen[rng() % seen.size()]; spoil(out); return; }   // (an old one, replayed, spoilt)
		out.resize(rng() % 1500);
		for (auto &b : out) b = uint8_t(rng());
		if (out.size() >= 5 && rng() % 2) { memcpy(out.data(), "VFNP", 4); out[4] = uint8_t(1 + rng() % 9); }
	}
	void spoil(std::vector<uint8_t> &p)
	{
		const int kinds = 1 + int(rng() % 3);
		for (int k = 0; k < kinds; k++) {
			switch (rng() % 7) {
			case 0:   // bits flipped (past the header mostly, so the token still matches)
				for (int i = 0; i < 1 + int(rng() % 4) && !p.empty(); i++) {
					const size_t at = p.size() > 13 && rng() % 4 ? 13 + rng() % (p.size() - 13) : rng() % p.size();
					p[at] ^= uint8_t(1u << (rng() % 8));
				}
				break;
			case 1: p.resize(p.empty() ? 0 : rng() % p.size()); break;   // cut short
			case 2: {   // a 32-bit field set to an extreme
				static const uint32_t X[] = { 0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x10000, 255, 256 };
				if (p.size() >= 17) {
					const size_t at = 13 + rng() % (p.size() - 16);
					const uint32_t v = X[rng() % (sizeof X / sizeof X[0])];
					memcpy(&p[at], &v, 4);
				}
				break;
			}
			case 3:   // another packet's bytes spliced on
				if (!seen.empty()) {
					const auto &o = seen[rng() % seen.size()];
					const size_t from = o.empty() ? 0 : rng() % o.size();
					p.insert(p.end(), o.begin() + long(from), o.end());
				}
				break;
			case 4: if (p.size() > 4) p[4] = uint8_t(rng() % 12); break;   // another type
			case 5: p.resize(p.size() + rng() % 64, uint8_t(rng())); break;   // longer
			default: if (!p.empty()) p[rng() % p.size()] = uint8_t(rng()); break;
			}
		}
	}
};

} // namespace

int main(int argc, char **argv)
{
	long runs = 2000;
	uint32_t seed = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atol(argv[++i]);
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = uint32_t(atol(argv[++i]));
	}
	std::mt19937 rng(seed);
	uint64_t frames = 0, ended = 0, matches = 0, packets = 0;
	for (long r = 0; r < runs; r++) {
		vfnet::SimWire wire({ double(5 + rng() % 60), double(rng() % 20), (rng() % 10) / 100.0 }, { double(5 + rng() % 60), double(rng() % 20), (rng() % 10) / 100.0 }, rng());
		std::vector<std::vector<uint8_t>> seen;
		// which sides hear a spoiler, and how much (the one spoilt is the one attacked)
		const double ca = rng() % 3 ? (rng() % 100) / 400.0 : 0, cb = rng() % 3 ? (rng() % 100) / 400.0 : 0;
		Spoiler la(wire.a(), rng, ca, seen), lb(wire.b(), rng, cb, seen);
		Toy ga, gb;
		vfnet::Config cfg;
		cfg.delay = int(rng() % 4);
		cfg.handshake_ms = 3000;
		cfg.disconnect_ms = 2000;
		std::vector<uint8_t> blob(rng() % 20000);
		for (auto &b : blob) b = uint8_t(rng());
		vfnet::Session a(la, ga, { "toy", "v1", 42, "HOST" }, cfg, true, blob);
		vfnet::Session b(lb, gb, { "toy", "v1", 42, "GUEST" }, cfg, false);
		const uint32_t length = 100 + rng() % 600;
		double t = 1000, na = 1000, nb = 1000 + rng() % 40;
		for (int i = 0; i < 40000; i++, t += 1) {
			wire.now = t;
			a.poll(t);
			b.poll(t);
			if (t >= na) { a.step(uint16_t(rng())); na += cfg.frame_ms; }
			if (t >= nb) { b.step(uint16_t(rng())); nb += cfg.frame_ms; }
			vfnet::Session::Event e;
			while (a.next_event(e)) {}
			while (b.next_event(e)) {}
			const bool da = a.state() == vfnet::Session::State::ended || a.frame() >= length;
			const bool db = b.state() == vfnet::Session::State::ended || b.frame() >= length;
			if (da && db) break;
		}
		// what a bad peer can make a side hold: inputs no more than the frames that passed (and a little)
		for (const vfnet::Session *s : { &a, &b })
			for (int p = 0; p < 2; p++)
				if (s->inputs(p).size() > size_t(s->frame()) + 64) {
					printf("FAIL run %ld: %zu inputs held at frame %u\n", r, s->inputs(p).size(), s->frame());
					return 1;
				}
		frames += a.frame() + b.frame();
		ended += (a.state() == vfnet::Session::State::ended) + (b.state() == vfnet::Session::State::ended);
		matches += a.frame() > 0 && b.frame() > 0;
		packets += wire.sent;
	}
	printf("%ld runs (seed %u): %llu packets, %llu frames, %llu runs got to a match, %llu sides ended early; nothing out of bounds\n", runs, seed,
	       (unsigned long long)packets, (unsigned long long)frames, (unsigned long long)matches, (unsigned long long)ended);
	return 0;
}
