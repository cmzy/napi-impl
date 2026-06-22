// Minimal end-to-end smoke test for the Hermes NAPI backend, mirroring the V8
// path's M1 check: napi_create_platform -> runtime -> env -> run "1+2" -> == 3.
// Uses only the public embedding API + standard napi_* surface.

#include <cstdio>
#include <cstring>

#include "napi/js_native_api.h"
#include "napi_v8/embedding.h"

#define CHECK(expr)                                                                                                    \
    do {                                                                                                              \
        napi_status _s = (expr);                                                                                      \
        if (_s != napi_ok) {                                                                                          \
            std::fprintf(stderr, "FAIL: %s -> status %d\n", #expr, (int)_s);                                         \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

static void on_error(const char *msg) { std::fprintf(stderr, "[engine error] %s\n", msg ? msg : "(null)"); }

int main() {
    napi_platform platform = nullptr;
    napi_runtime runtime = nullptr;
    napi_env env = nullptr;

    CHECK(napi_create_platform(0, nullptr, 0, nullptr, on_error, false, &platform));
    CHECK(napi_create_runtime(platform, &runtime));
    CHECK(napi_create_env(runtime, &env));

    napi_value source = nullptr, result = nullptr;
    const char *code = "1 + 2";
    CHECK(napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &source));
    CHECK(napi_run_script(env, source, &result));

    int32_t value = 0;
    CHECK(napi_get_value_int32(env, result, &value));

    std::printf("1 + 2 = %d\n", value);
    int rc = (value == 3) ? 0 : 2;

    CHECK(napi_destroy_env(env));
    CHECK(napi_destroy_runtime(runtime));
    CHECK(napi_destroy_platform(platform));

    std::puts(rc == 0 ? "SMOKE PASS" : "SMOKE FAIL");
    return rc;
}
