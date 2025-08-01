/* -*- Mode: c++; c-basic-offset: 2; tab-width: 20; indent-tabs-mode: nil; -*-
 * vim: set sw=2 ts=4 expandtab:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EventDispatcher.h"

#include "JavaBuiltins.h"
#include "nsAppShell.h"
#include "nsJSUtils.h"
#include "js/Array.h"  // JS::GetArrayLength, JS::IsArrayObject, JS::NewArrayObject
#include "js/PropertyAndElement.h"  // JS_Enumerate, JS_GetElement, JS_GetProperty, JS_GetPropertyById, JS_SetElement, JS_SetUCProperty
#include "js/String.h"              // JS::StringHasLatin1Chars
#include "js/Warnings.h"            // JS::WarnUTF8
#include "xpcpublic.h"

#include "mozilla/fallible.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/java/EventCallbackWrappers.h"
#include "mozilla/jni/GeckoBundleUtils.h"

namespace mozilla::widget {

namespace detail {

bool CheckJS(JSContext* aCx, bool aResult) {
  if (!aResult) {
    JS_ClearPendingException(aCx);
  }
  return aResult;
}

nsresult BoxData(const nsAString& aEvent, JSContext* aCx,
                 JS::Handle<JS::Value> aData, jni::Object::LocalRef& aOut,
                 bool aObjectOnly) {
  nsresult rv = jni::BoxData(aCx, aData, aOut, aObjectOnly);
  if (rv != NS_ERROR_INVALID_ARG) {
    return rv;
  }

  NS_ConvertUTF16toUTF8 event(aEvent);
  if (JS_IsExceptionPending(aCx)) {
    JS::WarnUTF8(aCx, "Error dispatching %s", event.get());
  } else {
    JS_ReportErrorUTF8(aCx, "Invalid event data for %s", event.get());
  }
  return NS_ERROR_INVALID_ARG;
}

nsresult UnboxString(JSContext* aCx, const jni::Object::LocalRef& aData,
                     JS::MutableHandle<JS::Value> aOut) {
  if (!aData) {
    aOut.setNull();
    return NS_OK;
  }

  MOZ_ASSERT(aData.IsInstanceOf<jni::String>());

  JNIEnv* const env = aData.Env();
  const jstring jstr = jstring(aData.Get());
  const size_t len = env->GetStringLength(jstr);
  const jchar* const jchars = env->GetStringChars(jstr, nullptr);

  if (NS_WARN_IF(!jchars)) {
    env->ExceptionClear();
    return NS_ERROR_FAILURE;
  }

  auto releaseStr = MakeScopeExit([env, jstr, jchars] {
    env->ReleaseStringChars(jstr, jchars);
    env->ExceptionClear();
  });

  JS::Rooted<JSString*> str(
      aCx,
      JS_NewUCStringCopyN(aCx, reinterpret_cast<const char16_t*>(jchars), len));
  NS_ENSURE_TRUE(CheckJS(aCx, !!str), NS_ERROR_FAILURE);

  aOut.setString(str);
  return NS_OK;
}

nsresult UnboxValue(JSContext* aCx, const jni::Object::LocalRef& aData,
                    JS::MutableHandle<JS::Value> aOut);

nsresult UnboxBundle(JSContext* aCx, const jni::Object::LocalRef& aData,
                     JS::MutableHandle<JS::Value> aOut) {
  if (!aData) {
    aOut.setNull();
    return NS_OK;
  }

  MOZ_ASSERT(aData.IsInstanceOf<java::GeckoBundle>());

  JNIEnv* const env = aData.Env();
  const auto& bundle = java::GeckoBundle::Ref::From(aData);
  jni::ObjectArray::LocalRef keys = bundle->Keys();
  jni::ObjectArray::LocalRef values = bundle->Values();
  const size_t len = keys->Length();
  JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));

  NS_ENSURE_TRUE(CheckJS(aCx, !!obj), NS_ERROR_FAILURE);
  NS_ENSURE_TRUE(values->Length() == len, NS_ERROR_FAILURE);

  for (size_t i = 0; i < len; i++) {
    jni::String::LocalRef key = keys->GetElement(i);
    const size_t keyLen = env->GetStringLength(key.Get());
    const jchar* const keyChars = env->GetStringChars(key.Get(), nullptr);
    if (NS_WARN_IF(!keyChars)) {
      env->ExceptionClear();
      return NS_ERROR_FAILURE;
    }

    auto releaseKeyChars = MakeScopeExit([env, &key, keyChars] {
      env->ReleaseStringChars(key.Get(), keyChars);
      env->ExceptionClear();
    });

    JS::Rooted<JS::Value> value(aCx);
    nsresult rv = UnboxValue(aCx, values->GetElement(i), &value);
    if (rv == NS_ERROR_INVALID_ARG && !JS_IsExceptionPending(aCx)) {
      JS_ReportErrorUTF8(
          aCx, "Invalid event data property %s",
          NS_ConvertUTF16toUTF8(reinterpret_cast<const char16_t*>(keyChars),
                                keyLen)
              .get());
    }
    NS_ENSURE_SUCCESS(rv, rv);

    NS_ENSURE_TRUE(
        CheckJS(aCx, JS_SetUCProperty(
                         aCx, obj, reinterpret_cast<const char16_t*>(keyChars),
                         keyLen, value)),
        NS_ERROR_FAILURE);
  }

  aOut.setObject(*obj);
  return NS_OK;
}

template <typename Type, typename JNIType, typename ArrayType,
          JNIType* (JNIEnv::*GetElements)(ArrayType, jboolean*),
          void (JNIEnv::*ReleaseElements)(ArrayType, JNIType*, jint),
          JS::Value (*ToValue)(Type)>
nsresult UnboxArrayPrimitive(JSContext* aCx, const jni::Object::LocalRef& aData,
                             JS::MutableHandle<JS::Value> aOut) {
  JNIEnv* const env = aData.Env();
  const ArrayType jarray = ArrayType(aData.Get());
  JNIType* const array = (env->*GetElements)(jarray, nullptr);
  JS::RootedVector<JS::Value> elements(aCx);

  if (NS_WARN_IF(!array)) {
    env->ExceptionClear();
    return NS_ERROR_FAILURE;
  }

  auto releaseArray = MakeScopeExit([env, jarray, array] {
    (env->*ReleaseElements)(jarray, array, JNI_ABORT);
    env->ExceptionClear();
  });

  const size_t len = env->GetArrayLength(jarray);
  NS_ENSURE_TRUE(elements.initCapacity(len), NS_ERROR_FAILURE);

  for (size_t i = 0; i < len; i++) {
    NS_ENSURE_TRUE(elements.append((*ToValue)(Type(array[i]))),
                   NS_ERROR_FAILURE);
  }

  JS::Rooted<JSObject*> obj(
      aCx, JS::NewArrayObject(aCx, JS::HandleValueArray(elements)));
  NS_ENSURE_TRUE(CheckJS(aCx, !!obj), NS_ERROR_FAILURE);

  aOut.setObject(*obj);
  return NS_OK;
}

struct StringArray : jni::ObjectBase<StringArray> {
  static const char name[];
};

struct GeckoBundleArray : jni::ObjectBase<GeckoBundleArray> {
  static const char name[];
};

const char StringArray::name[] = "[Ljava/lang/String;";
const char GeckoBundleArray::name[] = "[Lorg/mozilla/gecko/util/GeckoBundle;";

template <nsresult (*Unbox)(JSContext*, const jni::Object::LocalRef&,
                            JS::MutableHandle<JS::Value>)>
nsresult UnboxArrayObject(JSContext* aCx, const jni::Object::LocalRef& aData,
                          JS::MutableHandle<JS::Value> aOut) {
  jni::ObjectArray::LocalRef array(aData.Env(),
                                   jni::ObjectArray::Ref::From(aData));
  const size_t len = array->Length();
  JS::Rooted<JSObject*> obj(aCx, JS::NewArrayObject(aCx, len));
  NS_ENSURE_TRUE(CheckJS(aCx, !!obj), NS_ERROR_FAILURE);

  for (size_t i = 0; i < len; i++) {
    jni::Object::LocalRef element = array->GetElement(i);
    JS::Rooted<JS::Value> value(aCx);
    nsresult rv = (*Unbox)(aCx, element, &value);
    NS_ENSURE_SUCCESS(rv, rv);

    NS_ENSURE_TRUE(CheckJS(aCx, JS_SetElement(aCx, obj, i, value)),
                   NS_ERROR_FAILURE);
  }

  aOut.setObject(*obj);
  return NS_OK;
}

nsresult UnboxValue(JSContext* aCx, const jni::Object::LocalRef& aData,
                    JS::MutableHandle<JS::Value> aOut) {
  using jni::Java2Native;

  if (!aData) {
    aOut.setNull();
  } else if (aData.IsInstanceOf<jni::Boolean>()) {
    aOut.setBoolean(Java2Native<bool>(aData, aData.Env()));
  } else if (aData.IsInstanceOf<jni::Integer>()) {
    aOut.setInt32(Java2Native<int>(aData, aData.Env()));
  } else if (aData.IsInstanceOf<jni::Byte>() ||
             aData.IsInstanceOf<jni::Short>()) {
    aOut.setInt32(java::sdk::Number::Ref::From(aData)->IntValue());
  } else if (aData.IsInstanceOf<jni::Double>()) {
    aOut.setNumber(Java2Native<double>(aData, aData.Env()));
  } else if (aData.IsInstanceOf<jni::Float>() ||
             aData.IsInstanceOf<jni::Long>()) {
    aOut.setNumber(java::sdk::Number::Ref::From(aData)->DoubleValue());
  } else if (aData.IsInstanceOf<jni::String>()) {
    return UnboxString(aCx, aData, aOut);
  } else if (aData.IsInstanceOf<jni::Character>()) {
    return UnboxString(aCx, java::sdk::String::ValueOf(aData), aOut);
  } else if (aData.IsInstanceOf<java::GeckoBundle>()) {
    return UnboxBundle(aCx, aData, aOut);

  } else if (aData.IsInstanceOf<jni::BooleanArray>()) {
    return UnboxArrayPrimitive<
        bool, jboolean, jbooleanArray, &JNIEnv::GetBooleanArrayElements,
        &JNIEnv::ReleaseBooleanArrayElements, &JS::BooleanValue>(aCx, aData,
                                                                 aOut);

  } else if (aData.IsInstanceOf<jni::IntArray>()) {
    return UnboxArrayPrimitive<
        int32_t, jint, jintArray, &JNIEnv::GetIntArrayElements,
        &JNIEnv::ReleaseIntArrayElements, &JS::Int32Value>(aCx, aData, aOut);

  } else if (aData.IsInstanceOf<jni::DoubleArray>()) {
    return UnboxArrayPrimitive<
        double, jdouble, jdoubleArray, &JNIEnv::GetDoubleArrayElements,
        &JNIEnv::ReleaseDoubleArrayElements, &JS::DoubleValue>(aCx, aData,
                                                               aOut);

  } else if (aData.IsInstanceOf<StringArray>()) {
    return UnboxArrayObject<&UnboxString>(aCx, aData, aOut);
  } else if (aData.IsInstanceOf<GeckoBundleArray>()) {
    return UnboxArrayObject<&UnboxBundle>(aCx, aData, aOut);
  } else {
    NS_WARNING("Invalid type");
    return NS_ERROR_INVALID_ARG;
  }
  return NS_OK;
}

nsresult UnboxData(jni::String::Param aEvent, JSContext* aCx,
                   jni::Object::Param aData, JS::MutableHandle<JS::Value> aOut,
                   bool aBundleOnly) {
  MOZ_ASSERT(NS_IsMainThread());

  jni::Object::LocalRef jniData(jni::GetGeckoThreadEnv(), aData);
  nsresult rv = NS_ERROR_INVALID_ARG;

  if (!aBundleOnly) {
    rv = UnboxValue(aCx, jniData, aOut);
  } else if (!jniData || jniData.IsInstanceOf<java::GeckoBundle>()) {
    rv = UnboxBundle(aCx, jniData, aOut);
  }
  if (rv != NS_ERROR_INVALID_ARG || !aEvent) {
    return rv;
  }

  nsCString event = aEvent->ToCString();
  if (JS_IsExceptionPending(aCx)) {
    JS::WarnUTF8(aCx, "Error dispatching %s", event.get());
  } else {
    JS_ReportErrorUTF8(aCx, "Invalid event data for %s", event.get());
  }
  return NS_ERROR_INVALID_ARG;
}

class JavaCallbackDelegate final : public nsIGeckoViewEventCallback {
  const java::EventCallback::GlobalRef mCallback;

  virtual ~JavaCallbackDelegate() {}

  NS_IMETHOD Call(JSContext* aCx, JS::Handle<JS::Value> aData,
                  void (java::EventCallback::*aCall)(jni::Object::Param)
                      const) {
    MOZ_ASSERT(NS_IsMainThread());

    jni::Object::LocalRef data(jni::GetGeckoThreadEnv());
    nsresult rv = BoxData(u"callback"_ns, aCx, aData, data,
                          /* ObjectOnly */ false);
    NS_ENSURE_SUCCESS(rv, rv);

    dom::AutoNoJSAPI nojsapi;

    (java::EventCallback(*mCallback).*aCall)(data);
    return NS_OK;
  }

 public:
  explicit JavaCallbackDelegate(java::EventCallback::Param aCallback)
      : mCallback(jni::GetGeckoThreadEnv(), aCallback) {}

  NS_DECL_ISUPPORTS

  NS_IMETHOD OnSuccess(JS::Handle<JS::Value> aData, JSContext* aCx) override {
    return Call(aCx, aData, &java::EventCallback::SendSuccess);
  }

  NS_IMETHOD OnError(JS::Handle<JS::Value> aData, JSContext* aCx) override {
    return Call(aCx, aData, &java::EventCallback::SendError);
  }
};

NS_IMPL_ISUPPORTS(JavaCallbackDelegate, nsIGeckoViewEventCallback)

class NativeCallbackDelegateSupport final
    : public java::EventDispatcher::NativeCallbackDelegate ::Natives<
          NativeCallbackDelegateSupport> {
  using CallbackDelegate = java::EventDispatcher::NativeCallbackDelegate;
  using Base = CallbackDelegate::Natives<NativeCallbackDelegateSupport>;

  const nsCOMPtr<nsIGeckoViewEventCallback> mCallback;

  void Call(jni::Object::Param aData,
            nsresult (nsIGeckoViewEventCallback::*aCall)(JS::Handle<JS::Value>,
                                                         JSContext*)) {
    MOZ_ASSERT(NS_IsMainThread());

    // Use either the attached window's realm or a default realm.

    dom::AutoJSAPI jsapi;
    NS_ENSURE_TRUE_VOID(jsapi.Init(xpc::PrivilegedJunkScope()));

    JS::Rooted<JS::Value> data(jsapi.cx());
    nsresult rv = UnboxData(u"callback"_ns, jsapi.cx(), aData, &data,
                            /* BundleOnly */ false);
    NS_ENSURE_SUCCESS_VOID(rv);

    rv = (mCallback->*aCall)(data, jsapi.cx());
    NS_ENSURE_SUCCESS_VOID(rv);
  }

 public:
  using Base::AttachNative;

  template <typename Functor>
  static void OnNativeCall(Functor&& aCall) {
    if (NS_IsMainThread()) {
      // Invoke callbacks synchronously if we're already on Gecko thread.
      return aCall();
    }
    NS_DispatchToMainThread(
        NS_NewRunnableFunction("OnNativeCall", std::forward<Functor>(aCall)));
  }

  static void Finalize(const CallbackDelegate::LocalRef& aInstance) {
    DisposeNative(aInstance);
  }

  explicit NativeCallbackDelegateSupport(nsIGeckoViewEventCallback* callback)
      : mCallback(callback) {}

  void SendSuccess(jni::Object::Param aData) {
    Call(aData, &nsIGeckoViewEventCallback::OnSuccess);
  }

  void SendError(jni::Object::Param aData) {
    Call(aData, &nsIGeckoViewEventCallback::OnError);
  }
};

}  // namespace detail

using namespace detail;

void EventDispatcher::DispatchToGecko(jni::String::Param aEvent,
                                      jni::Object::Param aData,
                                      jni::Object::Param aCallback) {
  AssertIsOnMainThread();

  nsCOMPtr<nsIGeckoViewEventCallback> callback;
  if (aCallback) {
    callback =
        new JavaCallbackDelegate(java::EventCallback::Ref::From(aCallback));
  }

  dom::AutoJSAPI jsapi;
  NS_ENSURE_TRUE_VOID(jsapi.Init(xpc::PrivilegedJunkScope()));

  JS::Rooted<JS::Value> data(jsapi.cx());
  nsresult rv =
      UnboxData(aEvent, jsapi.cx(), aData, &data, /* BundleOnly */ true);
  NS_ENSURE_SUCCESS_VOID(rv);

  DispatchToGecko(jsapi.cx(), aEvent->ToString(), data, callback);
}

bool EventDispatcher::HasEmbedderListener(const nsAString& aEvent) {
  AssertIsOnMainThread();
  java::EventDispatcher::LocalRef dispatcher = GetDispatcher();
  if (!dispatcher) {
    return false;
  }

  return dispatcher->HasListener(aEvent);
}

nsresult EventDispatcher::DispatchToEmbedder(
    JSContext* aCx, const nsAString& aEvent, JS::Handle<JS::Value> aData,
    nsIGeckoViewEventCallback* aCallback) {
  AssertIsOnMainThread();
  JNIEnv* env = jni::GetGeckoThreadEnv();

  java::EventDispatcher::LocalRef dispatcher = GetDispatcher();
  if (!dispatcher) {
    return NS_OK;
  }

  jni::Object::LocalRef data(env);
  nsresult rv = BoxData(aEvent, aCx, aData, data, /* ObjectOnly */ true);
  NS_ENSURE_SUCCESS(rv, rv);

  java::EventDispatcher::NativeCallbackDelegate::LocalRef callback(env);
  if (aCallback) {
    callback = java::EventDispatcher::NativeCallbackDelegate::New();
    NativeCallbackDelegateSupport::AttachNative(
        callback, MakeUnique<NativeCallbackDelegateSupport>(aCallback));
  }

  dom::AutoNoJSAPI nojsapi;
  dispatcher->DispatchToThreads(aEvent, data, callback);
  return NS_OK;
}

void EventDispatcher::Attach(java::EventDispatcher::Param aDispatcher) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aDispatcher);

  java::EventDispatcher::LocalRef dispatcher = GetDispatcher();

  if (dispatcher) {
    if (dispatcher == aDispatcher) {
      return;
    }
    dispatcher->SetAttachedToGecko(java::EventDispatcher::REATTACHING);
  }

  dispatcher = java::EventDispatcher::LocalRef(aDispatcher);
  NativesBase::AttachNative(dispatcher, this);
  mDispatcher = dispatcher;

  dispatcher->SetAttachedToGecko(java::EventDispatcher::ATTACHED);
}

void EventDispatcher::Detach() {
  AssertIsOnMainThread();

  java::EventDispatcher::GlobalRef dispatcher = GetDispatcher();

  // SetAttachedToGecko will call disposeNative for us later on the Gecko
  // thread to make sure all pending dispatchToGecko calls have completed.
  if (dispatcher) {
    dispatcher->SetAttachedToGecko(java::EventDispatcher::DETACHED);
  }

  mDispatcher = nullptr;
  Shutdown();
}

/* static */
nsresult EventDispatcher::UnboxBundle(JSContext* aCx, jni::Object::Param aData,
                                      JS::MutableHandle<JS::Value> aOut) {
  return detail::UnboxBundle(aCx, aData, aOut);
}

}  // namespace mozilla::widget
