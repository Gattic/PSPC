# Handoff: fill the SWSS dedicated playlist with index-addressed presses

## Resume Prompt

Use this exact prompt in a fresh session:

From the repository root, read `handoffs/2026-10-25-swss-playlist-presses.md` first (and `handoffs/2026-10-24-swss-playlist.md`
for the Linux deployment it continues), verify the current repository state, then continue from the Next Actions section
without relying on prior chat history.

## Current Goal

Continue `handoffs/2026-10-24-swss-playlist.md`: the SWSS "Space" session (UDP 3660) and "Heroes vs Villains" session (3661)
must host a fixed, intended map/mode instead of the game's random playlist fill (SQUARE); the two Conquest sessions (3658,
3659) keep their random rotations. Success: all four live sessions advertise the intended `mapname`/`gametype` in the
GameSpy server browser (TCP 28910 query).

This session found the lever the 2026-10-24 handoff did not have: the Windows checkout already has a tested driver that fills
the playlist by **index**, with no OCR and no EBOOT patch. Nothing has been implemented or run yet.

## Current State

- Timestamp: 2026-10-25
- Working directory: `C:\Users\yoelr\CFW\PSPC` (Windows; the Linux server is a different machine, `/home/rob/dev/PSPC`)
- Branch: `swss-playlist-presses-handoff` (cut from `main` at `d683931`, "Prepare PSPC 0.1.2 release"); this handoff is its only
  commit. `main` was clean before it.
- Linux deployment: unchanged since the 2026-10-24 handoff (four sessions, `~/swss-server/bin/dedicated-host.sh`, the playable
  `Games/SWSS.iso`, UDP 3658-3661).

## User Requests And Constraints

- Asked whether anything on this machine can help with the 2026-10-24 handoff, then for this handoff to be committed, then for a
  zip of the files needed to run SWSS on the Linux server (made outside git, under `build/`, which is ignored).
- **No push.** Local commits only; merging to `main` needs the user's say-so.
- Not copied anywhere: `~/swss-server/data/gamespy-sessions.json` (the sessions' account passwords) and every `*.sqlite`.
- The Linux-side agent policy in the 2026-10-24 handoff still applies there (no `sudo`, atomic deploys, never rebuild PSPC in
  place).
- **Open, not answered yet:** (1) verify on this machine first, or port to `dedicated-host.sh` straight away; (2) what the Space
  session should host (see Known Issues).

## Work Completed

Analysis only; no source, test or deployment file changed.

- Read the 2026-10-24 handoff, `server/deploy/swss/dedicated-host.sh`, the deploy README and `docs/gamespy-server.md`.
- Found the driver the Linux session did not have: `private/swss/tools/dedicated.py` (git-ignored, so it never reached the Linux
  box). `playlist_presses()` (line 94) adds each entry with four CROSS steps on the Game Type screen: location, mode, era, area.
  Indices come from the disc's own data (`planets()`, `choices()`): location = position of the planet code in the sorted code
  list; mode = how many lower bits of the planet's `AllowedGameTypes` union are set (Conquest 0x1, 1-Flag 0x2, 2-Flag 0x4, Hero
  CTF 0x8, then Heroes vs Villains 0x10); era = Clone Wars first; area = Ground first. After each entry the location cursor is
  back at the top. Its session start wipes the memory stick, so the playlist starts empty.
- `private/swss/README.md` ("Dedicated servers") records the 2026-10-19 live test: ten installed sessions, each listed in the
  server browser with its own gametype (CQST, CTF1, CTF2, HCTF, HANV). **That confirmed the gametype, not which map was chosen.**
- Confirmed the Linux sessions run the **playable** disc, not the dedicated one: `Games/SWSS.iso` has sha256
  `fe4913f2100e581f781a3e7d817efe0c712c94e7666c3f2366716c852d4e8a80`, the value the deploy README pins.
  `private/swss/dedicated/SWSS.iso` (same size, built 40 minutes later with `patches/dedicated.txt` on the EBOOT) is
  `d225b9b8f8b87d286f773fe919bcc716c4573e02c0a21b1a5b64d3b379a5ee82`. So the Linux host plays as a real local player
  (the script takes a team and a spawn point) while the Windows sessions are true dedicated hosts.
- Ran `dedicated.choices()` against the current tree (`private/swss/trees/ded_swss`; same data as the playable disc, built
  from `trees/swss`): see Context Notes for the numbers.

## Work In Progress

None.

## Changed Files

- `handoffs/2026-10-25-swss-playlist-presses.md`: this handoff (new).

## Verification

- Ran: `choices()`/`planets()` over the current tree (read-only), `Get-FileHash` on both discs.
- **Not run:** any PSPC session, any server test, any query of the live server. No code changed, so no test suite was needed.
- Unverified hypotheses (do not state them as fact):
  1. The Linux attempt ("CROSS on the playlist page does not add a map") failed because its profile persists with a filled
     playlist, whereas the Windows driver always starts from a fresh memory stick.
  2. It also pressed too early: Linux presses `11600:SQUARE` 600 VBlanks after the Continue press (`11000:CROSS`), the Windows
     driver waits 1400 VBlanks (`21400` Continue, playlist from `22800`) and then about 500 between screens.
  3. The Linux timeline (profile loaded, not created) reaches the Game Type screen at the same relative time as the Windows one.

## Known Issues Or Blockers

- **The Space session's entry is not chosen.** The game has no space gametype: Space is an *area*. On the current disc Space
  offers 56 Conquest entries and 5 1-Flag CTF entries, nothing else. Decide planet + era (one fixed entry, or a short rotation).
- **The browser may not show the area.** `mapname` is `<PLANET CODE> <era>` (for example `ENDR classic`), so Ground and Space
  entries of one planet may advertise identically. Find out whether `zone`, `levelsettings` or another field of the 10-field
  record tells them apart before promising "advertises Space".
- The location index assumes the in-game location list holds every planet code in `ENVLIST.TXT`, 35 of them since Demon World
  (DMNW) was added after the 2026-10-19 test. Confirm with a `mapname` check, not only `gametype`.
- The roadmap item `26.1.8` named in the earlier handoff is not in this checkout (a search of the whole repository for
  `26.1.8` finds only that handoff); it lives on the Linux side.

## Next Actions

1. Ask the user the two open questions (verify here first or port now; the Space entry).
2. **Verify here** (recommended): a local GameSpy server (`private/swss/server/run_server.cmd`, or `server/` with the sessions
   file) plus a headless PSPC session of `Games/SWSS.iso` or the dedicated disc, with the Space entry and
   `prequel/mstr/MSTRHv` (location 22, mode 4, era 0, area 0) pinned; then the TCP 28910 query (`server/tests/live_check.py` shows
   how, and the 2026-10-24 handoff's Context Notes give the `\status\` request and the 10-field stride). Pass: `mapname` and
   `gametype` are the pinned ones. Needs native code for the disc used (see `AOT hints REBUILD nag` in the memory notes) and a
   fresh memory stick.
3. **Port** to `server/deploy/swss/dedicated-host.sh`: an optional `PLAYLIST` (entries as `location.mode.era.area`), turned into
   timed presses replacing `11600:SQUARE`; start from a fresh profile when it is set (delete `$PROFILE`); with it unset behave
   exactly as now, so the Conquest sessions are untouched. Add a `dedicated.py` subcommand that prints the indices for a planet
   code, mode, era and area; add stub-pspc tests to `server/tests/test_dedicated_host.py`; document it in
   `server/deploy/swss/README.md` and the example sessions file.
4. On the Linux box: deploy atomically, restart the Space and H&V sessions, query TCP 28910 for all four, check Conquest is
   unchanged. Update roadmap `26.1.8`. Commit and merge only with the user's authorisation; never push.

## Context Notes

- Run the helper tools with the full Python path (`C:/Users/yoelr/AppData/Local/Programs/Python/Python312/python.exe -I`; plain
  `python` is the Store alias). `-I` drops the script directory from `sys.path`, so a one-off script must do
  `sys.path.insert(0, r'C:\Users\yoelr\CFW\PSPC\private\swss\tools')` and then `import dedicated`;
  `dedicated.choices(r'...\private\swss\trees\ded_swss', 'hv')` lists the entries.
- Planet (location) indices of the current disc: 0 ALDR, 1 BSPN, 2 BZPT, 3 CATO, 4 CRSC, 5 CRSS, 6 DANT, 7 DETH, 8 DMNW, 9 ENDR,
  10 ENDS, 11 GEON, 12 HOTH, 13 HOTR, 14 HTIC, 15 HTSP, 16 KSHK, 17 KSHR, 18 KSHS, 19 KSSL, 20 KTOR, 21 MSTF, 22 MSTR, 23 MYGE,
  24 MYGS, 25 NABO, 26 ORDM, 27 SALU, 28 SULL, 29 TTNE, 30 TTNR, 31 VJUN, 32 YAVN, 33 YAVR, 34 YAVS.
- Entries offered, by mode: Conquest 126 (70 Ground, 56 Space), 1-Flag CTF 61 (56, 5), 2-Flag CTF 56 (56, 0), Hero CTF 44
  (44, 0), Heroes vs Villains 56 (56, 0). The 56 Conquest Space entries are mode index 0; the five 1-Flag ones are mode index 1.
  The area index is the position among the areas *offered* (Ground first), so Space is area 1 where Ground is also offered and
  area 0 where it is not (1-Flag `BSPN` and `CATO` prequel, `(1, 1, 0, 0)` and `(3, 1, 0, 0)`): never hard-code 1 for Space.
- Examples as `(location, mode, era, area)`: H&V Mustafar prequel `(22, 4, 0, 0)`, classic `(22, 4, 1, 0)`; Conquest-in-Space Hoth
  prequel `(12, 0, 0, 1)`, classic `(12, 0, 1, 1)`.
- The Windows press recipe (`session_presses`): Create at `19200`, name `20400`, Continue `21400`, playlist from `22800`; per entry
  the location presses are `DOWN` every 40 VBlanks, +300, `CROSS`, +500, mode `DOWN`s every 60, +200, `CROSS`, +500, era,
  +500, area, then +600+200 before the next entry; Players is raised with `RIGHT` after `SELECT`; `START` launches.
- The Linux script's timeline (`dedicated-host.sh`): `9500:CROSS` Create, `10200:CROSS` name, `11000:CROSS` Continue,
  `11600:SQUARE` random playlist, `14200:START`, then `CROSS` every 500 VBlanks; a first run of 7000 VBlanks makes the profile.
- The Linux box has only the public repository; everything under `private/` and `Games/` exists only on this machine, so any
  index must be generated here and carried over as data, not recomputed there.

## Update (2026-10-25, later session)

The port in Next Actions 3 is in the tree (commit 2e4f60d), and the "verify on this machine" step was done for the **dedicated**
disc: `dedicated-host.sh` unchanged, `PLAYLIST=22.4.0.0` -> `MSTR prequel`/`HANV`, `PLAYLIST=12.0.0.1` -> `Hoth prequel`/`CQST`
with `levelsettings` low bits 4 (space), both `numplayers=0`; the random fill still works. So the pinned entries land as
intended on a fresh profile with the script's timeline (consistent with hypothesis 1; hypotheses 2 and 3 were not tested
separately). What remains is on the Linux box, and is
the dedicated-disc work in `handoffs/2026-10-25-swss-dedicated-disc-transfer.md` (section "Windows side" and the bundle
`build/swss-dedicated-disc/swss-dedicated-disc-2026-10-25.zip`).
