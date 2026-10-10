# SPDX-License-Identifier: GPL-2.0-or-later
"""Accounts, sessions and friends in SQLite.

Passwords are kept as scrypt hashes; session tokens only as SHA-256 hashes, so a copy of the
database lets nobody sign in. The ticket signing key is generated on first use and lives in the
database too (table settings).
"""

import hashlib
import hmac
import os
import re
import secrets
import sqlite3
import threading
import time

ONLINE_ID_RE = re.compile(r'^[A-Za-z][A-Za-z0-9_-]{2,15}$')  # PSN's rules: 3-16 characters
COUNTRY_RE = re.compile(r'^[a-z]{2}$')
SESSION_DAYS = 180
SCRYPT = dict(n=2 ** 14, r=8, p=1, dklen=32)

SCHEMA = '''
CREATE TABLE IF NOT EXISTS accounts (
    id INTEGER PRIMARY KEY,
    account_id INTEGER NOT NULL UNIQUE,
    online_id TEXT NOT NULL UNIQUE COLLATE NOCASE,
    pw_salt BLOB NOT NULL,
    pw_hash BLOB NOT NULL,
    country TEXT NOT NULL DEFAULT 'us',
    created INTEGER NOT NULL,
    banned INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS sessions (
    token_hash BLOB PRIMARY KEY,
    account INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created INTEGER NOT NULL,
    expires INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS friend_requests (
    from_account INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    to_account INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    created INTEGER NOT NULL,
    PRIMARY KEY (from_account, to_account)
);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value BLOB NOT NULL
);
'''


class StoreError(Exception):
    """An error the API reports to the client: (HTTP status, code, message)."""

    def __init__(self, status, code, message):
        super().__init__(message)
        self.status, self.code, self.message = status, code, message


def _hash_password(password, salt):
    return hashlib.scrypt(password.encode('utf-8'), salt=salt, **SCRYPT)


def _token_hash(token):
    return hashlib.sha256(token.encode('ascii', 'replace')).digest()


class Store:
    def __init__(self, path):
        self._db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._db.row_factory = sqlite3.Row
        self._lock = threading.Lock()
        with self._lock:
            self._db.execute('PRAGMA foreign_keys = ON')
            self._db.execute('PRAGMA journal_mode = WAL')
            self._db.executescript(SCHEMA)

    def close(self):
        self._db.close()

    def _q(self, sql, args=()):
        with self._lock:
            return self._db.execute(sql, args).fetchall()

    # ---- settings

    def ticket_key(self):
        """The ticket signing key (32 bytes), made on first use."""
        with self._lock:
            row = self._db.execute("SELECT value FROM settings WHERE key = 'ticket_key'").fetchone()
            if row:
                return bytes(row['value'])
            key = secrets.token_bytes(32)
            self._db.execute("INSERT INTO settings (key, value) VALUES ('ticket_key', ?)", (key,))
            return key

    # ---- accounts

    def create_account(self, online_id, password, country='us'):
        if not isinstance(online_id, str) or not ONLINE_ID_RE.match(online_id):
            raise StoreError(400, 'invalid_online_id',
                             'Online IDs are 3-16 letters, digits, - or _, starting with a letter.')
        if not isinstance(password, str) or len(password) < 8:
            raise StoreError(400, 'weak_password', 'Passwords need at least 8 characters.')
        country = (country or 'us').lower()
        if not COUNTRY_RE.match(country):
            raise StoreError(400, 'invalid_country', 'The country is a two-letter code such as "us".')
        salt = os.urandom(16)
        pw_hash = _hash_password(password, salt)
        account_id = secrets.randbits(63) | 1
        try:
            self._q('INSERT INTO accounts (account_id, online_id, pw_salt, pw_hash, country, created) '
                    'VALUES (?, ?, ?, ?, ?, ?)',
                    (account_id, online_id, salt, pw_hash, country, int(time.time())))
        except sqlite3.IntegrityError:
            raise StoreError(409, 'online_id_taken', 'That online ID is already taken.')
        return self.account_by_online_id(online_id)

    def account_by_online_id(self, online_id):
        rows = self._q('SELECT * FROM accounts WHERE online_id = ?', (online_id,))
        return dict(rows[0]) if rows else None

    def check_password(self, online_id, password):
        account = self.account_by_online_id(online_id) if isinstance(online_id, str) else None
        if not account or not isinstance(password, str):
            # Same work either way, so timing doesn't tell which online IDs exist.
            _hash_password(password if isinstance(password, str) else '', b'\0' * 16)
            raise StoreError(401, 'bad_credentials', 'Wrong online ID or password.')
        if not hmac.compare_digest(_hash_password(password, bytes(account['pw_salt'])), bytes(account['pw_hash'])):
            raise StoreError(401, 'bad_credentials', 'Wrong online ID or password.')
        if account['banned']:
            raise StoreError(403, 'banned', 'This account is suspended.')
        return account

    def set_password(self, account, password):
        if not isinstance(password, str) or len(password) < 8:
            raise StoreError(400, 'weak_password', 'Passwords need at least 8 characters.')
        salt = os.urandom(16)
        self._q('UPDATE accounts SET pw_salt = ?, pw_hash = ? WHERE id = ?',
                (salt, _hash_password(password, salt), account['id']))
        self._q('DELETE FROM sessions WHERE account = ?', (account['id'],))

    def set_banned(self, online_id, banned):
        account = self.account_by_online_id(online_id)
        if not account:
            raise StoreError(404, 'no_such_account', 'No account with that online ID.')
        self._q('UPDATE accounts SET banned = ? WHERE id = ?', (1 if banned else 0, account['id']))
        if banned:
            self._q('DELETE FROM sessions WHERE account = ?', (account['id'],))

    def list_accounts(self):
        return [dict(r) for r in self._q('SELECT online_id, account_id, country, created, banned FROM accounts '
                                         'ORDER BY online_id')]

    # ---- sessions

    def create_session(self, account):
        token = secrets.token_urlsafe(32)
        now = int(time.time())
        self._q('INSERT INTO sessions (token_hash, account, created, expires) VALUES (?, ?, ?, ?)',
                (_token_hash(token), account['id'], now, now + SESSION_DAYS * 86400))
        return token

    def account_by_token(self, token):
        if not token:
            return None
        rows = self._q('SELECT a.* FROM sessions s JOIN accounts a ON a.id = s.account '
                       'WHERE s.token_hash = ? AND s.expires > ?', (_token_hash(token), int(time.time())))
        if not rows or rows[0]['banned']:
            return None
        return dict(rows[0])

    def delete_session(self, token):
        self._q('DELETE FROM sessions WHERE token_hash = ?', (_token_hash(token),))

    # ---- friends: a request each way makes two accounts friends

    def friends(self, account):
        me = account['id']
        sent = {r['online_id'] for r in self._q(
            'SELECT a.online_id FROM friend_requests f JOIN accounts a ON a.id = f.to_account '
            'WHERE f.from_account = ?', (me,))}
        received = {r['online_id'] for r in self._q(
            'SELECT a.online_id FROM friend_requests f JOIN accounts a ON a.id = f.from_account '
            'WHERE f.to_account = ?', (me,))}
        out = []
        for name in sorted(sent | received, key=str.lower):
            status = 'friend' if name in sent and name in received else 'sent' if name in sent else 'received'
            out.append({'online_id': name, 'status': status})
        return out

    def add_friend(self, account, online_id):
        other = self.account_by_online_id(online_id) if isinstance(online_id, str) else None
        if not other or other['banned']:
            raise StoreError(404, 'no_such_account', 'No account with that online ID.')
        if other['id'] == account['id']:
            raise StoreError(400, 'self', "You can't add yourself.")
        self._q('INSERT OR IGNORE INTO friend_requests (from_account, to_account, created) VALUES (?, ?, ?)',
                (account['id'], other['id'], int(time.time())))
        return next(f for f in self.friends(account) if f['online_id'].lower() == other['online_id'].lower())

    def remove_friend(self, account, online_id):
        other = self.account_by_online_id(online_id) if isinstance(online_id, str) else None
        if not other:
            raise StoreError(404, 'no_such_account', 'No account with that online ID.')
        self._q('DELETE FROM friend_requests WHERE (from_account = ? AND to_account = ?) '
                'OR (from_account = ? AND to_account = ?)', (account['id'], other['id'], other['id'], account['id']))
