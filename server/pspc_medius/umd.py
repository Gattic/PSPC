# SPDX-License-Identifier: GPL-2.0-or-later
"""Reading Combat Ops' own data: the official missions on the UMD, and mission files in general.

Mission files (.DEF) are "encoded buffers": [optional 0x1C-byte "qDEFSCE" header] then
[ENCB][flags u32][size u32][body]. Flag 1: the body is XTEA-encrypted (32 rounds, ECB, the game's
key) and starts with the MD5 of the rest. Flag 2: the rest is raw-deflate compressed to size bytes.
Inside is an ini file (KEY=value lines) describing the mission. The game only loads missions that
start with ENCB, so the UMD header is stripped before serving them.

The ini keys the game reads for the mission server (and how they map to its query parameters):
FILENAME (MissionName; "~KEY" names a string in COMMON/STRINGS.DAT), LVL_AUTHOR (Author),
LVL_THEMEID (Theme), PVPGAMEMODE (CombatType), LVL_CHUNKn (Terrain: a bitmask of the map chunks
used), LVL_LIGHTING (TimeOfDay), LVL_WEATHERFX (Weather).
"""

import hashlib
import os
import struct
import zlib

_KEY = struct.unpack('<4I', bytes.fromhex('3b9cb9c46c59b7bb437cecd6174c4957'))
_DELTA = 0x9E3779B9
UMD_HEADER = b'qDEFSCE\0'
UMD_HEADER_SIZE = 0x1C

MISSIONS_DIR = 'PSP_GAME/USRDIR/GAMEDATA/MULTI'
STRINGS_PATH = 'PSP_GAME/USRDIR/GAMEDATA/COMMON/STRINGS.DAT'


def _xtea_decrypt(block):
    v0, v1 = struct.unpack('<2I', block)
    s = (_DELTA * 32) & 0xFFFFFFFF
    for _ in range(32):
        v1 = (v1 - ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + _KEY[(s >> 11) & 3]))) & 0xFFFFFFFF
        s = (s - _DELTA) & 0xFFFFFFFF
        v0 = (v0 - ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + _KEY[s & 3]))) & 0xFFFFFFFF
    return struct.pack('<2I', v0, v1)


def _xtea_encrypt(block):
    v0, v1 = struct.unpack('<2I', block)
    s = 0
    for _ in range(32):
        v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + _KEY[s & 3]))) & 0xFFFFFFFF
        s = (s + _DELTA) & 0xFFFFFFFF
        v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + _KEY[(s >> 11) & 3]))) & 0xFFFFFFFF
    return struct.pack('<2I', v0, v1)


def strip_header(data):
    """The mission file as the game loads it: from the ENCB tag on."""
    data = bytes(data)
    if data.startswith(UMD_HEADER) and data[UMD_HEADER_SIZE:UMD_HEADER_SIZE + 4] == b'ENCB':
        return data[UMD_HEADER_SIZE:]
    return data


def decode(data):
    """An encoded buffer's contents, or None if it isn't a valid one."""
    data = strip_header(data)
    if len(data) < 12 or data[:4] != b'ENCB':
        return None
    flags, size = struct.unpack_from('<II', data, 4)
    body = data[12:]
    if flags & ~3 or size > 0x14000:
        return None
    if flags & 1:
        if len(body) < 0x18 or len(body) % 8:
            return None
        plain = b''.join(_xtea_decrypt(body[i:i + 8]) for i in range(0, len(body), 8))
        digest, body = plain[:16], plain[16:]
        if hashlib.md5(body).digest() != digest:
            return None
    if flags & 2:
        try:
            body = zlib.decompress(body, -15)
        except zlib.error:
            return None
        if len(body) != size:
            return None
    return body


def encode(content):
    """content as an encrypted, compressed encoded buffer (the inverse of decode)."""
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = c.compress(content) + c.flush()
    packed += bytes(-len(packed) % 8)
    plain = hashlib.md5(packed).digest() + packed
    body = b''.join(_xtea_encrypt(plain[i:i + 8]) for i in range(0, len(plain), 8))
    return b'ENCB' + struct.pack('<II', 3, len(content)) + body


def parse_ini(text):
    if isinstance(text, bytes):
        text = text.decode('latin-1')
    out = {}
    for line in text.splitlines():
        key, sep, value = line.partition('=')
        if sep and key.strip() and not key.lstrip().startswith((';', '[')):
            out[key.strip()] = value.strip()
    return out


def parse_strings(data):
    """COMMON/STRINGS.DAT: [count u32] then per string [key char[16]][length u32][text, padded to 4]."""
    count, = struct.unpack_from('<I', data, 0)
    pos, out = 4, {}
    for _ in range(count):
        if pos + 20 > len(data):
            break
        key = data[pos:pos + 16].split(b'\0', 1)[0].decode('latin-1')
        length, = struct.unpack_from('<I', data, pos + 16)
        out[key] = data[pos + 20:pos + 20 + length].split(b'\0', 1)[0].decode('latin-1')
        pos += 20 + (length + 3) // 4 * 4
    return out


def _int(ini, key, default=0):
    try:
        return int(ini.get(key, default))
    except ValueError:
        return default


def mission_info(data, strings=None):
    """What the mission server records about a mission file: name, author, theme, combat_type,
    terrain, time_of_day, weather, data (as served) and hash. None if the file isn't valid."""
    content = decode(data)
    if content is None:
        return None
    ini = parse_ini(content)
    strings = strings or {}

    def text(key):
        v = ini.get(key, '')
        return strings.get(v[1:], v[1:]) if v.startswith('~') else v

    terrain = 0
    for n in (1, 2, 3):
        chunk = _int(ini, 'LVL_CHUNK%d' % n, -1)
        if chunk > 0:
            terrain |= 1 << (chunk - 1)
    served = strip_header(data)
    mode = _int(ini, 'PVPGAMEMODE', -1)
    return {'name': text('FILENAME')[:64], 'author': text('LVL_AUTHOR')[:32], 'location': text('LVL_LOCATION'),
            'description': text('LVL_DESCRIPTION'), 'theme': _int(ini, 'LVL_THEMEID'),
            'combat_type': mode if 0 <= mode < 16 else 0, 'terrain': terrain,
            'time_of_day': _int(ini, 'LVL_LIGHTING'), 'weather': _int(ini, 'LVL_WEATHERFX'),
            'official': ini.get('FILENAME', '').startswith('~'), 'data': served,
            'hash': hashlib.md5(served).hexdigest()}


# ---------------------------------------------------------------- the UMD (an ISO or CSO image, or a folder)

class _Cso:
    """A CSO (compressed ISO, version 1) image read as the ISO inside it."""

    def __init__(self, f):
        self.f = f
        f.seek(0)
        _, header_size, self.size, self.block_size, _, self.align = struct.unpack('<4sIQIBB', f.read(22))
        if not self.block_size or self.size > 1 << 40:
            raise ValueError('bad CSO header')
        blocks = (self.size + self.block_size - 1) // self.block_size
        f.seek(header_size if header_size >= 24 else 24)
        self.index = struct.unpack('<%dI' % (blocks + 1), f.read(4 * (blocks + 1)))

    def read(self, offset, size):
        out = b''
        while size > 0 and offset < self.size:
            block, skip = divmod(offset, self.block_size)
            start, end = self.index[block] & 0x7FFFFFFF, self.index[block + 1] & 0x7FFFFFFF
            self.f.seek(start << self.align)
            data = self.f.read((end - start) << self.align)
            if not self.index[block] & 0x80000000:
                data = zlib.decompressobj(-15).decompress(data)
            chunk = data[skip:skip + size]
            out += chunk
            offset += len(chunk)
            size -= len(chunk)
            if not chunk:
                break
        return out


class Iso9660:
    """Just enough ISO 9660 to read files from a UMD image (.iso, or .cso)."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        try:
            self.cso = _Cso(self.f) if self.f.read(4) == b'CISO' else None
            pvd = self._read(16 * 2048, 2048)
            if pvd[1:6] != b'CD001':
                raise ValueError('%s is not an ISO 9660 image (or a CSO of one)' % path)
        except Exception:
            self.f.close()
            raise
        root = pvd[156:156 + 34]
        self.root = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]

    def close(self):
        self.f.close()

    def _read(self, offset, size):
        if self.cso is not None:
            return self.cso.read(offset, size)
        self.f.seek(offset)
        return self.f.read(size)

    def _entries(self, lba, size):
        data = self._read(lba * 2048, size)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + n]
            name = rec[33:33 + rec[32]]
            if name not in (b'\0', b'\1'):
                yield (name.decode('latin-1').split(';')[0], struct.unpack_from('<I', rec, 2)[0],
                       struct.unpack_from('<I', rec, 10)[0], bool(rec[25] & 2))
            i += n

    def _find(self, path):
        lba, size, is_dir = self.root[0], self.root[1], True
        for part in [p for p in path.replace('\\', '/').split('/') if p]:
            if not is_dir:
                return None
            for name, elba, esize, edir in self._entries(lba, size):
                if name.upper() == part.upper():
                    lba, size, is_dir = elba, esize, edir
                    break
            else:
                return None
        return lba, size, is_dir

    def read(self, path):
        e = self._find(path)
        return self._read(e[0] * 2048, e[1]) if e and not e[2] else None

    def walk(self, path):
        """The files under path (recursively), as paths."""
        e = self._find(path)
        if not e or not e[2]:
            return
        stack = [(path.rstrip('/'), e[0], e[1])]
        while stack:
            base, lba, size = stack.pop()
            for name, elba, esize, edir in self._entries(lba, size):
                if edir:
                    stack.append((base + '/' + name, elba, esize))
                else:
                    yield base + '/' + name


class Folder:
    """An extracted UMD (the folder holding PSP_GAME)."""

    def __init__(self, path):
        self.path = path

    def close(self):
        pass

    def read(self, path):
        p = os.path.join(self.path, *path.split('/'))
        if not os.path.isfile(p):
            return None
        with open(p, 'rb') as f:
            return f.read()

    def walk(self, path):
        top = os.path.join(self.path, *path.split('/'))
        for root, _, files in os.walk(top):
            for name in files:
                yield path + '/' + os.path.relpath(os.path.join(root, name), top).replace(os.sep, '/')


def open_umd(path):
    return Folder(path) if os.path.isdir(path) else Iso9660(path)


def official_missions(path):
    """The missions on a Combat Ops UMD (an ISO or CSO image, or an extracted folder), sorted by file name:
    [(file name, mission_info)]."""
    umd = open_umd(path)
    try:
        strings_data = umd.read(STRINGS_PATH)
        strings = parse_strings(strings_data) if strings_data else {}
        out = []
        for p in sorted(umd.walk(MISSIONS_DIR)):
            if p.upper().endswith('.DEF') and '/MISSIONS/' in p.upper():
                info = mission_info(umd.read(p), strings)
                if info is not None:
                    out.append((p.rsplit('/', 1)[1], info))
        return out
    finally:
        umd.close()
