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
find_package(Python3 REQUIRED COMPONENTS Interpreter)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

set(PLIC_SOURCES
  src/main.cpp
  src/diag.cpp
  src/explain.cpp
  src/hir.cpp
  src/lexer.cpp
  src/parser.cpp
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
find_path(LLD_INCLUDE_DIR lld/Common/Driver.h PATHS ${LLVM_INCLUDE_DIRS} NO_DEFAULT_PATH)
if(LLD_INCLUDE_DIR)
  set(PLIC_HAVE_LLD 1)
  message(STATUS "lld headers found: in-process link enabled")
else()
  set(PLIC_HAVE_LLD 0)
  message(STATUS "lld headers NOT found (brew install lld): clang-link fallback")
endif()

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
target_link_libraries(plic PRIVATE plic_rules)
llvm_map_components_to_libnames(llvm_libs core irreader support)
target_link_libraries(plic PRIVATE ${llvm_libs})

# libpli runtime (C11).
add_library(libpli STATIC ${RT_SRCS})
set_target_properties(libpli PROPERTIES OUTPUT_NAME pli)

# Embedded runtime (Phase 2): per-unit bitcode, linked bundle, byte array.
set(RT_BCS "")
foreach(src ${RT_SRCS})
  get_filename_component(base ${src} NAME_WE)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bc/${base}.bc
    COMMAND ${CMAKE_C_COMPILER} -emit-llvm -c ${CMAKE_SOURCE_DIR}/${src}
            -I${CMAKE_SOURCE_DIR}/runtime -O2
            -ffunction-sections -fdata-sections
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
