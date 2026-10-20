# SPDX-License-Identifier: GPL-2.0-or-later
"""The relay: a program on a player's PC that lets their game reach a public server through a
Cloudflare Tunnel (or any web proxy). PSPC starts it for a game whose server is public
(infrastructure.json: "tunnel"); to run it by hand:

    python -m pspc_gamespy tunnel https://swss.pspc.online

The game is told that the server is at 127.77.0.1 (the redirects of data/infrastructure.json), and
looks for it at the ports GameSpy uses. The relay listens there - TCP for Presence, the server
browser, stats and the web pages; UDP for the master server, NAT negotiation and the hosted games -
and passes each connection to the server's gateway (tunnel.py) over a WebSocket, TLS-protected for
https:// addresses and checked against the system's certificates.

- Each TCP connection of the game is one WebSocket.
- The game's UDP sockets are one WebSocket each (the server sends from an address of its own for each),
  carrying datagrams for any of the ports, tagged with the port.
- The ports to listen on come from the server (GET /tunnel/info), and are looked at again every few
  seconds: games hosted on the server's machine each have a port of their own.

The relay ends when the PSPC that started it does (--parent-pid), and tries again while the server
can't be reached."""

import asyncio
import http.client
import json
import logging
import os
import struct
import sys
import time
import urllib.request

from . import __version__
from . import ws as websocket
from .tunnel import MAX_DATAGRAM, VIRTUAL_HOST, race

log = logging.getLogger('gamespy')

REFRESH_INTERVAL = 4
UDP_IDLE = 120                  # seconds a UDP socket of the game may be silent before its WebSocket is closed
MAX_UDP_FLOWS = 16
MAX_PORTS = 512                 # of each kind: what the server may ask a relay to listen on
QUEUE = 64                      # datagrams waiting for a WebSocket to open
USER_AGENT = ('User-Agent', 'PSPC-relay/%s' % __version__)


def normalize(url):
    """'swss.pspc.online' -> 'https://swss.pspc.online'; no trailing slash. ValueError for an address
    that isn't a web address (urllib would open a file:// one)."""
    if '://' not in url:
        url = 'https://' + url
    websocket.split_url(url)
    return url.rstrip('/')


def process_alive(pid):
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        handle = kernel32.OpenProcess(0x100000, False, pid)         # SYNCHRONIZE
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(ctypes.c_void_p(handle), 0) == 0x102     # WAIT_TIMEOUT: still running
        finally:
            kernel32.CloseHandle(ctypes.c_void_p(handle))
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    return True


class UdpFlow:
    """One UDP socket of the game, and the WebSocket that stands for it."""

    def __init__(self, addr):
        self.addr = addr
        self.queue = asyncio.Queue(maxsize=QUEUE)
        self.last = time.monotonic()
        self.task = None


class UdpPort(asyncio.DatagramProtocol):
    def __init__(self, relay, port):
        self.relay = relay
        self.port = port
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.relay.from_game(self.port, data, addr)

    def error_received(self, exc):
        pass                            # an ICMP error for a datagram sent to the game, which has closed its socket


class Relay:
    def __init__(self, url, listen=VIRTUAL_HOST):
        self.base = normalize(url)
        self.listen = listen
        self.tcp_servers = {}           # port -> asyncio server
        self.udp_ports = {}             # port -> UdpPort
        self.unavailable = {}           # (kind, port) -> when it was last reported that the port can't be listened on
        self.gone = {}                  # port -> when the server stopped listing it
        self.flows = {}                 # the game's UDP address -> UdpFlow
        self.last_refresh = 0.0
        self.ready = asyncio.Event()

    # ---- the server's ports

    def _fetch_info(self):
        request = urllib.request.Request(self.base + '/tunnel/info', headers=dict([USER_AGENT]))
        with urllib.request.urlopen(request, timeout=10) as r:
            info = json.loads(r.read(65536))
        ports = {}
        for kind in ('tcp', 'udp'):
            ports[kind] = sorted({p for p in info[kind][:MAX_PORTS] if isinstance(p, int) and 0 < p < 65536})
        return ports

    async def refresh(self):
        """Listens on the ports the server has now. False if it couldn't be asked."""
        self.last_refresh = time.monotonic()
        try:
            ports = await asyncio.get_running_loop().run_in_executor(None, self._fetch_info)
        except (OSError, http.client.HTTPException, ValueError, KeyError, TypeError) as e:
            log.warning('the server at %s can not be reached: %s', self.base, e)
            return False
        loop = asyncio.get_running_loop()
        for port in ports['tcp']:
            if port not in self.tcp_servers:
                try:
                    self.tcp_servers[port] = await asyncio.start_server(
                        lambda r, w, p=port: self._tcp_connection(p, r, w), self.listen, port)
                    log.info('TCP %s:%d', self.listen, port)
                except OSError as e:
                    self._unavailable('TCP', port, e)
        for port in ports['udp']:
            self.gone.pop(port, None)
            if port not in self.udp_ports:
                try:
                    transport, protocol = await loop.create_datagram_endpoint(
                        lambda p=port: UdpPort(self, p), local_addr=(self.listen, port))
                    self.udp_ports[port] = protocol
                    log.info('UDP %s:%d', self.listen, port)
                except OSError as e:
                    self._unavailable('UDP', port, e)
        now = time.monotonic()
        for port in list(self.udp_ports):
            if port in ports['udp']:
                continue
            if now - self.gone.setdefault(port, now) > 60:       # a game that ended: its port is let go after a minute
                protocol = self.udp_ports.pop(port)
                del self.gone[port]
                if protocol.transport:
                    protocol.transport.close()
        self.ready.set()
        return True

    def _unavailable(self, kind, port, error):
        """A port taken by something else (another PSPC's relay: it is tried again, and taken over when that one ends) or one
        a program may not listen on (Linux: below 1024, such as the web pages' 80). Said once in a while."""
        now = time.monotonic()
        if now - self.unavailable.get((kind, port), -1e9) > 300:
            log.warning("can't listen on %s %s:%d: %s", kind, self.listen, port, error.strerror or error)
            self.unavailable[(kind, port)] = now

    async def keep_ports_current(self):
        while True:
            await asyncio.sleep(REFRESH_INTERVAL)
            await self.refresh()

    # ---- TCP: one WebSocket for each connection of the game

    async def _tcp_connection(self, port, reader, writer):
        if time.monotonic() - self.last_refresh > 1:             # the game is about to ask for the server's list: have the ports
            asyncio.ensure_future(self.refresh())
        try:
            sock = await websocket.connect('%s/tunnel/tcp/%d' % (self.base, port), headers=[USER_AGENT])
        except (OSError, websocket.WebSocketError, asyncio.TimeoutError) as e:
            log.warning('TCP %d: the server can not be reached: %s', port, e)
            writer.close()
            return

        async def to_server():
            while True:
                data = await reader.read(16384)
                if not data:
                    return
                await sock.send(data)

        async def to_game():
            while True:
                data = await sock.recv()
                if data is None:
                    return
                writer.write(data)
                await writer.drain()

        try:
            await race(to_server(), to_game())
        finally:
            await sock.close()
            writer.close()

    # ---- UDP: one WebSocket for each socket of the game

    def from_game(self, port, data, addr):
        if len(data) > MAX_DATAGRAM:
            return
        flow = self.flows.get(addr)
        if flow is None:
            if len(self.flows) >= MAX_UDP_FLOWS:
                return
            flow = self.flows[addr] = UdpFlow(addr)
            flow.task = asyncio.ensure_future(self._udp_flow(flow))
        flow.last = time.monotonic()
        try:
            flow.queue.put_nowait(struct.pack('>H', port) + data)
        except asyncio.QueueFull:
            pass                                                # a datagram lost, as UDP may

    async def _udp_flow(self, flow):
        try:
            sock = await websocket.connect(self.base + '/tunnel/udp', headers=[USER_AGENT])
        except (OSError, websocket.WebSocketError, asyncio.TimeoutError) as e:
            log.warning('UDP: the server can not be reached: %s', e)
            self.flows.pop(flow.addr, None)
            return

        async def to_server():
            while True:
                await sock.send(await flow.queue.get())

        async def to_game():
            while True:
                message = await sock.recv()
                if message is None:
                    return
                if len(message) < 3:
                    continue
                flow.last = time.monotonic()
                port = self.udp_ports.get(struct.unpack_from('>H', message)[0])
                if port is not None and port.transport is not None:
                    port.transport.sendto(message[2:], flow.addr)

        async def idle():
            while time.monotonic() - flow.last < UDP_IDLE:
                await asyncio.sleep(10)

        try:
            await race(to_server(), to_game(), idle())
        finally:
            self.flows.pop(flow.addr, None)
            await sock.close()

    # ---- running

    async def run(self, parent_pid=None):
        """Until the program that started the relay ends (if there is one), however the server is doing."""
        tasks = [self._serve()]
        if parent_pid:
            tasks.append(_watch_parent(parent_pid))
        await race(*tasks)

    async def _serve(self):
        delay = 1
        while not await self.refresh():
            await asyncio.sleep(delay)
            delay = min(delay * 2, 15)
        log.info('relay ready: %s reaches the server at %s', self.listen, self.base)
        await self.keep_ports_current()


async def _watch_parent(pid):
    while process_alive(pid):
        await asyncio.sleep(2)
    log.info('the program that started the relay (%d) has ended', pid)


def main(args):
    async def run():
        await Relay(args.url, args.listen).run(args.parent_pid)     # made here: an Event belongs to the loop that is running

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit('use: python -m pspc_gamespy tunnel URL')
