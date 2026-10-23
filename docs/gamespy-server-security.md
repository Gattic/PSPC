# GameSpy server: security audit

`server/pspc_gamespy` was written for a PC and its friends. Running it for anyone who can reach it
(the public SWSS server, swss.pspc.online) makes every protocol it speaks a target. This is the
audit made before that: what an attacker can do, what was changed, what is left. How the server is
run in public: [Public server](gamespy-server.md#public-server) and
[server/deploy/swss/README.md](../server/deploy/swss/README.md). It was reviewed three times: the first pass
(findings 1-16) before the public server existed, the second (17-27) on the finished change, which
added the tunnel and the option of forwarded ports, and the third (28) on the log lines added for testing
with real clients, together with a check of every claim in this document against the code (below).

**Scope.** All of `server/pspc_gamespy` (17 modules, about 3,300 lines, read in full) and the code
added for the public server: the WebSocket gateway and the relay (`ws.py`, `tunnel.py`, `relay.py`).
Method: reading, then tests. Not covered: PSPC itself (the dedicated servers that run guest code, and the
UDP port 3658 that the router forwards to them), Cloudflare, the operating system.

**What the tests prove.** The tests were written with the fixes, and were not all checked to fail without them.
That was done afterwards, for the third pass, by reverting each fix in turn and running the tests that claim to
cover it. A test that still passed was strengthened or replaced (the chunked-request test and the `--local-hosts-only`
start-up test had passed without their fix). Result:

- **Caught by at least one test when its main fix is reverted:** #1-#15, #17-#24 and #27 (one revert per finding,
  not one per limit it mentions: the per-service rates and sizes of #5, #7 and #8 are exercised by the tests but
  not each reverted separately). #3 is caught only when both of its guards (the row cap and the stop at the end
  of the packet) are reverted, as they overlap.
- **No test notices if the fix is reverted:** #25 (the tunnel's connection and descriptor limits are constants) and
  #26 (`--tunnel` binding to any 127.x.y.z address). #16 is the absence of a flaw.
- **Tested as a rule, not as the behaviour claimed:** #5's disconnection of a client that is not reading (see
  the finding: the claim made for it was wrong), and the file modes of `server/deploy/swss/install.sh` (#11, by hand).

**Threat model.** An anonymous player (or a program pretending to be one) with a valid PSPC or none:
can open connections, send any bytes on any of the server's protocols, create accounts, and host
nothing the server doesn't let them. Not an attacker with a shell on the server, or one who has
the Cloudflare account.

## Findings

Severity is for a server on the internet: High is something a single anonymous client can use to
take the server down or to act as another player; Medium needs more effort or does lasting but
bounded harm; Low is hygiene.

| # | Severity | Finding | Fix |
|---|---|---|---|
| 1 | High | **The stats server signed in any profile by number** (`\authp\pid\`): it can't check the password's response (the server keeps only MD5), so anyone could read and overwrite another player's record and attribute reports to them. | `pid` sign-in needs the profile to be logged in to Presence from the same address. Login tickets work as before. |
| 2 | High | **Message injection.** The server wrote players' text (messages, nicks, status) into other players' `\key\value\` streams unescaped: a message containing `\final\\bm\4\f\<id>\msg\...` made the receiving game see a message that the server never sent, such as a friend request or authorization from someone else. | Values can't contain the separator (`kv.build`). |
| 3 | High | **One packet could stop the server.** A heartbeat's player section claims how many rows it has; parsing was work in proportion to the claim (65535 rows of 20,000 names: about 10^9 steps) on the thread that serves everyone. | Work is proportional to the packet; at most 128 rows and 96 columns, 256-byte values. |
| 4 | High | **Server browser filters were attacker-chosen regular expressions and recursion.** `LIKE '%a%a%a%a%b'` is exponential on a long host name; deep parentheses raised `RecursionError`. | LIKE is matched without regular expressions (linear in the product of the lengths); filters are 1,024 characters and 24 levels at most. |
| 5 | High | **No limits on connections, idle time or message size** on any TCP service: a client that opens thousands of connections, or sends a message that never ends, used up memory and descriptors. | Per-address and total connection limits for each service; time to log in or ask for a list; idle timeouts; 64 KiB per unfinished message (128 KiB stats, 4 KiB web lines, 64 headers); a client that has more than 1 MiB waiting to be sent to it (other players' messages and pushed updates pile up for it) is disconnected. A client that only *sends* and never reads is not covered by that: see "What is left". |
| 6 | High | **The server browser relayed messages to hosts** for anyone: from a connection that never asked for a list, 4 KB each, resent five times, to any address a game was listed at - a reflection tool (about 1000x) when hosts can be listed from forged UDP addresses (#9). | Only after a list request, 256 bytes, rate limited, at most 1,024 messages waiting. |
| 7 | Medium | **Guessing passwords was free** - at Presence login and, with no login at all, with the search manager's `check` and `nicks`. | Ten wrong passwords per address, then one every 30 seconds, for both; the connection is closed after a failure. |
| 8 | Medium | **Account creation was free and unchecked**: unbounded growth, control characters and 10 MB nicks in logs and leaderboards. | Ten accounts per address, then ten an hour; nicks 30 and e-mail addresses 100 printable characters; 16 profiles per account. |
| 9 | Medium | **Anyone can list a game anywhere.** The master server never checks a host's answer to its challenge (GameSpy's validation of it isn't implemented), and UDP addresses can be forged: fake games, games "hosted" at a victim's address, career numbers invented in heartbeats (the leaderboards count what hosts report). | `--local-hosts-only`: only the server's own machine (its dedicated servers) can list games; heartbeats from any other address, forged or not, are dropped without an answer. With the tunnel, players' heartbeats are refused too. Without either: `--max-player-hosts` games in all (default 512; **5 on the public SWSS server, which lets players host**) and 2 per address. Verifying the challenge remains open (see below): with player hosting, forged heartbeats can still take the places (see "What is left"). |
| 10 | Medium | **Randomness from `random`** (a Mersenne Twister, predictable from its output) for Presence challenges, session keys, the QR2 challenge and login tickets (which are bearer tokens for the stats server). | `secrets`. |
| 11 | Medium | **The database was world-readable** (e-mail addresses, password MD5s). | Created with mode 0600 (existing files changed to it); the deployment's folders are 0700. |
| 12 | Medium | **Unbounded storage** by one player: offline messages (to profiles that don't exist, from non-friends), buddy requests, persistent records (any number of 1 MB records), reported games; leaderboard pages ran one query per player per request. | Messages only to real profiles that haven't blocked the sender (blocks were never honored), stored only between friends, 200 waiting per profile; 100 requests; 32 records of 64 KiB; 16 games per connection; old rows are pruned (games 90 days, messages 60, requests 180); the rankings are one query, kept until the stats change. |
| 13 | Medium | **Unauthenticated flood of log output**: every malformed UDP packet logged a traceback; client text went into log lines raw (line breaks, terminal escapes). | Malformed packets are logged at debug level; client text is escaped and cut. |
| 14 | Low | NAT negotiation answered a report with 73 bytes to a 15-byte request (reflection factor 5), kept sessions without bound and failed on short packets with tracebacks. | Answer no longer than the request; 4,096 sessions, two players each; short packets ignored; UDP packets limited per source address. |
| 15 | Low | The web server read a chunked request body as the next request (one of the ways a proxy and an origin disagree about where a request ends). | `Transfer-Encoding` is refused; bodies are 64 KiB at most. |
| 16 | Info | **SQL injection: none.** Every query is parameterized; the two statements built from names use fixed column lists. | - |
| 17 | High | **A number too big for SQLite ended the connection with an unhandled exception**: `\getprofile\profileid\99999999999999999999...` (and the same in `addbuddy`, `bm`, the stats server's `pid` and `dindex`). Cheap for an attacker, and each one put a traceback in the log. | Numbers beyond 2^62 are not numbers (`kv.Message.int`); a message the code can't handle ends that connection only, with a one-line warning. |
| 18 | High | **A server-browser list request could ask for 2,000 keys**: 512 games times 2,000 keys of work on the thread that serves everyone, one request after another from one connection. | 64 keys at most; 20 list or info requests at once, then 4 a second, per address. |
| 19 | High | **`--public-host` as a name gave players the name as the game's address** (the lists and NAT negotiation carry 4-byte addresses: players got 0.0.0.0). | The name is looked up (failing to start if it has no address) and again every five minutes, keeping the last good answer. |
| 20 | Medium | **Remote addresses could fill the list of hosted games** so that the server's own dedicated games could not be listed: the total limit was checked before the machine's own games were exempted. | The server's own machine is always allowed, and its games are not counted against the players' limit (`max_player_hosts` counts the games of other addresses only, including ones that have not answered the challenge yet); with `--local-hosts-only` nothing else lists a game. |
| 21 | Medium | **No limit on Presence messages** (the stats server had one): one account could flood another player's connection, and make the server drop the victim when it fell behind. | 100 messages at once, then 20 a second, per address; the connection is closed beyond it. |
| 22 | Medium | **A connection could log in twice**, as a second profile: the first stayed "online" on a connection that was no longer its own. | One login per connection. |
| 23 | Medium | **A tunnel could be opened inside a tunnel**: a player's connection to the web pages, through the relay, could ask the gateway for another tunnel, with an address and limits of its own. | The gateway refuses requests that come from a tunnelled address. |
| 24 | Medium | **The relay could be ended by a server answer it didn't expect** (a truncated or oversized handshake, an HTTP error it didn't know) and then listened for nothing. | Those are errors of that connection or that refresh, logged; the relay keeps going. |
| 25 | Low | The tunnel allowed 4,000 connections with 8,192 file descriptors: about three descriptors each, so a flood ran into "too many open files" before the limit. | 2,000 connections, 16,384 descriptors. |
| 26 | Low | `--tunnel` insisted on 127.0.0.1 or 0.0.0.0. | Any 127.x.y.z address (what the tests use to run next to a server already running). |
| 27 | Low | A server meant for the internet could be started without saying where it is (`--local-hosts-only` with no `--public-host`), and would tell players to join games at 127.0.0.1. | It refuses to start. |
| 28 | High | **The log line that says a player's heartbeat was ignored (added for testing with real clients) made forged heartbeats expensive.** It parsed every dropped heartbeat in full to read its game name: 11 ms for a 60 KB one (about 90 a second filled the loop; 1.4 KB ones, 650 microseconds each, about 1,500 a second), and its per-address limiter was emptied at 1,000 addresses, so spoofed sources got **one log line per packet** (3,000 lines for 3,000 packets). UDP source addresses are not trustworthy, so per-address limits did not help. It was in the deployed service from the diagnostics deploy (2026-10-20 11:27 UTC) until this fix was deployed (13:11 UTC); measured on the live service just before the fix: 11.7 ms of CPU per 60 KB heartbeat, 856 microseconds per 1.4 KB one, one log line per packet (1,434 lines for 1,440 packets), and a TCP greeting delayed by up to 180 ms. After, on the same harness: 140 and 98 microseconds, 10 lines for 26,000 packets, delay at most 25 ms. | The game name is read from the first 512 bytes without parsing (`heartbeat_game`): 25 microseconds for a 60 KB packet, 19 for a 1.4 KB one. The line is said once a minute per address (a bounded table), at most ten at once and then one every ten seconds in all, with the count so far; the total is kept. `access.is_local` and `is_tunnel` no longer parse an address outside 127/8 (every client on the internet), which took a bare dropped packet from 12.6 to 4.4 microseconds. |

## Exposed directly (forwarded ports)

What the router forwards is the server's own attack surface, with no front door in between:

| Port | Who can talk to it | What an attacker gets, after the fixes |
|---|---|---|
| TCP 29900 Presence | anyone | account creation (10 an address, then 10 an hour), password guesses (10, then 2 a minute), 16 connections an address, 100 messages then 20 a second, 64 KiB a message |
| TCP 29901 search | anyone | 60 requests then 2 a second, 50 a connection, the same failed-password budget as Presence |
| TCP 28910 browser | anyone | lists of the games on this machine at 20 requests then 4 a second; 256-byte messages to a listed game, 10 then one every 2 seconds |
| TCP 29920 stats | players logged in to Presence from the same address | records of 64 KiB, 32 a profile |
| TCP 80 (8080) web | anyone | the message of the day and the rankings, 60 requests then 20 a second, no bodies over 64 KiB |
| UDP 27900 master | anyone | an "available" answer of 7 bytes and a keepalive answer of 11, 60 packets then 20 a second per address; **without `--local-hosts-only`, a heartbeat from an address with no game listed is parsed only while there is room (`--max-player-hosts`, 2 per address) and then at most 5 a second in all (a burst of 10); every other is dropped unparsed and unanswered: 30 to 75 microseconds each, on average, when 3,000 forged 60 KB ones were measured, against 11,700 before #28's fix**; a game that does not answer the challenge within 30 seconds gives up its place |
| UDP 27901 NAT negotiation | anyone | answers no larger than 21 bytes to a request of 12 or more, 4,096 sessions, 40 packets a second per address |
| UDP 3658 (a dedicated game) | anyone | **not covered by this audit**: it reaches the dedicated PSPC's own network code (the game's sockets), not this server. Nothing listens on it until a dedicated game runs, and then everything the game's networking does with a datagram is exposed to the internet |

All of it is single-threaded Python. The per-address limits keep one address from using the whole loop, **for TCP**
(a run of 11,000 malformed and hostile inputs at every port, the tunnel included, left the longest stall of the loop at 35 ms and
logged no error; it does not log in, so the logged-in handlers are covered by the tests). **For UDP they do not help
against forged source addresses** (a new source starts with a full allowance), so what bounds a flood is the cost of a packet.
Two measurements, and the second is the one to believe: in the process alone (no socket, no event loop) a bare dropped packet costs
4.4 microseconds and a dropped heartbeat 19 to 25; **on the running service, counting the kernel's receive and asyncio's work for each
datagram, a bare dropped packet costs 60 microseconds, a 40-byte heartbeat 99, a 1.4 KB one 98 and a 60 KB one 140**
(from distinct loopback sources 127.64.x.y, which the server treats as strangers; `/proc/<pid>/schedstat`, the server's own CPU time).
So **about 16,000 forged packets a second (a few Mbit/s: 33 bytes each plus the headers) keep the server's one core busy**,
and the TCP services wait behind them: during 8,000 a second the greeting on the Presence port was delayed by 4 to 8 ms at worst (not measured
above that rate: past what the core can take it has no time left for them). This is a property of the design (Python, one thread) and the fixes only moved it from
"one forged heartbeat in ten milliseconds" to "ten to sixteen thousand packets a second". A flood of that kind is a bandwidth problem
that only the router, the ISP or a service in front can answer, and a few Mbit/s is within the reach of anyone who wants to try.
The server's address is public, so is the router's: that is the price of direct UDP.

## The tunnel

A Cloudflare Tunnel carries HTTP(S) only, so the public server's TCP and UDP go over WebSockets
([Public server](gamespy-server.md#public-server)). That adds a front door, written to be small:

- It is the web port's `/tunnel/` and only forwards to the server's own services (Presence, search,
  browser, stats, web; the master server, NAT negotiation, and the ports of games hosted on this
  machine), never to another address or port. Requests from web pages (`Origin`) and from tunnelled
  addresses (a tunnel inside a tunnel) are refused.
- Each player is one address in 127.64.0.0/16 (up to 4,095 at once; more get a 503): the services,
  their limits, bans and the server list see one player per address, as they would on the
  internet. The player's own address is Cloudflare's `CF-Connecting-IP`, believed only from this
  machine (`--behind-proxy`); other proxies' `X-Forwarded-For` counts from its last entry.
- Per player: 48 connections, 30 new ones at once then 3 a second, 6 UDP sockets, 256 KB/s and 300
  datagrams/s upstream; silent connections are dropped after 90 seconds (WebSockets are pinged every
  25 seconds, as a proxy may drop idle ones after about 100) and UDP sockets after 5 minutes; 2,000 connections in all.
- On a player's PC the relay listens on 127.77.0.1 only and takes at most 512 ports of each kind
  from the server; it checks the server's certificate against the system's and ends with PSPC.

## What is left

- **The master server's challenge isn't verified** (#9). With `--local-hosts-only` (and in the tunnel
  deployment) nothing outside the machine can list games, so it doesn't matter. **The public SWSS server
  has been opened to player hosting (`--max-player-hosts 5`) without it, on the owner's decision, and has the
  hole:** anyone who can send UDP to port 27900 can fill the five places with forged heartbeats from any source
  address and answers (the server doesn't check the answer), so that real players can't list a game until
  the forger stops (they have to repeat it at least every 3 minutes, and every 30 seconds for the places to stay
  taken by games that don't answer), and can make the list show games at a victim's address (players who join
  one send it traffic). What the limits do bound is the cost: five games at most, parsing capped, a refused heartbeat
  almost free, so a forger can take the places and nothing else. Closing it needs GameSpy's challenge check
  (`gsseckey` with the game's key), which this server does not implement and which can only be written correctly
  against a real game's answers: the dedicated game's, once one runs here. Until then, `--local-hosts-only`
  is the safe setting, and the dedicated servers are the games that can be relied on.
- **Player-hosted games are the players' machines.** The server only lists them and introduces players (NAT
  negotiation); what a joining player's game receives from a host is up to that host.
- **Passwords are MD5 of the password** and always will be: GameSpy's login proves knowledge of that
  value, so the server has to keep it. A copy of the database lets its holder log in to this server as
  anyone (and nowhere else, if players don't reuse passwords). Keep the database private and backed up.
- **Whoever shares a NAT with a player shares their address**: the stats sign-in (#1), the limits and
  the player's lockout after failed logins are per address.
- **`others` and `search` tell anyone who is on whose friends list and under which nicks**, as GameSpy's
  did; e-mail addresses are never returned.
- **Cloudflare is part of the trust**: it sees and can alter everything; the relay's TLS only
  protects the player from others on their network. Account passwords cross it in GameSpy's weak
  encoding, as they crossed the internet once.
- **Direct mode is not encrypted.** The protocols are GameSpy's, in the clear. A new account's password (and a changed one) crosses
  the internet in GameSpy's reversible encoding; logins prove the password by challenge and response, but the session after it
  is readable and changeable by anyone on the path, and the login ticket (`lt`), which the stats server accepts as proof of who a
  player is, can be taken from it. Only the tunnel adds TLS (between the relay and Cloudflare). Players should not use a password
  they use elsewhere.
- **A client that sends requests and never reads holds its connection.** The server waits to write to it (`drain`), stops
  reading from it, and has no timeout for that wait: measured, the connection stays open with about 80 KB unsent and its
  profile shows as online, until the client closes it or TCP gives up. It holds one of its address's 16 slots. The 1 MiB drop
  (#5) only ends clients that others send enough to; a timeout on the write wait would end this one too, and is not made.
- **The stats server accepts a wrong `\auth\` response** (it logs it and lets the client in): the key it is computed from is
  published in GameSpy's key list and in the game's own code, so the check would add nothing.
- **Log volume is bounded only per event.** Each request that is refused or served is a line at info level; journald's own rate
  limit is what stops a flood of them. The "ignored heartbeat" line (#28) is the one with a limit of its own.
- **Logs hold players' addresses and e-mail addresses.** Keep them as long as you need and no longer.
- **Dedicated servers run guest code** (the game's own, from a disc you trust): give them their own
  user and a folder of their own.

## Running it

The deployment in `server/deploy/swss/` follows from this. With forwarded ports: the service runs as an
ordinary user (port 80 is mapped to 8080 by the router, so it needs no privileges) with a private data
folder, `--max-player-hosts 5`, and the router forwards exactly the ports of the table above and the UDP
ports of the dedicated games. Through the tunnel: the services listen on 127.0.0.1 (so nothing but
cloudflared, on this machine, reaches them), no port is forwarded and the firewall can refuse everything
incoming, and the tunnel's token is readable by that user only. With the tunnel, in Cloudflare: leave WebSockets on, keep Access
and the Browser Integrity Check / Bot Fight Mode off for this hostname (the relay is not a
browser and cannot answer a challenge), and add a rate limiting rule for `/tunnel/` if you see
abuse (a player opens 5 to 10 connections to start, and asks `/tunnel/info` every 4 seconds).

## Tests

`server/tests/test_gamespy_security.py` (limits, injection, bounded parsing, sign-in rules,
storage caps, who may host) and `server/tests/test_tunnel.py` (WebSocket layer, certificate
checking, the gateway's rules, and a game's TCP and UDP through relay and gateway) run in
`python -m unittest discover -s server/tests`. `server/tests/e2e_bridge.py` runs the processes of a
deployment (server, relay, a dedicated game's UDP socket, players) and checks Presence, the server list,
NAT negotiation, UDP both ways (300 datagrams out and back without loss; a burst is cut at the gateway's
limit to within a few datagrams: 821 to 825 arrived where about 822 to 824 were allowed), 14 stats records of 60 KB byte for byte, many connections, a flood and the
server restarting; `--direct ADDRESS` runs it without the relay, on a real address, with forged heartbeats
from another address. It refuses to run while anything holds the games' ports on the addresses it uses, so on a machine where the
server itself runs on every address it has to run in a network of its own (it was run in a container with the host's `/usr` mounted
read-only). `server/tests/live_check.py` drives a running server the way a game does and was run
through a real Cloudflare Tunnel.
