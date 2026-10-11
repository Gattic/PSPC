# SPDX-License-Identifier: GPL-2.0-or-later
"""Leaderboards and cell (clan) results, from the statistics records the game stores here.

The game itself has no leaderboards; the Community pages (portal.py) show these. Players are ranked
from their decoded records (stats.py); cells by their members' totals and by the matches they won.
A cell's games and wins per mode (the game's cell profile asks for them: CellGetStatsURI) are counted
when a member's record shows a new ranked team-mode game: one game per cell per match.
"""

import logging
import os

from . import stats

log = logging.getLogger('medius.records')

# Player leaderboards: key -> (title, short name, column heading, value of a decoded record)
PLAYER_BOARDS = {
    'xp': ('Top Agents', 'XP', 'XP', lambda s: s['xp']),
    'wins': ('Most Wins', 'Wins', 'Wins', lambda s: s['wins']),
    'kills': ('Most Kills', 'Kills', 'Kills', lambda s: s['kills']),
    'kd': ('Best Kill/Death Ratio', 'K/D', 'K/D', lambda s: round(s['kills'] / max(s['deaths'], 1), 2) if s['kills'] >= 25 else 0),
    'headshots': ('Most Headshots', 'Headshots', 'Headshots', lambda s: s['headshots']),
    'objectives': ('Most Objectives', 'Objectives', 'Objectives', lambda s: s['objectives']),
    'revives': ('Most Revives', 'Revives', 'Revives', lambda s: s['revives']),
}
for _m in stats.MODES:
    PLAYER_BOARDS['won_' + _m] = (stats.MODE_NAMES[_m] + ' Wins', _m + ' wins', 'Wins', lambda s, m=_m: s['won'][m])

# Cell leaderboards
CELL_BOARDS = {
    'wins': ('Top Cells: Match Wins', 'Wins'),
    'xp': ('Top Cells: Member XP', 'XP'),
    'members': ('Biggest Cells', 'Members'),
}
TEAM_MODES = ('RET', 'SAB', 'TDM')       # the modes the cell profile shows


class Records:
    def __init__(self, server):
        self.server = server
        self.run = os.urandom(4).hex()   # tells matches of different server runs apart
        self._cache = (None, None)

    @property
    def store(self):
        return self.server.store

    # ---- players

    def players(self):
        """[(account_id, name, decoded record)] for everyone with a valid record, cached until a
        record changes."""
        # (bans come from the command line, another process: count them)
        key = (self.store.stats_version, self.store.one('SELECT COUNT(*) AS n FROM accounts WHERE banned = 1')['n'])
        if self._cache[0] != key:
            rows = []
            for r in self.store.all_stats(self.server.game.app_ids[0]):
                s = stats.decode_player(r['stats'])
                if s is not None:
                    rows.append((r['account_id'], r['name'], s))
            self._cache = (key, rows)
        return self._cache[1]

    def board(self, key):
        """A player leaderboard: [(position, account_id, name, value, record)], best first. Ties
        are broken by XP, then wins, then kills."""
        title, short, heading, value = PLAYER_BOARDS[key]
        rows = sorted(self.players(), key=lambda r: (-value(r[2]), -r[2]['xp'], -r[2]['wins'], -r[2]['kills'],
                                                      r[1].lower()))
        rows = [r for r in rows if value(r[2]) > 0] if key != 'xp' else [r for r in rows if r[2]['games'] > 0]
        return [(i + 1, a, n, value(s), s) for i, (a, n, s) in enumerate(rows)]

    def player(self, account_id):
        for a, n, s in self.players():
            if a == account_id:
                return s
        return None

    # ---- cells

    def cells(self):
        """[{clan, members, xp, wins, games, results}] for every active cell."""
        totals = {}
        for r in self.store.all_cell_results():
            t = totals.setdefault(r['clan_id'], {})
            t[r['mode']] = (r['games'], r['wins'] or 0)
        out = []
        for c in self.store.q('SELECT * FROM clans'):
            members = self.store.clan_members(c['clan_id'])
            member_ids = {m['account_id'] for m in members}
            records = [s for a, n, s in self.players() if a in member_ids]
            results = totals.get(c['clan_id'], {})
            out.append({'clan': c, 'members': len(members), 'xp': sum(s['xp'] for s in records),
                        'games': sum(g for g, w in results.values()), 'wins': sum(w for g, w in results.values()),
                        'results': results, 'cell': stats.decode_cell(c['stats'])})
        return out

    def cell_board(self, key):
        rows = sorted(self.cells(), key=lambda c: (-c[key], -c['wins'], -c['xp'], c['clan']['name'].lower()))
        return [(i + 1, c) for i, c in enumerate(rows)]

    # ---- recording

    def record_progress(self, account_id, old_blob, new_blob, game):
        """A player's record grew: count new ranked team-mode games for their cell (once per match)."""
        old, new = stats.decode_player(old_blob), stats.decode_player(new_blob)
        if new is None or new['cell_id'] is None:
            return
        for mode in TEAM_MODES:
            before = old['played'][mode] if old else 0
            if new['played'][mode] <= before:
                continue
            won = new['won'][mode] > (old['won'][mode] if old else 0)
            match = '%s:%d:%d' % (self.run, game.id, game.match) if game is not None else \
                '%s:p%d:%d' % (self.run, account_id, new['played'][mode])
            self.store.record_cell_match(new['cell_id'], mode, match, won)
            log.info('records: cell %d %s a %s match (%s)', new['cell_id'], 'won' if won else 'played',
                     stats.MODE_NAMES[mode], match)
