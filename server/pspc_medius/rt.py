# SPDX-License-Identifier: GPL-2.0-or-later
"""SCE-RT, the transport under Medius: framing, the encryption handshake, connection setup.

A message is [id u8][length u16 LE][payload]; id | 0x80 marks an encrypted payload, which is then
preceded by a 4-byte hash (crypto.py). Several messages may follow each other in one TCP read or
UDP datagram.

Message ids and layouts follow horizon-server's RT.Models (MIT, (c) 2020 Daniel Gerendasy), checked
against Syphon Filter: Combat Ops (Medius client library 3.02, which says "version 113").
"""

import asyncio
import logging
import struct

from . import crypto

# RT message ids
CLIENT_CONNECT_TCP = 0x00
CLIENT_DISCONNECT = 0x01
CLIENT_APP_BROADCAST = 0x02
CLIENT_APP_SINGLE = 0x03
CLIENT_APP_LIST = 0x04
CLIENT_ECHO = 0x05
SERVER_CONNECT_REJECT = 0x06
SERVER_CONNECT_ACCEPT_TCP = 0x07
SERVER_CONNECT_NOTIFY = 0x08
SERVER_DISCONNECT_NOTIFY = 0x09
SERVER_APP = 0x0A
CLIENT_APP_TOSERVER = 0x0B
UDP_APP = 0x0C
CLIENT_SET_RECV_FLAG = 0x0D
CLIENT_SET_AGG_TIME = 0x0E
CLIENT_FLUSH_ALL = 0x0F
CLIENT_FLUSH_SINGLE = 0x10
SERVER_FORCED_DISCONNECT = 0x11
CLIENT_CRYPTKEY_PUBLIC = 0x12
SERVER_CRYPTKEY_PEER = 0x13
SERVER_CRYPTKEY_GAME = 0x14
CLIENT_CONNECT_TCP_AUX_UDP = 0x15
CLIENT_CONNECT_AUX_UDP = 0x16
CLIENT_CONNECT_READY_AUX_UDP = 0x17
SERVER_INFO_AUX_UDP = 0x18
SERVER_CONNECT_ACCEPT_AUX_UDP = 0x19
SERVER_CONNECT_COMPLETE = 0x1A
CLIENT_CRYPTKEY_PEER = 0x1B
SERVER_SYSTEM_MESSAGE = 0x1C
SERVER_CHEAT_QUERY = 0x1D
SERVER_MEMORY_POKE = 0x1E
SERVER_ECHO = 0x1F
CLIENT_DISCONNECT_WITH_REASON = 0x20
CLIENT_CONNECT_READY_TCP = 0x21
SERVER_CONNECT_REQUIRE = 0x22
CLIENT_CONNECT_READY_REQUIRE = 0x23
CLIENT_HELLO = 0x24
SERVER_HELLO = 0x25
SERVER_STARTUP_INFO_NOTIFY = 0x26
CLIENT_PEER_QUERY = 0x27
SERVER_PEER_QUERY_NOTIFY = 0x28
CLIENT_PEER_QUERY_LIST = 0x29
SERVER_PEER_QUERY_LIST_NOTIFY = 0x2A
CLIENT_WALLCLOCK_QUERY = 0x2B
SERVER_WALLCLOCK_QUERY_NOTIFY = 0x2C
CLIENT_TIMEBASE_QUERY = 0x2D
SERVER_TIMEBASE_QUERY_NOTIFY = 0x2E
CLIENT_TOKEN_MESSAGE = 0x2F
SERVER_TOKEN_MESSAGE = 0x30
CLIENT_SYSTEM_MESSAGE = 0x31
CLIENT_APP_BROADCAST_QOS = 0x32
CLIENT_APP_SINGLE_QOS = 0x33
CLIENT_APP_LIST_QOS = 0x34
CLIENT_MAX_MSGLEN = 0x35
SERVER_MAX_MSGLEN = 0x36
CLIENT_MULTI_APP_TOSERVER = 0x3B
SERVER_MULTI_APP_TOCLIENT = 0x3C

NAMES = {v: k for k, v in globals().items() if k.isupper() and isinstance(v, int) and k not in ('HEADER',)}

# RT_RECV_FLAG bits (CLIENT_CONNECT_READY_TCP, CLIENT_SET_RECV_FLAG)
RECV_BROADCAST, RECV_LIST, RECV_SINGLE, RECV_NOTIFICATION = 1, 2, 4, 8

# The server certificate sent in SERVER_HELLO to 112/113 clients: SCE-RT's "MAS 1" certificate
# (as horizon-server sends it), whose RSA modulus is replaced by the server's own at MODULUS_AT.
# Clients take the server key from it and don't check the signature.
HELLO_CERT = bytes.fromhex(
    '71000006e702308202e3308201cba00302010202140100000000000000000000001100000000000001300d06092a86'
    '4886f70d0101050500308196310b3009060355040613025553310b3009060355040813024341311230100603550407'
    '130953616e20446965676f3131302f060355040a1328534f4e5920436f6d707574657220456e7465727461696e6d65'
    '6e7420416d657269636120496e632e31143012060355040b130b53434552542047726f7570311d301b060355040313'
    '14534345525420526f6f7420417574686f72697479301e170d3035303432363231303133385a170d33353034323532'
    '33353935395a308187310b3009060355040613025553310b3009060355040813024341311230100603550407130953'
    '616e20446965676f3131302f060355040a1328534f4e5920436f6d707574657220456e7465727461696e6d656e7420'
    '416d657269636120496e632e31143012060355040b130b53434552542047726f7570310e300c060355040313054d41'
    '532031305c300d06092a864886f70d0101010500034b003048024100c4f75716ec835d2325689f91ff85ed9bfc3211'
    'db9c164f41852e264e569d2802008054a0ef459e7e3eabb87fae576e735434d1d124b30b11bd6de098148601550203'
    '000011300d06092a864886f70d010105050003820101006c91abeeb59ac01dfbb080646e4df616f833c36a5a448773'
    'f7c1acb8ec162ff811ab11f8051294e20754361827b259a534b010dfbb42e56b571ae453779682ca8650ac5dd0b388'
    '8cfb16fb858e5c39dff094380bc4f6f0268ade80c22878afc4c16099c64d435a9ab67101e63b0f5336febb1f71683b'
    'a0b0ac7eab2ef0d10d9324b6ce5683d1ab359deda17c47f2a253162674be37c2ce185d90c76b7fd7d9983c289747ad'
    '10828b385b82d7eb18f52f2eced4c3a65b0dd63dd8c83c5f92203829fdbf1a85c78b869283b0b1d5fe1bb5e85749ab'
    'f50e46d9decca190c2d954b6e442e58fbde9958af397e9af575e1f76d63f35ee5987406c109db7da50557e86000000'
    '00')
MODULUS_AT = 0x194

# The key in HELLO_CERT, whose private exponent is known (horizon-server's DefaultKey).
DEFAULT_KEY = crypto.RsaKey(
    n=0xc4f75716ec835d2325689f91ff85ed9bfc3211db9c164f41852e264e569d2802008054a0ef459e7e3eabb87fae576e735434d1d124b30b11bd6de09814860155,
    d=0x5cb0a1743310a44cc64f5a26965d2485a3db53b2a3ce436a208e3024dd7721e1ffb72135f578033ed5d6a8b58b4013ff616a47cadb6072b5752495ebf2a7b7f1)

log = logging.getLogger('medius.rt')


def hello_certificate(key):
    cert = bytearray(HELLO_CERT)
    cert[MODULUS_AT:MODULUS_AT + 64] = key.modulus_be()
    return bytes(cert)


class Message:
    """payload is the plaintext. raw: an encrypted message passed on unread (the 4-byte hash, then the
    payload as sent), for game messages players encrypt for each other with the world's game key."""
    __slots__ = ('id', 'payload', 'raw')

    def __init__(self, msg_id, payload=b'', raw=None):
        self.id = msg_id
        self.payload = bytes(payload)
        self.raw = raw

    def __repr__(self):
        return '%s(%d bytes)' % (NAMES.get(self.id, '0x%02X' % self.id), len(self.payload))


# Messages whose first two payload bytes (a player index) stay plaintext when the rest is encrypted.
# The APP_LIST ones likewise keep their target list ([mask length][bitmask]) plaintext.
INDEXED = None  # set below, once the ids exist
LISTED = None


def plain_prefix(msg_id, body):
    """How many leading bytes of an encrypted message's body are not encrypted."""
    if msg_id in INDEXED:
        return 2
    if msg_id in LISTED and body:
        return 1 + body[0]
    return 0


class Crypto:
    """One connection's keys. Before the handshake nothing is encrypted."""

    def __init__(self, server_key):
        self.server_key = server_key   # decrypts the client's CRYPTKEY_PUBLIC
        self.client_key = None         # encrypts our CRYPTKEY_PEER
        self.session = None            # RcqCipher, for everything after
        self.encrypt_out = True

    def decrypt(self, msg_id, h, payload):
        ctx = crypto.hash_context(h)
        if crypto.is_plaintext_hash(h):
            return payload
        if ctx == crypto.CTX_RSA_AUTH:
            plain = self.server_key.decrypt(payload, h)
        elif self.session is not None:
            plain = self.session.decrypt(payload, h)
        else:
            plain = None
        if plain is None:
            raise ValueError('cannot decrypt %s (context %d)' % (NAMES.get(msg_id, hex(msg_id)), ctx))
        return plain

    def encode(self, msg):
        payload = msg.payload
        if msg.id in INDEXED and len(payload) > 2 and self.encrypt_out and self.session is not None:
            cipher, h = self.session.encrypt(payload[2:])
            return struct.pack('<BH', msg.id | 0x80, len(payload)) + h + payload[:2] + cipher
        if payload and self.encrypt_out:
            if msg.id == SERVER_CRYPTKEY_PEER and self.client_key is not None:
                cipher, h = self.client_key.encrypt(payload)
            elif self.session is not None and msg.id != SERVER_CRYPTKEY_PEER:
                cipher, h = self.session.encrypt(payload)
            else:
                cipher = None
            if cipher is not None:
                return struct.pack('<BH', msg.id | 0x80, len(cipher)) + h + cipher
        return struct.pack('<BH', msg.id, len(payload)) + payload


INDEXED = (CLIENT_APP_SINGLE, CLIENT_APP_SINGLE_QOS, CLIENT_CRYPTKEY_PEER)
LISTED = (CLIENT_APP_LIST, CLIENT_APP_LIST_QOS)


def parse(buf, keys, opaque=()):
    """Splits complete messages off the front of buf (a bytearray, consumed). Returns [Message].
    Encrypted messages whose id is in opaque that this connection's keys can't decrypt are passed on
    unread (Message.raw)."""
    out = []
    while len(buf) >= 3:
        raw_id, length = struct.unpack_from('<BH', buf, 0)
        encrypted = raw_id & 0x80 and length > 0
        need = 3 + (4 if encrypted else 0) + length
        if len(buf) < need:
            break
        msg_id = raw_id & 0x7F
        if encrypted:
            h = bytes(buf[3:7])
            body = bytes(buf[7:need])
            try:
                n = plain_prefix(msg_id, body)
                if not keys:
                    payload = body
                elif len(body) > n:
                    payload = body[:n] + keys.decrypt(msg_id, h, body[n:])
                else:
                    payload = body
            except ValueError:
                if msg_id not in opaque:
                    raise
                out.append(Message(msg_id, b'', raw=h + body))   # for someone else to decrypt
                del buf[:need]
                continue
        else:
            payload = bytes(buf[3:need])
        del buf[:need]
        if msg_id == CLIENT_MULTI_APP_TOSERVER:
            inner = bytearray(payload)
            out.extend(parse(inner, None))
        else:
            out.append(Message(msg_id, payload))
    return out


class Connection(asyncio.Protocol):
    """A client's TCP connection to one of the servers. The SCE-RT handshake (hello, key exchange,
    connect) is handled here; each server subclass handles on_connect (CLIENT_CONNECT_TCP...) and
    on_message (everything after)."""

    service = 'rt'
    opaque_ids = ()

    def __init__(self, server):
        self.server = server
        self.transport = None
        self.buf = bytearray()
        self.keys = Crypto(server.rsa_key)
        self.version = 108
        self.peer = ('?', 0)
        self.closed = False

    # ---- asyncio

    def connection_made(self, transport):
        self.transport = transport
        self.peer = transport.get_extra_info('peername') or ('?', 0)
        log.debug('%s: connection from %s:%d', self.service, *self.peer[:2])

    def data_received(self, data):
        self.buf += data
        try:
            messages = parse(self.buf, self.keys, self.opaque_ids)
        except ValueError as e:
            log.warning('%s %s:%d: %s; disconnecting', self.service, self.peer[0], self.peer[1], e)
            self.close()
            return
        for m in messages:
            if log.isEnabledFor(logging.DEBUG):
                log.debug("%s <- %s %s", self.service, m, m.payload.hex())
            try:
                self.handle(m)
            except Exception:
                log.exception('%s: error handling %r', self.service, m)

    def connection_lost(self, exc):
        self.closed = True
        try:
            self.on_close()
        except Exception:
            log.exception('%s: error on close', self.service)

    # ---- sending

    def send(self, msg_id, payload=b'', encrypt=True):
        if self.closed or self.transport is None:
            return
        m = Message(msg_id, payload)
        if log.isEnabledFor(logging.DEBUG):
            log.debug('%s -> %s %s', self.service, m, m.payload[:96].hex())
        if encrypt:
            self.transport.write(self.keys.encode(m))
        else:
            self.transport.write(struct.pack('<BH', msg_id, len(payload)) + bytes(payload))

    def close(self):
        if self.transport is not None and not self.closed:
            self.transport.close()

    # ---- the SCE-RT handshake

    def handle(self, m):
        if m.id == CLIENT_HELLO:
            words = struct.unpack_from('<%dH' % (len(m.payload) // 2), m.payload)
            self.version = words[1] if len(words) > 1 else 108
            if self.version >= 112:
                self.send(SERVER_HELLO, self.hello_payload())
            else:
                self.send(SERVER_HELLO, struct.pack('<HH', 0x006E, 0x0001))
        elif m.id == CLIENT_CRYPTKEY_PUBLIC:
            self.keys.client_key = crypto.RsaKey.from_wire(m.payload[:64])
            self.keys.session = crypto.RcqCipher(crypto.session_key(self.keys.client_key))
            self.send(SERVER_CRYPTKEY_PEER, self.keys.session.key)
        elif m.id == CLIENT_ECHO:
            self.send(CLIENT_ECHO, m.payload[:1] or b'\xa5')
        elif m.id == SERVER_ECHO:
            pass
        elif m.id in (CLIENT_DISCONNECT, CLIENT_DISCONNECT_WITH_REASON):
            self.close()
        else:
            self.on_message(m)

    def hello_payload(self):
        return hello_certificate(self.server.rsa_key)

    def connect_accept(self, player_id=0, scert_id=1, player_count=1):
        ip = self.peer[0].encode()[:15]
        if self.version > 108:
            payload = struct.pack('<HIH16s', player_id, scert_id, player_count, ip)
        else:
            payload = b'\x01\x08\x10' + struct.pack('<HH16s', player_id, player_count, ip)
        self.send(SERVER_CONNECT_ACCEPT_TCP, payload)

    def connect_require(self, max_packet=584, max_udp=None):
        payload = struct.pack('<Bh', 0, max_packet)
        if max_udp is not None:
            payload += struct.pack('<h', max_udp)
        self.send(SERVER_CONNECT_REQUIRE, payload)

    def connect_complete(self, count=1):
        self.send(SERVER_CONNECT_COMPLETE, struct.pack('<H', count))

    def send_app(self, data):
        self.send(SERVER_APP, data)

    # ---- for subclasses

    def on_message(self, m):
        log.info('%s: unhandled %r %s', self.service, m, m.payload[:64].hex())

    def on_close(self):
        pass


def parse_connect_tcp(payload, version):
    """CLIENT_CONNECT_TCP / CLIENT_CONNECT_TCP_AUX_UDP: (target world id, app id, session key, access
    token); the keys are '' when absent."""
    if version > 108:
        world, app = struct.unpack_from('<Ii', payload, 0)
        rest = payload[72:]
    else:
        world, = struct.unpack_from('<H', payload, 3)
        app, = struct.unpack_from('<i', payload, 5)
        rest = payload[73:]
    session = rest[:17].split(b'\0', 1)[0].decode('latin-1') if len(rest) >= 17 else ''
    token = rest[17:34].split(b'\0', 1)[0].decode('latin-1') if len(rest) >= 34 else ''
    return world, app, session, token
