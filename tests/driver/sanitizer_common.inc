# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
# tests/driver/sanitizer_common.inc — shared setup for the sanitizer driver
# tests (B0). Sourced, never run: the runner only picks up *.sh and *.pli.
#
# The compiler embeds its runtime bitcode uninstrumented, so a sanitizer run
# builds the runtime from source with the sanitizer and links it against the
# program's own IR. `-emit-llvm` stops before the embedded runtime is merged
# (ADR-349 keeps the driver link path embedded-only; this is a test harness,
# not a driver option). ADR-358 records the mechanism.
#
# Clang does not add sanitizer checks to IR input (ADR-341), and the ASan/TSan
# passes instrument only functions marked for the sanitizer, so the program IR
# is marked before clang compiles it. UBSan is a front-end pass: it covers the
# runtime source, which is where the storage and overlay code lives.

# The runtime sources, mirroring RT_PORTABLE_SRCS + RT_PLATFORM_SRCS in
# CMakeLists.txt. Keep the two lists in sync: a new runtime TU missing here
# would stay uninstrumented and silently unmeasured.
SAN_RT_SRCS="rt_core rt_stream rt_string rt_math rt_mathport rt_cond
  rt_storage rt_get rt_file rt_edit rt_task rt_sync rt_input rt_fbroker
  rt_sched rt_fiber_asm rt_plat_default"

# Resolve the clang of the compiler's own LLVM (LLVM_BINDIR, then CLANG as
# ctest passes it, then the build tree's CMake cache). Prints the path or
# nothing when no such clang exists.
san_cc() {
  if [ -n "${LLVM_BINDIR:-}" ] && [ -x "$LLVM_BINDIR/clang" ]; then
    echo "$LLVM_BINDIR/clang"
    return
  fi
  case "${CLANG:-}" in
    */clang|*/clang-[0-9]*)
      if [ -x "$CLANG" ]; then
        echo "$CLANG"
        return
      fi
      ;;
  esac
  if [ -f build/cmake/CMakeCache.txt ]; then
    llvm_dir=$(sed -n 's/^LLVM_DIR:[A-Z_]*=//p' build/cmake/CMakeCache.txt | head -1)
    if [ -n "$llvm_dir" ]; then
      # <prefix>/lib/cmake/llvm -> <prefix>
      prefix=$(dirname "$(dirname "$(dirname "$llvm_dir")")")
      if [ -x "$prefix/bin/clang" ]; then
        echo "$prefix/bin/clang"
        return
      fi
    fi
  fi
}

# A source-built clang has no default macOS SDK sysroot, so system headers and
# libSystem would be missing. Point it at the active SDK when one exists.
SAN_SDK=""
if [ "$(uname -s)" = Darwin ]; then
  sdk=$(xcrun --show-sdk-path 2>/dev/null) || sdk=""
  [ -n "$sdk" ] && SAN_SDK="-isysroot $sdk"
fi

# san_build_runtime <cc> <san flags> <outdir> — compile the runtime with the
# sanitizer. Non-zero when a TU does not compile; the caller reports which.
san_build_runtime() {
  mkdir -p "$3" || return 1
  for s in $SAN_RT_SRCS; do
    "$1" $2 $SAN_SDK -O1 -g -fno-omit-frame-pointer -Iruntime \
      -DPLI_LLVM_VERSION='"unknown"' -c "runtime/$s.c" \
      -o "$3/$s.o" >"$3/$s.log" 2>&1 || return 1
  done
}

# san_instrument_ir <in.ll> <out.ll> <attribute> — mark every emitted function
# with the sanitizer attribute so clang's pass instruments it. Fails rather
# than silently compiling an uninstrumented object when the IR has no groups.
san_instrument_ir() {
  sed -E "s/^attributes #([0-9]+) = \\{/attributes #\\1 = { $3/" "$1" > "$2" || return 1
  grep -q "$3" "$2"
}

# san_compile_program <cc> <san flags> <prog.ll> <prog.o> — compile the marked
# program IR with the same sanitizer; the link adds the sanitizer runtime.
san_compile_program() {
  "$1" $2 $SAN_SDK -c "$3" -o "$4"
}

# san_link <cc> <san flags> <program.o> <rtdir> <exe> — link the program
# against the instrumented runtime objects.
san_link() {
  case "$(uname -s)" in
    Linux) libs="-lpthread -lm -ldl" ;;
    *) libs="-lpthread -lm" ;;
  esac
  "$1" $2 $SAN_SDK "$3" "$4"/*.o $libs -o "$5"
}

# san_missing_runtime <link log> — non-zero when the link failed because this
# toolchain has no sanitizer runtime (the static stash ships none); the test
# then skips instead of failing on a bare toolchain.
san_missing_runtime() {
  grep -qE 'sanitiz|libclang_rt\.' "$1"
}
