# SPDX-License-Identifier: GPL-2.0-or-later
"""GameSpy's text messages: \\command\\\\key\\value\\...\\final\\ (Presence, search, stats)."""


class Message(dict):
    """One message: the command (its first key) and the key/value pairs (lower-case keys; the first
    value of a repeated key)."""

    def __init__(self, command, pairs):
        super().__init__()
        self.command = command
        self.pairs = pairs
        for k, v in pairs:
            self.setdefault(k.lower(), v)

    def int(self, key, default=0):
        try:
            return int(self.get(key, default))
        except (TypeError, ValueError):
            return default


def parse(text):
    """'\\a\\1\\b\\2\\' -> Message('a', [('a', '1'), ('b', '2')])."""
    parts = text.split('\\')
    if parts and parts[0] == '':
        parts = parts[1:]
    pairs = []
    for i in range(0, len(parts) - 1, 2):
        pairs.append((parts[i], parts[i + 1]))
    if len(parts) % 2 == 1 and parts[-1] != '':
        pairs.append((parts[-1], ''))
    command = pairs[0][0].lower() if pairs else ''
    return Message(command, pairs)


def build(*pairs, final=True):
    """build(('lc', 1), ('challenge', 'X')) -> '\\lc\\1\\challenge\\X\\final\\'."""
    out = ''.join('\\%s\\%s' % (k, '' if v is None else v) for k, v in pairs)
    return out + ('\\final\\' if final else '')


class Splitter:
    """Splits a TCP stream into messages ending in \\final\\."""

    def __init__(self):
        self.buf = ''

    def feed(self, text):
        self.buf += text
        while True:
            i = self.buf.find('\\final\\')
            if i < 0:
                return
            msg, self.buf = self.buf[:i], self.buf[i + 7:]
            if msg.strip():
                yield parse(msg)
