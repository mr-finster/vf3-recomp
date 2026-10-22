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
   curl -X POST https://vf-lobby.yourname.workers.dev/v1/turn -d '{}'
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

- **Rate limits** per address (an IPv6 one by its /64), in `wrangler.toml`: 120 requests a minute in all, 20 rooms
  opened, 20 joins, 10 relay logins. Enough for a LAN party or a phone network's shared address; a flood, or a
  room code guessed at (31^6, about 900 million codes), is stopped.
- **The relay's logins are capped**: `TURN_PER_DAY` in `src/index.js` (10,000 a day, about 5,000 matches). Past it
  no more are given out that day: matches still connect directly, only the relay's matches wait for tomorrow. The
  relay is the one thing that costs by use, so this is what bounds the bill; raise it as players come.
- **A billing alert** (do this once): Cloudflare dashboard > **Notifications** > **Add**, pick the usage-based
  billing alert, and set an amount you'd want to hear about (say $5). Cloudflare has no hard spending cap for the
  relay; the cap above and this alert are what there is.
- **The secrets** (`TURN_KEY_ID`, `TURN_KEY_API_TOKEN`) never leave the Worker: players get only day-long relay
  logins. If the token ever leaks, regenerate it in the dashboard (Realtime > TURN Server) and put it again.
- **What the games are told**: only another player's addresses (checked: nothing that points a game at its own
  machine, broadcast or multicast), name and version, and only to someone with the room's code. The room's host
  key (who may wait, reopen, close) and the match's token are random and new for each room.
- **No game data comes through here**, and nothing is logged by the Worker.

After changing anything here, `npx wrangler deploy` again.

## What it keeps

A room: the host's name, game, version and addresses, for as long as the host's game keeps asking (it's gone 30
seconds after the host stops, two hours at most). Requests are limited per IP address (Cloudflare's rate limiting, kept
for a minute), and the relay's logins counted for the day. No logs are written by the Worker.
