# The lobby (room codes)

A Cloudflare Worker that turns a room code into the other player's address. No game traffic goes through it: the
two games connect to each other directly, and only when they can't (a phone's 5G, some office or university
networks) through Cloudflare's TURN relay. One lobby serves VF1, VF2 and VF3.

Cost on the Workers Paid plan ($5 a month): the Worker and its Durable Objects are well inside what the plan
includes. The TURN relay is only used by matches that can't connect directly; a match through it uses under
100 MB an hour, and Cloudflare's first 1,000 GB a month are free ($0.05 a GB after).

## Setting it up (once)

Everything runs on the Mac, from this folder.

1. Put the Worker online. `wrangler login` opens the browser: allow it.

   ```
   cd ~/vf3-recomp/tools/lobby && npm install && npx wrangler login && npx wrangler deploy
   ```

   The last lines it prints give the lobby's address, like `https://vf-lobby.yourname.workers.dev`. Keep it.

2. Make a TURN key: in the Cloudflare dashboard, **Realtime** (in the left column; it may be under
   **Calls**) > **TURN Server** > **Create**, name it `vf-lobby`. It shows a **Turn Token ID** and an
   **API Token** (the token only once: copy it now).

3. Give them to the Worker (each command asks for the value: paste it, Enter):

   ```
   cd ~/vf3-recomp/tools/lobby && npx wrangler secret put TURN_KEY_ID && npx wrangler secret put TURN_KEY_API_TOKEN
   ```

4. Check it (with your address from step 1):

   ```
   curl https://vf-lobby.yourname.workers.dev/v1/ping
   curl -X POST https://vf-lobby.yourname.workers.dev/v1/turn -d '{"game":"vf3","version":"test","content":"0123456789abcdef"}'
   ```

   The first says `{"ok":true,...}`, the second lists `servers` with a username and credential. `no_turn`
   means step 2 or 3 went wrong: put the secrets again.

5. The games are built with the address: put it in `.lobby-url` (one line) at the top of each game's folder
   (VF1, VF2 and VF3: git ignores it, so the address is never in the source), or
   `export VF_LOBBY_URL=...` before the build commands. Neither: only the by-address matches.

## Changing it later

`npx wrangler deploy --env=` again after editing `src/index.js` (staging first: below). A deploy (or any change to a
variable in the dashboard) restarts the objects: a room is kept 30 s from then for its host to come back (its game
reconnects its socket at once, or waits over HTTPS), a watched match is kept on for its host's next upload (it sends
the match again if the lobby lost it), and those searching in Quick match enter again by themselves.
`npx wrangler tail` shows the requests as they come in.

## Testing on one's own computer

`npx wrangler dev --port 8787`, with a TURN server of your own (coturn) named in `.dev.vars`:

```
TURN_STATIC=[{"host":"127.0.0.1","port":3478,"username":"test","credential":"secret"}]
```

and the games started with `VF_LOBBY=http://127.0.0.1:8787`. Without wrangler, the Worker runs in Node with stand-ins
for its Durable Objects (in memory, no TURN): `node test/serve.mjs src/index.js 8787` (its variables after the port,
e.g. `SWITCH_WATCH=off`, and changed while it runs with `POST /__vars?SWITCH_WATCH=off`); `npm test` (`test/all.mjs`) runs the pairing,
the rooms and watching tests, every combination of the off switches, the pairing tests again under each switch
(the routes the games before rooms and watching use, answered as they were), and a code review's cases
(`test/review.test.mjs`: each failed before its fix), each file to its end, then lists every
failed check with what it got and what it wanted. No test sleeps on the lobby's background work (the online count
told, a socket's message handled): the stand-ins keep it, and `settle()` waits for exactly that, so a slow or busy
machine can't change a result.

## Off switches (an instant undo, no game release)

Four Worker variables (Settings > Variables and Secrets, plain text; or `[vars]` here), read at every request, so a
change is live in seconds. `off` turns one off; anything else, or none, leaves it on. Each leaves the games working
as the builds before rooms and watching did: Quick match and room codes untouched, one plain line where a feature is
off. (`ROOMS` and `WATCHES` are the Durable Objects' bindings, hence the `SWITCH_` names.)

| variable | off |
|---|---|
| `SWITCH_ROOMS` | no rooms list, nothing listed, no room that stays: Create room makes a code-only room, as before. The games' browser says "Rooms aren't available right now. Quick match and Enter code still work." |
| `SWITCH_LISTING` | rooms work (a room that stays, its line) but none is listed, nor any Quick match to watch: the same line in the browser |
| `SWITCH_WATCH` | no match is sent or watched: a host is told `off` (its game stops sending, as at the month's ceiling), watchers on a socket are told and let go, a new one is told "Watching isn't available right now."; the rooms list keeps only the open rooms |
| `SWITCH_WEBSOCKETS` | no WebSocket is taken (a host's room, its feed, a watcher's frames): each falls back to HTTPS as on a network that carries none; open ones are closed as they're next used (a room's within a minute) |

One the other way round: `SWITCH_SNAP` is off unless it's `on`. On, a watcher come late (the match more than ~35 s in)
gets the host's latest snapshot (its game's state at a confirmed frame, a few MB, asked of the host when none is fresh:
`/v1/watch/snap`, src/net/watch.h's Snapper) and runs only the frames after it, a few seconds, not the whole match;
off, or with an older build on either side, the watcher runs the match from its start as before. Off by default: a
snapshot is a state the host's game made, loaded by every late watcher's game, so it stays off until each game's
state loading has been checked against states made to do harm.

`/v1/online` says what's on (`features: {rooms, watch}`); an older lobby says nothing, and the games and the site read
that as both off. The games built before this see none of it.

## Staging

`wrangler.toml`'s `[env.staging]` is the same code as its own Worker, `vf-lobby-staging`, with its own Durable Objects
(its own rooms, queues, counts and matches: nothing shared with live) and per-address limits set high for load tests:
`npx wrangler deploy --env staging` (`npm run deploy:staging`); the live one stays `npx wrangler deploy --env=` (`npm
run deploy`; plain `npx wrangler deploy` does the same with a warning that there are two). Its address is the live one's
with `-staging` in the Worker's name. Point a game at it with `VF_LOBBY=<address>`, `tools/watchsim --lobby <address>`,
`tools/lobbyload/load.mjs --lobby <address>`, the site with its `LOBBY_URL`. Its relay secrets are its own (`npx wrangler
secret put TURN_KEY_ID --env staging`); without them it answers `no_turn` and matches connect directly.

## A load test (tools/lobbyload)

`node tools/lobbyload/load.mjs --lobby <address> --rooms 100 --quick 50 --watchers 400 --browsers 100 --seconds 60
--https 0.2` (Node 22, nothing installed): hosts with rooms that stay and their guests, Quick match pairs, each match
sent as the game sends it (its WebSocket, 5 a second while watched, every 30 s while not, woken by the first watcher;
or HTTPS POSTs), watchers on WebSockets, the HTTPS stream and pieces, browsers reading the list every 15 s. It prints
each route's requests, statuses and times (p50, p95), the sockets, and every error, and exits 1 on any error or 5xx.
All from one address: the live lobby's per-address limits answer 429 past them (staging's are high: run `test/serve.mjs`
with staging's `RANDOM_PER_ADDRESS=100000 RANDOM_MATCHES_PER_ADDRESS=100000 WATCHERS_PER_IP=100000
ROOMS_PER_ADDRESS=100000` after the port to do the same here). Against `test/serve.mjs` here, that run: 100 rooms and
29 Quick matches played, 400 watchers, ~8,200 requests and upgrades, ~23,000 WebSocket messages to the lobby and
~84,000 from it in a minute, no errors. Its first run found a room held only by its host's socket for over 30 s
being dropped the moment that socket closed (fixed, and tested).

## Rooms and watching

`/v1/online` also lists the rooms (`rooms`): rooms whose hosts listed them (Create room: listed, or listed with a lock,
the code then left out), waiting or playing (a room that stays through its match, with its line), and the matches that
may be watched, Quick match games among them (`random: true`: both players, the score, a watch code and no code to
join; listed only once the host's game sends the match, and only when both players' games said `qm_listed: true`, Show
my Quick Match games in Rooms On: so never with an older build's player, which never says; watchable by its watch code
either way). A listed room's score is the matches won between its host and the player it has now, all their matches in
it together: the same two again (Play again, or back from its line) go on from where they were, someone else starts
from 0-0; from its Watch's tellings (the match's id, the two players, the score), and kept as last known when the
Watch is told no more while the room is still held (its host's uploads stopped). A Quick match's watch code is one of its own, never its room's code: that is the code its Play again opens
again for the other player, and a stranger who had it from the list could take that room (and the host's addresses)
first; and `you`, the asker's continent
(the browser's "near me"). A match takes 400 watchers at most (`WATCH_MAX`): one more is answered `full` (429), which
the game says as "Too many watching right now". Each match that may be watched has a
`Watch` (a Durable Object for its watch code): the host sends it the match 5 times a second while anyone watches and
every 30 s while nobody does (the first watcher to come has it woken at once, on its WebSocket), watchers get the
frames from it (`src/net/watch.h` has the messages). No checkpoints: a state one watcher made, handed to
others, could be forged to match the players' hashes, so the lobby takes none and gives none out (`/v1/watch/cp` answers
410); a watcher who comes late runs the match from its start. A watcher on a WebSocket is told when the match is over
(the host's last upload, even one with no new frames), and when the host goes quiet with no last word (its game closed,
its connection gone): the `Watch`'s alarm looks 30 s after the host's last upload while anyone's on a socket, and
tells each one from what its socket kept even if the match itself went with the object's memory. A `Watch` restarted
(a deploy) takes the host's next upload as its sign of life, not its last before: a match on isn't told over meanwhile.
Only the end of a match its host said is over is kept a day at the edge; one taken for over because the host went
quiet, a second. A reply's hashes reach back
120 frames before its first frame (`HASH_BACK`): the players' hashes are made a few frames after their frames, so a
watcher still gets (and checks) one whose frame it already has. Deploying adds the `Watch` class (`wrangler.toml`'s migration `v4`):
nothing else changes for the games already out, which send none of the new fields and get none of it.

What it costs (measured with `test/serve.mjs`'s counts; the whole table is in the design notes): a match nobody watches,
its host on a WebSocket, about 120 small messages an hour (6 requests' worth: a WebSocket message in is billed as a
twentieth of one) and its room held by one socket, no requests; a watched one 18,000 messages an hour (900 requests'
worth); a watcher on a WebSocket about 4 requests a visit, the frames sent to it free. The HTTPS fallbacks (POSTs, the
stream, polling pieces mostly answered by Cloudflare's edge) cost a request each. A new game's join (it asks to wait in
line, `queue`) asks its room twice the first time (whether it's in the line, then the join), once after: one more
Durable Object request a join, $0.15 a million. Nothing's charged for the bytes.

## What stops abuse

- **Watching's caps** (`src/index.js`, the `Watch`): 400 watching a match (one more answered `full`; a stream counts,
  so it must say whose it is, `s`); 8 streams or WebSockets a match from one address, 32 watchers a match from one
  address however they watch (`WATCHERS_PER_IP`; an IPv6 address by its /48 for both, so one /48's many /64s can't
  fill a match between them); 60 watchers starting a minute from one address (`WATCH_LIMIT`). A watcher's WebSocket
  that sends anything but a ping is closed, one with no ping for 90 s (its game pings every 15 s) is closed and no
  longer counted, and a stream that hasn't taken what it was sent for 10 s is let go (its game asks the pieces). All matches
  together, as the `Stats` count them: past 4,000 watching, new watchers get no stream and ask for the frames every
  3 s; past 8,000 they're answered `busy` ("Watching is busy right now"); past 500 watched matches the hosts send
  every 400 ms, past 2,000 every second. The month's ceiling: past 2,200 watched match-hours this month (UTC; an
  hour nobody watched counts 0.4; a `MONTH_MATCH_HOURS` variable on the Worker changes it, `0` meaning no watching at
  all) the hosts are told to stop sending and new watchers (on a socket, a stream or the pieces) are told busy. None of it reaches the players. `node test/serve.mjs` counts the requests by route and the WebSocket
  messages (`GET /__counts`).
- **The rooms list**: 6 listed rooms or listed matches from one address at most (by a hash of it, an IPv6 one by its
  /48; its newest, `ROOMS_PER_ADDRESS`), so one address can't push everyone else's off the list (each game's 100 newest); a phone
  network's shared address still lists six.
- **Room codes aren't found by watching**: every new game's room is watchable by its code, so a code nobody has, a room
  with no Watch and a Watch with no match on yet answer `/v1/watch/seen`, `/count`, `/frames` and `/start` alike, and a
  code new to the asker counts as a join (above); a watcher asking again about the code it watches, or a guest its
  count, doesn't.
- **WebSockets**: the host's feed (`/v1/watch/feed`) and each watcher (`/v1/watch/ws`) on the `Watch`'s own
  WebSockets, and a room code's host on its `Room`'s (`/v1/room/ws?code=&key=`: told `{guest}` the moment a player
  joins, as a wait's answer, `{error: "no_room"}` if the room goes; while it's open the room's held, no `/v1/wait` or
  `/v1/hold`, and an alarm tells the online count it's there once a minute), all with the Hibernation API (no new
  binding or migration: the same classes); a ping answered by Cloudflare without waking them (set as each object is made, so after it hibernates too). A room's host socket with no
  ping for 90 s (its game pings every 15 s: gone without a close) is closed at the room's next alarm and the room let go, off the list. The HTTPS routes stay,
  for networks that won't carry a WebSocket.

- **Names**: a room's name and every player's name the others see (a host, a guest, those in a line, a watched match's
  two) are checked against a short list of words (`src/words.js`, English, Korean and Japanese romanised, Spanish,
  Portuguese and French; edit it freely). On a hit, quietly: a room is listed under its host's name ("<HOST>'S ROOM"),
  a player is shown as PLAYER; no error, so old games carry on. The host's answer says how its room is listed
  (`listed_as`), and the game shows it. Matched word by word on the name made plain (digits and $ @ ! | for letters, each
  word's marks out, one-letter words in a row joined, repeats cut short, and again with V as U: FVCK), never across two
  real words (SUMMER DAY, ORBIT CHAMP and WHO REALLY are fine); the strong words with their endings too (SHITS,
  SHITTING, WANKER), the short ones only as words of their own or their plurals, so CLASSIC, PASSION, SCUNTHORPE,
  ASSASSIN, MERDEKA and SIBALD are fine. A word split by a space is caught where the words in a row spell, all of them
  and no more, a strong word or one of those blocked anywhere (NI GGER, FU CK, BIT CH; never the short ones that way:
  PUT A, AS SOON); and one of them run into an insult with a word from a short list after it (SHITFACE, CUNTFACE,
  COCKSUCK, DICKHEAD), never with anything else (COCKPIT, SHITAKE, TITAN, HANCOCK). The price: a few real two-word
  phrases that spell one, all of it (SH IT, WHO RE, BULLS HIT, WAN KING), are taken for it (`npm test` has the cases).

- **Rate limits** per address (an IPv6 one by its /64), in `wrangler.toml`: 600 requests a minute in all (watching's repeated asks apart: a host's pushes over HTTPS, a watcher's
  pieces, seen and count, 1,800 a minute, `WATCHING_LIMIT`, so a crowd watching never blocks its address's players), 60 rooms
  opened or random searches, 20 joins (a join, an Enter code's peek, or a watch code new to the asker: a watcher's first seen, a
  guest's first count; an IPv6 /48's /64s 200 all told, `CODE48_LIMIT`; a player in a room's line asking again every
  4 s isn't charged: the room is asked first, and says so only for one in its line), 30 relay logins, 180 waits; 6 random searches at once and 120 random
  matches in 10 minutes; and 200 relay logins a day (an IPv4 address, or an IPv6 /48). High enough for a phone
  network's or a school's shared address (many players behind one); a flood, or a room code guessed at (31^6,
  about 900 million codes), is stopped.
- **Who meets whom** (issues #25, #32): a game that plays across settings sends a `pool` (its online version's
  base with a flag: VF3's `+host1`, VF2's `+mix1`) and `accepts` (the flags of an older version it can play that
  version's way). Two players meet if `terms()` finds what their match plays as: the same pool (VF3: the host's stage
  select, Dural and Team Battle mode; VF2: each player's own fighter), the same version (every game before pools,
  and VF1), or one's version the other can play (an older build's, played its way). Both are told it (`as`), and the
  games' handshake compares it, so two that would play different games never start. Old games send no pool and are
  matched as before, by their version; new ones still meet them where they can play the old one's terms.
- **The random queue**: one Durable Object a game, version base (its version up to a `+`) and ROM, in memory (a restart or a deploy drops those searching:
  their games enter again by themselves). Only a player with a wait open is matched (a game closed without saying
  so isn't); one who leaves mid-match-making closes the room made. The count on the Random card is those searching
  plus two a random match made in the last 10 minutes, kept 30 s at Cloudflare's edge (everyone on the Online
  screen asks: the queue answers once a place). It's fine into the thousands searching at once; well past that,
  the queue's pairing (each against all) would want splitting by region. A player who found the other too far (their
  Random: max ping) names the room when they search again (`avoid`), and the two aren't matched again for 10 minutes
  (each room's two players, by address and name, kept in the queue's storage that long: it's let go of while they
  connect).
- **A room's code after it's closed** is kept for 2 hours for its host alone (by the room's key): a Play again opens
  the same code, and no one else can take it while the other player waits on it. A room lasts 2 hours at most; one
  that stays, from its last match (it goes on while it's in use).
- **Addresses a game gives** are read whole (any way an IPv6 is written) and dropped if they'd point the other
  game at nowhere, everyone (multicast, broadcast) or itself (loopback).
- **The relay's logins are capped**: `TURN_PER_DAY` in `src/index.js` (100,000 a day, about 50,000 match tries), and
  200 a day from one address; past it no more are given out that day (matches still connect directly, only the
  relay's wait for tomorrow). A login is given only to an ask that names one of the games, its version and its ROM's
  hash, and works for 6 hours (`TURN_TTL`), not a day. The relay is the one thing that costs by use, billed by its
  traffic (the first 1000 GB a month free), not its logins. **It can't be locked to the games alone:** they're open
  source, so whatever a game asks, a script could ask too; the caps and the short logins limit what one address can
  take, and the billing alert below is what bounds the bill.
- **A billing alert** (do this once): Cloudflare dashboard > **Notifications** > **Add**, pick the usage-based
  billing alert, and set an amount you'd want to hear about (say $5). Cloudflare has no hard spending cap for the
  relay; the cap above and this alert are what there is.
- **The secrets** (`TURN_KEY_ID`, `TURN_KEY_API_TOKEN`) never leave the Worker: players get only 6-hour relay
  logins. If the token ever leaks, regenerate it in the dashboard (Realtime > TURN Server) and put it again.
- **What the games are told**: only another player's addresses (checked: nothing that points a game at its own
  machine, broadcast or multicast), name and version, and only to someone with the room's code. The room's host
  key (who may wait, reopen, close) and the match's token are random and new for each room.
- **No game data comes through here**, and nothing is logged by the Worker.

After changing anything here, `npx wrangler deploy` again.

## Who's online

`GET /v1/online` (JSON, read only, `access-control-allow-origin: *`) says how many play each game now, and the
**Online now** page (`tools/online`, a Pages site of its own) shows it. For each game (`vf1`, `vf2`, `vf3`,
`vf3tb`): everyone, one count a game, since players meet whatever their settings (a match plays the host's), and
of them those on an older version:

```
{"updated": 1793594882, "games": {"vf3tb": {"total": 3, "searching": 1, "random": 2, "hosting": 0, "queued": 0,
  "room": 0, "older": 1, "countries": {"JP": 2, "FR": 1}, "watching": 4}, ...}, "countries": {"JP": 5, "US": 3, "FR": 1, "": 1}}
```

- `searching`: in the Random queue now; `random`: in a Random match made in the last 10 minutes (as the RANDOM
  card's count); `hosting`: waiting with a room code; `queued`: in the line of a room that stays (still asking);
  `room`: in a room code match joined in the last 10 minutes (a host's reopen, the player who joined not getting
  going, takes it back), or in the match on in a room that stays, both its players for as long as its host says it's
  there. `total`: all of them (the parts add up to it). Play by address never reaches the lobby: not counted.
- `watching`: those watching the game's matches now, as each match's Watch counts them. Not in `total`: a watcher
  plays no one (and one who also plays is counted there, as a player). The sites' "Online now" and the Online page's
  counts are `total`; the page shows `watching` beside it.
- Each player once, in what they're doing now: searching, else hosting, else their latest match. A player is their
  address (an IPv6 one by its /64) and name, hashed (two searches, two rooms or a search again are one player; two
  players at one address with the same name count as one).
- `older`: of `total`, those on an older version (a version without the game's pool: before matchmaking across
  settings, or another version's), who meet only the players who can play their version's way. A game's pool is
  `POOLS` in `src/index.js` (its `NET_VERSION`'s base with `+host1`, VF2's `+mix1`): **when a game's `NET_VERSION`
  or pool changes, change it there** and deploy, or its players count as `older`. VF1 has no pool and nothing
  that splits players: its players are older only on a version other than `VF1_BASES`'.
- `countries`: each game's players by country (two capital letters, ISO 3166; `""` where it isn't known), most
  first, each player once as in `total` (a game's countries add up to its total); and the same for all the games
  together at the top (adding up to the games' totals). A player's country is where the lobby sees them connect
  from: Cloudflare's for the request (`country_of` in `src/index.js`, the same one Random's nearest-first matching
  and the TURN login use), never anything a game says of itself. Cloudflare's unknown (`XX`) and Tor (`T1`) and
  anything not two letters count as `""`. Only these totals are given out: no names, addresses, room codes or
  places finer than a country. (Older clients of `/v1/online` ignore the field.)
- **Not counted:** Play by address (the lobby never sees it), and a match whose players went on past 10 minutes
  (it's the matches *made* in the last 10 minutes: a long session counts while it's young, as the RANDOM card).

**How it's counted, and why it can't be pushed up:** the games never say how many there are. The count is only what
the lobby itself saw happen: a search it took (a ticket it gave out), a room it opened, a join it let in, a match it
made. One Durable Object (`Stats`) is told by the rooms and random queues as these happen, each player with the
country the lobby itself had for them; a version string is only
(a pool's player: its pool) compared against `POOLS`, never shown; each player counts once (hashed address and
name), so asking again doesn't add anything, and a flood from one address is held by the rate limits as anything
else. Entries go on the lobby's own timeouts: a host gone quiet 30 s (its room dropped), a queue not heard from in a
minute, a match after 10 minutes. The endpoint only reads.

**What it costs:** a page view reads the count kept at the edge (15 s, and each Worker copy's memory 15 s), so the
`Stats` object is asked about four times a minute a Cloudflare location, not once a viewer. The rooms tell it when a room
opens, is joined, reopens or goes, and a waiting host once a minute; each queue tells it when who's searching
changes, at most every 2 s, and every 15 s while anyone searches. `GET /v1/online` isn't limited per address: the
online page's function asks it from Cloudflare's own addresses, many visitors' asks from one, and being answered from
memory or the edge it costs a request each however often it's asked.

### The online page (tools/online)

`online.html` (and its icons, and the game's own Barlow Condensed font) fetches `/api/online` on its own site 15 s
after each answer while the page is seen (none while it's hidden, at once when it's shown again if one was missed;
one at a time, given up after 8 s); `functions/api/online.js` there asks the lobby's `/v1/online` and passes on only
the counts and the rooms, read and written again (no header, error text or field of the lobby's answer reaches the
browser; the countries only as two capital letters or `""` with a whole number, 250 at most), kept 15 s by the
counts' own time. A request that finds nothing fresh asks the lobby itself (no request waits on another's), gives
it 4 s, and if it fails passes on its last answer up to 2 minutes old marked `"stale": true` (the page shows
the rooms with "can't reach the lobby · last updated ..."), not asking again for 5 s; the answer with the viewer's
continent (`you`) is kept private. Rooms are listed only when the lobby's `features.rooms` is `true`, matches only
when `features.watch` is (no `features`, an older lobby's: neither). At the top, each game's players online and
its Quick match count, and the top 8 countries, flag and number (the browser's own name for it on hover). The flags
are the Twemoji Country Flags font in `site/fonts` (flags only, so Windows, which has none, shows them too; from the
npm package country-flag-emoji-polyfill 0.1.10; the art Twemoji's, by Twitter, Inc. and contributors, CC BY 4.0,
credited on the page; `fonts/TwemojiCountryFlags-LICENSE.md`). Barlow there is three subsets of `assets/fonts`'
(Latin and Latin Extended-A, for Polish and Turkish too: `pyftsubset BarlowCondensed-Medium.ttf
--unicodes=U+0020-007E,U+00A0-017F,U+0218-021B,U+02C6,U+02DA,U+02DC,U+2013-2014,U+2018-201A,U+201C-201E,U+2020-2022,U+2026,U+2030,U+2039-203A,U+20AC,U+2122,U+2212
--layout-features=kern,liga,locl,frac,numr,dnom --flavor=woff2`, and the same for SemiBold and BoldItalic). The
lobby's address is the Pages project's `LOBBY_URL` secret, never in the files. The page's content security policy
allows its own site, Google Fonts (the Noto fonts for Korean, Japanese, Chinese and Russian) and only the page's one
script, by its hash: after changing the script, `node sitelang.mjs --csp` puts the new hash in `site/_headers`.
`node sitelang.mjs` checks every language's lines, that the three fonts have every letter each language shows, and
the hash; `node fntest.mjs` the function (what it passes on, the timeout, the stale answer, the failure remembered). If
the lobby can't be reached and there's no answer under 2 minutes old, the function answers
`{"error":"unavailable"}` (502) and the page says so; after 2 minutes unreachable the page drops the rooms it showed
(their links likely gone) and says so too.

Setting it up (once; the project's name is its address, `vf-online.pages.dev`: another, if that's taken, in
`tools/online/wrangler.toml` and the commands). Deploy the lobby first (it has `/v1/online` from then):

```
cd ~/vf3-recomp/tools/lobby && npx wrangler deploy
```

then create the project, give it the lobby's address (paste `https://<lobby address>`, no `/` at the end) and put
the page up:

```
cd ~/vf3-recomp/tools/online && ../lobby/node_modules/.bin/wrangler pages project create vf-online --production-branch main
cd ~/vf3-recomp/tools/online && ../lobby/node_modules/.bin/wrangler pages secret put LOBBY_URL --project-name vf-online
cd ~/vf3-recomp/tools/online && node sitelang.mjs && ../lobby/node_modules/.bin/wrangler pages deploy --branch main
```

(It's run with the lobby's wrangler, from `tools/online`: inside `tools/lobby` wrangler would mix the lobby's
settings into the page's; `node sitelang.mjs` first, so a page whose script's hash isn't in `_headers`, which
wouldn't run, never goes up.) The page is then at `https://vf-online.pages.dev/online` (`/` sends there). After
changing the page, the last command again; after changing `LOBBY_URL`, the secret and the deploy again. The
dashboard's way to the same: Workers & Pages > vf-online > Settings > Variables and Secrets > Add, type Secret,
`LOBBY_URL`.

Testing on one's own computer: the lobby with `npx wrangler dev --port 8787` (with `TURN_STATIC` set, as above,
nothing is kept 30 s: each read is fresh, and a request's `x-test-country: JP` header stands in for Cloudflare's
country, never read without `TURN_STATIC`), then `tools/online/.dev.vars` (untracked) with
`LOBBY_URL=http://127.0.0.1:8787` and `../lobby/node_modules/.bin/wrangler pages dev --port 8788` from
`tools/online`: the page at `http://127.0.0.1:8788/online`.

## What it keeps

A room: the host's name, country, game, version and addresses (and a room that stays: its name and its line, each
player in it by a hash of their address and name, and their name, let go 15 s after they stop asking), for as long
as the host's game keeps asking (it's
gone 30 seconds after the host stops, two hours at most; a room that stays, two hours from its last match); after that, for two hours, only its key (so its host
alone can open the code again). The random queue, in memory only: each searcher's name, country, addresses and
rough location (Cloudflare's, for the nearest; never sent to anyone), while they search and a minute after; how
many matches each address had in the last 10 minutes; and when recent matches were made (for the count). Who's
online (`Stats`): each player searching, hosting or in a match made in the last 10 minutes as a hash of their address
and name (not the address), with their game, version, room code and country (the lobby's own, two letters), until
the timeouts above; only the counts by country are given out, and the names of hosts and players in rooms their
hosts listed or let be watched (the rooms list), with a hash of each listed host's address (6 listed rooms an
address); each kept as an entry of its own in the object's storage, and the month's watched hours (the ceiling, kept
through a restart). A watched match, in memory: its start, its inputs and hashes,
the score, until it's over (and while nobody watches it, in its Watch's storage too,
which Cloudflare's hibernation would otherwise lose between the idle uploads; gone 3 hours after the last); its watchers as random numbers their games pick, for the count. The
relay's logins counted for the day, all told and per address (the address itself kept as the count's key, wiped
each day). Requests are limited per IP address (Cloudflare's rate limiting, kept for a minute). No logs are
written by the Worker.
