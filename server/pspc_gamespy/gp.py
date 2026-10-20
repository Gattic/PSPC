# SPDX-License-Identifier: GPL-2.0-or-later
"""GameSpy Presence: the connection manager (gpcm.gamespy.com:29900: accounts, login, status,
buddies and their messages) and the search manager (gpsp.gamespy.com:29901: checks and searches
that don't need a login).

Login: the server sends a challenge; the client logs in with nick@email (\\user\\), a unique nick or a
token, its own challenge and a response, MD5(MD5(password) + 48 spaces + user + client challenge +
server challenge + MD5(password)); the server answers with the proof, the same with the challenges
the other way round, which the client checks. Error codes are GP's (GPErrorCode in the SDK)."""

import asyncio
import logging
import secrets
import string

from . import access
from . import crypt
from . import kv
from .store import StoreError

log = logging.getLogger('gamespy')

LOGIN_TIMEOUT = 60              # seconds of silence from a connection that hasn't logged in
IDLE_TIMEOUT = 7200             # seconds of silence from a player who has (their game would have said something)
SEARCH_TIMEOUT = 30
MAX_SEARCH_REQUESTS = 50        # per search connection (a game asks one or two)
MAX_NAME = 30                   # nicks, unique nicks, first and last names
MAX_EMAIL = 100
MAX_TEXT = 1024                 # a buddy message
MAX_STATUS = 256                # a status or location string

# Statuses (GPEnum).
GP_OFFLINE, GP_ONLINE, GP_PLAYING, GP_STAGING, GP_CHATTING, GP_AWAY = 0, 1, 2, 3, 4, 5
# Buddy messages (GPI_BM_*).
BM_MESSAGE, BM_REQUEST, BM_AUTH, BM_UTM, BM_REVOKE = 1, 2, 4, 5, 6
BM_STATUS, BM_INVITE, BM_PING, BM_PONG = 100, 101, 102, 103

SIG = 'd41d8cd98f00b204e9800998ecf8427e'


def _challenge(n=10):
    return ''.join(secrets.choice(string.ascii_uppercase) for _ in range(n))


def valid_name(text, limit=MAX_NAME):
    """A nick, unique nick or name a player may have: printable, and none of the protocol's separator."""
    return 0 < len(text) <= limit and text.isprintable() and '\\' not in text


def valid_email(text):
    return '@' in text[1:-1] and len(text) <= MAX_EMAIL and valid_name(text, MAX_EMAIL) and ' ' not in text


def check_new_user(limits, ip, email, nick, uniquenick):
    """The GP error code for a new account that may not be made (0: it may), and a message."""
    if not valid_email(email):
        return 513 if email else 259, 'The nick or e-mail address is invalid.'
    if not valid_name(nick) or (uniquenick and not valid_name(uniquenick)):
        return 259, 'The nick or e-mail address is invalid.'
    if not limits.allow(limits.new_accounts, ip):
        return 5, 'Too many accounts were made from this address. Try again later.'
    return 0, ''


def _error(code, message, op_id=None, fatal=False):
    pairs = [('error', ''), ('err', code)]
    if fatal:
        pairs.append(('fatal', ''))
    pairs.append(('errmsg', message))
    if op_id is not None:
        pairs.append(('id', op_id))
    return kv.build(*pairs)


class Session:
    """One gpcm connection."""

    def __init__(self, presence, reader, writer):
        self.presence = presence
        self.store = presence.store
        self.reader = reader
        self.writer = writer
        self.peer = writer.get_extra_info('peername') or ('?', 0)
        self.ip = self.peer[0]
        self.limits = presence.limits
        self.challenge = _challenge()
        self.profile = None
        self.sesskey = 0
        self.status = (GP_OFFLINE, '', '')
        self.game_port = 0
        self.closed = False
        self.ticket = ''

    @property
    def profileid(self):
        return self.profile['profileid'] if self.profile else 0

    def send(self, text):
        if self.closed:
            return
        log.debug('gpcm %s <- %s', self.peer, access.safe(text, 300))
        try:
            self.writer.write(text.encode('utf-8', 'replace'))
        except Exception:
            self.closed = True
        if access.drop_if_backlogged(self.writer):
            self.closed = True

    async def run(self):
        self.send(kv.build(('lc', 1), ('challenge', self.challenge), ('id', 1)))
        splitter = kv.Splitter()
        try:
            while not self.closed:
                try:
                    data = await asyncio.wait_for(self.reader.read(16384),
                                                  access.patience(self.ip, IDLE_TIMEOUT if self.profile else LOGIN_TIMEOUT))
                except asyncio.TimeoutError:
                    log.info('gpcm %s: silent for too long; closed', self.peer)
                    break
                if not data:
                    break
                for msg in splitter.feed(data.decode('utf-8', 'replace')):
                    log.debug('gpcm %s -> %s', self.peer, msg.pairs)
                    if not self.limits.allow(self.limits.presence_messages, self.ip):
                        log.info('gpcm %s: too many messages; closed', self.peer)
                        return
                    handler = getattr(self, 'on_' + msg.command, None) if msg.command.isidentifier() else None
                    if handler:
                        handler(msg)
                    else:
                        log.info('gpcm %s: unhandled %s', self.peer, access.safe(msg.command))
                    if self.closed:
                        break                   # the rest of what this connection sent is not read
                await self.writer.drain()
        except kv.TooLong as e:
            log.info('gpcm %s: %s; closed', self.peer, e)
        except (ConnectionError, asyncio.IncompleteReadError, OSError):
            pass
        except Exception:                   # a message this code can't handle ends this connection, nothing else
            log.warning('gpcm %s: a message was not handled; closed', self.peer, exc_info=log.isEnabledFor(logging.DEBUG))
        finally:
            self.closed = True
            self.presence.logout(self)
            try:
                self.writer.close()
            except Exception:
                pass

    # ---- accounts and login

    def on_newuser(self, m):
        email, nick, uniquenick = m.get('email', ''), m.get('nick', ''), m.get('uniquenick', '')
        if 'passwordenc' in m:
            password = crypt.decode_password(m['passwordenc'])
        elif 'passenc' in m:
            password = crypt.decode_password(m['passenc'])
        else:
            password = m.get('password', '')
        op = m.get('id', 1)
        code, message = check_new_user(self.limits, self.ip, email, nick, uniquenick)
        if code:
            log.info('gpcm %s: new account %s refused (%d: %s)', self.peer, access.safe(nick), code, message)
            self.send(_error(code, message, op))
            return
        try:
            userid, profileid = self.store.new_user(email, crypt.md5hex(password), nick, uniquenick, m.int('namespaceid'))
        except StoreError as e:
            log.info('gpcm %s: new account %s refused (%d: %s)', self.peer, access.safe(nick), e.code, e.message)
            self.send(_error(e.code, e.message, op))
            return
        log.info('gpcm: new account %s (%s), profile %d, from %s', access.safe(nick), access.safe(email), profileid, self.ip)
        self.send(kv.build(('nur', ''), ('userid', userid), ('profileid', profileid), ('id', op)))

    def refuse_login(self, code, message, op):
        """A login that failed: the address pays for it (guesses are slowed down), and the connection ends."""
        log.info('gpcm %s: login refused (%d: %s)', self.peer, code, message)
        self.limits.spend(self.limits.failed_logins, self.ip)
        self.send(_error(code, message, op, fatal=True))
        self.closed = True

    def on_login(self, m):
        op = m.get('id', 1)
        if self.profile:                    # a second login would leave the first profile "online" on a connection that is not its
            self.send(_error(263, 'This connection is logged in already.', op, fatal=True))
            self.closed = True
            return
        if not self.limits.available(self.limits.failed_logins, self.ip):
            log.info('gpcm %s: login refused: too many failed logins from this address', self.peer)
            self.send(_error(5, 'Too many failed logins from this address. Try again in a few minutes.', op, fatal=True))
            self.closed = True
            return
        client_challenge = m.get('challenge', '')
        response = m.get('response', '')[:32]
        if 'user' in m:
            user = m['user']
            nick, _, email = user.partition('@')
            profile = self.store.profile_by_nick_email(nick, email)
            if not profile:
                code = 258 if self.store.users_by_email(email) else 259
                self.refuse_login(code, 'There is no such profile.' if code == 258 else 'The e-mail address is incorrect.', op)
                return
        elif 'uniquenick' in m:
            user = m['uniquenick']
            profile = self.store.profile_by_uniquenick(user, m.int('namespaceid'))
            if not profile:
                self.refuse_login(265, 'The unique nick is incorrect.', op)
                return
        else:
            self.send(_error(263, 'This login method is not supported.', op, fatal=True))
            self.closed = True
            return
        if profile['banned']:
            self.send(_error(263, 'This account is banned.', op, fatal=True))
            self.closed = True
            return
        pw = profile['password_md5']
        if response != crypt.login_hash(pw, user, client_challenge, self.challenge):
            if response == crypt.login_hash(pw, user, self.challenge, client_challenge):
                log.warning('gpcm: login response with the challenges the other way round')
            else:
                self.refuse_login(260, 'The password is incorrect.', op)
                return
        proof = crypt.login_hash(pw, user, self.challenge, client_challenge)
        self.profile = self.store.profile(profile['profileid'])
        self.sesskey = secrets.randbelow(0x7FFFFFFF) + 1
        self.game_port = m.int('port')
        self.status = (GP_ONLINE, '', '')
        self.store.touch_login(self.profileid)
        self.presence.login(self)
        pairs = [('lc', 2), ('sesskey', self.sesskey), ('proof', proof), ('userid', self.profile['userid']),
                 ('profileid', self.profileid)]
        if self.profile['uniquenick']:
            pairs.append(('uniquenick', self.profile['uniquenick']))
        self.ticket = _challenge(22)
        self.presence.tickets[self.ticket] = self.profileid
        pairs += [('lt', self.ticket), ('id', op)]
        self.send(kv.build(*pairs))
        log.info('gpcm: %s logged in (profile %d) from %s', access.safe(self.profile['nick']), self.profileid, self.ip)
        self.send_lists()

    def send_lists(self):
        buddies = self.store.buddies(self.profileid)
        if buddies:
            self.send(kv.build(('bdy', len(buddies)), ('list', ','.join(map(str, buddies)))))
        blocks = self.store.blocks(self.profileid)
        if blocks:
            self.send(kv.build(('blk', len(blocks)), ('list', ','.join(map(str, blocks)))))
        for b in buddies:
            self.send(self.presence.status_message(b))
        for r in self.store.pending_requests(self.profileid):
            if r['fromid'] in buddies:
                self.grant(r['fromid'])      # someone on this player's list asked back (see on_addbuddy)
                continue
            self.send(kv.build(('bm', BM_REQUEST), ('f', r['fromid']), ('msg', r['reason'] + '|signed|' + SIG)))
        for msg in self.store.take_messages(self.profileid):
            self.send(kv.build(('bm', msg['type']), ('f', msg['fromid']), ('date', msg['created']), ('msg', msg['text'])))

    def on_logout(self, m):
        self.closed = True

    def on_ka(self, m):
        self.send(kv.build(('ka', '')))

    def needs_login(self, m):
        if not self.profile:
            self.send(_error(2, 'Not logged in.', m.get('id')))
            return True
        return False

    # ---- status

    def on_status(self, m):
        if self.needs_login(m):
            return
        self.status = (m.int('status'), m.get('statstring', '')[:MAX_STATUS], m.get('locstring', '')[:MAX_STATUS])
        self.presence.broadcast_status(self)

    # ---- profiles

    def on_getprofile(self, m):
        if self.needs_login(m):
            return
        op = m.get('id', 1)
        p = self.store.profile(m.int('profileid'))
        if not p:
            self.send(_error(2561, 'There is no such profile.', op))
            return
        pairs = [('pi', ''), ('profileid', p['profileid']), ('userid', p['userid']), ('nick', p['nick'])]
        if p['uniquenick']:
            pairs.append(('uniquenick', p['uniquenick']))
        if p['userid'] == self.profile['userid']:
            pairs.append(('email', p['email']))
        if p['firstname']:
            pairs.append(('firstname', p['firstname']))
        if p['lastname']:
            pairs.append(('lastname', p['lastname']))
        if p['countrycode']:
            pairs.append(('countrycode', p['countrycode']))
        pairs += [('sig', SIG), ('id', op)]
        self.send(kv.build(*pairs))

    def on_updatepro(self, m):
        if self.needs_login(m):
            return
        fields = {k: v for k, v in m.items() if k in ('nick', 'firstname', 'lastname', 'countrycode') and
                  len(v) <= MAX_NAME and v.isprintable() and '\\' not in v and (v or k != 'nick')}
        self.store.update_profile(self.profileid, fields)
        self.profile = self.store.profile(self.profileid)

    def on_updateui(self, m):
        if self.needs_login(m):
            return
        if 'passwordenc' in m:
            self.store.set_password(self.profile['userid'], crypt.md5hex(crypt.decode_password(m['passwordenc'])))

    def on_newprofile(self, m):
        if self.needs_login(m):
            return
        op = m.get('id', 1)
        if not valid_name(m.get('nick', '')):
            self.send(_error(1025, 'The nick is invalid.', op))
            return
        try:
            pid = self.store.new_profile(self.profile['userid'], m.get('nick', ''))
        except StoreError as e:
            self.send(_error(e.code, e.message, op))
            return
        self.send(kv.build(('npr', 1), ('profileid', pid), ('id', op)))

    def on_delprofile(self, m):
        if self.needs_login(m):
            return
        op = m.get('id', 1)
        if len(self.store.profiles_of(self.profile['userid'])) <= 1:
            self.send(_error(3073, 'The last profile cannot be deleted.', op))
            return
        self.store.delete_profile(self.profileid)
        self.send(kv.build(('dpr', 1), ('id', op)))

    def on_registernick(self, m):
        if self.needs_login(m):
            return
        op = m.get('id', 1)
        try:
            if not valid_name(m.get('uniquenick', '')):
                raise StoreError(4097, 'invalid')
            self.store.register_uniquenick(self.profileid, m.get('uniquenick', ''))
        except StoreError:
            self.send(_error(4097, 'That unique nick is taken.', op))
            return
        self.profile = self.store.profile(self.profileid)
        self.send(kv.build(('rn', 1), ('id', op)))

    def on_registercdkey(self, m):
        self.send(kv.build(('rc', 1), ('id', m.get('id', 1))))

    # ---- buddies

    def on_addbuddy(self, m):
        if self.needs_login(m):
            return
        to = m.int('newprofileid')
        target = self.store.profile(to)
        if not target or to == self.profileid:
            self.send(_error(1538, 'There is no such profile.', m.get('id')))
            return
        if to in self.store.buddies(self.profileid):
            self.send(_error(1539, 'That profile is already a buddy.', m.get('id')))
            return
        reason = m.get('reason', '')[:MAX_TEXT]
        other = self.presence.sessions.get(to)
        if self.profileid in self.store.buddies(to):
            # They have this player on their list already (their request was just accepted, and the
            # game asks back so that each is on the other's list): no need to ask them again.
            self.store.add_buddy_request(self.profileid, to, reason)
            self.store.authorize(self.profileid, to)
            self.send(kv.build(('bm', BM_AUTH), ('f', to), ('msg', 'I have authorized your request to add me to your list|signed|' + SIG)))
            self.send(self.presence.status_message(to))
            if other:
                other.send(self.presence.status_message(self.profileid))
            log.info('gpcm: %d and %d are on each other\'s lists', self.profileid, to)
            return
        try:
            self.store.add_buddy_request(self.profileid, to, reason)
        except StoreError as e:
            self.send(_error(e.code, e.message, m.get('id')))
            return
        if other:
            other.send(kv.build(('bm', BM_REQUEST), ('f', self.profileid), ('msg', reason + '|signed|' + SIG)))
        log.info('gpcm: %d asks %d to be a buddy', self.profileid, to)

    def on_authadd(self, m):
        if self.needs_login(m):
            return
        frm = m.int('fromprofileid')
        if not self.store.has_request(frm, self.profileid):
            self.send(_error(1793, 'There is no such request.', m.get('id')))
            return
        self.grant(frm)
        log.info('gpcm: %d authorized %d', self.profileid, frm)

    def grant(self, frm):
        """Grants frm's request to have this player on their list, telling them now or at their next login."""
        self.store.authorize(frm, self.profileid)
        text = 'I have authorized your request to add me to your list'
        other = self.presence.sessions.get(frm)
        if other:
            other.send(kv.build(('bm', BM_AUTH), ('f', self.profileid), ('msg', text + '|signed|' + SIG)))
            other.send(self.presence.status_message(self.profileid))
        else:
            self.store.queue_message(self.profileid, frm, BM_AUTH, text)

    def on_delbuddy(self, m):
        if self.needs_login(m):
            return
        self.store.del_buddy(self.profileid, m.int('delprofileid'))

    def on_revoke(self, m):
        if self.needs_login(m):
            return
        pid = m.int('profileid')
        self.store.del_buddy(pid, self.profileid)
        other = self.presence.sessions.get(pid)
        if other:
            other.send(kv.build(('bm', BM_REVOKE), ('f', self.profileid), ('msg', '|signed|' + SIG)))

    def on_addblock(self, m):
        if self.needs_login(m):
            return
        self.store.add_block(self.profileid, m.int('profileid'))

    def on_removeblock(self, m):
        if self.needs_login(m):
            return
        self.store.remove_block(self.profileid, m.int('profileid'))

    def on_bm(self, m):
        if self.needs_login(m):
            return
        mtype = m.int('bm')
        to = m.int('t')
        text = m.get('msg', '')[:MAX_TEXT]
        if mtype in (BM_MESSAGE, BM_UTM, BM_PING, BM_PONG, BM_INVITE) and self.profileid in self.store.blocks(to):
            return                          # they blocked this player
        if mtype in (BM_MESSAGE, BM_UTM, BM_PING, BM_PONG, BM_INVITE):
            other = self.presence.sessions.get(to)
            if other:
                other.send(kv.build(('bm', mtype), ('f', self.profileid), ('msg', text)))
            elif mtype == BM_MESSAGE and (to in self.store.buddies(self.profileid) or self.profileid in self.store.buddies(to)):
                self.store.queue_message(self.profileid, to, mtype, text)       # friends' messages wait for their next login

    def on_pinvite(self, m):
        if self.needs_login(m):
            return
        other = self.presence.sessions.get(m.int('profileid'))
        if other:
            other.send(kv.build(('bm', BM_INVITE), ('f', self.profileid), ('msg', '|p|%d|l|%s' % (m.int('productid'),
                                                                                              m.get('location', '')))))


class Presence:
    """The connection manager: who is logged in, and their statuses."""

    def __init__(self, store, limits=None):
        self.store = store
        self.limits = limits or access.Limits.none()
        self.sessions = {}  # profile id -> Session
        self.tickets = {}   # login ticket (lt) -> profile id: the stats server's authtoken sign-ins

    async def handle(self, reader, writer):
        await Session(self, reader, writer).run()

    def login(self, session):
        old = self.sessions.get(session.profileid)
        if old and old is not session:
            old.send(_error(6, 'This profile has been disconnected by another login.', fatal=True))
            old.closed = True
            try:
                old.writer.close()
            except Exception:
                pass
        self.sessions[session.profileid] = session
        self.broadcast_status(session)

    def logout(self, session):
        self.tickets.pop(session.ticket, None)
        if session.profileid and self.sessions.get(session.profileid) is session:
            del self.sessions[session.profileid]
            session.status = (GP_OFFLINE, '', '')
            self.broadcast_status(session)
            log.info('gpcm: profile %d logged out', session.profileid)

    def status_message(self, profileid):
        s = self.sessions.get(profileid)
        status, statstring, locstring = s.status if s else (GP_OFFLINE, '', '')
        msg = '|s|%d' % status
        if statstring:
            msg += '|ss|' + statstring
        if locstring:
            msg += '|ls|' + locstring
        if s and status != GP_OFFLINE:
            try:
                a, b, c, d = (int(x) for x in s.peer[0].split('.'))
                msg += '|ip|%d|p|%d' % ((d << 24) | (c << 16) | (b << 8) | a, s.game_port)
            except ValueError:
                pass
        return kv.build(('bm', BM_STATUS), ('f', profileid), ('msg', msg))

    def broadcast_status(self, session):
        """Everyone online with this profile on their list hears its new status."""
        message = self.status_message(session.profileid)
        for pid in self.store.buddy_of(session.profileid):
            other = self.sessions.get(pid)
            if other:
                other.send(message)

    def online(self):
        return [s for s in self.sessions.values() if s.profile]


class Search:
    """The search manager: one request per connection, answered, then the client closes it."""

    def __init__(self, store, presence):
        self.store = store
        self.presence = presence
        self.limits = presence.limits

    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername')
        ip = peer[0] if peer else ''
        splitter = kv.Splitter()
        requests = 0
        try:
            while True:
                try:
                    data = await asyncio.wait_for(reader.read(16384), SEARCH_TIMEOUT)
                except asyncio.TimeoutError:
                    break
                if not data:
                    break
                for m in splitter.feed(data.decode('utf-8', 'replace')):
                    log.debug('gpsp %s -> %s', peer, m.pairs)
                    requests += 1
                    if requests > MAX_SEARCH_REQUESTS or not self.limits.allow(self.limits.searches, ip):
                        return
                    reply = self.answer(m, ip)
                    if reply is None:
                        log.info('gpsp %s: unhandled %s', peer, access.safe(m.command))
                        continue
                    log.debug('gpsp %s <- %s', peer, access.safe(reply, 300))
                    writer.write(reply.encode('utf-8', 'replace'))
                await writer.drain()
        except kv.TooLong as e:
            log.info('gpsp %s: %s; closed', peer, e)
        except (ConnectionError, OSError):
            pass
        except Exception:
            log.warning('gpsp %s: a request was not handled; closed', peer, exc_info=log.isEnabledFor(logging.DEBUG))
        finally:
            try:
                writer.close()
            except Exception:
                pass

    def _password(self, m):
        if 'passenc' in m:
            return crypt.decode_password(m['passenc'])
        return m.get('pass', m.get('password', ''))

    def _owned(self, email, password, ip):
        """The user with this e-mail and password: (user, error code). Like a login, a wrong
        password is paid for by the address that tried it."""
        if not self.limits.available(self.limits.failed_logins, ip):
            return None, 'password'
        users = self.store.users_by_email(email)
        if not users:
            return None, 'email'
        pw = crypt.md5hex(password)
        user = next((u for u in users if u['password_md5'] == pw), None)
        if user is None:
            self.limits.spend(self.limits.failed_logins, ip)
        return (user, None) if user else (None, 'password')

    def answer(self, m, ip=''):
        c = m.command
        if c == 'valid':
            return kv.build(('vr', 1 if self.store.users_by_email(m.get('email', '')) else 0))
        if c == 'check':
            user, err = self._owned(m.get('email', ''), self._password(m), ip)
            if err:
                return kv.build(('cur', 3585 if err == 'email' else 3587))
            prof = self.store.profile_by_nick_email(m.get('nick', ''), m.get('email', ''))
            if not prof:
                return kv.build(('cur', 3586))
            return kv.build(('cur', 0), ('pid', prof['profileid']))
        if c == 'nicks':
            user, err = self._owned(m.get('email', ''), self._password(m), ip)
            if err:
                return kv.build(('nr', 259 if err == 'email' else 260), ('ndone', ''))
            pairs = [('nr', 0)]
            for p in self.store.profiles_of(user['userid']):
                pairs += [('nick', p['nick']), ('uniquenick', p['uniquenick'])]
            pairs.append(('ndone', ''))
            return kv.build(*pairs)
        if c == 'newuser':
            password = self._password(m)
            code, _ = check_new_user(self.limits, ip, m.get('email', ''), m.get('nick', ''), m.get('uniquenick', ''))
            if code:
                return kv.build(('nur', code), ('pid', 0))
            try:
                _, pid = self.store.new_user(m.get('email', ''), crypt.md5hex(password), m.get('nick', ''),
                                             m.get('uniquenick', ''), m.int('namespaceid'))
            except StoreError as e:
                prof = self.store.profile_by_nick_email(m.get('nick', ''), m.get('email', ''))
                return kv.build(('nur', e.code), ('pid', prof['profileid'] if prof else 0))
            log.info('gpsp: new account %s (%s), profile %d, from %s', access.safe(m.get('nick', '')),
                     access.safe(m.get('email', '')), pid, ip)
            return kv.build(('nur', 0), ('pid', pid))
        if c == 'search':
            rows = self.store.search(nick=m.get('nick', ''), uniquenick=m.get('uniquenick', ''), email=m.get('email', ''),
                                     firstname=m.get('firstname', ''), lastname=m.get('lastname', ''))
            pairs = []
            for p in rows:
                pairs += [('bsr', p['profileid']), ('nick', p['nick']), ('firstname', p['firstname']),
                          ('lastname', p['lastname']), ('email', '[hidden]'), ('uniquenick', p['uniquenick']),
                          ('namespaceid', p['namespaceid'])]
            pairs += [('bsrdone', ''), ('more', 0)]
            return kv.build(*pairs)
        if c in ('others', 'otherslist'):
            pid = m.int('profileid')
            pairs = [(c, '')]
            for other in self.store.buddy_of(pid):
                p = self.store.profile(other)
                if p:
                    pairs += [('o', p['profileid']), ('nick', p['nick']), ('first', p['firstname']), ('last', p['lastname']),
                              ('email', '[hidden]'), ('uniquenick', p['uniquenick']), ('namespaceid', p['namespaceid'])]
            pairs.append(('odone' if c == 'others' else 'oldone', ''))
            return kv.build(*pairs)
        if c == 'uniquesearch':
            nick = m.get('preferrednick', '')
            suggestions = [nick + str(n) for n in range(1, 30) if not self.store.uniquenick_taken(nick + str(n))][:5]
            pairs = [('us', len(suggestions))] + [('nick', s) for s in suggestions] + [('usdone', '')]
            return kv.build(*pairs)
        if c == 'pmatch':
            return kv.build(('psrdone', ''))
        return None
