// Out-of-line class method bodies that back the v8impl helpers declared in
// js_native_api_v8.h (RefTracker / Reference / Finalizer / TrackedFinalizer /
// ReferenceWith{Data,Finalizer}). Also hosts napi_env__::InvokeFinalizerFromGC.
// Per-category .cc files (error.cc, value.cc, ...) link against these.

#define NAPI_EXPERIMENTAL
#include "js_native_api_v8.h"
#include "js_native_api_v8_impl.h"
#include "napi/js_native_api.h"

void napi_env__::InvokeFinalizerFromGC(v8impl::RefTracker *finalizer) {
    if (module_api_version != NAPI_VERSION_EXPERIMENTAL) {
        EnqueueFinalizer(finalizer);
    } else {
        // The experimental code calls finalizers immediately to release native
        // objects as soon as possible. In that state any code that may affect GC
        // state causes a fatal error. To work around this issue the finalizer code
        // can call node_api_post_finalizer.
        auto restore_state = node::OnScopeLeave([this, saved = in_gc_finalizer] { in_gc_finalizer = saved; });
        in_gc_finalizer = true;
        finalizer->Finalize();
    }
}

namespace v8impl {

    void Finalizer::ResetEnv() { env_ = nullptr; }

    void Finalizer::ResetFinalizer() {
        finalize_callback_ = nullptr;
        finalize_data_ = nullptr;
        finalize_hint_ = nullptr;
    }

    void Finalizer::CallFinalizer() {
        napi_finalize finalize_callback = finalize_callback_;
        void *finalize_data = finalize_data_;
        void *finalize_hint = finalize_hint_;
        ResetFinalizer();

        if (finalize_callback == nullptr)
            return;
        if (env_ == nullptr) {
            // The environment is dead. Call the finalizer directly.
            finalize_callback(nullptr, finalize_data, finalize_hint);
        } else {
            env_->CallFinalizer(finalize_callback, finalize_data, finalize_hint);
        }
    }

    TrackedFinalizer::TrackedFinalizer(napi_env env, napi_finalize finalize_callback, void *finalize_data,
                                       void *finalize_hint) :
        RefTracker(),
        finalizer_(env, finalize_callback, finalize_data, finalize_hint) {}

    TrackedFinalizer *TrackedFinalizer::New(napi_env env, napi_finalize finalize_callback, void *finalize_data,
                                            void *finalize_hint) {
        TrackedFinalizer *finalizer = new TrackedFinalizer(env, finalize_callback, finalize_data, finalize_hint);
        finalizer->Link(&env->finalizing_reflist);
        return finalizer;
    }

    TrackedFinalizer::~TrackedFinalizer() {
        Unlink();
        finalizer_.env()->DequeueFinalizer(this);
    }

    void TrackedFinalizer::Finalize() {
        Unlink();
        finalizer_.CallFinalizer();
        delete this;
    }

    Reference::Reference(napi_env env, v8::Local<v8::Value> value, uint32_t initial_refcount,
                         ReferenceOwnership ownership) :
        RefTracker(),
        persistent_(env->isolate, value), refcount_(initial_refcount), ownership_(ownership),
        can_be_weak_(CanBeHeldWeakly(value)) {
        if (refcount_ == 0)
            SetWeak();
    }

    Reference::~Reference() {
        persistent_.Reset();
        Unlink();
    }

    Reference *Reference::New(napi_env env, v8::Local<v8::Value> value, uint32_t initial_refcount,
                              ReferenceOwnership ownership) {
        Reference *reference = new Reference(env, value, initial_refcount, ownership);
        reference->Link(&env->reflist);
        return reference;
    }

    uint32_t Reference::Ref() {
        if (persistent_.IsEmpty())
            return 0;
        if (++refcount_ == 1 && can_be_weak_)
            persistent_.ClearWeak();
        return refcount_;
    }

    uint32_t Reference::Unref() {
        if (persistent_.IsEmpty() || refcount_ == 0)
            return 0;
        if (--refcount_ == 0)
            SetWeak();
        return refcount_;
    }

    v8::Local<v8::Value> Reference::Get(napi_env env) {
        if (persistent_.IsEmpty())
            return v8::Local<v8::Value>();
        return v8::Local<v8::Value>::New(env->isolate, persistent_);
    }

    void Reference::Finalize() {
        persistent_.Reset();
        // kRuntime refs are always freed by the runtime. Userland refs are the
        // user's to delete via napi_delete_reference — EXCEPT during env teardown
        // (EnvTeardownFlag), where the env is dying and the user can never delete
        // them again: free them here to avoid leaking one Reference per un-deleted
        // userland ref for the life of the process (accumulates under multi-env
        // embedding). No double-free: napi_delete_reference unlinks first, so a
        // ref already deleted by the user is off the reflist and never reaches here.
        bool deleteMe = ownership_ == ReferenceOwnership::kRuntime || v8impl::EnvTeardownFlag();
        Unlink();
        CallUserFinalizer();
        if (deleteMe)
            delete this;
    }

    void Reference::InvokeFinalizerFromGC() { Finalize(); }

    void Reference::SetWeak() {
        if (can_be_weak_) {
            persistent_.SetWeak(this, WeakCallback, v8::WeakCallbackType::kParameter);
        } else {
            persistent_.Reset();
        }
    }

    void Reference::WeakCallback(const v8::WeakCallbackInfo<Reference> &data) {
        Reference *reference = data.GetParameter();
        reference->persistent_.Reset();
        reference->InvokeFinalizerFromGC();
    }

    ReferenceWithData *ReferenceWithData::New(napi_env env, v8::Local<v8::Value> value, uint32_t initial_refcount,
                                              ReferenceOwnership ownership, void *data) {
        ReferenceWithData *reference = new ReferenceWithData(env, value, initial_refcount, ownership, data);
        reference->Link(&env->reflist);
        return reference;
    }

    ReferenceWithData::ReferenceWithData(napi_env env, v8::Local<v8::Value> value, uint32_t initial_refcount,
                                         ReferenceOwnership ownership, void *data) :
        Reference(env, value, initial_refcount, ownership),
        data_(data) {}

    ReferenceWithFinalizer *ReferenceWithFinalizer::New(napi_env env, v8::Local<v8::Value> value,
                                                        uint32_t initial_refcount, ReferenceOwnership ownership,
                                                        napi_finalize finalize_callback, void *finalize_data,
                                                        void *finalize_hint) {
        ReferenceWithFinalizer *reference = new ReferenceWithFinalizer(env, value, initial_refcount, ownership,
                                                                       finalize_callback, finalize_data, finalize_hint);
        reference->Link(&env->finalizing_reflist);
        return reference;
    }

    ReferenceWithFinalizer::ReferenceWithFinalizer(napi_env env, v8::Local<v8::Value> value, uint32_t initial_refcount,
                                                   ReferenceOwnership ownership, napi_finalize finalize_callback,
                                                   void *finalize_data, void *finalize_hint) :
        Reference(env, value, initial_refcount, ownership),
        finalizer_(env, finalize_callback, finalize_data, finalize_hint) {}

    ReferenceWithFinalizer::~ReferenceWithFinalizer() { finalizer_.env()->DequeueFinalizer(this); }

    void ReferenceWithFinalizer::CallUserFinalizer() { finalizer_.CallFinalizer(); }

    void ReferenceWithFinalizer::InvokeFinalizerFromGC() { finalizer_.env()->InvokeFinalizerFromGC(this); }

} // namespace v8impl
