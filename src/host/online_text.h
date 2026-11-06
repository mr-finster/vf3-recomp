// Every line the rooms and watching screens show (the browser, Create room, a room's page, watching, Quick match's
// line, their messages: src/host/online.cpp, main.cpp, menu.cpp), in one place for the translations. Each is a whole
// sentence; what changes in it is a {name} filled in by fill(), so a translation can put it where its language does.
// (UTF-8; "\xC2\xB7" is a middle dot, "\xE2\x80\x93" an en dash, "\xE2\x80\x94" an em dash.)
#pragma once
#include <initializer_list>
#include <string>
#include <utility>

namespace tr {

// fill("Looking for the match {code}...", {{"code", "QX7-KMP"}})
inline std::string fill(const char *t, std::initializer_list<std::pair<const char *, std::string>> with)
{
	std::string s = t;
	for (const auto &[k, v] : with) {
		const std::string key = std::string("{") + k + "}";
		for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + v.size())) s.replace(at, key.size(), v);
	}
	return s;
}

// ---- the room browser (the Online screen) ----
inline constexpr const char *ONLINE = "ONLINE";
inline constexpr const char *GAME_VF3 = "Virtua Fighter 3";
inline constexpr const char *GAME_VF3TB = "VF3 Team Battle";
inline constexpr const char *SET_YOUR_NAME = "Set your name";
inline constexpr const char *PLAYING_AS = "Playing as";
inline constexpr const char *QUICK_MATCH = "QUICK MATCH";
inline constexpr const char *N_ONLINE = "{n} online";
inline constexpr const char *CREATE_ROOM = "CREATE ROOM";
inline constexpr const char *ENTER_CODE = "ENTER CODE";
inline constexpr const char *MORE = "MORE";
inline constexpr const char *FILTER_ALL = "All";
inline constexpr const char *FILTER_OPEN = "Open";
inline constexpr const char *FILTER_PLAYING = "Playing";
inline constexpr const char *ROWS_RANGE = "{from}\xE2\x80\x93{to} of {n}";
inline constexpr const char *ONE_ROOM = "1 room";
inline constexpr const char *N_ROOMS = "{n} rooms";
inline constexpr const char *N_NEW_ABOVE = "{n} new above";
inline constexpr const char *COL_ROOM = "ROOM";
inline constexpr const char *COL_HOST = "HOST";
inline constexpr const char *COL_STATUS = "STATUS";
inline constexpr const char *COL_WATCHING = "WATCHING";
inline constexpr const char *LOOKING_FOR_ROOMS = "Looking for rooms...";
inline constexpr const char *ONLINE_UNREACHABLE = "Can't reach the online service right now.";
inline constexpr const char *ONLINE_UNREACHABLE_MORE = "Trying again by itself. Play by address (More) still works.";
inline constexpr const char *NO_OPEN_ROOMS = "No open rooms right now.";
inline constexpr const char *NO_MATCHES_LISTED = "Nobody's playing in a listed room right now.";
inline constexpr const char *ALL_SHOWS_EVERY_ROOM = "All shows every room.";
inline constexpr const char *NO_ROOMS = "No rooms right now.";
inline constexpr const char *NO_ROOMS_MORE = "Create one, or Quick match to play someone near you.";
inline constexpr const char *ROOMS_OFF = "Rooms aren't available right now.";
inline constexpr const char *ROOMS_OFF_MORE = "Quick match and Enter code still work.";
inline constexpr const char *QUICK_MATCH_TAG = "QUICK MATCH";
inline constexpr const char *ROW_OPEN = "Open";
inline constexpr const char *ROW_WATCH = "WATCH";
inline constexpr const char *ROW_JOIN = "JOIN";
inline constexpr const char *BROWSER_NOTE = "Anyone with a room's code can watch its matches. Quick Match games can be watched by anyone.";
inline constexpr const char *HOSTS_ROOM = "{name}'S ROOM";
// (the key hints' words)
inline constexpr const char *HINT_JOIN = "join";
inline constexpr const char *HINT_WATCH = "watch";
inline constexpr const char *HINT_SELECT = "select";
inline constexpr const char *HINT_ROOM = "room";
inline constexpr const char *HINT_NAME = "name";
inline constexpr const char *HINT_PAGE = "page";
inline constexpr const char *HINT_BACK = "back";
inline constexpr const char *HINT_TYPE = "type";
inline constexpr const char *HINT_DELETE = "delete";
inline constexpr const char *HINT_SAVE = "save";
inline constexpr const char *HINT_CANCEL = "cancel";
inline constexpr const char *HINT_CHANGE_NAME = "change the name";
inline constexpr const char *HINT_CHANGE = "change";
inline constexpr const char *HINT_CREATE = "create";
inline constexpr const char *HINT_CLOSE_MENU = "close the menu";
inline constexpr const char *HINT_PLAY_SOLO = "play solo";

// ---- a room's own page ----
inline constexpr const char *ROOM_HOSTED_BY = "Hosted by {flag}{name} \xC2\xB7 code {code}";
inline constexpr const char *ROOM_QUICK_SUB = "Two players matched by Quick match \xC2\xB7 watch code {code}";
inline constexpr const char *ROOM_PLAYING = "PLAYING";
inline constexpr const char *ROOM_OPEN = "OPEN";
inline constexpr const char *ROOM_NEXT = "NEXT";
inline constexpr const char *N_WATCHING = "{n} watching";
inline constexpr const char *WAITING_FOR_PLAYER = "Waiting for someone to play";
inline constexpr const char *JOIN_AND_PLAY = "Join and play now.";
inline constexpr const char *N_WAITING_TO_PLAY = "{n} waiting to play. Join to play after them.";
inline constexpr const char *NOBODY_WAITING = "Nobody waiting. Join to play next.";
inline constexpr const char *BUTTON_WATCH = "WATCH";
inline constexpr const char *BUTTON_PLAY_NEXT = "PLAY NEXT";
inline constexpr const char *BUTTON_JOIN = "JOIN";
inline constexpr const char *BUTTON_BACK = "BACK";
inline constexpr const char *BUTTON_STOP = "STOP";

// ---- Create room, its name ----
inline constexpr const char *CREATE_ROOM_TITLE = "CREATE ROOM";
inline constexpr const char *ROOM_NAME_FIELD = "Name";
inline constexpr const char *LISTED_IN_ROOMS = "Listed in Rooms";
inline constexpr const char *ON = "On";
inline constexpr const char *OFF = "Off";
inline constexpr const char *LISTED_ON = "Anyone can see it and join";
inline constexpr const char *LISTED_OFF = "Only players with the code";
inline constexpr const char *CREATE_ROOM_NOTE = "Anyone with the code can watch your matches. When one ends, the next player waiting plays you.";
inline constexpr const char *BUTTON_CREATE = "CREATE";
inline constexpr const char *ROOM_NAME_TITLE = "ROOM NAME";
inline constexpr const char *ROOM_NAME_SUB = "How the room shows in Rooms and on the Online page \xC2\xB7 up to 16 letters";
inline constexpr const char *JUST_TYPE_IT = "Just type it";
inline constexpr const char *ROOM_NAME_LETTERS = "Letters, numbers and  !  .  -  '";
inline constexpr const char *ROOM_NAME_NEEDS = "A room's name needs a letter or a number.";
inline constexpr const char *LISTED_AS = "Listed in Rooms as {room}";
inline constexpr const char *SEND_THE_CODE = "Send it to your opponent: they choose Online > Enter code and type it";

// ---- More, Settings ----
inline constexpr const char *PLAY_BY_ADDRESS = "Play by address";
inline constexpr const char *PLAY_BY_ADDRESS_HELP = "Same network, or a VPN: no code, no lobby.";
inline constexpr const char *SETTINGS = "Settings";
inline constexpr const char *SETTINGS_HELP = "Input delay, the connection display, Hide my IP address and the rest.";
inline constexpr const char *QUICK_LISTED = "Show my Quick Match games in Rooms";
inline constexpr const char *QUICK_LISTED_HELP = "On: anyone can find your Quick Match games in Rooms and watch them. Off: they're not listed (listed only if both players have it On).";

// ---- Quick match ----
inline constexpr const char *QUICK_WATCHABLE = "Quick Match games can be watched by anyone.";
inline constexpr const char *ONLINE_UNAVAILABLE = "Online is unavailable right now.";
inline constexpr const char *ONLINE_DOWN = "Can't reach the online service right now. On the same network you can still play by address (More).";

// ---- watching ----
inline constexpr const char *WATCHING = "WATCHING";
inline constexpr const char *WATCHING_CODE = "WATCHING {code}";
inline constexpr const char *STEP_FIND = "FIND";
inline constexpr const char *STEP_START = "START";
inline constexpr const char *STEP_CATCH_UP = "CATCH UP";
inline constexpr const char *STEP_WATCH = "WATCH";
inline constexpr const char *WATCHING_A_MATCH = "Watching a match";
inline constexpr const char *WATCH_CODE_BAD = "A watch code is 6 letters and numbers (like QX7-KMP): Online > Rooms lists the matches to watch.";
inline constexpr const char *WATCH_IS_TEAM_BATTLE = "That match is Team Battle's: switch to it in Game > Version to watch it.";
inline constexpr const char *WATCH_IS_REVISION_D = "That match is Revision D's: switch to it in Game > Version to watch it.";
inline constexpr const char *WATCH_OTHER_ROM = "Your game files (the ROM) aren't the players': you can't watch their match.";
inline constexpr const char *WATCH_OTHER_VERSION = "The players have another version ({version}): update the game to watch them.";
inline constexpr const char *WATCH_START_BAD = "The match's start couldn't be loaded here.";
inline constexpr const char *WATCH_LOOKING = "Looking for the match {code}...";
inline constexpr const char *WATCH_GETTING = "Getting the match...";
inline constexpr const char *WATCH_STARTING = "Getting the match's start...";
inline constexpr const char *WATCH_CATCHING_UP = "Catching up with the players: {pct}%";
inline constexpr const char *WATCH_BUFFERING = "Waiting for the players' next frames...";
inline constexpr const char *WATCH_BETWEEN = "The match is over: waiting for their next one...";
inline constexpr const char *WATCH_ON = "Watching";
inline constexpr const char *WATCH_NOTHING = "Nothing to watch with the code {code}: the match may be over, or the code isn't right.";
inline constexpr const char *WATCH_FULL = "Too many watching right now. Try again in a little while.";
inline constexpr const char *WATCH_BUSY = "Watching is busy right now. Try again in a little while.";
inline constexpr const char *WATCH_OFF = "Watching isn't available right now.";
inline constexpr const char *WATCH_LOBBY_GONE = "Can't reach the online service right now, so watching stopped. The players' match goes on.";
inline constexpr const char *WATCH_NONE_STARTED = "No match started under the code {code} in two minutes.";
inline constexpr const char *WATCH_OUT_OF_STEP = "This match can't be watched here (it went out of step).";
inline constexpr const char *WATCH_MATCH_OVER = "The match is over.";
inline constexpr const char *WATCHING_P1_VS_P2 = "Watching {p1} vs {p2}";
inline constexpr const char *WATCHING_BEHIND = "Watching \xC2\xB7 a few seconds behind the players";
inline constexpr const char *WATCHING_HOW_TO_STOP = "The players never wait for you. Esc or Start: the menu, to stop watching";
inline constexpr const char *MATCH_OVER = "MATCH OVER";
inline constexpr const char *BUTTON_WATCH_AGAIN = "WATCH AGAIN";
inline constexpr const char *WATCHED_FOR = "You watched {time}.";
inline constexpr const char *WATCH_AGAIN_NOTE = "Watch again follows the same watch code: their next match, if they play on";
inline constexpr const char *STOP_WATCHING = "Stop watching";
inline constexpr const char *STOP_WATCHING_HELP = "Back to playing on your own.";
inline constexpr const char *PLAY_NEXT = "Play next";
inline constexpr const char *PLAY_NEXT_HELP = "Wait to play in this room: when the match on ends, the next player waiting plays the host.";
inline constexpr const char *IN_LINE = "In line for room {code}: {n} of {of}. You play the host when it's your turn.";

// ---- links into the game ----
inline constexpr const char *LINK_FINISH_FIRST = "Finish this match first: then open the link again.";
inline constexpr const char *LINK_IS_TEAM_BATTLE = "That room plays Team Battle: switch to it in Game > Version, then open the link again.";
inline constexpr const char *LINK_IS_REVISION_D = "That room plays Revision D: switch to it in Game > Version, then open the link again.";

} // namespace tr
