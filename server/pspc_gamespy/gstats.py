# SPDX-License-Identifier: GPL-2.0-or-later
"""The stats and persistent storage server (gamestats.gamespy.com, TCP 29920): games keep player
records here (getpd/setpd) and report finished games (newgame/updgame).

The stream is GameSpy's key/value messages, each byte XORed with "GameSpy3D" (the key restarts
after each \\final\\, which is sent in the clear):
- the server sends \\lc\\1\\challenge\\<10 characters>\\id\\1; the game answers \\auth\\ with its game
  name and response = MD5(the decimal of a number folded from the challenge + the game's secret
  key); the server replies \\lc\\2\\sesskey\\<n>\\proof\\0\\id\\1.
- \\authp\\ signs a player in (pid and resp, or the authtoken GP's login gave): \\pauthr\\<pid>\\lid\\..
  or \\pauthr\\-1\\errmsg\\... (a game closes the connection on a refusal).
- \\getpd\\ (pid, ptype 0-3: private read-only/read-write, public read-only/read-write, dindex, keys)
  -> \\getpdr\\1\\lid\\..\\pid\\..\\mod\\<time>\\length\\<n>\\data\\<n bytes>; \\setpd\\ (the same, kv, length,
  data) -> \\setpdr\\1\\lid\\..\\pid\\..\\mod\\<time>.
- \\newgame\\ (sesskey) and \\updgame\\ (sesskey, done, gamedata: \\x01-separated keys and values)
  record a game and its outcome; there is no reply.
"""

import asyncio
import logging
import secrets
import string
import time

from . import access
from . import crypt
from . import games
from . import kv
from .store import StoreError

log = logging.getLogger('gamespy')

PD_PRIVATE_RO, PD_PRIVATE_RW, PD_PUBLIC_RO, PD_PUBLIC_RW = 0, 1, 2, 3
KEEPALIVE_INTERVAL = 60
XOR_KEY = b'GameSpy3D'
FINAL = b'\\final\\'
AUTH_TIMEOUT = 60               # seconds a new connection has to send \auth\
IDLE_TIMEOUT = 7200             # seconds of silence before a signed-in connection is closed
MAX_BUFFER = 1 << 17            # the most a connection may send without ending a message
MAX_RECORD = 1 << 16            # a stored record (setpd's data)
MAX_GAMES = 16                  # games (newgame) one connection may report
MAX_GAMEDATA = 8192


def chresp_num(challenge):
    num = 0
    for ch in challenge.encode('latin-1'):
        num = (ch - num * 0x63306CE7) & 0xFFFFFFFF
    return num - (1 << 32) if num & 0x80000000 else num


def auth_response(challenge, secret_key):
    return crypt.md5hex('%d%s' % (chresp_num(challenge), secret_key))


class Xor:
    """One direction of the stream's XOR; the key position carries over between reads."""

    def __init__(self):
        self.k = 0
        self.held = b''             # a possible start of \final\ split between reads

    def apply(self, data):
        data = self.held + data
        self.held = b''
        out = bytearray()
        i = 0
        n = len(data)
        while i < n:
            if data[i] == 0x5C:
                rest = data[i:i + 7]
                if rest == FINAL:
                    out += FINAL
                    i += 7
                    self.k = 0
                    continue
                if len(rest) < 7 and FINAL.startswith(rest):
                    self.held = rest
                    break
            out.append(data[i] ^ XOR_KEY[self.k])
            self.k = (self.k + 1) % len(XOR_KEY)
            i += 1
        return bytes(out)


def parse_message(raw):
    """A message's (command, dict of lower-case keys -> text, data bytes or None). The data value
    (setpd) is binary and as long as the length key says, so it is cut out before parsing the rest."""
    data = None
    i = raw.find(b'\\data\\')
    if i >= 0:
        head = raw[:i]
        m = kv.parse(head.decode('latin-1'))
        length = m.int('length', -1)
        data = raw[i + 6:]
        if 0 <= length <= len(data):
            data = data[:length]
        return m, data
    return kv.parse(raw.decode('latin-1')), None


class Session:
    def __init__(self, server, reader, writer):
        self.server = server
        self.store = server.store
        self.reader = reader
        self.writer = writer
        self.peer = writer.get_extra_info('peername') or ('?', 0)
        self.limits = server.limits
        self.challenge = ''.join(secrets.choice(string.ascii_uppercase) for _ in range(10))
        self.sesskey = secrets.randbelow(0x7FFFFFFF) + 1
        self.rx = Xor()
        self.tx = Xor()
        self.game = None
        self.profiles = set()           # profile ids signed in on this connection
        self.profileid = 0
        self.games = {}                 # the game's sesskey -> game id
        self.closed = False

    def send(self, payload):
        """Sends a message (text, or bytes for binary data) with \\final\\ appended."""
        if self.closed:
            return
        if isinstance(payload, str):
            payload = payload.encode('latin-1', 'replace')
        log.debug('gstats %s <- %r', self.peer[0], payload[:200])
        try:
            self.writer.write(self.tx.apply(payload + FINAL))
        except Exception:
            self.closed = True
        if access.drop_if_backlogged(self.writer):
            self.closed = True

    def send_error(self, code, message, fatal=False):
        self.send(kv.build(('error', ''), ('err', code), ('errmsg', message), *([('fatal', 1)] if fatal else []),
                           final=False))
        if fatal:
            self.closed = True

    async def run(self):
        self.send(kv.build(('lc', 1), ('challenge', self.challenge), ('id', 1), final=False))
        buf = b''
        last_heard = time.monotonic()
        try:
            while not self.closed:
                try:
                    data = await asyncio.wait_for(self.reader.read(16384), KEEPALIVE_INTERVAL)
                except asyncio.TimeoutError:
                    silent = time.monotonic() - last_heard
                    limit = access.patience(self.peer[0], IDLE_TIMEOUT if self.game else AUTH_TIMEOUT)
                    if limit is not None and silent > limit:
                        log.info('gstats %s: silent for %d seconds; closed', self.peer[0], silent)
                        break
                    self.send('\\ka\\')
                    continue
                if not data:
                    break
                last_heard = time.monotonic()
                buf += self.rx.apply(data)
                while True:
                    i = buf.find(FINAL)
                    if i < 0:
                        break
                    raw, buf = buf[:i], buf[i + 7:]
                    if raw.strip():
                        if not self.limits.allow(self.limits.stats_messages, self.peer[0]):
                            log.info('gstats %s: too many messages; closed', self.peer[0])
                            return
                        self.dispatch(raw)
                if len(buf) > MAX_BUFFER:
                    log.info('gstats %s: a message longer than %d bytes; closed', self.peer[0], MAX_BUFFER)
                    break
                await self.writer.drain()
        except (ConnectionError, OSError):
            pass
        except Exception:
            log.warning('gstats %s: a message was not handled; closed', self.peer[0], exc_info=log.isEnabledFor(logging.DEBUG))
        finally:
            self.closed = True
            try:
                self.writer.close()
            except Exception:
                pass

    def dispatch(self, raw):
        m, data = parse_message(raw)
        log.debug('gstats %s -> %s%s', self.peer[0], m.pairs, ' + %d bytes' % len(data) if data is not None else '')
        handler = getattr(self, 'on_' + m.command, None) if m.command.isidentifier() else None
        if handler is None:
            log.info('gstats %s: unhandled %s', self.peer[0], access.safe(m.command))
            return
        handler(m, data)

    # ---- sign-in

    def on_auth(self, m, data):
        self.game = games.find(m.get('gamename'))
        if self.game is None:
            log.info("gstats %s: auth for %s, a game this server doesn't know", self.peer[0], access.safe(m.get('gamename')))
            self.send_error(0x100, 'Unknown game.', fatal=True)
            return
        expected = auth_response(self.challenge, self.game.secret_key)
        if m.get('response', '') != expected:
            log.warning('gstats %s: auth response %s for %s, expected %s (letting it in)', self.peer[0],
                        access.safe(m.get('response'), 40), self.game.gamename, expected)
        self.send(kv.build(('lc', 2), ('sesskey', self.sesskey), ('proof', 0), ('id', m.get('id', 1)), final=False))

    def on_authp(self, m, data):
        lid = m.get('lid', 0)
        profile = None
        if m.int('pid'):
            profile = self.store.profile(m.int('pid'))
            # The response is MD5(the plain password + a challenge made from the session key). This
            # server keeps only the password's MD5, so it can't check it. Games sign players in here
            # after GP has checked their password, so the player has to be logged in to GP - from
            # this address - or anyone could be anyone here.
            if profile is not None and not self.logged_in_from_here(profile['profileid']):
                log.info('gstats %s: sign-in as profile %d refused: not logged in to Presence from this address',
                         self.peer[0], profile['profileid'])
                profile = None
        elif m.get('authtoken'):
            pid = self.server.presence.tickets.get(m['authtoken']) if self.server.presence else None
            profile = self.store.profile(pid) if pid else None
        if profile is None or profile['banned']:
            log.info('gstats %s: player sign-in refused (%s)', self.peer[0],
                     'banned' if profile else 'no such profile %s' % access.safe(m.get('pid') or m.get('authtoken'), 30))
            self.send(kv.build(('pauthr', -1), ('errmsg', 'Invalid profile.'), ('lid', lid), final=False))
            return
        self.profileid = profile['profileid']
        self.profiles.add(self.profileid)
        log.info('gstats %s: %s signed in to %s stats', self.peer[0], access.safe(profile['nick']),
                 self.game.gamename if self.game else '?')
        self.send(kv.build(('pauthr', self.profileid), ('lid', lid), final=False))

    def logged_in_from_here(self, profileid):
        presence = self.server.presence
        if presence is None:
            return True                     # no Presence server here (tests of this one alone)
        session = presence.sessions.get(profileid)
        return session is not None and session.peer[0] == self.peer[0]

    def on_getpid(self, m, data):
        self.send(kv.build(('getpidr', -1), ('errmsg', 'CD keys are not used.'), ('lid', m.get('lid', 0)), final=False))

    def on_ka(self, m, data):
        pass

    # ---- persistent data

    def on_getpd(self, m, data):
        pid, ptype, dindex, lid = m.int('pid'), m.int('ptype'), m.int('dindex'), m.get('lid', 0)
        if ptype in (PD_PRIVATE_RO, PD_PRIVATE_RW) and pid not in self.profiles:
            self.send(kv.build(('getpdr', 0), ('lid', lid), ('pid', pid), ('mod', 0), ('length', 0), ('data', ''), final=False))
            return
        blob, modified = self.store.get_persist(pid, ptype, dindex)
        wanted = [k for k in m.get('keys', '').replace('\x01', '\\').split('\\') if k]
        if wanted:                          # key/value data: just those keys (an empty list is the whole record)
            values = decode_kv(blob)
            blob = b''.join(b'\x01' + k.encode('latin-1') + b'\x01' + values.get(k, '').encode('latin-1')
                            for k in wanted)
        elif not modified and self.game is not None:
            blob = self.game.empty_persist
        if modified and m.int('mod') and modified <= m.int('mod'):
            blob = b''                      # nothing newer than what the game has
        head = kv.build(('getpdr', 1), ('lid', lid), ('pid', pid), ('mod', modified), ('length', len(blob)), final=False)
        self.send(head.encode('latin-1') + b'\\data\\' + blob)

    def on_setpd(self, m, data):
        pid, ptype, dindex, lid = m.int('pid'), m.int('ptype'), m.int('dindex'), m.get('lid', 0)
        if pid not in self.profiles or ptype in (PD_PRIVATE_RO, PD_PUBLIC_RO):
            self.send(kv.build(('setpdr', 0), ('lid', lid), ('pid', pid), ('mod', 0), final=False))
            return
        data = data or b''
        if m.int('kv'):
            values = decode_kv(self.store.get_persist(pid, ptype, dindex)[0])
            values.update(decode_kv(data.rstrip(b'\0')))
            data = encode_kv(values)
        try:
            if len(data) > MAX_RECORD:
                raise StoreError(0, 'record too long')
            modified = self.store.set_persist(pid, ptype, dindex, data)
        except StoreError as e:
            log.info('gstats %s: profile %d: record not saved: %s', self.peer[0], pid, e.message)
            self.send(kv.build(('setpdr', 0), ('lid', lid), ('pid', pid), ('mod', 0), final=False))
            return
        log.info('gstats %s: profile %d saved %d bytes (type %d, index %d)', self.peer[0], pid, len(data), ptype, dindex)
        self.send(kv.build(('setpdr', 1), ('lid', lid), ('pid', pid), ('mod', modified), final=False))

    # ---- games

    def game_id(self, key):
        """The stored game for a game's session key, made if need be (None past MAX_GAMES a connection)."""
        game_id = self.games.get(key)
        if game_id is None and len(self.games) < MAX_GAMES:
            game_id = self.games[key] = self.store.new_game(self.game.gamename if self.game else '', self.profileid)
        return game_id

    def on_newgame(self, m, data):
        key = m.int('sesskey')
        game_id = self.game_id(key)
        if game_id is not None:
            log.info('gstats %s: game %d started (session %d)', self.peer[0], game_id, key)

    def on_updgame(self, m, data):
        key = m.int('sesskey')
        game_id = self.game_id(key)
        if game_id is None:
            return
        text = m.get('gamedata', '')[:MAX_GAMEDATA].replace('\x01', '\\')
        done = m.int('done') != 0
        self.store.update_game(game_id, text, done)
        if done:
            log.info('gstats %s: game %d finished: %s', self.peer[0], game_id, access.safe(text, 300))
            if self.server.on_round_end and self.game:
                self.server.on_round_end(self.game.gamename, self.peer[0], self.profileid, decode_kv(text.encode('latin-1')))


def decode_kv(blob):
    """'\\a\\1\\b\\2' (or with \\x01 separators) -> {'a': '1', 'b': '2'}."""
    text = blob.decode('latin-1').replace('\x01', '\\')
    parts = text.split('\\')
    if parts and parts[0] == '':
        parts = parts[1:]
    return {parts[i]: parts[i + 1] for i in range(0, len(parts) - 1, 2)}


def encode_kv(values):
    return ''.join('\\%s\\%s' % (k, v) for k, v in values.items()).encode('latin-1', 'replace')


class StatsServer:
    def __init__(self, store, presence=None, on_round_end=None, limits=None):
        self.store = store
        self.limits = limits or access.Limits.none()
        self.presence = presence
        self.on_round_end = on_round_end    # (gamename, host IP, host profile id, game data) at each reported round's end

    async def handle(self, reader, writer):
        await Session(self, reader, writer).run()
