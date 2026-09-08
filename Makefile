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
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags 2>/dev/null)
LLVM_LIBS     := $(shell $(LLVM_CONFIG) --libs core irreader support 2>/dev/null)
ifeq ($(LLVM_CXXFLAGS),)
  $(error llvm-config not found — install LLVM >= 18 via Homebrew (brew install llvm) or set LLVM_CONFIG=)
endif

CXX      ?= c++
# LLVM's cxxflags carry -std=c++17; we keep -std=c++20 in our own flags and
# strip the -std= and -stdlib= that llvm-config adds to avoid an override.
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter \
            $(filter-out -std=% -stdlib=%,$(LLVM_CXXFLAGS))
CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra

BUILD    := build
BIN      := $(BUILD)/plic
RTLIB    := $(BUILD)/libpli.a

SRCS     := src/main.cpp src/diag.cpp src/explain.cpp src/lexer.cpp src/parser.cpp src/sema.cpp src/irgen.cpp
OBJS     := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

# --explain data: the TR 25.084 productions, generated from the spec so the
# table cannot drift from TR25.084-concrete-syntax.md (see scripts/gen_rules.py).
RULES_CPP := $(BUILD)/rules.cpp
RULES_OBJ := $(BUILD)/rules.o

RT_SRCS  := runtime/pli_rt.c
RT_OBJS  := $(patsubst runtime/%.c,$(BUILD)/rt_%.o,$(RT_SRCS))

# Baked-in default path to the runtime archive, so `plic hello.pli` just works.
RTPATH   := $(abspath $(RTLIB))

.PHONY: all clean test install
all: $(BIN) $(RTLIB)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -DPLIC_RUNTIME_LIB='"$(RTPATH)"' -MMD -MP -c $< -o $@

$(RULES_CPP): TR25.084-concrete-syntax.md scripts/gen_rules.py | $(BUILD)
	python3 scripts/gen_rules.py $< $@

$(RULES_OBJ): $(RULES_CPP) src/explain.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

$(BUILD)/rt_%.o: runtime/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS) $(RULES_OBJ)
	$(CXX) $(CXXFLAGS) $(LLVM_LDFLAGS) $(OBJS) $(RULES_OBJ) $(LLVM_LIBS) -o $@

$(RTLIB): $(RT_OBJS)
	ar rcs $@ $(RT_OBJS)

test: all
	@tests/run_tests.sh

clean:
	rm -rf $(BUILD) tests/*/out

# Install plic and the runtime archive. The baked-in RTPATH inside the binary
# still points at build/libpli.a, so installed plic needs --runtime to find the
# archive unless PREFIX matches where libpli.a is placed.
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin
LIBDIR  := $(PREFIX)/lib

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/plic
	install -m 644 $(RTLIB) $(DESTDIR)$(LIBDIR)/libpli.a

-include $(DEPS)
