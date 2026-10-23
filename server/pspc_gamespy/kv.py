# SPDX-License-Identifier: GPL-2.0-or-later
"""GameSpy's text messages: \\command\\\\key\\value\\...\\final\\ (Presence, search, stats)."""

MAX_MESSAGE = 1 << 16       # the longest message (or unfinished one) a connection may send
MAX_INT = 1 << 62           # numbers beyond this are not numbers any game sends; SQLite stores 64 bits


class TooLong(ValueError):
    """A client sent more than MAX_MESSAGE without ending the message."""


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
            value = int(self.get(key, default))
        except (TypeError, ValueError):
            return default
        return value if -MAX_INT <= value <= MAX_INT else default


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
    """build(('lc', 1), ('challenge', 'X')) -> '\\lc\\1\\challenge\\X\\final\\'. A value can't contain
    the separator: text a player sent (a nick, a message) that did would let them write messages of
    their own into another player's stream, so the backslashes are dropped."""
    out = ''.join('\\%s\\%s' % (k, '' if v is None else str(v).replace('\\', '')) for k, v in pairs)
    return out + ('\\final\\' if final else '')


class Splitter:
    """Splits a TCP stream into messages ending in \\final\\."""

    def __init__(self):
        self.buf = ''

    def feed(self, text):
        """The complete messages in what has arrived so far; TooLong for a client that never finishes one."""
        self.buf += text
        while True:
            i = self.buf.find('\\final\\')
            if i < 0:
                if len(self.buf) > MAX_MESSAGE:
                    raise TooLong('a message longer than %d bytes' % MAX_MESSAGE)
                return
            msg, self.buf = self.buf[:i], self.buf[i + 7:]
            if msg.strip():
                yield parse(msg)

    def pending(self, limit=12):
        """What the client sent that never became a message, for a log: the keys of an unfinished message (not their
        values: a password is one of them), or the first characters of something that is not one."""
        if not self.buf:
            return 'nothing'
        if self.buf.startswith('\\'):
            return 'unfinished message with the keys ' + ', '.join(self.buf.split('\\')[1:2 * limit + 1:2])
        return 'something that does not start with a backslash: ' + self.buf[:16]
