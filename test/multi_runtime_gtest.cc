// gtest: same-thread multiple-runtime support (the "B 档" capability).
//
// A napi_runtime wraps one V8 isolate (independent heap). Scenario A — one
// runtime per thread — already works and is exercised by every other suite.
// This file targets Scenario B: two (or more) runtimes LIVE ON THE SAME THREAD
// at once, plus non-LIFO teardown and churn.
//
// The enter-once model (napi_create_env leaves the isolate + context Entered
// until destroy) cannot support Scenario B: the second create_env nests a
// second isolate Enter on top of the first, so v8::Isolate::GetCurrent() no
// longer matches the env being called into, and the entered-isolate stack must
// be Exited strictly LIFO — which arbitrary env lifetimes violate. The fix is
// per-call self-scoping: every napi entry re-enters its own isolate+context for
// the duration of the call, and no isolate is left Entered between calls.
//
// Uses the embedding API directly (NOT the shared NapiExtras fixture, which
// creates a single runtime per process) so each test drives its own
// runtime/env lifecycle. gtest_main provides main().

#include <gtest/gtest.h>

#include <thread>

#include "napi/js_native_api.h"
#include "napi_v8/embedding.h"

namespace {

// One process-wide platform (V8 platform/init is once-per-process). Leaked at
// exit like the shared fixture does.
napi_platform GetPlatform() {
  static napi_platform p = [] {
    napi_platform pf = nullptr;
    EXPECT_EQ(napi_create_platform(0, nullptr, 0, nullptr, nullptr, false, &pf), napi_ok);
    return pf;
  }();
  return p;
}

// Do enough real V8 work on `env` to trip the wrong-current-isolate bug: open a
// handle scope, compile+run a script, create an object, wrap/unwrap a native
// pointer (exercises the type_tag/wrapper Privates that read the *current*
// isolate), and round-trip a global property.
void ExerciseEnv(napi_env env, int tag) {
  napi_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_handle_scope(env, &scope), napi_ok);

  napi_value src = nullptr, res = nullptr;
  ASSERT_EQ(napi_create_string_utf8(env, "1 + 2", NAPI_AUTO_LENGTH, &src), napi_ok);
  ASSERT_EQ(napi_run_script(env, src, &res), napi_ok);
  int32_t n = -1;
  ASSERT_EQ(napi_get_value_int32(env, res, &n), napi_ok);
  EXPECT_EQ(n, 3);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env, &obj), napi_ok);
  static thread_local int natives[8];
  int* native = &natives[tag & 7];
  *native = 0xB000 + tag;
  ASSERT_EQ(napi_wrap(env, obj, native, nullptr, nullptr, nullptr), napi_ok);
  void* out = nullptr;
  ASSERT_EQ(napi_unwrap(env, obj, &out), napi_ok);
  EXPECT_EQ(out, native);

  napi_value g = nullptr;
  ASSERT_EQ(napi_get_global(env, &g), napi_ok);
  napi_value marker = nullptr;
  ASSERT_EQ(napi_create_int32(env, 100 + tag, &marker), napi_ok);
  ASSERT_EQ(napi_set_named_property(env, g, "marker", marker), napi_ok);
  napi_value mk = nullptr;
  ASSERT_EQ(napi_get_named_property(env, g, "marker", &mk), napi_ok);
  int32_t mn = -1;
  ASSERT_EQ(napi_get_value_int32(env, mk, &mn), napi_ok);
  EXPECT_EQ(mn, 100 + tag);

  ASSERT_EQ(napi_close_handle_scope(env, scope), napi_ok);
}

// Two runtimes live at once on THIS thread, used interleaved.
TEST(MultiRuntime, TwoRuntimesSameThread) {
  napi_platform pf = GetPlatform();
  napi_runtime rt1 = nullptr, rt2 = nullptr;
  ASSERT_EQ(napi_create_runtime(pf, &rt1), napi_ok);
  ASSERT_EQ(napi_create_runtime(pf, &rt2), napi_ok);
  napi_env e1 = nullptr, e2 = nullptr;
  ASSERT_EQ(napi_create_env(rt1, &e1), napi_ok);
  ASSERT_EQ(napi_create_env(rt2, &e2), napi_ok);

  ExerciseEnv(e1, 1);
  ExerciseEnv(e2, 2);
  ExerciseEnv(e1, 1);  // back to e1 while e2 is still live
  ExerciseEnv(e2, 2);

  napi_v8_run_event_loop_tasks(e1);
  napi_v8_run_event_loop_tasks(e2);

  ASSERT_EQ(napi_destroy_env(e1), napi_ok);
  ASSERT_EQ(napi_destroy_env(e2), napi_ok);
  ASSERT_EQ(napi_destroy_runtime(rt1), napi_ok);
  ASSERT_EQ(napi_destroy_runtime(rt2), napi_ok);
}

// Non-LIFO teardown: destroy the first-created env/runtime FIRST while the
// second is still in use. The enter-once model's nested Isolate::Enter stack
// requires LIFO Exit, which this violates.
TEST(MultiRuntime, DestroyOutOfOrder) {
  napi_platform pf = GetPlatform();
  napi_runtime rt1 = nullptr, rt2 = nullptr;
  ASSERT_EQ(napi_create_runtime(pf, &rt1), napi_ok);
  ASSERT_EQ(napi_create_runtime(pf, &rt2), napi_ok);
  napi_env e1 = nullptr, e2 = nullptr;
  ASSERT_EQ(napi_create_env(rt1, &e1), napi_ok);
  ASSERT_EQ(napi_create_env(rt2, &e2), napi_ok);

  ExerciseEnv(e1, 1);
  ExerciseEnv(e2, 2);

  ASSERT_EQ(napi_destroy_env(e1), napi_ok);       // first-created torn down first
  ASSERT_EQ(napi_destroy_runtime(rt1), napi_ok);
  ExerciseEnv(e2, 2);                              // e2 still fully usable
  ASSERT_EQ(napi_destroy_env(e2), napi_ok);
  ASSERT_EQ(napi_destroy_runtime(rt2), napi_ok);
}

// Scenario A regression guard: each runtime on its own thread.
TEST(MultiRuntime, TwoThreadsOneRuntimeEach) {
  napi_platform pf = GetPlatform();
  auto worker = [pf](int tag) {
    napi_runtime rt = nullptr;
    ASSERT_EQ(napi_create_runtime(pf, &rt), napi_ok);
    napi_env e = nullptr;
    ASSERT_EQ(napi_create_env(rt, &e), napi_ok);
    for (int i = 0; i < 50; ++i)
      ExerciseEnv(e, tag);
    ASSERT_EQ(napi_destroy_env(e), napi_ok);
    ASSERT_EQ(napi_destroy_runtime(rt), napi_ok);
  };
  std::thread t1(worker, 1), t2(worker, 2);
  t1.join();
  t2.join();
}

// Sequential create/use/destroy churn on one thread — isolate address reuse.
TEST(MultiRuntime, ChurnSameThread) {
  napi_platform pf = GetPlatform();
  for (int i = 0; i < 20; ++i) {
    napi_runtime rt = nullptr;
    ASSERT_EQ(napi_create_runtime(pf, &rt), napi_ok);
    napi_env e = nullptr;
    ASSERT_EQ(napi_create_env(rt, &e), napi_ok);
    ExerciseEnv(e, i);
    ASSERT_EQ(napi_destroy_env(e), napi_ok);
    ASSERT_EQ(napi_destroy_runtime(rt), napi_ok);
  }
}

}  // namespace
