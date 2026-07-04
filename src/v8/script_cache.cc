// Engine-neutral compilation-cache surface (napi/script_cache.h), V8 backend.
// Real caching via V8's ScriptCompiler code cache: consume a prior blob to skip
// parse/compile (silent recompile + *cache_rejected on mismatch), and/or produce
// a fresh blob after running (so it captures lazily-compiled inner functions).

#include "napi/script_cache.h"

#include "js_native_api_v8.h"
#include "js_native_api_v8_impl.h"

#include <cstdlib>  // std::malloc / std::free
#include <cstring>  // std::memcpy
#include <memory>   // std::unique_ptr

#include "v8-context.h"
#include "v8-script.h"

napi_status NAPI_CDECL napi_run_script_cached(napi_env env, napi_value script, const char *origin, size_t origin_len,
                                              const uint8_t *cached_data, size_t cached_len, bool *cache_rejected,
                                              uint8_t **out_cached_data, size_t *out_cached_len, napi_value *result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, script);
    if (cache_rejected != nullptr)
        *cache_rejected = false;
    if (out_cached_data != nullptr) {
        *out_cached_data = nullptr;
        if (out_cached_len != nullptr)
            *out_cached_len = 0;
    }

    v8::Local<v8::Value> v8_script = v8impl::V8LocalValueFromJsValue(script);
    if (!v8_script->IsString())
        return napi_set_last_error(env, napi_string_expected);

    v8::Local<v8::Context> context = env->context();
    v8::Local<v8::String> src = v8_script.As<v8::String>();

    // ScriptOrigin names the script (cache keying / stack traces); empty when unset.
    v8::Local<v8::String> name;
    if (!(origin != nullptr && origin_len > 0 &&
          v8::String::NewFromUtf8(env->isolate, origin, v8::NewStringType::kNormal, static_cast<int>(origin_len))
                  .ToLocal(&name)))
        name = v8::String::Empty(env->isolate);
    v8::ScriptOrigin script_origin(name);

    // A provided blob is handed to Source (which takes ownership) and consumed;
    // V8 validates it against the source hash and silently recompiles on mismatch.
    v8::ScriptCompiler::CachedData *cd = nullptr;
    if (cached_data != nullptr && cached_len > 0)
        cd = new v8::ScriptCompiler::CachedData(cached_data, static_cast<int>(cached_len),
                                                v8::ScriptCompiler::CachedData::BufferNotOwned);
    v8::ScriptCompiler::Source source(src, script_origin, cd);
    v8::ScriptCompiler::CompileOptions options =
            cd != nullptr ? v8::ScriptCompiler::kConsumeCodeCache : v8::ScriptCompiler::kNoCompileOptions;

    v8::Local<v8::Script> compiled;
    if (!v8::ScriptCompiler::Compile(context, &source, options).ToLocal(&compiled))
        return GET_RETURN_STATUS(env); // compile threw -> exception pending
    if (cd != nullptr && cache_rejected != nullptr) {
        const v8::ScriptCompiler::CachedData *used = source.GetCachedData();
        *cache_rejected = (used == nullptr) || used->rejected;
    }

    v8::Local<v8::Value> run_result;
    if (!compiled->Run(context).ToLocal(&run_result))
        return GET_RETURN_STATUS(env); // run threw

    // Produce AFTER running so the blob includes lazily-compiled inner functions.
    if (out_cached_data != nullptr && out_cached_len != nullptr) {
        std::unique_ptr<v8::ScriptCompiler::CachedData> produced(
                v8::ScriptCompiler::CreateCodeCache(compiled->GetUnboundScript()));
        if (produced && produced->data != nullptr && produced->length > 0) {
            auto *buf = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(produced->length)));
            if (buf != nullptr) {
                std::memcpy(buf, produced->data, static_cast<size_t>(produced->length));
                *out_cached_data = buf;
                *out_cached_len = static_cast<size_t>(produced->length);
            }
        }
    }

    if (result != nullptr)
        *result = v8impl::JsValueFromV8LocalValue(run_result);
    return GET_RETURN_STATUS(env);
}

napi_status NAPI_CDECL napi_free_script_cache(napi_env env, uint8_t *cached_data) {
    CHECK_ENV_NOT_IN_GC(env);
    // The blob is malloc'd inside the library; free it here to keep alloc/free on
    // one heap (cross-DLL/CRT correctness). NULL is a no-op.
    if (cached_data != nullptr)
        std::free(cached_data);
    return napi_clear_last_error(env);
}
