# SPDX-License-Identifier: GPL-2.0-or-later
"""The live state of a universe: signed-in players, lobby channels and games."""

import itertools
import secrets
import string
import time

_ALNUM = string.ascii_letters + string.digits


def random_key(n=16):
    return ''.join(secrets.choice(_ALNUM) for _ in range(n))


class Player:
    def __init__(self, account, session_key):
        self.account_id = account['account_id']
        self.name = account['name']
        self.app_id = account['app_id']
        self.session_key = session_key
        self.access_key = random_key()
        self.lobby = None          # the MLS connection
        self.channel = None        # Channel
        self.game = None           # Game
        self.state = 0             # MediusPlayerOnlineState
        self.login_time = time.time()
        self.char_encoding = 0
        self.language = 0

    def __repr__(self):
        return '%s (%d)' % (self.name, self.account_id)


class Channel:
    """A lobby world players chat in and look for games from."""

    def __init__(self, world_id, name, max_players=256, security=0, generic=(0, 0, 0, 0), generic_level=0, password=''):
        self.id = world_id
        self.name = name
        self.max_players = max_players
        self.security = security
        self.password = password
        self.generic = list(generic)
        self.generic_level = generic_level
        self.players = {}           # account id -> Player

    def games(self, lobby):
        return [g for g in lobby.games.values() if g.channel is self]


class Game:
    """A game world: its Medius record (name, rules, players) and the DME world it's played in."""

    def __init__(self, world_id, channel, host, fields):
        self.id = world_id
        self.channel = channel
        self.host = host
        self.created = time.time()
        self.name = fields.get('GameName', '')
        self.password = fields.get('GamePassword', '')
        self.spectator_password = fields.get('SpectatorPassword', '')
        self.min_players = fields.get('MinPlayers', 0)
        self.max_players = fields.get('MaxPlayers', 8) or 8
        self.game_level = fields.get('GameLevel', 0)
        self.skill = fields.get('PlayerSkillLevel', 0)
        self.rules = fields.get('RulesSet', 0)
        self.generic = [fields.get('GenericField%d' % i, 0) for i in range(1, 9)]
        self.host_type = fields.get('GameHostType', 0)
        self.attributes = fields.get('Attributes', 0)
        self.stats = bytes(256)
        self.status = 1            # MediusWorldStatus: WorldPendingCreation
        self.players = {}          # account id -> Player, as the DME reports them
        self.dme = None            # dme.World

    @property
    def player_count(self):
        return len(self.players)


class Lobby:
    def __init__(self, game_config):
        self.config = game_config
        self.sessions = {}          # session key -> Player or None (session without login)
        self.by_access = {}         # access key -> Player
        self.by_account = {}        # account id -> Player
        self.channels = {}
        self.games = {}
        self.buddy_invites = {}     # (inviter, invited account id) -> who to answer when the invited player does
        self._game_ids = itertools.count(1)
        for i, c in enumerate(game_config.channels):
            ch = Channel(c.get('id', i + 1), c['name'], c.get('max_players', 256), c.get('security', 0),
                         c.get('generic', (0, 0, 0, 0)), c.get('generic_level', 0))
            self.channels[ch.id] = ch

    def player_count(self):
        return len(self.by_account)

    def default_channel(self):
        return next(iter(self.channels.values()))

    # ---- sessions

    def begin_session(self):
        key = random_key()
        self.sessions[key] = None
        return key

    def end_session(self, key):
        p = self.sessions.pop(key, None)
        if p is not None:
            self.logout(p)

    def login(self, session_key, account):
        """Signs an account in on a session; a previous sign-in of the same account is dropped."""
        old = self.by_account.get(account['account_id'])
        if old is not None:
            self.logout(old)
        p = Player(account, session_key)
        self.sessions[session_key] = p
        self.by_access[p.access_key] = p
        self.by_account[p.account_id] = p
        return p

    def logout(self, p):
        if self.by_account.get(p.account_id) is p:
            del self.by_account[p.account_id]
        self.by_access.pop(p.access_key, None)
        if self.sessions.get(p.session_key) is p:
            self.sessions[p.session_key] = None
        self.leave_channel(p)
        if p.lobby is not None:
            conn, p.lobby = p.lobby, None
            conn.player = None
            conn.close()

    def player(self, session_key):
        return self.sessions.get(session_key)

    def find_player(self, name=None, account_id=None):
        if account_id is not None:
            return self.by_account.get(account_id)
        for p in self.by_account.values():
            if p.name.lower() == (name or '').lower():
                return p
        return None

    # ---- channels

    def join_channel(self, p, ch):
        if p.channel is ch:
            return
        self.leave_channel(p)
        ch.players[p.account_id] = p
        p.channel = ch

    def leave_channel(self, p):
        if p.channel is not None:
            p.channel.players.pop(p.account_id, None)
            p.channel = None

    # ---- games

    def create_game(self, host, fields):
        g = Game(next(self._game_ids), host.channel or self.default_channel(), host, fields)
        self.games[g.id] = g
        return g

    def end_game(self, g):
        self.games.pop(g.id, None)
        for p in list(g.players.values()):
            if p.game is g:
                p.game = None
        g.players.clear()
