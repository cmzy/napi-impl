// gtest covering the full TypedArray-type matrix (drives the napi<->JSC type
// switches ToJSTA/FromJSTA over all 11 kinds) plus napi_create_external_arraybuffer.
// Complements jsc_buffers (which exercises only a couple of kinds).

#include "napi_gtest_fixture.h"

namespace {

struct TAKind {
  napi_typedarray_type type;
  size_t elem_size;
  const char* name;
};

TEST_F(NapiExtras, TypedArrayAllKinds) {
  const TAKind kinds[] = {
      {napi_int8_array, 1, "Int8Array"},        {napi_uint8_array, 1, "Uint8Array"},
      {napi_uint8_clamped_array, 1, "Uint8ClampedArray"}, {napi_int16_array, 2, "Int16Array"},
      {napi_uint16_array, 2, "Uint16Array"},    {napi_int32_array, 4, "Int32Array"},
      {napi_uint32_array, 4, "Uint32Array"},    {napi_float32_array, 4, "Float32Array"},
      {napi_float64_array, 8, "Float64Array"},  {napi_bigint64_array, 8, "BigInt64Array"},
      {napi_biguint64_array, 8, "BigUint64Array"},
  };
  for (const TAKind& k : kinds) {
    napi_value ab = nullptr;
    ASSERT_EQ(napi_create_arraybuffer(env_, k.elem_size * 4, nullptr, &ab), napi_ok) << k.name;
    napi_value ta = nullptr;
    ASSERT_EQ(napi_create_typedarray(env_, k.type, 4, ab, 0, &ta), napi_ok) << k.name;
    bool is = false;
    ASSERT_EQ(napi_is_typedarray(env_, ta, &is), napi_ok) << k.name;
    EXPECT_TRUE(is) << k.name;

    napi_typedarray_type got_type;
    size_t len = 0;
    void* data = nullptr;
    napi_value out_ab = nullptr;
    size_t offset = 0;
    ASSERT_EQ(napi_get_typedarray_info(env_, ta, &got_type, &len, &data, &out_ab, &offset), napi_ok) << k.name;
    EXPECT_EQ(got_type, k.type) << k.name;  // FromJSTA mapped the JSC kind back
    EXPECT_EQ(len, 4u) << k.name;
    EXPECT_EQ(offset, 0u) << k.name;
  }
}

TEST_F(NapiExtras, CreateExternalArrayBuffer) {
  static unsigned char buf[16] = {0};
  buf[0] = 0x5A;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_create_external_arraybuffer(env_, buf, sizeof(buf), nullptr, nullptr, &ab), napi_ok);
  bool is = false;
  ASSERT_EQ(napi_is_arraybuffer(env_, ab, &is), napi_ok);
  EXPECT_TRUE(is);
  void* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(env_, ab, &data, &len), napi_ok);
  EXPECT_EQ(data, buf);
  EXPECT_EQ(len, 16u);
}

}  // namespace
