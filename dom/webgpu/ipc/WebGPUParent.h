/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WEBGPU_PARENT_H_
#define WEBGPU_PARENT_H_

#include <unordered_map>

#include "mozilla/WeakPtr.h"
#include "mozilla/ipc/SharedMemoryHandle.h"
#include "mozilla/webgpu/ffi/wgpu.h"
#include "mozilla/webgpu/PWebGPUParent.h"
#include "mozilla/webrender/WebRenderAPI.h"
#include "WebGPUTypes.h"
#include "base/timer.h"

namespace mozilla {

namespace layers {
class RemoteTextureOwnerClient;
}  // namespace layers

namespace webgpu {

class ErrorBuffer;
class SharedTexture;
class PresentationData;

// Destroy/Drop messages:
// - Messages with "Destroy" in their name request deallocation of resources
// owned by the
//   object and put the object in a destroyed state without deleting the object.
//   It is still safe to reffer to these objects.
// - Messages with "Drop" in their name can be thought of as C++ destructors.
// They completely
//   delete the object, so future attempts at accessing to these objects will
//   crash. The child process should *never* send a Drop message if it still
//   holds references to the object. An object that has been destroyed still
//   needs to be dropped when the last reference to it dies on the child
//   process.

class WebGPUParent final : public PWebGPUParent, public SupportsWeakPtr {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebGPUParent, override)

 public:
  explicit WebGPUParent();

  void PostAdapterRequestDevice(RawId aDeviceId);
  void BufferUnmap(RawId aDeviceId, RawId aBufferId, bool aFlush);
  ipc::IPCResult RecvMessages(uint32_t nrOfMessages,
                              ipc::ByteBuf&& aSerializedMessages,
                              nsTArray<ipc::ByteBuf>&& aDataBuffers,
                              nsTArray<MutableSharedMemoryHandle>&& aShmems);
  void QueueSubmit(RawId aQueueId, RawId aDeviceId,
                   Span<const RawId> aCommandBuffers,
                   Span<const RawId> aTextureIds);
  void DeviceCreateSwapChain(RawId aDeviceId, RawId aQueueId,
                             const layers::RGBDescriptor& aDesc,
                             const nsTArray<RawId>& aBufferIds,
                             const layers::RemoteTextureOwnerId& aOwnerId,
                             bool aUseSharedTextureInSwapChain);

  void SwapChainPresent(RawId aTextureId, RawId aCommandEncoderId,
                        const layers::RemoteTextureId& aRemoteTextureId,
                        const layers::RemoteTextureOwnerId& aOwnerId);
  void SwapChainDrop(const layers::RemoteTextureOwnerId& aOwnerId,
                     layers::RemoteTextureTxnType aTxnType,
                     layers::RemoteTextureTxnId aTxnId);

  void DevicePushErrorScope(RawId aDeviceId, dom::GPUErrorFilter);
  PopErrorScopeResult DevicePopErrorScope(RawId aDeviceId);

  ipc::IPCResult GetFrontBufferSnapshot(
      IProtocol* aProtocol, const layers::RemoteTextureOwnerId& aOwnerId,
      const RawId& aCommandEncoderId, Maybe<Shmem>& aShmem, gfx::IntSize& aSize,
      uint32_t& aByteStride);

  void ActorDestroy(ActorDestroyReason aWhy) override;

  struct BufferMapData {
    ipc::SharedMemoryMapping mShmem;
    // True if buffer's usage has MAP_READ or MAP_WRITE set.
    bool mHasMapFlags;
    uint64_t mMappedOffset;
    uint64_t mMappedSize;
    RawId mDeviceId;
  };

  BufferMapData* GetBufferMapData(RawId aBufferId);

  bool UseSharedTextureForSwapChain(ffi::WGPUSwapChainId aSwapChainId);

  void DisableSharedTextureForSwapChain(ffi::WGPUSwapChainId aSwapChainId);

  bool EnsureSharedTextureForSwapChain(ffi::WGPUSwapChainId aSwapChainId,
                                       ffi::WGPUDeviceId aDeviceId,
                                       ffi::WGPUTextureId aTextureId,
                                       uint32_t aWidth, uint32_t aHeight,
                                       struct ffi::WGPUTextureFormat aFormat,
                                       ffi::WGPUTextureUsages aUsage);

  void EnsureSharedTextureForReadBackPresent(
      ffi::WGPUSwapChainId aSwapChainId, ffi::WGPUDeviceId aDeviceId,
      ffi::WGPUTextureId aTextureId, uint32_t aWidth, uint32_t aHeight,
      struct ffi::WGPUTextureFormat aFormat, ffi::WGPUTextureUsages aUsage);

  std::shared_ptr<SharedTexture> CreateSharedTexture(
      const layers::RemoteTextureOwnerId& aOwnerId, ffi::WGPUDeviceId aDeviceId,
      ffi::WGPUTextureId aTextureId, uint32_t aWidth, uint32_t aHeight,
      const struct ffi::WGPUTextureFormat aFormat,
      ffi::WGPUTextureUsages aUsage);

  std::shared_ptr<SharedTexture> GetSharedTexture(ffi::WGPUTextureId aId);

  void PostSharedTexture(const std::shared_ptr<SharedTexture>&& aSharedTexture,
                         const layers::RemoteTextureId aRemoteTextureId,
                         const layers::RemoteTextureOwnerId aOwnerId);

  bool ForwardError(ErrorBuffer& aError);

  ffi::WGPUGlobal* GetContext() const { return mContext.get(); }

  bool IsDeviceActive(const RawId aDeviceId) {
    return mActiveDeviceIds.Contains(aDeviceId);
  }

  RefPtr<gfx::FileHandleWrapper> GetDeviceFenceHandle(const RawId aDeviceId);

  void RemoveSharedTexture(RawId aTextureId);
  void DeallocBufferShmem(RawId aBufferId);
  void PreDeviceDrop(RawId aDeviceId);

#if defined(XP_WIN)
  static Maybe<ffi::WGPUFfiLUID> GetCompositorDeviceLuid();
#endif

  struct MapRequest {
    WeakPtr<WebGPUParent> mParent;
    ffi::WGPUDeviceId mDeviceId;
    ffi::WGPUBufferId mBufferId;
    ffi::WGPUHostMap mHostMap;
    uint64_t mOffset;
    uint64_t mSize;
  };

  static void MapCallback(/* std::unique_ptr<MapRequest> */ uint8_t* aUserData,
                          ffi::WGPUBufferMapAsyncStatus aStatus);

  struct OnSubmittedWorkDoneRequest {
    WeakPtr<WebGPUParent> mParent;
    ffi::WGPUDeviceId mQueueId;
  };

  static void OnSubmittedWorkDoneCallback(
      /* std::unique_ptr<OnSubmittedWorkDoneRequest> */ uint8_t* userdata);

  void ReportError(RawId aDeviceId, GPUErrorFilter, const nsCString& message);

  nsTArray<Maybe<ipc::shared_memory::MutableMapping>> mTempMappings;

  /// A map from wgpu buffer ids to data about their shared memory segments.
  /// Includes entries about mappedAtCreation, MAP_READ and MAP_WRITE buffers,
  /// regardless of their state.
  std::unordered_map<RawId, BufferMapData> mSharedMemoryMap;

 private:
  static void DeviceLostCallback(uint8_t* aUserData, uint8_t aReason,
                                 const char* aMessage);

  virtual ~WebGPUParent();
  void MaintainDevices();
  void LoseDevice(const RawId aDeviceId, uint8_t aReason,
                  const nsACString& aMessage);

  UniquePtr<ffi::WGPUGlobal> mContext;
  base::RepeatingTimer<WebGPUParent> mTimer;

  /// Associated presentation data for each swapchain.
  std::unordered_map<layers::RemoteTextureOwnerId, RefPtr<PresentationData>,
                     layers::RemoteTextureOwnerId::HashFn>
      mPresentationDataMap;

  RefPtr<layers::RemoteTextureOwnerClient> mRemoteTextureOwner;

  /// Associated stack of error scopes for each device.
  std::unordered_map<uint64_t, std::vector<ErrorScope>>
      mErrorScopeStackByDevice;

  std::unordered_map<ffi::WGPUTextureId, std::shared_ptr<SharedTexture>>
      mSharedTextures;

  // Store a set of DeviceIds that have been SendDeviceLost. We use this to
  // limit each Device to one DeviceLost message.
  nsTHashSet<RawId> mLostDeviceIds;

  // Store active DeviceIds
  nsTHashSet<RawId> mActiveDeviceIds;

  // Shared handle of wgpu device's fence.
  std::unordered_map<RawId, RefPtr<gfx::FileHandleWrapper>> mDeviceFenceHandles;
};

#if defined(XP_LINUX) && !defined(MOZ_WIDGET_ANDROID)
class VkImageHandle {
 public:
  explicit VkImageHandle(WebGPUParent* aParent,
                         const ffi::WGPUDeviceId aDeviceId,
                         ffi::WGPUVkImageHandle* aVkImageHandle)
      : mParent(aParent),
        mDeviceId(aDeviceId),
        mVkImageHandle(aVkImageHandle) {}

  const ffi::WGPUVkImageHandle* Get() { return mVkImageHandle; }

  ~VkImageHandle();

 protected:
  const WeakPtr<WebGPUParent> mParent;
  const RawId mDeviceId;
  ffi::WGPUVkImageHandle* mVkImageHandle;
};

class VkSemaphoreHandle {
 public:
  explicit VkSemaphoreHandle(WebGPUParent* aParent,
                             const ffi::WGPUDeviceId aDeviceId,
                             ffi::WGPUVkSemaphoreHandle* aVkSemaphoreHandle)
      : mParent(aParent),
        mDeviceId(aDeviceId),
        mVkSemaphoreHandle(aVkSemaphoreHandle) {}

  const ffi::WGPUVkSemaphoreHandle* Get() { return mVkSemaphoreHandle; }

  ~VkSemaphoreHandle();

 protected:
  const WeakPtr<WebGPUParent> mParent;
  const RawId mDeviceId;
  ffi::WGPUVkSemaphoreHandle* mVkSemaphoreHandle;
};
#endif

}  // namespace webgpu
}  // namespace mozilla

#endif  // WEBGPU_PARENT_H_
