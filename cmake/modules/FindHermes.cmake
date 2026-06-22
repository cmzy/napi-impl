# FindHermes — locate a prebuilt microsoft/hermes-windows tree for the Hermes
# NAPI backend.
#
# Following the repo's V8 philosophy, the engine is built standalone (Hermes is
# its own root CMake project; it assumes CMAKE_SOURCE_DIR is its tree, so it
# cannot be add_subdirectory'd) and we link the resulting static archives.
#
# Inputs (cache variables, normally set by scripts/build.py):
#   HERMES_SRC_DIR    - checkout of microsoft/hermes-windows (has CMakeLists.txt)
#   HERMES_BUILD_DIR  - directory where Hermes was configured+built
#
# We need exactly three archives plus system ICU:
#   hermesNodeApi  - Node-API implemented on the Hermes VM (the napi_* symbols)
#   hermesvm_a     - the full VM+compiler aggregate (incl. InternalBytecode)
#   boost_context  - Hermes' renamed Boost.Context (fcontext) used by the VM
#
# Outputs:
#   HERMES_FOUND, HERMES_INCLUDE_DIRS, HERMES_ARCHIVES (the 3 .a, link-ordered),
#   and imported target Hermes::Hermes (include dirs + grouped archives + ICU).

include(FindPackageHandleStandardArgs)

if(NOT HERMES_SRC_DIR)
  set(HERMES_SRC_DIR "${NAPI_IMPL_ROOT}/third_party/hermes-windows")
endif()

if(NOT HERMES_BUILD_DIR)
  message(FATAL_ERROR
    "HERMES_BUILD_DIR not set. Build Hermes first (scripts/build.py --engine=hermes "
    "does this) and pass -DHERMES_BUILD_DIR=<dir>.")
endif()

# NO_CMAKE_FIND_ROOT_PATH: cross toolchains (Android NDK) set
# CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY, which would otherwise confine
# find_library to the target sysroot and miss our archives in HERMES_BUILD_DIR.
find_library(HERMES_NODEAPI_LIB  NAMES hermesNodeApi
  PATHS ${HERMES_BUILD_DIR} PATH_SUFFIXES API/hermes_node_api
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(HERMES_VM_LIB       NAMES hermesvm_a
  PATHS ${HERMES_BUILD_DIR} PATH_SUFFIXES lib
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(HERMES_BOOSTCTX_LIB NAMES boost_context
  PATHS ${HERMES_BUILD_DIR}
  PATH_SUFFIXES external/boost/boost_1_86_0/libs/context
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
# Boost version dir may drift across Hermes updates; fall back to a recursive glob.
if(NOT HERMES_BOOSTCTX_LIB)
  file(GLOB_RECURSE _hermes_boostctx "${HERMES_BUILD_DIR}/*libboost_context.a")
  if(_hermes_boostctx)
    list(GET _hermes_boostctx 0 HERMES_BOOSTCTX_LIB)
  endif()
endif()

# Hermes uses system ICU on POSIX (the *_NN versioned symbols).
find_package(ICU QUIET COMPONENTS uc i18n data)

set(_hermes_inc_candidates
  ${HERMES_SRC_DIR}/API/hermes_node_api
  ${HERMES_SRC_DIR}/include
  ${HERMES_SRC_DIR}/public
  ${HERMES_SRC_DIR}/external
  ${HERMES_SRC_DIR}/external/llvh/include
  ${HERMES_SRC_DIR}/external/llvh/gen/include
  ${HERMES_BUILD_DIR}/include
  ${HERMES_BUILD_DIR}/lib/config
  ${HERMES_BUILD_DIR}/external/llvh/include)
# Generated-header dirs vary by Hermes layout/version; keep only those present
# (CMake validates INTERFACE_INCLUDE_DIRECTORIES existence at generate time).
set(HERMES_INCLUDE_DIRS "")
foreach(_d ${_hermes_inc_candidates})
  if(IS_DIRECTORY ${_d})
    list(APPEND HERMES_INCLUDE_DIRS ${_d})
  endif()
endforeach()

set(HERMES_ARCHIVES ${HERMES_NODEAPI_LIB} ${HERMES_VM_LIB} ${HERMES_BOOSTCTX_LIB})

# ICU is needed only when Hermes was built against it (POSIX desktop uses system
# ICU for unicode/Intl). Android/iOS builds use HERMES_UNICODE_LITE — no ICU — so
# ICU is optional here: link it when present, skip it otherwise.
set(_hermes_icu_libs "")
if(ICU_FOUND)
  set(_hermes_icu_libs ICU::i18n ICU::uc ICU::data)
endif()

# Android: Hermes' VM logs through liblog (__android_log_*).
set(_hermes_sys_libs Threads::Threads ${CMAKE_DL_LIBS})
if(ANDROID)
  list(APPEND _hermes_sys_libs log)
endif()
# Windows: hermesvm's SamplingProfilerSampler calls timeBeginPeriod/timeEndPeriod
# from winmm (lib/VM/CMakeLists.txt links it as a platform lib). A static archive
# doesn't carry that transitively, so add it for our DLL link.
if(WIN32)
  list(APPEND _hermes_sys_libs winmm)
endif()
# Apple desktop (macOS): Hermes' platform-unicode backend is PlatformUnicodeCF,
# which calls CoreFoundation (CFLocale/CFString/CFDateFormatter). iOS builds set
# HERMES_UNICODE_LITE (no CF refs), so this is only needed where CF is compiled
# in — but linking the framework is harmless when unreferenced, so add it for any
# Apple target that pulled CoreFoundation symbols into the VM archive.
if(APPLE)
  list(APPEND _hermes_sys_libs "-framework CoreFoundation")
endif()

find_package_handle_standard_args(Hermes
  REQUIRED_VARS HERMES_NODEAPI_LIB HERMES_VM_LIB HERMES_BOOSTCTX_LIB HERMES_SRC_DIR)

if(Hermes_FOUND AND NOT TARGET Hermes::Hermes)
  add_library(Hermes::Hermes INTERFACE IMPORTED)
  set_target_properties(Hermes::Hermes PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${HERMES_INCLUDE_DIRS}")
  # The three archives have cyclic references. GNU ld / lld (Linux, Android NDK)
  # need them wrapped in a --start-group/--end-group rescan block
  # ($<LINK_GROUP:RESCAN>). Apple's ld64 and the Windows PE linkers
  # (link.exe / lld-link) resolve the whole archive set without a group and do
  # not define the RESCAN link feature (CMake errors: "Feature 'RESCAN' ... is
  # not supported for the 'CXX' link language"), so there we list the archives
  # directly — order/cycles don't matter to those linkers.
  if(APPLE OR WIN32)
    set(_hermes_archive_group
      ${HERMES_NODEAPI_LIB} ${HERMES_VM_LIB} ${HERMES_BOOSTCTX_LIB})
  else()
    set(_hermes_archive_group
      "$<LINK_GROUP:RESCAN,${HERMES_NODEAPI_LIB},${HERMES_VM_LIB},${HERMES_BOOSTCTX_LIB}>")
  endif()
  target_link_libraries(Hermes::Hermes INTERFACE
    ${_hermes_archive_group}
    ${_hermes_icu_libs}
    ${_hermes_sys_libs})

  # Hermes' public headers (e.g. Support/OSCompat.h) select Windows code paths on
  # _WINDOWS/WIN32. Hermes' own build defines these, but does not export them to
  # header consumers; CMake's MSVC platform would supply them, but we build with
  # the GNU clang driver (MSVC is false), so define them for anything that
  # includes Hermes headers (napi_hermes, hermes_smoke). Otherwise OSCompat.h
  # falls through to #include <unistd.h>, which does not exist on Windows.
  if(WIN32)
    set_property(TARGET Hermes::Hermes APPEND PROPERTY
      INTERFACE_COMPILE_DEFINITIONS WIN32 _WINDOWS)
  endif()
endif()
