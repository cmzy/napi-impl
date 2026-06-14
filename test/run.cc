// M1 smoke test: napi_run_script("1+2") must yield int32(3).
//
// Exit codes:
//   0  pass
//   1  napi call failure
//   2  result mismatch

#include <cstdio>
#include <cstdlib>

extern "C" {
#include "napi/js_native_api.h"
#include "napi_v8/embedding.h"
}

#define CHECK(expr)                                                          \
  do {                                                                       \
    napi_status _st = (expr);                                                \
    if (_st != napi_ok) {                                                    \
      std::fprintf(stderr, "[FAIL] %s -> %d\n", #expr, (int)_st);            \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

int main(int argc, char** argv) {
  napi_platform platform = nullptr;
  CHECK(napi_create_platform(argc, argv, 0, nullptr, nullptr, false, &platform));

  napi_runtime runtime = nullptr;
  CHECK(napi_create_runtime(platform, &runtime));

  napi_env env = nullptr;
  CHECK(napi_create_env(runtime, &env));

  napi_handle_scope scope = nullptr;
  CHECK(napi_open_handle_scope(env, &scope));

  napi_value src = nullptr;
  const char kSrc[] = "1+2";
  CHECK(napi_create_string_utf8(env, kSrc, NAPI_AUTO_LENGTH, &src));

  napi_value result = nullptr;
  CHECK(napi_run_script(env, src, &result));

  int32_t out = -1;
  CHECK(napi_get_value_int32(env, result, &out));

  CHECK(napi_close_handle_scope(env, scope));
  CHECK(napi_destroy_env(env));
  CHECK(napi_destroy_runtime(runtime));
  CHECK(napi_destroy_platform(platform));

  if (out != 3) {
    std::fprintf(stderr, "[FAIL] expected 3, got %d\n", (int)out);
    return 2;
  }
  std::printf("[OK] napi_run_script(\"1+2\") = %d\n", (int)out);
  return 0;
}
