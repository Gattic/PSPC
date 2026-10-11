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

log = logging.getLogger('gamespy')

TIME, KILLS, DEATHS, KD, WINS, LOSSES, WL, OVERALL, RANK, POINTS = 0, 1, 2, 3, 9, 10, 11, 12, 13, 14
RATIOS = (KD, WL)
MAX_ROWS = 50


def install(web, store):
    page = Leaderboard(store, 'swbfespsp')
    for path in ('/games/swbfespsp/web/bfes_score.asp', '/swbfespsp/bfes_score.asp'):
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

    def ranked(self, sortcol):
        """[(profile id, name, columns)] best first: players who finished at least one game."""
        totals = {}
        for r in self.store.q('SELECT s.profileid, s.key, s.value FROM player_stats s WHERE s.gamename = ?', (self.gamename,)):
            totals.setdefault(r['profileid'], {})[r['key']] = r['value']
        rows = []
        for pid, t in totals.items():
            if not t.get('games'):
                continue
            p = self.store.profile(pid)
            if p is None or p['banned']:
                continue
            rows.append((pid, p['uniquenick'] or p['nick'], career_columns(t)))
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
