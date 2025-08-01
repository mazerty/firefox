/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Main header first:
#include "SVGImageContext.h"

// Keep others in (case-insensitive) order:
#include "gfxUtils.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/ServoCSSParser.h"
#include "mozilla/StaticPrefs_svg.h"
#include "mozilla/dom/Document.h"
#include "nsIFrame.h"
#include "nsISVGPaintContext.h"
#include "nsPresContext.h"
#include "nsStyleStruct.h"

namespace mozilla {

/* static */
void SVGImageContext::MaybeStoreContextPaint(SVGImageContext& aContext,
                                             nsIFrame* aFromFrame,
                                             imgIContainer* aImgContainer) {
  return MaybeStoreContextPaint(aContext, *aFromFrame->PresContext(),
                                *aFromFrame->Style(), aImgContainer);
}

/* static */
void SVGImageContext::MaybeStoreContextPaint(SVGImageContext& aContext,
                                             const nsPresContext& aPresContext,
                                             const ComputedStyle& aStyle,
                                             imgIContainer* aImgContainer) {
  if (aImgContainer->GetType() != imgIContainer::TYPE_VECTOR) {
    // Avoid this overhead for raster images.
    return;
  }

  {
    auto scheme = LookAndFeel::ColorSchemeForStyle(
        *aPresContext.Document(), aStyle.StyleUI()->mColorScheme.bits,
        ColorSchemeMode::Preferred);
    aContext.SetColorScheme(Some(scheme));
  }

  const nsStyleSVG* style = aStyle.StyleSVG();
  if (!style->ExposesContextProperties()) {
    // Content must have '-moz-context-properties' set to the names of the
    // properties it wants to expose to images it links to.
    return;
  }

  bool haveContextPaint = false;

  auto contextPaint = MakeRefPtr<SVGEmbeddingContextPaint>();

  if ((style->mMozContextProperties.bits & StyleContextPropertyBits::FILL) &&
      style->mFill.kind.IsColor()) {
    haveContextPaint = true;
    contextPaint->SetFill(style->mFill.kind.AsColor().CalcColor(aStyle));
  }
  if ((style->mMozContextProperties.bits & StyleContextPropertyBits::STROKE) &&
      style->mStroke.kind.IsColor()) {
    haveContextPaint = true;
    contextPaint->SetStroke(style->mStroke.kind.AsColor().CalcColor(aStyle));
  }
  if (style->mMozContextProperties.bits &
      StyleContextPropertyBits::FILL_OPACITY) {
    haveContextPaint = true;
    contextPaint->SetFillOpacity(style->mFillOpacity.IsOpacity()
                                     ? style->mFillOpacity.AsOpacity()
                                     : 1.0f);
  }
  if (style->mMozContextProperties.bits &
      StyleContextPropertyBits::STROKE_OPACITY) {
    haveContextPaint = true;
    contextPaint->SetStrokeOpacity(style->mStrokeOpacity.IsOpacity()
                                       ? style->mStrokeOpacity.AsOpacity()
                                       : 1.0f);
  }

  if (haveContextPaint) {
    aContext.mContextPaint = std::move(contextPaint);
  }
}

/* static */
void SVGImageContext::MaybeStoreContextPaint(SVGImageContext& aContext,
                                             nsISVGPaintContext* aPaintContext,
                                             imgIContainer* aImgContainer) {
  if (aImgContainer->GetType() != imgIContainer::TYPE_VECTOR ||
      !aPaintContext) {
    // Avoid this overhead for raster images.
    return;
  }

  bool haveContextPaint = false;
  auto contextPaint = MakeRefPtr<SVGEmbeddingContextPaint>();
  nsCString value;
  float opacity;

  if (NS_SUCCEEDED(aPaintContext->GetStrokeColor(value)) && !value.IsEmpty()) {
    nscolor color;
    if (ServoCSSParser::ComputeColor(nullptr, NS_RGB(0, 0, 0), value, &color)) {
      haveContextPaint = true;
      contextPaint->SetStroke(color);
    }
  }

  if (NS_SUCCEEDED(aPaintContext->GetFillColor(value)) && !value.IsEmpty()) {
    nscolor color;
    if (ServoCSSParser::ComputeColor(nullptr, NS_RGB(0, 0, 0), value, &color)) {
      haveContextPaint = true;
      contextPaint->SetFill(color);
    }
  }

  if (NS_SUCCEEDED(aPaintContext->GetStrokeOpacity(&opacity))) {
    haveContextPaint = true;
    contextPaint->SetStrokeOpacity(opacity);
  }

  if (NS_SUCCEEDED(aPaintContext->GetFillOpacity(&opacity))) {
    haveContextPaint = true;
    contextPaint->SetFillOpacity(opacity);
  }

  if (haveContextPaint) {
    aContext.mContextPaint = std::move(contextPaint);
  }
}

}  // namespace mozilla
