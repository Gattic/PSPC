# SPDX-License-Identifier: GPL-2.0-or-later
"""Programs the server runs while it runs - dedicated game servers, for example - from a sessions
file (gamespy-sessions.json beside the database, or --sessions FILE):

  {"accounts": [{"nick": "Ded-Conquest", "email": "ded1@example.com", "password": "...", "namespace": 1}],
   "sessions": [{"name": "Conquest", "command": ["python", "host.py", "conquest"], "cwd": "...",
                 "restart_after": 30}]}

The accounts are GameSpy accounts for the sessions to log in with: made in the database when they're
missing (unique nick = nick), their password set when it differs. Each session's command starts with
the server, starts again restart_after seconds (default 30) after it ends, and ends with the server
even when the server is killed (Windows: a job object that kills its processes when its last handle
closes; Linux: the parent-death signal), its child processes too. A session's output goes to
sessions/<name>.log beside the database."""

import asyncio
import json
import logging
import os
import re
import subprocess
import sys
import time

from . import crypt
from .store import StoreError

log = logging.getLogger('gamespy')

SESSIONS_FILE = 'gamespy-sessions.json'


def load(path):
    """(accounts, sessions) of a sessions file; raises ValueError for a bad one."""
    with open(path, encoding='utf-8') as f:
        doc = json.load(f)
    accounts, sessions = doc.get('accounts', []), doc.get('sessions', [])
    for a in accounts:
        if not a.get('nick') or '@' not in a.get('email', '') or not a.get('password'):
            raise ValueError('%s: an account needs "nick", "email" and "password"' % path)
    names = set()
    for s in sessions:
        if not s.get('name') or not isinstance(s.get('command'), list) or not s['command']:
            raise ValueError('%s: a session needs "name" and a "command" list' % path)
        if s['name'] in names:
            raise ValueError('%s: two sessions named %r' % (path, s['name']))
        names.add(s['name'])
    return accounts, sessions


def make_accounts(store, accounts):
    """The accounts in the database, with these passwords. Returns how many were made."""
    made = 0
    for a in accounts:
        md5 = crypt.md5hex(a['password'])
        ns = int(a.get('namespace', 1))
        p = store.profile_by_uniquenick(a['nick'], ns)
        if p is None:
            try:
                store.new_user(a['email'], md5, a['nick'], a['nick'], ns)
                made += 1
            except StoreError as e:
                log.error('sessions: account %s: %s', a['nick'], e.message)
        elif p['password_md5'] != md5:
            store.set_password(p['userid'], md5)
    return made


class _Job:
    """Windows: a job object whose processes end when the server ends (its handle closes with it)."""

    def __init__(self):
        self.handle = None
        if sys.platform != 'win32':
            return
        try:
            import ctypes
            from ctypes import wintypes
            k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        except (ImportError, OSError) as e:  # sessions still run; they just outlive a killed server
            log.warning('sessions: no job object (%s)', e)
            return
        k32.CreateJobObjectW.restype = wintypes.HANDLE
        k32.OpenProcess.restype = wintypes.HANDLE
        handle = k32.CreateJobObjectW(None, None)
        if not handle:
            return

        class BASIC(ctypes.Structure):
            _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                        ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                        ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                        ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                        ('SchedulingClass', wintypes.DWORD)]

        class IO(ctypes.Structure):
            _fields_ = [(n, ctypes.c_uint64) for n in ('r', 'w', 'o', 'rb', 'wb', 'ob')]

        class EXTENDED(ctypes.Structure):
            _fields_ = [('BasicLimitInformation', BASIC), ('IoInfo', IO), ('ProcessMemoryLimit', ctypes.c_size_t),
                        ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                        ('PeakJobMemoryUsed', ctypes.c_size_t)]

        info = EXTENDED()
        info.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if k32.SetInformationJobObject(handle, 9, ctypes.byref(info), ctypes.sizeof(info)):  # extended limits
            self.handle, self.k32 = handle, k32

    def add(self, pid):
        if self.handle:
            h = self.k32.OpenProcess(0x0101, False, pid)  # PROCESS_SET_QUOTA | PROCESS_TERMINATE
            if h:
                self.k32.AssignProcessToJobObject(self.handle, h)
                self.k32.CloseHandle(h)


def _linux_parent_death():
    """In the child, before exec: SIGTERM when the server's thread that started it ends."""
    try:
        import ctypes
        import signal
        ctypes.CDLL('libc.so.6', use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except OSError:
        pass


class Sessions:
    def __init__(self, sessions, log_dir):
        self.sessions = sessions
        self.log_dir = log_dir
        self.job = _Job()
        self.procs = {}       # name -> the running subprocess.Popen
        self.state = {s['name']: {'name': s['name'], 'running': False, 'starts': 0, 'since': 0} for s in sessions}

    def log_path(self, name):
        return os.path.join(self.log_dir, re.sub(r'[^A-Za-z0-9_.-]+', '_', name) + '.log')

    def start(self, s):
        os.makedirs(self.log_dir, exist_ok=True)
        out = open(self.log_path(s['name']), 'ab')
        out.write(('==== %s started: %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), ' '.join(s['command']))).encode())
        out.flush()
        kw = {}
        if sys.platform == 'win32':
            kw['creationflags'] = 0x08000000  # CREATE_NO_WINDOW
        elif sys.platform.startswith('linux'):
            kw['preexec_fn'] = _linux_parent_death
        try:
            p = subprocess.Popen(s['command'], cwd=s.get('cwd') or None, stdin=subprocess.DEVNULL, stdout=out,
                                 stderr=subprocess.STDOUT, **kw)
        except OSError as e:
            out.write(('can\'t start: %s\n' % e).encode())
            out.close()
            log.error('sessions: %s can\'t start: %s', s['name'], e)
            return None
        self.job.add(p.pid)
        out.close()
        st = self.state[s['name']]
        st.update(running=True, since=int(time.time()), starts=st['starts'] + 1)
        log.info('sessions: %s started (process %d)', s['name'], p.pid)
        return p

    async def keep(self, s):
        """Runs a session, again and again."""
        while True:
            p = self.start(s)
            if p is not None:
                self.procs[s['name']] = p
                while p.poll() is None:
                    await asyncio.sleep(1)
                self.procs.pop(s['name'], None)
                self.state[s['name']]['running'] = False
                log.info('sessions: %s ended (%s); again in %ds', s['name'], p.returncode, s.get('restart_after', 30))
            await asyncio.sleep(float(s.get('restart_after', 30)))

    async def run(self):
        await asyncio.gather(*(self.keep(s) for s in self.sessions))

    def stop(self):
        """Ends every session and its child processes."""
        for name, p in list(self.procs.items()):
            if p.poll() is None:
                if sys.platform == 'win32':
                    subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, creationflags=0x08000000)
                else:
                    p.terminate()
        self.procs.clear()

    def status(self):
        return list(self.state.values())
