# SPDX-License-Identifier: GPL-2.0-or-later
"""DME, the game server a match is played through: players connect to their game's world over TCP
(and an auxiliary UDP channel) and the server relays the game's messages between them.

Connection (after the SCE-RT hello and key exchange):
  C CLIENT_CONNECT_TCP_AUX_UDP (world, app, key, session key, access key from MLS JoinGame)
  S SERVER_CONNECT_REQUIRE                      C CLIENT_CONNECT_READY_REQUIRE
  S SERVER_CONNECT_ACCEPT_TCP (player index)    C CLIENT_CONNECT_READY_TCP (receive flags)
  S SERVER_STARTUP_INFO_NOTIFY, SERVER_INFO_AUX_UDP (UDP address)
  C (UDP) CLIENT_CONNECT_AUX_UDP                S (UDP) SERVER_CONNECT_ACCEPT_AUX_UDP
  C CLIENT_CONNECT_READY_AUX_UDP                S SERVER_CONNECT_COMPLETE, the DME version
  S SERVER_CONNECT_NOTIFY to the others in the world

Game traffic: CLIENT_APP_BROADCAST / _SINGLE / _LIST (TCP or UDP) reach the other players as
CLIENT_APP_SINGLE carrying the sender's index, over the same transport, respecting each player's
receive flags. Follows horizon-server's Server.Dme (MIT, (c) 2020 Daniel Gerendasy).
"""

import asyncio
import logging
import os
import struct
import time

from . import rt
from .service import MediusConnection
from .world import random_key

log = logging.getLogger('medius.dme')

DME_VERSION = b'2.10.0009'
UDP_MAX = 584


class World:
    def __init__(self, dme, world_id, game, max_clients):
        self.dme = dme
        self.id = world_id
        self.game = game
        self.max_clients = max(2, min(max_clients, 16))
        self.clients = {}           # index -> DmeConnection
        self.pending = {}           # access key -> (player, index)
        self.created = time.time()
        self.host_index = None
        # Players encrypt game messages for each other with this key (SERVER_CRYPTKEY_GAME); the server
        # relays them without reading them.
        self.game_key = os.urandom(64)

    def free_index(self):
        taken = set(self.clients) | {i for _, i in self.pending.values()}
        for i in range(self.max_clients):
            if i not in taken:
                return i
        return None

    def reserve(self, player):
        index = self.free_index()
        if index is None:
            return None, None
        token = random_key()
        self.pending[token] = (player, index)
        return token, index

    def others(self, conn, flag):
        for c in list(self.clients.values()):
            if c is not conn and c.ready and c.recv_flags & flag:
                yield c


APP_IDS = (rt.CLIENT_APP_BROADCAST, rt.CLIENT_APP_SINGLE, rt.CLIENT_APP_LIST, rt.CLIENT_APP_BROADCAST_QOS,
           rt.CLIENT_APP_SINGLE_QOS, rt.CLIENT_APP_LIST_QOS, rt.CLIENT_CRYPTKEY_PEER)


class DmeConnection(MediusConnection):
    service = 'dme'
    opaque_ids = APP_IDS

    def __init__(self, server):
        super().__init__(server)
        self.world = None
        self.index = None
        self.player = None
        self.recv_flags = rt.RECV_BROADCAST | rt.RECV_LIST | rt.RECV_SINGLE | rt.RECV_NOTIFICATION
        self.udp_addr = None
        self.ready = False
        self.scert_id = 0
        self.stats = {'tcp': 0, 'udp': 0, 'tcp_bytes': 0, 'udp_bytes': 0, 'dropped': 0}

    @property
    def dme(self):
        return self.server.dme

    def send_game_key(self):
        if self.world is not None:
            self.send(rt.SERVER_CRYPTKEY_GAME, self.world.game_key)

    def send_frame(self, frame):
        if not self.closed and self.transport is not None:
            self.transport.write(frame)

    def who(self):
        name = self.player.name if self.player else '?'
        return '%s:%d %s/%s' % (self.peer[0], self.peer[1], self.world.id if self.world else '-', name)

    # ---- connection setup

    def on_message(self, m):
        if m.id == rt.CLIENT_CONNECT_TCP_AUX_UDP or m.id == rt.CLIENT_CONNECT_TCP:
            world_id, self.app_id, session, token = rt.parse_connect_tcp(m.payload, self.version)
            world = self.dme.worlds.get(world_id)
            pending = world.pending.pop(token, None) if world else None
            if pending is None:
                log.warning('dme %s: no reservation for world %d, access key %r', self.who(), world_id, token)
                self.send(rt.SERVER_CONNECT_REJECT, b'')
                self.close()
                return
            self.world, (self.player, self.index) = world, pending
            world.clients[self.index] = self
            if world.host_index is None:
                world.host_index = self.index
            self.scert_id = self.server.next_scert_id()
            log.info('dme %s: joined as player %d', self.who(), self.index)
            if self.version > 108:
                self.connect_require(512, UDP_MAX)
            else:
                self.send_game_key()
                self._accept()
                self._send_udp_info()
        elif m.id == rt.CLIENT_CONNECT_READY_REQUIRE:
            self.send_game_key()
            self._accept()
        elif m.id == rt.CLIENT_CONNECT_READY_TCP:
            if len(m.payload) >= 2:
                self.recv_flags = m.payload[1]
            self.send(rt.SERVER_STARTUP_INFO_NOTIFY, struct.pack('<BI', 4, self.world_ms()))
            self._send_udp_info()
        elif m.id == rt.CLIENT_CONNECT_READY_AUX_UDP:
            self._complete()
        elif m.id == rt.CLIENT_SET_RECV_FLAG:
            self.recv_flags = m.payload[0] if m.payload else self.recv_flags
        elif m.id in APP_IDS:
            self.relay(m, udp=False)
        elif m.id == rt.CLIENT_APP_TOSERVER:
            self.on_app(m.payload)
        elif m.id == rt.CLIENT_TIMEBASE_QUERY:
            client_time, = struct.unpack_from('<I', m.payload)
            self.send(rt.SERVER_TIMEBASE_QUERY_NOTIFY, struct.pack('<II', client_time, self.world_ms()))
        elif m.id in (rt.CLIENT_SET_AGG_TIME, rt.CLIENT_FLUSH_ALL, rt.CLIENT_FLUSH_SINGLE):
            pass
        else:
            log.info('dme %s: unhandled RT %r %s', self.who(), m, m.payload[:64].hex())

    def world_ms(self):
        return int((time.time() - self.world.created) * 1000) & 0xFFFFFFFF if self.world else 0

    def _accept(self):
        count = len(self.world.clients)
        ip = self.peer[0].encode()[:15]
        if self.version > 108:
            payload = struct.pack('<HIH16s', self.index, self.scert_id, count, ip)
        else:
            payload = b'\x01\x08\x10' + struct.pack('<HH16s', self.index, count, ip)
        self.send(rt.SERVER_CONNECT_ACCEPT_TCP, payload)

    def _send_udp_info(self):
        c = self.server.config
        self.send(rt.SERVER_INFO_AUX_UDP, struct.pack('<16sH', c.public_host.encode()[:15], c.dme_udp_port))

    def _complete(self):
        if self.ready:
            return
        self.ready = True
        self.send(rt.SERVER_CONNECT_COMPLETE, struct.pack('<H', len(self.world.clients)))
        if self.version > 108:
            self.send(rt.SERVER_APP, bytes((0, 0)) + DME_VERSION.ljust(16, b'\0'))
        # tell everyone already here about the newcomer, and the newcomer about them
        for other in list(self.world.clients.values()):
            if other is self or not other.ready:
                continue
            if other.recv_flags & rt.RECV_NOTIFICATION:
                other.send(rt.SERVER_CONNECT_NOTIFY, self.notify_payload(with_key=True))
            if self.recv_flags & rt.RECV_NOTIFICATION:
                self.send(rt.SERVER_CONNECT_NOTIFY, other.notify_payload(with_key=True))
        self.dme.player_joined(self.world, self)

    def notify_payload(self, with_key):
        ip = self.peer[0].encode()[:15]
        if self.version > 108:
            p = struct.pack('<hhh16s', self.index, self.scert_id & 0x7FFF, 0, ip)
        else:
            p = struct.pack('<h16s', self.index, ip)
        if with_key:
            p += self.keys.client_key.modulus_le() if self.keys.client_key else bytes(64)
        return p

    # ---- game traffic

    def relay(self, m, udp):
        world = self.world
        if world is None or not self.ready:
            self.stats['dropped'] += 1
            return
        kind = 'udp' if udp else 'tcp'
        self.stats[kind] += 1
        self.stats[kind + '_bytes'] += len(m.raw if m.raw is not None else m.payload)
        if m.raw is not None:
            return self.relay_encrypted(m, udp)
        payload = m.payload
        base = m.id if m.id < 0x32 else m.id - 0x30       # the _QOS variants
        if base == rt.CLIENT_APP_BROADCAST:
            targets = world.others(self, rt.RECV_BROADCAST)
        elif base in (rt.CLIENT_APP_SINGLE, rt.CLIENT_CRYPTKEY_PEER):
            target, = struct.unpack_from('<h', payload, 0)
            payload = payload[2:]
            t = world.clients.get(target)
            targets = [t] if t is not None and t is not self and t.ready and t.recv_flags & rt.RECV_SINGLE else []
        else:  # CLIENT_APP_LIST: mask length, index bitmask
            n = payload[0]
            mask = payload[1:1 + n]
            payload = payload[1 + n:]
            wanted = {i for i in range(n * 8) if mask[i // 8] & (1 << (i % 8))}
            targets = [c for i, c in world.clients.items() if i in wanted and c is not self and c.ready and
                       c.recv_flags & rt.RECV_LIST]
        out = struct.pack('<h', self.index) + payload
        out_id = rt.CLIENT_CRYPTKEY_PEER if base == rt.CLIENT_CRYPTKEY_PEER else rt.CLIENT_APP_SINGLE
        for t in targets:
            if udp and t.udp_addr is not None:
                self.dme.send_udp(t, out_id, out)
            else:
                t.send(out_id, out)

    def relay_encrypted(self, m, udp):
        """A game message the sender's session key doesn't decrypt (encrypted with the world's game key,
        which all its players share): [hash][target info][ciphertext]. The recipients get
        CLIENT_APP_SINGLE [hash][sender index][ciphertext], still encrypted."""
        world = self.world
        h, body = m.raw[:4], m.raw[4:]
        base = m.id if m.id < 0x32 else m.id - 0x30
        if base in (rt.CLIENT_APP_SINGLE, rt.CLIENT_CRYPTKEY_PEER):
            target, = struct.unpack_from('<h', body, 0)
            cipher = body[2:]
            t = world.clients.get(target)
            targets = [t] if t is not None and t is not self and t.ready and t.recv_flags & rt.RECV_SINGLE else []
        elif base == rt.CLIENT_APP_BROADCAST:
            cipher = body
            targets = list(world.others(self, rt.RECV_BROADCAST))
        else:
            n = body[0]
            mask = body[1:1 + n]
            cipher = body[1 + n:]
            wanted = {i for i in range(n * 8) if mask[i // 8] & (1 << (i % 8))}
            targets = [c for i, c in world.clients.items() if i in wanted and c is not self and c.ready and
                       c.recv_flags & rt.RECV_LIST]
        if log.isEnabledFor(logging.DEBUG):
            log.debug('dme %s: passing on encrypted %s (%d bytes) to %s', self.who(), rt.NAMES.get(m.id), len(cipher),
                      [x.index for x in targets])
        out = h + struct.pack('<h', self.index) + cipher
        out_id = rt.CLIENT_CRYPTKEY_PEER if m.id == rt.CLIENT_CRYPTKEY_PEER else rt.CLIENT_APP_SINGLE
        frame = struct.pack('<BH', out_id | 0x80, len(out) - 4) + out
        for t in targets:
            if udp and t.udp_addr is not None:
                self.dme.send_udp_frame(t, frame)
            else:
                t.send_frame(frame)

    # ---- Medius DME messages (class 0) the client sends the server

    def on_app(self, data):
        if len(data) >= 2 and data[0] == 0:
            log.debug('dme %s: DME message 0x%02X (%d bytes)', self.who(), data[1], len(data) - 2)
            return
        super().on_app(data)

    def on_close(self):
        if self.world is not None and self.index is not None:
            if self.world.clients.get(self.index) is self:
                del self.world.clients[self.index]
            log.info('dme %s: left', self.who())
            for other in list(self.world.clients.values()):
                if other.ready and other.recv_flags & rt.RECV_NOTIFICATION:
                    other.send(rt.SERVER_DISCONNECT_NOTIFY, self.notify_payload(with_key=False))
            self.dme.player_left(self.world, self)


class DmeUdp(asyncio.DatagramProtocol):
    def __init__(self, dme):
        self.dme = dme
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        try:
            self.dme.on_datagram(data, addr)
        except Exception:
            log.exception('dme udp %s:%d', *addr)


class GameServer:
    """All DME worlds of a server."""

    def __init__(self, server):
        self.server = server
        self.worlds = {}
        self.by_udp = {}            # (ip, port) -> DmeConnection
        self.udp = None
        self._report = None

    async def report(self):
        """Logs each world's traffic every 10 seconds while there is some."""
        last = {}
        while True:
            await asyncio.sleep(10)
            for w in list(self.worlds.values()):
                for c in list(w.clients.values()):
                    s = dict(c.stats)
                    key = (w.id, c.index)
                    if s != last.get(key):
                        last[key] = s
                        log.info('dme world %d player %d (%s): relayed %d TCP (%d bytes), %d UDP (%d bytes), %d dropped; '
                                 'UDP %s', w.id, c.index, c.player.name if c.player else '?', s['tcp'], s['tcp_bytes'],
                                 s['udp'], s['udp_bytes'], s['dropped'], '%s:%d' % c.udp_addr if c.udp_addr else 'none')

    async def start(self, loop, bind, tcp_port, udp_port):
        self._report = asyncio.ensure_future(self.report())
        srv = await loop.create_server(lambda: DmeConnection(self.server), bind, tcp_port)
        self.udp, _ = await loop.create_datagram_endpoint(lambda: DmeUdp(self), local_addr=(bind, udp_port))
        log.info('dme listening on %s:%d (TCP) and %d (UDP)', bind, tcp_port, udp_port)
        return srv

    def close(self):
        for w in list(self.worlds.values()):
            self.close_world(w)
        if self._report is not None:
            self._report.cancel()
        if self.udp is not None:
            self.udp.close()

    def create_world(self, game):
        w = World(self, game.id, game, game.max_players)
        self.worlds[w.id] = w
        game.dme = w
        return w

    def close_world(self, world):
        self.worlds.pop(world.id, None)
        for c in list(world.clients.values()):
            c.close()
        for addr in [a for a, c in self.by_udp.items() if c.world is world]:
            del self.by_udp[addr]

    def player_joined(self, world, conn):
        g = world.game
        if conn.player is not None:
            g.players[conn.player.account_id] = conn.player
            conn.player.game = g
        if g.status in (0, 4, 5):               # inactive / pending: someone is here now
            g.status = 1                        # staging
        log.info('dme: %s in game %r (%d players)', conn.player, g.name, len(world.clients))

    def player_left(self, world, conn):
        g = world.game
        p = conn.player
        if p is not None and g.players.get(p.account_id) is p:
            del g.players[p.account_id]
            if p.game is g:
                p.game = None
        for addr in [a for a, c in self.by_udp.items() if c is conn]:
            del self.by_udp[addr]
        if not world.clients and not world.pending:
            log.info('dme: game %r is empty: closed', g.name)
            self.server.end_game(g)

    # ---- UDP

    def send_udp_frame(self, conn, frame):
        if self.udp is not None and conn.udp_addr is not None:
            self.udp.sendto(frame, conn.udp_addr)

    def send_udp(self, conn, msg_id, payload):
        if self.udp is None or conn.udp_addr is None:
            return
        if log.isEnabledFor(logging.DEBUG):
            log.debug('dme udp -> %d %s(%d) %s', conn.index, rt.NAMES.get(msg_id, msg_id), len(payload), payload[:48].hex())
        frame = conn.keys.encode(rt.Message(msg_id, payload)) if conn.udp_encrypt else \
            struct.pack('<BH', msg_id, len(payload)) + payload
        self.udp.sendto(frame, conn.udp_addr)

    def on_datagram(self, data, addr):
        conn = self.by_udp.get(addr)
        messages = None
        if conn is not None:
            messages = rt.parse(bytearray(data), conn.keys, APP_IDS)
        else:
            # Not known yet: find the client whose session key decrypts it (or a plaintext connect).
            for world in self.worlds.values():
                for c in world.clients.values():
                    if c.udp_addr is not None:
                        continue
                    try:
                        msgs = rt.parse(bytearray(data), c.keys)
                    except ValueError:
                        continue
                    if msgs and msgs[0].id == rt.CLIENT_CONNECT_AUX_UDP and _udp_connect_is_for(msgs[0].payload, c):
                        conn, messages = c, msgs
                        break
                if conn is not None:
                    break
        if conn is None:
            log.debug('dme udp: datagram from unknown %s:%d (%d bytes)', addr[0], addr[1], len(data))
            return
        if conn.udp_encrypt is None:
            conn.udp_encrypt = bool(data[0] & 0x80)
        if log.isEnabledFor(logging.DEBUG):
            for m in messages:
                log.debug('dme udp <- %d %r %s', conn.index, m, (m.payload or m.raw or b'')[:48].hex())
        for m in messages:
            if m.id == rt.CLIENT_CONNECT_AUX_UDP:
                conn.udp_addr = addr
                self.by_udp[addr] = conn
                c = self.server.config
                ip = c.public_host.encode()[:15]
                if conn.version > 108:
                    payload = struct.pack('<HIH16sH', conn.index, conn.scert_id, len(conn.world.clients), ip, c.dme_udp_port)
                else:
                    payload = b'\x01\x08\x10' + struct.pack('<HH16sH', conn.index, len(conn.world.clients), ip,
                                                            c.dme_udp_port)
                self.send_udp(conn, rt.SERVER_CONNECT_ACCEPT_AUX_UDP, payload)
            elif m.id in APP_IDS or m.id == rt.UDP_APP:
                if m.id == rt.UDP_APP:
                    m = rt.Message(rt.CLIENT_APP_BROADCAST, m.payload)
                conn.relay(m, udp=True)
            elif m.id == rt.CLIENT_ECHO:
                self.send_udp(conn, rt.CLIENT_ECHO, m.payload[:1])
            else:
                log.debug('dme udp %s: %r', conn.who(), m)


def _udp_connect_is_for(payload, conn):
    """CLIENT_CONNECT_AUX_UDP is [world u32][app id][server ip char[16]][server port u16][player index u16]
    [scert id u32]; a plaintext one parses with anyone's keys, so match the world and player too."""
    if len(payload) < 28:
        return True
    world, _, _, _, index = struct.unpack_from('<Ii16sHH', payload)
    return world == conn.world.id and index == conn.index


DmeConnection.udp_encrypt = None
