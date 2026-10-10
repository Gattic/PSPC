# SPDX-License-Identifier: GPL-2.0-or-later
"""MAS, the Medius Authentication Server: sessions, logins (an NP ticket, or a name and password),
policies; a successful login sends the client on to the lobby server (MLS)."""

import asyncio
import logging

from . import messages as M
from . import structs as S
from .service import MediusConnection
from .tickets import find_ticket

log = logging.getLogger('medius')

POLICY_CHUNK = 255


class Authentication(MediusConnection):
    service = 'mas'

    def __init__(self, server):
        super().__init__(server)
        self.session_key = None

    @property
    def lobby(self):
        return self.server.lobby

    # ---- sessions

    def _begin_session(self, m):
        self.session_key = self.lobby.begin_session()
        self.reply('SessionBeginResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS, SessionKey=self.session_key)

    on_SessionBegin = on_SessionBegin1 = on_ExtendedSessionBegin = _begin_session

    def on_SessionEnd(self, m):
        self.lobby.end_session(m['SessionKey'])
        self.reply('SessionEndResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    def _localization(self, m):
        p = self.lobby.player(m['SessionKey'])
        if p is not None:
            p.char_encoding, p.language = m['CharacterEncoding'], m['Language']
        self.reply('SetLocalizationParamsResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    on_SetLocalizationParams = on_SetLocalizationParams1 = _localization

    def on_DnasSignaturePost(self, m):
        pass  # no reply, as on the original servers

    def on_GetPolicy(self, m):
        text = self.server.game.policy(m['Policy'])
        chunks = [text[i:i + POLICY_CHUNK] for i in range(0, len(text), POLICY_CHUNK)] or ['']
        for i, chunk in enumerate(chunks):
            self.reply('PolicyResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS, Policy=chunk,
                       EndOfText=i == len(chunks) - 1)

    def on_GetMyIP(self, m):
        self.reply('GetMyIPResponse', MessageID=m['MessageID'], IP=self.peer[0], StatusCode=M.SUCCESS)

    def on_AccountGetID(self, m):
        a = self.server.store.find_account(self.app_id, m['AccountName'])
        self.reply('AccountGetIDResponse', MessageID=m['MessageID'], AccountID=a['account_id'] if a else 0,
                   StatusCode=M.SUCCESS if a else M.ACCOUNT_NOT_FOUND)

    def on_AccountLogout(self, m):
        p = self.lobby.player(m['SessionKey'])
        if p is not None:
            self.lobby.logout(p)
        self.reply('AccountLogoutResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    # ---- logins

    def on_raw_4_58(self, body):
        """TicketLogin: MessageID, SessionKey, then the NP ticket (located by its header)."""
        mid = S.MID.unpack(body[:21])
        session = S.SESSIONKEY.unpack(body[21:38])
        at, blob = find_ticket(body)
        log.info('mas %s: TicketLogin session=%s ticket at %d (%s bytes); header %s', self.who(), session, at,
                 len(blob) if blob else 'no', body[:at if at > 0 else 64].hex())
        if blob is None:
            self.login_failed('TicketLoginResponse', mid, M.LOGIN_FAILED, 'no ticket in TicketLogin')
            return
        asyncio.ensure_future(self._ticket_login(mid, session, blob))

    async def _ticket_login(self, mid, session, blob):
        name, why = await self.server.tickets.verify(blob)
        if name is None:
            self.login_failed('TicketLoginResponse', mid, M.LOGIN_FAILED, why)
            return
        self.login(mid, session, name, 'TicketLoginResponse')

    def on_AccountLogin(self, m):
        if not self.server.config.allow_password_login:
            self.login_failed('AccountLoginResponse', m['MessageID'], M.LOGIN_FAILED, 'name/password logins are off')
            return
        self.login(m['MessageID'], m['SessionKey'], m['Username'], 'AccountLoginResponse')

    def on_AnonymousLogin(self, m):
        if not self.server.config.allow_password_login:
            self.login_failed('AnonymousLoginResponse', m['MessageID'], M.LOGIN_FAILED, 'anonymous logins are off')
            return
        self.login(m['MessageID'], m['SessionKey'], m['DisplayName'], 'AnonymousLoginResponse')

    def login_failed(self, response, mid, status, why):
        log.warning('mas %s: login refused: %s', self.who(), why)
        self.reply(response, MessageID=mid, StatusCode=status)

    def login(self, mid, session, name, response):
        if session not in self.lobby.sessions:
            self.login_failed(response, mid, M.SESSION_KEY_INVALID, 'unknown session key %r' % session)
            return
        account = self.server.store.account(self.app_id, name)
        if account['banned']:
            self.login_failed(response, mid, M.ACCOUNT_BANNED, '%s is banned' % name)
            return
        p = self.lobby.login(session, account)
        self.server.store.touch_login(p.account_id)
        ch = self.lobby.default_channel()
        log.info('mas %s: %s logged in (account %d)', self.who(), p.name, p.account_id)
        self.reply(response, MessageID=mid, StatusCode=M.SUCCESS, AccountID=p.account_id, AccountType=M.ACCOUNT_MASTER,
                   MediusWorldID=ch.id,
                   ConnectInfo=self.server.lobby_connect_info(p, ch.id, server_key=response != 'TicketLoginResponse'))
