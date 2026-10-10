# Syphon Filter: Combat Ops server

`server/pspc_medius` is a replacement for Sony's servers for Syphon Filter: Combat Ops (NPUG80114),
so the game's online mode works again: sign-in, the lobby and chat, hosting and joining games,
the matches themselves, shared missions (the Mission Modifier's uploads, ratings, favorites, the
official mission list), cells (clans), buddies and stats. It speaks the protocols the game was
built for: SCE-RT/Medius (lobby and game servers) and SVO (the game's web portal, including its
SSL 3.0 login page). Python 3.9 or later, standard library only; one SQLite database file.

**State.** Two PSPC instances on one PC sign in, meet in the lobby, host and join a game, download
the mission, and play a match together through it. Play across the internet hasn't been tried yet
(see [Hosting on the internet](#hosting-on-the-internet)).

## Quick start (one PC)

```sh
cd server
python -m pspc_medius serve --umd ../Games/SFCO.iso
```

`--umd` points at your copy of the game (an ISO, a CSO or an extracted folder) and loads the 20
missions on the disc as the official ones. Then point the game at the server: create
`infrastructure.json` next to pspc.exe:

```json
{"games": [{"name": "Combat Ops (local server)", "ids": ["NPUG80114"],
            "domains": {"combatops.psp.online.scea.com": "127.0.0.1"}}]}
```

Start Combat Ops, choose online play and the connection "PSPC 1". The game signs in to PSN
(PSPC Network, or locally under the name you chose: [pspc-network.md](pspc-network.md)), shows
the terms of use to accept, and enters the lobby. To try a match alone, start a second PSPC instance (another
memory stick folder and online ID, e.g. `--memstick <folder> --set np_online_id=Other`): each
instance gets its own console identity and ports.

## Running a server for others

```sh
python -m pspc_medius --db sfco.sqlite serve --public-host 203.0.113.7 --umd SFCO.iso --network https://np.example.org
```

| Option | |
|---|---|
| `--db <file>` (before `serve`) | database file (default `pspc_medius.sqlite`) |
| `--public-host <address>` | the address players' games reach this server at; the server tells the game to connect there for each next step (default `127.0.0.1`) |
| `--bind <address>` | address to listen on (default `0.0.0.0`) |
| `--umd <iso, cso or folder>` | the game's disc: its missions become the official ones (once is enough; they stay in the database) |
| `--network <url>` | the PSPC Network server whose tickets to accept; players' online IDs become their names here |
| `--allow-local` | with `--network`: also let in players who signed in locally (their names aren't checked). Without `--network` everyone is let in this way |
| `--allow-password-login` | accept the Medius name logins some versions of the client library use (no ticket) |
| `--log debug` (before `serve`) | log every message (large) |

Players redirect `combatops.psp.online.scea.com` to the server, as above but with its address.
The game reaches everything else through addresses the server gives it.

### Ports

| Port | | |
|---|---|---|
| 10071 TCP | MUIS | the game's first stop: where everything else is |
| 10075 TCP | MAS | sign-in |
| 10078 TCP | MLS | the lobby: channels, chat, games list, buddies, cells, files |
| 10079 TCP and UDP | DME | games: players' connections and the game traffic between them |
| 10070 TCP | SVO | the game's web portal (HTTP): mission sharing, stats |
| 10070 UDP | NAT service | tells a game its address as seen from outside |
| 10443 TCP | SVO login | HTTPS (SSL 3.0, the only protocol the game knows) |

### Hosting on the internet

Forward the ports above to the server and give `--public-host` the address players use. During a
match the players' games also send UDP straight to each other (peer to peer) as well as through
the DME, as the PSP did; whether that needs ports forwarded on players' routers hasn't been
tested yet. Use `--network` with a PSPC Network server so names are checked.

## Administration

The same command manages the database; it's safe to use while the server runs.

```sh
python -m pspc_medius --db sfco.sqlite accounts                    # players who have signed in
python -m pspc_medius --db sfco.sqlite ban Some_Cheater
python -m pspc_medius --db sfco.sqlite unban Some_Cheater
python -m pspc_medius --db sfco.sqlite missions list               # shared missions, downloads, plays
python -m pspc_medius --db sfco.sqlite missions import SFCO.iso    # official missions from the disc
python -m pspc_medius --db sfco.sqlite missions hide 42            # hide an inappropriate one (unhide, delete)
```

The database holds accounts (by online ID), buddies and ignore lists, cells, stats, uploaded
missions and files, and what the game sends at the end of matches (`game_reports`). Back it up.

## How it works

| Piece | File |
|---|---|
| SCE-RT: framing, the RSA/RCQ handshake, connection setup | `rt.py`, `crypto.py` |
| Medius messages: C structure layouts, the message table | `structs.py`, `messages.py`, `service.py` |
| MUIS, MAS (sessions, policy, ticket login), MLS (lobby) | `universe.py`, `auth.py`, `lobby.py` |
| Players, channels, games | `world.py` |
| DME: game worlds, TCP and UDP relay | `dme.py` |
| SVO portal (SVML pages, `X-SVOMac`), SSL 3.0 server, HTTP | `svo.py`, `sslv3.py`, `http.py` |
| Mission server, mission files, reading the UMD | `missions.py`, `umd.py` |
| Game settings (app ID, channels, files such as `LobbySettings.ini`) | `games.py` |
| Database | `store.py` |

**A session.** The game connects to MUIS for the universe (the MAS address and the SVO URL),
signs in at MAS (session, localization, DNAS post, the terms from GetPolicy), loads the SVO root
page (which gives it the service URLs, including the HTTPS ticket login), sends its NP ticket to
MAS (TicketLogin) and to the SVO login page, and connects to MLS. Joining a channel reconnects to
MLS. Hosting creates a game; joining one returns the DME's address, the world and an access key;
both host and guests connect to the DME, over TCP and UDP. The match runs on the DME and directly
between the players; the host reports the game's state to MLS (WorldReport, PlayerReport,
EndGameReport).

**Protocol notes** (found against the game; the client library is Medius 3.02, "version 113"):

- SCE-RT messages are `[id][length u16][payload]`; an encrypted one has id | 0x80 and a 4-byte hash
  first. The server's hello carries Sony's "MAS 1" certificate with the server's own RSA-512 key
  in it; the game doesn't check the signature. RSA (e = 17, little-endian, no padding) carries the
  game's key, then the session cipher (RCQ) is used. Ciphers and layouts follow horizon-server
  (MIT).
- Encrypted game messages keep their routing in the clear: a single-target message its 2-byte
  player index, a list message its target bitmask (`[length][mask]`). The DME decrypts each
  message with the sender's key and encrypts it again for each recipient.
- The SVO login page must be `https:`; the game's SSL 3.0 client sends an SSLv2-format hello and
  offers only RC4-MD5. Page replies carry `X-SVOMac` = MD5 of the client's value and a fixed salt.
- Mission files are encoded buffers: `ENCB`, flags, size, then XTEA (32 rounds, ECB, the game's
  key) over the MD5 of the content and the content, raw-deflated. Inside is an ini file
  (`FILENAME`, `LVL_THEMEID`, `PVPGAMEMODE`, `LVL_CHUNKn`, ...). Files on the disc have a 0x1C-byte
  `qDEFSCE` header the game doesn't accept over the network, so the server strips it.
- World status 3 means closed to new players (a match is running), not ended.

## Tests

```sh
python -m unittest discover -s server/tests       # from the repository root
```

`test_medius.py` covers the ciphers, layouts and file formats, then runs the server and drives it
like two consoles: handshakes, sign-in, a game hosted and joined, game traffic relayed over TCP
and UDP (including encrypted list messages), the SVO pages over HTTP and SSL 3.0, and the mission
server. With `Games/SFCO.iso` present it also reads the disc's missions.

## Limits

- Players' stats (rank, XP, wins and games per mode) are kept as the game reports them, but there
  are no ladders (rankings across players) yet.
- Cell statistics (games played and won per mode) stay at zero.
- What the game sends to the portal at the end of a match is recorded (`game_reports`) but not
  used yet.
