/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GPU_Queue_H_
#define GPU_Queue_H_

#include "nsWrapperCache.h"
#include "ObjectModel.h"
#include "mozilla/dom/BufferSourceBindingFwd.h"
#include "mozilla/dom/TypedArray.h"
#include "mozilla/webgpu/WebGPUTypes.h"

namespace mozilla {
class ErrorResult;
namespace dom {
class RangeEnforcedUnsignedLongSequenceOrGPUExtent3DDict;
template <typename T>
class Optional;
template <typename T>
class Sequence;
struct GPUCopyExternalImageDestInfo;
struct GPUCopyExternalImageSourceInfo;
struct GPUTexelCopyTextureInfo;
struct GPUTexelCopyBufferLayout;
struct TextureCopyView;
struct TextureDataLayout;
using GPUExtent3D = RangeEnforcedUnsignedLongSequenceOrGPUExtent3DDict;
}  // namespace dom
namespace webgpu {

class Buffer;
class CommandBuffer;
class Device;
class Fence;

class Queue final : public ObjectBase, public ChildOf<Device> {
 public:
  GPU_DECL_CYCLE_COLLECTION(Queue)
  GPU_DECL_JS_WRAP(Queue)

  Queue(Device* const aParent, WebGPUChild* aBridge, RawId aId);

  void Submit(
      const dom::Sequence<OwningNonNull<CommandBuffer>>& aCommandBuffers);

  already_AddRefed<dom::Promise> OnSubmittedWorkDone(ErrorResult& aRv);

  void WriteBuffer(const Buffer& aBuffer, uint64_t aBufferOffset,
                   const dom::AllowSharedBufferSource& data,
                   uint64_t aDataOffset, const dom::Optional<uint64_t>& aSize,
                   ErrorResult& aRv);

  void WriteTexture(const dom::GPUTexelCopyTextureInfo& aDestination,
                    const dom::AllowSharedBufferSource& data,
                    const dom::GPUTexelCopyBufferLayout& aDataLayout,
                    const dom::GPUExtent3D& aSize, ErrorResult& aRv);

  void CopyExternalImageToTexture(
      const dom::GPUCopyExternalImageSourceInfo& aSource,
      const dom::GPUCopyExternalImageDestInfo& aDestination,
      const dom::GPUExtent3D& aCopySize, ErrorResult& aRv);

  const RawId mId;

 private:
  virtual ~Queue();
  void Cleanup();

  RefPtr<WebGPUChild> mBridge;

 public:
};

}  // namespace webgpu
}  // namespace mozilla

#endif  // GPU_Queue_H_
