# SPDX-License-Identifier: GPL-2.0-or-later
"""Combat Ops game server (pspc_medius) tests: python -m unittest discover -s server/tests (from the repository).

The unit tests cover the wire formats; ServerTests runs the whole server on spare ports and drives
it the way consoles do: SCE-RT handshakes, a login, a game created and joined, game traffic relayed
between two players, the SVO pages over HTTP and SSL 3.0, and the mission server."""

import asyncio
import hashlib
import logging
import os
import re
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from pspc_medius import crypto, records, rt, sslv3, stats, umd  # noqa: E402
from pspc_medius import messages as M  # noqa: E402
from pspc_medius import structs as S  # noqa: E402
from pspc_medius.games import GAMES  # noqa: E402
from pspc_medius.missions import spywars  # noqa: E402
from pspc_medius.records import Records  # noqa: E402
from pspc_medius.server import Config, MediusServer  # noqa: E402
from pspc_medius.store import Store  # noqa: E402
from pspc_medius.svo import svomac  # noqa: E402
from pspc_medius.tickets import TicketVerifier  # noqa: E402

SFCO_APP_ID = 21064
ISO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Games', 'SFCO.iso')

MISSION_INI = (b'FILENAME=Test Mission\r\nLVL_AUTHOR=Tester\r\nLVL_THEMEID=7\r\nPVPGAMEMODE=2\r\nLVL_CHUNK1=1\r\n'
               b'LVL_CHUNK2=3\r\nLVL_LIGHTING=1\r\nLVL_WEATHERFX=4\r\nMAP_VALIDFLAG=1\r\n')


class CryptoTests(unittest.TestCase):
    def test_session_cipher_round_trip(self):
        c = crypto.RcqCipher(os.urandom(64))
        plain = b'Combat Ops ' * 7
        cipher, h = c.encrypt(plain)
        self.assertNotEqual(cipher, plain)
        self.assertEqual(crypto.hash_context(h), crypto.CTX_RC_CLIENT_SESSION)
        self.assertEqual(c.decrypt(cipher, h), plain)
        self.assertIsNone(crypto.RcqCipher(os.urandom(64)).decrypt(cipher, h))

    def test_rsa_round_trip(self):
        k = rt.DEFAULT_KEY
        # SCE-RT encrypts 64-byte blocks (key exchange); the second is larger than n
        for plain in (os.urandom(63) + b'\x00', (k.n + 5).to_bytes(64, 'little')):
            cipher, h = k.encrypt(plain)
            self.assertEqual(crypto.hash_context(h), crypto.CTX_RSA_AUTH)
            self.assertEqual(k.decrypt(cipher, h), plain)

    def test_generated_key(self):
        k = crypto.generate_rsa_key()
        self.assertEqual(k.n.bit_length(), 512)
        plain = os.urandom(63) + b'\x00'
        self.assertEqual(k.decrypt(*k.encrypt(plain)), plain)


class StructTests(unittest.TestCase):
    def test_c_alignment(self):
        aligned = S.Layout(('a', S.MID), ('b', S.I32))
        packed = S.Layout(('a', S.MID), ('b', S.I32), packed=True)
        self.assertEqual((aligned.offset('b'), aligned.size), (24, 28))
        self.assertEqual((packed.offset('b'), packed.size), (21, 25))

    def test_connection_info(self):
        info = {'Type': 3, 'AddressList': [{'AddressType': 1, 'Address': '10.0.0.1', 'Port': 10079}, {}],
                'WorldID': 7, 'ServerKey': bytes(range(64)), 'SessionKey': 'abc', 'AccessKey': 'def'}
        data = S.NET_CONNECTION_INFO.pack(info)
        self.assertEqual(len(data), 156)
        back = S.NET_CONNECTION_INFO.unpack(data)
        self.assertEqual((back['WorldID'], back['AccessKey'], back['AddressList'][0]['Port']), (7, 'def', 10079))

    def test_join_game_response_size(self):
        self.assertEqual(len(M.encode('JoinGameResponse', MessageID='1')), 2 + 192)


class RtTests(unittest.TestCase):
    def keys(self, session_key):
        k = rt.Crypto(rt.DEFAULT_KEY)
        k.session = crypto.RcqCipher(session_key)
        return k

    def test_framing(self):
        buf = bytearray(struct.pack('<BH', rt.CLIENT_ECHO, 1) + b'\xa5' + struct.pack('<BH', rt.SERVER_APP, 2) + b'\x01\x02'
                        + struct.pack('<BH', rt.SERVER_APP, 5) + b'\x01')
        msgs = rt.parse(buf, None)
        self.assertEqual([(m.id, m.payload) for m in msgs], [(rt.CLIENT_ECHO, b'\xa5'), (rt.SERVER_APP, b'\x01\x02')])
        self.assertEqual(len(buf), 4)       # the incomplete third message stays

    def test_single_keeps_index_plaintext(self):
        keys = self.keys(os.urandom(64))
        payload = b'\x01\x00' + b'game data'
        frame = keys.encode(rt.Message(rt.CLIENT_APP_SINGLE, payload))
        self.assertEqual(frame[0], rt.CLIENT_APP_SINGLE | 0x80)
        self.assertEqual(frame[7:9], b'\x01\x00')
        self.assertEqual(rt.parse(bytearray(frame), keys)[0].payload, payload)

    def test_list_keeps_targets_plaintext(self):
        # A console's encrypted CLIENT_APP_LIST: [mask length][mask] stay readable, the rest is encrypted.
        session = os.urandom(64)
        targets, data = b'\x01\x01', b'\x00\x15\x03\x01\xfa\x00\x01\x00'
        cipher, h = crypto.RcqCipher(session).encrypt(data)
        frame = struct.pack('<BH', rt.CLIENT_APP_LIST | 0x80, len(targets) + len(cipher)) + h + targets + cipher
        m = rt.parse(bytearray(frame), self.keys(session))[0]
        self.assertEqual((m.id, m.payload), (rt.CLIENT_APP_LIST, targets + data))

    def test_undecryptable_passes_through(self):
        cipher, h = crypto.RcqCipher(os.urandom(64)).encrypt(b'for another key')
        frame = struct.pack('<BH', rt.CLIENT_APP_BROADCAST | 0x80, len(cipher)) + h + cipher
        m = rt.parse(bytearray(frame), self.keys(os.urandom(64)), opaque=(rt.CLIENT_APP_BROADCAST,))[0]
        self.assertEqual(m.raw, h + cipher)
        with self.assertRaises(ValueError):
            rt.parse(bytearray(frame), self.keys(os.urandom(64)))

    def test_hello_certificate_carries_key(self):
        k = crypto.generate_rsa_key()
        cert = rt.hello_certificate(k)
        self.assertEqual(len(cert), len(rt.HELLO_CERT))
        self.assertEqual(cert[rt.MODULUS_AT:rt.MODULUS_AT + 64], k.modulus_be())

    def test_connect_payload(self):
        payload = struct.pack('<Ii', 7, SFCO_APP_ID) + bytes(64) + b'SESSIONKEY123456\0' + b'ACCESS'.ljust(17, b'\0')
        self.assertEqual(rt.parse_connect_tcp(payload, 113), (7, SFCO_APP_ID, 'SESSIONKEY123456', 'ACCESS'))


class UmdTests(unittest.TestCase):
    def test_encoded_buffer_round_trip(self):
        data = umd.encode(MISSION_INI)
        self.assertEqual(data[:4], b'ENCB')
        self.assertEqual(umd.decode(data), MISSION_INI)
        self.assertEqual(umd.decode(umd.UMD_HEADER + bytes(umd.UMD_HEADER_SIZE - 8) + data), MISSION_INI)
        tampered = bytearray(data)
        tampered[20] ^= 1
        self.assertIsNone(umd.decode(bytes(tampered)))
        self.assertIsNone(umd.decode(b'not a mission'))

    def test_mission_info(self):
        info = umd.mission_info(umd.UMD_HEADER + bytes(umd.UMD_HEADER_SIZE - 8) + umd.encode(MISSION_INI))
        self.assertEqual((info['name'], info['author'], info['theme'], info['combat_type']), ('Test Mission', 'Tester', 7, 2))
        self.assertEqual((info['terrain'], info['time_of_day'], info['weather']), (0b101, 1, 4))
        self.assertEqual(info['data'][:4], b'ENCB')            # served without the UMD header
        self.assertEqual(info['hash'], hashlib.md5(info['data']).hexdigest())
        self.assertFalse(info['official'])

    def test_official_missions_from_folder(self):
        root = tempfile.mkdtemp()
        try:
            strings = struct.pack('<I', 1) + b'TEXT_FTITLE_99'.ljust(16, b'\0') + struct.pack('<I', 9) + b'Big Test\0\0\0\0'
            mission = MISSION_INI.replace(b'FILENAME=Test Mission', b'FILENAME=~TEXT_FTITLE_99')
            for path, data in ((umd.STRINGS_PATH, strings),
                               (umd.MISSIONS_DIR + '/MAP/MISSIONS/MAP_SAB.DEF',
                                umd.UMD_HEADER + bytes(umd.UMD_HEADER_SIZE - 8) + umd.encode(mission)),
                               (umd.MISSIONS_DIR + '/MAP/LEVEL.INI', umd.encode(b'X=1'))):
                full = os.path.join(root, *path.split('/'))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, 'wb') as f:
                    f.write(data)
            missions = umd.official_missions(root)
            self.assertEqual([(n, i['name'], i['official']) for n, i in missions], [('MAP_SAB.DEF', 'Big Test', True)])
        finally:
            shutil.rmtree(root)

    def test_official_missions_from_iso_and_cso(self):
        strings = struct.pack('<I', 1) + b'TEXT_FTITLE_99'.ljust(16, b'\0') + struct.pack('<I', 9) + b'Big Test\0\0\0\0'
        mission = umd.UMD_HEADER + bytes(umd.UMD_HEADER_SIZE - 8) + umd.encode(
            MISSION_INI.replace(b'FILENAME=Test Mission', b'FILENAME=~TEXT_FTITLE_99'))
        image = _tiny_iso({umd.STRINGS_PATH: strings, umd.MISSIONS_DIR + '/MAP/MISSIONS/MAP_SAB.DEF': mission})
        root = tempfile.mkdtemp()
        try:
            for name, data in (('game.iso', image), ('game.cso', _cso(image))):
                path = os.path.join(root, name)
                with open(path, 'wb') as f:
                    f.write(data)
                missions = umd.official_missions(path)
                self.assertEqual([(n, i['name']) for n, i in missions], [('MAP_SAB.DEF', 'Big Test')], name)
        finally:
            shutil.rmtree(root)

    @unittest.skipUnless(os.path.exists(ISO), 'needs Games/SFCO.iso')
    def test_official_missions_from_iso(self):
        missions = umd.official_missions(ISO)
        self.assertEqual(len(missions), 20)
        self.assertIn(('AEROSPACE_DM.DEF', 'Jackboot Camp'), [(n, i['name']) for n, i in missions])


def _tiny_iso(files):
    """An ISO 9660 image holding files ({path: data}), as much of the format as umd.Iso9660 reads."""
    tree = {}
    for path, data in files.items():
        node = tree
        parts = path.split('/')
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = data
    sectors, next_lba = {}, [18]

    def put(data):
        lba = next_lba[0]
        next_lba[0] += max(1, (len(data) + 2047) // 2048)
        sectors[lba] = data
        return lba

    def record(name, lba, size, is_dir):
        n = len(name)
        rec = struct.pack('<BBIIII7sBBBHHB', 33 + n + (n + 1) % 2, 0, lba, 0, size, 0, bytes(7), 2 if is_dir else 0,
                          0, 0, 1, 0, n) + name
        return rec + bytes(rec[0] - len(rec))

    def directory(node):
        recs = b''
        for name, child in sorted(node.items()):
            if isinstance(child, dict):
                lba, size = directory(child)
                recs += record(name.encode(), lba, size, True)
            else:
                recs += record(name.encode() + b';1', put(child), len(child), False)
        body = record(b'\0', 0, 0, True) + record(b'\1', 0, 0, True) + recs
        return put(body), len(body)

    root_lba, root_size = directory(tree)
    image = bytearray(next_lba[0] * 2048)
    pvd = bytearray(2048)
    pvd[0:6] = b'\x01CD001'
    pvd[156:156 + 34] = record(b'\0', root_lba, root_size, True)
    image[16 * 2048:17 * 2048] = pvd
    for lba, data in sectors.items():
        image[lba * 2048:lba * 2048 + len(data)] = data
    return bytes(image)


def _cso(image, block=2048):
    """image as a version 1 CSO: per-block raw deflate (or stored), index of block offsets."""
    blocks = [image[i:i + block] for i in range(0, len(image), block)]
    offset = 24 + 4 * (len(blocks) + 1)
    index, body = [], b''
    for b in blocks:
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = c.compress(b) + c.flush()
        index.append((offset + len(body)) | (0 if len(packed) < len(b) else 0x80000000))
        body += packed if len(packed) < len(b) else b
    index.append(offset + len(body))
    return (struct.pack('<4sIQIBBH', b'CISO', 24, len(image), block, 1, 0, 0) +
            struct.pack('<%dI' % len(index), *index) + body)


class MissionXmlTests(unittest.TestCase):
    def test_reply_shape(self):
        resp = spywars('MissionGetID', 12, '<Mission><MissionID>3</MissionID></Mission>\n', {'x': 'a"b'})
        root = ET.fromstring(resp.body)
        self.assertEqual(root.tag, 'XML')
        sw = root.find('SpyWars')
        self.assertEqual((sw.get('type'), sw.get('x'), sw.findtext('TransactionID')), ('MissionGetID', 'a"b', '12'))
        self.assertEqual(sw.findtext('Mission/MissionID'), '3')


# ---------------------------------------------------------------- the whole server

def _free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _request(name, **fields):
    for (cls, typ), (n, layout) in M._REQUESTS.items():
        if n == name:
            return bytes((cls, typ)) + layout.pack(fields)
    raise KeyError(name)


class RtClient:
    """A console's end of an SCE-RT connection (a 3.02 client, version 113)."""

    def __init__(self, port, key=rt.DEFAULT_KEY):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.key = key
        self.keys = rt.Crypto(key)          # its private key decrypts what the server encrypts to us
        self.buf = bytearray()
        self.pending = []
        self.index = None

    def close(self):
        self.sock.close()

    def frame(self, msg_id, payload, rsa=None):
        if rsa is not None:
            cipher, h = rsa.encrypt(payload)
            return struct.pack('<BH', msg_id | 0x80, len(cipher)) + h + cipher
        if self.keys.session is not None and payload:
            n = rt.plain_prefix(msg_id, payload)
            cipher, h = self.keys.session.encrypt(payload[n:])
            return struct.pack('<BH', msg_id | 0x80, n + len(cipher)) + h + payload[:n] + cipher
        return struct.pack('<BH', msg_id, len(payload)) + payload

    def send(self, msg_id, payload=b'', rsa=None):
        self.sock.sendall(self.frame(msg_id, payload, rsa))

    def recv(self, want=None):
        """The next message with id want (others are skipped)."""
        while True:
            while self.pending:
                m = self.pending.pop(0)
                if want is None or m.id == want:
                    return m
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError('server closed the connection')
            self.buf += data
            self.pending += rt.parse(self.buf, self.keys)

    def handshake(self):
        self.send(rt.CLIENT_HELLO, struct.pack('<HH', 1, 113))
        hello = self.recv(rt.SERVER_HELLO)
        server_key = crypto.RsaKey(int.from_bytes(hello.payload[rt.MODULUS_AT:rt.MODULUS_AT + 64], 'big'))
        self.send(rt.CLIENT_CRYPTKEY_PUBLIC, self.key.modulus_le(), rsa=server_key)
        self.keys.session = crypto.RcqCipher(self.recv(rt.SERVER_CRYPTKEY_PEER).payload)

    def connect(self, world=0, session='', access='', aux_udp=False):
        self.send(rt.CLIENT_CONNECT_TCP_AUX_UDP if aux_udp else rt.CLIENT_CONNECT_TCP,
                  struct.pack('<Ii', world, SFCO_APP_ID) + bytes(64) + session.encode().ljust(17, b'\0') +
                  access.encode().ljust(17, b'\0'))
        self.recv(rt.SERVER_CONNECT_REQUIRE)
        self.send(rt.CLIENT_CONNECT_READY_REQUIRE, b'\x00')
        self.index = struct.unpack_from('<H', self.recv(rt.SERVER_CONNECT_ACCEPT_TCP).payload)[0]
        self.send(rt.CLIENT_CONNECT_READY_TCP, b'\x0e\x0f')     # receive everything
        if not aux_udp:
            self.recv(rt.SERVER_CONNECT_COMPLETE)

    def app(self, name, response, **fields):
        self.send(rt.CLIENT_APP_TOSERVER, _request(name, **fields))
        return self.reply(response)

    def reply(self, response):
        cls, typ, layout = M._RESPONSES[response]
        while True:
            m = self.recv(rt.SERVER_APP)
            if m.payload[:2] == bytes((cls, typ)):
                return layout.unpack(m.payload[2:])


class Ssl3Client:
    """An SSL 3.0 RC4-MD5 client like the game's rt_ssl: SSLv2-format hello, RSA key exchange."""

    def __init__(self, port, server_key):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.buf = b''
        challenge = os.urandom(16)
        hello = b'\x01' + struct.pack('>HHHH', 0x0300, 3, 0, 16) + sslv3.RSA_WITH_RC4_128_MD5.to_bytes(3, 'big') + challenge
        self.sock.sendall(struct.pack('>H', 0x8000 | len(hello)) + hello)
        s = self.state = sslv3.Session(None, [])
        s.handshake = hello
        msgs = []
        while not any(t == sslv3.HS_SERVER_HELLO_DONE for t, _ in msgs):
            ctype, body = self.record()
            s.handshake += body
            pos = 0
            while pos < len(body):
                n = int.from_bytes(body[pos + 1:pos + 4], 'big')
                msgs.append((body[pos], body[pos + 4:pos + 4 + n]))
                pos += 4 + n
        server_random = dict(msgs)[sslv3.HS_SERVER_HELLO][2:34]
        premaster = b'\x03\x00' + os.urandom(46)
        padding = bytes(b or 1 for b in os.urandom(server_key.size - 3 - len(premaster)))
        m = int.from_bytes(b'\x00\x02' + padding + b'\x00' + premaster, 'big')
        exchange = s.hs(sslv3.HS_CLIENT_KEY_EXCHANGE, pow(m, server_key.e, server_key.n).to_bytes(server_key.size, 'big'))
        self.sock.sendall(s.record(sslv3.CT_HANDSHAKE, exchange))
        client_random = bytes(16) + challenge
        s.master = sslv3._ssl3_hash_block(premaster, client_random + server_random, 48)
        block = sslv3._ssl3_hash_block(s.master, server_random + client_random, 64)
        self.sock.sendall(s.record(sslv3.CT_CHANGE_CIPHER_SPEC, b'\x01'))
        s.write_cipher, s.write_mac = sslv3.Rc4(block[32:48]), block[0:16]
        self.sock.sendall(s.record(sslv3.CT_HANDSHAKE, s.hs(sslv3.HS_FINISHED, s.finished_hash(b'CLNT'))))
        expected = s.finished_hash(b'SRVR')
        self.assertion(self.record()[0] == sslv3.CT_CHANGE_CIPHER_SPEC, 'no ChangeCipherSpec')
        s.read_cipher, s.read_mac = sslv3.Rc4(block[48:64]), block[16:32]
        ctype, body = self.record()
        self.assertion(body[4:] == expected, 'server Finished does not verify')

    @staticmethod
    def assertion(ok, why):
        if not ok:
            raise AssertionError(why)

    def record(self):
        while len(self.buf) < 5 or len(self.buf) < 5 + struct.unpack_from('>H', self.buf, 3)[0]:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError('closed')
            self.buf += data
        n = struct.unpack_from('>H', self.buf, 3)[0]
        ctype, body, self.buf = self.buf[0], self.buf[5:5 + n], self.buf[5 + n:]
        return ctype, self.state.open_record(ctype, body)

    def get(self, path):
        self.sock.sendall(self.state.record(sslv3.CT_APPLICATION_DATA,
                                            ('GET %s HTTP/1.1\r\nHost: test\r\n\r\n' % path).encode()))
        data = b''
        while b'\r\n\r\n' not in data or len(data) < data.index(b'\r\n\r\n') + 4 + self._length(data):
            data += self.record()[1]
        head, _, body = data.partition(b'\r\n\r\n')
        return int(head.split(b' ')[1]), body

    @staticmethod
    def _length(data):
        for line in data.split(b'\r\n\r\n')[0].split(b'\r\n'):
            if line.lower().startswith(b'content-length:'):
                return int(line.split(b':')[1])
        return 0


class ServerCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        logging.getLogger('medius').setLevel(logging.ERROR)
        cls.tmp = tempfile.mkdtemp()
        cls.store = Store(os.path.join(cls.tmp, 'medius.sqlite'))
        mission = umd.mission_info(umd.encode(MISSION_INI))
        cls.mission_id = cls.store.add_mission(mission, official=True)
        cls.config = Config(public_host='127.0.0.1', bind='127.0.0.1', muis_port=_free_port(), mas_port=_free_port(),
                            mls_port=_free_port(), dme_port=_free_port(), dme_udp_port=_free_port(socket.SOCK_DGRAM),
                            nat_port=_free_port(socket.SOCK_DGRAM), http_port=_free_port(), https_port=_free_port(),
                            allow_password_login=True)
        game = GAMES['sfco']()
        cls.server = MediusServer(cls.config, game, cls.store, TicketVerifier(None, True, game.service_ids))
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

    def client(self, port):
        c = RtClient(port)
        self.addCleanup(c.close)
        c.handshake()
        return c

    def http(self, path, headers=None):
        url = 'http://127.0.0.1:%d%s%s' % (self.config.http_port, self.server.game.svo_path, path)
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=5) as r:
            return r.read(), dict(r.headers)

    def sign_in(self, name):
        """MAS: session and login. Returns (session key, lobby connect info)."""
        mas = self.client(self.config.mas_port)
        mas.connect()
        session = mas.app('SessionBegin', 'SessionBeginResponse', MessageID='1')['SessionKey']
        r = mas.app('AccountLogin', 'AccountLoginResponse', MessageID='2', SessionKey=session, Username=name)
        self.assertEqual(r['StatusCode'], M.SUCCESS)
        return session, r['ConnectInfo']

    def lobby(self, name):
        session, info = self.sign_in(name)
        self.assertEqual(info['AddressList'][0]['Port'], self.config.mls_port)
        mls = self.client(self.config.mls_port)
        mls.connect(world=info['WorldID'], session=session, access=info['AccessKey'])
        return mls, session

    def join_dme(self, mls, session, world_id):
        r = mls.app('JoinGame', 'JoinGameResponse', MessageID='5', SessionKey=session, MediusWorldID=world_id)
        self.assertEqual(r['StatusCode'], M.SUCCESS)
        info = r['ConnectInfo']
        dme = self.client(info['AddressList'][0]['Port'])
        dme.connect(world=info['WorldID'], session=session, access=info['AccessKey'], aux_udp=True)
        dme.recv(rt.SERVER_INFO_AUX_UDP)
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(udp.close)
        udp.settimeout(5)
        udp.bind(('127.0.0.1', 0))
        udp.sendto(struct.pack('<BH', rt.CLIENT_CONNECT_AUX_UDP, 32) +
                   struct.pack('<Ii16sHHI', info['WorldID'], SFCO_APP_ID, b'127.0.0.1', self.config.dme_udp_port,
                               dme.index, 1), ('127.0.0.1', self.config.dme_udp_port))
        reply = rt.parse(bytearray(udp.recv(2048)), dme.keys)
        self.assertEqual(reply[0].id, rt.SERVER_CONNECT_ACCEPT_AUX_UDP)
        dme.send(rt.CLIENT_CONNECT_READY_AUX_UDP)
        dme.recv(rt.SERVER_CONNECT_COMPLETE)
        dme.udp = udp
        return dme

    # ---- tests

    def post(self, path, fields):
        url = 'http://127.0.0.1:%d%s%s' % (self.config.http_port, self.server.game.svo_path, path)
        data = urllib.parse.urlencode(fields).encode()
        req = urllib.request.Request(url, data, {'Content-Type': 'application/x-www-form-urlencoded'})
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.read()


class ServerTests(ServerCase):
    def test_universe_information(self):
        muis = self.client(self.config.muis_port)
        muis.connect()
        muis.send(rt.CLIENT_APP_TOSERVER, _request('GetUniverseInformation', MessageID='1',
                                                   InfoType=M.INFO_ID | M.INFO_NAME | M.INFO_DNS | M.INFO_SVO_URL))
        replies = {}
        while 0x11 not in replies:
            m = muis.recv(rt.SERVER_APP)
            replies[m.payload[1]] = m.payload[2:]
        self.assertIn(self.server.svo_url().encode(), replies[0x1E])
        info = replies[0x11]
        dns = 21 + 4 + 4 + 4 + 128             # MessageID (unpadded here), StatusCode, InfoType, ID, Name
        self.assertEqual(info[dns:dns + 9], b'127.0.0.1')
        self.assertEqual(struct.unpack_from('<i', info, dns + 128)[0], self.config.mas_port)

    def test_game_hosted_joined_and_relayed(self):
        host_mls, host_session = self.lobby('Host')
        guest_mls, guest_session = self.lobby('Guest')
        created = host_mls.app('CreateGame1', 'CreateGameResponse', MessageID='3', SessionKey=host_session,
                               ApplicationID=SFCO_APP_ID, MinPlayers=1, MaxPlayers=8, GameName='Test Game')
        self.assertEqual(created['StatusCode'], M.SUCCESS)
        world_id = created['MediusWorldID']
        host = self.join_dme(host_mls, host_session, world_id)
        listed = guest_mls.app('GameList_ExtraInfo', 'GameList_ExtraInfoResponse', MessageID='4', PageID=1, PageSize=20)
        self.assertEqual((listed['MediusWorldID'], listed['GameName']), (world_id, 'Test Game'))
        guest = self.join_dme(guest_mls, guest_session, world_id)
        self.assertNotEqual(host.index, guest.index)
        host.recv(rt.SERVER_CONNECT_NOTIFY)

        # TCP: a single message, a list message (encrypted, targets in the clear), a broadcast
        host.send(rt.CLIENT_APP_SINGLE, struct.pack('<h', guest.index) + b'to the guest')
        self.assertEqual(guest.recv(rt.CLIENT_APP_SINGLE).payload, struct.pack('<h', host.index) + b'to the guest')
        guest.send(rt.CLIENT_APP_LIST, bytes((1, 1 << host.index)) + b'loaded')
        self.assertEqual(host.recv(rt.CLIENT_APP_SINGLE).payload, struct.pack('<h', guest.index) + b'loaded')
        guest.send(rt.CLIENT_APP_BROADCAST, b'everyone')
        self.assertEqual(host.recv(rt.CLIENT_APP_SINGLE).payload, struct.pack('<h', guest.index) + b'everyone')

        # UDP
        host.udp.sendto(host.frame(rt.CLIENT_APP_BROADCAST, b'position'), ('127.0.0.1', self.config.dme_udp_port))
        got = rt.parse(bytearray(guest.udp.recv(2048)), guest.keys)
        self.assertEqual((got[0].id, got[0].payload), (rt.CLIENT_APP_SINGLE, struct.pack('<h', host.index) + b'position'))

        # leaving: the game ends with its last player
        host.close()
        guest.close()
        deadline = time.time() + 5
        while world_id in self.server.lobby.games and time.time() < deadline:
            time.sleep(0.05)
        self.assertNotIn(world_id, self.server.lobby.games)

    def test_buddy_invitation(self):
        alice, alice_session = self.lobby('Alice')
        bob, bob_session = self.lobby('Bob')
        bob_id = self.store.find_account(SFCO_APP_ID, 'Bob')['account_id']
        alice.send(rt.CLIENT_APP_TOSERVER, _request('BuddyGetPermission', MessageID='7', SessionKey=alice_session,
                                                    TargetAccountID=bob_id, AddType=0))
        asked = bob.reply('AddToBuddyListFwdConfirmation')
        self.assertEqual(asked['OriginatorAccountName'], 'Alice')
        bob.send(rt.CLIENT_APP_TOSERVER, _request('AddToBuddyListFwdConfirmationResponse', MessageID='1',
                                                  StatusCode=M.REQUEST_ACCEPTED, SessionKey=bob_session,
                                                  OriginatorAccountID=asked['OriginatorAccountID']))
        answer = alice.reply('BuddyGetPermissionResponse')
        self.assertEqual((answer['MessageID'], answer['StatusCode'], answer['TargetAccountName']), ('7', M.SUCCESS, 'Bob'))
        names = [r['name'] for r in self.store.buddies(asked['OriginatorAccountID'])]
        self.assertEqual(names, ['Bob'])
        self.assertEqual([r['name'] for r in self.store.buddies(bob_id)], ['Alice'])

    def test_svo_root_page(self):
        value = '0123456789abcdef' * 2
        body, headers = self.http('', {'X-SVOMac': value})
        self.assertIn(b'TicketLoginURI', body)
        self.assertIn(('https://127.0.0.1:%d' % self.config.https_port).encode(), body)
        self.assertEqual(headers['X-SVOMac'], hashlib.md5((value + 'sp9ck0348sld00000000000000000000').encode()).hexdigest())
        self.assertIsNone(svomac('short'))

    def test_svo_over_ssl3(self):
        client = Ssl3Client(self.config.https_port, self.server.rsa_key)
        self.addCleanup(client.sock.close)
        status, body = client.get(self.server.game.svo_path)
        self.assertEqual(status, 200)
        self.assertIn(b'TicketLoginURI', body)

    def test_mission_server(self):
        listed = ET.fromstring(self.http('mission/ListGet?TransactionID=9&AccountID=1&MissionListType=3')[0])
        names = [m.findtext('MissionName') for m in listed.iter('Mission')]
        self.assertIn('Test Mission', names)
        raw = ET.fromstring(self.http('mission/GetRaw?TransactionID=10&AccountID=1&MissionID=%d&MissionHash=x&'
                                      'IncrementDownloadCount=1' % self.mission_id)[0]).find('SpyWars')
        data = self.http('files/Download?fileID=%s' % raw.get('fileID'))[0]
        self.assertEqual(umd.decode(data), MISSION_INI)
        meta = ET.fromstring(self.http('mission/GetMetadata?TransactionID=11&MissionID=%d&AccountID=1' % self.mission_id)[0])
        self.assertEqual(meta.findtext('SpyWars/Mission/fileDownloadCount'), '1')

    def test_mission_upload(self):
        mission = umd.encode(MISSION_INI.replace(b'Test Mission', b'Uploaded One'))
        submitted = ET.fromstring(self.http('mission/Submit?TransactionID=20&AccountID=1&MissionName=Uploaded%%20One&'
                                            'Theme=7&CombatType=2&Author=Tester&Terrain=5&MissionHash=%s'
                                            % hashlib.md5(mission).hexdigest())[0]).find('SpyWars')
        file_id = submitted.findtext('GeneratedUniqueFileName')
        url = submitted.get('UploadServlet') + '&filePermission=7'
        self.assertIn('fileID=' + file_id, url)
        boundary = 'xYzZY'
        body = ('--%s\r\nContent-Disposition: form-data; name="file"; filename="%s"\r\n'
                'Content-Type: application/octet-stream\r\n\r\n' % (boundary, file_id)).encode() + mission + \
            ('\r\n--%s--\r\n' % boundary).encode()
        req = urllib.request.Request(url, body, {'Content-Type': 'multipart/form-data; boundary=' + boundary})
        with urllib.request.urlopen(req, timeout=5) as r:
            self.assertEqual(r.status, 200)
        mission_id = int(ET.fromstring(self.http('mission/GetID?TransactionID=21&MissionHash=%s'
                                                 % hashlib.md5(mission).hexdigest())[0]).findtext('SpyWars/Mission/MissionID'))
        meta = ET.fromstring(self.http('mission/GetMetadata?TransactionID=22&MissionID=%d&AccountID=1' % mission_id)[0])
        self.assertEqual((meta.findtext('SpyWars/Mission/MissionName'), meta.findtext('SpyWars/Mission/Weather')),
                         ('Uploaded One', '4'))
        self.assertEqual(umd.decode(self.http('files/Download?fileID=%s' % file_id)[0]),
                         MISSION_INI.replace(b'Test Mission', b'Uploaded One'))

    def test_game_report_kept(self):
        self.http('game/Finish?TransactionID=1&AccountID=5')
        row = self.store.one("SELECT * FROM game_reports WHERE kind = 'Finish' ORDER BY report_id DESC LIMIT 1")
        self.assertEqual(row['query'], 'TransactionID=1&AccountID=5')

    def test_ranked_game(self):
        # the root page gives createGameURI with a %d for the mode
        root = self.http('')[0].decode()
        create = [line for line in root.splitlines() if 'createGameURI' in line][0]
        self.assertIn('game/Create?gameMode=%d', create)
        form = ET.fromstring(self.http('game/Create?gameMode=8')[0]).find('Create_Game')
        names = [f.get('name') for f in form]
        self.assertEqual(tuple(names), ('gameMode', 'scertGameID', 'gameName', 'hostPlayerList', 'remotePlayerList',
                                        'timePerDiv', 'numDivs', 'locationID'))
        self.assertIsNone(form.find('status'))
        values = {'gameMode': '8', 'scertGameID': '3', 'gameName': 'Host-1', 'hostPlayerList': '1',
                  'remotePlayerList': '2,3,4', 'timePerDiv': '100', 'numDivs': '2', 'locationID': '1'}
        submit = urllib.parse.urlparse(form.get('action')).path[len(self.server.game.svo_path):]
        done = ET.fromstring(self.http(submit + '?' + urllib.parse.urlencode([(n, values[n]) for n in names]))[0])
        self.assertEqual(done.findtext('Create_Game/status/message'), 'GAME_CREATE_SUCCESS')
        game_id = int(done.findtext('Create_Game/gameID'))
        self.assertGreater(game_id, 0)
        # every player posts its result; the replies must carry <id>0</id>
        reply = self.post('game/Finish', {'SVOGameID': game_id, 'endStatus': 1, 'gameMode': 8, 'totalKills': 7,
                                          'deaths': 2, 'headshots': 1, 'clanid': -1, 'gameResult': 0})
        self.assertIn(b'<id>0</id>', reply)
        row = self.store.one('SELECT * FROM svo_results WHERE game_id = ?', (game_id,))
        self.assertEqual((row['kills'], row['deaths'], row['result']), (7, 2, 0))
        # mission stats come from every player: counted once
        for account in (1, 2):
            self.assertIn(b'<id>0</id>', self.post('mission/PostStats', {
                'SVOGameID': game_id, 'endStatus': 1, 'gameMode': 8, 'TransactionID': 54321, 'AccountID': account,
                'MissionID': self.mission_id, 'PlayTime': 15, 'WinningTeam': 2}))
        m = self.store.one('SELECT * FROM missions WHERE mission_id = ?', (self.mission_id,))
        self.assertEqual((m['team_b_wins'], m['play_time'] >= 15), (1, True))
        # votes: -1 is no vote, -2 inappropriate; plays counted once per match
        plays = m['plays']
        for account, rating in ((1, 4), (2, -1), (3, -2)):
            self.assertIn(b'<id>0</id>', self.post('mission/VoteCast', {
                'SVOGameID': game_id, 'endStatus': 1, 'gameMode': 8, 'TransactionID': 12345, 'AccountID': account,
                'MissionID': self.mission_id, 'Rating': rating}))
        m = self.store.one('SELECT * FROM missions WHERE mission_id = ?', (self.mission_id,))
        votes = {r['account_id']: r['vote'] for r in self.store.q('SELECT * FROM mission_votes WHERE mission_id = ?',
                                                                    (self.mission_id,))}
        self.assertEqual((votes.get(1), votes.get(2), votes.get(3)), (4, None, -2))
        self.assertEqual((m['plays'], m['inappropriate']), (plays + 1, 1))

    def test_stray_crlf_between_requests(self):
        s = socket.create_connection(('127.0.0.1', self.config.http_port), timeout=5)
        self.addCleanup(s.close)
        path = self.server.game.svo_path
        body = b'TransactionID=1'
        s.sendall(b'POST %smission/VoteCast HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n%s\r\n\r\n'
                  b'GET %shome.svml HTTP/1.1\r\nHost: x\r\n\r\n' % (path.encode(), len(body), body, path.encode()))
        data = b''
        while data.count(b'HTTP/1.') < 2:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        self.assertEqual(data.count(b'HTTP/1.0 200') + data.count(b'HTTP/1.1 200'), 2)

    def test_lobby_chat(self):
        alice, alice_session = self.lobby('Chatter1')
        bob, _ = self.lobby('Chatter2')
        text = b'gg everyone'
        alice.send(rt.CLIENT_APP_TOSERVER, bytes((M.LOBBY_EXT, 0x73)) + b'4'.ljust(21, b'\0') +
                   alice_session.encode().ljust(17, b'\0') + bytes(2) + struct.pack('<iii', M.CHAT_BROADCAST, 0, len(text) + 1)
                   + text + b'\0')
        while True:
            m = bob.recv(rt.SERVER_APP)
            if m.payload[:2] == bytes((M.LOBBY_EXT, 0x74)):
                break
        sender, mtype, name, n = struct.unpack_from('<ii32si', m.payload, 2 + 4)
        self.assertEqual((mtype, name.rstrip(b'\0'), m.payload[2 + 48:2 + 48 + n].rstrip(b'\0')),
                         (M.CHAT_BROADCAST, b'Chatter1', text))

    def test_text_filter(self):
        mls, session = self.lobby('Namer')
        text = b'Cell1'
        mls.send(rt.CLIENT_APP_TOSERVER, bytes((M.LOBBY_EXT, 0x75)) + b'9'.ljust(21, b'\0') +
                 session.encode().ljust(17, b'\0') + bytes(4) + struct.pack('<i', len(text)) + text)
        while True:
            m = mls.recv(rt.SERVER_APP)
            if m.payload[:2] == bytes((M.LOBBY_EXT, 0x76)):
                break
        self.assertEqual((m.payload[2:4], len(m.payload[2:]), m.payload[26:]), (b'9\0', 32, bytes(8)))


VISIBLE = {'TEXT', 'BUTTON', 'RECTANGLE', 'LINE', 'TEXTAREA', 'GRID', 'IMAGE', 'LISTBOX', 'SELECT', 'INPUT'}


def check_svml(test, body, where):
    """The rules the game's SVML browser imposes (see portal.py)."""
    text = body.decode('utf-8')
    for entity in re.findall(r'&[^;\s]*;', text):
        test.assertIn(entity, ('&amp;', '&lt;', '&gt;', '&quot;', '&apos;'), where)
    root = ET.fromstring(body)
    test.assertEqual(root.tag, 'SVML', where)
    test.assertLessEqual(len(root), 256, where)
    for el in root:
        for child in el.iter():
            if child is not el and child.tag in VISIBLE:
                test.assertEqual(el.tag, 'FORM', '%s: %s nested in %s' % (where, child.tag, el.tag))
        if el.tag == 'GRID':
            cols = el.find('COLUMNS')
            test.assertIsNotNone(cols, where)
            test.assertEqual(len(cols.findall('COLUMN')), int(el.get('numCols')), where)
            rows = el.find('ROWS').findall('ROW')
            test.assertLessEqual(len(rows), int(el.get('numTotalRows')), where)
            for attr in ('numVisRows', 'colWidth', 'rowHeight'):
                test.assertGreater(float(el.get(attr)), 0, '%s: GRID %s' % (where, attr))
            for row in rows:
                test.assertEqual(len(row.findall('CELL')), int(el.get('numCols')), where)
        if el.tag in ('TEXT', 'BUTTON'):
            test.assertLess(len(el.text or ''), 128, where)
        if el.tag in VISIBLE and el.get('y') is not None:
            test.assertGreaterEqual(float(el.get('y')), 38, '%s: %s under the tab header' % (where, el.tag))
    return root


class PortalTests(ServerCase):
    """The Community pages, with some players, a cell, posts and news to show."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        s = cls.store
        cls.ids = {}
        for i, name in enumerate(('Gabe_Logan', 'Lian_Xing', "Teresa_O'Hara", 'Mara<Aramov>')):
            a = s.account(SFCO_APP_ID, name)
            cls.ids[name] = a['account_id']
            s.set_stats(a['account_id'], stats.encode_player(
                xp=100 * (i + 1), kills=20 * (i + 1), deaths=10, headshots=i, played={'TDM': 3 + i, 'RET': 2},
                won={'TDM': 1 + i, 'RET': 1}, online_id=name))
        clan = s.create_clan(SFCO_APP_ID, 'Shadow Ops', cls.ids['Gabe_Logan'])
        s.invite_to_clan(clan, cls.ids['Lian_Xing'], '')
        s.x("UPDATE clan_invitations SET response = 1")
        s.x('INSERT OR IGNORE INTO clan_members (clan_id, account_id) VALUES (?, ?)', (clan, cls.ids['Lian_Xing']))
        s.add_clan_message(clan, 'Train hard & fight "smart"')
        s.record_cell_match(clan, 'TDM', 'k1', True)
        cls.clan = clan
        s.add_news('Servers are back online!')
        cls.topic = s.add_board_post('general', cls.ids['Gabe_Logan'], 'Gabe_Logan', 'Hello <agents>', 'First post & more')
        s.add_board_post('general', cls.ids['Lian_Xing'], 'Lian_Xing', '', 'Welcome back', cls.topic)

    def page(self, path, cookie=None):
        headers = {'Cookie': 'AcctID=%d' % cookie} if cookie else {}
        body, h = self.http(path, headers)
        self.assertEqual(h.get('Content-Type'), 'text/svml', path)
        return check_svml(self, body, path), body

    def test_every_page_is_valid_svml(self):
        me = self.ids['Lian_Xing']
        paths = ['home.svml', 'news.svml', 'find.svml', 'board.svml', 'topics.svml?b=general', 'topic.svml?id=%d' % self.topic,
                 'post.svml?b=general', 'post.svml?b=general&reply=%d' % self.topic, 'cells.svml', 'cells.svml?b=xp',
                 'cell.svml?id=%d' % self.clan, 'cell.svml?id=999', 'agent.svml?id=%d' % me, 'agent.svml?name=Nobody',
                 'missions.svml', 'missions.svml?b=played', 'missions.svml?b=new', 'mission.svml?id=%d' % self.mission_id]
        paths += ['lb.svml?b=%s' % key for key in records.PLAYER_BOARDS] + ['lb.svml?b=xp&p=5']
        for path in paths:
            self.page(path, me)

    def test_leaderboard(self):
        root, body = self.page('lb.svml?b=xp', self.ids['Lian_Xing'])
        rows = root.find('GRID/ROWS').findall('ROW')
        names = [r.findall('CELL')[1].text for r in rows]
        self.assertEqual(names[0], 'Mara<Aramov>')                  # the most XP
        self.assertIn(b'Teresa_O&apos;Hara', body)
        mine = root.find('GRID/ROWS/MY_RECORD_ROW').findall('CELL')
        self.assertEqual(mine[1].text, 'Lian_Xing')

    def test_agent_and_cell(self):
        root, body = self.page('agent.svml?name=Gabe_Logan')
        self.assertIn(b'Rank R1', body)
        self.assertIn(b'Shadow Ops', body)
        root, body = self.page('cell.svml?id=%d' % self.clan)
        self.assertIn(b'Train hard &amp; fight &quot;smart&quot;', body)
        self.assertIn(b'1 won / 1', body)

    def test_cell_stats_for_the_game(self):
        # the game's own cell profile reads CellGetStatsURI
        root = ET.fromstring(self.http('cell/GetStats?TransactionID=7&CellD=%d' % self.clan)[0]).find('SpyWars')
        self.assertEqual((root.findtext('TransactionID'), root.findtext('ClanName')), ('7', 'Shadow Ops'))
        self.assertEqual((root.findtext('TeamDeathmatch/PlayCount'), root.findtext('TeamDeathmatch/WinCount')), ('1', '1'))
        self.assertEqual(root.findtext('Sabotage/PlayCount'), '0')

    def test_message_board_post_and_reply(self):
        author = self.ids['Teresa_O\'Hara']
        url = 'http://127.0.0.1:%d%sboard/submit' % (self.config.http_port, self.server.game.svo_path)

        def post(fields):
            req = urllib.request.Request(url, urllib.parse.urlencode(fields).encode(),
                                         {'Content-Type': 'application/x-www-form-urlencoded', 'Cookie': 'AcctID=%d' % author})
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.read()
        reply = post({'subject': 'LFG tonight', 'b': 'games', 'reply': 0, 'body': 'Anyone up for TDM?'})
        self.assertIn(b'Posted.', reply)
        check_svml(self, reply, 'board/submit')
        topic = self.store.one("SELECT * FROM board_posts WHERE board = 'games' ORDER BY post_id DESC LIMIT 1")
        self.assertEqual((topic['author'], topic['subject']), ("Teresa_O'Hara", 'LFG tonight'))
        self.assertIn(b'Please wait', post({'b': 'games', 'reply': topic['post_id'], 'body': 'too fast'}))
        self.store.x('UPDATE board_posts SET created = created - 60')
        post({'b': 'games', 'reply': topic['post_id'], 'body': 'Count me in'})
        root, body = self.page('topic.svml?id=%d' % topic['post_id'])
        self.assertIn(b'Count me in', body)
        root, body = self.page('topics.svml?b=games')
        self.assertIn(b'LFG tonight', body)
        anonymous = urllib.request.Request(url, b'b=games&body=hi', {'Content-Type': 'application/x-www-form-urlencoded'})
        with urllib.request.urlopen(anonymous, timeout=5) as r:
            self.assertIn(b'Sign in', r.read())


class StatsTests(unittest.TestCase):
    def test_round_trip(self):
        blob = stats.encode_player(cell_id=4, cell_patch=12, cell_name='Shadows', cell_tag='SHD', xp=350,
                                   played={'TDM': 5, 'DM': 2}, won={'TDM': 3}, kills=40, deaths=20, headshots=9,
                                   online_id='Gabe')
        s = stats.decode_player(blob)
        self.assertEqual((s['cell_id'], s['cell_tag'], s['rank'], s['xp'], s['games'], s['wins'], s['won']['TDM']),
                         (4, 'SHD', 'R3', 350, 7, 3, 3))
        self.assertEqual((stats.rank_of(0), stats.rank_of(99), stats.rank_of(100), stats.rank_of(1000)),
                         ('R0', 'R0', 'R1', 'T3'))
        self.assertIsNone(stats.decode_player(bytes(256)))
        self.assertIsNone(stats.decode_player(b'short'))

    def test_progress(self):
        old = stats.encode_player(xp=300, played={'TDM': 5}, won={'TDM': 2}, kills=10)
        self.assertTrue(stats.progress(old, stats.encode_player(xp=320, played={'TDM': 6}, won={'TDM': 2}, kills=12)))
        self.assertFalse(stats.progress(old, stats.encode_player(xp=0)))               # a reset console
        self.assertFalse(stats.progress(old, b'garbage'))
        self.assertTrue(stats.progress(b'', stats.encode_player(xp=0)))                 # a new account
        self.assertTrue(stats.progress(old, stats.encode_player(xp=300, played={'TDM': 5}, won={'TDM': 2}, kills=10,
                                                                cell_id=3)))           # joined a cell


class RecordsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.store = Store(os.path.join(self.tmp, 'r.sqlite'))
        game = GAMES['sfco']()
        self.server = types.SimpleNamespace(store=self.store, game=game, lobby=types.SimpleNamespace(by_account={}))
        self.records = Records(self.server)
        self.ids = {}
        for name, xp, kills, wins in (('Alpha', 500, 30, 4), ('Bravo', 500, 50, 6), ('Charlie', 900, 10, 1),
                                      ('Delta', 0, 0, 0)):
            a = self.store.account(SFCO_APP_ID, name)
            self.ids[name] = a['account_id']
            self.store.set_stats(a['account_id'], stats.encode_player(xp=xp, kills=kills, played={'TDM': wins + 1},
                                                                      won={'TDM': wins}))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.addCleanup(self.store.close)

    def test_player_boards(self):
        xp = [(pos, name) for pos, _, name, _, _ in self.records.board('xp')]
        self.assertEqual(xp, [(1, 'Charlie'), (2, 'Bravo'), (3, 'Alpha'), (4, 'Delta')])   # ties: more wins first
        kills = [name for _, _, name, _, _ in self.records.board('kills')]
        self.assertEqual(kills, ['Bravo', 'Alpha', 'Charlie'])                             # no zero rows
        self.assertEqual(self.records.board('won_TDM')[0][3], 6)

    def test_cell_matches_count_once(self):
        clan = self.store.create_clan(SFCO_APP_ID, 'Shadows', self.ids['Alpha'])
        game = types.SimpleNamespace(id=7, match=1)
        for name in ('Alpha', 'Bravo'):
            old = self.store.account_by_id(self.ids[name])['stats']
            s = stats.decode_player(old)
            s['played']['TDM'] += 1
            s['won']['TDM'] += 1 if name == 'Bravo' else 0
            new = stats.encode_player(cell_id=clan, xp=s['xp'], played=s['played'], won=s['won'], kills=s['kills'])
            self.records.record_progress(self.ids[name], old, new, game)
        self.assertEqual(self.store.cell_results(clan), {'TDM': (1, 1)})
        board = self.records.cell_board('wins')
        self.assertEqual((board[0][1]['clan']['name'], board[0][1]['wins']), ('Shadows', 1))


if __name__ == '__main__':
    unittest.main()
