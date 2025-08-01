/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_IPC_MFCDMPARENT_H_
#define DOM_MEDIA_IPC_MFCDMPARENT_H_

#include <wrl.h>

#include "mozilla/Assertions.h"
#include "mozilla/EventTargetAndLockCapability.h"
#include "mozilla/PMFCDMParent.h"
#include "mozilla/RefPtr.h"
#include "MFCDMExtra.h"
#include "MFCDMSession.h"
#include "MFPMPHostWrapper.h"
#include "RemoteMediaManagerParent.h"

namespace mozilla {

class MFCDMProxy;

/**
 * MFCDMParent is a wrapper class for the Media Foundation CDM in the utility
 * process.
 * It's responsible to create and manage a CDM and its sessions, and acts as a
 * proxy to the Media Foundation interfaces
 * (https://learn.microsoft.com/en-us/windows/win32/api/mfcontentdecryptionmodule/)
 * by accepting calls from and calling back to MFCDMChild in the content
 * process.
 */
class MFCDMParent final : public PMFCDMParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MFCDMParent);

  MFCDMParent(const nsAString& aKeySystem, RemoteMediaManagerParent* aManager,
              nsISerialEventTarget* aManagerThread);

  static void SetWidevineL1Path(const char* aPath);

  // Perform clean-up when shutting down the MFCDM process.
  static void Shutdown();

  // Return capabilities from all key systems which the media foundation CDM
  // supports.
  using CapabilitiesPromise =
      MozPromise<CopyableTArray<MFCDMCapabilitiesIPDL>, nsresult, true>;
  static RefPtr<CapabilitiesPromise> GetAllKeySystemsCapabilities();

  static MFCDMParent* GetCDMById(uint64_t aId) {
    MOZ_ASSERT(sRegisteredCDMs.Contains(aId));
    return sRegisteredCDMs.Get(aId);
  }
  uint64_t Id() const { return mId; }

  mozilla::ipc::IPCResult RecvGetCapabilities(
      const MFCDMCapabilitiesRequest& aRequest,
      GetCapabilitiesResolver&& aResolver);

  mozilla::ipc::IPCResult RecvInit(const MFCDMInitParamsIPDL& aParams,
                                   InitResolver&& aResolver);

  mozilla::ipc::IPCResult RecvCreateSessionAndGenerateRequest(
      const MFCDMCreateSessionParamsIPDL& aParams,
      CreateSessionAndGenerateRequestResolver&& aResolver);

  mozilla::ipc::IPCResult RecvLoadSession(
      const KeySystemConfig::SessionType& aSessionType,
      const nsString& aSessionId, LoadSessionResolver&& aResolver);

  mozilla::ipc::IPCResult RecvUpdateSession(
      const nsString& aSessionId, const CopyableTArray<uint8_t>& aResponse,
      UpdateSessionResolver&& aResolver);

  mozilla::ipc::IPCResult RecvCloseSession(const nsString& aSessionId,
                                           UpdateSessionResolver&& aResolver);

  mozilla::ipc::IPCResult RecvRemoveSession(const nsString& aSessionId,
                                            UpdateSessionResolver&& aResolver);

  mozilla::ipc::IPCResult RecvSetServerCertificate(
      const CopyableTArray<uint8_t>& aCertificate,
      UpdateSessionResolver&& aResolver);

  mozilla::ipc::IPCResult RecvGetStatusForPolicy(
      const dom::HDCPVersion& aMinHdcpVersion,
      GetStatusForPolicyResolver&& aResolver);

  // A thread-safe method to access the CDM proxy. Returns nullptr if the CDM
  // has been shut down.
  MFCDMProxy* GetMFCDMProxy();

  void ShutdownCDM();

  void Destroy();

 private:
  ~MFCDMParent();

  enum class CapabilitesFlag {
    HarewareDecryption,
    NeedHDCPCheck,
    NeedClearLeadCheck,
    IsPrivateBrowsing,
  };
  using CapabilitesFlagSet = EnumSet<CapabilitesFlag, uint8_t>;

  static LPCWSTR GetCDMLibraryName(const nsString& aKeySystem);

  static HRESULT GetOrCreateFactory(
      const nsString& aKeySystem,
      Microsoft::WRL::ComPtr<IMFContentDecryptionModuleFactory>& aFactoryOut);

  static HRESULT LoadFactory(
      const nsString& aKeySystem,
      Microsoft::WRL::ComPtr<IMFContentDecryptionModuleFactory>& aFactoryOut);

  static void GetCapabilities(const nsString& aKeySystem,
                              const CapabilitesFlagSet& aFlags,
                              IMFContentDecryptionModuleFactory* aFactory,
                              MFCDMCapabilitiesIPDL& aCapabilitiesOut);

  void Register();
  void Unregister();

  void ConnectSessionEvents(MFCDMSession* aSession);

  MFCDMSession* GetSession(const nsString& aSessionId);

  mozilla::Mutex& Mutex() MOZ_RETURN_CAPABILITY(mCDMAccessLock.Lock()) {
    return mCDMAccessLock.Lock();
  }

  nsString mKeySystem;

  const RefPtr<RemoteMediaManagerParent> mManager;
  const RefPtr<nsISerialEventTarget> mManagerThread;

  MOZ_RUNINIT static inline nsTHashMap<nsUint64HashKey, MFCDMParent*>
      sRegisteredCDMs;

  static inline uint64_t sNextId = 1;
  const uint64_t mId;
  bool mIsInited = false;

  static inline BSTR sWidevineL1Path;

  RefPtr<MFCDMParent> mIPDLSelfRef;
  Microsoft::WRL::ComPtr<IMFContentDecryptionModuleFactory> mFactory;
  Microsoft::WRL::ComPtr<MFPMPHostWrapper> mPMPHostWrapper;

  std::map<nsString, UniquePtr<MFCDMSession>> mSessions;

  MediaEventForwarder<MFCDMKeyMessage> mKeyMessageEvents;
  MediaEventForwarder<MFCDMKeyStatusChange> mKeyChangeEvents;
  MediaEventForwarder<MFCDMKeyExpiration> mExpirationEvents;
  MediaEventForwarder<MFCDMSessionClosedResult> mClosedEvents;

  MediaEventListener mKeyMessageListener;
  MediaEventListener mKeyChangeListener;
  MediaEventListener mExpirationListener;
  MediaEventListener mClosedListener;

  // The mCDM and mCDMProxy members are exclusively modified on the manager
  // thread, while being read-only on other threads. To ensure thread-safe
  // access, we employ the EventTargetAndLockCapability mechanism.
  mozilla::EventTargetAndLockCapability<nsISerialEventTarget, mozilla::Mutex>
      mCDMAccessLock;
  Microsoft::WRL::ComPtr<IMFContentDecryptionModule> mCDM
      MOZ_GUARDED_BY(mCDMAccessLock);
  RefPtr<MFCDMProxy> mCDMProxy MOZ_GUARDED_BY(mCDMAccessLock);
};

// A helper class only used in the chrome process to handle CDM related tasks.
class MFCDMService {
 public:
  // This is used to display CDM capabilites in `about:support`.
  static void GetAllKeySystemsCapabilities(dom::Promise* aPromise);

  // If Widevine L1 is downloaded after the MFCDM process is created, then we
  // use this method to update the L1 path and setup L1 permission for the MFCDM
  // process.
  static void UpdateWidevineL1Path(nsIFile* aFile);

 private:
  static RefPtr<GenericNonExclusivePromise> LaunchMFCDMProcessIfNeeded(
      ipc::SandboxingKind aSandbox);
};

}  // namespace mozilla

#endif  // DOM_MEDIA_IPC_MFCDMPARENT_H_
