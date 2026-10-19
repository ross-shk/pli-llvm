# pli — PL/I -> LLVM compiler (CMake + Ninja build, ADR-002: LLVM C++ API).
#
# CMake + Ninja is the canonical build. The Makefile is a thin convenience
# wrapper that delegates to this CMake build. Requires an LLVM installation
# with development libraries, e.g.:
#   brew install llvm lld cmake ninja
#   cmake -G Ninja -S . -B build/cmake -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
#   cmake --build build/cmake -j$(nproc)
# Quality gate: cmake --build build/cmake --target check
cmake_minimum_required(VERSION 3.20)
project(pli CXX C)

include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(PLI_WERROR "Treat compiler warnings as errors" OFF)

# Hermetic static toolchain (scripts/fetch_llvm_static.sh + llvm_static.pins):
# a source-built LLVM/clang/lld install with static archives only. Empty keeps
# the distro LLVM (fast dev iteration); standalone releases always set it.
set(PLI_LLVM_STATIC_PREFIX "" CACHE PATH "Source-built static LLVM install prefix")
if(PLI_LLVM_STATIC_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${PLI_LLVM_STATIC_PREFIX}")
endif()

find_package(LLVM REQUIRED CONFIG)
# Prebuilt LLVM 23 on Windows was compiled with ZLIB, zstd, and LibXml2 support
# but doesn't ship those libraries. Provide them as linkable targets.
# Point the *_ROOT hints below at a local zlib/zstd build or a vcpkg install:
# real static libraries are REQUIRED at link time. The static lld/LLVM
# archives reference zlib/zstd symbols directly (adler32, compress2,
# ZSTD_compress2, ...), so the empty INTERFACE fallbacks below only satisfy
# configure — without real libs the link fails with undefined symbols.
# LibXml2 is the exception: manifest merging is covered by in-tree stubs
# (runtime/libxml2_stubs.c, added to pli sources below).
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # Drop the old dev-machine hint var (replaced by the *_ROOT vars below).
  unset(PLI_LLVM_DEPS_DIR CACHE)
  set(PLI_ZLIB_ROOT "" CACHE PATH "Optional zlib install prefix (contains lib/)")
  set(PLI_ZSTD_ROOT "" CACHE PATH "Optional zstd install prefix (contains lib/)")
  set(PLI_VCPKG_ROOT "C:/vcpkg/installed/x64-windows" CACHE PATH "Optional vcpkg triplet root")
  # ZLIB (prefer static: the import lib would add a zlib.dll startup dependency)
  if(NOT TARGET ZLIB::ZLIB)
    find_file(ZLIB_LIB NAMES zlibstatic.lib zlib.lib
      HINTS ${PLI_ZLIB_ROOT}/lib ${PLI_VCPKG_ROOT}/lib
      NO_DEFAULT_PATH)
    if(ZLIB_LIB)
      add_library(ZLIB::ZLIB STATIC IMPORTED)
      set_target_properties(ZLIB::ZLIB PROPERTIES IMPORTED_LOCATION "${ZLIB_LIB}")
      message(STATUS "ZLIB: ${ZLIB_LIB}")
    else()
      add_library(ZLIB::ZLIB INTERFACE IMPORTED)
      message(WARNING "ZLIB not found (set PLI_ZLIB_ROOT or install via vcpkg) — link will fail with undefined zlib symbols")
    endif()
  endif()
  # zstd
  if(NOT TARGET zstd::libzstd_static)
    find_file(ZSTD_LIB NAMES libzstd_static.lib zstd.lib
      HINTS ${PLI_ZSTD_ROOT}/lib ${PLI_VCPKG_ROOT}/lib
      NO_DEFAULT_PATH)
    if(ZSTD_LIB)
      add_library(zstd::libzstd_static STATIC IMPORTED)
      set_target_properties(zstd::libzstd_static PROPERTIES IMPORTED_LOCATION "${ZSTD_LIB}")
      message(STATUS "zstd: ${ZSTD_LIB}")
    else()
      add_library(zstd::libzstd_static INTERFACE IMPORTED)
      message(WARNING "zstd not found (set PLI_ZSTD_ROOT or install via vcpkg) — link will fail with undefined zstd symbols")
    endif()
  endif()
  # LibXml2: the prebuilt LLVM expects libxml2 (WindowsManifest merging) but
  # ships no binaries, and the old import-lib stub only covered 8 of the 16
  # symbols LLVM 23 needs (besides requiring a nonexistent libxml2.dll at
  # startup). Use an empty target and provide static abort-stubs in-tree
  # (runtime/libxml2_stubs.c, added to pli sources below) instead.
  if(NOT TARGET LibXml2::LibXml2)
    add_library(LibXml2::LibXml2 INTERFACE IMPORTED)
    message(STATUS "LibXml2: in-tree stubs (runtime/libxml2_stubs.c)")
  endif()
endif()
if(LLVM_PACKAGE_VERSION VERSION_LESS 23)
  message(FATAL_ERROR "pli requires LLVM 23 or newer (found ${LLVM_PACKAGE_VERSION})")
endif()
if(PLI_LLVM_STATIC_PREFIX)
  # The stash pins one LLVM per release; a stale prefix fails loudly here
  # instead of mixing headers/libs across versions.
  file(STRINGS "${CMAKE_SOURCE_DIR}/scripts/llvm_static.pins" _pins_line REGEX "^LLVM_VER=")
  string(REGEX MATCH "[0-9.]+" _pins_ver "${_pins_line}")
  if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL _pins_ver)
    message(FATAL_ERROR "static LLVM is ${LLVM_PACKAGE_VERSION} but llvm_static.pins requires ${_pins_ver} — re-run fetch_llvm_static.sh")
  endif()
  # Pure-static guard: never mix shared libLLVM with static component
  # archives (duplicated LLVM globals crash flakily at exit on ARM64).
  if(LLVM_LINK_LLVM_DYLIB)
    message(FATAL_ERROR "PLI_LLVM_STATIC_PREFIX points at a shared-LLVM build — refusing to mix")
  endif()
endif()
find_package(Clang REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

if(PLI_ENABLE_MLIR)
  find_package(MLIR REQUIRED CONFIG)
  message(STATUS "Using MLIR from ${MLIR_DIR}")
  # Verify LLVM and MLIR share the same installation prefix and version.
  get_filename_component(_mlir_lib_prefix "${MLIR_DIR}" DIRECTORY)
  get_filename_component(_llvm_lib_prefix "${LLVM_DIR}" DIRECTORY)
  if(NOT "${_mlir_lib_prefix}" STREQUAL "${_llvm_lib_prefix}")
    message(WARNING "MLIR (${MLIR_DIR}) does not match LLVM (${LLVM_DIR}) — versions or prefixes differ")
  endif()
  list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
  include(AddMLIR)
  include(TableGen)
  add_definitions(${MLIR_DEFINITIONS})
endif()

add_definitions(${LLVM_DEFINITIONS})
# Expose implicit C/C++ std-lib include paths in compile_commands.json so that
# clang-tidy (from the LLVM install) can resolve <cstdio>, <vector>, etc.
# when CMAKE_CXX_COMPILER is a different clang (e.g. Apple clang on macOS).
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})

set(PLI_SOURCES
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
  src/hostcrt.cpp
  src/pgo.cpp
)

# P2: MLIR canary dialect sources (compiled only with PLI_ENABLE_MLIR=ON).
# The canary is a synthetic operation that exercises the full MLIR pipeline
# without lowering any W1-W5 PL/I operation.
if(PLI_ENABLE_MLIR)
  set(LLVM_TARGET_DEFINITIONS src/mlir/pli_dialect.td)
  mlir_tablegen(PliDialect.h.inc -gen-dialect-decls -dialect=pli
    EXTRA_INCLUDES ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  mlir_tablegen(PliDialect.cpp.inc -gen-dialect-defs -dialect=pli
    EXTRA_INCLUDES ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  mlir_tablegen(PliOps.h.inc -gen-op-decls -dialect=pli
    EXTRA_INCLUDES ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  mlir_tablegen(PliOps.cpp.inc -gen-op-defs -dialect=pli
    EXTRA_INCLUDES ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  add_library(pli_mlir STATIC
    src/mlir/PliDialect.cpp
    src/mlir/PliLowering.cpp
    src/mlir/PliMlirCanary.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/PliDialect.h.inc
    ${CMAKE_CURRENT_BINARY_DIR}/PliOps.h.inc
    ${CMAKE_CURRENT_BINARY_DIR}/PliDialect.cpp.inc
    ${CMAKE_CURRENT_BINARY_DIR}/PliOps.cpp.inc)
   target_include_directories(pli_mlir PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MLIR_INCLUDE_DIRS})
  # MLIR shared library provides all needed MLIR symbols (MLIR_LINK_MLIR_DYLIB=1).
  target_link_libraries(pli_mlir PUBLIC MLIR)
  target_compile_features(pli_mlir PRIVATE cxx_std_20)
endif()

# Split runtime: portable TUs (pure standard C, no platform branches) embed as
# pli_runtime.bc; the platform TU (host calls behind pli_rt_plat.h) embeds as
# pli_rtplat.bc and is swappable via --rt-platform. RT_SRCS stays the union
# for the quality-gate source lists below.
set(RT_PORTABLE_SRCS
  runtime/rt_core.c runtime/rt_stream.c runtime/rt_string.c
  runtime/rt_math.c runtime/rt_mathport.c runtime/rt_cond.c
  runtime/rt_storage.c runtime/rt_get.c runtime/rt_file.c
  runtime/rt_edit.c runtime/rt_task.c)
set(RT_PLATFORM_SRCS
  runtime/rt_plat_default.c)
set(RT_SRCS ${RT_PORTABLE_SRCS} ${RT_PLATFORM_SRCS})
# Bitcode embeds must rebuild when any runtime header changes (the .c TUs
# all include pli_rt.h -> pli_rt_state.h -> pli_rt_plat.h; without this a
# header-only fix leaves stale .bc embedded in the binary).
set(RT_HEADERS
  ${CMAKE_SOURCE_DIR}/runtime/pli_rt.h
  ${CMAKE_SOURCE_DIR}/runtime/pli_rt_state.h
  ${CMAKE_SOURCE_DIR}/runtime/pli_rt_plat.h
  ${CMAKE_SOURCE_DIR}/runtime/pli_rt_abi.def
  ${CMAKE_SOURCE_DIR}/runtime/pli_wide128.h)

# lld detection (ADR-225): in-process lld is the only link; missing lld is a
# hard configure error. Prerequisite: stock brew `llvm` ships no lld; fail
# configure with install hint.
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
find_library(LLD_MINGW_LIB NAMES lldMinGW)
find_library(LLD_COMMON_LIB NAMES lldCommon)
# ADR-225: in-process lld is the ONLY link — every lld driver library is a
# hard requirement (the old soft gate + clang fallback are gone).
# ADR-227 adds the MinGW driver (vendored-mingw sysroot links need it).
foreach(_lld_lib LLD_ELF_LIB LLD_MACHO_LIB LLD_COFF_LIB LLD_MINGW_LIB LLD_COMMON_LIB)
  if(NOT ${_lld_lib} OR "${${_lld_lib}}" MATCHES "-NOTFOUND$")
    message(FATAL_ERROR "in-process link requires ${_lld_lib} (install lld for your LLVM)")
  endif()
endforeach()
set(PLI_HAVE_LLD 1)
set(LLD_LIBRARIES ${LLD_ELF_LIB} ${LLD_MACHO_LIB} ${LLD_COFF_LIB} ${LLD_MINGW_LIB} ${LLD_COMMON_LIB})
message(STATUS "lld libraries: ${LLD_LIBRARIES}")

add_library(pli_rules STATIC ${CMAKE_BINARY_DIR}/rules.cpp)
target_include_directories(pli_rules PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Embedded version for `pli version` (Go/Zig style build info).
execute_process(COMMAND git describe --tags --always --dirty
  OUTPUT_VARIABLE PLI_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PLI_VERSION)
  set(PLI_VERSION "unknown")
endif()

add_executable(pli ${PLI_SOURCES})
target_include_directories(pli PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})
# LLVM/Clang headers as SYSTEM to suppress their warnings under -Werror.
target_include_directories(pli SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

# Get the default target triple from LLVM for consistent bitcode generation.
# Use clang (not CMAKE_C_COMPILER) for triple detection and bitcode generation
# since GCC doesn't support -print-effective-triple or -emit-llvm -target.
set(PLI_TARGET_TRIPLE "" CACHE STRING "Target triple for runtime bitcode generation")
if(NOT PLI_TARGET_TRIPLE)
  execute_process(
    COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -print-effective-triple
    OUTPUT_VARIABLE PLI_HOST_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(PLI_TARGET_TRIPLE ${PLI_HOST_TRIPLE})
endif()
message(STATUS "Target triple for bitcode: ${PLI_TARGET_TRIPLE}")

# Apple host bitcode: a from-source clang ships no baked-in macOS SDK, so the
# host-triple runtime compile must be given a sysroot explicitly or it cannot
# find <stdio.h>. Resolve the active SDK once via xcrun (dev + CI runners).
set(PLI_HOST_SYSROOT_FLAG "")
if(APPLE)
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE PLI_HOST_SDK
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(PLI_HOST_SDK)
    set(PLI_HOST_SYSROOT_FLAG -isysroot ${PLI_HOST_SDK})
    message(STATUS "Host bitcode sysroot: ${PLI_HOST_SDK}")
  endif()
endif()

# Toolchain prefix baked in as a data-file fallback (ADR-225): the profile
# runtime / builtins probes try exe-relative first (release layout), then
# $PLI_TOOLCHAIN_PREFIX, then this build-time prefix (dev machines).
if(PLI_LLVM_STATIC_PREFIX)
  set(PLI_TOOLCHAIN_PREFIX "${PLI_LLVM_STATIC_PREFIX}")
else()
  get_filename_component(PLI_TOOLCHAIN_PREFIX "${LLVM_TOOLS_BINARY_DIR}" DIRECTORY)
endif()
target_compile_definitions(pli PRIVATE
  PLI_TOOLCHAIN_PREFIX="${PLI_TOOLCHAIN_PREFIX}"
  PLI_VERSION="${PLI_VERSION}"
  PLI_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
  PLI_HAVE_LLD=${PLI_HAVE_LLD}
  PLI_TARGETS_TO_BUILD="host"
  PLI_TARGET_TRIPLE="${PLI_TARGET_TRIPLE}"
  PLI_HAVE_LINUX_X86_64_RUNTIME=1
  PLI_HAVE_LINUX_AARCH64_RUNTIME=1
  PLI_HAVE_WINDOWS_X86_64_RUNTIME=1
  PLI_HAVE_WINDOWS_AARCH64_RUNTIME=1)

# P2: wire MLIR canary library into pli when MLIR is enabled.
if(PLI_ENABLE_MLIR)
  target_compile_definitions(pli PRIVATE PLI_ENABLE_MLIR)
  target_link_libraries(pli PRIVATE pli_mlir)
  # pli_mlir is PUBLIC-linked, so MLIR_INCLUDE_DIRS propagates transitively.
endif()

# Link LLVM components needed for in-process codegen + linking.
# Shared-LLVM builds (Debian LLVM 22 sets LLVM_LINK_LLVM_DYLIB): link the
# shared libLLVM only. Mixing static component archives with the shared lib
# (pulled in by the Clang targets below) duplicates LLVM globals and crashes
# flakily at exit after O2 codegen on ARM64 (free(): invalid pointer).
# Static builds, Apple, and the hermetic static prefix (pure-static by
# construction: no libLLVM.so exists there) keep the component archives.
# Static zlib/zstd ride in transitively: LLVMConfig re-finds ZLIB::ZLIB and
# zstd::libzstd_static from the prefix via CMAKE_PREFIX_PATH.
if(LLVM_LINK_LLVM_DYLIB AND NOT APPLE AND NOT PLI_LLVM_STATIC_PREFIX)
  set(llvm_libs_core LLVM z zstd)
  set(llvm_libs_mc "")
else()
  # Core/IR (superset of the old `core irreader support linker` set).
  # ProfileData: InstrProfReader for in-process --fprofile-use (ADR-225).
  llvm_map_components_to_libnames(llvm_libs_core
    Core IRReader Support BitReader BitWriter Linker ProfileData)
  # MC/Target: the distro prefix builds all targets (initializeAllTargets()
  # needs them all); the hermetic static prefix only builds X86+AArch64, so
  # map just those — other --triple values already diagnose "no target" at
  # TargetMachine creation instead of failing configure.
  if(PLI_LLVM_STATIC_PREFIX)
    llvm_map_components_to_libnames(llvm_libs_mc
      MC MCParser MCDisassembler Object BinaryFormat TargetParser AsmParser AsmPrinter
      CodeGen SelectionDAG GlobalISel Option
      X86CodeGen X86AsmParser X86Disassembler X86Desc X86Info
      AArch64CodeGen AArch64AsmParser AArch64Disassembler AArch64Desc AArch64Info
      WindowsManifest WindowsDriver LibDriver
      Passes LTO DTLTO)
  else()
    llvm_map_components_to_libnames(llvm_libs_mc
      MC MCParser MCDisassembler Object BinaryFormat TargetParser AsmParser AsmPrinter
      CodeGen SelectionDAG GlobalISel Option
      X86CodeGen X86AsmParser X86Disassembler X86Desc X86Info
      AArch64CodeGen AArch64AsmParser AArch64Disassembler AArch64Desc AArch64Info
      ARM ARMAsmParser ARMCodeGen ARMDesc ARMDisassembler ARMInfo
      BPF BPFAsmParser BPFCodeGen BPFDesc BPFDisassembler BPFInfo
      WebAssembly WebAssemblyAsmParser WebAssemblyCodeGen WebAssemblyDesc
      WebAssemblyDisassembler WebAssemblyInfo
      NVPTX NVPTXCodeGen NVPTXDesc NVPTXInfo
      RISCV RISCVAsmParser RISCVCodeGen RISCVDesc RISCVInfo
      WindowsManifest WindowsDriver LibDriver
      Passes LTO DTLTO)
  endif()
endif()
# lld
llvm_map_components_to_libnames(llvm_libs_lld
  lldELF lldMachO lldCOFF lldMinGW lldCommon)

# Use dynamically found lld libraries if available
if(LLD_LIBRARIES AND NOT "${LLD_LIBRARIES}" STREQUAL "lldELF lldMachO lldCOFF lldMinGW lldCommon lldDriver")
  set(llvm_libs_lld ${LLD_LIBRARIES})
endif()

# Use Clang imported targets directly
target_link_libraries(pli PRIVATE pli_rules
  ${llvm_libs_lld} ${llvm_libs_mc} ${llvm_libs_core}
  clangFrontend clangParse clangLex clangBasic clangAST)

# P2: link MLIR targets when MLIR is enabled.
if(PLI_ENABLE_MLIR)
  target_link_libraries(pli PRIVATE pli_mlir)
endif()

# compiler-rt provides __divti3 (128-bit division) needed for i128 arithmetic
# on Windows where the CRT doesn't include it.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  string(REGEX REPLACE "^([0-9]+)\\..*" "\\1" _LLVM_MAJOR "${LLVM_PACKAGE_VERSION}")
  # Prebuilt LLVM installs builtins under lib/clang/<ver>/lib/windows; the
  # standalone compiler-rt build in the hermetic stash installs under lib/windows.
  find_file(CLANG_RT_BUILTINS
    NAMES clang_rt.builtins-x86_64.lib clang_rt.builtins-aarch64.lib
    HINTS ${CMAKE_PREFIX_PATH}/lib/clang/${_LLVM_MAJOR}/lib/windows
          ${CMAKE_PREFIX_PATH}/lib/windows)
  if(CLANG_RT_BUILTINS)
    target_link_libraries(pli PRIVATE ${CLANG_RT_BUILTINS})
    message(STATUS "compiler-rt builtins: ${CLANG_RT_BUILTINS}")
  else()
    message(WARNING "compiler-rt builtins not found (clang_rt.builtins) — 128-bit division may fail to link")
  endif()
endif()

# clang-cl + lld-link: CMake doesn't auto-detect MSVC C++ stdlib (IMPLICIT_LINK_LIBRARIES is empty).
# The prebuilt LLVM 23 libraries use /MT (static CRT), so pli must use /MT too to avoid
# RuntimeLibrary mismatch (MT_StaticRelease vs MD_DynamicRelease).
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_property(TARGET pli PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  # CMake already invokes lld-link.exe directly for the clang-cl+Ninja link
  # (see CMAKE_LINKER); passing -fuse-ld=lld here would forward a driver-only
  # flag to lld-link ("ignoring unknown argument"). User programs link
  # in-process through lld (ADR-225), never through a clang driver.
  # /DEFAULTLIB:libcmt.lib pulls in the MT-variant CRT and C++ stdlib objects
  # (provides __std_remove_8, __std_replace_8, ___chkstk_ms, etc.) without a
  # RuntimeLibrary mismatch. MSVC libs must come after LLVM/lld libs on the link line.
  target_link_options(pli PRIVATE /DEFAULTLIB:libcmt.lib)
  target_link_libraries(pli PRIVATE
    kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib
    advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib)
  # MSVC STL helper stubs for symbols missing from VS 2022 14.39 local install
  # but expected by the prebuilt LLVM 23 Windows libraries (__std_remove_8, etc.)
  # Compiled from runtime/stl_stubs.c as part of the build. The hermetic stash
  # is compiled with the runner's own toolset, so its libcpmt already exports
  # these helpers; adding the stubs there produces duplicate symbols.
  target_sources(pli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/libxml2_stubs.c)
  if(NOT PLI_LLVM_STATIC_PREFIX)
    target_sources(pli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime/stl_stubs.c)
  endif()
  # Silence expected clang warnings for the __std_*/xml* helper stubs
  # (reserved double-underscore names, pre-C99 declaration style).
  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/stl_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/libxml2_stubs.c PROPERTIES
    COMPILE_OPTIONS "-Wno-reserved-identifier;-Wno-missing-prototypes;-Wno-unsafe-buffer-usage;-Wno-unreachable-code-return;-Wno-declaration-after-statement;-Wno-unused-parameter")
  message(STATUS "MSVC STL helper stubs: runtime/stl_stubs.c")
  message(STATUS "Using /MT (static CRT) to match prebuilt LLVM libraries")
endif()

# Force-include lld + host target so their symbols survive static dead-stripping.
# Homebrew lld only provides dynamic libraries, so use those directly.
# (No whole-archive for lldMinGW: its static initializers crash pli at
# startup; the direct lld::mingw::link reference pulls what the link needs.)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_options(pli PRIVATE
    -Wl,-force_load,${LLD_ELF_LIB}
    -Wl,-force_load,${LLD_MACHO_LIB}
    -Wl,-force_load,${LLD_COFF_LIB})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_link_options(pli PRIVATE
    /WHOLEARCHIVE:${LLD_ELF_LIB}
    /WHOLEARCHIVE:${LLD_MACHO_LIB}
    /WHOLEARCHIVE:${LLD_COFF_LIB})
else()
  target_link_options(pli PRIVATE
    -Wl,--whole-archive
    ${LLD_ELF_LIB}
    ${LLD_MACHO_LIB}
    ${LLD_COFF_LIB}
    -Wl,--no-whole-archive)
endif()

# Static C++ runtime for portability.
# Windows: pli itself uses /MT (MSVC_RUNTIME_LIBRARY=MultiThreaded, set above)
# to match the prebuilt LLVM 23 libs; user programs link against the dynamic
# CRT (see src/main.cpp linkCrtFlag).
# CRT selection is a per-target compile property, not a link option
# (passing /MD//MT to lld-link warns LNK4044), so no link options here.
if(APPLE)
  # Nothing additional needed on macOS.
elseif(WIN32)
  # No link options needed (see above).
else()
  target_link_options(pli PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Cross-compilation bitcode generation (Phase 2+): requires sysroots.
option(PLI_CROSS_BITCODE "Generate bitcode for cross-targets (needs sysroots)" OFF)

# P2: optional MLIR support for the canary dialect spike. Off by default;
# an MLIR-enabled build must link against the matching LLVM/MLIR package.
option(PLI_ENABLE_MLIR "Build the experimental MLIR canary dialect (P2)" OFF)

# pli runtime (C11). One source per section so the link drops unused pieces
# (ADR-079); the sources feed only the embedded bitcode below — no archive or
# runtime.bc file is built or installed (single-binary distribution). The
# --runtime flag still accepts a user-supplied external archive,
# and --no-bitcode-runtime materialises the link input from the embedded blob.

# Warnings: -Wall -Wextra on the C++ driver (pli_runtime is C11 with its own style).
# Use Clang compiler ID check FIRST: clang-cl defines the Clang ID but also
# sets MSVC=TRUE in CMake. We want -Wall -Wextra for clang-cl, not /W3.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(pli PRIVATE -Wall -Wextra)
  if(PLI_WERROR)
    target_compile_options(pli PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(pli PRIVATE /W3)
  if(PLI_WERROR)
    target_compile_options(pli PRIVATE /WX)
  endif()
endif()

# Match LLVM's RTTI setting. The hermetic static prefix is built with
# LLVM_ENABLE_RTTI=OFF, so those archives carry no typeinfo for LLVM's
# polymorphic classes; compiling the driver with RTTI on makes the linker
# demand symbols that were never emitted.
if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pli PRIVATE /GR-)
  else()
    target_compile_options(pli PRIVATE -fno-rtti)
  endif()
endif()

# Multi-target embedded runtime: generate bitcode for all supported targets.
if(PLI_CROSS_BITCODE)
  set(PLI_TARGETS "x86_64-unknown-linux-gnu;aarch64-unknown-linux-gnu;x86_64-pc-windows-gnu;aarch64-pc-windows-gnu")

  # Per-target bitcode generation and embedding: portable sources link into
  # pli_runtime.bc, the platform TU into pli_rtplat.bc (swappable at link).
  foreach(target ${PLI_TARGETS})
    string(REPLACE "-" "_" target_suffix "_${target}")
    set(TARGET_BC_DIR ${CMAKE_BINARY_DIR}/bc/${target})
    set(TARGET_RT_BCS "")
    foreach(src ${RT_PORTABLE_SRCS})
      get_filename_component(base ${src} NAME_WE)
      add_custom_command(
        OUTPUT ${TARGET_BC_DIR}/${base}.bc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TARGET_BC_DIR}
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
                -DPLI_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
                -I${CMAKE_SOURCE_DIR}/runtime
                -O2
                -ffunction-sections -fdata-sections
                $<$<OR:$<STREQUAL:${target},x86_64-unknown-linux-gnu>,$<STREQUAL:${target},aarch64-unknown-linux-gnu>>:-fPIC>
                -target ${target}
                -o ${TARGET_BC_DIR}/${base}.bc
        DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${RT_HEADERS} ${RT_HEADERS}
        COMMENT "bitcode: ${base} for ${target}"
        VERBATIM)
      list(APPEND TARGET_RT_BCS ${TARGET_BC_DIR}/${base}.bc)
    endforeach()
    set(TARGET_PLAT_BCS "")
    foreach(src ${RT_PLATFORM_SRCS})
      get_filename_component(base ${src} NAME_WE)
      add_custom_command(
        OUTPUT ${TARGET_BC_DIR}/${base}.bc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TARGET_BC_DIR}
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
                -DPLI_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
                -I${CMAKE_SOURCE_DIR}/runtime
                -O2
                -ffunction-sections -fdata-sections
                $<$<OR:$<STREQUAL:${target},x86_64-unknown-linux-gnu>,$<STREQUAL:${target},aarch64-unknown-linux-gnu>>:-fPIC>
                -target ${target}
                -o ${TARGET_BC_DIR}/${base}.bc
        DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${RT_HEADERS} ${RT_HEADERS}
        COMMENT "bitcode: ${base} for ${target}"
        VERBATIM)
      list(APPEND TARGET_PLAT_BCS ${TARGET_BC_DIR}/${base}.bc)
    endforeach()
    add_custom_command(
      OUTPUT ${TARGET_BC_DIR}/pli_runtime.bc
      COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${TARGET_RT_BCS} -o ${TARGET_BC_DIR}/pli_runtime.bc
      DEPENDS ${TARGET_RT_BCS})
    add_custom_command(
      OUTPUT ${TARGET_BC_DIR}/pli_rtplat.bc
      COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${TARGET_PLAT_BCS} -o ${TARGET_BC_DIR}/pli_rtplat.bc
      DEPENDS ${TARGET_PLAT_BCS})
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
              ${TARGET_BC_DIR}/pli_runtime.bc ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
              pli_embedded_runtime_bc ${target_suffix}
      DEPENDS ${TARGET_BC_DIR}/pli_runtime.bc ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
      COMMENT "embedding pli_runtime.bc for ${target}")
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/embedded_rtplat${target_suffix}.inc
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
              ${TARGET_BC_DIR}/pli_rtplat.bc ${CMAKE_BINARY_DIR}/embedded_rtplat${target_suffix}.inc
              pli_embedded_rtplat_bc ${target_suffix}
      DEPENDS ${TARGET_BC_DIR}/pli_rtplat.bc ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
      COMMENT "embedding pli_rtplat.bc for ${target}")
    add_custom_target(embedded_runtime_inc${target_suffix} DEPENDS ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc ${CMAKE_BINARY_DIR}/embedded_rtplat${target_suffix}.inc)
    add_dependencies(pli embedded_runtime_inc${target_suffix})
  endforeach()
else()
  # Define empty per-target blobs for non-cross builds (compiler still works)
  # Target suffixes must match PLI_TARGETS naming convention (full triple)
  foreach(target IN ITEMS x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-pc-windows-gnu aarch64-pc-windows-gnu)
    string(REPLACE "-" "_" target_suffix "_${target}")
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
              /dev/null ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc
              pli_embedded_runtime_bc ${target_suffix}
      DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
      COMMENT "stub embedding for ${target}")
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/embedded_rtplat${target_suffix}.inc
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
              /dev/null ${CMAKE_BINARY_DIR}/embedded_rtplat${target_suffix}.inc
              pli_embedded_rtplat_bc ${target_suffix}
      DEPENDS ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
      COMMENT "stub platform embedding for ${target}")
    add_custom_target(embedded_runtime_inc${target_suffix} DEPENDS ${CMAKE_BINARY_DIR}/embedded_runtime${target_suffix}.inc ${CMAKE_BINARY_DIR}/embedded_rtplat${target_suffix}.inc)
    add_dependencies(pli embedded_runtime_inc${target_suffix})
  endforeach()
endif()

# Default target (host) bitcode for backward compatibility: portable sources
# link into pli_runtime.bc, the platform TU into pli_rtplat.bc.
set(DEFAULT_BC_DIR ${CMAKE_BINARY_DIR}/bc/${PLI_TARGET_TRIPLE})
set(DEFAULT_RT_BCS "")
foreach(src ${RT_PORTABLE_SRCS})
  get_filename_component(base ${src} NAME_WE)
  add_custom_command(
    OUTPUT ${DEFAULT_BC_DIR}/${base}.bc
      COMMAND ${CMAKE_COMMAND} -E make_directory ${DEFAULT_BC_DIR}
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
            -DPLI_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
            -I${CMAKE_SOURCE_DIR}/runtime
            -O2
            -ffunction-sections -fdata-sections
            $<$<PLATFORM_ID:Linux>:-fPIC>
            ${PLI_HOST_SYSROOT_FLAG}
            -target ${PLI_TARGET_TRIPLE}
            -o ${DEFAULT_BC_DIR}/${base}.bc
    DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${RT_HEADERS}
    COMMENT "bitcode: ${base} for ${PLI_TARGET_TRIPLE}"
    VERBATIM)
  list(APPEND DEFAULT_RT_BCS ${DEFAULT_BC_DIR}/${base}.bc)
endforeach()
set(DEFAULT_PLAT_BCS "")
foreach(src ${RT_PLATFORM_SRCS})
  get_filename_component(base ${src} NAME_WE)
  add_custom_command(
    OUTPUT ${DEFAULT_BC_DIR}/${base}.bc
      COMMAND ${CMAKE_COMMAND} -E make_directory ${DEFAULT_BC_DIR}
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
            -DPLI_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
            -I${CMAKE_SOURCE_DIR}/runtime
            -O2
            -ffunction-sections -fdata-sections
            $<$<PLATFORM_ID:Linux>:-fPIC>
            ${PLI_HOST_SYSROOT_FLAG}
            -target ${PLI_TARGET_TRIPLE}
            -o ${DEFAULT_BC_DIR}/${base}.bc
    DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${RT_HEADERS}
    COMMENT "bitcode: ${base} for ${PLI_TARGET_TRIPLE}"
    VERBATIM)
  list(APPEND DEFAULT_PLAT_BCS ${DEFAULT_BC_DIR}/${base}.bc)
endforeach()
add_custom_command(
  OUTPUT ${DEFAULT_BC_DIR}/pli_runtime.bc
  COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${DEFAULT_RT_BCS} -o ${DEFAULT_BC_DIR}/pli_runtime.bc
  DEPENDS ${DEFAULT_RT_BCS})
add_custom_command(
  OUTPUT ${DEFAULT_BC_DIR}/pli_rtplat.bc
  COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-link ${DEFAULT_PLAT_BCS} -o ${DEFAULT_BC_DIR}/pli_rtplat.bc
  DEPENDS ${DEFAULT_PLAT_BCS})
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/embedded_runtime.inc
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
          ${DEFAULT_BC_DIR}/pli_runtime.bc ${CMAKE_BINARY_DIR}/embedded_runtime.inc
  DEPENDS ${DEFAULT_BC_DIR}/pli_runtime.bc ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
  COMMENT "embedding pli_runtime.bc for default target ${PLI_TARGET_TRIPLE}")
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/embedded_rtplat.inc
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
          ${DEFAULT_BC_DIR}/pli_rtplat.bc ${CMAKE_BINARY_DIR}/embedded_rtplat.inc
          pli_embedded_rtplat_bc
  DEPENDS ${DEFAULT_BC_DIR}/pli_rtplat.bc ${CMAKE_SOURCE_DIR}/scripts/gen_embed.py
  COMMENT "embedding pli_rtplat.bc for default target ${PLI_TARGET_TRIPLE}")
add_custom_target(embedded_runtime_inc_default DEPENDS ${CMAKE_BINARY_DIR}/embedded_runtime.inc ${CMAKE_BINARY_DIR}/embedded_rtplat.inc)
add_dependencies(pli embedded_runtime_inc_default)

# Embedded link sysroot (ADR-227: single-binary distribution). Release
# archives contain only pli(.exe): the link inputs under platform/ are
# compiled in as byte arrays (scripts/gen_sysroot_embed.py, one TU per file
# so large archives stay parallel) and materialized to a temp dir at link
# time (src/hostcrt.cpp). Only the build host's OS set is embedded, and only
# when the checked-in files match the build arch — an arch gap (or missing
# sources) embeds an empty table with a warning, and the driver falls back
# to the host toolchain. aarch64 releases populate the arch-native sysroot
# dirs before tagging (see .github/workflows/release.yml).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set(PLI_BUILD_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
  set(PLI_BUILD_ARCH aarch64)
else()
  set(PLI_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(PLI_SYSROOT_SRC_DIR "${CMAKE_SOURCE_DIR}")
set(PLI_SYSROOT_SET_NAME "")
set(PLI_SYSROOT_FILES "")
set(PLI_SYSROOT_KNOWN_OS FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(PLI_SYSROOT_KNOWN_OS TRUE)
  set(PLI_SYSROOT_SRC_DIR "${CMAKE_SOURCE_DIR}/platform/linux-musl")
  set(PLI_SYSROOT_SET_NAME "musl")
  if(PLI_BUILD_ARCH STREQUAL "x86_64")
    set(PLI_SYSROOT_FILES crt1.o crti.o crtn.o libc.a libdl.a libm.a
        libpthread.a librt.a "libpli_builtins-${PLI_BUILD_ARCH}.a")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(PLI_SYSROOT_KNOWN_OS TRUE)
  set(PLI_SYSROOT_SRC_DIR "${CMAKE_SOURCE_DIR}/platform/windows-mingw")
  set(PLI_SYSROOT_SET_NAME "mingw")
  if(PLI_BUILD_ARCH STREQUAL "x86_64")
    set(PLI_SYSROOT_FILES crt2.o crtbegin.o crtend.o fltused.o chkstk.o
        libgcc.a libkernel32.a libmsvcrt.a libmingw32.a libmingwex.a
        libmoldname.a libucrt.a libadvapi32.a libshell32.a libuser32.a)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # .tbd stubs are text symbol lists (arch-independent), so no arch gate.
  set(PLI_SYSROOT_KNOWN_OS TRUE)
  set(PLI_SYSROOT_SRC_DIR "${CMAKE_SOURCE_DIR}/platform/darwin-sdk")
  set(PLI_SYSROOT_SET_NAME "sdk")
  set(PLI_SYSROOT_FILES usr/lib/libm.tbd usr/lib/libSystem.tbd)
endif()
# Always generated (empty table on unknown OSes / arch gaps) so
# src/embedded_sysroot.cpp compiles everywhere.
set(PLI_SYSROOT_ABS "")
set(PLI_SYSROOT_MISSING "")
foreach(f ${PLI_SYSROOT_FILES})
  if(EXISTS "${PLI_SYSROOT_SRC_DIR}/${f}")
    list(APPEND PLI_SYSROOT_ABS "${PLI_SYSROOT_SRC_DIR}/${f}")
  else()
    list(APPEND PLI_SYSROOT_MISSING "${f}")
  endif()
endforeach()
if(PLI_SYSROOT_KNOWN_OS AND NOT "${PLI_SYSROOT_MISSING}" STREQUAL "")
  list(REMOVE_ITEM PLI_SYSROOT_FILES ${PLI_SYSROOT_MISSING})
  message(WARNING "link sysroot incomplete under ${PLI_SYSROOT_SRC_DIR} (missing: ${PLI_SYSROOT_MISSING}) — embedding ${PLI_SYSROOT_FILES}; user links fall back to the host toolchain for the rest")
elseif(PLI_SYSROOT_KNOWN_OS AND PLI_SYSROOT_FILES STREQUAL "")
  message(WARNING "no embeddable link sysroot for ${CMAKE_SYSTEM_NAME}/${PLI_BUILD_ARCH} — user links use the host toolchain (populate platform/ from arch-native packages before tagging)")
endif()
# Generated TU per file (sanitize mirrors gen_sysroot_embed.py: every
# non-alphanumeric run becomes '_'). "none" sentinel: an empty set name
# would risk CMake empty-arg dropping shifting the script's argv.
if(PLI_SYSROOT_SET_NAME STREQUAL "")
  set(PLI_SYSROOT_SET_NAME "none")
endif()
set(PLI_SYSROOT_GEN_CPPS "")
foreach(f ${PLI_SYSROOT_FILES})
  string(REGEX REPLACE "[^0-9A-Za-z]+" "_" _stem "${f}")
  string(REGEX REPLACE "^_+|_+$" "" _stem "${_stem}")
  list(APPEND PLI_SYSROOT_GEN_CPPS "${CMAKE_BINARY_DIR}/sysroot_embed/${_stem}.cpp")
endforeach()
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/embedded_sysroot.inc ${PLI_SYSROOT_GEN_CPPS}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_sysroot_embed.py
          "${PLI_SYSROOT_SRC_DIR}" "${CMAKE_BINARY_DIR}/sysroot_embed" pli_sysroot_
          ${CMAKE_BINARY_DIR}/embedded_sysroot.inc ${PLI_SYSROOT_SET_NAME} ${PLI_SYSROOT_FILES}
  DEPENDS ${PLI_SYSROOT_ABS} ${CMAKE_SOURCE_DIR}/scripts/gen_sysroot_embed.py
  COMMENT "embedding link sysroot (${CMAKE_SYSTEM_NAME}/${PLI_BUILD_ARCH})"
  VERBATIM)
add_custom_target(embedded_sysroot_inc
  DEPENDS ${CMAKE_BINARY_DIR}/embedded_sysroot.inc ${PLI_SYSROOT_GEN_CPPS})
add_dependencies(pli embedded_sysroot_inc)
target_sources(pli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_sysroot.cpp
                 ${PLI_SYSROOT_GEN_CPPS})

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
add_test(NAME pli-tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/run_tests.py)
set_tests_properties(pli-tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  ENVIRONMENT "PLI=$<TARGET_FILE:pli>;CLANG=${LLVM_TOOLS_BINARY_DIR}/clang")

# Single-binary distribution: only the pli driver ships. The runtime travels
# embedded (embedded_runtime.inc), so no libpli_runtime.a / runtime.bc is
# installed; the driver falls back to the embedded blob when they are absent.
install(TARGETS pli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Quality gate: formatting, static analysis, warnings-as-errors ---------------
# The clang tooling is sought from the same LLVM installation as the compiler.
find_program(CLANG_FORMAT_BIN NAMES clang-format clang-format-${LLVM_VERSION_MAJOR}
  HINTS ${LLVM_TOOLS_BINARY_DIR})
find_program(CLANG_TIDY_BIN NAMES clang-tidy clang-tidy-${LLVM_VERSION_MAJOR}
  HINTS ${LLVM_TOOLS_BINARY_DIR})
find_program(SCAN_BUILD_BIN NAMES scan-build scan-build-py
  HINTS ${LLVM_TOOLS_BINARY_DIR})

# All project sources managed by clang-format / clang-tidy.
  set(ALL_CPP_SOURCES ${PLI_SOURCES} src/embedded_sysroot.cpp)
  set(ALL_CPP_HEADERS
    src/ast.h src/codegen.h src/diag.h src/embedded_runtime.h src/embedded_sysroot.h src/explain.h
    src/hir.h src/irgen.h src/lexer.h src/parser.h src/preprocessor.h
    src/sema.h src/target.h src/token.h src/types.h)
  if(PLI_ENABLE_MLIR)
    list(APPEND ALL_CPP_SOURCES src/mlir/PliDialect.cpp src/mlir/PliLowering.cpp)
    list(APPEND ALL_CPP_HEADERS src/mlir/PliDialect.h)
  endif()
set(ALL_C_SOURCES ${RT_SRCS})
set(ALL_C_HEADERS
  runtime/pli_rt.h runtime/pli_rt_state.h runtime/pli_rt_abi.def
  runtime/pli_rt_plat.h runtime/pli_wide128.h)
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
add_dependencies(tidy pli)

# werror: rebuild with -DPLI_WERROR=ON to enforce warnings-as-errors.
add_custom_target(werror
  COMMAND ${CMAKE_COMMAND} -S ${CMAKE_SOURCE_DIR} -B ${CMAKE_BINARY_DIR} -DPLI_WERROR=ON
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target pli
  COMMENT "Building pli with -Werror (PLI_WERROR=ON)"
  VERBATIM)

# scan: run the clang static analyzer (scan-build) over the pli build.
add_custom_target(scan
  COMMAND ${SCAN_BUILD_BIN}
    --use-c++=${CMAKE_CXX_COMPILER}
    --use-cc=${CMAKE_C_COMPILER}
    ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target pli
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "scan-build: static analyzer over pli"
  VERBATIM)

# check: full analysis gate — format check + static analysis + warnings-as-errors.
# Runs each sub-target in order via cmake --build so the sequence is deterministic.
add_custom_target(check
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target fmt-check
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tidy
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target werror
  COMMENT "Running analysis gate (fmt-check + tidy + werror)"
  VERBATIM)

# size-report: report the stripped size of the pli binary.
# Use a portable CMake script for size reporting — stat/sh cannot be used
# in CMake custom targets on Windows (cmd.exe context with Ninja).
file(WRITE ${CMAKE_BINARY_DIR}/size_report.cmake
  "if(NOT EXISTS \"${CMAKE_BINARY_DIR}/pli.strip\")\n"
  "  message(FATAL_ERROR \"Stripped binary not found: ${CMAKE_BINARY_DIR}/pli.strip\")\n"
  "endif()\n"
  "file(SIZE \"${CMAKE_BINARY_DIR}/pli.strip\" pli_size)\n"
  "message(STATUS \"Stripped size of pli: \${pli_size} bytes (target: < 100 MB, hard cap 150 MB)\")\n"
  "if(\${pli_size} GREATER 157286400)\n"
  "  message(FATAL_ERROR \"Binary exceeds 150 MB limit\")\n"
  "endif()\n")
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  find_program(LLVM_OBJCOPY_BIN NAMES llvm-objcopy
    HINTS ${CMAKE_PREFIX_PATH}/bin ${LLVM_TOOLS_BINARY_DIR})
  if(LLVM_OBJCOPY_BIN)
    add_custom_target(size-report
      COMMAND ${LLVM_OBJCOPY_BIN} --strip-all -o ${CMAKE_BINARY_DIR}/pli.strip $<TARGET_FILE:pli>
      COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/size_report.cmake
      COMMENT "Stripped size of pli (target: < 100 MB, hard cap 150 MB)")
  else()
    # No llvm-objcopy: report the raw binary size portably (same 150 MB gate).
    file(WRITE ${CMAKE_BINARY_DIR}/size_report_raw.cmake
      "file(SIZE \"\${PLI_BIN}\" pli_size)\n"
      "message(STATUS \"Raw size of pli (no llvm-objcopy): \${pli_size} bytes (target: < 100 MB, hard cap 150 MB)\")\n"
      "if(\${pli_size} GREATER 157286400)\n"
      "  message(FATAL_ERROR \"Binary exceeds 150 MB limit\")\n"
      "endif()\n")
    add_custom_target(size-report
      COMMAND ${CMAKE_COMMAND} -DPLI_BIN=$<TARGET_FILE:pli> -P ${CMAKE_BINARY_DIR}/size_report_raw.cmake
      COMMENT "Raw pli size (target: < 100 MB, hard cap 150 MB)")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  add_custom_target(size-report
    COMMAND strip -o ${CMAKE_BINARY_DIR}/pli.strip $<TARGET_FILE:pli>
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/size_report.cmake
    COMMENT "Stripped size of pli (target: < 100 MB, hard cap 150 MB)")
else()
  add_custom_target(size-report
    COMMAND strip $<TARGET_FILE:pli> -o ${CMAKE_BINARY_DIR}/pli.strip
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/size_report.cmake
    COMMENT "Stripped size of pli (target: < 100 MB, hard cap 150 MB)")
endif()
