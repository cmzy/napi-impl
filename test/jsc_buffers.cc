// gtest conversion of the J2-surface test (formerly a bespoke main()+CHECK/EXPECT
// harness): ArrayBuffer / TypedArray / DataView, BigInt, type tags, and
// get_all_property_names. Public API only, driven through the shared NapiExtras
// fixture. One TEST_F per original check function.

#include <cstdint>

#include "jsc_gtest_fixture.h"

namespace {

TEST_F(NapiExtras, ArrayBufferTypedArrayDataView) {
  void* abdata = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(env_, 16, &abdata, &ab), napi_ok);
  bool is = false;
  ASSERT_EQ(napi_is_arraybuffer(env_, ab, &is), napi_ok);
  EXPECT_TRUE(is) << "AB: is_arraybuffer true";
  void* gi = nullptr;
  size_t gl = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(env_, ab, &gi, &gl), napi_ok);
  EXPECT_TRUE(gi == abdata && gl == 16) << "AB: get_info matches";

  // TypedArray (Uint8Array) over the buffer; zero-copy write -> JS read.
  napi_value ta = nullptr;
  ASSERT_EQ(napi_create_typedarray(env_, napi_uint8_array, 16, ab, 0, &ta), napi_ok);
  ASSERT_EQ(napi_is_typedarray(env_, ta, &is), napi_ok);
  EXPECT_TRUE(is) << "TA: is_typedarray true";
  napi_typedarray_type tt;
  size_t tlen = 0, toff = 99;
  void* tdata = nullptr;
  napi_value tbuf = nullptr;
  ASSERT_EQ(napi_get_typedarray_info(env_, ta, &tt, &tlen, &tdata, &tbuf, &toff), napi_ok);
  EXPECT_TRUE(tt == napi_uint8_array && tlen == 16 && toff == 0 && tdata == abdata) << "TA: get_info matches";
  static_cast<uint8_t*>(tdata)[2] = 0x7E;
  napi_value g = nullptr;
  ASSERT_EQ(napi_get_global(env_, &g), napi_ok);
  ASSERT_EQ(napi_set_named_property(env_, g, "__ta", ta), napi_ok);
  EXPECT_TRUE(I32(Run("__ta[2]")) == 0x7E) << "TA: zero-copy C->JS";

  // DataView over the buffer.
  napi_value dv = nullptr;
  ASSERT_EQ(napi_create_dataview(env_, 8, ab, 4, &dv), napi_ok);
  bool isdv = false;
  ASSERT_EQ(napi_is_dataview(env_, dv, &isdv), napi_ok);
  EXPECT_TRUE(isdv) << "DV: is_dataview true";
  size_t dlen = 0, doff = 0;
  void* ddata = nullptr;
  napi_value dbuf = nullptr;
  ASSERT_EQ(napi_get_dataview_info(env_, dv, &dlen, &ddata, &dbuf, &doff), napi_ok);
  EXPECT_TRUE(dlen == 8 && doff == 4 && ddata == static_cast<char*>(abdata) + 4) << "DV: get_info matches";

  // Detach — on a FRESH buffer: JSC permanently pins a buffer once its bytes
  // pointer has been fetched (as get_arraybuffer_info above does to `ab`), and
  // such a buffer can no longer be transfer()-detached. Verify both: a fresh
  // buffer detaches; a pinned one reports napi_detachable_arraybuffer_expected.
  napi_value fresh = nullptr;
  void* fd = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(env_, 8, &fd, &fresh), napi_ok);
  ASSERT_EQ(napi_detach_arraybuffer(env_, fresh), napi_ok);
  bool det = false;
  ASSERT_EQ(napi_is_detached_arraybuffer(env_, fresh, &det), napi_ok);
  EXPECT_TRUE(det) << "AB: fresh buffer detached after detach";
  EXPECT_TRUE(napi_detach_arraybuffer(env_, ab) == napi_detachable_arraybuffer_expected)
      << "AB: pinned buffer reports not-detachable";
}

TEST_F(NapiExtras, BigInt) {
  napi_value b = nullptr;
  int64_t i64 = 0;
  bool lossless = false;
  ASSERT_EQ(napi_create_bigint_int64(env_, -9223372036854775807LL - 1, &b), napi_ok);  // INT64_MIN
  ASSERT_EQ(napi_get_value_bigint_int64(env_, b, &i64, &lossless), napi_ok);
  EXPECT_TRUE(i64 == (-9223372036854775807LL - 1) && lossless) << "BigInt int64 round-trip (INT64_MIN)";

  uint64_t u64 = 0;
  ASSERT_EQ(napi_create_bigint_uint64(env_, 0xFEDCBA9876543210ULL, &b), napi_ok);
  ASSERT_EQ(napi_get_value_bigint_uint64(env_, b, &u64, &lossless), napi_ok);
  EXPECT_TRUE(u64 == 0xFEDCBA9876543210ULL && lossless) << "BigInt uint64 round-trip";

  // words: 0x2_fedcba9876543210 (two 64-bit words, positive). *word_count is
  // in/out: set it to the buffer capacity before each call.
  const uint64_t in[2] = {0xFEDCBA9876543210ULL, 0x2ULL};
  ASSERT_EQ(napi_create_bigint_words(env_, 0, 2, in, &b), napi_ok);
  int sign = -1;
  size_t wc = 2;  // capacity
  uint64_t out[2] = {0, 0};
  ASSERT_EQ(napi_get_value_bigint_words(env_, b, &sign, &wc, out), napi_ok);
  EXPECT_TRUE(sign == 0 && wc == 2 && out[0] == in[0] && out[1] == in[1]) << "BigInt words round-trip";

  // negative words round-trip
  ASSERT_EQ(napi_create_bigint_words(env_, 1, 1, in, &b), napi_ok);  // -0xfedcba9876543210
  wc = 2;                                                            // capacity
  ASSERT_EQ(napi_get_value_bigint_words(env_, b, &sign, &wc, out), napi_ok);
  EXPECT_TRUE(sign == 1 && wc == 1 && out[0] == in[0]) << "BigInt negative words round-trip";

  // query count with words=NULL
  wc = 0;
  ASSERT_EQ(napi_get_value_bigint_words(env_, b, nullptr, &wc, nullptr), napi_ok);
  EXPECT_TRUE(wc == 1) << "BigInt words count query";
}

TEST_F(NapiExtras, TypeTags) {
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  napi_type_tag tag = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
  napi_type_tag other = {0x1111111111111111ULL, 0x2222222222222222ULL};
  bool m = false;
  ASSERT_EQ(napi_check_object_type_tag(env_, obj, &tag, &m), napi_ok);
  EXPECT_TRUE(!m) << "tag: untagged object should not match";
  ASSERT_EQ(napi_type_tag_object(env_, obj, &tag), napi_ok);
  ASSERT_EQ(napi_check_object_type_tag(env_, obj, &tag, &m), napi_ok);
  EXPECT_TRUE(m) << "tag: same tag should match";
  ASSERT_EQ(napi_check_object_type_tag(env_, obj, &other, &m), napi_ok);
  EXPECT_TRUE(!m) << "tag: different tag should not match";
  EXPECT_TRUE(napi_type_tag_object(env_, obj, &tag) == napi_invalid_arg) << "tag: re-tag should fail";
}

TEST_F(NapiExtras, AllPropertyNames) {
  auto array_len = [&](napi_value arr) -> uint32_t {
    uint32_t n = 0;
    napi_get_array_length(env_, arr, &n);
    return n;
  };

  // proto { inh: 1 }, obj.own = 2, plus a non-enumerable "hidden".
  napi_value obj = nullptr, names = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  napi_value one = nullptr, two = nullptr;
  ASSERT_EQ(napi_create_int32(env_, 1, &one), napi_ok);
  ASSERT_EQ(napi_create_int32(env_, 2, &two), napi_ok);
  ASSERT_EQ(napi_set_named_property(env_, obj, "own", two), napi_ok);
  napi_property_descriptor hidden = {"hidden", nullptr, nullptr, nullptr, nullptr, one, napi_default, nullptr};
  ASSERT_EQ(napi_define_properties(env_, obj, 1, &hidden), napi_ok);  // non-enumerable

  // own + enumerable only -> just "own"
  ASSERT_EQ(napi_get_all_property_names(env_, obj, napi_key_own_only, napi_key_enumerable, napi_key_keep_numbers,
                                        &names),
            napi_ok);
  EXPECT_TRUE(array_len(names) == 1) << "names: own+enumerable -> 1 (own)";
  // own, all keys -> own + hidden = 2
  ASSERT_EQ(napi_get_all_property_names(env_, obj, napi_key_own_only, napi_key_all_properties, napi_key_keep_numbers,
                                        &names),
            napi_ok);
  EXPECT_TRUE(array_len(names) == 2) << "names: own all -> 2 (own,hidden)";
}

}  // namespace
