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

# Re-run CMake when the source list changes (file(STRINGS) is configure-time).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS sources.txt)
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

  # gtest-based unit tests. The former bespoke main()+CHECK/EXPECT smoke tests
  # (jsc_smoke / jsc_weakref / jsc_v8compat / jsc_buffers) were converted to
  # googletest and now link GTest::gtest_main alongside node_api_extras_gtest.
  # All are gated on -DNAPI_GTEST=ON (googletest is fetched at the top level); a
  # plain configure without NAPI_GTEST still succeeds — these tests are skipped.
  if(NAPI_GTEST)
    add_executable(jsc_smoke ${NAPI_IMPL_ROOT}/test/jsc_smoke.cc)
    target_link_libraries(jsc_smoke PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME jsc_smoke COMMAND jsc_smoke)

    # White-box weak-ref / finalizer test: needs the internal header (env layout)
    # and JSC directly (JSGarbageCollect) to force deterministic collection.
    add_executable(jsc_weakref ${NAPI_IMPL_ROOT}/test/jsc_weakref.cc)
    target_include_directories(jsc_weakref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(jsc_weakref PRIVATE napi_flags napi_jsc JSC::JSC GTest::gtest_main)
    add_test(NAME jsc_weakref COMMAND jsc_weakref)

    # V8-compat extensions: SharedArrayBuffer (real zero-copy) + Inspector.
    add_executable(jsc_v8compat ${NAPI_IMPL_ROOT}/test/jsc_v8compat.cc)
    target_link_libraries(jsc_v8compat PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME jsc_v8compat COMMAND jsc_v8compat)

    # ArrayBuffer / TypedArray / DataView, BigInt, type tags, all_property_names.
    add_executable(jsc_buffers_test ${NAPI_IMPL_ROOT}/test/jsc_buffers.cc)
    target_link_libraries(jsc_buffers_test PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME jsc_buffers COMMAND jsc_buffers_test)

    # gtest unit tests for the experimental core calls + fast accessor (coverage
    # target).
    add_executable(node_api_extras_gtest ${NAPI_IMPL_ROOT}/test/node_api_extras_gtest.cc)
    target_link_libraries(node_api_extras_gtest PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME node_api_extras_gtest COMMAND node_api_extras_gtest)

    # Engine-neutral fast-call fallback (src/common/fast_call_fallback.cc): the
    # slow-equivalent surface this (non-V8) backend exposes for napi/fast_call.h.
    add_executable(fast_call_fallback_gtest ${NAPI_IMPL_ROOT}/test/fast_call_fallback_gtest.cc)
    target_link_libraries(fast_call_fallback_gtest PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME fast_call_fallback_gtest COMMAND fast_call_fallback_gtest)

    # Common core napi_* surface not exercised by the conformance suite on this
    # backend (utf16/latin1 strings, coercions, errors, typeof, ...).
    add_executable(napi_core_gtest ${NAPI_IMPL_ROOT}/test/napi_core_gtest.cc)
    target_link_libraries(napi_core_gtest PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME napi_core_gtest COMMAND napi_core_gtest)

    # Object/property/lifecycle surface (properties, elements, promises, dates,
    # instances, references, externals, finalizers, freeze/seal, error helpers).
    add_executable(napi_object_gtest ${NAPI_IMPL_ROOT}/test/napi_object_gtest.cc)
    target_link_libraries(napi_object_gtest PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME napi_object_gtest COMMAND napi_object_gtest)

    # Full TypedArray-kind matrix + external arraybuffer (napi<->JSC type switches).
    add_executable(napi_typedarray_gtest ${NAPI_IMPL_ROOT}/test/napi_typedarray_gtest.cc)
    target_link_libraries(napi_typedarray_gtest PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME napi_typedarray_gtest COMMAND napi_typedarray_gtest)

    # More core surface: has_named_property, get_prototype, arrays, external
    # strings, post_finalizer.
    add_executable(napi_more_gtest ${NAPI_IMPL_ROOT}/test/napi_more_gtest.cc)
    target_link_libraries(napi_more_gtest PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME napi_more_gtest COMMAND napi_more_gtest)

    # Deeper core: new_target, property keys, attrs, exception forwarding, etc.
    add_executable(napi_more2_gtest ${NAPI_IMPL_ROOT}/test/napi_more2_gtest.cc)
    target_link_libraries(napi_more2_gtest PRIVATE napi_flags napi_jsc GTest::gtest_main)
    add_test(NAME napi_more2_gtest COMMAND napi_more2_gtest)
  endif()

  # The upstream js-native-api host runner (test/runner.cc), built for JSC via
  # NAPI_RUNNER_JSC: same engine-agnostic source as the V8/Hermes paths, with a
  # JSGarbageCollect-backed gc(). Needs JSC::JSC directly (JSGarbageCollect) and
  # the dl library (dlopen of each binding). scripts/run_tests.py --engine=jsc
  # drives it. POSIX-only (pipe()/fork()).
  if(NOT WIN32)
    add_executable(runner ${NAPI_IMPL_ROOT}/test/runner.cc)
    target_compile_definitions(runner PRIVATE NAPI_RUNNER_JSC)
    target_link_libraries(runner PRIVATE napi_flags napi_jsc JSC::JSC ${CMAKE_DL_LIBS})
    # Keep it a bare CLI executable (not a .app bundle) on iOS so scripts/run_tests
    # finds it at src/jsc/runner and simctl spawn can exec it directly.
    set_target_properties(runner PROPERTIES MACOSX_BUNDLE OFF)

    # gtest harness over the upstream js-native-api conformance suite: one gtest
    # case per (module, test.js), each spawning `runner`. The addons must be built
    # first (scripts/build_tests.py); unbuilt modules report as SKIP.
    if(NAPI_GTEST)
      add_executable(js_native_api_suite_gtest ${NAPI_IMPL_ROOT}/test/js_native_api_suite_gtest.cc)
      target_link_libraries(js_native_api_suite_gtest PRIVATE GTest::gtest)
      target_compile_definitions(js_native_api_suite_gtest PRIVATE
        "NAPI_RUNNER_PATH=\"$<TARGET_FILE:runner>\""
        "NAPI_TESTS_DIR=\"${NAPI_IMPL_ROOT}/test/js-native-api\""
        "NAPI_SO_SUBDIR=\"Release\""
        "NAPI_SUITE_ENGINE=\"jsc\"")
      add_dependencies(js_native_api_suite_gtest runner)
      add_test(NAME js_native_api_suite COMMAND js_native_api_suite_gtest)
    endif()
  endif()
endif()
