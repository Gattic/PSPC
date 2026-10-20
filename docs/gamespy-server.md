# GameSpy server (Star Wars Battlefront: Elite Squadron, Star Wars Super Squadron)

`server/pspc_gamespy` replaces GameSpy's servers, which closed in 2014, for Star Wars Battlefront:
Elite Squadron (ULUS10390) and its mod Star Wars Super Squadron (SWSS, ULUS99390), so their online
mode works again: GameSpy accounts, logging in, friends
and messages, hosting games and finding and joining them, the stats server's player records, and
the leaderboards. It speaks the GameSpy SDK's protocols as the game uses them: Presence (accounts,
login, buddies), Query & Reporting 2 and the server browser (hosted games), NAT negotiation, the
stats and persistent storage server, and the HTTP pages (message of the day, leaderboards). Python
3.9 or later, standard library only; one SQLite database file.

**State.** PSPC instances on one PC create accounts in the game and log in; one hosts a match that
another finds under *Player-Hosted Servers* and joins (the host's heartbeats then list both
players); players find each other by nickname and become friends (the request reaches the other
player at once or at their next login, and accepting puts each on the other's list); *Player
Rankings* shows the leaderboards; the stats server signs players in and keeps their records. Play
across the internet hasn't been tried yet; the public SWSS server below runs on the internet, and
[its security audit](gamespy-server-security.md) is the reason for its limits.

## From the launcher

The launcher's *Online* page runs this server: *Start* on the GameSpy card starts it and points
your game at it. *Change...* points the game at another server, at OpenSpy (the public GameSpy
revival), or nowhere; *Manage...* sets the address players on other PCs reach it at and lists the
players (ban, unban, set a new password), their career totals and the last games; *Log* shows what
it does. The card shows how many players are online and how many games are hosted. Its database
and log are in the `servers` folder next to pspc.exe. Friends on other PCs choose *Change...* and
enter your address.

## Quick start (one PC, command line)

```sh
cd server
python -m pspc_gamespy serve
```

Then point the game's GameSpy names at it: create `infrastructure.json` next to pspc.exe (the
launcher writes the same):

```json
{"games": [{"name": "Elite Squadron (local GameSpy)", "ids": ["ULUS10390"],
            "domains": {"*.gamespy.com": "127.0.0.1"}}]}
```

`*.gamespy.com` covers every GameSpy name the game looks up (gpcm, gpsp, swbfespsp.master,
swbfespsp.ms0, natneg1-3, swbfespsp.sdkdev, motd, ...). Start the game, choose *Multiplayer >
Infrastructure* and the connection "PSPC 1". At the *Login* screen choose *Create New GameSpy ID*
(nickname, e-mail address, password); after that, *Login Using Existing GameSpy ID*. The
*Infrastructure* menu then offers *Join*, *Create*, *Friend List* and *Player Rankings*.

- **Hosting:** *Create*, a server name, then the playlist (location, mode, era, ground or space for
  each entry) and *Launch* (START).
- **Joining:** *Join* opens the dedicated servers' list; R switches to *Player-Hosted Servers*,
  which is where games hosted from a PSP (or PSPC) appear. L hides full games, Square refreshes.

To try it alone, start a second PSPC instance with another memory stick folder (`--memstick
<folder>`): each instance gets its own console identity and, when the game's port is taken, its own
UDP port, which the server sees and passes on.

**OpenSpy instead.** OpenSpy (openspy.net) runs GameSpy's services for many games under the same
names in its own domain. To play there, redirect `"*.gamespy.com": "*.openspy.net"` (a target
starting with `*.` keeps the part of the name the wildcard matched: `gpcm.gamespy.com` goes to
`gpcm.openspy.net`). Whether it has Elite Squadron set up is up to OpenSpy.

## Running a server for others

```sh
python -m pspc_gamespy --db gamespy.sqlite serve --public-host 203.0.113.7
```

| Option | |
|---|---|
| `--db <file>` (before `serve`) | database file (default `pspc_gamespy.sqlite`) |
| `--public-host <address or name>` | what players on other PCs reach this server at. Games hosted on the server's own PC are seen at 127.0.0.1; players elsewhere are given this address for them instead (default `127.0.0.1`; `127.77.0.1` with `--tunnel`). A name is looked up at start and every five minutes |
| `--bind <address>` | address to listen on (default `0.0.0.0`) |
| `--http-port <n>` | the web pages' port (default 80; the game asks port 80, so change it only behind a proxy) |
| `--motd <text>` | the message of the day (the game fetches it but doesn't show it) |
| `--status-file <file>` | keep a JSON summary of who is online and the hosted games there (the launcher reads it) |
| `--log debug` (before `serve`) | log every message (large) |
| `--games <file>` (before `serve`) | more games, from a games file (default: `gamespy-games.json` beside the database, if there is one; see below) |
| `--sessions <file>` | programs to run alongside, such as dedicated game servers (default: `gamespy-sessions.json` beside the database, if there is one; see below) |
| `--local-hosts-only` | list only the games hosted on this machine (its dedicated servers): for a server on the internet, where UDP heartbeats can be forged. Needs `--public-host` |
| `--tunnel` | let players reach the server over WebSockets on the web port, for a Cloudflare Tunnel (see [Public server](#public-server)) |
| `--behind-proxy` | the web port's requests come through a proxy on this machine (cloudflared): take players' addresses from its `CF-Connecting-IP` / `X-Forwarded-For` header |

Players redirect `*.gamespy.com` to the server's address, as above. **For the internet** see
[Public server](#public-server): the server needs `--local-hosts-only` (or the tunnel), or anyone can list
games that don't exist ([audit](gamespy-server-security.md)).

### More games

Elite Squadron and Super Squadron are built in. A game that isn't is added with a games file: `gamespy-games.json` beside the database
(for the launcher's server, in its `servers` folder) or one named with `--games`. A mod or another
release of a known game names that game with `like` and gets its secret key, product ID, namespace,
stored record and leaderboard pages unless the entry gives its own; any other game needs
`secret_key` and `product_id` (and gets no leaderboard pages).

```json
[{"gamename": "mymodpsp", "name": "My Elite Squadron mod", "like": "swbfespsp", "disc_ids": ["ULUS99999"]}]
```

Each game has its own hosted games (a game's server list holds only its hosts), careers and
leaderboards; accounts are shared, as GameSpy's were. The executable has to call itself by that
game name (it is in its GameSpy code, beside the secret key), and its players need a redirect for
its disc ID: add the ID to the game's entry in `infrastructure.json`, or an entry of its own.

### Sessions

Programs the server runs while it runs - dedicated game servers that host games on it, say - come
from a sessions file: `gamespy-sessions.json` beside the database (for the launcher's server, in
its `servers` folder) or one named with `--sessions`.

```json
{"accounts": [{"nick": "Ded-Conquest", "email": "ded@example.com", "password": "...", "namespace": 1}],
 "sessions": [{"name": "Conquest", "command": ["python", "host.py", "conquest"], "cwd": "...", "restart_after": 30}]}
```

The accounts are GameSpy accounts for the sessions to log in with: made in the database when they
are missing (unique nick = nick), their password set to the file's. Each session's command starts
once the server is listening, starts again `restart_after` seconds (default 30) after it ends, and
ends with the server together with its child processes, even when the server is killed (as the
launcher stops it): on Windows they run in a job object that ends them when the server's handle to
it closes, on Linux with a parent-death signal. A session's output goes to `sessions/<name>.log`
beside the database; the status file lists the sessions (running, starts).

### Ports

| Port | | |
|---|---|---|
| 29900 TCP | Presence (gpcm) | accounts, login, status, buddies and their messages |
| 29901 TCP | Presence search (gpsp) | checks and searches without a login: e-mail and nick checks, profile search |
| 27900 UDP | master server (QR2) | hosts register their games and keep them up to date |
| 28910 TCP | server browser | the lists of hosted games, their details, messages to hosts |
| 27901 UDP | NAT negotiation | introduces two players behind routers to each other |
| 29920 TCP | stats | players' persistent records, game reports |
| 80 TCP | web | message of the day, leaderboards (the server runs without it if the port is taken; only the leaderboards go) |

### Hosting on the internet

See [Public server](#public-server): do not forward the ports of a server that anyone may host on.

## Public server

The public Star Wars Super Squadron server is **swss.pspc.online**. There are two ways to put a server
on the internet, and the first is the one to use when the router can forward ports:

| | Forwarded ports (recommended) | Cloudflare Tunnel |
|---|---|---|
| Players need | nothing: PSPC's shipped redirect names the domain | the relay, which PSPC starts (Python, `relay.py`) |
| The domain is | a **DNS-only** (grey cloud) record for the router's public address | a proxied record for the tunnel |
| Ports open on the router | yes (below) | none |
| Delay, NAT negotiation | the games' own: direct UDP, as GameSpy did | a WebSocket hop each way; the game must not use UDP port 3658 (below) |
| Players' game traffic reaches | the server, from their own address | the server, from an address the gateway gives them |
| Web port 80 | forwarded like the others (the router can map 80 to 8080) | through the relay: needs a privileged port on Linux |

Both are verified end to end with separate processes (`server/tests/e2e_bridge.py`, below).

### With forwarded ports

```
game --TCP/UDP--> swss.pspc.online (the router's public address) --forwarded--> this server
```

1. **DNS.** A record `swss` in `pspc.online` pointing at the router's public address, **DNS only**
   (Cloudflare's grey cloud). A proxied (orange) record answers with Cloudflare's addresses, which carry
   web pages and nothing else: every other port would time out and the server would tell players to join
   games there. If the public address changes, use dynamic DNS (the server looks the name up again every five
   minutes). Check it (on the server's machine; it also says what to fix):
   `python server/tests/live_check.py --host swss.pspc.online --dns-only --here`.
2. **The router** forwards to this machine (give it a fixed LAN address):

   | Protocol | Outside | Inside | For |
   |---|---|---|---|
   | TCP | 29900 | 29900 | Presence: accounts, login, friends |
   | TCP | 29901 | 29901 | Presence search |
   | TCP | 28910 | 28910 | the server browser |
   | TCP | 29920 | 29920 | stats |
   | TCP | 80 | 8080 | the web pages (message of the day, Player Rankings) |
   | UDP | 27900 | 27900 | the master server (games ask it whether the service is up) |
   | UDP | 27901 | 27901 | NAT negotiation |
   | UDP | the **game port** of each dedicated game | the same | the matches themselves |

   **The UDP port(s) of each dedicated game** are the ones its process has open: ask the machine, not the
   router: `ss -lunp | grep pspc` while it runs. PSPC binds the port the game asks for (3658 is the game's
   default) and takes another free one when that is taken (`UDP port 3658 is in use on this PC: using N instead`
   in its log), so the first dedicated game is the simple case: forward UDP 3658 and check it against `ss`. The
   server also reports what it observes when a game is listed: `qr: swsspsp hosts ... UDP ports players use: 3658`
   in its log and `"udp_ports"` for each game in `status.json`: the port the heartbeats come from (what the server
   list gives players) and the game's own `localport`. Forward all of them. Players on the same network as the
   server need *NAT loopback* (hairpin) on the router to use the public address.

   **If port 80 hangs, look at the rule's direction first.** The game asks for the web pages on port 80, which the
   server answers on 8080: the rule is *external 80 -> internal 8080*. Entered the other way round (external 8080 ->
   internal 80) it forwards nothing useful, and leaves external 80 with no rule: routers drop 80 and 443 on their
   public address (their own admin pages), so that **hangs** while an unmapped port such as 81 is refused at once.
   You can tell the cases apart from a PC on the LAN by connecting to the router's public address (a router
   forwards LAN traffic to its own public address as well, which makes this a test of the rule table):

   | Connecting to `swss.pspc.online:PORT` from the LAN | Means |
   |---|---|
   | connects, and the web page answers | the rule works |
   | refused at once, nothing on the server's machine noticed | no rule for that port (the router refused) |
   | refused at once, and the server's machine sent a reset (`OutRsts` in `/proc/net/snmp` goes up) | a rule delivers to **this machine, to a port nothing listens on**: the internal port is wrong |
   | hangs | no rule, and the router drops it on its public address |

   That is what was seen here: external 8080 reached this machine but on none of its unprivileged ports (so, by
   elimination, on 80, which cannot be listened on without privileges), and external 80 and 443 hung. The fix is
   in the router: delete the 8080 -> 80 rule and add TCP **external 80 -> the server's LAN address, internal 8080**. If 80
   still hangs with that rule in place, look for *Remote Management* / *Remote Access* (turn it off); if it then
   connects from the LAN but not from outside, the ISP filters inbound 80, which can only be seen at that point.
   Nothing needed to find or join a game uses 80: only the message of the day (the game ignores it) and *Player
   Rankings*. A game's web request to a port that hangs waits for its connect timeout (30 s in PSPC); a client can
   avoid that by redirecting `motd.gamespy.com` to `127.0.0.1` in its own `infrastructure.json` (an exact name beats
   `*.gamespy.com`).
3. **The server**, as a user without privileges:

```sh
python -m pspc_gamespy --db gamespy.sqlite serve --bind 0.0.0.0 --public-host swss.pspc.online \
    --local-hosts-only --http-port 8080
```

   `--public-host` is what players are told for the games on this machine: a name or an address (a name
   is looked up at start, which fails if it has no address, and every five minutes). `--local-hosts-only`
   lists only the games hosted on this machine: UDP heartbeats can be sent from any address,
   including forged ones, and would otherwise fill the list with games that don't exist or point
   players at a victim. The dedicated servers run here and talk to the server on 127.0.0.1, so they are unaffected.
   `server/deploy/swss/install.sh` does this as a user service.
4. **PSPC** (a build with this change) needs nothing: `data/infrastructure.json` has an entry for Super
   Squadron that sends `*.gamespy.com` to `swss.pspc.online`, which PSPC looks up when the game connects.
   An existing PSPC takes the same entry from an `infrastructure.json` next to pspc.exe (Linux:
   `~/.config/pspc/infrastructure.json`):

```json
{"games": [{"name": "Star Wars Super Squadron", "ids": ["ULUS99390"], "domains": {"*.gamespy.com": "swss.pspc.online"}}]}
```

   Everyone who plays needs the same SWSS disc build as the dedicated servers.

A server on your own PC overrides it: an `infrastructure.json` next to pspc.exe (Linux:
`~/.config/pspc/`) with an entry for the game replaces the shipped one. The launcher's *Online* page
writes one when you start the GameSpy server there, for Elite Squadron and Super Squadron both, and
*Change...* points them at another server or at OpenSpy. Remove the entry (choose *None*, or delete it
from the file) and the game is on the public server again.

### Without open ports: the Cloudflare Tunnel

A Cloudflare Tunnel carries HTTP(S) only, so the games' TCP and UDP go over WebSockets: a relay on each
player's PC and a gateway on the server's web port, joined through Cloudflare:

```
game --TCP/UDP--> relay (127.77.0.1, on the player's PC) --wss--> Cloudflare --> cloudflared
   --http--> gateway (the server's web port, /tunnel/) --TCP/UDP--> the services, on 127.0.0.1
```

Use it when ports can't be forwarded, or the server's address must stay unpublished. The shipped
entry then has `"tunnel": "https://swss.pspc.online"` and `"domains": {"*.gamespy.com": "127.77.0.1"}`:
when the game connects to the network PSPC starts the relay (`server/pspc_gamespy/relay.py`; it needs
Python 3.9 or later: `PSPC_PYTHON` if set, else the one an installed PSPC brings, else `python3` or `python`
on the PATH; output in `servers/relay.log`), which ends with PSPC. The server runs with `serve --bind
127.0.0.1 --tunnel --behind-proxy` and cloudflared sends the hostname to its web port
([server/deploy/swss/README.md](../server/deploy/swss/README.md); `MODE=tunnel install.sh`).
To use the relay by hand: `python -m pspc_gamespy tunnel https://swss.pspc.online` (from `server/`).

The relay opens one WebSocket per TCP connection of the game and one per UDP socket, which the
gateway meets with a real connection or socket on the server, from an address of its own for each player
(127.64.x.y, one per real address: the services see one player per address, as on the internet), and
`public_host` for the games on the server's machine is 127.77.0.1, where the relay is. What the tunnel can't do:

- **Players can't host games through it.** Nobody could join them: there is no way from one
  player's PC to another's. Games are hosted by dedicated servers on the server's machine (below),
  which can list games; heartbeats from players are ignored. (Players on one LAN still host
  for each other on a server of their own.)
- **A host can answer a player, or write to the address the player's UDP socket has on the server**
  (as through a NAT that keeps its mappings), **but can't start with a player whose game hasn't sent
  anything yet.** NAT negotiation between a player and a dedicated game works (checked with
  `e2e_bridge.py`); with the real game it hasn't been tried.
- **A dedicated game must not use UDP port 3658**, the game's default: the player's own game binds
  3658 for itself, and then the relay can't listen on the same port for the dedicated game's (on Linux
  the second bind fails whichever comes first, checked; Windows binds differently). Give the
  dedicated games other ports, or have a program on the server hold 3658 so that they pick others.
- **Port 80**: on Linux the relay can't listen on it (below 1024) unless allowed
  (`sysctl net.ipv4.ip_unprivileged_port_start=80`): the *Player Rankings* page, which the game asks for
  there, isn't reachable until then. Everything else is. (A Cloudflare Tunnel can't carry the page to the
  game on its own either: the game sends `Host: sdkdev.gamespy.com`, which Cloudflare refuses, error 1001,
  since that name isn't in your zone.)
- A relay that is half-closed by the game (the game stops sending but waits for the answer) closes
  both ways: the games' protocols don't do that.
- Round-trip delay is the web's, and a lost packet holds the ones behind it up (TCP underneath).

### Checking it

```sh
python server/tests/e2e_bridge.py                     # separate processes: server, relay, a dedicated game, players
python server/tests/e2e_bridge.py --direct 192.0.2.5  # no relay: the server on one of this machine's addresses
python server/tests/live_check.py --host swss.pspc.online --http-port 80 --no-host     # from outside, once it is up
```

### Dedicated servers on a machine without a GPU

The games on the public server are hosted by dedicated servers: PSPC instances that run the game
headless and host a Conquest match (or another mode) on it, as [Sessions](#sessions) describes.
PSPC's `--renderer none` is for them: display lists run (their timing, which the game depends on)
but nothing is drawn, so no GPU, no OpenGL and no window are needed (a modded Elite Squadron's
64-player match: 70 s of CPU for the simulated time that D3D11 needed 148 s for, and the software
renderer 1093). Each needs its own GameSpy account (made by the sessions file), a memory stick
folder, and an `infrastructure.json` of its own, in its own configuration folder
(`XDG_CONFIG_HOME`), that sends `*.gamespy.com` to 127.0.0.1: it talks to the server directly,
not through the tunnel. `server/deploy/swss/gamespy-sessions.example.json` is an example. Each
dedicated game is one UDP port: the first takes the game's 3658, the others ones the system chooses (PSPC
falls back to a free port when 3658 is taken, and tells the server in its heartbeat). With forwarded ports
the router has to forward them, so start with one dedicated game and forward the ports it uses (above);
through the tunnel the gateway follows whichever ports have a game listed.

## Leaderboards and stats

*Player Rankings* has four boards: Overall Score, Kill:Death Ratio, Win:Loss Ratio and Points Won,
each with the players' rank and time online, five to a page, and the player's own standing. The
game itself uploads no career stats (the stats server only keeps its GameSpy message inbox), so the
server counts careers from what hosts report while they play (`careers.py`):

- the host's heartbeats list each player (their GameSpy unique nick) with their score, deaths and
  team; time online is how long a player was in them;
- at the end of a round the host's stats connection reports which team won;
- a round also ends when the hosted game goes away, or when the scores start again from nothing;
  those count no win or loss.

The game reports no separate kill count, so kills (and the kill:death ratio) use its score. Overall
Score is points won + 50 per win + 10 per kill; the rank goes up at 500, 1500, 3000, ... (500 times
the triangular numbers) to 99. Players are ranked once they have finished a round. The
leaderboard request and its reply format are in `leaderboards.py`.

## Administration

The same command manages the database; it's safe to use while the server runs.

```sh
python -m pspc_gamespy --db gamespy.sqlite accounts            # GameSpy profiles (add --json for JSON)
python -m pspc_gamespy --db gamespy.sqlite ban Some_Cheater    # by nick or unique nick; unban
python -m pspc_gamespy --db gamespy.sqlite passwd Someone      # by nick, unique nick or e-mail (--password-stdin)
python -m pspc_gamespy --db gamespy.sqlite stats               # career totals
python -m pspc_gamespy --db gamespy.sqlite games               # the last reported games
```

A banned player can't log in, and leaves the leaderboards. The database holds accounts (e-mail and
the password's MD5) and their profiles, buddies, blocks and offline messages, the stats server's
records, reported games and career totals. Back it up.

## How it works

| Module | |
|---|---|
| `gp.py` | Presence: the connection manager (gpcm) and the search manager (gpsp) |
| `qr.py` | the master server (QR2) and the registry of hosted games |
| `sb.py` | the server browser |
| `natneg.py` | NAT negotiation |
| `gstats.py` | the stats and persistent storage server |
| `web.py`, `leaderboards.py` | the HTTP pages |
| `careers.py` | career totals from hosts' reports |
| `crypt.py` | GameSpy's base64 and password encoding, login proofs, the stats XOR, the server list cipher |
| `kv.py`, `sqlfilter.py` | `\key\value\` messages; the server browser's filters |
| `games.py` | the games: name, secret key, disc IDs; games files |
| `access.py` | who may ask how much: connection limits, rate limits, timeouts; which addresses are the server's own machine and which are tunnelled players |
| `ws.py`, `tunnel.py`, `relay.py` | the public server's tunnel: WebSockets, the server's gateway, the player's relay |
| `sessions.py` | programs run alongside the server (dedicated game servers) |
| `store.py` | the database |

The game's GameSpy name is `swbfespsp` and its secret key `wLfbMH` (the game builds it byte by byte
in its code; it also appears in GameSpy's key list). The protocols are written from what the game
sends and expects, with OpenSpy (github.com/openspy/openspy-core) as a reference for their formats;
no code is taken from it.

**Presence.** The server sends a challenge; the client logs in with `nick@email`, a unique nick or
a token, its own challenge and MD5(MD5(password) + 48 spaces + user + client challenge + server
challenge + MD5(password)); the server proves it knows the password the same way with the
challenges swapped. Elite Squadron creates accounts with `\newuser\` (nickname, e-mail, password
encoded with GameSpy's XOR stream and base64, unique nick = nickname, namespace 1) and logs in by
unique nick. Buddy requests, authorizations, status and messages follow GP's `\bm\` messages;
messages for players who are offline wait for their next login. When a player accepts a request,
the game asks back so that each is on the other's list; the server grants that at once, since the
other player asked first.

**Hosted games.** A host's heartbeats carry its keys (hostname, mapname, gametype, numplayers,
maxplayers, gamemode, natneg, localip0/localport, levelsettings, ...) and a player and a team
section; a new host is challenged and listed once it answers. The server browser's list request
names the keys the game wants and a filter (Elite Squadron: `gamever = 10901 and levelsettings >=
1073741824` for dedicated servers, `< 1073741824` for player-hosted ones); the reply is encrypted
with the game's secret key. When the joining player and the host are seen at the same address (one
PC, or one network behind a router), the game connects to the host's private address from the list.

**Stats.** The stats connection is XORed with "GameSpy3D". The game authenticates with
MD5(a number folded from the server's challenge + the secret key), signs the player in by profile
ID, and reads and writes one record (private, index 206628862): its inbox of GameSpy messages
(version 0, a count, then sender, date, nick and text per message). A player who never saved gets
an empty inbox. Hosts report each round's end (`\updgame\`: map, mode, which team won).

## Tests

```sh
python -m unittest discover -s server/tests       # from the repository root
```

`test_gamespy_security.py` and `test_tunnel.py` cover the limits and the tunnel ([audit](gamespy-server-security.md)).
`live_check.py` drives a running server the way a game does. `test_gamespy.py` covers the encodings and ciphers (the server list's against vectors from the
GameSpy SDK's C implementation), filters and heartbeats, and runs the whole server on spare ports:
an account created and logged in, a game hosted and found, pushed updates and full details, a
message forwarded to the host, NAT negotiation between two players, the stats server's sign-in and
records, a round counted into careers and the leaderboard pages; games from a games file (their
own server list, their leaderboard pages, bad entries refused); sessions (their accounts made and
their passwords kept up to date, a session started again when it ends, sessions stopped with the
server, and on Windows ended when the server process is killed).

## Limits

- Elite Squadron and Super Squadron are the only games built in. Others come from a games file
  (above); one that plays unlike Elite Squadron needs whatever else it uses (its leaderboards, its
  stats records).
- NAT negotiation is covered by the tests, not by a match: on one PC and on one network the game
  connects to the host's private address directly.
- The leaderboards come from hosts' heartbeats; a host that leaves before reporting a round still
  counts its players' numbers, without a win or loss.
- In headless test runs, two instances with `--fast-time` lose the connection while joining (each
  runs its own fast clock); in real time they don't.
