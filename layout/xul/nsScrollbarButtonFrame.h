/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**

  Eric D Vaughan
  This class lays out its children either vertically or horizontally

**/

#ifndef nsScrollbarButtonFrame_h___
#define nsScrollbarButtonFrame_h___

#include "SimpleXULLeafFrame.h"
#include "mozilla/Attributes.h"
#include "nsLeafFrame.h"
#include "nsRepeatService.h"

class nsScrollbarFrame;
class nsIScrollbarMediator;

namespace mozilla {
class PresShell;
}  // namespace mozilla

class nsScrollbarButtonFrame final : public mozilla::SimpleXULLeafFrame {
 public:
  NS_DECL_FRAMEARENA_HELPERS(nsScrollbarButtonFrame)

  nsScrollbarButtonFrame(ComputedStyle* aStyle, nsPresContext* aPresContext)
      : mozilla::SimpleXULLeafFrame(aStyle, aPresContext, kClassID) {}

  // Overrides
  void Destroy(DestroyContext&) override;

  friend nsIFrame* NS_NewScrollbarButtonFrame(mozilla::PresShell* aPresShell,
                                              ComputedStyle* aStyle);

  nsresult HandleEvent(nsPresContext* aPresContext,
                       mozilla::WidgetGUIEvent* aEvent,
                       nsEventStatus* aEventStatus) override;

  nsScrollbarFrame* GetScrollbar();
  nsIScrollbarMediator* GetMediator();

  bool HandleButtonPress(nsPresContext* aPresContext,
                         mozilla::WidgetGUIEvent* aEvent,
                         nsEventStatus* aEventStatus);

  NS_IMETHOD HandleMultiplePress(nsPresContext* aPresContext,
                                 mozilla::WidgetGUIEvent* aEvent,
                                 nsEventStatus* aEventStatus,
                                 bool aControlHeld) override {
    return NS_OK;
  }

  MOZ_CAN_RUN_SCRIPT
  NS_IMETHOD HandleDrag(nsPresContext* aPresContext,
                        mozilla::WidgetGUIEvent* aEvent,
                        nsEventStatus* aEventStatus) override {
    return NS_OK;
  }

  NS_IMETHOD HandleRelease(nsPresContext* aPresContext,
                           mozilla::WidgetGUIEvent* aEvent,
                           nsEventStatus* aEventStatus) override;

 protected:
  void StartRepeat() {
    nsRepeatService::GetInstance()->Start(Notify, this, mContent->OwnerDoc(),
                                          "nsScrollbarButtonFrame"_ns);
  }
  void StopRepeat() { nsRepeatService::GetInstance()->Stop(Notify, this); }
  void Notify();
  static void Notify(void* aData) {
    static_cast<nsScrollbarButtonFrame*>(aData)->Notify();
  }

  bool mCursorOnThis = false;
};

#endif
