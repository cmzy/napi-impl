# iOS cross-compile toolchain (device arm64 / simulator x86_64+arm64).
#
# Inputs (set by scripts/build.py):
#   IOS_PLATFORM   OS (device, default) | SIMULATOR
#   IOS_ARCH       arm64 (default) | x86_64
# Deployment target is iOS 13.0 (matches the V8 path).
#
# Like Android, host hermesc/shermes are built first and imported via
# -DIMPORT_HOST_COMPILERS when cross-compiling (scripts/build.py wires this).

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "")

if(NOT IOS_ARCH)
  set(IOS_ARCH "arm64")
endif()
set(CMAKE_OSX_ARCHITECTURES "${IOS_ARCH}" CACHE STRING "")

# CMake does not derive CMAKE_SYSTEM_PROCESSOR from CMAKE_OSX_ARCHITECTURES for
# iOS, so Hermes' bundled Boost.Context can't auto-detect the target arch/ABI
# (it would fall back to the host's x86_64/sysv and pick the wrong fcontext asm).
# Pin it to the target arch so make_<arch>_<abi>_macho_gas.S resolves correctly.
set(CMAKE_SYSTEM_PROCESSOR "${IOS_ARCH}")

if(IOS_PLATFORM STREQUAL "SIMULATOR")
  set(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "")
else()
  set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "")
endif()

# jitless on iOS (no W^X / JIT entitlement); matches the V8 path's iOS policy.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
