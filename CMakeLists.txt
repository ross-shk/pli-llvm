# plic — PL/I -> LLVM compiler (CMake + Ninja build, ADR-002: LLVM C++ API).
#
# CMake + Ninja is the canonical build. The Makefile is a thin convenience
# wrapper that delegates to this CMake build. Requires an LLVM installation
# with development libraries, e.g.:
#   brew install llvm lld cmake ninja
#   cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
#   cmake --build build/cmake -j$(nproc)
# Quality gate: cmake --build build/cmake --target check
cmake_minimum_required(VERSION 3.20)
project(plic CXX C)

include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(PLIC_WERROR "Treat compiler warnings as errors" OFF)

find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "plic requires LLVM 18 or newer")
endif()
find_package(Clang REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
add_definitions(${LLVM_DEFINITIONS})
# Expose implicit C/C++ std-lib include paths in compile_commands.json so that
# clang-tidy (from the LLVM install) can resolve <cstdio>, <vector>, etc.
# when CMAKE_CXX_COMPILER is a different clang (e.g. Apple clang on macOS).
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})

set(PLIC_SOURCES
  src/main.cpp
  src/diag.cpp
  src/explain.cpp
  src/hir.cpp
  src/lexer.cpp
  src/parser.cpp
  src/preprocessor.cpp
  src/sema.cpp
  src/irgen.cpp
  src/target.cpp
  src/codegen.cpp
  src/embedded_runtime.cpp
)

set(RT_SRCS
  runtime/rt_core.c runtime/rt_stream.c runtime/rt_string.c
  runtime/rt_math.c runtime/rt_mathport.c runtime/rt_cond.c
  runtime/rt_storage.c runtime/rt_get.c runtime/rt_file.c
  runtime/rt_edit.c runtime/rt_task.c)

# lld detection (review S1): in-process link gates on PLIC_HAVE_LLD.
# Prerequisite: stock brew `llvm` ships no lld; fail configure with install hint.
# Check both the LLVM prefix and the separate lld formula (brew install lld).
find_path(LLD_INCLUDE_DIR lld/Common/Driver.h
  PATHS ${LLVM_INCLUDE_DIRS} /opt/homebrew/opt/lld/include
  NO_DEFAULT_PATH)
if(NOT LLD_INCLUDE_DIR)
  # Try to find lld cmake config to get its include dir
  # (LLDConfig.cmake sets LLD_INCLUDE_DIRS, uppercase).
  find_package(lld QUIET)
  if(LLD_INCLUDE_DIRS)
    set(LLD_INCLUDE_DIR ${LLD_INCLUDE_DIRS})
  elseif(lld_INCLUDE_DIRS)
    set(LLD_INCLUDE_DIR ${lld_INCLUDE_DIRS})
  endif()
endif()
if(NOT LLD_INCLUDE_DIR)
  message(FATAL_ERROR "lld headers not found. Install lld (e.g., 'brew install lld') and ensure LLVM_DIR points to an LLVM with lld.")
endif()
set(PLIC_HAVE_LLD 1)
message(STATUS "lld headers found: ${LLD_INCLUDE_DIR}")

# Find lld library directory (Homebrew puts dylibs in /opt/homebrew/opt/lld/lib)
# Link directly against the dynamic libraries using full paths
set(LLD_ROOT "" CACHE PATH "Path to lld installation root")
if(LLD_ROOT)
  list(APPEND CMAKE_PREFIX_PATH ${LLD_ROOT})
endif()
find_library(LLD_ELF_LIB NAMES lldELF)
find_library(LLD_MACHO_LIB NAMES lldMachO)
find_library(LLD_COFF_LIB NAMES lldCOFF)
find_library(LLD_COMMON_LIB NAMES lldCommon)
set(LLD_LIBRARIES ${LLD_ELF_LIB} ${LLD_MACHO_LIB} ${LLD_COFF_LIB} ${LLD_COMMON_LIB})
message(STATUS "lld libraries: ${LLD_LIBRARIES}")

add_library(plic_rules STATIC ${CMAKE_BINARY_DIR}/rules.cpp)
target_include_directories(plic_rules PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Embedded version for `plic version` (Go/Zig style build info).
execute_process(COMMAND git describe --tags --always --dirty
  OUTPUT_VARIABLE PLIC_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PLIC_VERSION)
  set(PLIC_VERSION "unknown")
endif()

add_executable(plic ${PLIC_SOURCES})
target_include_directories(plic PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})
# LLVM/Clang headers as SYSTEM to suppress their warnings under -Werror.
target_include_directories(plic SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

# Get the default target triple from LLVM for consistent bitcode generation.
# Use clang (not CMAKE_C_COMPILER) for triple detection and bitcode generation
# since GCC doesn't support -print-effective-triple or -emit-llvm -target.
set(PLIC_TARGET_TRIPLE "" CACHE STRING "Target triple for runtime bitcode generation")
if(NOT PLIC_TARGET_TRIPLE)
  execute_process(
    COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -print-effective-triple
    OUTPUT_VARIABLE PLIC_HOST_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(PLIC_TARGET_TRIPLE ${PLIC_HOST_TRIPLE})
endif()
message(STATUS "Target triple for bitcode: ${PLIC_TARGET_TRIPLE}")

target_compile_definitions(plic PRIVATE
  PLIC_RUNTIME_LIB="$<TARGET_FILE:libpli>"
  PLIC_INSTALL_RUNTIME_LIB="${CMAKE_INSTALL_FULL_LIBDIR}/libpli.a"
  PLIC_RUNTIME_BC="${CMAKE_BINARY_DIR}/runtime.bc"
  PLIC_INSTALL_RUNTIME_BC="${CMAKE_INSTALL_FULL_LIBDIR}/runtime.bc"
  PLIC_CLANG="${LLVM_TOOLS_BINARY_DIR}/clang"
  PLIC_VERSION="${PLIC_VERSION}"
  PLIC_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
  PLIC_HAVE_LLD=${PLIC_HAVE_LLD}
  PLIC_TARGETS_TO_BUILD="host"
  PLIC_TARGET_TRIPLE="${PLIC_TARGET_TRIPLE}"
  PLIC_HAVE_LINUX_X86_64_RUNTIME=1
  PLIC_HAVE_LINUX_AARCH64_RUNTIME=1
  PLIC_HAVE_WINDOWS_X86_64_RUNTIME=1
  PLIC_HAVE_WINDOWS_AARCH64_RUNTIME=1)

# Link LLVM components needed for in-process codegen + linking.
# Shared-LLVM builds (Debian LLVM 22 sets LLVM_LINK_LLVM_DYLIB): link the
# shared libLLVM only. Mixing static component archives with the shared lib
# (pulled in by the Clang targets below) duplicates LLVM globals and crashes
# flakily at exit after O2 codegen on ARM64 (free(): invalid pointer).
# Static builds and Apple keep the component archives as before.
if(LLVM_LINK_LLVM_DYLIB AND NOT APPLE)
  set(llvm_libs_core LLVM z zstd)
  set(llvm_libs_mc "")
else()
  # Core/IR (superset of the old `core irreader support linker` set).
  llvm_map_components_to_libnames(llvm_libs_core
    Core IRReader Support BitReader BitWriter Linker)
  # MC/Target (host arch only for milestone)
  llvm_map_components_to_libnames(llvm_libs_mc
    MC MCParser MCDisassembler Object BinaryFormat TargetParser AsmParser AsmPrinter
    CodeGen SelectionDAG GlobalISel Option
    X86CodeGen X86AsmParser X86Disassembler X86Desc X86Info
    AArch64CodeGen AArch64AsmParser AArch64Disassembler AArch64Desc AArch64Info
    WindowsManifest WindowsDriver LibDriver
    Passes LTO)
endif()
# lld
llvm_map_components_to_libnames(llvm_libs_lld
  lldELF lldMachO lldCOFF lldCommon)

# Use dynamically found lld libraries if available
if(LLD_LIBRARIES AND NOT "${LLD_LIBRARIES}" STREQUAL "lldELF lldMachO lldCOFF lldCommon lldDriver")
  set(llvm_libs_lld ${LLD_LIBRARIES})
endif()

# Use Clang imported targets directly
target_link_libraries(plic PRIVATE plic_rules
  ${llvm_libs_lld} ${llvm_libs_mc} ${llvm_libs_core}
  clangFrontend clangParse clangLex clangBasic clangAST)

# Force-include lld + host target so their symbols survive static dead-stripping.
# Homebrew lld only provides dynamic libraries, so use those directly.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_options(plic PRIVATE
    -Wl,-force_load,${LLD_ELF_LIB}
    -Wl,-force_load,${LLD_MACHO_LIB}
    -Wl,-force_load,${LLD_COFF_LIB})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_link_options(plic PRIVATE
    /WHOLEARCHIVE:${LLD_ELF_LIB}
    /WHOLEARCHIVE:${LLD_MACHO_LIB}
    /WHOLEARCHIVE:${LLD_COFF_LIB})
else()
  target_link_options(plic PRIVATE
    -Wl,--whole-archive
    ${LLD_ELF_LIB}
    ${LLD_MACHO_LIB}
    ${LLD_COFF_LIB}
    -Wl,--no-whole-archive)
endif()

# Static C++ runtime for portability.
if(APPLE)
  # Nothing additional needed on macOS.
elseif(WIN32)
  # Default is already /MD (dynamic CRT) matching LLVM. Do not pass /MD or
  # /MT to the linker (LNK4044); CRT selection is a compiler flag.
else()
  target_link_options(plic PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Cross-compilation bitcode generation (Phase 2+): requires sysroots.
option(PLIC_CROSS_BITCODE "Generate bitcode for cross-targets (needs sysroots)" OFF)

# libpli runtime (C11). One source per section so the archive strips unused
# pieces (ADR-079); the same sources feed both runtime paths below.
add_library(libpli STATIC ${RT_SRCS})
set_target_properties(libpli PROPERTIES OUTPUT_NAME pli)

# Warnings: -Wall -Wextra on the C++ driver (libpli is C11 with its own style).
if(MSVC)
  target_compile_options(plic PRIVATE /W3)
else()
  target_compile_options(plic PRIVATE -Wall -Wextra)
endif()
if(PLIC_WERROR)
  if(MSVC)
    target_compile_options(plic PRIVATE /WX)
  else()
    target_compile_options(plic PRIVATE -Werror)
  endif()
endif()

# Multi-target embedded runtime: generate bitcode for all supported targets.
if(PLIC_CROSS_BITCODE)
  set(PLIC_TARGETS "x86_64-unknown-linux-gnu;aarch64-unknown-linux-gnu;x86_64-pc-windows-gnu;aarch64-pc-windows-gnu")

  # Per-target bitcode generation and embedding.
  foreach(target ${PLIC_TARGETS})
    string(REPLACE "-" "_" target_suffix "_${target}")
    set(TARGET_BC_DIR ${CMAKE_BINARY_DIR}/bc/${target})
    set(TARGET_RT_BCS "")
    foreach(src ${RT_SRCS})
      get_filename_component(base ${src} NAME_WE)
      add_custom_command(
        OUTPUT ${TARGET_BC_DIR}/${base}.bc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TARGET_BC_DIR}
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
                -I${CMAKE_SOURCE_DIR}/runtime -I${CMAKE_SOURCE_DIR}/runtime/sync
                -O2
                -ffunction-sections -fdata-sections
                $<$<OR:$<STREQUAL:${target},x86_64-unknown-linux-gnu>,$<STREQUAL:${target},aarch64-unknown-linux-gnu>>:-fPIC>
                -target ${target}
                -o ${TARGET_BC_DIR}/${base}.bc
        DEPENDS ${CMAKE_SOURCE_DIR}/${src}
        COMMENT "bitcode: ${base} for ${target}")
      list(APPEND TARGET_RT_BCS ${TARGET_BC_DIR}/${base}.bc)
    endforeach()
    add_custom_command(
      OUTPUT ${TARGET_BC_DIR}/libpli.bc
      COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${TARGET_RT_BCS} -o ${TARGET_BC_DIR}/libpli.bc
      DEPENDS ${TARGET_RT_BCS})
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
              ${TARGET_BC_DIR}/libpli.bc ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
              ${target_suffix}
      DEPENDS ${TARGET_BC_DIR}/libpli.bc ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
      COMMENT "embedding libpli.bc for ${target}")
    add_custom_target(embedded_runtime_inc${target_suffix} DEPENDS ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc)
    add_dependencies(plic embedded_runtime_inc${target_suffix})
  endforeach()
else()
  # Define empty per-target blobs for non-cross builds (compiler still works)
  # Target suffixes must match PLIC_TARGETS naming convention (full triple)
  foreach(target IN ITEMS x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-pc-windows-gnu aarch64-pc-windows-gnu)
    string(REPLACE "-" "_" target_suffix "_${target}")
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
              /dev/null ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
              ${target_suffix}
      DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
      COMMENT "stub embedding for ${target}")
    add_custom_target(embedded_runtime_inc${target_suffix} DEPENDS ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc)
    add_dependencies(plic embedded_runtime_inc${target_suffix})
  endforeach()
endif()

# Default target (host) bitcode for backward compatibility.
set(DEFAULT_BC_DIR ${CMAKE_BINARY_DIR}/bc/${PLIC_TARGET_TRIPLE})
set(DEFAULT_RT_BCS "")
foreach(src ${RT_SRCS})
  get_filename_component(base ${src} NAME_WE)
  add_custom_command(
    OUTPUT ${DEFAULT_BC_DIR}/${base}.bc
      COMMAND ${CMAKE_COMMAND} -E make_directory ${DEFAULT_BC_DIR}
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
            -I${CMAKE_SOURCE_DIR}/runtime -I${CMAKE_SOURCE_DIR}/runtime/sync
            -O2
            -ffunction-sections -fdata-sections
            $<$<PLATFORM_ID:Linux>:-fPIC>
            -target ${PLIC_TARGET_TRIPLE}
            -o ${DEFAULT_BC_DIR}/${base}.bc
    DEPENDS ${CMAKE_SOURCE_DIR}/${src}
    COMMENT "bitcode: ${base} for ${PLIC_TARGET_TRIPLE}")
  list(APPEND DEFAULT_RT_BCS ${DEFAULT_BC_DIR}/${base}.bc)
endforeach()
add_custom_command(
  OUTPUT ${DEFAULT_BC_DIR}/libpli.bc
  COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${DEFAULT_RT_BCS} -o ${DEFAULT_BC_DIR}/libpli.bc
  DEPENDS ${DEFAULT_RT_BCS})
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/embedded_runtime.inc
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
          ${DEFAULT_BC_DIR}/libpli.bc ${CMAKE_BINARY_DIR}/embedded_runtime.inc
  DEPENDS ${DEFAULT_BC_DIR}/libpli.bc ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
  COMMENT "embedding libpli.bc for default target ${PLIC_TARGET_TRIPLE}")
add_custom_target(embedded_runtime_inc_default DEPENDS ${CMAKE_BINARY_DIR}/embedded_runtime.inc)
add_dependencies(plic embedded_runtime_inc_default)

# File-based bitcode runtime (OPTIMIZATION.md §10, P3): the same sources
# compiled to LLVM IR with the matching clang and merged into runtime.bc with
# llvm-link. IRGen merges the runtime into the MAIN unit so clang sees real
# bodies; the archive stays for --no-bitcode-runtime. rt_core.c stamps
# runtime.bc with the LLVM version, checked at load time so a stale bitcode
# is diagnosed, not mis-linked. (The embedded blob above serves the
# in-process path; this file serves --runtime-bc and the Makefile build.)
set(PLIC_RUNTIME_BC ${CMAKE_BINARY_DIR}/runtime.bc)
set(PLI_RUNTIME_BCS "")
foreach(rt_src ${RT_SRCS})
  get_filename_component(rt_base ${rt_src} NAME_WE)
  set(rt_bc ${CMAKE_BINARY_DIR}/${rt_base}.bc)
  add_custom_command(
    OUTPUT ${rt_bc}
    COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c
            -DPLIC_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
            -I${CMAKE_SOURCE_DIR}/runtime -I${CMAKE_SOURCE_DIR}/runtime/sync
            -O2 -ffunction-sections -fdata-sections
            $<$<PLATFORM_ID:Linux>:-fPIC>
            -target ${PLIC_TARGET_TRIPLE}
            ${CMAKE_SOURCE_DIR}/${rt_src} -o ${rt_bc}
    DEPENDS ${CMAKE_SOURCE_DIR}/${rt_src}
    COMMENT "Compiling ${rt_base}.c to bitcode"
    VERBATIM
  )
  list(APPEND PLI_RUNTIME_BCS ${rt_bc})
endforeach()
add_custom_command(
  OUTPUT ${PLIC_RUNTIME_BC}
  COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${PLI_RUNTIME_BCS}
          -o ${PLIC_RUNTIME_BC}
  DEPENDS ${PLI_RUNTIME_BCS}
  COMMENT "Merging the runtime bitcode (runtime.bc)"
  VERBATIM
)
add_custom_target(runtime_bc ALL DEPENDS ${PLIC_RUNTIME_BC})

# Generated rules table for --explain (see scripts/gen_rules.py); the spec and
# the generator are the source of truth, so regenerate on every build.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/rules.cpp
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_rules.py
          ${CMAKE_SOURCE_DIR}/TR25.084-concrete-syntax.md ${CMAKE_BINARY_DIR}/rules.cpp
  DEPENDS ${CMAKE_SOURCE_DIR}/TR25.084-concrete-syntax.md
          ${CMAKE_SOURCE_DIR}/scripts/gen_rules.py
  COMMENT "Generating rules table for --explain"
  VERBATIM
)

enable_testing()
add_test(NAME plic-tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/run_tests.py)
set_tests_properties(plic-tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  ENVIRONMENT "PLIC=$<TARGET_FILE:plic>;RTLIB=$<TARGET_FILE:libpli>;CLANG=${LLVM_TOOLS_BINARY_DIR}/clang")

install(TARGETS plic libpli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${PLIC_RUNTIME_BC} DESTINATION ${CMAKE_INSTALL_LIBDIR})

# --- Quality gate: formatting, static analysis, warnings-as-errors ---------------
# The clang tooling is sought from the same LLVM installation as the compiler.
find_program(CLANG_FORMAT_BIN NAMES clang-format clang-format-${LLVM_VERSION_MAJOR}
  HINTS ${LLVM_TOOLS_BINARY_DIR})
find_program(CLANG_TIDY_BIN NAMES clang-tidy clang-tidy-${LLVM_VERSION_MAJOR}
  HINTS ${LLVM_TOOLS_BINARY_DIR})
find_program(SCAN_BUILD_BIN NAMES scan-build scan-build-py
  HINTS ${LLVM_TOOLS_BINARY_DIR})

# All project sources managed by clang-format / clang-tidy.
set(ALL_CPP_SOURCES ${PLIC_SOURCES})
set(ALL_CPP_HEADERS
  src/ast.h src/codegen.h src/diag.h src/embedded_runtime.h src/explain.h
  src/hir.h src/irgen.h src/lexer.h src/parser.h src/preprocessor.h
  src/sema.h src/target.h src/token.h src/types.h)
set(ALL_C_SOURCES ${RT_SRCS})
set(ALL_C_HEADERS
  runtime/pli_rt.h runtime/pli_rt_state.h runtime/pli_rt_abi.def
  runtime/sync/plic_thread.h)
# clang-format scope: src/ only (ADR-042: src/ is clang-format-normalized).
# The C runtime follows its own brace-less style; clang-tidy covers C++ only.
set(ALL_FORMAT_SOURCES ${ALL_CPP_SOURCES} ${ALL_CPP_HEADERS})

# fmt: format all sources in place.
add_custom_target(fmt
  COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_FORMAT_SOURCES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "clang-format: formatting sources"
  VERBATIM)

# fmt-check: verify formatting without modifying files.
add_custom_target(fmt-check
  COMMAND ${CLANG_FORMAT_BIN} --dry-run --Werror ${ALL_FORMAT_SOURCES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "clang-format: checking formatting"
  VERBATIM)

# tidy: run clang-tidy over the C++ sources (needs compile_commands.json from a build).
add_custom_target(tidy
  COMMAND ${CLANG_TIDY_BIN} -p ${CMAKE_BINARY_DIR} --quiet ${ALL_CPP_SOURCES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "clang-tidy: analyzing C++ sources"
  VERBATIM)
add_dependencies(tidy plic)

# werror: rebuild with -DPLIC_WERROR=ON to enforce warnings-as-errors.
add_custom_target(werror
  COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR} -B ${CMAKE_BINARY_DIR} -DPLIC_WERROR=ON
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target plic
  COMMENT "Building plic with -Werror (PLIC_WERROR=ON)"
  VERBATIM)

# scan: run the clang static analyzer (scan-build) over the plic build.
add_custom_target(scan
  COMMAND ${SCAN_BUILD_BIN}
    --use-c++=${CMAKE_CXX_COMPILER}
    --use-cc=${CMAKE_C_COMPILER}
    ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target plic
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "scan-build: static analyzer over plic"
  VERBATIM)

# check: full analysis gate — format check + static analysis + warnings-as-errors.
# Runs each sub-target in order via cmake --build so the sequence is deterministic.
add_custom_target(check
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target fmt-check
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tidy
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target werror
  COMMENT "Running analysis gate (fmt-check + tidy + werror)"
  VERBATIM)

# size-report: report the stripped size of the plic binary.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  add_custom_target(size-report
    COMMAND strip -o ${CMAKE_BINARY_DIR}/plic.strip $<TARGET_FILE:plic>
    COMMAND stat -f%z ${CMAKE_BINARY_DIR}/plic.strip
    COMMENT "Stripped size of plic (target: < 100 MB, hard cap 150 MB)")
else()
  add_custom_target(size-report
    COMMAND strip $<TARGET_FILE:plic> -o ${CMAKE_BINARY_DIR}/plic.strip
    COMMAND stat -c%s ${CMAKE_BINARY_DIR}/plic.strip
    COMMENT "Stripped size of plic (target: < 100 MB, hard cap 150 MB)")
endif()
