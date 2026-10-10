# SPDX-License-Identifier: GPL-2.0-or-later
"""SVO: the game's web portal. SVML pages over HTTP (the universe's SVO URL, TCP 10070), and the
HTTP services the game calls (ticket login, mission sharing, file upload/download, cell stats).

Every response carries X-SVOMac: the MD5 of the client's X-SVOMac value and a fixed salt, which
the client checks (as PlayStation Home's SVO servers do)."""

import asyncio
import hashlib
import html
import logging

from .http import Response
from .missions import MissionServer
from .tickets import find_ticket

log = logging.getLogger('medius.svo')

SVOMAC_SALT = 'sp9ck0348sld00000000000000000000'


def svomac(client_value):
    if not client_value or len(client_value) != 32:
        return None
    return hashlib.md5((client_value + SVOMAC_SALT).encode('ascii')).hexdigest()


def esc(text):
    return html.escape(str(text), quote=True)


class Svo:
    def __init__(self, server):
        self.server = server
        self.routes = {
            '': self.root,
            'home.svml': self.home,
            'account/TicketLogin': self.ticket_login,
        }
        for name, path in SERVICE_URIS:
            if path.startswith('game/'):
                self.routes[path] = lambda req, kind=path[5:]: self.game_report(req, kind)
        self.missions = MissionServer(server)
        self.routes.update(self.missions.routes())

    @property
    def base(self):
        return self.server.svo_url()

    def uri(self, path):
        return self.base + path

    def secure_uri(self, path):
        c = self.server.config
        return 'https://%s:%d%s%s' % (c.public_host, c.https_port, self.server.game.svo_path, path)

    async def handle(self, req):
        prefix = self.server.game.svo_path
        path = None
        for p in (prefix, prefix.replace('_SVML/', '_XML/')):   # the mission server swaps these
            if req.path.startswith(p):
                path = req.path[len(p):]
        handler = self.routes.get(path) if path is not None else None
        log.info('svo %s: %s %s%s', req.peer[0], req.method, req.target,
                 ' (%d bytes)' % len(req.body) if req.body else '')
        if handler is None:
            log.warning('svo %s: no page %s; headers %s', req.peer[0], req.target, req.headers)
            resp = Response(404, svml('<TEXT name="t" x="20" y="20" width="440" height="30">Page not found</TEXT>'),
                            'text/svml')
        else:
            resp = handler(req)
            if asyncio.iscoroutine(resp):
                resp = await resp
        mac = svomac(req.headers.get('x-svomac'))
        if mac:
            resp.headers['X-SVOMac'] = mac
        return resp

    # ---- pages

    def root(self, req):
        """The SVO URL itself: browser setup, the service URIs the game reads, then the home page."""
        data = [('DATA', 'TicketLoginURI', self.secure_uri('account/TicketLogin')),
                ('URI', 'homeURI', self.uri('home.svml'))]
        data += [('URI', name, self.uri(path)) for name, path in SERVICE_URIS]
        body = ['<BROWSER_INIT name="init" />']
        for kind, name, value in data:
            body.append('<DATA dataType="%s" name="%s" value="%s" />' % (kind, name, esc(value)))
        body.append('<LOBBY_PLUGIN type="NOTIFY" message="TRIGGER_LOGIN" />')
        body.append('<REDIRECT name="redirect" href="%s" />' % esc(self.uri('home.svml')))
        return Response(200, svml('\n'.join(body)), 'text/svml')

    def home(self, req):
        return Response(200, svml('<TEXT name="welcome" x="20" y="20" width="440" height="30">%s</TEXT>'
                                  % esc(self.server.game.news)), 'text/svml')

    def game_report(self, req, kind):
        """The end-of-game services (finishGameURI, gamePostBinaryStatsURI, createGame...): what the game
        sends is kept in the database (game_reports) and acknowledged."""
        account = self.missions.account_of(req)
        self.server.store.add_game_report(account, kind, req.target.partition('?')[2], req.body)
        log.info('svo %s: game report %s from %d: query %s, %d-byte body %s', req.peer[0], kind, account, req.query,
                 len(req.body), req.body[:64].hex())
        if kind == 'Finish':
            return Response(200, svml('<REDIRECT name="redirect" href="%s" />' % esc(self.uri('home.svml'))), 'text/svml')
        return Response(200, '<?xml version="1.0" encoding="UTF-8"?>\n<XML><Status>OK</Status></XML>\n', 'text/xml')

    async def ticket_login(self, req):
        at, blob = find_ticket(req.body)
        log.info('svo %s: TicketLogin query %s, %d-byte body, ticket at %d; head %s', req.peer[0], req.query,
                 len(req.body), at, req.body[:at if at > 0 else 96].hex())
        name, why = (None, 'no ticket') if blob is None else await self.server.tickets.verify(blob)
        if name is None:
            log.warning('svo %s: ticket login refused: %s', req.peer[0], why)
            return Response(200, sp_login(20601, 'ACCT_LOGIN_FAILED', 0), 'text/xml')
        app_id = int(req.query.get('applicationID') or self.server.game.app_ids[0])
        account = self.server.store.account(app_id, name)
        cookies = {'AcctID': account['account_id'], 'AcctName': name, 'LangID': req.query.get('languageID', '1')}
        resp = Response(200, sp_login(20600, 'ACCT_LOGIN_SUCCESS', account['account_id']), 'text/xml')
        resp.headers['Set-Cookie'] = '; '.join('%s=%s' % kv for kv in cookies.items()) + '; Path=/'
        return resp


# The HTTP services the game reads URIs for (the SVO URI store), and where they are served.
SERVICE_URIS = [
    ('MissionSubmitURI', 'mission/Submit'),
    ('MissionGetRawURI', 'mission/GetRaw'),
    ('MissionGetMetadataURI', 'mission/GetMetadata'),
    ('MissionInitMetadataURI', 'mission/InitMetadata'),
    ('MissionGetIDURI', 'mission/GetID'),
    ('MissionVoteGetURI', 'mission/VoteGet'),
    ('MissionVoteCastURI', 'mission/VoteCast'),
    ('MissionListGetURI', 'mission/ListGet'),
    ('MissionListGetPopularURI', 'mission/ListGetPopular'),
    ('MissionListAddURI', 'mission/ListAdd'),
    ('MissionListRemoveURI', 'mission/ListRemove'),
    ('MissionPostStatsURI', 'mission/PostStats'),
    ('CellGetStatsURI', 'cell/GetStats'),
    ('gamePostBinaryStatsURI', 'game/PostBinaryStats'),
    ('finishGameURI', 'game/Finish'),
    ('createGameURI', 'game/Create'),
    ('createGamePlayerURI', 'game/CreatePlayer'),
    ('createGameSubmitURI', 'game/CreateSubmit'),
]


def svml(body):
    return '<?xml version="1.0" encoding="UTF-8"?>\n<SVML>\n%s\n</SVML>\n' % body


def sp_login(status_id, message, account_id):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<XML>\n<SP_Login>\n<status>\n<id>%d</id>\n<message>%s</message>\n'
            '</status>\n<accountID>%d</accountID>\n<userContext>0</userContext>\n</SP_Login>\n</XML>\n'
            % (status_id, message, account_id))
