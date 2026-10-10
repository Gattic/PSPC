# SPDX-License-Identifier: GPL-2.0-or-later
"""What the game server keeps between runs (SQLite): Medius accounts (one per online ID), their
stats, buddy and ignore lists, clans ("cells" in Combat Ops), clan messages and invitations, files,
and user-made missions."""

import sqlite3
import threading
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS accounts (
    account_id INTEGER PRIMARY KEY AUTOINCREMENT,
    app_id INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE NOCASE,
    created INTEGER NOT NULL,
    last_login INTEGER NOT NULL DEFAULT 0,
    banned INTEGER NOT NULL DEFAULT 0,
    stats BLOB NOT NULL DEFAULT x'',
    ladder_stats BLOB NOT NULL DEFAULT x'',
    UNIQUE (app_id, name)
);
CREATE TABLE IF NOT EXISTS buddies (
    account_id INTEGER NOT NULL,
    buddy_id INTEGER NOT NULL,
    confirmed INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY (account_id, buddy_id)
);
CREATE TABLE IF NOT EXISTS ignores (
    account_id INTEGER NOT NULL,
    ignored_id INTEGER NOT NULL,
    PRIMARY KEY (account_id, ignored_id)
);
CREATE TABLE IF NOT EXISTS clans (
    clan_id INTEGER PRIMARY KEY AUTOINCREMENT,
    app_id INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE NOCASE,
    leader_id INTEGER NOT NULL,
    stats BLOB NOT NULL DEFAULT x'',
    status INTEGER NOT NULL DEFAULT 0,
    created INTEGER NOT NULL,
    UNIQUE (app_id, name)
);
CREATE TABLE IF NOT EXISTS clan_members (
    clan_id INTEGER NOT NULL,
    account_id INTEGER NOT NULL,
    PRIMARY KEY (clan_id, account_id)
);
CREATE TABLE IF NOT EXISTS clan_invitations (
    invitation_id INTEGER PRIMARY KEY AUTOINCREMENT,
    clan_id INTEGER NOT NULL,
    account_id INTEGER NOT NULL,
    message TEXT NOT NULL DEFAULT '',
    response INTEGER NOT NULL DEFAULT 0,
    response_message TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL,
    UNIQUE (clan_id, account_id)
);
CREATE TABLE IF NOT EXISTS clan_messages (
    message_id INTEGER PRIMARY KEY AUTOINCREMENT,
    clan_id INTEGER NOT NULL,
    text TEXT NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS files (
    name TEXT PRIMARY KEY,
    owner_id INTEGER NOT NULL,
    data BLOB NOT NULL DEFAULT x'',
    attributes BLOB NOT NULL DEFAULT x'',
    created INTEGER NOT NULL,
    modified INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS missions (
    mission_id INTEGER PRIMARY KEY AUTOINCREMENT,
    hash TEXT NOT NULL UNIQUE,
    account_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    author TEXT NOT NULL,
    theme INTEGER NOT NULL DEFAULT 0,
    combat_type INTEGER NOT NULL DEFAULT 0,
    terrain INTEGER NOT NULL DEFAULT 0,
    time_of_day INTEGER NOT NULL DEFAULT 0,
    weather INTEGER NOT NULL DEFAULT 0,
    file_id TEXT NOT NULL DEFAULT '',
    downloads INTEGER NOT NULL DEFAULT 0,
    plays INTEGER NOT NULL DEFAULT 0,
    play_time INTEGER NOT NULL DEFAULT 0,
    team_a_wins INTEGER NOT NULL DEFAULT 0,
    team_b_wins INTEGER NOT NULL DEFAULT 0,
    official INTEGER NOT NULL DEFAULT 0,
    inappropriate INTEGER NOT NULL DEFAULT 0,
    hidden INTEGER NOT NULL DEFAULT 0,
    created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS mission_lists (
    account_id INTEGER NOT NULL,
    list_type INTEGER NOT NULL,
    mission_id INTEGER NOT NULL,
    added INTEGER NOT NULL,
    PRIMARY KEY (account_id, list_type, mission_id)
);
CREATE TABLE IF NOT EXISTS mission_votes (
    account_id INTEGER NOT NULL,
    mission_id INTEGER NOT NULL,
    vote INTEGER NOT NULL,
    PRIMARY KEY (account_id, mission_id)
);
CREATE TABLE IF NOT EXISTS clan_game_stats (
    clan_id INTEGER NOT NULL,
    mode TEXT NOT NULL,
    plays INTEGER NOT NULL DEFAULT 0,
    wins INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (clan_id, mode)
);
CREATE TABLE IF NOT EXISTS blobs (
    file_id TEXT PRIMARY KEY,
    owner_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    data BLOB NOT NULL,
    created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS game_reports (
    report_id INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id INTEGER NOT NULL,
    kind TEXT NOT NULL,
    query TEXT NOT NULL,
    data BLOB NOT NULL,
    created INTEGER NOT NULL
);
"""


class Store:
    def __init__(self, path):
        self._db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._db.row_factory = sqlite3.Row
        self._lock = threading.Lock()
        with self._lock:
            self._db.execute('PRAGMA journal_mode = WAL')
            self._db.executescript(SCHEMA)

    def close(self):
        self._db.close()

    def q(self, sql, args=()):
        with self._lock:
            return self._db.execute(sql, args).fetchall()

    def x(self, sql, args=()):
        """Executes a write; returns lastrowid."""
        with self._lock:
            return self._db.execute(sql, args).lastrowid

    def one(self, sql, args=()):
        rows = self.q(sql, args)
        return rows[0] if rows else None

    # ---- accounts

    def account(self, app_id, name):
        """The account for an online ID, created on first use."""
        row = self.one('SELECT * FROM accounts WHERE app_id = ? AND name = ?', (app_id, name))
        if row is None:
            self.x('INSERT OR IGNORE INTO accounts (app_id, name, created) VALUES (?, ?, ?)', (app_id, name, int(time.time())))
            row = self.one('SELECT * FROM accounts WHERE app_id = ? AND name = ?', (app_id, name))
        return row

    def find_account(self, app_id, name):
        return self.one('SELECT * FROM accounts WHERE app_id = ? AND name = ?', (app_id, name))

    def account_by_id(self, account_id):
        return self.one('SELECT * FROM accounts WHERE account_id = ?', (account_id,))

    def touch_login(self, account_id):
        self.x('UPDATE accounts SET last_login = ? WHERE account_id = ?', (int(time.time()), account_id))

    def set_stats(self, account_id, stats):
        self.x('UPDATE accounts SET stats = ? WHERE account_id = ?', (bytes(stats), account_id))

    def set_ladder_stats(self, account_id, stats):
        self.x('UPDATE accounts SET ladder_stats = ? WHERE account_id = ?', (bytes(stats), account_id))

    def set_banned(self, app_id, name, banned):
        self.x('UPDATE accounts SET banned = ? WHERE app_id = ? AND name = ?', (1 if banned else 0, app_id, name))

    # ---- buddies and ignore lists

    def buddies(self, account_id):
        return self.q('SELECT a.account_id, a.name FROM buddies b JOIN accounts a ON a.account_id = b.buddy_id '
                      'WHERE b.account_id = ? AND b.confirmed = 1 ORDER BY a.name', (account_id,))

    def add_buddy(self, account_id, buddy_id, confirmed=1):
        self.x('INSERT OR REPLACE INTO buddies (account_id, buddy_id, confirmed) VALUES (?, ?, ?)',
               (account_id, buddy_id, confirmed))

    def remove_buddy(self, account_id, buddy_id):
        self.x('DELETE FROM buddies WHERE account_id = ? AND buddy_id = ?', (account_id, buddy_id))

    def ignores(self, account_id):
        return self.q('SELECT a.account_id, a.name FROM ignores i JOIN accounts a ON a.account_id = i.ignored_id '
                      'WHERE i.account_id = ? ORDER BY a.name', (account_id,))

    def add_ignore(self, account_id, ignored_id):
        self.x('INSERT OR IGNORE INTO ignores (account_id, ignored_id) VALUES (?, ?)', (account_id, ignored_id))

    def remove_ignore(self, account_id, ignored_id):
        self.x('DELETE FROM ignores WHERE account_id = ? AND ignored_id = ?', (account_id, ignored_id))

    # ---- clans

    def clan(self, clan_id):
        return self.one('SELECT * FROM clans WHERE clan_id = ?', (clan_id,))

    def clans_of(self, account_id):
        return self.q('SELECT c.* FROM clans c JOIN clan_members m ON m.clan_id = c.clan_id WHERE m.account_id = ? '
                      'ORDER BY c.clan_id', (account_id,))

    def is_ignoring(self, account_id, other_id):
        return self.one('SELECT 1 FROM ignores WHERE account_id = ? AND ignored_id = ?', (account_id, other_id)) is not None

    def buddy_requests_to(self, account_id):
        """Players who added account_id as a buddy that account_id hasn't added back."""
        return self.q('SELECT a.account_id, a.name FROM buddies b JOIN accounts a ON a.account_id = b.account_id '
                      'WHERE b.buddy_id = ? AND NOT EXISTS (SELECT 1 FROM buddies r WHERE r.account_id = ? '
                      'AND r.buddy_id = b.account_id) ORDER BY a.name', (account_id, account_id))

    def clans_led_by(self, account_id):
        return self.q('SELECT * FROM clans WHERE leader_id = ? AND status = 0', (account_id,))

    def create_clan(self, app_id, name, leader_id):
        name = (name or '').strip()
        if not name or self.one('SELECT 1 FROM clans WHERE app_id = ? AND name = ? AND status = 0', (app_id, name)):
            return None
        clan_id = self.x('INSERT INTO clans (app_id, name, leader_id, created) VALUES (?, ?, ?, ?)',
                         (app_id, name, leader_id, int(time.time())))
        self.x('INSERT OR IGNORE INTO clan_members (clan_id, account_id) VALUES (?, ?)', (clan_id, leader_id))
        return clan_id

    def disband_clan(self, clan_id):
        self.x('DELETE FROM clan_members WHERE clan_id = ?', (clan_id,))
        self.x('DELETE FROM clan_invitations WHERE clan_id = ?', (clan_id,))
        self.x('DELETE FROM clan_messages WHERE clan_id = ?', (clan_id,))
        self.x('DELETE FROM clans WHERE clan_id = ?', (clan_id,))

    def clan_members(self, clan_id):
        return self.q('SELECT a.account_id, a.name, a.stats FROM clan_members m JOIN accounts a ON a.account_id = '
                      'm.account_id WHERE m.clan_id = ? ORDER BY a.name', (clan_id,))

    def is_clan_member(self, clan_id, account_id):
        return self.one('SELECT 1 FROM clan_members WHERE clan_id = ? AND account_id = ?', (clan_id, account_id)) is not None

    def remove_clan_member(self, clan_id, account_id):
        self.x('DELETE FROM clan_members WHERE clan_id = ? AND account_id = ?', (clan_id, account_id))

    def set_clan_stats(self, clan_id, stats):
        self.x('UPDATE clans SET stats = ? WHERE clan_id = ?', (bytes(stats), clan_id))

    def invite_to_clan(self, clan_id, account_id, message):
        self.x('INSERT OR REPLACE INTO clan_invitations (clan_id, account_id, message, response, created) '
               'VALUES (?, ?, ?, 0, ?)', (clan_id, account_id, message or '', int(time.time())))

    def clan_invitations_for(self, account_id):
        return self.q('SELECT i.*, c.leader_id, a.name AS leader_name FROM clan_invitations i JOIN clans c ON '
                      'c.clan_id = i.clan_id JOIN accounts a ON a.account_id = c.leader_id WHERE i.account_id = ? '
                      'AND i.response = 0 ORDER BY i.invitation_id', (account_id,))

    def respond_clan_invitation(self, invitation_id, account_id, response, message):
        inv = self.one('SELECT * FROM clan_invitations WHERE invitation_id = ? AND account_id = ?',
                       (invitation_id, account_id))
        if inv is None:
            return False
        self.x('UPDATE clan_invitations SET response = ?, response_message = ? WHERE invitation_id = ?',
               (response, message or '', invitation_id))
        if response == 1:   # accepted
            self.x('INSERT OR IGNORE INTO clan_members (clan_id, account_id) VALUES (?, ?)', (inv['clan_id'], account_id))
        return True

    def clan_messages(self, clan_id):
        return self.q('SELECT * FROM clan_messages WHERE clan_id = ? ORDER BY message_id DESC', (clan_id,))

    def add_clan_message(self, clan_id, text):
        return self.x('INSERT INTO clan_messages (clan_id, text, created) VALUES (?, ?, ?)',
                      (clan_id, text or '', int(time.time())))

    def modify_clan_message(self, clan_id, message_id, text):
        row = self.one('SELECT 1 FROM clan_messages WHERE clan_id = ? AND message_id = ?', (clan_id, message_id))
        if row is None:
            # the game edits "the" message of the day: replace the newest one
            latest = self.one('SELECT message_id FROM clan_messages WHERE clan_id = ? ORDER BY message_id DESC LIMIT 1',
                              (clan_id,))
            if latest is None:
                self.add_clan_message(clan_id, text)
                return True
            message_id = latest['message_id']
        self.x('UPDATE clan_messages SET text = ? WHERE message_id = ?', (text or '', message_id))
        return True

    # ---- missions

    def add_mission(self, info, account_id=0, official=False, file_id=None):
        """Records a mission file (umd.mission_info) and stores the file; an existing mission with the
        same hash is updated instead. Returns the mission id."""
        now = int(time.time())
        row = self.one('SELECT mission_id, file_id FROM missions WHERE hash = ?', (info['hash'],))
        file_id = (row['file_id'] if row else None) or file_id or 'm' + info['hash'][:15]
        fields = (info['name'], info['author'], info['theme'], info['combat_type'], info['terrain'],
                  info['time_of_day'], info['weather'], 1 if official else 0)
        if row is None:
            mission_id = self.x('INSERT INTO missions (name, author, theme, combat_type, terrain, time_of_day, weather, '
                                'official, hash, account_id, file_id, created) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
                                fields + (info['hash'], account_id, file_id, now))
        else:
            mission_id = row['mission_id']
            self.x('UPDATE missions SET name = ?, author = ?, theme = ?, combat_type = ?, terrain = ?, time_of_day = ?, '
                   'weather = ?, official = ? WHERE mission_id = ?', fields + (mission_id,))
        self.x('INSERT OR REPLACE INTO blobs (file_id, owner_id, name, data, created) VALUES (?, ?, ?, ?, ?)',
               (file_id, account_id, info['name'], info['data'], now))
        return mission_id

    def add_game_report(self, account_id, kind, query, data):
        return self.x('INSERT INTO game_reports (account_id, kind, query, data, created) VALUES (?, ?, ?, ?, ?)',
                      (account_id, kind, query, bytes(data), int(time.time())))

    # ---- files players upload

    def file(self, name):
        return self.one('SELECT * FROM files WHERE name = ?', (name,))

    def write_file(self, name, owner_id, data):
        now = int(time.time())
        self.x('INSERT INTO files (name, owner_id, data, created, modified) VALUES (?, ?, ?, ?, ?) '
               'ON CONFLICT(name) DO UPDATE SET data = excluded.data, modified = excluded.modified',
               (name, owner_id, bytes(data), now, now))

    def delete_file(self, name):
        self.x('DELETE FROM files WHERE name = ?', (name,))
