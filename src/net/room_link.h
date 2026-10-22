// The link for a match made with a room code: one UDP socket that finds its own public address (STUN), gets a relay
// address (TURN, Cloudflare's) for when a direct connection can't be made, and, given the other player's addresses
// (from the lobby), tries every way to them at once: their address on their own network, their public one, their
// relay; directly and through this side's relay. The first that answers is the way from then on. Holes in routers
// are made by both sides sending first (a host sends small "VFNQ" datagrams, a guest its hellos), as hole punching
// does. Addresses are "ip:port" text throughout ("[v6]:port" for IPv6), numbers only: names are resolved before.
#pragma once
#include "link.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vfnet {

class RoomLink : public Link
{
public:
	RoomLink();
	~RoomLink() override;
	bool open(uint16_t port, std::string &error);   // 0: any free port

	// A name's address, numbers only ("" if none): blocking, a thread's to call
	static std::string resolve(const std::string &host, int port);

	// Finding this side's addresses: the STUN server's address, and the TURN relay's with its login (either may be
	// left out). Then call tick() until gathered().
	void use_stun(const std::string &server);
	void use_turn(const std::string &server, const std::string &user, const std::string &password);
	bool gathered() const;                          // done (or given up): the addresses below are what there are
	std::vector<std::string> candidates() const;    // this side's: on its network, and public (STUN's)
	std::string relay_address() const;              // the relay's ("" if none)
	std::string public_address() const;             // as STUN saw it ("" if not)

	// The other player's addresses (theirs on their network, public, relay); a host after the lobby says who joined,
	// a guest before it says hello. Starts the hole punching (a host's VFNQs).
	void set_peer(const std::vector<std::string> &cands, const std::string &relay, bool host);
	// Through this side's relay only: its own addresses never given out nor sent from, so the other player sees
	// only the relay's ("hide my IP"). Before gathering.
	void set_relay_only(bool on);
	void clear_peer();                              // (a host waiting again: the last player's gone)

	// Housekeeping, every frame or so (retries, the relay kept alive, hole punching); recv does it too
	void tick();

	void send(const uint8_t *p, size_t n) override;
	bool recv(std::vector<uint8_t> &out) override;
	void lock() override;
	bool locked() const;
	bool via_relay() const;          // the way found goes through a relay (this side's or the other's)
	std::string way() const;         // for the log: "direct to 1.2.3.4:5678", "through this side's relay to ..."
	// For the log when it doesn't connect: what was sent and got each way, and what the relay said (the addresses
	// through mask)
	std::string report(const std::function<std::string(const std::string &)> &mask) const;
	// Sends to the other player's address on this network refused by the system: a Mac's Local Network permission off
	bool local_blocked() const;
	uint16_t local_port() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};

} // namespace vfnet
