# JSC NAPI backend (CMake track).
#
# JavaScriptCore ships no Node-API, so — unlike the Hermes backend, which links
# Hermes' bundled hermesNodeApi — this backend hand-writes the full napi_*
# surface on JSC's stable C API (js_native_api_jsc.cc + jsc_object.cc) and adds
# the cross-engine embedding adapter (napi_jsc_engine.cc) over JSContextGroup /
# JSGlobalContext. The result, libnapi_jsc, exports only napi_* and is a drop-in
# ABI swap for libnapi_v8 / libnapi_hermes.

list(APPEND CMAKE_MODULE_PATH ${NAPI_IMPL_ROOT}/cmake/modules)
find_package(JSC REQUIRED)

file(STRINGS sources.txt JSC_SOURCES_RAW)
list(FILTER JSC_SOURCES_RAW EXCLUDE REGEX "^#|^$")

add_library(napi_jsc SHARED ${JSC_SOURCES_RAW})
set_target_properties(napi_jsc PROPERTIES OUTPUT_NAME napi_jsc)

target_link_libraries(napi_jsc PRIVATE napi_flags napi_common JSC::JSC)

# Export only the canonical napi_*/node_api_* surface; hide our internals
# (-fvisibility=hidden from napi_flags handles the rest). The list omits the
# V8-only inspector/SharedArrayBuffer extensions, same as the Hermes list.
if(APPLE)
  target_link_options(napi_jsc PRIVATE
    "-Wl,-exported_symbols_list,${NAPI_IMPL_ROOT}/gn/exports/napi_jsc.exp")
elseif(WIN32)
  target_link_options(napi_jsc PRIVATE
    "LINKER:/DEF:${NAPI_IMPL_ROOT}/gn/exports/napi_jsc.def")
else()
  target_link_options(napi_jsc PRIVATE
    "-Wl,--version-script=${NAPI_IMPL_ROOT}/gn/exports/napi_jsc.lds")
endif()

# ---- smoke test (J1: napi_run_script("1+2") == 3, plus core round-trips) ----

option(NAPI_JSC_BUILD_TESTS "Build the JSC backend smoke test" ON)
if(NAPI_JSC_BUILD_TESTS)
  enable_testing()
  add_executable(jsc_smoke ${NAPI_IMPL_ROOT}/test/jsc_smoke.cc)
  target_link_libraries(jsc_smoke PRIVATE napi_flags napi_jsc)
  add_test(NAME jsc_smoke COMMAND jsc_smoke)
endif()
