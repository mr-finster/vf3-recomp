// HTTPS for the lobby (one request at a time, blocking: callers run it on a thread of their own). The system's own:
// libcurl on macOS and Linux (loaded as the game runs, so nothing's linked or shipped for it; a Linux without it
// says so), WinHTTP on Windows. Their certificates and proxy settings are the system's.
#pragma once
#include <string>

namespace vfnet::http {

struct Reply
{
	int status = 0;        // the HTTP status; 0 if no reply came
	std::string body;
	std::string error;     // why there was no reply, in words
};

// method "GET" or "POST" (body sent as JSON); timeout_ms for the whole request
Reply request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms);

// At a quit: every request on its way given up (curl's at once; WinHTTP's at its next step), none started after; and
// how many are still on their way (the quit waits a moment for none, so no thread's inside them as the program ends)
void cancel_all();
int active();

} // namespace vfnet::http
