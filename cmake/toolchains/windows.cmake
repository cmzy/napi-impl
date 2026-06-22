# Windows (x64) toolchain.
#
# Builds with the GNU-style clang driver (clang/clang++), NOT MSVC cl.exe and NOT
# clang-cl. This matches microsoft/hermes-windows' own "base-ninja-clang" preset
# and is the only combination that actually compiles the engine:
#   - cl.exe can't parse StaticH.cpp's raw longjmp (__attribute__((naked)) + GNU
#     __asm__) — "error C2065: 'naked'".
#   - clang-cl lands in a gap in Hermes' CMake: hermes_update_cxx_flags / jsi key
#     C++-exception handling on CMAKE_CXX_COMPILER_ID, emitting GNU -fexceptions
#     for "Clang" (which clang-cl silently ignores) and reserving /EHsc for
#     "MSVC" only — so API/jsi/jsi.cpp fails with "cannot use 'throw' with
#     exceptions disabled".
# clang++ (ID=Clang) honors -fexceptions, so JSI builds. clang.exe still targets
# x86_64-pc-windows-msvc (MSVC ABI + the SDK from the vcvars env), linking via
# lld-link/link.exe — so the .def export wiring (gated on WIN32) still applies.
#
# Override CMAKE_C/CXX_COMPILER before include for a different compiler.

if(NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER clang++)
endif()

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "")

# Static CRT (/MT), matching the Hermes engine — HermesWindows.cmake forces
# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded, and its archives embed a static-CRT
# RuntimeLibrary directive. Linking our /MD object against them trips lld-link's
# /failifmismatch ("MD_DynamicRelease" vs "MT_StaticRelease"). Same genex as
# Hermes so Debug picks MultiThreadedDebug.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
