# plic — PL/I -> LLVM compiler (M0 wireframe)
#
# CMake + the LLVM C++ API arrive in M1 (see docs/IMPLEMENTATION-PLAN.md);
# M0 deliberately depends on nothing but a C++20 compiler and `clang`.

CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter
CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra

BUILD    := build
BIN      := $(BUILD)/plic
RTLIB    := $(BUILD)/libpli.a

SRCS     := src/main.cpp src/diag.cpp src/lexer.cpp src/parser.cpp src/sema.cpp src/irgen.cpp
OBJS     := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

RT_SRCS  := runtime/pli_rt.c
RT_OBJS  := $(patsubst runtime/%.c,$(BUILD)/rt_%.o,$(RT_SRCS))

# Baked-in default path to the runtime archive, so `plic hello.pli` just works.
RTPATH   := $(abspath $(RTLIB))

.PHONY: all clean test
all: $(BIN) $(RTLIB)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -DPLIC_RUNTIME_LIB='"$(RTPATH)"' -MMD -MP -c $< -o $@

$(BUILD)/rt_%.o: runtime/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

$(RTLIB): $(RT_OBJS)
	ar rcs $@ $(RT_OBJS)

test: all
	@tests/run_tests.sh

clean:
	rm -rf $(BUILD) tests/*/out

-include $(DEPS)
