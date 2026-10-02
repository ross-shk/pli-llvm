# plic — PL/I -> LLVM compiler
#
# CMake is the canonical build (it links the LLVM C++ API per ADR-002 plus
# lld and embeds the runtime bitcode). The Makefile delegates to CMake for
# the static LLVM single-binary distribution, keeping the familiar
# `make -j8 && make test` workflow. The only hard external dependencies
# are a C++20 compiler, CMake, Ninja, and an LLVM installation with lld, e.g.
#   brew install llvm lld cmake ninja
#   make -j8 && make test
#
# Direct (non-CMake) compilation was retired when the driver gained its
# in-process lld link and embedded runtime (new sources under src/ need
# lld + generated bitcode includes that only the CMake build provides).

LLVM_STATIC_PREFIX ?=
CMAKE_ARGS ?=
CMAKE_BUILD_DIR = $(BUILD)/cmake

BUILD    := build
BIN      := $(BUILD)/plic
RTLIB    := $(BUILD)/libpli.a

.PHONY: all clean test install check tidy fmt fmt-check scan werror size-report

# Default target delegates to CMake (parallelise with `make -j8`).
all:
	cmake -G Ninja -S . -B $(CMAKE_BUILD_DIR) $(CMAKE_ARGS) \
		$(if $(LLVM_STATIC_PREFIX),-DLLVM_STATIC_PREFIX=$(LLVM_STATIC_PREFIX))
	cmake --build $(CMAKE_BUILD_DIR) -j8
	# Convenience symlinks for existing scripts/tests
	ln -sf cmake/plic $(BIN)
	ln -sf cmake/libpli.a $(RTLIB)

# All compilation happens inside the CMake build above (it provides the
# LLVM/lld flags, the generated rules table, and the embedded runtime
# bitcode). The targets below operate on its outputs via $(BIN)/$(RTLIB).

# Run tests (uses the CMake-built plic via the build/plic symlink)
test: all
	@python3 tests/run_tests.py

# Stripped size of the driver (Phase 6 gate: target < 100 MB, cap 150 MB).
size-report: all
	@echo "stripped size:" && strip -o /tmp/plic.strip $(BIN) && \
	  (stat -f%z /tmp/plic.strip 2>/dev/null || stat -c%s /tmp/plic.strip)
	@echo "target: < 100 MB (hard cap 150 MB)"

# Clean build artifacts
clean:
	rm -rf $(BUILD) tests/*/out

# Install plic, libpli.a and runtime.bc via the CMake install rules
# (see the install() commands in CMakeLists.txt).
PREFIX  ?= /usr/local

install: all
	cmake --install $(CMAKE_BUILD_DIR) --prefix "$(DESTDIR)$(PREFIX)"

# Static analysis / lint gate (requires LLVM tooling from the same LLVM
# installation the CMake build uses; the Makefile itself no longer compiles).
# These need the LLVM tooling from the static build - skip for now
check fmt fmt-check tidy scan werror:
	@echo "Target '$@' requires LLVM tooling. Run with CMake build using static LLVM tooling."
	@echo "Use: cmake --build build/cmake --target <target>"
	@exit 1
