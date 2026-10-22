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

5. The games are built with the address: the Makefile's `VF_LOBBY_URL` (the one in use now), or
   `export VF_LOBBY_URL=...` before the build commands for another. Empty: only the by-address matches.

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
- **The random queue**: one Durable Object a game version, in memory (a restart or a deploy drops those searching:
  their games enter again by themselves). Only a player with a wait open is matched (a game closed without saying
  so isn't); one who leaves mid-match-making closes the room made. The count on the Random card is those searching
  plus two a random match made in the last 10 minutes, kept 30 s at Cloudflare's edge (everyone on the Online
  screen asks: the queue answers once a place). It's fine into the thousands searching at once; well past that,
  the queue's pairing (each against all) would want splitting by region.
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

## What it keeps

A room: the host's name, country, game, version and addresses, for as long as the host's game keeps asking (it's
gone 30 seconds after the host stops, two hours at most); after that, for two hours, only its key (so its host
alone can open the code again). The random queue, in memory only: each searcher's name, country, addresses and
rough location (Cloudflare's, for the nearest; never sent to anyone), while they search and a minute after; how
many matches each address had in the last 10 minutes; and when recent matches were made (for the count). The
relay's logins counted for the day, all told and per address (the address itself kept as the count's key, wiped
each day). Requests are limited per IP address (Cloudflare's rate limiting, kept for a minute). No logs are
written by the Worker.
