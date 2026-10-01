# Toolchain for cross-compiling to Windows x86_64 (GNU/MinGW)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER clang -target x86_64-pc-windows-gnu)
set(CMAKE_CXX_COMPILER clang++ -target x86_64-pc-windows-gnu)

# mingw-w64 provides headers/libs (macOS: brew install mingw-w64)
set(CMAKE_SYSROOT /opt/homebrew/opt/mingw-w64/toolchain-x86_64)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)