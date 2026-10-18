#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt a single Linux command (and its children) into PSPC's local SDL build."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import sys

LIBRARIES = ("libSDL2-2.0.so.0", "libSDL3.so.0")


def local_environment(build_dir):
    lib_dir = (build_dir / "local-sdl" / "lib").resolve()
    if ":" in str(lib_dir):
        raise ValueError("Local SDL directory cannot contain ':' (LD_LIBRARY_PATH separator)")
    for name in LIBRARIES:
        if not (lib_dir / name).is_file():
            raise ValueError(f"Missing {lib_dir / name}; build the pspc-local-sdl target first")
    env = os.environ.copy()
    # These overrides can bypass SONAME lookup even with our directory first.
    for key in ("LD_PRELOAD", "SDL_DYNAMIC_API", "SDL3_DYNAMIC_API"):
        if env.get(key):
            raise ValueError(f"Unset {key} before selecting local SDL")
    paths = [str(lib_dir)] + [p for p in env.get("LD_LIBRARY_PATH", "").split(":") if p]
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    return lib_dir, env


def probe(lib_dir):
    # Load by SONAME in a fresh process, just like the SDL2-linked runtime. Calling
    # SDL2 triggers its compatibility layer; do not pre-load SDL3 by absolute path.
    sdl2 = ctypes.CDLL(LIBRARIES[0])
    sdl2.SDL_WasInit.argtypes = [ctypes.c_uint32]
    sdl2.SDL_WasInit.restype = ctypes.c_uint32
    sdl2.SDL_WasInit(0)
    paths = set()
    for line in Path("/proc/self/maps").read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and fields[5].startswith("/"):
            paths.add(Path(fields[5]).resolve())
    loaded = {}
    for name in LIBRARIES:
        expected = (lib_dir / name).resolve()
        if expected not in paths:
            raise ValueError(f"Loader did not select {expected}")
        loaded[name] = str(expected)
    print(json.dumps(loaded, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/LinuxRuntime"))
    parser.add_argument("--check", action="store_true", help="report verified loaded SDL2/SDL3 paths and exit")
    parser.add_argument("--probe", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- command [arguments...]")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("local SDL selection is Linux-only")
    try:
        if args.probe:
            probe(args.probe)
            return
        command = args.command
        if command[:1] == ["--"]:
            command = command[1:]
        if args.check and command:
            parser.error("--check does not accept a command")
        if not args.check and not command:
            parser.error("provide --check or -- command [arguments...]")
        lib_dir, env = local_environment(args.build_dir)
        if args.check:
            command = [sys.executable, "-I", str(Path(__file__).resolve()), "--probe", str(lib_dir)]
        os.execvpe(command[0], command, env)
    except (ValueError, OSError) as error:
        parser.exit(1, f"local SDL: {error}\n")


if __name__ == "__main__":
    main()
