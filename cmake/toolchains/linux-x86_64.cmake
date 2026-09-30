# Toolchain for cross-compiling to Linux x86_64 from macOS
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Use host clang cross-compiling to Linux target
set(CMAKE_C_COMPILER clang -target x86_64-unknown-linux-gnu)
set(CMAKE_CXX_COMPILER clang++ -target x86_64-unknown-linux-gnu)

# Sysroot: from musl-cross or Debian cross packages
set(CMAKE_SYSROOT /usr/local/x86_64-unknown-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)