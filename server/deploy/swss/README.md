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
| UDP | **3658** | **3658** | the first dedicated game's matches; any other dedicated game: the port the server prints (see 3) |

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

The games on the server are hosted by dedicated PSPC instances, which the server starts and keeps running
(`gamespy-sessions.json` beside the database; `gamespy-sessions.example.json` is an example). **Only the
server's own machine may list games** (`--local-hosts-only`: players cannot host on this server). There is
no GPU here, so each runs `--headless --renderer none`: the game's display lists run for their timing and
nothing is drawn. Each needs:

- a build of PSPC for this machine, its firmware, and the dedicated disc images - none of which are in this
  repository (`private/` and `Games/` are not committed);
- a GameSpy account of its own: the `accounts` of the sessions file make it;
- an `infrastructure.json` in its own configuration folder (`XDG_CONFIG_HOME`) that sends `*.gamespy.com` to
  127.0.0.1, so that it talks to the server directly and not through the internet:

```json
{"games": [{"name": "SWSS dedicated", "ids": ["ULUS99390"], "domains": {"*.gamespy.com": "127.0.0.1"}}]}
```

- its own memory stick folder (`--memstick`), and a user of its own if you can;
- **a UDP port the router forwards.** The first dedicated game binds the game's 3658 when it is free; PSPC gives
  any other a free port, which changes at each start, so start with one dedicated game and forward UDP 3658. What
  a game really has open: `ss -lunp | grep pspc` while it runs. What the server saw when it listed: `journalctl
  --user -u swss-server | grep "UDP ports players use"`, and `"udp_ports"` for each game in `status.json`. Forward
  all of them. Players are told the server's address (from `swss.pspc.online`) and the heartbeat's port.

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

What a client PC needs: PSPC, its firmware, the SWSS disc (ULUS99390, the same build as the dedicated servers), and
this next to `pspc.exe` (Linux: `~/.config/pspc/infrastructure.json`), unless its PSPC already ships the entry:

```json
{"games": [{"name": "Star Wars Super Squadron", "ids": ["ULUS99390"], "domains": {"*.gamespy.com": "swss.pspc.online"}}]}
```

In the game: *Multiplayer > Infrastructure*, the connection "PSPC 1", *Create New GameSpy ID* (once), log in, then *Friend List*,
*Player Rankings* and *Join* (*R* switches to player-hosted servers). Games are only listed while a dedicated server (or an
allowed host) is running; `--local-hosts-only` drops everyone else's heartbeats, and says so in the log.

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
