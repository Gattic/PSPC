# Networking

PSPC gives PSP software a real network. The WLAN switch reads *on* (pspc.ini `network=1`, the
default) and:

- **Infrastructure mode**: network configuration 1, "PSPC 1" (SSID `PSPC`), connects to a virtual
  access point that is the host's own network. Games and the XMB connect as on a PSP and get the
  host's IP address, netmask, gateway and DNS server as their DHCP lease. BSD sockets
  (sceNetInet), name resolution (sceNetResolver) and HTTP/HTTPS (sceHttp, sceSsl) go through
  Windows (Winsock, getaddrinfo, WinHTTP).
- **Ad hoc mode**: players meet through an ad hoc server that speaks PPSSPP's "pro adhoc server"
  protocol; game data then goes directly between them. `pspc --adhoc-server` runs one.

`network=0` turns all of it off: the libraries then behave as on a PSP whose WLAN switch is off
(connecting fails with WLAN_SWITCH_OFF, names don't resolve, sockets reach nobody).

## Using it

```ini
network=1                 # 0 = offline (the WLAN switch is off)
adhoc_server=localhost    # ad hoc server: host name or address, optionally :port (default 27312)
```

**Infrastructure games.** When a game asks to connect (its own menu or the system's network
dialog), pick the connection "PSPC 1". A PSN sign-in goes to PSPC Network, PSPC's PlayStation
Network stand-in (accounts, tickets, friends; see [pspc-network.md](pspc-network.md)). Old online
services are mostly gone, so a game usually gets as far as its own server and then reports that
the server can't be reached - unless a community-run replacement server exists and the game's
server names are redirected to it (`infrastructure.json`, PPSSPP's `infra-dns.json`, or a revival
DNS server: [pspc-network.md](pspc-network.md#redirects)). Syphon Filter: Combat Ops has one in this
repository: [combat-ops-server.md](combat-ops-server.md).

**The XMB** (`pspc --vsh`):

- *Settings > Network Update* connects through "PSPC 1" and asks Sony's update server, which
  still answers: it reports that a later version (6.61) is available. Don't install it: PSPC runs
  4.01 (its flash0 is read-only to guest code anyway).
- *Network > Internet Browser*, *Online Instruction Manuals* and *Internet Search* run the PSP's
  own NetFront browser. The browser's modules and the connection dialog don't fit in 24 MB of
  user memory (PSPC places firmware modules at their native-code addresses, which fragments the
  partition), so the XMB gets the PSP-2000's larger user memory by default (`--no-large-mem`
  turns that off, see docs/vsh-boot.md). Plain http:// pages load and render; https works too
  (TLS is WinHTTP's, so modern sites' certificates and protocols are fine, but most sites send
  pages NetFront 2.0 can't show - Google answers "Update your browser").
- The system's connection lists (Network Update's "Select connection", the browser's connection
  dialog) show "PSPC 1". A new connection can be set up but isn't kept: there is only the virtual
  access point.

### Ad hoc multiplayer

PSPC plays PSP ad hoc games over your network the way PPSSPP does: an *ad hoc server* introduces
the players of a game to each other, and the game data then goes directly between them. The
server speaks PPSSPP's protocol, so PSPC and PPSSPP players can share one.

**Two instances on one PC**

    pspc.exe --adhoc-server --adhoc-bind 127.0.0.1

Leave it running, then start the game in two pspc windows (the default `adhoc_server=localhost`
already points at it). Each instance gets its own loopback address (127.0.0.1, 127.0.0.2, ...)
and MAC address, so they see each other as two consoles. Choose the game's ad hoc / local
multiplayer mode in both. Stop the server with Ctrl+C.

**Over a LAN**

On one PC run `pspc.exe --adhoc-server` (it listens on all interfaces at TCP 27312; allow it
through Windows Firewall when asked). On every PC, including that one, set in pspc.ini:

    adhoc_server=192.168.1.10      # the server PC's LAN address (host or host:port)

Use the LAN address, not `localhost`, on the server PC as well, or the other players are told a
loopback address they can't reach. Each installation has its own MAC address (the console identity
in `flash1\pspc_idstorage.txt`, which the XMB's System Information shows too); a copied pspc folder
shares it, so delete that file in the copy.
Allow pspc.exe through Windows Firewall (private networks) when a game first opens its sockets.

**With PPSSPP or a public server**

Any PPSSPP-compatible ad hoc server works: PPSSPP's built-in one, `pspc --adhoc-server`, or a
public server (`adhoc_server=host` or `host:port`). PSPC uses PPSSPP's default port offset (game
ports + 10000), so PSPC and PPSSPP players can join the same group. Playing over the internet
needs those ports reachable: there is no UPnP.

Server options: `--adhoc-port <n>` (default 27312), `--adhoc-bind <address>` (default 0.0.0.0).

## What PSPC connects to, and when

Only what the guest asks for: a game's (or the XMB's) sockets, its HTTP requests and name
lookups, and - for ad hoc - the configured ad hoc server and the peers it names. Nothing is opened
at start-up, and nothing listens unless the guest listens (a game's server socket, an ad hoc PTP
listen) or you run `--adhoc-server`. Before the access point is connected only loopback is
reachable, as on a PSP without a network; a game's wildcard binds take the virtual access point's
address (the host's), and ad hoc sockets bind to the ad hoc address.

## How it works

| Piece | Where |
|---|---|
| Host sockets with readiness events, the poller thread, PSP errno / sockaddr / socket option conversion, the host's network configuration | `src/net/host_net.*` (library `pspc_net`) |
| HTTP/HTTPS requests (WinHTTP) | `src/net/http_client.*` |
| Ad hoc server and protocol | `src/net/adhoc_server.*`, `src/net/adhoc_proto.h` |
| Guest side: blocking waits, library threads, configuration, MAC/instance | `src/runtime/hle/modules/net_host.cpp`, `net_internal.h` |
| sceNet, sceNetInet, sceNetApctl, sceNetResolver, sceWlanDrv | `net.cpp` |
| sceHttp, sceSsl | `net_http.cpp` |
| sceNp, sceNpAuth, sceNpService (PSN: PSPC Network), redirects to revival servers | `net_np.cpp`, `net_infra.cpp` - see [pspc-network.md](pspc-network.md) |
| sceNetAdhoc, sceNetAdhocctl, sceNetAdhocMatching (+ offline versions) | `net_adhoc*.cpp`, `net_adhocctl.cpp` |
| Netconf utility dialog's connect logic | `src/runtime/dialogs/simple_dialogs.cpp` |

Host sockets are always non-blocking. A guest call that would block puts its guest thread to sleep
(`net::WaitHostEvents`) until the poller thread sees the socket's event, the guest's time-out
passes or the socket is closed/aborted; other guest threads keep running and the emulation thread
never waits on the network. Blocking host APIs (getaddrinfo, WinHTTP) run on worker threads
(`net::RunBlockingOnHost`). The libraries' own threads (SceNetApctl, SceNetAdhocctl, the matching
threads) are guest threads that run native code and call the game's handlers.

With `--fast-time`, idle time normally jumps ahead; while host sockets are open or a host call is
pending it passes in real time instead (`sched::HoldRealTime`), so network time-outs keep their
meaning.

Behaviour is ported from PPSSPP (sceNetInet, sceNetApctl, sceNetResolver, sceHttp, sceNetAdhoc*,
the ad hoc server; GPL-2.0-or-later, marked in the files), with PSPC's blocking model instead of
PPSSPP's delays.

## Testing

```bat
build\RelWithDebInfo\bin\pspc_unittests.exe net_         :: socket layer and poller over loopback
build\RelWithDebInfo\bin\pspc_unittests.exe adhoc        :: ad hoc server with two fake clients
python -I tools\net_selftest.py                          :: end-to-end scenarios (see below)
python -I tools\run_autotests.py net/                    :: pspautotests
```

`tools/net_selftest.py` runs `pspc --net-selftest <scenario>`: a test program written in C++ that
calls the libraries through their real syscall stubs on guest threads with guest memory (PSPC's
toolchain has no MIPS compiler, so it stands in for homebrew). Scenarios: `inet` (access point,
resolver, TCP/UDP over loopback with blocking calls, select/poll, time-outs, abort), `http` (sceHttp
against a local server: redirects, chunked and large bodies, POST, HEAD, errors; `--internet` adds
http/https to example.com), `adhoc` and `adhoc-gamemode` (two instances and the ad hoc server:
matching, PDP, PTP, game mode), `adhoc-dialog`, `adhoc-noserver`, `adhoc-offline`. Add
`--fast-time` to run them in fast-time mode.

## Limits

- One virtual access point; network configurations created in the XMB aren't stored.
- IPv4 only. Raw sockets need administrator rights on Windows (CAP_NET_RAW on Linux). Control messages of
  sceNetInetSendmsg/Recvmsg aren't passed on.
- Without that right, a raw ICMP socket still opens, and every echo request sent on it is answered at once by PSPC
  (a reply from the address pinged, as a raw socket receives it), whether or not the address answers. Games that ping
  the servers of their list (Star Wars Super Squadron) show a server only after its reply, so none would be listed
  otherwise; the ping they show reads 0.
- sceNetResolver's asynchronous calls complete before they return.
- sceHttp: no HTTP cache (the browser's cache calls succeed and do nothing); cookies live in each
  request's WinHTTP session; auth/redirect/cookie callbacks are stored, never called; sceSsl
  certificate information calls return nothing.
- Windows retries a refused loopback TCP connect for about two seconds before reporting it.
- Ad hoc: no relay mode or UPnP; the server doesn't link different regions of a game; chat is
  only logged; the port offset is fixed at 10000; all instances use the nickname "PSPC";
  sceNetAdhocDiscover finds nobody. Several instances on one PC need a loopback server address.
