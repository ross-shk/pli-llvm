# plic — PL/I -> LLVM compiler
#
# CMake + Ninja is the canonical build (ADR-042: clang stays the link driver;
# `check` is the analysis gate). This Makefile is a thin convenience wrapper
# that keeps the familiar `make -j8 && make test` workflow while delegating
# all compilation, quality-gate, and install work to CMake/Ninja targets.
#
# Prerequisites:
#   brew install llvm lld cmake ninja        (macOS)
#   apt-get install clang lld llvm ninja-build cmake  (Linux)
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

# Ensure the CMake build directory is configured (Ninja generator).
$(CMAKE_BUILD_DIR)/build.ninja:
	cmake -G Ninja -S . -B $(CMAKE_BUILD_DIR) $(CMAKE_ARGS) \
		$(if $(LLVM_STATIC_PREFIX),-DLLVM_STATIC_PREFIX=$(LLVM_STATIC_PREFIX))

# Default target: configure (if needed) + build via CMake/Ninja, create symlinks.
all: $(CMAKE_BUILD_DIR)/build.ninja
	cmake --build $(CMAKE_BUILD_DIR) -j8
	# Convenience symlinks for existing scripts/tests
	ln -sf cmake/plic $(BIN)
	ln -sf cmake/libpli.a $(RTLIB)

# Run tests (uses the CMake-built plic via the build/plic symlink).
test: all
	@python3 tests/run_tests.py

# Stripped binary size (Phase 6 gate: target < 100 MB, hard cap 150 MB).
size-report: $(CMAKE_BUILD_DIR)/build.ninja
	cmake --build $(CMAKE_BUILD_DIR) --target size-report

# Clean build artifacts.
clean:
	rm -rf $(BUILD) tests/*/out

# Install plic, libpli.a and runtime.bc via the CMake install rules
# (see the install() commands in CMakeLists.txt).
PREFIX  ?= /usr/local
install: all
	cmake --install $(CMAKE_BUILD_DIR) --prefix "$(DESTDIR)$(PREFIX)"

# Quality gate targets — delegate to CMake custom targets.
# See CMakeLists.txt "Quality gate" section for full definitions:
#   check   — fmt-check + tidy + werror (the full analysis gate)
#   werror  — rebuild with -DPLIC_WERROR=ON
#   fmt     — clang-format all sources in place
#   fmt-check — verify formatting without modifying files
#   tidy    — clang-tidy over C++ sources
#   scan    — scan-build static analyzer
check werror fmt fmt-check tidy scan: $(CMAKE_BUILD_DIR)/build.ninja
	cmake --build $(CMAKE_BUILD_DIR) --target $@
