/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/net/UrlClassifierFeatureFactory.h"

// List of Features
#include "UrlClassifierFeatureAntiFraudAnnotation.h"
#include "UrlClassifierFeatureCryptominingAnnotation.h"
#include "UrlClassifierFeatureCryptominingProtection.h"
#include "UrlClassifierFeatureConsentManagerAnnotation.h"
#include "UrlClassifierFeatureEmailTrackingDataCollection.h"
#include "UrlClassifierFeatureEmailTrackingProtection.h"
#include "UrlClassifierFeatureFingerprintingAnnotation.h"
#include "UrlClassifierFeatureFingerprintingProtection.h"
#include "UrlClassifierFeaturePhishingProtection.h"
#include "UrlClassifierFeatureSocialTrackingAnnotation.h"
#include "UrlClassifierFeatureSocialTrackingProtection.h"
#include "UrlClassifierFeatureTrackingProtection.h"
#include "UrlClassifierFeatureTrackingAnnotation.h"
#include "UrlClassifierFeatureCustomTables.h"

#include "nsIWebProgressListener.h"
#include "nsAppRunner.h"

namespace mozilla {
namespace net {

/* static */
void UrlClassifierFeatureFactory::Shutdown() {
  // We want to expose Features only in the parent process.
  if (!XRE_IsParentProcess()) {
    return;
  }

  UrlClassifierFeatureCryptominingAnnotation::MaybeShutdown();
  UrlClassifierFeatureCryptominingProtection::MaybeShutdown();
  UrlClassifierFeatureConsentManagerAnnotation::MaybeShutdown();
  UrlClassifierFeatureAntiFraudAnnotation::MaybeShutdown();
  UrlClassifierFeatureEmailTrackingDataCollection::MaybeShutdown();
  UrlClassifierFeatureEmailTrackingProtection::MaybeShutdown();
  UrlClassifierFeatureFingerprintingAnnotation::MaybeShutdown();
  UrlClassifierFeatureFingerprintingProtection::MaybeShutdown();
  UrlClassifierFeaturePhishingProtection::MaybeShutdown();
  UrlClassifierFeatureSocialTrackingAnnotation::MaybeShutdown();
  UrlClassifierFeatureSocialTrackingProtection::MaybeShutdown();
  UrlClassifierFeatureTrackingAnnotation::MaybeShutdown();
  UrlClassifierFeatureTrackingProtection::MaybeShutdown();
}

/* static */
void UrlClassifierFeatureFactory::GetFeaturesFromChannel(
    nsIChannel* aChannel,
    nsTArray<nsCOMPtr<nsIUrlClassifierFeature>>& aFeatures) {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(aChannel);

  nsCOMPtr<nsIUrlClassifierFeature> feature;

  // Note that the order of the features is extremely important! When more than
  // 1 feature classifies the channel, we call ::ProcessChannel() following this
  // feature order, and this could produce different results with a different
  // feature ordering.

  // Email Tracking Data Collection
  // This needs to be run before other features so that other blocking features
  // won't stop us to collect data for email trackers. Note that this feature
  // is not a blocking feature.
  feature =
      UrlClassifierFeatureEmailTrackingDataCollection::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Consent Manager Annotation
  // This must be run before any blocking features because the annotation will
  // affect whether the channel should be blocked.
  feature = UrlClassifierFeatureConsentManagerAnnotation::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Anti-fraud Annotation
  // This must be run before any blocking features because the annotation will
  // affect whether the channel should be blocked.
  feature = UrlClassifierFeatureAntiFraudAnnotation::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Email Tracking Protection
  feature = UrlClassifierFeatureEmailTrackingProtection::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Cryptomining Protection
  feature = UrlClassifierFeatureCryptominingProtection::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Fingerprinting Protection
  feature = UrlClassifierFeatureFingerprintingProtection::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // SocialTracking Protection
  feature = UrlClassifierFeatureSocialTrackingProtection::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Tracking Protection
  feature = UrlClassifierFeatureTrackingProtection::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Cryptomining Annotation
  feature = UrlClassifierFeatureCryptominingAnnotation::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Fingerprinting Annotation
  feature = UrlClassifierFeatureFingerprintingAnnotation::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // SocialTracking Annotation
  feature = UrlClassifierFeatureSocialTrackingAnnotation::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }

  // Tracking Annotation
  feature = UrlClassifierFeatureTrackingAnnotation::MaybeCreate(aChannel);
  if (feature) {
    aFeatures.AppendElement(feature);
  }
}

/* static */
void UrlClassifierFeatureFactory::GetPhishingProtectionFeatures(
    nsTArray<RefPtr<nsIUrlClassifierFeature>>& aFeatures) {
  UrlClassifierFeaturePhishingProtection::MaybeCreate(aFeatures);
}

/* static */
already_AddRefed<nsIUrlClassifierFeature>
UrlClassifierFeatureFactory::GetFeatureByName(const nsACString& aName) {
  if (!XRE_IsParentProcess()) {
    return nullptr;
  }

  nsCOMPtr<nsIUrlClassifierFeature> feature;

  // Anti-fraud Annotation
  feature = UrlClassifierFeatureAntiFraudAnnotation::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Cryptomining Annotation
  feature = UrlClassifierFeatureCryptominingAnnotation::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Cryptomining Protection
  feature = UrlClassifierFeatureCryptominingProtection::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Consent Manager Annotation
  feature =
      UrlClassifierFeatureConsentManagerAnnotation::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Email Tracking Data Collection
  feature =
      UrlClassifierFeatureEmailTrackingDataCollection::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Email Tracking Protection
  feature =
      UrlClassifierFeatureEmailTrackingProtection::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Fingerprinting Annotation
  feature =
      UrlClassifierFeatureFingerprintingAnnotation::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Fingerprinting Protection
  feature =
      UrlClassifierFeatureFingerprintingProtection::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // SocialTracking Annotation
  feature =
      UrlClassifierFeatureSocialTrackingAnnotation::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // SocialTracking Protection
  feature =
      UrlClassifierFeatureSocialTrackingProtection::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Tracking Protection
  feature = UrlClassifierFeatureTrackingProtection::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // Tracking Annotation
  feature = UrlClassifierFeatureTrackingAnnotation::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  // PhishingProtection features
  feature = UrlClassifierFeaturePhishingProtection::GetIfNameMatches(aName);
  if (feature) {
    return feature.forget();
  }

  return nullptr;
}

/* static */
void UrlClassifierFeatureFactory::GetFeatureNames(nsTArray<nsCString>& aArray) {
  if (!XRE_IsParentProcess()) {
    return;
  }

  nsAutoCString name;

  // Anti-fraud Annotation
  name.Assign(UrlClassifierFeatureAntiFraudAnnotation::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Cryptomining Annotation
  name.Assign(UrlClassifierFeatureCryptominingAnnotation::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Cryptomining Protection
  name.Assign(UrlClassifierFeatureCryptominingProtection::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Consent Manager Annotation
  name.Assign(UrlClassifierFeatureConsentManagerAnnotation::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Email Tracking Data Collection
  name.Assign(UrlClassifierFeatureEmailTrackingDataCollection::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Email Tracking Protection
  name.Assign(UrlClassifierFeatureEmailTrackingProtection::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Fingerprinting Annotation
  name.Assign(UrlClassifierFeatureFingerprintingAnnotation::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Fingerprinting Protection
  name.Assign(UrlClassifierFeatureFingerprintingProtection::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // SocialTracking Annotation
  name.Assign(UrlClassifierFeatureSocialTrackingAnnotation::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // SocialTracking Protection
  name.Assign(UrlClassifierFeatureSocialTrackingProtection::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Tracking Protection
  name.Assign(UrlClassifierFeatureTrackingProtection::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // Tracking Annotation
  name.Assign(UrlClassifierFeatureTrackingAnnotation::Name());
  if (!name.IsEmpty()) {
    aArray.AppendElement(name);
  }

  // PhishingProtection features
  {
    nsTArray<nsCString> features;
    UrlClassifierFeaturePhishingProtection::GetFeatureNames(features);
    aArray.AppendElements(features);
  }
}

/* static */
already_AddRefed<nsIUrlClassifierFeature>
UrlClassifierFeatureFactory::CreateFeatureWithTables(
    const nsACString& aName, const nsTArray<nsCString>& aBlocklistTables,
    const nsTArray<nsCString>& aEntitylistTables) {
  nsCOMPtr<nsIUrlClassifierFeature> feature =
      new UrlClassifierFeatureCustomTables(aName, aBlocklistTables,
                                           aEntitylistTables);
  return feature.forget();
}

namespace {

struct BlockingErrorCode {
  nsresult mErrorCode;
  uint32_t mBlockingEventCode;
  const char* mConsoleMessage;
  nsLiteralCString mConsoleCategory;
};

static constexpr BlockingErrorCode sBlockingErrorCodes[] = {
    {NS_ERROR_TRACKING_URI,
     nsIWebProgressListener::STATE_BLOCKED_TRACKING_CONTENT,
     "TrackerUriBlockedByETP", "Tracking Protection"_ns},
    {NS_ERROR_FINGERPRINTING_URI,
     nsIWebProgressListener::STATE_BLOCKED_FINGERPRINTING_CONTENT,
     "TrackerUriBlockedByETP", "Tracking Protection"_ns},
    {NS_ERROR_CRYPTOMINING_URI,
     nsIWebProgressListener::STATE_BLOCKED_CRYPTOMINING_CONTENT,
     "TrackerUriBlockedByETP", "Tracking Protection"_ns},
    {NS_ERROR_SOCIALTRACKING_URI,
     nsIWebProgressListener::STATE_BLOCKED_SOCIALTRACKING_CONTENT,
     "TrackerUriBlockedByETP", "Tracking Protection"_ns},
    {NS_ERROR_EMAILTRACKING_URI,
     nsIWebProgressListener::STATE_BLOCKED_EMAILTRACKING_CONTENT,
     "TrackerUriBlockedByETP", "Tracking Protection"_ns},
};

}  // namespace

/* static */
bool UrlClassifierFeatureFactory::IsClassifierBlockingErrorCode(
    nsresult aError) {
  // In theory we can iterate through the features, but at the moment, we can
  // just have a simple check here.
  for (const auto& blockingErrorCode : sBlockingErrorCodes) {
    if (aError == blockingErrorCode.mErrorCode) {
      return true;
    }
  }

  return false;
}

/* static */
bool UrlClassifierFeatureFactory::IsClassifierBlockingEventCode(
    uint32_t aEventCode) {
  for (const auto& blockingErrorCode : sBlockingErrorCodes) {
    if (aEventCode == blockingErrorCode.mBlockingEventCode) {
      return true;
    }
  }
  return false;
}

/* static */
uint32_t UrlClassifierFeatureFactory::GetClassifierBlockingEventCode(
    nsresult aErrorCode) {
  for (const auto& blockingErrorCode : sBlockingErrorCodes) {
    if (aErrorCode == blockingErrorCode.mErrorCode) {
      return blockingErrorCode.mBlockingEventCode;
    }
  }
  return 0;
}

/* static */ const char*
UrlClassifierFeatureFactory::ClassifierBlockingErrorCodeToConsoleMessage(
    nsresult aError, nsACString& aCategory) {
  for (const auto& blockingErrorCode : sBlockingErrorCodes) {
    if (aError == blockingErrorCode.mErrorCode) {
      aCategory = blockingErrorCode.mConsoleCategory;
      return blockingErrorCode.mConsoleMessage;
    }
  }

  return nullptr;
}

}  // namespace net
}  // namespace mozilla
