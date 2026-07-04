// Tests for the engine-neutral compilation-cache API (napi/script_cache.h):
// napi_run_script_cached / napi_free_script_cache. Real code cache on V8 (this
// build), so these assert the produce/consume path; the contract also holds on
// the fallback backends (no blob produced, provided blobs rejected).

#include "napi_gtest_fixture.h"

#include "napi/script_cache.h"

namespace {

// Run `code` with the cache API; returns the completion value. Optionally captures
// a produced cache blob (caller frees) and/or consumes one.
napi_value RunCached(napi_env env, const char *code, const uint8_t *in, size_t in_len, bool *rejected, uint8_t **out,
                     size_t *out_len) {
    napi_value src = nullptr;
    EXPECT_EQ(napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &src), napi_ok);
    napi_value result = nullptr;
    EXPECT_EQ(napi_run_script_cached(env, src, "test.js", 7, in, in_len, rejected, out, out_len, &result), napi_ok);
    return result;
}

// Produce a cache blob for `code`, then consume it: result is correct and the
// blob is accepted (not rejected). On V8 a non-empty blob is produced.
TEST_F(NapiExtras, ScriptCacheProduceThenConsume) {
    const char *code = "(function () { let s = 0; for (let i = 0; i < 10; i++) s += i; return s; })()";

    uint8_t *blob = nullptr;
    size_t blob_len = 0;
    napi_value r1 = RunCached(env_, code, nullptr, 0, nullptr, &blob, &blob_len);
    int32_t v1 = 0;
    ASSERT_EQ(napi_get_value_int32(env_, r1, &v1), napi_ok);
    EXPECT_EQ(v1, 45);

#if defined(NAPI_HAS_SCRIPT_CACHE)
    ASSERT_NE(blob, nullptr);
    ASSERT_GT(blob_len, 0u);

    // Consume the produced blob in a fresh env (same runtime): accepted + correct.
    napi_env env2 = nullptr;
    ASSERT_EQ(napi_create_env(runtime_, &env2), napi_ok);
    napi_handle_scope scope2 = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env2, &scope2), napi_ok);
    bool rejected = true;
    napi_value r2 = RunCached(env2, code, blob, blob_len, &rejected, nullptr, nullptr);
    EXPECT_FALSE(rejected);  // cache accepted (parse/compile skipped)
    int32_t v2 = 0;
    ASSERT_EQ(napi_get_value_int32(env2, r2, &v2), napi_ok);
    EXPECT_EQ(v2, 45);
    napi_close_handle_scope(env2, scope2);
    napi_destroy_env(env2);
#endif
    napi_free_script_cache(env_, blob);
}

// A garbage/incompatible cache is rejected, but the script still runs correctly
// (silent recompile from source).
TEST_F(NapiExtras, ScriptCacheRejectsGarbageButStillRuns) {
    const char *code = "1 + 2 + 3";
    uint8_t garbage[64];
    for (size_t i = 0; i < sizeof(garbage); i++)
        garbage[i] = static_cast<uint8_t>(i * 7 + 1);
    bool rejected = false;
    napi_value r = RunCached(env_, code, garbage, sizeof(garbage), &rejected, nullptr, nullptr);
    EXPECT_TRUE(rejected);  // bad cache -> recompiled
    int32_t v = 0;
    ASSERT_EQ(napi_get_value_int32(env_, r, &v), napi_ok);
    EXPECT_EQ(v, 6);  // result correct despite rejected cache
}

// No cache in/out: behaves like a plain run.
TEST_F(NapiExtras, ScriptCacheNoCacheRunsNormally) {
    napi_value r = RunCached(env_, "'a' + 'b' + 'c'", nullptr, 0, nullptr, nullptr, nullptr);
    char buf[8] = {0};
    size_t len = 0;
    ASSERT_EQ(napi_get_value_string_utf8(env_, r, buf, sizeof(buf), &len), napi_ok);
    EXPECT_STREQ(buf, "abc");
}

TEST_F(NapiExtras, ScriptCacheRejectsNonString) {
    napi_value num = nullptr;
    napi_create_int32(env_, 5, &num);
    napi_value result = nullptr;
    EXPECT_EQ(napi_run_script_cached(env_, num, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, &result),
              napi_string_expected);
}

TEST_F(NapiExtras, ScriptCacheThrowingScriptSurfacesException) {
    napi_value src = nullptr;
    napi_create_string_utf8(env_, "throw new Error('boom')", NAPI_AUTO_LENGTH, &src);
    napi_value result = nullptr;
    EXPECT_EQ(napi_run_script_cached(env_, src, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, &result),
              napi_pending_exception);
    napi_value ex = nullptr;
    EXPECT_EQ(napi_get_and_clear_last_exception(env_, &ex), napi_ok);
    EXPECT_NE(ex, nullptr);
}

TEST_F(NapiExtras, ScriptCacheFreeNullIsNoOp) {
    EXPECT_EQ(napi_free_script_cache(env_, nullptr), napi_ok);
}

// The result out-param is optional (run for side effects only).
TEST_F(NapiExtras, ScriptCacheNullResultOk) {
    napi_value src = nullptr;
    napi_create_string_utf8(env_, "globalThis.__sc_side = 99", NAPI_AUTO_LENGTH, &src);
    EXPECT_EQ(napi_run_script_cached(env_, src, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr), napi_ok);
    EXPECT_TRUE(Truthy(Run("globalThis.__sc_side === 99")));
}

}  // namespace
