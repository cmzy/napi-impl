// End-to-end gtest suite for the V8 fast-call backend (napi/fast_call.h).
//
// Mirrors hermes_smoke.cc's embedding style: create platform/runtime/env, wire a
// native handle + fast functions, drive JS, assert. Built as the GN target
// //napi/test:fast_call_gtest. The platform is started with
// --allow-natives-syntax so the script can force TurboFan to optimize the call
// site and deterministically exercise the fast path (otherwise fast vs slow is
// load-dependent).
//
// V8's platform/runtime/env cannot be created/destroyed per-test
// (v8::Initialize/Dispose is once-per-process), so a single platform + Calc env
// is built once in SetUpTestSuite and shared by every TEST_F. The static members
// hold the wrapped handles, fast methods, fast accessor and globals.
//
// Covers: A-tier numeric (add) + fast-hit counter + napi_fast_unwrap; B-tier
// bytes (sum a Float32Array via napi_fast_get_buffersource); B-tier object
// (napi_fast_value_unwrap of a wrapped arg); wants_options; overloads; the fast
// accessor; the hardening type-tag / stale-field / non-object / forged-object /
// unaligned-tag / NULL-tag cases; and slow-path equivalence.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gtest/gtest.h>

#include "napi/fast_call.h"
#include "napi/js_native_api.h"
#include "napi/node_api.h"
#include "napi_v8/embedding.h"

namespace {

// Returns _s (a napi_status) on failure; used by the file-scope helpers that
// themselves return napi_status. TEST_F bodies use ASSERT_EQ/EXPECT_TRUE.
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        napi_status _s = (expr);                                                                                       \
        if (_s != napi_ok) {                                                                                           \
            std::fprintf(stderr, "FAIL(status %d): %s\n", (int)_s, #expr);                                             \
            return _s;                                                                                                 \
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

// Diagnostic: value_unwrap with NO type tag (the opt-out path). 1 if NULL.
// Used to check whether a TypedArray / plain object (which the tag check would
// otherwise reject) reads a garbage field-0 on the NULL-tag path.
double notag_fast(napi_fast_recv recv, napi_fast_value v) {
    (void)recv;
    return napi_fast_value_unwrap(v, nullptr) == nullptr ? 1.0 : 0.0;
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
    (void)info;
    napi_value r = nullptr;
    napi_create_double(env, 0, &r);
    return r;
}

// --- scalar arg/return type matrix -----------------------------------------
// The other fast methods use only float64 + jsvalue; these cover CTypeOf for
// the remaining scalar kinds (int32/uint32/float32/bool/int64/uint64), as both
// argument and (where representable) return types.
int32_t i32_fast(napi_fast_recv recv, int32_t a, int32_t b) {
    (void)recv;
    return a + b;
}
uint32_t u32_fast(napi_fast_recv recv, uint32_t a) {
    (void)recv;
    return a + 1u;
}
float f32_fast(napi_fast_recv recv, float a) {
    (void)recv;
    return a + 0.5f;
}
bool bool_fast(napi_fast_recv recv, bool a) {
    (void)recv;
    return !a;
}
double i64_fast(napi_fast_recv recv, int64_t a) {
    (void)recv;
    return static_cast<double>(a + 1);
}
double u64_fast(napi_fast_recv recv, uint64_t a) {
    (void)recv;
    return static_cast<double>(a + 1);
}

// --- fast accessor getter (napi_define_fast_accessor) ----------------------
// Both paths return 777.0 so correctness is path-agnostic; the counters reveal
// whether V8 inlined the C fast entry on the accessor access site.
int64_t g_getx_fast = 0;
int64_t g_getx_slow = 0;

double getx_fast(napi_fast_recv recv) {
    (void)recv;
    g_getx_fast++;
    return 777.0;
}

napi_value getx_slow(napi_env env, napi_callback_info info) {
    (void)info;
    g_getx_slow++;
    napi_value r = nullptr;
    napi_create_double(env, 777.0, &r);
    return r;
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

// Leak amplifier: create then drop N fast functions, each in its own handle
// scope so it becomes collectable, periodically forcing GC + draining
// finalizers. If the per-function FastFnHolder / CallbackBundle / two References
// leaked, `leaks` would report ~N identically sized blocks.
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

// ---------------------------------------------------------------------------
// Fixture: one platform / runtime / env / Calc class shared by all tests.
// v8::Initialize/Dispose is once-per-process, so SetUpTestSuite/TearDownTestSuite
// (static, once-per-suite) own that lifecycle; each TEST_F drives shared state.
// ---------------------------------------------------------------------------
class FastCall : public ::testing::Test {
 protected:
    static napi_platform platform_;
    static napi_runtime runtime_;
    static napi_env env_;
    static napi_handle_scope scope_;

    static napi_value cls_;

    // Native objects the handles wrap (shared, mutated by the fast callbacks).
    static Ctx calc_ctx_;
    static Ctx other_ctx_;
    static Ctx cross_ctx_;
    static Ctx removable_ctx_;
    static Ctx bare_ctx_;

    // Wrapped handles + the un-wrapped `raw` instance.
    static napi_value calc_;
    static napi_value other_;
    static napi_value cross_;
    static napi_value removable_;
    static napi_value bare_;
    static napi_value raw_;

    static void SetUpTestSuite() {
        // --allow-natives-syntax enables %OptimizeFunctionOnNextCall in scripts.
        // argv[0] is the program name (SetFlagsFromCommandLine parses from argv[1]).
        char prog[] = "napi";
        char flag[] = "--allow-natives-syntax";
        char* argv[] = {prog, flag};
        ASSERT_EQ(napi_create_platform(2, argv, 0, nullptr, on_error, false, &platform_), napi_ok);
        ASSERT_EQ(napi_create_runtime(platform_, &runtime_), napi_ok);
        ASSERT_EQ(napi_create_env(runtime_, &env_), napi_ok);
        ASSERT_EQ(napi_open_handle_scope(env_, &scope_), napi_ok);

        ASSERT_EQ(napi_define_class(env_, "Calc", NAPI_AUTO_LENGTH, calc_ctor, nullptr, 0, nullptr, &cls_), napi_ok);

        other_ctx_.last_sum = 42.5;
        ASSERT_EQ(make_handle(env_, cls_, &calc_ctx_, &kCalcTag, &calc_), napi_ok);
        ASSERT_EQ(make_handle(env_, cls_, &other_ctx_, &kCalcTag, &other_), napi_ok);

        // Same Calc class (same JS shape) but wrapped with a DIFFERENT type tag,
        // to simulate a foreign native handle for the type-confusion test (#1).
        cross_ctx_.last_sum = 7.0;
        ASSERT_EQ(make_handle(env_, cls_, &cross_ctx_, &kOtherTag, &cross_), napi_ok);

        // A handle to napi_remove_wrap mid-life, for the stale-field test (#3).
        removable_ctx_.last_sum = 5.0;
        ASSERT_EQ(make_handle(env_, cls_, &removable_ctx_, &kCalcTag, &removable_), napi_ok);

        // A correctly-tagged handle with NO own JS properties — same hidden class
        // as `cross`/`removable` (which also only carry prototype methods). The
        // fast call site is warmed on `bare` so the SAME-shape `cross`/`removable`
        // stay on the fast path (warming on `calc`, which has own method props,
        // would deopt).
        ASSERT_EQ(make_handle(env_, cls_, &bare_ctx_, &kCalcTag, &bare_), napi_ok);

        const napi_fast_type add_args[] = {napi_fast_receiver, napi_fast_float64, napi_fast_float64};
        const napi_fast_signature add_sig{napi_fast_float64, 3, add_args, false};
        ASSERT_EQ(define_method(env_, calc_, "add", add_slow, &add_sig, reinterpret_cast<const void*>(&add_fast)),
                  napi_ok);

        const napi_fast_type one_obj[] = {napi_fast_receiver, napi_fast_jsvalue};
        const napi_fast_signature sumf32_sig{napi_fast_float64, 2, one_obj, false};
        ASSERT_EQ(
                define_method(env_, calc_, "sumf32", generic_slow, &sumf32_sig, reinterpret_cast<const void*>(&sumf32_fast)),
                napi_ok);

        const napi_fast_signature peek_sig{napi_fast_float64, 2, one_obj, false};
        ASSERT_EQ(define_method(env_, calc_, "peek", generic_slow, &peek_sig, reinterpret_cast<const void*>(&peek_fast)),
                  napi_ok);

        const napi_fast_signature isnull_sig{napi_fast_float64, 2, one_obj, false};
        ASSERT_EQ(
                define_method(env_, calc_, "isnull", generic_slow, &isnull_sig, reinterpret_cast<const void*>(&isnull_fast)),
                napi_ok);

        const napi_fast_signature notag_sig{napi_fast_float64, 2, one_obj, false};
        ASSERT_EQ(
                define_method(env_, calc_, "notag", generic_slow, &notag_sig, reinterpret_cast<const void*>(&notag_fast)),
                napi_ok);

        // wants_options=true: data bound at registration, read in the fast path.
        const napi_fast_type recv_only[] = {napi_fast_receiver};
        const napi_fast_signature getdata_sig{napi_fast_float64, 1, recv_only, /*wants_options=*/true};
        {
            napi_value fn = nullptr;
            ASSERT_EQ(napi_create_fast_function(env_, "getdata", NAPI_AUTO_LENGTH, generic_slow, &getdata_sig,
                                                reinterpret_cast<const void*>(&getdata_fast),
                                                reinterpret_cast<void*>(static_cast<intptr_t>(1234)), &fn),
                      napi_ok);
            ASSERT_EQ(napi_set_named_property(env_, calc_, "getdata", fn), napi_ok);
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
            ASSERT_EQ(napi_create_fast_function_overloads(env_, "ov", NAPI_AUTO_LENGTH, generic_slow, overloads, 2,
                                                          nullptr, &fn),
                      napi_ok);
            ASSERT_EQ(napi_set_named_property(env_, calc_, "ov", fn), napi_ok);
        }

        // selfnull lives on the class PROTOTYPE so calc/cross/removable share it
        // at the same JS shape — that keeps the optimized call site monomorphic so
        // the wrong-tag receiver still hits the fast path (the point of the test).
        napi_value proto = nullptr;
        ASSERT_EQ(napi_get_named_property(env_, cls_, "prototype", &proto), napi_ok);
        const napi_fast_signature selfnull_sig{napi_fast_float64, 1, recv_only, false};
        ASSERT_EQ(define_method(env_, proto, "selfnull", generic_slow, &selfnull_sig,
                                reinterpret_cast<const void*>(&selfnull_fast)),
                  napi_ok);

        // A fast getter installed via napi_define_fast_accessor (the FastAccessor
        // test reads `calc.fastx`).
        {
            napi_value getter = nullptr;
            const napi_fast_signature getx_sig{napi_fast_float64, 1, recv_only, false};
            ASSERT_EQ(napi_create_fast_function(env_, "getx", NAPI_AUTO_LENGTH, getx_slow, &getx_sig,
                                                reinterpret_cast<const void*>(&getx_fast), nullptr, &getter),
                      napi_ok);
            // napi_define_fast_accessor takes a napi_value name (not const char*).
            napi_value fastx_name = nullptr;
            ASSERT_EQ(napi_create_string_utf8(env_, "fastx", NAPI_AUTO_LENGTH, &fastx_name), napi_ok);
            ASSERT_EQ(napi_define_fast_accessor(env_, calc_, fastx_name, getter, nullptr, napi_default), napi_ok);
        }

        // A define_class instance that is NOT napi_fast_wrap'd: its reserved
        // internal field 0 is unset. Passing it to value_unwrap probes the UB case.
        ASSERT_EQ(napi_new_instance(env_, cls_, 0, nullptr, &raw_), napi_ok);

        napi_value global = nullptr;
        ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
        ASSERT_EQ(napi_set_named_property(env_, global, "calc", calc_), napi_ok);
        ASSERT_EQ(napi_set_named_property(env_, global, "other", other_), napi_ok);
        ASSERT_EQ(napi_set_named_property(env_, global, "raw", raw_), napi_ok);
        ASSERT_EQ(napi_set_named_property(env_, global, "cross", cross_), napi_ok);
        ASSERT_EQ(napi_set_named_property(env_, global, "removable", removable_), napi_ok);
        ASSERT_EQ(napi_set_named_property(env_, global, "bare", bare_), napi_ok);
    }

    static void TearDownTestSuite() {
        if (scope_)
            napi_close_handle_scope(env_, scope_);
        if (env_)
            napi_destroy_env(env_);
        if (runtime_)
            napi_destroy_runtime(runtime_);
        if (platform_)
            napi_destroy_platform(platform_);
        scope_ = nullptr;
        env_ = nullptr;
        runtime_ = nullptr;
        platform_ = nullptr;
    }
};

napi_platform FastCall::platform_ = nullptr;
napi_runtime FastCall::runtime_ = nullptr;
napi_env FastCall::env_ = nullptr;
napi_handle_scope FastCall::scope_ = nullptr;
napi_value FastCall::cls_ = nullptr;
Ctx FastCall::calc_ctx_;
Ctx FastCall::other_ctx_;
Ctx FastCall::cross_ctx_;
Ctx FastCall::removable_ctx_;
Ctx FastCall::bare_ctx_;
napi_value FastCall::calc_ = nullptr;
napi_value FastCall::other_ = nullptr;
napi_value FastCall::cross_ = nullptr;
napi_value FastCall::removable_ = nullptr;
napi_value FastCall::bare_ = nullptr;
napi_value FastCall::raw_ = nullptr;

// --- A-tier: numeric add, forced into the fast path ------------------------
TEST_F(FastCall, AddFastPath) {
    const char* js =
            "function hot(o,a,b){return o.add(a,b);}"
            "%PrepareFunctionForOptimization(hot);"
            "hot(calc,1,2); hot(calc,1,2);"
            "%OptimizeFunctionOnNextCall(hot);"
            "var s=0; for (var i=0;i<5;i++) s+=hot(calc,i,1);"
            "s;";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    double s = 0;
    ASSERT_EQ(napi_get_value_double(env_, r, &s), napi_ok);
    EXPECT_TRUE(s == (0 + 1) + (1 + 1) + (2 + 1) + (3 + 1) + (4 + 1)) << "add returns correct sum";
    EXPECT_TRUE(calc_ctx_.fast_calls > 0) << "add took the fast path at least once";
    EXPECT_TRUE(calc_ctx_.last_sum == 5.0) << "add_fast wrote ctx->last_sum (unwrap works)";
    std::printf("  [info] add fast=%lld slow=%lld\n", (long long)calc_ctx_.fast_calls,
                (long long)calc_ctx_.slow_calls);
}

// --- B-tier bytes: sum a Float32Array via get_buffersource -----------------
TEST_F(FastCall, Sumf32Bytes) {
    const char* js =
            "function hotb(o,a){return o.sumf32(a);}"
            "var ta=new Float32Array([1.5,2.0,3.0,4.0]);"
            "%PrepareFunctionForOptimization(hotb);"
            "hotb(calc,ta); hotb(calc,ta);"
            "%OptimizeFunctionOnNextCall(hotb);"
            "hotb(calc,ta);";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    double s = 0;
    ASSERT_EQ(napi_get_value_double(env_, r, &s), napi_ok);
    EXPECT_TRUE(s == 10.5) << "sumf32 reads Float32Array bytes correctly";
}

// --- B-tier object: unwrap a wrapped argument ------------------------------
TEST_F(FastCall, PeekUnwrapsArgument) {
    const char* js =
            "function hotp(o,x){return o.peek(x);}"
            "%PrepareFunctionForOptimization(hotp);"
            "hotp(calc,other); hotp(calc,other);"
            "%OptimizeFunctionOnNextCall(hotp);"
            "hotp(calc,other);";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    double s = 0;
    ASSERT_EQ(napi_get_value_double(env_, r, &s), napi_ok);
    EXPECT_TRUE(s == 42.5) << "peek unwraps the wrapped object argument";
}

// --- wants_options: read bound data in the fast path -----------------------
TEST_F(FastCall, WantsOptions) {
    const char* js =
            "function hotd(o){return o.getdata();}"
            "%PrepareFunctionForOptimization(hotd);"
            "hotd(calc); hotd(calc);"
            "%OptimizeFunctionOnNextCall(hotd);"
            "hotd(calc);";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    double s = 0;
    ASSERT_EQ(napi_get_value_double(env_, r, &s), napi_ok);
    EXPECT_TRUE(s == 1234.0) << "options_get_data returns the bound data in the fast path";
}

// --- overloads: V8 resolves by argument count ------------------------------
TEST_F(FastCall, Overloads) {
    const char* js =
            "function hot1(o,a){return o.ov(a);}"
            "function hot2(o,a,b){return o.ov(a,b);}"
            "%PrepareFunctionForOptimization(hot1);%PrepareFunctionForOptimization(hot2);"
            "hot1(calc,5); hot1(calc,5); hot2(calc,5,6); hot2(calc,5,6);"
            "%OptimizeFunctionOnNextCall(hot1);%OptimizeFunctionOnNextCall(hot2);"
            "[hot1(calc,5), hot2(calc,5,6)];";
    napi_value r = nullptr, e0 = nullptr, e1 = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 0, &e0), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 1, &e1), napi_ok);
    double a = 0, b = 0;
    ASSERT_EQ(napi_get_value_double(env_, e0, &a), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e1, &b), napi_ok);
    EXPECT_TRUE(a == 50.0) << "overload ov(a) resolved to the 1-arg fast fn";
    EXPECT_TRUE(b == 11.0) << "overload ov(a,b) resolved to the 2-arg fast fn";
}

// --- fast accessor: a fast getter installed via napi_define_fast_accessor.
// V8 reaches the C fast entry of an accessor getter on optimized monomorphic
// access. Correctness is path-agnostic (both return 777); the counters show
// whether the fast entry was taken (accessor inlining is V8-version-sensitive,
// so we report it rather than require it).
TEST_F(FastCall, FastAccessor) {
    g_getx_fast = 0;  // make the "ran 7×" assertion robust to test order/reruns
    g_getx_slow = 0;
    const char* js =
            "function hotg(o){return o.fastx;}"
            "%PrepareFunctionForOptimization(hotg);"
            "hotg(calc); hotg(calc);"
            "%OptimizeFunctionOnNextCall(hotg);"
            "var s=0; for (var i=0;i<5;i++) s+=hotg(calc);"
            "s;";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    double s = 0;
    ASSERT_EQ(napi_get_value_double(env_, r, &s), napi_ok);
    EXPECT_TRUE(s == 777.0 * 5) << "fast accessor getter returns the correct value";
    EXPECT_TRUE(g_getx_fast + g_getx_slow == 7) << "accessor getter ran for every read (2 warmup + 5 loop)";
    std::printf("  [info] fast accessor getter fast=%lld slow=%lld\n", (long long)g_getx_fast, (long long)g_getx_slow);
}

// --- HARDENING #1: type-tag blocks cross-class confusion on the fast path.
// `ps` is optimized on `bare`; `cross` is the SAME JS shape (Calc instance) so
// the call site stays monomorphic and `ps(cross)` still hits the fast path —
// but its tag is kOtherTag, so the unwrap must reject it.
TEST_F(FastCall, TypeTagBlocksConfusion) {
    const char* js =
            "function ps(o){return o.selfnull();}"
            "%PrepareFunctionForOptimization(ps);"
            "ps(bare); ps(bare);"
            "%OptimizeFunctionOnNextCall(ps);"
            "[ps(bare), ps(cross)];";
    napi_value r = nullptr, e0 = nullptr, e1 = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 0, &e0), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 1, &e1), napi_ok);
    double proper = 0, wrong = 0;
    ASSERT_EQ(napi_get_value_double(env_, e0, &proper), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e1, &wrong), napi_ok);
    EXPECT_TRUE(proper == 0.0) << "correctly-tagged receiver unwraps on the fast path";
    EXPECT_TRUE(wrong == 1.0) << "wrong-tag receiver blocked -> NULL (type confusion prevented)";
}

// --- HARDENING #3: napi_remove_wrap clears the fast field (no UAF).
// Read the fast slot directly (the helper is a plain internal-field read, valid
// outside a fast call too). napi_remove_wrap deletes the wrapper private
// property, which transitions the object's hidden class, so a fast-path probe
// would just deopt — the field state is what matters here.
TEST_F(FastCall, RemoveWrapClearsField) {
    void* before = napi_fast_unwrap(reinterpret_cast<napi_fast_recv>(removable_), &kCalcTag);
    EXPECT_TRUE(before == &removable_ctx_) << "fast slot holds the native pointer before remove_wrap";
    void* removed = nullptr;
    ASSERT_EQ(napi_remove_wrap(env_, removable_, &removed), napi_ok);
    EXPECT_TRUE(removed == &removable_ctx_) << "napi_remove_wrap returned the native pointer";
    void* after = napi_fast_unwrap(reinterpret_cast<napi_fast_recv>(removable_), &kCalcTag);
    EXPECT_TRUE(after == nullptr) << "after napi_remove_wrap the fast slot reads NULL (no use-after-remove)";
}

// --- HARDENING #2: non-object receiver must not crash.
// Reaching the assertion at all proves no crash (an unguarded read of a
// primitive's "internal field" would fault). 1 = fast path took the guard;
// 0 = V8 chose the slow path for the string receiver. Either is acceptable.
TEST_F(FastCall, NonObjectReceiver) {
    const char* js =
            "String.prototype.selfnull = calc.selfnull;"
            "function psr(s){return s.selfnull();}"
            "%PrepareFunctionForOptimization(psr);"
            "psr('a'); psr('a');"
            "%OptimizeFunctionOnNextCall(psr);"
            "psr('b');";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    double s = -1;
    ASSERT_EQ(napi_get_value_double(env_, r, &s), napi_ok);
    std::printf("  [info] non-object receiver selfnull -> %g\n", s);
    EXPECT_TRUE(s == 0.0 || s == 1.0) << "non-object (string) receiver: no crash";
}

// --- HARDENING: forged object via Object.create (0 internal fields).
// A page can do Object.create(handle.__proto__) to get an object that has the
// prototype's fast methods but NO internal fields. The fast path must treat it
// as "no native slot" (NULL), never read field 0 of a zero-field object.
TEST_F(FastCall, ObjectCreateForgery) {
    const char* js =
            "var fake = Object.create(Object.getPrototypeOf(bare));"
            "function pf(o){return o.selfnull();}"
            "%PrepareFunctionForOptimization(pf);"
            "pf(fake); pf(fake);"
            "%OptimizeFunctionOnNextCall(pf);"
            "pf(fake);";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    double s = -1;
    ASSERT_EQ(napi_get_value_double(env_, r, &s), napi_ok);
    std::printf("  [info] Object.create forgery selfnull -> %g\n", s);
    EXPECT_TRUE(s == 1.0) << "forged (0-field) receiver unwraps to NULL, no crash";
}

// --- HARDENING: napi_fast_wrap rejects an unaligned type_tag ---------------
TEST_F(FastCall, UnalignedTypeTagRejected) {
    const void* bad_tag = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(&kCalcTag) | 1u);
    napi_value tmp = nullptr;
    ASSERT_EQ(napi_new_instance(env_, cls_, 0, nullptr, &tmp), napi_ok);
    napi_status st = napi_fast_wrap(env_, tmp, &calc_ctx_, bad_tag, nullptr, nullptr, nullptr);
    EXPECT_TRUE(st == napi_invalid_arg) << "napi_fast_wrap rejects an unaligned type_tag (no V8 fatal)";
}

// --- DIAGNOSTIC: does the NULL-tag (opt-out) unwrap stay safe on a
// TypedArray / plain object (which V8 may give internal fields to)?
TEST_F(FastCall, NullTagUnwrapSafe) {
    const char* js =
            "function pn(o,x){return o.notag(x);}"
            "var ta=new Float32Array([1,2,3,4]); var po={};"
            "%PrepareFunctionForOptimization(pn);"
            "pn(calc,ta); pn(calc,ta);"
            "%OptimizeFunctionOnNextCall(pn);"
            "[pn(calc,ta), pn(calc,po)];";
    napi_value r = nullptr, e0 = nullptr, e1 = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 0, &e0), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 1, &e1), napi_ok);
    double ta_null = 0, po_null = 0;
    ASSERT_EQ(napi_get_value_double(env_, e0, &ta_null), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e1, &po_null), napi_ok);
    std::printf("  [info] NULL-tag unwrap: Float32Array->null?=%g plainObject->null?=%g\n", ta_null, po_null);
    EXPECT_TRUE(ta_null == 1.0) << "NULL-tag unwrap of a TypedArray is NULL (no garbage field-0 read)";
    EXPECT_TRUE(po_null == 1.0) << "NULL-tag unwrap of a plain object is NULL";
}

// --- ADVERSARIAL: unwrap an UNSET internal field (not fast_wrap'd).
// Probes V8 UB: GetAlignedPointerFromInternalField on a reserved-but-unset
// field. Must yield NULL, not a garbage pointer.
TEST_F(FastCall, UnsetInternalField) {
    const char* js =
            "function hotn(o,x){return o.isnull(x);}"
            "%PrepareFunctionForOptimization(hotn);"
            "hotn(calc,raw); hotn(calc,raw);"
            "%OptimizeFunctionOnNextCall(hotn);"
            "[hotn(calc,raw), hotn(calc,null), hotn(calc,other)];";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    napi_value e0 = nullptr, e1 = nullptr, e2 = nullptr;
    ASSERT_EQ(napi_get_element(env_, r, 0, &e0), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 1, &e1), napi_ok);
    ASSERT_EQ(napi_get_element(env_, r, 2, &e2), napi_ok);
    double raw_null = 0, null_null = 0, other_null = 0;
    ASSERT_EQ(napi_get_value_double(env_, e0, &raw_null), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e1, &null_null), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e2, &other_null), napi_ok);
    std::printf("  [info] unwrap(raw)->null?=%g unwrap(null)->null?=%g unwrap(other)->null?=%g\n", raw_null, null_null,
                other_null);
    EXPECT_TRUE(raw_null == 1.0) << "value_unwrap of a non-fast-wrapped instance is NULL (no garbage)";
    EXPECT_TRUE(null_null == 1.0) << "value_unwrap of JS null is NULL";
    EXPECT_TRUE(other_null == 0.0) << "value_unwrap of a fast-wrapped instance is non-NULL";
}

// --- scalar type matrix: register one fast fn per scalar kind (drives CTypeOf
// for each), then force the fast path and check the C entry's result.
TEST_F(FastCall, ScalarArgTypes) {
    napi_value g = nullptr;
    ASSERT_EQ(napi_get_global(env_, &g), napi_ok);

    const napi_fast_type i32a[] = {napi_fast_receiver, napi_fast_int32, napi_fast_int32};
    const napi_fast_signature i32s{napi_fast_int32, 3, i32a, false};
    ASSERT_EQ(define_method(env_, g, "fi32", generic_slow, &i32s, reinterpret_cast<const void*>(&i32_fast)), napi_ok);
    const napi_fast_type u32a[] = {napi_fast_receiver, napi_fast_uint32};
    const napi_fast_signature u32s{napi_fast_uint32, 2, u32a, false};
    ASSERT_EQ(define_method(env_, g, "fu32", generic_slow, &u32s, reinterpret_cast<const void*>(&u32_fast)), napi_ok);
    const napi_fast_type f32a[] = {napi_fast_receiver, napi_fast_float32};
    const napi_fast_signature f32s{napi_fast_float32, 2, f32a, false};
    ASSERT_EQ(define_method(env_, g, "ff32", generic_slow, &f32s, reinterpret_cast<const void*>(&f32_fast)), napi_ok);
    const napi_fast_type boola[] = {napi_fast_receiver, napi_fast_bool};
    const napi_fast_signature bools{napi_fast_bool, 2, boola, false};
    ASSERT_EQ(define_method(env_, g, "fbool", generic_slow, &bools, reinterpret_cast<const void*>(&bool_fast)), napi_ok);
    const napi_fast_type i64a[] = {napi_fast_receiver, napi_fast_int64};
    const napi_fast_signature i64s{napi_fast_float64, 2, i64a, false};
    ASSERT_EQ(define_method(env_, g, "fi64", generic_slow, &i64s, reinterpret_cast<const void*>(&i64_fast)), napi_ok);
    const napi_fast_type u64a[] = {napi_fast_receiver, napi_fast_uint64};
    const napi_fast_signature u64s{napi_fast_float64, 2, u64a, false};
    ASSERT_EQ(define_method(env_, g, "fu64", generic_slow, &u64s, reinterpret_cast<const void*>(&u64_fast)), napi_ok);

    const char* js =
            "function hi(a,b){return fi32(a,b);} function hu(a){return fu32(a);}"
            "function hf(a){return ff32(a);} function hb(a){return fbool(a);}"
            "function h6(a){return fi64(a);} function h7(a){return fu64(a);}"
            "for (const w of [hi,hu,hf,hb,h6,h7]) %PrepareFunctionForOptimization(w);"
            "hi(1,2);hu(1);hf(1);hb(true);h6(1);h7(1);"
            "hi(1,2);hu(1);hf(1);hb(true);h6(1);h7(1);"
            "for (const w of [hi,hu,hf,hb,h6,h7]) %OptimizeFunctionOnNextCall(w);"
            "[hi(10,20), hu(41), hf(1.5), hb(false), h6(100), h7(200)];";
    napi_value r = nullptr;
    ASSERT_EQ(run(env_, js, &r), napi_ok);
    napi_value e[6];
    for (int i = 0; i < 6; ++i)
        ASSERT_EQ(napi_get_element(env_, r, i, &e[i]), napi_ok);
    double i32v = 0, u32v = 0, f32v = 0, i64v = 0, u64v = 0;
    bool bv = false;
    ASSERT_EQ(napi_get_value_double(env_, e[0], &i32v), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e[1], &u32v), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e[2], &f32v), napi_ok);
    ASSERT_EQ(napi_get_value_bool(env_, e[3], &bv), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e[4], &i64v), napi_ok);
    ASSERT_EQ(napi_get_value_double(env_, e[5], &u64v), napi_ok);
    EXPECT_EQ(i32v, 30.0) << "int32 fast arg+return";
    EXPECT_EQ(u32v, 42.0) << "uint32 fast arg+return";
    EXPECT_EQ(f32v, 2.0) << "float32 fast arg+return";
    EXPECT_TRUE(bv) << "bool fast arg+return (!false)";
    EXPECT_EQ(i64v, 101.0) << "int64 fast arg";
    EXPECT_EQ(u64v, 201.0) << "uint64 fast arg";
}

// --- Leak amplifier: create+drop a batch of fast functions, force GC, drain
// finalizers. Exercises the create/drop path; a fixed modest iteration count.
TEST_F(FastCall, Churn) {
    ASSERT_EQ(churn(env_, 1000), napi_ok);
}

} // namespace
