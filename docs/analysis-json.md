# Analysis JSON format (`pspc-analysis`, version 1)

`tools/ghidra/PspcExportAnalysis.java` writes one file per module to
`private/fw/401/analysis/<stem>.json`, where `<stem>` is the module's file
name without its extension (`systemctrl.prx` becomes `systemctrl.json`). The
recompiler reads these files as function-boundary hints. The pipeline that
produces them is described in [firmware-tooling.md](firmware-tooling.md).

The files are derived from copyrighted modules, so they stay under `private/`.

## Conventions

- Every address is a string of the form `"0x%08X"`. It is an absolute address
  in the Ghidra program, which loaded the module at `program.image_base`.
- Subtract `image_base` from an address to get its **module offset**. For a
  PRX (`e_type 0xFFA0`) the offset equals the ELF virtual address, because PRX
  segments start at virtual address 0. Hints should be keyed by offset, never
  by absolute address, because the runtime loads modules elsewhere.
- Image bases used by the pipeline:
  - kernel modules (module attribute bit `0x1000`): `0x88000000`
  - user and VSH modules: `0x08804000`
  - raw payloads: the base given with `--raw-base`. For systemctrl's reboot
    payload this is `0x88FC0000`.
- Ranges are half-open: `[start, end)`.
- Readers must ignore keys they do not know. New keys may be added without
  bumping `version`. Changing or removing an existing key bumps `version`.

## Top level

| Key | Type | Meaning |
|---|---|---|
| `format` | string | Always `"pspc-analysis"`. |
| `version` | int | Format version (currently 1). |
| `generator` | string | The script that wrote the file. |
| `ghidra_version` | string | For example `"12.1.3"`. |
| `program` | object | See below. |
| `module` | object | The parsed `SceModuleInfo`. Empty for raw payloads. |
| `segments` | array | The loaded memory blocks. |
| `imports` | array | One entry per imported function stub. |
| `exports` | array | One entry per exported function or variable. |
| `functions` | array | One entry per function Ghidra knows about, sorted by entry address. |
| `code_pointers` | array | References from non-executable memory into executable memory. |

### `program`

| Key | Meaning |
|---|---|
| `name` | The program name in the Ghidra project (the file name). |
| `executable_path` | The path the file was imported from. |
| `sha1` | SHA-1 of the original file bytes, so a hint file can be matched to its module. |
| `language` | Always `Allegrex:LE:32:default`. |
| `compiler_spec` | `default`. |
| `image_base` | Load address used for this analysis. |

### `module`

| Key | Meaning |
|---|---|
| `address` | Address of `SceModuleInfo`. |
| `attribute` | Module attribute, for example `"0x1006"`. Bit `0x1000` means kernel, bit `0x0800` means VSH. |
| `version` | `"major.minor"`. |
| `name` | Module name, for example `"SystemControl"`. |
| `gp` | The `gp` value from module info, after relocation. |

### `segments[]`

`name` (the ELF section name, or `segment_N` when there are no section
headers), `start`, `size`, `read`, `write`, `execute` and `initialized`.
Relocation and other non-loaded sections are omitted.

### `imports[]`

| Key | Meaning |
|---|---|
| `library` | Library name from `.lib.stub`, for example `"IoFileMgrForKernel"`. |
| `nid` | NID, `"0x%08X"`. |
| `name` | Function name from `data/nids.csv`, or `<library>_<NID>` when the name is unknown. |
| `name_known` | Whether `name` came from the NID database. |
| `stub` | Address of the 8-byte import stub. Calls to the import target this address. |

### `exports[]`

| Key | Meaning |
|---|---|
| `library` | Library name, or `syslib` for the unnamed system export table (`module_start`, `module_info`, ...). |
| `nid`, `name`, `name_known` | As for imports. |
| `kind` | `"function"` or `"variable"`. |
| `address` | The address of the exported function or variable. |

### `functions[]`

| Key | Type | Meaning |
|---|---|---|
| `entry` | addr | Entry point. |
| `offset` | addr | `entry - image_base`. |
| `name` | string | The Ghidra name: a NID or export name, or a default `FUN_xxxxxxxx`. |
| `name_source` | string | `imported` (from a NID or export table), `analysis`, `default` or `user_defined`. |
| `size` | int | Number of bytes in the function body. |
| `ranges` | array | `[[start, end), ...]` body ranges. Usually one range. More than one means Ghidra found non-contiguous code, such as a shared tail. |
| `is_import_stub` | bool | The function is an import stub. Its body is the 8-byte stub. |
| `is_thunk` | bool | Ghidra treats it as a thunk. All import stubs are thunks of external functions. |
| `no_return` | bool | Ghidra decided the function never returns. |
| `signature` | string | Ghidra's prototype. It is a hint only, because arguments are inferred. |
| `instructions` | int | Number of instructions in the body, delay slots included. |
| `calls` | array | Call sites, see below. |
| `jump_tables` | array | Computed jumps whose targets Ghidra resolved, see below. |
| `string_refs` | array | `{from, address, value}`: an instruction at `from` references a string at `address`. |

`calls[]` entries are `{from, to, kind, target}`:

- `from` is the call instruction. Its delay slot is at `from + 4`.
- `kind` is one of:
  - `call`: `jal`/`bal`
  - `indirect`: a `jalr` whose target Ghidra resolved. An unresolved `jalr` has no `to` and carries `insn`, the disassembly.
  - `tail`: an unconditional `j`/`b` to the entry of another function
- `target` is the callee's name when it is a known function.

`jump_tables[]` entries are `{at, insn, targets: [addr, ...]}`. `at` is the
`jr` instruction and `targets` are the case addresses, in Ghidra's order,
duplicates removed.

### `code_pointers[]`

`{from, to, target}`. `from` is a non-executable address, such as `.data`,
`.rodata` or an export table, that holds or is relocated to a pointer into
code at `to`. These entries are the function-pointer, callback and vtable
candidates the recompiler should treat as indirect-call targets. They include
the export and stub tables, so the list overlaps `imports` and `exports`.

## How the recompiler uses it

1. Match the file to the module by `program.sha1`.
2. Seed function discovery with every `functions[].offset`. The other seeds,
   such as relocation targets and `jal` targets, are found by the
   recompiler's own analysis.
3. Turn `jump_tables` into switch dispatch at `at`.
4. Add `code_pointers[].to` to the set of indirect-call targets.
5. Use import `stub` offsets to wire stubs to the NID dispatcher, and export
   addresses to register the module's exports.

Ghidra can be wrong. A per-module override file always wins over these hints.
