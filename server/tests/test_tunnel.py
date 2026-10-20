# SPDX-License-Identifier: GPL-2.0-or-later
"""The tunnel (tunnel.py, relay.py, ws.py): a game's TCP and UDP reach a server that is only
reachable over HTTP, through a relay on the player's PC and a gateway on the server.

ServerTests runs a whole server with its gateway and a relay, on spare ports, and drives them as a
game does: the relay listens where the game looks for the server (127.77.0.1), and what the game
sends there ends up at the real services - Presence, the server browser, a dedicated game's own UDP
port - from an address of the player's own, with the answers coming back."""

import asyncio
import json
import logging
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from pspc_gamespy import access, crypt, kv, qr, relay, tunnel, ws  # noqa: E402
from pspc_gamespy.server import Config, GameSpyServer  # noqa: E402
from pspc_gamespy.store import Store  # noqa: E402
from test_gamespy import GAMENAME, SECRET, _free_port, heartbeat  # noqa: E402


def can_bind(address):
    s = socket.socket()
    try:
        s.bind((address, 0))
        return True
    except OSError:
        return False
    finally:
        s.close()


def run(coro, timeout=10):
    return asyncio.run(asyncio.wait_for(coro, timeout))


class WebSocketTests(unittest.TestCase):
    async def echo_server(self, ssl_context=None):
        async def handle(reader, writer):
            head = await reader.readuntil(b'\r\n\r\n')
            headers = {}
            for line in head.decode().split('\r\n')[1:]:
                name, _, value = line.partition(':')
                headers[name.strip().lower()] = value.strip()
            sock = await ws.accept(headers, reader, writer, max_message=1000)
            while True:
                message = await sock.recv()
                if message is None:
                    break
                await sock.send(message[::-1])
            await sock.close()
        server = await asyncio.start_server(handle, '127.0.0.1', 0, ssl=ssl_context)
        return server, server.sockets[0].getsockname()[1]

    def test_messages_both_ways(self):
        async def main():
            server, port = await self.echo_server()
            sock = await ws.connect('http://127.0.0.1:%d/x' % port)
            for payload in (b'', b'a', b'hello' * 20, bytes(range(256)) * 3, b'z' * 999):     # empty, short, 16-bit length
                await sock.send(payload)
                self.assertEqual(await sock.recv(), payload[::-1])
            await sock.send(b'q' * 1001)                                                     # over the server's limit
            self.assertIsNone(await sock.recv())
            server.close()
        run(main())

    def test_fragments_pings_and_bad_frames(self):
        async def main():
            server, port = await self.echo_server()
            reader, writer = await asyncio.open_connection('127.0.0.1', port)
            writer.write(b'GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                         b'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n')
            head = await reader.readuntil(b'\r\n\r\n')
            self.assertIn(b'Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=', head)      # RFC 6455's example

            def frame(opcode, payload, fin=True, key=b'\x01\x02\x03\x04'):
                masked = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
                return bytes([(0x80 if fin else 0) | opcode, 0x80 | len(payload)]) + key + masked

            writer.write(frame(ws.OP_BINARY, b'ab', fin=False) + frame(ws.OP_PING, b'p') + frame(ws.OP_CONTINUATION, b'cd'))
            pong = await reader.readexactly(3)
            self.assertEqual(pong, bytes([0x80 | ws.OP_PONG, 1]) + b'p')
            reply = await reader.readexactly(2 + 4)
            self.assertEqual(reply[2:], b'dcba')                                            # the fragments made one message
            writer.write(bytes([0x80 | ws.OP_BINARY, 1]) + b'x')                            # not masked: a client must mask
            close = await reader.readexactly(4)
            self.assertEqual((close[0] & 0x0F, struct.unpack('>H', close[2:])[0]), (ws.OP_CLOSE, ws.CLOSE_PROTOCOL))
            writer.close()
            server.close()
        run(main())

    def test_a_handshake_that_is_not_one_is_refused(self):
        self.assertFalse(ws.wants_upgrade({'upgrade': 'h2c'}))
        self.assertTrue(ws.wants_upgrade({'upgrade': 'WebSocket', 'connection': 'keep-alive, Upgrade'}))

        class Writer:
            def write(self, data):
                pass

            async def drain(self):
                pass

        async def main():
            with self.assertRaises(ws.WebSocketError):
                await ws.accept({'sec-websocket-key': 'short', 'sec-websocket-version': '13'}, None, Writer())
            with self.assertRaises(ws.WebSocketError):
                await ws.accept({'sec-websocket-key': 'dGhlIHNhbXBsZSBub25jZQ==', 'sec-websocket-version': '8'}, None, Writer())
        run(main())

    @unittest.skipUnless(shutil.which('openssl'), 'needs the openssl program to make a certificate')
    def test_wss_checks_the_certificate(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        cert, key = os.path.join(tmp, 'cert.pem'), os.path.join(tmp, 'key.pem')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '2',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(cert, key)
        trusting = ssl.create_default_context(cafile=cert)

        async def main():
            server, port = await self.echo_server(server_context)
            sock = await ws.connect('wss://localhost:%d/x' % port, ssl_context=trusting)
            await sock.send(b'abc')
            self.assertEqual(await sock.recv(), b'cba')
            await sock.close()
            with self.assertRaises(ssl.SSLError):                                           # a certificate nobody vouches for
                await ws.connect('wss://localhost:%d/x' % port)
            server.close()
        run(main())


class AliasTests(unittest.TestCase):
    def test_a_player_keeps_their_address_and_others_get_their_own(self):
        aliases = tunnel.Aliases(size=3, linger=0.05)
        a = aliases.acquire('203.0.113.1')
        self.assertEqual(aliases.acquire('203.0.113.1'), a)
        b, c = aliases.acquire('203.0.113.2'), aliases.acquire('203.0.113.3')
        self.assertEqual(len({a, b, c}), 3)
        self.assertTrue(all(access.is_tunnel(x) and not access.is_local(x) for x in (a, b, c)))
        self.assertIsNone(aliases.acquire('203.0.113.4'))                                   # all in use
        aliases.release('203.0.113.1')
        self.assertIsNone(aliases.acquire('203.0.113.4'))                                   # one is still connected
        aliases.release('203.0.113.1')
        time.sleep(0.1)
        self.assertEqual(aliases.acquire('203.0.113.4'), a)                                 # taken back after the linger
        self.assertEqual(aliases.players(), 3)


@unittest.skipUnless(can_bind('127.77.0.1') and can_bind('127.64.0.1'), 'needs 127.x.y.z addresses')
class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        logging.getLogger('gamespy').setLevel(logging.ERROR)
        cls.saved = (dict(tunnel.TCP_SERVICES), dict(tunnel.UDP_SERVICES), tunnel.PORT_CACHE, relay.REFRESH_INTERVAL)
        # Stand-ins for GameSpy's fixed ports, which may be taken (or not allowed) here.
        cls.public_tcp = {attr: _free_port() for attr in ('gpcm_port', 'gpsp_port', 'sb_port', 'gstats_port', 'http_port')}
        cls.public_udp = {attr: _free_port(socket.SOCK_DGRAM) for attr in ('qr_port', 'natneg_port')}
        tunnel.TCP_SERVICES.clear()
        tunnel.TCP_SERVICES.update({port: attr for attr, port in cls.public_tcp.items()})
        tunnel.UDP_SERVICES.clear()
        tunnel.UDP_SERVICES.update({port: attr for attr, port in cls.public_udp.items()})
        tunnel.PORT_CACHE = 0.2
        relay.REFRESH_INTERVAL = 0.3
        cls.tmp = tempfile.mkdtemp()
        cls.store = Store(os.path.join(cls.tmp, 'gamespy.sqlite'))
        cls.config = Config(public_host=tunnel.VIRTUAL_HOST, bind='127.0.0.1', gpcm_port=_free_port(), gpsp_port=_free_port(),
                            qr_port=_free_port(socket.SOCK_DGRAM), natneg_port=_free_port(socket.SOCK_DGRAM),
                            sb_port=_free_port(), gstats_port=_free_port(), http_port=_free_port(), tunnel=True, behind_proxy=True)
        cls.server = GameSpyServer(cls.config, cls.store)
        cls.loop = asyncio.new_event_loop()
        cls.thread = threading.Thread(target=cls.loop.run_forever, daemon=True)
        cls.thread.start()
        asyncio.run_coroutine_threadsafe(cls.server.start(), cls.loop).result(10)
        cls.relay = relay.Relay('http://127.0.0.1:%d' % cls.config.http_port)
        cls.relay_task = asyncio.run_coroutine_threadsafe(cls.relay.run(), cls.loop)
        asyncio.run_coroutine_threadsafe(cls.relay.ready.wait(), cls.loop).result(10)

    @classmethod
    def tearDownClass(cls):
        cls.relay_task.cancel()

        async def stop():
            for server in cls.relay.tcp_servers.values():
                if server:
                    server.close()
            for port in cls.relay.udp_ports.values():
                if port.transport:
                    port.transport.close()
            for flow in list(cls.relay.flows.values()):
                flow.task.cancel()
            await asyncio.sleep(0.3)
            cls.server.close()
            for s in cls.server._servers:
                try:
                    await asyncio.wait_for(s.wait_closed(), 2)      # waits for every connection to end, in Python 3.12
                except asyncio.TimeoutError:
                    pass
        asyncio.run_coroutine_threadsafe(stop(), cls.loop).result(10)
        cls.loop.call_soon_threadsafe(cls.loop.stop)
        cls.thread.join(5)
        cls.loop.close()
        cls.store.close()
        tunnel.TCP_SERVICES.clear()
        tunnel.TCP_SERVICES.update(cls.saved[0])
        tunnel.UDP_SERVICES.clear()
        tunnel.UDP_SERVICES.update(cls.saved[1])
        tunnel.PORT_CACHE, relay.REFRESH_INTERVAL = cls.saved[2:]
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def game_tcp(self, attr):
        s = socket.create_connection((tunnel.VIRTUAL_HOST, self.public_tcp[attr]), timeout=5)
        self.addCleanup(s.close)
        return s

    def message(self, s, buf):
        while b'\\final\\' not in buf[0]:
            data = s.recv(65536)
            if not data:
                raise ConnectionError('closed')
            buf[0] += data
        msg, _, buf[0] = buf[0].partition(b'\\final\\')
        return kv.parse(msg.decode('latin-1'))

    def login(self, nick):
        s, buf = self.game_tcp('gpcm_port'), [b'']
        server_chal = self.message(s, buf)['challenge']
        email = '%s@example.com' % nick.lower()
        s.sendall(kv.build(('newuser', ''), ('email', email), ('nick', nick), ('passwordenc', crypt.encode_password('secret')),
                           ('namespaceid', 0), ('id', 1)).encode())
        self.assertEqual(self.message(s, buf).command, 'nur')
        user = '%s@%s' % (nick, email)
        s.sendall(kv.build(('login', ''), ('challenge', 'C' * 32), ('user', user),
                           ('response', crypt.login_hash(crypt.md5hex('secret'), user, 'C' * 32, server_chal)), ('port', 3658),
                           ('id', 1)).encode())
        reply = self.message(s, buf)
        self.assertEqual(reply.command, 'lc', reply.pairs)
        self.assertEqual(reply['proof'], crypt.login_hash(crypt.md5hex('secret'), user, server_chal, 'C' * 32))
        return s, reply

    def on_loop(self, fn):
        async def call():
            return fn()
        return asyncio.run_coroutine_threadsafe(call(), self.loop).result(5)

    # ---- the relay

    def test_the_relay_listens_where_the_server_says(self):
        for port in list(self.public_tcp.values()):
            self.assertIn(port, self.relay.tcp_servers)
        for port in self.public_udp.values():
            self.assertIn(port, self.relay.udp_ports)

    def test_presence_through_the_tunnel_with_an_address_for_the_player(self):
        s, reply = self.login('Tina')
        peer = self.on_loop(lambda: [x.peer[0] for x in self.server.presence.online() if x.profile['nick'] == 'Tina'])[0]
        self.assertTrue(access.is_tunnel(peer) and not access.is_local(peer), peer)        # not 127.0.0.1: a player of their own
        tunnel_status = self.on_loop(self.server.status)['tunnel']
        self.assertGreaterEqual(tunnel_status['connections'], 1)
        s.sendall(kv.build(('logout', '')).encode())

    def test_players_are_told_where_to_find_a_game_on_the_servers_machine(self):
        """A dedicated game is on the server's machine, at 127.0.0.1; a tunnelled player is given the relay's address
        for it, and what they send there reaches the game, which can answer."""
        game = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        game.bind(('127.0.0.1', 0))
        game.settimeout(5)
        self.addCleanup(game.close)
        port = game.getsockname()[1]
        instance = b'\x0a\x0b\x0c\x0d'
        master = ('127.0.0.1', self.config.qr_port)
        keys = {'gamename': GAMENAME, 'hostname': 'Dedicated', 'mapname': 'Hoth', 'gamemode': 'openplaying'}
        game.sendto(heartbeat(instance, keys, statechanged=3), master)
        challenge = game.recv(256)
        game.sendto(bytes([qr.CHALLENGE]) + instance + challenge[7:], master)
        self.assertEqual(game.recv(64)[2], qr.CLIENT_REGISTERED)

        # The player asks the server browser, through the relay, for the list.
        sb = self.game_tcp('sb_port')
        body = (bytes([1, 3]) + struct.pack('>I', 0) + GAMENAME.encode() + b'\0' + GAMENAME.encode() + b'\0' + b'ABCDEFGH' +
                b"gamemode='openplaying'\0\\hostname\0" + struct.pack('>I', 0))
        sb.sendall(struct.pack('>H', len(body) + 3) + b'\0' + body)
        data = b''
        while True:
            data += sb.recv(65536)
            if len(data) > 1 and len(data) >= 2 + (data[0] ^ 0xEC) + (data[1 + (data[0] ^ 0xEC)] ^ 0xEA) + 20:
                break
        n1 = data[0] ^ 0xEC
        servchal = data[2 + n1:2 + n1 + (data[1 + n1] ^ 0xEA)]
        cipher = crypt.GOACipher(crypt.sb_crypt_key(b'ABCDEFGH', SECRET, servchal))
        plain = cipher.decrypt(data[2 + n1 + len(servchal):])
        self.assertIn(b'Dedicated', plain)
        self.assertIn(socket.inet_aton(tunnel.VIRTUAL_HOST), plain)                         # where the relay is, not 127.0.0.1

        # The relay learns the game's port (it asks the server every few moments), then the player's game talks to it.
        deadline = time.time() + 5
        while port not in self.relay.udp_ports and time.time() < deadline:
            time.sleep(0.05)
        self.assertIn(port, self.relay.udp_ports)
        player = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        player.bind(('127.0.0.1', 0))
        player.settimeout(5)
        self.addCleanup(player.close)
        player.sendto(b'hello from a player', (tunnel.VIRTUAL_HOST, port))
        got, seen_from = game.recvfrom(256)
        self.assertEqual(got, b'hello from a player')
        self.assertTrue(access.is_tunnel(seen_from[0]), seen_from)                          # the game sees the player's own address
        game.sendto(b'welcome', seen_from)
        reply, replied_from = player.recvfrom(256)
        self.assertEqual((reply, replied_from), (b'welcome', (tunnel.VIRTUAL_HOST, port)))   # from where the player sent to
        game.sendto(b'and again', seen_from)                                                 # the game may speak first, now
        self.assertEqual(player.recvfrom(256)[0], b'and again')

    def test_players_cannot_host_through_the_tunnel(self):
        player = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        player.bind(('127.0.0.1', 0))
        player.settimeout(1)
        self.addCleanup(player.close)
        keys = {'gamename': GAMENAME, 'hostname': 'Tunnelled host', 'gamemode': 'openplaying'}
        player.sendto(heartbeat(b'\x01\x02\x03\x04', keys, statechanged=3), (tunnel.VIRTUAL_HOST, self.public_udp['qr_port']))
        with self.assertRaises(socket.timeout):
            player.recv(256)
        self.assertNotIn('Tunnelled host', [h.name() for h in self.server.registry.hosts.values()])

    def test_web_pages_through_the_tunnel(self):
        s = self.game_tcp('http_port')
        s.sendall(b'GET /motd/motd.asp HTTP/1.1\r\nHost: motd.gamespy.com\r\nConnection: close\r\n\r\n')
        data = b''
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        self.assertTrue(data.startswith(b'HTTP/1.1 200'))
        self.assertIn(b'PSPC', data)

    # ---- the gateway

    def url(self, path):
        return 'http://127.0.0.1:%d%s' % (self.config.http_port, path)

    def test_info(self):
        info = json.load(urllib.request.urlopen(self.url('/tunnel/info'), timeout=5))
        self.assertEqual(info['tcp'], sorted(self.public_tcp.values()))
        self.assertTrue(set(self.public_udp.values()) <= set(info['udp']))

    def test_a_tunnel_cannot_be_opened_inside_a_tunnel(self):
        """Through the relay a player reaches the web pages from an address of their own; asking that port for
        a tunnel would give them connections (and an address) of their own again, without the limits that count them."""
        s = self.game_tcp('http_port')
        s.sendall(('GET /tunnel/udp HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                   'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        data = s.recv(4096)
        self.assertTrue(data.startswith(b'HTTP/1.1 403'), data[:40])

    def test_the_gateway_only_goes_where_it_should(self):
        for path, status in (('/tunnel/tcp/22', 404), ('/tunnel/tcp/x', 404), ('/tunnel/other', 404), ('/tunnel/udp', 400),
                             ('/tunnel/tcp/%d' % self.public_tcp['gpcm_port'], 400)):
            with self.assertRaises(urllib.error.HTTPError) as e:
                urllib.request.urlopen(self.url(path), timeout=5)                           # a plain request, not a WebSocket
            self.assertEqual(e.exception.code, status, path)
        with self.assertRaises(ws.WebSocketError):                                          # from a web page
            run(ws.connect(self.url('/tunnel/udp'), headers=[('Origin', 'https://example.org')]))

    def test_the_players_address_comes_from_the_proxy_only_when_it_is_trusted(self):
        async def connect_as(ip):
            sock = await ws.connect(self.url('/tunnel/udp'), headers=[('CF-Connecting-IP', ip)])
            await asyncio.sleep(0.1)
            return sock

        async def main():
            a, b = await connect_as('203.0.113.5'), await connect_as('203.0.113.6')
            a2 = await connect_as('203.0.113.5')
            gateway = self.server.web.tunnel
            self.assertEqual(gateway.aliases.by_ip['203.0.113.5'][1], 2)                    # the same player twice
            self.assertNotEqual(gateway.aliases.by_ip['203.0.113.5'][0], gateway.aliases.by_ip['203.0.113.6'][0])
            for sock in (a, b, a2):
                await sock.close()
        run(main())

    def test_a_player_may_only_have_so_many_connections(self):
        gateway = self.server.web.tunnel
        old = gateway.max_flows_per_ip
        gateway.max_flows_per_ip = 2
        self.addCleanup(setattr, gateway, 'max_flows_per_ip', old)

        async def main():
            socks = []
            for _ in range(2):
                socks.append(await ws.connect(self.url('/tunnel/udp'), headers=[('CF-Connecting-IP', '203.0.113.77')]))
            with self.assertRaises(ws.WebSocketError) as e:
                await ws.connect(self.url('/tunnel/udp'), headers=[('CF-Connecting-IP', '203.0.113.77')])
            self.assertIn('429', str(e.exception))
            other = await ws.connect(self.url('/tunnel/udp'), headers=[('CF-Connecting-IP', '203.0.113.78')])
            for sock in socks + [other]:
                await sock.close()
        run(main())

    def test_a_player_may_only_open_so_many_connections_at_once(self):
        gateway = self.server.web.tunnel
        old = gateway.opens
        gateway.opens = access.RateLimiter(3, 0.0001)
        self.addCleanup(setattr, gateway, 'opens', old)

        async def main():
            refused = []
            for _ in range(5):
                try:
                    sock = await ws.connect(self.url('/tunnel/udp'), headers=[('CF-Connecting-IP', '203.0.113.88')])
                    await sock.close()
                    refused.append(False)
                except ws.WebSocketError as e:
                    refused.append('429' in str(e))
            self.assertEqual(refused, [False, False, False, True, True])
        run(main())


class RelayTests(unittest.TestCase):
    def test_a_server_that_misbehaves_does_not_end_the_relay(self):
        import http.client
        relay_ = relay.Relay('http://127.0.0.1:9')
        for error in (http.client.IncompleteRead(b'x'), http.client.BadStatusLine('x'), json.JSONDecodeError('x', '', 0),
                      TypeError('a list'), KeyError('tcp'), ConnectionResetError()):
            relay_._fetch_info = lambda e=error: (_ for _ in ()).throw(e)
            self.assertFalse(run(relay_.refresh()))                      # said, and tried again later

    def test_a_server_that_hangs_up_during_the_handshake_is_an_error_not_a_crash(self):
        async def main():
            async def hang_up(reader, writer):
                await reader.readuntil(b'\r\n\r\n')
                writer.write(b'HTTP/1.1 101 Swit')
                writer.close()

            async def talk_forever(reader, writer):
                await reader.readuntil(b'\r\n\r\n')
                writer.write(b'HTTP/1.1 200 OK\r\n' + b'X: y\r\n' * 10000)
                await asyncio.sleep(1)
                writer.close()
            for handler in (hang_up, talk_forever):
                server = await asyncio.start_server(handler, '127.0.0.1', 0)
                with self.assertRaises(ws.WebSocketError):
                    await ws.connect('http://127.0.0.1:%d/' % server.sockets[0].getsockname()[1])
                server.close()
        run(main())


    def test_it_ends_with_the_program_that_started_it(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(0.3)'])
        pid = child.pid
        self.assertTrue(relay.process_alive(pid))
        child.wait()
        self.assertFalse(relay.process_alive(pid))
        started = time.monotonic()
        run(relay._watch_parent(pid))
        self.assertLess(time.monotonic() - started, 4)
        # Also while the server can't be reached: nothing keeps a relay alive that PSPC no longer wants.
        started = time.monotonic()
        run(relay.Relay('http://127.0.0.1:%d' % _free_port()).run(parent_pid=pid))
        self.assertLess(time.monotonic() - started, 6)

    def test_the_shipped_entry_for_swss_points_the_game_at_the_public_server(self):
        from pspc_gamespy import games
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
        with open(os.path.join(root, 'data', 'infrastructure.json'), encoding='utf-8') as f:
            entry = next(g for g in json.load(f)['games'] if g['name'] == 'Star Wars Super Squadron')
        game = games.find('swsspsp')
        self.assertIn(entry['ids'][0], game.disc_ids)                       # the server knows the game the entry is for
        self.assertEqual(entry['domains'], {'*.gamespy.com': 'swss.pspc.online'})    # forwarded ports: the game connects to the domain
        self.assertNotIn('tunnel', entry)                                   # no relay unless the entry asks for one
        self.assertEqual((game.secret_key, game.leaderboard), (games.find('swbfespsp').secret_key, 'bfes'))

    def test_addresses(self):
        self.assertEqual(relay.normalize('swss.pspc.online'), 'https://swss.pspc.online')
        self.assertEqual(relay.normalize('http://127.0.0.1:8080/'), 'http://127.0.0.1:8080')
        with self.assertRaises(ValueError):
            relay.normalize('file:///etc/passwd')


if __name__ == '__main__':
    unittest.main()
