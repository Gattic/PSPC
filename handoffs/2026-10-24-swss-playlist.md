# Handoff: deterministic Space / Heroes-vs-Villains playlist for the SWSS dedicated sessions

## Resume Prompt

Use this exact prompt in a fresh session:

From the repository root, read `handoffs/2026-10-24-swss-playlist.md` first, verify the current repository state, then continue from the Next Actions section without relying on prior chat history.

## Current Goal

The SWSS "Space" session (UDP 3660) and the "Heroes vs Villains" session (3661) must host a fixed, intended
map/mode instead of whatever the game's "random" playlist fill (the SQUARE button, `SWB.FE.SelectRandomMaps`) picks.
The two Conquest sessions (3658, 3659) must keep their existing random rotations. Success means all four live
sessions advertise the intended `mapname`/`gametype` in the GameSpy server browser.

## Current State

- Timestamp: 2026-10-24 21:38 UTC
- Original working directory: `/home/rob/dev/PSPC`
- Original repository path: `/home/rob/dev/PSPC` (git)
- Handoff path: `handoffs/2026-10-24-swss-playlist.md`
- Original absolute handoff path: `/home/rob/dev/PSPC/.worktrees/swss-public-server/handoffs/2026-10-24-swss-playlist.md`
- Branch: `swss-public-server` (worktree `/home/rob/dev/PSPC/.worktrees/swss-public-server`), already merged into `main`
- HEAD: `4e18f09` on the branch, `6130ca9` on `main` (both before this handoff commit)
- Status summary: repository clean; the experimental one-map disc feature was reverted, so this handoff commit is the
  only change. The sibling worktree `.worktrees/swss-dedicated` (`dedba44`) has one untracked, machine-specific
  `build-ports.sh` that was not committed.

Live deployment (unchanged by this session): the systemd **user** unit `swss-server` is active and runs four sessions
from `~/swss-server/data/gamespy-sessions.json` through `~/swss-server/bin/dedicated-host.sh`, using
`PSPC=/home/rob/dev/PSPC/.worktrees/swss-dedicated/build/LinuxRuntime/bin/pspc` and
`ISO=/home/rob/dev/PSPC/Games/SWSS.iso`, with `UDP_PORT` 3658-3661 and per-session `DEDICATED` directories under
`~/swss-server/`.

## User Requests And Constraints

- Latest request: create a handoff file, commit the handoff and the session's changes, then merge to `main`.
- Earlier request (still the objective): replace SQUARE (Random) playlist randomization with deterministic playlist
  selection for the **Space** and **Heroes vs Villains** sessions; **preserve the Conquest rotations**; verify that all
  four sessions advertise the intended `mapname`/`gametype`. Restarting the sessions for verification was authorised.
- Deploy atomically (write a temp file, then `mv`); never rebuild in place inside `build/LinuxRuntime`.
- No push. Local commits and merging to `main` were authorised.
- Environment policy forbids `sudo`, piped `curl`/`wget`, and raw sockets in agent shell commands; plain `rm <file>` is
  allowed, `rm -f`/`rm -rf` are not.
- Do not copy or publish secrets: `~/swss-server/data/gamespy-sessions.json` holds the session accounts' passwords.

## Work Completed

- Established how the rotation is chosen: `SWB.FE.SelectRandomMaps` fills the playlist, which is **created together with
  the profile** and then persists for the life of that profile; SQUARE does not replace an existing playlist.
- Built and tested a disc-image editor (`swss-map-iso.py`: rewrites `ENVS/ENVLIST.TXT` to one map and/or trims the
  `ENVS` tree, patching LE+BE directory record sizes) plus per-session image support in `dedicated-host.sh`
  (`$DEDICATED/map`, atomic `SWSS.iso.tmp` + `mv`, rebuild only on map change, profile dropped on rebuild) and 12 tests.
  All of it worked mechanically but **did not change the playlist**, so it was reverted.
- Proved the disc is not the playlist's source, with three fresh profiles (profile `DATA.BIN` deleted each time) under
  three different images: `ENVLIST` = one line + retail tree -> advertised `CRSS classic`; tree **and** `ENVLIST`
  trimmed to `HothSp` -> `HOTR classic`; retail control -> `ENDR classic`. None matched the trimmed map.
- Two trimmed-image runs also stopped heartbeating: the session hosted and completed rounds but never appeared in
  `status.json` or the server browser, so trimming the disc is actively harmful.
- Determined the profile cannot be patched: `DATA.BIN` is fully encrypted by the game itself (entropy 8.0, no plaintext
  map names) because PSPC's savedata layer is transparent (`src/runtime/hle/modules/utility.cpp` has no crypto).
- Determined `games.data.mapname` in `~/swss-server/data/gamespy.sqlite` is stale (it keeps reporting the first round's
  map, e.g. `ends`/`CTF2` for the Space session across many runs). Only the server-browser query on TCP 28910 is
  authoritative for what a live session advertises.
- Restored all four sessions to the retail ISO and confirmed they are healthy and listed.

## Work In Progress

None. The experimental feature is fully reverted; the repository tree is clean apart from this handoff.

## Changed Files

- `handoffs/2026-10-24-swss-playlist.md`: this handoff (new).
- No source, deployment, test or documentation files are changed; everything tried this session was reverted to `main`'s
  content. `~/swss-server/bin/dedicated-host.sh` is functionally identical to `main`'s version (one comment rewrap only).

## Verification

- `cd server && python3 -m unittest discover -s tests -q`: **176 tests OK, 2 skipped** (run after the revert; pytest is
  not installed, use `unittest`).
- `sh -n server/deploy/swss/dedicated-host.sh`: syntax OK.
- Live server-browser query at 21:32 UTC (TCP 28910, `\status\` request, 10-field stride):

  | Session | mapname | gametype |
  | --- | --- | --- |
  | SWSS Conquest | `ENDR prequel` | `HANV` |
  | SWSS Conquest 2 | `ENDS prequel` | `CTF2` |
  | SWSS Heroes vs | `ENDR classic` | `CQST` |
  | SWSS Space | `CRSS classic` | `CTF2` |

  So Conquest is preserved, but Space and Heroes vs Villains still advertise random picks: the objective is **not** met.

## Known Issues Or Blockers

- **Blocked on a missing lever.** Every disc-side mechanism (ENVLIST, tree, fresh profile, per-session image) has been
  tested live and provably does not influence the playlist; the playlist lives in the game-encrypted profile.
- The playlist editor cannot be driven blindly: CROSS on the playlist page does not add a map, and the dumped frames'
  text is not legible enough for OCR, so the highlighted row/attribute cannot be read back.
- The game's console (`AddMap` exists in the EBOOT) is unreachable from the dedicated sessions.
- Roadmap item `26.1.8` ("Space/H&V mode selection in the game's playlist editor") is marked blocked with this evidence.

## Next Actions

1. Read this file, then confirm the deployment is still healthy: `systemctl --user is-active swss-server`, `ss -lunp |
   grep pspc` (expect 3658-3661), and a TCP 28910 server-browser query for the four games' `mapname`/`gametype`.
2. Choose one of the remaining levers before writing any code:
   - patch the EBOOT's playlist fill/seed (`PSP_GAME/SYSDIR/EBOOT.BIN`; the gametype table is at `0x3f664c`, the map
     path format `envs\%s\%s\%s%s.psp`, the suffix jump table at `0x3f6668`) — needs a decision on whether PSPC verifies
     the PRX signature;
   - automate the playlist GUI with readable frames (dump with `--renderer software --dump-frames <dir>
     --render-scale 4 --texture-upscale 4`) and OCR the planet list and the attribute sub-list;
   - search `--rtc-base` values, since PSPC documents that a fixed guest clock makes a run repeat exactly; the fill is
     ~1/350 per attempt for an exact map+mode, so this is only viable if the mapping can be computed rather than tried.
3. Implement the chosen lever for the Space session first, verify it through the TCP 28910 query, then apply it to
   Heroes vs Villains (`prequel/mstr/MSTRHv` is the intended Hv map, gametype `HANV`), keeping both Conquest sessions
   untouched.
4. Re-run `python3 -m unittest discover -s tests -q` in `server/`, update roadmap `26.1.8`, and report; commit and merge
   only with the user's authorisation, and never push.

## Context Notes

- The authoritative live check is a GameSpy server-browser query on **TCP 28910**: build the `\status\` request
  (`bytes([0,1,3]) + pack('>I',10901) + b'swsspsp\0swsspsp\0' + challenge + b'\0' + b'\' + fields + b'\0' +
  pack('>I',0)`), decrypt with `pspc_gamespy.crypt.GOACipher` keyed by `pspc_gamespy.crypt.sb_crypt_key(challenge,
  game.secret_key, ...)` for `pspc_gamespy.games.find('swsspsp')`, and parse the payload as **10 fields per server**
  (`hostname, mapname, gametype, gamemode, numplayers, maxplayers, gamever, zone, era, levelsettings`). A working script
  from this session was at `/tmp/qr2.py` (ephemeral); recreate it from `server/tests/live_check.py` if it is gone.
- `status.json` in `~/swss-server/data/` lists only sessions that heartbeat; a session running a broken image is absent
  from it even while its rounds complete.
- `HOURS=6` in `dedicated-host.sh` makes every session stop and restart roughly every six hours, which re-randomises the
  rotation; the server restarts a stopped session after about 30 seconds.
- The EBOOT gametype table is exactly `CQST, CTF1, CTF2, HCTF, HANV, CMPN`; there is no space-specific gametype - space
  is a map attribute, so the intended Space advertisement is a space map (`*Sp`) with one of those modes.
- A restart of a dedicated session: `kill <pid>` (`ss -lunp | grep pspc` finds it by UDP port); the unit restarts it 30
  seconds later. Never rebuild PSPC in place; a fresh binary is deployed with a temp file plus `mv`.
- `~/swss-server/dedicated-space/` and the other session directories keep `ms0/`, `xdg-*` and their `DATA.BIN` profile;
  deleting the profile forces the game to create a new one (and therefore a new random playlist).
