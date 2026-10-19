# GameSpy server (Star Wars Battlefront: Elite Squadron)

`server/pspc_gamespy` replaces GameSpy's servers, which closed in 2014, for Star Wars Battlefront:
Elite Squadron (ULUS10390), so its online mode works again: GameSpy accounts, logging in, friends
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
across the internet hasn't been tried yet.

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
| `--public-host <address>` | the address players on other PCs reach this server at. Games hosted on the server's own PC are seen at 127.0.0.1; players elsewhere are given this address for them instead (default `127.0.0.1`) |
| `--bind <address>` | address to listen on (default `0.0.0.0`) |
| `--http-port <n>` | the web pages' port (default 80; the game asks port 80, so change it only behind a proxy) |
| `--motd <text>` | the message of the day (the game fetches it but doesn't show it) |
| `--status-file <file>` | keep a JSON summary of who is online and the hosted games there (the launcher reads it) |
| `--log debug` (before `serve`) | log every message (large) |
| `--games <file>` (before `serve`) | more games, from a games file (default: `gamespy-games.json` beside the database, if there is one; see below) |

Players redirect `*.gamespy.com` to the server's address, as above.

### More games

A game that isn't built in is added with a games file: `gamespy-games.json` beside the database
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

Forward the ports above to the server and give `--public-host` the address players use. Matches
themselves go directly between the players' games (UDP, the game's port 3658 by default): the host
needs that port reachable, or NAT negotiation (which the game asks for) has to get the players
through their routers. This hasn't been tried across the internet yet.

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

`test_gamespy.py` covers the encodings and ciphers (the server list's against vectors from the
GameSpy SDK's C implementation), filters and heartbeats, and runs the whole server on spare ports:
an account created and logged in, a game hosted and found, pushed updates and full details, a
message forwarded to the host, NAT negotiation between two players, the stats server's sign-in and
records, a round counted into careers and the leaderboard pages; games from a games file (their
own server list, their leaderboard pages, bad entries refused).

## Limits

- Elite Squadron is the only game built in. Others come from a games file (above); one that
  plays unlike Elite Squadron needs whatever else it uses (its leaderboards, its stats records).
- NAT negotiation is covered by the tests, not by a match: on one PC and on one network the game
  connects to the host's private address directly.
- The leaderboards come from hosts' heartbeats; a host that leaves before reporting a round still
  counts its players' numbers, without a win or loss.
- In headless test runs, two instances with `--fast-time` lose the connection while joining (each
  runs its own fast clock); in real time they don't.
