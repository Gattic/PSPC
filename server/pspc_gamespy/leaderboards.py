# SPDX-License-Identifier: GPL-2.0-or-later
"""Elite Squadron's leaderboards (sdkdev.gamespy.com/games/swbfespsp/web/bfes_score.asp), from the
players' career totals this server keeps (careers.py).

The game asks ?version=0&sortcol=<S>&col=0&col=13 with &offset=<O>&numrows=5 (a page) or &p=<profile
id> (the player's own row). Columns: 0 time online (seconds), 1 kills, 2 deaths, 3 kill:death ratio,
9 wins, 10 losses, 11 win:loss ratio, 12 overall score, 13 rank, 14 points won; its boards sort by
12, 3, 11 and 14.

The reply is text: a header line "BFESScore\\t\\n", then one line per row of 7 fields, each ended by a
tab, the line by \\n alone: [profile id][position][name][(unused)][the sort column][the first col=]
[the second col=]. The game overwrites the body's last byte with a NUL before reading it, so one
extra byte follows the last line. A page with no rows at offset 0 reads as an error (the game
retries forever), so an empty board has a placeholder row; the player's own query answers just the
header when they aren't ranked."""

import logging
import time

from . import games

log = logging.getLogger('gamespy')

TIME, KILLS, DEATHS, KD, WINS, LOSSES, WL, OVERALL, RANK, POINTS = 0, 1, 2, 3, 9, 10, 11, 12, 13, 14
RATIOS = (KD, WL)
SORTABLE = (TIME, KILLS, DEATHS, KD, WINS, LOSSES, WL, OVERALL, RANK, POINTS)
MAX_ROWS = 50
MAX_AGE = 60                    # seconds the rankings are kept when nothing here changed them (an admin may ban from outside)


def install(web, store):
    """Every known game whose leaderboards are in this format (Elite Squadron's and games like it) gets its pages."""
    for g in games.GAMES.values():
        if g.leaderboard == 'bfes':
            page = Leaderboard(store, g.gamename)
            for path in ('/games/%s/web/bfes_score.asp' % g.gamename, '/%s/bfes_score.asp' % g.gamename):
                web.add(path, page.handle)


def career_columns(totals):
    """A player's leaderboard columns from their career totals (careers.py's keys)."""
    kills, deaths = totals.get('kills', 0), totals.get('deaths', 0)
    wins, losses = totals.get('wins', 0), totals.get('losses', 0)
    points = totals.get('points', 0)
    overall = points + 50 * wins + 10 * kills
    return {TIME: totals.get('time', 0), KILLS: kills, DEATHS: deaths, KD: kills / deaths if deaths else kills,
            WINS: wins, LOSSES: losses, WL: wins / losses if losses else wins, OVERALL: overall,
            RANK: rank_for(overall), POINTS: points}


def rank_for(overall):
    """Rank levels: 1 to start, one more at 500, 1500, 3000, ... (500 x the triangular numbers), up to 99."""
    rank, need, step = 1, 500, 500
    while overall >= need and rank < 99:
        rank += 1
        step += 500
        need += step
    return rank


def fmt(col, value):
    return '%.3f' % value if col in RATIOS else '%d' % int(round(value))


class Leaderboard:
    def __init__(self, store, gamename):
        self.store = store
        self.gamename = gamename
        self.cache = {}                 # sort column -> the ranking
        self.cached = (None, 0.0)       # the store's stats version and the time the cache was made

    def ranked(self, sortcol):
        """[(profile id, name, columns)] best first: players who finished at least one game. Anyone can
        ask for a page, so the ranking is kept until the stats change (or MAX_AGE passes)."""
        now = time.monotonic()
        if self.cached[0] != self.store.stats_version or now - self.cached[1] > MAX_AGE:
            self.cache = {}
            self.cached = (self.store.stats_version, now)
        sortcol = sortcol if sortcol in SORTABLE else OVERALL
        if sortcol not in self.cache:
            self.cache[sortcol] = self._rank(sortcol)
        return self.cache[sortcol]

    def _rank(self, sortcol):
        totals, names = {}, {}
        for r in self.store.q('SELECT s.profileid, s.key, s.value, p.nick, p.uniquenick FROM player_stats s '
                              'JOIN profiles p ON p.profileid = s.profileid JOIN users u ON u.userid = p.userid '
                              'WHERE s.gamename = ? AND p.deleted = 0 AND u.banned = 0', (self.gamename,)):
            totals.setdefault(r['profileid'], {})[r['key']] = r['value']
            names[r['profileid']] = r['uniquenick'] or r['nick']
        rows = [(pid, names[pid], career_columns(t)) for pid, t in totals.items() if t.get('games')]
        rows.sort(key=lambda r: (-r[2].get(sortcol, 0), r[0]))
        return rows

    def handle(self, req):
        q = req.query_all
        try:
            sortcol = int(q.get('sortcol', ['12'])[0])
            cols = [int(c) for c in q.get('col', ['0', '13'])][:2]
            offset = int(q.get('offset', ['0'])[0])
            numrows = min(max(int(q.get('numrows', ['5'])[0]), 1), MAX_ROWS)
            pid = int(q['p'][0]) if 'p' in q else None
        except (ValueError, IndexError):
            return 400, 'BFESScore\t\n\n', 'text/plain'
        while len(cols) < 2:
            cols.append(RANK)
        ranked = self.ranked(sortcol)
        lines = ['BFESScore\t']
        if pid is not None:
            for pos, row in enumerate(ranked, 1):
                if row[0] == pid:
                    lines.append(self.line(pos, row, sortcol, cols))
                    break
        else:
            page = ranked[offset:offset + numrows]
            if not page and offset == 0:
                lines.append('0\t-\tNo ranked players yet\t\t0\t0\t0\t')
            for pos, row in enumerate(page, offset + 1):
                lines.append(self.line(pos, row, sortcol, cols))
        body = '\n'.join(lines) + '\n\n'
        log.debug('leaderboard sortcol %d %s: %d rows', sortcol, 'p=%d' % pid if pid is not None else 'offset %d' % offset,
                  len(lines) - 1)
        return 200, body, 'text/plain'

    @staticmethod
    def line(pos, row, sortcol, cols):
        pid, name, c = row
        name = ''.join(ch for ch in name if ch not in '\t\r\n')[:30]
        return '%d\t%d\t%s\t\t%s\t%s\t%s\t' % (pid, pos, name, fmt(sortcol, c.get(sortcol, 0)),
                                             fmt(cols[0], c.get(cols[0], 0)), fmt(cols[1], c.get(cols[1], 0)))
