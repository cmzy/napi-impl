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

enable_testing()

add_subdirectory(src)
