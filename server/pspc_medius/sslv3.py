# SPDX-License-Identifier: GPL-2.0-or-later
"""A minimal SSL 3.0 server for SCE-RT's rt_ssl 3.02 client (the game's HTTPS: SVO ticket login).

The client sends an SSLv2-format ClientHello offering SSL 3.0 and one cipher suite,
SSL_RSA_WITH_RC4_128_MD5 - neither of which modern TLS libraries still do - so this implements just
that: RSA key exchange (PKCS#1 v1.5), the SSL 3.0 key derivation, MACs and Finished messages
(RFC 6101), and RC4. Application data is handed to an HTTP handler through a stream pair.
"""

import asyncio
import hashlib
import logging
import os
import struct

log = logging.getLogger('medius.ssl')

SSL3 = 0x0300
CT_CHANGE_CIPHER_SPEC, CT_ALERT, CT_HANDSHAKE, CT_APPLICATION_DATA = 20, 21, 22, 23
HS_CLIENT_HELLO, HS_SERVER_HELLO, HS_CERTIFICATE, HS_SERVER_HELLO_DONE = 1, 2, 11, 14
HS_CLIENT_KEY_EXCHANGE, HS_FINISHED = 16, 20
RSA_WITH_RC4_128_MD5 = 0x0004
PAD1_MD5, PAD2_MD5 = b'\x36' * 48, b'\x5c' * 48
PAD1_SHA, PAD2_SHA = b'\x36' * 40, b'\x5c' * 40


class Rc4:
    def __init__(self, key):
        s = list(range(256))
        j = 0
        for i in range(256):
            j = (j + s[i] + key[i % len(key)]) & 255
            s[i], s[j] = s[j], s[i]
        self.s, self.i, self.j = s, 0, 0

    def __call__(self, data):
        s, i, j = self.s, self.i, self.j
        out = bytearray(len(data))
        for n, b in enumerate(data):
            i = (i + 1) & 255
            j = (j + s[i]) & 255
            s[i], s[j] = s[j], s[i]
            out[n] = b ^ s[(s[i] + s[j]) & 255]
        self.i, self.j = i, j
        return bytes(out)


def _ssl3_hash_block(secret, seed, length):
    """SSL 3.0 key expansion: MD5(secret + SHA1('A' + secret + seed)) + MD5(secret + SHA1('BB' ...))..."""
    out = b''
    n = 0
    while len(out) < length:
        label = bytes([0x41 + n]) * (n + 1)
        out += hashlib.md5(secret + hashlib.sha1(label + secret + seed).digest()).digest()
        n += 1
    return out[:length]


class Session:
    """One SSL connection's state machine (handshake, then records)."""

    def __init__(self, rsa_key, certificates):
        self.key = rsa_key
        self.certificates = certificates
        self.handshake = b''          # all handshake messages so far, for Finished
        self.client_random = self.server_random = b''
        self.master = None
        self.read_cipher = self.write_cipher = None
        self.read_mac = self.write_mac = None
        self.read_seq = self.write_seq = 0
        self.pending_read = None      # keys to switch to at the client's ChangeCipherSpec
        self.established = False

    # ---- records

    def record(self, ctype, payload):
        if self.write_cipher is not None:
            mac = self._mac(self.write_mac, self.write_seq, ctype, payload)
            self.write_seq += 1
            payload = self.write_cipher(payload + mac)
        return struct.pack('>BHH', ctype, SSL3, len(payload)) + payload

    def _mac(self, secret, seq, ctype, data):
        inner = hashlib.md5(secret + PAD1_MD5 + struct.pack('>QBH', seq, ctype, len(data)) + data).digest()
        return hashlib.md5(secret + PAD2_MD5 + inner).digest()

    def open_record(self, ctype, payload):
        if self.read_cipher is None:
            return payload
        plain = self.read_cipher(payload)
        data, mac = plain[:-16], plain[-16:]
        if self._mac(self.read_mac, self.read_seq, ctype, data) != mac:
            raise ValueError('bad record MAC')
        self.read_seq += 1
        return data

    # ---- handshake

    def hs(self, htype, body):
        msg = struct.pack('>B', htype) + len(body).to_bytes(3, 'big') + body
        self.handshake += msg
        return msg

    def on_v2_client_hello(self, msg):
        """SSLv2-format ClientHello: msg = [type 1][version][cipher len][session len][challenge len]..."""
        version, cs_len, sid_len, ch_len = struct.unpack_from('>HHHH', msg, 1)
        specs = msg[9:9 + cs_len]
        challenge = msg[9 + cs_len + sid_len:9 + cs_len + sid_len + ch_len]
        suites = [int.from_bytes(specs[i:i + 3], 'big') for i in range(0, len(specs), 3)]
        log.debug('ssl: v2 ClientHello version %04x suites %s', version, ['%06x' % s for s in suites])
        if RSA_WITH_RC4_128_MD5 not in suites:
            raise ValueError('client offers no RC4-MD5 (%s)' % suites)
        self.handshake += msg                         # the v2 hello counts as handshake data as-is
        self.client_random = bytes(32 - len(challenge)) + challenge
        return self._server_flight()

    def on_client_hello(self, body):
        version = struct.unpack_from('>H', body, 0)[0]
        self.client_random = body[2:34]
        sid_len = body[34]
        pos = 35 + sid_len
        cs_len = struct.unpack_from('>H', body, pos)[0]
        suites = [struct.unpack_from('>H', body, pos + 2 + i)[0] for i in range(0, cs_len, 2)]
        log.debug('ssl: ClientHello version %04x suites %s', version, suites)
        if RSA_WITH_RC4_128_MD5 not in suites:
            raise ValueError('client offers no RC4-MD5 (%s)' % suites)
        return self._server_flight()

    def _server_flight(self):
        self.server_random = struct.pack('>I', int.from_bytes(os.urandom(4), 'big')) + os.urandom(28)
        hello = struct.pack('>H', SSL3) + self.server_random + b'\x00' + struct.pack('>HB', RSA_WITH_RC4_128_MD5, 0)
        chain = b''.join(len(c).to_bytes(3, 'big') + c for c in self.certificates)
        out = self.hs(HS_SERVER_HELLO, hello)
        out += self.hs(HS_CERTIFICATE, len(chain).to_bytes(3, 'big') + chain)
        out += self.hs(HS_SERVER_HELLO_DONE, b'')
        return self.record(CT_HANDSHAKE, out)

    def on_client_key_exchange(self, body):
        # SSL 3.0 sends the RSA block without a length prefix (TLS added one); accept both.
        if len(body) == self.key.size + 2 and int.from_bytes(body[:2], 'big') == self.key.size:
            body = body[2:]
        m = pow(int.from_bytes(body, 'big'), self.key.d, self.key.n).to_bytes(self.key.size, 'big')
        if m[0] != 0 or m[1] != 2 or 0 not in m[2:]:
            raise ValueError('bad premaster padding')
        premaster = m[m.index(0, 2) + 1:]
        if len(premaster) != 48:
            raise ValueError('bad premaster length %d' % len(premaster))
        self.master = _ssl3_hash_block(premaster, self.client_random + self.server_random, 48)
        block = _ssl3_hash_block(self.master, self.server_random + self.client_random, 64)
        client_mac, server_mac, client_key, server_key = block[0:16], block[16:32], block[32:48], block[48:64]
        self.pending_read = (Rc4(client_key), client_mac)
        self._pending_write = (Rc4(server_key), server_mac)

    def finished_hash(self, sender):
        md5 = hashlib.md5(self.master + PAD2_MD5 +
                          hashlib.md5(self.handshake + sender + self.master + PAD1_MD5).digest()).digest()
        sha = hashlib.sha1(self.master + PAD2_SHA +
                           hashlib.sha1(self.handshake + sender + self.master + PAD1_SHA).digest()).digest()
        return md5 + sha

    def on_change_cipher_spec(self):
        if self.pending_read is None:
            raise ValueError('ChangeCipherSpec before key exchange')
        self.read_cipher, self.read_mac = self.pending_read
        self.read_seq = 0

    def on_finished(self, body):
        if body != self.finished_hash(b'CLNT'):
            raise ValueError('client Finished does not verify')
        self.handshake += struct.pack('>B', HS_FINISHED) + len(body).to_bytes(3, 'big') + body
        out = self.record(CT_CHANGE_CIPHER_SPEC, b'\x01')
        self.write_cipher, self.write_mac = self._pending_write
        self.write_seq = 0
        out += self.record(CT_HANDSHAKE, self.hs(HS_FINISHED, self.finished_hash(b'SRVR')))
        self.established = True
        return out


class _Writer:
    """What http.serve's client coroutine writes to: encrypts into SSL records."""

    def __init__(self, protocol):
        self.p = protocol

    def write(self, data):
        for i in range(0, len(data), 16384):
            self.p.transport.write(self.p.session.record(CT_APPLICATION_DATA, data[i:i + 16384]))

    async def drain(self):
        pass

    def close(self):
        if not self.p.transport.is_closing():
            self.p.transport.write(self.p.session.record(CT_ALERT, b'\x01\x00'))  # close_notify
            self.p.transport.close()

    def get_extra_info(self, name, default=None):
        return self.p.transport.get_extra_info(name, default)


class SslServerProtocol(asyncio.Protocol):
    def __init__(self, rsa_key, certificates, client_cb):
        self.session = Session(rsa_key, certificates)
        self.client_cb = client_cb
        self.buf = bytearray()
        self.hs_buf = b''
        self.reader = None
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def data_received(self, data):
        self.buf += data
        try:
            self._process()
        except (ValueError, IndexError, struct.error) as e:
            log.warning('ssl %s: %s', (self.transport.get_extra_info('peername') or ('?',))[0], e)
            self.transport.close()

    def connection_lost(self, exc):
        if self.reader is not None:
            self.reader.feed_eof()

    def _process(self):
        while self.buf:
            if not self.session.established and self.session.client_random == b'' and self.buf[0] & 0x80:
                # SSLv2-format ClientHello: 2-byte length with the top bit set.
                if len(self.buf) < 2:
                    return
                n = ((self.buf[0] & 0x7F) << 8) | self.buf[1]
                if len(self.buf) < 2 + n:
                    return
                msg = bytes(self.buf[2:2 + n])
                del self.buf[:2 + n]
                if msg[0] != HS_CLIENT_HELLO:
                    raise ValueError('unexpected SSLv2 message %d' % msg[0])
                self.transport.write(self.session.on_v2_client_hello(msg))
                continue
            if len(self.buf) < 5:
                return
            ctype, version, length = struct.unpack_from('>BHH', self.buf, 0)
            if len(self.buf) < 5 + length:
                return
            payload = self.session.open_record(ctype, bytes(self.buf[5:5 + length]))
            del self.buf[:5 + length]
            if ctype == CT_HANDSHAKE:
                self.hs_buf += payload
                self._handshake_messages()
            elif ctype == CT_CHANGE_CIPHER_SPEC:
                self.session.on_change_cipher_spec()
            elif ctype == CT_ALERT:
                log.debug('ssl: alert %s', payload.hex())
                if len(payload) >= 2 and payload[0] == 2:
                    raise ValueError('client alert %d' % payload[1])
                if payload[1:2] == b'\x00':
                    self.transport.close()
                    return
            elif ctype == CT_APPLICATION_DATA:
                if self.reader is None:
                    raise ValueError('application data before the handshake')
                self.reader.feed_data(payload)

    def _handshake_messages(self):
        while len(self.hs_buf) >= 4:
            htype = self.hs_buf[0]
            n = int.from_bytes(self.hs_buf[1:4], 'big')
            if len(self.hs_buf) < 4 + n:
                return
            msg, body = self.hs_buf[:4 + n], self.hs_buf[4:4 + n]
            self.hs_buf = self.hs_buf[4 + n:]
            s = self.session
            if htype == HS_CLIENT_HELLO:
                s.handshake += msg
                self.transport.write(s.on_client_hello(body))
            elif htype == HS_CLIENT_KEY_EXCHANGE:
                s.handshake += msg
                s.on_client_key_exchange(body)
            elif htype == HS_FINISHED:
                self.transport.write(s.on_finished(body))
                log.debug('ssl: handshake done')
                self.reader = asyncio.StreamReader()
                asyncio.ensure_future(self.client_cb(self.reader, _Writer(self)))
            else:
                raise ValueError('unexpected handshake message %d' % htype)
