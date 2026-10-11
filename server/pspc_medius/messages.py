# SPDX-License-Identifier: GPL-2.0-or-later
"""Medius app messages: [class u8][type u8][struct]. Layouts follow horizon-server's RT.Models (MIT,
(c) 2020 Daniel Gerendasy) unless a comment says they were taken from Syphon Filter: Combat Ops
(Medius client library 3.02) itself.
"""

from .structs import (ACCOUNTNAME, BOOL, Bytes, CHATMESSAGE, GAMENAME, I32, Layout, MID, NET_ADDRESS_LIST,
                      NET_CONNECTION_INFO, PASSWORD, RSA_KEY, SESSIONKEY, STATS, Pad, Str, U16, U32, U8, WORLDNAME)

# Message classes
DME, LOBBY, APPLICATION, LOBBY_REPORT, LOBBY_EXT, LOBBY_AUTH = 0, 1, 2, 3, 4, 5

# MediusCallbackStatus
SUCCESS = 0
NO_RESULT = 1
REQUEST_ACCEPTED = 2
WORLD_CREATED_SIZE_REDUCED = 3
PASS = 4
IN_QUEUE = 5
BEGIN_SESSION_FAILED = -1000
ACCOUNT_ALREADY_EXISTS = -999
ACCOUNT_NOT_FOUND = -998
ACCOUNT_LOGGED_IN = -997
END_SESSION_FAILED = -996
LOGIN_FAILED = -995
REGISTRATION_FAILED = -994
INCORRECT_LOGIN_STEP = -993
ALREADY_LEADER_OF_CLAN = -992
WM_ERROR = -991
NOT_CLAN_LEADER = -990
PLAYER_NOT_PRIVILEGED = -989
DB_ERROR = -988
DME_ERROR = -987
EXCEEDS_MAX_WORLDS = -986
REQUEST_DENIED = -985
SET_GAME_LIST_FILTER_FAILED = -984
CLEAR_GAME_LIST_FILTER_FAILED = -983
GET_GAME_LIST_FILTER_FAILED = -982
NUM_FILTERS_AT_MAX = -981
FILTER_NOT_FOUND = -980
INVALID_REQUEST_MSG = -979
INVALID_PASSWORD = -978
GAME_NOT_FOUND = -977
CHANNEL_NOT_FOUND = -976
GAME_NAME_EXISTS = -975
CHANNEL_NAME_EXISTS = -974
GAME_NAME_NOT_FOUND = -973
PLAYER_BANNED = -972
CLAN_NOT_FOUND = -971
CLAN_NAME_IN_USE = -970
SESSION_KEY_INVALID = -969
TEXT_STRING_INVALID = -968
FILTER_FAILED = -967
FAIL = -966
FILE_INTERNAL_ACCESS_ERROR = -965
FILE_NO_PERMISSIONS = -964
FILE_DOES_NOT_EXIST = -963
FILE_ALREADY_EXISTS = -962
FILE_INVALID_FILENAME = -961
FILE_QUOTA_EXCEEDED = -960
CACHE_FAILURE = -959
DATA_ALREADY_EXISTS = -958
DATA_DOES_NOT_EXIST = -957
MAX_EXCEEDED = -956
KEY_ERROR = -955
INCOMPATIBLE_APP_ID = -954
ACCOUNT_BANNED = -953
MACHINE_BANNED = -952
LEADER_CANNOT_LEAVE_CLAN = -951
FEATURE_NOT_ENABLED = -950
DNAS_SIGNATURE_LOGGED_IN = -949
WORLD_IS_FULL = -948
NOT_CLAN_MEMBER = -947
SERVER_BUSY = -946
NUM_GAME_WORLDS_PER_LOBBY_WORLD_EXCEEDED = -945

# MediusCharacterEncodingType / MediusLanguageType are passed through; MediusAccountType:
ACCOUNT_CHILD, ACCOUNT_MASTER = 0, 1

# MediusUniverseVariableInformationInfoFilter
INFO_UNIVERSES, INFO_NEWS, INFO_ID, INFO_NAME, INFO_DNS = 1, 2, 4, 8, 16
INFO_DESCRIPTION, INFO_STATUS, INFO_BILLING, INFO_EXTRAINFO, INFO_SVO_URL = 32, 64, 128, 256, 512

_REQUESTS = {}
_RESPONSES = {}
NAMES = {}


def request(cls, typ, name, *fields, packed=False):
    layout = Layout(*fields, packed=packed)
    _REQUESTS[(cls, typ)] = (name, layout)
    NAMES[(cls, typ)] = name
    return layout


def response(cls, typ, name, *fields, packed=False):
    layout = Layout(*fields, packed=packed)
    _RESPONSES[name] = (cls, typ, layout)
    NAMES[(cls, typ)] = name
    return layout


def decode(cls, typ, body):
    """(name, fields dict) for a client message, or (None, None) when unknown."""
    entry = _REQUESTS.get((cls, typ))
    if entry is None:
        return None, None
    name, layout = entry
    return name, layout.unpack(body)


def encode(name, fields=None, **kw):
    cls, typ, layout = _RESPONSES[name]
    return bytes((cls, typ)) + layout.pack(fields, **kw)


def name_of(cls, typ):
    return NAMES.get((cls, typ), 'class %d type 0x%02X' % (cls, typ))


# ---------------------------------------------------------------- universe information (MUIS)

request(LOBBY, 0xC8, 'GetUniverseInformation',
        ('MessageID', MID), ('InfoType', U32), ('CharacterEncoding', I32), ('Language', I32))

# ---------------------------------------------------------------- authentication (MAS)

request(LOBBY, 0x03, 'SessionBegin', ('MessageID', MID), ('ConnectionClass', I32))
request(LOBBY_EXT, 0x8B, 'SessionBegin1', ('MessageID', MID), ('ConnectionClass', I32))
# Aligned as in Combat Ops (horizon-server reads the ints right after the MessageID).
request(LOBBY_EXT, 0x27, 'ExtendedSessionBegin',
        ('MessageID', MID), ('VersionMajor', I32), ('VersionMinor', I32), ('VersionBuild', I32))
response(LOBBY, 0x04, 'SessionBeginResponse', ('MessageID', MID), ('StatusCode', I32), ('SessionKey', SESSIONKEY))

request(LOBBY, 0x05, 'SessionEnd', ('MessageID', MID), ('SessionKey', SESSIONKEY))
response(LOBBY, 0x06, 'SessionEndResponse', ('MessageID', MID), ('StatusCode', I32))

request(LOBBY, 0xA3, 'SetLocalizationParams',
        ('MessageID', MID), ('SessionKey', SESSIONKEY), ('CharacterEncoding', I32), ('Language', I32))
request(LOBBY_EXT, 0x7B, 'SetLocalizationParams1',
        ('MessageID', MID), ('SessionKey', SESSIONKEY), ('CharacterEncoding', I32), ('Language', I32))
response(LOBBY, 0xA4, 'SetLocalizationParamsResponse', ('MessageID', MID), ('StatusCode', I32))

request(LOBBY, 0x47, 'GetPolicy', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('Policy', I32))
response(LOBBY, 0x48, 'PolicyResponse',
        ('MessageID', MID), ('StatusCode', I32), ('Policy', Str(256)), ('EndOfText', BOOL))

request(LOBBY, 0x07, 'AccountLogin',
        ('MessageID', MID), ('SessionKey', SESSIONKEY), ('Username', ACCOUNTNAME), ('Password', PASSWORD))
response(LOBBY, 0x08, 'AccountLoginResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountType', I32),
         ('MediusWorldID', I32), ('ConnectInfo', NET_CONNECTION_INFO))

request(LOBBY, 0x09, 'AccountRegistration',
        ('MessageID', MID), ('SessionKey', SESSIONKEY), ('AccountType', I32), ('AccountName', ACCOUNTNAME),
        ('Password', PASSWORD))
response(LOBBY, 0x0A, 'AccountRegistrationResponse', ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32))

request(LOBBY, 0x15, 'AccountLogout', ('MessageID', MID), ('SessionKey', SESSIONKEY))
response(LOBBY, 0x16, 'AccountLogoutResponse', ('MessageID', MID), ('StatusCode', I32))

request(LOBBY, 0x17, 'AccountGetID', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('AccountName', ACCOUNTNAME))
response(LOBBY, 0x18, 'AccountGetIDResponse', ('MessageID', MID), ('AccountID', I32), ('StatusCode', I32))

request(LOBBY, 0x19, 'AnonymousLogin',
        ('MessageID', MID), ('SessionKey', SESSIONKEY), ('DisplayName', ACCOUNTNAME), ('Stats', STATS))
response(LOBBY, 0x1A, 'AnonymousLoginResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountType', I32),
         ('MediusWorldID', I32), ('ConnectInfo', NET_CONNECTION_INFO))

request(LOBBY, 0x1B, 'GetMyIP', ('MessageID', MID), ('SessionKey', SESSIONKEY))
response(LOBBY, 0x1C, 'GetMyIPResponse', ('MessageID', MID), ('IP', Str(20)), ('StatusCode', I32))

request(LOBBY_EXT, 0x0A, 'DnasSignaturePost',
        ('MessageID', MID), ('SessionKey', SESSIONKEY), ('DnasSignatureType', I32), ('DnasSignatureLength', U8),
        ('DnasSignature', Bytes(32)))

# TicketLogin (LobbyExt 0x58): the NP ticket follows a header; parsed by hand in auth.py.
NAMES[(LOBBY_EXT, 0x58)] = 'TicketLogin'
response(LOBBY_EXT, 0x59, 'TicketLoginResponse',
         ('MessageID', MID), ('', Pad(3)), ('StatusCode', I32), ('Unknown', Bytes(29)), ('AccountID', I32),
         ('AccountType', I32), ('MediusWorldID', I32), ('ConnectInfo', NET_CONNECTION_INFO), packed=True)

# ---------------------------------------------------------------- lobby (MLS): players

request(LOBBY, 0x31, 'PlayerInfo', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('AccountID', I32))
response(LOBBY, 0x32, 'PlayerInfoResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountName', ACCOUNTNAME), ('ApplicationID', I32),
         ('PlayerStatus', I32), ('ConnectionClass', I32), ('Stats', STATS))

# MediusPlayerStatus
PLAYER_DISCONNECTED, PLAYER_IN_AUTH_WORLD, PLAYER_IN_CHAT_WORLD, PLAYER_IN_GAME_WORLD, PLAYER_IN_OTHER_UNIVERSE = 0, 1, 2, 3, 4

# ---------------------------------------------------------------- lobby (MLS): file services

MEDIUS_FILE = Layout(
    ('Filename', Str(128)), ('ServerChecksum', Bytes(16)), ('FileID', U32), ('FileSize', U32),
    ('CreationTimeStamp', U32), ('OwnerID', U32), ('GroupID', U32), ('OwnerPermissionRWX', U16),
    ('GroupPermissionRWX', U16), ('GlobalPermissionRWX', U16), ('ServerOperationID', U16))
assert MEDIUS_FILE.size == 172
FILE_CHUNK = 464

# Combat Ops sends these unpadded: the MediusFile, then the MessageID.
request(LOBBY, 0xAC, 'FileDownload', ('File', MEDIUS_FILE), ('MessageID', MID))
# Data, then the chunk's position; XferStatus 1 initial, 2 middle, 3 last (a one-chunk file is 3).
response(LOBBY, 0xAD, 'FileDownloadResponse',
         ('Data', Bytes(FILE_CHUNK)), ('StartByteIndex', I32), ('DataSize', I32), ('PacketNumber', I32),
         ('XferStatus', I32), ('StatusCode', I32), ('MessageID', MID))
XFER_ERROR, XFER_INITIAL, XFER_MID, XFER_END = 0, 1, 2, 3

# ---------------------------------------------------------------- lobby (MLS): stats

request(LOBBY, 0x11, 'AccountUpdateStats', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('Stats', STATS))
response(LOBBY, 0x12, 'AccountUpdateStatsResponse', ('MessageID', MID), ('StatusCode', I32))

# ---------------------------------------------------------------- lobby (MLS): channels

CLANNAME = Str(32)
LOBBYNAME = Str(64)

request(LOBBY_EXT, 0x1F, 'ChannelList_ExtraInfo', ('MessageID', MID), ('', U8), ('PageID', U16), ('PageSize', U16))
request(LOBBY_EXT, 0x15, 'ChannelList_ExtraInfo1', ('MessageID', MID), ('', U8), ('PageID', U16), ('PageSize', U16))
request(LOBBY, 0xEC, 'ChannelList_ExtraInfo0', ('MessageID', MID), ('', U8), ('PageID', U16), ('PageSize', U16))
response(LOBBY, 0xED, 'ChannelList_ExtraInfoResponse',
         ('MessageID', MID), ('StatusCode', I32), ('MediusWorldID', I32), ('PlayerCount', U16), ('MaxPlayers', U16),
         ('GameWorldCount', U16), ('SecurityLevel', I32), ('GenericField1', U32), ('GenericField2', U32),
         ('GenericField3', U32), ('GenericField4', U32), ('GenericFieldLevel', I32), ('LobbyName', LOBBYNAME),
         ('EndOfList', BOOL))

# ---------------------------------------------------------------- lobby (MLS): games

request(LOBBY_EXT, 0x37, 'GameList_ExtraInfo', ('MessageID', MID), ('', U8), ('PageID', U16), ('PageSize', U16))
request(LOBBY, 0xEA, 'GameList_ExtraInfo0', ('MessageID', MID), ('', U8), ('PageID', U16), ('PageSize', U16))
response(LOBBY_EXT, 0x38, 'GameList_ExtraInfoResponse',
         ('MessageID', MID), ('StatusCode', I32), ('MediusWorldID', I32), ('PlayerCount', U16), ('MinPlayers', U16),
         ('MaxPlayers', U16), ('GameLevel', I32), ('PlayerSkillLevel', I32), ('RulesSet', I32),
         ('GenericField1', I32), ('GenericField2', I32), ('GenericField3', I32), ('GenericField4', I32),
         ('GenericField5', I32), ('GenericField6', I32), ('GenericField7', I32), ('GenericField8', I32),
         ('SecurityLevel', I32), ('WorldStatus', I32), ('GameHostType', I32), ('GameName', GAMENAME),
         ('GameStats', STATS), ('EndOfList', BOOL))

# ---------------------------------------------------------------- lobby (MLS): clans

request(LOBBY, 0x7C, 'GetMyClans', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('Start', I32), ('PageSize', I32))
response(LOBBY, 0x7D, 'GetMyClansResponse',
         ('MessageID', MID), ('StatusCode', I32), ('ClanID', I32), ('ApplicationID', I32), ('ClanName', CLANNAME),
         ('LeaderAccountID', I32), ('LeaderAccountName', ACCOUNTNAME), ('Stats', STATS), ('Status', I32),
         ('EndOfList', BOOL))

# ---------------------------------------------------------------- lobby (MLS): buddies, ignore list

PLAYER_ONLINE_STATE = Layout(('ConnectStatus', I32), ('MediusLobbyWorldID', I32), ('MediusGameWorldID', I32),
                             ('LobbyName', WORLDNAME), ('GameName', WORLDNAME))

request(LOBBY, 0xD6, 'GetBuddyList_ExtraInfo', ('MessageID', MID))
response(LOBBY, 0xD7, 'GetBuddyList_ExtraInfoResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountName', ACCOUNTNAME),
         ('OnlineState', PLAYER_ONLINE_STATE), ('EndOfList', BOOL))

request(LOBBY, 0xC0, 'GetIgnoreList', ('MessageID', MID), ('SessionKey', SESSIONKEY))
response(LOBBY, 0xC1, 'GetIgnoreListResponse',
         ('MessageID', MID), ('StatusCode', I32), ('IgnoreAccountID', I32), ('IgnoreAccountName', ACCOUNTNAME),
         ('PlayerStatus', I32), ('EndOfList', BOOL))

CLANMSG = Str(200)
GAMEPASSWORD = Str(32)

request(LOBBY, 0x25, 'JoinChannel', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MediusWorldID', I32),
        ('LobbyChannelPassword', Str(32)))
response(LOBBY, 0x26, 'JoinChannelResponse', ('MessageID', MID), ('StatusCode', I32), ('ConnectInfo', NET_CONNECTION_INFO))

request(LOBBY, 0x64, 'CheckMyClanInvitations', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('Start', I32),
        ('PageSize', I32))
response(LOBBY, 0x65, 'CheckMyClanInvitationsResponse',
         ('MessageID', MID), ('StatusCode', I32), ('ClanInvitationID', I32), ('ClanID', I32), ('ResponseStatus', I32),
         ('Message', CLANMSG), ('LeaderAccountID', I32), ('LeaderAccountName', ACCOUNTNAME), ('EndOfList', BOOL))

# Buddy invitations (layouts from clank, which added them for Syphon Filter).
request(LOBBY_EXT, 0x08, 'GetBuddyInvitations', ('MessageID', MID))
response(LOBBY_EXT, 0x09, 'GetBuddyInvitationsResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountName', ACCOUNTNAME), ('AddType', I32),
         ('EndOfList', BOOL))

# Buddy requests that need the other player's consent (the game's "Invite to buddy list"). Layouts
# from the client library's serializers; the game sends BuddyGetPermission.
_BUDDY_ASK = (('MessageID', MID), ('SessionKey', SESSIONKEY), ('TargetAccountID', I32), ('AddType', I32))
_BUDDY_ASKED = (('MessageID', MID), ('StatusCode', I32), ('TargetAccountID', I32), ('TargetAccountName', ACCOUNTNAME))
request(LOBBY_EXT, 0x63, 'BuddyGetPermission', *_BUDDY_ASK)
response(LOBBY_EXT, 0x64, 'BuddyGetPermissionResponse', *_BUDDY_ASKED)
request(LOBBY_EXT, 0x05, 'AddToBuddyListConfirmation', *_BUDDY_ASK)
response(LOBBY, 0x44, 'AddToBuddyListConfirmationResponse', *_BUDDY_ASKED)
response(LOBBY_EXT, 0x06, 'AddToBuddyListFwdConfirmation',            # to the invited player
         ('MessageID', MID), ('OriginatorAccountID', I32), ('OriginatorAccountName', ACCOUNTNAME), ('AddType', I32))
request(LOBBY_EXT, 0x07, 'AddToBuddyListFwdConfirmationResponse',     # their answer (REQUEST_ACCEPTED or _DENIED)
        ('MessageID', MID), ('StatusCode', I32), ('SessionKey', SESSIONKEY), ('OriginatorAccountID', I32),
        ('AddType', I32))

# ---------------------------------------------------------------- lobby (MLS): players in a channel

request(LOBBY, 0xDC, 'GetLobbyPlayerNames_ExtraInfo', ('MessageID', MID), ('MediusWorldID', I32))
response(LOBBY, 0xDD, 'GetLobbyPlayerNames_ExtraInfoResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountName', ACCOUNTNAME),
         ('OnlineState', PLAYER_ONLINE_STATE), ('EndOfList', BOOL))
request(LOBBY, 0x9F, 'GetLobbyPlayerNames', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MediusWorldID', I32))
response(LOBBY, 0xA0, 'GetLobbyPlayerNamesResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountName', ACCOUNTNAME), ('EndOfList', BOOL))

request(LOBBY, 0x88, 'GetWorldSecurityLevel', ('MessageID', MID), ('', Pad(2)), ('SessionKey', SESSIONKEY),
        ('MediusWorldID', I32), ('AppType', I32))
response(LOBBY, 0x89, 'GetWorldSecurityLevelResponse',
         ('MessageID', MID), ('StatusCode', I32), ('MediusWorldID', I32), ('AppType', I32), ('SecurityLevel', I32))

# ---------------------------------------------------------------- lobby (MLS): creating and joining games

_CREATE_COMMON = (('MessageID', MID), ('SessionKey', SESSIONKEY), ('ApplicationID', I32), ('MinPlayers', I32),
                  ('MaxPlayers', I32), ('GameLevel', I32), ('GameName', GAMENAME), ('GamePassword', GAMEPASSWORD),
                  ('SpectatorPassword', GAMEPASSWORD), ('PlayerSkillLevel', I32), ('RulesSet', I32))
request(LOBBY_EXT, 0x2F, 'CreateGame', *(_CREATE_COMMON + tuple(('GenericField%d' % i, I32) for i in range(1, 9)) +
                                          (('GameHostType', I32), ('Attributes', I32))))
request(LOBBY, 0xF4, 'CreateGame1', *(_CREATE_COMMON + tuple(('GenericField%d' % i, I32) for i in range(1, 4)) +
                                       (('GameHostType', I32), ('Attributes', I32))))
response(LOBBY, 0x1E, 'CreateGameResponse', ('MessageID', MID), ('StatusCode', I32), ('MediusWorldID', I32))

request(LOBBY, 0xF3, 'JoinGame', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MediusWorldID', I32),
        ('JoinType', I32), ('GamePassword', GAMEPASSWORD), ('GameHostType', I32), ('pubKey', RSA_KEY),
        ('AddressList', NET_ADDRESS_LIST))
response(LOBBY, 0x24, 'JoinGameResponse', ('MessageID', MID), ('StatusCode', I32), ('GameHostType', I32),
         ('ConnectInfo', NET_CONNECTION_INFO), ('', Pad(4)))

_GAMEINFO = (('MessageID', MID), ('StatusCode', I32), ('ApplicationID', I32), ('MinPlayers', I32), ('MaxPlayers', I32),
             ('GameLevel', I32), ('PlayerSkillLevel', I32), ('PlayerCount', I32), ('GameStats', STATS),
             ('GameName', GAMENAME), ('RulesSet', I32))
request(LOBBY_EXT, 0x35, 'GameInfo', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MediusWorldID', I32))
request(LOBBY, 0x33, 'GameInfo0', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MediusWorldID', I32))
response(LOBBY_EXT, 0x36, 'GameInfoResponse', *(_GAMEINFO + tuple(('GenericField%d' % i, I32) for i in range(1, 9)) +
                                                 (('WorldStatus', I32), ('GameHostType', I32))))
response(LOBBY, 0x34, 'GameInfoResponse0', *(_GAMEINFO + tuple(('GenericField%d' % i, I32) for i in range(1, 4)) +
                                              (('WorldStatus', I32), ('GameHostType', I32))))

request(LOBBY_EXT, 0x33, 'SetGameListFilter', ('MessageID', MID), ('FilterField', I32), ('Mask', U32),
        ('ComparisonOperator', I32), ('BaselineValue', I32))
response(LOBBY_EXT, 0x34, 'SetGameListFilterResponse', ('MessageID', MID), ('StatusCode', I32), ('FilterID', U32))
request(LOBBY, 0x4E, 'SetGameListFilter0', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('FilterField', I32),
        ('ComparisonOperator', I32), ('BaselineValue', I32))
response(LOBBY, 0x4F, 'SetGameListFilterResponse0', ('MessageID', MID), ('StatusCode', I32), ('FilterID', U32))
request(LOBBY_EXT, 0x31, 'ClearGameListFilter', ('MessageID', MID), ('FilterID', U32))
request(LOBBY, 0x50, 'ClearGameListFilter0', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('FilterID', U32))
response(LOBBY, 0x51, 'ClearGameListFilterResponse', ('MessageID', MID), ('StatusCode', I32))

# Reports from players in a game (no reply).
_WORLD_REPORT = (('MediusWorldID', I32), ('PlayerCount', I32), ('GameName', GAMENAME), ('GameStats', STATS),
                 ('MinPlayers', I32), ('MaxPlayers', I32), ('GameLevel', I32), ('PlayerSkillLevel', I32), ('RulesSet', I32))
request(LOBBY_EXT, 0x30, 'WorldReport', *(_WORLD_REPORT + tuple(('GenericField%d' % i, I32) for i in range(1, 9)) +
                                           (('WorldStatus', I32),)))
request(LOBBY, 0x00, 'WorldReport0', *((('SessionKey', SESSIONKEY),) + _WORLD_REPORT +
                                        tuple(('GenericField%d' % i, I32) for i in range(1, 4)) + (('WorldStatus', I32),)))
request(LOBBY, 0x01, 'PlayerReport', ('SessionKey', SESSIONKEY), ('MediusWorldID', I32), ('Stats', STATS))
request(LOBBY, 0x02, 'EndGameReport', ('SessionKey', SESSIONKEY), ('MediusWorldID', I32), ('WinningTeam', Str(64)),
        ('WinningPlayer', ACCOUNTNAME), ('FinalScore', I32))
request(LOBBY, 0x49, 'UpdateUserState', ('SessionKey', SESSIONKEY), ('UserAction', I32))

# MediusWorldStatus
WORLD_INACTIVE, WORLD_STAGING, WORLD_ACTIVE, WORLD_CLOSED, WORLD_PENDING_CREATION, WORLD_PENDING_CONNECT = 0, 1, 2, 3, 4, 5

# ---------------------------------------------------------------- lobby (MLS): chat, binary messages

request(LOBBY, 0x3B, 'ChatMessage', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MessageType', I32),
        ('TargetID', I32), ('Message', CHATMESSAGE))
response(LOBBY, 0x3C, 'ChatFwdMessage', ('MessageID', MID), ('OriginatorAccountID', I32),
         ('OriginatorAccountName', ACCOUNTNAME), ('MessageType', I32), ('Message', CHATMESSAGE))
request(LOBBY_EXT, 0x23, 'GenericChatMessage', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MessageType', I32),
        ('TargetID', I32), ('Message', CHATMESSAGE))
response(LOBBY_EXT, 0x24, 'GenericChatFwdMessage', ('TimeStamp', U32), ('OriginatorAccountID', I32),
         ('MessageType', I32), ('OriginatorAccountName', ACCOUNTNAME), ('Message', CHATMESSAGE))
BINARY = Bytes(400)
request(LOBBY_EXT, 0x16, 'BinaryMessage', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MessageType', I32),
        ('TargetAccountID', I32), ('Message', BINARY))
response(LOBBY_EXT, 0x17, 'BinaryFwdMessage', ('MessageID', MID), ('OriginatorAccountID', I32), ('MessageType', I32),
         ('Message', BINARY))

# MediusChatMessageType / MediusBinaryMessageType
CHAT_BROADCAST, CHAT_WHISPER, CHAT_BROADCAST_ACROSS_ENTIRE_UNIVERSE, CHAT_CLAN, CHAT_BUDDY = 0, 1, 2, 3, 4
BINARY_BROADCAST, BINARY_TARGETED, BINARY_BROADCAST_ACROSS_ENTIRE_UNIVERSE, BINARY_CLAN, BINARY_BUDDY = 0, 1, 2, 3, 4

# ---------------------------------------------------------------- lobby (MLS): buddies, ignore list

for _typ, _name in ((0x3F, 'AddToBuddyList'), (0x41, 'RemoveFromBuddyList')):
    request(LOBBY, _typ, _name, ('MessageID', MID), ('SessionKey', SESSIONKEY), ('AccountID', I32))
    response(LOBBY, _typ + 1, _name + 'Response', ('MessageID', MID), ('StatusCode', I32))
for _typ, _name in ((0xC2, 'AddToIgnoreList'), (0xC4, 'RemoveFromIgnoreList')):
    request(LOBBY, _typ, _name, ('MessageID', MID), ('SessionKey', SESSIONKEY), ('IgnoreAccountID', I32))
    response(LOBBY, _typ + 1, _name + 'Response', ('MessageID', MID), ('StatusCode', I32))

# ---------------------------------------------------------------- lobby (MLS): clans

request(LOBBY, 0x54, 'CreateClan', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('ApplicationID', I32),
        ('ClanName', CLANNAME))
response(LOBBY, 0x55, 'CreateClanResponse', ('MessageID', MID), ('StatusCode', I32), ('ClanID', I32))
request(LOBBY, 0x56, 'DisbandClan', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('ClanID', I32))
response(LOBBY, 0x57, 'DisbandClanResponse', ('MessageID', MID), ('StatusCode', I32))
request(LOBBY, 0x58, 'GetClanByID', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('AccountID', I32), ('ClanID', I32))
response(LOBBY, 0x59, 'GetClanByIDResponse', ('MessageID', MID), ('StatusCode', I32), ('ApplicationID', I32),
         ('ClanName', CLANNAME), ('LeaderAccountID', I32), ('LeaderAccountName', ACCOUNTNAME), ('Stats', STATS),
         ('Status', I32))
request(LOBBY, 0x60, 'RemovePlayerFromClan', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('PlayerAccountID', I32),
        ('ClanID', I32))
response(LOBBY, 0x61, 'RemovePlayerFromClanResponse', ('MessageID', MID), ('StatusCode', I32))
request(LOBBY, 0x62, 'InvitePlayerToClan', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('PlayerAccountID', I32),
        ('InviteMessage', CLANMSG))
response(LOBBY, 0x63, 'InvitePlayerToClanResponse', ('MessageID', MID), ('StatusCode', I32))
request(LOBBY, 0x66, 'RespondToClanInvitation', ('MessageID', MID), ('SessionKey', SESSIONKEY),
        ('ClanInvitationID', I32), ('Response', I32), ('Message', CLANMSG))
response(LOBBY, 0x67, 'RespondToClanInvitationResponse', ('MessageID', MID), ('StatusCode', I32))
request(LOBBY, 0x6C, 'GetMyClanMessages', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('ClanID', I32), ('Start', I32),
        ('PageSize', I32))
# Combat Ops' client library reads the answer as LOBBY_EXT 0x77 (its 1/0x6D isn't registered), with the
# message's id (what ModifyClanMessage names) after the cell's.
response(LOBBY_EXT, 0x77, 'GetMyClanMessagesResponse', ('MessageID', MID), ('StatusCode', I32), ('ClanID', I32),
         ('ClanMessageID', I32), ('Message', CLANMSG), ('EndOfList', BOOL))
request(LOBBY, 0x6E, 'SendClanMessage', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('Message', CLANMSG))
response(LOBBY, 0x6F, 'SendClanMessageResponse', ('MessageID', MID), ('StatusCode', I32))
request(LOBBY, 0x70, 'ModifyClanMessage', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('ClanMessageID', I32),
        ('NewMessage', CLANMSG))
response(LOBBY, 0x71, 'ModifyClanMessageResponse', ('MessageID', MID), ('StatusCode', I32))
request(LOBBY, 0x84, 'UpdateClanStats', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('ClanID', I32), ('Stats', STATS))
response(LOBBY, 0x85, 'UpdateClanStatsResponse', ('MessageID', MID), ('StatusCode', I32))
request(LOBBY, 0xDA, 'GetClanMemberList_ExtraInfo', ('MessageID', MID), ('ClanID', I32), ('LadderStatIndex', I32),
        ('SortOrder', I32))
response(LOBBY, 0xDB, 'GetClanMemberList_ExtraInfoResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountName', ACCOUNTNAME), ('Stats', STATS),
         ('OnlineState', PLAYER_ONLINE_STATE), ('LadderStat', I32), ('LadderPosition', U32), ('TotalRankings', U32),
         ('EndOfList', BOOL))

# MediusClanInvitationsResponseStatus
CLAN_INVITE_UNDECIDED, CLAN_INVITE_ACCEPT, CLAN_INVITE_DECLINE, CLAN_INVITE_REVOKED = 0, 1, 2, 3

# ---------------------------------------------------------------- lobby (MLS): file uploads

MEDIUS_FILE_ATTRIBUTES = Layout(('Description', Bytes(256)), ('LastChangedTimeStamp', U32), ('LastChangedByUserID', U32),
                                ('NumberAccesses', U32), ('StreamableFlag', U32), ('StreamingDataRate', U32))
request(LOBBY, 0xA5, 'FileCreate', ('File', MEDIUS_FILE), ('Attributes', MEDIUS_FILE_ATTRIBUTES), ('MessageID', MID))
response(LOBBY, 0xA6, 'FileCreateResponse', ('File', MEDIUS_FILE), ('StatusCode', I32), ('MessageID', MID))
request(LOBBY, 0xA7, 'FileUpload', ('File', MEDIUS_FILE), ('DataStart', U32), ('DataSize', U32), ('MessageID', MID))
response(LOBBY, 0xA9, 'FileUploadServerReq', ('ReqStartByteIndex', I32), ('PacketNumber', I32), ('XferStatus', I32),
         ('StatusCode', I32), ('MessageID', MID))
request(LOBBY, 0xA8, 'FileUploadResponse', ('Data', Bytes(FILE_CHUNK)), ('StartByteIndex', I32), ('DataSize', I32),
        ('PacketNumber', I32), ('XferStatus', I32), ('StatusCode', I32), ('MessageID', MID))
request(LOBBY, 0xAA, 'FileClose', ('File', MEDIUS_FILE), ('MessageID', MID))
response(LOBBY, 0xAB, 'FileCloseResponse', ('File', MEDIUS_FILE), ('StatusCode', I32), ('MessageID', MID))
request(LOBBY, 0xB0, 'FileDelete', ('File', MEDIUS_FILE), ('MessageID', MID))
response(LOBBY, 0xB1, 'FileDeleteResponse', ('StatusCode', I32), ('MessageID', MID))

# ---------------------------------------------------------------- lobby (MLS): totals, world player lists

for _typ, _name in ((0x9B, 'GetTotalGames'), (0x9D, 'GetTotalChannels'), (0xA1, 'GetTotalUsers')):
    request(LOBBY, _typ, _name, ('MessageID', MID), ('SessionKey', SESSIONKEY), ('ApplicationID', I32))
    response(LOBBY, _typ + 1, _name + 'Response', ('MessageID', MID), ('Total', U32), ('StatusCode', I32))

request(LOBBY, 0x2D, 'LobbyWorldPlayerList', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MediusWorldID', I32))
response(LOBBY, 0x2E, 'LobbyWorldPlayerListResponse',
         ('MessageID', MID), ('StatusCode', I32), ('PlayerStatus', I32), ('AccountID', I32), ('AccountName', ACCOUNTNAME),
         ('Stats', STATS), ('ConnectionClass', I32), ('EndOfList', BOOL))
request(LOBBY, 0x2F, 'GameWorldPlayerList', ('MessageID', MID), ('SessionKey', SESSIONKEY), ('MediusWorldID', I32))
response(LOBBY, 0x30, 'GameWorldPlayerListResponse',
         ('MessageID', MID), ('StatusCode', I32), ('AccountID', I32), ('AccountName', ACCOUNTNAME), ('Stats', STATS),
         ('ConnectionClass', I32), ('EndOfList', BOOL))
