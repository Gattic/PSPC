# Handoff: run the four SWSS sessions on the dedicated disc (no afk player) - Linux side

## Resume Prompt

Use this exact prompt in a fresh session **on the Linux server** (`/home/rob/dev/PSPC`):

> From the repository root, read `handoffs/2026-10-25-swss-dedicated-linux.md` first. The bundle
> `swss-dedicated-disc-2026-10-25.zip` should be in the home directory (sha256 in the handoff). Verify the current server state,
> then continue from the Next Actions section without relying on prior chat history.

If the handoff file is not in the Linux checkout, the person running the session copies it there (it was written on the
Windows PC; the repository changes it mentions are on branch `swss-playlist-presses-handoff` and may not be pushed).

## Current Goal

The four dedicated SWSS sessions (UDP 3658-3661, `~/swss-server/bin/dedicated-host.sh`) must host **without a local player**.
Today every one of them lists `numplayers=1` in the GameSpy server browser, because they run the playable disc
`Games/SWSS.iso` and the script takes a team and a spawn point as a real player (the "afk player"). Success: all four games
are listed with **`numplayers=0`**, the same pinned `mapname`/`gametype` as now, and a round ends and the next map loads
without anyone pressing anything.

## Current State

- Written 2026-10-25 on the Windows PC (`C:\Users\yoelr\CFW\PSPC`), branch `swss-playlist-presses-handoff` (cut from `main` at
  `d683931`; `origin` already holds it up to `3847581`, the Linux-authored commits). Nothing here was run on the Linux box.
- Linux deployment as last reported (2026-10-25): systemd **user** unit `swss-server`, four sessions from
  `~/swss-server/data/gamespy-sessions.json`, each `dedicated-host.sh NICK` with `PSPC=.../.worktrees/swss-dedicated/build/LinuxRuntime/bin/pspc`,
  `ISO=.../Games/SWSS.iso`, `UDP_PORT` 3658-3661, `DEDICATED=~/swss-server/dedicated*`, and these pinned `PLAYLIST`s
  (`LOCATION.MODE.ERA.AREA`): Conquest `12.0.1.0` (Hoth classic, CQST), Conquest 2 `22.1.0.0` (MSTR prequel, CTF1), Space
  `12.0.0.1` (Hoth prequel, CQST, in space), Heroes `22.4.0.0` (MSTR prequel, HANV). Check all of this still holds before you
  change anything.
- The fix is a different **disc**, not a different script: the dedicated disc is the playable disc with a patched
  `PSP_GAME/SYSDIR/EBOOT.BIN`. The patch makes the session start set the host's local players to 0 (the engine already
  supports a host without a player; the retail menus never set one up) and sets bit 30 of the game's `levelsettings` (how the
  browser's dedicated list picks a game, so the games stay on that list). Nothing else on the disc differs.

## User Requests And Constraints

- Goal as the user put it: "get our server to run dedicated servers without an afk player".
- **No push** from here unless the user says so. Commit only with their authorisation.
- The Linux agent policy still applies: no `sudo`, no piped `curl`/`wget`, no `rm -f`/`rm -rf` (plain `rm <file>` is fine),
  no raw sockets. Deploy atomically (temp file + `mv`); never rebuild PSPC in place in `build/LinuxRuntime`.
- Do not leak `~/swss-server/data/gamespy-sessions.json` (account passwords) or copy `*.sqlite` anywhere.
- Do not change `qr.py`'s `DEDICATED_LEVEL` marking (pinned by `server/tests/test_gamespy_security.py`).
- Keep the four pinned playlist entries as they are; this changes who hosts, not what is hosted.
- Players keep the playable disc (`Games/SWSS.iso`); only the sessions use the dedicated one.

## Work Completed (on the Windows PC)

- Compared the two discs: the executables are both 6,162,040 bytes at the same sector (168); 27 bytes differ; the whole discs
  differ in exactly three sectors (1095, 1129, 2715), all inside the executable's extent. So the dedicated disc can be made
  by overwriting those sectors in a copy of the playable disc.
- Wrote `server/deploy/swss/swss-dedicated-iso.py` (a copy is in the bundle) and tested it: run on the Windows PC's
  `Games/SWSS.iso` with the bundle's `EBOOT.BIN` it produces a file that `cmp` says is identical to the Windows PC's dedicated
  disc, whose sha256 is `d225b9b8f8b87d286f773fe919bcc716c4573e02c0a21b1a5b64d3b379a5ee82`. Nine unit tests
  (`server/tests/test_swss_dedicated_iso.py`).
- Reproduced the patch: `elfpatch.py <playable EBOOT> out dedicated.txt` ("8 words patched") equals the bundle's `EBOOT.BIN`.
- **Ran `dedicated-host.sh` unchanged on the dedicated disc** (Git Bash, a throwaway local `pspc_gamespy`, native code built for
  the dedicated executable). Server-browser query (TCP 28910):

  | PLAYLIST | mapname | gametype | numplayers | levelsettings |
  |---|---|---|---|---|
  | `22.4.0.0` | MSTR prequel | HANV | 0 | 1610874882 |
  | `12.0.0.1` | Hoth prequel | CQST | 0 | 1610612740 (0x60000004: low bits 4 = space) |
  | none (random fill) | TTNE classic | CTF1 | 0 | 1627455490 |

  All three had bit 30 of `levelsettings` set and were listed 4 to 6 minutes after their start. The Heroes game stayed
  listed with `numplayers=0` for 30 minutes of its first round, the Space game for 20; their logs held nothing but the loader's
  `Asura: 12 bad relocations` (printed at every start, harmless).
- Updated `server/deploy/swss/README.md` (section 3, "A host without a player"), `docs/gamespy-server.md`, the comment header of
  `dedicated-host.sh` (comments only; its presses are untouched) and the earlier handoffs.

## Work In Progress

None on the Windows side. The Linux deployment has not been started.

## Changed Files

On the Windows branch (committed there): `server/deploy/swss/swss-dedicated-iso.py` (new), `server/tests/test_swss_dedicated_iso.py`
(new), `server/deploy/swss/README.md`, `server/deploy/swss/dedicated-host.sh` (comments only), `docs/gamespy-server.md`, the
handoffs under `handoffs/`. **Nothing is needed from the branch to run this**: the bundle carries the two runtime files.

The bundle `swss-dedicated-disc-2026-10-25.zip` - 2,220,179 bytes, sha256
`02da79dfed07971a8e4a2d7e7209a8074c5eae2cd442fb76f80c2dd81a75a1bc` (a `.sha256` file goes with it) - holds:

| File | What | sha256 |
|---|---|---|
| `EBOOT.BIN` | the dedicated executable, 6,162,040 bytes | `155a7a7fc84da335f38cf01d578e7aa258ae01480685d006cb3dbaf9d7f61fcf` |
| `swss-dedicated-iso.py` | makes the dedicated image from your playable one | `f5a9ebfe9b933d5172a594ba638cf13e0f06a0e5b8cec76461fa10392b035606` |
| `dedicated-host.sh` | this branch's script (only its header comment differs from your deployed PLAYLIST version) | `65d519e050574617bc2141beec233c7801bb4817a33771d01f2fdb11338a63de` |
| `dedicated.txt`, `elfpatch.py` | the patch and its applier, documentation only | |
| `NOTES.txt`, `SHA256SUMS` | notes and checksums | |

## Verification

- Done on Windows: the tests above; `python -m unittest discover -s tests` in `server/` passes (192 tests, 12 skipped - the
  shell-script tests are Linux-only, so run `python3 -m unittest discover -s tests -q` here if you touch the script).
- **Not verified, and the first thing to watch:** a round **ending** on a player-less host with this script's presses. The
  script presses CROSS every 500 VBlanks after the first map; the Windows driver (`dedicated.py`) presses every 1800 and took
  rounds to their end on this disc on 2026-10-19. Nothing ended a round in the 20-30 minutes tested.
- Not verified: the Linux PSPC build on the dedicated executable (only the Windows build was run), and a real client joining a
  Linux session on the dedicated disc (a Windows client joined a Windows dedicated host on 2026-10-19).

## Known Issues Or Blockers

- Where the native code has to go on Linux is not known from here. Sessions run with `XDG_CACHE_HOME=$DEDICATED/xdg-cache`
  (see `dedicated-host.sh`) while `tools/pspc_build.py` writes to `<build dir>/bin/cache/aot` unless given `--cache`. Look at how
  the **existing** playable-disc native code reaches the sessions (find the `.so` named by the playable EBOOT's plain-ELF SHA-1,
  `d7ec354f0d5b3e60878f3c6af01aaa1842c3ac5b`, and the `games.json` beside it) and build the new one into the same place with the
  same `--build-dir`. README section 3 notes a host without native code "was not tried".
- If the round end does stall (a game listed but still on the same map long after the time limit, or `numplayers` fine but no
  `game N finished` in the journal), the likely lever is the press interval: `dedicated-host.sh` has
  `seq $((start + 2200)) 500 "$END"`; the dedicated driver uses 1800. Do not edit a script a running session executes
  (`sh` reads it incrementally): stop the server, edit atomically, start it. `server/tests/test_dedicated_host.py` pins the
  last press (`215900:CROSS:6`) and would need updating.

## Next Actions

Everything below is on the Linux box, in `/home/rob/dev/PSPC`.

1. **Baseline.** `systemctl --user is-active swss-server`; `ss -lunp | grep pspc` (expect 3658-3661); the TCP 28910 query in
   step 7 (expect `numplayers=1` on all four, the four pinned maps). Check free space: `df -h Games` (the new image is 2.3 GB).
   Save the numbers for the report.
2. **Unpack and verify.**

   ```sh
   cd ~ && sha256sum swss-dedicated-disc-2026-10-25.zip     # 02da79df...a1bc
   mkdir -p /tmp/swss-dedicated && cd /tmp/swss-dedicated
   unzip -o ~/swss-dedicated-disc-2026-10-25.zip
   sha256sum -c SHA256SUMS
   ```

3. **Make the dedicated image** (a copy; the playable disc is never touched, and nothing is written over a disc a running game has
   open):

   ```sh
   cd /home/rob/dev/PSPC
   python3 /tmp/swss-dedicated/swss-dedicated-iso.py Games/SWSS.iso Games/SWSS-dedicated.iso /tmp/swss-dedicated/EBOOT.BIN \
     --expect-old efdb4423055514fcf7808c31ac036b394331e22cb71b1886f535e19c6d4f1471 \
     --expect-new 155a7a7fc84da335f38cf01d578e7aa258ae01480685d006cb3dbaf9d7f61fcf
   sha256sum Games/SWSS-dedicated.iso      # must be d225b9b8f8b87d286f773fe919bcc716c4573e02c0a21b1a5b64d3b379a5ee82
   ```

   It prints "27 bytes differ". If `--expect-old` refuses, the playable disc is not the one this was made for (it must be
   2,333,759,488 bytes, `fe4913f2100e581f781a3e7d817efe0c712c94e7666c3f2366716c852d4e8a80`): stop and report. If the
   whole-image hash does not match `d225b9b8…` but the tool succeeded, report both hashes; do not guess.
4. **Native code for the new executable** (its plain-ELF SHA-1 is `7a0eb8ad3e423c9bd22029768bb54c9779a48ee9`; it took 41 seconds on
   the Windows PC, expect minutes here): `tools/pspc_build.py` on `Games/SWSS-dedicated.iso` with the **same `--build-dir` (and
   `--cache`, if the sessions read another) that the existing playable-disc entry was built with** - see Known Issues. Then
   check that `games.json` in that cache lists the dedicated image with that SHA-1.
5. **Switch the sessions.** Back up the sessions file and keep it private:

   ```sh
   cd ~/swss-server/data && cp -p gamespy-sessions.json gamespy-sessions.json.bak-20261025 && chmod 600 gamespy-sessions.json.bak-20261025
   ```

   Change `ISO=` to `/home/rob/dev/PSPC/Games/SWSS-dedicated.iso` in all four sessions (edit a temp copy and `mv` it into place;
   never print the file). Keep `PLAYLIST`, `UDP_PORT`, `DEDICATED`, `GAME_NAME`. You may deploy this branch's
   `dedicated-host.sh` (atomically, over `~/swss-server/bin/dedicated-host.sh`) but it is not needed: only a header comment differs.
6. **Restart and wait.** `systemctl --user restart swss-server`; the games list 4 to 6 minutes later (each session first makes a
   profile, about two minutes, because `PLAYLIST` deletes it at every start). Look at `~/swss-server/data/sessions/<name>.log` if one does not.
7. **Query the server browser** (TCP 28910). Pass: all four listed, **`numplayers=0` on every one**, `levelsettings >= 1073741824`
   (dedicated bit), and `mapname`/`gametype` exactly `Hoth classic`/`CQST`, `MSTR prequel`/`CTF1`, `Hoth prequel`/`CQST`,
   `MSTR prequel`/`HANV`. The Space session's `levelsettings & 7` should be 4 (space; ground reads 2).

   ```sh
   python3 - <<'EOF'
   import socket, struct, sys
   sys.path.insert(0, 'server')
   from pspc_gamespy import crypt, games
   FIELDS = [b'hostname', b'mapname', b'gametype', b'numplayers', b'levelsettings']
   game = games.find('swsspsp')
   challenge = b'abcdefgh'
   req = (bytes([0, 1, 3]) + struct.pack('>I', 10901) + b'swsspsp\0swsspsp\0' + challenge + b'\0'
          + b'\\' + b'\\'.join(FIELDS) + b'\0' + struct.pack('>I', 0))
   s = socket.create_connection(('127.0.0.1', 28910), timeout=10)
   s.sendall(struct.pack('>H', len(req) + 2) + req)
   data = b''; s.settimeout(1.5)
   try:
       while True:
           c = s.recv(65536)
           if not c: break
           data += c
   except socket.timeout:
       pass
   off = 1 + (data[0] ^ 0xEC); slen = data[off] ^ 0xEA
   plain = crypt.GOACipher(crypt.sb_crypt_key(challenge, game.secret_key, data[off + 1:off + 1 + slen])).decrypt(data[off + 1 + slen:])
   cur = []
   for t in plain.split(b'\xff')[1:]:
       cur.append(t.split(b'\x00')[0].decode('latin-1'))
       if len(cur) == len(FIELDS):
           print(' | '.join('%s=%s' % (FIELDS[i].decode(), cur[i]) for i in range(len(FIELDS))))
           cur = []
   EOF
   ```

   If `numplayers` is still 1: the session is not on the new image (check the first line of its log and `ps` for the `ISO`
   it was given; check that the server was restarted after the edit) or the patched executable is not the one running. If a
   game is missing: read its session log before touching presses; "bad memory access" lines or a profile that was not saved
   are the usual causes.
8. **Watch a round end** (the untested part): leave it running and, per session, confirm in `journalctl --user -u swss-server`
   (`gstats ... game N finished`, then `game N+1 started`) or in `status.json` that a round ends and the next map loads with the
   game still listed and `numplayers` still 0. Rounds are long (none ended within 30 minutes on the Windows PC); `HOURS=6` also restarts
   each session every six hours. If one stalls, see Known Issues; to go back, stop the server, `mv` the backup over the
   sessions file, start it.
9. **Optional, ask the user:** have a real client join a session from the dedicated list (the client keeps the playable disc)
   and play a few minutes; the game counts `numplayers=1` then.
10. **Record and report.** Add the before/after `numplayers` to `handoffs/debug-swss-no-dedicated-servers-listed.md` (attempt 3
    has the Windows-side evidence and says the Linux before/after is still to do), update roadmap `26.1.8`, and tell the user
    the result and the round-end observation. Keep the bundle's `EBOOT.BIN` and `dedicated.txt` under `private/` (git-ignored);
    `/tmp/swss-dedicated` and `Games/SWSS-dedicated.iso` are rebuildable. Commit and merge only with the user's authorisation;
    never push.

## Context Notes

- The sha256 values: playable disc `fe4913f2100e581f781a3e7d817efe0c712c94e7666c3f2366716c852d4e8a80` (2,333,759,488 bytes), its
  `EBOOT.BIN` `efdb4423055514fcf7808c31ac036b394331e22cb71b1886f535e19c6d4f1471`; dedicated disc `d225b9b8f8b87d286f773fe919bcc716c4573e02c0a21b1a5b64d3b379a5ee82`
  (same size), its `EBOOT.BIN` `155a7a7fc84da335f38cf01d578e7aa258ae01480685d006cb3dbaf9d7f61fcf`.
- `numplayers` reaches the browser through the game's own heartbeat (`server/pspc_gamespy/qr.py`), so it can only become 0 if the
  game really stops spawning a local player, which is what this disc does.
- The earlier "no dedicated servers listed" report was the client's list filter, not the server (`levelsettings >= 1073741824` is the
  dedicated list, `< 1073741824` the player-hosted one; *R* switches in the game); see the debug thread, attempt 2.
- The dedicated disc shows the retail menus: the `DED_*` strings in `MENU_AM.ASR` are never reached, so there is nothing to
  capture or re-derive; the Create flow of `dedicated-host.sh` (profile, login, Create, name, playlist, START) is the same.
- `PLAYLIST` indices are the game's own: location = place among the disc's 35 sorted planet codes (12 HOTH, 22 MSTR), mode (0
  Conquest, 1 one-flag CTF, 4 Heroes vs Villains), era (0 prequel, 1 classic), area (0 ground, 1 space where both are offered).
- Windows-side test rig, only if you need to reproduce: agent_net's `pspc`, a scratch `--aot-dir`, a throwaway
  `python -m pspc_gamespy ... serve --bind 127.0.0.1` with a sessions file for the accounts (scratch files, not in the repository).
