# Games: plan

## Status (2026-10-12)

Phase 0 is done: the checkpoint harness, deeper scripted paths for all seven games (42
checkpoints, every one byte-identical from run to run), and the fix for freshly compiled Daxter.
The rest is the plan. The four decisions in Phase 3 are deferred, as a to-do list for later.

## Summary

| Game | Furthest point checked | Interpreter | Gameplay speed (native) | What's in the way |
|---|---|---|---|---|
| Daxter (NPUG80329) | first level, at the light beacon | same frames | 145-190 VBlanks/s | the beacon never triggers: script or bug, unknown (1.1) |
| Syphon Filter: Combat Ops (NPUG80114) | Mission Modifier's 3D spawn editor | same frames | 56-84 in 3D | nothing offline; matches need two players (1.2) |
| Syphon Filter: Logan's Shadow (UCUS98606) | Episode 1 Part 1, on the ship | same frames | **22-35** (0.4-0.6x) | GE vertex work (2.1) |
| Monster Hunter Freedom Unite (ULUS10391) | first quest, Snowy Mountains | same frames | 118-145 | nothing; overlays run interpreted (1.4) |
| Star Wars Battlefront II (ULUS10053) | Dagobah battle | the only mode | **16-19** (0.3x) | no native code, GE vertex work (1.5) |
| Battlefront: Elite Squadron (ULUS10390) | Bespin ground battle | same frames | 86-183 | nothing (1.6) |
| Battlefront: Renegade Squadron (ULUS10292) | Boz Pity battle | same frames | 200-260 | nothing (1.7) |

60 VBlanks/s is real time.

- **"Same frames" means byte-identical at every 300 VBlanks along the path, where checked.**
  Before the fixed clock, runs started in different seconds diverged once the game drew random
  numbers. That limited the comparison in battles.
- **No PSPC bug stops single-player play in six of the seven games.** Renegade Squadron only
  looked stuck: its old script never pressed DOWN to reach Accept.
  - Several games go further than `docs/COMPATIBILITY.md` said: Elite Squadron and Combat Ops
    in the interpreter, MHFU into a quest, Battlefront II at all.
  - Daxter's first-level beacon is the one open correctness question.
- **The biggest gap is speed:**
  - Logan's Shadow missions and Battlefront II battles run below real time.
  - In both, most of the emulation thread goes to the GE's vertex work: decoding, transform
    and lighting with exact float24 arithmetic, one vertex at a time. The rasterizer threads sit
    idle.
  - The Direct3D 11 renderer doesn't help, because it does the same vertex work on the CPU.
- **Native code goes stale:**
  - Battlefront II has none.
  - Daxter's and Combat Ops' DLLs predate their hints.
  - MHFU's overlays can't be compiled ahead of time.
- **Not checked:**
  - audio: headless runs have no sound output (0.1);
  - online play: covered by `docs/combat-ops-server.md` and `docs/gamespy-server.md`, run by hand.

## Ground rules

- **Pass the gates in every change:**
  - unit tests;
  - pspautotests with 539 passing and 20 known differences, identical in the interpreter and as
    AOT;
  - `tools/guest_tests.py`;
  - `tools/smoke_games.py`, with all checkpoints byte-identical to the references.
- **References change only when a script goes further or a fix changes what a game shows.** Look
  at the new frames before re-recording (`--record`), and say so in the commit.
- **Speed work keeps frames byte-identical.** The exact GE arithmetic is the reference: the gpu
  autotests and the checkpoints check it.
- **Test runs are isolated** (`tools/smoke_games.py` does all of this):
  - networking off;
  - a fixed guest clock (`--rtc-base`);
  - a fresh memory stick;
  - `--no-plugins` (which no longer lays the plugins folder over ms0:);
  - an explicit flash0.

## Phase 0: The harness (done, apart from 0.1 and 0.2)

What went in:

- **Running to a point in game time:** `pspc --exit-vblank <n>` stops a run after VBlank n.
- **Checkpoints:** `tools/checkpoints.txt` lists them; `tools/smoke_games.py --record` stores
  this machine's references in `build/checkpoints`. A run fails if a checkpoint frame changes.
- **Repeatable runs:**
  - **Networking off.** `--config none` left it on. A game that starts ad hoc networking
    (Battlefront II does, even in single player) makes PSPC keep real time, so frames followed
    the wall clock. That was the 2% variation seen on Battlefront II's title.
  - **A fixed boot time** (`pspc --rtc-base <seconds>`). Games seed their random numbers from
    the clock at boot: Logan's Shadow through `sceKernelLibcTime`, MHFU through
    `sceRtcGetCurrentClockLocalTime`.
  - **A fresh memory stick per run.** pspc's own ms0 held a real Elite Squadron profile, which
    changes the path, and some paths save.
  - **`--no-plugins` no longer overlays the plugins folder onto ms0:.**
- **Richer input:**
  - analog stick directions in `--press`: AUP, ADOWN, ALEFT, ARIGHT;
  - `osk=<text>` in `tools/smoke_inputs.txt`, for the system keyboard (MHFU and Elite
    Squadron name a profile with it).
- **Scripted paths into gameplay for all seven games.** They came from four parallel
  investigations; the checkpoints sit along them.
- **Guest programs:** `tools/guest_tests.py` runs `tests/guest` programs in the interpreter and
  as freshly compiled native code.
- **The Daxter fix:**
  - **Before:** its task scheduler swaps `$sp` and jumps, and freshly compiled code ran out of
    host stack on entering the first level.
  - **After:** a jump that leaves its stack unwinds (docs/RECOMPILER.md).
  - **Regression test:** `tests/guest/coroutines`.

### 0.1 Headless audio check (S-M)
- **The problem:** nothing checks sound today. The games' sound engines run without errors
  (SAS, ATRAC3, `sceAudioOutputPannedBlocking`), but headless runs mix to nowhere.
- **The change:** `pspc --dump-audio <wav>`, from the mixer in
  `src/runtime/hle/modules/audio.cpp` (`AudioUpdate`), as PPSSPP's `bDumpAudio` does. Then a
  per-checkpoint level (silent, or RMS over the last second) in the harness.
- **Verify:** non-silent menus and battles in every game. Deliberately muting a voice shows up.

### 0.2 Interpreter against the native references (S)
- **The change:** `smoke_games.py --interp --refs-mode aot` compares interpreted frames with the
  native references. With the clock fixed they should be identical. That makes every game a
  differential test of the recompiler, as pspautotests already are.
- **The cost:** an interpreted run of MHFU's 36600 VBlanks takes about 8 minutes. Run it before
  recompiler changes, not on every commit.

## Phase 1: Per game

### 1.1 Daxter (NPUG80329)
- **Status:**
  - Title, New Game and the first level, where the hint says to walk into the light beacon.
  - Walking up to it works, with the analog stick.
  - Entering it does nothing. About 40 scripted variants were tried: angles, running through,
    standing in it for 1600 VBlanks, every button.
  - There are no warnings and no unimplemented calls. The interpreter gives the same frames.
- **1.1.1 (S, needs a person):** play to the beacon in the windowed build, with the stick on
  I/J/K/L. That tells "the script misses the trigger" from a real fault. The minimap's Osmo
  marker sits a little north of the beacon, so the trigger may be elsewhere.
- **1.1.2 (M, if it fails by hand too):**
  - Run PPSSPP to the same point and find the trigger test in the game (`tools/ghidra`,
    `PSPC_TRACE_PC`).
  - Compare Daxter's position and the mission state between the two.
  - **Leads:**
    - CPU semantics in the collision test (VFPU/FPU in `src/allegrex`, which both modes share,
      so identical modes don't clear it);
    - sound-voice state: the level polls `sceSasGetEnvelopeHeight` up to 17,900 times per 100
      frames (`audio_sas.cpp` and `src/audio/sas.cpp` against PPSSPP's `Core/HLE/sceSas.cpp`).
  - **Verify:** a checkpoint after the beacon.
- **1.1.3 (S):** rebuild the native code with its hints. Every run still records 14 interpreter
  entries.

### 1.2 Syphon Filter: Combat Ops (NPUG80114)
- **Status:**
  - Offline, the game offers the Mission Modifier (create, save and load missions, the 3D
    Combat Modifier), profiles, options and credits.
  - A match needs at least two players (the game's own `STRINGS.DAT`). The training screens in
    its code are Logan's Shadow leftovers whose files aren't on the disc.
  - All of that works natively and in the interpreter. Online matches are in
    `docs/combat-ops-server.md`.
- **1.2.1 (S):** rebuild the native code. The DLL from 2026-10-10 lacks 4 entry points that its
  hints list, so 184-412 instructions per run fall back to the interpreter.
- **1.2.2 (M, part of 2.1):** the 3D editor costs 10-15 ms of GE time per VBlank with the
  software renderer (5300 primitives). A window would drop under 60 fps when the machine is
  busy.
- **1.2.3 (S-M, Direct3D 11):** in 33 s of the 3D editor the renderer logged:
  - 2540 framebuffer downloads;
  - 2504 guest faults;
  - 4 s of waiting.

  Find what the guest reads back from VRAM (the download path in
  `src/gpu/hw/d3d11_backend.cpp`). PPSSPP's `compat.ini` lists this game under
  `[SoftwareRasterDepth]`, which points at light-flare depth tests, and under
  `[DisableMemcpySlicing]`.

### 1.3 Syphon Filter: Logan's Shadow (UCUS98606)
- **Status:**
  - The profile, the menus, Training 1 and story Episode 1 Part 1 all play.
  - A long run went on until the game's own "MISSION FAILED" (the scripted player doesn't
    destroy the missile launchers), so the game logic runs.
- **1.3.1 (part of 2.1):** missions run at 22-35 VBlanks/s (24-38 ms of GE time per VBlank,
  9.7k-11.5k primitives). That is the main reason to do 2.1.
- **1.3.2 (S):** at the story's start, `media_engine.cpp` `SetupStreams` warns "unexpected
  stream count: 30720". The disc's PMF files have count 2 at offset 0x80, so the header PSPC
  passes in isn't the file's. The fallback plays the movie; fix the header source.
- **1.3.3 (S, low):** stub the four `sceNetApDialogDummy` NIDs (3811281e, bb73ff67, ca9be5bf,
  f213be65). They are unresolved imports now; only infrastructure multiplayer could reach them,
  and PPSSPP doesn't implement them either.
- **1.3.4 (S):** the main menu's ticker says "SYSTEM ERROR - - - RECONNECTING...". It's
  probably the game reporting that the WLAN is off; check it once with networking on.

### 1.4 Monster Hunter Freedom Unite (ULUS10391)
- **Status:**
  - Character creation (named with `--osk-text`), the intro, saving at the bed, Continue, Pokke
    Village, the Village Chief's quest and the Snowy Mountains, with the HUD, movement and
    attacks.
  - Saves, autoload and Data Install (580 MB in 183 writes) work.
  - There are no warnings. The interpreter gives the same frames.
- **1.4.1 (S):** in the village the D-pad and L seem to do nothing, while in the field D-pad LEFT
  turns the camera. Both come from the same `sceCtrlReadBufferPositive` call, so it is likely the
  game's village camera. Confirm once on PPSSPP.
- **1.4.2 (L, optional):** native code for runtime-loaded overlays.
  - **The gap:** 4.6% of guest instructions still run interpreted. 213 of the 220 hint addresses
    fall in the EBOOT's empty overlay segments, which the game fills from `DATA.BIN` at run time,
    and `pspc_build.py` can't compile code that isn't in the ELF (a known limit in
    docs/RECOMPILER.md).
  - **An approach:** capture overlay images by hash, compile them as extra modules, and register
    one when an icache invalidation leaves a matching image in memory.
  - **Verify:** fewer interpreted instructions, frames unchanged.
- **1.4.3 (S, optional):** a second path with Data Install on. It adds a dialog at boot and then
  reads the installed files.

### 1.5 Star Wars Battlefront II (ULUS10053)
- **Status:**
  - New to the compatibility list.
  - Plays Instant Action ground battles (Dagobah: spawn as a clone trooper, run, fire, capture
    command posts) and a space map.
  - Challenges reach character select. The profile saves and loads.
  - There are no warnings and no unimplemented calls. Interpreted only, because no native code
    has been built.
- **1.5.1 (S):** build its native code (`tools/pspc_build.py Games\SWBF2.iso`). The estimate is
  about 30 VBlanks/s from that alone.
- **1.5.2 (part of 2.1):**
  - **Load:** battles draw 10-15k primitives per VBlank, against 1.8k for Renegade Squadron.
  - **Where the time goes:** 25-50 ms of GE time per VBlank with the software renderer, and
    27-28 ms with Direct3D 11. Half the samples of the emulation thread were in `gpu::Run`
    (`ReadVertex`, `GERowSum4`/`GEDot3`, `ProcessLighting`).
- **1.5.3 (M, unconfirmed):** on Space Yavin, when the pilot walks to the hangar mouth, about 75%
  of the screen turns flat white under a strip with the planet, the same in both renderers.
  - It may be the hangar's force field drawn opaque.
  - Compare with PPSSPP or a PSP at the same spot.
  - If PSPC is wrong, add a GE dump of the draws that cover a given pixel (state and vertices).
- **1.5.4 (part of 2.2):** with networking on, the game slows to the wall clock from boot,
  because of its `sceNetAdhocctlInit`.

### 1.6 Star Wars Battlefront: Elite Squadron (ULUS10390)
- **Status:**
  - Offline natively and in the interpreter: profile creation, Instant Action ground and space
    battles, the campaign tutorial, the Galactic Conquest map and the tutorial videos.
  - A 12.5-minute battle ran without trouble.
  - Loading an existing profile works. Multiplayer with the WLAN off gives "Connection Failed"
    and goes back.
  - Online play is in `docs/gamespy-server.md`.
- **1.6.1 (S, optional):** a second scripted path for a space battle or the campaign.

### 1.7 Star Wars Battlefront: Renegade Squadron (ULUS10292)
- **Status:**
  - Natively and in the interpreter: profile creation, Instant Action ground battles (Boz Pity,
    Hoth), a space battle, and Campaign (the opening crawl and the Yavin 4 tutorial level).
  - 36000 VBlanks of gameplay ran without a crash. The profile saves and loads.
- Nothing to do beyond keeping the checkpoints.

## Phase 2: Shared work

### 2.1 GE vertex work (L, the biggest item)
- **Evidence:**
  - **Logan's Shadow:** the emulation thread spends 82% of its time in `gpu::Run`, split as
    follows:

    | Part | Share |
    |---|---|
    | `ReadVertex` | 38% |
    | `ProcessLighting` | 20% |
    | `binner::Flush` | 32% (16% caused by `TargetChanged`) |
    | `DrawRectangle` | 16% |

  - **Battlefront II:** 15 of 30 samples were in `gpu::Run`.
  - **The renderers:** the raster workers are idle 89-94% of the time, and Direct3D 11 does the
    same vertex work on the CPU.
  - **The cause:** `src/gpu/transform.cpp` transforms each vertex with scalar float24-exact
    arithmetic (`gemath.cpp`: "scalar versions only"), about 2 µs per primitive.
  - **PPSSPP:** it decodes vertices with a JIT and transforms them in shaders. Its exact
    arithmetic is only in its software renderer, which uses SSE.
- **Work, measured stage by stage with `PSPC_GPU_STATS`:**
  - (a) Bit-exact SIMD versions of `GERowSum4`, `GEDot3`, `GENormalize3` and the lighting loop.
    The exact path stays the definition: compare the two on every vertex in a test build.
  - (b) Vertex decoding specialised per vertex format: templates per format, instead of decoding
    field by field.
  - (c) Transform of large indexed draws in parallel chunks. `ReadVertex` already pre-transforms
    into a cache.
  - (d) `binner::Flush` on `TargetChanged`: avoid flushes when the target doesn't really change.
- **Verify:**
  - every checkpoint byte-identical;
  - the gpu pspautotests unchanged;
  - Logan's Shadow missions and Battlefront II battles (with native code) at 60 VBlanks/s or
    more;
  - the other games faster too.

### 2.2 Ad hoc networking keeping real time (M)
- **The problem:** `sceNetAdhocctlInit` (`net_adhocctl.cpp`) calls `net::HostSocketOpened()`,
  which keeps real time until `CtlShutdown`. `sched::IdleUntil` then sleeps for real, and
  `--fast-time` stops working.
  - Battlefront II calls it at boot even in single player. With networking on, 3000 VBlanks of
    boot took 68 s; with it off, 4500 VBlanks took 18 s.
  - PPSSPP blocks only while logging in.
- **The change:** keep real time only while a guest thread waits on the network (`WaitFor`
  already does) or while connected or scanning.
- **Verify:**
  - Battlefront II boots as fast with networking on as with it off;
  - `tools/net_selftest.py` passes;
  - the two-instance joins in `docs/gamespy-server.md` and `docs/combat-ops-server.md` still
    work.

### 2.3 Fresh native code (S)
- **The change:** rebuild Daxter and Combat Ops, and build Battlefront II.
  - `pspc_build.py` already rebuilds a module whose hints are newer than its DLL.
  - Since Phase 0, a first build also survives Daxter's stack switches and records its hints.
- **Also:** the launcher could say when hints are newer than the native code ("native code can be
  improved: Build"), so players' builds don't go stale.

### 2.4 Online checkpoints (M, optional)
The server documents have manual two-instance procedures. A scripted version would protect the
online features in the same way:
- the Combat Ops lobby and a match;
- an Elite Squadron match.

Joining needs both instances in real time (not `--fast-time`), so it can't be byte-exact. It
would check states instead (in the lobby, in a match) from the servers' logs.

## Phase 3: Decisions, deferred to later (to do)

Set aside on 2026-10-12, to come back to. Each one says what to do and what it decides.

- [ ] **3.1 Play to Daxter's light beacon by hand** (1.1.1).
  - **What to do:** a person plays the windowed build: New Game, then walk into the first level's
    light beacon (analog stick on I/J/K/L).
  - **What it decides:** whether the beacon triggers. If it does, record the inputs and extend
    the script and checkpoints past it. If not, it's a bug: do 1.1.2.
  - Scripted attempts (about 40) can't settle it.
- [ ] **3.2 Pick the renderer for Combat Ops' 3D editor** (1.2.2).
  - **What to do:** first do 1.2.3 (why Direct3D 11 reads VRAM back so often there). Then
    measure both renderers in the editor under load.
  - **What it decides:** keep the software default, or recommend Direct3D 11 for this game.
- [ ] **3.3 Decide on online checkpoints** (2.4).
  - **What to do:** weigh a scripted two-instance check (the Combat Ops lobby and a match, an
    Elite Squadron match) against keeping the manual procedures in the server documents.
  - **The catch:** joining needs real time, so such a check would compare states from the
    server logs, not frames.
- [ ] **3.4 Decide on MHFU's overlays as native code** (1.4.2).
  - **What to do:** decide after 2.1 (GE vertex work) is done, with fresh numbers for how much
    of MHFU still runs interpreted (4.6% today).
  - **The cost:** it's an L item. My recommendation is to do it after 2.1, if at all.

## Order and effort

| Item | Effort | Depends on |
|---|---|---|
| 2.3 fresh native code | S | — |
| 1.3.2, 1.3.3, 1.3.4, 1.4.1 | S each | — |
| 0.2 interpreter against native references | S | — |
| 0.1 headless audio check | S-M | — |
| 2.2 networking and real time | M | — |
| 1.1.1 → 1.1.2 Daxter's beacon | S + M | a person (3.1) |
| 2.1 GE vertex work | L (several steps) | — |
| 1.2.3 Direct3D 11 downloads | S-M | — |
| 1.5.3 Battlefront II's hangar | M | a PPSSPP or PSP reference |
| 1.4.2 MHFU overlays | L | 3.4 |
| 2.4 online checkpoints | M | 3.3 |
