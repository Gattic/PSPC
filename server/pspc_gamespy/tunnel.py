# SPDX-License-Identifier: GPL-2.0-or-later
"""The server's side of the tunnel: players reach it over WebSockets, so it can sit behind a
Cloudflare Tunnel, which carries only HTTP(S).

Games talk plain TCP and UDP to fixed ports (29900, 28910, 27900, ...). A Cloudflare Tunnel (or any
web proxy) can't carry those, so a small program on each player's PC (relay.py, which PSPC starts for
a game that has a public server) listens on those ports at 127.77.0.1, where the game was told the
server is, and passes everything over WebSockets to the gateway here, on the web server's port:

    GET /tunnel/info              the ports the relay should listen on (JSON)
    GET /tunnel/tcp/<port>        WebSocket: one TCP connection to the service on that port; messages are the bytes
    GET /tunnel/udp               WebSocket: one UDP socket; messages are [port, 2 bytes big-endian][datagram]

The gateway opens the real connection to the real service on this machine. It does that from an
address of its own for each player (access.TUNNEL_NET), so the services see one player per address,
as they would on the internet - limits, bans, "who is online" and the server list all work - and a
player's UDP socket keeps its port for as long as the WebSocket lives, as behind a NAT that keeps
its mappings. A host on this machine that is sent a player's address can answer it.

Only the services' own ports are reachable (TCP: Presence, search, server browser, stats and the web
pages; UDP: the master server, NAT negotiation and the games hosted on this machine), never an
arbitrary address or port. Players can't host games through the tunnel (qr.py): nobody could join them.

Behind a proxy (--behind-proxy) the player's own address comes from the proxy's CF-Connecting-IP or
X-Forwarded-For header, trusted only from this machine (web.py puts it in the request's peer)."""

import asyncio
import json
import logging
import struct
import time

from . import access
from . import ws as websocket

log = logging.getLogger('gamespy')

VIRTUAL_HOST = '127.77.0.1'     # where a tunnelled player's game finds the server: its relay listens here
PATH = '/tunnel/'

# The games' fixed ports, and the Config attribute that says where each service really listens.
TCP_SERVICES = {29900: 'gpcm_port', 29901: 'gpsp_port', 28910: 'sb_port', 29920: 'gstats_port', 80: 'http_port'}
UDP_SERVICES = {27900: 'qr_port', 27901: 'natneg_port'}

MAX_DATAGRAM = 2048
PORT_CACHE = 1.0                # seconds the list of reachable UDP ports is kept
PING_INTERVAL = 25
DEAD_AFTER = 90                 # seconds without a frame (not even a pong) before a WebSocket is dropped
UDP_IDLE = 300                  # seconds without a datagram before a UDP socket is closed
CONNECT_TIMEOUT = 5
ALIAS_LINGER = 120              # seconds a player keeps their address after their last connection closes


async def race(*coroutines):
    """Runs coroutines together until the first one is done, then stops the others."""
    tasks = [asyncio.ensure_future(c) for c in coroutines]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        for result in await asyncio.gather(*tasks, return_exceptions=True):
            if isinstance(result, Exception) and not isinstance(result, (ConnectionError, OSError)):
                log.warning('tunnel: %r', result)


class Aliases:
    """The address each tunnelled player appears at, for as long as they have connections (and a little
    longer, so that a quick reconnection is the same player)."""

    def __init__(self, size=4095, linger=ALIAS_LINGER):
        self.size = size
        self.linger = linger
        self.by_ip = {}                 # player's address -> [alias, connections, time the last one closed]
        self.taken = set()
        self.next = 1

    @staticmethod
    def address(n):
        return '127.64.%d.%d' % (n >> 8, n & 255)

    def acquire(self, ip):
        entry = self.by_ip.get(ip)
        if entry is None:
            alias = self._free()
            if alias is None:
                return None
            entry = self.by_ip[ip] = [alias, 0, 0.0]
            self.taken.add(alias)
        entry[1] += 1
        return entry[0]

    def _free(self):
        for attempt in (0, 1):
            for _ in range(self.size):
                alias = self.address(self.next)
                self.next = self.next % self.size + 1
                if alias not in self.taken:
                    return alias
            if attempt == 0:
                self.reap(0)            # all taken: take back the addresses nobody is using
        return None

    def release(self, ip):
        entry = self.by_ip.get(ip)
        if entry:
            entry[1] -= 1
            entry[2] = time.monotonic()

    def reap(self, linger=None):
        now = time.monotonic()
        linger = self.linger if linger is None else linger
        for ip, (alias, connections, closed) in list(self.by_ip.items()):
            if connections <= 0 and now - closed >= linger:
                del self.by_ip[ip]
                self.taken.discard(alias)

    def players(self):
        return sum(1 for e in self.by_ip.values() if e[1] > 0)


def http_response(status, body, content_type='text/plain'):
    reasons = {400: 'Bad Request', 403: 'Forbidden', 404: 'Not Found', 429: 'Too Many Requests', 503: 'Service Unavailable'}
    body = body.encode('utf-8') if isinstance(body, str) else body
    return ('HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' %
            (status, reasons.get(status, 'OK'), content_type, len(body))).encode('latin-1') + body


class Gateway:
    def __init__(self, config, registry, limits, name='PSPC GameSpy server'):
        self.config = config
        self.registry = registry
        self.limits = limits
        self.name = name
        self.aliases = Aliases()
        self.flows = 0
        self.flows_by_ip = {}
        self.udp_sockets_by_ip = {}
        self.opens = access.RateLimiter(30, 3.0)            # connections a player opens, per second
        self.max_flows = 2000                                # each is about three descriptors here (WebSocket, upstream, service's side)
        self.max_flows_per_ip = 48
        self.max_udp_per_ip = 6
        self.upstream = (1 << 20, 256 << 10)                # a connection may send a megabyte at once, then 256 KB a second
        self.upstream_packets = (600, 300.0)                # a UDP socket's datagrams: 600 at once, then 300 a second
        # Where the services are: on 127.0.0.1 unless the server was told to listen on one loopback address only.
        self.local = '127.0.0.1' if config.bind == '0.0.0.0' else config.bind
        self._udp_ports = (0.0, {}, {})                   # when they were looked up, public port -> (address, port), local port -> public port

    # ---- what is reachable

    def tcp_ports(self):
        """The games' TCP ports -> where those services listen here."""
        return {public: getattr(self.config, attr) for public, attr in TCP_SERVICES.items()}

    def udp_ports(self):
        """The ports a player's UDP socket may send to -> where they are here ((address, port)): the
        services, and the games hosted on this machine (their ports are where their heartbeats come from)."""
        return self._lookup_udp_ports()[1]

    def _lookup_udp_ports(self):
        now = time.monotonic()
        if now - self._udp_ports[0] > PORT_CACHE:
            ports = {public: (self.local, getattr(self.config, attr)) for public, attr in UDP_SERVICES.items()}
            for host in list(self.registry.hosts.values()):
                if access.is_local(host.ip):
                    ports.setdefault(host.port, (host.ip, host.port))
            self._udp_ports = (now, ports, {local[1]: public for public, local in ports.items()})
        return self._udp_ports

    def info(self):
        return {'name': self.name, 'tcp': sorted(self.tcp_ports()), 'udp': sorted(self.udp_ports())}

    def status(self):
        return {'players': self.aliases.players(), 'connections': self.flows}

    # ---- requests

    async def handle(self, req, reader, writer):
        """Answers a request under /tunnel/ (the web server hands it over, with the connection)."""
        ip = req.peer[0]
        reply = None
        if access.is_tunnel(ip):                         # a player's connection to the web pages, asking for a tunnel inside it
            reply = http_response(403, 'Not through the tunnel')
        elif not self.limits.allow(self.opens, ip):
            reply = http_response(429, 'Too many connections')
        elif req.path == PATH + 'info':
            reply = http_response(200, json.dumps(self.info()), 'application/json')
        else:
            kind, port = self.route(req.path)
            if kind is None or (kind == 'tcp' and port not in self.tcp_ports()):
                reply = http_response(404, 'Not found')
            elif 'origin' in req.headers:               # a web page, not a relay
                reply = http_response(403, 'Not for browsers')
            elif not websocket.wants_upgrade(req.headers):
                reply = http_response(400, 'A WebSocket is needed')
            elif self.flows >= self.max_flows or self.flows_by_ip.get(ip, 0) >= self.max_flows_per_ip:
                log.info('tunnel: too many connections (%s has %d, %d in all)', access.safe(ip), self.flows_by_ip.get(ip, 0), self.flows)
                reply = http_response(429, 'Too many connections')
        if reply is not None:
            writer.write(reply)
            await writer.drain()
            writer.close()
            return
        alias = self.aliases.acquire(ip)
        if alias is None:
            writer.write(http_response(503, 'The server is full'))
            await writer.drain()
            writer.close()
            return
        if self.aliases.by_ip[ip][1] == 1:
            log.info('tunnel: %s appears as %s', access.safe(ip), alias)
        self.flows += 1
        self.flows_by_ip[ip] = self.flows_by_ip.get(ip, 0) + 1
        try:
            sock = await websocket.accept(req.headers, reader, writer)
        except (websocket.WebSocketError, ConnectionError, OSError) as e:
            self._left(ip)
            try:
                if isinstance(e, websocket.WebSocketError):
                    writer.write(http_response(400, str(e)))
                    await writer.drain()
            except (ConnectionError, OSError):
                pass
            writer.close()
            return
        keepalive = asyncio.ensure_future(self._keepalive(sock))
        try:
            if kind == 'tcp':
                await self._tcp(sock, alias, port)
            else:
                await self._udp(sock, ip, alias)
        except (ConnectionError, OSError, asyncio.TimeoutError) as e:
            log.debug('tunnel: %s (%s): %s', access.safe(ip), kind, e)
        finally:
            keepalive.cancel()
            await sock.close()
            self._left(ip)

    def _left(self, ip):
        self.flows -= 1
        self.flows_by_ip[ip] -= 1
        if self.flows_by_ip[ip] <= 0:
            del self.flows_by_ip[ip]
        self.aliases.release(ip)

    @staticmethod
    def route(path):
        """('tcp', public port), ('udp', 0) or (None, 0) for a request's path."""
        if path == PATH + 'udp':
            return 'udp', 0
        if path.startswith(PATH + 'tcp/'):
            try:
                return 'tcp', int(path[len(PATH + 'tcp/'):])
            except ValueError:
                pass
        return None, 0

    async def _keepalive(self, sock):
        """Pings (the proxy drops silent connections) and drops a peer that has gone."""
        while not sock.closed:
            await asyncio.sleep(PING_INTERVAL)
            if time.monotonic() - sock.last_heard > DEAD_AFTER:
                log.debug('tunnel: a connection went silent')
                await sock.close()
                return
            try:
                await sock.ping()
            except (ConnectionError, OSError):
                return

    # ---- TCP

    async def _tcp(self, sock, alias, public_port):
        local_port = self.tcp_ports()[public_port]
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.local, local_port, local_addr=(alias, 0)), CONNECT_TIMEOUT)
        except (OSError, asyncio.TimeoutError):
            await sock.close(websocket.CLOSE_ERROR)
            return
        meter = access.RateLimiter(*self.upstream, max_keys=1)

        async def to_service():
            while True:
                data = await sock.recv()
                if data is None:
                    return
                if not meter.allow('flow', len(data)):
                    log.info('tunnel: %s sent too much; closed', alias)
                    return
                writer.write(data)
                await writer.drain()

        async def to_player():
            while True:
                data = await reader.read(16384)
                if not data:
                    return
                await sock.send(data)

        try:
            await race(to_service(), to_player())
        finally:
            writer.close()

    # ---- UDP

    async def _udp(self, sock, ip, alias):
        if self.udp_sockets_by_ip.get(ip, 0) >= self.max_udp_per_ip:
            await sock.close(websocket.CLOSE_ERROR)
            return
        loop = asyncio.get_running_loop()
        outgoing = asyncio.Queue(maxsize=256)
        endpoint = UdpEndpoint(self, outgoing)
        transport, _ = await loop.create_datagram_endpoint(lambda: endpoint, local_addr=(alias, 0))
        self.udp_sockets_by_ip[ip] = self.udp_sockets_by_ip.get(ip, 0) + 1
        meter = access.RateLimiter(*self.upstream_packets, max_keys=1)
        bytes_meter = access.RateLimiter(*self.upstream, max_keys=1)

        async def to_services():
            while True:
                message = await sock.recv()
                if message is None:
                    return
                if len(message) < 3 or len(message) > MAX_DATAGRAM + 2:
                    continue
                if not meter.allow('socket') or not bytes_meter.allow('socket', len(message)):
                    continue                                    # datagrams over the rate are dropped, as on a slow link
                endpoint.last_data = time.monotonic()
                destination = self.udp_ports().get(struct.unpack_from('>H', message)[0])
                if destination is not None:
                    transport.sendto(message[2:], destination)

        async def to_player():
            while True:
                await sock.send(await outgoing.get())

        async def idle():
            while time.monotonic() - endpoint.last_data < UDP_IDLE:
                await asyncio.sleep(10)

        try:
            await race(to_services(), to_player(), idle())
        finally:
            transport.close()
            self.udp_sockets_by_ip[ip] -= 1
            if self.udp_sockets_by_ip[ip] <= 0:
                del self.udp_sockets_by_ip[ip]


class UdpEndpoint(asyncio.DatagramProtocol):
    """A tunnelled player's UDP socket, here: what the services and hosts on this machine send it goes to the player."""

    def __init__(self, gateway, outgoing):
        self.gateway = gateway
        self.outgoing = outgoing
        self.last_data = time.monotonic()

    def datagram_received(self, data, addr):
        if not access.is_local(addr[0]) or len(data) > MAX_DATAGRAM:
            return
        public = self.gateway._lookup_udp_ports()[2].get(addr[1])
        if public is None:
            return                                              # not a service or host the player may hear from
        self.last_data = time.monotonic()
        try:
            self.outgoing.put_nowait(struct.pack('>H', public) + data)
        except asyncio.QueueFull:
            pass
