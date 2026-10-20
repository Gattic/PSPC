# SPDX-License-Identifier: GPL-2.0-or-later
"""WebSocket (RFC 6455) as much as the tunnel needs, over asyncio streams: binary messages, ping and
pong, close. Both ends are here: accept() is the server's side of the handshake (the tunnel gateway,
behind Cloudflare), connect() the client's (the relay on a player's PC).

No extensions, no sub-protocols, no text messages. A peer that breaks the rules (an unmasked frame
from a client, a reserved bit, a message over the limit) gets a close and is dropped."""

import asyncio
import base64
import hashlib
import os
import ssl
import struct
import time
import urllib.parse

GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
OP_CONTINUATION, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0, 1, 2, 8, 9, 10
MAX_MESSAGE = 1 << 16
MAX_HANDSHAKE = 16384

CLOSE_NORMAL, CLOSE_PROTOCOL, CLOSE_UNSUPPORTED, CLOSE_TOO_BIG, CLOSE_ERROR = 1000, 1002, 1003, 1009, 1011


class WebSocketError(Exception):
    pass


def accept_key(key):
    # RFC 6455 says SHA-1 here: it proves the server read the handshake, it protects nothing.
    return base64.b64encode(hashlib.sha1(key.encode('ascii') + GUID).digest()).decode('ascii')


class WebSocket:
    """One end of an open connection. recv() gives the next message (bytes), or None once the other end has closed."""

    def __init__(self, reader, writer, is_client, max_message=MAX_MESSAGE):
        self.reader = reader
        self.writer = writer
        self.is_client = is_client          # a client's frames are masked, a server's are not
        self.max_message = max_message
        self.closed = False
        self.last_heard = time.monotonic()      # when the last frame arrived
        self._write_lock = asyncio.Lock()

    # ---- frames

    def _frame(self, opcode, payload):
        head = bytes([0x80 | opcode])
        mask_bit = 0x80 if self.is_client else 0
        n = len(payload)
        if n < 126:
            head += bytes([mask_bit | n])
        elif n < 1 << 16:
            head += bytes([mask_bit | 126]) + struct.pack('>H', n)
        else:
            head += bytes([mask_bit | 127]) + struct.pack('>Q', n)
        if self.is_client:
            key = os.urandom(4)
            masked = (int.from_bytes(payload, 'big') ^ int.from_bytes((key * (n // 4 + 1))[:n], 'big')).to_bytes(n, 'big') if n else b''
            return head + key + masked
        return head + payload

    async def _send_frame(self, opcode, payload=b''):
        async with self._write_lock:
            if self.writer.is_closing():
                raise ConnectionError('closed')
            self.writer.write(self._frame(opcode, payload))
            await self.writer.drain()

    async def _read_frame(self):
        """(fin, opcode, payload) of the next frame."""
        b0, b1 = await self.reader.readexactly(2)
        self.last_heard = time.monotonic()
        if b0 & 0x70:
            raise WebSocketError('reserved bits set')
        fin, opcode, masked, n = bool(b0 & 0x80), b0 & 0x0F, bool(b1 & 0x80), b1 & 0x7F
        if masked == self.is_client:
            raise WebSocketError('frame masked wrongly')
        if n == 126:
            n = struct.unpack('>H', await self.reader.readexactly(2))[0]
        elif n == 127:
            n = struct.unpack('>Q', await self.reader.readexactly(8))[0]
        if opcode >= 8 and (n > 125 or not fin):
            raise WebSocketError('bad control frame')
        if n > self.max_message:
            raise WebSocketError('frame too big')
        key = await self.reader.readexactly(4) if masked else b''
        payload = await self.reader.readexactly(n) if n else b''
        if masked and n:
            payload = (int.from_bytes(payload, 'big') ^ int.from_bytes((key * (n // 4 + 1))[:n], 'big')).to_bytes(n, 'big')
        return fin, opcode, payload

    # ---- messages

    async def recv(self):
        message = bytearray()
        started = False
        try:
            while True:
                fin, opcode, payload = await self._read_frame()
                if opcode == OP_PING:
                    await self._send_frame(OP_PONG, payload)
                elif opcode == OP_PONG:
                    pass
                elif opcode == OP_CLOSE:
                    await self.close(CLOSE_NORMAL)
                    return None
                elif opcode == OP_BINARY and not started or opcode == OP_CONTINUATION and started:
                    started = True
                    message += payload
                    if len(message) > self.max_message:
                        raise WebSocketError('message too big')
                    if fin:
                        return bytes(message)
                else:
                    raise WebSocketError('unexpected opcode %d' % opcode)
        except asyncio.IncompleteReadError:
            self.closed = True
            return None
        except WebSocketError:
            await self.close(CLOSE_PROTOCOL)
            return None
        except (ConnectionError, OSError):
            self.closed = True
            return None

    async def send(self, data):
        """Sends one binary message; ConnectionError if the connection is gone."""
        await self._send_frame(OP_BINARY, data)

    async def ping(self):
        await self._send_frame(OP_PING)

    async def close(self, code=CLOSE_NORMAL):
        if not self.closed:
            self.closed = True
            try:
                await asyncio.wait_for(self._send_frame(OP_CLOSE, struct.pack('>H', code)), 2)
            except (ConnectionError, OSError, asyncio.TimeoutError):
                pass
        if not self.writer.is_closing():
            self.writer.close()


# ---- the server's handshake

def wants_upgrade(headers):
    """Whether a request's (lower-cased) headers ask for a WebSocket."""
    return headers.get('upgrade', '').lower() == 'websocket' and \
        'upgrade' in [t.strip() for t in headers.get('connection', '').lower().split(',')]


async def accept(headers, reader, writer, max_message=MAX_MESSAGE):
    """Completes the handshake for a request that wants_upgrade(); WebSocketError if it is malformed."""
    key = headers.get('sec-websocket-key', '')
    try:
        if len(base64.b64decode(key, validate=True)) != 16:
            raise ValueError
    except ValueError:
        raise WebSocketError('bad Sec-WebSocket-Key')
    if headers.get('sec-websocket-version') != '13':
        raise WebSocketError('unsupported WebSocket version')
    writer.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                  'Sec-WebSocket-Accept: %s\r\n\r\n' % accept_key(key)).encode('ascii'))
    await writer.drain()
    return WebSocket(reader, writer, False, max_message)


# ---- the client's handshake

def split_url(url):
    """(secure, host, port, path) of an http://, https://, ws:// or wss:// URL."""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ('http', 'https', 'ws', 'wss') or not parts.hostname:
        raise ValueError('not a web address: %s' % url)
    secure = parts.scheme in ('https', 'wss')
    path = (parts.path or '/') + ('?' + parts.query if parts.query else '')
    return secure, parts.hostname, parts.port or (443 if secure else 80), path


async def connect(url, headers=(), ssl_context=None, timeout=15, max_message=MAX_MESSAGE):
    """Opens a WebSocket to a URL (TLS for https:// and wss://, checked against the system's certificates)."""
    secure, host, port, path = split_url(url)
    if secure and ssl_context is None:
        ssl_context = ssl.create_default_context()
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(host, port, ssl=ssl_context if secure else None, server_hostname=host if secure else None,
                                limit=MAX_HANDSHAKE), timeout)
    try:
        key = base64.b64encode(os.urandom(16)).decode('ascii')
        default_port = 443 if secure else 80
        lines = ['GET %s HTTP/1.1' % path, 'Host: %s' % (host if port == default_port else '%s:%d' % (host, port)),
                 'Upgrade: websocket', 'Connection: Upgrade', 'Sec-WebSocket-Key: ' + key, 'Sec-WebSocket-Version: 13']
        lines += ['%s: %s' % h for h in headers]
        writer.write(('\r\n'.join(lines) + '\r\n\r\n').encode('latin-1'))
        await writer.drain()
        try:
            head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), timeout)
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError) as e:
            raise WebSocketError('the server did not answer the handshake properly (%s)' % type(e).__name__)
        status_line, *header_lines = head.decode('latin-1').split('\r\n')
        status = status_line.split(' ', 2)
        if len(status) < 2 or status[1] != '101':
            raise WebSocketError('the server answered "%s"' % status_line)
        response = {}
        for line in header_lines:
            name, _, value = line.partition(':')
            response[name.strip().lower()] = value.strip()
        if response.get('sec-websocket-accept') != accept_key(key):
            raise WebSocketError('the server did not accept the WebSocket handshake')
        return WebSocket(reader, writer, True, max_message)
    except BaseException:
        writer.close()
        raise
