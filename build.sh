#!/usr/bin/env bash
# Configure and build PSPC on Linux. JOBS caps memory-heavy C++ compilation.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
config=${1:-RelWithDebInfo}
if (($#)); then shift; fi
cmake -S . -B "build/$config" -G Ninja -DCMAKE_BUILD_TYPE="$config" ${PSPC_CMAKE_ARGS:-}
cmake --build "build/$config" --parallel "${JOBS:-4}" "$@"
