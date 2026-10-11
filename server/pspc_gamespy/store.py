# SPDX-License-Identifier: GPL-2.0-or-later
"""What the GameSpy servers keep between runs (SQLite): GameSpy accounts (users: e-mail and password)
and their profiles (nick, unique nick), buddy lists and requests, blocks, offline messages, the
games' persistent player data (the stats server's getpd/setpd), and the stats of finished games."""

import sqlite3
import threading
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    userid INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT NOT NULL COLLATE NOCASE,
    password_md5 TEXT NOT NULL,
    created INTEGER NOT NULL,
    banned INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS users_email ON users (email);
CREATE TABLE IF NOT EXISTS profiles (
    profileid INTEGER PRIMARY KEY AUTOINCREMENT,
    userid INTEGER NOT NULL,
    nick TEXT NOT NULL COLLATE NOCASE,
    uniquenick TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
    namespaceid INTEGER NOT NULL DEFAULT 0,
    firstname TEXT NOT NULL DEFAULT '',
    lastname TEXT NOT NULL DEFAULT '',
    countrycode TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL,
    last_login INTEGER NOT NULL DEFAULT 0,
    deleted INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS profiles_user ON profiles (userid);
CREATE INDEX IF NOT EXISTS profiles_nick ON profiles (nick);
CREATE INDEX IF NOT EXISTS profiles_uniquenick ON profiles (uniquenick, namespaceid);
CREATE TABLE IF NOT EXISTS buddies (
    profileid INTEGER NOT NULL,
    buddyid INTEGER NOT NULL,
    PRIMARY KEY (profileid, buddyid)
);
CREATE TABLE IF NOT EXISTS buddy_requests (
    fromid INTEGER NOT NULL,
    toid INTEGER NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL,
    PRIMARY KEY (fromid, toid)
);
CREATE TABLE IF NOT EXISTS blocks (
    profileid INTEGER NOT NULL,
    blockedid INTEGER NOT NULL,
    PRIMARY KEY (profileid, blockedid)
);
CREATE TABLE IF NOT EXISTS messages (
    message_id INTEGER PRIMARY KEY AUTOINCREMENT,
    fromid INTEGER NOT NULL,
    toid INTEGER NOT NULL,
    type INTEGER NOT NULL DEFAULT 1,
    text TEXT NOT NULL,
    created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS persist (
    profileid INTEGER NOT NULL,
    ptype INTEGER NOT NULL,
    dindex INTEGER NOT NULL,
    data BLOB NOT NULL,
    modified INTEGER NOT NULL,
    PRIMARY KEY (profileid, ptype, dindex)
);
CREATE TABLE IF NOT EXISTS games (
    game_id INTEGER PRIMARY KEY AUTOINCREMENT,
    gamename TEXT NOT NULL,
    host_profileid INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL DEFAULT '',
    started INTEGER NOT NULL,
    finished INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS player_stats (
    profileid INTEGER NOT NULL,
    gamename TEXT NOT NULL,
    key TEXT NOT NULL,
    value REAL NOT NULL DEFAULT 0,
    PRIMARY KEY (profileid, gamename, key)
);
"""

# Profile and user ids start high, like GameSpy's (games may treat small ids as invalid).
FIRST_USERID = 10000000
FIRST_PROFILEID = 100000000


class StoreError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


class Store:
    def __init__(self, path):
        self._db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._db.row_factory = sqlite3.Row
        self._lock = threading.RLock()
        with self._lock:
            self._db.execute('PRAGMA journal_mode=WAL')
            self._db.executescript(SCHEMA)
            for table, first in (('users', FIRST_USERID), ('profiles', FIRST_PROFILEID)):
                if not self.one('SELECT name FROM sqlite_sequence WHERE name = ?', (table,)):
                    self._db.execute('INSERT INTO sqlite_sequence (name, seq) VALUES (?, ?)', (table, first))

    def close(self):
        self._db.close()

    def q(self, sql, args=()):
        with self._lock:
            return self._db.execute(sql, args).fetchall()

    def one(self, sql, args=()):
        with self._lock:
            return self._db.execute(sql, args).fetchone()

    def x(self, sql, args=()):
        with self._lock:
            cur = self._db.execute(sql, args)
            return cur.lastrowid

    # ---- users and profiles

    def users_by_email(self, email):
        return self.q('SELECT * FROM users WHERE email = ? ORDER BY userid', (email,))

    def user(self, userid):
        return self.one('SELECT * FROM users WHERE userid = ?', (userid,))

    def profile(self, profileid):
        return self.one('SELECT p.*, u.email, u.banned FROM profiles p JOIN users u ON u.userid = p.userid '
                        'WHERE p.profileid = ? AND p.deleted = 0', (profileid,))

    def profiles_of(self, userid):
        return self.q('SELECT * FROM profiles WHERE userid = ? AND deleted = 0 ORDER BY profileid', (userid,))

    def profile_by_nick_email(self, nick, email, namespaceid=None):
        rows = self.q('SELECT p.*, u.email, u.password_md5, u.banned FROM profiles p JOIN users u ON u.userid = p.userid '
                      'WHERE p.nick = ? AND u.email = ? AND p.deleted = 0 ORDER BY p.profileid', (nick, email))
        return rows[0] if rows else None

    def profile_by_uniquenick(self, uniquenick, namespaceid=0):
        return self.one('SELECT p.*, u.email, u.password_md5, u.banned FROM profiles p JOIN users u ON u.userid = p.userid '
                        'WHERE p.uniquenick = ? AND p.uniquenick != \'\' AND p.deleted = 0 ORDER BY p.profileid', (uniquenick,))

    def nick_taken(self, nick):
        return self.one('SELECT 1 FROM profiles WHERE nick = ? AND deleted = 0', (nick,)) is not None

    def uniquenick_taken(self, uniquenick):
        return bool(uniquenick) and self.one('SELECT 1 FROM profiles WHERE uniquenick = ? AND deleted = 0', (uniquenick,)) is not None

    def new_user(self, email, password_md5, nick, uniquenick='', namespaceid=0):
        """A user with its first profile; an existing user (same e-mail and password) gets the profile.
        Returns (userid, profileid). StoreError(code) with GP's new-user error codes."""
        with self._lock:
            users = self.users_by_email(email)
            user = next((u for u in users if u['password_md5'] == password_md5), None)
            if users and not user:
                raise StoreError(514, 'The password does not match the e-mail address.')
            if uniquenick and self.uniquenick_taken(uniquenick):
                raise StoreError(516, 'That unique nick is already in use.')
            now = int(time.time())
            if user:
                existing = self.one('SELECT profileid FROM profiles WHERE userid = ? AND nick = ? AND deleted = 0',
                                    (user['userid'], nick))
                if existing:
                    raise StoreError(513, 'A profile with that nick already exists.')
                userid = user['userid']
            else:
                userid = self.x('INSERT INTO users (email, password_md5, created) VALUES (?, ?, ?)', (email, password_md5, now))
            profileid = self.x('INSERT INTO profiles (userid, nick, uniquenick, namespaceid, created) VALUES (?, ?, ?, ?, ?)',
                               (userid, nick, uniquenick, namespaceid, now))
            return userid, profileid

    def new_profile(self, userid, nick):
        if self.one('SELECT 1 FROM profiles WHERE userid = ? AND nick = ? AND deleted = 0', (userid, nick)):
            raise StoreError(1025, 'A profile with that nick already exists.')
        return self.x('INSERT INTO profiles (userid, nick, created) VALUES (?, ?, ?)', (userid, nick, int(time.time())))

    def delete_profile(self, profileid):
        self.x('UPDATE profiles SET deleted = 1 WHERE profileid = ?', (profileid,))

    def register_uniquenick(self, profileid, uniquenick):
        if self.uniquenick_taken(uniquenick):
            raise StoreError(516, 'That unique nick is already in use.')
        self.x('UPDATE profiles SET uniquenick = ? WHERE profileid = ?', (uniquenick, profileid))

    def update_profile(self, profileid, fields):
        allowed = {k: v for k, v in fields.items() if k in ('nick', 'firstname', 'lastname', 'countrycode')}
        if allowed:
            self.x('UPDATE profiles SET %s WHERE profileid = ?' % ', '.join('%s = ?' % k for k in allowed),
                   tuple(allowed.values()) + (profileid,))

    def touch_login(self, profileid):
        self.x('UPDATE profiles SET last_login = ? WHERE profileid = ?', (int(time.time()), profileid))

    def set_password(self, userid, password_md5):
        self.x('UPDATE users SET password_md5 = ? WHERE userid = ?', (password_md5, userid))

    def set_banned(self, name, banned):
        """Bans the user owning the profile with this nick or unique nick. Returns how many users changed."""
        rows = self.q('SELECT DISTINCT userid FROM profiles WHERE (nick = ? OR uniquenick = ?) AND deleted = 0', (name, name))
        for r in rows:
            self.x('UPDATE users SET banned = ? WHERE userid = ?', (1 if banned else 0, r['userid']))
        return len(rows)

    def list_profiles(self):
        return self.q('SELECT p.profileid, p.userid, p.nick, p.uniquenick, p.created, p.last_login, u.email, u.banned '
                      'FROM profiles p JOIN users u ON u.userid = p.userid WHERE p.deleted = 0 ORDER BY p.profileid')

    def search(self, nick='', uniquenick='', email='', firstname='', lastname='', limit=50):
        where, args = ['p.deleted = 0', 'u.banned = 0'], []
        for col, val in (('p.nick', nick), ('p.uniquenick', uniquenick), ('u.email', email), ('p.firstname', firstname),
                         ('p.lastname', lastname)):
            if val:
                where.append('%s = ?' % col)
                args.append(val)
        if len(args) == 0:
            return []
        return self.q('SELECT p.*, u.email FROM profiles p JOIN users u ON u.userid = p.userid WHERE %s ORDER BY p.profileid LIMIT ?'
                      % ' AND '.join(where), tuple(args) + (limit,))

    # ---- buddies

    def buddies(self, profileid):
        return [r['buddyid'] for r in self.q('SELECT buddyid FROM buddies WHERE profileid = ? ORDER BY buddyid', (profileid,))]

    def buddy_of(self, profileid):
        """Profiles that have profileid on their list."""
        return [r['profileid'] for r in self.q('SELECT profileid FROM buddies WHERE buddyid = ?', (profileid,))]

    def add_buddy_request(self, fromid, toid, reason):
        self.x('INSERT OR REPLACE INTO buddy_requests (fromid, toid, reason, created) VALUES (?, ?, ?, ?)',
               (fromid, toid, reason, int(time.time())))

    def pending_requests(self, toid):
        return self.q('SELECT * FROM buddy_requests WHERE toid = ? ORDER BY created', (toid,))

    def authorize(self, fromid, toid):
        """toid authorized fromid's request: fromid gets toid on its list (and toid fromid, as GP does
        for a mutual list)."""
        with self._lock:
            self.x('DELETE FROM buddy_requests WHERE fromid = ? AND toid = ?', (fromid, toid))
            self.x('INSERT OR IGNORE INTO buddies (profileid, buddyid) VALUES (?, ?)', (fromid, toid))

    def has_request(self, fromid, toid):
        return self.one('SELECT 1 FROM buddy_requests WHERE fromid = ? AND toid = ?', (fromid, toid)) is not None

    def del_buddy(self, profileid, buddyid):
        self.x('DELETE FROM buddies WHERE profileid = ? AND buddyid = ?', (profileid, buddyid))

    def blocks(self, profileid):
        return [r['blockedid'] for r in self.q('SELECT blockedid FROM blocks WHERE profileid = ?', (profileid,))]

    def add_block(self, profileid, blockedid):
        self.x('INSERT OR IGNORE INTO blocks (profileid, blockedid) VALUES (?, ?)', (profileid, blockedid))

    def remove_block(self, profileid, blockedid):
        self.x('DELETE FROM blocks WHERE profileid = ? AND blockedid = ?', (profileid, blockedid))

    # ---- offline messages

    def queue_message(self, fromid, toid, mtype, text):
        self.x('INSERT INTO messages (fromid, toid, type, text, created) VALUES (?, ?, ?, ?, ?)',
               (fromid, toid, mtype, text, int(time.time())))

    def take_messages(self, toid):
        with self._lock:
            rows = self.q('SELECT * FROM messages WHERE toid = ? ORDER BY message_id', (toid,))
            self.x('DELETE FROM messages WHERE toid = ?', (toid,))
            return rows

    # ---- persistent data (stats server)

    def get_persist(self, profileid, ptype, dindex):
        row = self.one('SELECT data, modified FROM persist WHERE profileid = ? AND ptype = ? AND dindex = ?',
                       (profileid, ptype, dindex))
        return (bytes(row['data']), row['modified']) if row else (b'', 0)

    def set_persist(self, profileid, ptype, dindex, data):
        now = int(time.time())
        self.x('INSERT OR REPLACE INTO persist (profileid, ptype, dindex, data, modified) VALUES (?, ?, ?, ?, ?)',
               (profileid, ptype, dindex, data, now))
        return now

    # ---- finished games and player stats

    def new_game(self, gamename, host_profileid=0):
        return self.x('INSERT INTO games (gamename, host_profileid, started) VALUES (?, ?, ?)',
                      (gamename, host_profileid, int(time.time())))

    def update_game(self, game_id, data, finished):
        self.x('UPDATE games SET data = ?, finished = ? WHERE game_id = ?', (data, int(time.time()) if finished else 0, game_id))

    def add_stats(self, profileid, gamename, values):
        """Adds to a player's totals (key -> number)."""
        with self._lock:
            for key, value in values.items():
                self.x('INSERT INTO player_stats (profileid, gamename, key, value) VALUES (?, ?, ?, ?) '
                       'ON CONFLICT (profileid, gamename, key) DO UPDATE SET value = value + excluded.value',
                       (profileid, gamename, key, value))

    def stats(self, profileid, gamename):
        return {r['key']: r['value'] for r in self.q('SELECT key, value FROM player_stats WHERE profileid = ? AND gamename = ?',
                                                     (profileid, gamename))}
