# pspc-decrypt

Turns encrypted or packed PSP executables and Sony firmware updates into plain ELF/PRX files.

**License: GPL-3.0-or-later** (see [COPYING](COPYING)). The tool links libkirk, whose
`kirk_engine.c` is GPL-3.0-or-later, so this directory is a separate program on purpose:

- Nothing under PSPC's `src/` may link these targets or include these sources.
- The PSPC runtime and the AOT pipeline only run `pspc-decrypt.exe` as a subprocess.
- The tests are a separate executable (`pspc_decrypt_tests`), not part of `pspc_unittests`.

Every source file here carries `// SPDX-License-Identifier: GPL-3.0-or-later`, except the
unmodified libkirk files, which keep their own notices (see [libkirk/NOTICE](libkirk/NOTICE)).

What it outputs (decrypted Sony modules, game executables) is copyrighted. Per PLAN.md §7 it
belongs in the gitignored `private/` folder, never in the repository.

## Building and testing

It builds with the rest of PSPC (`tools/CMakeLists.txt` adds it):

```
cmd /c .\build.cmd                                   # everything
cmd /c .\build.cmd RelWithDebInfo --target pspc-decrypt pspc_decrypt_tests
cd build\RelWithDebInfo && ctest -R pspc_decrypt --output-on-failure
```

The executable is `build\RelWithDebInfo\bin\pspc-decrypt.exe` (static CRT, no dependencies). It
also configures on its own: `cmake -S tools/pspc-decrypt -B <dir> -G Ninja`.

## Command line

```
pspc-decrypt info  <file>                 [--json]
pspc-decrypt prx   <in> <out>             [--json] [--seed HEX32] [--licensee-key HEX32]
pspc-decrypt pbp   <in.pbp> <outdir>      [--json] [--seed HEX32]
pspc-decrypt psar  <updater> <outdir>     [--json] [--model any|01g..12g] [--keep-encrypted] [-v]
pspc-decrypt carve <file> <outdir>        [--json] [--keep-encrypted]
pspc-decrypt batch <listfile>             [--json] [--seed HEX32]
pspc-decrypt version | help
```

| Exit code | Meaning |
|---|---|
| 0 | success |
| 1 | usage error (bad command, option or argument count) |
| 2 | failure (unreadable input, corrupt data, unsupported format, write error) |
| 3 | needs a key we don't have (unknown tag, or an NPDRM seed/licensee key that wasn't given) |

- **info** describes a file without writing anything: ELF header and module info; every `~PSP`
  header field (module name, attribute, comp_attribute, decrypt mode, tag, oe_tag, elf/psp/comp
  size, segments) plus what decoding it gives; PBP sections and PARAM.SFO; a PSAR's version byte
  and the firmware version from its first record; EDAT headers; `~PSP` modules embedded in an
  ELF. `--json` prints the same as JSON.
- **prx** writes the final plain ELF: it peels `~SCE` headers, `\0PSPEDAT` wrappers and PBPs
  (using DATA.PSP), decrypts `~PSP`, unpacks custom-firmware gzip packing, and decompresses
  gzip/KL4E/KL3E/2RLZ as the header's compression attribute (and the payload's magic) say. A
  plain ELF is copied through. Output is written to `<out>.part` and renamed, so a reader never
  sees half a file; nothing is written on failure.
- **pbp** writes every non-empty section under its standard name (PARAM.SFO, ICON0.PNG, ...,
  DATA.PSP, DATA.PSAR) and, when DATA.PSP is a `~PSP`/`~SCE`/EDAT module, DATA.ELF decoded.
- **psar** unpacks a system software update - an updater `EBOOT.PBP`, its `DATA.PSAR`, or a
  disc's `PSP_GAME/SYSDIR/UPDATE/DATA.BIN` - see below.
- **carve** finds `~PSP` modules inside any file (the 4.01 M33 installer carries eleven) and
  decodes each to `<outdir>/<NN>_<modname>.prx` (`.psp` raw copies with `--keep-encrypted`).
- **batch** reads lines of `<in>\t<out>` (UTF-8, CRLF or LF, `#` comments and blank lines
  skipped) and does each like `prx`, carrying on past failures. Relative paths are relative to
  the current directory.

Options: `--seed` supplies the 16-byte NPDRM seed (version key) a type 5 module may need;
`--licensee-key` the game's licensee key for a `.sprx` whose EDAT header asks for it.

## What it decrypts

The PRX decrypter is PPSSPP's `Core/ELF/PrxDecrypter.cpp`, with every key table carried over,
plus the extra tags in John-K/pspdecrypt:

- Header types 0 and 1 (144-byte keys: early firmware, updaters, EBOOT 1.x/2.x, ME images, demo
  and game-sharing keys, the PSAR block tag 0x0E000000 and the dev PSAR tag 0x06000000), and
  types 2, 5, 6 and 9 (16-byte keys: firmware modules from 2.60 to 6.6x including 4.01's
  0x4C9414F0/0x4C9415F0 family, 1seg, UMD game EBOOT.BIN tags 0xD916xxF0, PSN tags 0x2E5ExxF0 /
  0x407810F0 with their XOR keys, pauth, the sceResmgr index_XXg.dat tags, the Vita PSP emulator
  tags). Unknown tags exit with code 3.
- PSN EBOOT `DATA.PSP` decrypts with the tag keys alone. `.sprx` modules inside `\0PSPEDAT`
  get their NPDRM seed derived from the content ID (sceNpDrmGetFixedKey + npdrm's module key);
  those that also need the licensee key take `--licensee-key`.
- Custom firmware packing: a `~PSP` header in front of a plain gzip stream (M33's psp-packer and
  similar, recognised by the gzip magic after the header, whatever tag/oe_tag it claims), or in
  front of a plain ELF.
- Compression: gzip (own RFC 1951/1952 inflater, CRC checked), KL4E and KL3E (PPSSPP's decoder),
  2RLZ (PPSSPP's tlzrc), and zlib for PSAR contents.

Not handled: NPUMDIMG game images (PSN ISOs), PGD/EDAT *data* files, and IPL stages after the
first.

## Firmware updates (`psar`)

Ported from PPSSPP's `Core/Util/PSARUnpack.cpp` (format notes in its `docs/PsarFileFormat.md`),
with pspdecrypt's version 5 demangling, sixth table key, version 4 tables, IPL handling and
reboot.bin extraction. It covers PSAR versions 1 (oldschool), 2, 3, 4 and 5, pre-decrypted
archives, all three naming eras (real paths in 1.x/2.x; `com:`/`NNg:` groups with `|` lists in
3.x/4.x - the 4.01 updater is this one; flat numbers with `,` lists in 5.x/6.x) and DES file lists
with the key chosen from the firmware version.

Output layout under `<outdir>`:

| Path | What |
|---|---|
| `flash0/...`, `flash1/...` | the files, modules decrypted and decompressed |
| `ipl/...` | IPL images as stored |
| `derived/reboot.bin`, `derived/reboot_NNg.bin` | reboot.bin pulled out of loadexec and decoded |
| `derived/ipl/<name>.stage1.bin` | an IPL's first stage, KIRK-decrypted and linearized |
| `derived/<path>.2` | a second compressed stream found after a module's first |
| `encrypted/...` | the original encrypted modules (`--keep-encrypted`) |
| `conflicts/<NNg>/...` | with `--model any`: a second, different file for a path already written |
| `unnamed/<entry>` | entries no file list names |
| `psar/lists/*.txt` | the decrypted file lists |
| `manifest.json` | everything above, entry by entry |

A module that can't be decrypted is still written where it belongs (encrypted), so the tree is
complete; the manifest marks it. `--model 02g` (the PSP-2000) resolves names against that model's
lists only and skips other models' files.

`manifest.json` has the firmware version, PSAR version, model, a `summary` (entries, files,
directories, file lists, modules, modules decrypted, needs-key, failed, unnamed, other model,
conflicts, derived), and one record per entry:

```json
{
  "entry": "com:00001", "device_path": "flash0:/kd/sysmem.prx", "group": "com",
  "kind": "module", "path": "flash0/kd/sysmem.prx", "size": 6000, "sha1": "...",
  "decrypted": true, "encrypted": true, "tag": "0x4c9414f0", "prx_type": 2,
  "compression": "gzip", "archive_compression": "zlib", "raw_size": 6368, "raw_sha1": "..."
}
```

`kind` is one of `module`, `file`, `ipl`, `directory`, `file_list`, `derived`. A failed module has
`"decrypted": false`, `"status": "needs-key"` or `"failed"`, and `"error"`. `psar` exits 0 when
everything decoded, 3 when the only problems are missing keys, 2 otherwise.

## Use from the AOT pipeline

Run the executable; never link it. For a set of modules, write a list file and run one batch:

```
pspc-decrypt.exe batch --json <list.tsv>
```

with one `<input>\t<output>` per line (absolute paths, UTF-8). stdout is one JSON document:

```json
{ "ok": 2, "failed": 0, "needs_key": 0,
  "results": [ { "input": "...", "output": "...", "status": "ok", "steps": [...],
                 "module_name": "...", "tag": "0x...", "encrypted": true, "prx_type": 2,
                 "compression": "gzip", "size": 56350, "sha1": "...", "elf_type": "0x0000ffa0",
                 "line": 1 } ] }
```

`status` is `ok`, `needs-key` or `failed` (with `error`). Use each output only if its status is
`ok`; the exit code summarises the run (0 all ok, 3 only missing keys, 2 anything else). For a
single file, `pspc-decrypt.exe prx --json <in> <out>` prints one such result object. For a
firmware, run `psar` once into `private/fw/<version>/` and read `manifest.json`.

## Tests

`pspc_decrypt_tests` (registered with CTest as `pspc_decrypt_tests`) needs no Sony data:

- AES (FIPS-197), AES-CMAC (RFC 4493), SHA-1, DES known answers; KIRK CMD1/4/7/10/11/12/13/16/17,
  including a real KIRK CMD1 ECDSA block and a real NPUMDIMG signature from pspautotests.
- `~PSP` decryption for types 0 and 2 across many tags, using modules built by running each
  decryption step backwards (KIRK CMD0 and kirk4), with gzip, KL4E and no compression; corrupt,
  truncated and unknown-tag input.
- inflate against Python zlib/gzip streams; KL4E/KL3E and 2RLZ round trips through a literal
  encoder; KL4E differentially against artart78's decoder reverse engineered from the 6.60
  firmware (`tests/ref_kl4e_artart78.c`, test-only).
- The real 4.01 M33 installer: all eleven embedded modules decode to the expected sizes and
  SHA-1s (SystemControl: 56350 bytes, e_type 0xFFA0), and the SDK sample's DATA.PSP decodes to
  exactly the `bootload.prx` beside it.
- Synthetic updaters (4.01-style grouped names with DES lists and per-model files, version 5,
  pre-decrypted) through `psar`, and the whole CLI as a subprocess with its exit codes.

No real Sony updater has been run through it yet; the PSAR path is verified against archives
built with the same layers in reverse. Running the 4.01 updater (`401.PBP`) is the next check.

## Sources and credits

| Here | From | License |
|---|---|---|
| `libkirk/` | PPSSPP `ext/libkirk` (Draan's Kirk Engine; tpu's amctrl) | GPL-3.0-or-later, see NOTICE |
| `src/prx_decrypter.cpp` | PPSSPP `Core/ELF/PrxDecrypter.cpp`; extra tags from John-K/pspdecrypt | GPL-2.0-or-later / GPL-3.0 |
| `src/kl4e.cpp` | PPSSPP `Core/Util/KL4E.cpp` | GPL-2.0-or-later |
| `src/lzrc.cpp` | PPSSPP `Core/FileSystems/tlzrc.cpp` (benhur, tpu) | GPL-2.0-or-later |
| `src/psar.cpp`, `src/des.cpp` | PPSSPP `Core/Util/PSARUnpack.cpp`; John-K/pspdecrypt `PsarDecrypter.cpp` (newpsardumper by PspPet and many others) | GPL-2.0-or-later / GPL-3.0 |
| `src/ipl.cpp` | John-K/pspdecrypt `pspdecrypt_lib.cpp` (descrambling by Davee and Proxima) | GPL-3.0 |
| `src/formats.cpp` (EDAT seed) | PPSSPP `Core/HLE/scePspNpDrm_user.cpp` (from JPCSP) | GPL-2.0-or-later |
| `src/inflate.cpp`, the rest | written for pspc-decrypt | GPL-3.0-or-later |
| `tests/ref_kl4e_artart78.c` | John-K/pspdecrypt `kl4e.c` (artart78) | GPL-3.0 |
