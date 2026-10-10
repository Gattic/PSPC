# SPDX-License-Identifier: GPL-2.0-or-later
"""C structs as Medius sends them: little-endian, each integer aligned to its size, char arrays not
aligned, the struct padded to its largest alignment.

    Layout(('MessageID', MID), ('StatusCode', I32), ('Name', Str(32)))

Strings are NUL-padded and decoded as UTF-8 (cut at the first NUL); a value that doesn't fit is
truncated to leave room for the NUL. Missing values pack as zero / empty.
"""

import struct


class Field:
    size = 0
    align = 1

    def pack(self, value):
        raise NotImplementedError

    def unpack(self, data):
        raise NotImplementedError

    def default(self):
        return 0


class Int(Field):
    def __init__(self, fmt):
        self.fmt = '<' + fmt
        self.size = self.align = struct.calcsize(self.fmt)

    def pack(self, value):
        return struct.pack(self.fmt, int(value or 0))

    def unpack(self, data):
        return struct.unpack(self.fmt, data)[0]


class Bool(Field):
    size = align = 1

    def pack(self, value):
        return b'\x01' if value else b'\x00'

    def unpack(self, data):
        return data[0] != 0

    def default(self):
        return False


class Str(Field):
    def __init__(self, size):
        self.size = size

    def pack(self, value):
        b = (value or '').encode('utf-8') if not isinstance(value, bytes) else value
        b = b[:self.size - 1]
        return b + bytes(self.size - len(b))

    def unpack(self, data):
        return data.split(b'\0', 1)[0].decode('utf-8', 'replace')

    def default(self):
        return ''


class Bytes(Field):
    def __init__(self, size):
        self.size = size

    def pack(self, value):
        b = bytes(value or b'')[:self.size]
        return b + bytes(self.size - len(b))

    def unpack(self, data):
        return bytes(data)

    def default(self):
        return bytes(self.size)


class Pad(Field):
    """Explicit padding the C layout rules wouldn't produce (some messages need it)."""

    def __init__(self, size):
        self.size = size

    def pack(self, value):
        return bytes(self.size)

    def unpack(self, data):
        return None


class Array(Field):
    def __init__(self, field, count):
        self.field, self.count = field, count
        self.size = field.size * count
        self.align = field.align

    def pack(self, value):
        value = list(value or [])
        value += [self.field.default()] * (self.count - len(value))
        return b''.join(self.field.pack(v) for v in value[:self.count])

    def unpack(self, data):
        s = self.field.size
        return [self.field.unpack(data[i * s:(i + 1) * s]) for i in range(self.count)]

    def default(self):
        return [self.field.default()] * self.count


class Layout(Field):
    def __init__(self, *fields, packed=False):
        self.fields = []
        off = 0
        align = 1
        for name, f in fields:
            a = 1 if packed else f.align
            off = (off + a - 1) // a * a
            self.fields.append((name, f, off))
            off += f.size
            align = max(align, a)
        self.align = align
        self.size = (off + align - 1) // align * align

    def pack(self, value=None, **kw):
        values = dict(value or {}, **kw)
        out = bytearray(self.size)
        for name, f, off in self.fields:
            v = values.get(name, f.default())
            out[off:off + f.size] = f.pack(v)
        return bytes(out)

    def unpack(self, data):
        if len(data) < self.size:
            data = bytes(data) + bytes(self.size - len(data))
        out = {}
        for name, f, off in self.fields:
            v = f.unpack(data[off:off + f.size])
            if name and v is not None:
                out[name] = v
        return out

    def default(self):
        return {}

    def offset(self, name):
        for n, f, off in self.fields:
            if n == name:
                return off
        raise KeyError(name)


I8, U8, I16, U16, I32, U32 = Int('b'), Int('B'), Int('h'), Int('H'), Int('i'), Int('I')
BOOL = Bool()
MID = Str(21)          # MessageID: the client's request tag, echoed in the response
SESSIONKEY = Str(17)
ACCESSKEY = Str(17)
ACCOUNTNAME = Str(32)
PASSWORD = Str(32)
WORLDNAME = Str(64)
GAMENAME = Str(64)
CHATMESSAGE = Str(64)
STATS = Bytes(256)     # account / game stats blobs (game-defined)
RSA_KEY = Bytes(64)

# NetAddress: how a client reaches a server
NET_ADDRESS = Layout(('AddressType', I32), ('Address', Str(16)), ('Port', U32))
NET_ADDRESS_LIST = Array(NET_ADDRESS, 2)

# Address types
ADDR_NONE, ADDR_EXTERNAL, ADDR_INTERNAL, ADDR_NAT_SERVICE = 0, 1, 2, 3
ADDR_BINARY_EXTERNAL, ADDR_BINARY_INTERNAL = 4, 5

# NetConnectionType
CONN_NONE, CONN_CLIENT_SERVER_TCP, CONN_P2P_UDP, CONN_CLIENT_SERVER_TCP_AUX_UDP, CONN_CLIENT_LISTENER_TCP = 0, 1, 2, 3, 4

# NetConnectionInfo: where to connect next (the MLS after login, a DME world after joining a game).
# 156 bytes for clients newer than 108 (2 bytes of padding at the end).
NET_CONNECTION_INFO = Layout(
    ('Type', I32),
    ('AddressList', NET_ADDRESS_LIST),
    ('WorldID', I32),
    ('ServerKey', RSA_KEY),
    ('SessionKey', SESSIONKEY),
    ('AccessKey', ACCESSKEY),
)
assert NET_CONNECTION_INFO.size == 156
