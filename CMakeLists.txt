# plic — PL/I -> LLVM compiler (CMake build, M1: LLVM C++ API).
#
# The Makefile remains the bootstrap build; this CMake build links the LLVM C++
# API (ADR-002). Requires an LLVM installation with development libraries, e.g.
#   brew install llvm cmake
#   cmake -B build/cmake -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
#   cmake --build build/cmake
cmake_minimum_required(VERSION 3.20)
project(plic CXX C)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "plic requires LLVM 18 or newer")
endif()
find_package(Clang REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})

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
  find_package(lld QUIET)
  if(lld_INCLUDE_DIRS)
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
set(LLD_LIB_DIR /opt/homebrew/opt/lld/lib)
set(LLD_ELF_LIB ${LLD_LIB_DIR}/liblldELF.dylib)
set(LLD_MACHO_LIB ${LLD_LIB_DIR}/liblldMachO.dylib)
set(LLD_COFF_LIB ${LLD_LIB_DIR}/liblldCOFF.dylib)
set(LLD_COMMON_LIB ${LLD_LIB_DIR}/liblldCommon.dylib)

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
target_compile_definitions(plic PRIVATE
  PLIC_RUNTIME_LIB="$<TARGET_FILE:libpli>"
  PLIC_INSTALL_RUNTIME_LIB="${CMAKE_INSTALL_FULL_LIBDIR}/libpli.a"
  PLIC_CLANG="${LLVM_TOOLS_BINARY_DIR}/clang"
  PLIC_VERSION="${PLIC_VERSION}"
  PLIC_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
  PLIC_HAVE_LLD=${PLIC_HAVE_LLD}
  PLIC_TARGETS_TO_BUILD="host")

# Link LLVM components needed for in-process codegen + linking.
# Core/IR
llvm_map_components_to_libnames(llvm_libs_core
  Core IRReader Support BitReader BitWriter)
# MC/Target (host arch only for milestone)
llvm_map_components_to_libnames(llvm_libs_mc
  MC MCParser Object BinaryFormat TargetParser AsmParser AsmPrinter
  CodeGen SelectionDAG GlobalISel
  X86CodeGen X86AsmParser X86Desc X86Info
  AArch64CodeGen AArch64AsmParser AArch64Desc AArch64Info
  Passes LTO)
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
if(NOT APPLE)
  target_link_options(plic PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Get the default target triple from LLVM for consistent bitcode generation
# Use macosx14.0: macOS 15+ emits SME features (zcm/zcz) in function
# attributes that LLVM 23 does not support at codegen time ("Unsupported
# stack probing method"). macOS 14 and earlier do not emit them.
set(PLIC_TARGET_TRIPLE "arm64-apple-macosx14.0")
message(STATUS "Target triple for bitcode: ${PLIC_TARGET_TRIPLE}")

# libpli runtime (C11).
add_library(libpli STATIC ${RT_SRCS})
set_target_properties(libpli PROPERTIES OUTPUT_NAME pli)

# Embedded runtime (Phase 2): per-unit bitcode, linked bundle, byte array.
set(RT_BCS "")
foreach(src ${RT_SRCS})
  get_filename_component(base ${src} NAME_WE)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bc/${base}.bc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bc
    COMMAND ${CMAKE_C_COMPILER} -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
            -I${CMAKE_SOURCE_DIR}/runtime -I${CMAKE_SOURCE_DIR}/runtime/sync
            -O2
            -ffunction-sections -fdata-sections
            -target ${PLIC_TARGET_TRIPLE}
            -o ${CMAKE_BINARY_DIR}/bc/${base}.bc
    DEPENDS ${CMAKE_SOURCE_DIR}/${src}
    COMMENT "bitcode: ${base}")
  list(APPEND RT_BCS ${CMAKE_BINARY_DIR}/bc/${base}.bc)
endforeach()
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/libpli.bc
  COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${RT_BCS} -o ${CMAKE_BINARY_DIR}/libpli.bc
  DEPENDS ${RT_BCS})
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/embedded_runtime.inc
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
          ${CMAKE_BINARY_DIR}/libpli.bc ${CMAKE_BINARY_DIR}/embedded_runtime.inc
  DEPENDS ${CMAKE_BINARY_DIR}/libpli.bc ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
  COMMENT "embedding libpli.bc")
add_custom_target(embedded_runtime_inc DEPENDS ${CMAKE_BINARY_DIR}/embedded_runtime.inc)
add_dependencies(plic embedded_runtime_inc)

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
