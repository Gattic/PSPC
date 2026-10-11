# SPDX-License-Identifier: GPL-2.0-or-later
"""The games these servers know: GameSpy's game name, the secret key (it encrypts server lists and
checks hosts' heartbeats), the product ID, and their disc IDs. Keys as the games' own code builds
them (SWES assembles "wLfbMH" byte by byte at 0x08B376D4) and as GameSpy's key list has them."""


class Game:
    def __init__(self, gamename, name, secret_key, product_id, disc_ids, namespace_id=0, empty_persist=b''):
        self.gamename = gamename
        self.name = name
        self.secret_key = secret_key
        self.product_id = product_id
        self.disc_ids = disc_ids
        self.namespace_id = namespace_id
        self.empty_persist = empty_persist  # what a player who never saved gets from the stats server


GAMES = {g.gamename: g for g in (
    # Its stored record is the GP message inbox: version 0, a count, then the messages (little-endian).
    Game('swbfespsp', 'Star Wars Battlefront: Elite Squadron', 'wLfbMH', 11878, ('ULUS10390',), namespace_id=1,
         empty_persist=bytes(8)),
)}

# Other names of the same games (demo, automatch) share their keys.
ALIASES = {'swbfespspam': 'swbfespsp', 'swbfespspd': 'swbfespsp'}


def find(gamename):
    gamename = (gamename or '').lower()
    return GAMES.get(ALIASES.get(gamename, gamename))
