# SPDX-License-Identifier: GPL-2.0-or-later
"""Combat Ops' mission server: sharing missions made with the Mission Modifier (upload, browse,
download, rate, favorites) and cell (clan) statistics, over HTTP.

The game sends GET requests with the parameters in the query (formats taken from the game) and reads
XML replies: <SpyWars type="..."> with a <TransactionID> echoing the request's, then per type:

  MissionListGet / MissionVoteGetTopList   <Mission> records (see mission_xml)
  MissionUpload                            UploadServlet="url" attribute, <GeneratedUniqueFileName>
  MissionGetRaw                            DownloadServlet, fileID, fileNameBeginsWith attributes
  MissionGetID                             <Mission><MissionID>
  MissionGetMetadata                       one <Mission>
  MissionVoteGet                           <Rating> (-9999: not rated)
  MissionInitMetadata, MissionListAdd/Remove   just the TransactionID
  CellGetStats                             <ClanName>, <Sabotage|Retrieval|TeamDeathmatch><PlayCount><WinCount>

Mission files themselves go through the upload/download servlets (SVO file services).
"""

import html
import logging
import secrets
import time

from . import results, umd
from .http import Response

log = logging.getLogger('medius.missions')

NOT_RATED = -9999                       # MissionVoteGet: this player hasn't rated it
VOTE_NONE, VOTE_INAPPROPRIATE = -1, -2  # MissionVoteCast ratings besides 1-5
# MissionListType: which list a query is for. Lists the game keeps per player (it adds to them with
# MissionListAdd) are stored as such; the others are computed.
LIST_OFFICIAL = 3


def esc(value):
    return html.escape(str(value), quote=True)


def spywars(kind, transaction, body='', attrs=None):
    a = ''.join(' %s="%s"' % (k, esc(v)) for k, v in (attrs or {}).items())
    # The game parses replies like SVO pages: an <XML> document whose <SpyWars> tag a tag module handles.
    return Response(200, '<?xml version="1.0" encoding="UTF-8"?>\n<XML>\n<SpyWars type="%s"%s>\n<TransactionID>%s'
                    '</TransactionID>\n%s</SpyWars>\n</XML>\n' % (kind, a, esc(transaction), body), 'text/xml; charset=UTF-8')


def uploaded_file(req):
    """An upload's file: the body, or the file part of a multipart/form-data body."""
    ctype = req.headers.get('content-type', '')
    if ctype.lower().startswith('multipart/form-data') and 'boundary=' in ctype:
        boundary = ctype.split('boundary=', 1)[1].split(';', 1)[0].strip().strip('"').encode('latin-1')
        parts = req.body.split(b'--' + boundary)
        found = [p.partition(b'\r\n\r\n') for p in parts[1:]]
        files = [data for head, sep, data in found if sep and b'filename=' in head.lower()]
        for data in files or [data for head, sep, data in found if sep]:
            return data[:-2] if data.endswith(b'\r\n') else data
    return req.body


def _int(q, name, default=0):
    try:
        return int(q.get(name, default))
    except (TypeError, ValueError):
        return default


class MissionServer:
    def __init__(self, server):
        self.server = server

    @property
    def store(self):
        return self.server.store

    def routes(self):
        return {
            'mission/Submit': self.submit,
            'mission/GetRaw': self.get_raw,
            'mission/GetMetadata': self.get_metadata,
            'mission/InitMetadata': self.init_metadata,
            'mission/GetID': self.get_id,
            'mission/VoteGet': self.vote_get,
            'mission/VoteCast': self.vote_cast,
            'mission/ListGet': self.list_get,
            'mission/ListGetPopular': self.list_popular,
            'mission/ListAdd': self.list_add,
            'mission/ListRemove': self.list_remove,
            'mission/PostStats': self.post_stats,
            'cell/GetStats': self.cell_stats,
            'files/Upload': self.upload,
            'files/Download': self.download,
        }

    # ---- records

    def mission_xml(self, m, account_id=0):
        votes = self.store.q('SELECT COUNT(*) AS n, AVG(vote) AS avg FROM mission_votes WHERE mission_id = ? AND vote > 0',
                             (m['mission_id'],))[0]
        fields = [
            ('MissionID', m['mission_id']), ('MissionName', m['name']), ('Author', m['author']),
            ('MissionHash', m['hash']), ('Theme', m['theme']), ('CombatType', m['combat_type']),
            ('Terrain', m['terrain']), ('TimeOfDay', m['time_of_day']), ('Weather', m['weather']),
            ('fileRatingNumberVotes', votes['n'] or 0), ('fileRatingAverageRating', '%.2f' % (votes['avg'] or 0)),
            ('Inappropriate', m['inappropriate']), ('IsInappropriate', 1 if m['hidden'] else 0),
            ('fileDownloadCount', m['downloads']), ('Plays', m['plays']),
            ('AvgPlayTime', m['play_time'] // m['plays'] if m['plays'] else 0),
            ('TeamAWins', m['team_a_wins']), ('TeamBWins', m['team_b_wins']),
        ]
        return '<Mission>%s</Mission>\n' % ''.join('<%s>%s</%s>' % (k, esc(v), k) for k, v in fields)

    def account_of(self, req):
        """The requesting player's account (the SVO login's AcctID cookie), else the AccountID parameter."""
        cookies = dict(c.strip().split('=', 1) for c in req.headers.get('cookie', '').split(';') if '=' in c)
        try:
            return int(cookies.get('AcctID') or req.query.get('AccountID') or 0)
        except ValueError:
            return 0

    def mission(self, mission_id):
        return self.store.one('SELECT * FROM missions WHERE mission_id = ?', (mission_id,))

    # ---- lists

    def list_get(self, req):
        q = req.query
        list_type = _int(q, 'MissionListType')
        account = self.account_of(req)
        if list_type == LIST_OFFICIAL:
            rows = self.store.q('SELECT * FROM missions WHERE official = 1 AND hidden = 0 ORDER BY name LIMIT 100')
        else:
            rows = self.store.q('SELECT m.* FROM mission_lists l JOIN missions m ON m.mission_id = l.mission_id '
                                'WHERE l.account_id = ? AND l.list_type = ? AND m.hidden = 0 ORDER BY l.added DESC LIMIT 100',
                                (account, list_type))
            if not rows and list_type not in self.player_list_types():
                # lists the game never adds to: the newest uploads
                rows = self.store.q('SELECT * FROM missions WHERE hidden = 0 ORDER BY created DESC LIMIT 100')
        log.info('missions: list %d for %d: %d missions', list_type, account, len(rows))
        return spywars('MissionListGet', q.get('TransactionID', 0), ''.join(self.mission_xml(m) for m in rows))

    def player_list_types(self):
        return {r['list_type'] for r in self.store.q('SELECT DISTINCT list_type FROM mission_lists')}

    def list_popular(self, req):
        q = req.query
        size = min(max(_int(q, 'Size', 25), 1), 100)
        rows = self.store.q('SELECT m.*, (SELECT AVG(vote) FROM mission_votes v WHERE v.mission_id = m.mission_id AND '
                            'vote > 0) AS rating FROM missions m WHERE hidden = 0 ORDER BY rating DESC, downloads DESC '
                            'LIMIT ?', (size,))
        return spywars('MissionVoteGetTopList', q.get('TransactionID', 0), ''.join(self.mission_xml(m) for m in rows))

    def list_add(self, req):
        q = req.query
        self.store.x('INSERT OR REPLACE INTO mission_lists (account_id, list_type, mission_id, added) VALUES (?, ?, ?, ?)',
                     (self.account_of(req), _int(q, 'MissionListType'), _int(q, 'MissionID'), int(time.time())))
        return spywars('MissionListAdd', q.get('TransactionID', 0))

    def list_remove(self, req):
        q = req.query
        self.store.x('DELETE FROM mission_lists WHERE account_id = ? AND list_type = ? AND mission_id = ?',
                     (self.account_of(req), _int(q, 'MissionListType'), _int(q, 'MissionID')))
        return spywars('MissionListRemove', q.get('TransactionID', 0))

    # ---- one mission

    def get_metadata(self, req):
        q = req.query
        m = self.mission(_int(q, 'MissionID', -1))
        return spywars('MissionGetMetadata', q.get('TransactionID', 0), self.mission_xml(m) if m else '')

    def get_id(self, req):
        q = req.query
        m = self.store.one('SELECT mission_id FROM missions WHERE hash = ?', (q.get('MissionHash', ''),))
        return spywars('MissionGetID', q.get('TransactionID', 0),
                       '<Mission><MissionID>%d</MissionID></Mission>\n' % (m['mission_id'] if m else -1))

    def vote_get(self, req):
        q = req.query
        v = self.store.one('SELECT vote FROM mission_votes WHERE account_id = ? AND mission_id = ?',
                           (self.account_of(req), _int(q, 'MissionID')))
        return spywars('MissionVoteGet', q.get('TransactionID', 0), '<Rating>%d</Rating>\n' % (v['vote'] if v else NOT_RATED))

    def match_key(self, req, account):
        """Which match a post-match report is about: the ranked game's SVO id, else the lobby game
        the player is in and its match number."""
        game_id = _int(req.query, 'SVOGameID', -1)
        if game_id > 0:
            return 'svo:%d' % game_id
        p = self.server.lobby.find_player(account_id=account)
        if p is not None and p.game is not None:
            return 'game:%s:%d:%d' % (self.server.records.run, p.game.id, p.game.match)
        return None

    def vote_cast(self, req):
        """After every match (ranked or not), each player rates the mission: Rating 1-5, -1 = no vote,
        -2 = inappropriate. Counts the mission as played, once per match."""
        q = req.query
        mission_id = _int(q, 'MissionID')
        rating = _int(q, 'Rating', VOTE_NONE)
        account = _int(q, 'AccountID') or self.account_of(req)
        if 1 <= rating <= 5 or rating == VOTE_INAPPROPRIATE:
            self.store.x('INSERT OR REPLACE INTO mission_votes (account_id, mission_id, vote) VALUES (?, ?, ?)',
                         (account, mission_id, rating))
            if rating == VOTE_INAPPROPRIATE:
                self.store.x('UPDATE missions SET inappropriate = (SELECT COUNT(*) FROM mission_votes WHERE mission_id = ? '
                             'AND vote = ?) WHERE mission_id = ?', (mission_id, VOTE_INAPPROPRIATE, mission_id))
        key = self.match_key(req, account)
        if key is not None and self.store.first_report(key, mission_id):
            self.store.x('UPDATE missions SET plays = plays + 1 WHERE mission_id = ?', (mission_id,))
        log.info('missions: %d rated mission %d: %d (%s, match %s)', account, mission_id, rating,
                 results.GAME_MODES.get(_int(q, 'gameMode'), q.get('gameMode')), key)
        return results.ok()

    def post_stats(self, req):
        """After a ranked team match: PlayTime (minutes) and WinningTeam (0 draw, 1 or 2), from every
        player; counted once per game."""
        q = req.query
        mission_id = _int(q, 'MissionID')
        game_id = _int(q, 'SVOGameID', -1)
        winner = _int(q, 'WinningTeam')
        log.info('missions: game %d on mission %d: %s minutes, winning team %s', game_id, mission_id, q.get('PlayTime'),
                 winner)
        if game_id > 0 and mission_id > 0 and self.store.first_report('stats:svo:%d' % game_id, mission_id):
            self.store.x('UPDATE missions SET play_time = play_time + ?, team_a_wins = team_a_wins + ?, '
                         'team_b_wins = team_b_wins + ? WHERE mission_id = ?',
                         (_int(q, 'PlayTime'), 1 if winner == 1 else 0, 1 if winner == 2 else 0, mission_id))
        return results.ok()

    # ---- uploading and downloading missions

    def submit(self, req):
        """A new mission: record it, and tell the game where to upload the file."""
        q = req.query
        account = self.account_of(req)
        mhash = q.get('MissionHash', '')[:32]
        existing = self.store.one('SELECT * FROM missions WHERE hash = ?', (mhash,))
        file_id = existing['file_id'] if existing else secrets.token_hex(8)
        if existing is None:
            self.store.x('INSERT INTO missions (hash, account_id, name, author, theme, combat_type, terrain, file_id, created) '
                         'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)',
                         (mhash, account, q.get('MissionName', '')[:64], q.get('Author', '')[:32], _int(q, 'Theme'),
                          _int(q, 'CombatType'), _int(q, 'Terrain'), file_id, int(time.time())))
        log.info('missions: %d submits %r (hash %s, file %s)', account, q.get('MissionName'), mhash, file_id)
        # The game swaps SPYWARSGPS_XML for SPYWARSGPS_SVML in this URL and appends &filePermission=...
        url = self.server.svo.uri('files/Upload?fileID=%s' % file_id).replace('SPYWARSGPS_SVML', 'SPYWARSGPS_XML')
        return spywars('MissionUpload', q.get('TransactionID', 0),
                       '<GeneratedUniqueFileName>%s</GeneratedUniqueFileName>\n' % file_id, {'UploadServlet': url})

    def init_metadata(self, req):
        q = req.query
        self.store.x('UPDATE missions SET name = ?, author = ?, theme = ?, combat_type = ?, terrain = ?, time_of_day = ?, '
                     'weather = ? WHERE mission_id = ? OR hash = ?',
                     (q.get('MissionName', '')[:64], q.get('Author', '')[:32], _int(q, 'Theme'), _int(q, 'CombatType'),
                      _int(q, 'Terrain'), _int(q, 'TimeOfDay'), _int(q, 'Weather'), _int(q, 'MissionID', -1),
                      q.get('MissionHash', '')))
        return spywars('MissionInitMetadata', q.get('TransactionID', 0))

    def get_raw(self, req):
        q = req.query
        m = self.mission(_int(q, 'MissionID', -1))
        if m is None:
            return spywars('MissionGetRaw', q.get('TransactionID', 0))
        if _int(q, 'IncrementDownloadCount'):
            self.store.x('UPDATE missions SET downloads = downloads + 1 WHERE mission_id = ?', (m['mission_id'],))
        url = self.server.svo.uri('files/Download').replace('SPYWARSGPS_SVML', 'SPYWARSGPS_XML')
        return spywars('MissionGetRaw', q.get('TransactionID', 0),
                       '<GeneratedUniqueFileName>%s</GeneratedUniqueFileName>\n' % m['file_id'],
                       {'DownloadServlet': url, 'fileID': m['file_id'], 'fileNameBeginsWith': m['file_id']})

    def upload(self, req):
        file_id = req.query.get('fileID', '')
        data = uploaded_file(req)
        info = umd.mission_info(data)
        log.info('missions: upload %s: %d bytes (%s), query %s, type %s', file_id, len(data),
                 'mission %r' % info['name'] if info else 'not a mission file', req.query, req.headers.get('content-type'))
        if not file_id:
            return Response(400, 'no fileID')
        if info is not None:
            data = info['data']
            # what the file says is more complete than the submit request (time of day, weather)
            self.store.x('UPDATE missions SET theme = ?, combat_type = ?, terrain = ?, time_of_day = ?, weather = ? '
                         'WHERE file_id = ?', (info['theme'], info['combat_type'], info['terrain'], info['time_of_day'],
                                              info['weather'], file_id))
        self.store.x('INSERT OR REPLACE INTO blobs (file_id, owner_id, name, data, created) VALUES (?, ?, ?, ?, ?)',
                     (file_id, self.account_of(req), file_id, data, int(time.time())))
        return Response(200, '<?xml version="1.0" encoding="UTF-8"?>\n<XML><Status>OK</Status></XML>\n', 'text/xml')

    def download(self, req):
        file_id = req.query.get('fileID', '')
        row = self.store.one('SELECT data FROM blobs WHERE file_id = ?', (file_id,))
        log.info('missions: download %s: %s', file_id, 'found' if row else 'missing')
        if row is None:
            return Response(404, 'no such file')
        return Response(200, bytes(row['data']), 'application/octet-stream')

    # ---- cells

    def cell_stats(self, req):
        q = req.query
        clan_id = _int(q, 'CellD', _int(q, 'CellID'))
        c = self.store.clan(clan_id)
        results = self.store.cell_results(clan_id)      # records.py counts them from members' ranked games
        body = '<ClanName>%s</ClanName>\n' % esc(c['name'] if c else '')
        for tag, mode in (('Sabotage', 'SAB'), ('Retrieval', 'RET'), ('TeamDeathmatch', 'TDM')):
            games, wins = results.get(mode, (0, 0))
            body += '<%s><PlayCount>%d</PlayCount><WinCount>%d</WinCount></%s>\n' % (tag, games, wins or 0, tag)
        return spywars('CellGetStats', q.get('TransactionID', 0), body)
