// Netplay's links: how a session's packets get to the other side. A UDP socket for the real thing, and a simulated
// wire (latency, jitter, loss) for tests, both behind one interface. Nothing here knows about any game: this folder
// is meant to be shared by VF1, VF2 and VF3 Recomp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace vfnet {

// One datagram at a time, unreliable and unordered, as UDP is
struct Link
{
	virtual void send(const uint8_t *p, size_t n) = 0;
	virtual bool recv(std::vector<uint8_t> &out) = 0;   // the next datagram that's come, false if none
	// A host's, the session's to call: the sender of the datagram recv gave last is the peer from now on (its
	// hello was good). Before that, a host answers whoever sent the last datagram, and takes anyone's.
	virtual void lock() {}
	virtual ~Link() = default;
};

// A UDP socket. A host binds a port and takes as its peer the address the session locks on (a good hello's: not
// just the first sender, who could be anyone's forged packet); a guest is given the host's address. Non-blocking.
std::string lan_address();   // this machine's address on its network, as others there would reach it ("" if none)
class UdpLink : public Link
{
public:
	UdpLink();
	~UdpLink() override;
	bool bind(uint16_t port, std::string &error);                         // a host's: 0 for any free port
	bool connect(const std::string &host, uint16_t port, std::string &error);   // a guest's (binds any free port)
	uint16_t local_port() const;
	std::string peer_address() const;   // the peer's, as text
	bool has_peer() const { return peer_set; }
	void send(const uint8_t *p, size_t n) override;
	bool recv(std::vector<uint8_t> &out) override;
	void lock() override;

private:
	struct Impl;
	std::unique_ptr<Impl> m;
	bool peer_set = false;
};

// Two ends of a simulated wire, on a clock the test moves on (milliseconds). Each datagram is late by latency plus
// up to jitter (so some come out of order), and lost with the loss chance.
class SimWire
{
public:
	struct Params { double latency_ms = 30, jitter_ms = 0, loss = 0; };
	SimWire(Params a_to_b, Params b_to_a, uint32_t seed = 1);
	~SimWire();
	Link &a();
	Link &b();
	void set(int dir, Params p) { params[dir] = p; }   // dir 0: a to b, 1: b to a
	void cut(bool on) { cut_ = on; }                   // nothing gets through (a cable pulled)
	double now = 0;
	uint64_t sent = 0, lost = 0;

private:
	struct Packet { double at; std::vector<uint8_t> data; };
	struct End;
	Params params[2];
	std::deque<Packet> queue[2];
	std::unique_ptr<End> ends[2];
	std::mt19937 rng;
	bool cut_ = false;
	void push(int dir, const uint8_t *p, size_t n);
	bool pop(int dir, std::vector<uint8_t> &out);
};

} // namespace vfnet
