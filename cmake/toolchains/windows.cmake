# Windows (x64) toolchain.
#
# Builds with clang-cl, NOT MSVC cl.exe. microsoft/hermes-windows' VM contains
# Clang-only constructs — StaticH.cpp's raw longjmp uses __attribute__((naked))
# + GNU __asm__ (its preset is "base-ninja-clang"), which cl.exe cannot parse
# (error C2065: 'naked'). clang-cl is Clang in MSVC-compatible mode: it keeps the
# MSVC ABI and CMake's MSVC=TRUE (so the napi_hermes /DEF export wiring still
# fires) while accepting the GNU asm. Pins policy too:
#   - target Windows 10+ ABI
#   - /MD runtime (match the host app by default)
#
# Override CMAKE_C/CXX_COMPILER before include for a different compiler.

if(NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER clang-cl)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER clang-cl)
endif()

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "")

# clang-cl ignores Hermes' GNU -fexceptions/-fno-exceptions flags and defaults to
# exceptions OFF (MSVC mode). API/jsi/jsi.cpp throws, so enable the MSVC C++
# exception model. (The other ignored GNU flags — -fvisibility, -ffunction/
# data-sections — are non-fatal: exports come from the .def, not -fvisibility.)
string(APPEND CMAKE_CXX_FLAGS_INIT " /EHsc")
