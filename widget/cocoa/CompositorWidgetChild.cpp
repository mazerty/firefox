/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CompositorWidgetChild.h"
#include "mozilla/Unused.h"
#include "gfxPlatform.h"

namespace mozilla {
namespace widget {

CompositorWidgetChild::CompositorWidgetChild(
    RefPtr<CompositorVsyncDispatcher> aVsyncDispatcher,
    RefPtr<CompositorWidgetVsyncObserver> aVsyncObserver,
    const CompositorWidgetInitData&)
    : mVsyncDispatcher(aVsyncDispatcher), mVsyncObserver(aVsyncObserver) {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(!gfxPlatform::IsHeadless());
}

CompositorWidgetChild::~CompositorWidgetChild() = default;

bool CompositorWidgetChild::Initialize() { return true; }

void CompositorWidgetChild::Shutdown() { Unused << Send__delete__(this); }

mozilla::ipc::IPCResult CompositorWidgetChild::RecvObserveVsync() {
  mVsyncDispatcher->SetCompositorVsyncObserver(mVsyncObserver);
  return IPC_OK();
}

mozilla::ipc::IPCResult CompositorWidgetChild::RecvUnobserveVsync() {
  mVsyncDispatcher->SetCompositorVsyncObserver(nullptr);
  return IPC_OK();
}

void CompositorWidgetChild::NotifyClientSizeChanged(
    const LayoutDeviceIntSize& aClientSize) {
  Unused << SendNotifyClientSizeChanged(aClientSize);
}

}  // namespace widget
}  // namespace mozilla
