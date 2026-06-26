// gtest conversion of the V8-compat extensions test (formerly a bespoke
// main()+CHECK/EXPECT harness): SharedArrayBuffer (real zero-copy C<->JS) and
// the Inspector (start/stop toggle inspectability; the CDP message-loop calls
// are no-ops on JSC). Public API only, driven through the shared NapiExtras
// fixture.

// SharedArrayBuffer is experimental core node-api now (node_api_*); reading its
// backing store goes through napi_get_arraybuffer_info (extended to accept SAB).
#define NAPI_EXPERIMENTAL

#include <cstdlib>

#include "napi_gtest_fixture.h"
#include "napi_v8/inspector.h"

namespace {

TEST_F(NapiExtras, SharedArrayBuffer) {
  void* data = nullptr;
  napi_value sab = nullptr;
  ASSERT_EQ(node_api_create_sharedarraybuffer(env_, 32, &data, &sab), napi_ok);
  EXPECT_TRUE(data != nullptr) << "SAB: backing pointer must be non-null";

  bool is = false;
  ASSERT_EQ(node_api_is_sharedarraybuffer(env_, sab, &is), napi_ok);
  EXPECT_TRUE(is) << "SAB: created buffer should report is_shared_arraybuffer == true";

  // napi_get_arraybuffer_info is extended to read a SAB's backing store.
  void* info_data = nullptr;
  size_t info_len = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(env_, sab, &info_data, &info_len), napi_ok);
  EXPECT_TRUE(info_data == data) << "SAB: get_arraybuffer_info pointer must match create pointer";
  EXPECT_TRUE(info_len == 32) << "SAB: get_arraybuffer_info length must be 32";

  // Zero-copy: write through the C pointer, read back from JS.
  static_cast<unsigned char*>(data)[5] = 0xAB;
  napi_value global = nullptr;
  ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
  ASSERT_EQ(napi_set_named_property(env_, global, "__sab", sab), napi_ok);
  napi_value src = nullptr, res = nullptr;
  ASSERT_EQ(napi_create_string_utf8(env_, "(new Uint8Array(__sab))[5]", NAPI_AUTO_LENGTH, &src), napi_ok);
  ASSERT_EQ(napi_run_script(env_, src, &res), napi_ok);
  int32_t seen = 0;
  ASSERT_EQ(napi_get_value_int32(env_, res, &seen), napi_ok);
  EXPECT_TRUE(seen == 0xAB) << "SAB: JS must observe the byte written through the C pointer";

  // A plain object is not a shared arraybuffer.
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  ASSERT_EQ(node_api_is_sharedarraybuffer(env_, obj, &is), napi_ok);
  EXPECT_TRUE(!is) << "SAB: a plain object must not report is_shared_arraybuffer";

  // External SAB: wrap caller-owned memory zero-copy.
  auto* ext = static_cast<unsigned char*>(std::calloc(1, 16));
  EXPECT_TRUE(ext != nullptr) << "SAB: external alloc failed";
  ext[3] = 0x5C;
  napi_value esab = nullptr;
  ASSERT_EQ(node_api_create_external_sharedarraybuffer(env_, ext, 16, nullptr, nullptr, &esab), napi_ok);
  bool eis = false;
  ASSERT_EQ(node_api_is_sharedarraybuffer(env_, esab, &eis), napi_ok);
  EXPECT_TRUE(eis) << "SAB(external): must report is_shared_arraybuffer == true";
  void* edata = nullptr;
  size_t elen = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(env_, esab, &edata, &elen), napi_ok);
  EXPECT_TRUE(edata == ext) << "SAB(external): info pointer must equal the wrapped buffer";
  EXPECT_TRUE(elen == 16) << "SAB(external): info length must be 16";
  // We passed no finalizer, so we own `ext`; free it after use.
  std::free(ext);
}

TEST_F(NapiExtras, Inspector) {
  // start/stop are real (toggle JSGlobalContextSetInspectable + set name).
  ASSERT_EQ(napi_v8_inspector_start(env_, 9229, "napi-jsc-test"), napi_ok);
  ASSERT_EQ(napi_v8_inspector_wait_for_connection(env_), napi_ok);  // no-op on JSC (no client signal)
  EXPECT_TRUE(napi_v8_inspector_is_paused(env_) == false) << "inspector: should not be paused";
  size_t dispatched = 123;
  ASSERT_EQ(napi_v8_inspector_pump_messages(env_, &dispatched), napi_ok);
  EXPECT_TRUE(dispatched == 0) << "inspector: JSC has no host queue to drain (expect 0)";
  ASSERT_EQ(napi_v8_inspector_wait(env_, 0), napi_ok);
  ASSERT_EQ(napi_v8_inspector_set_pause_handler(env_, nullptr, nullptr), napi_ok);
  ASSERT_EQ(napi_v8_inspector_set_wake_handler(env_, nullptr, nullptr), napi_ok);
  ASSERT_EQ(napi_v8_inspector_stop(env_), napi_ok);
}

}  // namespace
