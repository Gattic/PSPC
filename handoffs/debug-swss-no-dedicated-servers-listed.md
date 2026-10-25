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

### Attempt 3 (Windows side, 2026-10-25): the player-less host works with the Linux script's presses

Not the symptom itself (the empty list was the filter, attempt 2) but its follow-up: the four sessions list `numplayers=1`
because the Linux box hosts from the playable disc. On the Windows PC the dedicated disc (the same disc with a patched
`EBOOT.BIN`, 27 bytes) was built from the playable one with `server/deploy/swss/swss-dedicated-iso.py` (byte-identical to
`private/swss/dedicated/SWSS.iso`) and `dedicated-host.sh` was run on it unchanged under Git Bash against a local
`pspc_gamespy`. Server-browser query: `22.4.0.0` -> `MSTR prequel`/`HANV`, `12.0.0.1` -> `Hoth prequel`/`CQST` (space),
random fill -> `TTNE classic`/`CTF1`; **`numplayers=0` on all three**, dedicated bit set. Details and the Linux steps:
`handoffs/2026-10-25-swss-dedicated-disc-transfer.md` (section "Windows side") and the bundle it names. The Linux before/after
is still to do.

### Attempt 4 (Linux side, 2026-10-25): the four sessions host without a player

The follow-up in attempt 3 is done here. The bundle `swss-dedicated-disc-2026-10-25.zip` (sha256 `02da79df…`, every member
checked) held the dedicated `EBOOT.BIN`, and `server/deploy/swss/swss-dedicated-iso.py` made `Games/SWSS-dedicated.iso` from
the playable disc in 10 seconds; the result is 2,333,759,488 bytes with sha256
`d225b9b8f8b87d286f773fe919bcc716c4573e02c0a21b1a5b64d3b379a5ee82`, byte-identical to the Windows disc. Its executable's
plain-ELF sha1 is `7a0eb8ad3e423c9bd22029768bb54c9779a48ee9` (the playable one's is `c5f57033…`), so it needed its own native
code: `tools/pspc_build.py Games/SWSS-dedicated.iso --build-dir .worktrees/swss-dedicated/build/LinuxRuntime --cache
~/swss-server/dedicated/xdg-cache/pspc/aot --compiler gcc` (7 min 26 s; `--compiler gcc` is required here, there is no
`clang++`). The `.so`, its `.semver` and the merged `games.json` were copied into the other three sessions' caches
(`dedicated-2`, `dedicated-space`, `dedicated-heroes`), each of which reads its own `xdg-cache`.

The sessions file was backed up (`gamespy-sessions.json.bak-20261025-pre-dedicated`, 0600) and only the `ISO=` item changed in
all four sessions, atomically; `PLAYLIST` stayed `12.0.1.0`, `22.1.0.0`, `12.0.0.1`, `22.4.0.0`. `swss-server` restarted
15:39:22, each session rebuilt its profile (15:43) and the games were listed at 15:50.

Server browser (TCP 28910), before and after:

| Session | mapname | gametype | numplayers before | numplayers after | levelsettings |
|---|---|---|---|---|---|
| SWSS Conquest | Hoth classic | CQST | 1 | **0** | 1627389954 (low bits 2) |
| SWSS Space | Hoth prequel | CQST | 1 | **0** | 1610612740 (low bits 4 = space) |
| SWSS Conquest 2 | MSTR prequel | CTF1 | 1 | **0** | 1610678274 (low bits 2) |
| SWSS Heroes vs | MSTR prequel | HANV | 1 | **0** | 1610874882 (low bits 2) |

`levelsettings` is unchanged because `qr.py` already ORs bit 30 for hosts on this machine, which masks the disc's own bit;
`numplayers` is the observable that moved. The maps and gametypes are exactly the pinned ones, so the playlist presses and
indices carried over to the dedicated disc unchanged.

Rounds end without anyone pressing anything: the first round of each host started 15:46 (games 261-264, one per host) and the
next round for each host started later - DedConquest at 16:36:26 (50 min), DedSpace 16:46:24, DedConquest2 16:46:31,
DedHeroes 16:46:34 (the 60-minute limit). The pspc processes never restarted, `status.json` kept `since` at 15:46 with
`players=0`, and the browser kept listing all four with `numplayers=0` through the transition. **One difference from the
playable disc:** with 0 players the game sends no final stats report, so there is no `game N finished` journal line and the
`games` row keeps `finished=NULL`, where the playable-disc rows had it set; the round end shows as a new game id for the same
host instead.

Not covered: a real client joining one of these sessions (the client keeps the playable disc) - the game counts it as a
player then.
