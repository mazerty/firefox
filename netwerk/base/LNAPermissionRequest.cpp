/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "LNAPermissionRequest.h"
#include "nsGlobalWindowInner.h"
#include "mozilla/dom/Document.h"
#include "nsPIDOMWindow.h"
#include "mozilla/Preferences.h"
#include "nsContentUtils.h"

namespace mozilla::net {

//-------------------------------------------------
// LNA Permission Requests
//-------------------------------------------------

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(LNAPermissionRequest,
                                               ContentPermissionRequestBase)

NS_IMPL_CYCLE_COLLECTION_INHERITED(LNAPermissionRequest,
                                   ContentPermissionRequestBase)

LNAPermissionRequest::LNAPermissionRequest(PermissionPromptCallback&& aCallback,
                                           nsILoadInfo* aLoadInfo,
                                           const nsACString& aType)
    : dom::ContentPermissionRequestBase(
          aLoadInfo->GetLoadingPrincipal(), nullptr,
          (aType.Equals(LOCAL_HOST_PERMISSION_KEY) ? "network.localhost"_ns
                                                   : "network.localnetwork"_ns),
          aType),
      mPermissionPromptCallback(std::move(aCallback)) {
  MOZ_ASSERT(aLoadInfo);

  aLoadInfo->GetTriggeringPrincipal(getter_AddRefs(mPrincipal));
  mTopLevelPrincipal = aLoadInfo->GetTopLevelPrincipal();
  if (!mTopLevelPrincipal) {
    // top-level principal is not always available we could re-use the
    // triggering principal for this
    mTopLevelPrincipal = mPrincipal;
  }

  mLoadInfo = aLoadInfo;

  MOZ_ASSERT(mPrincipal);
}

NS_IMETHODIMP
LNAPermissionRequest::GetElement(mozilla::dom::Element** aElement) {
  NS_ENSURE_ARG_POINTER(aElement);
  RefPtr<mozilla::dom::BrowsingContext> bc;
  mLoadInfo->GetBrowsingContext(getter_AddRefs(bc));
  if (!bc) {
    return NS_ERROR_FAILURE;
  }

  return bc->GetTopFrameElement(aElement);
}

// callback when the permission request is denied
NS_IMETHODIMP
LNAPermissionRequest::Cancel() {
  // callback to the http channel on the prompt failure result
  mPermissionPromptCallback(false, mType);
  return NS_OK;
}

// callback when the permission request is allowed
NS_IMETHODIMP
LNAPermissionRequest::Allow(JS::Handle<JS::Value> aChoices) {
  // callback to the http channel on the prompt success result
  mPermissionPromptCallback(true, mType);
  return NS_OK;
}

nsresult LNAPermissionRequest::RequestPermission() {
  MOZ_ASSERT(NS_IsMainThread());
  // This check always returns true
  // See Bug 1978550
  if (!CheckPermissionDelegate()) {
    return Cancel();
  }

  PromptResult pr = CheckPromptPrefs();
  if (pr == PromptResult::Granted) {
    return Allow(JS::UndefinedHandleValue);
  }

  if (pr == PromptResult::Denied) {
    return Cancel();
  }

  if (NS_SUCCEEDED(
          dom::nsContentPermissionUtils::AskPermission(this, mWindow))) {
    // Here we could be getting synchronous callback from the prompts depending
    // on whether there is already a permission for this or not. If we have a
    // permission, we will get a synchronous callback Allow/Deny and async if
    // we don't have a permission yet and waiting for user permission.
    return NS_OK;
  }

  return Cancel();
}

}  // namespace mozilla::net
