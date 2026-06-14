// Adapted from nodejs/node src/js_native_api_v8_internals.h.
// Strips Node.js core dependencies (env.h, node_internals.h, node_errors.h)
// and substitutes minimal equivalents so the NAPI V8 backend can build
// standalone for embedding.

#ifndef SRC_JS_NATIVE_API_V8_INTERNALS_H_
#define SRC_JS_NATIVE_API_V8_INTERNALS_H_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "v8-persistent-handle.h"

// With -fmodules, macros defined in a header included as a module are not
// always visible at the consuming TU; redefine here to make our impl headers
// usable regardless of module layout.
#ifndef NAPI_VERSION_EXPERIMENTAL
#define NAPI_VERSION_EXPERIMENTAL 2147483647
#endif

// NAPI_VERSION lives in js_native_api_types.h.
// Define version range constants Node uses but that are part of node_version.h.
#ifndef NODE_API_DEFAULT_MODULE_API_VERSION
#define NODE_API_DEFAULT_MODULE_API_VERSION 8
#endif
#ifndef NODE_API_SUPPORTED_VERSION_MAX
#define NODE_API_SUPPORTED_VERSION_MAX 9
#endif
#ifndef NODE_API_SUPPORTED_VERSION_MIN
#define NODE_API_SUPPORTED_VERSION_MIN 1
#endif

#define NAPI_ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

// FIXED_ONE_BYTE_STRING — internalized one-byte literal.
#define NAPI_FIXED_ONE_BYTE_STRING(isolate, literal)                                                                   \
    (v8::String::NewFromOneByte((isolate), reinterpret_cast<const uint8_t *>(literal),                                 \
                                v8::NewStringType::kInternalized, sizeof(literal) - 1)                                 \
             .ToLocalChecked())

// Per-context private key cache. We attach a small embedder-data slot to the
// V8 context to memoize the private symbols used by napi_wrap and type_tag.
// (Node uses Environment-attached caches; we use context embedder data
// slots 1..2.)
#define NAPI_PRIVATE_KEY(context, suffix) napi_v8_priv::GetPrivateKey((context), napi_v8_priv::PrivateKeyKind::suffix)

namespace napi_v8_priv {
    enum class PrivateKeyKind {
        wrapper = 1,
        type_tag = 2,
    };
    v8::Local<v8::Private> GetPrivateKey(v8::Local<v8::Context> ctx, PrivateKeyKind kind);
} // namespace napi_v8_priv

namespace v8impl {

    template<typename T>
    using Persistent = v8::Global<T>;

    class PersistentToLocal {
    public:
        template<typename T>
        static inline v8::Local<T> Strong(const v8::Global<T> &global) {
            return *reinterpret_cast<v8::Local<T> *>(const_cast<v8::Global<T> *>(&global));
        }
    };

    [[noreturn]] inline void OnFatalError(const char *location, const char *message) {
        std::fprintf(stderr, "FATAL ERROR: %s %s\n", location ? location : "(no location)", message ? message : "");
        std::fflush(stderr);
        std::abort();
    }

} // namespace v8impl

// Replacement for node::OnScopeLeave (used by RAII restore).
namespace node {
    template<typename F>
    class OnScopeLeaveGuard {
    public:
        explicit OnScopeLeaveGuard(F &&fn) : fn_(std::move(fn)), active_(true) {}
        ~OnScopeLeaveGuard() {
            if (active_)
                fn_();
        }
        OnScopeLeaveGuard(const OnScopeLeaveGuard &) = delete;
        OnScopeLeaveGuard(OnScopeLeaveGuard &&other) noexcept : fn_(std::move(other.fn_)), active_(other.active_) {
            other.active_ = false;
        }

    private:
        F fn_;
        bool active_;
    };
    template<typename F>
    inline OnScopeLeaveGuard<F> OnScopeLeave(F &&fn) {
        return OnScopeLeaveGuard<F>(std::forward<F>(fn));
    }

    // Minimal CHECK_EQ used by env CallIntoModule.
} // namespace node

#ifndef CHECK
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "CHECK failed: %s at %s:%d\n", #cond, __FILE__, __LINE__);                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)
#endif

#define DCHECK(cond) CHECK(cond)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_NULL(a) CHECK((a) == nullptr)
#define CHECK_NOT_NULL(a) CHECK((a) != nullptr)

#define DCHECK_EQ(a, b) CHECK_EQ((a), (b))
#define DCHECK_NE(a, b) CHECK_NE((a), (b))
#define DCHECK_LT(a, b) CHECK_LT((a), (b))
#define DCHECK_LE(a, b) CHECK_LE((a), (b))
#define DCHECK_GT(a, b) CHECK_GT((a), (b))
#define DCHECK_GE(a, b) CHECK_GE((a), (b))
#define DCHECK_NULL(a) CHECK_NULL((a))
#define DCHECK_NOT_NULL(a) CHECK_NOT_NULL((a))

#define UNREACHABLE()                                                                                                  \
    do {                                                                                                               \
        std::fprintf(stderr, "UNREACHABLE at %s:%d\n", __FILE__, __LINE__);                                            \
        std::abort();                                                                                                  \
    } while (0)

#endif // SRC_JS_NATIVE_API_V8_INTERNALS_H_
