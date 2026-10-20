# PSPC Network server

The account server behind PSPC's PlayStation Network stand-in: online IDs and passwords, signed
NP tickets in Sony's format, friend lists, and ticket checks for game servers. PSPC players sign
in to it from the launcher's **Network** button; their games then sign in to "PSN" through it.
How the pieces fit together, the API and the ticket format: [docs/pspc-network.md](../docs/pspc-network.md).

Python 3.9 or later, standard library only. One file of state (SQLite).

**On a player's PC, the launcher runs it:** the *Online* page's PSPC Network card starts and stops it,
lists and bans accounts and resets passwords, and shows its log (an installed PSPC brings its own
Python). Its database and log are in the `servers` folder next to pspc.exe. The rest of this page is
for running it yourself, for example on a server for a community.

## Running it

```sh
cd server
python -m pspc_network serve                       # http://0.0.0.0:8423, database pspc_network.sqlite
```

| Option | |
|---|---|
| `--db <file>` (before `serve`) | database file (default `pspc_network.sqlite` in the current folder) |
| `--listen <address:port>` | default `0.0.0.0:8423`; `127.0.0.1:8423` for this machine only or behind a proxy |
| `--name <text>` | the server's name, shown in the launcher |
| `--no-registration` | don't accept new accounts (create them some other way, or open registration for a while) |
| `--tls-cert <pem> --tls-key <pem>` | serve HTTPS itself |
| `--behind-proxy` | take players' addresses from the proxy's `X-Forwarded-For` (rate limits, log) |

Players set the server's address in the launcher (`https://np.example.org`, or
`http://192.168.1.10:8423` on a LAN) and create an account there.

**Use HTTPS on the internet.** Passwords and session tokens go over this connection. Either put
the server behind a reverse proxy that does TLS (recommended: it renews certificates) or give it
a certificate with `--tls-cert/--tls-key`. PSPC accepts any certificate Windows trusts. Plain
HTTP is fine on a LAN or on one PC.

### Behind nginx (or Caddy)

```nginx
server {
    listen 443 ssl;
    server_name np.example.org;
    ssl_certificate     /etc/letsencrypt/live/np.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/np.example.org/privkey.pem;
    location / {
        proxy_pass http://127.0.0.1:8423;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

Caddy: `np.example.org { reverse_proxy 127.0.0.1:8423 }`. Run the server with
`--listen 127.0.0.1:8423 --behind-proxy` so it's only reachable through the proxy and rate limits
apply per player rather than to the proxy.

### As a service (systemd)

```ini
# /etc/systemd/system/pspc-network.service
[Unit]
Description=PSPC Network
After=network.target

[Service]
User=pspc
WorkingDirectory=/opt/pspc/server
ExecStart=/usr/bin/python3 -m pspc_network --db /var/lib/pspc-network/np.sqlite serve --listen 127.0.0.1:8423 --behind-proxy --name "My PSPC Network"
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```sh
sudo useradd --system pspc && sudo mkdir -p /var/lib/pspc-network && sudo chown pspc /var/lib/pspc-network
sudo systemctl enable --now pspc-network
journalctl -u pspc-network -f          # requests, accounts created, tickets issued
```

On Windows, run it in a console (or as a scheduled task at start-up) and allow Python through
Windows Firewall when asked.

## Administration

The same command manages accounts. Point it at the server's database with `--db`; it's safe to
use while the server runs.

```sh
python -m pspc_network --db np.sqlite accounts           # online ID, account ID, country, created, BANNED
python -m pspc_network --db np.sqlite ban Some_Cheater   # signs them out; their tickets stop verifying
python -m pspc_network --db np.sqlite unban Some_Cheater
python -m pspc_network --db np.sqlite passwd Gabe_Logan  # set a new password (asks for it); signs them out
```

**Back up the database file.** It holds the accounts (losing it loses everyone's online ID and
friends) and the key the server signs tickets with, generated on first start. Passwords are stored
as scrypt hashes and session tokens as SHA-256 hashes, so a copy of the database can't be used to
sign in; keep it private anyway, because whoever has the key can forge tickets.

## For game server authors

A PSP game that uses PSN sends its NP ticket to its game server when it logs in. Check it with
the PSPC Network server that issued it:

```
POST /v1/tickets/verify
{"ticket": "<base64 of the ticket bytes>"}

200 {"valid": true, "online_id": "Gabe_Logan", "account_id": "1234...", "service_id": "UP9000-NPUG80114_00",
     "country": "us", "issued_ms": ..., "expires_ms": ...}
200 {"valid": false, "reason": "expired" | "bad signature" | "account suspended or gone" | ...}
```

No token is needed. Also check that `service_id` is your game's (each game asks for tickets with its
own service ID), and use `online_id` as the player's name. Tickets last 15 minutes, which is long
enough to log in. Keep your own session after that. Local tickets (cipher `PSPL`, from players
without a server) never verify; a server may let them in as guests if it wants to.

The ticket layout is described in [docs/pspc-network.md](../docs/pspc-network.md#how-it-works), and
`pspc_network/ticket.py` parses it if you want the fields without a round trip, but only the
server can check the signature.

## Game servers

`pspc_medius` is a game server for Syphon Filter: Combat Ops (lobby, games, mission sharing,
leaderboards, cells, a message board), which checks players' tickets with this server: see
[docs/combat-ops-server.md](../docs/combat-ops-server.md).

```sh
python -m pspc_medius --db sfco.sqlite serve --public-host 203.0.113.7 --umd SFCO.iso --network https://np.example.org
```

`pspc_gamespy` replaces GameSpy's servers for Star Wars Battlefront: Elite Squadron (accounts,
friends, hosted games, NAT negotiation, stats, leaderboards); it has accounts of its own, made in
the game: see [docs/gamespy-server.md](../docs/gamespy-server.md).

```sh
python -m pspc_gamespy --db gamespy.sqlite serve --public-host 203.0.113.7
```

It also serves Star Wars Super Squadron. On the internet it runs with `--bind 0.0.0.0 --public-host
swss.pspc.online --local-hosts-only` behind forwarded ports, or with `--tunnel --behind-proxy` behind a
Cloudflare Tunnel (players then reach it through a relay PSPC starts: `python -m pspc_gamespy tunnel
https://swss.pspc.online`); `server/deploy/swss/` sets either up and
[docs/gamespy-server-security.md](../docs/gamespy-server-security.md) says why.

## Tests

```sh
python -m unittest discover -s server/tests       # from the repository root
```
