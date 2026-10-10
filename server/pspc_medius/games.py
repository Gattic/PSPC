# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-game settings: what each game expects from its universe."""


class GameConfig:
    name = ''
    app_ids = ()
    service_ids = ()            # NP service IDs its tickets are issued for
    universe_name = 'PSPC'
    universe_description = 'PSPC game server'
    news = ''
    max_users = 1024
    channels = [{'name': 'Lobby'}]
    usage_policy = ''
    privacy_policy = ''
    svo_path = '/'              # the SVO (SVML portal) URL's path
    max_game_players = 8
    files = {}                  # name -> text served by the lobby's file services

    def policy(self, which):
        return self.privacy_policy if which == 1 else self.usage_policy


class CombatOps(GameConfig):
    """Syphon Filter: Combat Ops (NPUG80114, app ID 21064; internal name "Spy Wars")."""
    name = 'Syphon Filter: Combat Ops'
    app_ids = (21064,)
    service_ids = ('UP9000-NPUG80114_00',)
    universe_name = 'Combat Ops (PSPC)'
    universe_description = 'Syphon Filter: Combat Ops on a PSPC game server'
    news = 'Welcome back to Combat Ops!'
    channels = [
        {'name': 'Combat Ops', 'max_players': 256},
    ]
    usage_policy = ('This is a community game server for Syphon Filter: Combat Ops run with PSPC. '
                    'Play fair and be respectful to other players. The server operator may remove players '
                    'who cheat or abuse others.')
    privacy_policy = ('This server stores your online ID, your game statistics, your buddy, ignore and cell '
                      'lists, missions you share, and the network address you connect from while you play. '
                      'It does not store passwords.')
    svo_path = '/SPYWARSGPS_SVML/'
    max_game_players = 8
    # Files the lobby hands out (Medius file services). LobbySettings.ini: timeouts (the 18
    # FETCH_*/JOINCHANNEL_*/USER_REFRESH_*/POSTJOIN_* keys; unset = the game's defaults),
    # CHANNEL_GAME_FILTER (only list games of the joined channel) and RANK_MULTIPLIER.
    files = {
        'LobbySettings.ini': (
            '; Syphon Filter: Combat Ops lobby settings (PSPC game server)\r\n'
            '[Lobby]\r\n'
            'CHANNEL_GAME_FILTER = 0\r\n'
            'RANK_MULTIPLIER = 2.5\r\n'),
    }


GAMES = {'sfco': CombatOps}


def by_app_id(app_id):
    for g in GAMES.values():
        if app_id in g.app_ids:
            return g
    return None
