# Hermes NAPI backend (CMake track).
#
# Strategy A: link microsoft/hermes-windows' Node-API implementation
# (hermesNodeApi, which runs directly on the Hermes VM) and wrap its
# runtime/env creation behind our cross-engine embedding API
# (napi_v8/embedding.h). The result, libnapi_hermes, statically swallows Hermes
# and exports only napi_*, so it is a drop-in ABI swap for libnapi_v8.
#
# Hermes is built standalone (it must be its own CMake root) and linked here as
# prebuilt archives — see cmake/modules/FindHermes.cmake. We deliberately avoid
# Hermes JSI and the Chrome inspector (the latter does not build off-Windows and
# debugging is out of scope).

list(APPEND CMAKE_MODULE_PATH ${NAPI_IMPL_ROOT}/cmake/modules)
find_package(Threads REQUIRED)
find_package(Hermes REQUIRED)

file(STRINGS sources.txt HERMES_SOURCES_RAW)
list(FILTER HERMES_SOURCES_RAW EXCLUDE REGEX "^#|^$")

add_library(napi_hermes SHARED ${HERMES_SOURCES_RAW})
set_target_properties(napi_hermes PROPERTIES OUTPUT_NAME napi_hermes)

target_link_libraries(napi_hermes PRIVATE napi_flags napi_common Hermes::Hermes)

# Export only napi_*; hide all Hermes/VM/ICU/LLVH symbols. The Hermes export
# list (scripts/gen_export_list.py --engine=hermes) omits the V8-only inspector
# and SharedArrayBuffer extensions.
if(NOT APPLE AND NOT MSVC)
  target_link_options(napi_hermes PRIVATE
    "-Wl,--version-script=${NAPI_IMPL_ROOT}/gn/exports/napi_hermes.lds")
endif()

# ---- smoke test (M7.1: napi_run_script("1+2") == 3) -----------------------

option(NAPI_HERMES_BUILD_TESTS "Build the Hermes backend smoke test" ON)
if(NAPI_HERMES_BUILD_TESTS)
  enable_testing()
  add_executable(hermes_smoke ${NAPI_IMPL_ROOT}/test/hermes_smoke.cc)
  target_link_libraries(hermes_smoke PRIVATE napi_flags napi_hermes)
  add_test(NAME hermes_smoke COMMAND hermes_smoke)

  # The js-native-api host runner — same source as the V8 path (test/runner.cc),
  # built engine-agnostic via NAPI_RUNNER_HERMES (skips V8 inspector + internal
  # finalizer drain). scripts/run_tests.py --engine=hermes drives it.
  add_executable(runner ${NAPI_IMPL_ROOT}/test/runner.cc)
  target_compile_definitions(runner PRIVATE NAPI_RUNNER_HERMES)
  target_link_libraries(runner PRIVATE napi_flags napi_hermes ${CMAKE_DL_LIBS})
endif()
