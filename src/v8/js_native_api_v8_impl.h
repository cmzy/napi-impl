// Implementation-side helpers extracted from the Node.js js_native_api_v8.cc
// monolith. Everything here lived in an anonymous namespace inside the .cc;
// to make it visible across the per-category split files, we move it into a
// header and mark the free functions inline so the ODR holds.

#ifndef SRC_JS_NATIVE_API_V8_IMPL_H_
#define SRC_JS_NATIVE_API_V8_IMPL_H_

#include <algorithm>
#include <climits>
#include <cstdint>

#include "napi/js_native_api.h"

#include "js_native_api_v8.h"

#define CHECK_NEW_STRING_ARGS(env, str, length, result)                                                                \
    do {                                                                                                               \
        CHECK_ENV_NOT_IN_GC((env));                                                                                    \
        if ((length) > 0)                                                                                              \
            CHECK_ARG((env), (str));                                                                                   \
        CHECK_ARG((env), (result));                                                                                    \
        RETURN_STATUS_IF_FALSE((env), ((length) == NAPI_AUTO_LENGTH) || (length) <= INT_MAX, napi_invalid_arg);        \
    } while (0)

#define CHECK_NEW_FROM_UTF8_LEN(env, result, str, len)                                                                 \
    do {                                                                                                               \
        static_assert(static_cast<int>(NAPI_AUTO_LENGTH) == -1, "Casting NAPI_AUTO_LENGTH to int must result in -1");  \
        RETURN_STATUS_IF_FALSE((env), (len == NAPI_AUTO_LENGTH) || len <= INT_MAX, napi_invalid_arg);                  \
        RETURN_STATUS_IF_FALSE((env), (str) != nullptr, napi_invalid_arg);                                             \
        auto str_maybe = v8::String::NewFromUtf8((env)->isolate, (str), v8::NewStringType::kInternalized,              \
                                                 static_cast<int>(len));                                               \
        CHECK_MAYBE_EMPTY((env), str_maybe, napi_generic_failure);                                                     \
        (result) = str_maybe.ToLocalChecked();                                                                         \
    } while (0)

#define CHECK_NEW_FROM_UTF8(env, result, str) CHECK_NEW_FROM_UTF8_LEN((env), (result), (str), NAPI_AUTO_LENGTH)

namespace v8impl {

    template<typename CCharType, typename StringMaker>
    inline napi_status NewString(napi_env env, const CCharType *str, size_t length, napi_value *result,
                                 StringMaker string_maker) {
        CHECK_NEW_STRING_ARGS(env, str, length, result);

        auto isolate = env->isolate;
        auto str_maybe = string_maker(isolate);
        CHECK_MAYBE_EMPTY(env, str_maybe, napi_generic_failure);
        *result = v8impl::JsValueFromV8LocalValue(str_maybe.ToLocalChecked());
        return napi_clear_last_error(env);
    }

    template<typename CharType, typename CreateAPI, typename StringMaker>
    inline napi_status NewExternalString(napi_env env, CharType *str, size_t length, napi_finalize finalize_callback,
                                         void *finalize_hint, napi_value *result, bool *copied, CreateAPI create_api,
                                         StringMaker string_maker) {
        CHECK_NEW_STRING_ARGS(env, str, length, result);

        napi_status status;
#if defined(V8_ENABLE_SANDBOX)
        status = create_api(env, str, length, result);
        if (status == napi_ok) {
            if (copied != nullptr)
                *copied = true;
            if (finalize_callback) {
                env->CallFinalizer(finalize_callback, static_cast<CharType *>(str), finalize_hint);
            }
        }
#else
        status = NewString(env, str, length, result, string_maker);
        if (status == napi_ok && copied != nullptr)
            *copied = false;
#endif
        return status;
    }

    class TrackedStringResource : private RefTracker {
    public:
        TrackedStringResource(napi_env env, napi_finalize finalize_callback, void *data, void *finalize_hint) :
            RefTracker(), finalizer_(env, finalize_callback, data, finalize_hint) {
            Link(finalize_callback == nullptr ? &env->reflist : &env->finalizing_reflist);
        }

    protected:
        void Finalize() override {
            Unlink();
            finalizer_.ResetEnv();
        }

        ~TrackedStringResource() override {
            Unlink();
            finalizer_.CallFinalizer();
        }

    private:
        Finalizer finalizer_;
    };

    class ExternalOneByteStringResource final : public v8::String::ExternalOneByteStringResource,
                                                TrackedStringResource {
    public:
        ExternalOneByteStringResource(napi_env env, char *string, const size_t length, napi_finalize finalize_callback,
                                      void *finalize_hint) :
            TrackedStringResource(env, finalize_callback, string, finalize_hint),
            string_(string), length_(length) {}

        const char *data() const override { return string_; }
        size_t length() const override { return length_; }

    private:
        const char *string_;
        const size_t length_;
    };

    class ExternalStringResource final : public v8::String::ExternalStringResource, TrackedStringResource {
    public:
        ExternalStringResource(napi_env env, char16_t *string, const size_t length, napi_finalize finalize_callback,
                               void *finalize_hint) :
            TrackedStringResource(env, finalize_callback, string, finalize_hint),
            string_(reinterpret_cast<uint16_t *>(string)), length_(length) {}

        const uint16_t *data() const override { return string_; }
        size_t length() const override { return length_; }

    private:
        const uint16_t *string_;
        const size_t length_;
    };

    inline napi_status V8NameFromPropertyDescriptor(napi_env env, const napi_property_descriptor *p,
                                                    v8::Local<v8::Name> *result) {
        if (p->utf8name != nullptr) {
            CHECK_NEW_FROM_UTF8(env, *result, p->utf8name);
        } else {
            v8::Local<v8::Value> property_value = v8impl::V8LocalValueFromJsValue(p->name);
            RETURN_STATUS_IF_FALSE(env, property_value->IsName(), napi_name_expected);
            *result = property_value.As<v8::Name>();
        }
        return napi_ok;
    }

    inline v8::PropertyAttribute V8PropertyAttributesFromDescriptor(const napi_property_descriptor *descriptor) {
        unsigned int attribute_flags = v8::PropertyAttribute::None;
        if ((descriptor->getter == nullptr && descriptor->setter == nullptr) &&
            (descriptor->attributes & napi_writable) == 0) {
            attribute_flags |= v8::PropertyAttribute::ReadOnly;
        }
        if ((descriptor->attributes & napi_enumerable) == 0) {
            attribute_flags |= v8::PropertyAttribute::DontEnum;
        }
        if ((descriptor->attributes & napi_configurable) == 0) {
            attribute_flags |= v8::PropertyAttribute::DontDelete;
        }
        return static_cast<v8::PropertyAttribute>(attribute_flags);
    }

    inline napi_deferred JsDeferredFromNodePersistent(v8impl::Persistent<v8::Value> *local) {
        return reinterpret_cast<napi_deferred>(local);
    }

    inline v8impl::Persistent<v8::Value> *NodePersistentFromJsDeferred(napi_deferred local) {
        return reinterpret_cast<v8impl::Persistent<v8::Value> *>(local);
    }

    class HandleScopeWrapper {
    public:
        explicit HandleScopeWrapper(v8::Isolate *isolate) : scope(isolate) {}

    private:
        v8::HandleScope scope;
    };

    class EscapableHandleScopeWrapper {
    public:
        explicit EscapableHandleScopeWrapper(v8::Isolate *isolate) : scope(isolate), escape_called_(false) {}
        bool escape_called() const { return escape_called_; }
        template<typename T>
        v8::Local<T> Escape(v8::Local<T> handle) {
            escape_called_ = true;
            return scope.Escape(handle);
        }

    private:
        v8::EscapableHandleScope scope;
        bool escape_called_;
    };

    inline napi_handle_scope JsHandleScopeFromV8HandleScope(HandleScopeWrapper *s) {
        return reinterpret_cast<napi_handle_scope>(s);
    }
    inline HandleScopeWrapper *V8HandleScopeFromJsHandleScope(napi_handle_scope s) {
        return reinterpret_cast<HandleScopeWrapper *>(s);
    }
    inline napi_escapable_handle_scope
    JsEscapableHandleScopeFromV8EscapableHandleScope(EscapableHandleScopeWrapper *s) {
        return reinterpret_cast<napi_escapable_handle_scope>(s);
    }
    inline EscapableHandleScopeWrapper *
    V8EscapableHandleScopeFromJsEscapableHandleScope(napi_escapable_handle_scope s) {
        return reinterpret_cast<EscapableHandleScopeWrapper *>(s);
    }

    inline napi_status ConcludeDeferred(napi_env env, napi_deferred deferred, napi_value result, bool is_resolved) {
        NAPI_PREAMBLE(env);
        CHECK_ARG(env, result);

        v8::Local<v8::Context> context = env->context();
        v8impl::Persistent<v8::Value> *deferred_ref = NodePersistentFromJsDeferred(deferred);
        v8::Local<v8::Value> v8_deferred = v8::Local<v8::Value>::New(env->isolate, *deferred_ref);

        auto v8_resolver = v8_deferred.As<v8::Promise::Resolver>();
        v8::Maybe<bool> success = is_resolved ? v8_resolver->Resolve(context, v8impl::V8LocalValueFromJsValue(result))
                                              : v8_resolver->Reject(context, v8impl::V8LocalValueFromJsValue(result));

        delete deferred_ref;
        RETURN_STATUS_IF_FALSE(env, success.FromMaybe(false), napi_generic_failure);
        return GET_RETURN_STATUS(env);
    }

    enum UnwrapAction { KeepWrap, RemoveWrap };

    inline napi_status Unwrap(napi_env env, napi_value js_object, void **result, UnwrapAction action) {
        NAPI_PREAMBLE(env);
        CHECK_ARG(env, js_object);
        if (action == KeepWrap)
            CHECK_ARG(env, result);

        v8::Local<v8::Context> context = env->context();
        v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(js_object);
        RETURN_STATUS_IF_FALSE(env, value->IsObject(), napi_invalid_arg);
        v8::Local<v8::Object> obj = value.As<v8::Object>();

        // Fast path: napi_wrap mirrors the native pointer into internal field 0
        // for objects that reserve one (every napi_define_class instance). A
        // non-null slot means the object is wrapped, so we can return the pointer
        // with an O(1) field read instead of a private-property lookup. RemoveWrap
        // needs the Reference*, so it skips this and takes the slow path below.
        if (action == KeepWrap && obj->InternalFieldCount() >= 1) {
            void *p = obj->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault);
            if (p != nullptr) {
                *result = p;
                return napi_clear_last_error(env);
            }
        }

        auto val = obj->GetPrivate(context, NAPI_PRIVATE_KEY(context, wrapper)).ToLocalChecked();
        RETURN_STATUS_IF_FALSE(env, val->IsExternal(), napi_invalid_arg);
        Reference *reference = static_cast<v8impl::Reference *>(val.As<v8::External>()->Value());

        if (result)
            *result = reference->Data();

        if (action == RemoveWrap) {
            CHECK(obj->DeletePrivate(context, NAPI_PRIVATE_KEY(context, wrapper)).FromJust());
            // Also clear the fast-call mirror (fields 0=native, 1=type tag) so a
            // still-live JS object can't hand a now-removed (possibly freed)
            // pointer to napi_fast_unwrap — i.e. no use-after-remove via fast call.
            int fields = obj->InternalFieldCount();
            if (fields >= 1)
                obj->SetAlignedPointerInInternalField(0, nullptr, v8::kEmbedderDataTypeTagDefault);
            if (fields >= 2)
                obj->SetAlignedPointerInInternalField(1, nullptr, v8::kEmbedderDataTypeTagDefault);
            if (reference->ownership() == ReferenceOwnership::kUserland) {
                reference->ResetFinalizer();
            } else {
                delete reference;
            }
        }
        return GET_RETURN_STATUS(env);
    }

    class CallbackBundle {
    public:
        static inline v8::Local<v8::Value> New(napi_env env, napi_callback cb, void *data) {
            CallbackBundle *bundle = new CallbackBundle();
            bundle->cb = cb;
            bundle->cb_data = data;
            bundle->env = env;

            v8::Local<v8::Value> cbdata = v8::External::New(env->isolate, bundle);
            ReferenceWithFinalizer::New(env, cbdata, 0, ReferenceOwnership::kRuntime, Delete, bundle, nullptr);
            return cbdata;
        }

        static CallbackBundle *FromCallbackData(v8::Local<v8::Value> data) {
            return reinterpret_cast<CallbackBundle *>(data.As<v8::External>()->Value());
        }

    public:
        napi_env env;
        void *cb_data;
        napi_callback cb;

    private:
        static void Delete(napi_env env, void *data, void *hint) {
            CallbackBundle *bundle = static_cast<CallbackBundle *>(data);
            delete bundle;
        }
    };

    class FunctionCallbackWrapper {
    public:
        static void Invoke(const v8::FunctionCallbackInfo<v8::Value> &info) {
            FunctionCallbackWrapper cbwrapper(info);
            cbwrapper.InvokeCallback();
        }

        static inline napi_status NewFunction(napi_env env, napi_callback cb, void *cb_data,
                                              v8::Local<v8::Function> *result) {
            v8::Local<v8::Value> cbdata = v8impl::CallbackBundle::New(env, cb, cb_data);
            RETURN_STATUS_IF_FALSE(env, !cbdata.IsEmpty(), napi_generic_failure);

            v8::MaybeLocal<v8::Function> maybe_function = v8::Function::New(env->context(), Invoke, cbdata);
            CHECK_MAYBE_EMPTY(env, maybe_function, napi_generic_failure);

            *result = maybe_function.ToLocalChecked();
            return napi_clear_last_error(env);
        }

        static inline napi_status NewTemplate(napi_env env, napi_callback cb, void *cb_data,
                                              v8::Local<v8::FunctionTemplate> *result,
                                              v8::Local<v8::Signature> sig = v8::Local<v8::Signature>()) {
            v8::Local<v8::Value> cbdata = v8impl::CallbackBundle::New(env, cb, cb_data);
            RETURN_STATUS_IF_FALSE(env, !cbdata.IsEmpty(), napi_generic_failure);
            *result = v8::FunctionTemplate::New(env->isolate, Invoke, cbdata, sig);
            return napi_clear_last_error(env);
        }

        napi_value GetNewTarget() {
            if (cbinfo_.IsConstructCall()) {
                return v8impl::JsValueFromV8LocalValue(cbinfo_.NewTarget());
            } else {
                return nullptr;
            }
        }

        void Args(napi_value *buffer, size_t buffer_length) {
            size_t i = 0;
            size_t min_arg_count = std::min(buffer_length, ArgsLength());
            for (; i < min_arg_count; ++i)
                buffer[i] = JsValueFromV8LocalValue(cbinfo_[i]);
            if (i < buffer_length) {
                napi_value undefined = JsValueFromV8LocalValue(v8::Undefined(cbinfo_.GetIsolate()));
                for (; i < buffer_length; ++i)
                    buffer[i] = undefined;
            }
        }

        napi_value This() { return JsValueFromV8LocalValue(cbinfo_.This()); }
        size_t ArgsLength() { return static_cast<size_t>(cbinfo_.Length()); }
        void *Data() { return bundle_->cb_data; }

    private:
        explicit FunctionCallbackWrapper(const v8::FunctionCallbackInfo<v8::Value> &cbinfo) :
            cbinfo_(cbinfo), bundle_(CallbackBundle::FromCallbackData(cbinfo.Data())) {}

        void InvokeCallback() {
            napi_callback_info cbinfo_wrapper = reinterpret_cast<napi_callback_info>(this);
            napi_env env = bundle_->env;
            napi_callback cb = bundle_->cb;
            // On construction of an object that reserves fast-call slots (internal
            // fields 0=native, 1=type tag; every napi_define_class instance does,
            // see function.cc / fast_call.cc), initialize them to nullptr *before*
            // running the user constructor (which may napi_fast_wrap and thus
            // overwrite them). Without this, napi_fast_unwrap on an instance that
            // was never napi_fast_wrap'd reads an uninitialized aligned-pointer
            // field — V8 undefined behavior yielding a garbage pointer. The
            // IsConstructCall() guard keeps the (hot) method-call path free.
            if (cbinfo_.IsConstructCall()) {
                v8::Local<v8::Object> self = cbinfo_.This();
                int fields = self->InternalFieldCount();
                if (fields >= 1)
                    self->SetAlignedPointerInInternalField(0, nullptr, v8::kEmbedderDataTypeTagDefault);
                if (fields >= 2)
                    self->SetAlignedPointerInInternalField(1, nullptr, v8::kEmbedderDataTypeTagDefault);
            }
            napi_value result = nullptr;
            bool exceptionOccurred = false;
            env->CallIntoModule([&](napi_env env) { result = cb(env, cbinfo_wrapper); },
                                [&](napi_env env, v8::Local<v8::Value> value) {
                                    exceptionOccurred = true;
                                    if (env->terminatedOrTerminating())
                                        return;
                                    env->isolate->ThrowException(value);
                                });
            if (!exceptionOccurred && (result != nullptr)) {
                cbinfo_.GetReturnValue().Set(V8LocalValueFromJsValue(result));
            }
        }

    private:
        const v8::FunctionCallbackInfo<v8::Value> &cbinfo_;
        CallbackBundle *bundle_;
    };

    inline napi_status Wrap(napi_env env, napi_value js_object, void *native_object, napi_finalize finalize_cb,
                            void *finalize_hint, napi_ref *result) {
        NAPI_PREAMBLE(env);
        CHECK_ARG(env, js_object);

        v8::Local<v8::Context> context = env->context();
        v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(js_object);
        RETURN_STATUS_IF_FALSE(env, value->IsObject(), napi_invalid_arg);
        v8::Local<v8::Object> obj = value.As<v8::Object>();

        RETURN_STATUS_IF_FALSE(env, !obj->HasPrivate(context, NAPI_PRIVATE_KEY(context, wrapper)).FromJust(),
                               napi_invalid_arg);

        v8impl::Reference *reference = nullptr;
        if (result != nullptr) {
            CHECK_ARG(env, finalize_cb);
            reference = v8impl::ReferenceWithFinalizer::New(env, obj, 0, v8impl::ReferenceOwnership::kUserland,
                                                            finalize_cb, native_object, finalize_hint);
            *result = reinterpret_cast<napi_ref>(reference);
        } else if (finalize_cb != nullptr) {
            reference = v8impl::ReferenceWithFinalizer::New(env, obj, 0, v8impl::ReferenceOwnership::kRuntime,
                                                            finalize_cb, native_object, finalize_hint);
        } else {
            reference =
                    v8impl::ReferenceWithData::New(env, obj, 0, v8impl::ReferenceOwnership::kRuntime, native_object);
        }

        CHECK(obj->SetPrivate(context, NAPI_PRIVATE_KEY(context, wrapper), v8::External::New(env->isolate, reference))
                      .FromJust());

        // Mirror the native pointer into internal field 0 (when the object
        // reserves one — every napi_define_class instance does) so napi_unwrap can
        // retrieve it with an O(1) field read instead of a private-property
        // lookup. Only 2-byte-aligned, non-null pointers fit an aligned-pointer
        // slot; anything else simply isn't mirrored and falls back to the slow
        // path (correctness preserved, just not accelerated).
        if (obj->InternalFieldCount() >= 1 && native_object != nullptr &&
            (reinterpret_cast<uintptr_t>(native_object) & 1u) == 0) {
            obj->SetAlignedPointerInInternalField(0, native_object, v8::kEmbedderDataTypeTagDefault);
        }
        return GET_RETURN_STATUS(env);
    }

    inline bool CanBeHeldWeakly(v8::Local<v8::Value> value) { return value->IsObject() || value->IsSymbol(); }

    class ExternalWrapper {
    private:
        explicit ExternalWrapper(void *data) : data_(data), type_tag_{0, 0} {}

        static void WeakCallback(const v8::WeakCallbackInfo<ExternalWrapper> &data) {
            ExternalWrapper *wrapper = data.GetParameter();
            delete wrapper;
        }

    public:
        static v8::Local<v8::External> New(napi_env env, void *data) {
            ExternalWrapper *wrapper = new ExternalWrapper(data);
            v8::Local<v8::External> external = v8::External::New(env->isolate, wrapper);
            wrapper->persistent_.Reset(env->isolate, external);
            wrapper->persistent_.SetWeak(wrapper, WeakCallback, v8::WeakCallbackType::kParameter);
            return external;
        }

        static ExternalWrapper *From(v8::Local<v8::External> external) {
            return static_cast<ExternalWrapper *>(external->Value());
        }

        void *Data() { return data_; }

        bool TypeTag(const napi_type_tag *type_tag) {
            if (has_tag_)
                return false;
            type_tag_ = *type_tag;
            has_tag_ = true;
            return true;
        }

        bool CheckTypeTag(const napi_type_tag *type_tag) {
            return has_tag_ && type_tag->lower == type_tag_.lower && type_tag->upper == type_tag_.upper;
        }

    private:
        v8impl::Persistent<v8::Value> persistent_;
        void *data_;
        napi_type_tag type_tag_;
        bool has_tag_ = false;
    };

} // namespace v8impl

// Local helper used by napi_create_*_error / napi_throw_*_error.
inline napi_status set_error_code(napi_env env, v8::Local<v8::Value> error, napi_value code, const char *code_cstring) {
    if ((code != nullptr) || (code_cstring != nullptr)) {
        v8::Local<v8::Context> context = env->context();
        v8::Local<v8::Object> err_object = error.As<v8::Object>();

        v8::Local<v8::Value> code_value = v8impl::V8LocalValueFromJsValue(code);
        if (code != nullptr) {
            code_value = v8impl::V8LocalValueFromJsValue(code);
            RETURN_STATUS_IF_FALSE(env, code_value->IsString(), napi_string_expected);
        } else {
            CHECK_NEW_FROM_UTF8(env, code_value, code_cstring);
        }

        v8::Local<v8::Name> code_key;
        CHECK_NEW_FROM_UTF8(env, code_key, "code");

        v8::Maybe<bool> set_maybe = err_object->Set(context, code_key, code_value);
        RETURN_STATUS_IF_FALSE(env, set_maybe.FromMaybe(false), napi_generic_failure);
    }
    return napi_ok;
}

#endif // SRC_JS_NATIVE_API_V8_IMPL_H_
