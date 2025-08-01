/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/ArrayBuffer.h"
#include "js/Value.h"
#include "mozilla/Attributes.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Logging.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/Console.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/WebGPUBinding.h"
#include "Device.h"
#include "CommandEncoder.h"
#include "BindGroup.h"

#include "Adapter.h"
#include "Buffer.h"
#include "CompilationInfo.h"
#include "ComputePipeline.h"
#include "DeviceLostInfo.h"
#include "InternalError.h"
#include "OutOfMemoryError.h"
#include "PipelineLayout.h"
#include "QuerySet.h"
#include "Queue.h"
#include "RenderBundleEncoder.h"
#include "RenderPipeline.h"
#include "Sampler.h"
#include "SupportedFeatures.h"
#include "SupportedLimits.h"
#include "Texture.h"
#include "TextureView.h"
#include "ValidationError.h"
#include "ipc/WebGPUChild.h"
#include "Utility.h"
#include "nsGlobalWindowInner.h"

namespace mozilla::webgpu {

mozilla::LazyLogModule gWebGPULog("WebGPU");

GPU_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_INHERITED(Device, DOMEventTargetHelper,
                                                 mBridge, mQueue, mFeatures,
                                                 mLimits, mAdapterInfo,
                                                 mLostPromise);
NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(Device, DOMEventTargetHelper)
GPU_IMPL_JS_WRAP(Device)

/* static */ CheckedInt<uint32_t> Device::BufferStrideWithMask(
    const gfx::IntSize& aSize, const gfx::SurfaceFormat& aFormat) {
  constexpr uint32_t kBufferAlignmentMask = 0xff;
  return CheckedInt<uint32_t>(aSize.width) * gfx::BytesPerPixel(aFormat) +
         kBufferAlignmentMask;
}

RefPtr<WebGPUChild> Device::GetBridge() { return mBridge; }

Device::Device(Adapter* const aParent, RawId aDeviceId, RawId aQueueId,
               RefPtr<SupportedFeatures> aFeatures,
               RefPtr<SupportedLimits> aLimits,
               RefPtr<webgpu::AdapterInfo> aAdapterInfo,
               RefPtr<dom::Promise> aLostPromise)
    : DOMEventTargetHelper(aParent->GetParentObject()),
      mId(aDeviceId),
      mFeatures(std::move(aFeatures)),
      mLimits(std::move(aLimits)),
      mAdapterInfo(std::move(aAdapterInfo)),
      mSupportSharedTextureInSwapChain(
          aParent->SupportSharedTextureInSwapChain()),
      mBridge(aParent->mBridge),
      mLostPromise(std::move(aLostPromise)),
      mQueue(new class Queue(this, aParent->mBridge, aQueueId)) {
  mBridge->RegisterDevice(this);
}

Device::~Device() { Cleanup(); }

void Device::Cleanup() {
  if (!mValid) {
    return;
  }

  mValid = false;

  if (mBridge) {
    mBridge->UnregisterDevice(mId);
  }
}

bool Device::IsLost() const {
  return !mBridge || !mBridge->CanSend() ||
         (mLostPromise &&
          (mLostPromise->State() != dom::Promise::PromiseState::Pending));
}

void Device::TrackBuffer(Buffer* aBuffer) { mTrackedBuffers.Insert(aBuffer); }

void Device::UntrackBuffer(Buffer* aBuffer) { mTrackedBuffers.Remove(aBuffer); }

void Device::GetLabel(nsAString& aValue) const { aValue = mLabel; }
void Device::SetLabel(const nsAString& aLabel) { mLabel = aLabel; }

dom::Promise* Device::GetLost(ErrorResult& aRv) {
  aRv = NS_OK;
  return mLostPromise;
}

void Device::ResolveLost(dom::GPUDeviceLostReason aReason,
                         const nsAString& aMessage) {
  if (mLostPromise->State() != dom::Promise::PromiseState::Pending) {
    // The lost promise was already resolved or rejected.
    return;
  }
  RefPtr<DeviceLostInfo> info =
      MakeRefPtr<DeviceLostInfo>(GetParentObject(), aReason, aMessage);
  mLostPromise->MaybeResolve(info);
}

already_AddRefed<Buffer> Device::CreateBuffer(
    const dom::GPUBufferDescriptor& aDesc, ErrorResult& aRv) {
  return Buffer::Create(this, mId, aDesc, aRv);
}

already_AddRefed<Texture> Device::CreateTextureForSwapChain(
    const dom::GPUCanvasConfiguration* const aConfig,
    const gfx::IntSize& aCanvasSize, layers::RemoteTextureOwnerId aOwnerId) {
  MOZ_ASSERT(aConfig);

  dom::GPUTextureDescriptor desc;
  desc.mDimension = dom::GPUTextureDimension::_2d;
  auto& sizeDict = desc.mSize.SetAsGPUExtent3DDict();
  sizeDict.mWidth = aCanvasSize.width;
  sizeDict.mHeight = aCanvasSize.height;
  sizeDict.mDepthOrArrayLayers = 1;
  desc.mFormat = aConfig->mFormat;
  desc.mMipLevelCount = 1;
  desc.mSampleCount = 1;
  desc.mUsage = aConfig->mUsage | dom::GPUTextureUsage_Binding::COPY_SRC;
  desc.mViewFormats = aConfig->mViewFormats;

  return CreateTexture(desc, Some(aOwnerId));
}

already_AddRefed<Texture> Device::CreateTexture(
    const dom::GPUTextureDescriptor& aDesc) {
  return CreateTexture(aDesc, /* aOwnerId */ Nothing());
}

already_AddRefed<Texture> Device::CreateTexture(
    const dom::GPUTextureDescriptor& aDesc,
    Maybe<layers::RemoteTextureOwnerId> aOwnerId) {
  ffi::WGPUTextureDescriptor desc = {};

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();

  if (aDesc.mSize.IsRangeEnforcedUnsignedLongSequence()) {
    const auto& seq = aDesc.mSize.GetAsRangeEnforcedUnsignedLongSequence();
    desc.size.width = seq.Length() > 0 ? seq[0] : 1;
    desc.size.height = seq.Length() > 1 ? seq[1] : 1;
    desc.size.depth_or_array_layers = seq.Length() > 2 ? seq[2] : 1;
  } else if (aDesc.mSize.IsGPUExtent3DDict()) {
    const auto& dict = aDesc.mSize.GetAsGPUExtent3DDict();
    desc.size.width = dict.mWidth;
    desc.size.height = dict.mHeight;
    desc.size.depth_or_array_layers = dict.mDepthOrArrayLayers;
  } else {
    MOZ_CRASH("Unexpected union");
  }
  desc.mip_level_count = aDesc.mMipLevelCount;
  desc.sample_count = aDesc.mSampleCount;
  desc.dimension = ffi::WGPUTextureDimension(aDesc.mDimension);
  desc.format = ConvertTextureFormat(aDesc.mFormat);
  desc.usage = aDesc.mUsage;

  AutoTArray<ffi::WGPUTextureFormat, 8> viewFormats;
  for (auto format : aDesc.mViewFormats) {
    viewFormats.AppendElement(ConvertTextureFormat(format));
  }
  desc.view_formats = {viewFormats.Elements(), viewFormats.Length()};

  Maybe<ffi::WGPUSwapChainId> ownerId;
  if (aOwnerId.isSome()) {
    ownerId = Some(ffi::WGPUSwapChainId{aOwnerId->mId});
  }

  RawId id = ffi::wgpu_client_create_texture(mBridge->GetClient(), mId, &desc,
                                             ownerId.ptrOr(nullptr));

  RefPtr<Texture> texture = new Texture(this, id, aDesc);
  texture->SetLabel(aDesc.mLabel);
  return texture.forget();
}

already_AddRefed<Sampler> Device::CreateSampler(
    const dom::GPUSamplerDescriptor& aDesc) {
  ffi::WGPUSamplerDescriptor desc = {};
  webgpu::StringHelper label(aDesc.mLabel);

  desc.label = label.Get();
  desc.address_modes[0] = ffi::WGPUAddressMode(aDesc.mAddressModeU);
  desc.address_modes[1] = ffi::WGPUAddressMode(aDesc.mAddressModeV);
  desc.address_modes[2] = ffi::WGPUAddressMode(aDesc.mAddressModeW);
  desc.mag_filter = ffi::WGPUFilterMode(aDesc.mMagFilter);
  desc.min_filter = ffi::WGPUFilterMode(aDesc.mMinFilter);
  desc.mipmap_filter = ffi::WGPUFilterMode(aDesc.mMipmapFilter);
  desc.lod_min_clamp = aDesc.mLodMinClamp;
  desc.lod_max_clamp = aDesc.mLodMaxClamp;
  desc.max_anisotropy = aDesc.mMaxAnisotropy;

  ffi::WGPUCompareFunction comparison = ffi::WGPUCompareFunction_Sentinel;
  if (aDesc.mCompare.WasPassed()) {
    comparison = ConvertCompareFunction(aDesc.mCompare.Value());
    desc.compare = &comparison;
  }

  RawId id = ffi::wgpu_client_create_sampler(mBridge->GetClient(), mId, &desc);

  RefPtr<Sampler> sampler = new Sampler(this, id);
  sampler->SetLabel(aDesc.mLabel);
  return sampler.forget();
}

already_AddRefed<CommandEncoder> Device::CreateCommandEncoder(
    const dom::GPUCommandEncoderDescriptor& aDesc) {
  ffi::WGPUCommandEncoderDescriptor desc = {};

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();

  RawId id =
      ffi::wgpu_client_create_command_encoder(mBridge->GetClient(), mId, &desc);

  RefPtr<CommandEncoder> encoder = new CommandEncoder(this, mBridge, id);
  encoder->SetLabel(aDesc.mLabel);
  return encoder.forget();
}

already_AddRefed<RenderBundleEncoder> Device::CreateRenderBundleEncoder(
    const dom::GPURenderBundleEncoderDescriptor& aDesc) {
  RefPtr<RenderBundleEncoder> encoder =
      new RenderBundleEncoder(this, mBridge, aDesc);
  encoder->SetLabel(aDesc.mLabel);
  return encoder.forget();
}

already_AddRefed<QuerySet> Device::CreateQuerySet(
    const dom::GPUQuerySetDescriptor& aDesc, ErrorResult& aRv) {
  ffi::WGPURawQuerySetDescriptor desc = {};

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();
  ffi::WGPURawQueryType type;
  switch (aDesc.mType) {
    case dom::GPUQueryType::Occlusion:
      type = ffi::WGPURawQueryType_Occlusion;
      break;
    case dom::GPUQueryType::Timestamp:
      type = ffi::WGPURawQueryType_Timestamp;
      if (!mFeatures->Features().count(dom::GPUFeatureName::Timestamp_query)) {
        aRv.ThrowTypeError(
            "requested query set of type `timestamp`, but the "
            "`timestamp-query` feature is not enabled on the device");
        return nullptr;
      }
      break;
  };
  desc.ty = type;
  desc.count = aDesc.mCount;

  RawId id =
      ffi::wgpu_client_create_query_set(mBridge->GetClient(), mId, &desc);

  RefPtr<QuerySet> querySet = new QuerySet(this, aDesc, id);
  querySet->SetLabel(aDesc.mLabel);
  return querySet.forget();
}

already_AddRefed<BindGroupLayout> Device::CreateBindGroupLayout(
    const dom::GPUBindGroupLayoutDescriptor& aDesc) {
  struct OptionalData {
    ffi::WGPUTextureViewDimension dim;
    ffi::WGPURawTextureSampleType type;
    ffi::WGPUTextureFormat format;
  };
  nsTArray<OptionalData> optional(aDesc.mEntries.Length());
  for (const auto& entry : aDesc.mEntries) {
    OptionalData data = {};
    if (entry.mTexture.WasPassed()) {
      const auto& texture = entry.mTexture.Value();
      data.dim = ffi::WGPUTextureViewDimension(texture.mViewDimension);
      switch (texture.mSampleType) {
        case dom::GPUTextureSampleType::Float:
          data.type = ffi::WGPURawTextureSampleType_Float;
          break;
        case dom::GPUTextureSampleType::Unfilterable_float:
          data.type = ffi::WGPURawTextureSampleType_UnfilterableFloat;
          break;
        case dom::GPUTextureSampleType::Uint:
          data.type = ffi::WGPURawTextureSampleType_Uint;
          break;
        case dom::GPUTextureSampleType::Sint:
          data.type = ffi::WGPURawTextureSampleType_Sint;
          break;
        case dom::GPUTextureSampleType::Depth:
          data.type = ffi::WGPURawTextureSampleType_Depth;
          break;
      }
    }
    if (entry.mStorageTexture.WasPassed()) {
      const auto& texture = entry.mStorageTexture.Value();
      data.dim = ffi::WGPUTextureViewDimension(texture.mViewDimension);
      data.format = ConvertTextureFormat(texture.mFormat);
    }
    optional.AppendElement(data);
  }

  nsTArray<ffi::WGPUBindGroupLayoutEntry> entries(aDesc.mEntries.Length());
  for (size_t i = 0; i < aDesc.mEntries.Length(); ++i) {
    const auto& entry = aDesc.mEntries[i];
    ffi::WGPUBindGroupLayoutEntry e = {};
    e.binding = entry.mBinding;
    e.visibility = entry.mVisibility;
    if (entry.mBuffer.WasPassed()) {
      switch (entry.mBuffer.Value().mType) {
        case dom::GPUBufferBindingType::Uniform:
          e.ty = ffi::WGPURawBindingType_UniformBuffer;
          break;
        case dom::GPUBufferBindingType::Storage:
          e.ty = ffi::WGPURawBindingType_StorageBuffer;
          break;
        case dom::GPUBufferBindingType::Read_only_storage:
          e.ty = ffi::WGPURawBindingType_ReadonlyStorageBuffer;
          break;
      }
      e.has_dynamic_offset = entry.mBuffer.Value().mHasDynamicOffset;
      e.min_binding_size = entry.mBuffer.Value().mMinBindingSize;
    }
    if (entry.mTexture.WasPassed()) {
      e.ty = ffi::WGPURawBindingType_SampledTexture;
      e.view_dimension = &optional[i].dim;
      e.texture_sample_type = &optional[i].type;
      e.multisampled = entry.mTexture.Value().mMultisampled;
    }
    if (entry.mStorageTexture.WasPassed()) {
      switch (entry.mStorageTexture.Value().mAccess) {
        case dom::GPUStorageTextureAccess::Write_only: {
          e.ty = ffi::WGPURawBindingType_WriteonlyStorageTexture;
          break;
        }
        case dom::GPUStorageTextureAccess::Read_only: {
          e.ty = ffi::WGPURawBindingType_ReadonlyStorageTexture;
          break;
        }
        case dom::GPUStorageTextureAccess::Read_write: {
          e.ty = ffi::WGPURawBindingType_ReadWriteStorageTexture;
          break;
        }
        default: {
          MOZ_ASSERT_UNREACHABLE();
        }
      }
      e.view_dimension = &optional[i].dim;
      e.storage_texture_format = &optional[i].format;
    }
    if (entry.mSampler.WasPassed()) {
      e.ty = ffi::WGPURawBindingType_Sampler;
      switch (entry.mSampler.Value().mType) {
        case dom::GPUSamplerBindingType::Filtering:
          e.sampler_filter = true;
          break;
        case dom::GPUSamplerBindingType::Non_filtering:
          break;
        case dom::GPUSamplerBindingType::Comparison:
          e.sampler_compare = true;
          break;
      }
    }
    if (entry.mExternalTexture.WasPassed()) {
      e.ty = ffi::WGPURawBindingType_ExternalTexture;
    }
    entries.AppendElement(e);
  }

  ffi::WGPUBindGroupLayoutDescriptor desc = {};

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();
  desc.entries = {entries.Elements(), entries.Length()};

  RawId id = ffi::wgpu_client_create_bind_group_layout(mBridge->GetClient(),
                                                       mId, &desc);

  RefPtr<BindGroupLayout> object = new BindGroupLayout(this, id);
  object->SetLabel(aDesc.mLabel);
  return object.forget();
}

already_AddRefed<PipelineLayout> Device::CreatePipelineLayout(
    const dom::GPUPipelineLayoutDescriptor& aDesc) {
  nsTArray<ffi::WGPUBindGroupLayoutId> bindGroupLayouts(
      aDesc.mBindGroupLayouts.Length());

  for (const auto& layout : aDesc.mBindGroupLayouts) {
    bindGroupLayouts.AppendElement(layout->mId);
  }

  ffi::WGPUPipelineLayoutDescriptor desc = {};

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();
  desc.bind_group_layouts = {bindGroupLayouts.Elements(),
                             bindGroupLayouts.Length()};

  RawId id =
      ffi::wgpu_client_create_pipeline_layout(mBridge->GetClient(), mId, &desc);

  RefPtr<PipelineLayout> object = new PipelineLayout(this, id);
  object->SetLabel(aDesc.mLabel);
  return object.forget();
}

already_AddRefed<BindGroup> Device::CreateBindGroup(
    const dom::GPUBindGroupDescriptor& aDesc) {
  nsTArray<ffi::WGPUBindGroupEntry> entries(aDesc.mEntries.Length());
  CanvasContextArray canvasContexts;
  for (const auto& entry : aDesc.mEntries) {
    ffi::WGPUBindGroupEntry e = {};
    e.binding = entry.mBinding;
    auto setTextureViewBinding =
        [&e, &canvasContexts](const TextureView& texture_view) {
          e.texture_view = texture_view.mId;
          auto context = texture_view.GetTargetContext();
          if (context) {
            canvasContexts.AppendElement(context);
          }
        };
    if (entry.mResource.IsGPUBuffer()) {
      const auto& buffer = entry.mResource.GetAsGPUBuffer();
      if (!buffer->mId) {
        NS_WARNING("Buffer has no id -- ignoring.");
        continue;
      }
      e.buffer = buffer->mId;
      e.offset = 0;
      e.size = 0;
    } else if (entry.mResource.IsGPUBufferBinding()) {
      const auto& bufBinding = entry.mResource.GetAsGPUBufferBinding();
      if (!bufBinding.mBuffer->mId) {
        NS_WARNING("Buffer binding has no id -- ignoring.");
        continue;
      }
      e.buffer = bufBinding.mBuffer->mId;
      e.offset = bufBinding.mOffset;
      e.size = bufBinding.mSize.WasPassed() ? bufBinding.mSize.Value() : 0;
    } else if (entry.mResource.IsGPUTexture()) {
      auto texture = entry.mResource.GetAsGPUTexture();
      const dom::GPUTextureViewDescriptor defaultDesc{};
      RefPtr<TextureView> texture_view = texture->CreateView(defaultDesc);
      setTextureViewBinding(*texture_view);
    } else if (entry.mResource.IsGPUTextureView()) {
      auto texture_view = entry.mResource.GetAsGPUTextureView();
      setTextureViewBinding(texture_view);
    } else if (entry.mResource.IsGPUSampler()) {
      e.sampler = entry.mResource.GetAsGPUSampler()->mId;
    } else {
      // Not a buffer, nor a texture view, nor a sampler. If we pass
      // this to wgpu_client, it'll panic. Log a warning instead and
      // ignore this entry.
      NS_WARNING("Bind group entry has unknown type.");
      continue;
    }
    entries.AppendElement(e);
  }

  ffi::WGPUBindGroupDescriptor desc = {};

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();
  desc.layout = aDesc.mLayout->mId;
  desc.entries = {entries.Elements(), entries.Length()};

  RawId id =
      ffi::wgpu_client_create_bind_group(mBridge->GetClient(), mId, &desc);

  RefPtr<BindGroup> object = new BindGroup(this, id, std::move(canvasContexts));
  object->SetLabel(aDesc.mLabel);

  return object.forget();
}

void reportCompilationMessagesToConsole(
    const RefPtr<ShaderModule>& aShaderModule,
    const nsTArray<WebGPUCompilationMessage>& aMessages) {
  auto* global = aShaderModule->GetParentObject();

  dom::AutoJSAPI api;
  if (!api.Init(global)) {
    return;
  }

  const auto& cx = api.cx();
  dom::GlobalObject globalObj(cx, global->GetGlobalJSObject());

  dom::Sequence<JS::Value> args;
  dom::SequenceRooter<JS::Value> msgArgsRooter(cx, &args);
  auto SetSingleStrAsArgs =
      [&](const nsString& message, dom::Sequence<JS::Value>* args)
          MOZ_CAN_RUN_SCRIPT {
            args->Clear();
            JS::Rooted<JSString*> jsStr(
                cx, JS_NewUCStringCopyN(cx, message.Data(), message.Length()));
            if (!jsStr) {
              return;
            }
            JS::Rooted<JS::Value> val(cx, JS::StringValue(jsStr));
            if (!args->AppendElement(val, fallible)) {
              return;
            }
          };

  nsString label;
  aShaderModule->GetLabel(label);
  auto appendNiceLabelIfPresent = [&label](nsString* buf) MOZ_CAN_RUN_SCRIPT {
    if (!label.IsEmpty()) {
      buf->AppendLiteral(u" \"");
      buf->Append(label);
      buf->AppendLiteral(u"\"");
    }
  };

  // We haven't actually inspected a message for severity, but
  // it doesn't actually matter, since we don't do anything at
  // this level.
  auto highestSeveritySeen = WebGPUCompilationMessageType::Info;
  uint64_t errorCount = 0;
  uint64_t warningCount = 0;
  uint64_t infoCount = 0;
  for (const auto& message : aMessages) {
    bool higherThanSeen =
        static_cast<std::underlying_type_t<WebGPUCompilationMessageType>>(
            message.messageType) <
        static_cast<std::underlying_type_t<WebGPUCompilationMessageType>>(
            highestSeveritySeen);
    if (higherThanSeen) {
      highestSeveritySeen = message.messageType;
    }
    switch (message.messageType) {
      case WebGPUCompilationMessageType::Error:
        errorCount += 1;
        break;
      case WebGPUCompilationMessageType::Warning:
        warningCount += 1;
        break;
      case WebGPUCompilationMessageType::Info:
        infoCount += 1;
        break;
    }
  }
  switch (highestSeveritySeen) {
    case WebGPUCompilationMessageType::Info:
      // shouldn't happen, but :shrug:
      break;
    case WebGPUCompilationMessageType::Warning: {
      nsString msg(
          u"Encountered one or more warnings while creating shader module");
      appendNiceLabelIfPresent(&msg);
      SetSingleStrAsArgs(msg, &args);
      dom::Console::Warn(globalObj, args);
      break;
    }
    case WebGPUCompilationMessageType::Error: {
      nsString msg(
          u"Encountered one or more errors while creating shader module");
      appendNiceLabelIfPresent(&msg);
      SetSingleStrAsArgs(msg, &args);
      dom::Console::Error(globalObj, args);
      break;
    }
  }

  nsString header;
  header.AppendLiteral(u"WebGPU compilation info for shader module");
  appendNiceLabelIfPresent(&header);
  header.AppendLiteral(u" (");
  header.AppendInt(errorCount);
  header.AppendLiteral(u" error(s), ");
  header.AppendInt(warningCount);
  header.AppendLiteral(u" warning(s), ");
  header.AppendInt(infoCount);
  header.AppendLiteral(u" info)");
  SetSingleStrAsArgs(header, &args);
  dom::Console::GroupCollapsed(globalObj, args);

  for (const auto& message : aMessages) {
    SetSingleStrAsArgs(message.message, &args);
    switch (message.messageType) {
      case WebGPUCompilationMessageType::Error:
        dom::Console::Error(globalObj, args);
        break;
      case WebGPUCompilationMessageType::Warning:
        dom::Console::Warn(globalObj, args);
        break;
      case WebGPUCompilationMessageType::Info:
        dom::Console::Info(globalObj, args);
        break;
    }
  }
  dom::Console::GroupEnd(globalObj);
}

already_AddRefed<ShaderModule> Device::CreateShaderModule(
    const dom::GPUShaderModuleDescriptor& aDesc, ErrorResult& aRv) {
  RefPtr<dom::Promise> promise = dom::Promise::Create(GetParentObject(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  webgpu::StringHelper label(aDesc.mLabel);

  RawId moduleId = ffi::wgpu_client_create_shader_module(
      mBridge->GetClient(), mId, label.Get(), &aDesc.mCode);

  RefPtr<ShaderModule> shaderModule = new ShaderModule(this, moduleId, promise);

  shaderModule->SetLabel(aDesc.mLabel);

  auto pending_promise = WebGPUChild::PendingCreateShaderModulePromise{
      RefPtr(promise), RefPtr(this), RefPtr(shaderModule)};
  mBridge->mPendingCreateShaderModulePromises.push_back(
      std::move(pending_promise));

  return shaderModule.forget();
}

RawId CreateComputePipelineImpl(PipelineCreationContext* const aContext,
                                WebGPUChild* aBridge,
                                const dom::GPUComputePipelineDescriptor& aDesc,
                                bool isAsync) {
  ffi::WGPUComputePipelineDescriptor desc = {};
  nsCString entryPoint;
  nsTArray<nsCString> constantKeys;
  nsTArray<ffi::WGPUConstantEntry> constants;

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();

  if (aDesc.mLayout.IsGPUAutoLayoutMode()) {
    desc.layout = 0;
  } else if (aDesc.mLayout.IsGPUPipelineLayout()) {
    desc.layout = aDesc.mLayout.GetAsGPUPipelineLayout()->mId;
  } else {
    MOZ_ASSERT_UNREACHABLE();
  }
  desc.stage.module = aDesc.mCompute.mModule->mId;
  if (aDesc.mCompute.mEntryPoint.WasPassed()) {
    CopyUTF16toUTF8(aDesc.mCompute.mEntryPoint.Value(), entryPoint);
    desc.stage.entry_point = entryPoint.get();
  } else {
    desc.stage.entry_point = nullptr;
  }
  if (aDesc.mCompute.mConstants.WasPassed()) {
    const auto& descConstants = aDesc.mCompute.mConstants.Value().Entries();
    constantKeys.SetCapacity(descConstants.Length());
    constants.SetCapacity(descConstants.Length());
    for (const auto& entry : descConstants) {
      ffi::WGPUConstantEntry constantEntry = {};
      nsCString key = NS_ConvertUTF16toUTF8(entry.mKey);
      constantKeys.AppendElement(key);
      constantEntry.key = key.get();
      constantEntry.value = entry.mValue;
      constants.AppendElement(constantEntry);
    }
    desc.stage.constants = {constants.Elements(), constants.Length()};
  }

  RawId implicit_bgl_ids[WGPUMAX_BIND_GROUPS] = {};
  RawId id = ffi::wgpu_client_create_compute_pipeline(
      aBridge->GetClient(), aContext->mParentId, &desc,
      &aContext->mImplicitPipelineLayoutId, implicit_bgl_ids, isAsync);

  for (const auto& cur : implicit_bgl_ids) {
    if (!cur) break;
    aContext->mImplicitBindGroupLayoutIds.AppendElement(cur);
  }

  return id;
}

RawId CreateRenderPipelineImpl(PipelineCreationContext* const aContext,
                               WebGPUChild* aBridge,
                               const dom::GPURenderPipelineDescriptor& aDesc,
                               bool isAsync) {
  // A bunch of stack locals that we can have pointers into
  nsTArray<ffi::WGPUVertexBufferLayout> vertexBuffers;
  nsTArray<ffi::WGPUVertexAttribute> vertexAttributes;
  ffi::WGPURenderPipelineDescriptor desc = {};
  nsCString vsEntry, fsEntry;
  nsTArray<nsCString> vsConstantKeys, fsConstantKeys;
  nsTArray<ffi::WGPUConstantEntry> vsConstants, fsConstants;
  ffi::WGPUIndexFormat stripIndexFormat = ffi::WGPUIndexFormat_Uint16;
  ffi::WGPUFace cullFace = ffi::WGPUFace_Front;
  ffi::WGPUVertexState vertexState = {};
  ffi::WGPUFragmentState fragmentState = {};
  nsTArray<ffi::WGPUColorTargetState> colorStates;
  nsTArray<ffi::WGPUBlendState> blendStates;

  webgpu::StringHelper label(aDesc.mLabel);
  desc.label = label.Get();

  if (aDesc.mLayout.IsGPUAutoLayoutMode()) {
    desc.layout = 0;
  } else if (aDesc.mLayout.IsGPUPipelineLayout()) {
    desc.layout = aDesc.mLayout.GetAsGPUPipelineLayout()->mId;
  } else {
    MOZ_ASSERT_UNREACHABLE();
  }

  {
    const auto& stage = aDesc.mVertex;
    vertexState.stage.module = stage.mModule->mId;
    if (stage.mEntryPoint.WasPassed()) {
      CopyUTF16toUTF8(stage.mEntryPoint.Value(), vsEntry);
      vertexState.stage.entry_point = vsEntry.get();
    } else {
      vertexState.stage.entry_point = nullptr;
    }
    if (stage.mConstants.WasPassed()) {
      const auto& descConstants = stage.mConstants.Value().Entries();
      vsConstantKeys.SetCapacity(descConstants.Length());
      vsConstants.SetCapacity(descConstants.Length());
      for (const auto& entry : descConstants) {
        ffi::WGPUConstantEntry constantEntry = {};
        nsCString key = NS_ConvertUTF16toUTF8(entry.mKey);
        vsConstantKeys.AppendElement(key);
        constantEntry.key = key.get();
        constantEntry.value = entry.mValue;
        vsConstants.AppendElement(constantEntry);
      }
      vertexState.stage.constants = {vsConstants.Elements(),
                                     vsConstants.Length()};
    }

    for (const auto& vertex_desc : stage.mBuffers) {
      ffi::WGPUVertexBufferLayout vb_desc = {};
      if (!vertex_desc.IsNull()) {
        const auto& vd = vertex_desc.Value();
        vb_desc.array_stride = vd.mArrayStride;
        vb_desc.step_mode = ffi::WGPUVertexStepMode(vd.mStepMode);
        // Note: we are setting the length but not the pointer
        vb_desc.attributes = {nullptr, vd.mAttributes.Length()};
        for (const auto& vat : vd.mAttributes) {
          ffi::WGPUVertexAttribute ad = {};
          ad.offset = vat.mOffset;
          ad.format = ConvertVertexFormat(vat.mFormat);
          ad.shader_location = vat.mShaderLocation;
          vertexAttributes.AppendElement(ad);
        }
      }
      vertexBuffers.AppendElement(vb_desc);
    }
    // Now patch up all the pointers to attribute lists.
    size_t numAttributes = 0;
    for (auto& vb_desc : vertexBuffers) {
      vb_desc.attributes.data = vertexAttributes.Elements() + numAttributes;
      numAttributes += vb_desc.attributes.length;
    }

    vertexState.buffers = {vertexBuffers.Elements(), vertexBuffers.Length()};
    desc.vertex = &vertexState;
  }

  if (aDesc.mFragment.WasPassed()) {
    const auto& stage = aDesc.mFragment.Value();
    fragmentState.stage.module = stage.mModule->mId;
    if (stage.mEntryPoint.WasPassed()) {
      CopyUTF16toUTF8(stage.mEntryPoint.Value(), fsEntry);
      fragmentState.stage.entry_point = fsEntry.get();
    } else {
      fragmentState.stage.entry_point = nullptr;
    }
    if (stage.mConstants.WasPassed()) {
      const auto& descConstants = stage.mConstants.Value().Entries();
      fsConstantKeys.SetCapacity(descConstants.Length());
      fsConstants.SetCapacity(descConstants.Length());
      for (const auto& entry : descConstants) {
        ffi::WGPUConstantEntry constantEntry = {};
        nsCString key = NS_ConvertUTF16toUTF8(entry.mKey);
        fsConstantKeys.AppendElement(key);
        constantEntry.key = key.get();
        constantEntry.value = entry.mValue;
        fsConstants.AppendElement(constantEntry);
      }
      fragmentState.stage.constants = {fsConstants.Elements(),
                                       fsConstants.Length()};
    }

    // Note: we pre-collect the blend states into a different array
    // so that we can have non-stale pointers into it.
    for (const auto& colorState : stage.mTargets) {
      ffi::WGPUColorTargetState desc = {};
      desc.format = ConvertTextureFormat(colorState.mFormat);
      desc.write_mask = colorState.mWriteMask;
      colorStates.AppendElement(desc);
      ffi::WGPUBlendState bs = {};
      if (colorState.mBlend.WasPassed()) {
        const auto& blend = colorState.mBlend.Value();
        bs.alpha = ConvertBlendComponent(blend.mAlpha);
        bs.color = ConvertBlendComponent(blend.mColor);
      }
      blendStates.AppendElement(bs);
    }
    for (size_t i = 0; i < colorStates.Length(); ++i) {
      if (stage.mTargets[i].mBlend.WasPassed()) {
        colorStates[i].blend = &blendStates[i];
      }
    }

    fragmentState.targets = {colorStates.Elements(), colorStates.Length()};
    desc.fragment = &fragmentState;
  }

  {
    const auto& prim = aDesc.mPrimitive;
    desc.primitive.topology = ffi::WGPUPrimitiveTopology(prim.mTopology);
    if (prim.mStripIndexFormat.WasPassed()) {
      stripIndexFormat = ffi::WGPUIndexFormat(prim.mStripIndexFormat.Value());
      desc.primitive.strip_index_format = &stripIndexFormat;
    }
    desc.primitive.front_face = ffi::WGPUFrontFace(prim.mFrontFace);
    if (prim.mCullMode != dom::GPUCullMode::None) {
      cullFace = prim.mCullMode == dom::GPUCullMode::Front ? ffi::WGPUFace_Front
                                                           : ffi::WGPUFace_Back;
      desc.primitive.cull_mode = &cullFace;
    }
    desc.primitive.unclipped_depth = prim.mUnclippedDepth;
  }
  desc.multisample = ConvertMultisampleState(aDesc.mMultisample);

  ffi::WGPUDepthStencilState depthStencilState = {};
  if (aDesc.mDepthStencil.WasPassed()) {
    depthStencilState = ConvertDepthStencilState(aDesc.mDepthStencil.Value());
    desc.depth_stencil = &depthStencilState;
  }

  RawId implicit_bgl_ids[WGPUMAX_BIND_GROUPS] = {};
  RawId id = ffi::wgpu_client_create_render_pipeline(
      aBridge->GetClient(), aContext->mParentId, &desc,
      &aContext->mImplicitPipelineLayoutId, implicit_bgl_ids, isAsync);

  for (const auto& cur : implicit_bgl_ids) {
    if (!cur) break;
    aContext->mImplicitBindGroupLayoutIds.AppendElement(cur);
  }

  return id;
}

already_AddRefed<ComputePipeline> Device::CreateComputePipeline(
    const dom::GPUComputePipelineDescriptor& aDesc) {
  PipelineCreationContext context = {mId};
  RawId id = CreateComputePipelineImpl(&context, mBridge, aDesc, false);

  RefPtr<ComputePipeline> object =
      new ComputePipeline(this, id, context.mImplicitPipelineLayoutId,
                          std::move(context.mImplicitBindGroupLayoutIds));
  object->SetLabel(aDesc.mLabel);
  return object.forget();
}

already_AddRefed<RenderPipeline> Device::CreateRenderPipeline(
    const dom::GPURenderPipelineDescriptor& aDesc) {
  PipelineCreationContext context = {mId};
  RawId id = CreateRenderPipelineImpl(&context, mBridge, aDesc, false);

  RefPtr<RenderPipeline> object =
      new RenderPipeline(this, id, context.mImplicitPipelineLayoutId,
                         std::move(context.mImplicitBindGroupLayoutIds));
  object->SetLabel(aDesc.mLabel);

  return object.forget();
}

already_AddRefed<dom::Promise> Device::CreateComputePipelineAsync(
    const dom::GPUComputePipelineDescriptor& aDesc, ErrorResult& aRv) {
  RefPtr<dom::Promise> promise = dom::Promise::Create(GetParentObject(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  std::shared_ptr<PipelineCreationContext> context(
      new PipelineCreationContext());
  context->mParentId = mId;

  RawId pipelineId =
      CreateComputePipelineImpl(context.get(), mBridge, aDesc, true);

  auto pending_promise = WebGPUChild::PendingCreatePipelinePromise{
      RefPtr(promise),
      RefPtr(this),
      false,
      pipelineId,
      context->mImplicitPipelineLayoutId,
      std::move(context->mImplicitBindGroupLayoutIds),
      aDesc.mLabel};
  mBridge->mPendingCreatePipelinePromises.push_back(std::move(pending_promise));

  return promise.forget();
}

already_AddRefed<dom::Promise> Device::CreateRenderPipelineAsync(
    const dom::GPURenderPipelineDescriptor& aDesc, ErrorResult& aRv) {
  RefPtr<dom::Promise> promise = dom::Promise::Create(GetParentObject(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  std::shared_ptr<PipelineCreationContext> context(
      new PipelineCreationContext());
  context->mParentId = mId;

  RawId pipelineId =
      CreateRenderPipelineImpl(context.get(), mBridge, aDesc, true);

  auto pending_promise = WebGPUChild::PendingCreatePipelinePromise{
      RefPtr(promise),
      RefPtr(this),
      true,
      pipelineId,
      context->mImplicitPipelineLayoutId,
      std::move(context->mImplicitBindGroupLayoutIds),
      aDesc.mLabel};
  mBridge->mPendingCreatePipelinePromises.push_back(std::move(pending_promise));

  return promise.forget();
}

already_AddRefed<Texture> Device::InitSwapChain(
    const dom::GPUCanvasConfiguration* const aConfig,
    const layers::RemoteTextureOwnerId aOwnerId,
    mozilla::Span<RawId const> aBufferIds, bool aUseSharedTextureInSwapChain,
    gfx::SurfaceFormat aFormat, gfx::IntSize aCanvasSize) {
  MOZ_ASSERT(aConfig);

  // Check that aCanvasSize and aFormat will generate a texture stride
  // within limits.
  const auto bufferStrideWithMask = BufferStrideWithMask(aCanvasSize, aFormat);
  if (!bufferStrideWithMask.isValid()) {
    return nullptr;
  }

  const layers::RGBDescriptor rgbDesc(aCanvasSize, aFormat);

  ffi::wgpu_client_create_swap_chain(
      mBridge->GetClient(), mId, mQueue->mId, rgbDesc.size().Width(),
      rgbDesc.size().Height(), (int8_t)rgbDesc.format(),
      {aBufferIds.Elements(), aBufferIds.Length()}, aOwnerId.mId,
      aUseSharedTextureInSwapChain);

  // TODO: `mColorSpace`: <https://bugzilla.mozilla.org/show_bug.cgi?id=1846608>
  // TODO: `mAlphaMode`: <https://bugzilla.mozilla.org/show_bug.cgi?id=1846605>
  return CreateTextureForSwapChain(aConfig, aCanvasSize, aOwnerId);
}

bool Device::CheckNewWarning(const nsACString& aMessage) {
  return mKnownWarnings.EnsureInserted(aMessage);
}

void Device::Destroy() {
  // Unmap all buffers from this device, as specified by
  // https://gpuweb.github.io/gpuweb/#dom-gpudevice-destroy.
  dom::AutoJSAPI jsapi;
  if (jsapi.Init(GetOwnerGlobal())) {
    IgnoredErrorResult rv;
    for (const auto& buffer : mTrackedBuffers) {
      buffer->Unmap(jsapi.cx(), rv);
    }

    mTrackedBuffers.Clear();
  }

  ffi::wgpu_client_destroy_device(mBridge->GetClient(), mId);

  if (mLostPromise->State() != dom::Promise::PromiseState::Pending) {
    return;
  }
  RefPtr<dom::Promise> pending_promise = mLostPromise;
  mBridge->mPendingDeviceLostPromises.insert({mId, std::move(pending_promise)});
}

void Device::PushErrorScope(const dom::GPUErrorFilter& aFilter) {
  ffi::wgpu_client_push_error_scope(mBridge->GetClient(), mId,
                                    (uint8_t)aFilter);
}

already_AddRefed<dom::Promise> Device::PopErrorScope(ErrorResult& aRv) {
  RefPtr<dom::Promise> promise = dom::Promise::Create(GetParentObject(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  ffi::wgpu_client_pop_error_scope(mBridge->GetClient(), mId);

  auto pending_promise =
      WebGPUChild::PendingPopErrorScopePromise{RefPtr(promise), RefPtr(this)};
  mBridge->mPendingPopErrorScopePromises.push_back(std::move(pending_promise));

  return promise.forget();
}

}  // namespace mozilla::webgpu
