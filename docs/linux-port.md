# Linux port status

The Linux runtime and a native GTK game-library launcher now build and pass the automated
suite below. Retail validation and its remaining limits are documented below. The port is **in progress**, not full
Windows feature parity or a supported release. Windows backends remain in the source tree, but have not been built or tested
on Windows after these changes.

## Buildable foundation

On a Linux host with a C/C++20 compiler, CMake 3.21+, Ninja, Python 3 and pthreads:

```sh
cmake -S . -B build/RelWithDebInfo -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPC_BUILD_RUNTIME=OFF
./build.sh RelWithDebInfo
ctest --test-dir build/RelWithDebInfo --output-on-failure -R '^unittests$'
```

`build.sh` reuses that CMake configuration. `JOBS` controls build parallelism (default
four). This configuration builds the filesystem, loader, semantics, recompiler and
decryption tools, not the emulator or launcher. The unit executable also builds the
host socket, DNS and ad hoc server code without the runtime/HTTP dependencies.

The foundation build and unit CTest passed on Fedora 44 with GCC 16.2.1: 75 registered
unit cases, zero reported failures. This includes TCP/UDP loopback, refused connects,
network poller watches, ad hoc group operations, fiber stack/rounding-mode preservation,
and event reset/wakeup behavior. Several reference-dependent VFPU cases report internal
skips because `reference/pspautotests` and `reference/pspdecompiler` are absent; the count
is not a claim that those hardware comparisons ran.

After provisioning the gitignored M33
[test fixtures](../tools/pspc-decrypt/README.md#test-fixtures), a full tools-only CTest run
passed all three entries (`unittests`, `pspc_decrypt_tests`, and `aot_pipeline`):

```sh
ctest --test-dir build/RelWithDebInfo --output-on-failure
```

The separate decryption suite executes all 38 cases and 7,224 checks with zero failures,
including the real installer's eleven module hashes, SDK sample decoding and CLI cases.
The previous eight failures were caused by missing M33 input files, not a Linux decoder
or subprocess defect. Restoring the original inputs resolved them without changing code,
assertions or skip behavior. The related filesystem M33/PBP tests also run with these
fixtures. This does not validate a real Sony updater. Full-runtime checks are described below.

The intermittent ten-assertion failure in `adhoc_server_groups` was reproduced after
36 passing runs and traced to a pre-existing server timeout bug, also present in the
Windows-only code at `c74d91f`: the loop sampled `now` before receiving packets, then
subtracted a potentially newer receive timestamp. A millisecond tick could wrap the unsigned
elapsed time and disconnect an active client immediately. Sampling the clock after each
user's receive/processing step fixes the ordering without changing the 15-second timeout.

The new `adhoc_server_activity_across_clock_tick` regression queues two loopback clients
before serving them and deliberately slows login logging across a clock tick. It failed
before the fix and passed afterward. Both ad hoc tests passed 100 consecutive fresh-process
runs (200 test executions), followed by a passing full tools-only CTest run. Existing group
assertions were preserved; reference-dependent VFPU skips remain as noted above.

## Synthetic AOT build and loading

The Linux-only `aot_pipeline` CTest generates three tiny fixed-address MIPS ELFs and runs
`tools/pspc_build.py` through both direct and generated-CMake builds. No PSP compiler,
firmware assets, or full runtime is needed:

```sh
./build.sh RelWithDebInfo --target pspc_aot_probe
ctest --test-dir build/RelWithDebInfo --output-on-failure -R '^aot_pipeline$'
```

Both routes passed with GCC and Clang. The native probe loads the resulting `.so` files
through `hostsys`, rejects invalid ABI inputs, checks descriptor hashes/semantics versions,
and executes generated call/body entries. It checks guest loads/stores, arithmetic, a return
delay slot, budget accounting, syscall callbacks, isolation between simultaneously loaded
modules, and patched-entry handoff to an interpreter callback. A third ELF selects guest
round-up with `CTC1`, computes `1.0f + 2^-24`, returns its bits, and restores round-to-nearest.
The probe requires `0x3f800001` through both build routes. The Python driver also
checks `games.json`, warm-cache reuse, and selective rebuilding of a stale semantics stamp.
It uses relative work/cache paths with spaces and an input filename with non-ASCII text.
Generated fixtures, library snapshots, and modules are removed after each test; failure
output remains in the CTest log.

This exposed a reproducible `--cmake` failure: a relative library snapshot path became a
`-lwork/.../libpspc_sem.a` library name at link time. Resolving the snapshot directory to an
absolute path fixes the boundary. The regression failed before this change and passed
afterward, followed by the full tools-only build and all three CTest entries.

The round-up regression also exposed GCC folding this sum to `0x3f800000` despite the
emitted `FENV_ACCESS` pragma. Both Linux build routes now use `-frounding-math`; the regression
failed independently in each route before the flag was added. The cache semantics version
now includes the emitter and build script, so older miscompiled modules are rebuilt rather
than reused, and rejected by the runtime until rebuilt.

This validates the host loading boundary and generated-code ABI, not retail AOT execution
or firmware execution. The runtime-linked check below covers actual interpreter fallback;
the interpreted game checks establish a separate, bounded runtime result.

### Synthetic AOT through the runtime

With the full runtime dependencies below, `aot_runtime` builds two tiny fixed-address
ELFs and links its probe against `pspc_runtime`, rather than substituting ABI callbacks:

```sh
cmake --build build/LinuxRuntime --parallel 4 --target pspc_aot_runtime_probe
ctest --test-dir build/LinuxRuntime --output-on-failure -R '^aot_runtime$'
```

The GCC check passes through the real kernel module loader, CPU API, dispatch table and
interpreter. It checks native registration and execution (zero interpreted instructions),
then patches a callee and invalidates its code. The still-native caller and a direct
dispatch miss both execute the changed callee through the real interpreter and produce
the expected memory/return values. Disabled AOT, a missing cache entry and a present but
hash-mismatched module all correctly use interpreted execution. Every mode checks unload
removes module/dispatch entries, releases guest memory and reports no bad memory accesses.

The initial probe allocated its scratch/stack over the fixed ELF address; allocating
scratch at the top of user RAM corrected the fixture without changing runtime behavior.
Temporary inputs, snapshots and caches are removed after each run; diagnostics remain in
CTest logs. No game assets, source saves, acquisition attempt or save reload are involved.
The full Linux build and all **16 CTest entries** pass with this check and the UDP receive
tests below registered. The tools-only build and its existing `aot_pipeline` test also
pass, without requiring the runtime probe. This does **not** cover relocatable PRXs, guest
imports, scheduler-driven AOT execution, retail AOT or firmware/XMB.

## Guest UDP receive truncation

The Linux socket wrapper uses `recvmsg` to preserve truncation information, normalizing
it to the same copied-prefix/`EMSGSIZE` result as the Windows backend. The existing guest
`sceNetInetRecvmsg` path then sets `PSP_MSG_TRUNC` (`0x10`) for oversized datagrams only.

`net_recvmsg_exact_fit` and `net_recvmsg_oversized` exercise registered guest HLE functions
with loopback UDP and temporary guest RAM. They check copied length and payload, exact
output flags, peek versus consume, source address, buffer bounds and discarded tails.
No game assets or savedata are used. Run them after building the runtime:

```sh
ctest --test-dir build/LinuxRuntime --output-on-failure -R '^net_recvmsg_'
```

## Full runtime and launcher

In addition to the foundation dependencies, install development packages for SDL2,
FFmpeg (`libavcodec`, `libavutil`, `libswresample`, `libswscale`), FreeType, fontconfig,
libcurl 7.85+, GTK 3.22+, OpenGL development libraries, and pkg-config. With native system packages:

```sh
cmake -S . -B build/LinuxRuntime -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPC_BUILD_RUNTIME=ON
cmake --build build/LinuxRuntime --parallel 4
ctest --test-dir build/LinuxRuntime --output-on-failure
build/LinuxRuntime/bin/pspc-launcher Games --list
build/LinuxRuntime/bin/pspc-launcher Games
python3 tools/net_selftest.py --pspc "$PWD/build/LinuxRuntime/bin/pspc"
```

Validation used Fedora 44, GCC 16.2.1, SDL2 compatibility headers 2.32.74, FFmpeg 8.1.3,
and GTK headers 3.24.52. Matching development RPMs were extracted locally instead of
installed system-wide; that environment additionally required
`CMAKE_PREFIX_PATH=build/linux-deps/root/usr`, its `lib64/pkgconfig` in `PKG_CONFIG_PATH`,
and `-DPKG_CONFIG_ARGN=--define-prefix`. This local dependency tree is not distributed.

The default full build includes `pspc`, `pspc-iso`, `pspc-launcher`, the recompiler,
decryption tools and tests. Native dependency selection replaces Windows `.lib`/resource
rules only on Linux. Guest networking uses the host socket/event layer, with abstract
Unix sockets reserving per-process network identities. HTTP uses libcurl's streaming
multi interface with bounded body buffering, phase timeouts and concurrent cancellation;
normal TLS verification uses the host trust store. The existing explicit revival-server
certificate exception remains scoped by the request's `connectAddress` setting.

Checks completed:

- Full build and all CTest entries: 126 unit cases with zero failures, 38 decryption cases
  with 7,224 checks, synthetic AOT pipeline, and the Linux launcher CLI test. The same
  reference-dependent VFPU skips remain; no assertions were removed. GPU fixture memory
  allocation is portable; the optional Windows sampling profiler is not a Linux feature
  (use `perf`), but GPU correctness tests and benchmarks run on Linux.
- New tests cover HTTP headers returning before a stalled body, header/read timeouts,
  cancellation during both Send and Read, persistent cancellation, Unicode ISO/PBP
  metadata, scan boundaries, and display-independent launcher CLI behavior.
- All seven existing guest networking scenarios pass: inet, HTTP, ad hoc matching/data,
  game mode, dialog, unavailable server, and offline mode. A separate `--internet http`
  run also fetched both HTTP and HTTPS `example.com` successfully.
- The GTK launcher listed all three supplied ISOs and stayed up for a four-second GUI
  smoke with GLib warnings treated as fatal. Its game metadata code is shared with the
  Windows launcher. Missing directories and unavailable displays produce explicit errors.
- A windowed SDL Combat Ops run reached VBlank 720 and exited at the requested limit
  with zero reported bad memory accesses. Its inspected host-renderer capture shows the
  Sony intro, confirming presentation beyond the guest framebuffer dump. Volume was
  muted; this does not verify audible output quality.
- The tools-only build and all three of its CTest entries still pass.

The GTK frontend now includes settings/input, plugins, account/friends, server/admin/routing,
artwork, native-cache status, build-before-play and firmware-install workflows. See the
[implementation and synthetic verification](#linux-renderer-and-launcher-workflows) below;
the earlier startup smoke alone did not establish these actions.

### Bounded retail interpreter smoke

The three supplied ISOs were run without modifying `Games/`, with networking and plugins
off, fresh writable state, no installed firmware, and the software renderer. Each run
was limited to 30 wall seconds or VBlank 1800, with at most six frame dumps. All exited
with the requested limit code 124, with zero reported bad guest-memory accesses:

| Game | Last captured VBlank | Captures / distinct frames |
|---|---:|---:|
| Combat Ops (`NPUG80114`) | 1800 | 6 / 6 |
| Dark Mirror (`UCUS98641`) | 1200 | 4 / 4 |
| Logan's Shadow (`UCUS98606`) | 1500 | 5 / 5 |

The final frames were inspected: each reached its title/save-data notice. Dark Mirror
and Logan's Shadow stopped at the wall-time limit rather than reaching VBlank 1800.
Logan's Shadow logged four unresolved `sceNetApDialogDummy` imports; this smoke did not
exercise them. These are boot/render checks, **not gameplay completion, real-time speed,
retail AOT, firmware/XMB, or checkpoint-reference comparisons**. No reference frames were
created or updated. Local logs, frames and the summary are retained in
`build/linux-runtime-smoke/initial/` (one bounded validation batch, not an accumulating
capture loop). The final windowed presentation capture is in `SFCO-sdl-late/` below it.

### Initial longer input, audio and save validation — partial

Longer windowed interpreter sessions exercised all three games with isolated writable
state, networking/plugins disabled, volume 100, and both real-time and virtual-time
(`--fast-time`) scheduling. Original saves, `Games/`, and checkpoint references were not
modified. Runtime limits were at most 600 wall seconds or 18,000 VBlanks, with a
25-second wrapper termination grace and at most 60 periodic frame captures. Initially,
twenty cases, including three harness setup failures and two repair-validation waves,
occupied about 971 MiB under a 1.5 GiB aggregate retention budget. The setup failures were
caused by a harness file-size limit also limiting Linux guest-memory memfd allocation;
removing that limit fixed setup without changing emulator memory code.

Two scheduling hazards were reproduced and repaired:

- Real-time audio submission no longer sleeps waiting for SDL queue space. It reports
  a dropped block instead of allowing host backpressure to hold up the emulator; the
  existing approximately 100 ms queue bound remains. Virtual-time submission still waits
  to pace execution. The `host_audio` regression timed out after five seconds before the
  change, then passed in about 0.45 seconds, checking bounded/nonblocking submission and
  retained virtual-time pacing with real SDL dummy devices.
- Real-time timer dispatch uses a fixed due-time cutoff for each batch. Repeatedly
  sampling a moving wall-clock deadline could indefinitely defer interrupts/guest work
  when callbacks took longer than their recurrence interval. Virtual-time dispatch keeps
  its existing clock semantics. The `scheduler` regression deliberately overloads a
  periodic callback: interrupt dispatch was deferred until the 100-callback failure
  limit before the change; afterward it runs after one callback.

These two repairs alone **did not resolve the observed retail stalls**. That build passed all
six CTest entries (decryption, units, scheduler, host audio, synthetic AOT and launcher
CLI). Targeted checks can be repeated with:

```sh
ctest --test-dir build/LinuxRuntime -R '^(host_audio|scheduler)$' --output-on-failure
```

Final replay results with both repairs, each reaching VBlank 18,000 in approximately
301 seconds with zero reported bad guest-memory accesses:

| Game | Final observed state | Save/reload evidence across the sessions | Final recorded audio |
|---|---|---|---|
| Combat Ops | Black after intro | A virtual-time run created an encrypted profile and reached Mission Modifier setup over a rendered level; fresh-process reload and editor movement remain unconfirmed | Silent |
| Dark Mirror | Remained on animated Sony intro | A virtual-time run created a profile; the audio-only repair build subsequently loaded it and reached training mission loading. The final build did not reach reload | Silent |
| Logan's Shadow | Episode 1, Part 1 loading screen | Profile creation/reload passed; a later run wrote an 83-byte story-progress payload (99-byte encrypted file). The final fresh process accepted that save and offered Continue, but mission resume remains unconfirmed | Non-silent in 75 of 300 one-second intervals |

The final three runs were concurrent, not isolated performance measurements. Earlier
real-time Dark Mirror runs varied between intro and mission loading; Combat Ops also
stalled in a separate diagnostic run. A final Combat Ops debugger sample finds software
rasterization inside GE interrupt dispatch with two ready guest threads. This localizes
further investigation but does not prove the remaining root cause. Logan's Shadow still
reports four unresolved `sceNetApDialogDummy` imports and a media stream-count warning
(30720); an earlier Dark Mirror run reported stream count zero.

Scripted PSP buttons navigated menus and save dialogs. PID-targeted X11 key events also
produced SDL screenshots, but actual analog movement/combat and physical controller
behavior are **not established**. No game reached confirmed mission gameplay or an
in-mission checkpoint round trip within these bounds. Merely reaching the VBlank limit,
changing frame hashes, or copying a save directory is not a gameplay/save-reload pass.

Audio was recorded from a per-run PulseAudio null-sink monitor after explicitly moving
only the owned application's stream; no microphone or default-device changes were used.
Earlier virtual-time runs produced substantial nonzero waveforms for all three games.
The table counts intervals with stereo PCM RMS greater than 10 (16-bit samples), not
subjective sound quality: fidelity, dropouts, synchronization and sustained real-time
sound remain unverified. The real-time overflow policy can drop audio under load.

Exact invocations, binary hashes, save-file hashes, logs, frame captures, WAVs and audio
measurements are retained in `build/linux-gameplay-validation/`; final cases are
`SFCO-final`, `SFDM-final`, and `SFLS-final`. Their binary SHA-256 is
`ac84ccd05d04d182f0c08af54fb6f99d9a964e50527831dc7fa46e88fe322396`.
Earlier cases are retained separately, not overwritten by successful regression tests.

### Interrupted-thread identity repair

A resumed single-game diagnosis reproduced Logan's Shadow's loading stall without
concurrent games. Its VBlank handler wakes `VSyncThread` and suspends `user_main`; the
VSync loop draws, resumes main, and sleeps. The scheduler cleared its current-thread
pointer when yielding to dispatch interrupts, so `CurrentId()` returned zero in the
handler. This bypassed the existing self-wakeup rejection when VSync itself had been
interrupted. A conditional live trace caught exactly that case with 143 pending wakeups:
VSync could keep consuming wake tokens instead of sleeping, starving the suspended main.

The scheduler now retains the identity of a thread that yielded without blocking, or
still owns the CPU inside an emulated busy syscall, and exposes it during guest interrupt
handlers. Idle, blocked, or merely timer-woken threads are not treated as interrupted;
termination/deletion clear the retained pointer. There are no game-specific branches.
The earlier audio and event-batching repairs remain unchanged.

`tests/runtime/test_interrupt_context.cpp` exercises the real HLE wakeup syscall through
public scheduler/interrupt APIs. Before the repair, both real-time and virtual-time
preemption cases wrongly succeeded and added a wake token. Afterward all seven cases
pass: preemption and blocked/just-woken idle controls in both clock modes, plus virtual
busy-syscall identity. The full runtime build and all six existing CTest entries also
pass (13 total). Repeat the focused check with:

```sh
ctest --test-dir build/LinuxRuntime -R '^interrupt_context_' --output-on-failure
```

The post-repair Logan's Shadow run (`SFLS-identity-fixed`) accepted the earlier story
save and progressed past loading into the ship mission, with a live HUD. PID-targeted
keyboard analog-down input changed the view from sky to the ship; trigger input produced
visible shots. The before/after SDL screenshots are `SFLS_20261013_001941.bmp`,
`SFLS_20261013_001942.bmp`, and `SFLS_20261013_001944.bmp` under `bin/screenshots/` in the
same validation tree. This establishes interactive mission entry, not mission completion
or an in-mission checkpoint round trip.

Dark Mirror (`SFDM-identity-fixed`) also passes loading into the movement training mission.
The character moves away from the crates under analog-down input, and firing changes the
HUD ammunition from 12/24 to 11/24. SDL captures `SFDM_20261013_003717.bmp`,
`SFDM_20261013_003718.bmp`, and `SFDM_20261013_003720.bmp` preserve those transitions.
Scripted Start also opens/closes the pause screen. Its existing profile was reloaded.

Combat Ops (`SFCO-identity-replay`) reloads its profile and reaches the final Mission
Modifier configuration confirmation without an intro freeze. Editor movement and combat
are still unconfirmed: the script needed explicit Force A/B selections and the bounded
run ended before confirming editor entry. The unchanged pre-repair binary had also passed
the intro when run alone, so this is not an isolated causal proof for its earlier black
frames under concurrent load. A 40-second conditional debugger probe during its menus
saw no matching self-wakeup; it is not positive runtime-return evidence.

All three post-repair replays reached VBlank 18,000 in approximately 301 seconds with
zero reported bad guest-memory accesses. Recorded PCM was non-silent (RMS > 10) in
270/300 seconds for Combat Ops, 274/300 for Dark Mirror, and 257/300 for Logan's Shadow.
This demonstrates sustained nonzero output, not subjective fidelity or synchronization.
Per-case manifests retain the exact commands, seeds and binary hashes.

These replays ran one game at a time, not three competing software-rendered games.
At that stage, the retained evidence was about 1.32 GiB across 29 cases, including one
preserved missing-input setup failure (`SFCO-identity-fixed`), under a 1.5 GiB aggregate
limit. Original saves, game assets and reference frames remained untouched.

### Native save/restart/reload validation — bounded results

Eight subsequent sessions ran sequentially with isolated copies of existing memory
sticks, separate writable firmware directories, interpreter execution, and networking
and plugins disabled. Each was bounded by at most 780 seconds or 45,000 VBlanks, with
at most 60 periodic captures. Audio remained routed to owned null sinks without recording
redundant WAVs. No emulator code changed during this checkpoint-validation wave; its
binary SHA-256 is `f9843de75b5b351334b74051d91d78beca42eac564e626cc9a9ccfd57f8865a0`.

| Game | Newly written state | Fresh-process result | Scope |
|---|---|---|---|
| Combat Ops | Named incomplete Fortress mission `123`, a new 752-byte `NPUG801140000/LVL00000.DEF` | Native FILES/READDATA operations discover/read it; `[FT]123` appears in the mission list, its settings are accepted, and the Fortress editor reopens | **Pass: mission draft persistence**, not a playable multiplayer match or campaign checkpoint |
| Dark Mirror | Quitting after story play writes a 52-byte payload, producing a 68-byte encrypted `UCUS986410000/DATA.BIN` | Two clean processes read the unchanged save; both Normal and Easy selections replay the opening | **Partial:** disk write/read established, but checkpoint resumption cannot be distinguished from new-game entry |
| Logan's Shadow | Starting Normal story mode from a profile-only 40-byte file autosaves an 83-byte payload, producing a 99-byte encrypted `UCUS986060000/DATA.BIN` | The new process offers **Continue / Restart** instead of the previous difficulty selection; Continue returns to Episode 1, Part 1, “Pirates of Somalia,” with a live ship-combat HUD and working pause menu | **Pass: initial story checkpoint resume**, not a later in-mission checkpoint or mission completion |

The Combat Ops save UI explicitly reports **“Incomplete mission saved.”** The draft and
profile bytes remain unchanged through reload. Logan's Shadow's newly generated save is
also byte-identical after reload; this run did not merely reuse the earlier 99-byte save.
Dark Mirror's first replay accidentally entered training and rewrote its copied save;
that replay is not counted as a checkpoint test. Separate Normal/Easy controls used the
untouched story-save output, avoided training, and left its bytes unchanged. No later
Dark Mirror checkpoint was demonstrably reached, so a progressed checkpoint is still
needed for a discriminating resume test. No save I/O or decryption failure was observed;
there is no demonstrated runtime defect here warranting a speculative patch.

Evidence is retained in `build/linux-gameplay-validation/`:

- `SFCO-checkpoint-save` / `SFCO-checkpoint-load`: save confirmation
  `SFCO_20261013_010612.bmp`, saved mission list `...010911.bmp`, reopened editor
  `...011142.bmp` under `bin/screenshots/`.
- `SFDM-checkpoint-save`, `SFDM-checkpoint-load`, `SFDM-checkpoint-cleanload`, and
  `SFDM-checkpoint-easyload`: story play/quit plus the inconclusive and clean controls.
  `SFDM_20261013_013625.bmp` shows Easy selected; `...013657.bmp` shows its opening.
- `SFLS-checkpoint-save` / `SFLS-checkpoint-load`: `SFLS_20261013_014645.bmp` shows
  Continue/Restart, `...014834.bmp` identifies the resumed episode, and `...015025.bmp`
  / `...015027.bmp` show the live HUD and pause objectives.
- `checkpoint-roundtrips.json` summarizes exact save hashes, durations, outcomes and
  preservation checks. Each case retains commands, binary/seed hashes, runtime logs,
  live-input logs, frames and final save hashes. The draft, Dark Mirror story save and
  Logan's Shadow story save have SHA-256 prefixes `6b6ab9a5851c`, `577ef20e8d6f`, and
  `7081708d5d97`, respectively; the summary contains the full hashes.

All eight runtime processes exited normally with zero reported bad guest-memory accesses.
The first Combat Ops wrapper incorrectly rejected that normal exit because it expected
only timeout code 124; its injector also attempted key releases after window destruction.
The task-local harness now accepts normal exit 0 or bounded exit 124 and omits key-release
events after Ctrl+Q. Subsequent completed runs exercise both corrections; this was a
harness failure, not a game save failure.

All 114 pre-existing save-file hashes were rechecked unchanged. Owned game processes
exited and validation audio sinks were removed. That wave ended with about
1.40 GiB across 37 cases; the continuation explicitly allowed at most 12 additional cases
(41 total) and 2 GiB aggregate, preserving earlier failures and evidence. Python harness
syntax checks passed. The previously passing 13 CTests cover the unchanged native code;
they were not rerun for this documentation/harness-only wave. Generic test inference has
no applicable full profile; these retail runs are manual integration evidence, not new
automated CTests or proof of full Linux compatibility.

### Dark Mirror progressed-checkpoint follow-up — not verified

Two further **separate, sequential processes** started from independent copies of
`SFDM-checkpoint-save/ms0`, not from each other's output. The original 68-byte
`UCUS986410000/DATA.BIN` has SHA-256
`577ef20e8d6fe2cc1313f94e9f364f7aca93282185dd5dc4895fa2945214f3f2`.
Both used the same `f9843de75b5b...` runtime binary as above, Easy story mode, isolated
writable memory sticks/firmware, and disabled plugins/networking. No cheats, guest-memory
state injection, reference changes, or emulator patches were used.

| Case under `build/linux-gameplay-validation/` | Bound / observed duration | Observed outcome |
|---|---|---|
| `SFDM-progress-save` | 780 seconds or 45,000 VBlanks / 751.605 seconds | Reached “Insertion Point Alpha”; inspected controls, left cover and fired the pistol; player eliminated before a distinct checkpoint |
| `SFDM-progress-save2` | 1,800 seconds or 105,000 VBlanks / 1,752.674 seconds | Selected and fired the MB-150, exercised scope/zoom and native control settings; player eliminated, then retried in the **same process**; final pause still listed the commander/valve objectives |

Both stopped at their VBlank limits with runtime exit **124**, zero reported bad
guest-memory accesses, and successful wrapper cleanup. Late attempts to send Ctrl+Q
found the windows already gone; these were input-injector assertions after bounded
termination, not game crashes. The second session's per-run allowance was explicitly
raised for operator-paced gameplay, retaining the 60-periodic-capture, 41-case and 2-GiB
aggregate limits; it produced 58 periodic captures.

Each log contains one native `AUTOLOAD`/`AUTOSAVE` pair writing a 52-byte payload **before
the first gameplay capture**, producing a 68-byte encrypted file. Final SHA-256 values:

- `SFDM-progress-save`: `3ca4b6875d111d6afbc0c082fcb993c15ee73d90c807f767c2758829b1e1eb49`
- `SFDM-progress-save2`: `9ae3cf3b4d6911eee74caa4cc79080a09a136086c62ab86b1efe4302ce02734f`

Changed encrypted bytes and an in-memory Retry Mission are **not** a progressed-checkpoint
round trip. No distinct later checkpoint or corresponding disk save was established.
Consequently, **fresh-process resume of progressed state was not run**: repeating the
already-tested initial-point reload would not answer the question. These attempts failed
to acquire the required gameplay fixture; they did not demonstrate a save/decryption
failure or justify a runtime regression test. No emulator code/tests were changed.

Exact commands, PIDs, input logs, native save calls, limits, binary/seed/output hashes and
outcomes are retained in the two case directories and `dm-progress-attempts.json`.
Relevant SDL captures in `bin/screenshots/` are `SFDM_20261013_065822.bmp` (first elimination),
`...070435.bmp` (weapon selector), `...072150.bmp` (confirmed Standard controls),
`...072310.bmp` (second elimination), `...072530.bmp` (post-retry scope), and
`...072922.bmp` (final objectives). The open diagnostic ledger is
`dark-mirror-checkpoint-resume-not`; attempts 3–4 preserve these non-passing results.

All **164 pre-existing save files** hashed for this follow-up remain unchanged. Both
owned runtimes and their helpers exited, with no validation audio sinks left behind.
Retained evidence is now about 1.45 GiB across 39 cases, still below the existing aggregate
caps. Harness Python syntax and scoped documentation whitespace checks passed; native
code is unchanged, so its prior CTests were not rerun. **Next prerequisite:** reach a
later checkpoint interactively or supply a known-progressed save together with its
`PARAM.SFO`; then perform a native save and fresh-process resume in new isolated copies.
The roadmap leaves this checkpoint goal unfinished rather than recording a pass.

### Dark Mirror progression diagnosis — commander objective completes

`SFDM-progression-diagnostic` used another isolated copy of the same pristine seed and
unchanged `f9843de75b5b...` runtime. Reviewing the earlier scope captures showed shots
below/left of the commander's marker, intersecting catwalk geometry rather than a
visible target. A bounded, read-only controller trace then recorded the guest receiving
R (`0x200`), L+R (`0x300`), analog down/right, Cross (`0x4000`), and neutral releases
from the expected host inputs. This establishes those sampled bindings/chords; it is
not a timing/performance test or proof that every short operator tap was observed.

The first debugger attempt expired during debuginfod loading; the next used an
unresolved C++ breakpoint and collected no samples. Disabling external/shared-library
debug-symbol loading and resolving a source-line breakpoint produced 164 controller
reads in the bounded probe (`controller-probe-resolved.log`). All attempts are retained.

Using **Standard (Free Aim)**, the game's own aim toggle/reduced pitch and yaw settings,
and increased MB-150 scope zoom, the operator brought the visible commander into the
reticle. His health decreased, a subsequent shot killed him, and the game displayed
**“Objective Complete: Squad Commander eliminated.”** The following pause menu listed
only **“Open distillation column valve.”** No emulator patch, guest-memory write, cheat,
or altered reference was used. This demonstrates functioning targeting/damage and
commander-objective progression in this session; it supports an aiming/line-of-sight
explanation for the inspected failed shots, not a confirmed emulator softlock. It does
not establish that the valve or later mission scripts are correct.

Evidence in `build/linux-gameplay-validation/bin/screenshots/`:

- `SFDM_20261013_081428.bmp` / `...081429.bmp`: targeted shot, ammo decrement and damage.
- `...081503.bmp` / `...081505.bmp`: commander eliminated and objective-complete banner.
- `...081541.bmp`: pause menu with only the valve objective remaining.
- `...081633.bmp` / `...081752.bmp`: subsequent player elimination, then same-process
  Retry Mission restoring both objectives. Later replay did not reacquire completion.

**Persistence remains unverified.** The valve was not completed, and no distinct native
checkpoint save was obtained. The log still contains just one initial 52-byte AUTOSAVE,
before the gameplay captures; the final encrypted 68-byte `DATA.BIN` has SHA-256
`45d683782cd3b75e79cad1a4ac1e9f2b71745c4e5fd8ef20578443873d5d53d9`.
The objective banner and same-process retry are not a disk-save/restart/resume round trip.
A fresh-process progressed-state reload was therefore **not run**.

The run stopped at 105,000 VBlanks after 1,752.979 seconds (runtime exit 124, successful
wrapper cleanup, zero reported bad guest-memory accesses). A late Ctrl+Q found the
already-closed window, not a crash. All **176 protected pre-existing save files** remain
byte-identical; the native sources/tests and runtime binary are unchanged. Owned game
and debugger processes exited and validation audio sinks were removed. Retained evidence
is about **1.49 GiB across 40 cases**, within the 41-case/2-GiB caps. The case directory and
`dm-progression-diagnosis.json` preserve commands, hashes, controller traces and outcomes.
No native regression test is justified by this result, and the unchanged native CTests
were not rerun. The next persistence prerequisite remains completing the valve/obtaining
a distinguishable native checkpoint, not patching an unobserved emulator defect.

### Dark Mirror valve continuation — gameplay fixture still blocked

At the next continuation request, `SFDM-valve-save` started a new Easy story process
from an independent copy of `SFDM-checkpoint-save/ms0`. The preceding commander kill
had not been persisted, so this was **not** a resume of that completed objective.
The unchanged runtime used the same isolation settings, no debugger/cheats or guest-state
injection, and an 1,800-second/105,000-VBlank bound. Retention was explicitly adjusted
from 41 to **42 total cases** for one acquisition and one conditional reload, retaining
the 2-GiB aggregate cap. No old evidence was removed or overwritten.

The run confirmed Standard (Free Aim), aim toggle ON and reduced pitch/yaw settings,
then exercised MB-150 targeting, movement out of cover, perimeter navigation, inventory,
goggles and close-range combat. Input batches now have UTC timestamps in `live-input.log`;
post-action pause captures include a transition wait rather than assuming an immediate
screenshot proves the game is paused. Nevertheless, the commander kill was **not
reacquired**, the attempted paths ended at fencing/wall corners, and no valve interaction
or completion was observed. The final pause still listed **both** initial objectives.
Assumed camera directions outside scoped aiming were not reliable enough to establish
a route; context-sensitive aim/navigation sequencing remains unresolved. These observations
are not proof of a controller, collision, valve-script or save-system emulator defect.
No native source/test change was made.

Key captures in `bin/screenshots/`: `SFDM_20261013_084042.bmp` (control layout),
`...084816.bmp` (fence), `...085606.bmp` (wall corner), `...090016.bmp` (gadget selector),
and `...090501.bmp` / `...090502.bmp` (final gameplay/pause with both objectives pending).
The case stopped at its VBlank bound after **1,752.932 seconds**, runtime exit **124**,
zero reported bad guest-memory accesses, and successful wrapper cleanup. Its only native
save was the initial **52-byte AUTOSAVE**; the final 68-byte encrypted `DATA.BIN` hash is
`f12297c862a9db6e020f379fbd3dd467feca365787a325d9cdfdead1bb3e15b0`.
**No progressed checkpoint was acquired; fresh-process progressed resume was not run.**
The unused reload slot is retained rather than repeating the inconclusive initial-point load.

All **182 protected pre-existing save files** remain byte-identical. The owned runtime,
KIRK helper and audio sink were cleaned up. Evidence occupies about **1.53 GiB across
41 cases**; exact output and preservation evidence is in `dm-valve-continuation.json`.
Harness syntax and scoped/Git whitespace checks passed. Generic full-profile test running
is not applicable; unchanged native CTests were not rerun. **Blocker:** a reliable native
control/route sequence through the valve and a distinguishable disk checkpoint, or a
known-progressed save with its `PARAM.SFO`, is still required for the resume comparison.

### Dark Mirror guarded aiming strategy — acquisition remains incomplete

The next attempt inspected the failed input/capture pairs and added an operator-driven
strategy in `.pi/gameplay-validation/dm_strategy.py`. It validates the owned runtime's
isolated memory stick, requires a **fresh native PAUSED capture** before an `aim` or
`move` batch, limits requested actions to eight seconds, and captures the result before
returning to a confirmed pause. Aiming explicitly holds/releases L through `xkeys.py`
(`down:q` / `up:q`), rather than assuming a short toggle tap left the correct state.
The helper also bounds individual holds, manual captures and aggregate evidence growth;
held keys are released on batch exceptions. `ui` is an explicit operator-observed menu
mode, **not** a gameplay-pause guarantee. The helper recognizes the pause title only;
it does not recognize objectives, choose a valve route or certify gameplay correctness.

`test_dm_strategy.py` exercises retained pause/non-pause frames, action limits and
held-key ordering, plus actual press/release delivery to an invisible owned X11 window,
including exception cleanup. A live paused frame initially failed the strict title
comparison: 92 threshold differences lay on antialiased glyph edges. The retained
`SFDM_20261013_094424.bmp` reproduced the failure in the test. The fix excludes only a
one-pixel edge while requiring 99% agreement separately on solid glyph interiors and
clear background; the non-pause assertions were retained. Five focused tests pass after
the fix, including briefing and Mission Failed negatives. Native aiming also needed a
1.2-second scope-settle interval before the baseline capture. These are harness changes,
not emulator repairs.

`SFDM-strategy-save` used an independent copy of the pristine initial save, unchanged
runtime, networking/plugins off and no debugger, cheats or guest-memory injection.
Easy was visibly selected (`...093312.bmp`); aim-toggle OFF, auto-recenter OFF, target
lock Disabled, zoom remember ON and autosave ON were captured (`...093817.bmp`).
Pitch/yaw were reduced through the native menu and Standard (Free Aim) was confirmed
(`...094017.bmp`). The guarded steps moved out of cover and raised the MB-150 view to
the commander's platform. Nearby guard fire/hit reactions complicated aiming; their
causal contribution was not isolated. The first attempt ended in player elimination
(`...095529.bmp`), correctly rejected as a pause. One native, same-process Retry Mission
followed. Scoped firing reduced ammunition from 10 to 8 and then 7; a short up/right
correction changed the scoped view (`...095858.bmp`, `...095936.bmp`). Later comparison
with the successful `...081428.bmp`–`...081503.bmp` evidence showed that the final view
appeared to track the separate vented object/range marker, not the soldier. The original
interpretation that the reticle was below/left of the commander's body is withdrawn.
**No commander or valve completion was demonstrated.** Final pause `...095938.bmp` still lists both objectives. A proposed
further correction was refused because the owned runtime had already exited; it was
not executed. Native Select/Command is available as `space` (confirmed from the host
bindings), but was not exercised in this run.

The process stopped at **105,000 VBlanks after 1,752.645 seconds**, runtime exit **124**,
wrapper exit **0**, with zero reported bad guest-memory accesses. The only disk save
was the initial **52-byte AUTOSAVE**, yielding a 68-byte encrypted `DATA.BIN` with SHA-256
`b6bd779317b6384cff89c22dbe6a2be72e95830dcf0639a3eb023826cccbf866`.
**No progressed checkpoint was acquired, and fresh-process progressed resume was not
run.** Successful helper tests, wrapper cleanup, ammunition changes and same-process
retry are not substitutes for that persistence proof. No emulator defect was established.

All **188 protected pre-existing save files** remain byte-identical, and the owned
runtime/helper group and validation audio sink are gone. Retention was increased from
42 to **43 cases** for this acquisition and a conditional reload, without changing the
2-GiB cap or deleting old evidence; **42 cases** are now retained and the reload slot
is unused. The case contains 58 periodic frames and 70 manual captures. Exact hashes,
inputs, limits and outcomes are recorded in `dm-guarded-strategy.json`, the case's
`strategy-input.log` / `strategy-steps.jsonl`, and `result.json`.

Python syntax checks, five focused tests and scoped/Git whitespace checks passed. Baseline
native source/tests/binary are unchanged; native CTests were not rerun and repository-wide
verification is not claimed. **Remaining prerequisite:** complete native combat and
establish the actual valve interaction/route, then obtain a distinguishable checkpoint.
The guarded controller is available for that work; a successful acquisition/navigation
recipe and a save/restart/resume roundtrip are still missing.

### Commander visible-body follow-up — stopped at the correction limit

One verification-only case, `SFDM-commander-body-test`, used unchanged code and helpers,
with a 900-second/54,000-VBlank bound, four failed corrections maximum, and one shot
permitted only after visible-body alignment in a separate aiming step. Correction 3
(`...105358.bmp`) distinguished the named soldier from the vented object; correction 4
(`...105706.bmp`) overshot horizontally, leaving him left of the reticle on a pillar.
Acquisition therefore **failed within the bound**. It stopped with **zero shots** and
unchanged 10/30 ammunition; shot feedback was not tested. No retry or reload occurred.

Owned Ctrl+Q ended the runtime cleanly after 870.463 seconds (runtime/wrapper exit 0,
zero reported bad memory accesses). Only the initial 52-byte AUTOSAVE was observed,
not a progressed checkpoint. All 194 then-protected saves and preexisting code remained
unchanged, and owned processes/audio resources were cleaned up. Evidence is in
`build/linux-gameplay-validation/dm-commander-body-test.json`. Retention after this case
was **43/43 cases**, about **1.60/2 GiB**. Acquisition and progressed-save resume remained
incomplete; the later attempts below did not resolve them either.

### Subsequent acquisition and ground-first attempts — no progressed checkpoint

The authorized case limit was raised from 43 to **45**, keeping the **2-GiB** limit and
all existing evidence. Both later attempts used the unchanged validation runtime:

- `SFDM-checkpoint-acquire45` became case 44. The commander was visibly acquired, but
  damage/elimination was not established and both objectives remained pending. The run
  ended after confirmed player elimination, with owned Ctrl+Q at **1677.758 seconds**
  (runtime/wrapper exit 0). No retry or conditional progressed-save reload was run.
- `SFDM-ground-first` became case 45. Two ground-target falls and two scoped commander
  hits were observed, but the commander and valve objectives remained pending. It stopped
  at the **105,000-VBlank** bound after **1752.548 seconds** (runtime exit 124, wrapper
  exit 0), without confirmed player death. No retry or reload was run.

Each attempt produced only the initial **52-byte AUTOSAVE** / **68-byte encrypted
DATA.BIN**, not a progressed checkpoint. Neither established an emulator/save-system
fault. Pre-existing saves and evidence remained intact, and owned runtime/audio resources
were cleaned up. The summaries are `dm-checkpoint-acquire45-summary.json` and
`dm-ground-first-summary.json` under `build/linux-gameplay-validation/`.

Current retention is **45/45 cases**, about **1.71/2 GiB**. All case slots are occupied;
no further gameplay run or retention increase is authorized. Native combat completion,
valve progression and a distinguishable checkpoint/restart/resume roundtrip remain unproven.

## Shared GPU preparation

The D3D11 geometry preparation, packed draw state, texture atlas layout, texture-data
hashing and reference-sampler decoding now live in portable `src/gpu/hw/draw_data.{h,cpp}`.
D3D11 uses that shared implementation. Six `gpu_draw_data` unit tests run on Linux and
cover primitives, batch boundaries, packed state, texture options and decoding all eleven
texture formats against the software sampler, including swizzling, palettes and bounded
reads. The runtime-enabled Linux build and all 16 CTests passed after the extraction.

The extraction itself did not establish acceleration. The Linux backend below now uses
this layer too. Windows build and D3D11 execution were not tested by these Linux runs.

## Linux renderer and launcher workflows

### OpenGL hardware renderer

`--renderer opengl` selects the Linux GPU backend. It requires OpenGL 4.5 and
`GL_ARB_fragment_shader_interlock`; unsupported drivers produce a diagnostic and fall
back to software. Software remains the default. `PSPC_RENDERER_REQUIRE=1` makes an
unavailable requested OpenGL backend fatal, for validation rather than silent skips.

The backend uses ordered integer color/depth images, shared GE preparation and GLSL
generated from the D3D11 shader arithmetic. It supports 1–16× internal resolution,
nearest/linear/game-selected filtering, xBR texture upscaling, framebuffer feedback,
scaled display readback, presentation and overlays. Unsupported raster states or aliased
color/depth storage use the software reference. VRAM synchronization is eager, including
CPU writes and transfers; Linux does not install D3D11-style page-fault handlers. This
prioritizes correctness, not a claim of equivalent D3D11 performance.

Focused tests ran on Intel UHD Graphics (CML GT2), Mesa. They compare color/depth output
against software, exercise filtering/xBR, direct and indexed/swizzled framebuffer feedback,
GE transfers, CPU edits, mirrored VRAM, surface eviction and scales through 16×. A separate
SDL test covers preserving a peer context, worker-thread presentation, resizing and
idempotent release. Disabling the interlock extension in a fresh Mesa process verified
explicit capability rejection and context restoration. A separate bounded CLI run verifies
that unavailable interlock selects software and continues running the native synthetic
program without bad memory accesses (expected timeout status 124). Neither rejection case
is a GPU-rendering pass.

```sh
PSPC_RENDERER_REQUIRE=1 build/LinuxRuntime/bin/pspc_unittests gpu_hardware
PSPC_RENDERER_REQUIRE=1 build/LinuxRuntime/bin/pspc_unittests gpu_opengl
```

These synthetic scenes do not establish retail GPU correctness, driver-wide compatibility,
performance, or screenshot/overlay pixel parity. D3D11 and WARP remain Windows-only.

### GTK workflows and native builds

The GTK launcher provides:

- Search, icons, artwork, persistent folder selection and file reveal.
- Shared settings validation, graphics/audio/network controls, keyboard/controller
  bindings and per-list plugin editing.
- Network registration/sign-in/out, friends, owned servers, bounded logs, routing and
  named account/mission/news/board/GameSpy admin operations, plus custom arguments.
- Shared Windows/Linux cache inspection: platform module suffixes, semantics stamps,
  newer hints, duplicate modules, failed/partial/stale results and moved-disc matching.
- Build-before-play choices, forced rebuilding, progress, cancellation and stopping the
  owned runtime. Builds use the existing compiler pipeline and private temporary work
  directories, removed after the child group has stopped.
- Sony 4.01 extraction/install steps, optional firmware AOT, progress/error output and
  XMB launch. A successful real Sony installation/XMB run remains **unverified**.

Native building requires the source tree and development tools; a missing builder is
reported explicitly. Child commands use argv/stdin, not shell command interpolation.
Their process groups are owned and reaped; shutdown never targets arbitrary matching
process names. Captured output is bounded to 256 KiB, with 2 MiB disk logs and one rotated
copy per role. Admin output remains parseable rather than including lifecycle log markers.

The local server integration tests cover routing preservation, accounts/friends,
password reset, ban/unban, JSON administration and start/stop cleanup. They exposed and
fixed missing Linux HTTP PUT/DELETE support. AT-SPI testing also exposed modal actions
holding the accessibility D-Bus dispatcher open; button actions now leave that dispatcher
before opening dialogs, and form entries have explicit accessible names.

```sh
build/LinuxRuntime/bin/pspc_unittests launcher_
build/LinuxRuntime/bin/pspc_unittests native_cache
# Opt-in: real X11, PyGObject, Atspi 2 and Gdk/GdkX11 3 typelibs.
# This combined GPU/fault-injection check requires Mesa with GL 4.5 + interlock.
python3 tests/launcher/test_ui.py --build-dir build/LinuxRuntime \
  --evidence-dir build/linux-parity-review
```

The UI check uses one disposable installation, a synthetic ELF and a loopback-only
account server, with isolated HOME/XDG directories. It exercises persisted graphics settings,
plugins, accounts, server/admin and routing actions, firmware missing-tool errors and chooser
cancellation, build-before-play/cancel selection, failed-build cleanup without launching,
real native compilation, OpenGL startup with native registration and zero interpreted
instructions/bad accesses, Stop game and launcher close with server cleanup. It does not
use retail assets, existing installation data or global key events. The GTK layout is not a pixel
copy of the Windows UI; firmware installation and every admin mutation are not covered by
this UI check.

The full runtime regression passed **16/16 CTests** with hardware availability required;
the tools-only build passed **3/3 CTests**. Focused launcher checks follow the final
process-ownership and raw-admin-output changes. Local evidence is under
`build/linux-parity-review/`. Canonical checks are CTest and the explicit drivers above:
the generic test-runner's inferred `pytest -q` command is not applicable to the repository's
argument-taking CLI drivers and failed collection. Existing reference-dependent VFPU
skips remain; no assertions or reference baselines were weakened.

## Linux scripts and delivery tooling

The script audit found that `tools/guest_tests.py` rejected successful Linux native builds
because it looked only for `.dll` files. It now checks the host's module suffix (`.so` on
Linux, `.dll` on Windows); `large_image_test.py --aot` uses the same corrected helper.
`guest_tests.py`, `run_autotests.py` and `large_image_test.py` also select the host executable
name rather than always defaulting to `pspc.exe`. Their default build tree remains
`build/RelWithDebInfo`; pass `--pspc` when using the separate runtime tree:

```sh
python3 -I tools/guest_tests.py --pspc "$PWD/build/LinuxRuntime/bin/pspc"
python3 -I tools/run_autotests.py --pspc "$PWD/build/LinuxRuntime/bin/pspc" --jobs 2
python3 -I tools/large_image_test.py --pspc "$PWD/build/LinuxRuntime/bin/pspc" --aot
```

These are asset-dependent validation commands, not a fixture-free CI gate. The first and
third require guest programs built with `tools/build_guest_tests.sh` and pspdev; the second
requires the pspautotests reference tree and any per-test firmware/rebuilt fixtures. The
large-image test creates an 8.75 GB sparse image, so check filesystem support and available
space before running it. Missing guest programs reported as `SKIP` are not validation passes.

The fixture-free `aot_pipeline` CTest now calls the real guest native-build helper with a
synthetic ELF and executes its output through the native module probe. It reproduced the
false failure before the suffix fix. This covers the shared helper without PSP tools or
firmware, not the full guest/reference/large-image suites.

The delivery audit also leaves these explicit follow-ups under roadmap item 3.1.2:

- `build.sh`, guest-build shell scripts, smoke/network helpers and the native builder have
  Linux entry points. Ghidra's headless/JDK discovery still assumes Windows. The firmware
  helper now distinguishes executables from writable firmware/cache/work destinations;
  successful installation/boot with Sony assets remains unverified.
- `tools/make_installer.py` and its bundled-toolchain release test are Windows-only. The
  Linux AOT SDK component below installs PSPC's build dependencies, but is not a full
  application package or a self-contained compiler distribution by itself. Runtime builds now
  also provide the [full application install/archive](linux-package.md), with matching source,
  system-library dependencies, no installed RUNPATH and XDG writes. Explicit per-user
  [desktop registration](linux-package.md#application-menu-registration) handles relocation,
  version switching and removal without touching user data. Distro-specific package hooks,
  broader ABI baselines and automated upgrade/uninstall integration remain pending.
- The [fixture-free Linux package CI workflow](linux-package.md#fixture-free-ci) builds on
  Ubuntu 24.04 and requires synthetic install/archive, isolation and real GTK desktop
  activation checks. M33 decrypt tests are explicitly excluded, reference-dependent unit
  omissions remain visible, and required GPU/retail validation is separate. Hosted Actions
  execution/artifact upload has not yet been observed; local rehearsal is not a hosted pass.
- General README/architecture/networking documentation still contains Windows-specific
  examples and historical validation claims; this document remains the Linux status source.

### Relocatable Linux AOT SDK

The Linux `AOT` CMake install component provides the PSPC-specific dependencies needed to
build native modules without the source or build tree. It works with the tools-only
configuration above; `pspc-iso` is now built independently of the runtime:

```sh
./build.sh RelWithDebInfo --target pspc_aot_probe
ctest --test-dir build/RelWithDebInfo --output-on-failure -R '^(aot_install|aot_pipeline)$'
cmake --install build/RelWithDebInfo --component AOT --prefix "$HOME/PSPC-AOT"
python3 -I "$HOME/PSPC-AOT/tools/pspc_build.py" --compiler gcc /path/to/module.elf
```

Choose a dedicated prefix, not `/usr` or `/usr/local`: the SDK has a flat, relocatable
layout, not an FHS `bin/lib/share` layout. Its contents are:

- `pspc-recomp`, `pspc-iso`, `pspc-decrypt`: native host tools;
- `aot/lib/libpspc_sem.a`, `aot/lib/libpspc_common.a`: matching native archives;
- `aot/src/allegrex`, `aot/src/common/common.h`, `aot/src/recomp/codegen.cpp` and
  `aot/generated/pspc_sem_version.h`: compilation and semantics-hash inputs;
- `tools/pspc_build.py`, documentation, license and third-party notices.

The installed script auto-detects `aot/`; `--pspc-dir /path/to/SDK` selects an explicit
installation. It uses only that installation's PSPC dependencies and checks the semantics
hash before using the cache. Do not mix installed mode with `--build-dir`. The default
cache/work roots are `cache/aot` and `cache/aot-work` inside the SDK; for a read-only SDK,
pass **both** writable paths (and point the runtime at that cache with `--aot-dir`):

```sh
python3 -I /opt/PSPC-AOT/tools/pspc_build.py --compiler gcc \
  --cache "$HOME/.cache/pspc/aot" --work "$HOME/.cache/pspc/aot-work" /path/to/module.elf
```

The SDK does **not** bundle Python, a compiler, system headers, libc, libstdc++/libgcc,
CMake or Ninja. Python 3 and a compatible C++20 GCC/Clang toolchain must be installed;
Clang is the default, `--compiler gcc` selects GCC, and `CXX` overrides either selection.
Direct builds need no CMake/Ninja; `--cmake` additionally requires both. Built tools,
archives and modules remain tied to the host architecture and Linux ABI baseline. This
is not an AppImage, cross-distribution binary guarantee, or replacement for the Windows
private LLVM/MinGW compiler bundle. No firmware, games, saves or caches are installed.
The GTK launcher uses the installed SDK when `aot/` is beside its executable, otherwise the
development build tree. It explicitly supplies the runtime's user cache and temporary work
paths. Standalone SDK CLI defaults remain SDK-local; pass `--cache`/`--work` to share the
runtime's XDG cache. The SDK component still does not install the runtime or launcher.

`aot_install` stages the actual CMake component in a temporary directory outside the
repository, relocates it to a path with spaces/non-ASCII text, builds three synthetic ELFs
and executes their native output. It covers auto-detection, explicit installed mode,
warm-cache reuse, a read-only SDK with external cache/work paths through the CMake route,
and rejection of missing archives, stale semantics and conflicting build-tree arguments.
Temporary files are removed on exit. Both tools-only and runtime builds passed, followed
by tools-only `aot_install`/`aot_pipeline` (2/2) and runtime
`aot_install`/`aot_pipeline`/`aot_runtime`/`launcher_cli` (4/4). Python diagnostics and Ruff
F checks passed. These are fixture-free checks, not retail AOT or successful
firmware-install evidence. Evidence: `build/linux-aot-install-review/`.

### Linux runtime and GTK user-data paths

`common/app_paths` is the shared Linux path policy for the runtime, GTK launcher/settings,
plugin editor and hosted servers. Linux no longer reads or writes executable-local user
settings/data by default, even in development builds. Windows keeps its existing layout.

| Root | Default when the XDG variable is unset/empty | PSPC contents |
| --- | --- | --- |
| `$XDG_CONFIG_HOME/pspc` | `$HOME/.config/pspc` | `pspc.ini`, user `infrastructure.json` and `infra-dns.json` |
| `$XDG_DATA_HOME/pspc` | `$HOME/.local/share/pspc` | `ms0`, `ms1`, `flash0`, `flash1`, `Games`, `plugins`, `screenshots`, hosted-server databases in `servers`; `host0` when booting without a program file |
| `$XDG_CACHE_HOME/pspc` | `$HOME/.cache/pspc` | `aot`, `decrypted`, temporary native-build and firmware work directories |
| `$XDG_STATE_HOME/pspc` | `$HOME/.local/state/pspc` | launcher/runtime job logs in `logs`, hosted-server logs in `servers` |

Only absolute XDG overrides are used; relative overrides are ignored as required by XDG,
not resolved against the caller's working directory. If a fallback is needed but HOME is
missing/relative, PSPC fails with an actionable error instead of writing in the working
or installation directory. Relative `games_dir`/`plugins_dir` settings are resolved against
the user data root in both runtime and GTK. Absolute configured paths and explicit runtime
CLI paths (`--config`, `--memstick`, `--flash0`, `--flash1`, `--aot-dir`, etc.) remain honored.

Executables and the AOT SDK remain installation resources. The nearest executable ancestor
containing `data/nids.csv` identifies the immutable resource root (`data/`, `tools/`,
`server/`); this supports the flat installed layout and normal source builds without using
cwd. GTK uses per-user working directories for children, keeps the installed server package
as Python's import directory, and sends database/log/firmware/build output elsewhere.
User routing overrides are written by GTK and read by the runtime from the same config root.

On Linux the firmware CLI requires `--data-dir`, and AOT additionally requires explicit
`--cache` and `--work`; `--pspc-dir` identifies executables/SDK, not a firmware destination.
GTK supplies these automatically. For a source build, for example:

```sh
python3 -I tools/pspc_install_fw.py --fw /path/to/decrypted/firmware \
  --pspc-dir "$PWD/build/LinuxRuntime/bin" --build-dir "$PWD/build/LinuxRuntime" \
  --data-dir "${XDG_DATA_HOME:-$HOME/.local/share}/pspc" \
  --cache "${XDG_CACHE_HOME:-$HOME/.cache}/pspc/aot" \
  --work "${XDG_CACHE_HOME:-$HOME/.cache}/pspc/firmware-work" --aot-core
```

#### Existing Linux data: migrate explicitly before using the new defaults

No automatic copying, deletion or merge of old executable-relative user data occurs.
**Back up and migrate existing saves/settings before launching against the new defaults.**
Stop all PSPC processes and servers, choose the destination XDG roots above, then transfer
these items from the old executable directory to **empty destinations**:

- `pspc.ini`, `infrastructure.json`, `infra-dns.json` → the config root;
- `ms0`, `ms1`, `flash0`, `flash1`, `plugins`, `Games`, `screenshots`, and server databases
  (including SQLite sidecars if any) → their names under the data root;
- `cache/aot` and `cache/decrypted` → `aot` and `decrypted` under the cache root, or rebuild them;
- `logs` → the state root's `logs`; old `servers/*.log` → the state root's `servers`.

Do not overwrite/merge an already-used destination blindly. Keep the old copy until save
loading and server accounts are confirmed. Adjust relative game/plugin settings when their
files stay elsewhere; absolute paths can keep large game libraries in place. Old folders
are not a secondary live data source, so there is only one active set of user data.

#### Focused installed-layout verification

`launcher_paths` stages the actual complete CMake installation (no manually copied runtime
binaries/resources), relocates and seals it read-only, and uses disposable HOME/XDG roots. It tests
settings shared between CLI/runtime/GTK, absolute overrides and HOME defaults, ignored
relative XDG values, default/relative game folders, memory-stick override precedence,
existing-save preservation and synthetic firmware copy/AOT output outside the installation.
It checks the payload hashes remain unchanged. No Sony firmware is needed.

```sh
ctest --test-dir build/LinuxRuntime --output-on-failure -R '^(launcher_paths|launcher_cli)$'
python3 -I tests/launcher/test_ui.py --build-dir build/LinuxRuntime \
  --evidence-dir build/linux-user-path-review/ui
```

The opt-in GTK check uses the same read-only installed layout, with writable isolated XDG
roots. It checks actual settings/plugin/routing writes, hosted accounts/server/admin/logs,
missing-tool errors, firmware chooser cancellation, installed-SDK native build/play,
live rebuild and owned shutdown. Missing tools are injected between actions; the payload
is read-only during every application action and its final hashes must match.

The [runtime package](linux-package.md) uses these install rules for a complete CPack archive,
including Runtime, AOT and Sources. Installed executables have no RPATH/RUNPATH: SDL2, GTK3,
OpenGL, fontconfig/FreeType, curl, FFmpeg and C/C++ runtime libraries must come from the host.
`runtime_package` tests the archive itself; its explicit `--isolate` gate hides the repository
and build/dependency tree while checking native startup and installed resource/server operations.
These synthetic checks establish a relocatable **host-dependent** installation, not an AppImage,
other-distro ABI compatibility, a bundled compiler/Python, Sony firmware/XMB or Windows evidence.
See the package guide for manual update/removal semantics and remaining delivery scope.

## Ghidra headless tooling

`tools/ghidra/ghidra_analyze.py` now selects Linux's `support/analyzeHeadless`
and `java`/`javac`, retaining Windows's `.bat`/`.exe` selection. The personal
Windows Ghidra-directory default is removed: pass `--ghidra` or set
`GHIDRA_INSTALL_DIR`. JDK selection uses `--java-home`, `PSPC_JAVA_HOME`,
`JAVA_HOME`, Windows installation discovery, then `javac` on `PATH`; Linux
alternatives/symlinks resolve to the JDK home. Invalid configured homes and
missing/nonexecutable launchers produce explicit failures. Linux dry runs use
POSIX quoting. The Windows wrapper delegates JDK discovery to the same helper.
See [firmware tooling](firmware-tooling.md) for usage and prerequisites.

The new fixture-only `ghidra_headless` CTest executes the Python CLI against a
recording launcher using temporary JDKs and original module headers. All 11
tests pass, covering user/kernel/raw grouping, analysis/export options, paths
with spaces/Unicode/shell characters, JDK precedence and alternatives, explicit
errors, dry-run behavior, child exit propagation and simulated Windows selection.
The tools build and all five tools CTests passed, including this new gate.
No games, saves, firmware or existing Ghidra projects are opened by these tests.
Actual Ghidra/Allegrex analysis and native Windows batch execution remain unrun;
these tests establish the helper's launch contract, not full analysis parity.

## Sony 4.01 firmware installation and XMB validation

A bounded Linux run installed genuine Sony 4.01 PSP-2000/02g assets into a fresh,
XDG-isolated profile. The archive linked from `fwdownloads.txt` confirmed the cached
`401.PBP` MD5 (`c593abc023aa9b215710df6e263b3fff`); extraction produced 300 files,
with all 205 modules decrypted and no failures. Installation delivered 301 flash0
files, including M33's `satelite.prx` and `recovery.prx`. All 16 core firmware AOT
modules compiled successfully with one compiler worker.

The interpreter boot reached Sony's real XMB and navigated its category bar. An
initial windowed run also opened/closed the M33 menu but crashed during process
exit. Firmware-free regressions isolated two host lifetime defects: deferred thread
deletion bypassed scheduler pointer cleanup, and kernel pools outlived the partition
allocators their destructors use. Deferred deletion now uses `DeleteThread`, and the
runtime destroys kernel objects before leaving the emulation callback. Plain and
Memcheck regression cases cover both paths.

After repair, a fresh windowed X11/software-renderer run loaded eight firmware/M33
modules natively, navigated to Settings, opened and closed the M33 menu, and exited
cleanly at the requested VBlank 1440 (intentional code 124). It took 39.1 seconds,
logged only four interpreted instructions and zero bad guest memory accesses, and
produced 12 guest captures plus a host-renderer capture. Images were inspected for
actual XMB/menu contents, not merely nonblank output. This validates the installed
Sony user-mode XMB over PSPC's native kernel, not execution of Sony kernel modules.

Evidence is local and ignored under `build/linux-firmware-validation/`: extraction
and installation results, `aot-result.json`, four bounded run cases, red/green
lifetime regressions and the final runtime CTest log. The task preserves the original
firmware, settings and saves; no gameplay acceptance runs were added. The GTK install
dialog, changed-setting persistence, XMB game launch/return, hardware GE rendering,
physical audio/controller behavior and other firmware versions remain untested by
this validation. See [VSH boot](vsh-boot.md) for the workflow and architecture.

## Work still required

- Validate real Ghidra/Allegrex imports and analysis on Linux; fixture headless tests
  cover the helper's orchestration only.
- Validate the GTK firmware-install dialog, XMB setting persistence and game launch/return,
  other firmware versions, additional desktop/driver combinations, and remaining UI/admin paths.
- Validate Combat Ops editor manipulation/playable missions, a progressed Dark Mirror
  checkpoint resume, and later Logan's Shadow checkpoints. Draft persistence and the
  initial Logan's Shadow story checkpoint now pass within the limits above.
- Validate physical controllers and audio quality/synchronization. Basic movement/aiming/
  firing in Dark Mirror and Logan's Shadow is established by the earlier bounded replays.
- Validate the remaining firmware/XMB paths above, retail-game AOT/relocatable PRX loading
  and fallback, and checkpoint-reference runs with the required assets. Resolve exercised game-specific HLE
  gaps rather than masking unresolved-import warnings.
- Validate retail OpenGL rendering, overlays/captures, performance and additional GPU drivers;
  the default software renderer and synthetic hardware checks are not retail parity proof.
- Finish remaining script, packaging/install and CI portability.
- Run a Windows build and regression tests before claiming Windows support is preserved.
