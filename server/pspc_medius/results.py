# SPDX-License-Identifier: GPL-2.0-or-later
"""The SVO game service: how ranked games are created and their results reported.

Ranked games only (unranked ones send nothing but the mission vote). When a ranked match starts the
host fetches createGameURI (printf'd with the game mode), gets a <Create_Game> form listing fields,
then submits them to the form's action and expects a game id back; without it the match doesn't
start ("Failed to launch game"). At the end every player posts their own results to finishGameURI,
then MissionPostStatsURI (team modes) and MissionVoteCastURI, as form POSTs; each expects a reply
with <id>N</id>, N >= 0. Formats from the game (create state machine 0x08d95fb4, result posts
0x08a27610, reply parser 0x08da9d68).
"""

import logging

from .http import Response

log = logging.getLogger('medius.results')

# The fields the game knows how to fill in a Create_Game form (others stall it).
CREATE_FIELDS = ('gameMode', 'scertGameID', 'gameName', 'hostPlayerList', 'remotePlayerList', 'timePerDiv',
                 'numDivs', 'locationID')
# gameMode values in these requests: the game's mode + 8
GAME_MODES = {8: 'Team Deathmatch', 9: 'Retrieval', 10: 'Sabotage', 11: 'Rogue Agent', 12: 'Deathmatch',
              15: 'Single Retrieval', 16: 'Reversed Retrieval', 17: 'Incursion', 18: 'Recovery'}


def svml(body):
    return Response(200, '<?xml version="1.0" encoding="UTF-8"?>\n<SVML>\n%s\n</SVML>\n' % body, 'text/svml')


def ok(message='OK', **extra):
    """The reply score posts expect: the first <id> must parse to a number >= 0."""
    more = ''.join('<%s>%s</%s>' % (k, v, k) for k, v in extra.items())
    return Response(200, '<?xml version="1.0" encoding="UTF-8"?>\n<XML><status><id>0</id><message>%s</message></status>'
                         '%s</XML>\n' % (message, more), 'text/xml')


def _int(q, name, default=0):
    try:
        return int(q.get(name, default))
    except (TypeError, ValueError):
        return default


class GameService:
    def __init__(self, server, svo):
        self.server = server
        self.svo = svo

    @property
    def store(self):
        return self.server.store

    def routes(self):
        return {'game/Create': self.create, 'game/CreateSubmit': self.create_submit, 'game/Finish': self.finish,
                'game/PostBinaryStats': self.binary_stats}

    def create(self, req):
        """Step 1: the form. The submit URL keeps no query of its own (the game appends the fields)."""
        fields = ''.join('<FIELD name="%s"/>' % f for f in CREATE_FIELDS)
        return svml('<Create_Game action="%s">%s</Create_Game>' % (self.svo.uri('game/CreateSubmit'), fields))

    def create_submit(self, req):
        q = req.query
        players = q.get('remotePlayerList', '')
        game_id = self.store.create_svo_game(_int(q, 'scertGameID'), _int(q, 'gameMode'), q.get('gameName', '')[:64],
                                             _int(q, 'hostPlayerList'), players)
        log.info('results: ranked game %d: %s, %s, host %s, players %s', game_id, q.get('gameName'),
                 GAME_MODES.get(_int(q, 'gameMode'), q.get('gameMode')), q.get('hostPlayerList'), players or '-')
        return svml('<Create_Game><status><id>0</id><message>GAME_CREATE_SUCCESS</message></status>'
                    '<gameID>%d</gameID></Create_Game>' % game_id)

    def player_of(self, req, game):
        """Result posts carry no account id: the SVO login cookie says who it is, else the one player
        of the game connecting from that address."""
        account = self.svo.missions.account_of(req)
        if account or game is None:
            return account
        ids = [int(x) for x in ([str(game['host_id'])] + game['players'].split(',')) if x.strip().lstrip('-').isdigit()]
        here = [p.account_id for p in self.server.lobby.by_account.values()
                if p.account_id in ids and p.lobby is not None and p.lobby.peer[0] == req.peer[0]]
        return here[0] if len(here) == 1 else 0

    def finish(self, req):
        q = req.query
        game_id = _int(q, 'SVOGameID', -1)
        game = self.store.svo_game(game_id) if game_id > 0 else None
        account = self.player_of(req, game)
        log.info('results: game %d finished for %d: %s, result %s, kills %s, deaths %s (end status %s)', game_id,
                 account, GAME_MODES.get(_int(q, 'gameMode'), q.get('gameMode')), q.get('gameResult'),
                 q.get('totalKills'), q.get('deaths'), q.get('endStatus'))
        if game is not None:
            self.store.add_svo_result(game_id, account, q, req.target.partition('?')[2] or req.body.decode('latin-1'))
        else:
            self.store.add_game_report(account, 'Finish', req.target.partition('?')[2], req.body)
        return ok('GAME_FINISH_SUCCESS')

    def binary_stats(self, req):
        # The game never posts binary stats in this version; kept so the URI answers.
        self.store.add_game_report(self.svo.missions.account_of(req), 'PostBinaryStats', req.target.partition('?')[2],
                                   req.body)
        return ok()
