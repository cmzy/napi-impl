# Native Linux toolchain (x86_64 / arm64 host == target).
#
# Intentionally does NOT set CMAKE_SYSTEM_NAME — doing so would flip CMake into
# cross-compiling mode and break Hermes' host-tool (hermesc) build and its
# try_run feature checks. Compilers come from the environment (CC/CXX) or
# CMake's defaults. Present so scripts/build.py can pass a uniform per-platform
# toolchain.

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
