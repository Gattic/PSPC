# SPDX-License-Identifier: GPL-2.0-or-later
"""GameSpy's encodings and ciphers, written from the protocols (as the games' GameSpy SDK and
OpenSpy, github.com/openspy/openspy-core, implement them):

- GameSpy's base64 (A-Z a-z 0-9 [ ], padding _) and the password encoding (an XOR stream from the
  Park-Miller generator seeded with "gspy"): GP's passwordenc and passenc.
- The login proofs of GameSpy Presence: MD5 over the password's MD5, 48 spaces, the user, both
  challenges and the password's MD5 again.
- The XOR with "GameSpy3D" of the stats protocol (gamestats.gamespy.com:29920).
- The server browser's stream cipher ("GOA", a variant of Michael Paul Johnson's card-shuffling
  cipher), keyed by the client's challenge mixed with the game's secret key and the server's
  challenge.
"""

import hashlib

B64 = b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789[]'
_B64_DEC = {c: i for i, c in enumerate(B64)}
_B64_DEC.update({ord('+'): 62, ord('/'): 63})


def gs_b64encode(data):
    out = bytearray()
    for i in range(0, len(data), 3):
        chunk = data[i:i + 3]
        n = int.from_bytes(chunk + b'\0' * (3 - len(chunk)), 'big')
        quad = [B64[(n >> s) & 63] for s in (18, 12, 6, 0)]
        if len(chunk) < 3:
            quad[len(chunk) + 1:] = b'_' * (3 - len(chunk))
        out += bytes(quad)
    return bytes(out)


def gs_b64decode(text):
    if isinstance(text, str):
        text = text.encode('latin-1')
    vals = [_B64_DEC[c] for c in text if c in _B64_DEC]
    out = bytearray()
    for i in range(0, len(vals), 4):
        q = vals[i:i + 4]
        n = 0
        for v in q:
            n = (n << 6) | v
        n <<= 6 * (4 - len(q))
        out += n.to_bytes(3, 'big')[:max(0, len(q) - 1)]
    return bytes(out)


def _park_miller(num):
    return (num * 16807) % 0x7FFFFFFF


def gs_passenc(data):
    """XOR with the "gspy" generator stream: both encodes and decodes."""
    num = 0x79707367 & 0x7FFFFFFF
    out = bytearray(data)
    for i in range(len(out)):
        num = _park_miller(num)
        out[i] ^= num % 0xFF
    return bytes(out)


def decode_password(enc):
    """A GP passwordenc / passenc value -> the password."""
    return gs_passenc(gs_b64decode(enc)).decode('utf-8', 'replace')


def encode_password(password):
    return gs_b64encode(gs_passenc(password.encode('utf-8'))).decode('ascii')


def md5hex(data):
    if isinstance(data, str):
        data = data.encode('utf-8')
    return hashlib.md5(data).hexdigest()


def login_hash(password_md5, user, first_challenge, second_challenge):
    """GP's login response (client challenge first) or proof (server challenge first)."""
    return md5hex(password_md5 + ' ' * 48 + user + first_challenge + second_challenge + password_md5)


def gamespy3d_xor(data):
    """The stats protocol's XOR with "GameSpy3D" (the key restarts after each \\final\\)."""
    key = b'GameSpy3D'
    out = bytearray(data)
    k = 0
    i = 0
    while i < len(out):
        if out[i:i + 7] == b'\\final\\':
            i += 7
            k = 0
            continue
        out[i] ^= key[k]
        k = (k + 1) % len(key)
        i += 1
    return bytes(out)


class GOACipher:
    """The server browser's stream cipher. Encrypting and decrypting use the same state machine."""

    def __init__(self, key):
        cards = list(range(256))
        rsum = 0
        keypos = 0
        mask = 255
        for i in range(255, 0, -1):
            # A pseudo-random card position from 0 to i, driven by the key.
            retry = 0
            while True:
                rsum = (cards[rsum] + key[keypos]) & 0xFF
                keypos += 1
                if keypos >= len(key):
                    keypos = 0
                    rsum = (rsum + len(key)) & 0xFF
                u = mask & rsum
                retry += 1
                if retry > 11:
                    u %= i
                if u <= i:
                    break
            cards[i], cards[u] = cards[u], cards[i]
            if i & (i - 1) == 0:
                mask >>= 1
        self.cards = cards
        self.rotor = cards[1]
        self.ratchet = cards[3]
        self.avalanche = cards[5]
        self.last_plain = cards[7]
        self.last_cipher = cards[rsum]

    def _step(self):
        c = self.cards
        self.ratchet = (self.ratchet + c[self.rotor]) & 0xFF
        self.rotor = (self.rotor + 1) & 0xFF
        swap = c[self.last_cipher]
        c[self.last_cipher] = c[self.ratchet]
        c[self.ratchet] = c[self.last_plain]
        c[self.last_plain] = c[self.rotor]
        c[self.rotor] = swap
        self.avalanche = (self.avalanche + c[swap]) & 0xFF
        return (c[(c[self.avalanche] + c[self.rotor]) & 0xFF] ^
                c[c[(c[self.last_plain] + c[self.last_cipher] + c[self.ratchet]) & 0xFF]])

    def encrypt(self, data):
        out = bytearray(len(data))
        for i, b in enumerate(data):
            k = self._step()
            self.last_cipher = b ^ k
            self.last_plain = b
            out[i] = self.last_cipher
        return bytes(out)

    def decrypt(self, data):
        out = bytearray(len(data))
        for i, b in enumerate(data):
            k = self._step()
            self.last_plain = b ^ k
            self.last_cipher = b
            out[i] = self.last_plain
        return bytes(out)


def sb_crypt_key(client_challenge, secret_key, server_challenge):
    """The server list's cipher key: the client's 8-byte challenge, mixed with the game's secret key
    and the server's challenge."""
    key = bytearray(client_challenge)
    secret = secret_key.encode('ascii') if isinstance(secret_key, str) else secret_key
    for i in range(len(server_challenge)):
        j = (i * secret[i % len(secret)]) % len(key)
        key[j] ^= (key[i % len(key)] ^ server_challenge[i]) & 0xFF
    return bytes(key)
