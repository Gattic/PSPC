# SPDX-License-Identifier: GPL-2.0-or-later
"""One game server process: the universe (MUIS), authentication (MAS), lobby (MLS) and game (DME)
servers, the NAT service and the SVO/HTTP server, sharing one universe state."""

import asyncio
import itertools
import logging
import time

from . import rt
from . import structs as S
from .auth import Authentication
from .dme import GameServer
from . import http
from . import sslv3
from .lobby import LobbyServer
from .records import Records
from .svo import Svo
from .universe import UniverseInformation
from .world import Lobby

log = logging.getLogger('medius')


class Config:
    def __init__(self, public_host='127.0.0.1', bind='0.0.0.0', muis_port=10071, mas_port=10075, mls_port=10078,
                 dme_port=10079, dme_udp_port=10079, nat_port=10070, http_port=10070, https_port=10443,
                 allow_password_login=False):
        self.public_host = public_host          # the address clients are told to connect to
        self.bind = bind
        self.muis_port = muis_port
        self.mas_port = mas_port
        self.mls_port = mls_port
        self.dme_port = dme_port
        self.dme_udp_port = dme_udp_port
        self.nat_port = nat_port
        self.http_port = http_port
        self.https_port = https_port
        self.allow_password_login = allow_password_login


class Files:
    """What the lobby's file services serve: the game's own files (games.py), then files players
    uploaded (the database)."""

    def __init__(self, game, store):
        self.game = game
        self.store = store

    def read(self, app_id, name):
        if name in self.game.files:
            data = self.game.files[name]
            return data.encode('utf-8') if isinstance(data, str) else data
        row = self.store.file(name)
        return bytes(row['data']) if row else None

    def create(self, app_id, name, owner_id, size):
        """A MediusFile record for a new (or the player's own existing) file; None if someone else's."""
        if name in self.game.files:
            return None
        row = self.store.file(name)
        if row is not None and row['owner_id'] != owner_id:
            return None
        if row is None:
            self.store.write_file(name, owner_id, b'')
        return {'Filename': name, 'FileSize': size, 'OwnerID': owner_id, 'CreationTimeStamp': int(time.time()),
                'OwnerPermissionRWX': 7, 'GroupPermissionRWX': 4, 'GlobalPermissionRWX': 4}

    def write(self, app_id, name, owner_id, data):
        row = self.store.file(name)
        if row is None or row['owner_id'] == owner_id:
            self.store.write_file(name, owner_id, data)

    def delete(self, name, owner_id):
        row = self.store.file(name)
        if row is None or row['owner_id'] != owner_id:
            return False
        self.store.delete_file(name)
        return True


class NatService(asyncio.DatagramProtocol):
    """The Medius NAT service: a 4-byte query is answered with the address it came from (IPv4, then
    the port big-endian), so a client learns its external address."""

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if len(data) == 4 and data[3] != 0xD4:
            ip = bytes(int(x) for x in addr[0].split('.'))
            self.transport.sendto(ip + addr[1].to_bytes(2, 'big'), addr)
        else:
            log.debug('nat: %d bytes from %s:%d: %s', len(data), addr[0], addr[1], data[:32].hex())


class MediusServer:
    def __init__(self, config, game, store, tickets, rsa_key=None):
        self.config = config
        self.game = game
        self.store = store
        self.tickets = tickets
        self.rsa_key = rsa_key or rt.DEFAULT_KEY
        self.lobby = Lobby(game)
        self.files = Files(game, store)
        self._scert_ids = itertools.count(1)
        self._t0 = time.monotonic()
        self._servers = []
        self._nat = None
        self.dme = GameServer(self)
        self.records = Records(self)

    # ---- helpers for the services

    def next_scert_id(self):
        return next(self._scert_ids)

    def unix_time(self):
        return int(time.time())

    def ms_time(self):
        return int((time.monotonic() - self._t0) * 1000) & 0xFFFFFFFF

    def svo_url(self):
        c = self.config
        port = '' if c.http_port == 80 else ':%d' % c.http_port
        return 'http://%s%s%s' % (c.public_host, port, self.game.svo_path)

    def address_list(self, port):
        c = self.config
        return [{'AddressType': S.ADDR_EXTERNAL, 'Address': c.public_host, 'Port': port},
                {'AddressType': S.ADDR_NAT_SERVICE, 'Address': c.public_host, 'Port': c.nat_port}]

    def lobby_connect_info(self, player, world_id, server_key=True):
        # Ticket logins get no server key (as horizon-server does; 3.x clients then take the
        # lobby server's key from its hello).
        return {'Type': S.CONN_CLIENT_SERVER_TCP, 'AddressList': self.address_list(self.config.mls_port),
                'WorldID': world_id, 'ServerKey': self.rsa_key.modulus_le() if server_key else bytes(64),
                'SessionKey': player.session_key, 'AccessKey': player.access_key}

    def end_game(self, game):
        if self.dme is not None and game.dme is not None:
            self.dme.close_world(game.dme)
        self.lobby.end_game(game)

    # ---- running

    async def start(self):
        loop = asyncio.get_running_loop()
        c = self.config
        for cls, port in ((UniverseInformation, c.muis_port), (Authentication, c.mas_port), (LobbyServer, c.mls_port)):
            srv = await loop.create_server(lambda cls=cls: cls(self), c.bind, port)
            self._servers.append(srv)
            log.info('%s listening on %s:%d', cls.service, c.bind, port)
        self._servers.append(await self.dme.start(loop, c.bind, c.dme_port, c.dme_udp_port))
        self._nat, _ = await loop.create_datagram_endpoint(NatService, local_addr=(c.bind, c.nat_port))
        log.info('nat listening on %s:%d (UDP)', c.bind, c.nat_port)
        self.svo = Svo(self)
        srv = await asyncio.start_server(http.serve(self.svo.handle), c.bind, c.http_port)
        self._servers.append(srv)
        log.info('svo (http) listening on %s:%d', c.bind, c.http_port)
        cert = rt.hello_certificate(self.rsa_key)[6:6 + 0x2E7]   # the DER certificate inside
        srv = await loop.create_server(lambda: sslv3.SslServerProtocol(self.rsa_key, [cert], http.serve(self.svo.handle)),
                                       c.bind, c.https_port)
        self._servers.append(srv)
        log.info('svo (https, SSL 3.0) listening on %s:%d', c.bind, c.https_port)

    async def serve_forever(self):
        await self.start()
        await asyncio.gather(*(s.serve_forever() for s in self._servers))

    def close(self):
        for s in self._servers:
            s.close()
        self.dme.close()
        if self._nat is not None:
            self._nat.close()
