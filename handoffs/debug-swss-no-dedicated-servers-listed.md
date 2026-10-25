# Debug thread: swss-no-dedicated-servers-listed

SWSS: no dedicated servers listed when a player logs in.

The `debug_ledger` tool is not available in this session (it is not in the tool set and `searchTools` finds no such
tool), so the thread is kept here, in the repository, and each attempt is appended under "Attempts".

## Symptom

A player signs in to the server from the game and the server list comes up empty, although four dedicated sessions
run on this machine.

## Attempts

### Attempt 1 (read-only, earlier session)

Inspected: `journalctl --user -u swss-server`, `~/swss-server/data/status.json`, `gamespy-sessions.json`, the pspc
processes and UDP listeners, the files and builds on this machine, the docs, and part of the bundle's README. No cause
found and nothing changed.

### Attempt 2 (read-only, 2026-10-25): the server browser answers; the filter decides

Evidence, all from this machine:

- The four sessions run and are listed: `ss -lunp` shows pspc on UDP 3658, 3659, 3660 and 3661, and
  `status.json` holds four games.
- The server browser (TCP 28910) answers a `\status\` request with four games, each with its own `mapname`/`gametype`.
- The journal for the last 24 hours holds 57 list requests: 33 answered "4 of 4", 23 "3 of 3" (while the Space session
  was stopped), and exactly **one** "0 of 4":

  ```
  Oct 24 15:11:33 sb 127.0.0.1:45558: swsspsp list, 0 of 4 hosted games (filter: gamever = 10901 and levelsettings < 1073741824)
  ```

- A real client on the internet was answered correctly an hour ago:
  `Oct 25 11:42:28 sb 37.186.117.47:62382: swsspsp list, 4 of 4 hosted games (filter: gamever = 10901 and levelsettings >= 1073741824)`.

So the server is not hiding the games. The list a client sees depends on the filter the client sends, and the game
sends one of two:

- `levelsettings >= 1073741824` - the game's list of **dedicated** servers. The server sets bit 30
  (`DEDICATED_LEVEL`, `server/pspc_gamespy/qr.py`) for hosts on its own machine, so the four sessions appear here
  (verified: `4 of 4`).
- `levelsettings < 1073741824` - the game's list of **player-hosted** servers (`server/tests/live_check.py` sends this
  one). The four sessions do not appear here, by design (verified: `0 of 4`, and `test_gamespy_security.py` pins both
  behaviours).

Hypothesis (not verified, because it needs a client): the player was looking at the player-hosted list. The deploy
README already says *R* switches between the two lists in the game; if a player reports an empty list, ask which list
they are on before changing the server.

Not tried, and deliberately not: changing `qr.py`'s `DEDICATED_LEVEL` marking. It is what puts the sessions on the
dedicated list, and the tests pin it.
