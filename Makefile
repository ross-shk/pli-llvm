# plic — PL/I -> LLVM compiler
#
# CMake is the canonical build; the Makefile delegates to CMake for the
# static LLVM single-binary distribution. The only hard external dependencies
# are a C++20 compiler, CMake, and a static LLVM installation (official release).

LLVM_STATIC_PREFIX ?=
CMAKE_ARGS ?=

BUILD    := build
BIN      := $(BUILD)/plic
RTLIB    := $(BUILD)/libpli.a

.PHONY: all clean test install check tidy fmt fmt-check scan werror size-report

# Default target delegates to CMake
all:
	cmake -S . -B $(BUILD)/cmake $(CMAKE_ARGS) \
		$(if $(LLVM_STATIC_PREFIX),-DLLVM_STATIC_PREFIX=$(LLVM_STATIC_PREFIX))
	cmake --build $(BUILD)/cmake -j8
	# Convenience symlinks for existing scripts/tests
	ln -sf cmake/plic $(BIN)
	ln -sf cmake/libpli.a $(RTLIB)

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

# The CMake build handles codegen, embedding, and testing.
# Keep useful targets that work with the CMake build.

# Run tests (uses the CMake-built plic from build/cmake)
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

# Install plic and the runtime archive from CMake build
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin
LIBDIR  := $(PREFIX)/lib

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR)
	cp $(BIN) $(BUILD)/plic.install
	strip $(BUILD)/plic.install
	install -m 755 $(BUILD)/plic.install $(DESTDIR)$(BINDIR)/plic
	install -m 644 $(RTLIB) $(DESTDIR)$(LIBDIR)/libpli.a
	rm -f $(BUILD)/plic.install

# Static analysis / lint gate (requires LLVM tooling from the same static LLVM)
# These need the LLVM tooling from the static build - skip for now
check fmt fmt-check tidy scan werror:
	@echo "Target '$@' requires LLVM tooling. Run with CMake build using static LLVM tooling."
	@echo "Use: cmake --build build/cmake --target <target>"
	@exit 1
