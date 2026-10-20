# SPDX-License-Identifier: GPL-2.0-or-later
"""The server browser (<gamename>.ms<N>.gamespy.com, TCP 28910): lists the hosted games the master
server knows (qr.py), with the keys the game asks for, filtered; sends a host's full rules; pushes
changes to the list while the connection is open; and forwards players' messages to hosts.

Requests are [length, 2 bytes big-endian, including itself][type][...]:
- 0, list: [protocol version][encoding version][game version 4][for game name][from game name]
  [challenge 8][filter][key names "\\a\\b"][options 4, big-endian][source IP 4, if option 8]
  [max results 4, if option 128]. Options: 1 send keys for all servers, 2 no server list, 4 push
  updates, 8 alternate source IP, 32 groups, 64 no list cache, 128 limit.
- 1, a server's info: [IP 4][port 2]. 2, a message to a host: [IP 4][port 2][data]. 3, keepalive.

The reply stream starts with a header the client derives the cipher key from: [10 ^ 0xEC][10 bytes,
the first two the backend flags][25 ^ 0xEA][25 bytes, the server's challenge]. Everything after it is
encrypted (crypt.GOACipher, keyed by crypt.sb_crypt_key). The list is [the client's IP 4][default
query port 2][key count][each: type (0 string) and name][popular value count, 0][the servers][0 and
FF FF FF FF]. A server is [flags][IP 4][port 2, if not the default][private IP 4][private port 2]
[keys: FF and the value, a string]. Later messages are length-prefixed: 2 a server (full rules for
an info request, the keys for a push), 3 keepalive, 4 a server gone [IP 4][port 2].
"""

import asyncio
import logging
import os
import socket
import struct

from . import access
from . import crypt
from . import games
from .qr import MAX_MESSAGE
from .sqlfilter import Filter

log = logging.getLogger('gamespy')

LIST_REQUEST, INFO_REQUEST, SEND_MESSAGE_REQUEST, KEEPALIVE_REPLY = 0, 1, 2, 3
PUSH_KEYS_MESSAGE, PUSH_SERVER_MESSAGE, KEEPALIVE_MESSAGE, DELETE_SERVER_MESSAGE = 1, 2, 3, 4

SEND_FIELDS_FOR_ALL, NO_SERVER_LIST, PUSH_UPDATES, ALTERNATE_SOURCE_IP = 1, 2, 4, 8
SEND_GROUPS, NO_LIST_CACHE, LIMIT_RESULT_COUNT = 32, 64, 128

UNSOLICITED_UDP_FLAG, PRIVATE_IP_FLAG, CONNECT_NEGOTIATE_FLAG, ICMP_IP_FLAG = 1, 2, 4, 8
NONSTANDARD_PORT_FLAG, NONSTANDARD_PRIVATE_PORT_FLAG, HAS_KEYS_FLAG, HAS_FULL_RULES_FLAG = 16, 32, 64, 128

DEFAULT_QUERY_PORT = 6500
KEEPALIVE_INTERVAL = 60
MAX_REQUEST = 4096
MAX_FIELDS = 64                 # keys a list request may ask for (a game asks for about ten)
FIRST_REQUEST_TIMEOUT = 60      # seconds a new connection has to ask for a list
IDLE_TIMEOUT = 1200             # seconds of silence (not even a keepalive's reply) before a connection is closed


def ip_bytes(ip):
    try:
        return socket.inet_aton(ip)
    except OSError:
        return b'\0\0\0\0'


def nts(text):
    return str(text).encode('latin-1', 'replace') + b'\0'


class ListRequest:
    def __init__(self, body):
        self.protocol, self.encoding = body[0], body[1]
        self.game_version = struct.unpack_from('>I', body, 2)[0]
        pos = 6
        self.for_game, pos = self._nts(body, pos)
        self.from_game, pos = self._nts(body, pos)
        self.challenge = body[pos:pos + 8]
        pos += 8
        self.filter, pos = self._nts(body, pos)
        fields, pos = self._nts(body, pos)
        self.fields = [f for f in fields.split('\\') if f][:MAX_FIELDS]
        self.options = struct.unpack_from('>I', body, pos)[0] if pos + 4 <= len(body) else 0
        pos += 4
        self.source_ip = None
        if self.options & ALTERNATE_SOURCE_IP and pos + 4 <= len(body):
            self.source_ip = body[pos:pos + 4]
            pos += 4
        self.max_results = 0
        if self.options & LIMIT_RESULT_COUNT and pos + 4 <= len(body):
            self.max_results = struct.unpack_from('>I', body, pos)[0]

    @staticmethod
    def _nts(data, pos):
        end = data.index(b'\0', pos)
        return data[pos:end].decode('latin-1'), end + 1


class Session:
    """One server browser connection."""

    def __init__(self, browser, reader, writer):
        self.browser = browser
        self.registry = browser.registry
        self.reader = reader
        self.writer = writer
        self.peer = writer.get_extra_info('peername') or ('0.0.0.0', 0)
        self.limits = browser.limits
        self.cipher = None
        self.game = None
        self.request = None
        self.filter = None
        self.watching = False
        self.closed = False

    # ---- sending

    def _start_cipher(self, client_challenge):
        """The crypt header (sent once, in the clear) and the cipher for everything after it."""
        cryptchal = b'\0\0' + os.urandom(8)          # backend flags 0: no query challenges for direct queries
        servchal = os.urandom(25)
        self.cipher = crypt.GOACipher(crypt.sb_crypt_key(client_challenge, self.game.secret_key, servchal))
        return bytes([10 ^ 0xEC]) + cryptchal + bytes([25 ^ 0xEA]) + servchal

    def send(self, data, length_prefixed=True, header=b''):
        if self.closed or self.cipher is None:
            return
        if length_prefixed:
            data = struct.pack('>H', len(data) + 2) + data
        try:
            self.writer.write(header + self.cipher.encrypt(data))
        except Exception:
            self.closed = True
        if access.drop_if_backlogged(self.writer):
            self.closed = True

    def send_error(self, text):
        """Errors go out in the clear, before any encrypted data, and close the connection."""
        log.info('sb %s:%d: %s', self.peer[0], self.peer[1], text)
        try:
            self.writer.write(text.encode('latin-1', 'replace'))
        except Exception:
            pass
        self.closed = True

    # ---- the servers

    def server_ip(self, host):
        return self.browser.reach.ip_for(host.ip, self.peer[0])

    def encode_server(self, host, fields, push=False, full_rules=False):
        keys = host.keys
        flags = 0
        out = bytearray()
        port = host.port
        if port != DEFAULT_QUERY_PORT:
            flags |= NONSTANDARD_PORT_FLAG
        if keys.get('natneg') == '1':
            flags |= CONNECT_NEGOTIATE_FLAG
        private_ip = keys.get('localip0')
        if private_ip:
            flags |= PRIVATE_IP_FLAG
        private_port = 0
        try:
            private_port = int(keys.get('localport', '') or 0)
        except ValueError:
            pass
        if private_port and private_port != DEFAULT_QUERY_PORT:
            flags |= NONSTANDARD_PRIVATE_PORT_FLAG
        flags |= HAS_FULL_RULES_FLAG if full_rules else HAS_KEYS_FLAG
        out.append(flags)
        out += ip_bytes(self.server_ip(host))
        if flags & NONSTANDARD_PORT_FLAG:
            out += struct.pack('>H', port)
        if flags & PRIVATE_IP_FLAG:
            out += ip_bytes(private_ip)
        if flags & NONSTANDARD_PRIVATE_PORT_FLAG:
            out += struct.pack('>H', private_port)
        if full_rules:
            for k, v in host.rules():
                out += nts(k) + nts(v)
            out.append(0)
        else:
            for f in fields:
                if not push:
                    out.append(0xFF)            # not one of the popular values: the string follows
                out += nts(self.value(host, f))
        return bytes(out)

    @staticmethod
    def value(host, field):
        if field in host.keys:
            return host.keys[field]
        if field == 'numplayers':
            return str(len(host.players))
        return ''

    def matching(self):
        hosts = [h for h in self.registry.listed(self.request.for_game) if self.filter.matches(h.keys)]
        if self.request.max_results:
            hosts = hosts[:self.request.max_results]
        return hosts

    def send_list(self, header):
        req = self.request
        source = req.source_ip or ip_bytes(self.peer[0])
        out = bytearray(source + struct.pack('>H', DEFAULT_QUERY_PORT))
        hosts = []
        if not req.options & NO_SERVER_LIST:
            hosts = self.matching()
            out.append(len(req.fields) & 0xFF)
            for f in req.fields:
                out += b'\0' + nts(f)                # key type 0: a string
            out.append(0)                           # no popular values
            for h in hosts:
                out += self.encode_server(h, req.fields)
            out += b'\0\xff\xff\xff\xff'
        self.send(bytes(out), length_prefixed=False, header=header)
        log.info('sb %s:%d: %s list, %d of %d hosted games%s', self.peer[0], self.peer[1], access.safe(req.for_game), len(hosts),
                 len(self.registry.listed(req.for_game)), ' (filter: %s)' % access.safe(req.filter) if req.filter else '')
        if not self.filter.ok:
            log.info("sb: couldn't read the filter %s; listing everything", access.safe(req.filter))

    # ---- push updates (Registry watcher)

    def host_added(self, host):
        if self.request and self.filter.matches(host.keys) and self._for_us(host):
            self.send(bytes([PUSH_SERVER_MESSAGE]) + self.encode_server(host, self.request.fields, push=True))

    host_updated = host_added

    def host_removed(self, host):
        if self.request and self._for_us(host):
            self.send(bytes([DELETE_SERVER_MESSAGE]) + ip_bytes(self.server_ip(host)) + struct.pack('>H', host.port))

    def _for_us(self, host):
        a, b = games.find(host.gamename), games.find(self.request.for_game)
        return a is not None and a is b

    # ---- requests

    def handle(self, body):
        rtype, body = body[0], body[1:]
        if rtype in (LIST_REQUEST, INFO_REQUEST) and not self.limits.allow(self.limits.browser_requests, self.peer[0]):
            log.info('sb %s:%d: too many requests; closed', self.peer[0], self.peer[1])
            self.closed = True
            return
        if rtype == LIST_REQUEST:
            req = ListRequest(body)
            game = games.find(req.from_game)
            for_game = games.find(req.for_game)
            if game is None or for_game is None:
                self.send_error('\\error\\Unknown game name: %s\\final\\' %
                                access.safe(req.from_game if game is None else req.for_game, 30))
                return
            log.debug('sb %s:%d: list request for %s from %s, fields %s, filter %r, options %#x', self.peer[0], self.peer[1],
                      req.for_game, req.from_game, req.fields, req.filter, req.options)
            header = b''
            if self.cipher is None:
                self.game = game
                header = self._start_cipher(req.challenge)
            self.request = req
            self.filter = Filter(req.filter)
            self.send_list(header)
            if req.options & PUSH_UPDATES and not self.watching:
                self.registry.watchers.add(self)
                self.watching = True
        elif rtype == INFO_REQUEST:
            if len(body) < 6 or self.cipher is None:
                return
            ip, port = socket.inet_ntoa(body[:4]), struct.unpack_from('>H', body, 4)[0]
            host = self.registry.find(ip, port)
            log.debug('sb %s:%d: info request for %s:%d%s', self.peer[0], self.peer[1], ip, port, '' if host else ' (not hosted)')
            if host:
                self.send(bytes([PUSH_SERVER_MESSAGE]) + self.encode_server(host, [], full_rules=True))
        elif rtype == SEND_MESSAGE_REQUEST:
            # Players have the server send hosts the messages that start NAT negotiation: small, and only
            # from a connection that has asked for a list. Otherwise this would send what a stranger likes
            # (several times over) to any address a game was listed at.
            if len(body) < 6 or len(body) - 6 > MAX_MESSAGE or self.cipher is None:
                return
            if not self.limits.allow(self.limits.host_messages, self.peer[0]):
                log.info('sb %s:%d: too many messages to hosts', self.peer[0], self.peer[1])
                return
            ip, port = socket.inet_ntoa(body[:4]), struct.unpack_from('>H', body, 4)[0]
            host = self.registry.find(ip, port)
            if host is None:
                log.info('sb %s:%d: message for %s:%d, which is not hosting', self.peer[0], self.peer[1], ip, port)
                return
            log.debug('sb %s:%d: message for %s (%d bytes)', self.peer[0], self.peer[1], access.safe(host.name()), len(body) - 6)
            self.browser.master.send_client_message(host, body[6:])
        elif rtype == KEEPALIVE_REPLY:
            pass
        else:
            log.debug('sb %s:%d: request type %d: %s', self.peer[0], self.peer[1], rtype, body[:64].hex())

    async def run(self):
        buf = b''
        ka = asyncio.get_running_loop().call_later(KEEPALIVE_INTERVAL, self._keepalive)
        self._ka = ka
        try:
            while not self.closed:
                timeout = IDLE_TIMEOUT if self.cipher is not None else FIRST_REQUEST_TIMEOUT
                try:
                    data = await asyncio.wait_for(self.reader.read(4096), access.patience(self.peer[0], timeout))
                except asyncio.TimeoutError:
                    break
                if not data:
                    break
                buf += data
                while len(buf) >= 2:
                    n = struct.unpack_from('>H', buf, 0)[0]
                    if n < 3 or n > MAX_REQUEST:
                        log.info('sb %s:%d: bad request length %d', self.peer[0], self.peer[1], n)
                        self.closed = True
                        break
                    if len(buf) < n:
                        break
                    body, buf = buf[2:n], buf[n:]
                    try:
                        self.handle(body)
                    except (ValueError, IndexError, struct.error) as e:
                        log.info('sb %s:%d: bad request (%s): %s', self.peer[0], self.peer[1], e, body[:64].hex())
                    if self.closed:
                        break
                await self.writer.drain()
        except (ConnectionError, OSError):
            pass
        except Exception:
            log.warning('sb %s:%d: a request was not handled; closed', self.peer[0], self.peer[1],
                        exc_info=log.isEnabledFor(logging.DEBUG))
        finally:
            self.closed = True
            self._ka.cancel()
            self.registry.watchers.discard(self)
            try:
                self.writer.close()
            except Exception:
                pass

    def _keepalive(self):
        if self.closed:
            return
        if self.watching:
            self.send(bytes([KEEPALIVE_MESSAGE, KEEPALIVE_REPLY]))
        self._ka = asyncio.get_running_loop().call_later(KEEPALIVE_INTERVAL, self._keepalive)


class ServerBrowser:
    def __init__(self, registry, master, reach):
        self.registry = registry
        self.master = master
        self.reach = reach
        self.limits = registry.limits

    async def handle(self, reader, writer):
        await Session(self, reader, writer).run()
