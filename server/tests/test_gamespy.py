# SPDX-License-Identifier: GPL-2.0-or-later
"""GameSpy server (pspc_gamespy) tests: python -m unittest discover -s server/tests (from the repository).

The unit tests cover the encodings and ciphers (the server list's cipher against vectors from the
GameSpy SDK's C implementation), filters and heartbeats; ServerTests runs the whole server on spare
ports and drives it the way the games' GameSpy SDK does: an account created and logged in, a game
hosted (QR2) and found in the server list, a message forwarded to the host, NAT negotiation
between two players, and the stats server's sign-in and persistent data."""

import asyncio
import json
import logging
import os
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
from contextlib import contextmanager

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from pspc_gamespy import crypt, games, gstats, kv, leaderboards, qr  # noqa: E402
from pspc_gamespy.web import WebServer  # noqa: E402
from pspc_gamespy.server import Config, GameSpyServer  # noqa: E402
from pspc_gamespy.sqlfilter import Filter  # noqa: E402
from pspc_gamespy.store import Store  # noqa: E402

GAMENAME = 'swbfespsp'
SECRET = 'wLfbMH'
SDK_CIPHER_VECTOR = ''.join((
    '7fa2cba4df2c73e5bce38de5b1aff680b1e3853ea916a8ac526d1f09be5ef461298147fa8b667c50c02b2dd1d29e1b9e'
    '1622215f9548b7106b7a0c9571716e56551a977372d4222181c0416c0d21216e85fbba85d5d9e04940f1381599316db2'
    '303def71ccf72675a2422c5f550fbd6c18303969ba4f71c76c0b2657b76332a3703f55256cec821e00983f45f1827d40'
    '94e4773e0db24250db20b9dcbcf7d4a989c0fad005be9cf8467bba169451b28721e94067719f2f3ea974a89e5ad09735'
    'd9bf9f6f35674b0ebd6f46c46149034a726ebd8e78d527aab750bc45dfd4f39a52830a7340005a5af3598fad1cffdee1'
    '0111522703c1affe34ba5f2323e5903cacb7a70b8f09a830a5dd8a9d70dc1d5b90e9fffb81718964a3a74f6b4937f7c7'
    '56b5be8dfb45dab63b537e35'))


def _free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class CryptTests(unittest.TestCase):
    def test_base64_round_trip(self):
        for data in (b'', b'a', b'ab', b'abc', bytes(range(256))):
            self.assertEqual(crypt.gs_b64decode(crypt.gs_b64encode(data)), data)
        self.assertEqual(crypt.gs_b64encode(b'\xfb\xff'), b'[]8_')

    def test_password_encoding(self):
        enc = crypt.encode_password('hunter2')
        self.assertNotIn('hunter2', enc)
        self.assertEqual(crypt.decode_password(enc), 'hunter2')

    def test_server_list_cipher_matches_the_sdk(self):
        # Vectors from the GameSpy SDK's sb_crypt.c (as OpenSpy carries it), compiled and run on
        # the same inputs.
        chal = bytes([1, 2, 3, 4, 250, 6, 7, 8])
        serv = bytes((i * 37 + 11) & 255 for i in range(25))
        key = crypt.sb_crypt_key(chal, SECRET, serv)
        self.assertEqual(key.hex(), '83026f048b063908')
        c = crypt.GOACipher(key)
        data = bytes((i * 7) & 255 for i in range(300))
        out = c.encrypt(data[:100]) + c.encrypt(data[100:])
        self.assertEqual(out.hex(), SDK_CIPHER_VECTOR)
        self.assertEqual(crypt.GOACipher(key).decrypt(out), data)

    def test_stats_xor(self):
        x, y = gstats.Xor(), gstats.Xor()
        msg = b'\\lc\\1\\challenge\\ABCDEFGHIJ\\final\\\\ka\\\\final\\'
        enc = x.apply(msg)
        self.assertIn(b'\\final\\', enc)
        self.assertNotIn(b'challenge', enc)
        # Split anywhere, even inside \final\, it decodes the same.
        self.assertEqual(y.apply(enc[:20]) + y.apply(enc[20:39]) + y.apply(enc[39:]), msg)

    def test_stats_auth_response(self):
        # num = c - num * 0x63306CE7 over the challenge, as a signed 32-bit number.
        self.assertEqual(gstats.chresp_num('A'), 65)
        self.assertEqual(gstats.chresp_num('AB'), struct.unpack('<i', struct.pack('<I', (66 - 65 * 0x63306CE7) & 0xFFFFFFFF))[0])
        self.assertEqual(gstats.auth_response('AB', SECRET), crypt.md5hex('%d%s' % (gstats.chresp_num('AB'), SECRET)))


class FilterTests(unittest.TestCase):
    KEYS = {'gamever': '1.0', 'numplayers': '3', 'maxplayers': '8', 'hostname': 'Fun Server', 'gamemode': 'openplaying'}

    def test_expressions(self):
        for text, want in (("gamever='1.0'", True), ("gamever = '1.1'", False), ('numplayers < maxplayers', True),
                           ('numplayers >= 4', False), ("hostname like '%fun%'", True), ("hostname not like '%fun%'", False),
                           ("gamemode = 'openplaying' and (numplayers > 1 or maxplayers = 2)", True),
                           ("not gamever='1.0' || numplayers=3", True), ('password', False), ('', True)):
            self.assertEqual(Filter(text).matches(self.KEYS), want, text)

    def test_bad_filter_lets_everything_through(self):
        f = Filter("numplayers <")
        self.assertFalse(f.ok)
        self.assertTrue(f.matches(self.KEYS))


def heartbeat(instance, keys, players=(), statechanged=None):
    body = b''
    if statechanged is not None:
        keys = dict(keys, statechanged=str(statechanged))
    for k, v in keys.items():
        body += k.encode() + b'\0' + str(v).encode() + b'\0'
    body += b'\0'
    names = ['player_', 'score_'] + [n for n in ('deaths_', 'team_') if any(n in p for p in players)]
    body += struct.pack('>H', len(players)) + b''.join(n.encode() + b'\0' for n in names) + b'\0'
    for p in players:
        body += b''.join(str(p.get(n, '')).encode() + b'\0' for n in names)
    body += struct.pack('>H', 2) + b'team_t\0score_t\0\0' + b'Rebels\x0010\x00Empire\x0020\x00'
    return bytes([qr.HEARTBEAT]) + instance + body


class HeartbeatTests(unittest.TestCase):
    def test_parse(self):
        pkt = heartbeat(b'\1\2\3\4', {'hostname': 'Host', 'gamename': GAMENAME},
                        [{'player_': 'Alice', 'score_': 5}, {'player_': 'Bob', 'score_': 7}], 3)
        keys, players, teams = qr.parse_heartbeat(pkt[5:])
        self.assertEqual(keys['hostname'], 'Host')
        self.assertEqual(keys['statechanged'], '3')
        self.assertEqual([p['player_'] for p in players], ['Alice', 'Bob'])
        self.assertEqual(teams[1], {'team_t': 'Empire', 'score_t': '20'})

    def test_no_players(self):
        keys, players, teams = qr.parse_heartbeat(heartbeat(b'\0\0\0\1', {'hostname': 'Empty'})[5:])
        self.assertEqual((keys['hostname'], players, len(teams)), ('Empty', [], 2))


@contextmanager
def more_games(entries):
    """The games in entries loaded from a games file, and gone again afterwards."""
    tmp = tempfile.mkdtemp()
    path = os.path.join(tmp, 'gamespy-games.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(entries, f)
    before_games, before_aliases = dict(games.GAMES), dict(games.ALIASES)
    try:
        yield path
    finally:
        games.GAMES.clear()
        games.GAMES.update(before_games)
        games.ALIASES.clear()
        games.ALIASES.update(before_aliases)
        shutil.rmtree(tmp, ignore_errors=True)


class MoreGamesTests(unittest.TestCase):
    def test_a_game_like_elite_squadron(self):
        with more_games([{'gamename': 'ModGame', 'name': 'A Mod', 'like': GAMENAME, 'disc_ids': ['ULUS99999']}]) as path:
            added = games.load(path)
            self.assertEqual([g.gamename for g in added], ['modgame'])
            g = games.find('modgame')
            self.assertEqual((g.secret_key, g.product_id, g.namespace_id, g.disc_ids, g.leaderboard),
                             (SECRET, 11878, 1, ('ULUS99999',), 'bfes'))
            self.assertIs(games.find('modgameam'), g)       # Elite Squadron's aliases, by suffix
            self.assertIs(games.find(GAMENAME).gamename, GAMENAME)
            web = WebServer(None)
            leaderboards.install(web, None)
            self.assertIn('/games/modgame/web/bfes_score.asp', web.pages)
            self.assertIn('/games/swbfespsp/web/bfes_score.asp', web.pages)
        self.assertIsNone(games.find('modgame'))

    def test_a_game_of_its_own_and_bad_entries(self):
        with more_games({'games': [{'gamename': 'other', 'secret_key': 'abcdef', 'product_id': 1}]}) as path:
            g = games.load(path)[0]
            self.assertEqual((g.secret_key, g.leaderboard, g.empty_persist), ('abcdef', None, b''))
        for bad in ([{'gamename': GAMENAME, 'like': GAMENAME}], [{'gamename': 'x', 'like': 'nosuchgame'}],
                    [{'gamename': 'x'}], [{'name': 'no game name'}]):
            with more_games(bad) as path:
                with self.assertRaises(ValueError):
                    games.load(path)


class Tcp:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.buf = b''

    def close(self):
        self.s.close()

    def send(self, data):
        self.s.sendall(data.encode('latin-1') if isinstance(data, str) else data)

    def recv_some(self):
        data = self.s.recv(65536)
        if not data:
            raise ConnectionError('closed')
        return data

    def message(self):
        """The next \\final\\-terminated message (GP)."""
        while b'\\final\\' not in self.buf:
            self.buf += self.recv_some()
        i = self.buf.index(b'\\final\\')
        msg, self.buf = self.buf[:i], self.buf[i + 7:]
        return kv.parse(msg.decode('latin-1'))


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        logging.getLogger('gamespy').setLevel(logging.ERROR)
        cls.tmp = tempfile.mkdtemp()
        cls.store = Store(os.path.join(cls.tmp, 'gamespy.sqlite'))
        cls.config = Config(public_host='127.0.0.1', bind='127.0.0.1', gpcm_port=_free_port(), gpsp_port=_free_port(),
                            qr_port=_free_port(socket.SOCK_DGRAM), natneg_port=_free_port(socket.SOCK_DGRAM),
                            sb_port=_free_port(), gstats_port=_free_port(), http_port=_free_port(),
                            status_file=os.path.join(cls.tmp, 'status.json'))
        cls.server = GameSpyServer(cls.config, cls.store)
        cls.loop = asyncio.new_event_loop()
        cls.thread = threading.Thread(target=cls.loop.run_forever, daemon=True)
        cls.thread.start()
        asyncio.run_coroutine_threadsafe(cls.server.start(), cls.loop).result(10)

    @classmethod
    def tearDownClass(cls):
        async def stop():
            cls.server.close()
            for s in cls.server._servers:
                await s.wait_closed()
            await asyncio.sleep(0.1)
        asyncio.run_coroutine_threadsafe(stop(), cls.loop).result(10)
        cls.loop.call_soon_threadsafe(cls.loop.stop)
        cls.thread.join(5)
        cls.loop.close()
        cls.store.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def tcp(self, port):
        c = Tcp(port)
        self.addCleanup(c.close)
        return c

    def udp(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('127.0.0.1', 0))
        s.settimeout(5)
        self.addCleanup(s.close)
        return s

    def on_loop(self, fn):
        async def call():
            return fn()
        return asyncio.run_coroutine_threadsafe(call(), self.loop).result(5)

    # ---- Presence

    def login(self, nick, email, password='secret'):
        """Creates the account if needed and logs in. Returns (connection, login reply)."""
        gp = self.tcp(self.config.gpcm_port)
        server_chal = gp.message()['challenge']
        if not self.store.profile_by_nick_email(nick, email):
            gp.send(kv.build(('newuser', ''), ('email', email), ('nick', nick),
                             ('passwordenc', crypt.encode_password(password)), ('productid', 11878),
                             ('gamename', GAMENAME), ('namespaceid', 0), ('id', 1)))
            self.assertEqual(gp.message().command, 'nur')
        user = '%s@%s' % (nick, email)
        client_chal = 'C' * 32
        pw = crypt.md5hex(password)
        gp.send(kv.build(('login', ''), ('challenge', client_chal), ('user', user),
                         ('response', crypt.login_hash(pw, user, client_chal, server_chal)), ('port', 3658),
                         ('productid', 11878), ('gamename', GAMENAME), ('namespaceid', 0), ('id', 1)))
        reply = gp.message()
        self.assertEqual(reply.command, 'lc', reply.pairs)
        self.assertEqual(reply['proof'], crypt.login_hash(pw, user, server_chal, client_chal))
        return gp, reply

    def test_account_and_login(self):
        gp, reply = self.login('Alice', 'alice@example.com')
        self.assertGreaterEqual(int(reply['profileid']), 100000000)
        bad = self.tcp(self.config.gpcm_port)
        chal = bad.message()['challenge']
        bad.send(kv.build(('login', ''), ('challenge', 'X' * 32), ('user', 'Alice@alice@example.com'),
                          ('response', crypt.login_hash(crypt.md5hex('wrong'), 'Alice@alice@example.com', 'X' * 32, chal)),
                          ('id', 1)))
        err = bad.message()
        self.assertEqual((err.command, err['err']), ('error', '260'))

    def test_friends(self):
        # Ruth finds Sam (search manager) and asks; Sam, online, accepts and asks back, as Elite
        # Squadron does: they are on each other's lists without Ruth having to accept.
        ruth, r = self.login('Ruth', 'ruth@example.com')
        sam, s = self.login('Sam', 'sam@example.com')
        ruth_id, sam_id = int(r['profileid']), int(s['profileid'])
        gpsp = self.tcp(self.config.gpsp_port)
        gpsp.send(kv.build(('search', ''), ('sesskey', r['sesskey']), ('profileid', ruth_id), ('nick', 'Sam'),
                           ('gamename', GAMENAME)))
        self.assertEqual(gpsp.message()['bsr'], str(sam_id))
        ruth.send(kv.build(('addbuddy', ''), ('sesskey', r['sesskey']), ('newprofileid', sam_id), ('reason', 'Ruth')))
        asked = sam.message()
        self.assertEqual((asked.command, asked['bm'], asked['f']), ('bm', '2', str(ruth_id)))
        sam.send(kv.build(('authadd', ''), ('sesskey', s['sesskey']), ('fromprofileid', ruth_id), ('sig', 'x')))
        got = ruth.message()
        self.assertEqual((got['bm'], got['f']), ('4', str(sam_id)))
        self.assertEqual(ruth.message()['bm'], '100')                  # Sam's status
        sam.send(kv.build(('addbuddy', ''), ('sesskey', s['sesskey']), ('newprofileid', ruth_id), ('reason', 'Sam')))
        got = sam.message()
        self.assertEqual((got['bm'], got['f']), ('4', str(ruth_id)))
        self.assertEqual(self.store.buddies(ruth_id), [sam_id])
        self.assertEqual(self.store.buddies(sam_id), [ruth_id])
        self.assertFalse(self.store.pending_requests(ruth_id))

    # ---- hosting and the server list

    def register_host(self, udp, instance, keys, players=()):
        master = ('127.0.0.1', self.config.qr_port)
        udp.sendto(bytes([qr.AVAILABLE]) + b'\0\0\0\0' + GAMENAME.encode() + b'\0', master)
        self.assertEqual(udp.recv(64), b'\xfe\xfd\x09\0\0\0\0')
        udp.sendto(heartbeat(instance, keys, players, statechanged=3), master)
        chal = udp.recv(256)
        self.assertEqual(chal[:7], b'\xfe\xfd\x01' + instance)
        challenge = chal[7:].rstrip(b'\0').decode()
        port = udp.getsockname()[1]
        self.assertTrue(challenge.endswith('7F000001%04X' % port), challenge)
        udp.sendto(bytes([qr.CHALLENGE]) + instance + b'answer\0', master)
        self.assertEqual(udp.recv(64), b'\xfe\xfd\x0a' + instance)

    def list_request(self, fields, filter_text='', options=0):
        body = (bytes([1, 3]) + struct.pack('>I', 0) + GAMENAME.encode() + b'\0' + GAMENAME.encode() + b'\0' +
                b'ABCDEFGH' + filter_text.encode() + b'\0' + ''.join('\\' + f for f in fields).encode() + b'\0' +
                struct.pack('>I', options))
        return struct.pack('>H', len(body) + 3) + b'\0' + body

    def read_list(self, sb, fields):
        """Reads the crypt header and the list; returns (cipher, servers, rest of the stream)."""
        data = b''
        while len(data) < 1 or len(data) < 1 + (data[0] ^ 0xEC) + 1:
            data += sb.recv_some()
        n1 = data[0] ^ 0xEC
        while len(data) < 1 + n1 + 1 + ((data[1 + n1] ^ 0xEA)):
            data += sb.recv_some()
        n2 = data[1 + n1] ^ 0xEA
        servchal = data[2 + n1:2 + n1 + n2]
        cipher = crypt.GOACipher(crypt.sb_crypt_key(b'ABCDEFGH', SECRET, servchal))
        plain = bytearray(cipher.decrypt(data[2 + n1 + n2:]))
        while not plain.endswith(b'\0\xff\xff\xff\xff'):
            plain += cipher.decrypt(sb.recv_some())
        pos = 6
        nfields = plain[pos]
        pos += 1
        names = []
        for _ in range(nfields):
            pos += 1
            end = plain.index(0, pos)
            names.append(plain[pos:end].decode())
            pos = end + 1
        self.assertEqual(names, fields)
        self.assertEqual(plain[pos], 0)          # popular values
        pos += 1
        servers = []
        while plain[pos] != 0:
            flags = plain[pos]
            ip = socket.inet_ntoa(bytes(plain[pos + 1:pos + 5]))
            pos += 5
            port = 6500
            if flags & 16:
                port = struct.unpack_from('>H', plain, pos)[0]
                pos += 2
            if flags & 2:
                pos += 4
            if flags & 32:
                pos += 2
            values = {}
            for f in fields:
                self.assertEqual(plain[pos], 0xFF)
                end = plain.index(0, pos + 1)
                values[f] = plain[pos + 1:end].decode()
                pos = end + 1
            servers.append((flags, ip, port, values))
        return cipher, servers

    def read_pushed(self, sb, cipher):
        data = b''
        while len(data) < 2 or len(data) < struct.unpack_from('>H', data, 0)[0]:
            data += cipher.decrypt(sb.recv_some())
        n = struct.unpack_from('>H', data, 0)[0]
        return data[2:n]

    def test_hosted_game_listed_updated_and_messaged(self):
        host = self.udp()
        instance = b'\x11\x22\x33\x44'
        keys = {'gamename': GAMENAME, 'hostname': "Alice's game", 'mapname': 'Hoth', 'numplayers': 1, 'maxplayers': 8,
                'gamemode': 'openplaying', 'natneg': 1, 'localip0': '192.168.1.5', 'localport': 3658}
        self.register_host(host, instance, keys, [{'player_': 'Alice', 'score_': 0}])
        sb = self.tcp(self.config.sb_port)
        fields = ['hostname', 'mapname', 'numplayers', 'maxplayers']
        sb.send(self.list_request(fields, "gamemode='openplaying'", options=4))
        cipher, servers = self.read_list(sb, fields)
        mine = [s for s in servers if s[3]['hostname'] == "Alice's game"]
        self.assertEqual(len(mine), 1)
        flags, ip, port, values = mine[0]
        self.assertEqual((ip, port, values['mapname'], values['maxplayers']), ('127.0.0.1', host.getsockname()[1], 'Hoth', '8'))
        self.assertTrue(flags & 4 and flags & 2 and flags & 64)        # negotiate, private address, keys

        # a change is pushed
        host.sendto(heartbeat(instance, dict(keys, mapname='Endor', numplayers=2), [{'player_': 'Alice', 'score_': 3}]), ('127.0.0.1', self.config.qr_port))
        pushed = self.read_pushed(sb, cipher)
        self.assertEqual(pushed[0], 2)
        self.assertIn(b'Endor\0', pushed)

        # full rules for an info request
        sb.send(struct.pack('>H', 9) + b'\x01' + socket.inet_aton('127.0.0.1') + struct.pack('>H', port))
        info = self.read_pushed(sb, cipher)
        self.assertEqual(info[0], 2)
        self.assertTrue(info[1] & 128)
        self.assertIn(b'player_0\0Alice\0', info)
        self.assertIn(b'team_t1\0Empire\0', info)

        # a message to the host arrives as a QR2 client message
        msg = b'\xfd\xfc\x1e\x66\x6a\xb2\x03\x00hello'
        sb.send(struct.pack('>H', 9 + len(msg)) + b'\x02' + socket.inet_aton('127.0.0.1') + struct.pack('>H', port) + msg)
        got = host.recv(256)
        self.assertEqual(got[:7], b'\xfe\xfd\x06' + instance)
        self.assertEqual(got[11:], msg)
        host.sendto(bytes([qr.CLIENT_MESSAGE_ACK]) + instance + got[7:11], ('127.0.0.1', self.config.qr_port))

        # the status file lists it
        self.on_loop(self.server.write_status)
        with open(self.config.status_file, encoding='utf-8') as f:
            status = json.load(f)
        self.assertIn("Alice's game", [g['name'] for g in status['games']])

        # leaving: the game disappears, and the browser hears of it
        host.sendto(heartbeat(instance, keys, statechanged=2), ('127.0.0.1', self.config.qr_port))
        gone = self.read_pushed(sb, cipher)
        self.assertEqual(gone, b'\x04' + socket.inet_aton('127.0.0.1') + struct.pack('>H', port))

    def test_more_games_apart(self):
        """A game from a games file has its own server list: hosts of each game are listed to that game only."""
        global GAMENAME
        with more_games([{'gamename': 'modgame', 'like': GAMENAME}]) as path:
            games.load(path)
            fields = ['hostname']
            for game, name, inst in (('modgame', 'Mod host', b'abcd'), ('swbfespsp', 'Plain host', b'qrst')):
                GAMENAME = game
                try:
                    self.register_host(self.udp(), inst, {'gamename': game, 'hostname': name, 'gamemode': 'openplaying'})
                finally:
                    GAMENAME = 'swbfespsp'
            listed = {}
            for game in ('modgame', 'swbfespsp'):
                GAMENAME = game
                try:
                    sb = self.tcp(self.config.sb_port)
                    sb.send(self.list_request(fields, "gamemode='openplaying'"))
                    listed[game] = {v['hostname'] for _, _, _, v in self.read_list(sb, fields)[1]}
                finally:
                    GAMENAME = 'swbfespsp'
            self.assertIn('Mod host', listed['modgame'])
            self.assertNotIn('Plain host', listed['modgame'])
            self.assertIn('Plain host', listed['swbfespsp'])
            self.assertNotIn('Mod host', listed['swbfespsp'])

    def test_unknown_game_refused(self):
        sb = self.tcp(self.config.sb_port)
        body = (bytes([1, 3]) + struct.pack('>I', 0) + b'nosuchgame\0nosuchgame\0ABCDEFGH\0\0' + struct.pack('>I', 0))
        sb.send(struct.pack('>H', len(body) + 3) + b'\0' + body)
        self.assertIn(b'Unknown game', sb.recv_some())

    # ---- NAT negotiation

    def test_nat_negotiation(self):
        a, b = self.udp(), self.udp()
        nn = ('127.0.0.1', self.config.natneg_port)
        cookie = 0x12345678

        def init(sock, index, porttype):
            pkt = (b'\xfd\xfc\x1e\x66\x6a\xb2\x03\x00' + struct.pack('>I', cookie) + bytes([porttype, index, 0]) +
                   socket.inet_aton('192.168.1.%d' % (10 + index)) + struct.pack('>H', 3658) + GAMENAME.encode() + b'\0')
            sock.sendto(pkt, nn)
            ack = sock.recv(64)
            self.assertEqual(ack[7], 1)
            self.assertEqual(ack[12:14], bytes([porttype, index]))

        init(a, 0, 1)
        init(a, 0, 2)
        init(b, 1, 1)
        connect_a, connect_b = a.recv(64), b.recv(64)
        for got, other in ((connect_a, b), (connect_b, a)):
            self.assertEqual(got[7], 5)
            self.assertEqual(socket.inet_ntoa(got[12:16]), '127.0.0.1')
            self.assertEqual(struct.unpack_from('>H', got, 16)[0], other.getsockname()[1])
            self.assertEqual(got[19], 0)
        # an address check answers with the address it came from
        a.sendto(b'\xfd\xfc\x1e\x66\x6a\xb2\x03\x0a' + struct.pack('>I', cookie) + bytes(9), nn)
        reply = a.recv(64)
        self.assertEqual((reply[7], struct.unpack_from('>H', reply, 19)[0]), (11, a.getsockname()[1]))

    # ---- stats and persistent data

    def test_stats_sign_in_and_persistent_data(self):
        gp, reply = self.login('Bob', 'bob@example.com')
        pid = int(reply['profileid'])
        st = self.tcp(self.config.gstats_port)
        rx, tx = gstats.Xor(), gstats.Xor()

        def message():
            while b'\\final\\' not in st.buf:
                st.buf += rx.apply(st.recv_some())
            i = st.buf.index(b'\\final\\')
            raw, st.buf = st.buf[:i], st.buf[i + 7:]
            return gstats.parse_message(raw)

        def send(data):
            st.send(tx.apply((data.encode('latin-1') if isinstance(data, str) else data) + b'\\final\\'))

        m, _ = message()
        self.assertEqual(m.command, 'lc')
        send(kv.build(('auth', ''), ('gamename', GAMENAME), ('response', gstats.auth_response(m['challenge'], SECRET)),
                      ('port', 0), ('id', 1), final=False))
        m, _ = message()
        self.assertEqual((m.command, m['lc']), ('lc', '2'))
        send(kv.build(('authp', ''), ('authtoken', reply['lt']), ('resp', 'x' * 32), ('lid', 0), final=False))
        m, _ = message()
        self.assertEqual((m.command, m['pauthr']), ('pauthr', str(pid)))
        record = b'\x01\x02\\\x00binary\\final-ish' * 3
        send(kv.build(('setpd', ''), ('pid', pid), ('ptype', 1), ('dindex', 0), ('kv', 0), ('lid', 1),
                      ('length', len(record)), final=False).encode() + b'\\data\\' + record)
        m, _ = message()
        self.assertEqual((m.command, m['setpdr'], m['pid']), ('setpdr', '1', str(pid)))
        send(kv.build(('getpd', ''), ('pid', pid), ('ptype', 1), ('dindex', 0), ('lid', 2), final=False))
        m, data = message()
        self.assertEqual((m.command, m['getpdr'], m.int('length')), ('getpdr', '1', len(record)))
        self.assertEqual(data, record)
        # key/value data
        send(kv.build(('setpd', ''), ('pid', pid), ('ptype', 3), ('dindex', 0), ('kv', 1), ('lid', 3),
                      ('length', 16), final=False).encode() + b'\\data\\\\kills\\12\\rank\\3\0')
        self.assertEqual(message()[0]['setpdr'], '1')
        send(kv.build(('getpd', ''), ('pid', pid), ('ptype', 3), ('dindex', 0), ('keys', '\x01rank'), ('lid', 4), final=False))
        m, data = message()
        self.assertEqual(data, b'\x01rank\x013')

    def stats_connection(self, nick, email):
        """A stats connection signed in as the player (the way Elite Squadron does it)."""
        gp, reply = self.login(nick, email)
        pid = int(reply['profileid'])
        st = self.tcp(self.config.gstats_port)
        rx, tx = gstats.Xor(), gstats.Xor()

        def message():
            while b'\\final\\' not in st.buf:
                st.buf += rx.apply(st.recv_some())
            i = st.buf.index(b'\\final\\')
            raw, st.buf = st.buf[:i], st.buf[i + 7:]
            return gstats.parse_message(raw)

        def send(data):
            st.send(tx.apply((data.encode('latin-1') if isinstance(data, str) else data) + b'\\final\\'))

        chal = message()[0]['challenge']
        send(kv.build(('auth', ''), ('gamename', GAMENAME), ('response', gstats.auth_response(chal, SECRET)), ('port', 0),
                      ('id', 1), final=False))
        sesskey = message()[0]['sesskey']
        send(kv.build(('authp', ''), ('pid', pid), ('resp', 'x' * 32), ('lid', 0), final=False))
        self.assertEqual(message()[0]['pauthr'], str(pid))
        return pid, sesskey, send, message, gp

    def test_new_player_gets_an_empty_record(self):
        pid, _, send, message, gp = self.stats_connection('Erin', 'erin@example.com')
        send(kv.build(('getpd', ''), ('pid', pid), ('ptype', 1), ('dindex', 206628862), ('keys', ''), ('lid', 0),
                      final=False))
        m, data = message()
        self.assertEqual((m['getpdr'], m['lid'], m['pid'], data), ('1', '0', str(pid), bytes(8)))

    def test_careers_and_leaderboards(self):
        carol, sesskey, send, message, gp = self.stats_connection('Carol', 'carol@example.com')
        dave = int(self.login('Dave', 'dave@example.com')[1]['profileid'])
        host = self.udp()
        instance = b'\x55\x66\x77\x88'
        keys = {'gamename': GAMENAME, 'hostname': "Carol's game", 'mapname': 'BSPN prequel', 'numplayers': 2,
                'maxplayers': 6, 'gamemode': 'openplaying', 'natneg': 1}
        self.register_host(host, instance, keys, [{'player_': 'Carol', 'score_': 0, 'deaths_': 0, 'team_': 0},
                                                  {'player_': 'Dave', 'score_': 0, 'deaths_': 0, 'team_': 1}])
        host.sendto(heartbeat(instance, keys, [{'player_': 'Carol', 'score_': 12, 'deaths_': 3, 'team_': 0},
                                               {'player_': 'Dave', 'score_': 5, 'deaths_': 6, 'team_': 1}]),
                    ('127.0.0.1', self.config.qr_port))
        time.sleep(0.3)
        # the round ends: the host's stats connection reports it, Carol's team (team_t0) won
        send(kv.build(('newgame', ''), ('connid', sesskey), ('sesskey', 1), final=False))
        send(kv.build(('updgame', ''), ('sesskey', 1), ('connid', sesskey), ('done', 1),
                      ('gamedata', '\x01statsver\x010\x01gamever\x0110901\x01score_t0\x011\x01score_t1\x010\x01mapname\x01BSPN '
                                   'prequel\x01numplayers\x012'), final=False))
        deadline = time.time() + 5
        while not self.store.stats(dave, GAMENAME) and time.time() < deadline:
            time.sleep(0.05)
        c, d = self.store.stats(carol, GAMENAME), self.store.stats(dave, GAMENAME)
        self.assertEqual((c['kills'], c['deaths'], c['wins'], c['games']), (12, 3, 1, 1))
        self.assertEqual((d['kills'], d['deaths'], d['losses']), (5, 6, 1))

        base = 'http://127.0.0.1:%d/games/swbfespsp/web/bfes_score.asp?version=0' % self.config.http_port
        with urllib.request.urlopen(base + '&sortcol=12&col=0&col=13&offset=0&numrows=5', timeout=5) as r:
            body = r.read().decode()
        lines = body.split('\n')
        self.assertEqual(lines[0], 'BFESScore\t')
        self.assertTrue(body.endswith('\t\n\n'))          # the game overwrites the last byte
        rows = [l.split('\t') for l in lines[1:] if l]
        self.assertTrue(all(len(r) == 8 and r[7] == '' for r in rows))     # 7 fields, each ended by a tab
        names = [r[2] for r in rows]
        self.assertLess(names.index('Carol'), names.index('Dave'))
        with urllib.request.urlopen(base + '&sortcol=3&col=0&col=13&p=%d' % dave, timeout=5) as r:
            mine = r.read().decode().split('\n')
        self.assertEqual(mine[1].split('\t')[2:5], ['Dave', '', '0.833'])      # 5 kills / 6 deaths
        with urllib.request.urlopen(base + '&sortcol=12&col=0&col=13&p=1', timeout=5) as r:
            self.assertEqual(r.read(), b'BFESScore\t\n\n')                    # not ranked

    # ---- web

    def test_motd_and_version_check(self):
        base = 'http://127.0.0.1:%d' % self.config.http_port
        with urllib.request.urlopen(base + '/motd/motd.asp?userid=0&productid=11878&gamename=swbfespsp', timeout=5) as r:
            self.assertIn(b'PSPC', r.read())
        with urllib.request.urlopen(base + '/motd/vercheck.asp?productid=11878&gamename=swbfespsp', timeout=5) as r:
            self.assertEqual(r.read(), b'\\newver\\0')


if __name__ == '__main__':
    unittest.main()
