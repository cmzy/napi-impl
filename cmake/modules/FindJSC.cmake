# FindJSC — locate JavaScriptCore for the JSC NAPI backend.
#
# J1 targets Apple's system JavaScriptCore.framework: no engine to build or
# vendor, the C API headers (<JavaScriptCore/JavaScript.h>) and library both
# come from the framework, and it is already present on every macOS/iOS host.
# Because JSC is a shared system framework (not statically swallowed like the V8
# monolith or the Hermes archives), nothing foreign is absorbed into our dylib —
# JSC stays an external dynamic dependency.
#
# Outputs: JSC_FOUND and the imported target JSC::JSC (the framework, which also
# supplies the framework header search path so #include <JavaScriptCore/...>
# resolves).
#
# Other platforms (Linux/Android/Windows) would need a built/prebuilt WebKit
# JavaScriptCore; that is J2 and intentionally errors here for now.

include(FindPackageHandleStandardArgs)

if(APPLE)
  find_library(JSC_FRAMEWORK JavaScriptCore)
  find_package_handle_standard_args(JSC REQUIRED_VARS JSC_FRAMEWORK)
  if(JSC_FOUND AND NOT TARGET JSC::JSC)
    add_library(JSC::JSC INTERFACE IMPORTED)
    target_link_libraries(JSC::JSC INTERFACE ${JSC_FRAMEWORK})
  endif()
else()
  message(FATAL_ERROR
    "FindJSC: only Apple's system JavaScriptCore.framework is wired (J1). "
    "Linux/Android/Windows need a built WebKit JavaScriptCore (J2).")
endif()
