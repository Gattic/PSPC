# SPDX-License-Identifier: GPL-2.0-or-later
"""The games these servers know: GameSpy's game name, the secret key (it encrypts server lists and
checks hosts' heartbeats), the product ID, and their disc IDs. Keys as the games' own code builds
them (SWES assembles "wLfbMH" byte by byte at 0x08B376D4) and as GameSpy's key list has them.

More games come from a JSON file (load(); the server reads gamespy-games.json beside its database):
a list of games, each {"gamename", "name", "disc_ids", and either "like": a game known already that it
plays as (a mod, a region's release: its key, product ID, namespace, stored record and leaderboard
pages unless given) or "secret_key" and "product_id"; "namespace_id", "aliases" optional}."""

import json


class Game:
    def __init__(self, gamename, name, secret_key, product_id, disc_ids, namespace_id=0, empty_persist=b'',
                 leaderboard=None):
        self.gamename = gamename
        self.name = name
        self.secret_key = secret_key
        self.product_id = product_id
        self.disc_ids = disc_ids
        self.namespace_id = namespace_id
        self.empty_persist = empty_persist  # what a player who never saved gets from the stats server
        self.leaderboard = leaderboard      # its leaderboard pages' format (leaderboards.py), or None


GAMES = {g.gamename: g for g in (
    # Its stored record is the GP message inbox: version 0, a count, then the messages (little-endian).
    Game('swbfespsp', 'Star Wars Battlefront: Elite Squadron', 'wLfbMH', 11878, ('ULUS10390',), namespace_id=1,
         empty_persist=bytes(8), leaderboard='bfes'),
)}

# Other names of the same games (demo, automatch) share their keys.
ALIASES = {'swbfespspam': 'swbfespsp', 'swbfespspd': 'swbfespsp'}


def find(gamename):
    gamename = (gamename or '').lower()
    return GAMES.get(ALIASES.get(gamename, gamename))


def load(path):
    """Adds the games in a JSON file (see above). Returns them; raises ValueError for a bad entry."""
    with open(path, encoding='utf-8') as f:
        entries = json.load(f)
    if isinstance(entries, dict):
        entries = entries.get('games', [])
    added = []
    for e in entries:
        gamename = str(e.get('gamename', '')).lower()
        if not gamename or find(gamename):
            raise ValueError('%s: game name %r missing or known already' % (path, gamename))
        like = find(e['like']) if 'like' in e else None
        if 'like' in e and like is None:
            raise ValueError('%s: %s plays like %r, a game this server doesn\'t know' % (path, gamename, e['like']))
        secret, product = e.get('secret_key', like and like.secret_key), e.get('product_id', like and like.product_id)
        if not secret or not product:
            raise ValueError('%s: %s needs "secret_key" and "product_id" (or "like")' % (path, gamename))
        g = Game(gamename, e.get('name', gamename), secret, int(product), tuple(e.get('disc_ids', ())),
                 int(e.get('namespace_id', like.namespace_id if like else 0)),
                 like.empty_persist if like else b'', like.leaderboard if like else None)
        GAMES[gamename] = g
        aliases = e.get('aliases')
        if aliases is None and like:  # the like game's aliases, by their suffixes: swbfespspam -> <gamename>am
            aliases = [gamename + a[len(like.gamename):] for a, t in ALIASES.items()
                       if t == like.gamename and a.startswith(like.gamename)]
        for a in aliases or ():
            ALIASES[str(a).lower()] = gamename
        added.append(g)
    return added
