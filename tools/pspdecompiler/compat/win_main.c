/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 PSPC contributors */

/*
 * Windows entry point shim for the out-of-tree pspdecompiler build.
 *
 * pspdecompiler (AFL-2.0, reference/pspdecompiler) is built as a separate
 * tool and never linked into PSPC. This file is our own glue, not part of
 * pspdecompiler.
 *
 * Upstream derives the output file names for -c / -g (<base>.c, <base>.h,
 * <base>_*.dot, written to the current directory) with strrchr(path, '/'),
 * so a Windows path such as "..\m33\systemctrl.prx" would produce a bogus
 * base name containing directories. Upstream main.c is compiled with
 * -Dmain=pspdecompiler_main; this wrapper rewrites '\' to '/' in the
 * arguments (the CRT accepts both separators) and then calls it.
 */

int pspdecompiler_main (int argc, char **argv);

int main (int argc, char **argv)
{
  int i;
  char *s;

  for (i = 1; i < argc; i++) {
    for (s = argv[i]; *s; s++) {
      if (*s == '\\') *s = '/';
    }
  }
  return pspdecompiler_main (argc, argv);
}
