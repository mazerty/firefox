/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Retrieves and displays icons in native menu items on Mac OS X.
 */

/* exception_defines.h defines 'try' to 'if (true)' which breaks objective-c
   exceptions and produces errors like: error: unexpected '@' in program'.
   If we define __EXCEPTIONS exception_defines.h will avoid doing this.

   See bug 666609 for more information.

   We use <limits> to get the libstdc++ version. */
#include <limits>
#if __GLIBCXX__ <= 20070719
#  ifndef __EXCEPTIONS
#    define __EXCEPTIONS
#  endif
#endif

#include "MOZIconHelper.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/widget/NativeMenu.h"
#include "mozilla/SVGImageContext.h"
#include "nsCocoaUtils.h"
#include "nsComputedDOMStyle.h"
#include "nsContentUtils.h"
#include "nsGkAtoms.h"
#include "nsIContent.h"
#include "nsIContentPolicy.h"
#include "nsMenuItemX.h"
#include "nsMenuItemIconX.h"
#include "nsNameSpaceManager.h"
#include "nsObjCExceptions.h"

using namespace mozilla;

using mozilla::dom::Element;
using mozilla::widget::IconLoader;

static const uint32_t kIconSize = 16;

nsMenuItemIconX::nsMenuItemIconX(Listener* aListener) : mListener(aListener) {
  MOZ_COUNT_CTOR(nsMenuItemIconX);
}

nsMenuItemIconX::~nsMenuItemIconX() {
  if (mIconLoader) {
    mIconLoader->Destroy();
  }
  if (mIconImage) {
    [mIconImage release];
    mIconImage = nil;
  }
  MOZ_COUNT_DTOR(nsMenuItemIconX);
}

void nsMenuItemIconX::SetupIcon(nsIContent* aContent) {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  bool shouldHaveIcon = StartIconLoad(aContent);
  if (!shouldHaveIcon) {
    // There is no icon for this menu item, as an error occurred while loading
    // it. An icon might have been set earlier or the place holder icon may have
    // been set.  Clear it.
    if (mIconImage) {
      [mIconImage release];
      mIconImage = nil;
    }
    return;
  }

  if (!mIconImage) {
    // Set a placeholder icon, so that the menuitem reserves space for the icon
    // during the load and there is no sudden shift once the icon finishes
    // loading.
    NSSize iconSize = NSMakeSize(kIconSize, kIconSize);
    mIconImage = [[MOZIconHelper placeholderIconWithSize:iconSize] retain];
  }

  NS_OBJC_END_TRY_ABORT_BLOCK;
}

bool nsMenuItemIconX::StartIconLoad(nsIContent* aContent) {
  if (!aContent->IsElement()) {
    return false;
  }
  auto icon = widget::NativeMenu::GetIcon(*aContent->AsElement());
  if (!icon) {
    return false;
  }

  mComputedStyle = std::move(icon.mStyle);
  mPresContext = aContent->OwnerDoc()->GetPresContext();
  if (!mIconLoader) {
    mIconLoader = new IconLoader(this);
  }

  return NS_SUCCEEDED(mIconLoader->LoadIcon(icon.mURI, aContent));
}

//
// mozilla::widget::IconLoader::Listener
//

nsresult nsMenuItemIconX::OnComplete(imgIContainer* aImage) {
  NS_OBJC_BEGIN_TRY_ABORT_BLOCK;

  if (mIconImage) {
    [mIconImage release];
    mIconImage = nil;
  }
  RefPtr<nsPresContext> pc = mPresContext.get();
  UniquePtr<SVGImageContext> svgContext;
  if (pc && mComputedStyle) {
    svgContext = MakeUnique<SVGImageContext>();
    SVGImageContext::MaybeStoreContextPaint(*svgContext, *pc, *mComputedStyle,
                                            aImage);
  }

  mIconImage = [[MOZIconHelper
      iconImageFromImageContainer:aImage
                         withSize:NSMakeSize(kIconSize, kIconSize)
                       svgContext:svgContext.get()
                      scaleFactor:0.0f] retain];
  mComputedStyle = nullptr;
  mPresContext = nullptr;

  if (mListener) {
    mListener->IconUpdated();
  }

  mIconLoader->Destroy();

  return NS_OK;

  NS_OBJC_END_TRY_ABORT_BLOCK;
}
