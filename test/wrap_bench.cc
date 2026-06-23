// Microbenchmark for napi_wrap / napi_unwrap, to quantify the cost of the
// private-key recomputation (Opt 0: cache keys) and the GetPrivate property
// lookup (Opt 1: read native from internal field 0).
//
// Two unwrap scenarios:
//   [class] a napi_define_class instance  (has reserved internal fields)
//   [plain] a napi_create_object object   (no internal fields -> private-prop path)
//
// We also time a wrap+remove cycle. Each scenario is run REPS times and the
// best (lowest) wall time is reported (least noisy). The accumulated pointer is
// printed so the loop can't be optimized away.
//
// Run: //napi/test:wrap_bench   (env WRAP_BENCH_N overrides the iteration count)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "napi/js_native_api.h"
#include "napi/node_api.h"
#include "napi_v8/embedding.h"

namespace {

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        napi_status _s = (expr);                                                                                       \
        if (_s != napi_ok) {                                                                                           \
            std::fprintf(stderr, "FAIL(status %d): %s\n", (int) _s, #expr);                                            \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

struct Ctx {
    int64_t x = 0;
};

napi_value Ctor(napi_env env, napi_callback_info info) {
    napi_value thisv = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisv, nullptr);
    return thisv;
}

using Clock = std::chrono::steady_clock;

double best_ns_per_op(int reps, long n, void (*body)(long, volatile uintptr_t*), volatile uintptr_t* sink) {
    double best = 1e30;
    for (int r = 0; r < reps; ++r) {
        auto t0 = Clock::now();
        body(n, sink);
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double>(t1 - t0).count() * 1e9;
        double per = ns / static_cast<double>(n);
        if (per < best)
            best = per;
    }
    return best;
}

// Globals so the timed lambdas (plain function pointers) can reach them.
napi_env g_env = nullptr;
napi_value g_class_inst = nullptr;     // define_class instance (internal fields)
napi_value g_plain_inst = nullptr;     // plain object (private-prop path)
napi_value g_wrapcycle_inst = nullptr; // define_class instance kept unwrapped for the wrap/remove cycle
Ctx g_ctx;                             // the native the benchmarks wrap

void unwrap_class(long n, volatile uintptr_t* sink) {
    uintptr_t acc = 0;
    for (long i = 0; i < n; ++i) {
        void* p = nullptr;
        napi_unwrap(g_env, g_class_inst, &p);
        acc += reinterpret_cast<uintptr_t>(p);
    }
    *sink += acc;
}

void unwrap_plain(long n, volatile uintptr_t* sink) {
    uintptr_t acc = 0;
    for (long i = 0; i < n; ++i) {
        void* p = nullptr;
        napi_unwrap(g_env, g_plain_inst, &p);
        acc += reinterpret_cast<uintptr_t>(p);
    }
    *sink += acc;
}

void wrap_remove_class(long n, volatile uintptr_t* sink) {
    uintptr_t acc = 0;
    for (long i = 0; i < n; ++i) {
        napi_wrap(g_env, g_wrapcycle_inst, &g_ctx, nullptr, nullptr, nullptr);
        void* p = nullptr;
        napi_remove_wrap(g_env, g_wrapcycle_inst, &p);
        acc += reinterpret_cast<uintptr_t>(p);
    }
    *sink += acc;
}

void scope_openclose(long n, volatile uintptr_t* sink) {
    uintptr_t acc = 0;
    for (long i = 0; i < n; ++i) {
        napi_handle_scope s = nullptr;
        napi_open_handle_scope(g_env, &s);
        acc += reinterpret_cast<uintptr_t>(s);
        napi_close_handle_scope(g_env, s);
    }
    *sink += acc;
}

} // namespace

int main(int argc, char** argv) {
    long n = 20'000'000;
    if (const char* e = std::getenv("WRAP_BENCH_N"))
        n = std::atol(e);
    const int reps = 5;

    napi_platform platform = nullptr;
    napi_runtime runtime = nullptr;
    CHECK(napi_create_platform(argc, argv, 0, nullptr, nullptr, false, &platform));
    CHECK(napi_create_runtime(platform, &runtime));
    CHECK(napi_create_env(runtime, &g_env));

    napi_handle_scope scope = nullptr;
    CHECK(napi_open_handle_scope(g_env, &scope));

    g_ctx.x = 42;

    // [class] a napi_define_class instance — reserves internal fields.
    napi_value cls = nullptr;
    CHECK(napi_define_class(g_env, "K", NAPI_AUTO_LENGTH, Ctor, nullptr, 0, nullptr, &cls));
    CHECK(napi_new_instance(g_env, cls, 0, nullptr, &g_class_inst));
    CHECK(napi_wrap(g_env, g_class_inst, &g_ctx, nullptr, nullptr, nullptr));
    // [wrap] a second define_class instance, left UNWRAPPED for the wrap/remove cycle.
    CHECK(napi_new_instance(g_env, cls, 0, nullptr, &g_wrapcycle_inst));

    // [plain] a plain object — no internal fields, forces the private-prop path.
    CHECK(napi_create_object(g_env, &g_plain_inst));
    CHECK(napi_wrap(g_env, g_plain_inst, &g_ctx, nullptr, nullptr, nullptr));

    // Sanity: both unwrap to &g_ctx.
    void* pc = nullptr;
    void* pp = nullptr;
    CHECK(napi_unwrap(g_env, g_class_inst, &pc));
    CHECK(napi_unwrap(g_env, g_plain_inst, &pp));
    if (pc != &g_ctx || pp != &g_ctx) {
        std::fprintf(stderr, "unwrap returned wrong pointer\n");
        return 1;
    }

    volatile uintptr_t sink = 0;
    const long wn = n / 5; // wrap/remove is heavier (alloc+free); fewer iterations

    // warm up
    unwrap_class(n / 10, &sink);
    unwrap_plain(n / 10, &sink);
    wrap_remove_class(wn / 10, &sink);

    double cls_ns = best_ns_per_op(reps, n, unwrap_class, &sink);
    double plain_ns = best_ns_per_op(reps, n, unwrap_plain, &sink);
    double scope_ns = best_ns_per_op(reps, n, scope_openclose, &sink);
    double wrap_ns = best_ns_per_op(reps, wn, wrap_remove_class, &sink);

    std::printf("napi_unwrap microbench  (N=%ld, best of %d)\n", n, reps);
    std::printf("  [class] define_class instance : %7.2f ns/op   %8.1f Mops/s\n", cls_ns, 1000.0 / cls_ns);
    std::printf("  [plain] plain object          : %7.2f ns/op   %8.1f Mops/s\n", plain_ns, 1000.0 / plain_ns);
    std::printf("  [scope] open+close handle scope: %6.2f ns/op   %8.1f Mops/s\n", scope_ns, 1000.0 / scope_ns);
    std::printf("  [wrap]  wrap+remove cycle      : %6.2f ns/op   %8.1f Mops/s\n", wrap_ns, 1000.0 / wrap_ns);
    std::printf("  (sink=%llu)\n", static_cast<unsigned long long>(sink));

    CHECK(napi_close_handle_scope(g_env, scope));
    return 0;
}
