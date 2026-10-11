#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Linux/macOS wrapper for the PL/I benchmark harness (cf. run_benchmarks.cmd).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export CC="${CC:-clang}"
export PLIC="${PLIC:-$DIR/../build/plic}"
if [ ! -x "$PLIC" ] && [ -x "$DIR/../build/cmake/plic" ]; then
  PLIC="$DIR/../build/cmake/plic"
  export PLIC
fi
exec python3 "$DIR/run_benchmarks.py" "$@"
