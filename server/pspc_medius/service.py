# SPDX-License-Identifier: GPL-2.0-or-later
"""The common part of the Medius servers: SCE-RT connection setup and Medius app message dispatch.

A subclass lists its handlers by message name (messages.py):

    class Auth(MediusConnection):
        def on_SessionBegin(self, m): ...

Requests nobody handles are logged with their bytes, so a client's needs show up in the log.
"""

import logging
import struct

from . import messages as M
from . import rt

log = logging.getLogger('medius')


class MediusConnection(rt.Connection):
    service = 'medius'

    def __init__(self, server):
        super().__init__(server)
        self.app_id = 0
        self.connected = False

    # ---- RT connection setup (same for MUIS, MAS and MLS)

    def on_message(self, m):
        if m.id == rt.CLIENT_CONNECT_TCP:
            world, self.app_id, session, token = rt.parse_connect_tcp(m.payload, self.version)
            if not self.accept_connect(world, session, token):
                self.send(rt.SERVER_CONNECT_REJECT, b'')
                self.close()
                return
            if self.version > 108:
                self.connect_require()
            else:
                self.send_game_key()
                self.connect_accept(player_id=self.player_id())
                self.connect_complete()
        elif m.id == rt.CLIENT_CONNECT_READY_REQUIRE:
            self.send_game_key()
            self.connect_accept(player_id=self.player_id(), scert_id=self.server.next_scert_id())
        elif m.id == rt.CLIENT_CONNECT_READY_TCP:
            self.connect_complete()
            self.connected = True
            if self.version > 108:
                self.send(rt.SERVER_ECHO, struct.pack('<II', self.server.unix_time(), 0))
            self.on_connected()
        elif m.id == rt.CLIENT_APP_TOSERVER:
            self.on_app(m.payload)
        elif m.id in (rt.CLIENT_SET_RECV_FLAG, rt.CLIENT_SET_AGG_TIME, rt.CLIENT_FLUSH_ALL, rt.CLIENT_TIMEBASE_QUERY):
            if m.id == rt.CLIENT_TIMEBASE_QUERY:
                client_time, = struct.unpack_from('<I', m.payload)
                self.send(rt.SERVER_TIMEBASE_QUERY_NOTIFY, struct.pack('<II', client_time, self.server.ms_time()))
        else:
            log.info('%s %s: unhandled RT %r %s', self.service, self.who(), m, m.payload[:64].hex())

    def send_game_key(self):
        # PS3/PSP (112+) clients get no CRYPTKEY_GAME from MAS/MLS (horizon-server); MUIS sends it.
        if self.keys.session is not None and (self.version < 112 or self.service == 'muis'):
            self.send(rt.SERVER_CRYPTKEY_GAME, self.keys.session.key)

    def accept_connect(self, world, session, token):
        return True

    def player_id(self):
        return 0

    def on_connected(self):
        pass

    # ---- Medius app messages

    def on_app(self, data):
        if len(data) < 2:
            return
        cls, typ, body = data[0], data[1], data[2:]
        name, fields = M.decode(cls, typ, body)
        handler = getattr(self, 'on_' + name, None) if name else getattr(self, 'on_raw_%d_%02X' % (cls, typ), None)
        if handler is None and name:
            handler = getattr(self, 'on_raw_%d_%02X' % (cls, typ), None)
            if handler is not None:
                fields = body
        if handler is None:
            log.warning('%s %s: unhandled %s (%d bytes): %s', self.service, self.who(), M.name_of(cls, typ), len(body),
                        body[:200].hex())
            return
        log.info('%s %s: %s %s', self.service, self.who(), M.name_of(cls, typ),
                 _brief(fields) if isinstance(fields, dict) else '(%d bytes)' % len(body))
        handler(fields if fields is not None else body)

    def reply(self, name, fields=None, **kw):
        data = M.encode(name, fields, **kw)
        log.info('%s %s: -> %s %s', self.service, self.who(), name, _brief(dict(fields or {}, **kw)))
        self.send_app(data)

    def who(self):
        return '%s:%d' % (self.peer[0], self.peer[1])


def _brief(fields):
    parts = []
    for k, v in fields.items():
        if isinstance(v, bytes):
            v = v.hex() if len(v) <= 8 else '<%d bytes>' % len(v)
        elif isinstance(v, str) and len(v) > 48:
            v = v[:45] + '...'
        elif isinstance(v, (dict, list)):
            v = '{...}'
        parts.append('%s=%s' % (k, v))
    return ' '.join(parts)
