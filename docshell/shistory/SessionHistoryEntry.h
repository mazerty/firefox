/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SessionHistoryEntry_h
#define mozilla_dom_SessionHistoryEntry_h

#include "mozilla/dom/DocumentBinding.h"
#include "mozilla/Maybe.h"
#include "mozilla/UniquePtr.h"
#include "nsILayoutHistoryState.h"
#include "nsISHEntry.h"
#include "nsSHEntryShared.h"
#include "nsStructuredCloneContainer.h"
#include "nsTHashMap.h"
#include "nsWeakReference.h"

class nsDocShellLoadState;
class nsIChannel;
class nsIInputStream;
class nsIReferrerInfo;
class nsISHistory;
class nsIURI;

namespace mozilla::ipc {
template <typename P>
struct IPDLParamTraits;
}

namespace mozilla {
namespace dom {

struct LoadingSessionHistoryInfo;
class SessionHistoryEntry;
class SHEntrySharedParentState;

// SessionHistoryInfo stores session history data for a load. It can be sent
// over IPC and is used in both the parent and the child processes.
class SessionHistoryInfo {
 public:
  SessionHistoryInfo() = default;
  SessionHistoryInfo(const SessionHistoryInfo& aInfo) = default;
  SessionHistoryInfo(nsDocShellLoadState* aLoadState, nsIChannel* aChannel);
  SessionHistoryInfo(const SessionHistoryInfo& aSharedStateFrom, nsIURI* aURI);
  SessionHistoryInfo(nsIURI* aURI, nsIPrincipal* aTriggeringPrincipal,
                     nsIPrincipal* aPrincipalToInherit,
                     nsIPrincipal* aPartitionedPrincipalToInherit,
                     nsIPolicyContainer* aPolicyContainer,
                     const nsACString& aContentType);
  SessionHistoryInfo(nsIChannel* aChannel, uint32_t aLoadType,
                     nsIPrincipal* aPartitionedPrincipalToInherit,
                     nsIPolicyContainer* aPolicyContainer);

  void Reset(nsIURI* aURI, const nsID& aDocShellID, bool aDynamicCreation,
             nsIPrincipal* aTriggeringPrincipal,
             nsIPrincipal* aPrincipalToInherit,
             nsIPrincipal* aPartitionedPrincipalToInherit,
             nsIPolicyContainer* aPolicyContainer,
             const nsACString& aContentType);

  bool operator==(const SessionHistoryInfo& aInfo) const {
    return false;  // FIXME
  }

  nsIURI* GetURI() const { return mURI; }
  void SetURI(nsIURI* aURI) { mURI = aURI; }

  nsIURI* GetOriginalURI() const { return mOriginalURI; }
  void SetOriginalURI(nsIURI* aOriginalURI) { mOriginalURI = aOriginalURI; }

  nsIURI* GetUnstrippedURI() const { return mUnstrippedURI; }
  void SetUnstrippedURI(nsIURI* aUnstrippedURI) {
    mUnstrippedURI = aUnstrippedURI;
  }

  nsIURI* GetResultPrincipalURI() const { return mResultPrincipalURI; }
  void SetResultPrincipalURI(nsIURI* aResultPrincipalURI) {
    mResultPrincipalURI = aResultPrincipalURI;
  }

  nsCOMPtr<nsIReferrerInfo> GetReferrerInfo() { return mReferrerInfo; }
  void SetReferrerInfo(nsIReferrerInfo* aReferrerInfo) {
    mReferrerInfo = aReferrerInfo;
  }

  bool HasPostData() const { return mPostData; }
  already_AddRefed<nsIInputStream> GetPostData() const;
  void SetPostData(nsIInputStream* aPostData);

  void GetScrollPosition(int32_t* aScrollPositionX, int32_t* aScrollPositionY) {
    *aScrollPositionX = mScrollPositionX;
    *aScrollPositionY = mScrollPositionY;
  }

  void SetScrollPosition(int32_t aScrollPositionX, int32_t aScrollPositionY) {
    mScrollPositionX = aScrollPositionX;
    mScrollPositionY = aScrollPositionY;
  }

  bool GetScrollRestorationIsManual() const {
    return mScrollRestorationIsManual;
  }
  const nsAString& GetTitle() { return mTitle; }
  void SetTitle(const nsAString& aTitle) {
    mTitle = aTitle;
    MaybeUpdateTitleFromURI();
  }

  const nsAString& GetName() { return mName; }
  void SetName(const nsAString& aName) { mName = aName; }

  void SetScrollRestorationIsManual(bool aIsManual) {
    mScrollRestorationIsManual = aIsManual;
  }

  nsStructuredCloneContainer* GetStateData() const { return mStateData; }
  void SetStateData(nsStructuredCloneContainer* aStateData) {
    mStateData = aStateData;
  }

  void SetLoadReplace(bool aLoadReplace) { mLoadReplace = aLoadReplace; }

  void SetURIWasModified(bool aURIWasModified) {
    mURIWasModified = aURIWasModified;
  }
  bool GetURIWasModified() const { return mURIWasModified; }

  void SetHasUserInteraction(bool aHasUserInteraction) {
    mHasUserInteraction = aHasUserInteraction;
  }
  bool GetHasUserInteraction() const { return mHasUserInteraction; }

  uint64_t SharedId() const;

  nsILayoutHistoryState* GetLayoutHistoryState();
  void SetLayoutHistoryState(nsILayoutHistoryState* aState);

  nsIPrincipal* GetTriggeringPrincipal() const;

  nsIPrincipal* GetPrincipalToInherit() const;

  nsIPrincipal* GetPartitionedPrincipalToInherit() const;
  void SetPartitionedPrincipalToInherit(nsIPrincipal* aPrincipal);

  nsIPolicyContainer* GetPolicyContainer() const;

  uint32_t GetCacheKey() const;
  void SetCacheKey(uint32_t aCacheKey);

  bool IsSubFrame() const;

  bool SharesDocumentWith(const SessionHistoryInfo& aOther) const {
    return SharedId() == aOther.SharedId();
  }

  void FillLoadInfo(nsDocShellLoadState& aLoadState) const;

  uint32_t LoadType() { return mLoadType; }

  void SetSaveLayoutStateFlag(bool aSaveLayoutStateFlag);

  bool IsTransient() { return mTransient; }
  void SetTransient() { mTransient = true; }

  nsID& NavigationKey() { return mNavigationKey; }
  const nsID& NavigationKey() const { return mNavigationKey; }
  const nsID& NavigationId() const { return mNavigationId; }

  nsStructuredCloneContainer* GetNavigationState() const;

 private:
  friend class SessionHistoryEntry;
  friend struct mozilla::ipc::IPDLParamTraits<SessionHistoryInfo>;

  void MaybeUpdateTitleFromURI();

  nsCOMPtr<nsIURI> mURI;
  nsCOMPtr<nsIURI> mOriginalURI;
  nsCOMPtr<nsIURI> mResultPrincipalURI;
  nsCOMPtr<nsIURI> mUnstrippedURI;
  nsCOMPtr<nsIReferrerInfo> mReferrerInfo;
  nsString mTitle;
  nsString mName;
  nsCOMPtr<nsIInputStream> mPostData;
  uint32_t mLoadType = 0;
  int32_t mScrollPositionX = 0;
  int32_t mScrollPositionY = 0;
  RefPtr<nsStructuredCloneContainer> mStateData;
  Maybe<nsString> mSrcdocData;
  nsCOMPtr<nsIURI> mBaseURI;

  // Fields needed for NavigationHistoryEntry.
  nsID mNavigationKey = nsID::GenerateUUID();
  nsID mNavigationId = nsID::GenerateUUID();

  bool mLoadReplace = false;
  bool mURIWasModified = false;
  bool mScrollRestorationIsManual = false;
  bool mTransient = false;
  bool mHasUserInteraction = false;
  bool mHasUserActivation = false;

  union SharedState {
    SharedState();
    explicit SharedState(const SharedState& aOther);
    explicit SharedState(const Maybe<const SharedState&>& aOther);
    ~SharedState();

    SharedState& operator=(const SharedState& aOther);

    SHEntrySharedState* Get() const;

    void Set(SHEntrySharedParentState* aState) { mParent = aState; }

    void ChangeId(uint64_t aId);

    static SharedState Create(nsIPrincipal* aTriggeringPrincipal,
                              nsIPrincipal* aPrincipalToInherit,
                              nsIPrincipal* aPartitionedPrincipalToInherit,
                              nsIPolicyContainer* aPolicyContainer,
                              const nsACString& aContentType);

   private:
    explicit SharedState(SHEntrySharedParentState* aParent)
        : mParent(aParent) {}
    explicit SharedState(UniquePtr<SHEntrySharedState>&& aChild)
        : mChild(std::move(aChild)) {}

    void Init();
    void Init(const SharedState& aOther);

    // In the parent process this holds a strong reference to the refcounted
    // SHEntrySharedParentState. In the child processes this holds an owning
    // pointer to a SHEntrySharedState.
    RefPtr<SHEntrySharedParentState> mParent;
    UniquePtr<SHEntrySharedState> mChild;
  };

  SharedState mSharedState;
};

struct LoadingSessionHistoryInfo {
  LoadingSessionHistoryInfo() = default;
  explicit LoadingSessionHistoryInfo(SessionHistoryEntry* aEntry);
  // Initializes mInfo using aEntry and otherwise copies the values from aInfo.
  LoadingSessionHistoryInfo(SessionHistoryEntry* aEntry,
                            const LoadingSessionHistoryInfo* aInfo);
  // For about:blank only.
  explicit LoadingSessionHistoryInfo(const SessionHistoryInfo& aInfo);

  already_AddRefed<nsDocShellLoadState> CreateLoadInfo() const;

  SessionHistoryInfo mInfo;

  uint64_t mLoadId = 0;

  // The following three member variables are used to inform about a load from
  // the session history. The session-history-in-child approach has just
  // an nsISHEntry in the nsDocShellLoadState and access to the nsISHistory,
  // but session-history-in-parent needs to pass needed information explicitly
  // to the relevant child process.
  bool mLoadIsFromSessionHistory = false;
  // mOffset and mLoadingCurrentEntry are relevant only if
  // mLoadIsFromSessionHistory is true.
  int32_t mOffset = 0;
  // If we're loading from the current entry we want to treat it as not a
  // same-document navigation (see nsDocShell::IsSameDocumentNavigation).
  bool mLoadingCurrentEntry = false;
  // If mForceMaybeResetName.isSome() is true then the parent process has
  // determined whether the BC's name should be cleared and stored in session
  // history (see https://html.spec.whatwg.org/#history-traversal step 4.2).
  // This is used when we're replacing the BC for BFCache in the parent. In
  // other cases mForceMaybeResetName.isSome() will be false and the child
  // process should be able to make that determination itself.
  Maybe<bool> mForceMaybeResetName;
};

// HistoryEntryCounterForBrowsingContext is used to count the number of entries
// which are added to the session history for a particular browsing context.
// If a SessionHistoryEntry is cloned because of navigation in some other
// browsing context, that doesn't cause the counter value to be increased.
// The browsing context specific counter is needed to make it easier to
// synchronously update history.length value in a child process when
// an iframe is removed from DOM.
class HistoryEntryCounterForBrowsingContext {
 public:
  HistoryEntryCounterForBrowsingContext()
      : mCounter(new RefCountedCounter()), mHasModified(false) {
    ++(*this);
  }

  HistoryEntryCounterForBrowsingContext(
      const HistoryEntryCounterForBrowsingContext& aOther)
      : mCounter(aOther.mCounter), mHasModified(false) {}

  HistoryEntryCounterForBrowsingContext(
      HistoryEntryCounterForBrowsingContext&& aOther) = delete;

  ~HistoryEntryCounterForBrowsingContext() {
    if (mHasModified) {
      --(*mCounter);
    }
  }

  void CopyValueFrom(const HistoryEntryCounterForBrowsingContext& aOther) {
    if (mHasModified) {
      --(*mCounter);
    }
    mCounter = aOther.mCounter;
    mHasModified = false;
  }

  HistoryEntryCounterForBrowsingContext& operator=(
      const HistoryEntryCounterForBrowsingContext& aOther) = delete;

  HistoryEntryCounterForBrowsingContext& operator++() {
    mHasModified = true;
    ++(*mCounter);
    return *this;
  }

  operator uint32_t() const { return *mCounter; }

  bool Modified() { return mHasModified; }

  void SetModified(bool aModified) { mHasModified = aModified; }

  void Reset() {
    if (mHasModified) {
      --(*mCounter);
    }
    mCounter = new RefCountedCounter();
    mHasModified = false;
  }

 private:
  class RefCountedCounter {
   public:
    NS_INLINE_DECL_REFCOUNTING(
        mozilla::dom::HistoryEntryCounterForBrowsingContext::RefCountedCounter)

    RefCountedCounter& operator++() {
      ++mCounter;
      return *this;
    }

    RefCountedCounter& operator--() {
      --mCounter;
      return *this;
    }

    operator uint32_t() const { return mCounter; }

   private:
    ~RefCountedCounter() = default;

    uint32_t mCounter = 0;
  };

  RefPtr<RefCountedCounter> mCounter;
  bool mHasModified;
};

// SessionHistoryEntry is used to store session history data in the parent
// process. It holds a SessionHistoryInfo, some state shared amongst multiple
// SessionHistoryEntries, a parent and children.
#define NS_SESSIONHISTORYENTRY_IID \
  {0x5b66a244, 0x8cec, 0x4caa, {0xaa, 0x0a, 0x78, 0x92, 0xfd, 0x17, 0xa6, 0x67}}

class SessionHistoryEntry : public nsISHEntry,
                            public nsSupportsWeakReference,
                            public LinkedListElement<SessionHistoryEntry> {
 public:
  SessionHistoryEntry(nsDocShellLoadState* aLoadState, nsIChannel* aChannel);
  SessionHistoryEntry();
  explicit SessionHistoryEntry(SessionHistoryInfo* aInfo);
  explicit SessionHistoryEntry(const SessionHistoryEntry& aEntry);

  NS_DECL_ISUPPORTS
  NS_DECL_NSISHENTRY
  NS_INLINE_DECL_STATIC_IID(NS_SESSIONHISTORYENTRY_IID)

  bool IsInSessionHistory() {
    SessionHistoryEntry* entry = this;
    while (nsCOMPtr<SessionHistoryEntry> parent =
               do_QueryReferent(entry->mParent)) {
      entry = parent;
    }
    return entry->SharedInfo()->mSHistory &&
           entry->SharedInfo()->mSHistory->IsAlive();
  }

  void ReplaceWith(const SessionHistoryEntry& aSource);

  const SessionHistoryInfo& Info() const { return *mInfo; }

  SHEntrySharedParentState* SharedInfo() const;

  void SetFrameLoader(nsFrameLoader* aFrameLoader);
  nsFrameLoader* GetFrameLoader();

  void AddChild(SessionHistoryEntry* aChild, int32_t aOffset,
                bool aUseRemoteSubframes);
  void RemoveChild(SessionHistoryEntry* aChild);
  // Finds the child with the same docshell ID as aNewChild, replaces it with
  // aNewChild and returns true. If there is no child with the same docshell ID
  // then it returns false.
  bool ReplaceChild(SessionHistoryEntry* aNewChild);

  void SetInfo(SessionHistoryInfo* aInfo);

  bool ForInitialLoad() { return mForInitialLoad; }
  void SetForInitialLoad(bool aForInitialLoad) {
    mForInitialLoad = aForInitialLoad;
  }

  const nsID& DocshellID() const;

  HistoryEntryCounterForBrowsingContext& BCHistoryLength() {
    return mBCHistoryLength;
  }

  void SetBCHistoryLength(HistoryEntryCounterForBrowsingContext& aCounter) {
    mBCHistoryLength.CopyValueFrom(aCounter);
  }

  void ClearBCHistoryLength() { mBCHistoryLength.Reset(); }

  void SetIsDynamicallyAdded(bool aDynamic);

  void SetWireframe(const Maybe<Wireframe>& aWireframe);

  struct LoadingEntry {
    // A pointer to the entry being loaded. Will be cleared by the
    // SessionHistoryEntry destructor, at latest.
    SessionHistoryEntry* mEntry;
    // Snapshot of the entry's SessionHistoryInfo when the load started, to be
    // used for validation purposes only.
    UniquePtr<SessionHistoryInfo> mInfoSnapshotForValidation;
  };

  // Get an entry based on LoadingSessionHistoryInfo's mLoadId. Parent process
  // only.
  static LoadingEntry* GetByLoadId(uint64_t aLoadId);
  static void SetByLoadId(uint64_t aLoadId, SessionHistoryEntry* aEntry);
  static void RemoveLoadId(uint64_t aLoadId);

  const nsTArray<RefPtr<SessionHistoryEntry>>& Children() { return mChildren; }

 private:
  friend struct LoadingSessionHistoryInfo;
  virtual ~SessionHistoryEntry();

  UniquePtr<SessionHistoryInfo> mInfo;
  nsWeakPtr mParent;
  uint32_t mID;
  nsTArray<RefPtr<SessionHistoryEntry>> mChildren;
  Maybe<Wireframe> mWireframe;

  bool mForInitialLoad = false;

  HistoryEntryCounterForBrowsingContext mBCHistoryLength;

  static nsTHashMap<nsUint64HashKey, LoadingEntry>* sLoadIdToEntry;
};

}  // namespace dom

namespace ipc {

class IProtocol;

// Allow sending SessionHistoryInfo objects over IPC.
template <>
struct IPDLParamTraits<dom::SessionHistoryInfo> {
  static void Write(IPC::MessageWriter* aWriter, IProtocol* aActor,
                    const dom::SessionHistoryInfo& aParam);
  static bool Read(IPC::MessageReader* aReader, IProtocol* aActor,
                   dom::SessionHistoryInfo* aResult);
};

// Allow sending LoadingSessionHistoryInfo objects over IPC.
template <>
struct IPDLParamTraits<dom::LoadingSessionHistoryInfo> {
  static void Write(IPC::MessageWriter* aWriter, IProtocol* aActor,
                    const dom::LoadingSessionHistoryInfo& aParam);
  static bool Read(IPC::MessageReader* aReader, IProtocol* aActor,
                   dom::LoadingSessionHistoryInfo* aResult);
};

// Allow sending nsILayoutHistoryState objects over IPC.
template <>
struct IPDLParamTraits<nsILayoutHistoryState*> {
  static void Write(IPC::MessageWriter* aWriter, IProtocol* aActor,
                    nsILayoutHistoryState* aParam);
  static bool Read(IPC::MessageReader* aReader, IProtocol* aActor,
                   RefPtr<nsILayoutHistoryState>* aResult);
};

// Allow sending dom::Wireframe objects over IPC.
template <>
struct IPDLParamTraits<mozilla::dom::Wireframe> {
  static void Write(IPC::MessageWriter* aWriter, IProtocol* aActor,
                    const mozilla::dom::Wireframe& aParam);
  static bool Read(IPC::MessageReader* aReader, IProtocol* aActor,
                   mozilla::dom::Wireframe* aResult);
};

}  // namespace ipc

}  // namespace mozilla

inline nsISupports* ToSupports(mozilla::dom::SessionHistoryEntry* aEntry) {
  return static_cast<nsISHEntry*>(aEntry);
}

#endif /* mozilla_dom_SessionHistoryEntry_h */
