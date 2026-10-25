# Handoff: move the SWSS dedicated disc from the Windows machine to the Linux server

## Resume Prompt

Use this exact prompt on the **Windows** machine (the checkout at `C:\Users\yoelr\CFW\PSPC`):

> From the repository root, read `handoffs/2026-10-25-swss-dedicated-disc-transfer.md` first. This machine has the patched
> SWSS dedicated disc; the Linux server (`/home/rob/dev/PSPC`) does not. Produce the two files described in Part 1, with
> their checksums, and report the exact command that built the dedicated disc. Do not change any code.

The **Linux** side reads Part 2 of the same file when the files arrive.

## What This Is For

The four dedicated SWSS sessions on the Linux server (`~/swss-server/bin/dedicated-host.sh`, UDP 3658-3661) currently host
as a **real local player**: every one of them advertises `numplayers=1` in the GameSpy server browser, because the Linux box
runs the **playable** disc `Games/SWSS.iso` and the script drives the normal Create flow (Create, name, Continue, playlist,
START, then a CROSS every 500 VBlanks to take a team and a spawn point each round).

The Windows machine already runs **true dedicated hosts** - no player - using a patched disc. The goal is to get that disc
onto the Linux box so the four sessions host without a player.

Evidence that the game itself supports it (both already established, do not re-derive):

- The disc's localization `MENU_AM.ASR` contains a dedicated-server screen set that the retail menus never show:
  `DED_DEDICATED_SERVER_TITLE`, `DED_COMMAND_TITLE`, `DED_STATUS_TITLE`, `DED_PLANET_SELECT_TITLE`,
  `DED_MAPROTATION_TITLE`, `DED_SCOREBOARD_TITLE`, `DED_NEXT_MAP`, `DED_QUIT`, `DED_HOST`, `DED_STOP`,
  `DED_STATUS_GAMEMODE`, `DED_STATUS_NUM_PLAYERS`.
- `handoffs/2026-10-25-swss-playlist-presses.md` records: "`private/swss/dedicated/SWSS.iso` (same size, built 40 minutes
  later with `patches/dedicated.txt` on the EBOOT) is `d225b9b8…`. So the Linux host plays as a real local player (the script
  takes a team and a spawn point) while the Windows sessions are true dedicated hosts."

PSPC cannot fake this away: `numplayers` comes from the game's own heartbeat (`server/pspc_gamespy/qr.py` reads
`numplayers` from the keys the game sends), and the player slot really is taken.

## The Two Discs

| | Windows | Linux |
|---|---|---|
| Playable | `Games/SWSS.iso` | `Games/SWSS.iso` - 2,333,759,488 bytes, sha256 `fe4913f2100e581f781a3e7d817efe0c712c94e7666c3f2366716c852d4e8a80` |
| Dedicated | `private/swss/dedicated/SWSS.iso` - same size, sha256 `d225b9b8f8b87d286f773fe919bcc716c4573e02c0a21b1a5b64d3b379a5ee82` | **not present** - `private/` here holds only `fw/` and `server-zips/` |

The playable disc's `PSP_GAME/SYSDIR/EBOOT.BIN` on the Linux box is 6,162,040 bytes, sha256
`efdb4423055514fcf7808c31ac036b394331e22cb71b1886f535e19c6d4f1471`.

## Part 1 - On the Windows machine: produce and hand over the files

Collect **both** files, plus the command that made them. Do not copy the whole 2.3 GB ISO unless step 3 shows the EBOOT
changed size.

1. **The patch.** `private/swss/patches/dedicated.txt`. If it references or includes other patch files, include the whole
   `private/swss/patches/` directory and say which one is the entry point. If the patch is applied by a tool (a PPF
   patcher, a Python script under `private/swss/tools/`, or something else), say exactly which and how.

2. **The patched executable.** Extract `PSP_GAME/SYSDIR/EBOOT.BIN` from `private/swss/dedicated/SWSS.iso`. Any ISO reader
   works (7-Zip opens ISO9660, or `pspc-iso`, or a short Python script). Keep the name `EBOOT.BIN`.

3. **Compare it with the playable disc's EBOOT** (`Games/SWSS.iso`, same path). Report both sizes and both SHA-256 values,
   and whether they are the same size. If they differ, the ISO layout was rebuilt rather than patched in place, so send the
   whole `private/swss/dedicated/SWSS.iso` instead (2.3 GB; use a USB stick or a network share, not a zip in chat).

4. **The build command.** The exact command(s) that produced `private/swss/dedicated/SWSS.iso` - how the patch was applied to
   the EBOOT, and whether anything else was changed. This is what the Linux side will reproduce; a guess is not enough.

5. **Package it.** Put the files in one folder with a `SHA256SUMS` (`sha256sum` / `Get-FileHash`) and zip it:

   ```powershell
   $d = "$env:TEMP\swss-dedicated"
   New-Item -ItemType Directory -Force $d | Out-Null
   Copy-Item private\swss\patches\dedicated.txt $d
   Copy-Item EBOOT.BIN $d
   # checksums, sizes and the build command go in $d\NOTES.txt
   Compress-Archive -Path $d\* -DestinationPath swss-dedicated-disc.zip -Force
   Get-FileHash swss-dedicated-disc.zip
   ```

6. **Report back:** the zip's SHA-256, the two files' SHA-256 and sizes, whether the EBOOT sizes match, and the build
   command. Nothing else is needed from this machine.

**Do not** include `private/swss/server/` or anything holding GameSpy account passwords, and do not include the `trees/`
directory (the Linux side has its own copy of the disc data it needs).

## Part 2 - On the Linux machine: instructions for when the files arrive

Working directory `/home/rob/dev/PSPC`; the deployment is `~/swss-server/`. Nothing here is pushed.

1. **Unpack and verify.**

   ```sh
   mkdir -p /tmp/swss-dedicated && cd /tmp/swss-dedicated
   unzip -o ~/swss-dedicated-disc.zip
   sha256sum -c SHA256SUMS
   ```

   Check the patched `EBOOT.BIN` against `efdb4423055514fcf7808c31ac036b394331e22cb71b1886f535e19c6d4f1471` (the playable
   one): it must differ. Check its size is 6,162,040 bytes - that is what allows a plain overwrite (step 3).

2. **Read `NOTES.txt`** and reproduce the patch from `dedicated.txt` on a scratch copy first. Diff the result against the
   supplied `EBOOT.BIN`: they should be identical. If they are not, the supplied file wins (it is the one that has been
   tested) and the patch is recorded as documentation only.

3. **Build the dedicated disc.** Copy the playable disc and overwrite the EBOOT in place:

   ```sh
   cp Games/SWSS.iso Games/SWSS-dedicated.iso
   ```

   The EBOOT's extent comes from its ISO9660 directory record under `PSP_GAME/SYSDIR/`; write the 6,162,040 bytes at that
   offset. `server/deploy/swss/swss-map-iso.py` is the worked example of editing sectors of a disc image this way (it writes
   a copy, changes only the sectors it names, and never rebuilds the ISO). Verify the result: `Games/SWSS-dedicated.iso`
   should be 2,333,759,488 bytes, its `PSP_GAME/SYSDIR/EBOOT.BIN` should equal the supplied file, and every other file on the
   disc should be unchanged. If the supplied EBOOT is a different size, stop and use the whole dedicated ISO from step 3 of
   Part 1 instead - an in-place overwrite is not possible then.

4. **Rebuild the native code.** The AOT cache is keyed by the SHA-1 of the disc's `EBOOT.BIN` as a plain ELF, so the patched
   EBOOT needs a new build (about 6 minutes):

   ```sh
   tools/pspc_build.py Games/SWSS-dedicated.iso
   ```

   The PSPC binary in use is `.worktrees/swss-dedicated/build/LinuxRuntime/bin/pspc` (the sessions file's `PSPC=`).

5. **Point the sessions at the new disc.** In `~/swss-server/data/gamespy-sessions.json`, change `ISO=` for all four
   sessions to `Games/SWSS-dedicated.iso` (back the file up first, `chmod 600`). The `PLAYLIST` values stay as they are:
   Conquest `12.0.1.0`, Conquest 2 `22.1.0.0`, Space `12.0.0.1`, Heroes `22.4.0.0`. Stop the server before replacing a disc a
   running game has open.

6. **Re-derive the presses.** The dedicated flow is not the Create flow: `DED_*` screens (planet select, map rotation, Host)
   come before the game starts, so `dedicated-host.sh`'s timeline and its `playlist_presses()` indices will not carry over
   unchanged. Capture the dedicated menus the way the playlist presses were found:

   ```sh
   "$PSPC" Games/SWSS-dedicated.iso --headless --renderer none --memstick "$DEDICATED/ms0" \
     --set firmware=4.01 --dump-frames /tmp/ded-frames --dump-every 60 --exit-vblank 20000 --log info
   ```

   then read the screens and write the new presses. Keep the `PLAYLIST` index semantics (location, mode, era, area) unless
   the dedicated screens show something different.

7. **Deploy and verify.** Deploy `dedicated-host.sh` atomically (temp file + `mv`) if it changed, `systemctl --user restart
   swss-server`, wait for the games to list (about 4-6 minutes), then query TCP 28910:

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

   Pass: all four games are listed, **`numplayers=0`** on every one, and the pinned `mapname`/`gametype` are unchanged
   (`Hoth classic`/`CQST`, `MSTR prequel`/`CTF1`, `Hoth prequel`/`CQST`, `MSTR prequel`/`HANV`). If `numplayers` is still 1,
   the patched EBOOT is not in use (wrong disc, or the session's `ISO=` was not changed) - check `status.json` and
   `~/swss-server/data/sessions/<name>.log` before touching the presses.

8. **Record and report.** Update the debug thread `handoffs/debug-swss-no-dedicated-servers-listed.md` (attempt 3) with the
   before/after `numplayers` values, note the new disc's sha256 and size in `server/deploy/swss/README.md` §3 (which currently
   says "The game has no server mode" - that sentence is true of the retail disc and must be corrected for the dedicated one),
   and report to the user. Commit only if asked; never push; merging to `main` needs the user's say-so.

## Constraints

- The Linux agent's policy still applies: no `sudo`, no piped `curl`/`wget`, no `rm -f`/`rm -rf` in shell commands (plain
  `rm <file>` is fine), no raw sockets. Deploy atomically; never rebuild PSPC in place in `build/LinuxRuntime`.
- Do not leak `~/swss-server/data/gamespy-sessions.json` (account passwords). Do not retry the `qr.py` `DEDICATED_LEVEL`
  marking - it is pinned by `server/tests/test_gamespy_security.py`.
- Keep the four pinned playlist entries as they are; this change is about who hosts, not what is hosted.
- Bound artifacts: `/tmp/swss-dedicated`, `/tmp/ded-frames` and `Games/SWSS-dedicated.iso` are disposable after the change is
  verified; keep the supplied `EBOOT.BIN` and `dedicated.txt` under `private/` (git-ignored) if a rebuild is ever needed.

## Context Notes

- The disc of 2026-10-24 is 2,333,759,488 bytes with sha256 `fe4913f2…`; its `EBOOT.BIN` is 6,162,040 bytes with sha256
  `efdb4423…`. The dedicated disc is the same size, `d225b9b8…`, so the change is inside the EBOOT and did not move anything
  else on the disc.
- PSPC's AOT cache (`cache/aot/games.json`) records each disc's `modules` and their SHA-1s, and `size`; a disc whose EBOOT
  changed needs `pspc_build.py` before the launcher will use native code for it (see `server/deploy/swss/README.md`,
  "A new disc").
- `dedicated-host.sh` runs `pspc "$ISO" --headless --renderer none --memstick ... --set firmware=4.01 --osk-text ... --press
  VBLANK:BUTTON:6 --exit-vblank ...`. `--dump-frames <dir> --dump-every <n>` is how the menus were read before; the captured
  frames from the playlist work are under `~/swss-server/dedicated/frames/`.
- The session names and UDP ports are fixed: Conquest 3658, Conquest 2 3659, Space 3660, Heroes 3661. `numplayers` reaches
  the browser through the game's heartbeat, so it can only change if the game stops spawning a player.
- Open question, answered by the Windows side in Part 1: whether `dedicated.txt` is a text patch applied by a tool, and
  whether the EBOOT stayed the same size. Both determine whether the Linux side reproduces the patch or copies the file.
