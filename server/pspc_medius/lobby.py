# SPDX-License-Identifier: GPL-2.0-or-later
"""MLS, the Medius Lobby Server: channels, chat, players, games, buddies, ignore lists, clans,
stats and files."""

import asyncio
import logging
import struct

from . import messages as M
from . import stats
from .service import MediusConnection

log = logging.getLogger('medius')


class LobbyServer(MediusConnection):
    service = 'mls'

    def __init__(self, server):
        super().__init__(server)
        self.player = None

    @property
    def lobby(self):
        return self.server.lobby

    def accept_connect(self, world, session, token):
        p = self.lobby.by_access.get(token)
        if p is None:
            log.warning('mls %s: unknown access key %r', self.who(), token)
            return False
        if p.lobby is not None and p.lobby is not self:
            p.lobby.player = None
            p.lobby.close()
        p.lobby = self
        self.player = p
        ch = self.lobby.channels.get(world) or self.lobby.default_channel()
        self.lobby.join_channel(p, ch)
        log.info('mls %s: %s connected (channel %s)', self.who(), p.name, ch.name)
        return True

    def player_id(self):
        return 0x19

    # A client changing channels (JoinChannel) reconnects with the same access key; a player whose
    # lobby connection closed stays signed in this long.
    RECONNECT_GRACE = 20

    def on_close(self):
        if self.player is None:
            return
        p, self.player = self.player, None
        if p.lobby is self:
            p.lobby = None
        log.info('mls %s: %s disconnected', self.who(), p.name)

        def expire():
            if p.lobby is None and self.lobby.by_account.get(p.account_id) is p:
                log.info('mls: %s signed out (did not reconnect)', p.name)
                self.lobby.logout(p)
        asyncio.get_running_loop().call_later(self.RECONNECT_GRACE, expire)

    # ---- players

    def on_PlayerInfo(self, m):
        a = self.server.store.account_by_id(m['AccountID'])
        if a is None:
            self.reply('PlayerInfoResponse', MessageID=m['MessageID'], StatusCode=M.ACCOUNT_NOT_FOUND)
            return
        p = self.lobby.find_player(account_id=a['account_id'])
        status = M.PLAYER_DISCONNECTED if p is None else (M.PLAYER_IN_GAME_WORLD if p.game else M.PLAYER_IN_CHAT_WORLD)
        self.reply('PlayerInfoResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS, AccountName=a['name'],
                   ApplicationID=a['app_id'], PlayerStatus=status, ConnectionClass=0, Stats=a['stats'])

    def on_AccountLogout(self, m):
        self.reply('AccountLogoutResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)
        if self.player is not None:
            p, self.player = self.player, None
            p.lobby = None
            self.lobby.logout(p)

    # ---- files

    def on_FileDownload(self, m):
        name = m['File']['Filename']
        data = self.server.files.read(self.app_id, name)
        if data is None:
            log.info('mls %s: no file %r', self.who(), name)
            self.reply('FileDownloadResponse', MessageID=m['MessageID'], StatusCode=M.FILE_DOES_NOT_EXIST,
                       XferStatus=M.XFER_ERROR)
            return
        chunks = [data[i:i + M.FILE_CHUNK] for i in range(0, len(data), M.FILE_CHUNK)] or [b'']
        for n, chunk in enumerate(chunks):
            last = n == len(chunks) - 1
            self.reply('FileDownloadResponse', MessageID=m['MessageID'], Data=chunk, StartByteIndex=n * M.FILE_CHUNK,
                       DataSize=len(chunk), PacketNumber=n, StatusCode=M.SUCCESS,
                       XferStatus=M.XFER_END if last else (M.XFER_INITIAL if n == 0 else M.XFER_MID))

    # ---- stats

    def on_AccountUpdateStats(self, m):
        self.store_stats(m['Stats'])
        self.reply('AccountUpdateStatsResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    def store_stats(self, blob):
        """Keeps the player's statistics record if it moves forward (stats.progress); counts what it
        adds for their cell."""
        p = self.player
        if p is None:
            return
        account = self.server.store.account_by_id(p.account_id)
        old = bytes(account['stats']) if account else b''
        if bytes(blob) == old:
            return
        if not stats.progress(old, blob):
            new = stats.decode_player(blob)
            if new is None:     # some PlayerReports during a match carry something else than the record
                log.debug('mls %s: %s sent stats that are not a player record; ignored', self.who(), p.name)
            else:
                log.warning('mls %s: kept the stored stats of %s: the new record has less (XP %s -> %s)', self.who(),
                            p.name, (stats.decode_player(old) or {}).get('xp'), new['xp'])
            return
        self.server.store.set_stats(p.account_id, blob)
        self.server.records.record_progress(p.account_id, old, blob, p.game)

    # ---- lists: one response per entry; an empty list is a single NoResult with EndOfList

    def reply_list(self, name, mid, entries):
        if not entries:
            self.reply(name, MessageID=mid, StatusCode=M.NO_RESULT, EndOfList=True)
            return
        for i, e in enumerate(entries):
            self.reply(name, dict(e, MessageID=mid, StatusCode=M.SUCCESS, EndOfList=i == len(entries) - 1))

    @staticmethod
    def page(items, page_id, page_size):
        page_size = page_size or len(items) or 1
        start = (max(page_id, 1) - 1) * page_size
        return items[start:start + page_size]

    # ---- channels

    def _channel_list(self, m):
        channels = self.page(list(self.lobby.channels.values()), m['PageID'], m['PageSize'])
        self.reply_list('ChannelList_ExtraInfoResponse', m['MessageID'], [
            {'MediusWorldID': ch.id, 'PlayerCount': len(ch.players), 'MaxPlayers': ch.max_players,
             'GameWorldCount': len(ch.games(self.lobby)), 'SecurityLevel': 1 if ch.password else 0,
             'GenericField1': ch.generic[0], 'GenericField2': ch.generic[1], 'GenericField3': ch.generic[2],
             'GenericField4': ch.generic[3], 'GenericFieldLevel': ch.generic_level, 'LobbyName': ch.name}
            for ch in channels])

    on_ChannelList_ExtraInfo = on_ChannelList_ExtraInfo1 = on_ChannelList_ExtraInfo0 = _channel_list

    # ---- games

    def game_entry(self, g):
        e = {'MediusWorldID': g.id, 'PlayerCount': g.player_count, 'MinPlayers': g.min_players,
             'MaxPlayers': g.max_players, 'GameLevel': g.game_level, 'PlayerSkillLevel': g.skill,
             'RulesSet': g.rules, 'SecurityLevel': 1 if g.password else 0, 'WorldStatus': g.status,
             'GameHostType': g.host_type, 'GameName': g.name, 'GameStats': g.stats}
        for i in range(8):
            e['GenericField%d' % (i + 1)] = g.generic[i]
        return e

    def visible_games(self):
        games = [g for g in self.lobby.games.values() if g.status in (M.WORLD_STAGING, M.WORLD_ACTIVE, M.WORLD_CLOSED)]
        p = self.player
        if p is not None and p.channel is not None and getattr(p, 'game_filter_channel', False):
            games = [g for g in games if g.channel is p.channel]
        return sorted(games, key=lambda g: g.id)

    def _game_list(self, m):
        games = self.page(self.visible_games(), m['PageID'], m['PageSize'])
        self.reply_list('GameList_ExtraInfoResponse', m['MessageID'], [self.game_entry(g) for g in games])

    on_GameList_ExtraInfo = on_GameList_ExtraInfo0 = _game_list

    # ---- clans ("cells")

    def clan_row(self, c):
        leader = self.server.store.account_by_id(c['leader_id'])
        return {'ClanID': c['clan_id'], 'ApplicationID': c['app_id'], 'ClanName': c['name'],
                'LeaderAccountID': c['leader_id'], 'LeaderAccountName': leader['name'] if leader else '',
                'Stats': c['stats'], 'Status': c['status']}

    def on_GetMyClans(self, m):
        rows = self.server.store.clans_of(self.player.account_id) if self.player else []
        rows = self.page(rows, m['Start'], m['PageSize'])
        self.reply_list('GetMyClansResponse', m['MessageID'], [self.clan_row(c) for c in rows])

    # ---- buddies, ignore list

    def online_state(self, account_id):
        p = self.lobby.find_player(account_id=account_id)
        if p is None:
            return {'ConnectStatus': M.PLAYER_DISCONNECTED}
        return {'ConnectStatus': M.PLAYER_IN_GAME_WORLD if p.game else M.PLAYER_IN_CHAT_WORLD,
                'MediusLobbyWorldID': p.channel.id if p.channel else 0,
                'MediusGameWorldID': p.game.id if p.game else 0,
                'LobbyName': p.channel.name if p.channel else '', 'GameName': p.game.name if p.game else ''}

    def player_status(self, account_id):
        return self.online_state(account_id)['ConnectStatus']

    def on_GetBuddyList_ExtraInfo(self, m):
        rows = self.server.store.buddies(self.player.account_id) if self.player else []
        self.reply_list('GetBuddyList_ExtraInfoResponse', m['MessageID'], [
            {'AccountID': r['account_id'], 'AccountName': r['name'], 'OnlineState': self.online_state(r['account_id'])}
            for r in rows])

    def on_GetIgnoreList(self, m):
        rows = self.server.store.ignores(self.player.account_id) if self.player else []
        self.reply_list('GetIgnoreListResponse', m['MessageID'], [
            {'IgnoreAccountID': r['account_id'], 'IgnoreAccountName': r['name'],
             'PlayerStatus': self.player_status(r['account_id'])} for r in rows])

    # ---- channels

    def on_JoinChannel(self, m):
        ch = self.lobby.channels.get(m['MediusWorldID'])
        if ch is None:
            self.reply('JoinChannelResponse', MessageID=m['MessageID'], StatusCode=M.CHANNEL_NOT_FOUND)
            return
        if ch.password and ch.password != m['LobbyChannelPassword']:
            self.reply('JoinChannelResponse', MessageID=m['MessageID'], StatusCode=M.INVALID_PASSWORD)
            return
        if len(ch.players) >= ch.max_players and self.player.account_id not in ch.players:
            self.reply('JoinChannelResponse', MessageID=m['MessageID'], StatusCode=M.WORLD_IS_FULL)
            return
        self.lobby.join_channel(self.player, ch)
        self.reply('JoinChannelResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS,
                   ConnectInfo=self.server.lobby_connect_info(self.player, ch.id, server_key=False))

    def on_GetWorldSecurityLevel(self, m):
        ch = self.lobby.channels.get(m['MediusWorldID'])
        g = self.lobby.games.get(m['MediusWorldID'])
        world = ch or g
        level = 1 if world is not None and world.password else 0
        self.reply('GetWorldSecurityLevelResponse', MessageID=m['MessageID'],
                   StatusCode=M.SUCCESS if world else M.NO_RESULT, MediusWorldID=m['MediusWorldID'],
                   AppType=m['AppType'], SecurityLevel=level)

    def _lobby_players(self, m, name, extra):
        ch = self.lobby.channels.get(m['MediusWorldID']) or (self.player.channel if self.player else None)
        players = sorted(ch.players.values(), key=lambda p: p.name.lower()) if ch else []
        entries = []
        for p in players:
            e = {'AccountID': p.account_id, 'AccountName': p.name}
            if extra:
                e['OnlineState'] = self.online_state(p.account_id)
            entries.append(e)
        self.reply_list(name, m['MessageID'], entries)

    def on_GetLobbyPlayerNames_ExtraInfo(self, m):
        self._lobby_players(m, 'GetLobbyPlayerNames_ExtraInfoResponse', True)

    def on_GetLobbyPlayerNames(self, m):
        self._lobby_players(m, 'GetLobbyPlayerNamesResponse', False)

    # ---- games: info, filters, reports

    def _game_info(self, m, name, generic):
        g = self.lobby.games.get(m['MediusWorldID'])
        if g is None:
            self.reply(name, MessageID=m['MessageID'], StatusCode=M.GAME_NOT_FOUND)
            return
        e = {'MessageID': m['MessageID'], 'StatusCode': M.SUCCESS, 'ApplicationID': self.app_id,
             'MinPlayers': g.min_players, 'MaxPlayers': g.max_players, 'GameLevel': g.game_level,
             'PlayerSkillLevel': g.skill, 'PlayerCount': g.player_count, 'GameStats': g.stats, 'GameName': g.name,
             'RulesSet': g.rules, 'WorldStatus': g.status, 'GameHostType': g.host_type}
        for i in range(generic):
            e['GenericField%d' % (i + 1)] = g.generic[i]
        self.reply(name, e)

    def on_GameInfo(self, m):
        self._game_info(m, 'GameInfoResponse', 8)

    def on_GameInfo0(self, m):
        self._game_info(m, 'GameInfoResponse0', 3)

    def on_SetGameListFilter(self, m):
        fid = self.add_filter(m)
        self.reply('SetGameListFilterResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS, FilterID=fid)

    def on_SetGameListFilter0(self, m):
        fid = self.add_filter(dict(m, Mask=0xFFFFFFFF))
        self.reply('SetGameListFilterResponse0', MessageID=m['MessageID'], StatusCode=M.SUCCESS, FilterID=fid)

    def add_filter(self, m):
        self.filters = getattr(self, 'filters', {})
        self.next_filter = getattr(self, 'next_filter', 0) + 1
        self.filters[self.next_filter] = (m['FilterField'], m['Mask'], m['ComparisonOperator'], m['BaselineValue'])
        return self.next_filter

    def _clear_filter(self, m):
        getattr(self, 'filters', {}).pop(m['FilterID'], None) if m['FilterID'] else getattr(self, 'filters', {}).clear()
        self.reply('ClearGameListFilterResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    on_ClearGameListFilter = on_ClearGameListFilter0 = _clear_filter

    def _report(self, m):
        g = self.lobby.games.get(m['MediusWorldID'])
        if g is None:
            return
        g.name = m['GameName'] or g.name
        g.stats = m['GameStats']
        g.min_players, g.max_players = m['MinPlayers'], m['MaxPlayers'] or g.max_players
        g.game_level, g.skill, g.rules = m['GameLevel'], m['PlayerSkillLevel'], m['RulesSet']
        for i in range(8):
            if 'GenericField%d' % (i + 1) in m:
                g.generic[i] = m['GenericField%d' % (i + 1)]
        # WorldClosed means the match has started and takes no more players (it stays listed as in
        # progress); the game ends when its DME world empties.
        if g.status not in (M.WORLD_ACTIVE, M.WORLD_CLOSED) and m['WorldStatus'] in (M.WORLD_ACTIVE, M.WORLD_CLOSED):
            g.match += 1
        g.status = m['WorldStatus']

    on_WorldReport = on_WorldReport0 = _report

    def on_PlayerReport(self, m):
        self.store_stats(m['Stats'])

    def on_EndGameReport(self, m):
        g = self.lobby.games.get(m['MediusWorldID'])
        log.info('mls %s: game %s ended: winning team %r, player %r, score %d', self.who(), g.name if g else m['MediusWorldID'],
                 m['WinningTeam'], m['WinningPlayer'], m['FinalScore'])

    def on_raw_4_75(self, body):
        """TextFilter (a cell's name and tag, before creating it): [MessageID][SessionKey][4][length]
        [text]. Answered "passes": MessageID, then two zero ints (status, filter type)."""
        mid = body[:21]
        n = int.from_bytes(body[42:46], 'little') if len(body) >= 46 else 0
        log.info('mls %s: text check %r: passes', self.who(), body[46:46 + n].decode('latin-1', 'replace'))
        self.send_app(bytes((M.LOBBY_EXT, 0x76)) + mid.ljust(21, b'\0') + bytes(3 + 8))

    def on_SessionEnd(self, m):
        # Leaving online play: answer, then sign out (which closes this connection).
        self.reply('SessionEndResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)
        self.lobby.end_session(m['SessionKey'])

    def on_UpdateUserState(self, m):
        if self.player is not None:
            self.player.state = m['UserAction']

    # ---- chat and binary messages

    def recipients(self, mtype, target):
        """Players a chat/binary message goes to (not the sender, not players ignoring the sender)."""
        p = self.player
        if mtype in (M.CHAT_WHISPER,):
            t = self.lobby.find_player(account_id=target)
            players = [t] if t else []
        elif mtype == M.CHAT_BROADCAST_ACROSS_ENTIRE_UNIVERSE:
            players = list(self.lobby.by_account.values())
        elif mtype == M.CHAT_CLAN:
            members = {r['account_id'] for r in self.server.store.clan_members(target)} if target else set()
            if not members:
                for c in self.server.store.clans_of(p.account_id):
                    members |= {r['account_id'] for r in self.server.store.clan_members(c['clan_id'])}
            players = [self.lobby.by_account[a] for a in members if a in self.lobby.by_account]
        elif mtype == M.CHAT_BUDDY:
            players = [self.lobby.by_account[r['account_id']] for r in self.server.store.buddies(p.account_id)
                       if r['account_id'] in self.lobby.by_account]
        else:
            players = list(p.channel.players.values()) if p.channel else []
        out = []
        for q in players:
            if q is p or q is None or q.lobby is None:
                continue
            if self.server.store.is_ignoring(q.account_id, p.account_id):
                continue
            out.append(q)
        return out

    def _chat(self, m, generic):
        if self.player is None:
            return
        text = m['Message']
        for q in self.recipients(m['MessageType'], m['TargetID']):
            if generic:
                q.lobby.reply('GenericChatFwdMessage', TimeStamp=self.server.unix_time(),
                              OriginatorAccountID=self.player.account_id, MessageType=m['MessageType'],
                              OriginatorAccountName=self.player.name, Message=text)
            else:
                q.lobby.reply('ChatFwdMessage', MessageID='', OriginatorAccountID=self.player.account_id,
                              OriginatorAccountName=self.player.name, MessageType=m['MessageType'], Message=text)

    def on_raw_4_73(self, body):
        """GenericChatMessage1, the only chat Combat Ops sends (lobby, whisper, cell, buddies):
        [MessageID][SessionKey][2][type i32][target i32][length i32][text]. Forwarded as
        GenericChatFwdMessage1 (4/0x74): [time u32][sender id][type][sender name 32][length][text]."""
        if self.player is None or len(body) < 0x34:
            return
        mtype, target, n = struct.unpack_from('<iii', body, 0x28)
        text = body[0x34:0x34 + max(0, min(n, 512))].split(b'\0', 1)[0]
        log.info('mls %s: %s chats (type %d, target %d): %r', self.who(), self.player.name, mtype, target,
                 text.decode('latin-1', 'replace'))
        fwd = struct.pack('<Iii32si', self.server.unix_time() & 0xFFFFFFFF, self.player.account_id, mtype,
                          self.player.name.encode('latin-1', 'replace')[:31], len(text) + 1) + text + b'\0'
        for q in self.recipients(mtype, target):
            q.lobby.send_app(bytes((M.LOBBY_EXT, 0x74)) + fwd)

    def on_ChatMessage(self, m):
        self._chat(m, False)

    def on_GenericChatMessage(self, m):
        self._chat(m, True)

    def on_BinaryMessage(self, m):
        if self.player is None:
            return
        for q in self.recipients(m['MessageType'], m['TargetAccountID']):
            q.lobby.reply('BinaryFwdMessage', MessageID='', OriginatorAccountID=self.player.account_id,
                          MessageType=m['MessageType'], Message=m['Message'])

    # ---- buddies, ignore list

    def on_AddToBuddyList(self, m):
        a = self.server.store.account_by_id(m['AccountID'])
        if a is None or a['account_id'] == self.player.account_id:
            self.reply('AddToBuddyListResponse', MessageID=m['MessageID'], StatusCode=M.ACCOUNT_NOT_FOUND)
            return
        self.server.store.add_buddy(self.player.account_id, a['account_id'])
        self.reply('AddToBuddyListResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    def on_RemoveFromBuddyList(self, m):
        self.server.store.remove_buddy(self.player.account_id, m['AccountID'])
        self.server.store.remove_buddy(m['AccountID'], self.player.account_id)
        self.reply('RemoveFromBuddyListResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    # Buddy invitations: the invited player is asked (AddToBuddyListFwdConfirmation) if they're online,
    # otherwise the invitation waits in their list (GetBuddyInvitations); accepting makes both buddies.

    def _buddy_invite(self, m, response):
        store, me = self.server.store, self.player
        target = store.account_by_id(m['TargetAccountID'])
        if me is None or target is None or target['account_id'] == me.account_id:
            self.reply(response, MessageID=m['MessageID'], StatusCode=M.ACCOUNT_NOT_FOUND,
                       TargetAccountID=m['TargetAccountID'])
            return
        tid = target['account_id']
        done = dict(MessageID=m['MessageID'], TargetAccountID=tid, TargetAccountName=target['name'])
        if any(b['account_id'] == tid for b in store.buddies(me.account_id)):
            self.reply(response, StatusCode=M.SUCCESS, **done)
            return
        store.add_buddy(me.account_id, tid, confirmed=0)
        p = self.lobby.find_player(account_id=tid)
        log.info('mls %s: %s invites %s to be buddies (%s)', self.who(), me.name, target['name'],
                 'asking them' if p is not None and p.lobby is not None else 'offline: they will see it later')
        if p is not None and p.lobby is not None and not store.is_ignoring(tid, me.account_id):
            self.lobby.buddy_invites[(me.account_id, tid)] = (self, response, done)
            p.lobby.reply('AddToBuddyListFwdConfirmation', MessageID=m['MessageID'], OriginatorAccountID=me.account_id,
                          OriginatorAccountName=me.name, AddType=m['AddType'])
        else:
            self.reply(response, StatusCode=M.SUCCESS, **done)

    def on_BuddyGetPermission(self, m):
        self._buddy_invite(m, 'BuddyGetPermissionResponse')

    def on_AddToBuddyListConfirmation(self, m):
        self._buddy_invite(m, 'AddToBuddyListConfirmationResponse')

    def on_AddToBuddyListFwdConfirmationResponse(self, m):
        store, me, origin = self.server.store, self.player, m['OriginatorAccountID']
        if me is None:
            return
        accepted = m['StatusCode'] in (M.SUCCESS, M.REQUEST_ACCEPTED)
        if accepted:
            store.add_buddy(origin, me.account_id)
            store.add_buddy(me.account_id, origin)
        else:
            store.remove_buddy(origin, me.account_id)
        log.info('mls %s: %s %s the buddy invitation from %d', self.who(), me.name, 'accepted' if accepted else 'declined',
                 origin)
        waiting = self.lobby.buddy_invites.pop((origin, me.account_id), None)
        if waiting is not None and not waiting[0].closed:
            conn, response, done = waiting
            conn.reply(response, StatusCode=M.SUCCESS if accepted else M.REQUEST_DENIED, **done)

    def on_GetBuddyInvitations(self, m):
        rows = self.server.store.buddy_requests_to(self.player.account_id) if self.player else []
        self.reply_list('GetBuddyInvitationsResponse', m['MessageID'],
                        [{'AccountID': r['account_id'], 'AccountName': r['name'], 'AddType': 0} for r in rows])

    def on_AddToIgnoreList(self, m):
        a = self.server.store.account_by_id(m['IgnoreAccountID'])
        if a is None:
            self.reply('AddToIgnoreListResponse', MessageID=m['MessageID'], StatusCode=M.ACCOUNT_NOT_FOUND)
            return
        self.server.store.add_ignore(self.player.account_id, a['account_id'])
        self.reply('AddToIgnoreListResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    def on_RemoveFromIgnoreList(self, m):
        self.server.store.remove_ignore(self.player.account_id, m['IgnoreAccountID'])
        self.reply('RemoveFromIgnoreListResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    # ---- clans ("cells")

    def on_CreateClan(self, m):
        store = self.server.store
        if store.clans_led_by(self.player.account_id):
            self.reply('CreateClanResponse', MessageID=m['MessageID'], StatusCode=M.ALREADY_LEADER_OF_CLAN)
            return
        clan_id = store.create_clan(self.app_id, m['ClanName'], self.player.account_id)
        self.reply('CreateClanResponse', MessageID=m['MessageID'],
                   StatusCode=M.SUCCESS if clan_id else M.CLAN_NAME_IN_USE, ClanID=clan_id or 0)

    def _leader_of(self, clan_id):
        c = self.server.store.clan(clan_id)
        return c if c is not None and c['leader_id'] == self.player.account_id else None

    def on_DisbandClan(self, m):
        c = self._leader_of(m['ClanID'])
        if c is None:
            self.reply('DisbandClanResponse', MessageID=m['MessageID'], StatusCode=M.NOT_CLAN_LEADER)
            return
        self.server.store.disband_clan(c['clan_id'])
        self.reply('DisbandClanResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    def on_GetClanByID(self, m):
        c = self.server.store.clan(m['ClanID'])
        if c is None:
            self.reply('GetClanByIDResponse', MessageID=m['MessageID'], StatusCode=M.CLAN_NOT_FOUND)
            return
        row = self.clan_row(c)
        row.pop('ClanID')
        self.reply('GetClanByIDResponse', dict(row, MessageID=m['MessageID'], StatusCode=M.SUCCESS))

    def on_GetClanMemberList_ExtraInfo(self, m):
        rows = self.server.store.clan_members(m['ClanID'])
        self.reply_list('GetClanMemberList_ExtraInfoResponse', m['MessageID'], [
            {'AccountID': r['account_id'], 'AccountName': r['name'], 'Stats': r['stats'],
             'OnlineState': self.online_state(r['account_id']), 'LadderPosition': i + 1, 'TotalRankings': len(rows)}
            for i, r in enumerate(rows)])

    def on_RemovePlayerFromClan(self, m):
        store = self.server.store
        c = store.clan(m['ClanID'])
        me = self.player.account_id
        if c is None:
            status = M.CLAN_NOT_FOUND
        elif m['PlayerAccountID'] == c['leader_id']:
            status = M.LEADER_CANNOT_LEAVE_CLAN
        elif me != c['leader_id'] and me != m['PlayerAccountID']:
            status = M.NOT_CLAN_LEADER
        else:
            store.remove_clan_member(c['clan_id'], m['PlayerAccountID'])
            status = M.SUCCESS
        self.reply('RemovePlayerFromClanResponse', MessageID=m['MessageID'], StatusCode=status)

    def on_InvitePlayerToClan(self, m):
        store = self.server.store
        led = store.clans_led_by(self.player.account_id)
        if not led:
            self.reply('InvitePlayerToClanResponse', MessageID=m['MessageID'], StatusCode=M.NOT_CLAN_LEADER)
            return
        if store.account_by_id(m['PlayerAccountID']) is None:
            self.reply('InvitePlayerToClanResponse', MessageID=m['MessageID'], StatusCode=M.ACCOUNT_NOT_FOUND)
            return
        store.invite_to_clan(led[0]['clan_id'], m['PlayerAccountID'], m['InviteMessage'])
        self.reply('InvitePlayerToClanResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    def on_CheckMyClanInvitations(self, m):
        rows = self.server.store.clan_invitations_for(self.player.account_id) if self.player else []
        rows = self.page(rows, m['Start'], m['PageSize'])
        self.reply_list('CheckMyClanInvitationsResponse', m['MessageID'], [
            {'ClanInvitationID': r['invitation_id'], 'ClanID': r['clan_id'], 'ResponseStatus': r['response'],
             'Message': r['message'], 'LeaderAccountID': r['leader_id'], 'LeaderAccountName': r['leader_name']}
            for r in rows])

    def on_RespondToClanInvitation(self, m):
        ok = self.server.store.respond_clan_invitation(m['ClanInvitationID'], self.player.account_id, m['Response'],
                                                       m['Message'])
        self.reply('RespondToClanInvitationResponse', MessageID=m['MessageID'],
                   StatusCode=M.SUCCESS if ok else M.NO_RESULT)

    def on_GetMyClanMessages(self, m):
        rows = self.server.store.clan_messages(m['ClanID'])
        rows = self.page(rows, m['Start'], m['PageSize'])
        # The game shows the newest as the cell's Message of the Day. "No result" is an error to it
        # ("Failed to get The Message Of The Day"): with none set, answer one empty message.
        entries = [{'ClanID': m['ClanID'], 'ClanMessageID': r['message_id'], 'Message': r['text']} for r in rows]
        entries = entries or [{'ClanID': m['ClanID'], 'ClanMessageID': 0, 'Message': ''}]
        self.reply_list('GetMyClanMessagesResponse', m['MessageID'], entries)

    def on_SendClanMessage(self, m):
        led = self.server.store.clans_led_by(self.player.account_id)
        if not led:
            self.reply('SendClanMessageResponse', MessageID=m['MessageID'], StatusCode=M.NOT_CLAN_LEADER)
            return
        self.server.store.add_clan_message(led[0]['clan_id'], m['Message'])
        self.reply('SendClanMessageResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    def on_ModifyClanMessage(self, m):
        led = self.server.store.clans_led_by(self.player.account_id)
        ok = bool(led) and self.server.store.modify_clan_message(led[0]['clan_id'], m['ClanMessageID'], m['NewMessage'])
        self.reply('ModifyClanMessageResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS if ok else M.NOT_CLAN_LEADER)

    def on_UpdateClanStats(self, m):
        c = self.server.store.clan(m['ClanID'])
        if c is None or not self.server.store.is_clan_member(c['clan_id'], self.player.account_id):
            self.reply('UpdateClanStatsResponse', MessageID=m['MessageID'], StatusCode=M.NOT_CLAN_MEMBER)
            return
        self.server.store.set_clan_stats(c['clan_id'], m['Stats'])
        self.reply('UpdateClanStatsResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS)

    # ---- file uploads: FileCreate, then FileUpload (the server asks for each chunk with
    # FileUploadServerReq, the client answers FileUploadResponse), then FileClose

    def on_FileCreate(self, m):
        f = m['File']
        info = self.server.files.create(self.app_id, f['Filename'], self.player.account_id, f['FileSize'])
        status = M.SUCCESS if info else M.FILE_NO_PERMISSIONS
        self.reply('FileCreateResponse', File=info or f, StatusCode=status, MessageID=m['MessageID'])

    def on_FileUpload(self, m):
        f = m['File']
        self.upload = {'name': f['Filename'], 'size': m['DataSize'] or f['FileSize'], 'data': bytearray(),
                       'mid': m['MessageID']}
        self.reply('FileUploadServerReq', ReqStartByteIndex=0, PacketNumber=0, XferStatus=M.XFER_INITIAL,
                   StatusCode=M.SUCCESS, MessageID=m['MessageID'])

    def on_FileUploadResponse(self, m):
        up = getattr(self, 'upload', None)
        if up is None:
            return
        chunk = m['Data'][:max(0, m['DataSize'])]
        up['data'][m['StartByteIndex']:m['StartByteIndex'] + len(chunk)] = chunk
        done = m['XferStatus'] == M.XFER_END or len(up['data']) >= up['size']
        if done:
            self.server.files.write(self.app_id, up['name'], self.player.account_id, bytes(up['data']))
            self.reply('FileUploadServerReq', ReqStartByteIndex=len(up['data']), PacketNumber=m['PacketNumber'] + 1,
                       XferStatus=M.XFER_END, StatusCode=M.SUCCESS, MessageID=up['mid'])
            self.upload = None
        else:
            self.reply('FileUploadServerReq', ReqStartByteIndex=len(up['data']), PacketNumber=m['PacketNumber'] + 1,
                       XferStatus=M.XFER_MID, StatusCode=M.SUCCESS, MessageID=up['mid'])

    def on_FileClose(self, m):
        self.reply('FileCloseResponse', File=m['File'], StatusCode=M.SUCCESS, MessageID=m['MessageID'])

    def on_FileDelete(self, m):
        ok = self.server.files.delete(m['File']['Filename'], self.player.account_id)
        self.reply('FileDeleteResponse', StatusCode=M.SUCCESS if ok else M.FILE_NO_PERMISSIONS, MessageID=m['MessageID'])

    def on_AccountGetID(self, m):
        a = self.server.store.find_account(self.app_id, m['AccountName'])
        self.reply('AccountGetIDResponse', MessageID=m['MessageID'], AccountID=a['account_id'] if a else 0,
                   StatusCode=M.SUCCESS if a else M.ACCOUNT_NOT_FOUND)

    # ---- totals and world player lists

    def on_GetTotalGames(self, m):
        self.reply('GetTotalGamesResponse', MessageID=m['MessageID'], Total=len(self.visible_games()), StatusCode=M.SUCCESS)

    def on_GetTotalChannels(self, m):
        self.reply('GetTotalChannelsResponse', MessageID=m['MessageID'], Total=len(self.lobby.channels), StatusCode=M.SUCCESS)

    def on_GetTotalUsers(self, m):
        self.reply('GetTotalUsersResponse', MessageID=m['MessageID'], Total=self.lobby.player_count(), StatusCode=M.SUCCESS)

    def on_LobbyWorldPlayerList(self, m):
        ch = self.lobby.channels.get(m['MediusWorldID'])
        players = sorted(ch.players.values(), key=lambda p: p.name.lower()) if ch else []
        stats = {a['account_id']: a['stats'] for a in (self.server.store.account_by_id(p.account_id) for p in players) if a}
        self.reply_list('LobbyWorldPlayerListResponse', m['MessageID'], [
            {'PlayerStatus': self.player_status(p.account_id), 'AccountID': p.account_id, 'AccountName': p.name,
             'Stats': stats.get(p.account_id, b'')} for p in players])

    def on_GameWorldPlayerList(self, m):
        g = self.lobby.games.get(m['MediusWorldID'])
        players = sorted(g.players.values(), key=lambda p: p.name.lower()) if g else []
        self.reply_list('GameWorldPlayerListResponse', m['MessageID'], [
            {'AccountID': p.account_id, 'AccountName': p.name,
             'Stats': (self.server.store.account_by_id(p.account_id) or {'stats': b''})['stats']} for p in players])

    # ---- creating and joining games

    def _create_game(self, m):
        name = (m['GameName'] or '').strip()
        if not name:
            self.reply('CreateGameResponse', MessageID=m['MessageID'], StatusCode=M.GAME_NAME_NOT_FOUND)
            return
        if any(g.name.lower() == name.lower() for g in self.lobby.games.values()):
            self.reply('CreateGameResponse', MessageID=m['MessageID'], StatusCode=M.GAME_NAME_EXISTS)
            return
        fields = dict(m)
        fields['MaxPlayers'] = min(max(m['MaxPlayers'] or self.server.game.max_game_players, 1), 16)
        g = self.lobby.create_game(self.player, fields)
        g.status = M.WORLD_PENDING_CREATION
        self.server.dme.create_world(g)
        log.info('mls %s: %s created game %d %r (max %d, rules %d, generic %s, host type %d, attributes %x)',
                 self.who(), self.player.name, g.id, g.name, g.max_players, g.rules, g.generic, g.host_type,
                 g.attributes)
        self.reply('CreateGameResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS, MediusWorldID=g.id)

    on_CreateGame = on_CreateGame1 = _create_game

    def on_JoinGame(self, m):
        g = self.lobby.games.get(m['MediusWorldID'])
        status = M.SUCCESS
        if g is None or g.dme is None:
            status = M.GAME_NOT_FOUND
        elif g.password and g.password != m['GamePassword'] and g.host is not self.player:
            status = M.INVALID_PASSWORD
        elif len(g.dme.clients) + len(g.dme.pending) >= g.max_players:
            status = M.WORLD_IS_FULL
        if status != M.SUCCESS:
            self.reply('JoinGameResponse', MessageID=m['MessageID'], StatusCode=status)
            return
        token, index = g.dme.reserve(self.player)
        if token is None:
            self.reply('JoinGameResponse', MessageID=m['MessageID'], StatusCode=M.WORLD_IS_FULL)
            return
        c = self.server.config
        info = {'Type': 3,      # NetConnectionTypeClientServerTCPAuxUDP
                'AddressList': self.server.address_list(c.dme_port), 'WorldID': g.dme.id,
                'ServerKey': self.server.rsa_key.modulus_le(), 'SessionKey': self.player.session_key,
                'AccessKey': token}
        log.info('mls %s: %s joins game %d %r as player %d', self.who(), self.player.name, g.id, g.name, index)
        self.reply('JoinGameResponse', MessageID=m['MessageID'], StatusCode=M.SUCCESS, GameHostType=g.host_type,
                   ConnectInfo=info)
