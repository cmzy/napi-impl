// End-to-end smoke test for the V8 fast-call backend (napi/fast_call.h).
//
// Mirrors hermes_smoke.cc's embedding style: create platform/runtime/env, wire a
// native handle + fast functions, drive JS, assert. Run via the GN target
// //napi/test:fast_call_smoke. The platform is started with
// --allow-natives-syntax so the script can force TurboFan to optimize the call
// site and deterministically exercise the fast path (otherwise fast vs slow is
// load-dependent).
//
// Covers: A-tier numeric (add) + fast-hit counter + napi_fast_unwrap; B-tier
// bytes (sum a Float32Array via napi_fast_get_buffersource); B-tier object
// (napi_fast_value_unwrap of a wrapped arg); and slow-path equivalence.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "napi/fast_call.h"
#include "napi/js_native_api.h"
#include "napi/node_api.h"
#include "napi_v8/embedding.h"

namespace {

int g_failures = 0;

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        napi_status _s = (expr);                                                                                       \
        if (_s != napi_ok) {                                                                                           \
            std::fprintf(stderr, "FAIL(status %d): %s\n", (int)_s, #expr);                                             \
            return _s;                                                                                                 \
        }                                                                                                              \
    } while (0)

#define EXPECT(cond, msg)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "EXPECT FAILED: %s  (%s)\n", msg, #cond);                                             \
            ++g_failures;                                                                                              \
        } else {                                                                                                       \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
    } while (0)

// Fake native object a handle wraps.
struct Ctx {
    int64_t fast_calls = 0;
    int64_t slow_calls = 0;
    double last_sum = 0;
};

// --- fast callbacks (v8-free) ----------------------------------------------

// Per-class type tags (their addresses are the tokens). kOtherTag simulates a
// different native handle class for the type-confusion test. Use a >=2-aligned
// type (int) — napi_fast_wrap stores the tag as a V8 aligned pointer.
const int kCalcTag = 0;
const int kOtherTag = 0;

double add_fast(napi_fast_recv recv, double a, double b) {
    Ctx* c = static_cast<Ctx*>(napi_fast_unwrap(recv, &kCalcTag));
    if (c) {
        c->fast_calls++;
        c->last_sum = a + b;
    }
    return a + b;
}

double sumf32_fast(napi_fast_recv recv, napi_fast_value arr) {
    Ctx* c = static_cast<Ctx*>(napi_fast_unwrap(recv, &kCalcTag));
    uint8_t scratch[64];
    void* data = nullptr;
    size_t len = 0;
    napi_fast_bs_type elem = napi_fast_bs_unknown;
    if (!c || !napi_fast_get_buffersource(arr, scratch, sizeof scratch, &data, &len, &elem))
        return -1;
    double s = 0;
    if (elem == napi_fast_bs_f32) {
        const float* f = static_cast<const float*>(data);
        for (size_t i = 0, n = len / sizeof(float); i < n; ++i)
            s += f[i];
    }
    c->fast_calls++;
    c->last_sum = s;
    return s;
}

// Returns the last_sum of another wrapped handle passed as an argument.
double peek_fast(napi_fast_recv recv, napi_fast_value other) {
    (void)recv;
    Ctx* o = static_cast<Ctx*>(napi_fast_value_unwrap(other, &kCalcTag));
    return o ? o->last_sum : -999;
}

// Adversarial probe: returns 1 if value_unwrap is NULL, else 0. Used to check
// that unwrapping a define_class instance that was NOT napi_fast_wrap'd (its
// reserved internal field is unset) yields NULL rather than a garbage pointer.
double isnull_fast(napi_fast_recv recv, napi_fast_value v) {
    (void)recv;
    return napi_fast_value_unwrap(v, &kCalcTag) == nullptr ? 1.0 : 0.0;
}

// Receiver-side probe: 1 if napi_fast_unwrap(recv, kCalcTag) is NULL, else 0.
// Drives the type-confusion (wrong tag), non-object-receiver and post-remove_wrap
// checks — all on the optimized fast path.
double selfnull_fast(napi_fast_recv recv) {
    return napi_fast_unwrap(recv, &kCalcTag) == nullptr ? 1.0 : 0.0;
}

// wants_options=true: recover the bound data via the options handle.
double getdata_fast(napi_fast_recv recv, napi_fast_options opts) {
    (void)recv;
    void* d = napi_fast_options_get_data(opts);
    return d ? static_cast<double>(reinterpret_cast<intptr_t>(d)) : -1.0;
}

// Two overloads resolved by V8 on argument count.
double ov1_fast(napi_fast_recv recv, double a) {
    (void)recv;
    return a * 10.0;
}
double ov2_fast(napi_fast_recv recv, double a, double b) {
    (void)recv;
    return a + b;
}

// --- slow fallbacks (standard napi_callback) -------------------------------

napi_value add_slow(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_value thisv = nullptr;
    napi_get_cb_info(env, info, &argc, argv, &thisv, nullptr);
    Ctx* c = nullptr;
    napi_unwrap(env, thisv, reinterpret_cast<void**>(&c));
    double a = 0, b = 0;
    napi_get_value_double(env, argv[0], &a);
    napi_get_value_double(env, argv[1], &b);
    if (c) {
        c->slow_calls++;
        c->last_sum = a + b;
    }
    napi_value r = nullptr;
    napi_create_double(env, a + b, &r);
    return r;
}

napi_value generic_slow(napi_env env, napi_callback_info info) {
    // Minimal slow fallback for sumf32/peek: return 0 (exercised only if V8 does
    // not take the fast path). Correctness of the fast path is asserted directly.
    napi_value r = nullptr;
    napi_create_double(env, 0, &r);
    return r;
    (void)info;
}

// Wrap a fresh Calc instance carrying `ctx` with type tag `tag`, return it.
napi_status make_handle(napi_env env, napi_value cls, Ctx* ctx, const void* tag, napi_value* out) {
    CHECK(napi_new_instance(env, cls, 0, nullptr, out));
    CHECK(napi_fast_wrap(env, *out, ctx, tag, nullptr, nullptr, nullptr));
    return napi_ok;
}

napi_status define_method(napi_env env, napi_value target, const char* name, napi_callback slow,
                          const napi_fast_signature* sig, const void* fast_fn) {
    napi_value fn = nullptr;
    CHECK(napi_create_fast_function(env, name, NAPI_AUTO_LENGTH, slow, sig, fast_fn, nullptr, &fn));
    CHECK(napi_set_named_property(env, target, name, fn));
    return napi_ok;
}

napi_value calc_ctor(napi_env env, napi_callback_info info) {
    napi_value thisv = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisv, nullptr);
    return thisv;
}

napi_status run(napi_env env, const char* code, napi_value* result) {
    napi_value src = nullptr;
    CHECK(napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &src));
    CHECK(napi_run_script(env, src, result));
    return napi_ok;
}

void on_error(const char* msg) { std::fprintf(stderr, "[engine error] %s\n", msg ? msg : "(null)"); }

// Leak amplifier (FASTCALL_CHURN=N): create then drop N fast functions, each in
// its own handle scope so it becomes collectable, periodically forcing GC +
// draining finalizers. If the per-function FastFnHolder / CallbackBundle / two
// References leaked, `leaks` would report ~N identically sized blocks.
napi_status churn(napi_env env, int iters) {
    const napi_fast_type args[] = {napi_fast_receiver, napi_fast_float64, napi_fast_float64};
    const napi_fast_signature sig{napi_fast_float64, 3, args, false};
    for (int i = 0; i < iters; ++i) {
        napi_handle_scope s = nullptr;
        CHECK(napi_open_handle_scope(env, &s));
        napi_value fn = nullptr;
        CHECK(napi_create_fast_function(env, "tmp", NAPI_AUTO_LENGTH, add_slow, &sig,
                                        reinterpret_cast<const void*>(&add_fast), nullptr, &fn));
        CHECK(napi_close_handle_scope(env, s));  // fn now unreferenced -> collectable
        if ((i & 2047) == 0) {
            napi_value r = nullptr;
            CHECK(run(env, "gc()", &r));
            CHECK(napi_v8_run_event_loop_tasks(env));
        }
    }
    napi_value r = nullptr;
    CHECK(run(env, "gc(); gc()", &r));
    CHECK(napi_v8_run_event_loop_tasks(env));
    std::printf("  [churn] created+dropped %d fast functions\n", iters);
    return napi_ok;
}

} // namespace

int main() {
    napi_platform platform = nullptr;
    napi_runtime runtime = nullptr;
    napi_env env = nullptr;

    // --allow-natives-syntax enables %OptimizeFunctionOnNextCall in the script.
    // argv[0] is the program name (SetFlagsFromCommandLine parses from argv[1]).
    char prog[] = "napi";
    char flag[] = "--allow-natives-syntax";
    char* argv[] = {prog, flag};
    CHECK(napi_create_platform(2, argv, 0, nullptr, on_error, false, &platform));
    CHECK(napi_create_runtime(platform, &runtime));
    CHECK(napi_create_env(runtime, &env));

    napi_handle_scope scope = nullptr;
    CHECK(napi_open_handle_scope(env, &scope));

    napi_value cls = nullptr;
    CHECK(napi_define_class(env, "Calc", NAPI_AUTO_LENGTH, calc_ctor, nullptr, 0, nullptr, &cls));

    Ctx calc_ctx;
    Ctx other_ctx;
    other_ctx.last_sum = 42.5;

    napi_value calc = nullptr, other = nullptr;
    CHECK(make_handle(env, cls, &calc_ctx, &kCalcTag, &calc));
    CHECK(make_handle(env, cls, &other_ctx, &kCalcTag, &other));

    // Same Calc class (same JS shape) but wrapped with a DIFFERENT type tag, to
    // simulate a foreign native handle for the type-confusion test (#1).
    Ctx cross_ctx;
    cross_ctx.last_sum = 7.0;
    napi_value cross = nullptr;
    CHECK(make_handle(env, cls, &cross_ctx, &kOtherTag, &cross));

    // A handle to napi_remove_wrap mid-life, for the stale-field test (#3).
    Ctx removable_ctx;
    removable_ctx.last_sum = 5.0;
    napi_value removable = nullptr;
    CHECK(make_handle(env, cls, &removable_ctx, &kCalcTag, &removable));

    // A correctly-tagged handle with NO own JS properties — same hidden class as
    // `cross`/`removable` (which also only carry prototype methods). The fast
    // call site is warmed on `bare` so the SAME-shape `cross`/`removable` stay on
    // the fast path (warming on `calc`, which has own method props, would deopt).
    Ctx bare_ctx;
    napi_value bare = nullptr;
    CHECK(make_handle(env, cls, &bare_ctx, &kCalcTag, &bare));

    const napi_fast_type add_args[] = {napi_fast_receiver, napi_fast_float64, napi_fast_float64};
    const napi_fast_signature add_sig{napi_fast_float64, 3, add_args, false};
    CHECK(define_method(env, calc, "add", add_slow, &add_sig, reinterpret_cast<const void*>(&add_fast)));

    const napi_fast_type one_obj[] = {napi_fast_receiver, napi_fast_jsvalue};
    const napi_fast_signature sumf32_sig{napi_fast_float64, 2, one_obj, false};
    CHECK(define_method(env, calc, "sumf32", generic_slow, &sumf32_sig, reinterpret_cast<const void*>(&sumf32_fast)));

    const napi_fast_signature peek_sig{napi_fast_float64, 2, one_obj, false};
    CHECK(define_method(env, calc, "peek", generic_slow, &peek_sig, reinterpret_cast<const void*>(&peek_fast)));

    const napi_fast_signature isnull_sig{napi_fast_float64, 2, one_obj, false};
    CHECK(define_method(env, calc, "isnull", generic_slow, &isnull_sig, reinterpret_cast<const void*>(&isnull_fast)));

    // wants_options=true: data bound at registration, read in the fast path.
    const napi_fast_type recv_only[] = {napi_fast_receiver};
    const napi_fast_signature getdata_sig{napi_fast_float64, 1, recv_only, /*wants_options=*/true};
    {
        napi_value fn = nullptr;
        CHECK(napi_create_fast_function(env, "getdata", NAPI_AUTO_LENGTH, generic_slow, &getdata_sig,
                                        reinterpret_cast<const void*>(&getdata_fast),
                                        reinterpret_cast<void*>(static_cast<intptr_t>(1234)), &fn));
        CHECK(napi_set_named_property(env, calc, "getdata", fn));
    }

    // Overloads resolved by arg count: ov(a)->a*10, ov(a,b)->a+b.
    const napi_fast_type ov1_args[] = {napi_fast_receiver, napi_fast_float64};
    const napi_fast_type ov2_args[] = {napi_fast_receiver, napi_fast_float64, napi_fast_float64};
    const napi_fast_overload overloads[] = {
            {{napi_fast_float64, 2, ov1_args, false}, reinterpret_cast<const void*>(&ov1_fast)},
            {{napi_fast_float64, 3, ov2_args, false}, reinterpret_cast<const void*>(&ov2_fast)},
    };
    {
        napi_value fn = nullptr;
        CHECK(napi_create_fast_function_overloads(env, "ov", NAPI_AUTO_LENGTH, generic_slow, overloads, 2, nullptr,
                                                  &fn));
        CHECK(napi_set_named_property(env, calc, "ov", fn));
    }

    // selfnull lives on the class PROTOTYPE so calc/cross/removable share it at
    // the same JS shape — that keeps the optimized call site monomorphic so the
    // wrong-tag receiver still hits the fast path (the point of the test).
    napi_value proto = nullptr;
    CHECK(napi_get_named_property(env, cls, "prototype", &proto));
    const napi_fast_signature selfnull_sig{napi_fast_float64, 1, recv_only, false};
    CHECK(define_method(env, proto, "selfnull", generic_slow, &selfnull_sig,
                        reinterpret_cast<const void*>(&selfnull_fast)));

    // A define_class instance that is NOT napi_fast_wrap'd: its reserved internal
    // field 0 is unset. Passing it to value_unwrap probes the UB case.
    napi_value raw = nullptr;
    CHECK(napi_new_instance(env, cls, 0, nullptr, &raw));

    napi_value global = nullptr;
    CHECK(napi_get_global(env, &global));
    CHECK(napi_set_named_property(env, global, "calc", calc));
    CHECK(napi_set_named_property(env, global, "other", other));
    CHECK(napi_set_named_property(env, global, "raw", raw));
    CHECK(napi_set_named_property(env, global, "cross", cross));
    CHECK(napi_set_named_property(env, global, "removable", removable));
    CHECK(napi_set_named_property(env, global, "bare", bare));

    // --- A-tier: numeric add, forced into the fast path -------------------
    {
        const char* js =
                "function hot(o,a,b){return o.add(a,b);}"
                "%PrepareFunctionForOptimization(hot);"
                "hot(calc,1,2); hot(calc,1,2);"
                "%OptimizeFunctionOnNextCall(hot);"
                "var s=0; for (var i=0;i<5;i++) s+=hot(calc,i,1);"
                "s;";
        napi_value r = nullptr;
        CHECK(run(env, js, &r));
        double s = 0;
        CHECK(napi_get_value_double(env, r, &s));
        EXPECT(s == (0 + 1) + (1 + 1) + (2 + 1) + (3 + 1) + (4 + 1), "add returns correct sum");
        EXPECT(calc_ctx.fast_calls > 0, "add took the fast path at least once");
        EXPECT(calc_ctx.last_sum == 5.0, "add_fast wrote ctx->last_sum (unwrap works)");
        std::printf("  [info] add fast=%lld slow=%lld\n", (long long)calc_ctx.fast_calls,
                    (long long)calc_ctx.slow_calls);
    }

    // --- B-tier bytes: sum a Float32Array via get_buffersource -------------
    {
        const char* js =
                "function hotb(o,a){return o.sumf32(a);}"
                "var ta=new Float32Array([1.5,2.0,3.0,4.0]);"
                "%PrepareFunctionForOptimization(hotb);"
                "hotb(calc,ta); hotb(calc,ta);"
                "%OptimizeFunctionOnNextCall(hotb);"
                "hotb(calc,ta);";
        napi_value r = nullptr;
        CHECK(run(env, js, &r));
        double s = 0;
        CHECK(napi_get_value_double(env, r, &s));
        EXPECT(s == 10.5, "sumf32 reads Float32Array bytes correctly");
    }

    // --- B-tier object: unwrap a wrapped argument --------------------------
    {
        const char* js =
                "function hotp(o,x){return o.peek(x);}"
                "%PrepareFunctionForOptimization(hotp);"
                "hotp(calc,other); hotp(calc,other);"
                "%OptimizeFunctionOnNextCall(hotp);"
                "hotp(calc,other);";
        napi_value r = nullptr;
        CHECK(run(env, js, &r));
        double s = 0;
        CHECK(napi_get_value_double(env, r, &s));
        EXPECT(s == 42.5, "peek unwraps the wrapped object argument");
    }

    // --- wants_options: read bound data in the fast path -------------------
    {
        const char* js =
                "function hotd(o){return o.getdata();}"
                "%PrepareFunctionForOptimization(hotd);"
                "hotd(calc); hotd(calc);"
                "%OptimizeFunctionOnNextCall(hotd);"
                "hotd(calc);";
        napi_value r = nullptr;
        CHECK(run(env, js, &r));
        double s = 0;
        CHECK(napi_get_value_double(env, r, &s));
        EXPECT(s == 1234.0, "options_get_data returns the bound data in the fast path");
    }

    // --- overloads: V8 resolves by argument count --------------------------
    {
        const char* js =
                "function hot1(o,a){return o.ov(a);}"
                "function hot2(o,a,b){return o.ov(a,b);}"
                "%PrepareFunctionForOptimization(hot1);%PrepareFunctionForOptimization(hot2);"
                "hot1(calc,5); hot1(calc,5); hot2(calc,5,6); hot2(calc,5,6);"
                "%OptimizeFunctionOnNextCall(hot1);%OptimizeFunctionOnNextCall(hot2);"
                "[hot1(calc,5), hot2(calc,5,6)];";
        napi_value r = nullptr, e0 = nullptr, e1 = nullptr;
        CHECK(run(env, js, &r));
        CHECK(napi_get_element(env, r, 0, &e0));
        CHECK(napi_get_element(env, r, 1, &e1));
        double a = 0, b = 0;
        CHECK(napi_get_value_double(env, e0, &a));
        CHECK(napi_get_value_double(env, e1, &b));
        EXPECT(a == 50.0, "overload ov(a) resolved to the 1-arg fast fn");
        EXPECT(b == 11.0, "overload ov(a,b) resolved to the 2-arg fast fn");
    }

    // --- HARDENING #1: type-tag blocks cross-class confusion on the fast path
    // `ps` is optimized on `calc`; `cross` is the SAME JS shape (Calc instance)
    // so the call site stays monomorphic and `ps(cross)` still hits the fast
    // path — but its tag is kOtherTag, so the unwrap must reject it.
    {
        const char* js =
                "function ps(o){return o.selfnull();}"
                "%PrepareFunctionForOptimization(ps);"
                "ps(bare); ps(bare);"
                "%OptimizeFunctionOnNextCall(ps);"
                "[ps(bare), ps(cross)];";
        napi_value r = nullptr, e0 = nullptr, e1 = nullptr;
        CHECK(run(env, js, &r));
        CHECK(napi_get_element(env, r, 0, &e0));
        CHECK(napi_get_element(env, r, 1, &e1));
        double proper = 0, wrong = 0;
        CHECK(napi_get_value_double(env, e0, &proper));
        CHECK(napi_get_value_double(env, e1, &wrong));
        EXPECT(proper == 0.0, "correctly-tagged receiver unwraps on the fast path");
        EXPECT(wrong == 1.0, "wrong-tag receiver blocked -> NULL (type confusion prevented)");
    }

    // --- HARDENING #3: napi_remove_wrap clears the fast field (no UAF) ------
    // Read the fast slot directly (the helper is a plain internal-field read,
    // valid outside a fast call too). napi_remove_wrap deletes the wrapper
    // private property, which transitions the object's hidden class, so a
    // fast-path probe would just deopt — the field state is what matters here.
    {
        void* before = napi_fast_unwrap(reinterpret_cast<napi_fast_recv>(removable), &kCalcTag);
        EXPECT(before == &removable_ctx, "fast slot holds the native pointer before remove_wrap");
        void* removed = nullptr;
        CHECK(napi_remove_wrap(env, removable, &removed));
        EXPECT(removed == &removable_ctx, "napi_remove_wrap returned the native pointer");
        void* after = napi_fast_unwrap(reinterpret_cast<napi_fast_recv>(removable), &kCalcTag);
        EXPECT(after == nullptr, "after napi_remove_wrap the fast slot reads NULL (no use-after-remove)");
    }

    // --- HARDENING #2: non-object receiver must not crash ------------------
    // Reaching the assertion at all proves no crash (an unguarded read of a
    // primitive's "internal field" would fault). 1 = fast path took the guard;
    // 0 = V8 chose the slow path for the string receiver. Either is acceptable.
    {
        const char* js =
                "String.prototype.selfnull = calc.selfnull;"
                "function psr(s){return s.selfnull();}"
                "%PrepareFunctionForOptimization(psr);"
                "psr('a'); psr('a');"
                "%OptimizeFunctionOnNextCall(psr);"
                "psr('b');";
        napi_value r = nullptr;
        CHECK(run(env, js, &r));
        double s = -1;
        CHECK(napi_get_value_double(env, r, &s));
        std::printf("  [info] non-object receiver selfnull -> %g\n", s);
        EXPECT(s == 0.0 || s == 1.0, "non-object (string) receiver: no crash");
    }

    // --- ADVERSARIAL: unwrap an UNSET internal field (not fast_wrap'd) ------
    // Probes V8 UB: GetAlignedPointerFromInternalField on a reserved-but-unset
    // field. Must yield NULL, not a garbage pointer.
    {
        const char* js =
                "function hotn(o,x){return o.isnull(x);}"
                "%PrepareFunctionForOptimization(hotn);"
                "hotn(calc,raw); hotn(calc,raw);"
                "%OptimizeFunctionOnNextCall(hotn);"
                "[hotn(calc,raw), hotn(calc,null), hotn(calc,other)];";
        napi_value r = nullptr;
        CHECK(run(env, js, &r));
        napi_value e0 = nullptr, e1 = nullptr, e2 = nullptr;
        CHECK(napi_get_element(env, r, 0, &e0));
        CHECK(napi_get_element(env, r, 1, &e1));
        CHECK(napi_get_element(env, r, 2, &e2));
        double raw_null = 0, null_null = 0, other_null = 0;
        CHECK(napi_get_value_double(env, e0, &raw_null));
        CHECK(napi_get_value_double(env, e1, &null_null));
        CHECK(napi_get_value_double(env, e2, &other_null));
        std::printf("  [info] unwrap(raw)->null?=%g unwrap(null)->null?=%g unwrap(other)->null?=%g\n", raw_null,
                    null_null, other_null);
        EXPECT(raw_null == 1.0, "value_unwrap of a non-fast-wrapped instance is NULL (no garbage)");
        EXPECT(null_null == 1.0, "value_unwrap of JS null is NULL");
        EXPECT(other_null == 0.0, "value_unwrap of a fast-wrapped instance is non-NULL");
    }

    if (const char* n = std::getenv("FASTCALL_CHURN"))
        CHECK(churn(env, std::atoi(n)));

    CHECK(napi_close_handle_scope(env, scope));
    CHECK(napi_destroy_env(env));
    CHECK(napi_destroy_runtime(runtime));
    CHECK(napi_destroy_platform(platform));

    std::puts(g_failures == 0 ? "FAST_CALL SMOKE PASS" : "FAST_CALL SMOKE FAIL");
    return g_failures == 0 ? 0 : 2;
}
