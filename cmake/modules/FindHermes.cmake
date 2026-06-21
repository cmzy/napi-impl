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

find_library(HERMES_NODEAPI_LIB  NAMES hermesNodeApi
  PATHS ${HERMES_BUILD_DIR} PATH_SUFFIXES API/hermes_node_api NO_DEFAULT_PATH)
find_library(HERMES_VM_LIB       NAMES hermesvm_a
  PATHS ${HERMES_BUILD_DIR} PATH_SUFFIXES lib NO_DEFAULT_PATH)
find_library(HERMES_BOOSTCTX_LIB NAMES boost_context
  PATHS ${HERMES_BUILD_DIR}
  PATH_SUFFIXES external/boost/boost_1_86_0/libs/context
  NO_DEFAULT_PATH)
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

find_package_handle_standard_args(Hermes
  REQUIRED_VARS HERMES_NODEAPI_LIB HERMES_VM_LIB HERMES_BOOSTCTX_LIB
                ICU_FOUND HERMES_SRC_DIR)

if(Hermes_FOUND AND NOT TARGET Hermes::Hermes)
  add_library(Hermes::Hermes INTERFACE IMPORTED)
  set_target_properties(Hermes::Hermes PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${HERMES_INCLUDE_DIRS}")
  # The three archives have cyclic references; wrap them in a rescan group.
  target_link_libraries(Hermes::Hermes INTERFACE
    "$<LINK_GROUP:RESCAN,${HERMES_NODEAPI_LIB},${HERMES_VM_LIB},${HERMES_BOOSTCTX_LIB}>"
    ICU::i18n ICU::uc ICU::data
    Threads::Threads ${CMAKE_DL_LIBS})
endif()
