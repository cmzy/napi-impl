# Shared compile settings for the CMake (Hermes/JSC/QuickJS) build track.
# Mirrors the intent of gn/napi_flags.gni for the GN (V8) track. Drift between
# the two is guarded by scripts/verify_flags_parity.py.
#
# Exposed as an INTERFACE target `napi_flags` that every backend target links.

if(TARGET napi_flags)
  return()
endif()

add_library(napi_flags INTERFACE)

# C++17 is the floor: Hermes itself is C++17. (The V8/GN track uses C++20; our
# own code stays C++17-compatible so it builds under both.)
target_compile_features(napi_flags INTERFACE cxx_std_17)

# Public NAPI headers consumers (and our backends) compile against.
target_include_directories(napi_flags INTERFACE ${NAPI_IMPL_ROOT}/include)

# Hide everything by default; the per-backend version script re-exports napi_*.
if(NOT MSVC)
  target_compile_options(napi_flags INTERFACE
    -fvisibility=hidden
    -fvisibility-inlines-hidden)
endif()
