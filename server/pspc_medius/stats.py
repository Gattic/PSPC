# SPDX-License-Identifier: GPL-2.0-or-later
"""Combat Ops' statistics records, which the game writes and the server stores as opaque 256-byte
blobs (MediusAccountUpdateStats for players, MediusUpdateClanStats for cells). Decoded here for the
leaderboards and the Community pages. Layouts from the game (the record builder at 0x088a7560, its
validator at 0x088abba8); everything is little-endian and bytes 0x6C on are zero.

Player record:
  0x00 u32 magic 0x12345678      0x04 i32 cell id (-1: none)    0x08 i32 cell patch 0-59
  0x0C char[16] cell name        0x1C char[5] cell tag          0x21 char[3] rank ("R0".."T3")
  0x24 i32 ranked XP (0-1000)    0x28 u16[5] games played       0x32 u16[5] games won
                                 (both in the order RET, SAB, DM, TDM, ROG)
  0x40 u32 kills, deaths, suicides, revives, objectives, headshots
  0x58 char[20] online ID

The game only changes it after ranked games (4+ players, even teams). Cell record: magic "OFNC"
(0x434E464F), u32 patch, char[5] tag; nothing else the game reads.
"""

import struct

PLAYER_MAGIC = 0x12345678
CELL_MAGIC = 0x434E464F
MODES = ('RET', 'SAB', 'DM', 'TDM', 'ROG')
MODE_NAMES = {'RET': 'Retrieval', 'SAB': 'Sabotage', 'DM': 'Deathmatch', 'TDM': 'Team Deathmatch',
              'ROG': 'Rogue Agent'}
RANKS = ('R0', 'R1', 'R2', 'R3', 'O1', 'O2', 'O3', 'O4', 'T1', 'T2', 'T3')
RANK_XP = (0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000)
MAX_XP = 1000
COUNTERS = ('kills', 'deaths', 'suicides', 'revives', 'objectives', 'headshots')


def _cstr(b):
    return b.split(b'\0', 1)[0].decode('latin-1')


def rank_of(xp):
    return RANKS[max(i for i, t in enumerate(RANK_XP) if t <= max(xp, 0))]


def decode_player(blob):
    """The record's fields, or None if it isn't a valid player record."""
    blob = bytes(blob or b'')
    if len(blob) < 0x6C:
        return None
    magic, cell_id, patch = struct.unpack_from('<Iii', blob, 0)
    xp, = struct.unpack_from('<i', blob, 0x24)
    rank = _cstr(blob[0x21:0x24])
    if magic != PLAYER_MAGIC or not rank or not 0 <= patch <= 59 or xp < 0:
        return None
    played = struct.unpack_from('<5H', blob, 0x28)
    won = struct.unpack_from('<5H', blob, 0x32)
    counters = struct.unpack_from('<6I', blob, 0x40)
    in_cell = cell_id >= 0
    out = {'cell_id': cell_id if in_cell else None, 'cell_patch': patch if in_cell else None,
           'cell_name': _cstr(blob[0x0C:0x1C]) if in_cell else '', 'cell_tag': _cstr(blob[0x1C:0x21]) if in_cell else '',
           'rank': rank, 'xp': xp, 'played': dict(zip(MODES, played)), 'won': dict(zip(MODES, won)),
           'online_id': _cstr(blob[0x58:0x6C])}
    out.update(zip(COUNTERS, counters))
    out['games'] = sum(played)
    out['wins'] = sum(won)
    return out


def decode_cell(blob):
    blob = bytes(blob or b'')
    if len(blob) < 13 or struct.unpack_from('<I', blob, 0)[0] != CELL_MAGIC:
        return None
    patch, = struct.unpack_from('<I', blob, 4)
    return {'patch': patch, 'tag': _cstr(blob[8:13])}


def progress(old, new):
    """Whether a new player record may replace the stored one. Stats only grow, so a record with
    less XP or fewer games than the stored one (a console that lost its stats and started over) is
    refused and the stored record kept; the game reads it back at the next login."""
    a, b = decode_player(old), decode_player(new)
    if b is None:
        return a is None
    if a is None:
        return True
    if b['xp'] < a['xp']:
        return False
    return all(b['played'][m] >= a['played'][m] and b['won'][m] >= a['won'][m] for m in MODES) and \
        all(b[c] >= a[c] for c in COUNTERS)


def encode_player(**fields):
    """A player record (for tests and tools): the fields decode_player returns."""
    blob = bytearray(256)
    cell = fields.get('cell_id')
    struct.pack_into('<Iii', blob, 0, PLAYER_MAGIC, -1 if cell is None else cell, fields.get('cell_patch') or 0)
    blob[0x0C:0x1C] = fields.get('cell_name', '').encode('latin-1')[:15].ljust(16, b'\0')
    blob[0x1C:0x21] = fields.get('cell_tag', '').encode('latin-1')[:4].ljust(5, b'\0')
    xp = fields.get('xp', 0)
    blob[0x21:0x24] = fields.get('rank', rank_of(xp)).encode('latin-1')[:2].ljust(3, b'\0')
    struct.pack_into('<i', blob, 0x24, xp)
    struct.pack_into('<5H', blob, 0x28, *(fields.get('played', {}).get(m, 0) for m in MODES))
    struct.pack_into('<5H', blob, 0x32, *(fields.get('won', {}).get(m, 0) for m in MODES))
    struct.pack_into('<6I', blob, 0x40, *(fields.get(c, 0) for c in COUNTERS))
    blob[0x58:0x6C] = fields.get('online_id', '').encode('latin-1')[:19].ljust(20, b'\0')
    return bytes(blob)
