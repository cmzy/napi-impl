# CMake src entry — selects the engine backend by NAPI_ENGINE.
# (The V8 backend is built via GN, not here.)

add_subdirectory(common)

if(NAPI_ENGINE STREQUAL "hermes")
  add_subdirectory(hermes)
elseif(NAPI_ENGINE STREQUAL "jsc")
  message(FATAL_ERROR "NAPI_ENGINE=jsc not implemented yet")
elseif(NAPI_ENGINE STREQUAL "quickjs")
  message(FATAL_ERROR "NAPI_ENGINE=quickjs not implemented yet")
else()
  message(FATAL_ERROR "Unknown NAPI_ENGINE='${NAPI_ENGINE}' (expected hermes/jsc/quickjs)")
endif()
