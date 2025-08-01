/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/NativeLayerCA.h"

#ifdef XP_MACOSX
#  import <AppKit/NSAnimationContext.h>
#  import <AppKit/NSColor.h>
#  import <OpenGL/gl.h>
#endif
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

#include "gfxUtils.h"
#include "GLBlitHelper.h"
#ifdef XP_MACOSX
#  include "GLContextCGL.h"
#else
#  include "GLContextEAGL.h"
#endif
#include "GLContextProvider.h"
#include "MozFramebuffer.h"
#include "mozilla/gfx/Swizzle.h"
#include "mozilla/layers/ScreenshotGrabber.h"
#include "mozilla/layers/SurfacePoolCA.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/glean/GfxMetrics.h"
#include "mozilla/webrender/RenderMacIOSurfaceTextureHost.h"
#include "ScopedGLHelpers.h"

@interface CALayer (PrivateSetContentsOpaque)
- (void)setContentsOpaque:(BOOL)opaque;
@end

namespace mozilla {
namespace layers {

using gfx::DataSourceSurface;
using gfx::IntPoint;
using gfx::IntRect;
using gfx::IntRegion;
using gfx::IntSize;
using gfx::Matrix4x4;
using gfx::SurfaceFormat;
using gl::GLContext;
#ifdef XP_MACOSX
using gl::GLContextCGL;
#endif

static void EmitTelemetryForVideoLowPower(VideoLowPowerType aVideoLowPower) {
  switch (aVideoLowPower) {
    case VideoLowPowerType::NotVideo:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eNotvideo)
          .Add();
      return;

    case VideoLowPowerType::LowPower:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eLowpower)
          .Add();
      return;

    case VideoLowPowerType::FailMultipleVideo:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailmultiplevideo)
          .Add();
      return;

    case VideoLowPowerType::FailWindowed:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailwindowed)
          .Add();
      return;

    case VideoLowPowerType::FailOverlaid:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailoverlaid)
          .Add();
      return;

    case VideoLowPowerType::FailBacking:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailbacking)
          .Add();
      return;

    case VideoLowPowerType::FailMacOSVersion:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailmacosversion)
          .Add();
      return;

    case VideoLowPowerType::FailPref:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailpref)
          .Add();
      return;

    case VideoLowPowerType::FailSurface:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailsurface)
          .Add();
      return;

    case VideoLowPowerType::FailEnqueue:
      glean::gfx::macos_video_low_power
          .EnumGet(glean::gfx::MacosVideoLowPowerLabel::eFailenqueue)
          .Add();
      return;
  }
}

// Utility classes for NativeLayerRootSnapshotter (NLRS) profiler screenshots.

class RenderSourceNLRS : public profiler_screenshots::RenderSource {
 public:
  explicit RenderSourceNLRS(UniquePtr<gl::MozFramebuffer>&& aFramebuffer)
      : RenderSource(aFramebuffer->mSize),
        mFramebuffer(std::move(aFramebuffer)) {}
  auto& FB() { return *mFramebuffer; }

 protected:
  UniquePtr<gl::MozFramebuffer> mFramebuffer;
};

class DownscaleTargetNLRS : public profiler_screenshots::DownscaleTarget {
 public:
  DownscaleTargetNLRS(gl::GLContext* aGL,
                      UniquePtr<gl::MozFramebuffer>&& aFramebuffer)
      : profiler_screenshots::DownscaleTarget(aFramebuffer->mSize),
        mGL(aGL),
        mRenderSource(new RenderSourceNLRS(std::move(aFramebuffer))) {}
  already_AddRefed<profiler_screenshots::RenderSource> AsRenderSource()
      override {
    return do_AddRef(mRenderSource);
  };
  bool DownscaleFrom(profiler_screenshots::RenderSource* aSource,
                     const IntRect& aSourceRect,
                     const IntRect& aDestRect) override;

 protected:
  RefPtr<gl::GLContext> mGL;
  RefPtr<RenderSourceNLRS> mRenderSource;
};

class AsyncReadbackBufferNLRS
    : public profiler_screenshots::AsyncReadbackBuffer {
 public:
  AsyncReadbackBufferNLRS(gl::GLContext* aGL, const IntSize& aSize,
                          GLuint aBufferHandle)
      : profiler_screenshots::AsyncReadbackBuffer(aSize),
        mGL(aGL),
        mBufferHandle(aBufferHandle) {}
  void CopyFrom(profiler_screenshots::RenderSource* aSource) override;
  bool MapAndCopyInto(DataSourceSurface* aSurface,
                      const IntSize& aReadSize) override;

 protected:
  virtual ~AsyncReadbackBufferNLRS();
  RefPtr<gl::GLContext> mGL;
  GLuint mBufferHandle = 0;
};

// Needs to be on the stack whenever CALayer mutations are performed.
// (Mutating CALayers outside of a transaction can result in permanently stuck
// rendering, because such mutations create an implicit transaction which never
// auto-commits if the current thread does not have a native runloop.) Uses
// NSAnimationContext, which wraps CATransaction with additional off-main-thread
// protection, see bug 1585523.
struct MOZ_STACK_CLASS AutoCATransaction final {
  AutoCATransaction() {
#ifdef XP_MACOSX
    [NSAnimationContext beginGrouping];
#else
    [CATransaction begin];
#endif
    // By default, mutating a CALayer property triggers an animation which
    // smoothly transitions the property to the new value. We don't need these
    // animations, and this call turns them off:
    [CATransaction setDisableActions:YES];
  }
  ~AutoCATransaction() {
#ifdef XP_MACOSX
    [NSAnimationContext endGrouping];
#else
    [CATransaction commit];
#endif
  }
};

/* static */ already_AddRefed<NativeLayerRootCA>
NativeLayerRootCA::CreateForCALayer(CALayer* aLayer) {
  RefPtr<NativeLayerRootCA> layerRoot = new NativeLayerRootCA(aLayer);
  return layerRoot.forget();
}

// Returns an autoreleased CALayer* object.
static CALayer* MakeOffscreenRootCALayer() {
  // This layer should behave similarly to the backing layer of a flipped
  // NSView. It will never be rendered on the screen and it will never be
  // attached to an NSView's layer; instead, it will be the root layer of a
  // "local" CAContext. Setting geometryFlipped to YES causes the orientation of
  // descendant CALayers' contents (such as IOSurfaces) to be consistent with
  // what happens in a layer subtree that is attached to a flipped NSView.
  // Setting it to NO would cause the surfaces in individual leaf layers to
  // render upside down (rather than just flipping the entire layer tree upside
  // down).
  AutoCATransaction transaction;
  CALayer* layer = [CALayer layer];
  layer.position = CGPointZero;
  layer.bounds = CGRectZero;
  layer.anchorPoint = CGPointZero;
  layer.contentsGravity = kCAGravityTopLeft;
  layer.masksToBounds = YES;
  layer.geometryFlipped = YES;
  return layer;
}

NativeLayerRootCA::NativeLayerRootCA(CALayer* aLayer)
    : mMutex("NativeLayerRootCA"),
      mOnscreenRepresentation(aLayer),
      mOffscreenRepresentation(MakeOffscreenRootCALayer()) {}

NativeLayerRootCA::~NativeLayerRootCA() {
  MOZ_RELEASE_ASSERT(
      mSublayers.IsEmpty(),
      "Please clear all layers before destroying the layer root.");
}

already_AddRefed<NativeLayer> NativeLayerRootCA::CreateLayer(
    const IntSize& aSize, bool aIsOpaque,
    SurfacePoolHandle* aSurfacePoolHandle) {
  RefPtr<NativeLayer> layer = new NativeLayerCA(
      aSize, aIsOpaque, aSurfacePoolHandle->AsSurfacePoolHandleCA());
  return layer.forget();
}

already_AddRefed<NativeLayer> NativeLayerRootCA::CreateLayerForExternalTexture(
    bool aIsOpaque) {
  RefPtr<NativeLayer> layer = new NativeLayerCA(aIsOpaque);
  return layer.forget();
}

already_AddRefed<NativeLayer> NativeLayerRootCA::CreateLayerForColor(
    gfx::DeviceColor aColor) {
  RefPtr<NativeLayer> layer = new NativeLayerCA(aColor);
  return layer.forget();
}

already_AddRefed<NativeLayerCA>
NativeLayerRootCA::CreateLayerForSurfacePresentation(const IntSize& aSize,
                                                     bool aIsOpaque) {
  RefPtr<NativeLayerCA> layer = new NativeLayerCA(aSize, aIsOpaque);
  return layer.forget();
}

void NativeLayerRootCA::AppendLayer(NativeLayer* aLayer) {
  MutexAutoLock lock(mMutex);

  RefPtr<NativeLayerCA> layerCA = aLayer->AsNativeLayerCA();
  MOZ_RELEASE_ASSERT(layerCA);

  mSublayers.AppendElement(layerCA);
  layerCA->SetBackingScale(mBackingScale);
  layerCA->SetRootWindowIsFullscreen(mWindowIsFullscreen);
  ForAllRepresentations(
      [&](Representation& r) { r.mMutatedLayerStructure = true; });
}

void NativeLayerRootCA::RemoveLayer(NativeLayer* aLayer) {
  MutexAutoLock lock(mMutex);

  RefPtr<NativeLayerCA> layerCA = aLayer->AsNativeLayerCA();
  MOZ_RELEASE_ASSERT(layerCA);

  mSublayers.RemoveElement(layerCA);
  ForAllRepresentations(
      [&](Representation& r) { r.mMutatedLayerStructure = true; });
}

void NativeLayerRootCA::SetLayers(
    const nsTArray<RefPtr<NativeLayer>>& aLayers) {
  MutexAutoLock lock(mMutex);

  // Ideally, we'd just be able to do mSublayers = std::move(aLayers).
  // However, aLayers has a different type: it carries NativeLayer objects,
  // whereas mSublayers carries NativeLayerCA objects, so we have to downcast
  // all the elements first. There's one other reason to look at all the
  // elements in aLayers first: We need to make sure any new layers know about
  // our current backing scale.

  nsTArray<RefPtr<NativeLayerCA>> layersCA(aLayers.Length());
  for (auto& layer : aLayers) {
    RefPtr<NativeLayerCA> layerCA = layer->AsNativeLayerCA();
    MOZ_RELEASE_ASSERT(layerCA);
    layerCA->SetBackingScale(mBackingScale);
    layerCA->SetRootWindowIsFullscreen(mWindowIsFullscreen);
    layersCA.AppendElement(std::move(layerCA));
  }

  if (layersCA != mSublayers) {
    mSublayers = std::move(layersCA);
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedLayerStructure = true; });
  }
}

void NativeLayerRootCA::SetBackingScale(float aBackingScale) {
  MutexAutoLock lock(mMutex);

  mBackingScale = aBackingScale;
  for (auto layer : mSublayers) {
    layer->SetBackingScale(aBackingScale);
  }
}

float NativeLayerRootCA::BackingScale() {
  MutexAutoLock lock(mMutex);
  return mBackingScale;
}

void NativeLayerRootCA::SuspendOffMainThreadCommits() {
  MutexAutoLock lock(mMutex);
  mOffMainThreadCommitsSuspended = true;
}

bool NativeLayerRootCA::UnsuspendOffMainThreadCommits() {
  MutexAutoLock lock(mMutex);
  mOffMainThreadCommitsSuspended = false;
  return mCommitPending;
}

bool NativeLayerRootCA::AreOffMainThreadCommitsSuspended() {
  MutexAutoLock lock(mMutex);
  return mOffMainThreadCommitsSuspended;
}

bool NativeLayerRootCA::CommitToScreen() {
  {
    MutexAutoLock lock(mMutex);

    if (!NS_IsMainThread() && mOffMainThreadCommitsSuspended) {
      mCommitPending = true;
      return false;
    }

    mOnscreenRepresentation.Commit(WhichRepresentation::ONSCREEN, mSublayers,
                                   mWindowIsFullscreen);

    mCommitPending = false;

    if (StaticPrefs::gfx_webrender_debug_dump_native_layer_tree_to_file()) {
      static uint32_t sFrameID = 0;
      uint32_t frameID = sFrameID++;

      NSString* dirPath =
          [NSString stringWithFormat:@"%@/Desktop/nativelayerdumps-%d",
                                     NSHomeDirectory(), getpid()];
      if ([NSFileManager.defaultManager createDirectoryAtPath:dirPath
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nullptr]) {
        NSString* filename =
            [NSString stringWithFormat:@"frame-%d.html", frameID];
        NSString* filePath = [dirPath stringByAppendingPathComponent:filename];
        DumpLayerTreeToFile([filePath UTF8String], lock);
      } else {
        NSLog(@"Failed to create directory %@", dirPath);
      }
    }

    // Decide if we are going to emit telemetry about video low power on this
    // commit.
    static const int32_t TELEMETRY_COMMIT_PERIOD =
        StaticPrefs::gfx_core_animation_low_power_telemetry_frames_AtStartup();
    mTelemetryCommitCount =
        (mTelemetryCommitCount + 1) % TELEMETRY_COMMIT_PERIOD;
    if (mTelemetryCommitCount == 0) {
      // Figure out if we are hitting video low power mode.
      VideoLowPowerType videoLowPower = CheckVideoLowPower(lock);
      EmitTelemetryForVideoLowPower(videoLowPower);
    }
  }

  return true;
}

UniquePtr<NativeLayerRootSnapshotter> NativeLayerRootCA::CreateSnapshotter() {
#ifdef XP_MACOSX
  MutexAutoLock lock(mMutex);
  MOZ_RELEASE_ASSERT(!mWeakSnapshotter,
                     "No NativeLayerRootSnapshotter for this NativeLayerRoot "
                     "should exist when this is called");

  auto cr = NativeLayerRootSnapshotterCA::Create(
      this, mOffscreenRepresentation.mRootCALayer);
  if (cr) {
    mWeakSnapshotter = cr.get();
  }
  return cr;
#else
  return nullptr;
#endif
}

#ifdef XP_MACOSX
void NativeLayerRootCA::OnNativeLayerRootSnapshotterDestroyed(
    NativeLayerRootSnapshotterCA* aNativeLayerRootSnapshotter) {
  MutexAutoLock lock(mMutex);
  MOZ_RELEASE_ASSERT(mWeakSnapshotter == aNativeLayerRootSnapshotter);
  mWeakSnapshotter = nullptr;
}
#endif

void NativeLayerRootCA::CommitOffscreen() {
  MutexAutoLock lock(mMutex);
  mOffscreenRepresentation.Commit(WhichRepresentation::OFFSCREEN, mSublayers,
                                  mWindowIsFullscreen);
}

template <typename F>
void NativeLayerRootCA::ForAllRepresentations(F aFn) {
  aFn(mOnscreenRepresentation);
  aFn(mOffscreenRepresentation);
}

NativeLayerRootCA::Representation::Representation(CALayer* aRootCALayer)
    : mRootCALayer([aRootCALayer retain]) {}

NativeLayerRootCA::Representation::~Representation() {
  if (mMutatedLayerStructure) {
    // Clear the root layer's sublayers. At this point the window is usually
    // closed, so this transaction does not cause any screen updates.
    AutoCATransaction transaction;
    mRootCALayer.sublayers = @[];
  }

  [mRootCALayer release];
}

void NativeLayerRootCA::Representation::Commit(
    WhichRepresentation aRepresentation,
    const nsTArray<RefPtr<NativeLayerCA>>& aSublayers,
    bool aWindowIsFullscreen) {
  bool mustRebuild = mMutatedLayerStructure;
  if (!mustRebuild) {
    // Check which type of update we need to do, if any.
    NativeLayerCA::UpdateType updateRequired = NativeLayerCA::UpdateType::None;

    for (auto layer : aSublayers) {
      // Use the ordering of our UpdateType enums to build a maximal update
      // type.
      updateRequired =
          std::max(updateRequired, layer->HasUpdate(aRepresentation));
      if (updateRequired == NativeLayerCA::UpdateType::All) {
        break;
      }
    }

    if (updateRequired == NativeLayerCA::UpdateType::None) {
      // Nothing more needed, so early exit.
      return;
    }

    if (updateRequired == NativeLayerCA::UpdateType::OnlyVideo) {
      bool allUpdatesSucceeded = std::all_of(
          aSublayers.begin(), aSublayers.end(),
          [=](const RefPtr<NativeLayerCA>& layer) {
            return layer->ApplyChanges(aRepresentation,
                                       NativeLayerCA::UpdateType::OnlyVideo);
          });

      if (allUpdatesSucceeded) {
        // Nothing more needed, so early exit;
        return;
      }
    }
  }

  // We're going to do a full update now, which requires a transaction. Update
  // all of the sublayers. Afterwards, only continue processing the sublayers
  // which have an extent.
  AutoCATransaction transaction;
  nsTArray<NativeLayerCA*> sublayersWithExtent;
  for (auto layer : aSublayers) {
    mustRebuild |= layer->WillUpdateAffectLayers(aRepresentation);
    layer->ApplyChanges(aRepresentation, NativeLayerCA::UpdateType::All);
    CALayer* caLayer = layer->UnderlyingCALayer(aRepresentation);
    if (!caLayer.masksToBounds || !CGRectIsEmpty(caLayer.bounds)) {
      // This layer has an extent. If it didn't before, we need to rebuild.
      mustRebuild |= !layer->HasExtent();
      layer->SetHasExtent(true);
      sublayersWithExtent.AppendElement(layer);
    } else {
      // This layer has no extent. If it did before, we need to rebuild.
      mustRebuild |= layer->HasExtent();
      layer->SetHasExtent(false);
    }

    // One other reason we may need to rebuild is if the caLayer is not part of
    // the root layer's sublayers. This might happen if the caLayer was rebuilt.
    // We construct this check in a way that maximizes the boolean
    // short-circuit, because we don't want to call containsObject unless
    // absolutely necessary.
    mustRebuild =
        mustRebuild || ![mRootCALayer.sublayers containsObject:caLayer];
  }

  if (mustRebuild) {
    uint32_t sublayersCount = sublayersWithExtent.Length();
    NSMutableArray<CALayer*>* sublayers =
        [NSMutableArray arrayWithCapacity:sublayersCount];
    for (auto layer : sublayersWithExtent) {
      [sublayers addObject:layer->UnderlyingCALayer(aRepresentation)];
    }
    mRootCALayer.sublayers = sublayers;
  }

  mMutatedLayerStructure = false;
}

#ifdef XP_MACOSX
/* static */ UniquePtr<NativeLayerRootSnapshotterCA>
NativeLayerRootSnapshotterCA::Create(NativeLayerRootCA* aLayerRoot,
                                     CALayer* aRootCALayer) {
  if (NS_IsMainThread()) {
    // Disallow creating snapshotters on the main thread.
    // On the main thread, any explicit CATransaction / NSAnimationContext is
    // nested within a global implicit transaction. This makes it impossible to
    // apply CALayer mutations synchronously such that they become visible to
    // CARenderer. As a result, the snapshotter would not capture the right
    // output on the main thread.
    return nullptr;
  }

  nsCString failureUnused;
  RefPtr<gl::GLContext> gl = gl::GLContextProvider::CreateHeadless(
      {gl::CreateContextFlags::ALLOW_OFFLINE_RENDERER |
       gl::CreateContextFlags::REQUIRE_COMPAT_PROFILE},
      &failureUnused);
  if (!gl) {
    return nullptr;
  }

  return UniquePtr<NativeLayerRootSnapshotterCA>(
      new NativeLayerRootSnapshotterCA(aLayerRoot, std::move(gl),
                                       aRootCALayer));
}
#endif

void NativeLayerRootCA::DumpLayerTreeToFile(const char* aPath,
                                            const MutexAutoLock& aProofOfLock) {
  NSLog(@"Dumping NativeLayer contents to %s", aPath);
  std::ofstream fileOutput(aPath);
  if (fileOutput.fail()) {
    NSLog(@"Opening %s for writing failed.", aPath);
  }

  // Make sure floating point values use a period for the decimal separator.
  fileOutput.imbue(std::locale("C"));

  fileOutput << "<html>\n";
  for (const auto& layer : mSublayers) {
    layer->DumpLayer(fileOutput);
  }
  fileOutput << "</html>\n";
  fileOutput.close();
}

void NativeLayerRootCA::SetWindowIsFullscreen(bool aFullscreen) {
  MutexAutoLock lock(mMutex);

  if (mWindowIsFullscreen != aFullscreen) {
    mWindowIsFullscreen = aFullscreen;

    for (auto layer : mSublayers) {
      layer->SetRootWindowIsFullscreen(mWindowIsFullscreen);
    }
  }
}

/* static */ bool IsCGColorOpaqueBlack(CGColorRef aColor) {
  if (CGColorEqualToColor(aColor, CGColorGetConstantColor(kCGColorBlack))) {
    return true;
  }
  size_t componentCount = CGColorGetNumberOfComponents(aColor);
  if (componentCount == 0) {
    // This will happen if aColor is kCGColorClear. It's not opaque black.
    return false;
  }

  const CGFloat* components = CGColorGetComponents(aColor);
  for (size_t c = 0; c < componentCount - 1; ++c) {
    if (components[c] > 0.0f) {
      return false;
    }
  }
  return components[componentCount - 1] >= 1.0f;
}

VideoLowPowerType NativeLayerRootCA::CheckVideoLowPower(
    const MutexAutoLock& aProofOfLock) {
  // This deteremines whether the current layer contents qualify for the
  // macOS Core Animation video low power mode. Those requirements are
  // summarized at
  // https://developer.apple.com/documentation/webkit/delivering_video_content_for_safari
  // and we verify them by checking:
  // 1) There must be exactly one video showing.
  // 2) The topmost CALayer must be a AVSampleBufferDisplayLayer.
  // 3) The video layer must be showing a buffer encoded in one of the
  //    kCVPixelFormatType_420YpCbCr pixel formats.
  // 4) The layer below that must cover the entire screen and have a black
  //    background color.
  // 5) The window must be fullscreen.
  // This function checks these requirements empirically. If one of the checks
  // fail, we either return immediately or do additional processing to
  // determine more detail.

  uint32_t videoLayerCount = 0;
  NativeLayerCA* topLayer = nullptr;
  CALayer* topCALayer = nil;
  CALayer* secondCALayer = nil;
  bool topLayerIsVideo = false;

  for (auto layer : mSublayers) {
    // Only layers with extent are contributing to our sublayers.
    if (layer->HasExtent()) {
      topLayer = layer;

      secondCALayer = topCALayer;
      topCALayer = topLayer->UnderlyingCALayer(WhichRepresentation::ONSCREEN);
      topLayerIsVideo = topLayer->IsVideo(aProofOfLock);
      if (topLayerIsVideo) {
        ++videoLayerCount;
      }
    }
  }

  if (videoLayerCount == 0) {
    return VideoLowPowerType::NotVideo;
  }

  // Most importantly, check if the window is fullscreen. If the user is
  // watching video in a window, then all of the other enums are irrelevant to
  // achieving the low power mode.
  if (!mWindowIsFullscreen) {
    return VideoLowPowerType::FailWindowed;
  }

  if (videoLayerCount > 1) {
    return VideoLowPowerType::FailMultipleVideo;
  }

  if (!topLayerIsVideo) {
    return VideoLowPowerType::FailOverlaid;
  }

  if (!secondCALayer || !IsCGColorOpaqueBlack(secondCALayer.backgroundColor) ||
      !CGRectContainsRect(secondCALayer.frame,
                          secondCALayer.superlayer.bounds)) {
    return VideoLowPowerType::FailBacking;
  }

  CALayer* topContentCALayer = topCALayer.sublayers[0];
  if (![topContentCALayer isKindOfClass:[AVSampleBufferDisplayLayer class]]) {
    // We didn't create a AVSampleBufferDisplayLayer for the top video layer.
    // Try to figure out why by following some of the logic in
    // NativeLayerCA::ShouldSpecializeVideo.

    if (!StaticPrefs::gfx_core_animation_specialize_video()) {
      return VideoLowPowerType::FailPref;
    }

    // The only remaining reason is that the surface wasn't eligible. We
    // assert this instead of if-ing it, to ensure that we always have a
    // return value from this clause.
#ifdef DEBUG
    CFTypeRefPtr<IOSurfaceRef> surface;
    if (auto textureHost = topLayer->mTextureHost) {
      MacIOSurface* macIOSurface = textureHost->GetSurface();
      surface = macIOSurface->GetIOSurfaceRef();
    } else {
      surface = topLayer->mSurfaceToPresent;
    }
    MOZ_ASSERT(surface);
    OSType pixelFormat = IOSurfaceGetPixelFormat(surface.get());
    MOZ_ASSERT(
        !(pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
          pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
          pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
          pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange));
#endif
    return VideoLowPowerType::FailSurface;
  }

  AVSampleBufferDisplayLayer* topVideoLayer =
      (AVSampleBufferDisplayLayer*)topContentCALayer;
  if (topVideoLayer.status != AVQueuedSampleBufferRenderingStatusRendering) {
    return VideoLowPowerType::FailEnqueue;
  }

  // As best we can tell, we're eligible for video low power mode. Hurrah!
  return VideoLowPowerType::LowPower;
}

#ifdef XP_MACOSX
NativeLayerRootSnapshotterCA::NativeLayerRootSnapshotterCA(
    NativeLayerRootCA* aLayerRoot, RefPtr<GLContext>&& aGL,
    CALayer* aRootCALayer)
    : mLayerRoot(aLayerRoot), mGL(aGL) {
  AutoCATransaction transaction;
  mRenderer = [[CARenderer
      rendererWithCGLContext:gl::GLContextCGL::Cast(mGL)->GetCGLContext()
                     options:nil] retain];
  mRenderer.layer = aRootCALayer;
}

NativeLayerRootSnapshotterCA::~NativeLayerRootSnapshotterCA() {
  mLayerRoot->OnNativeLayerRootSnapshotterDestroyed(this);
  [mRenderer release];
}

already_AddRefed<profiler_screenshots::RenderSource>
NativeLayerRootSnapshotterCA::GetWindowContents(const IntSize& aWindowSize) {
  UpdateSnapshot(aWindowSize);
  return do_AddRef(mSnapshot);
}

void NativeLayerRootSnapshotterCA::UpdateSnapshot(const IntSize& aSize) {
  CGRect bounds = CGRectMake(0, 0, aSize.width, aSize.height);

  {
    // Set the correct bounds and scale on the renderer and its root layer.
    // CARenderer always renders at unit scale, i.e. the coordinates on the root
    // layer must map 1:1 to render target pixels. But the coordinates on our
    // content layers are in "points", where 1 point maps to 2 device pixels on
    // HiDPI. So in order to render at the full device pixel resolution, we set
    // a scale transform on the root offscreen layer.
    AutoCATransaction transaction;
    mRenderer.layer.bounds = bounds;
    float scale = mLayerRoot->BackingScale();
    mRenderer.layer.sublayerTransform = CATransform3DMakeScale(scale, scale, 1);
    mRenderer.bounds = bounds;
  }

  mLayerRoot->CommitOffscreen();

  mGL->MakeCurrent();

  bool needToRedrawEverything = false;
  if (!mSnapshot || mSnapshot->Size() != aSize) {
    mSnapshot = nullptr;
    auto fb = gl::MozFramebuffer::Create(mGL, aSize, 0, false);
    if (!fb) {
      return;
    }
    mSnapshot = new RenderSourceNLRS(std::move(fb));
    needToRedrawEverything = true;
  }

  const gl::ScopedBindFramebuffer bindFB(mGL, mSnapshot->FB().mFB);
  mGL->fViewport(0.0, 0.0, aSize.width, aSize.height);

  // These legacy OpenGL function calls are part of CARenderer's API contract,
  // see CARenderer.h. The size passed to glOrtho must be the device pixel size
  // of the render target, otherwise CARenderer will produce incorrect results.
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0, aSize.width, 0.0, aSize.height, -1, 1);

  float mediaTime = CACurrentMediaTime();
  [mRenderer beginFrameAtTime:mediaTime timeStamp:nullptr];
  if (needToRedrawEverything) {
    [mRenderer addUpdateRect:bounds];
  }
  if (!CGRectIsEmpty([mRenderer updateBounds])) {
    // CARenderer assumes the layer tree is opaque. It only ever paints over
    // existing content, it never erases anything. However, our layer tree is
    // not necessarily opaque. So we manually erase the area that's going to be
    // redrawn. This ensures correct rendering in the transparent areas.
    //
    // Since we erase the bounds of the update area, this will erase more than
    // necessary if the update area is not a single rectangle. Unfortunately we
    // cannot get the precise update region from CARenderer, we can only get the
    // bounds.
    CGRect updateBounds = [mRenderer updateBounds];
    gl::ScopedGLState scopedScissorTestState(mGL, LOCAL_GL_SCISSOR_TEST, true);
    gl::ScopedScissorRect scissor(
        mGL, updateBounds.origin.x, updateBounds.origin.y,
        updateBounds.size.width, updateBounds.size.height);
    mGL->fClearColor(0.0, 0.0, 0.0, 0.0);
    mGL->fClear(LOCAL_GL_COLOR_BUFFER_BIT);
    // We erased the update region's bounds. Make sure the entire update bounds
    // get repainted.
    [mRenderer addUpdateRect:updateBounds];
  }
  [mRenderer render];
  [mRenderer endFrame];
}

bool NativeLayerRootSnapshotterCA::ReadbackPixels(
    const IntSize& aReadbackSize, SurfaceFormat aReadbackFormat,
    const Range<uint8_t>& aReadbackBuffer) {
  if (aReadbackFormat != SurfaceFormat::B8G8R8A8) {
    return false;
  }

  UpdateSnapshot(aReadbackSize);
  if (!mSnapshot) {
    return false;
  }

  const gl::ScopedBindFramebuffer bindFB(mGL, mSnapshot->FB().mFB);
  gl::ScopedPackState safePackState(mGL);
  mGL->fReadPixels(0.0f, 0.0f, aReadbackSize.width, aReadbackSize.height,
                   LOCAL_GL_BGRA, LOCAL_GL_UNSIGNED_BYTE, &aReadbackBuffer[0]);

  return true;
}

already_AddRefed<profiler_screenshots::DownscaleTarget>
NativeLayerRootSnapshotterCA::CreateDownscaleTarget(const IntSize& aSize) {
  auto fb = gl::MozFramebuffer::Create(mGL, aSize, 0, false);
  if (!fb) {
    return nullptr;
  }
  RefPtr<profiler_screenshots::DownscaleTarget> dt =
      new DownscaleTargetNLRS(mGL, std::move(fb));
  return dt.forget();
}

already_AddRefed<profiler_screenshots::AsyncReadbackBuffer>
NativeLayerRootSnapshotterCA::CreateAsyncReadbackBuffer(const IntSize& aSize) {
  size_t bufferByteCount = aSize.width * aSize.height * 4;
  GLuint bufferHandle = 0;
  mGL->fGenBuffers(1, &bufferHandle);

  gl::ScopedPackState scopedPackState(mGL);
  mGL->fBindBuffer(LOCAL_GL_PIXEL_PACK_BUFFER, bufferHandle);
  mGL->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 1);
  mGL->fBufferData(LOCAL_GL_PIXEL_PACK_BUFFER, bufferByteCount, nullptr,
                   LOCAL_GL_STREAM_READ);
  return MakeAndAddRef<AsyncReadbackBufferNLRS>(mGL, aSize, bufferHandle);
}
#endif

NativeLayerCA::NativeLayerCA(const IntSize& aSize, bool aIsOpaque,
                             SurfacePoolHandleCA* aSurfacePoolHandle)
    : mMutex("NativeLayerCA"), mIsOpaque(aIsOpaque) {
  // We need a surface handler for this type of layer.
  mSurfaceHandler.emplace(aSize, aSurfacePoolHandle);
}

NativeLayerCA::NativeLayerCA(bool aIsOpaque)
    : mMutex("NativeLayerCA"), mIsOpaque(aIsOpaque) {
#ifdef NIGHTLY_BUILD
  if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
    NSLog(@"VIDEO_LOG: NativeLayerCA: %p is being created to host an external "
          @"image, which may force a video layer rebuild.",
          this);
  }
#endif
}

CGColorRef CGColorCreateForDeviceColor(gfx::DeviceColor aColor) {
  if (StaticPrefs::gfx_color_management_native_srgb()) {
    return CGColorCreateSRGB(aColor.r, aColor.g, aColor.b, aColor.a);
  }

  return CGColorCreateGenericRGB(aColor.r, aColor.g, aColor.b, aColor.a);
}

NativeLayerCA::NativeLayerCA(gfx::DeviceColor aColor)
    : mMutex("NativeLayerCA"), mIsOpaque(aColor.a >= 1.0f) {
  MOZ_ASSERT(aColor.a > 0.0f, "Can't handle a fully transparent backdrop.");
  mColor.AssignUnderCreateRule(CGColorCreateForDeviceColor(aColor));
}

NativeLayerCA::NativeLayerCA(const IntSize& aSize, bool aIsOpaque)
    : mMutex("NativeLayerCA"), mSize(aSize), mIsOpaque(aIsOpaque) {}

NativeLayerCA::~NativeLayerCA() {
#ifdef NIGHTLY_BUILD
  if (mHasEverAttachExternalImage &&
      StaticPrefs::gfx_core_animation_specialize_video_log()) {
    NSLog(@"VIDEO_LOG: ~NativeLayerCA: %p is being destroyed after hosting "
          @"an external image.",
          this);
  }
#endif
}

void NativeLayerCA::AttachExternalImage(wr::RenderTextureHost* aExternalImage) {
  MutexAutoLock lock(mMutex);

#ifdef NIGHTLY_BUILD
  mHasEverAttachExternalImage = true;
  MOZ_RELEASE_ASSERT(!mHasEverNotifySurfaceReady,
                     "Shouldn't change layer type to external.");
#endif

  MOZ_ASSERT(!mSurfaceHandler,
             "Shouldn't have a surface handler for external images.");

  wr::RenderMacIOSurfaceTextureHost* texture =
      aExternalImage->AsRenderMacIOSurfaceTextureHost();
  MOZ_ASSERT(texture);
  mTextureHost = texture;
  if (!mTextureHost) {
    gfxCriticalNoteOnce << "ExternalImage is not RenderMacIOSurfaceTextureHost";
    return;
  }

  // Determine if TextureHost is a video surface.
  mTextureHostIsVideo = gfx::Info(mTextureHost->GetFormat())->isYuv;

  gfx::IntSize oldSize = mSize;
  mSize = texture->GetSize(0);
  bool changedSizeAndDisplayRect = (mSize != oldSize);

  mDisplayRect = IntRect(IntPoint{}, mSize);

  bool isDRM = aExternalImage->IsFromDRMSource();
  bool changedIsDRM = (mIsDRM != isDRM);
  mIsDRM = isDRM;

  bool isHDR = false;
  MacIOSurface* macIOSurface = texture->GetSurface();
  if (macIOSurface->GetYUVColorSpace() == gfx::YUVColorSpace::BT2020) {
    // BT2020 colorSpace is a signifier of HDR.
    isHDR = true;
  }

  if (macIOSurface->GetColorDepth() == gfx::ColorDepth::COLOR_10) {
    // 10-bit color is a signifier of HDR.
    isHDR = true;
  }
  mIsHDR = isHDR;

  bool specializeVideo = ShouldSpecializeVideo(lock);
  bool changedSpecializeVideo = (mSpecializeVideo != specializeVideo);
  mSpecializeVideo = specializeVideo;

#ifdef NIGHTLY_BUILD
  if (changedSpecializeVideo &&
      StaticPrefs::gfx_core_animation_specialize_video_log()) {
    NSLog(
        @"VIDEO_LOG: AttachExternalImage: %p is forcing a video layer rebuild.",
        this);
  }
#endif

  ForAllRepresentations([&](Representation& r) {
    r.mMutatedFrontSurface = true;
    r.mMutatedDisplayRect |= changedSizeAndDisplayRect;
    r.mMutatedSize |= changedSizeAndDisplayRect;
    r.mMutatedSpecializeVideo |= changedSpecializeVideo;
    r.mMutatedIsDRM |= changedIsDRM;
  });
}

GpuFence* NativeLayerCA::GetGpuFence() {
  if (!mTextureHost) {
    return nullptr;
  }

  wr::RenderMacIOSurfaceTextureHost* texture =
      mTextureHost->AsRenderMacIOSurfaceTextureHost();
  if (!texture) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    gfxCriticalNoteOnce << "ExternalImage is not RenderMacIOSurfaceTextureHost";
    return nullptr;
  }

  return texture->GetGpuFence();
}

bool NativeLayerCA::IsVideo(const MutexAutoLock& aProofOfLock) {
  return mTextureHostIsVideo;
}

bool NativeLayerCA::ShouldSpecializeVideo(const MutexAutoLock& aProofOfLock) {
  if (!IsVideo(aProofOfLock)) {
    // Only videos are eligible.
    return false;
  }

  // DRM video is supported in macOS 10.15 and beyond, and such video must use
  // a specialized video layer.
  if (mIsDRM) {
    return true;
  }

  if (mIsHDR) {
    return true;
  }

  // Beyond this point, we return true if-and-only-if we think we can achieve
  // the power-saving "detached mode" of the macOS compositor.

  if (!StaticPrefs::gfx_core_animation_specialize_video()) {
    // Pref must be set.
    return false;
  }

  // It will only detach if we're fullscreen.
  return mRootWindowIsFullscreen;
}

void NativeLayerCA::SetRootWindowIsFullscreen(bool aFullscreen) {
  if (mRootWindowIsFullscreen == aFullscreen) {
    return;
  }

  MutexAutoLock lock(mMutex);

  mRootWindowIsFullscreen = aFullscreen;

  bool oldSpecializeVideo = mSpecializeVideo;
  mSpecializeVideo = ShouldSpecializeVideo(lock);
  bool changedSpecializeVideo = (mSpecializeVideo != oldSpecializeVideo);

  if (changedSpecializeVideo) {
#ifdef NIGHTLY_BUILD
    if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
      NSLog(@"VIDEO_LOG: SetRootWindowIsFullscreen: %p is forcing a video "
            @"layer rebuild.",
            this);
    }
#endif

    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedSpecializeVideo = true; });
  }
}

void NativeLayerCA::SetSurfaceIsFlipped(bool aIsFlipped) {
  MutexAutoLock lock(mMutex);

  bool oldIsFlipped = mSurfaceIsFlipped;
  if (mSurfaceHandler) {
    oldIsFlipped = mSurfaceHandler->SurfaceIsFlipped();
    mSurfaceHandler->SetSurfaceIsFlipped(aIsFlipped);
  } else {
    mSurfaceIsFlipped = aIsFlipped;
  }

  if (aIsFlipped != oldIsFlipped) {
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedSurfaceIsFlipped = true; });
  }
}

bool NativeLayerCA::SurfaceIsFlipped() {
  MutexAutoLock lock(mMutex);
  if (mSurfaceHandler) {
    return mSurfaceHandler->SurfaceIsFlipped();
  }
  return mSurfaceIsFlipped;
}

IntSize NativeLayerCA::GetSize() {
  MutexAutoLock lock(mMutex);
  if (mSurfaceHandler) {
    return mSurfaceHandler->Size();
  }
  return mSize;
}

void NativeLayerCA::SetPosition(const IntPoint& aPosition) {
  MutexAutoLock lock(mMutex);

  if (aPosition != mPosition) {
    mPosition = aPosition;
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedPosition = true; });
  }
}

IntPoint NativeLayerCA::GetPosition() {
  MutexAutoLock lock(mMutex);
  return mPosition;
}

void NativeLayerCA::SetTransform(const Matrix4x4& aTransform) {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(aTransform.IsRectilinear());

  if (aTransform != mTransform) {
    mTransform = aTransform;
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedTransform = true; });
  }
}

void NativeLayerCA::SetSamplingFilter(gfx::SamplingFilter aSamplingFilter) {
  MutexAutoLock lock(mMutex);

  if (aSamplingFilter != mSamplingFilter) {
    mSamplingFilter = aSamplingFilter;
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedSamplingFilter = true; });
  }
}

Matrix4x4 NativeLayerCA::GetTransform() {
  MutexAutoLock lock(mMutex);
  return mTransform;
}

IntRect NativeLayerCA::GetRect() {
  MutexAutoLock lock(mMutex);
  IntSize size = mSize;
  if (mSurfaceHandler) {
    size = mSurfaceHandler->Size();
  }
  return IntRect(mPosition, size);
}

void NativeLayerCA::SetBackingScale(float aBackingScale) {
  MutexAutoLock lock(mMutex);

  if (aBackingScale != mBackingScale) {
    mBackingScale = aBackingScale;
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedBackingScale = true; });
  }
}

bool NativeLayerCA::IsOpaque() {
  // mIsOpaque is const, so no need for a lock.
  return mIsOpaque;
}

void NativeLayerCA::SetClipRect(const Maybe<gfx::IntRect>& aClipRect) {
  MutexAutoLock lock(mMutex);

  if (aClipRect != mClipRect) {
    mClipRect = aClipRect;
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedClipRect = true; });
  }
}

Maybe<gfx::IntRect> NativeLayerCA::ClipRect() {
  MutexAutoLock lock(mMutex);
  return mClipRect;
}

void NativeLayerCA::SetRoundedClipRect(const Maybe<gfx::RoundedRect>& aClip) {
  MutexAutoLock lock(mMutex);

  if (aClip != mRoundedClipRect) {
    mRoundedClipRect = aClip;
    ForAllRepresentations(
        [&](Representation& r) { r.mMutatedRoundedClipRect = true; });
  }
}

Maybe<gfx::RoundedRect> NativeLayerCA::RoundedClipRect() {
  MutexAutoLock lock(mMutex);
  return mRoundedClipRect;
}

void NativeLayerCA::DumpLayer(std::ostream& aOutputStream) {
  MutexAutoLock lock(mMutex);

  IntSize surfaceSize = mSize;
  IntRect displayRect = mDisplayRect;
  bool surfaceIsFlipped = mSurfaceIsFlipped;
  if (mSurfaceHandler) {
    surfaceSize = mSurfaceHandler->Size();
    displayRect = mSurfaceHandler->DisplayRect();
    surfaceIsFlipped = mSurfaceHandler->SurfaceIsFlipped();
  }

  Maybe<CGRect> scaledClipRect =
      CalculateClipGeometry(surfaceSize, mPosition, mTransform, displayRect,
                            mClipRect, mBackingScale);

  CGRect useClipRect;
  if (scaledClipRect.isSome()) {
    useClipRect = *scaledClipRect;
  } else {
    useClipRect = CGRectZero;
  }

  aOutputStream << "<div style=\"";
  aOutputStream << "position: absolute; ";
  aOutputStream << "left: " << useClipRect.origin.x << "px; ";
  aOutputStream << "top: " << useClipRect.origin.y << "px; ";
  aOutputStream << "width: " << useClipRect.size.width << "px; ";
  aOutputStream << "height: " << useClipRect.size.height << "px; ";

  if (scaledClipRect.isSome()) {
    aOutputStream << "overflow: hidden; ";
  }

  if (mColor) {
    const CGFloat* components = CGColorGetComponents(mColor.get());
    aOutputStream << "background: rgb(" << components[0] * 255.0f << " "
                  << components[1] * 255.0f << " " << components[2] * 255.0f
                  << "); opacity: " << components[3] << "; ";

    // That's all we need for color layers. We don't need to specify an image.
    aOutputStream << "\"/></div>\n";
    return;
  }

  aOutputStream << "\">";

  auto size = gfx::Size(surfaceSize) / mBackingScale;

  aOutputStream << "<img style=\"";
  aOutputStream << "width: " << size.width << "px; ";
  aOutputStream << "height: " << size.height << "px; ";

  if (mSamplingFilter == gfx::SamplingFilter::POINT) {
    aOutputStream << "image-rendering: crisp-edges; ";
  }

  Matrix4x4 transform = mTransform;
  transform.PreTranslate(mPosition.x, mPosition.y, 0);
  transform.PostTranslate((-useClipRect.origin.x * mBackingScale),
                          (-useClipRect.origin.y * mBackingScale), 0);

  if (surfaceIsFlipped) {
    transform.PreTranslate(0, surfaceSize.height, 0).PreScale(1, -1, 1);
  }

  if (!transform.IsIdentity()) {
    const auto& m = transform;
    aOutputStream << "transform-origin: top left; ";
    aOutputStream << "transform: matrix3d(";
    aOutputStream << m._11 << ", " << m._12 << ", " << m._13 << ", " << m._14
                  << ", ";
    aOutputStream << m._21 << ", " << m._22 << ", " << m._23 << ", " << m._24
                  << ", ";
    aOutputStream << m._31 << ", " << m._32 << ", " << m._33 << ", " << m._34
                  << ", ";
    aOutputStream << m._41 / mBackingScale << ", " << m._42 / mBackingScale
                  << ", " << m._43 << ", " << m._44;
    aOutputStream << "); ";
  }
  aOutputStream << "\" ";

  CFTypeRefPtr<IOSurfaceRef> surface;
  if (mSurfaceToPresent) {
    surface = mSurfaceToPresent;
    aOutputStream << "alt=\"presented surface 0x" << std::hex
                  << int(IOSurfaceGetID(surface.get())) << "\" ";
  } else if (mSurfaceHandler) {
    if (auto frontSurface = mSurfaceHandler->FrontSurface()) {
      surface = frontSurface->mSurface;
      aOutputStream << "alt=\"regular surface 0x" << std::hex
                    << int(IOSurfaceGetID(surface.get())) << "\" ";
    }
  } else if (mTextureHost) {
    surface = mTextureHost->GetSurface()->GetIOSurfaceRef();
    aOutputStream << "alt=\"TextureHost surface 0x" << std::hex
                  << int(IOSurfaceGetID(surface.get())) << "\" ";
  }
  if (!surface) {
    aOutputStream << "alt=\"no surface 0x\" ";
  }

  aOutputStream << "src=\"";

  if (surface) {
    // Attempt to render the surface as a PNG. Skia can do this for RGB
    // surfaces.
    RefPtr<MacIOSurface> surf = new MacIOSurface(surface);
    if (surf->Lock(true)) {
      SurfaceFormat format = surf->GetFormat();
      if (format == SurfaceFormat::B8G8R8A8 ||
          format == SurfaceFormat::B8G8R8X8) {
        RefPtr<gfx::DrawTarget> dt =
            surf->GetAsDrawTargetLocked(gfx::BackendType::SKIA);
        if (dt) {
          RefPtr<gfx::SourceSurface> sourceSurf = dt->Snapshot();
          nsCString dataUrl;
          gfxUtils::EncodeSourceSurface(sourceSurf, ImageType::PNG, u""_ns,
                                        gfxUtils::eDataURIEncode, nullptr,
                                        &dataUrl);
          aOutputStream << dataUrl.get();
        }
      }
      surf->Unlock(true);
    }
  }

  aOutputStream << "\"/></div>\n";
}

gfx::IntRect NativeLayerCA::CurrentSurfaceDisplayRect() {
  MutexAutoLock lock(mMutex);
  if (mSurfaceHandler) {
    return mSurfaceHandler->DisplayRect();
  }
  return mDisplayRect;
}

void NativeLayerCA::SetDisplayRect(const gfx::IntRect& aDisplayRect) {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(!mSurfaceHandler, "Setting display rect will have no effect.");
  mDisplayRect = aDisplayRect;
}

void NativeLayerCA::SetSurfaceToPresent(CFTypeRefPtr<IOSurfaceRef> aSurfaceRef,
                                        gfx::IntSize& aSize, bool aIsDRM,
                                        bool aIsHDR) {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(!mSurfaceHandler,
             "Shouldn't call this for layers that manage their own surfaces.");
  MOZ_ASSERT(!mTextureHost,
             "Shouldn't call this for layers that get external surfaces.");

  bool changedSurface = (mSurfaceToPresent != aSurfaceRef);
  mSurfaceToPresent = aSurfaceRef;

  bool changedSizeAndDisplayRect = (mSize != aSize);
  mSize = aSize;
  mDisplayRect = IntRect(IntPoint{}, mSize);

  // Figure out if the surface is a video.
  if (mSurfaceToPresent) {
    auto pixelFormat = IOSurfaceGetPixelFormat(mSurfaceToPresent.get());
    bool hasAlpha = !mIsOpaque;
    auto surfaceFormat =
        MacIOSurface::SurfaceFormatForPixelFormat(pixelFormat, hasAlpha);
    mTextureHostIsVideo = gfx::Info(surfaceFormat)->isYuv;
  } else {
    mTextureHostIsVideo = false;
  }

  bool changedIsDRM = (mIsDRM != aIsDRM);
  mIsDRM = aIsDRM;

  mIsHDR = aIsHDR;

  bool specializeVideo = ShouldSpecializeVideo(lock);
  bool changedSpecializeVideo = (mSpecializeVideo != specializeVideo);
  mSpecializeVideo = specializeVideo;

#ifdef NIGHTLY_BUILD
  if (changedSpecializeVideo &&
      StaticPrefs::gfx_core_animation_specialize_video_log()) {
    NSLog(
        @"VIDEO_LOG: SetSurfaceToPresent: %p is forcing a video layer rebuild.",
        this);
  }
#endif

  ForAllRepresentations([&](Representation& r) {
    r.mMutatedFrontSurface |= changedSurface;
    r.mMutatedSize |= changedSizeAndDisplayRect;
    r.mMutatedDisplayRect |= changedSizeAndDisplayRect;
    r.mMutatedSpecializeVideo |= changedSpecializeVideo;
    r.mMutatedIsDRM |= changedIsDRM;
  });
}

NativeLayerCA::Representation::Representation()
    : mMutatedPosition(true),
      mMutatedTransform(true),
      mMutatedDisplayRect(true),
      mMutatedClipRect(true),
      mMutatedRoundedClipRect(true),
      mMutatedBackingScale(true),
      mMutatedSize(true),
      mMutatedSurfaceIsFlipped(true),
      mMutatedFrontSurface(true),
      mMutatedSamplingFilter(true),
      mMutatedSpecializeVideo(true),
      mMutatedIsDRM(true) {}

NativeLayerCA::Representation::~Representation() {
  [mContentCALayer release];
  [mOpaquenessTintLayer release];
  [mWrappingCALayer release];
  [mRoundedClipCALayer release];
}

void NativeLayerCA::InvalidateRegionThroughoutSwapchain(
    const MutexAutoLock& aProofOfLock, const IntRegion& aRegion) {
  MOZ_ASSERT(mSurfaceHandler);
  mSurfaceHandler->InvalidateRegionThroughoutSwapchain(aRegion);
}

template <typename F>
void NativeLayerCA::HandlePartialUpdate(const MutexAutoLock& aProofOfLock,
                                        const IntRect& aDisplayRect,
                                        const IntRegion& aUpdateRegion,
                                        F&& aCopyFn) {
  MOZ_ASSERT(mSurfaceHandler);
  mSurfaceHandler->HandlePartialUpdate(aDisplayRect, aUpdateRegion, aCopyFn);
}

RefPtr<gfx::DrawTarget> NativeLayerCA::NextSurfaceAsDrawTarget(
    const IntRect& aDisplayRect, const IntRegion& aUpdateRegion,
    gfx::BackendType aBackendType) {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(mSurfaceHandler);
  return mSurfaceHandler->NextSurfaceAsDrawTarget(aDisplayRect, aUpdateRegion,
                                                  aBackendType);
}

Maybe<GLuint> NativeLayerCA::NextSurfaceAsFramebuffer(
    const IntRect& aDisplayRect, const IntRegion& aUpdateRegion,
    bool aNeedsDepth) {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(mSurfaceHandler);
  return mSurfaceHandler->NextSurfaceAsFramebuffer(aDisplayRect, aUpdateRegion,
                                                   aNeedsDepth);
}

void NativeLayerCA::NotifySurfaceReady() {
  MutexAutoLock lock(mMutex);

#ifdef NIGHTLY_BUILD
  mHasEverNotifySurfaceReady = true;
  MOZ_RELEASE_ASSERT(!mHasEverAttachExternalImage,
                     "Shouldn't change layer type to drawn.");
#endif

  MOZ_ASSERT(mSurfaceHandler);
  bool mutatedDisplayRect = mSurfaceHandler->NotifySurfaceReady();

  ForAllRepresentations([&](Representation& r) {
    r.mMutatedFrontSurface = true;
    r.mMutatedDisplayRect = mutatedDisplayRect;
  });
}

void NativeLayerCA::DiscardBackbuffers() {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(mSurfaceHandler);
  mSurfaceHandler->DiscardBackbuffers();
}

NativeLayerCA::Representation& NativeLayerCA::GetRepresentation(
    WhichRepresentation aRepresentation) {
  switch (aRepresentation) {
    case WhichRepresentation::ONSCREEN:
      return mOnscreenRepresentation;
    case WhichRepresentation::OFFSCREEN:
      return mOffscreenRepresentation;
  }
}

template <typename F>
void NativeLayerCA::ForAllRepresentations(F aFn) {
  aFn(mOnscreenRepresentation);
  aFn(mOffscreenRepresentation);
}

NativeLayerCA::UpdateType NativeLayerCA::HasUpdate(
    WhichRepresentation aRepresentation) {
  MutexAutoLock lock(mMutex);
  return GetRepresentation(aRepresentation).HasUpdate(IsVideo(lock));
}

/* static */
Maybe<CGRect> NativeLayerCA::CalculateClipGeometry(
    const gfx::IntSize& aSize, const gfx::IntPoint& aPosition,
    const gfx::Matrix4x4& aTransform, const gfx::IntRect& aDisplayRect,
    const Maybe<gfx::IntRect>& aClipRect, float aBackingScale) {
  Maybe<IntRect> clipFromDisplayRect;
  if (!aDisplayRect.IsEqualInterior(IntRect({}, aSize))) {
    // When the display rect is a subset of the layer, then we want to guarantee
    // that no pixels outside that rect are sampled, since they might be
    // uninitialized. Transforming the display rect into a post-transform clip
    // only maintains this if it's an integer translation, which is all we
    // support for this case currently.
    MOZ_ASSERT(aTransform.Is2DIntegerTranslation());
    clipFromDisplayRect = Some(RoundedToInt(
        aTransform.TransformBounds(IntRectToRect(aDisplayRect + aPosition))));
  }

  Maybe<gfx::IntRect> effectiveClip =
      IntersectMaybeRects(aClipRect, clipFromDisplayRect);
  if (!effectiveClip) {
    return Nothing();
  }

  return Some(CGRectMake(effectiveClip->X() / aBackingScale,
                         effectiveClip->Y() / aBackingScale,
                         effectiveClip->Width() / aBackingScale,
                         effectiveClip->Height() / aBackingScale));
}

bool NativeLayerCA::ApplyChanges(WhichRepresentation aRepresentation,
                                 NativeLayerCA::UpdateType aUpdate) {
  MutexAutoLock lock(mMutex);
  CFTypeRefPtr<IOSurfaceRef> surface;
  IntSize size = mSize;
  IntRect displayRect = mDisplayRect;
  bool surfaceIsFlipped = mSurfaceIsFlipped;

  if (mSurfaceToPresent) {
    surface = mSurfaceToPresent;
  } else if (mSurfaceHandler) {
    if (auto frontSurface = mSurfaceHandler->FrontSurface()) {
      surface = frontSurface->mSurface;
    }
    size = mSurfaceHandler->Size();
    displayRect = mSurfaceHandler->DisplayRect();
    surfaceIsFlipped = mSurfaceHandler->SurfaceIsFlipped();
  } else if (mTextureHost) {
    surface = mTextureHost->GetSurface()->GetIOSurfaceRef();
  }

  return GetRepresentation(aRepresentation)
      .ApplyChanges(aUpdate, size, mIsOpaque, mPosition, mTransform,
                    displayRect, mClipRect, mRoundedClipRect, mBackingScale,
                    surfaceIsFlipped, mSamplingFilter, mSpecializeVideo,
                    surface, mColor, mIsDRM, IsVideo(lock));
}

CALayer* NativeLayerCA::UnderlyingCALayer(WhichRepresentation aRepresentation) {
  MutexAutoLock lock(mMutex);
  return GetRepresentation(aRepresentation).UnderlyingCALayer();
}

static NSString* NSStringForOSType(OSType type) {
  unichar c[4];
  c[0] = (type >> 24) & 0xFF;
  c[1] = (type >> 16) & 0xFF;
  c[2] = (type >> 8) & 0xFF;
  c[3] = (type >> 0) & 0xFF;
  NSString* string = [[NSString stringWithCharacters:c length:4] autorelease];
  return string;
}

/* static */ void LogSurface(IOSurfaceRef aSurfaceRef, CVPixelBufferRef aBuffer,
                             CMVideoFormatDescriptionRef aFormat) {
  NSLog(@"VIDEO_LOG: LogSurface...\n");

  CFDictionaryRef surfaceValues = IOSurfaceCopyAllValues(aSurfaceRef);
  NSLog(@"Surface values are %@.\n", surfaceValues);
  CFRelease(surfaceValues);

  if (aBuffer) {
#ifdef XP_MACOSX
    CGColorSpaceRef colorSpace = CVImageBufferGetColorSpace(aBuffer);
    NSLog(@"ColorSpace is %@.\n", colorSpace);
#endif

    CFDictionaryRef bufferAttachments =
        CVBufferGetAttachments(aBuffer, kCVAttachmentMode_ShouldPropagate);
    NSLog(@"Buffer attachments are %@.\n", bufferAttachments);
  }

  if (aFormat) {
    OSType codec = CMFormatDescriptionGetMediaSubType(aFormat);
    NSLog(@"Codec is %@.\n", NSStringForOSType(codec));

    CFDictionaryRef extensions = CMFormatDescriptionGetExtensions(aFormat);
    NSLog(@"Format extensions are %@.\n", extensions);
  }
}

bool NativeLayerCA::Representation::EnqueueSurface(IOSurfaceRef aSurfaceRef) {
  MOZ_ASSERT(
      [mContentCALayer isKindOfClass:[AVSampleBufferDisplayLayer class]]);
  AVSampleBufferDisplayLayer* videoLayer =
      (AVSampleBufferDisplayLayer*)mContentCALayer;

  if (@available(macOS 11.0, iOS 14.0, *)) {
    if (videoLayer.requiresFlushToResumeDecoding) {
      [videoLayer flush];
    }
  }

  // If the layer can't handle a new sample, note that in the log.
  if (!videoLayer.readyForMoreMediaData) {
#ifdef NIGHTLY_BUILD
    if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
      NSLog(@"VIDEO_LOG: EnqueueSurface even though layer is not ready for "
            @"more data.");
    }
#endif
  }

  // Convert the IOSurfaceRef into a CMSampleBuffer, so we can enqueue it in
  // mContentCALayer
  CVPixelBufferRef pixelBuffer = nullptr;
  CVReturn cvValue = CVPixelBufferCreateWithIOSurface(
      kCFAllocatorDefault, aSurfaceRef, nullptr, &pixelBuffer);
  if (cvValue != kCVReturnSuccess) {
    MOZ_ASSERT(pixelBuffer == nullptr,
               "Failed call shouldn't allocate memory.");
#ifdef NIGHTLY_BUILD
    if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
      NSLog(@"VIDEO_LOG: EnqueueSurface failed on allocating pixel buffer.");
    }
#endif
    return false;
  }

#if defined(NIGHTLY_BUILD) && defined(XP_MACOSX)
  if (StaticPrefs::gfx_core_animation_specialize_video_check_color_space()) {
    // Ensure the resulting pixel buffer has a color space. If it doesn't, then
    // modify the surface and create the buffer again.
    CFTypeRefPtr<CGColorSpaceRef> colorSpace =
        CFTypeRefPtr<CGColorSpaceRef>::WrapUnderGetRule(
            CVImageBufferGetColorSpace(pixelBuffer));
    if (!colorSpace) {
      // Use our main display color space.
      colorSpace = CFTypeRefPtr<CGColorSpaceRef>::WrapUnderCreateRule(
          CGDisplayCopyColorSpace(CGMainDisplayID()));
      auto colorData = CFTypeRefPtr<CFDataRef>::WrapUnderCreateRule(
          CGColorSpaceCopyICCData(colorSpace.get()));
      IOSurfaceSetValue(aSurfaceRef, CFSTR("IOSurfaceColorSpace"),
                        colorData.get());

      // Get rid of our old pixel buffer and create a new one.
      CFRelease(pixelBuffer);
      cvValue = CVPixelBufferCreateWithIOSurface(
          kCFAllocatorDefault, aSurfaceRef, nullptr, &pixelBuffer);
      if (cvValue != kCVReturnSuccess) {
        MOZ_ASSERT(pixelBuffer == nullptr,
                   "Failed call shouldn't allocate memory.");
        return false;
      }
    }
    MOZ_ASSERT(CVImageBufferGetColorSpace(pixelBuffer),
               "Pixel buffer should have a color space.");
  }
#endif

  CFTypeRefPtr<CVPixelBufferRef> pixelBufferDeallocator =
      CFTypeRefPtr<CVPixelBufferRef>::WrapUnderCreateRule(pixelBuffer);

  CMVideoFormatDescriptionRef formatDescription = nullptr;
  OSStatus osValue = CMVideoFormatDescriptionCreateForImageBuffer(
      kCFAllocatorDefault, pixelBuffer, &formatDescription);
  if (osValue != noErr) {
    MOZ_ASSERT(formatDescription == nullptr,
               "Failed call shouldn't allocate memory.");
#ifdef NIGHTLY_BUILD
    if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
      NSLog(@"VIDEO_LOG: EnqueueSurface failed on allocating format "
            @"description.");
    }
#endif
    return false;
  }
  CFTypeRefPtr<CMVideoFormatDescriptionRef> formatDescriptionDeallocator =
      CFTypeRefPtr<CMVideoFormatDescriptionRef>::WrapUnderCreateRule(
          formatDescription);

#ifdef NIGHTLY_BUILD
  if (mLogNextVideoSurface &&
      StaticPrefs::gfx_core_animation_specialize_video_log()) {
    LogSurface(aSurfaceRef, pixelBuffer, formatDescription);
    mLogNextVideoSurface = false;
  }
#endif

  CMSampleTimingInfo timingInfo = kCMTimingInfoInvalid;

  bool spoofTiming = false;
#ifdef NIGHTLY_BUILD
  spoofTiming = StaticPrefs::gfx_core_animation_specialize_video_spoof_timing();
#endif
  if (spoofTiming) {
    // Since we don't have timing information for the sample, set the sample to
    // play at the current timestamp.
    CMTimebaseRef timebase =
        [(AVSampleBufferDisplayLayer*)mContentCALayer controlTimebase];
    CMTime nowTime = CMTimebaseGetTime(timebase);
    timingInfo = {.presentationTimeStamp = nowTime};
  }

  CMSampleBufferRef sampleBuffer = nullptr;
  osValue = CMSampleBufferCreateReadyWithImageBuffer(
      kCFAllocatorDefault, pixelBuffer, formatDescription, &timingInfo,
      &sampleBuffer);
  if (osValue != noErr) {
    MOZ_ASSERT(sampleBuffer == nullptr,
               "Failed call shouldn't allocate memory.");
#ifdef NIGHTLY_BUILD
    if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
      NSLog(@"VIDEO_LOG: EnqueueSurface failed on allocating sample buffer.");
    }
#endif
    return false;
  }
  CFTypeRefPtr<CMSampleBufferRef> sampleBufferDeallocator =
      CFTypeRefPtr<CMSampleBufferRef>::WrapUnderCreateRule(sampleBuffer);

  if (!spoofTiming) {
    // Since we don't have timing information for the sample, before we enqueue
    // it, we attach an attribute that specifies that the sample should be
    // played immediately.
    CFArrayRef attachmentsArray =
        CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, YES);
    if (!attachmentsArray || CFArrayGetCount(attachmentsArray) == 0) {
      // No dictionary to alter.
      return false;
    }
    CFMutableDictionaryRef sample0Dictionary =
        (__bridge CFMutableDictionaryRef)CFArrayGetValueAtIndex(
            attachmentsArray, 0);
    CFDictionarySetValue(sample0Dictionary,
                         kCMSampleAttachmentKey_DisplayImmediately,
                         kCFBooleanTrue);
  }

  [videoLayer enqueueSampleBuffer:sampleBuffer];

  return true;
}

bool NativeLayerCA::Representation::ApplyChanges(
    NativeLayerCA::UpdateType aUpdate, const IntSize& aSize, bool aIsOpaque,
    const IntPoint& aPosition, const Matrix4x4& aTransform,
    const IntRect& aDisplayRect, const Maybe<IntRect>& aClipRect,
    const Maybe<gfx::RoundedRect>& aRoundedClip, float aBackingScale,
    bool aSurfaceIsFlipped, gfx::SamplingFilter aSamplingFilter,
    bool aSpecializeVideo, CFTypeRefPtr<IOSurfaceRef> aFrontSurface,
    CFTypeRefPtr<CGColorRef> aColor, bool aIsDRM, bool aIsVideo) {
  // If we have an OnlyVideo update, handle it and early exit.
  if (aUpdate == UpdateType::OnlyVideo) {
    // If we don't have any updates to do, exit early with success. This is
    // important to do so that the overall OnlyVideo pass will succeed as long
    // as the video layers are successful.
    if (HasUpdate(true) == UpdateType::None) {
      return true;
    }

    MOZ_ASSERT(!mMutatedSpecializeVideo && mMutatedFrontSurface,
               "Shouldn't attempt a OnlyVideo update in this case.");

    bool updateSucceeded = false;
    if (aSpecializeVideo) {
      IOSurfaceRef surface = aFrontSurface.get();
      updateSucceeded = EnqueueSurface(surface);

      if (updateSucceeded) {
        mMutatedFrontSurface = false;
      } else {
        // Set mMutatedSpecializeVideo, which will ensure that the next update
        // will rebuild the video layer.
        mMutatedSpecializeVideo = true;
#ifdef NIGHTLY_BUILD
        if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
          NSLog(@"VIDEO_LOG: EnqueueSurface failed in OnlyVideo update.");
        }
#endif
      }
    }

    return updateSucceeded;
  }

  MOZ_ASSERT(aUpdate == UpdateType::All);

  if (mWrappingCALayer && mMutatedSpecializeVideo) {
    // Since specialize video changes the way we construct our wrapping and
    // content layers, we have to scrap them if this value has changed.
#ifdef NIGHTLY_BUILD
    if (aIsVideo && StaticPrefs::gfx_core_animation_specialize_video_log()) {
      NSLog(@"VIDEO_LOG: Scrapping existing video layer.");
    }
#endif
    [mContentCALayer release];
    mContentCALayer = nil;
    [mOpaquenessTintLayer release];
    mOpaquenessTintLayer = nil;
    [mWrappingCALayer release];
    mWrappingCALayer = nil;
    [mRoundedClipCALayer release];
    mRoundedClipCALayer = nil;
  }

  bool layerNeedsInitialization = false;
  if (!mWrappingCALayer) {
    layerNeedsInitialization = true;
    mWrappingCALayer = [[CALayer layer] retain];
    mWrappingCALayer.position = CGPointZero;
    mWrappingCALayer.bounds = CGRectZero;
    mWrappingCALayer.anchorPoint = CGPointZero;
    mWrappingCALayer.contentsGravity = kCAGravityTopLeft;
    mWrappingCALayer.edgeAntialiasingMask = 0;

    mRoundedClipCALayer = [[CALayer layer] retain];
    mRoundedClipCALayer.position = CGPointZero;
    mRoundedClipCALayer.bounds = CGRectZero;
    mRoundedClipCALayer.anchorPoint = CGPointZero;
    mRoundedClipCALayer.contentsGravity = kCAGravityTopLeft;
    mRoundedClipCALayer.edgeAntialiasingMask = 0;
    mRoundedClipCALayer.masksToBounds = NO;

    [mWrappingCALayer addSublayer:mRoundedClipCALayer];

    if (aColor) {
      // Color layers set a color on the clip layer and don't get a content
      // layer.
      mRoundedClipCALayer.backgroundColor = aColor.get();
    } else {
      if (aSpecializeVideo) {
#ifdef NIGHTLY_BUILD
        if (aIsVideo &&
            StaticPrefs::gfx_core_animation_specialize_video_log()) {
          NSLog(@"VIDEO_LOG: Rebuilding video layer with "
                @"AVSampleBufferDisplayLayer.");
          mLogNextVideoSurface = true;
        }
#endif
        mContentCALayer = [[AVSampleBufferDisplayLayer layer] retain];
        CMTimebaseRef timebase;
#ifdef CMTIMEBASE_USE_SOURCE_TERMINOLOGY
        CMTimebaseCreateWithSourceClock(kCFAllocatorDefault,
                                        CMClockGetHostTimeClock(), &timebase);
#else
        CMTimebaseCreateWithMasterClock(kCFAllocatorDefault,
                                        CMClockGetHostTimeClock(), &timebase);
#endif
        CMTimebaseSetRate(timebase, 1.0f);
        [(AVSampleBufferDisplayLayer*)mContentCALayer
            setControlTimebase:timebase];
        CFRelease(timebase);
      } else {
#ifdef NIGHTLY_BUILD
        if (aIsVideo &&
            StaticPrefs::gfx_core_animation_specialize_video_log()) {
          NSLog(@"VIDEO_LOG: Rebuilding video layer with CALayer.");
          mLogNextVideoSurface = true;
        }
#endif
        mContentCALayer = [[CALayer layer] retain];
      }
      mContentCALayer.position = CGPointZero;
      mContentCALayer.anchorPoint = CGPointZero;
      mContentCALayer.contentsGravity = kCAGravityTopLeft;
      mContentCALayer.contentsScale = 1;
      mContentCALayer.bounds = CGRectMake(0, 0, aSize.width, aSize.height);
      mContentCALayer.edgeAntialiasingMask = 0;
      mContentCALayer.opaque = aIsOpaque;
      if ([mContentCALayer respondsToSelector:@selector(setContentsOpaque:)]) {
        // The opaque property seems to not be enough when using IOSurface
        // contents. Additionally, call the private method setContentsOpaque.
        [mContentCALayer setContentsOpaque:aIsOpaque];
      }

      [mRoundedClipCALayer addSublayer:mContentCALayer];
    }
  }

  if (aSpecializeVideo && mMutatedIsDRM) {
    ((AVSampleBufferDisplayLayer*)mContentCALayer).preventsCapture = aIsDRM;
  }

  bool shouldTintOpaqueness = StaticPrefs::gfx_core_animation_tint_opaque();
  if (shouldTintOpaqueness && !mOpaquenessTintLayer) {
    mOpaquenessTintLayer = [[CALayer layer] retain];
    mOpaquenessTintLayer.position = CGPointZero;
    mOpaquenessTintLayer.bounds = mContentCALayer.bounds;
    mOpaquenessTintLayer.anchorPoint = CGPointZero;
    mOpaquenessTintLayer.contentsGravity = kCAGravityTopLeft;
    if (aIsOpaque) {
      mOpaquenessTintLayer.backgroundColor =
          CGColorCreateGenericRGB(0, 1, 0, 0.5);
    } else {
      mOpaquenessTintLayer.backgroundColor =
          CGColorCreateGenericRGB(1, 0, 0, 0.5);
    }
    [mRoundedClipCALayer addSublayer:mOpaquenessTintLayer];
  } else if (!shouldTintOpaqueness && mOpaquenessTintLayer) {
    [mOpaquenessTintLayer removeFromSuperlayer];
    [mOpaquenessTintLayer release];
    mOpaquenessTintLayer = nullptr;
  }

  // CALayers have a position and a size, specified through the position and the
  // bounds properties. layer.bounds.origin must always be (0, 0). A layer's
  // position affects the layer's entire layer subtree. In other words, each
  // layer's position is relative to its superlayer's position. We implement the
  // clip rect using masksToBounds on mWrappingCALayer. So mContentCALayer's
  // position is relative to the clip rect position. Note: The Core Animation
  // docs on "Positioning and Sizing Sublayers" say:
  //  Important: Always use integral numbers for the width and height of your
  //  layer.
  // We hope that this refers to integral physical pixels, and not to integral
  // logical coordinates.

  if (mContentCALayer &&
      (mMutatedBackingScale || mMutatedSize || layerNeedsInitialization)) {
    mContentCALayer.bounds = CGRectMake(0, 0, aSize.width / aBackingScale,
                                        aSize.height / aBackingScale);
    if (mOpaquenessTintLayer) {
      mOpaquenessTintLayer.bounds = mContentCALayer.bounds;
    }
    mContentCALayer.contentsScale = aBackingScale;
  }

  if (mMutatedBackingScale || mMutatedPosition || mMutatedDisplayRect ||
      mMutatedClipRect || mMutatedRoundedClipRect || mMutatedTransform ||
      mMutatedSurfaceIsFlipped || mMutatedSize || layerNeedsInitialization) {
    Maybe<CGRect> scaledClipRect = CalculateClipGeometry(
        aSize, aPosition, aTransform, aDisplayRect, aClipRect, aBackingScale);

    CGRect useClipRect;
    if (scaledClipRect.isSome()) {
      useClipRect = *scaledClipRect;
    } else {
      useClipRect = CGRectZero;
    }

    mWrappingCALayer.position = useClipRect.origin;
    mWrappingCALayer.bounds =
        CGRectMake(0, 0, useClipRect.size.width, useClipRect.size.height);
    mWrappingCALayer.masksToBounds = scaledClipRect.isSome();

    // Default the clip rect for the rounded rect clip layer to be the
    // same as the wrapping layer clip. This ensures that if it's not used,
    // and the background color is applied to this layer, it draws correctly.
    CGRect rrClipRect =
        CGRectMake(0, 0, useClipRect.size.width, useClipRect.size.height);

    // We currently support only the easy case here (where the radii is uniform
    // or zero). If a clip is supplied with non-uniform, non-zero radii we'll
    // silently render incorrectly. However, WR only promotes compositor clips
    // that match this criteria, so we won't hit an incorrect rendering path due
    // to the higher level check.
    // https://bugzilla.mozilla.org/show_bug.cgi?id=1960515#c2 has details on
    // supporting the harder cases.

    // Reset the rounded clip layer params to disabled, in case they were
    // mutated and don't get selected below.
    mRoundedClipCALayer.cornerRadius = 0.0f;
    mRoundedClipCALayer.masksToBounds = NO;
    mRoundedClipCALayer.maskedCorners = 0;

    if (aRoundedClip.isSome()) {
      // Select which corner(s) the rounded clip should be applied to
      // Select from the corners which is the maximum radius (since we know
      // they are either uniform or zero).
      CACornerMask maskedCorners = 0;
      auto effectiveRadius = 0.0f;
      if (aRoundedClip->corners.radii[0].width > 0.0) {
        maskedCorners |= kCALayerMinXMinYCorner;
        effectiveRadius =
            std::max(effectiveRadius, aRoundedClip->corners.radii[0].width);
      }
      if (aRoundedClip->corners.radii[1].width > 0.0) {
        maskedCorners |= kCALayerMaxXMinYCorner;
        effectiveRadius =
            std::max(effectiveRadius, aRoundedClip->corners.radii[1].width);
      }
      if (aRoundedClip->corners.radii[2].width > 0.0) {
        maskedCorners |= kCALayerMaxXMaxYCorner;
        effectiveRadius =
            std::max(effectiveRadius, aRoundedClip->corners.radii[2].width);
      }
      if (aRoundedClip->corners.radii[3].width > 0.0) {
        maskedCorners |= kCALayerMinXMaxYCorner;
        effectiveRadius =
            std::max(effectiveRadius, aRoundedClip->corners.radii[3].width);
      }

      // Only create a rounded clip mask if we had 1+ non-zero corner radius
      if (maskedCorners != 0) {
        rrClipRect.origin.x = aRoundedClip->rect.x / aBackingScale;
        rrClipRect.origin.y = aRoundedClip->rect.y / aBackingScale;
        rrClipRect.size.width = aRoundedClip->rect.width / aBackingScale;
        rrClipRect.size.height = aRoundedClip->rect.height / aBackingScale;

        // Move in to local space relative to the parent wrapping layer
        rrClipRect.origin.x -= useClipRect.origin.x;
        rrClipRect.origin.y -= useClipRect.origin.y;

        mRoundedClipCALayer.cornerRadius = effectiveRadius / aBackingScale;
        mRoundedClipCALayer.masksToBounds = YES;
        mRoundedClipCALayer.maskedCorners = maskedCorners;
      }
    }

    // Position the rounded clip layer in the right space
    mRoundedClipCALayer.position = rrClipRect.origin;
    mRoundedClipCALayer.bounds =
        CGRectMake(0, 0, rrClipRect.size.width, rrClipRect.size.height);

    if (mContentCALayer) {
      Matrix4x4 transform = aTransform;
      transform.PreTranslate(aPosition.x, aPosition.y, 0);
      transform.PostTranslate(
          ((-useClipRect.origin.x - rrClipRect.origin.x) * aBackingScale),
          ((-useClipRect.origin.y - rrClipRect.origin.y) * aBackingScale), 0);

      if (aSurfaceIsFlipped) {
        transform.PreTranslate(0, aSize.height, 0).PreScale(1, -1, 1);
      }

      CATransform3D transformCA{transform._11,
                                transform._12,
                                transform._13,
                                transform._14,
                                transform._21,
                                transform._22,
                                transform._23,
                                transform._24,
                                transform._31,
                                transform._32,
                                transform._33,
                                transform._34,
                                transform._41 / aBackingScale,
                                transform._42 / aBackingScale,
                                transform._43,
                                transform._44};
      mContentCALayer.transform = transformCA;
      if (mOpaquenessTintLayer) {
        mOpaquenessTintLayer.transform = mContentCALayer.transform;
      }
    }
  }

  if (mContentCALayer && (mMutatedSamplingFilter || layerNeedsInitialization)) {
    if (aSamplingFilter == gfx::SamplingFilter::POINT) {
      mContentCALayer.minificationFilter = kCAFilterNearest;
      mContentCALayer.magnificationFilter = kCAFilterNearest;
    } else {
      mContentCALayer.minificationFilter = kCAFilterLinear;
      mContentCALayer.magnificationFilter = kCAFilterLinear;
    }
  }

  if (mMutatedFrontSurface) {
    // This is handled last because a video update could fail, causing us to
    // early exit, leaving the mutation bits untouched. We do this so that the
    // *next* update will clear the video layer and setup a regular layer.

    IOSurfaceRef surface = aFrontSurface.get();
    if (aSpecializeVideo) {
      // If we just rebuilt our layer, ensure the first frame is visible by
      // forcing the layer contents to display that frame. Our call to
      // enqueueSampleBuffer will handle future async updates to the layer;
      // buffers queued with enqueueSampleBuffer overwrite the layer contents.
      if (layerNeedsInitialization) {
        mContentCALayer.contents = (id)surface;
      }

      // Attempt to enqueue this as a video frame. If we fail, we'll rebuild
      // our video layer in the next update.
      bool isEnqueued = EnqueueSurface(surface);
      if (!isEnqueued) {
        // Set mMutatedSpecializeVideo, which will ensure that the next update
        // will rebuild the video layer.
        mMutatedSpecializeVideo = true;
#ifdef NIGHTLY_BUILD
        if (StaticPrefs::gfx_core_animation_specialize_video_log()) {
          NSLog(@"VIDEO_LOG: EnqueueSurface failed in All update.");
        }
#endif
        return false;
      }
    } else {
#ifdef NIGHTLY_BUILD
      if (mLogNextVideoSurface &&
          StaticPrefs::gfx_core_animation_specialize_video_log()) {
        LogSurface(surface, nullptr, nullptr);
        mLogNextVideoSurface = false;
      }
#endif
      mContentCALayer.contents = (id)surface;
    }
  }

  mMutatedPosition = false;
  mMutatedTransform = false;
  mMutatedBackingScale = false;
  mMutatedSize = false;
  mMutatedSurfaceIsFlipped = false;
  mMutatedDisplayRect = false;
  mMutatedClipRect = false;
  mMutatedRoundedClipRect = false;
  mMutatedFrontSurface = false;
  mMutatedSamplingFilter = false;
  mMutatedSpecializeVideo = false;
  mMutatedIsDRM = false;

  return true;
}

NativeLayerCA::UpdateType NativeLayerCA::Representation::HasUpdate(
    bool aIsVideo) {
  if (!mWrappingCALayer) {
    return UpdateType::All;
  }

  // This check intentionally skips mMutatedFrontSurface. We'll check it later
  // to see if we can attempt an OnlyVideo update.
  if (mMutatedPosition || mMutatedTransform || mMutatedDisplayRect ||
      mMutatedClipRect || mMutatedRoundedClipRect || mMutatedBackingScale ||
      mMutatedSize || mMutatedSurfaceIsFlipped || mMutatedSamplingFilter ||
      mMutatedSpecializeVideo || mMutatedIsDRM) {
    return UpdateType::All;
  }

  // Check if we should try an OnlyVideo update. We know from the above check
  // that our specialize video is stable (we don't know what value we'll
  // receive, though), so we just have to check that we have a surface to
  // display.
  if (mMutatedFrontSurface) {
    return (aIsVideo ? UpdateType::OnlyVideo : UpdateType::All);
  }

  return UpdateType::None;
}

bool NativeLayerCA::WillUpdateAffectLayers(
    WhichRepresentation aRepresentation) {
  MutexAutoLock lock(mMutex);
  auto& r = GetRepresentation(aRepresentation);
  return r.mMutatedSpecializeVideo || !r.UnderlyingCALayer();
}

bool DownscaleTargetNLRS::DownscaleFrom(
    profiler_screenshots::RenderSource* aSource, const IntRect& aSourceRect,
    const IntRect& aDestRect) {
  mGL->BlitHelper()->BlitFramebufferToFramebuffer(
      static_cast<RenderSourceNLRS*>(aSource)->FB().mFB,
      mRenderSource->FB().mFB, aSourceRect, aDestRect, LOCAL_GL_LINEAR);

  return true;
}

void AsyncReadbackBufferNLRS::CopyFrom(
    profiler_screenshots::RenderSource* aSource) {
  IntSize size = aSource->Size();
  MOZ_RELEASE_ASSERT(Size() == size);

  gl::ScopedPackState scopedPackState(mGL);
  mGL->fBindBuffer(LOCAL_GL_PIXEL_PACK_BUFFER, mBufferHandle);
  mGL->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 1);
  const gl::ScopedBindFramebuffer bindFB(
      mGL, static_cast<RenderSourceNLRS*>(aSource)->FB().mFB);
  mGL->fReadPixels(0, 0, size.width, size.height, LOCAL_GL_RGBA,
                   LOCAL_GL_UNSIGNED_BYTE, 0);
}

bool AsyncReadbackBufferNLRS::MapAndCopyInto(DataSourceSurface* aSurface,
                                             const IntSize& aReadSize) {
  MOZ_RELEASE_ASSERT(aReadSize <= aSurface->GetSize());

  if (!mGL || !mGL->MakeCurrent()) {
    return false;
  }

  gl::ScopedPackState scopedPackState(mGL);
  mGL->fBindBuffer(LOCAL_GL_PIXEL_PACK_BUFFER, mBufferHandle);
  mGL->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 1);

  const uint8_t* srcData = nullptr;
  if (mGL->IsSupported(gl::GLFeature::map_buffer_range)) {
    srcData = static_cast<uint8_t*>(mGL->fMapBufferRange(
        LOCAL_GL_PIXEL_PACK_BUFFER, 0, aReadSize.height * aReadSize.width * 4,
        LOCAL_GL_MAP_READ_BIT));
  } else {
    srcData = static_cast<uint8_t*>(
        mGL->fMapBuffer(LOCAL_GL_PIXEL_PACK_BUFFER, LOCAL_GL_READ_ONLY));
  }

  if (!srcData) {
    return false;
  }

  int32_t srcStride = mSize.width * 4;  // Bind() sets an alignment of 1
  DataSourceSurface::ScopedMap map(aSurface, DataSourceSurface::WRITE);
  uint8_t* destData = map.GetData();
  int32_t destStride = map.GetStride();
  SurfaceFormat destFormat = aSurface->GetFormat();
  for (int32_t destRow = 0; destRow < aReadSize.height; destRow++) {
    // Turn srcData upside down during the copy.
    int32_t srcRow = aReadSize.height - 1 - destRow;
    const uint8_t* src = &srcData[srcRow * srcStride];
    uint8_t* dest = &destData[destRow * destStride];
    SwizzleData(src, srcStride, SurfaceFormat::R8G8B8A8, dest, destStride,
                destFormat, IntSize(aReadSize.width, 1));
  }

  mGL->fUnmapBuffer(LOCAL_GL_PIXEL_PACK_BUFFER);

  return true;
}

AsyncReadbackBufferNLRS::~AsyncReadbackBufferNLRS() {
  if (mGL && mGL->MakeCurrent()) {
    mGL->fDeleteBuffers(1, &mBufferHandle);
  }
}

}  // namespace layers
}  // namespace mozilla
