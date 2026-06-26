// Tests the JSC adaptation of the V8-specific extensions: SharedArrayBuffer
// (real zero-copy C<->JS) and the Inspector (start/stop toggle inspectability;
// the CDP message-loop calls are no-ops on JSC). Public API only.

#include <cstdio>
#include <cstdlib>
#include <cstring>

// SharedArrayBuffer is experimental core node-api now (node_api_*); reading its
// backing store goes through napi_get_arraybuffer_info (extended to accept SAB).
#define NAPI_EXPERIMENTAL
#include "napi/js_native_api.h"
#include "napi_v8/embedding.h"
#include "napi_v8/inspector.h"

#define CHECK(expr)                                                                                                    \
    do {                                                                                                              \
        napi_status _s = (expr);                                                                                      \
        if (_s != napi_ok) {                                                                                          \
            std::fprintf(stderr, "FAIL: %s -> status %d\n", #expr, (int)_s);                                         \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

#define EXPECT(cond, msg)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                                                  \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

static void on_error(const char* msg) { std::fprintf(stderr, "[engine error] %s\n", msg ? msg : "(null)"); }

static int sab_checks(napi_env env) {
    void* data = nullptr;
    napi_value sab = nullptr;
    CHECK(node_api_create_sharedarraybuffer(env, 32, &data, &sab));
    EXPECT(data != nullptr, "SAB: backing pointer must be non-null");

    bool is = false;
    CHECK(node_api_is_sharedarraybuffer(env, sab, &is));
    EXPECT(is, "SAB: created buffer should report is_shared_arraybuffer == true");

    // napi_get_arraybuffer_info is extended to read a SAB's backing store.
    void* info_data = nullptr;
    size_t info_len = 0;
    CHECK(napi_get_arraybuffer_info(env, sab, &info_data, &info_len));
    EXPECT(info_data == data, "SAB: get_arraybuffer_info pointer must match create pointer");
    EXPECT(info_len == 32, "SAB: get_arraybuffer_info length must be 32");

    // Zero-copy: write through the C pointer, read back from JS.
    static_cast<unsigned char*>(data)[5] = 0xAB;
    napi_value global = nullptr;
    CHECK(napi_get_global(env, &global));
    CHECK(napi_set_named_property(env, global, "__sab", sab));
    napi_value src = nullptr, res = nullptr;
    CHECK(napi_create_string_utf8(env, "(new Uint8Array(__sab))[5]", NAPI_AUTO_LENGTH, &src));
    CHECK(napi_run_script(env, src, &res));
    int32_t seen = 0;
    CHECK(napi_get_value_int32(env, res, &seen));
    std::printf("SAB zero-copy: wrote 0xAB via C ptr, JS read 0x%02X\n", seen);
    EXPECT(seen == 0xAB, "SAB: JS must observe the byte written through the C pointer");

    // A plain object is not a shared arraybuffer.
    napi_value obj = nullptr;
    CHECK(napi_create_object(env, &obj));
    CHECK(node_api_is_sharedarraybuffer(env, obj, &is));
    EXPECT(!is, "SAB: a plain object must not report is_shared_arraybuffer");

    // External SAB: wrap caller-owned memory zero-copy.
    auto* ext = static_cast<unsigned char*>(std::calloc(1, 16));
    EXPECT(ext != nullptr, "SAB: external alloc failed");
    ext[3] = 0x5C;
    napi_value esab = nullptr;
    CHECK(node_api_create_external_sharedarraybuffer(env, ext, 16, nullptr, nullptr, &esab));
    bool eis = false;
    CHECK(node_api_is_sharedarraybuffer(env, esab, &eis));
    EXPECT(eis, "SAB(external): must report is_shared_arraybuffer == true");
    void* edata = nullptr;
    size_t elen = 0;
    CHECK(napi_get_arraybuffer_info(env, esab, &edata, &elen));
    EXPECT(edata == ext, "SAB(external): info pointer must equal the wrapped buffer");
    EXPECT(elen == 16, "SAB(external): info length must be 16");
    // We passed no finalizer, so we own `ext`; free it after the env tears down.
    // (Skipping free here is fine for a short-lived test process.)
    std::free(ext);
    return 0;
}

static int inspector_checks(napi_env env) {
    // start/stop are real (toggle JSGlobalContextSetInspectable + set name).
    CHECK(napi_v8_inspector_start(env, 9229, "napi-jsc-test"));
    EXPECT(napi_v8_inspector_is_paused(env) == false, "inspector: should not be paused");
    size_t dispatched = 123;
    CHECK(napi_v8_inspector_pump_messages(env, &dispatched));
    EXPECT(dispatched == 0, "inspector: JSC has no host queue to drain (expect 0)");
    CHECK(napi_v8_inspector_wait(env, 0));
    CHECK(napi_v8_inspector_set_pause_handler(env, nullptr, nullptr));
    CHECK(napi_v8_inspector_set_wake_handler(env, nullptr, nullptr));
    CHECK(napi_v8_inspector_stop(env));
    std::puts("inspector: start/stop + no-op message loop OK (attach via Safari Develop)");
    return 0;
}

int main() {
    napi_platform platform = nullptr;
    napi_runtime runtime = nullptr;
    napi_env env = nullptr;
    CHECK(napi_create_platform(0, nullptr, 0, nullptr, on_error, false, &platform));
    CHECK(napi_create_runtime(platform, &runtime));
    CHECK(napi_create_env(runtime, &env));

    int rc = sab_checks(env);
    if (rc == 0)
        rc = inspector_checks(env);

    CHECK(napi_destroy_env(env));
    CHECK(napi_destroy_runtime(runtime));
    CHECK(napi_destroy_platform(platform));

    std::puts(rc == 0 ? "V8COMPAT PASS" : "V8COMPAT FAIL");
    return rc;
}
