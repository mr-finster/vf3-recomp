// HTTPS for the lobby (one request at a time, blocking: callers run it on a thread of their own). The system's own:
// libcurl on macOS and Linux (loaded as the game runs, so nothing's linked or shipped for it; a Linux without it
// says so), WinHTTP on Windows. Their certificates and proxy settings are the system's.
#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace vfnet::http {

struct Reply
{
	int status = 0;        // the HTTP status; 0 if no reply came
	std::string body;
	std::string error;     // why there was no reply, in words
};

// method "GET" or "POST" (body sent as JSON); timeout_ms for the whole request; a reply longer than max_reply is cut
// short there (the lobby's are small: a watched match's start and checkpoints are the long ones)
Reply request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms, size_t max_reply = 65536);

// A response read as it comes (a watched match's frames, src/net/watch.h): each piece to on_data, whose false ends it; the
// whole thing given up when nothing at all has come for idle_ms (the lobby sends a little every few seconds)
Reply stream(const std::string &url, const std::function<bool(const char *, size_t)> &on_data, int connect_ms, int idle_ms);

// Requests one after another on one connection kept open (a match's host sending it to the lobby several times a
// second: no new connection, and its TLS, each time). One thread's at a time.
class Client
{
public:
	Client();
	~Client();
	Reply request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms, size_t max_reply = 65536);
private:
	struct Impl;
	Impl *m = nullptr;
};

// A WebSocket to the lobby (a watched match's host sending it, a watcher getting it as it comes: one connection, small
// messages, nothing new a time): the system's own HTTPS (curl connected only, the WebSocket's few bytes of framing
// written here; WinHTTP's WebSocket on Windows), so nothing new is linked or shipped. Its own threads: the caller
// queues messages and takes what came, and never waits on the network. It keeps itself alive ("ping" every 15 s,
// the lobby answering "pong" without waking anything) and counts as closed after 45 s with nothing at all.
class Socket
{
public:
	struct Message { std::string data; bool binary = true; };
	enum class State { connecting, open, closed };
	explicit Socket(const std::string &url);   // https://... (wss) or http://... (ws, a lobby on this computer)
	~Socket();                                  // closed, its threads let go
	void send(std::string data, bool binary = true);   // queued; sent in order once open
	bool take(Message &m);                      // a message that came, oldest first
	State state() const;
	int status() const;                         // the upgrade's HTTP status (101; 429 the lobby busy; 0 none)
	std::string error() const;
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;
private:
	struct Impl;
	Impl *m = nullptr;
};

// The lobby's WebSocket frames read, as they come (Socket's on macOS and Linux; tools/wsfuzz throws anything at it):
// whole frames taken off the front of buf, a message's fragments joined, a ping handed back to be answered. Strict,
// as a client must be: a frame masked by the server, a reserved bit or opcode, a control frame fragmented or longer
// than 125, a continuation with no message begun or a new message in the middle of one, a message over max: all end
// the connection, as a close frame does.
struct FrameReader
{
	// (a message at most: the lobby's longest is a reply of 2,048 frames and their hashes, some 35 KB)
	static constexpr size_t MOST = 1u << 20;
	size_t max = MOST;
	enum class Got { more, closed };
	// each message whole to on_message (its bytes, binary or not); a ping's payload to on_ping (to be sent back as a pong)
	Got take(std::string &buf, const std::function<void(std::string &&, bool)> &on_message, const std::function<void(const std::string &)> &on_ping);
	// how much more buf may hold before take: a frame's head and max, no more (the rest left unread till it's taken;
	// with this much held and still no whole frame, take has ended it: a frame past max)
	size_t room(const std::string &buf) const { return buf.size() < max + 14 ? max + 14 - buf.size() : 0; }
	std::string why;   // (closed: what ended it, for the log)
private:
	std::string msg;
	bool in_msg = false, msg_binary = false;
};

// At a quit: every request on its way given up (curl's at once; WinHTTP's at its next step), none started after; and
// how many are still on their way (the quit waits a moment for none, so no thread's inside them as the program ends)
void cancel_all();
int active();

} // namespace vfnet::http
