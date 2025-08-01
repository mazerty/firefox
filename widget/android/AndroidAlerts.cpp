/* -*- Mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AndroidAlerts.h"
#include "mozilla/dom/notification/NotificationHandler.h"
#include "mozilla/java/GeckoRuntimeWrappers.h"
#include "mozilla/java/WebNotificationWrappers.h"
#include "mozilla/java/WebNotificationActionWrappers.h"
#include "nsContentUtils.h"
#include "nsIPrincipal.h"
#include "nsIScriptSecurityManager.h"
#include "nsIURI.h"

using namespace mozilla::dom::notification;

namespace mozilla {
namespace widget {

NS_IMPL_ISUPPORTS(AndroidAlerts, nsIAlertsService)

struct AndroidNotificationTuple {
  // Can be null if the caller doesn't care about the result.
  nsCOMPtr<nsIObserver> mObserver;

  // The Gecko alert notification.
  nsCOMPtr<nsIAlertNotification> mAlert;

  // The Java represented form of mAlert.
  mozilla::java::WebNotification::GlobalRef mNotificationRef;
};

using NotificationMap = nsTHashMap<nsStringHashKey, AndroidNotificationTuple>;
static StaticAutoPtr<NotificationMap> sNotificationMap;

NS_IMETHODIMP
AndroidAlerts::ShowAlertNotification(
    const nsAString& aImageUrl, const nsAString& aAlertTitle,
    const nsAString& aAlertText, bool aAlertTextClickable,
    const nsAString& aAlertCookie, nsIObserver* aAlertListener,
    const nsAString& aAlertName, const nsAString& aBidi, const nsAString& aLang,
    const nsAString& aData, nsIPrincipal* aPrincipal, bool aInPrivateBrowsing,
    bool aRequireInteraction) {
  MOZ_ASSERT_UNREACHABLE("Should be implemented by nsAlertsService.");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
AndroidAlerts::ShowAlert(nsIAlertNotification* aAlert,
                         nsIObserver* aAlertListener) {
  // nsAlertsService disables our alerts backend if we ever return failure
  // here. To keep the backend enabled, we always return NS_OK even if we
  // encounter an error here.
  nsresult rv;

  nsAutoString imageUrl;
  rv = aAlert->GetImageURL(imageUrl);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsAutoString title;
  rv = aAlert->GetTitle(title);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsAutoString text;
  rv = aAlert->GetText(text);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsAutoString cookie;
  rv = aAlert->GetCookie(cookie);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsAutoString name;
  rv = aAlert->GetName(name);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsAutoString lang;
  rv = aAlert->GetLang(lang);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsAutoString dir;
  rv = aAlert->GetDir(dir);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  bool requireInteraction;
  rv = aAlert->GetRequireInteraction(&requireInteraction);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsCOMPtr<nsIURI> uri;
  rv = aAlert->GetURI(getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsCString spec;
  if (uri) {
    rv = uri->GetDisplaySpec(spec);
    NS_ENSURE_SUCCESS(rv, NS_OK);
  }

  bool silent;
  rv = aAlert->GetSilent(&silent);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  bool privateBrowsing;
  rv = aAlert->GetInPrivateBrowsing(&privateBrowsing);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsTArray<uint32_t> vibrate;
  rv = aAlert->GetVibrate(vibrate);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  nsTArray<RefPtr<nsIAlertAction>> nsActions;
  MOZ_TRY(aAlert->GetActions(nsActions));
  jni::ObjectArray::LocalRef actions =
      jni::ObjectArray::New(nsActions.Length());
  size_t index = 0;
  for (auto& nsAction : nsActions) {
    nsAutoString name;
    MOZ_TRY(nsAction->GetAction(name));

    nsAutoString title;
    MOZ_TRY(nsAction->GetTitle(title));

    java::WebNotificationAction::LocalRef action =
        java::WebNotificationAction::New(name, title);
    actions->SetElement(index, action);
    ++index;
  }

  nsAutoCString origin;
  rv = aAlert->GetOrigin(origin);
  NS_ENSURE_SUCCESS(rv, NS_OK);

  if (!sNotificationMap) {
    sNotificationMap = new NotificationMap();
  } else if (Maybe<AndroidNotificationTuple> tuple =
                 sNotificationMap->Extract(name)) {
    if (tuple->mObserver) {
      tuple->mObserver->Observe(nullptr, "alertfinished", nullptr);
    }
  }

  java::WebNotification::LocalRef notification = notification->New(
      title, name, cookie, text, imageUrl, dir, lang, requireInteraction, spec,
      silent, privateBrowsing, jni::IntArray::From(vibrate), actions, origin);
  AndroidNotificationTuple tuple{
      .mObserver = aAlertListener,
      .mAlert = aAlert,
      .mNotificationRef = notification,
  };
  sNotificationMap->InsertOrUpdate(name, std::move(tuple));

  if (java::GeckoRuntime::LocalRef runtime =
          java::GeckoRuntime::GetInstance()) {
    runtime->NotifyOnShow(notification);
  }

  return NS_OK;
}

NS_IMETHODIMP
AndroidAlerts::CloseAlert(const nsAString& aAlertName, bool aContextClosed) {
  if (!sNotificationMap) {
    return NS_OK;
  }

  Maybe<AndroidNotificationTuple> tuple = sNotificationMap->Extract(aAlertName);
  if (!tuple) {
    return NS_OK;
  }

  if (tuple->mObserver) {
    // All CloseAlert implementation is expected to fire alertfinished
    // synchronously. (See bug 1975432 to deduplicate this logic)
    // We have to fire alertfinished here as we are closing it ourselves;
    // GeckoView will only send it when it's closed from Android side.
    tuple->mObserver->Observe(nullptr, "alertfinished", nullptr);
  }

  java::GeckoRuntime::LocalRef runtime = java::GeckoRuntime::GetInstance();
  if (runtime != NULL) {
    runtime->NotifyOnClose(tuple->mNotificationRef);
  }

  return NS_OK;
}

NS_IMETHODIMP AndroidAlerts::GetHistory(nsTArray<nsString>& aResult) {
  // TODO: Implement this using NotificationManager.getActiveNotifications
  // https://developer.android.com/reference/android/app/NotificationManager#getActiveNotifications()
  // See bug 1971394.
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP AndroidAlerts::Teardown() {
  sNotificationMap = nullptr;
  return NS_OK;
}

NS_IMETHODIMP AndroidAlerts::PbmTeardown() { return NS_ERROR_NOT_IMPLEMENTED; }

nsresult RespondViaNotificationHandler(const nsAString& aName,
                                       const nsACString& aTopic,
                                       Maybe<nsString> aAction,
                                       const nsACString& aOrigin) {
  if (aTopic != "alertclickcallback"_ns) {
    // NOTE(krosylight): we are not handling alertfinished as we don't want to
    // open the app for each notification dismiss.
    return NS_OK;
  }

  nsCOMPtr<nsIPrincipal> principal;
  if (nsCOMPtr<nsIScriptSecurityManager> ssm =
          nsContentUtils::GetSecurityManager()) {
    MOZ_TRY(ssm->CreateContentPrincipalFromOrigin(aOrigin,
                                                  getter_AddRefs(principal)));
  } else {
    return NS_ERROR_NOT_AVAILABLE;
  }

  RefPtr<NotificationHandler> handler = NotificationHandler::GetSingleton();
  MOZ_TRY(handler->RespondOnClick(principal, aName, aAction ? *aAction : u""_ns,
                                  /* aAutoClosed */ !aAction, nullptr));
  return NS_OK;
}

void AndroidAlerts::NotifyListener(const nsAString& aName, const char* aTopic,
                                   Maybe<nsString> aAction,
                                   const nsACString& aOrigin) {
  nsDependentCString topic(aTopic);

  if (!sNotificationMap) {
    RespondViaNotificationHandler(aName, topic, aAction, aOrigin);
    return;
  }

  Maybe<AndroidNotificationTuple> tuple = sNotificationMap->MaybeGet(aName);
  if (!tuple) {
    RespondViaNotificationHandler(aName, topic, aAction, aOrigin);
    return;
  }

  if (tuple->mObserver) {
    nsCOMPtr<nsIAlertAction> action;
    if (aAction) {
      tuple->mAlert->GetAction(*aAction, getter_AddRefs(action));
    }
    tuple->mObserver->Observe(action, aTopic, nullptr);
  }

  if ("alertfinished"_ns.Equals(topic)) {
    sNotificationMap->Remove(aName);
  }
}

}  // namespace widget
}  // namespace mozilla
