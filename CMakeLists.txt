# Top-level CMake for the non-V8 engine backends (Hermes today; JSC/QuickJS
# reserved). The V8 backend uses the GN track (.gn / BUILD.gn) instead; the two
# build systems coexist in this repo and never run together.
#
#   cmake -S . -B out/build/hermes-linux-x86_64 -G Ninja \
#         -DNAPI_ENGINE=hermes -DCMAKE_BUILD_TYPE=Release
#   cmake --build out/build/hermes-linux-x86_64
#
# scripts/build.py --engine=hermes wraps this.

cmake_minimum_required(VERSION 3.21)
project(napi_impl CXX C)

set(NAPI_IMPL_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(NAPI_ENGINE "hermes" CACHE STRING "JS engine backend: hermes | jsc | quickjs")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/napi_flags.cmake)

# Optional: llvm source-based coverage. Instruments every target that links
# napi_flags (the backend lib + the tests). Pair with -DCMAKE_BUILD_TYPE=Debug
# for accurate line mapping. Report via llvm-profdata + llvm-cov.
option(NAPI_COVERAGE "Build with llvm source-based coverage instrumentation" OFF)
if(NAPI_COVERAGE)
  target_compile_options(napi_flags INTERFACE -fprofile-instr-generate -fcoverage-mapping)
  target_link_options(napi_flags INTERFACE -fprofile-instr-generate -fcoverage-mapping)
endif()

# Optional: gtest-based unit tests (fetched via FetchContent). Off by default so
# ordinary builds don't pull googletest; enable with -DNAPI_GTEST=ON. To reuse a
# local checkout instead of cloning, pass -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<path>.
option(NAPI_GTEST "Build gtest-based unit tests (fetches googletest)" OFF)
if(NAPI_GTEST)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

add_subdirectory(src)
