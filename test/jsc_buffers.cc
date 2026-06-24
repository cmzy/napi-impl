// Tests the J2 surface now implemented: ArrayBuffer / TypedArray / DataView,
// BigInt, type tags, and get_all_property_names. Public API only.

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "napi/js_native_api.h"
#include "napi_v8/embedding.h"

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

static napi_env E;

static int32_t eval_int(const char* code) {
    napi_value s = nullptr, r = nullptr;
    napi_create_string_utf8(E, code, NAPI_AUTO_LENGTH, &s);
    napi_run_script(E, s, &r);
    int32_t v = -1;
    napi_get_value_int32(E, r, &v);
    return v;
}

static int arraybuffer_typedarray_dataview() {
    void* abdata = nullptr;
    napi_value ab = nullptr;
    CHECK(napi_create_arraybuffer(E, 16, &abdata, &ab));
    bool is = false;
    CHECK(napi_is_arraybuffer(E, ab, &is));
    EXPECT(is, "AB: is_arraybuffer true");
    void* gi = nullptr;
    size_t gl = 0;
    CHECK(napi_get_arraybuffer_info(E, ab, &gi, &gl));
    EXPECT(gi == abdata && gl == 16, "AB: get_info matches");

    // TypedArray (Uint8Array) over the buffer; zero-copy write -> JS read.
    napi_value ta = nullptr;
    CHECK(napi_create_typedarray(E, napi_uint8_array, 16, ab, 0, &ta));
    CHECK(napi_is_typedarray(E, ta, &is));
    EXPECT(is, "TA: is_typedarray true");
    napi_typedarray_type tt;
    size_t tlen = 0, toff = 99;
    void* tdata = nullptr;
    napi_value tbuf = nullptr;
    CHECK(napi_get_typedarray_info(E, ta, &tt, &tlen, &tdata, &tbuf, &toff));
    EXPECT(tt == napi_uint8_array && tlen == 16 && toff == 0 && tdata == abdata, "TA: get_info matches");
    static_cast<uint8_t*>(tdata)[2] = 0x7E;
    napi_value g = nullptr;
    CHECK(napi_get_global(E, &g));
    CHECK(napi_set_named_property(E, g, "__ta", ta));
    EXPECT(eval_int("__ta[2]") == 0x7E, "TA: zero-copy C->JS");

    // DataView over the buffer.
    napi_value dv = nullptr;
    CHECK(napi_create_dataview(E, 8, ab, 4, &dv));
    bool isdv = false;
    CHECK(napi_is_dataview(E, dv, &isdv));
    EXPECT(isdv, "DV: is_dataview true");
    size_t dlen = 0, doff = 0;
    void* ddata = nullptr;
    napi_value dbuf = nullptr;
    CHECK(napi_get_dataview_info(E, dv, &dlen, &ddata, &dbuf, &doff));
    EXPECT(dlen == 8 && doff == 4 && ddata == static_cast<char*>(abdata) + 4, "DV: get_info matches");

    // Detach — on a FRESH buffer: JSC permanently pins a buffer once its bytes
    // pointer has been fetched (as get_arraybuffer_info above does to `ab`), and
    // such a buffer can no longer be transfer()-detached. Verify both: a fresh
    // buffer detaches; a pinned one reports napi_detachable_arraybuffer_expected.
    napi_value fresh = nullptr;
    void* fd = nullptr;
    CHECK(napi_create_arraybuffer(E, 8, &fd, &fresh));
    CHECK(napi_detach_arraybuffer(E, fresh));
    bool det = false;
    CHECK(napi_is_detached_arraybuffer(E, fresh, &det));
    EXPECT(det, "AB: fresh buffer detached after detach");
    EXPECT(napi_detach_arraybuffer(E, ab) == napi_detachable_arraybuffer_expected,
           "AB: pinned buffer reports not-detachable");
    std::puts("arraybuffer/typedarray/dataview OK");
    return 0;
}

static int bigint() {
    napi_value b = nullptr;
    int64_t i64 = 0;
    bool lossless = false;
    CHECK(napi_create_bigint_int64(E, -9223372036854775807LL - 1, &b));  // INT64_MIN
    CHECK(napi_get_value_bigint_int64(E, b, &i64, &lossless));
    EXPECT(i64 == (-9223372036854775807LL - 1) && lossless, "BigInt int64 round-trip (INT64_MIN)");

    uint64_t u64 = 0;
    CHECK(napi_create_bigint_uint64(E, 0xFEDCBA9876543210ULL, &b));
    CHECK(napi_get_value_bigint_uint64(E, b, &u64, &lossless));
    EXPECT(u64 == 0xFEDCBA9876543210ULL && lossless, "BigInt uint64 round-trip");

    // words: 0x2_fedcba9876543210 (two 64-bit words, positive). *word_count is
    // in/out: set it to the buffer capacity before each call.
    const uint64_t in[2] = {0xFEDCBA9876543210ULL, 0x2ULL};
    CHECK(napi_create_bigint_words(E, 0, 2, in, &b));
    int sign = -1;
    size_t wc = 2;  // capacity
    uint64_t out[2] = {0, 0};
    CHECK(napi_get_value_bigint_words(E, b, &sign, &wc, out));
    EXPECT(sign == 0 && wc == 2 && out[0] == in[0] && out[1] == in[1], "BigInt words round-trip");

    // negative words round-trip
    CHECK(napi_create_bigint_words(E, 1, 1, in, &b));  // -0xfedcba9876543210
    wc = 2;  // capacity
    CHECK(napi_get_value_bigint_words(E, b, &sign, &wc, out));
    EXPECT(sign == 1 && wc == 1 && out[0] == in[0], "BigInt negative words round-trip");

    // query count with words=NULL
    wc = 0;
    CHECK(napi_get_value_bigint_words(E, b, nullptr, &wc, nullptr));
    EXPECT(wc == 1, "BigInt words count query");
    std::puts("bigint OK");
    return 0;
}

static int type_tags() {
    napi_value obj = nullptr;
    CHECK(napi_create_object(E, &obj));
    napi_type_tag tag = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    napi_type_tag other = {0x1111111111111111ULL, 0x2222222222222222ULL};
    bool m = false;
    CHECK(napi_check_object_type_tag(E, obj, &tag, &m));
    EXPECT(!m, "tag: untagged object should not match");
    CHECK(napi_type_tag_object(E, obj, &tag));
    CHECK(napi_check_object_type_tag(E, obj, &tag, &m));
    EXPECT(m, "tag: same tag should match");
    CHECK(napi_check_object_type_tag(E, obj, &other, &m));
    EXPECT(!m, "tag: different tag should not match");
    EXPECT(napi_type_tag_object(E, obj, &tag) == napi_invalid_arg, "tag: re-tag should fail");
    std::puts("type tags OK");
    return 0;
}

static uint32_t array_len(napi_value arr) {
    uint32_t n = 0;
    napi_get_array_length(E, arr, &n);
    return n;
}

static int all_property_names() {
    // proto { inh: 1 }, obj.own = 2, plus a non-enumerable "hidden".
    napi_value obj = nullptr, names = nullptr;
    CHECK(napi_create_object(E, &obj));
    napi_value one = nullptr, two = nullptr;
    CHECK(napi_create_int32(E, 1, &one));
    CHECK(napi_create_int32(E, 2, &two));
    CHECK(napi_set_named_property(E, obj, "own", two));
    napi_property_descriptor hidden = {"hidden", nullptr, nullptr, nullptr, nullptr, one, napi_default, nullptr};
    CHECK(napi_define_properties(E, obj, 1, &hidden));  // non-enumerable

    // own + enumerable only -> just "own"
    CHECK(napi_get_all_property_names(E, obj, napi_key_own_only, napi_key_enumerable, napi_key_keep_numbers, &names));
    EXPECT(array_len(names) == 1, "names: own+enumerable -> 1 (own)");
    // own, all keys -> own + hidden = 2
    CHECK(napi_get_all_property_names(E, obj, napi_key_own_only, napi_key_all_properties, napi_key_keep_numbers,
                                      &names));
    EXPECT(array_len(names) == 2, "names: own all -> 2 (own,hidden)");
    std::puts("get_all_property_names OK");
    return 0;
}

int main() {
    napi_platform platform = nullptr;
    napi_runtime runtime = nullptr;
    CHECK(napi_create_platform(0, nullptr, 0, nullptr, on_error, false, &platform));
    CHECK(napi_create_runtime(platform, &runtime));
    CHECK(napi_create_env(runtime, &E));

    int rc = arraybuffer_typedarray_dataview();
    if (rc == 0) rc = bigint();
    if (rc == 0) rc = type_tags();
    if (rc == 0) rc = all_property_names();

    CHECK(napi_destroy_env(E));
    CHECK(napi_destroy_runtime(runtime));
    CHECK(napi_destroy_platform(platform));
    std::puts(rc == 0 ? "BUFFERS PASS" : "BUFFERS FAIL");
    return rc;
}
