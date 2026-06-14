# CMake薄壳 — 与 BUILD.gn 共享 sources.txt 单一信源。
# 仅在 CMake 路径（Hermes/JSC/QuickJS）下激活。M7 启用。

file(STRINGS sources.txt COMMON_SOURCES_RAW)
list(FILTER COMMON_SOURCES_RAW EXCLUDE REGEX "^#|^$")

if(COMMON_SOURCES_RAW)
  add_library(napi_common OBJECT ${COMMON_SOURCES_RAW})
  target_link_libraries(napi_common PRIVATE napi_flags)
else()
  add_library(napi_common INTERFACE)
endif()
