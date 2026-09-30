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
)

add_library(plic_rules STATIC ${CMAKE_BINARY_DIR}/rules.cpp)
target_include_directories(plic_rules PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_executable(plic ${PLIC_SOURCES})
target_include_directories(plic PRIVATE ${CMAKE_SOURCE_DIR}/src)
# Version identity for `plic version` (self-contained milestone).
execute_process(COMMAND git describe --tags --always --dirty
  OUTPUT_VARIABLE PLIC_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT PLIC_VERSION)
  set(PLIC_VERSION "unknown")
endif()
target_compile_definitions(plic PRIVATE
  PLIC_RUNTIME_LIB="$<TARGET_FILE:libpli>"
  PLIC_INSTALL_RUNTIME_LIB="${CMAKE_INSTALL_FULL_LIBDIR}/libpli.a"
  PLIC_RUNTIME_BC="${CMAKE_BINARY_DIR}/runtime.bc"
  PLIC_INSTALL_RUNTIME_BC="${CMAKE_INSTALL_FULL_LIBDIR}/runtime.bc"
  PLIC_CLANG="${LLVM_TOOLS_BINARY_DIR}/clang"
  PLIC_VERSION="${PLIC_VERSION}"
  PLIC_LLVM_VERSION="${LLVM_PACKAGE_VERSION}")
target_link_libraries(plic PRIVATE plic_rules)
llvm_map_components_to_libnames(llvm_libs core irreader support linker)
target_link_libraries(plic PRIVATE ${llvm_libs})

# libpli runtime (C11). One source per section so the archive strips unused
# pieces (ADR-079); the same sources feed the bitcode runtime
# (OPTIMIZATION.md §10, P3): each is compiled to LLVM IR with the matching
# clang and merged into runtime.bc with llvm-link. IRGen merges the runtime
# into the MAIN unit so clang sees real bodies; the archive stays for
# --no-bitcode-runtime. rt_core.c stamps runtime.bc with the LLVM version,
# checked at load time so a stale bitcode is diagnosed, not mis-linked.
set(PLI_RUNTIME_SOURCES
  runtime/rt_core.c
  runtime/rt_stream.c
  runtime/rt_string.c
  runtime/rt_math.c
  runtime/rt_mathport.c
  runtime/rt_cond.c
  runtime/rt_storage.c
  runtime/rt_get.c
  runtime/rt_file.c
  runtime/rt_edit.c
  runtime/rt_task.c
)
add_library(libpli STATIC ${PLI_RUNTIME_SOURCES})
set_target_properties(libpli PROPERTIES OUTPUT_NAME pli)

set(PLIC_RUNTIME_BC ${CMAKE_BINARY_DIR}/runtime.bc)
set(PLI_RUNTIME_BCS)
foreach(rt_src ${PLI_RUNTIME_SOURCES})
  get_filename_component(rt_base ${rt_src} NAME_WE)
  set(rt_bc ${CMAKE_BINARY_DIR}/${rt_base}.bc)
  add_custom_command(
    OUTPUT ${rt_bc}
    COMMAND ${LLVM_TOOLS_BINARY_DIR}/clang -emit-llvm -c
            -DPLIC_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
            ${rt_src} -o ${rt_bc}
    DEPENDS ${rt_src}
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
