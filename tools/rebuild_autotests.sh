#!/bin/bash
# Rebuilds the pspautotests whose shipped .prx is older than their .expected (run_autotests.py's
# 'rebuilt' entries) with the pspdev toolchain, into build/autotest-rebuilt. Run it under Linux or
# WSL with pspdev ($PSPDEV, default ~/pspdev) and make installed. The tests are built in a copy;
# the reference tree isn't changed.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/reference/pspautotests"
OUT="$ROOT/build/autotest-rebuilt"
export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PATH:$PSPDEV/bin"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT

cp -r "$SRC/common" "$WORK/"
make -C "$WORK/common" >/dev/null

# build <test without .prx> <libraries>: the folder Makefiles also link GL, GLUT and pspmath,
# which pspdev doesn't ship, so each test gets the libraries it needs.
build() {
	local rel="$1" libs="$2"
	local dir name
	dir="$(dirname "$rel")"
	name="$(basename "$rel")"
	mkdir -p "$WORK/tests/$(dirname "$dir")"
	[ -d "$WORK/tests/$dir" ] || cp -r "$SRC/tests/$dir" "$WORK/tests/$(dirname "$dir")/"
	rm -f "$WORK/tests/$dir/$name.prx"
	make -C "$WORK/tests/$dir" "$name.prx" LIBS="$libs" >/dev/null
	mkdir -p "$OUT/$dir"
	cp "$WORK/tests/$dir/$name.prx" "$OUT/$dir/"
	echo "built $rel"
}

# The shipped vregs.prx predates its source: one value in its table and three sections differ.
build cpu/vfpu/vregs "-lpspvfpu -lpspgu -lpsprtc -lpspgum -lcommon -lpspdebug -lpspdisplay -lpspge -lpspctrl -lc -lm"
