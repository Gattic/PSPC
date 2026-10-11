# PSPC Network (the PlayStation Network stand-in)

Sony shut the PSP's PlayStation Network services down, and several online games refuse to go
further without them: Syphon Filter: Combat Ops signs in to PSN, gets a *ticket* (a signed proof
of who the player is) and reads the friend list before it talks to its own lobby server. PSPC
gives these games a PSN of its own:

- **Hosted accounts.** A *PSPC Network server* (`server/pspc_network`, Python, see
  [server/README.md](../server/README.md)) keeps accounts (online ID + password), issues signed
  tickets in Sony's format and keeps friend lists. Players sign in once from the launcher; games
  then sign in to "PSN" without asking. Game servers check a player's ticket with the server.
- **Local sign-in** (no server configured). Games sign in as `np_online_id` (default
  `PSPCPlayer`) and get an unsigned ticket. Enough for games that only need *a* sign-in; a game
  server can't trust it.
- **Redirects.** Games' own server names (Sony's SCE-RT/Medius lobbies, SVML, GameSpy) are sent
  to community revival servers through a per-game table compatible with PPSSPP's
  `infra-dns.json`, or a revival DNS server.

This is the account layer. The game servers themselves (a Medius lobby for the Syphon Filter
games, GameSpy for Star Wars Battlefront) are separate projects that plug into it: they take the
game's ticket and ask the PSPC Network server whether it's valid.

## Using it

**Hosted.** In the launcher, click the account at the bottom of the sidebar (**Network**), choose
*PSPC Network server*, enter the server's address, your online ID and password, and press
*Create account* (first time) or *Sign in*. The same dialog shows your friends and adds or removes them. From the command line:

```bat
pspc.exe --np register --server https://np.example.org --id Gabe_Logan   :: asks for a password
pspc.exe --np signin --server https://np.example.org --id Gabe_Logan
pspc.exe --np status
pspc.exe --np friends
pspc.exe --np friend-add --id Lian_Xing        :: a request; becomes a friendship when Lian_Xing adds you too
pspc.exe --np friend-remove --id Lian_Xing
pspc.exe --np signout
pspc.exe --np local --id Gabe_Logan            :: forget the server; sign games in locally as Gabe_Logan
```

`--config <file>` picks another settings file. The password is never stored: signing in stores a
token (`np_token`, keep pspc.ini private) that the server can revoke.

```ini
np_server=https://np.example.org   # empty = local sign-in
np_online_id=Gabe_Logan            # the account (hosted) or the name games see (local)
np_token=...                       # written by the launcher / --np signin
infra_dns=                         # optional revival DNS server for every game (see Redirects)
```

**In a game,** pick the connection "PSPC 1" as usual. When the game asks for a PSN sign-in, the
system's sign-in screen appears briefly ("PSPC Network: Signing in.") and returns the account.
If the server can't be reached or the token was revoked, the sign-in fails the way PSN's did and
the game shows its own error; sign in again from the launcher.

**Running a server** for friends or a community: see [server/README.md](../server/README.md). For
a quick test on one PC:

```bat
cd server
python -m pspc_network serve --listen 127.0.0.1:8423
pspc.exe --np register --server http://127.0.0.1:8423 --id Gabe_Logan
```

## Redirects

Old games look up fixed host names (`combatops.psp.online.scea.com`, `gpcm.gamespy.com`, ...).
PSPC can answer those lookups with a revival server's address. The table is read from, in order
(the first file with an entry for the running game wins):

1. `infrastructure.json` next to pspc.exe (your own),
2. `data/infrastructure.json` (shipped with PSPC: lists the servers each supported game uses,
   no revival servers yet; for Combat Ops, run [its server](combat-ops-server.md)),
3. PPSSPP's `infra-dns.json`, next to pspc.exe or in `data/`.

The format is PPSSPP's, so community revival tables work unchanged:

```json
{
  "games": [
    {
      "name": "Syphon Filter: Combat Ops",
      "ids": ["NPUG80114"],
      "domains": { "combatops.psp.online.scea.com": "203.0.113.7" },
      "dns": "203.0.113.53"
    }
  ]
}
```

`ids` (or PPSSPP's `known_working_ids`, `other_ids`, `not_working_ids`) are disc IDs. `domains`
maps a host name to an IPv4 address or another host name. `dns` is a revival DNS server that
answers for all of the game's other names. pspc.ini `infra_dns=` is a revival DNS server for every
game.

Games resolve names three ways, and all are covered:

| Path | Who uses it | What PSPC does |
|---|---|---|
| sceNetResolver | most games | answers from the table (then the revival DNS server, then Windows) |
| sceHttp | SVML, GameSpy stats, browsers | connects to the redirected address, keeps the original `Host:` header; for https, certificate errors are ignored (revival servers can't have Sony's certificates) |
| DNS queries the game sends itself (UDP to the access point's DNS server, port 53) | SCE-RT / Medius games such as Combat Ops | a redirected name is answered on the spot: the answer datagram is put into the game's socket; other queries go out as normal |

With a revival DNS server configured, the access point also reports it as the DNS server, so a
game's own queries go straight there.

## How it works

| Piece | Where |
|---|---|
| sceNp, sceNpAuth (tickets), sceNpService (roster: friend list), sceNpLookup stubs | `src/runtime/hle/modules/net_np.cpp` |
| Sign-in dialog (sceUtilityNpSignin) | `src/runtime/dialogs/simple_dialogs.cpp` (`NpSigninDialog`) |
| Ticket builder / parser | `src/net/np_ticket.*`, and `server/pspc_network/ticket.py` |
| Client for the server's API | `src/net/np_client.*` (over WinHTTP, `src/net/http_client.*`) |
| JSON reader | `src/common/json.*` |
| Redirect table, DNS answers | `src/runtime/hle/modules/net_infra.cpp`, `src/net/dns.*`; the socket layer's interception in `net.cpp` (`TryAnswerDns`) |
| Launcher dialog, `--np` commands | `src/frontend/network_dialog.*`, `src/frontend/main.cpp` |
| Server | `server/pspc_network` (`api.py`, `store.py`, `ticket.py`) |

**Sign-in.** sceUtilityNpSignin asks the server for the account behind `np_token` (`GET /v1/me`)
on a host worker thread. Success fills the profile sceNpGetOnlineId, sceNpGetNpId,
sceNpGetUserProfile and sceNpGetAccountRegion return, and the dialog ends with status 1 (signed
in). Failure ends it with status 3. Local mode signs in immediately, with an account ID derived
from the name (FNV-1a), so it stays the same across runs.

**Tickets.** sceNpAuthCreateStartRequest starts a library thread ("SceNpAuthReq") and returns
the request ID. The thread asks the server for a ticket for the game's service ID
(`POST /v1/tickets`), or builds a local one, then calls the game's callback
`(requestId, ticketSize or error, arg)`. Never sooner than 30 ms: games expect the answer after a
round trip, not before CreateStartRequest returns. sceNpAuthGetTicket copies it out;
sceNpAuthGetTicketParam returns one parameter. Entitlement lists are empty.

The ticket is Sony's version 2.1 layout, big-endian, 224 bytes:

```
u32 0x21010000, u32 size of the rest
section 0x3000 (body): u16 type, u16 size, then parameters (u16 type, u16 length, data)
    binary[20]  serial              int     issuer (0x100)
    date        issued (ms)         date    expires (issued + 15 min)
    long        account ID          string[32] online ID
    binary[4]   country ("us")      string[4]  domain ("c9")
    binary[24]  service ID          int     status ((age & 0x7f) << 24)
    null, null
section 0x3002 (footer):
    binary[4]   cipher ID: "PSPC" (server) or "PSPL" (local)
    binary[32]  signature
```

Parameter types: 0 null, 1 int, 2 long, 4 string, 7 date, 8 binary. Sony signed tickets with
ECDSA and a key only its servers had. PSPC Network signs with HMAC-SHA256 over the header and body
using a key that never leaves the server, which is why game servers verify tickets through the
server rather than checking them themselves. Local tickets carry a zero signature.

**Friend list.** In firmware 4.01 the roster lives in np_service.prx (Sony's servers answered at
`/imgw/sec/psp/friend/...`). sceNpRosterGetFriendListEntry(request, maxEntries, startIndex,
\*returned, \*total, entries, option) fills 40-byte entries `{u32 status; SceNpId npId;}` (the
SceNpId is the 20-byte online ID, then 8 option and 8 reserved bytes) with the server's mutual
friends (`GET /v1/friends`) and returns the number written; GetFriendListEntryCount returns the
total. Pending requests aren't friends yet and aren't listed. The block list is empty. Local mode
has no friends.

## Server API (v1)

JSON over HTTP(S). Authenticated calls send `Authorization: Bearer <token>`. Errors are
`{"error": "<code>", "message": "<text for players>"}` with a 4xx/5xx status.

| Call | Body | Answer |
|---|---|---|
| `GET /v1/info` | | `{api: 1, name, version, registration, ticket_cipher: "PSPC"}` |
| `POST /v1/accounts` | `{online_id, password, country}` | 201 `{online_id, account_id, country, domain, token}` |
| `POST /v1/sessions` | `{online_id, password}` | `{online_id, account_id, country, domain, token}` |
| `DELETE /v1/sessions/current` | | 204: the token stops working |
| `GET /v1/me` | | `{online_id, account_id, country, domain}` |
| `PUT /v1/me/password` | `{old_password, new_password}` | `{online_id, account_id, country, domain, token}`: every other sign-in ends; use the new token |
| `POST /v1/tickets` | `{service_id}` | `{ticket: base64, expires_ms}` |
| `POST /v1/tickets/verify` | `{ticket: base64}` | `{valid, online_id, account_id, service_id, country, issued_ms, expires_ms}` or `{valid: false, reason}` |
| `GET /v1/friends` | | `{friends: [{online_id, status: friend\|sent\|received}]}` |
| `PUT /v1/friends/<online ID>` | | a request, or accepts theirs: `{online_id, status}` |
| `DELETE /v1/friends/<online ID>` | | 204: ends the friendship or request |

`account_id` is a decimal string (it's 64-bit). `/v1/tickets/verify` needs no token: it's for game
servers. It rejects a forged or tampered ticket, an expired one and one for a banned or deleted
account. A game server should also check that `service_id` is its game's.

Online IDs are 3-16 characters, letters, digits, `_` and `-`, starting with a letter, unique
regardless of case. Passwords are at least 8 characters and are stored as scrypt hashes; tokens
are stored as SHA-256 hashes and last 180 days. Sign-in and registration are limited to 10
attempts a minute per address.

## Games

| Game | Online services | State |
|---|---|---|
| Syphon Filter: Combat Ops (NPUG80114) | PSN sign-in, ticket and friend list, then SCE-RT Medius lobby `combatops.psp.online.scea.com:10071` (resolved by the game's own DNS queries) and SVML (`combatopspsp.online.scee.com:10070`) | Works with PSPC's own server, `server/pspc_medius`: lobby, games, matches, mission sharing ([combat-ops-server.md](combat-ops-server.md)) |
| Syphon Filter: Logan's Shadow (UCUS98606) | SVML (same servers), no PSN | Needs a revival server |
| Star Wars Battlefront: Renegade Squadron (ULUS10292), Elite Squadron (ULUS10390) | GameSpy (presence, master server, NAT negotiation, stats), no PSN | Needs GameSpy-compatible servers (redirect `*.gamespy.com`) |
| Monster Hunter Freedom Unite, Daxter | ad hoc (and HTTP downloads for MHFU) | Ad hoc works through `pspc --adhoc-server` |

None of these use DNAS.

## Testing

```bat
build\RelWithDebInfo\bin\pspc_unittests.exe json_reader np_ticket_layout np_server_url dns_messages
python -I -m unittest discover -s server\tests       :: servers: accounts, tickets, verify, friends, limits; Combat Ops
```

End to end: run a server on 127.0.0.1, `pspc --np register` against it, and start Combat Ops'
online mode; the log shows `[NP] Signed in to PSPC Network`, `Ticket request 1: 224-byte ticket`
and `Friend list: n of n friends`. To test redirects, map `combatops.psp.online.scea.com` to
127.0.0.1 in an `infrastructure.json` next to pspc.exe and watch the game connect to
127.0.0.1:10071.

## Limits

- No presence, messaging or block lists; sceNpLookup (other players' profiles) answers nothing.
- One account per pspc.ini. Several instances on one PC can use different accounts with
  `--config`.
- Tickets are signed with HMAC, not Sony's ECDSA: servers written for real PSN tickets need to
  call `/v1/tickets/verify` instead of checking the signature.
- The redirect table is IPv4 only, and only names the game looks up are redirected (a game that
  connects to a fixed IP address needs a patch).
