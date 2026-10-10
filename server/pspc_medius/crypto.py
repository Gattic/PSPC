# SPDX-License-Identifier: GPL-2.0-or-later
"""SCE-RT message encryption as Medius 3.x clients (PS3, PSP: "Medius version" 112/113) use it.

Ported from horizon-server's RT.Cryptography (PS3_RCQ, PS3_RSA; MIT, (c) 2020 Daniel Gerendasy).

Every encrypted RT message carries a 4-byte hash of its plaintext. The top 3 bits of the hash's
last byte name the key ("context") it was encrypted with:

  RSA_AUTH (7)           textbook RSA-512, e = 17, numbers little-endian on the wire. The server's
                         key (sent in its hello certificate) for the client's first message, the
                         client's key (from RT_MSG_CLIENT_CRYPTKEY_PUBLIC) for the server's answer.
  RC_CLIENT_SESSION (3)  the session key the server hands out (RT_MSG_SERVER_CRYPTKEY_PEER); a
                         stream cipher over 32-bit words ("RCQ"), seeded from the key and the hash.

A hash whose low 29 bits are zero marks a plaintext payload.
"""

import os
import struct

CTX_RC_SERVER_SESSION = 1
CTX_RC_CLIENT_SESSION = 3
CTX_RSA_AUTH = 7

_M32 = 0xFFFFFFFF
_R3 = 0x5B3AA654
_R5 = 0x75970A4D


def _words(data):
    """The data as little-endian 32-bit words, the last one zero-padded."""
    if len(data) % 4:
        data = bytes(data) + bytes(4 - len(data) % 4)
    return struct.unpack('<%dI' % (len(data) // 4), data)


def _round(r16, r17, r18, r19):
    r19 ^= _R3
    r18 = (r18 + r16 + r19) & _M32
    r18 = ((r18 << 7) | (r18 >> 25)) & _M32
    r17 = (r17 + r19 + r18) & _M32
    r18 ^= _R5
    r17 = ((r17 << 11) | (r17 >> 21)) & _M32
    r16 = (r16 + r18 + r17) & _M32
    r16 = ((r16 >> 15) | (r16 << 17)) & _M32
    r0 = r16 & r17
    r17 = ~r17 & _M32
    r0 |= r18 & r17
    r19 = (r19 + r0) & _M32
    r16 = ~r16 & _M32
    return r16, r17, r18, r19


def _pass(data, state, mode=None):
    """One keystream pass over data. mode None: only mix it into the state; 'enc'/'dec': also return
    the transformed bytes (as long as data). Returns (state, output)."""
    r16, r17, r18, r19 = state
    out = []
    for w in _words(data):
        r16, r17, r18, r19 = _round(r16, r17, r18, r19)
        if mode == 'dec':
            w ^= r19
            r19 ^= w
            out.append(w)
        else:
            r19 ^= w
            out.append(r19)
    state = (r16, r17, r18, r19)
    if mode is None:
        return state, None
    return state, struct.pack('<%dI' % len(out), *out)[:len(data)]


_HASH_IV, _ = _pass(bytes(16), (0, 0, 0, 0))  # B5A0559C 88AA4C20 013D2CC7 CB2DE2B6


def rcq_hash(data, context):
    """The 4-byte message hash: a keyless pass over the data, the context in the top 3 bits."""
    (r16, r17, r18, r19), _ = _pass(data, _HASH_IV)
    value = ((r16 + r17 + r18 + r19) & 0x1FFFFFFF) | (context << 29)
    return struct.pack('<I', value)


def is_plaintext_hash(h):
    return h[0] == 0 and h[1] == 0 and h[2] == 0 and (h[3] & 0x1F) == 0


def hash_context(h):
    return h[3] >> 5


class RcqCipher:
    """The session cipher. Only the first 16 bytes of the 64-byte key are used."""

    def __init__(self, key, context=CTX_RC_CLIENT_SESSION):
        if len(key) < 16:
            raise ValueError('RCQ key too short')
        self.key = bytes(key)
        self.context = context

    def _seed(self, h):
        state = _words(self.key[:16])
        state, _ = _pass(h, state)
        iv = struct.pack('<4I', *state)
        state, _ = _pass(iv, state, 'enc')
        return state

    def encrypt(self, plain):
        h = rcq_hash(plain, self.context)
        _, cipher = _pass(plain, self._seed(h), 'enc')
        return cipher, h

    def decrypt(self, cipher, h):
        """The plaintext, or None when the hash doesn't match it (wrong key or corrupt data)."""
        if is_plaintext_hash(h):
            return bytes(cipher)
        _, plain = _pass(cipher, self._seed(h), 'dec')
        return plain if rcq_hash(plain, hash_context(h)) == bytes(h) else None


def _int_le(b):
    return int.from_bytes(b, 'little')


class RsaKey:
    """Textbook RSA as SCE-RT does it: no padding, little-endian numbers, e = 17."""

    def __init__(self, n, e=17, d=None):
        self.n, self.e, self.d = n, e, d
        self.size = (n.bit_length() + 7) // 8

    @classmethod
    def from_wire(cls, modulus_le):
        return cls(_int_le(modulus_le))

    def modulus_le(self):
        return self.n.to_bytes(self.size, 'little')

    def modulus_be(self):
        return self.n.to_bytes(self.size, 'big')

    def encrypt(self, plain):
        h = rcq_hash(plain, CTX_RSA_AUTH)
        c = pow(_int_le(plain), self.e, self.n)
        return c.to_bytes(max(len(plain), self.size), 'little')[:max(len(plain), self.size)], h

    def decrypt(self, cipher, h):
        if is_plaintext_hash(h):
            return bytes(cipher)
        if self.d is None:
            raise ValueError('no private key')
        m = pow(_int_le(cipher), self.d, self.n)
        # A plaintext of 64 bytes can be larger than n: then the client encrypted m - n.
        for candidate in (m, m + self.n):
            if candidate.bit_length() > len(cipher) * 8:
                continue
            plain = candidate.to_bytes(len(cipher), 'little')
            if rcq_hash(plain, hash_context(h)) == bytes(h):
                return plain
        return None


def generate_rsa_key(bits=512, e=17):
    """A fresh RSA key (Miller-Rabin primes from os.urandom)."""
    def is_prime(n):
        if n < 2:
            return False
        for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
            if n % p == 0:
                return n == p
        d, s = n - 1, 0
        while d % 2 == 0:
            d //= 2
            s += 1
        for _ in range(40):
            a = 2 + _int_le(os.urandom(bits // 8)) % (n - 3)
            x = pow(a, d, n)
            if x in (1, n - 1):
                continue
            for _ in range(s - 1):
                x = pow(x, 2, n)
                if x == n - 1:
                    break
            else:
                return False
        return True

    def prime(nbits):
        while True:
            p = _int_le(os.urandom(nbits // 8)) | (1 << (nbits - 1)) | (1 << (nbits - 2)) | 1
            if (p - 1) % e and is_prime(p):
                return p

    while True:
        p, q = prime(bits // 2), prime(bits // 2)
        n = p * q
        if p != q and n.bit_length() == bits:
            d = pow(e, -1, (p - 1) * (q - 1))
            return RsaKey(n, e, d)


def session_key(rsa):
    """A random 64-byte session key that, read as a little-endian number, is smaller than the
    client's RSA modulus (so RSA doesn't reduce it)."""
    while True:
        k = os.urandom(64)
        if _int_le(k) < rsa.n:
            return k
