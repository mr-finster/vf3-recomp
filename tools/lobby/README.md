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

`npx wrangler deploy` again after editing `src/index.js`. Rooms open at the time are dropped, nothing else.
`npx wrangler tail` shows the requests as they come in.

## Testing on one's own computer

`npx wrangler dev --port 8787`, with a TURN server of your own (coturn) named in `.dev.vars`:

```
TURN_STATIC=[{"host":"127.0.0.1","port":3478,"username":"test","credential":"secret"}]
```

and the games started with `VF_LOBBY=http://127.0.0.1:8787`.

## What stops abuse

- **Rate limits** per address (an IPv6 one by its /64), in `wrangler.toml`: 600 requests a minute in all, 60 rooms
  opened or random searches, 20 joins, 30 relay logins, 180 waits; 6 random searches at once and 120 random
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
  the same code, and no one else can take it while the other player waits on it.
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
{"updated": 1793594882, "games": {"vf3tb": {"total": 3, "searching": 1, "random": 2, "hosting": 0, "room": 0,
  "older": 1, "countries": {"JP": 2, "FR": 1}}, ...}, "countries": {"JP": 5, "US": 3, "FR": 1, "": 1}}
```

- `searching`: in the Random queue now; `random`: in a Random match made in the last 10 minutes (as the RANDOM
  card's count); `hosting`: waiting with a room code; `room`: in a room code match joined in the last 10 minutes
  (a host's reopen, the player who joined not getting going, takes it back). `total`: all of them.
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

**What it costs:** a page view reads the count kept at the edge (30 s, and each Worker copy's memory 30 s), so the
`Stats` object is asked about twice a minute a Cloudflare location, not once a viewer. The rooms tell it when a room
opens, is joined, reopens or goes, and a waiting host once a minute; each queue tells it when who's searching
changes, at most every 2 s, and every 15 s while anyone searches. `GET /v1/online` is limited per address as
everything else (`ANY_LIMIT`, 600 a minute).

### The online page (tools/online)

`online.html` (and its icons, and the game's own Barlow Condensed font) fetches `/api/online` on its own site every
30 s; `functions/api/online.js` there asks the lobby's `/v1/online` and passes on only the counts, read and written
again (no header, error text or field of the lobby's answer reaches the browser; the countries only as two capital
letters or `""` with a whole number, 250 at most), kept 30 s. Under each card's count, one row
(Searching: in Random's queue; Waiting: hosting a room code; Playing: in a match, Random or room code) with its top
countries, and "By country": all the games together, flag and name (the browser's own name for it), the top 12
and how many more. The flags are the Twemoji Country Flags font in `site/fonts` (flags only, so Windows, which
has none, shows them too; from the npm package country-flag-emoji-polyfill 0.1.10; the art Twemoji's, by Twitter,
Inc. and contributors, CC BY 4.0, credited on the page; `fonts/TwemojiCountryFlags-LICENSE.md`). The lobby's address is
the Pages project's `LOBBY_URL` secret, never in the files; the page's content security policy allows only its own
site. If the lobby can't be reached it answers `{"error":"unavailable"}` (502) and the page says so.

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
cd ~/vf3-recomp/tools/online && ../lobby/node_modules/.bin/wrangler pages deploy --branch main
```

(It's run with the lobby's wrangler, from `tools/online`: inside `tools/lobby` wrangler would mix the lobby's
settings into the page's.) The page is then at `https://vf-online.pages.dev/online` (`/` sends there). After
changing the page, the last command again; after changing `LOBBY_URL`, the secret and the deploy again. The
dashboard's way to the same: Workers & Pages > vf-online > Settings > Variables and Secrets > Add, type Secret,
`LOBBY_URL`.

Testing on one's own computer: the lobby with `npx wrangler dev --port 8787` (with `TURN_STATIC` set, as above,
nothing is kept 30 s: each read is fresh, and a request's `x-test-country: JP` header stands in for Cloudflare's
country, never read without `TURN_STATIC`), then `tools/online/.dev.vars` (untracked) with
`LOBBY_URL=http://127.0.0.1:8787` and `../lobby/node_modules/.bin/wrangler pages dev --port 8788` from
`tools/online`: the page at `http://127.0.0.1:8788/online`.

## What it keeps

A room: the host's name, country, game, version and addresses, for as long as the host's game keeps asking (it's
gone 30 seconds after the host stops, two hours at most); after that, for two hours, only its key (so its host
alone can open the code again). The random queue, in memory only: each searcher's name, country, addresses and
rough location (Cloudflare's, for the nearest; never sent to anyone), while they search and a minute after; how
many matches each address had in the last 10 minutes; and when recent matches were made (for the count). Who's
online (`Stats`): each player searching, hosting or in a match made in the last 10 minutes as a hash of their address
and name (not the address), with their game, version, room code and country (the lobby's own, two letters), until
the timeouts above; only the counts by country are given out. The
relay's logins counted for the day, all told and per address (the address itself kept as the count's key, wiped
each day). Requests are limited per IP address (Cloudflare's rate limiting, kept for a minute). No logs are
written by the Worker.
