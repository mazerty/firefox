/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/StaticPtr.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/jni/GeckoBundleUtils.h"

#include "AndroidWebAuthnService.h"
#include "JavaBuiltins.h"
#include "JavaExceptions.h"
#include "WebAuthnPromiseHolder.h"
#include "WebAuthnEnumStrings.h"
#include "WebAuthnResult.h"
#include "mozilla/StaticPrefs_security.h"
#include "mozilla/java/WebAuthnTokenManagerWrappers.h"
#include "mozilla/jni/Conversions.h"

namespace mozilla {
namespace jni {
template <>
dom::AndroidWebAuthnError Java2Native(mozilla::jni::Object::Param aData,
                                      JNIEnv* aEnv) {
  MOZ_ASSERT(aData.IsInstanceOf<jni::Throwable>());
  java::sdk::Throwable::LocalRef throwable(aData);
  return dom::AndroidWebAuthnError(throwable->GetMessage()->ToString());
}
}  // namespace jni

namespace dom {

NS_IMPL_ISUPPORTS(AndroidWebAuthnService, nsIWebAuthnService)

NS_IMETHODIMP
AndroidWebAuthnService::GetIsUVPAA(bool* aAvailable) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::MakeCredential(uint64_t aTransactionId,
                                       uint64_t aBrowsingContextId,
                                       nsIWebAuthnRegisterArgs* aArgs,
                                       nsIWebAuthnRegisterPromise* aPromise) {
  Reset();

  GetMainThreadSerialEventTarget()->Dispatch(NS_NewRunnableFunction(
      "java::WebAuthnTokenManager::WebAuthnMakeCredential",
      [aArgs = RefPtr{aArgs}, aPromise = RefPtr{aPromise}]() {
        AssertIsOnMainThread();

        GECKOBUNDLE_START(credentialBundle);
        GECKOBUNDLE_PUT(credentialBundle, "isWebAuthn",
                        java::sdk::Integer::ValueOf(1));

        {
          GECKOBUNDLE_START(rpBundle);

          nsString rpId;
          Unused << aArgs->GetRpId(rpId);
          GECKOBUNDLE_PUT(rpBundle, "id", jni::StringParam(rpId));

          nsString rpName;
          Unused << aArgs->GetRpName(rpName);
          GECKOBUNDLE_PUT(rpBundle, "name", jni::StringParam(rpName));

          GECKOBUNDLE_FINISH(rpBundle);
          GECKOBUNDLE_PUT(credentialBundle, "rp", rpBundle);
        }

        {
          GECKOBUNDLE_START(userBundle);

          nsString userName;
          Unused << aArgs->GetUserName(userName);
          GECKOBUNDLE_PUT(userBundle, "name", jni::StringParam(userName));

          nsString userDisplayName;
          Unused << aArgs->GetUserDisplayName(userDisplayName);
          GECKOBUNDLE_PUT(userBundle, "displayName",
                          jni::StringParam(userDisplayName));

          GECKOBUNDLE_FINISH(userBundle);
          GECKOBUNDLE_PUT(credentialBundle, "user", userBundle);
        }

        nsString origin;
        Unused << aArgs->GetOrigin(origin);
        GECKOBUNDLE_PUT(credentialBundle, "origin", jni::StringParam(origin));

        uint32_t timeout;
        Unused << aArgs->GetTimeoutMS(&timeout);
        GECKOBUNDLE_PUT(credentialBundle, "timeout",
                        java::sdk::Double::New(timeout));

        // Add UI support to consent to attestation, bug 1550164
        GECKOBUNDLE_PUT(credentialBundle, "attestation",
                        jni::StringParam(u"none"_ns));

        GECKOBUNDLE_FINISH(credentialBundle);

        nsTArray<uint8_t> userId;
        Unused << aArgs->GetUserId(userId);
        jni::ByteBuffer::LocalRef uid = jni::ByteBuffer::New(
            const_cast<void*>(static_cast<const void*>(userId.Elements())),
            userId.Length());

        nsTArray<uint8_t> challBuf;
        Unused << aArgs->GetChallenge(challBuf);
        jni::ByteBuffer::LocalRef challenge = jni::ByteBuffer::New(
            const_cast<void*>(static_cast<const void*>(challBuf.Elements())),
            challBuf.Length());

        nsTArray<nsTArray<uint8_t>> excludeList;
        Unused << aArgs->GetExcludeList(excludeList);
        jni::ObjectArray::LocalRef idList =
            jni::ObjectArray::New(excludeList.Length());
        int ix = 0;
        for (const nsTArray<uint8_t>& credId : excludeList) {
          jni::ByteBuffer::LocalRef id = jni::ByteBuffer::New(
              const_cast<void*>(static_cast<const void*>(credId.Elements())),
              credId.Length());

          idList->SetElement(ix, id);

          ix += 1;
        }

        nsTArray<uint8_t> transportBuf;
        Unused << aArgs->GetExcludeListTransports(transportBuf);
        jni::ByteBuffer::LocalRef transportList = jni::ByteBuffer::New(
            const_cast<void*>(
                static_cast<const void*>(transportBuf.Elements())),
            transportBuf.Length());

        nsTArray<uint8_t> clientDataHash;
        Unused << aArgs->GetClientDataHash(clientDataHash);
        jni::ByteBuffer::LocalRef hash = jni::ByteBuffer::New(
            const_cast<void*>(
                static_cast<const void*>(clientDataHash.Elements())),
            clientDataHash.Length());

        nsTArray<int32_t> coseAlgs;
        Unused << aArgs->GetCoseAlgs(coseAlgs);
        jni::IntArray::LocalRef algs =
            jni::IntArray::New(coseAlgs.Elements(), coseAlgs.Length());

        GECKOBUNDLE_START(authSelBundle);

        nsString residentKey;
        Unused << aArgs->GetResidentKey(residentKey);

        // Get extensions
        bool requestedCredProps;
        Unused << aArgs->GetCredProps(&requestedCredProps);

        // Unfortunately, GMS's FIDO2 API has no option for Passkey. If using
        // residentKey, credential will be synced with Passkey via Google
        // account or credential provider service. So this is experimental.
        if (requestedCredProps &&
            StaticPrefs::
                security_webauthn_webauthn_enable_android_fido2_residentkey()) {
          GECKOBUNDLE_PUT(authSelBundle, "residentKey",
                          jni::StringParam(residentKey));
        }

        nsString userVerification;
        Unused << aArgs->GetUserVerification(userVerification);
        if (userVerification.EqualsLiteral(
                MOZ_WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED)) {
          GECKOBUNDLE_PUT(authSelBundle, "requireUserVerification",
                          java::sdk::Integer::ValueOf(1));
        }

        nsString authenticatorAttachment;
        nsresult rv =
            aArgs->GetAuthenticatorAttachment(authenticatorAttachment);
        if (rv != NS_ERROR_NOT_AVAILABLE) {
          if (NS_FAILED(rv)) {
            aPromise->Reject(rv);
            return;
          }
          if (authenticatorAttachment.EqualsLiteral(
                  MOZ_WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM) ||
              authenticatorAttachment.EqualsLiteral(
                  MOZ_WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM)) {
            GECKOBUNDLE_PUT(authSelBundle, "authenticatorAttachment",
                            jni::StringParam(authenticatorAttachment));
          }
        }
        GECKOBUNDLE_FINISH(authSelBundle);

        GECKOBUNDLE_START(extensionsBundle);
        GECKOBUNDLE_PUT(extensionsBundle, "credProps",
                        requestedCredProps ? java::sdk::Boolean::TRUE()
                                           : java::sdk::Boolean::FALSE());
        GECKOBUNDLE_FINISH(extensionsBundle);

        auto result = java::WebAuthnTokenManager::WebAuthnMakeCredential(
            credentialBundle, uid, challenge, idList, transportList,
            authSelBundle, extensionsBundle, algs, hash);

        auto geckoResult = java::GeckoResult::LocalRef(std::move(result));

        MozPromise<RefPtr<WebAuthnRegisterResult>, AndroidWebAuthnError,
                   true>::FromGeckoResult(geckoResult)
            ->Then(
                GetCurrentSerialEventTarget(), __func__,
                [aPromise](RefPtr<WebAuthnRegisterResult>&& aValue) {
                  aPromise->Resolve(aValue);
                },
                [aPromise](AndroidWebAuthnError&& aValue) {
                  aPromise->Reject(aValue.GetError());
                });
      }));

  return NS_OK;
}

NS_IMETHODIMP
AndroidWebAuthnService::GetAssertion(uint64_t aTransactionId,
                                     uint64_t aBrowsingContextId,
                                     nsIWebAuthnSignArgs* aArgs,
                                     nsIWebAuthnSignPromise* aPromise) {
  Reset();

  bool conditionallyMediated;
  Unused << aArgs->GetConditionallyMediated(&conditionallyMediated);
  if (conditionallyMediated) {
    aPromise->Reject(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
    return NS_OK;
  }

  GetMainThreadSerialEventTarget()->Dispatch(NS_NewRunnableFunction(
      "java::WebAuthnTokenManager::WebAuthnGetAssertion",
      [self = RefPtr{this}, aArgs = RefPtr{aArgs},
       aPromise = RefPtr{aPromise}]() {
        AssertIsOnMainThread();

        nsTArray<uint8_t> challBuf;
        Unused << aArgs->GetChallenge(challBuf);
        jni::ByteBuffer::LocalRef challenge = jni::ByteBuffer::New(
            const_cast<void*>(static_cast<const void*>(challBuf.Elements())),
            challBuf.Length());

        nsTArray<nsTArray<uint8_t>> allowList;
        Unused << aArgs->GetAllowList(allowList);
        jni::ObjectArray::LocalRef idList =
            jni::ObjectArray::New(allowList.Length());
        int ix = 0;
        for (const nsTArray<uint8_t>& credId : allowList) {
          jni::ByteBuffer::LocalRef id = jni::ByteBuffer::New(
              const_cast<void*>(static_cast<const void*>(credId.Elements())),
              credId.Length());

          idList->SetElement(ix, id);

          ix += 1;
        }

        nsTArray<uint8_t> clientDataHash;
        Unused << aArgs->GetClientDataHash(clientDataHash);
        jni::ByteBuffer::LocalRef hash = jni::ByteBuffer::New(
            const_cast<void*>(
                static_cast<const void*>(clientDataHash.Elements())),
            clientDataHash.Length());

        nsTArray<uint8_t> transportBuf;
        Unused << aArgs->GetAllowListTransports(transportBuf);
        jni::ByteBuffer::LocalRef transportList = jni::ByteBuffer::New(
            const_cast<void*>(
                static_cast<const void*>(transportBuf.Elements())),
            transportBuf.Length());

        GECKOBUNDLE_START(assertionBundle);

        GECKOBUNDLE_PUT(assertionBundle, "isWebAuthn",
                        java::sdk::Integer::ValueOf(1));

        nsString rpId;
        Unused << aArgs->GetRpId(rpId);
        GECKOBUNDLE_PUT(assertionBundle, "rpId", jni::StringParam(rpId));

        nsString origin;
        Unused << aArgs->GetOrigin(origin);
        GECKOBUNDLE_PUT(assertionBundle, "origin", jni::StringParam(origin));

        uint32_t timeout;
        Unused << aArgs->GetTimeoutMS(&timeout);
        GECKOBUNDLE_PUT(assertionBundle, "timeout",
                        java::sdk::Double::New(timeout));

        nsString userVerification;
        Unused << aArgs->GetUserVerification(userVerification);
        GECKOBUNDLE_PUT(assertionBundle, "userVerification",
                        jni::StringParam(userVerification));

        GECKOBUNDLE_FINISH(assertionBundle);

        GECKOBUNDLE_START(extensionsBundle);

        nsString appId;
        nsresult rv = aArgs->GetAppId(appId);
        if (rv != NS_ERROR_NOT_AVAILABLE) {
          if (NS_FAILED(rv)) {
            aPromise->Reject(NS_ERROR_DOM_NOT_ALLOWED_ERR);
            return;
          }
          GECKOBUNDLE_PUT(extensionsBundle, "fidoAppId",
                          jni::StringParam(appId));
        }

        GECKOBUNDLE_FINISH(extensionsBundle);

        auto result = java::WebAuthnTokenManager::WebAuthnGetAssertion(
            challenge, idList, transportList, assertionBundle, extensionsBundle,
            hash);
        auto geckoResult = java::GeckoResult::LocalRef(std::move(result));
        MozPromise<RefPtr<WebAuthnSignResult>, AndroidWebAuthnError,
                   true>::FromGeckoResult(geckoResult)
            ->Then(
                GetCurrentSerialEventTarget(), __func__,
                [aPromise](RefPtr<WebAuthnSignResult>&& aValue) {
                  aPromise->Resolve(aValue);
                },
                [aPromise](AndroidWebAuthnError&& aValue) {
                  aPromise->Reject(aValue.GetError());
                });
      }));

  return NS_OK;
}

NS_IMETHODIMP
AndroidWebAuthnService::Reset() {
  mRegisterCredPropsRk.reset();

  return NS_OK;
}

NS_IMETHODIMP
AndroidWebAuthnService::Cancel(uint64_t aTransactionId) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::HasPendingConditionalGet(uint64_t aBrowsingContextId,
                                                 const nsAString& aOrigin,
                                                 uint64_t* aRv) {
  // Signal that there is no pending conditional get request, so the caller
  // will not attempt to call GetAutoFillEntries, SelectAutoFillEntry, or
  // ResumeConditionalGet (as these are not implemented).
  *aRv = 0;
  return NS_OK;
}

NS_IMETHODIMP
AndroidWebAuthnService::GetAutoFillEntries(
    uint64_t aTransactionId, nsTArray<RefPtr<nsIWebAuthnAutoFillEntry>>& aRv) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::SelectAutoFillEntry(
    uint64_t aTransactionId, const nsTArray<uint8_t>& aCredentialId) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::ResumeConditionalGet(uint64_t aTransactionId) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::PinCallback(uint64_t aTransactionId,
                                    const nsACString& aPin) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::SetHasAttestationConsent(uint64_t aTransactionId,
                                                 bool aHasConsent) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::SelectionCallback(uint64_t aTransactionId,
                                          uint64_t aIndex) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::AddVirtualAuthenticator(
    const nsACString& aProtocol, const nsACString& aTransport,
    bool aHasResidentKey, bool aHasUserVerification, bool aIsUserConsenting,
    bool aIsUserVerified, nsACString& aRetval) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::RemoveVirtualAuthenticator(
    const nsACString& aAuthenticatorId) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::AddCredential(const nsACString& aAuthenticatorId,
                                      const nsACString& aCredentialId,
                                      bool aIsResidentCredential,
                                      const nsACString& aRpId,
                                      const nsACString& aPrivateKey,
                                      const nsACString& aUserHandle,
                                      uint32_t aSignCount) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::GetCredentials(
    const nsACString& aAuthenticatorId,
    nsTArray<RefPtr<nsICredentialParameters>>& _aRetval) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::RemoveCredential(const nsACString& aAuthenticatorId,
                                         const nsACString& aCredentialId) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::RemoveAllCredentials(
    const nsACString& aAuthenticatorId) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::SetUserVerified(const nsACString& aAuthenticatorId,
                                        bool aIsUserVerified) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidWebAuthnService::Listen() { return NS_ERROR_NOT_IMPLEMENTED; }

NS_IMETHODIMP
AndroidWebAuthnService::RunCommand(const nsACString& aCmd) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

}  // namespace dom
}  // namespace mozilla
