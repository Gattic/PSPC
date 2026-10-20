# SPDX-License-Identifier: GPL-2.0-or-later
"""Players' career totals (for the leaderboards), from what hosts report while they play.

Elite Squadron uploads no player stats of its own. Its hosts' heartbeats (qr.py) name each player
(player_, their GameSpy unique nick) with their score, deaths and team (team_: 0 and 1 are
team_t0 and team_t1); at the end of a round the host's stats connection reports it (updgame, with
score_t0/score_t1 1 for the winning team). So a round's totals are the last heartbeat's numbers,
and time online is how long each player was in the heartbeats. A round also ends when the hosted
game goes away, or when the scores start again from nothing (a new round without a report); those
count no win or loss. The game has no separate kill count, so kills are its score.

Totals kept per player (store.player_stats): time (seconds), kills, deaths, points, wins, losses,
games."""

import logging
import time

from . import access

log = logging.getLogger('gamespy')

MAX_GAP = 90                # seconds between heartbeats counted as time online at most
MIN_ROUND_TIME = 60         # a round nobody scored in counts only if it lasted this long


def _int(v):
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return 0


class Round:
    def __init__(self, host):
        self.host = host
        self.players = {}           # name -> {'score', 'deaths', 'team', 'time'}
        self.last = None            # when the last heartbeat came
        self.present = set()        # names in the last heartbeat

    def empty(self):
        return not any(e['score'] or e['deaths'] for e in self.players.values()) and \
            max((e['time'] for e in self.players.values()), default=0) < MIN_ROUND_TIME


class Careers:
    def __init__(self, store):
        self.store = store
        self.rounds = {}            # host id -> Round

    # ---- the registry's hosted games (a watcher, see qr.Registry)

    def host_added(self, host):
        self.observe(host)

    def host_updated(self, host):
        self.observe(host)

    def host_removed(self, host):
        r = self.rounds.pop(host.id, None)
        if r:
            self.commit(r, None, 'the game closed')

    def observe(self, host, now=None):
        now = time.monotonic() if now is None else now
        r = self.rounds.get(host.id)
        if r is None:
            r = self.rounds[host.id] = Round(host)
        seen = {}
        for p in host.players:
            name = p.get('player_', '').strip()
            if name:
                team = p.get('team_', '')
                seen[name] = (_int(p.get('score_')), _int(p.get('deaths_')), _int(team) if team != '' else -1)
        # Everyone's numbers went down: a new round started without a report.
        both = [n for n in r.players if n in seen]
        before = sum(r.players[n]['score'] + r.players[n]['deaths'] for n in both)
        after = sum(seen[n][0] + seen[n][1] for n in both)
        if before > 0 and after < before and all(seen[n][0] <= r.players[n]['score'] and seen[n][1] <= r.players[n]['deaths']
                                                 for n in both):
            self.commit(r, None, 'a new round started')
            r = self.rounds[host.id] = Round(host)
        dt = min(now - r.last, MAX_GAP) if r.last is not None else 0
        for name, (score, deaths, team) in seen.items():
            e = r.players.setdefault(name, {'score': 0, 'deaths': 0, 'team': -1, 'time': 0})
            if name in r.present:
                e['time'] += dt
            e['score'], e['deaths'] = score, deaths
            if team in (0, 1):
                e['team'] = team
        r.present = set(seen)
        r.last = now

    # ---- a round's end, as the host's stats connection reports it

    def round_ended(self, gamename, host_ip, host_profileid, gamedata):
        """updgame with done=1 from a host: finds its hosted game, counts the round with its winners."""
        r = self.find_round(host_ip, host_profileid)
        if r is None:
            log.info('careers: a round ended on %s, but no hosted game from there is known', host_ip)
            return
        winner = None
        for team in (0, 1):
            if gamedata.get('score_t%d' % team) == '1':
                winner = team
        self.commit(r, winner, 'round over')
        self.rounds[r.host.id] = Round(r.host)

    def find_round(self, host_ip, host_profileid):
        candidates = [r for r in self.rounds.values() if r.host.ip == host_ip]
        if host_profileid:
            p = self.store.profile(host_profileid)
            names = {p['uniquenick'].lower(), p['nick'].lower()} if p else set()
            mine = [r for r in self.rounds.values() if names & {n.lower() for n in r.players}]
            if len(mine) == 1:
                return mine[0]
            candidates = [r for r in mine if r.host.ip == host_ip] or candidates
        return candidates[0] if len(candidates) == 1 else None

    # ---- counting

    def profile_for(self, name, gamename):
        from . import games
        g = games.find(gamename)
        p = self.store.profile_by_uniquenick(name, g.namespace_id if g else 0)
        if p is None:
            rows = self.store.q('SELECT profileid FROM profiles WHERE (uniquenick = ? OR nick = ?) AND deleted = 0', (name, name))
            p = self.store.profile(rows[0]['profileid']) if len(rows) == 1 else None
        return p

    def commit(self, r, winner, why):
        if r.empty():
            return
        gamename = r.host.gamename
        counted = []
        for name, e in r.players.items():
            p = self.profile_for(name, gamename)
            if p is None:
                continue
            values = {'time': e['time'], 'kills': e['score'], 'points': e['score'], 'deaths': e['deaths'], 'games': 1}
            if winner is not None and e['team'] in (0, 1):
                values['wins' if e['team'] == winner else 'losses'] = 1
            self.store.add_stats(p['profileid'], gamename, values)
            counted.append('%s %d/%d%s' % (name, e['score'], e['deaths'],
                                           '' if winner is None or e['team'] not in (0, 1) else ' won' if e['team'] == winner else ' lost'))
        if counted:
            log.info('careers: %s on %s (%s): %s', access.safe(r.host.keys.get('mapname', '?')), access.safe(r.host.name()), why,
                     access.safe(', '.join(counted), 300))
