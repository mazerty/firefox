/* -*- Mode: IDL; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://gpuweb.github.io/gpuweb/
 */

interface mixin GPUObjectBase {
    attribute USVString label;
};

dictionary GPUObjectDescriptorBase {
    USVString label = "";
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUSupportedLimits {
    readonly attribute unsigned long maxTextureDimension1D;
    readonly attribute unsigned long maxTextureDimension2D;
    readonly attribute unsigned long maxTextureDimension3D;
    readonly attribute unsigned long maxTextureArrayLayers;
    readonly attribute unsigned long maxBindGroups;
    readonly attribute unsigned long maxBindGroupsPlusVertexBuffers;
    readonly attribute unsigned long maxBindingsPerBindGroup;
    readonly attribute unsigned long maxDynamicUniformBuffersPerPipelineLayout;
    readonly attribute unsigned long maxDynamicStorageBuffersPerPipelineLayout;
    readonly attribute unsigned long maxSampledTexturesPerShaderStage;
    readonly attribute unsigned long maxSamplersPerShaderStage;
    readonly attribute unsigned long maxStorageBuffersPerShaderStage;
    readonly attribute unsigned long maxStorageTexturesPerShaderStage;
    readonly attribute unsigned long maxUniformBuffersPerShaderStage;
    readonly attribute unsigned long long maxUniformBufferBindingSize;
    readonly attribute unsigned long long maxStorageBufferBindingSize;
    readonly attribute unsigned long minUniformBufferOffsetAlignment;
    readonly attribute unsigned long minStorageBufferOffsetAlignment;
    readonly attribute unsigned long maxVertexBuffers;
    readonly attribute unsigned long long maxBufferSize;
    readonly attribute unsigned long maxVertexAttributes;
    readonly attribute unsigned long maxVertexBufferArrayStride;
    readonly attribute unsigned long maxInterStageShaderVariables;
    readonly attribute unsigned long maxColorAttachments;
    readonly attribute unsigned long maxColorAttachmentBytesPerSample;
    readonly attribute unsigned long maxComputeWorkgroupStorageSize;
    readonly attribute unsigned long maxComputeInvocationsPerWorkgroup;
    readonly attribute unsigned long maxComputeWorkgroupSizeX;
    readonly attribute unsigned long maxComputeWorkgroupSizeY;
    readonly attribute unsigned long maxComputeWorkgroupSizeZ;
    readonly attribute unsigned long maxComputeWorkgroupsPerDimension;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUSupportedFeatures {
    readonly setlike<DOMString>;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface WGSLLanguageFeatures {
    readonly setlike<DOMString>;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUAdapterInfo {
    readonly attribute DOMString vendor;
    readonly attribute DOMString architecture;
    readonly attribute DOMString device;
    readonly attribute DOMString description;
    readonly attribute unsigned long subgroupMinSize;
    readonly attribute unsigned long subgroupMaxSize;
    readonly attribute boolean isFallbackAdapter;

    // Non-standard; see <https://bugzilla.mozilla.org/show_bug.cgi?id=1831994>.
    [ChromeOnly] readonly attribute DOMString wgpuName;
    [ChromeOnly] readonly attribute unsigned long wgpuVendor;
    [ChromeOnly] readonly attribute unsigned long wgpuDevice;
    [ChromeOnly] readonly attribute DOMString wgpuDeviceType;
    [ChromeOnly] readonly attribute DOMString wgpuDriver;
    [ChromeOnly] readonly attribute DOMString wgpuDriverInfo;
    [ChromeOnly] readonly attribute DOMString wgpuBackend;
};

interface mixin NavigatorGPU {
    [SameObject, Func="mozilla::webgpu::Instance::PrefEnabled", SecureContext] readonly attribute GPU gpu;
};
// NOTE: see `dom/webidl/Navigator.webidl`
// Navigator includes NavigatorGPU;
// NOTE: see `dom/webidl/WorkerNavigator.webidl`
// WorkerNavigator includes NavigatorGPU;

[
    Func="mozilla::webgpu::Instance::PrefEnabled",
    Exposed=(Window, Worker), SecureContext
]
interface GPU {
    [Throws]
    Promise<GPUAdapter?> requestAdapter(optional GPURequestAdapterOptions options = {});
    GPUTextureFormat getPreferredCanvasFormat();
    [SameObject] readonly attribute WGSLLanguageFeatures wgslLanguageFeatures;
};

dictionary GPURequestAdapterOptions {
    DOMString featureLevel = "core";
    GPUPowerPreference powerPreference;
    boolean forceFallbackAdapter = false;
    boolean xrCompatible = false;
};

enum GPUPowerPreference {
    "low-power",
    "high-performance",
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUAdapter {
    [SameObject] readonly attribute GPUSupportedFeatures features;
    [SameObject] readonly attribute GPUSupportedLimits limits;
    [SameObject] readonly attribute GPUAdapterInfo info;

    [Throws]
    Promise<GPUDevice> requestDevice(optional GPUDeviceDescriptor descriptor = {});

    [Func="nsRFPService::IsSystemPrincipalOrAboutFingerprintingProtection"]
    readonly attribute unsigned long long missingFeatures;
};

dictionary GPUDeviceDescriptor
         : GPUObjectDescriptorBase {
    sequence<GPUFeatureName> requiredFeatures = [];
    record<DOMString, GPUSize64> requiredLimits;
    GPUQueueDescriptor defaultQueue = {};
};

enum GPUFeatureName {
    "core-features-and-limits",
    "depth-clip-control",
    "depth32float-stencil8",
    "texture-compression-bc",
    "texture-compression-bc-sliced-3d",
    "texture-compression-etc2",
    "texture-compression-astc",
    "texture-compression-astc-sliced-3d",
    "timestamp-query",
    "indirect-first-instance",
    "shader-f16",
    "rg11b10ufloat-renderable",
    "bgra8unorm-storage",
    "float32-filterable",
    "float32-blendable",
    "clip-distances",
    "dual-source-blending",
    "subgroups",
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUDevice : EventTarget {
    [SameObject] readonly attribute GPUSupportedFeatures features;
    [SameObject] readonly attribute GPUSupportedLimits limits;
    [SameObject, BinaryName="GetAdapterInfo"] readonly attribute GPUAdapterInfo adapterInfo;

    [SameObject, BinaryName="getQueue"] readonly attribute GPUQueue queue;

    undefined destroy();

    [Throws]
    GPUBuffer createBuffer(GPUBufferDescriptor descriptor);
    GPUTexture createTexture(GPUTextureDescriptor descriptor);
    GPUSampler createSampler(optional GPUSamplerDescriptor descriptor = {});

    GPUBindGroupLayout createBindGroupLayout(GPUBindGroupLayoutDescriptor descriptor);
    GPUPipelineLayout createPipelineLayout(GPUPipelineLayoutDescriptor descriptor);
    GPUBindGroup createBindGroup(GPUBindGroupDescriptor descriptor);

    [Throws]
    GPUShaderModule createShaderModule(GPUShaderModuleDescriptor descriptor);
    GPUComputePipeline createComputePipeline(GPUComputePipelineDescriptor descriptor);
    GPURenderPipeline createRenderPipeline(GPURenderPipelineDescriptor descriptor);
    [Throws]
    Promise<GPUComputePipeline> createComputePipelineAsync(GPUComputePipelineDescriptor descriptor);
    [Throws]
    Promise<GPURenderPipeline> createRenderPipelineAsync(GPURenderPipelineDescriptor descriptor);

    GPUCommandEncoder createCommandEncoder(optional GPUCommandEncoderDescriptor descriptor = {});
    GPURenderBundleEncoder createRenderBundleEncoder(GPURenderBundleEncoderDescriptor descriptor);

    [Throws]
    GPUQuerySet createQuerySet(GPUQuerySetDescriptor descriptor);
};
GPUDevice includes GPUObjectBase;

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUBuffer {
    readonly attribute GPUSize64Out size;
    readonly attribute GPUFlagsConstant usage;

    readonly attribute GPUBufferMapState mapState;

    [Throws]
    Promise<undefined> mapAsync(GPUMapModeFlags mode, optional GPUSize64 offset = 0, optional GPUSize64 size);
    [Throws]
    ArrayBuffer getMappedRange(optional GPUSize64 offset = 0, optional GPUSize64 size);
    [Throws]
    undefined unmap();

    [Throws]
    undefined destroy();
};
GPUBuffer includes GPUObjectBase;

enum GPUBufferMapState {
    "unmapped",
    "pending",
    "mapped",
};

dictionary GPUBufferDescriptor
         : GPUObjectDescriptorBase {
    required GPUSize64 size;
    required GPUBufferUsageFlags usage;
    boolean mappedAtCreation = false;
};

typedef [EnforceRange] unsigned long GPUBufferUsageFlags;
[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
namespace GPUBufferUsage {
    const GPUFlagsConstant MAP_READ      = 0x0001;
    const GPUFlagsConstant MAP_WRITE     = 0x0002;
    const GPUFlagsConstant COPY_SRC      = 0x0004;
    const GPUFlagsConstant COPY_DST      = 0x0008;
    const GPUFlagsConstant INDEX         = 0x0010;
    const GPUFlagsConstant VERTEX        = 0x0020;
    const GPUFlagsConstant UNIFORM       = 0x0040;
    const GPUFlagsConstant STORAGE       = 0x0080;
    const GPUFlagsConstant INDIRECT      = 0x0100;
    const GPUFlagsConstant QUERY_RESOLVE = 0x0200;
};

typedef [EnforceRange] unsigned long GPUMapModeFlags;
[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
namespace GPUMapMode {
    const GPUFlagsConstant READ  = 0x0001;
    const GPUFlagsConstant WRITE = 0x0002;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUTexture {
    GPUTextureView createView(optional GPUTextureViewDescriptor descriptor = {});

    undefined destroy();

    readonly attribute GPUIntegerCoordinateOut width;
    readonly attribute GPUIntegerCoordinateOut height;
    readonly attribute GPUIntegerCoordinateOut depthOrArrayLayers;
    readonly attribute GPUIntegerCoordinateOut mipLevelCount;
    readonly attribute GPUSize32Out sampleCount;
    readonly attribute GPUTextureDimension dimension;
    readonly attribute GPUTextureFormat format;
    readonly attribute GPUFlagsConstant usage;
};
GPUTexture includes GPUObjectBase;

dictionary GPUTextureDescriptor
         : GPUObjectDescriptorBase {
    required GPUExtent3D size;
    GPUIntegerCoordinate mipLevelCount = 1;
    GPUSize32 sampleCount = 1;
    GPUTextureDimension dimension = "2d";
    required GPUTextureFormat format;
    required GPUTextureUsageFlags usage;
    sequence<GPUTextureFormat> viewFormats = [];
};

enum GPUTextureDimension {
    "1d",
    "2d",
    "3d",
};

typedef [EnforceRange] unsigned long GPUTextureUsageFlags;
[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
namespace GPUTextureUsage {
    const GPUFlagsConstant COPY_SRC          = 0x01;
    const GPUFlagsConstant COPY_DST          = 0x02;
    const GPUFlagsConstant TEXTURE_BINDING   = 0x04;
    const GPUFlagsConstant STORAGE_BINDING   = 0x08;
    const GPUFlagsConstant RENDER_ATTACHMENT = 0x10;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUTextureView {
};
GPUTextureView includes GPUObjectBase;

dictionary GPUTextureViewDescriptor
         : GPUObjectDescriptorBase {
    GPUTextureFormat format;
    GPUTextureViewDimension dimension;
    GPUTextureAspect aspect = "all";
    GPUIntegerCoordinate baseMipLevel = 0;
    GPUIntegerCoordinate mipLevelCount;
    GPUIntegerCoordinate baseArrayLayer = 0;
    GPUIntegerCoordinate arrayLayerCount;
};

enum GPUTextureViewDimension {
    "1d",
    "2d",
    "2d-array",
    "cube",
    "cube-array",
    "3d",
};

enum GPUTextureAspect {
    "all",
    "stencil-only",
    "depth-only",
};

enum GPUTextureFormat {
    // 8-bit formats
    "r8unorm",
    "r8snorm",
    "r8uint",
    "r8sint",

    // 16-bit formats
    "r16uint",
    "r16sint",
    "r16float",
    "rg8unorm",
    "rg8snorm",
    "rg8uint",
    "rg8sint",

    // 32-bit formats
    "r32uint",
    "r32sint",
    "r32float",
    "rg16uint",
    "rg16sint",
    "rg16float",
    "rgba8unorm",
    "rgba8unorm-srgb",
    "rgba8snorm",
    "rgba8uint",
    "rgba8sint",
    "bgra8unorm",
    "bgra8unorm-srgb",
    // Packed 32-bit formats
    "rgb9e5ufloat",
    "rgb10a2uint",
    "rgb10a2unorm",
    "rg11b10ufloat",

    // 64-bit formats
    "rg32uint",
    "rg32sint",
    "rg32float",
    "rgba16uint",
    "rgba16sint",
    "rgba16float",

    // 128-bit formats
    "rgba32uint",
    "rgba32sint",
    "rgba32float",

    // Depth/stencil formats
    "stencil8",
    "depth16unorm",
    "depth24plus",
    "depth24plus-stencil8",
    "depth32float",

    // "depth32float-stencil8" feature
    "depth32float-stencil8",

    // BC compressed formats usable if "texture-compression-bc" is both
    // supported by the device/user agent and enabled in requestDevice.
    "bc1-rgba-unorm",
    "bc1-rgba-unorm-srgb",
    "bc2-rgba-unorm",
    "bc2-rgba-unorm-srgb",
    "bc3-rgba-unorm",
    "bc3-rgba-unorm-srgb",
    "bc4-r-unorm",
    "bc4-r-snorm",
    "bc5-rg-unorm",
    "bc5-rg-snorm",
    "bc6h-rgb-ufloat",
    "bc6h-rgb-float",
    "bc7-rgba-unorm",
    "bc7-rgba-unorm-srgb",

    // ETC2 compressed formats usable if "texture-compression-etc2" is both
    // supported by the device/user agent and enabled in requestDevice.
    "etc2-rgb8unorm",
    "etc2-rgb8unorm-srgb",
    "etc2-rgb8a1unorm",
    "etc2-rgb8a1unorm-srgb",
    "etc2-rgba8unorm",
    "etc2-rgba8unorm-srgb",
    "eac-r11unorm",
    "eac-r11snorm",
    "eac-rg11unorm",
    "eac-rg11snorm",

    // ASTC compressed formats usable if "texture-compression-astc" is both
    // supported by the device/user agent and enabled in requestDevice.
    "astc-4x4-unorm",
    "astc-4x4-unorm-srgb",
    "astc-5x4-unorm",
    "astc-5x4-unorm-srgb",
    "astc-5x5-unorm",
    "astc-5x5-unorm-srgb",
    "astc-6x5-unorm",
    "astc-6x5-unorm-srgb",
    "astc-6x6-unorm",
    "astc-6x6-unorm-srgb",
    "astc-8x5-unorm",
    "astc-8x5-unorm-srgb",
    "astc-8x6-unorm",
    "astc-8x6-unorm-srgb",
    "astc-8x8-unorm",
    "astc-8x8-unorm-srgb",
    "astc-10x5-unorm",
    "astc-10x5-unorm-srgb",
    "astc-10x6-unorm",
    "astc-10x6-unorm-srgb",
    "astc-10x8-unorm",
    "astc-10x8-unorm-srgb",
    "astc-10x10-unorm",
    "astc-10x10-unorm-srgb",
    "astc-12x10-unorm",
    "astc-12x10-unorm-srgb",
    "astc-12x12-unorm",
    "astc-12x12-unorm-srgb",
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUExternalTexture {
};
GPUExternalTexture includes GPUObjectBase;

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUSampler {
};
GPUSampler includes GPUObjectBase;

dictionary GPUSamplerDescriptor
         : GPUObjectDescriptorBase {
    GPUAddressMode addressModeU = "clamp-to-edge";
    GPUAddressMode addressModeV = "clamp-to-edge";
    GPUAddressMode addressModeW = "clamp-to-edge";
    GPUFilterMode magFilter = "nearest";
    GPUFilterMode minFilter = "nearest";
    GPUMipmapFilterMode mipmapFilter = "nearest";
    float lodMinClamp = 0;
    float lodMaxClamp = 32;
    GPUCompareFunction compare;
    [Clamp] unsigned short maxAnisotropy = 1;
};

enum GPUAddressMode {
    "clamp-to-edge",
    "repeat",
    "mirror-repeat",
};

enum GPUFilterMode {
    "nearest",
    "linear",
};

enum GPUMipmapFilterMode {
    "nearest",
    "linear",
};

enum GPUCompareFunction {
    "never",
    "less",
    "equal",
    "less-equal",
    "greater",
    "not-equal",
    "greater-equal",
    "always",
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUBindGroupLayout {
};
GPUBindGroupLayout includes GPUObjectBase;

dictionary GPUBindGroupLayoutDescriptor
         : GPUObjectDescriptorBase {
    required sequence<GPUBindGroupLayoutEntry> entries;
};

dictionary GPUBindGroupLayoutEntry {
    required GPUIndex32 binding;
    required GPUShaderStageFlags visibility;

    GPUBufferBindingLayout buffer;
    GPUSamplerBindingLayout sampler;
    GPUTextureBindingLayout texture;
    GPUStorageTextureBindingLayout storageTexture;
    GPUExternalTextureBindingLayout externalTexture;
};

typedef [EnforceRange] unsigned long GPUShaderStageFlags;
[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
namespace GPUShaderStage {
    const GPUFlagsConstant VERTEX   = 0x1;
    const GPUFlagsConstant FRAGMENT = 0x2;
    const GPUFlagsConstant COMPUTE  = 0x4;
};

enum GPUBufferBindingType {
    "uniform",
    "storage",
    "read-only-storage",
};

dictionary GPUBufferBindingLayout {
    GPUBufferBindingType type = "uniform";
    boolean hasDynamicOffset = false;
    GPUSize64 minBindingSize = 0;
};

enum GPUSamplerBindingType {
    "filtering",
    "non-filtering",
    "comparison",
};

dictionary GPUSamplerBindingLayout {
    GPUSamplerBindingType type = "filtering";
};

enum GPUTextureSampleType {
    "float",
    "unfilterable-float",
    "depth",
    "sint",
    "uint",
};

dictionary GPUTextureBindingLayout {
    GPUTextureSampleType sampleType = "float";
    GPUTextureViewDimension viewDimension = "2d";
    boolean multisampled = false;
};

enum GPUStorageTextureAccess {
    "write-only",
    "read-only",
    "read-write",
};

dictionary GPUStorageTextureBindingLayout {
    GPUStorageTextureAccess access = "write-only";
    required GPUTextureFormat format;
    GPUTextureViewDimension viewDimension = "2d";
};

dictionary GPUExternalTextureBindingLayout {
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUBindGroup {
};
GPUBindGroup includes GPUObjectBase;

dictionary GPUBindGroupDescriptor
         : GPUObjectDescriptorBase {
    required GPUBindGroupLayout layout;
    required sequence<GPUBindGroupEntry> entries;
};

typedef (GPUSampler or
         GPUTexture or
         GPUTextureView or
         GPUBuffer or
         GPUBufferBinding) GPUBindingResource;

dictionary GPUBindGroupEntry {
    required GPUIndex32 binding;
    required GPUBindingResource resource;
};

dictionary GPUBufferBinding {
    required GPUBuffer buffer;
    GPUSize64 offset = 0;
    GPUSize64 size;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUPipelineLayout {
};
GPUPipelineLayout includes GPUObjectBase;

dictionary GPUPipelineLayoutDescriptor
         : GPUObjectDescriptorBase {
    required sequence<GPUBindGroupLayout> bindGroupLayouts;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUShaderModule {
    [Throws]
    Promise<GPUCompilationInfo> getCompilationInfo();
};
GPUShaderModule includes GPUObjectBase;

dictionary GPUShaderModuleDescriptor
         : GPUObjectDescriptorBase {
    // UTF8String is not observably different from USVString
    required UTF8String code;
    sequence<GPUShaderModuleCompilationHint> compilationHints = [];
};

dictionary GPUShaderModuleCompilationHint {
    required USVString entryPoint;
    (GPUPipelineLayout or GPUAutoLayoutMode) layout;
};

enum GPUCompilationMessageType {
    "error",
    "warning",
    "info",
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUCompilationMessage {
    readonly attribute DOMString message;
    readonly attribute GPUCompilationMessageType type;
    readonly attribute unsigned long long lineNum;
    readonly attribute unsigned long long linePos;
    readonly attribute unsigned long long offset;
    readonly attribute unsigned long long length;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUCompilationInfo {
    [Cached, Frozen, Pure]
    readonly attribute sequence<GPUCompilationMessage> messages;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUPipelineError : DOMException {
    constructor(optional DOMString message = "", GPUPipelineErrorInit options);
    readonly attribute GPUPipelineErrorReason reason;
};

dictionary GPUPipelineErrorInit {
    required GPUPipelineErrorReason reason;
};

enum GPUPipelineErrorReason {
    "validation",
    "internal",
};

enum GPUAutoLayoutMode {
    "auto",
};

dictionary GPUPipelineDescriptorBase
         : GPUObjectDescriptorBase {
    required (GPUPipelineLayout or GPUAutoLayoutMode) layout;
};

interface mixin GPUPipelineBase {
    GPUBindGroupLayout getBindGroupLayout(unsigned long index);
};

dictionary GPUProgrammableStage {
    required GPUShaderModule module;
    USVString entryPoint;
    record<USVString, GPUPipelineConstantValue> constants;
};

typedef double GPUPipelineConstantValue; // May represent WGSL's bool, f32, i32, u32, and f16 if enabled.

//TODO: Serializable
// https://bugzilla.mozilla.org/show_bug.cgi?id=1696219
[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUComputePipeline {
};
GPUComputePipeline includes GPUObjectBase;
GPUComputePipeline includes GPUPipelineBase;

dictionary GPUComputePipelineDescriptor
         : GPUPipelineDescriptorBase {
    required GPUProgrammableStage compute;
};

//TODO: Serializable
// https://bugzilla.mozilla.org/show_bug.cgi?id=1696219
[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPURenderPipeline {
};
GPURenderPipeline includes GPUObjectBase;
GPURenderPipeline includes GPUPipelineBase;

dictionary GPURenderPipelineDescriptor
         : GPUPipelineDescriptorBase {
    required GPUVertexState vertex;
    GPUPrimitiveState primitive = {};
    GPUDepthStencilState depthStencil;
    GPUMultisampleState multisample = {};
    GPUFragmentState fragment;
};

dictionary GPUPrimitiveState {
    GPUPrimitiveTopology topology = "triangle-list";
    GPUIndexFormat stripIndexFormat;
    GPUFrontFace frontFace = "ccw";
    GPUCullMode cullMode = "none";

    // Requires "depth-clip-control" feature.
    boolean unclippedDepth = false;
};

enum GPUPrimitiveTopology {
    "point-list",
    "line-list",
    "line-strip",
    "triangle-list",
    "triangle-strip",
};

enum GPUFrontFace {
    "ccw",
    "cw",
};

enum GPUCullMode {
    "none",
    "front",
    "back",
};

dictionary GPUMultisampleState {
    GPUSize32 count = 1;
    GPUSampleMask mask = 0xFFFFFFFF;
    boolean alphaToCoverageEnabled = false;
};

dictionary GPUFragmentState
         : GPUProgrammableStage {
    required sequence<GPUColorTargetState> targets;
};

dictionary GPUColorTargetState {
    required GPUTextureFormat format;

    GPUBlendState blend;
    GPUColorWriteFlags writeMask = 0xF;  // GPUColorWrite.ALL
};

dictionary GPUBlendState {
    required GPUBlendComponent color;
    required GPUBlendComponent alpha;
};

typedef [EnforceRange] unsigned long GPUColorWriteFlags;
[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
namespace GPUColorWrite {
    const GPUFlagsConstant RED   = 0x1;
    const GPUFlagsConstant GREEN = 0x2;
    const GPUFlagsConstant BLUE  = 0x4;
    const GPUFlagsConstant ALPHA = 0x8;
    const GPUFlagsConstant ALL   = 0xF;
};

dictionary GPUBlendComponent {
    GPUBlendOperation operation = "add";
    GPUBlendFactor srcFactor = "one";
    GPUBlendFactor dstFactor = "zero";
};

enum GPUBlendFactor {
    "zero",
    "one",
    "src",
    "one-minus-src",
    "src-alpha",
    "one-minus-src-alpha",
    "dst",
    "one-minus-dst",
    "dst-alpha",
    "one-minus-dst-alpha",
    "src-alpha-saturated",
    "constant",
    "one-minus-constant",
};

enum GPUBlendOperation {
    "add",
    "subtract",
    "reverse-subtract",
    "min",
    "max",
};

dictionary GPUDepthStencilState {
    required GPUTextureFormat format;

    boolean depthWriteEnabled = false;
    GPUCompareFunction depthCompare = "always";

    GPUStencilFaceState stencilFront = {};
    GPUStencilFaceState stencilBack = {};

    GPUStencilValue stencilReadMask = 0xFFFFFFFF;
    GPUStencilValue stencilWriteMask = 0xFFFFFFFF;

    GPUDepthBias depthBias = 0;
    float depthBiasSlopeScale = 0;
    float depthBiasClamp = 0;
};

dictionary GPUStencilFaceState {
    GPUCompareFunction compare = "always";
    GPUStencilOperation failOp = "keep";
    GPUStencilOperation depthFailOp = "keep";
    GPUStencilOperation passOp = "keep";
};

enum GPUStencilOperation {
    "keep",
    "zero",
    "replace",
    "invert",
    "increment-clamp",
    "decrement-clamp",
    "increment-wrap",
    "decrement-wrap",
};

enum GPUIndexFormat {
    "uint16",
    "uint32",
};

enum GPUVertexFormat {
    "uint8",
    "uint8x2",
    "uint8x4",
    "sint8",
    "sint8x2",
    "sint8x4",
    "unorm8",
    "unorm8x2",
    "unorm8x4",
    "snorm8",
    "snorm8x2",
    "snorm8x4",
    "uint16",
    "uint16x2",
    "uint16x4",
    "sint16",
    "sint16x2",
    "sint16x4",
    "unorm16",
    "unorm16x2",
    "unorm16x4",
    "snorm16",
    "snorm16x2",
    "snorm16x4",
    "float16",
    "float16x2",
    "float16x4",
    "float32",
    "float32x2",
    "float32x3",
    "float32x4",
    "uint32",
    "uint32x2",
    "uint32x3",
    "uint32x4",
    "sint32",
    "sint32x2",
    "sint32x3",
    "sint32x4",
    "unorm10-10-10-2",
    "unorm8x4-bgra",
};

enum GPUVertexStepMode {
    "vertex",
    "instance",
};

dictionary GPUVertexState
         : GPUProgrammableStage {
    sequence<GPUVertexBufferLayout?> buffers = [];
};

dictionary GPUVertexBufferLayout {
    required GPUSize64 arrayStride;
    GPUVertexStepMode stepMode = "vertex";
    required sequence<GPUVertexAttribute> attributes;
};

dictionary GPUVertexAttribute {
    required GPUVertexFormat format;
    required GPUSize64 offset;

    required GPUIndex32 shaderLocation;
};

dictionary GPUTexelCopyBufferLayout {
    GPUSize64 offset = 0;
    GPUSize32 bytesPerRow;
    GPUSize32 rowsPerImage;
};

dictionary GPUTexelCopyBufferInfo
         : GPUTexelCopyBufferLayout {
    required GPUBuffer buffer;
};

dictionary GPUTexelCopyTextureInfo {
    required GPUTexture texture;
    GPUIntegerCoordinate mipLevel = 0;
    GPUOrigin3D origin = {};
    GPUTextureAspect aspect = "all";
};

dictionary GPUCopyExternalImageDestInfo
         : GPUTexelCopyTextureInfo {
    //GPUPredefinedColorSpace colorSpace = "srgb"; //TODO
    boolean premultipliedAlpha = false;
};

typedef (ImageBitmap or
         HTMLImageElement or
         HTMLCanvasElement or
         OffscreenCanvas) GPUCopyExternalImageSource;

dictionary GPUCopyExternalImageSourceInfo {
    required GPUCopyExternalImageSource source;
    GPUOrigin2D origin = {};
    boolean flipY = false;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUCommandBuffer {
};
GPUCommandBuffer includes GPUObjectBase;

dictionary GPUCommandBufferDescriptor
         : GPUObjectDescriptorBase {
};

interface mixin GPUCommandsMixin {
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUCommandEncoder {
    GPURenderPassEncoder beginRenderPass(GPURenderPassDescriptor descriptor);
    GPUComputePassEncoder beginComputePass(optional GPUComputePassDescriptor descriptor = {});

    undefined copyBufferToBuffer(
        GPUBuffer source,
        GPUBuffer destination,
        optional GPUSize64 size);
    undefined copyBufferToBuffer(
        GPUBuffer source,
        GPUSize64 sourceOffset,
        GPUBuffer destination,
        GPUSize64 destinationOffset,
        optional GPUSize64 size);

    undefined copyBufferToTexture(
        GPUTexelCopyBufferInfo source,
        GPUTexelCopyTextureInfo destination,
        GPUExtent3D copySize);

    undefined copyTextureToBuffer(
        GPUTexelCopyTextureInfo source,
        GPUTexelCopyBufferInfo destination,
        GPUExtent3D copySize);

    undefined copyTextureToTexture(
        GPUTexelCopyTextureInfo source,
        GPUTexelCopyTextureInfo destination,
        GPUExtent3D copySize);

    undefined clearBuffer(
        GPUBuffer buffer,
        optional GPUSize64 offset = 0,
        optional GPUSize64 size);

    undefined resolveQuerySet(
        GPUQuerySet querySet,
        GPUSize32 firstQuery,
        GPUSize32 queryCount,
        GPUBuffer destination,
        GPUSize64 destinationOffset);

    GPUCommandBuffer finish(optional GPUCommandBufferDescriptor descriptor = {});
};
GPUCommandEncoder includes GPUObjectBase;
GPUCommandEncoder includes GPUCommandsMixin;
GPUCommandEncoder includes GPUDebugCommandsMixin;

dictionary GPUCommandEncoderDescriptor
         : GPUObjectDescriptorBase {
};

interface mixin GPUBindingCommandsMixin {
    [Throws]
    undefined setBindGroup(GPUIndex32 index, GPUBindGroup? bindGroup,
        optional sequence<GPUBufferDynamicOffset> dynamicOffsets = []);
    [Throws]
    undefined setBindGroup(GPUIndex32 index, GPUBindGroup? bindGroup,
        [AllowShared] Uint32Array dynamicOffsetsData,
        GPUSize64 dynamicOffsetsDataStart,
        GPUSize32 dynamicOffsetsDataLength);
};

interface mixin GPUDebugCommandsMixin {
    undefined pushDebugGroup(USVString groupLabel);
    undefined popDebugGroup();
    undefined insertDebugMarker(USVString markerLabel);
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUComputePassEncoder {
    undefined setPipeline(GPUComputePipeline pipeline);
    undefined dispatchWorkgroups(GPUSize32 workgroupCountX, optional GPUSize32 workgroupCountY = 1, optional GPUSize32 workgroupCountZ = 1);
    undefined dispatchWorkgroupsIndirect(GPUBuffer indirectBuffer, GPUSize64 indirectOffset);

    undefined end();
};
GPUComputePassEncoder includes GPUObjectBase;
GPUComputePassEncoder includes GPUCommandsMixin;
GPUComputePassEncoder includes GPUDebugCommandsMixin;
GPUComputePassEncoder includes GPUBindingCommandsMixin;

dictionary GPUComputePassTimestampWrites {
    required GPUQuerySet querySet;
    GPUSize32 beginningOfPassWriteIndex;
    GPUSize32 endOfPassWriteIndex;
};

dictionary GPUComputePassDescriptor
         : GPUObjectDescriptorBase {
    GPUComputePassTimestampWrites timestampWrites;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPURenderPassEncoder {
    undefined setViewport(float x, float y,
        float width, float height,
        float minDepth, float maxDepth);

    undefined setScissorRect(GPUIntegerCoordinate x, GPUIntegerCoordinate y,
                        GPUIntegerCoordinate width, GPUIntegerCoordinate height);

    undefined setBlendConstant(GPUColor color);
    undefined setStencilReference(GPUStencilValue reference);

    undefined beginOcclusionQuery(GPUSize32 queryIndex);
    undefined endOcclusionQuery();

    undefined executeBundles(sequence<GPURenderBundle> bundles);
    undefined end();
};
GPURenderPassEncoder includes GPUObjectBase;
GPURenderPassEncoder includes GPUCommandsMixin;
GPURenderPassEncoder includes GPUDebugCommandsMixin;
GPURenderPassEncoder includes GPUBindingCommandsMixin;
GPURenderPassEncoder includes GPURenderCommandsMixin;

dictionary GPURenderPassTimestampWrites {
    required GPUQuerySet querySet;
    GPUSize32 beginningOfPassWriteIndex;
    GPUSize32 endOfPassWriteIndex;
};

dictionary GPURenderPassDescriptor
         : GPUObjectDescriptorBase {
    required sequence<GPURenderPassColorAttachment> colorAttachments;
    GPURenderPassDepthStencilAttachment depthStencilAttachment;
    GPUQuerySet occlusionQuerySet;
    GPURenderPassTimestampWrites timestampWrites;
};

dictionary GPURenderPassColorAttachment {
    required (GPUTexture or GPUTextureView) view;
    GPUIntegerCoordinate depthSlice;
    (GPUTexture or GPUTextureView) resolveTarget;

    GPUColor clearValue;
    required GPULoadOp loadOp;
    required GPUStoreOp storeOp;
};

dictionary GPURenderPassDepthStencilAttachment {
    required (GPUTexture or GPUTextureView) view;

    float depthClearValue;
    GPULoadOp depthLoadOp;
    GPUStoreOp depthStoreOp;
    boolean depthReadOnly = false;

    GPUStencilValue stencilClearValue = 0;
    GPULoadOp stencilLoadOp;
    GPUStoreOp stencilStoreOp;
    boolean stencilReadOnly = false;
};

enum GPULoadOp {
    "load",
    "clear",
};

enum GPUStoreOp {
    "store",
    "discard",
};

dictionary GPURenderPassLayout
         : GPUObjectDescriptorBase {
    required sequence<GPUTextureFormat> colorFormats;
    GPUTextureFormat depthStencilFormat;
    GPUSize32 sampleCount = 1;
};

interface mixin GPURenderCommandsMixin {
    undefined setPipeline(GPURenderPipeline pipeline);

    undefined setIndexBuffer(GPUBuffer buffer, GPUIndexFormat indexFormat, optional GPUSize64 offset = 0, optional GPUSize64 size);
    undefined setVertexBuffer(GPUIndex32 slot, GPUBuffer buffer, optional GPUSize64 offset = 0, optional GPUSize64 size);

    undefined draw(GPUSize32 vertexCount, optional GPUSize32 instanceCount = 1,
        optional GPUSize32 firstVertex = 0, optional GPUSize32 firstInstance = 0);
    undefined drawIndexed(GPUSize32 indexCount, optional GPUSize32 instanceCount = 1,
        optional GPUSize32 firstIndex = 0,
        optional GPUSignedOffset32 baseVertex = 0,
        optional GPUSize32 firstInstance = 0);

    undefined drawIndirect(GPUBuffer indirectBuffer, GPUSize64 indirectOffset);
    undefined drawIndexedIndirect(GPUBuffer indirectBuffer, GPUSize64 indirectOffset);
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPURenderBundle {
};
GPURenderBundle includes GPUObjectBase;

dictionary GPURenderBundleDescriptor
         : GPUObjectDescriptorBase {
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPURenderBundleEncoder {
    GPURenderBundle finish(optional GPURenderBundleDescriptor descriptor = {});
};
GPURenderBundleEncoder includes GPUObjectBase;
GPURenderBundleEncoder includes GPUCommandsMixin;
GPURenderBundleEncoder includes GPUDebugCommandsMixin;
GPURenderBundleEncoder includes GPUBindingCommandsMixin;
GPURenderBundleEncoder includes GPURenderCommandsMixin;

dictionary GPURenderBundleEncoderDescriptor
         : GPURenderPassLayout {
    boolean depthReadOnly = false;
    boolean stencilReadOnly = false;
};

dictionary GPUQueueDescriptor
         : GPUObjectDescriptorBase {
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUQueue {
    undefined submit(sequence<GPUCommandBuffer> buffers);

    [Throws]
    Promise<undefined> onSubmittedWorkDone();

    [Throws]
    undefined writeBuffer(
        GPUBuffer buffer,
        GPUSize64 bufferOffset,
        AllowSharedBufferSource data,
        optional GPUSize64 dataOffset = 0,
        optional GPUSize64 size);

    [Throws]
    undefined writeTexture(
        GPUTexelCopyTextureInfo destination,
        AllowSharedBufferSource data,
        GPUTexelCopyBufferLayout dataLayout,
        GPUExtent3D size);

    [Throws]
    undefined copyExternalImageToTexture(
        GPUCopyExternalImageSourceInfo source,
        GPUCopyExternalImageDestInfo destination,
        GPUExtent3D copySize);
};
GPUQueue includes GPUObjectBase;

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUQuerySet {
    undefined destroy();

    readonly attribute GPUQueryType type;
    readonly attribute GPUSize32Out count;
};
GPUQuerySet includes GPUObjectBase;

dictionary GPUQuerySetDescriptor
         : GPUObjectDescriptorBase {
    required GPUQueryType type;
    required GPUSize32 count;
};

enum GPUQueryType {
    "occlusion",
    "timestamp",
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUCanvasContext {
    readonly attribute (HTMLCanvasElement or OffscreenCanvas) canvas;

    [Throws]
    undefined configure(GPUCanvasConfiguration configuration);
    undefined unconfigure();

    GPUCanvasConfiguration? getConfiguration();
    [Throws]
    GPUTexture getCurrentTexture();
};

enum GPUCanvasAlphaMode {
    "opaque",
    "premultiplied",
};

dictionary GPUCanvasConfiguration {
    required GPUDevice device;
    required GPUTextureFormat format;
    GPUTextureUsageFlags usage = 0x10;  // GPUTextureUsage.RENDER_ATTACHMENT
    sequence<GPUTextureFormat> viewFormats = [];
    //GPUPredefinedColorSpace colorSpace = "srgb"; //TODO bug 1834395
    //GPUCanvasToneMapping toneMapping = {}; //TODO bug 1834395
    GPUCanvasAlphaMode alphaMode = "opaque";
};

enum GPUDeviceLostReason {
    "unknown",
    "destroyed",
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUDeviceLostInfo {
    readonly attribute GPUDeviceLostReason reason;
    readonly attribute DOMString message;
};

partial interface GPUDevice {
    [Throws]
    readonly attribute Promise<GPUDeviceLostInfo> lost;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUError {
    readonly attribute DOMString message;
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUValidationError
        : GPUError {
    [Throws]
    constructor(DOMString message);
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUOutOfMemoryError
        : GPUError {
    [Throws]
    constructor(DOMString message);
};

[Func="mozilla::webgpu::Instance::PrefEnabled",
 Exposed=(Window, Worker), SecureContext]
interface GPUInternalError
        : GPUError {
    [Throws]
    constructor(DOMString message);
};

enum GPUErrorFilter {
    "validation",
    "out-of-memory",
    "internal",
};

partial interface GPUDevice {
    undefined pushErrorScope(GPUErrorFilter filter);
    [Throws]
    Promise<GPUError?> popErrorScope();
};

// NOTE: `GPUUncapturedErrorEvent{,Init}` is in `GPUUncapturedErrorEvent.webidl`.

partial interface GPUDevice {
    [Exposed=(Window, Worker)]
    attribute EventHandler onuncapturederror;
};

typedef [EnforceRange] unsigned long GPUBufferDynamicOffset;
typedef [EnforceRange] unsigned long GPUStencilValue;
typedef [EnforceRange] unsigned long GPUSampleMask;
typedef [EnforceRange] long GPUDepthBias;

typedef [EnforceRange] unsigned long long GPUSize64;
typedef [EnforceRange] unsigned long GPUIntegerCoordinate;
typedef [EnforceRange] unsigned long GPUIndex32;
typedef [EnforceRange] unsigned long GPUSize32;
typedef [EnforceRange] long GPUSignedOffset32;

typedef unsigned long long GPUSize64Out;
typedef unsigned long GPUIntegerCoordinateOut;
typedef unsigned long GPUSize32Out;

typedef unsigned long GPUFlagsConstant;

dictionary GPUColorDict {
    required double r;
    required double g;
    required double b;
    required double a;
};
typedef (sequence<double> or GPUColorDict) GPUColor;

dictionary GPUOrigin2DDict {
    GPUIntegerCoordinate x = 0;
    GPUIntegerCoordinate y = 0;
};
typedef (sequence<GPUIntegerCoordinate> or GPUOrigin2DDict) GPUOrigin2D;

dictionary GPUOrigin3DDict {
    GPUIntegerCoordinate x = 0;
    GPUIntegerCoordinate y = 0;
    GPUIntegerCoordinate z = 0;
};
typedef (sequence<GPUIntegerCoordinate> or GPUOrigin3DDict) GPUOrigin3D;

dictionary GPUExtent3DDict {
    required GPUIntegerCoordinate width;
    GPUIntegerCoordinate height = 1;
    GPUIntegerCoordinate depthOrArrayLayers = 1;
};
typedef (sequence<GPUIntegerCoordinate> or GPUExtent3DDict) GPUExtent3D;
