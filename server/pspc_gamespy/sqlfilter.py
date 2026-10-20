# SPDX-License-Identifier: GPL-2.0-or-later
"""The server browser's filters: SQL-like expressions over a server's keys, such as
"gamever='1.0' and numplayers < maxplayers and hostname like '%fun%'".

Identifiers are server keys (a missing key is ''), comparisons are numeric when both sides are
numbers and case-insensitive text otherwise. A filter that doesn't parse lets everything through.

Filters come from anyone who can reach the server, so they are bounded: MAX_FILTER characters, MAX_DEPTH
levels of parentheses and `not`, and LIKE patterns are matched in time proportional to the product of
the lengths (no regular expressions, which a pattern such as %a%a%a%a%b makes exponential)."""

import re

MAX_FILTER = 1024
MAX_DEPTH = 24
MAX_VALUE = 512         # LIKE looks at no more than this much of a key's value

_TOKEN = re.compile(r"""\s*(?:
    (?P<num>-?\d+(?:\.\d+)?) |
    (?P<str>'(?:[^']|'')*'|"(?:[^"]|"")*") |
    (?P<op><>|!=|==|<=|>=|&&|\|\||[=<>()!]) |
    (?P<id>[A-Za-z_][A-Za-z0-9_.]*)
)""", re.X)

_KEYWORDS = {'and', 'or', 'not', 'like'}


class FilterError(ValueError):
    pass


def _tokens(text):
    out = []
    pos = 0
    text = text.strip()
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise FilterError('bad filter near %r' % text[pos:pos + 20])
        pos = m.end()
        if m.group('num') is not None:
            out.append(('val', m.group('num')))
        elif m.group('str') is not None:
            s = m.group('str')
            out.append(('val', s[1:-1].replace(s[0] * 2, s[0])))
        elif m.group('op') is not None:
            op = {'&&': 'and', '||': 'or', '!': 'not', '==': '='}.get(m.group('op'), m.group('op'))
            out.append(('kw' if op in _KEYWORDS else 'op', op))
        else:
            word = m.group('id')
            out.append(('kw', word.lower()) if word.lower() in _KEYWORDS else ('id', word))
    return out


def _number(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _like(value, pattern):
    """SQL LIKE, case-insensitive: % is any text, _ any one character. Greedy matching with one
    backtrack point (the last %), so it never takes more than len(value) * len(pattern) steps."""
    value, pattern = value[:MAX_VALUE].lower(), pattern[:MAX_VALUE].lower()
    v = p = 0
    star = mark = -1
    while v < len(value):
        if p < len(pattern) and pattern[p] == '%':
            star, mark = p, v
            p += 1
        elif p < len(pattern) and pattern[p] in ('_', value[v]):
            p += 1
            v += 1
        elif star >= 0:
            p = star + 1
            mark += 1
            v = mark
        else:
            return False
    return pattern[p:].strip('%') == ''


def _compare(a, op, b):
    if op == 'like':
        return _like(a, b)
    na, nb = _number(a), _number(b)
    if na is not None and nb is not None:
        a, b = na, nb
    else:
        a, b = a.lower(), b.lower()
    return {'=': a == b, '<>': a != b, '!=': a != b, '<': a < b, '>': a > b, '<=': a <= b, '>=': a >= b}[op]


class Filter:
    def __init__(self, text):
        self.text = text or ''
        self.ok = True
        self.tree = None
        self.depth = 0
        if self.text.strip():
            try:
                if len(self.text) > MAX_FILTER:
                    raise FilterError('filter longer than %d characters' % MAX_FILTER)
                self.toks = _tokens(self.text)
                self.i = 0
                self.tree = self._or()
                if self.i != len(self.toks):
                    raise FilterError('trailing tokens in filter')
            except FilterError:
                self.ok = False
                self.tree = None

    # ---- parsing (recursive descent into nested tuples)

    def _peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def _take(self):
        tok = self._peek()
        if tok[0] is None:
            raise FilterError('unexpected end of filter')
        self.i += 1
        return tok

    def _or(self):
        node = self._and()
        while self._peek() == ('kw', 'or'):
            self.i += 1
            node = ('or', node, self._and())
        return node

    def _and(self):
        node = self._not()
        while self._peek() == ('kw', 'and'):
            self.i += 1
            node = ('and', node, self._not())
        return node

    def _not(self):
        if self._peek() == ('kw', 'not'):
            self.i += 1
            return ('not', self._nested(self._not))
        return self._cmp()

    def _nested(self, parse):
        self.depth += 1
        if self.depth > MAX_DEPTH:
            raise FilterError('filter nested more than %d levels' % MAX_DEPTH)
        try:
            return parse()
        finally:
            self.depth -= 1

    def _operand(self):
        kind, value = self._take()
        if kind not in ('val', 'id'):
            raise FilterError('expected a value, got %r' % value)
        return (kind, value)

    def _cmp(self):
        if self._peek() == ('op', '('):
            self.i += 1
            node = self._nested(self._or)
            if self._take() != ('op', ')'):
                raise FilterError('missing )')
            return node
        left = self._operand()
        kind, value = self._peek()
        negate = False
        if (kind, value) == ('kw', 'not') and self.i + 1 < len(self.toks) and self.toks[self.i + 1] == ('kw', 'like'):
            self.i += 1
            negate = True
            kind, value = self._peek()
        if kind == 'op' and value in ('=', '<>', '!=', '<', '>', '<=', '>=') or (kind, value) == ('kw', 'like'):
            self.i += 1
            node = ('cmp', left, value, self._operand())
            return ('not', node) if negate else node
        return ('truth', left)

    # ---- evaluating

    def matches(self, keys):
        if self.tree is None:
            return True
        return bool(self._eval(self.tree, keys))

    def _value(self, operand, keys):
        kind, value = operand
        return str(keys.get(value, '')) if kind == 'id' else value

    def _eval(self, node, keys):
        op = node[0]
        if op == 'or':
            return self._eval(node[1], keys) or self._eval(node[2], keys)
        if op == 'and':
            return self._eval(node[1], keys) and self._eval(node[2], keys)
        if op == 'not':
            return not self._eval(node[1], keys)
        if op == 'cmp':
            return _compare(self._value(node[1], keys), node[2], self._value(node[3], keys))
        v = self._value(node[1], keys)
        n = _number(v)
        return n != 0 if n is not None else bool(v)
