/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/WebGPUBinding.h"
#include "Adapter.h"

#include <algorithm>
#include "Device.h"
#include "Instance.h"
#include "SupportedFeatures.h"
#include "SupportedLimits.h"
#include "ipc/WebGPUChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/webgpu/ffi/wgpu.h"

namespace mozilla::webgpu {

GPU_IMPL_CYCLE_COLLECTION(AdapterInfo, mParent)
GPU_IMPL_JS_WRAP(AdapterInfo)

uint32_t AdapterInfo::SubgroupMinSize() const {
  // From the spec. at
  // <https://www.w3.org/TR/2025/CRD-webgpu-20250319/#dom-gpuadapterinfo-subgroupminsize>:
  //
  // > If `["subgroups"](https://www.w3.org/TR/webgpu/#subgroups)` is supported,
  // > set `subgroupMinSize` to the smallest supported subgroup size. Otherwise,
  // > set this value to 4.
  // >
  // > Note: To preserve privacy, the user agent may choose to not support some
  // > features or provide values for the property which do not distinguish
  // > different devices, but are still usable (e.g. use the default value of
  // > 4 for all devices).

  if (GetParentObject()->ShouldResistFingerprinting(
          RFPTarget::WebGPUSubgroupSizes)) {
    return 4;
  }

  // TODO: When we support `subgroups`, use the supported amount instead:
  // <https://bugzilla.mozilla.org/show_bug.cgi?id=1955417>
  return 4;
}

uint32_t AdapterInfo::SubgroupMaxSize() const {
  // From the spec. at
  // <https://www.w3.org/TR/2025/CRD-webgpu-20250319/#dom-gpuadapterinfo-subgroupmaxsize>:
  //
  // > If `["subgroups"](https://www.w3.org/TR/webgpu/#subgroups)` is supported,
  // > set `subgroupMaxSize` to the largest supported subgroup size. Otherwise,
  // > set this value to 128.
  // >
  // > Note: To preserve privacy, the user agent may choose to not support some
  // > features or provide values for the property which do not distinguish
  // > different devices, but are still usable (e.g. use the default value of
  // > 128 for all devices).

  if (GetParentObject()->ShouldResistFingerprinting(
          RFPTarget::WebGPUSubgroupSizes)) {
    return 128;
  }

  // TODO: When we support `subgroups`, use the supported amount instead:
  // <https://bugzilla.mozilla.org/show_bug.cgi?id=1955417>
  return 128;
}

bool AdapterInfo::IsFallbackAdapter() const {
  if (GetParentObject()->ShouldResistFingerprinting(
          RFPTarget::WebGPUIsFallbackAdapter)) {
    // Always report hardware support for WebGPU.
    // This behaviour matches with media capabilities API.
    return false;
  }

  return mAboutSupportInfo->device_type ==
         ffi::WGPUDeviceType::WGPUDeviceType_Cpu;
}

void AdapterInfo::GetWgpuName(nsString& s) const {
  s = mAboutSupportInfo->name;
}

uint32_t AdapterInfo::WgpuVendor() const { return mAboutSupportInfo->vendor; }

uint32_t AdapterInfo::WgpuDevice() const { return mAboutSupportInfo->device; }

void AdapterInfo::GetWgpuDeviceType(nsString& s) const {
  switch (mAboutSupportInfo->device_type) {
    case ffi::WGPUDeviceType_Cpu:
      s.AssignLiteral("Cpu");
      return;
    case ffi::WGPUDeviceType_DiscreteGpu:
      s.AssignLiteral("DiscreteGpu");
      return;
    case ffi::WGPUDeviceType_IntegratedGpu:
      s.AssignLiteral("IntegratedGpu");
      return;
    case ffi::WGPUDeviceType_VirtualGpu:
      s.AssignLiteral("VirtualGpu");
      return;
    case ffi::WGPUDeviceType_Other:
      s.AssignLiteral("Other");
      return;
    case ffi::WGPUDeviceType_Sentinel:
      break;
  }
  MOZ_CRASH("Bad `ffi::WGPUDeviceType`");
}

void AdapterInfo::GetWgpuDriver(nsString& s) const {
  s = mAboutSupportInfo->driver;
}

void AdapterInfo::GetWgpuDriverInfo(nsString& s) const {
  s = mAboutSupportInfo->driver_info;
}

void AdapterInfo::GetWgpuBackend(nsString& s) const {
  switch (mAboutSupportInfo->backend) {
    case ffi::WGPUBackend_Noop:
      s.AssignLiteral("No-op");
      return;
    case ffi::WGPUBackend_Vulkan:
      s.AssignLiteral("Vulkan");
      return;
    case ffi::WGPUBackend_Metal:
      s.AssignLiteral("Metal");
      return;
    case ffi::WGPUBackend_Dx12:
      s.AssignLiteral("Dx12");
      return;
    case ffi::WGPUBackend_Gl:
      s.AssignLiteral("Gl");
      return;
    case ffi::WGPUBackend_BrowserWebGpu:  // This should never happen, because
                                          // we _are_ the browser.
    case ffi::WGPUBackend_Sentinel:
      break;
  }
  MOZ_CRASH("Bad `ffi::WGPUBackend`");
}

// -

GPU_IMPL_CYCLE_COLLECTION(Adapter, mParent, mBridge, mFeatures, mLimits, mInfo)
GPU_IMPL_JS_WRAP(Adapter)

enum class FeatureImplementationStatusTag {
  Implemented,
  NotImplemented,
};

struct FeatureImplementationStatus {
  FeatureImplementationStatusTag tag =
      FeatureImplementationStatusTag::NotImplemented;
  union {
    struct {
      ffi::WGPUFeaturesWebGPU wgpuBit;
    } implemented;
    struct {
      const char* bugzillaUrlAscii;
    } unimplemented;
  } value = {
      .unimplemented = {
          .bugzillaUrlAscii =
              "https://bugzilla.mozilla.org/"
              "enter_bug.cgi?product=Core&component=Graphics%3A+WebGPU"}};

  static FeatureImplementationStatus fromDomFeature(
      const dom::GPUFeatureName aFeature) {
    auto implemented = [](const ffi::WGPUFeaturesWebGPU aBit) {
      FeatureImplementationStatus feat;
      feat.tag = FeatureImplementationStatusTag::Implemented;
      feat.value.implemented.wgpuBit = aBit;
      return feat;
    };
    auto unimplemented = [](const char* aBugzillaUrl) {
      FeatureImplementationStatus feat;
      feat.tag = FeatureImplementationStatusTag::NotImplemented;
      feat.value.unimplemented.bugzillaUrlAscii = aBugzillaUrl;
      return feat;
    };
    switch (aFeature) {
      case dom::GPUFeatureName::Depth_clip_control:
        return implemented(WGPUWEBGPU_FEATURE_DEPTH_CLIP_CONTROL);

      case dom::GPUFeatureName::Depth32float_stencil8:
        return implemented(WGPUWEBGPU_FEATURE_DEPTH32FLOAT_STENCIL8);

      case dom::GPUFeatureName::Texture_compression_bc:
        return implemented(WGPUWEBGPU_FEATURE_TEXTURE_COMPRESSION_BC);

      case dom::GPUFeatureName::Texture_compression_bc_sliced_3d:
        return implemented(WGPUWEBGPU_FEATURE_TEXTURE_COMPRESSION_BC_SLICED_3D);

      case dom::GPUFeatureName::Texture_compression_etc2:
        return implemented(WGPUWEBGPU_FEATURE_TEXTURE_COMPRESSION_ETC2);

      case dom::GPUFeatureName::Texture_compression_astc:
        return implemented(WGPUWEBGPU_FEATURE_TEXTURE_COMPRESSION_ASTC);

      case dom::GPUFeatureName::Texture_compression_astc_sliced_3d:
        return implemented(
            WGPUWEBGPU_FEATURE_TEXTURE_COMPRESSION_ASTC_SLICED_3D);

      case dom::GPUFeatureName::Timestamp_query:
        return implemented(WGPUWEBGPU_FEATURE_TIMESTAMP_QUERY);

      case dom::GPUFeatureName::Indirect_first_instance:
        return implemented(WGPUWEBGPU_FEATURE_INDIRECT_FIRST_INSTANCE);

      case dom::GPUFeatureName::Shader_f16:
        return implemented(WGPUWEBGPU_FEATURE_SHADER_F16);

      case dom::GPUFeatureName::Rg11b10ufloat_renderable:
        return implemented(WGPUWEBGPU_FEATURE_RG11B10UFLOAT_RENDERABLE);

      case dom::GPUFeatureName::Bgra8unorm_storage:
        return implemented(WGPUWEBGPU_FEATURE_BGRA8UNORM_STORAGE);

      case dom::GPUFeatureName::Float32_filterable:
        return implemented(WGPUWEBGPU_FEATURE_FLOAT32_FILTERABLE);

      case dom::GPUFeatureName::Float32_blendable:
        return unimplemented(
            "https://bugzilla.mozilla.org/show_bug.cgi?id=1931630");

      case dom::GPUFeatureName::Clip_distances:
        return unimplemented(
            "https://bugzilla.mozilla.org/show_bug.cgi?id=1931629");

      case dom::GPUFeatureName::Dual_source_blending:
        // return implemented(WGPUWEBGPU_FEATURE_DUAL_SOURCE_BLENDING);
        return unimplemented(
            "https://bugzilla.mozilla.org/show_bug.cgi?id=1924328");

      case dom::GPUFeatureName::Subgroups:
        // return implemented(WGPUWEBGPU_FEATURE_SUBGROUPS);
        return unimplemented(
            "https://bugzilla.mozilla.org/show_bug.cgi?id=1955417");

      case dom::GPUFeatureName::Core_features_and_limits:
        // NOTE: `0` means that no bits are set in calling code, but this is on
        // purpose. We currently _always_ return this feature elsewhere. If this
        // actually corresponds to a value in the future, remove the
        // unconditional setting of this feature!
        return implemented(0);
    }
    MOZ_CRASH("Bad GPUFeatureName.");
  }
};

double GetLimitDefault(Limit aLimit) {
  switch (aLimit) {
      // clang-format off
      case Limit::MaxTextureDimension1D: return 8192;
      case Limit::MaxTextureDimension2D: return 8192;
      case Limit::MaxTextureDimension3D: return 2048;
      case Limit::MaxTextureArrayLayers: return 256;
      case Limit::MaxBindGroups: return 4;
      case Limit::MaxBindGroupsPlusVertexBuffers: return 24;
      case Limit::MaxBindingsPerBindGroup: return 1000;
      case Limit::MaxDynamicUniformBuffersPerPipelineLayout: return 8;
      case Limit::MaxDynamicStorageBuffersPerPipelineLayout: return 4;
      case Limit::MaxSampledTexturesPerShaderStage: return 16;
      case Limit::MaxSamplersPerShaderStage: return 16;
      case Limit::MaxStorageBuffersPerShaderStage: return 8;
      case Limit::MaxStorageTexturesPerShaderStage: return 4;
      case Limit::MaxUniformBuffersPerShaderStage: return 12;
      case Limit::MaxUniformBufferBindingSize: return 65536;
      case Limit::MaxStorageBufferBindingSize: return 134217728;
      case Limit::MinUniformBufferOffsetAlignment: return 256;
      case Limit::MinStorageBufferOffsetAlignment: return 256;
      case Limit::MaxVertexBuffers: return 8;
      case Limit::MaxBufferSize: return 268435456;
      case Limit::MaxVertexAttributes: return 16;
      case Limit::MaxVertexBufferArrayStride: return 2048;
      case Limit::MaxInterStageShaderVariables: return 16;
      case Limit::MaxColorAttachments: return 8;
      case Limit::MaxColorAttachmentBytesPerSample: return 32;
      case Limit::MaxComputeWorkgroupStorageSize: return 16384;
      case Limit::MaxComputeInvocationsPerWorkgroup: return 256;
      case Limit::MaxComputeWorkgroupSizeX: return 256;
      case Limit::MaxComputeWorkgroupSizeY: return 256;
      case Limit::MaxComputeWorkgroupSizeZ: return 64;
      case Limit::MaxComputeWorkgroupsPerDimension: return 65535;
      // clang-format on
  }
  MOZ_CRASH("Bad Limit");
}

Adapter::Adapter(Instance* const aParent, WebGPUChild* const aBridge,
                 const std::shared_ptr<ffi::WGPUAdapterInformation>& aInfo)
    : ChildOf(aParent),
      mBridge(aBridge),
      mId(aInfo->id),
      mFeatures(new SupportedFeatures(this)),
      mLimits(new SupportedLimits(this, aInfo->limits)),
      mInfo(new AdapterInfo(this, aInfo)),
      mInfoInner(aInfo) {
  ErrorResult ignoredRv;  // It's onerous to plumb this in from outside in this
                          // case, and we don't really need to.

  static const auto FEATURE_BY_BIT = []() {
    auto ret =
        std::unordered_map<ffi::WGPUFeaturesWebGPU, dom::GPUFeatureName>{};

    for (const auto feature :
         dom::MakeWebIDLEnumeratedRange<dom::GPUFeatureName>()) {
      const auto status = FeatureImplementationStatus::fromDomFeature(feature);
      switch (status.tag) {
        case FeatureImplementationStatusTag::Implemented:
          ret[status.value.implemented.wgpuBit] = feature;
          break;
        case FeatureImplementationStatusTag::NotImplemented:
          break;
      }
    }

    return ret;
  }();

  auto remainingFeatureBits = aInfo->features;
  auto bitMask = decltype(remainingFeatureBits){0};
  while (remainingFeatureBits) {
    if (bitMask) {
      bitMask <<= 1;
    } else {
      bitMask = 1;
    }
    const auto bit = remainingFeatureBits & bitMask;
    remainingFeatureBits &= ~bitMask;  // Clear bit.
    if (!bit) {
      continue;
    }

    const auto featureForBit = FEATURE_BY_BIT.find(bit);
    if (featureForBit != FEATURE_BY_BIT.end()) {
      mFeatures->Add(featureForBit->second, ignoredRv);
    } else {
      // One of two cases:
      //
      // 1. WGPU claims to implement this, but we've explicitly marked this as
      // not implemented.
      // 2. We don't recognize that bit, but maybe it's a wpgu-native-only
      // feature.
    }
  }
  // TODO: Once we implement compat mode (see
  // <https://bugzilla.mozilla.org/show_bug.cgi?id=1905951>), do not report this
  // unconditionally.
  //
  // Meanwhile, the current spec. proposal's `Initialization` section (see
  // <https://github.com/gpuweb/gpuweb/blob/main/proposals/compatibility-mode.md#initialization>)
  // says:
  //
  // > Core-defaulting adapters *always* support the
  // > `"core-features-and-limits"` feature. It is *automatically enabled* on
  // > devices created from such adapters.
  mFeatures->Add(dom::GPUFeatureName::Core_features_and_limits, ignoredRv);

  // We clamp limits to defaults when requestDevice is called, but
  // we return the actual limits when only requestAdapter is called.
  // So, we should clamp the limits here too if we should RFP.
  if (GetParentObject()->ShouldResistFingerprinting(RFPTarget::WebGPULimits)) {
    for (const auto limit : MakeInclusiveEnumeratedRange(Limit::_LAST)) {
      SetLimit(mLimits->mFfi.get(), limit, GetLimitDefault(limit));
    }
  }
}

Adapter::~Adapter() { Cleanup(); }

void Adapter::Cleanup() {
  if (!mValid) {
    return;
  }
  mValid = false;

  if (!mBridge) {
    return;
  }

  ffi::wgpu_client_drop_adapter(mBridge->GetClient(), mId);
}

const RefPtr<SupportedFeatures>& Adapter::Features() const { return mFeatures; }
const RefPtr<SupportedLimits>& Adapter::Limits() const { return mLimits; }
const RefPtr<AdapterInfo>& Adapter::Info() const { return mInfo; }

bool Adapter::SupportSharedTextureInSwapChain() const {
  return mInfoInner->support_use_shared_texture_in_swap_chain;
}

static std::string_view ToJsKey(const Limit limit) {
  switch (limit) {
    case Limit::MaxTextureDimension1D:
      return "maxTextureDimension1D";
    case Limit::MaxTextureDimension2D:
      return "maxTextureDimension2D";
    case Limit::MaxTextureDimension3D:
      return "maxTextureDimension3D";
    case Limit::MaxTextureArrayLayers:
      return "maxTextureArrayLayers";
    case Limit::MaxBindGroups:
      return "maxBindGroups";
    case Limit::MaxBindGroupsPlusVertexBuffers:
      return "maxBindGroupsPlusVertexBuffers";
    case Limit::MaxBindingsPerBindGroup:
      return "maxBindingsPerBindGroup";
    case Limit::MaxDynamicUniformBuffersPerPipelineLayout:
      return "maxDynamicUniformBuffersPerPipelineLayout";
    case Limit::MaxDynamicStorageBuffersPerPipelineLayout:
      return "maxDynamicStorageBuffersPerPipelineLayout";
    case Limit::MaxSampledTexturesPerShaderStage:
      return "maxSampledTexturesPerShaderStage";
    case Limit::MaxSamplersPerShaderStage:
      return "maxSamplersPerShaderStage";
    case Limit::MaxStorageBuffersPerShaderStage:
      return "maxStorageBuffersPerShaderStage";
    case Limit::MaxStorageTexturesPerShaderStage:
      return "maxStorageTexturesPerShaderStage";
    case Limit::MaxUniformBuffersPerShaderStage:
      return "maxUniformBuffersPerShaderStage";
    case Limit::MaxUniformBufferBindingSize:
      return "maxUniformBufferBindingSize";
    case Limit::MaxStorageBufferBindingSize:
      return "maxStorageBufferBindingSize";
    case Limit::MinUniformBufferOffsetAlignment:
      return "minUniformBufferOffsetAlignment";
    case Limit::MinStorageBufferOffsetAlignment:
      return "minStorageBufferOffsetAlignment";
    case Limit::MaxVertexBuffers:
      return "maxVertexBuffers";
    case Limit::MaxBufferSize:
      return "maxBufferSize";
    case Limit::MaxVertexAttributes:
      return "maxVertexAttributes";
    case Limit::MaxVertexBufferArrayStride:
      return "maxVertexBufferArrayStride";
    case Limit::MaxInterStageShaderVariables:
      return "maxInterStageShaderVariables";
    case Limit::MaxColorAttachments:
      return "maxColorAttachments";
    case Limit::MaxColorAttachmentBytesPerSample:
      return "maxColorAttachmentBytesPerSample";
    case Limit::MaxComputeWorkgroupStorageSize:
      return "maxComputeWorkgroupStorageSize";
    case Limit::MaxComputeInvocationsPerWorkgroup:
      return "maxComputeInvocationsPerWorkgroup";
    case Limit::MaxComputeWorkgroupSizeX:
      return "maxComputeWorkgroupSizeX";
    case Limit::MaxComputeWorkgroupSizeY:
      return "maxComputeWorkgroupSizeY";
    case Limit::MaxComputeWorkgroupSizeZ:
      return "maxComputeWorkgroupSizeZ";
    case Limit::MaxComputeWorkgroupsPerDimension:
      return "maxComputeWorkgroupsPerDimension";
  }
  MOZ_CRASH("Bad Limit");
}

uint64_t Adapter::MissingFeatures() const {
  uint64_t missingFeatures = 0;

  // Turn on all implemented features.
  for (const auto feature :
       dom::MakeWebIDLEnumeratedRange<dom::GPUFeatureName>()) {
    const auto status = FeatureImplementationStatus::fromDomFeature(feature);
    switch (status.tag) {
      case FeatureImplementationStatusTag::Implemented:
        missingFeatures |= status.value.implemented.wgpuBit;
        break;
      case FeatureImplementationStatusTag::NotImplemented:
        break;
    }
  }

  // Turn off features that are supported by the adapter.
  for (auto feature : mFeatures->Features()) {
    const auto status = FeatureImplementationStatus::fromDomFeature(feature);
    switch (status.tag) {
      case FeatureImplementationStatusTag::Implemented:
        missingFeatures &= ~status.value.implemented.wgpuBit;
        break;
      case FeatureImplementationStatusTag::NotImplemented:
        break;
    }
  }

  return missingFeatures;
}

// -
// String helpers

static auto ToACString(const nsAString& s) { return NS_ConvertUTF16toUTF8(s); }

// -
// Adapter::RequestDevice

already_AddRefed<dom::Promise> Adapter::RequestDevice(
    const dom::GPUDeviceDescriptor& aDesc, ErrorResult& aRv) {
  RefPtr<dom::Promise> promise = dom::Promise::Create(GetParentObject(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }
  RefPtr<dom::Promise> lost_promise =
      dom::Promise::Create(GetParentObject(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  ffi::WGPULimits deviceLimits = *mLimits->mFfi;
  for (const auto limit : MakeInclusiveEnumeratedRange(Limit::_LAST)) {
    SetLimit(&deviceLimits, limit, GetLimitDefault(limit));
  }

  // -

  [&]() {  // So that we can `return;` instead of `return promise.forget();`.
    // -
    // Validate Features

    ffi::WGPUFeaturesWebGPU featureBits = 0;
    for (const auto requested : aDesc.mRequiredFeatures) {
      auto status = FeatureImplementationStatus::fromDomFeature(requested);
      switch (status.tag) {
        case FeatureImplementationStatusTag::Implemented:
          featureBits |= status.value.implemented.wgpuBit;
          break;
        case FeatureImplementationStatusTag::NotImplemented: {
          const auto featureStr = dom::GetEnumString(requested);
          (void)featureStr;
          nsPrintfCString msg(
              "`GPUAdapter.requestDevice`: '%s' was requested in "
              "`requiredFeatures`, but it is not supported by Firefox."
              "Follow <%s> for updates.",
              featureStr.get(), status.value.unimplemented.bugzillaUrlAscii);
          promise->MaybeRejectWithTypeError(msg);
          return;
        }
      }

      const bool supportedByAdapter = mFeatures->Features().count(requested);
      if (!supportedByAdapter) {
        const auto fstr = dom::GetEnumString(requested);
        const auto astr = this->LabelOrId();
        nsPrintfCString msg(
            "`GPUAdapter.requestDevice`: '%s' was requested in "
            "`requiredFeatures`, but it is not supported by adapter %s.",
            fstr.get(), astr.get());
        promise->MaybeRejectWithTypeError(msg);
        return;
      }
    }

    // -
    // Validate Limits

    if (aDesc.mRequiredLimits.WasPassed()) {
      static const auto LIMIT_BY_JS_KEY = []() {
        std::unordered_map<std::string_view, Limit> ret;
        for (const auto limit : MakeInclusiveEnumeratedRange(Limit::_LAST)) {
          const auto jsKeyU8 = ToJsKey(limit);
          ret[jsKeyU8] = limit;
        }
        return ret;
      }();

      for (const auto& entry : aDesc.mRequiredLimits.Value().Entries()) {
        const auto& keyU16 = entry.mKey;
        const nsCString keyU8 = ToACString(keyU16);
        const auto itr = LIMIT_BY_JS_KEY.find(keyU8.get());
        if (itr == LIMIT_BY_JS_KEY.end()) {
          nsPrintfCString msg("requestDevice: Limit '%s' not recognized.",
                              keyU8.get());
          promise->MaybeRejectWithOperationError(msg);
          return;
        }

        const auto& limit = itr->second;
        uint64_t requestedValue = entry.mValue;
        const auto supportedValue = GetLimit(*mLimits->mFfi, limit);
        if (StringBeginsWith(keyU8, "max"_ns)) {
          if (requestedValue > supportedValue) {
            nsPrintfCString msg(
                "requestDevice: Request for limit '%s' must be <= supported "
                "%s, was %s.",
                keyU8.get(), std::to_string(supportedValue).c_str(),
                std::to_string(requestedValue).c_str());
            promise->MaybeRejectWithOperationError(msg);
            return;
          }
          // Clamp to default if lower than default
          requestedValue =
              std::max(requestedValue, GetLimit(deviceLimits, limit));
        } else {
          MOZ_ASSERT(StringBeginsWith(keyU8, "min"_ns));
          if (requestedValue < supportedValue) {
            nsPrintfCString msg(
                "requestDevice: Request for limit '%s' must be >= supported "
                "%s, was %s.",
                keyU8.get(), std::to_string(supportedValue).c_str(),
                std::to_string(requestedValue).c_str());
            promise->MaybeRejectWithOperationError(msg);
            return;
          }
          if (StringEndsWith(keyU8, "Alignment"_ns)) {
            if (!IsPowerOfTwo(requestedValue)) {
              nsPrintfCString msg(
                  "requestDevice: Request for limit '%s' must be a power of "
                  "two, "
                  "was %s.",
                  keyU8.get(), std::to_string(requestedValue).c_str());
              promise->MaybeRejectWithOperationError(msg);
              return;
            }
          }
          /// Clamp to default if higher than default
          /// Changing implementation in a way that increases fingerprinting
          /// surface? Please create a bug in [Core::Privacy: Anti
          /// Tracking](https://bugzilla.mozilla.org/enter_bug.cgi?product=Core&component=Privacy%3A%20Anti-Tracking)
          requestedValue =
              std::min(requestedValue, GetLimit(deviceLimits, limit));
        }

        SetLimit(&deviceLimits, limit, requestedValue);
      }
    }

    // -

    RefPtr<SupportedFeatures> features = new SupportedFeatures(this);
    for (const auto& feature : aDesc.mRequiredFeatures) {
      features->Add(feature, aRv);
    }
    // TODO: Once we implement compat mode (see
    // <https://bugzilla.mozilla.org/show_bug.cgi?id=1905951>), do not report
    // this unconditionally.
    //
    // Meanwhile, the current spec. proposal's `Initialization` section (see
    // <https://github.com/gpuweb/gpuweb/blob/main/proposals/compatibility-mode.md#initialization>)
    // says:
    //
    // > Core-defaulting adapters *always* support the
    // > `"core-features-and-limits"` feature. It is *automatically enabled* on
    // > devices created from such adapters.
    features->Add(dom::GPUFeatureName::Core_features_and_limits, aRv);

    RefPtr<SupportedLimits> limits = new SupportedLimits(this, deviceLimits);

    ffi::WGPUFfiDeviceDescriptor ffiDesc = {};
    ffiDesc.required_features = featureBits;
    ffiDesc.required_limits = deviceLimits;

    ffi::WGPUDeviceQueueId ids =
        ffi::wgpu_client_request_device(mBridge->GetClient(), mId, &ffiDesc);

    auto pending_promise = WebGPUChild::PendingRequestDevicePromise{
        RefPtr(promise), ids.device, ids.queue, aDesc.mLabel, RefPtr(this),
        features,        limits,     mInfo,     lost_promise};
    mBridge->mPendingRequestDevicePromises.push_back(
        std::move(pending_promise));

  }();

  return promise.forget();
}

}  // namespace mozilla::webgpu
