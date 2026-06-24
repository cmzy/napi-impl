// End-to-end smoke test for the JSC NAPI backend. Mirrors the V8/Hermes M1
// check (napi_create_platform -> runtime -> env -> run "1+2" -> == 3) and then
// exercises a bit more of the hand-written surface — string round-trip, object
// properties, and a native function called from JS — to confirm the core works.
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

static void on_error(const char* msg) { std::fprintf(stderr, "[engine error] %s\n", msg ? msg : "(null)"); }

// A native function: returns its first argument doubled (arg0 + arg0).
static napi_value double_it(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1)
        return nullptr;
    double v = 0;
    napi_get_value_double(env, argv[0], &v);
    napi_value result = nullptr;
    napi_create_double(env, v + v, &result);
    return result;
}

static int run_value_checks(napi_env env) {
    // (1) run "1 + 2" == 3
    {
        napi_value src = nullptr, result = nullptr;
        CHECK(napi_create_string_utf8(env, "1 + 2", NAPI_AUTO_LENGTH, &src));
        CHECK(napi_run_script(env, src, &result));
        int32_t v = 0;
        CHECK(napi_get_value_int32(env, result, &v));
        std::printf("1 + 2 = %d\n", v);
        if (v != 3) return 2;
    }

    // (2) string round-trip through a script
    {
        napi_value src = nullptr, result = nullptr;
        CHECK(napi_create_string_utf8(env, "'hello ' + 'world'", NAPI_AUTO_LENGTH, &src));
        CHECK(napi_run_script(env, src, &result));
        char buf[64];
        size_t len = 0;
        CHECK(napi_get_value_string_utf8(env, result, buf, sizeof(buf), &len));
        std::printf("string = \"%s\" (len %zu)\n", buf, len);
        if (std::strcmp(buf, "hello world") != 0) return 3;
    }

    // (3) object property set/get
    {
        napi_value obj = nullptr, val = nullptr, got = nullptr;
        CHECK(napi_create_object(env, &obj));
        CHECK(napi_create_int32(env, 42, &val));
        CHECK(napi_set_named_property(env, obj, "x", val));
        CHECK(napi_get_named_property(env, obj, "x", &got));
        int32_t v = 0;
        CHECK(napi_get_value_int32(env, got, &v));
        std::printf("obj.x = %d\n", v);
        if (v != 42) return 4;
    }

    // (4) native function called from JS: double_it(21) == 42
    {
        napi_value global = nullptr, fn = nullptr, arg = nullptr, result = nullptr;
        CHECK(napi_get_global(env, &global));
        CHECK(napi_create_function(env, "double_it", NAPI_AUTO_LENGTH, double_it, nullptr, &fn));
        CHECK(napi_create_int32(env, 21, &arg));
        CHECK(napi_call_function(env, global, fn, 1, &arg, &result));
        int32_t v = 0;
        CHECK(napi_get_value_int32(env, result, &v));
        std::printf("double_it(21) = %d\n", v);
        if (v != 42) return 5;
    }
    return 0;
}

int main() {
    napi_platform platform = nullptr;
    napi_runtime runtime = nullptr;
    napi_env env = nullptr;

    CHECK(napi_create_platform(0, nullptr, 0, nullptr, on_error, false, &platform));
    CHECK(napi_create_runtime(platform, &runtime));
    CHECK(napi_create_env(runtime, &env));

    int rc = run_value_checks(env);

    CHECK(napi_destroy_env(env));
    CHECK(napi_destroy_runtime(runtime));
    CHECK(napi_destroy_platform(platform));

    std::puts(rc == 0 ? "SMOKE PASS" : "SMOKE FAIL");
    return rc;
}
