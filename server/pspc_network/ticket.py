# SPDX-License-Identifier: GPL-2.0-or-later
"""NP tickets: the proof of sign-in a PSP game sends to its own server.

The layout is Sony's version 2.1 ticket as PSP games receive it (big-endian; layout as in PPSSPP's
Core/HLE/sceNp.cpp, which community revival servers accept):

    u32 version (0x21010000)    s32 size of everything after these 8 bytes
    section 0x3000 (body):  u16 type, u16 size, then parameters (u16 type, u16 length, data):
        binary[20]  serial id          int   issuer id
        date        issued (ms)        date  expires (ms)
        long        account id         string[32] online id
        binary[4]   country ("us")     string[4]  domain ("c9")
        binary[24]  service id         int   status ((age & 0x7f) << 24 | flags)
        null                           null
    section 0x3002 (footer):
        binary[4]   cipher id          binary[32] signature

Sony signed with ECDSA; PSPC Network signs with HMAC-SHA256 over the header and body (cipher id
"PSPC"), and game servers check tickets with POST /v1/tickets/verify. Tickets pspc makes itself
when no server is configured carry cipher id "PSPL" and a signature nobody can check.
"""

import hashlib
import hmac
import struct

VERSION_2_1 = 0x21010000
T_NULL, T_INT, T_LONG, T_STRING, T_DATE, T_BINARY = 0, 1, 2, 4, 7, 8
SECTION_BODY, SECTION_FOOTER = 0x3000, 0x3002
CIPHER_SERVER = b'PSPC'
CIPHER_LOCAL = b'PSPL'
ISSUER_ID = 0x100


class TicketError(ValueError):
    pass


def _param(ptype, data):
    return struct.pack('>HH', ptype, len(data)) + data


def _fixed(data, size):
    return data[:size].ljust(size, b'\0')


def _signed_part(*, serial, issued_ms, expires_ms, account_id, online_id, country, domain, service_id,
                 status):
    body = b''.join([
        _param(T_BINARY, _fixed(serial, 20)),
        _param(T_INT, struct.pack('>I', ISSUER_ID)),
        _param(T_DATE, struct.pack('>Q', issued_ms)),
        _param(T_DATE, struct.pack('>Q', expires_ms)),
        _param(T_LONG, struct.pack('>Q', account_id & 0xFFFFFFFFFFFFFFFF)),
        _param(T_STRING, _fixed(online_id.encode('ascii'), 32)),
        _param(T_BINARY, _fixed(country.encode('ascii'), 4)),
        _param(T_STRING, _fixed(domain.encode('ascii'), 4)),
        _param(T_BINARY, _fixed(service_id.encode('ascii', 'replace'), 24)),
        _param(T_INT, struct.pack('>I', status)),
        _param(T_NULL, b''),
        _param(T_NULL, b''),
    ])
    body_section = struct.pack('>HH', SECTION_BODY, len(body)) + body
    footer_size = 4 + 4 + 4 + 32
    total = len(body_section) + 4 + footer_size
    return struct.pack('>Ii', VERSION_2_1, total) + body_section


def build(key, *, serial, issued_ms, expires_ms, account_id, online_id, country='us', domain='c9',
          service_id='', status=24 << 24, cipher=CIPHER_SERVER):
    """A signed ticket (bytes)."""
    signed = _signed_part(serial=serial, issued_ms=issued_ms, expires_ms=expires_ms, account_id=account_id,
                          online_id=online_id, country=country, domain=domain, service_id=service_id,
                          status=status)
    signature = hmac.new(key, signed, hashlib.sha256).digest()
    footer = _param(T_BINARY, cipher) + _param(T_BINARY, signature)
    return signed + struct.pack('>HH', SECTION_FOOTER, len(footer)) + footer


def _params(data):
    out, pos = [], 0
    while pos + 4 <= len(data):
        ptype, length = struct.unpack_from('>HH', data, pos)
        pos += 4
        if pos + length > len(data):
            raise TicketError('parameter runs past its section')
        out.append((ptype, data[pos:pos + length]))
        pos += length
    if pos != len(data):
        raise TicketError('trailing bytes in a section')
    return out


def parse(ticket):
    """Fields of a ticket (dict). Raises TicketError if it isn't one of ours or Sony's layout."""
    if len(ticket) < 12:
        raise TicketError('too short')
    version, size = struct.unpack_from('>Ii', ticket, 0)
    if version >> 24 not in (0x21, 0x31, 0x41) or size != len(ticket) - 8:
        raise TicketError('bad header')
    pos, sections = 8, {}
    while pos + 4 <= len(ticket):
        stype, ssize = struct.unpack_from('>HH', ticket, pos)
        start = pos + 4
        if start + ssize > len(ticket):
            raise TicketError('section runs past the end')
        sections[stype] = (start, ticket[start:start + ssize])
        pos = start + ssize
    if SECTION_BODY not in sections:
        raise TicketError('no body')
    body = _params(sections[SECTION_BODY][1])
    if len(body) < 10:
        raise TicketError('body too short')

    def text(value):
        return value.split(b'\0', 1)[0].decode('ascii', 'replace')

    fields = {
        'version': version,
        'serial': body[0][1],
        'issuer_id': struct.unpack('>I', body[1][1])[0],
        'issued_ms': struct.unpack('>Q', body[2][1])[0],
        'expires_ms': struct.unpack('>Q', body[3][1])[0],
        'account_id': struct.unpack('>Q', body[4][1])[0],
        'online_id': text(body[5][1]),
        'country': text(body[6][1]),
        'domain': text(body[7][1]),
        'service_id': text(body[8][1]),
        'status': struct.unpack('>I', body[9][1])[0],
        'cipher': b'',
        'signature': b'',
        'signed_part': ticket[:sections[SECTION_BODY][0] + len(sections[SECTION_BODY][1])],
    }
    if SECTION_FOOTER in sections:
        footer = _params(sections[SECTION_FOOTER][1])
        if len(footer) >= 2:
            fields['cipher'], fields['signature'] = footer[0][1], footer[1][1]
    return fields


def verify(key, ticket, now_ms):
    """(fields, None) for a valid PSPC Network ticket, else (fields or None, reason)."""
    try:
        fields = parse(ticket)
    except TicketError as e:
        return None, 'malformed: %s' % e
    if fields['cipher'] != CIPHER_SERVER:
        return fields, 'not issued by a PSPC Network server'
    expected = hmac.new(key, fields['signed_part'], hashlib.sha256).digest()
    if not hmac.compare_digest(expected, fields['signature']):
        return fields, 'bad signature'
    if now_ms >= fields['expires_ms']:
        return fields, 'expired'
    return fields, None
