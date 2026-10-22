// roomtest: two sessions over RoomLinks (src/net/room_link.h) on this machine, through a real STUN / TURN server
// (coturn: turnserver -n --listening-ip=127.0.0.1 --lt-cred-mech --user=test:secret --realm=vf --allow-loopback-peers
// --relay-ip=127.0.0.1 --no-tls --no-dtls --no-cli --denied-peer-ip=192.0.2.0-192.0.2.255: the made-up "on its network"
// addresses refused, as Cloudflare's relay refuses a private network's), as a room code's match makes them: each side finds its
// addresses (STUN) and a relay (TURN), is given the other's, and the first way that answers is the match's.
//   roomtest [--server 127.0.0.1:3478] [--user test --pass secret]
// The ways tried: direct (both sides' real addresses); both relays (the real addresses unreachable: only the relays
// get through); the host's relay only (the guest has none); and a guest with the wrong lobby token (never let in).
#include "../src/net/room_link.h"
#include "../src/net/session.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// A game standing in: a number every input changes
struct Count : vfnet::Game
{
	uint64_t s = 0;
	bool begin(const std::vector<uint8_t> &blob) override { s = blob.size(); return true; }
	void save(std::vector<uint8_t> &st) override { st.resize(8); memcpy(st.data(), &s, 8); }
	bool load(const std::vector<uint8_t> &st) override { if (st.size() != 8) return false; memcpy(&s, st.data(), 8); return true; }
	void advance(const uint16_t in[2], bool) override { s = (s ^ in[0] ^ uint64_t(in[1]) << 16) * 0x9E3779B97F4A7C15ull + 1; }
};

struct Side
{
	vfnet::RoomLink link;
	bool open(const std::string &server, bool turn, const std::string &user, const std::string &pass)
	{
		std::string err;
		if (!link.open(0, err)) { printf("  open: %s\n", err.c_str()); return false; }
		link.use_stun(server);
		if (turn) link.use_turn(server, user, pass);
		return true;
	}
};

// One case: true if it went as it should
bool run(const char *what, const std::string &server, const std::string &user, const std::string &pass, bool a_turn, bool b_turn,
         bool real_addresses, bool wrong_token, bool expect_relay)
{
	Side a, b;
	if (!a.open(server, a_turn, user, pass) || !b.open(server, b_turn, user, pass)) return false;
	const double t0 = now_ms();
	while (!(a.link.gathered() && b.link.gathered()) && now_ms() - t0 < 8000) {
		std::vector<uint8_t> p;
		a.link.recv(p);
		b.link.recv(p);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	auto join = [](const std::vector<std::string> &v) { std::string s; for (auto &x : v) s += x + " "; return s; };
	printf("  %s: host %s(public %s, relay %s); guest %s(public %s, relay %s)\n", what, join(a.link.candidates()).c_str(), a.link.public_address().c_str(),
	       a.link.relay_address().c_str(), join(b.link.candidates()).c_str(), b.link.public_address().c_str(), b.link.relay_address().c_str());
	if ((a_turn && a.link.relay_address().empty()) || (b_turn && b.link.relay_address().empty()) || a.link.public_address().empty()) {
		printf("  FAIL %s: no relay or public address\n", what);
		return false;
	}
	const std::vector<std::string> nowhere = { "10.255.255.1:9" };   // (a private network's: nothing there, and never through a relay)
	// (the host always has the guest's real addresses, as the lobby gives them: its relay lets in only those; the
	// guest, in the relay cases, only the host's relay)
	a.link.set_peer(real_addresses || !b_turn ? b.link.candidates() : nowhere, b.link.relay_address(), true);
	b.link.set_peer(real_addresses ? a.link.candidates() : nowhere, a.link.relay_address(), false);
	Count ga, gb;
	vfnet::Config ca, cb;
	ca.token = 0x1234567890ABCDEFull;
	cb.token = wrong_token ? 0x1111111111111111ull : ca.token;
	ca.handshake_ms = cb.handshake_ms = wrong_token ? 3000 : 15000;
	vfnet::Session sa(a.link, ga, { "test", "v1", 1, "HOST" }, ca, true, std::vector<uint8_t>(3000, 7));
	vfnet::Session sb(b.link, gb, { "test", "v1", 1, "GUEST" }, cb, false);
	double next = now_ms();
	const double t1 = now_ms();
	while ((sa.frame() < 300 || sb.frame() < 300) && sa.state() != vfnet::Session::State::ended && sb.state() != vfnet::Session::State::ended &&
	       now_ms() - t1 < 20000) {
		sa.poll(now_ms());
		sb.poll(now_ms());
		if (now_ms() >= next) { sa.step(uint16_t(sa.frame() / 7)); sb.step(uint16_t(sb.frame() / 5)); next += 17.4; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (wrong_token) {
		const bool ok = sa.state() != vfnet::Session::State::running && sb.state() != vfnet::Session::State::running;
		printf("  %-4s %s: the wrong token never got in\n", ok ? "ok" : "FAIL", what);
		return ok;
	}
	const bool ok = sa.frame() >= 300 && sb.frame() >= 300 && a.link.via_relay() == expect_relay && b.link.via_relay() == expect_relay;
	printf("  %-4s %s: frames %u/%u in %.1f s; host %s, guest %s; ping %.0f ms\n", ok ? "ok" : "FAIL", what, sa.frame(), sb.frame(), (now_ms() - t1) / 1000,
	       a.link.way().c_str(), b.link.way().c_str(), sa.stats().rtt_ms);
	return ok;
}

} // namespace

int main(int argc, char **argv)
{
	std::string server = "127.0.0.1:3478", user = "test", pass = "secret";
	int only = 0;
	for (int i = 1; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "--server")) server = argv[++i];
		else if (!strcmp(argv[i], "--user")) user = argv[++i];
		else if (!strcmp(argv[i], "--pass")) pass = argv[++i];
		else if (!strcmp(argv[i], "--only")) only = atoi(argv[++i]);
	}
	int failed = 0;
	if (!only || only == 1) failed += !run("direct", server, user, pass, true, true, true, false, false);
	if (!only || only == 2) failed += !run("both relays", server, user, pass, true, true, false, false, true);
	if (!only || only == 3) failed += !run("the host's relay only", server, user, pass, true, false, false, false, true);
	if (!only || only == 4) failed += !run("a wrong token", server, user, pass, true, true, true, true, false);
	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed ? 1 : 0;
}
