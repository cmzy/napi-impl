# Windows (x64) toolchain.
#
# Native MSVC builds need no toolchain file (the VS generator / vcvarsall env
# provides the compiler). This file exists so scripts/build.py can pass a
# uniform per-platform toolchain and to pin policy:
#   - target Windows 10+ ABI
#   - /MD runtime (match the host app by default)
#
# For a clang-cl or cross build, set CMAKE_C/CXX_COMPILER before include.

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "")
