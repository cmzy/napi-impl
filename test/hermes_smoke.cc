// gtest conversion of the Hermes NAPI backend smoke test (M7.1, formerly a
// bespoke main()+CHECK harness): the embedding env works end-to-end and
// napi_run_script evaluates correctly. Driven through the shared NapiExtras
// fixture (public embedding + standard napi surface only).

#include "napi_gtest_fixture.h"

namespace {

TEST_F(NapiExtras, RunScriptArithmetic) {
  EXPECT_EQ(I32(Run("1 + 2")), 3);
}

TEST_F(NapiExtras, RunScriptStringConcat) {
  napi_value res = Run("'hello ' + 'world'");
  char buf[32] = {0};
  size_t len = 0;
  ASSERT_EQ(napi_get_value_string_utf8(env_, res, buf, sizeof(buf), &len), napi_ok);
  EXPECT_STREQ(buf, "hello world");
}

}  // namespace
