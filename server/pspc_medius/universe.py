# SPDX-License-Identifier: GPL-2.0-or-later
"""MUIS, the Medius Universe Information Server (the game's first stop, TCP 10071): tells the client
where the authentication server (MAS) is, the universe's name and status, and the SVO (SVML portal)
URL."""

import struct

from . import messages as M
from .service import MediusConnection
from .structs import Layout, MID, I32, Str, BOOL

NEWS = Layout(('MessageID', MID), ('StatusCode', I32), ('News', Str(256)), ('EndOfList', BOOL))


def _fixed(text, size):
    b = (text or '').encode('utf-8')[:size - 1]
    return b + bytes(size - len(b))


class UniverseInformation(MediusConnection):
    service = 'muis'

    def on_GetUniverseInformation(self, m):
        info = m['InfoType']
        server = self.server
        game = server.game
        mid = M.MID.pack(m['MessageID'])
        if info & M.INFO_SVO_URL:
            url = server.svo_url().encode('ascii')
            self.send_app(bytes((M.LOBBY_EXT, 0x1E)) + mid + bytes((len(url) + 1,)) + url + b'\0')
        body = bytearray(mid)
        body += struct.pack('<iI', M.SUCCESS, info)
        if info & M.INFO_ID:
            body += struct.pack('<I', 1)
        if info & M.INFO_NAME:
            body += _fixed(game.universe_name, 128)
        if info & M.INFO_DNS:
            body += _fixed(server.config.public_host, 128) + struct.pack('<i', server.config.mas_port)
        if info & M.INFO_DESCRIPTION:
            body += _fixed(game.universe_description, 256)
        if info & M.INFO_STATUS:
            body += struct.pack('<iii', 1, server.lobby.player_count(), game.max_users)  # 1 = online
        if info & M.INFO_BILLING:
            body += bytes(8) + bytes(128)
        if info & M.INFO_EXTRAINFO:
            body += _fixed('', 128)
        body += b'\x01' + bytes(3)  # EndOfList
        self.log_reply('UniverseVariableInformationResponse', 'MAS %s:%d' % (server.config.public_host, server.config.mas_port))
        self.send_app(bytes((M.LOBBY_EXT, 0x11)) + bytes(body))
        if info & M.INFO_NEWS:
            self.send_app(bytes((M.LOBBY, 0xC9)) + NEWS.pack(MessageID=m['MessageID'], StatusCode=M.SUCCESS,
                                                             News=game.news, EndOfList=True))

    def log_reply(self, name, what):
        from .service import log
        log.info('%s %s: -> %s %s', self.service, self.who(), name, what)
