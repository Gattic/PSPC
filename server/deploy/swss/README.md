# Running the public SWSS server

The Star Wars Super Squadron (and Elite Squadron) GameSpy server on **swss.pspc.online**, on a machine
without a GPU. How it works and why: [docs/gamespy-server.md](../../../docs/gamespy-server.md#public-server);
what was audited and fixed first: [docs/gamespy-server-security.md](../../../docs/gamespy-server-security.md).

There are two ways in. **Forwarded ports** (below) is the one to use when the router can forward them; the
**Cloudflare Tunnel** (at the end) needs no open port. `install.sh` does the same server either way; `MODE=`
chooses (default `direct`).

## 1. The server (forwarded ports)

```sh
server/deploy/swss/install.sh                 # HOST=swss.pspc.online MODE=direct are the defaults
```

as the user that will run it (no root). It copies the code to `~/swss-server/app`, makes the private data
folder `~/swss-server/data` (the database `gamespy.sqlite`, `status.json`, `mode.env`), installs the user
service and starts `swss-server`. `PREFIX=/somewhere` chooses another place. Run it again after a `git pull`
to update the code (the database is untouched). The services listen on every address; the web pages on 8080.

**It will not start until `HOST` resolves** (explicit failure: "can't look up the public host"): do step 2 first.

```sh
systemctl --user status swss-server          # running?
journalctl --user -u swss-server -f          # what players do
cat ~/swss-server/data/status.json           # who is online, the games hosted
```

To keep it running when nobody is logged in: `loginctl enable-linger $USER` (may need root). Or as a system
service: copy `swss-server.service` to `/etc/systemd/system/`, fill in `@PREFIX@` and `@PYTHON@`, add `User=`
and the sandboxing you like (`ProtectSystem=strict`, `ReadWritePaths=`, `PrivateTmp=yes`, ...).

## 2. DNS and the router

**DNS.** In Cloudflare (or wherever `pspc.online` is): an **A record `swss`** with the router's public address,
**DNS only (grey cloud)**. Orange-cloud (proxied) answers with Cloudflare's addresses, which carry web pages
only: every other port times out, and the server would hand players Cloudflare's address for the games.
`install.sh` refuses to install direct mode until this is right; to check it yourself:

```sh
python3 server/tests/live_check.py --host swss.pspc.online --dns-only --here
```

If your address changes, use dynamic DNS for the record (the server looks the name up again every five minutes;
players' PSPC looks it up when the game connects).

**Router.** Forward to this machine's LAN address (make it static or reserve it):

| Protocol | Port (outside) | To (inside) | For |
|---|---|---|---|
| TCP | 29900 | 29900 | Presence: accounts, login, friends |
| TCP | 29901 | 29901 | Presence search |
| TCP | 28910 | 28910 | the server browser |
| TCP | 29920 | 29920 | stats |
| TCP | **80** | **8080** | the web pages (rankings, message of the day): see below |
| UDP | 27900 | 27900 | the master server (games ask whether the service is up) |
| UDP | 27901 | 27901 | NAT negotiation |
| UDP | **3658-3661** | **3658-3661** | the dedicated games' matches: one port each (`UDP_PORT`, see 3) |

**Port 80 is the one that goes wrong.** The game asks for port 80 and the server answers on 8080, so the rule is
*external 80 -> this machine's LAN address, internal 8080* (TCP). Entered the other way round (external 8080 -> internal 80) it
forwards to a port nothing listens on and leaves external 80 with no rule, which a router drops on its public address:
a connection that **hangs** instead of being refused. The table in
[docs/gamespy-server.md](../../../docs/gamespy-server.md#with-forwarded-ports) shows how to tell the cases apart from a
PC on the LAN. If 80 still hangs with the right rule, turn off the router's *Remote Management*; if it then works from
the LAN and not from outside, your ISP filters inbound 80. Nothing needed to find or join a game uses 80 (the message
of the day and *Player Rankings* only). Without it, give clients `"motd.gamespy.com": "127.0.0.1"` next to the
`*.gamespy.com` redirect so that the game's request fails at once instead of waiting 30 seconds.

Players on this network need **NAT loopback** (hairpin) on the router to use the public address. The server
itself needs no privileges.

Check it from **outside** the network (a phone on mobile data, or a friend), since inside it proves little (many
routers hairpin some forwards and not others):

```sh
python3 server/tests/live_check.py --host swss.pspc.online --http-port 80 --game swsspsp --no-host
```

The web pages are reported on their own lines, so a filtered port 80 shows as one failed line and nothing else.

Here, on this machine, with its own LAN address in place of `LAN_ADDRESS` (this makes one test account):

```sh
python3 server/tests/live_check.py --host LAN_ADDRESS --http-port 8080 --game swsspsp --no-host
python3 server/tests/e2e_bridge.py --direct LAN_ADDRESS        # the whole flow, with its own throwaway server
```

## 3. Dedicated games

The games on the server's first list ("Select Dedicated Server") are hosted by dedicated PSPC instances, which the
server starts and keeps running (`gamespy-sessions.json` beside the database; `gamespy-sessions.example.json` is an
example). A game hosted from the server's own machine is listed there; the server marks it. **Players may host too, at
most 5 at a time** (*R* on the Join screen; `--max-player-hosts 5`, `MAX_PLAYER_HOSTS` for `install.sh`; at most 2 from
one address; the dedicated games take none of the 5). That is a trade-off the audit explains: the master server cannot yet
check who a heartbeat really comes from.

The retail game has no server mode: `dedicated-host.sh NICK` is a headless PSPC (`--renderer none`, so no GPU, no window) that
signs in as the sessions file's account NICK and creates a game, driven by timed button presses. A round ends, the map is
loaded again and the next one starts the same way. On the playable disc the host also plays - it takes a team and a spawn
point - so the server browser shows `numplayers=1` for a game nobody has joined; on the **dedicated disc** (below) it has no
player and the game lists with 0. The script stops after `HOURS` (6) and the session starts it again 30 seconds later; the
game is back on the list about 4 minutes after a start. Each needs:

- a build of PSPC for this machine, the 4.01 firmware as a profile (`tools/pspc_install_fw.py --profile 4.01`,
  docs/firmware-manager.md), and the SWSS disc image - none of which are in this repository (`private/` and `Games/`
  are not committed). Native code for the game (`tools/pspc_build.py <iso>`, docs/RECOMPILER.md, about 6 minutes) is
  what the dedicated game here runs on; a host without it was not tried;
- a GameSpy account of its own: the `accounts` of the sessions file make it;
- an `infrastructure.json` in its own configuration folder (`XDG_CONFIG_HOME`) that sends `*.gamespy.com` to
  127.0.0.1, so that it talks to the server directly and not through the internet:

```json
{"games": [{"name": "SWSS dedicated", "ids": ["ULUS99390"], "domains": {"*.gamespy.com": "127.0.0.1"}}]}
```

- its own folder for that, its memory stick and its data and cache: `dedicated-host.sh` reads `PSPC`, `ISO` and
  `DEDICATED` (`xdg-config`, `xdg-data`, `xdg-cache` and `ms0` in it) from its environment, and the session's command sets
  them, as the example does. The first start makes the game's profile on the memory stick;
- **a UDP port the router forwards, fixed for each session.** `UDP_PORT` (3658, 3659, 3660 and 3661 for four
  games) makes pspc bind the game's own UDP port (3658, the only one it opens) to that port instead, so it is the
  same at every start; forward each one. Without it, a game that can't have 3658 takes a free port that changes at
  each start; if the fixed port is taken pspc says so and uses another (the game stays up, the forward won't reach
  it). What a game really has open: `ss -lunp | grep pspc` while it runs. What the server saw when it listed:
  `journalctl --user -u swss-server | grep "UDP ports players use"`, and `"udp_ports"` for each game in
  `status.json`. Players are told the server's address (from `swss.pspc.online`) and the heartbeat's port.
- **the map and mode it hosts.** Without more, the script presses square and the game fills the playlist at random, so
  what a session hosts differs at every start (the Conquest sessions are meant to rotate like that). `PLAYLIST` pins it:
  `LOCATION.MODE.ERA.AREA`, comma separated, in the game's own indices. Location is the planet's place among the disc's
  planet codes in sorted order (35 on the disc of 2026-10-24: 0 ALDR, 5 CRSS, 9 ENDR, 10 ENDS, 12 HOTH, 22 MSTR,
  31 VJUN, 34 YAVS); mode counts the planet's allowed game types (0 Conquest, 1 one-flag CTF, 2 two-flag CTF, 3 hero CTF,
  4 Heroes vs Villains); era is 0 prequel, 1 classic, 2 campaign; and area is the place among the areas offered, ground
  first, so 1 is space where the planet offers both (never assume 1: BSPN and CATO prequel offer space alone, area 0).
  For example `PLAYLIST=22.4.0.0` hosts Heroes vs Villains on Mustafar, prequel, ground, and `PLAYLIST=12.0.0.1` hosts
  Conquest on Hoth, prequel, in space. The game keeps its playlist in the profile, so a session with `PLAYLIST` deletes
  the profile and makes a new one (whose playlist is empty) before the run that adds the entries; that costs about two
  minutes at every start, and `HOURS` should be well above it. What a game really advertises: the server browser
  (TCP 28910) shows its `mapname` (`<PLANET CODE> <era>`) and `gametype`; `status.json` shows the same, but only while
  the game heartbeats. `mapname` does not tell ground from space on one planet - both read `Hoth prequel` - so compare
  `levelsettings`: for the same planet, mode and era its low bits are 2 for ground and 4 for space (measured on Hoth
  prequel Conquest, `12.0.0.0` and `12.0.0.1`).

**A host without a player.** The dedicated disc is the playable one with another `PSP_GAME/SYSDIR/EBOOT.BIN`: the session
start makes the host's local players 0 (the engine already runs dedicated servers; the menus never set one up) and sets
bit 30 of `levelsettings`, which is how the server browser's dedicated list picks a game. The executable is the same size
(27 bytes differ) and is not in this repository; it comes from the machine that has the SWSS build, with its sha256.
`swss-dedicated-iso.py` makes the image from the playable disc in seconds, rewriting only the executable's sectors and
checking both hashes (the playable disc's executable is `efdb4423055514fcf7808c31ac036b394331e22cb71b1886f535e19c6d4f1471`,
the dedicated one `155a7a7fc84da335f38cf01d578e7aa258ae01480685d006cb3dbaf9d7f61fcf`):

```sh
python3 server/deploy/swss/swss-dedicated-iso.py Games/SWSS.iso Games/SWSS-dedicated.iso EBOOT.BIN \
  --expect-old efdb4423055514fcf7808c31ac036b394331e22cb71b1886f535e19c6d4f1471 \
  --expect-new 155a7a7fc84da335f38cf01d578e7aa258ae01480685d006cb3dbaf9d7f61fcf
tools/pspc_build.py Games/SWSS-dedicated.iso      # its executable changed, so it needs its own native code
```

A session that `dedicated-host.sh` starts runs with its own `XDG_CACHE_HOME` (`$DEDICATED/xdg-cache`), so it reads the AOT
cache in `$DEDICATED/xdg-cache/pspc/aot`, not the one beside the build: build into that cache with `--cache <that folder>`
(and `--build-dir` of the tree the pspc binary came from). Each session has its own, so the `.so`, its `.semver` and the
`games.json` entry have to be in every one; copying them over after one build is enough. On a machine without `clang++`,
`--compiler gcc` is needed (the build takes about 7 minutes for this game). The playable disc's entry stays in `games.json`,
so pointing `ISO=` back is all a rollback needs.

Then set `ISO=` of the sessions that should host without a player to `Games/SWSS-dedicated.iso` and restart the server.
The presses are the same as for the playable disc (the dedicated disc's menus are the retail ones; the game never shows
its `DED_*` screens). The image is for the sessions only: **players keep the playable disc.** The check is the server
browser (TCP 28910): `numplayers=0`, and the `mapname`/`gametype` the session pins.

**A new disc.** Stop the server first: a running game keeps the old image open. Replace `$ISO`, then check whether the
native code is still valid. It is keyed by the SHA-1 of the disc's `EBOOT.BIN` as a plain ELF (the `sha1` of `modules` in
the AOT cache's `games.json`; `pspc-iso modules <iso> <dir>` and `pspc-decrypt prx` produce it). While that hash is the
same, only `size` in `games.json` needs updating; if it changed, rebuild (`tools/pspc_build.py "$ISO"`, about 6 minutes).
The disc of 2026-10-24 has the same executable as the one before it, so only its data changed. Players need this same
image: 2333759488 bytes, sha256 `fe4913f2100e581f781a3e7d817efe0c712c94e7666c3f2366716c852d4e8a80`.

Edit the sessions file, then `systemctl --user restart swss-server`. The sessions' output is in
`~/swss-server/data/sessions/<name>.log`; `status.json` lists them.

## 4. Looking after it

```sh
~/swss-server/bin/swss-admin accounts            # ban NAME | unban NAME | passwd NAME | games | stats
sqlite3 ~/swss-server/data/gamespy.sqlite ".backup '/safe/place/gamespy.sqlite'"     # back it up
ss -ltnup                                        # what listens: the ports of the table, and ssh
```

Back up `gamespy.sqlite` (accounts, friends, careers). Keep it private: it holds e-mail addresses and the
passwords' MD5s, which are all that GameSpy's login needs. Logs hold players' addresses; keep them as long as
you need. If a player is attacking the server, `ban` them and block their address at the router.

**If it doesn't work.** `journalctl --user -u swss-server` first. "can't look up the public host": the record
isn't there yet. A player's game can't connect: the record is orange-cloud, a port isn't forwarded (UDP and
TCP are separate rules), or the router has no hairpin NAT and the player is inside your network. Games
are listed but can't be joined: the dedicated game's UDP port isn't forwarded.

## 5. Testing with real clients on other networks

What a client PC needs: PSPC, its firmware, the SWSS disc (ULUS99390, the same build as the dedicated servers: 2333759488
bytes, sha256 `fe4913f2100e581f781a3e7d817efe0c712c94e7666c3f2366716c852d4e8a80`), and
this next to `pspc.exe` (Linux: `~/.config/pspc/infrastructure.json`), unless its PSPC already ships the entry:

```json
{"games": [{"name": "Star Wars Super Squadron", "ids": ["ULUS99390"], "domains": {"*.gamespy.com": "swss.pspc.online"}}]}
```

In the game: *Multiplayer > Infrastructure*, the connection "PSPC 1", *Create New GameSpy ID* (once), log in, then *Friend List*,
*Player Rankings* and *Join* (*R* switches to player-hosted servers). Games are listed while a dedicated server or one of at most 5
players is hosting. A player's game that finds the room taken is not listed, and the log says so ("not listed"); a player behind a
router needs the game's UDP port (3658) forwarded to their PC, or NAT negotiation to get through.

**Before the game, on each network** (no disc needed): unzip a copy of `server/` and run
`python3 tests/live_check.py --host swss.pspc.online --no-host --game swsspsp` (Windows: PSPC's `python\python.exe`). It makes
one `Smoke...` account and tells which step fails: DNS, TCP 29900/29901/28910/29920, UDP 27900/27901, the web pages.

**What the server says.** At info level it logs logins (and why one was refused), accounts, who asked for the list and how many
games it held, who hosted, every NAT negotiation (who joined, who was told to connect to whom, who never came), stats sign-ins
(and why one was refused) and web requests. While testing, record it and the kernel's counters:

```sh
python3 ~/swss-server-captures/capture.py          # server.log + counters.log under ~/swss-server-captures/<time>/ (keeps running)
python3 server/deploy/swss/session_report.py       # per client address: the events in order, the stages reached, and what looks wrong
python3 server/deploy/swss/session_report.py --since "-30min" --ip 203.0.113.9
```

`capture.py` is not part of the repository (it is a few lines around `journalctl -f`); the report works from the journal alone.
The logs hold players' addresses and e-mail addresses: delete captures when the diagnosis is done.

## Instead: the Cloudflare Tunnel (no open ports)

```sh
MODE=tunnel server/deploy/swss/install.sh
```

The server then listens on 127.0.0.1 only and players reach it through the tunnel, with a relay that PSPC
starts for them (the shipped redirect must then be the tunnel one: see
[the docs](../../../docs/gamespy-server.md#without-open-ports-the-cloudflare-tunnel)). The domain is a
*proxied* record made by the tunnel. In the Zero Trust dashboard:

1. *Networks > Tunnels > Create a tunnel > Cloudflared*, name it `swss`. Copy the **token** only.
2. *Public Hostname*: subdomain `swss`, domain `pspc.online`, service type **HTTP**, URL **`127.0.0.1:8080`**.
3. Put `cloudflared` and the token here:

```sh
cd ~/swss-server/bin
curl -L -o cloudflared https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64
chmod 755 cloudflared
sha256sum cloudflared                  # compare with the release's notes on github.com/cloudflare/cloudflared/releases
umask 077; echo 'TUNNEL_TOKEN=<the token>' > ~/swss-server/cloudflared/token.env
```

   then run `MODE=tunnel server/deploy/swss/install.sh` again: it starts `swss-cloudflared`.
4. For the hostname: *WebSockets* **On** (the default); **no** Cloudflare Access; **Browser Integrity Check**
   and **Bot Fight Mode** off or skipped by a WAF rule (the relay is not a browser). Add a rate limiting
   rule for `/tunnel/` if you see abuse.

Check: `curl https://swss.pspc.online/tunnel/info`; and as a player's PC does:
`python3 -m pspc_gamespy tunnel https://swss.pspc.online` (from `server/`), then
`python3 server/tests/live_check.py --host 127.77.0.1 --no-host --web-base https://swss.pspc.online`.
Tunnel mode's limits (no hosting by players, the game's UDP port 3658, port 80 on Linux) are in the docs.
