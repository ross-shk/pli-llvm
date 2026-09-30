# plic — PL/I -> LLVM compiler
#
# M1 generates IR with the LLVM C++ API (ADR-002); the Makefile uses
# llvm-config to locate the LLVM installation.
#
# The only hard external dependencies are a C++20 compiler and LLVM >= 18.

LLVM_CONFIG ?= $(shell PATH="/opt/homebrew/opt/llvm/bin:$$PATH" which llvm-config 2>/dev/null)
ifeq ($(LLVM_CONFIG),)
  LLVM_CONFIG := $(shell which llvm-config 2>/dev/null)
endif
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags 2>/dev/null)
LLVM_VERSION  := $(shell $(LLVM_CONFIG) --version 2>/dev/null)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags 2>/dev/null)
LLVM_LIBS     := $(shell $(LLVM_CONFIG) --libs core irreader support 2>/dev/null)
LLVM_SYSTEM_LIBS := $(shell $(LLVM_CONFIG) --system-libs 2>/dev/null)
ifeq ($(LLVM_CXXFLAGS),)
  $(error llvm-config not found — install LLVM >= 18 via Homebrew (brew install llvm) or set LLVM_CONFIG=)
endif
LLVM_VERSION_OK := $(shell major=`$(LLVM_CONFIG) --version | cut -d. -f1`; \
                           test "$$major" -ge 18 2>/dev/null && echo yes)
ifeq ($(LLVM_VERSION_OK),)
  $(error plic requires LLVM >= 18)
endif

CXX      ?= c++
# Maximum optimization (-O3). Deliberately no -flto and no -march=native: LTO
# records host CPU features that leak into the emitted IR, and a compiler must
# not bake the build machine's features into its output (the assembler would
# reject them). Both would also slow every incremental rebuild.
CXXFLAGS ?= -O3 -Wall -Wextra -Wno-unused-parameter
# LLVM's flags are required even when callers override CXXFLAGS. Keep its ABI
# options, but put our language standard last so llvm-config cannot override it.
PLIC_CXXFLAGS := $(CXXFLAGS) $(filter-out -std=%,$(LLVM_CXXFLAGS)) -std=c++20
CC       ?= cc
CFLAGS   ?= -O3 -Wall -Wextra

BUILD    := build
BIN      := $(BUILD)/plic
RTLIB    := $(BUILD)/libpli.a

SRCS     := src/main.cpp src/diag.cpp src/explain.cpp src/hir.cpp src/lexer.cpp src/parser.cpp src/preprocessor.cpp src/sema.cpp src/irgen.cpp src/target.cpp src/codegen.cpp src/embedded_runtime.cpp
OBJS     := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

# --explain data: the TR 25.084 productions, generated from the spec so the
# table cannot drift from TR25.084-concrete-syntax.md (see scripts/gen_rules.py).
RULES_CPP := $(BUILD)/rules.cpp
RULES_OBJ := $(BUILD)/rules.o

RT_SRCS  := runtime/rt_core.c runtime/rt_stream.c runtime/rt_string.c \
            runtime/rt_math.c runtime/rt_mathport.c runtime/rt_cond.c \
            runtime/rt_storage.c runtime/rt_get.c runtime/rt_file.c \
            runtime/rt_edit.c runtime/rt_task.c
RT_OBJS  := $(patsubst runtime/%.c,$(BUILD)/rt_%.o,$(RT_SRCS))
# One function/data item per section, so the linker's dead-code strip keeps
# only the runtime pieces a program references (ADR-079).
RTCFLAGS := $(CFLAGS) -ffunction-sections -fdata-sections

# Baked-in default path to the runtime archive, so `plic hello.pli` just works.
RTPATH   := $(abspath $(RTLIB))

# Baked-in default clang used to assemble/optimize/link the emitted IR. plic
# emits IR in the syntax of the LLVM it links against, so the matching clang
# from that same LLVM install must be used (see main.cpp PLIC_CLANG).
CLANGPATH := $(shell $(LLVM_CONFIG) --bindir)/clang
LLVM_LINK := $(shell $(LLVM_CONFIG) --bindir)/llvm-link
LLVM_INCDIR := $(shell $(LLVM_CONFIG) --includedir 2>/dev/null)
LLVM_LIBDIR := $(shell $(LLVM_CONFIG) --libdir 2>/dev/null)

# lld detection (review S1): stock brew `llvm` ships no lld; the separate `lld`
# formula installs headers/libs under its own prefix. Probe LLVM first (a static
# LLVM bundles lld), then `brew --prefix lld`. The in-process link step gates on
# this (codegen.cpp PLIC_HAVE_LLD); without it plic falls back to clang.
LLD_PREFIX ?= $(shell brew --prefix lld 2>/dev/null)
ifneq ($(wildcard $(LLVM_INCDIR)/lld/Common/Driver.h),)
  LLD_INCDIR := $(LLVM_INCDIR)
  LLD_LIBDIR := $(LLVM_LIBDIR)
else
  LLD_INCDIR := $(LLD_PREFIX)/include
  LLD_LIBDIR := $(LLD_PREFIX)/lib
endif
ifeq ($(wildcard $(LLD_INCDIR)/lld/Common/Driver.h),)
  HAVE_LLD := 0
else
  HAVE_LLD := 1
endif
LLD_CXXFLAGS :=
LLD_LDFLAGS  :=
ifeq ($(HAVE_LLD),1)
  # Plain assignment (not $(if)): the commas in -Wl,-rpath would split the
  # function arguments and silently truncate this to just -Wl.
  LLD_CXXFLAGS := -I$(LLD_INCDIR)
  LLD_LDFLAGS  := -L$(LLD_LIBDIR) -Wl,-rpath,$(LLD_LIBDIR) \
                  -llldELF -llldMachO -llldCOFF -llldCommon
endif

# Embedded version for `plic version` (Go/Zig style build info).
PLIC_VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

# Static-analysis tooling from the same LLVM install (no system copies are
# assumed on PATH). Used by `make check` / its individual targets.
LLVM_BINDIR := $(shell $(LLVM_CONFIG) --bindir)
CLANG_TIDY := $(shell command -v $(LLVM_BINDIR)/clang-tidy 2>/dev/null || command -v clang-tidy 2>/dev/null)
CLANG_FORMAT := $(shell command -v $(LLVM_BINDIR)/clang-format 2>/dev/null || command -v clang-format 2>/dev/null)
SCAN_BUILD := $(shell command -v $(LLVM_BINDIR)/scan-build 2>/dev/null || command -v scan-build 2>/dev/null)

# Sources clang-format / clang-tidy operate on (C++ only; the runtime is C).
SRCS_TXT := $(SRCS) src/*.h

.PHONY: all clean test install check tidy fmt fmt-check scan werror
all: $(BIN) $(RTLIB)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(PLIC_CXXFLAGS) $(LLD_CXXFLAGS) -I$(BUILD) -DPLIC_RUNTIME_LIB='"$(RTPATH)"' \
		-DPLIC_INSTALL_RUNTIME_LIB='"$(LIBDIR)/libpli.a"' \
		-DPLIC_CLANG='"$(CLANGPATH)"' -DPLIC_VERSION='"$(PLIC_VERSION)"' \
		-DPLIC_LLVM_VERSION='"$(LLVM_VERSION)"' -DPLIC_HAVE_LLD=$(HAVE_LLD) \
		-MMD -MP -c $< -o $@

$(RULES_CPP): TR25.084-concrete-syntax.md scripts/gen_rules.py | $(BUILD)
	python3 scripts/gen_rules.py $< $@

$(RULES_OBJ): $(RULES_CPP) src/explain.h | $(BUILD)
	$(CXX) $(PLIC_CXXFLAGS) -Isrc -c $< -o $@

$(BUILD)/rt_%.o: runtime/%.c | $(BUILD)
	$(CC) $(RTCFLAGS) -MMD -MP -c $< -o $@

$(BIN): $(OBJS) $(RULES_OBJ) $(EMBED_INC)
	$(CXX) $(PLIC_CXXFLAGS) $(LLVM_LDFLAGS) $(OBJS) $(RULES_OBJ) $(LLD_LDFLAGS) $(LLVM_LIBS) $(LLVM_SYSTEM_LIBS) -o $@

$(RTLIB): $(RT_OBJS)
	ar rcs $@ $(RT_OBJS)

# --- embedded runtime (Phase 2) -------------------------------------------
# libpli.bc bundles the runtime as bitcode; gen_embed.py turns it into a C++
# byte array compiled into plic (no binutils, no install tree at runtime).
RT_BCS    := $(patsubst runtime/%.c,$(BUILD)/bc/%.bc,$(RT_SRCS))
LIBPLIBC  := $(BUILD)/libpli.bc
EMBED_INC := $(BUILD)/embedded_runtime.inc

$(BUILD)/bc:
	@mkdir -p $(BUILD)/bc

$(BUILD)/bc/%.bc: runtime/%.c | $(BUILD)/bc
	$(CLANGPATH) -emit-llvm -c $< -Iruntime -O2 -ffunction-sections -fdata-sections -o $@

$(LIBPLIBC): $(RT_BCS)
	$(LLVM_LINK) $(RT_BCS) -o $@

$(EMBED_INC): $(LIBPLIBC) scripts/gen_embed.py | $(BUILD)
	python3 scripts/gen_embed.py $(LIBPLIBC) $@

# The embedding TU includes the generated inc; ensure it exists first.
$(BUILD)/embedded_runtime.o: $(EMBED_INC)

# The object rules above are independent, so `make -j` parallelises the build;
# the test runner (tests/run_tests.py) parallelises its compile+run jobs
# (override with JOBS).
test: all
	@python3 tests/run_tests.py

# --- static analysis / lint gate -------------------------------------------
# `make check` is the single gate for major edits: a -Werror build, a
# clang-format drift check, clang-tidy, and the clang static analyzer, in that
# order, stopping on the first failure. Each analyzer also has its own target.
check: werror fmt-check tidy scan

# Rebuild the C++ objects with -Werror so warnings fail the build. The objects
# are dropped first so the new flag actually reaches the compiler.
werror:
	@rm -f $(OBJS) $(RULES_OBJ)
	@$(MAKE) CXXFLAGS="$(CXXFLAGS) -Werror" $(OBJS) $(RULES_OBJ)

# Rewrite sources in place to the repo format.
fmt:
	@test -n "$(CLANG_FORMAT)" || { echo "clang-format not found"; exit 1; }
	@$(CLANG_FORMAT) -i $(SRCS_TXT)

# Report (without fixing) whether sources drift from the repo format.
fmt-check:
	@test -n "$(CLANG_FORMAT)" || { echo "clang-format not found"; exit 1; }
	@if $(CLANG_FORMAT) --dry-run --Werror $(SRCS_TXT) >/dev/null 2>&1; then \
	  echo "fmt: clean"; \
	else \
	  echo "fmt: sources drift from .clang-format (run make fmt)"; \
	  $(CLANG_FORMAT) --dry-run $(SRCS_TXT) 2>&1 | sed -n '1,20p'; \
	  exit 1; \
	fi

# clang-tidy over the C++ sources (config in .clang-tidy). No compile database
# exists for the Makefile, so the compile flags are passed after `--`.
tidy:
	@test -n "$(CLANG_TIDY)" || { echo "clang-tidy not found"; exit 1; }
	@$(CLANG_TIDY) $(SRCS) --quiet -- \
		$(PLIC_CXXFLAGS) $(LLD_CXXFLAGS) -I$(BUILD) -DPLIC_RUNTIME_LIB='"$(RTPATH)"' \
		-DPLIC_INSTALL_RUNTIME_LIB='"$(LIBDIR)/libpli.a"' \
		-DPLIC_CLANG='"$(CLANGPATH)"' -DPLIC_HAVE_LLD=$(HAVE_LLD)

# The clang static analyzer; report only real bugs (--status-bugs). Drop the
# objects first so every source is re-analyzed.
scan:
	@test -n "$(SCAN_BUILD)" || { echo "scan-build not found"; exit 1; }
	@rm -f $(OBJS) $(RULES_OBJ)
	@$(SCAN_BUILD) --status-bugs $(MAKE) $(OBJS) $(RULES_OBJ)

# Stripped size of the driver (Phase 6 gate: target < 100 MB, cap 150 MB).
size-report: $(BIN)
	@echo "stripped size:" && strip -o /tmp/plic.strip $(BIN) && \
	  (stat -f%z /tmp/plic.strip 2>/dev/null || stat -c%s /tmp/plic.strip)
	@echo "target: < 100 MB (hard cap 150 MB)"

clean:
	rm -rf $(BUILD) tests/*/out

# Install plic and the runtime archive. The driver falls back to the sibling
# lib directory when its baked-in build-tree runtime is unavailable.
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin
LIBDIR  := $(PREFIX)/lib

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR)
	# Install a minimized (stripped) copy of the driver: keep the build tree
	# binary intact, strip the installed one to reduce installed size.
	cp $(BIN) $(BUILD)/plic.install
	strip $(BUILD)/plic.install
	install -m 755 $(BUILD)/plic.install $(DESTDIR)$(BINDIR)/plic
	install -m 644 $(RTLIB) $(DESTDIR)$(LIBDIR)/libpli.a
	rm -f $(BUILD)/plic.install

-include $(DEPS)
