/* -*- Mode: C++; tab-width: 40; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Theme.h"
#include <utility>
#include "ThemeCocoa.h"

#include "ThemeDrawing.h"
#include "Units.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/HTMLMeterElement.h"
#include "mozilla/dom/HTMLProgressElement.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/gfx/Types.h"
#include "mozilla/gfx/Filters.h"
#include "mozilla/RelativeLuminanceUtils.h"
#include "mozilla/ScrollContainerFrame.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/webrender/WebRenderAPI.h"
#include "nsCSSColorUtils.h"
#include "nsCSSRendering.h"
#include "nsScrollbarFrame.h"
#include "nsIScrollbarMediator.h"
#include "nsDeviceContext.h"
#include "nsLayoutUtils.h"
#include "nsRangeFrame.h"
#include "PathHelpers.h"
#include "ScrollbarDrawingAndroid.h"
#include "ScrollbarDrawingCocoa.h"
#include "ScrollbarDrawingGTK.h"
#include "ScrollbarDrawingWin.h"
#include "ScrollbarDrawingWin11.h"

#ifdef XP_WIN
#  include "mozilla/WindowsVersion.h"
#endif

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::gfx;
using namespace mozilla::widget;

namespace {

static constexpr gfx::sRGBColor sColorGrey10(
    gfx::sRGBColor::UnusualFromARGB(0xffe9e9ed));
static constexpr gfx::sRGBColor sColorGrey10Alpha50(
    gfx::sRGBColor::UnusualFromARGB(0x7fe9e9ed));
static constexpr gfx::sRGBColor sColorGrey20(
    gfx::sRGBColor::UnusualFromARGB(0xffd0d0d7));
static constexpr gfx::sRGBColor sColorGrey40(
    gfx::sRGBColor::UnusualFromARGB(0xff8f8f9d));
static constexpr gfx::sRGBColor sColorGrey40Alpha50(
    gfx::sRGBColor::UnusualFromARGB(0x7f8f8f9d));
static constexpr gfx::sRGBColor sColorGrey50(
    gfx::sRGBColor::UnusualFromARGB(0xff676774));
static constexpr gfx::sRGBColor sColorGrey60(
    gfx::sRGBColor::UnusualFromARGB(0xff484851));

static constexpr gfx::sRGBColor sColorMeterGreen10(
    gfx::sRGBColor::UnusualFromARGB(0xff00ab60));
static constexpr gfx::sRGBColor sColorMeterGreen20(
    gfx::sRGBColor::UnusualFromARGB(0xff056139));
static constexpr gfx::sRGBColor sColorMeterYellow10(
    gfx::sRGBColor::UnusualFromARGB(0xffffbd4f));
static constexpr gfx::sRGBColor sColorMeterYellow20(
    gfx::sRGBColor::UnusualFromARGB(0xffd2811e));
static constexpr gfx::sRGBColor sColorMeterRed10(
    gfx::sRGBColor::UnusualFromARGB(0xffe22850));
static constexpr gfx::sRGBColor sColorMeterRed20(
    gfx::sRGBColor::UnusualFromARGB(0xff810220));

static const CSSCoord kMinimumDropdownArrowButtonWidth = 18.0f;
static const CSSCoord kMinimumSpinnerButtonWidth = 18.0f;
static const CSSCoord kMinimumSpinnerButtonHeight = 9.0f;
static const CSSCoord kButtonBorderWidth = 1.0f;
static const CSSCoord kRangeOutlineOffset = 1.0f;
static const CSSCoord kTextFieldBorderWidth = 1.0f;
static const CSSCoord kRangeHeight = 6.0f;
static const CSSCoord kProgressbarHeight = 6.0f;
static const CSSCoord kMeterHeight = 12.0f;

static constexpr CSSCoord kCheckboxRadioBorderWidth = 1.0f;
static constexpr CSSCoord kCheckboxRadioSize = 14.0f;

static constexpr sRGBColor sTransparent = sRGBColor::White(0.0);

// This pushes and pops a clip rect to the draw target.
//
// This is done to reduce fuzz in places where we may have antialiasing,
// because skia is not clip-invariant: given different clips, it does not
// guarantee the same result, even if the painted content doesn't intersect
// the clips.
//
// This is a bit sad, overall, but...
struct MOZ_RAII AutoClipRect {
  AutoClipRect(DrawTarget& aDt, const LayoutDeviceRect& aRect) : mDt(aDt) {
    mDt.PushClipRect(aRect.ToUnknownRect());
  }

  ~AutoClipRect() { mDt.PopClip(); }

 private:
  DrawTarget& mDt;
};

static StaticRefPtr<Theme> gNativeInstance;
static StaticRefPtr<Theme> gNonNativeInstance;
static StaticRefPtr<Theme> gRDMInstance;

}  // namespace

#ifdef ANDROID
already_AddRefed<Theme> do_CreateNativeThemeDoNotUseDirectly() {
  // Android doesn't have a native theme.
  return do_AddRef(new Theme(Theme::ScrollbarStyle()));
}
#endif

already_AddRefed<nsITheme> do_GetBasicNativeThemeDoNotUseDirectly() {
  if (MOZ_UNLIKELY(!gNonNativeInstance)) {
    UniquePtr<ScrollbarDrawing> scrollbarDrawing = Theme::ScrollbarStyle();
#ifdef MOZ_WIDGET_COCOA
    gNonNativeInstance = new ThemeCocoa(std::move(scrollbarDrawing));
#else
    gNonNativeInstance = new Theme(std::move(scrollbarDrawing));
#endif
    ClearOnShutdown(&gNonNativeInstance);
  }
  return do_AddRef(gNonNativeInstance);
}

already_AddRefed<nsITheme> do_GetNativeThemeDoNotUseDirectly() {
  if (MOZ_UNLIKELY(!gNativeInstance)) {
    gNativeInstance = do_CreateNativeThemeDoNotUseDirectly();
    ClearOnShutdown(&gNativeInstance);
  }
  return do_AddRef(gNativeInstance);
}

already_AddRefed<nsITheme> do_GetRDMThemeDoNotUseDirectly() {
  if (MOZ_UNLIKELY(!gRDMInstance)) {
    gRDMInstance = new Theme(MakeUnique<ScrollbarDrawingAndroid>());
    ClearOnShutdown(&gRDMInstance);
  }
  return do_AddRef(gRDMInstance);
}

namespace mozilla::widget {

NS_IMPL_ISUPPORTS_INHERITED(Theme, nsNativeTheme, nsITheme)

static constexpr nsLiteralCString kPrefs[] = {
    "widget.non-native-theme.use-theme-accent"_ns,
    "widget.non-native-theme.win.scrollbar.use-system-size"_ns,
    "widget.non-native-theme.scrollbar.size.override"_ns,
    "widget.non-native-theme.scrollbar.style"_ns,
};

void Theme::Init() {
  for (const auto& pref : kPrefs) {
    Preferences::RegisterCallback(PrefChangedCallback, pref);
  }
  LookAndFeelChanged();
}

void Theme::Shutdown() {
  for (const auto& pref : kPrefs) {
    Preferences::UnregisterCallback(PrefChangedCallback, pref);
  }
}

/* static */
void Theme::LookAndFeelChanged() {
  ThemeColors::RecomputeAccentColors();
  if (gNonNativeInstance) {
    gNonNativeInstance->SetScrollbarDrawing(ScrollbarStyle());
  }
  if (gNativeInstance) {
    gNativeInstance->SetScrollbarDrawing(ScrollbarStyle());
  }
}

auto Theme::GetDPIRatio(nsIFrame* aFrame, StyleAppearance aAppearance)
    -> DPIRatio {
  // Widgets react to zoom, except scrollbars.
  nsPresContext* pc = aFrame->PresContext();
  if (IsWidgetScrollbarPart(aAppearance)) {
    return GetScrollbarDrawing().GetDPIRatioForScrollbarPart(pc);
  }
  return DPIRatio(aFrame->Style()->EffectiveZoom().Zoom(
      float(AppUnitsPerCSSPixel()) / pc->AppUnitsPerDevPixel()));
}

// Checkbox and radio need to preserve aspect-ratio for compat. We also snap the
// size to exact device pixels to avoid snapping disorting the circles.
static LayoutDeviceRect CheckBoxRadioRect(const LayoutDeviceRect& aRect) {
  // Place a square rect in the center of aRect.
  auto size = std::trunc(std::min(aRect.width, aRect.height));
  auto position = aRect.Center() - LayoutDevicePoint(size * 0.5, size * 0.5);
  return LayoutDeviceRect(position, LayoutDeviceSize(size, size));
}

std::tuple<sRGBColor, sRGBColor, sRGBColor> Theme::ComputeCheckboxColors(
    const ElementState& aState, StyleAppearance aAppearance,
    const Colors& aColors) {
  MOZ_ASSERT(aAppearance == StyleAppearance::Checkbox ||
             aAppearance == StyleAppearance::Radio);

  bool isDisabled = aState.HasState(ElementState::DISABLED);
  bool isChecked = aState.HasState(ElementState::CHECKED);
  bool isIndeterminate = aAppearance == StyleAppearance::Checkbox &&
                         aState.HasState(ElementState::INDETERMINATE);

  if (isChecked || isIndeterminate) {
    if (isDisabled) {
      auto bg = ComputeBorderColor(aState, aColors, OutlineCoversBorder::No);
      auto fg = aColors.HighContrast()
                    ? aColors.System(StyleSystemColor::Buttonface)
                    : sRGBColor::White(aColors.IsDark() ? .4f : .8f);
      return std::make_tuple(bg, bg, fg);
    }

    bool isActive =
        aState.HasAllStates(ElementState::HOVER | ElementState::ACTIVE);
    bool isHovered = aState.HasState(ElementState::HOVER);

    if (aColors.HighContrast()) {
      sRGBColor border = (isHovered && !isActive)
                             ? aColors.System(StyleSystemColor::Selecteditem)
                             : aColors.System(StyleSystemColor::Buttontext);
      auto bg = (isHovered || isActive)
                    ? aColors.System(StyleSystemColor::Selecteditem)
                    : aColors.System(StyleSystemColor::Buttontext);
      auto fg = (isHovered || isActive)
                    ? aColors.System(StyleSystemColor::Selecteditemtext)
                    : aColors.System(StyleSystemColor::Buttonface);
      return std::make_tuple(bg, border, fg);
    }

    const auto& bg = isActive    ? aColors.Accent().GetDarker()
                     : isHovered ? aColors.Accent().GetDark()
                                 : aColors.Accent().Get();
    const auto& fg = aColors.Accent().GetForeground();
    return std::make_tuple(bg, bg, fg);
  }

  auto [bg, border] =
      ComputeTextfieldColors(aState, aColors, OutlineCoversBorder::No);
  // We don't paint a checkmark in this case so any color would do.
  return std::make_tuple(bg, border, sTransparent);
}

sRGBColor Theme::ComputeBorderColor(const ElementState& aState,
                                    const Colors& aColors,
                                    OutlineCoversBorder aOutlineCoversBorder) {
  bool isDisabled = aState.HasState(ElementState::DISABLED);
  bool isActive =
      aState.HasAllStates(ElementState::HOVER | ElementState::ACTIVE);
  bool isHovered = aState.HasState(ElementState::HOVER);
  if (isDisabled) {
    return aColors.System(StyleSystemColor::MozButtondisabledborder);
  }
  bool isFocused = aState.HasState(ElementState::FOCUSRING);
  if (isFocused && aOutlineCoversBorder == OutlineCoversBorder::Yes) {
    // If we draw the outline over the border, prevent issues where the border
    // shows underneath if it snaps in the wrong direction by using a
    // transparent border. An alternative to this is ensuring that we snap the
    // offset in PaintRoundedFocusRect the same was a we snap border widths, so
    // that negative offsets are guaranteed to cover the border.
    // But this looks harder to mess up.
    return sTransparent;
  }
  if (isActive) {
    return aColors.System(StyleSystemColor::MozButtonactiveborder);
  }
  if (isHovered) {
    return aColors.System(StyleSystemColor::MozButtonhoverborder);
  }
  return aColors.System(StyleSystemColor::Buttonborder);
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeButtonColors(
    const ElementState& aState, const Colors& aColors) {
  bool isActive =
      aState.HasAllStates(ElementState::HOVER | ElementState::ACTIVE);
  bool isDisabled = aState.HasState(ElementState::DISABLED);
  bool isHovered = aState.HasState(ElementState::HOVER);

  nscolor backgroundColor = [&] {
    if (aState.HasState(ElementState::AUTOFILL)) {
      return aColors.SystemNs(StyleSystemColor::MozAutofillBackground);
    }
    if (isDisabled) {
      return aColors.SystemNs(StyleSystemColor::MozButtondisabledface);
    }
    if (isActive) {
      return aColors.SystemNs(StyleSystemColor::MozButtonactiveface);
    }
    if (isHovered) {
      return aColors.SystemNs(StyleSystemColor::MozButtonhoverface);
    }
    return aColors.SystemNs(StyleSystemColor::Buttonface);
  }();
  const sRGBColor borderColor =
      ComputeBorderColor(aState, aColors, OutlineCoversBorder::Yes);
  return std::make_pair(sRGBColor::FromABGR(backgroundColor), borderColor);
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeTextfieldColors(
    const ElementState& aState, const Colors& aColors,
    OutlineCoversBorder aOutlineCoversBorder) {
  nscolor backgroundColor = [&] {
    if (aState.HasState(ElementState::AUTOFILL)) {
      return aColors.SystemNs(StyleSystemColor::MozAutofillBackground);
    }
    if (aState.HasState(ElementState::DISABLED)) {
      return aColors.SystemNs(StyleSystemColor::MozDisabledfield);
    }
    return aColors.SystemNs(StyleSystemColor::Field);
  }();

  const sRGBColor borderColor =
      ComputeBorderColor(aState, aColors, aOutlineCoversBorder);
  return std::make_pair(sRGBColor::FromABGR(backgroundColor), borderColor);
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeRangeProgressColors(
    const ElementState& aState, const Colors& aColors) {
  bool isActive =
      aState.HasAllStates(ElementState::HOVER | ElementState::ACTIVE);
  bool isDisabled = aState.HasState(ElementState::DISABLED);
  bool isHovered = aState.HasState(ElementState::HOVER);

  if (aColors.HighContrast()) {
    if (isDisabled) {
      return aColors.SystemPair(StyleSystemColor::Graytext,
                                StyleSystemColor::Graytext);
    }
    if (isActive) {
      return aColors.SystemPair(StyleSystemColor::Selecteditem,
                                StyleSystemColor::Buttontext);
    }
    if (isHovered) {
      return aColors.SystemPair(StyleSystemColor::Selecteditem,
                                StyleSystemColor::Selecteditem);
    }
    return aColors.SystemPair(StyleSystemColor::Buttontext,
                              StyleSystemColor::Buttontext);
  }

  if (isDisabled) {
    return std::make_pair(sColorGrey40Alpha50, sColorGrey40Alpha50);
  }
  if (isActive || isHovered) {
    return std::make_pair(aColors.Accent().GetDark(),
                          aColors.Accent().GetDarker());
  }
  return std::make_pair(aColors.Accent().Get(), aColors.Accent().GetDark());
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeRangeTrackColors(
    const ElementState& aState, const Colors& aColors) {
  bool isActive =
      aState.HasAllStates(ElementState::HOVER | ElementState::ACTIVE);
  bool isDisabled = aState.HasState(ElementState::DISABLED);
  bool isHovered = aState.HasState(ElementState::HOVER);

  if (aColors.HighContrast()) {
    if (isDisabled) {
      return aColors.SystemPair(StyleSystemColor::Buttonface,
                                StyleSystemColor::Graytext);
    }
    if (isActive) {
      return aColors.SystemPair(StyleSystemColor::Buttonface,
                                StyleSystemColor::Buttontext);
    }
    if (isHovered) {
      return aColors.SystemPair(StyleSystemColor::Selecteditemtext,
                                StyleSystemColor::Selecteditem);
    }
    return aColors.SystemPair(StyleSystemColor::Buttonface,
                              StyleSystemColor::Buttontext);
  }

  if (isDisabled) {
    return std::make_pair(sColorGrey10Alpha50, sColorGrey40Alpha50);
  }
  if (isActive || isHovered) {
    return std::make_pair(sColorGrey20, sColorGrey50);
  }
  return std::make_pair(sColorGrey10, sColorGrey40);
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeRangeThumbColors(
    const ElementState& aState, const Colors& aColors) {
  bool isActive =
      aState.HasAllStates(ElementState::HOVER | ElementState::ACTIVE);
  bool isDisabled = aState.HasState(ElementState::DISABLED);
  bool isHovered = aState.HasState(ElementState::HOVER);

  if (aColors.HighContrast()) {
    if (isDisabled) {
      return aColors.SystemPair(StyleSystemColor::Buttonface,
                                StyleSystemColor::Graytext);
    }
    if (isActive || isHovered) {
      return aColors.SystemPair(StyleSystemColor::Selecteditemtext,
                                StyleSystemColor::Selecteditem);
    }
    return aColors.SystemPair(StyleSystemColor::Buttonface,
                              StyleSystemColor::Buttontext);
  }

  const sRGBColor& backgroundColor = [&] {
    if (isDisabled) {
      return sColorGrey40;
    }
    if (isActive) {
      return aColors.Accent().Get();
    }
    if (isHovered) {
      return sColorGrey60;
    }
    return sColorGrey50;
  }();

  const sRGBColor borderColor = sRGBColor::OpaqueWhite();
  return std::make_pair(backgroundColor, borderColor);
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeProgressColors(
    const Colors& aColors) {
  if (aColors.HighContrast()) {
    return aColors.SystemPair(StyleSystemColor::Selecteditem,
                              StyleSystemColor::Buttontext);
  }
  return std::make_pair(aColors.Accent().Get(), aColors.Accent().GetDark());
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeProgressTrackColors(
    const Colors& aColors) {
  if (aColors.HighContrast()) {
    return aColors.SystemPair(StyleSystemColor::Buttonface,
                              StyleSystemColor::Buttontext);
  }
  return std::make_pair(sColorGrey10, sColorGrey40);
}

std::pair<sRGBColor, sRGBColor> Theme::ComputeMeterchunkColors(
    const ElementState& aMeterState, const Colors& aColors) {
  if (aColors.HighContrast()) {
    return ComputeProgressColors(aColors);
  }
  sRGBColor borderColor = sColorMeterGreen20;
  sRGBColor chunkColor = sColorMeterGreen10;

  if (aMeterState.HasState(ElementState::SUB_OPTIMUM)) {
    borderColor = sColorMeterYellow20;
    chunkColor = sColorMeterYellow10;
  } else if (aMeterState.HasState(ElementState::SUB_SUB_OPTIMUM)) {
    borderColor = sColorMeterRed20;
    chunkColor = sColorMeterRed10;
  }

  return std::make_pair(chunkColor, borderColor);
}

void Theme::PaintCheckboxControl(DrawTarget& aDrawTarget,
                                 const LayoutDeviceRect& aRect,
                                 const ElementState& aState,
                                 const Colors& aColors, DPIRatio aDpiRatio) {
  auto [backgroundColor, borderColor, checkColor] =
      ComputeCheckboxColors(aState, StyleAppearance::Checkbox, aColors);
  const CSSCoord radius = 2.0f;
  {
    CSSCoord borderWidth = kCheckboxRadioBorderWidth;
    if (backgroundColor == borderColor) {
      borderWidth = 0.0f;
    }
    ThemeDrawing::PaintRoundedRectWithRadius(aDrawTarget, aRect,
                                             backgroundColor, borderColor,
                                             borderWidth, radius, aDpiRatio);
  }

  if (aState.HasState(ElementState::INDETERMINATE)) {
    PaintIndeterminateMark(aDrawTarget, aRect, checkColor);
  } else if (aState.HasState(ElementState::CHECKED)) {
    PaintCheckMark(aDrawTarget, aRect, checkColor);
  }

  if (aState.HasState(ElementState::FOCUSRING)) {
    PaintAutoStyleOutline(aDrawTarget, aRect, aColors, radius, 0.0f,
                          InvertColors::Yes, aDpiRatio);
  }
}

void Theme::PaintCheckMark(DrawTarget& aDrawTarget,
                           const LayoutDeviceRect& aRect,
                           const sRGBColor& aColor) {
  // Points come from the coordinates on a 14X14 (kCheckboxRadioSize)
  // unit box centered at 0,0
  const float checkPolygonX[] = {-4.5f, -1.5f, -0.5f, 5.0f, 4.75f,
                                 3.5f,  -0.5f, -1.5f, -3.5f};
  const float checkPolygonY[] = {0.5f,  4.0f, 4.0f,  -2.5f, -4.0f,
                                 -4.0f, 1.0f, 1.25f, -1.0f};
  const int32_t checkNumPoints = sizeof(checkPolygonX) / sizeof(float);
  const float scale = ThemeDrawing::ScaleToFillRect(aRect, kCheckboxRadioSize);
  auto center = aRect.Center().ToUnknownPoint();

  RefPtr<PathBuilder> builder = aDrawTarget.CreatePathBuilder();
  Point p = center + Point(checkPolygonX[0] * scale, checkPolygonY[0] * scale);
  builder->MoveTo(p);
  for (int32_t i = 1; i < checkNumPoints; i++) {
    p = center + Point(checkPolygonX[i] * scale, checkPolygonY[i] * scale);
    builder->LineTo(p);
  }
  RefPtr<Path> path = builder->Finish();

  aDrawTarget.Fill(path, ColorPattern(ToDeviceColor(aColor)));
}

void Theme::PaintIndeterminateMark(DrawTarget& aDrawTarget,
                                   const LayoutDeviceRect& aRect,
                                   const sRGBColor& aColor) {
  const CSSCoord borderWidth = kCheckboxRadioBorderWidth;
  const float scale = ThemeDrawing::ScaleToFillRect(aRect, kCheckboxRadioSize);
  Rect rect = aRect.ToUnknownRect();
  rect.y += (rect.height / 2) - (borderWidth * scale / 2);
  rect.height = borderWidth * scale;
  rect.x += (borderWidth * scale) + (borderWidth * scale / 8);
  rect.width -= ((borderWidth * scale) + (borderWidth * scale / 8)) * 2;

  aDrawTarget.FillRect(rect, ColorPattern(ToDeviceColor(aColor)));
}

template <typename PaintBackendData>
void Theme::PaintStrokedCircle(PaintBackendData& aPaintData,
                               const LayoutDeviceRect& aRect,
                               const sRGBColor& aBackgroundColor,
                               const sRGBColor& aBorderColor,
                               const CSSCoord aBorderWidth,
                               DPIRatio aDpiRatio) {
  auto radius = LayoutDeviceCoord(aRect.Size().width) / aDpiRatio;
  ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, aRect, aBackgroundColor,
                                           aBorderColor, aBorderWidth, radius,
                                           aDpiRatio);
}

void Theme::PaintCircleShadow(WebRenderBackendData& aWrData,
                              const LayoutDeviceRect& aBoxRect,
                              const LayoutDeviceRect& aClipRect,
                              float aShadowAlpha, const CSSPoint& aShadowOffset,
                              CSSCoord aShadowBlurStdDev, DPIRatio aDpiRatio) {
  const bool kBackfaceIsVisible = true;
  const LayoutDeviceCoord stdDev = aShadowBlurStdDev * aDpiRatio;
  const LayoutDevicePoint shadowOffset = aShadowOffset * aDpiRatio;
  const IntSize inflation =
      gfxAlphaBoxBlur::CalculateBlurRadius(gfxPoint(stdDev, stdDev));
  LayoutDeviceRect shadowRect = aBoxRect;
  shadowRect.MoveBy(shadowOffset);
  shadowRect.Inflate(inflation.width, inflation.height);
  const auto boxRect = wr::ToLayoutRect(aBoxRect);
  aWrData.mBuilder.PushBoxShadow(
      wr::ToLayoutRect(shadowRect), wr::ToLayoutRect(aClipRect),
      kBackfaceIsVisible, boxRect,
      wr::ToLayoutVector2D(aShadowOffset * aDpiRatio),
      wr::ToColorF(DeviceColor(0.0f, 0.0f, 0.0f, aShadowAlpha)), stdDev,
      /* aSpread = */ 0.0f,
      wr::ToBorderRadius(gfx::RectCornerRadii(aBoxRect.Size().width)),
      wr::BoxShadowClipMode::Outset);
}

void Theme::PaintCircleShadow(DrawTarget& aDrawTarget,
                              const LayoutDeviceRect& aBoxRect,
                              const LayoutDeviceRect& aClipRect,
                              float aShadowAlpha, const CSSPoint& aShadowOffset,
                              CSSCoord aShadowBlurStdDev, DPIRatio aDpiRatio) {
  Float stdDev = aShadowBlurStdDev * aDpiRatio;
  Point offset = (aShadowOffset * aDpiRatio).ToUnknownPoint();

  RefPtr<FilterNode> blurFilter =
      aDrawTarget.CreateFilter(FilterType::GAUSSIAN_BLUR);
  if (!blurFilter) {
    return;
  }

  blurFilter->SetAttribute(ATT_GAUSSIAN_BLUR_STD_DEVIATION, stdDev);

  IntSize inflation =
      gfxAlphaBoxBlur::CalculateBlurRadius(gfxPoint(stdDev, stdDev));
  Rect inflatedRect = aBoxRect.ToUnknownRect();
  inflatedRect.Inflate(inflation.width, inflation.height);
  Rect sourceRectInFilterSpace =
      inflatedRect - aBoxRect.TopLeft().ToUnknownPoint();
  Point destinationPointOfSourceRect = inflatedRect.TopLeft() + offset;

  IntSize dtSize = RoundedToInt(aBoxRect.Size().ToUnknownSize());
  if (dtSize.IsEmpty()) {
    return;
  }
  RefPtr<DrawTarget> ellipseDT = aDrawTarget.CreateSimilarDrawTargetForFilter(
      dtSize, SurfaceFormat::A8, blurFilter, blurFilter,
      sourceRectInFilterSpace, destinationPointOfSourceRect);
  if (!ellipseDT) {
    return;
  }

  AutoClipRect clipRect(aDrawTarget, aClipRect);

  RefPtr<Path> ellipse = MakePathForEllipse(
      *ellipseDT, (aBoxRect - aBoxRect.TopLeft()).Center().ToUnknownPoint(),
      aBoxRect.Size().ToUnknownSize());
  ellipseDT->Fill(ellipse,
                  ColorPattern(DeviceColor(0.0f, 0.0f, 0.0f, aShadowAlpha)));
  RefPtr<SourceSurface> ellipseSurface = ellipseDT->Snapshot();

  blurFilter->SetInput(IN_GAUSSIAN_BLUR_IN, ellipseSurface);
  aDrawTarget.DrawFilter(blurFilter, sourceRectInFilterSpace,
                         destinationPointOfSourceRect);
}

template <typename PaintBackendData>
void Theme::PaintRadioControl(PaintBackendData& aPaintData,
                              const LayoutDeviceRect& aRect,
                              const ElementState& aState, const Colors& aColors,
                              DPIRatio aDpiRatio) {
  auto [backgroundColor, borderColor, checkColor] =
      ComputeCheckboxColors(aState, StyleAppearance::Radio, aColors);
  {
    CSSCoord borderWidth = kCheckboxRadioBorderWidth;
    if (backgroundColor == borderColor) {
      borderWidth = 0.0f;
    }
    PaintStrokedCircle(aPaintData, aRect, backgroundColor, borderColor,
                       borderWidth, aDpiRatio);
  }

  if (aState.HasState(ElementState::CHECKED)) {
    // See bug 1951930 and bug 1941755 for some discussion on this chunk of
    // code.
    const CSSCoord kOuterBorderWidth = 1.0f;
    const CSSCoord kInnerBorderWidth = 2.0f;
    LayoutDeviceRect rect(aRect);
    auto width = LayoutDeviceCoord(
        ThemeDrawing::SnapBorderWidth(kOuterBorderWidth, aDpiRatio));
    rect.Deflate(width);
    PaintStrokedCircle(aPaintData, rect, backgroundColor, checkColor,
                       kInnerBorderWidth, aDpiRatio);
  }

  if (aState.HasState(ElementState::FOCUSRING)) {
    CSSCoord radius = LayoutDeviceCoord(aRect.Size().width) / aDpiRatio;
    PaintAutoStyleOutline(aPaintData, aRect, aColors, radius, 0.0f,
                          InvertColors::Yes, aDpiRatio);
  }
}

template <typename PaintBackendData>
void Theme::PaintTextField(PaintBackendData& aPaintData,
                           const LayoutDeviceRect& aRect,
                           const ElementState& aState, const Colors& aColors,
                           DPIRatio aDpiRatio) {
  auto [backgroundColor, borderColor] =
      ComputeTextfieldColors(aState, aColors, OutlineCoversBorder::Yes);

  const CSSCoord radius = 2.0f;

  ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, aRect, backgroundColor,
                                           borderColor, kTextFieldBorderWidth,
                                           radius, aDpiRatio);

  if (aState.HasState(ElementState::FOCUSRING)) {
    PaintAutoStyleOutline(aPaintData, aRect, aColors, radius,
                          -kTextFieldBorderWidth, InvertColors::No, aDpiRatio);
  }
}

template <typename PaintBackendData>
void Theme::PaintListbox(PaintBackendData& aPaintData,
                         const LayoutDeviceRect& aRect,
                         const ElementState& aState, const Colors& aColors,
                         DPIRatio aDpiRatio) {
  // We happen to share style between text fields and list boxes.
  return PaintTextField(aPaintData, aRect, aState, aColors, aDpiRatio);
}

template <typename PaintBackendData>
void Theme::PaintMenulist(PaintBackendData& aPaintData,
                          const LayoutDeviceRect& aRect,
                          const ElementState& aState, const Colors& aColors,
                          DPIRatio aDpiRatio) {
  // We share styles between menulists and buttons.
  return PaintButton(aPaintData, aRect, StyleAppearance::Menulist, aState,
                     aColors, aDpiRatio);
}

enum class PhysicalArrowDirection {
  Right,
  Left,
  Bottom,
};

void Theme::PaintMenulistArrow(nsIFrame* aFrame, DrawTarget& aDrawTarget,
                               const LayoutDeviceRect& aRect) {
  // not const: these may be negated in-place below
  float polygonX[] = {-4.0f, -0.5f, 0.5f, 4.0f,  4.0f,
                      3.0f,  0.0f,  0.0f, -3.0f, -4.0f};
  float polygonY[] = {-1,    3.0f, 3.0f, -1.0f, -2.0f,
                      -2.0f, 1.5f, 1.5f, -2.0f, -2.0f};

  const float kPolygonSize = kMinimumDropdownArrowButtonWidth;
  const auto direction = [&] {
    const auto wm = aFrame->GetWritingMode();
    switch (wm.GetBlockDir()) {
      case WritingMode::BlockDir::LR:
        return PhysicalArrowDirection::Right;
      case WritingMode::BlockDir::RL:
        return PhysicalArrowDirection::Left;
      case WritingMode::BlockDir::TB:
        return PhysicalArrowDirection::Bottom;
    }
    MOZ_ASSERT_UNREACHABLE("Unknown direction?");
    return PhysicalArrowDirection::Bottom;
  }();

  auto const [xs, ys] = [&] {
    using Pair = std::pair<const float*, const float*>;
    switch (direction) {
      case PhysicalArrowDirection::Left:
        // rotate 90°: [[0,1],[-1,0]]
        for (float& f : polygonY) {
          f = -f;
        }
        return Pair(polygonY, polygonX);

      case PhysicalArrowDirection::Right:
        // rotate 270°: [[0,-1],[1,0]]
        for (float& f : polygonX) {
          f = -f;
        }
        return Pair(polygonY, polygonX);

      case PhysicalArrowDirection::Bottom:
        // rotate 0°: [[1,0],[0,1]]
        return Pair(polygonX, polygonY);
    }
    MOZ_ASSERT_UNREACHABLE("Unknown direction?");
    return Pair(polygonX, polygonY);
  }();

  const auto arrowColor = sRGBColor::FromABGR(
      nsLayoutUtils::GetTextColor(aFrame, &nsStyleText::mWebkitTextFillColor));
  ThemeDrawing::PaintArrow(aDrawTarget, aRect, xs, ys, kPolygonSize,
                           std::size(polygonX), arrowColor);
}

void Theme::PaintSpinnerButton(nsIFrame* aFrame, DrawTarget& aDrawTarget,
                               const LayoutDeviceRect& aRect,
                               const ElementState& aState,
                               StyleAppearance aAppearance,
                               const Colors& aColors, DPIRatio aDpiRatio) {
  auto [backgroundColor, borderColor] = ComputeButtonColors(aState, aColors);

  aDrawTarget.FillRect(aRect.ToUnknownRect(),
                       ColorPattern(ToDeviceColor(backgroundColor)));

  const float kPolygonX[] = {-3.5f, -0.5f, 0.5f, 3.5f,  3.5f,
                             2.5f,  0.0f,  0.0f, -2.5f, -3.5f};
  float polygonY[] = {-1.5f, 1.5f, 1.5f, -1.5f, -2.5f,
                      -2.5f, 0.0f, 0.0f, -2.5f, -2.5f};

  const float kPolygonSize = kMinimumSpinnerButtonHeight;
  if (aAppearance == StyleAppearance::SpinnerUpbutton) {
    for (auto& coord : polygonY) {
      coord = -coord;
    }
  }

  ThemeDrawing::PaintArrow(aDrawTarget, aRect, kPolygonX, polygonY,
                           kPolygonSize, std::size(kPolygonX), borderColor);
}

template <typename PaintBackendData>
void Theme::PaintRange(nsIFrame* aFrame, PaintBackendData& aPaintData,
                       const LayoutDeviceRect& aRect,
                       const ElementState& aState, const Colors& aColors,
                       DPIRatio aDpiRatio, bool aHorizontal) {
  nsRangeFrame* rangeFrame = do_QueryFrame(aFrame);
  if (!rangeFrame) {
    return;
  }

  auto tickMarks = rangeFrame->TickMarks();
  double progress = rangeFrame->GetValueAsFractionOfRange();
  auto rect = aRect;
  const CSSCoord minThumbSize = GetMinimumRangeThumbSize();
  LayoutDeviceRect thumbRect(0, 0, minThumbSize * aDpiRatio,
                             minThumbSize * aDpiRatio);
  LayoutDeviceRect progressClipRect(aRect);
  LayoutDeviceRect trackClipRect(aRect);
  const LayoutDeviceCoord verticalSize = kRangeHeight * aDpiRatio;
  const LayoutDeviceCoord tickMarkWidth(
      ThemeDrawing::SnapBorderWidth(1.0f, aDpiRatio));
  const LayoutDeviceCoord tickMarkHeight(
      ThemeDrawing::SnapBorderWidth(5.0f, aDpiRatio));
  LayoutDevicePoint tickMarkOrigin, tickMarkDirection;
  LayoutDeviceSize tickMarkSize;
  if (aHorizontal) {
    rect.height = verticalSize;
    rect.y = aRect.y + (aRect.height - rect.height) / 2;
    tickMarkSize = LayoutDeviceSize(tickMarkWidth, tickMarkHeight);
    thumbRect.y = aRect.y + (aRect.height - thumbRect.height) / 2;

    if (IsFrameRTL(aFrame)) {
      tickMarkOrigin =
          LayoutDevicePoint(aRect.XMost() - thumbRect.width / 2, aRect.YMost());
      tickMarkDirection = LayoutDevicePoint(-1.0f, 0.0f);
      thumbRect.x =
          aRect.x + (aRect.width - thumbRect.width) * (1.0 - progress);
      float midPoint = thumbRect.Center().X();
      trackClipRect.SetBoxX(aRect.X(), midPoint);
      progressClipRect.SetBoxX(midPoint, aRect.XMost());
    } else {
      tickMarkOrigin =
          LayoutDevicePoint(aRect.x + thumbRect.width / 2, aRect.YMost());
      tickMarkDirection = LayoutDevicePoint(1.0, 0.0f);
      thumbRect.x = aRect.x + (aRect.width - thumbRect.width) * progress;
      float midPoint = thumbRect.Center().X();
      progressClipRect.SetBoxX(aRect.X(), midPoint);
      trackClipRect.SetBoxX(midPoint, aRect.XMost());
    }
  } else {
    rect.width = verticalSize;
    rect.x = aRect.x + (aRect.width - rect.width) / 2;
    tickMarkOrigin = LayoutDevicePoint(aRect.XMost() - tickMarkHeight / 4,
                                       aRect.YMost() - thumbRect.width / 2);
    tickMarkDirection = LayoutDevicePoint(0.0f, -1.0f);
    tickMarkSize = LayoutDeviceSize(tickMarkHeight, tickMarkWidth);
    thumbRect.x = aRect.x + (aRect.width - thumbRect.width) / 2;

    if (rangeFrame->IsUpwards()) {
      thumbRect.y =
          aRect.y + (aRect.height - thumbRect.height) * (1.0 - progress);
      float midPoint = thumbRect.Center().Y();
      trackClipRect.SetBoxY(aRect.Y(), midPoint);
      progressClipRect.SetBoxY(midPoint, aRect.YMost());
    } else {
      thumbRect.y = aRect.y + (aRect.height - thumbRect.height) * progress;
      float midPoint = thumbRect.Center().Y();
      trackClipRect.SetBoxY(midPoint, aRect.YMost());
      progressClipRect.SetBoxY(aRect.Y(), midPoint);
    }
  }

  const CSSCoord borderWidth = 1.0f;
  const CSSCoord radius = 3.0f;

  auto [progressColor, progressBorderColor] =
      ComputeRangeProgressColors(aState, aColors);
  auto [trackColor, trackBorderColor] =
      ComputeRangeTrackColors(aState, aColors);
  auto tickMarkColor = trackBorderColor;

  ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, rect, progressClipRect,
                                           progressColor, progressBorderColor,
                                           borderWidth, radius, aDpiRatio);

  ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, rect, trackClipRect,
                                           trackColor, trackBorderColor,
                                           borderWidth, radius, aDpiRatio);

  if (!aState.HasState(ElementState::DISABLED)) {
    // Ensure the shadow doesn't expand outside of our overflow rect declared in
    // GetWidgetOverflow().
    auto overflowRect = aRect;
    overflowRect.Inflate(CSSCoord(6.0f) * aDpiRatio);
    // Thumb shadow
    PaintCircleShadow(aPaintData, thumbRect, overflowRect, 0.3f,
                      CSSPoint(0.0f, 2.0f), 2.0f, aDpiRatio);
  }

  tickMarkDirection.x *= aRect.width - thumbRect.width;
  tickMarkDirection.y *= aRect.height - thumbRect.height;
  tickMarkOrigin -=
      LayoutDevicePoint(tickMarkSize.width, tickMarkSize.height) / 2;
  auto tickMarkRect = LayoutDeviceRect(tickMarkOrigin, tickMarkSize);
  for (auto tickMark : tickMarks) {
    auto tickMarkOffset =
        tickMarkDirection *
        float(rangeFrame->GetDoubleAsFractionOfRange(tickMark));
    ThemeDrawing::FillRect(aPaintData, tickMarkRect + tickMarkOffset,
                           tickMarkColor);
  }

  // Draw the thumb on top.
  const CSSCoord thumbBorderWidth = 2.0f;
  auto [thumbColor, thumbBorderColor] =
      ComputeRangeThumbColors(aState, aColors);

  PaintStrokedCircle(aPaintData, thumbRect, thumbColor, thumbBorderColor,
                     thumbBorderWidth, aDpiRatio);

  if (aState.HasState(ElementState::FOCUSRING)) {
    PaintAutoStyleOutline(aPaintData, aRect, aColors, radius,
                          kRangeOutlineOffset, InvertColors::No, aDpiRatio);
  }
}

template <typename PaintBackendData>
void Theme::PaintProgress(nsIFrame* aFrame, PaintBackendData& aPaintData,
                          const LayoutDeviceRect& aRect,
                          const ElementState& aState, const Colors& aColors,
                          DPIRatio aDpiRatio, bool aIsMeter) {
  const CSSCoord borderWidth = 1.0f;
  const CSSCoord radius = aIsMeter ? 6.0f : 3.0f;

  LayoutDeviceRect rect(aRect);
  const LayoutDeviceCoord thickness =
      (aIsMeter ? kMeterHeight : kProgressbarHeight) * aDpiRatio;

  const bool isHorizontal = !nsNativeTheme::IsVerticalProgress(aFrame);
  if (isHorizontal) {
    // Center it vertically.
    rect.y += (rect.height - thickness) / 2;
    rect.height = thickness;
  } else {
    // Center it horizontally.
    rect.x += (rect.width - thickness) / 2;
    rect.width = thickness;
  }

  {
    // Paint the track, unclipped.
    auto [backgroundColor, borderColor] = ComputeProgressTrackColors(aColors);
    ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, rect, rect,
                                             backgroundColor, borderColor,
                                             borderWidth, radius, aDpiRatio);
  }

  // Now paint the chunk, clipped as needed.
  LayoutDeviceRect clipRect = rect;
  if (aState.HasState(ElementState::INDETERMINATE)) {
    // For indeterminate progress, we paint an animated chunk of 1/3 of the
    // progress size.
    //
    // Animation speed and math borrowed from GTK.
    const LayoutDeviceCoord size = isHorizontal ? rect.width : rect.height;
    const LayoutDeviceCoord barSize = size * 0.3333f;
    const LayoutDeviceCoord travel = 2.0f * (size - barSize);

    // Period equals to travel / pixelsPerMillisecond where pixelsPerMillisecond
    // equals progressSize / 1000.0.  This is equivalent to 1600.
    const unsigned kPeriod = 1600;

    const int t = PR_IntervalToMilliseconds(PR_IntervalNow()) % kPeriod;
    const LayoutDeviceCoord dx = travel * float(t) / float(kPeriod);
    if (isHorizontal) {
      rect.width = barSize;
      rect.x += (dx < travel * .5f) ? dx : travel - dx;
    } else {
      rect.height = barSize;
      rect.y += (dx < travel * .5f) ? dx : travel - dx;
    }
    clipRect = rect;
    // Queue the next frame if needed.
    if (!QueueAnimatedContentForRefresh(aFrame->GetContent(), 60)) {
      NS_WARNING("Couldn't refresh indeterminate <progress>");
    }
  } else {
    // This is the progress chunk, clip it to the right amount.
    double position = [&] {
      if (aIsMeter) {
        auto* meter = dom::HTMLMeterElement::FromNode(aFrame->GetContent());
        if (!meter) {
          return 0.0;
        }
        return meter->Position();
      }
      auto* progress = dom::HTMLProgressElement::FromNode(aFrame->GetContent());
      if (!progress) {
        return 0.0;
      }
      return progress->Position();
    }();
    if (isHorizontal) {
      double clipWidth = rect.width * position;
      clipRect.width = clipWidth;
      if (IsFrameRTL(aFrame)) {
        clipRect.x += rect.width - clipWidth;
      }
    } else {
      double clipHeight = rect.height * position;
      clipRect.height = clipHeight;
      clipRect.y += rect.height - clipHeight;
    }
  }

  auto [backgroundColor, borderColor] =
      aIsMeter ? ComputeMeterchunkColors(aState, aColors)
               : ComputeProgressColors(aColors);
  ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, rect, clipRect,
                                           backgroundColor, borderColor,
                                           borderWidth, radius, aDpiRatio);
}

template <typename PaintBackendData>
void Theme::PaintButton(PaintBackendData& aPaintData,
                        const LayoutDeviceRect& aRect,
                        StyleAppearance aAppearance, const ElementState& aState,
                        const Colors& aColors, DPIRatio aDpiRatio) {
  const CSSCoord radius = 4.0f;
  auto [backgroundColor, borderColor] = ComputeButtonColors(aState, aColors);

  if (aAppearance == StyleAppearance::Toolbarbutton &&
      (!aState.HasState(ElementState::HOVER) ||
       aState.HasState(ElementState::DISABLED))) {
    backgroundColor = borderColor = sTransparent;
  }

  ThemeDrawing::PaintRoundedRectWithRadius(aPaintData, aRect, backgroundColor,
                                           borderColor, kButtonBorderWidth,
                                           radius, aDpiRatio);

  if (aState.HasState(ElementState::FOCUSRING)) {
    PaintAutoStyleOutline(aPaintData, aRect, aColors, radius,
                          -kButtonBorderWidth, InvertColors::No, aDpiRatio);
  }
}

void Theme::DrawWidgetBackground(gfxContext* aContext, nsIFrame* aFrame,
                                 StyleAppearance aAppearance,
                                 const nsRect& aRect,
                                 const nsRect& /* aDirtyRect */,
                                 DrawOverflow aDrawOverflow) {
  DoDrawWidgetBackground(*aContext->GetDrawTarget(), aFrame, aAppearance, aRect,
                         aDrawOverflow);
}

bool Theme::CreateWebRenderCommandsForWidget(
    mozilla::wr::DisplayListBuilder& aBuilder,
    mozilla::wr::IpcResourceUpdateQueue& aResources,
    const mozilla::layers::StackingContextHelper& aSc,
    mozilla::layers::RenderRootStateManager* aManager, nsIFrame* aFrame,
    StyleAppearance aAppearance, const nsRect& aRect) {
  WebRenderBackendData data{aBuilder, aResources, aSc, aManager};
  return DoDrawWidgetBackground(data, aFrame, aAppearance, aRect,
                                DrawOverflow::Yes);
}

static LayoutDeviceRect ToSnappedRect(const nsRect& aRect,
                                      nscoord aTwipsPerPixel, DrawTarget& aDt) {
  return LayoutDeviceRect::FromUnknownRect(
      NSRectToSnappedRect(aRect, aTwipsPerPixel, aDt));
}

static LayoutDeviceRect ToSnappedRect(const nsRect& aRect,
                                      nscoord aTwipsPerPixel,
                                      WebRenderBackendData& aDt) {
  // TODO: Do we need to do any more snapping here?
  return LayoutDeviceRect::FromAppUnits(aRect, aTwipsPerPixel);
}

static ScrollbarDrawing::ScrollbarKind ComputeScrollbarKind(
    nsIFrame* aFrame, bool aIsHorizontal) {
  if (aIsHorizontal) {
    return ScrollbarDrawing::ScrollbarKind::Horizontal;
  }
  nsScrollbarFrame* scrollbar =
      ScrollbarDrawing::GetParentScrollbarFrame(aFrame);
  if (NS_WARN_IF(!scrollbar)) {
    return ScrollbarDrawing::ScrollbarKind::VerticalRight;
  }
  nsIScrollbarMediator* sm = scrollbar->GetScrollbarMediator();
  if (NS_WARN_IF(!sm)) {
    return ScrollbarDrawing::ScrollbarKind::VerticalRight;
  }
  return sm->IsScrollbarOnRight()
             ? ScrollbarDrawing::ScrollbarKind::VerticalRight
             : ScrollbarDrawing::ScrollbarKind::VerticalLeft;
}

static ScrollbarDrawing::ScrollbarKind ComputeScrollbarKindForScrollCorner(
    nsIFrame* aFrame) {
  ScrollContainerFrame* sf = do_QueryFrame(aFrame->GetParent());
  if (!sf) {
    return ScrollbarDrawing::ScrollbarKind::VerticalRight;
  }
  return sf->IsScrollbarOnRight()
             ? ScrollbarDrawing::ScrollbarKind::VerticalRight
             : ScrollbarDrawing::ScrollbarKind::VerticalLeft;
}

template <typename PaintBackendData>
bool Theme::DoDrawWidgetBackground(PaintBackendData& aPaintData,
                                   nsIFrame* aFrame,
                                   StyleAppearance aAppearance,
                                   const nsRect& aRect,
                                   DrawOverflow aDrawOverflow) {
  static_assert(std::is_same_v<PaintBackendData, DrawTarget> ||
                std::is_same_v<PaintBackendData, WebRenderBackendData>);

  const nsPresContext* pc = aFrame->PresContext();
  const nscoord twipsPerPixel = pc->AppUnitsPerDevPixel();
  const auto devPxRect = ToSnappedRect(aRect, twipsPerPixel, aPaintData);

  ElementState elementState = GetContentState(aFrame, aAppearance);
  // Paint the outline iff we're asked to draw overflow and we have
  // outline-style: auto.
  if (aDrawOverflow == DrawOverflow::Yes &&
      aFrame->StyleOutline()->mOutlineStyle.IsAuto()) {
    elementState |= ElementState::FOCUSRING;
  } else {
    elementState &= ~ElementState::FOCUSRING;
  }

  // Hack to avoid skia fuzziness: Add a dummy clip if the widget doesn't
  // overflow devPxRect.
  Maybe<AutoClipRect> maybeClipRect;
  if constexpr (std::is_same_v<PaintBackendData, DrawTarget>) {
    if (aAppearance != StyleAppearance::FocusOutline &&
        aAppearance != StyleAppearance::Range &&
        !elementState.HasState(ElementState::FOCUSRING)) {
      maybeClipRect.emplace(aPaintData, devPxRect);
    }
  }

  const Colors colors(aFrame, aAppearance);
  DPIRatio dpiRatio = GetDPIRatio(aFrame, aAppearance);

  switch (aAppearance) {
    case StyleAppearance::Radio: {
      auto rect = CheckBoxRadioRect(devPxRect);
      PaintRadioControl(aPaintData, rect, elementState, colors, dpiRatio);
      break;
    }
    case StyleAppearance::Checkbox: {
      if constexpr (std::is_same_v<PaintBackendData, WebRenderBackendData>) {
        // TODO: Need to figure out how to best draw this using WR.
        return false;
      } else {
        auto rect = CheckBoxRadioRect(devPxRect);
        PaintCheckboxControl(aPaintData, rect, elementState, colors, dpiRatio);
      }
      break;
    }
    case StyleAppearance::Textarea:
    case StyleAppearance::Textfield:
    case StyleAppearance::NumberInput:
    case StyleAppearance::PasswordInput:
      PaintTextField(aPaintData, devPxRect, elementState, colors, dpiRatio);
      break;
    case StyleAppearance::Listbox:
      PaintListbox(aPaintData, devPxRect, elementState, colors, dpiRatio);
      break;
    case StyleAppearance::Menulist:
      PaintMenulist(aPaintData, devPxRect, elementState, colors, dpiRatio);
      break;
    case StyleAppearance::MozMenulistArrowButton:
      if constexpr (std::is_same_v<PaintBackendData, WebRenderBackendData>) {
        // TODO: Need to figure out how to best draw this using WR.
        return false;
      } else {
        PaintMenulistArrow(aFrame, aPaintData, devPxRect);
      }
      break;
    case StyleAppearance::Tooltip: {
      const CSSCoord strokeWidth(1.0f);
      const CSSCoord strokeRadius(2.0f);
      ThemeDrawing::PaintRoundedRectWithRadius(
          aPaintData, devPxRect,
          colors.System(StyleSystemColor::Infobackground),
          colors.System(StyleSystemColor::Infotext), strokeWidth, strokeRadius,
          dpiRatio);
      break;
    }
    case StyleAppearance::SpinnerUpbutton:
    case StyleAppearance::SpinnerDownbutton:
      if constexpr (std::is_same_v<PaintBackendData, WebRenderBackendData>) {
        // TODO: Need to figure out how to best draw this using WR.
        return false;
      } else {
        PaintSpinnerButton(aFrame, aPaintData, devPxRect, elementState,
                           aAppearance, colors, dpiRatio);
      }
      break;
    case StyleAppearance::Range:
      PaintRange(aFrame, aPaintData, devPxRect, elementState, colors, dpiRatio,
                 IsRangeHorizontal(aFrame));
      break;
    case StyleAppearance::ProgressBar:
      PaintProgress(aFrame, aPaintData, devPxRect, elementState, colors,
                    dpiRatio,
                    /* aIsMeter = */ false);
      break;
    case StyleAppearance::Meter:
      PaintProgress(aFrame, aPaintData, devPxRect, elementState, colors,
                    dpiRatio,
                    /* aIsMeter = */ true);
      break;
    case StyleAppearance::ScrollbarthumbHorizontal:
    case StyleAppearance::ScrollbarthumbVertical: {
      bool isHorizontal =
          aAppearance == StyleAppearance::ScrollbarthumbHorizontal;
      auto kind = ComputeScrollbarKind(aFrame, isHorizontal);
      return GetScrollbarDrawing().PaintScrollbarThumb(
          aPaintData, devPxRect, kind, aFrame,
          *nsLayoutUtils::StyleForScrollbar(aFrame), elementState, colors,
          dpiRatio);
    }
    case StyleAppearance::ScrollbarHorizontal:
    case StyleAppearance::ScrollbarVertical: {
      bool isHorizontal = aAppearance == StyleAppearance::ScrollbarHorizontal;
      auto kind = ComputeScrollbarKind(aFrame, isHorizontal);
      return GetScrollbarDrawing().PaintScrollbar(
          aPaintData, devPxRect, kind, aFrame,
          *nsLayoutUtils::StyleForScrollbar(aFrame), elementState, colors,
          dpiRatio);
    }
    case StyleAppearance::Scrollcorner: {
      auto kind = ComputeScrollbarKindForScrollCorner(aFrame);
      return GetScrollbarDrawing().PaintScrollCorner(
          aPaintData, devPxRect, kind, aFrame,
          *nsLayoutUtils::StyleForScrollbar(aFrame), colors, dpiRatio);
    }
    case StyleAppearance::ScrollbarbuttonUp:
    case StyleAppearance::ScrollbarbuttonDown:
    case StyleAppearance::ScrollbarbuttonLeft:
    case StyleAppearance::ScrollbarbuttonRight: {
      // For scrollbar-width:thin, we don't display the buttons.
      if (!ScrollbarDrawing::IsScrollbarWidthThin(aFrame)) {
        if constexpr (std::is_same_v<PaintBackendData, WebRenderBackendData>) {
          // TODO: Need to figure out how to best draw this using WR.
          return false;
        } else {
          bool isHorizontal =
              aAppearance == StyleAppearance::ScrollbarbuttonLeft ||
              aAppearance == StyleAppearance::ScrollbarbuttonRight;
          auto kind = ComputeScrollbarKind(aFrame, isHorizontal);
          GetScrollbarDrawing().PaintScrollbarButton(
              aPaintData, aAppearance, devPxRect, kind, aFrame,
              *nsLayoutUtils::StyleForScrollbar(aFrame), elementState, colors,
              dpiRatio);
        }
      }
      break;
    }
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton:
      PaintButton(aPaintData, devPxRect, aAppearance, elementState, colors,
                  dpiRatio);
      break;
    case StyleAppearance::FocusOutline:
      PaintAutoStyleOutline(aFrame, aPaintData, devPxRect, colors, dpiRatio);
      break;
    default:
      // Various appearance values are used for XUL elements.  Normally these
      // will not be available in content documents (and thus in the content
      // processes where the native basic theme can be used), but tests are
      // run with the remote XUL pref enabled and so we can get in here.  So
      // we just return an error rather than assert.
      return false;
  }

  return true;
}

template <typename PaintBackendData>
void Theme::PaintAutoStyleOutline(PaintBackendData& aPaintData,
                                  const LayoutDeviceRect& aRect,
                                  const Colors& aColors, CSSCoord aRadius,
                                  CSSCoord aOffset, InvertColors aInvertColors,
                                  DPIRatio aDpiRatio) {
  RectCornerRadii radii(aRadius * aDpiRatio);
  LayoutDeviceCoord offset(aOffset * aDpiRatio);
  return PaintAutoStyleOutline(aPaintData, aRect, aColors, radii, offset,
                               aInvertColors, aDpiRatio);
}

template <typename PaintBackendData>
void Theme::PaintAutoStyleOutline(nsIFrame* aFrame,
                                  PaintBackendData& aPaintData,
                                  const LayoutDeviceRect& aRect,
                                  const Colors& aColors, DPIRatio aDpiRatio) {
  const nscoord a2d = aFrame->PresContext()->AppUnitsPerDevPixel();
  const auto cssOffset = aFrame->StyleOutline()->mOutlineOffset.ToAppUnits();

  LayoutDeviceRect rect(aRect);
  auto devOffset = LayoutDevicePixel::FromAppUnits(cssOffset, a2d);
  nscoord cssRadii[8] = {0};
  if (!aFrame->GetBorderRadii(cssRadii)) {
    // The goal of this code is getting a 0px inner radius, but 2px outer
    // radius.
    const auto twoPixels = 2 * AppUnitsPerCSSPixel();
    const nscoord radius =
        cssOffset >= 0 ? twoPixels : std::max(twoPixels + cssOffset, 0);
    auto twoDevPixels = CSSCoord(2) * aDpiRatio;
    rect.Inflate(devOffset + twoDevPixels);
    devOffset = -twoDevPixels;
    for (auto& r : cssRadii) {
      r = radius;
    }
  }

  RectCornerRadii innerRadii;
  nsCSSRendering::ComputePixelRadii(cssRadii, a2d, &innerRadii);
  return PaintAutoStyleOutline(aPaintData, rect, aColors, innerRadii, devOffset,
                               InvertColors::No, aDpiRatio);
}

template <typename PaintBackendData>
void Theme::PaintAutoStyleOutline(
    PaintBackendData& aPaintData, const LayoutDeviceRect& aRect,
    const Colors& aColors, const RectCornerRadii& aInnerRadii,
    LayoutDeviceCoord aOffset, InvertColors aInvertColors, DPIRatio aDpiRatio) {
  const auto& accentColor = aColors.Accent();

  LayoutDeviceCoord primaryStrokeWidth(
      ThemeDrawing::SnapBorderWidth(2.0f, aDpiRatio));
  LayoutDeviceCoord secondaryStrokeWidth(
      ThemeDrawing::SnapBorderWidth(1.0f, aDpiRatio));
  auto primaryColor = aColors.HighContrast()
                          ? aColors.System(StyleSystemColor::Selecteditem)
                          : accentColor.Get();
  auto secondaryColor = aColors.HighContrast()
                            ? aColors.System(StyleSystemColor::Canvastext)
                            : accentColor.GetForeground();
  if (aInvertColors == InvertColors::Yes) {
    std::swap(primaryColor, secondaryColor);
    std::swap(primaryStrokeWidth, secondaryStrokeWidth);
  }

  LayoutDeviceCoord strokeWidth = primaryStrokeWidth;

  LayoutDeviceRect rect(aRect);
  rect.Inflate(strokeWidth + aOffset);

  // NOTE(emilio): This doesn't use PaintRoundedRectWithRadius because we need
  // to support arbitrary radii.
  auto DrawRect = [&](const sRGBColor& aColor) {
    RectCornerRadii outerRadii;
    if constexpr (std::is_same_v<PaintBackendData, WebRenderBackendData>) {
      const Float widths[4] = {strokeWidth + aOffset, strokeWidth + aOffset,
                               strokeWidth + aOffset, strokeWidth + aOffset};
      nsCSSBorderRenderer::ComputeOuterRadii(aInnerRadii, widths, &outerRadii);
      const auto dest = wr::ToLayoutRect(rect);
      const auto side =
          wr::ToBorderSide(ToDeviceColor(aColor), StyleBorderStyle::Solid);
      const wr::BorderSide sides[4] = {side, side, side, side};
      const bool kBackfaceIsVisible = true;
      const auto wrWidths = wr::ToBorderWidths(strokeWidth, strokeWidth,
                                               strokeWidth, strokeWidth);
      const auto wrRadius = wr::ToBorderRadius(outerRadii);
      aPaintData.mBuilder.PushBorder(dest, dest, kBackfaceIsVisible, wrWidths,
                                     {sides, 4}, wrRadius);
    } else {
      const LayoutDeviceCoord halfWidth = strokeWidth * 0.5f;
      const Float widths[4] = {halfWidth + aOffset, halfWidth + aOffset,
                               halfWidth + aOffset, halfWidth + aOffset};
      nsCSSBorderRenderer::ComputeOuterRadii(aInnerRadii, widths, &outerRadii);
      LayoutDeviceRect dest(rect);
      dest.Deflate(halfWidth);
      RefPtr<Path> path =
          MakePathForRoundedRect(aPaintData, dest.ToUnknownRect(), outerRadii);
      aPaintData.Stroke(path, ColorPattern(ToDeviceColor(aColor)),
                        StrokeOptions(strokeWidth));
    }
  };

  DrawRect(primaryColor);

  aOffset += strokeWidth;

  strokeWidth = secondaryStrokeWidth;
  rect.Inflate(strokeWidth);
  DrawRect(secondaryColor);
}

LayoutDeviceIntMargin Theme::GetWidgetBorder(nsDeviceContext* aContext,
                                             nsIFrame* aFrame,
                                             StyleAppearance aAppearance) {
  switch (aAppearance) {
    case StyleAppearance::Textfield:
    case StyleAppearance::Textarea:
    case StyleAppearance::NumberInput:
    case StyleAppearance::PasswordInput:
    case StyleAppearance::Listbox:
    case StyleAppearance::Menulist:
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton:
    case StyleAppearance::ProgressBar:
      // Return the border size from the UA sheet, even though what we paint
      // doesn't actually match that. We know this is the UA sheet border
      // because we disable native theming when different border widths are
      // specified by authors, see Theme::IsWidgetStyled.
      //
      // The Rounded() bit is technically redundant, but needed to appease the
      // type system, we should always end up with full device pixels due to
      // round_border_to_device_pixels at style time.
      return LayoutDeviceIntMargin::FromAppUnits(
                 aFrame->StyleBorder()->GetComputedBorder(),
                 aFrame->PresContext()->AppUnitsPerDevPixel())
          .Rounded();
    default:
      return LayoutDeviceIntMargin();
  }
}

bool Theme::GetWidgetPadding(nsDeviceContext* aContext, nsIFrame* aFrame,
                             StyleAppearance aAppearance,
                             LayoutDeviceIntMargin* aResult) {
  switch (aAppearance) {
    // Radios and checkboxes return a fixed size in GetMinimumWidgetSize
    // and have a meaningful baseline, so they can't have
    // author-specified padding.
    case StyleAppearance::Radio:
    case StyleAppearance::Checkbox:
      aResult->SizeTo(0, 0, 0, 0);
      return true;
    default:
      break;
  }
  return false;
}

bool Theme::GetWidgetOverflow(nsDeviceContext* aContext, nsIFrame* aFrame,
                              StyleAppearance aAppearance,
                              nsRect* aOverflowRect) {
  // NOTE: This should theoretically use SnapBorderWidth to account for DPI,
  // but that would end up truncating in most cases (unless you're really
  // zoomed out maybe), so should be ~fine.
  CSSCoord outlineWidth = 3;
  CSSCoord outlineOffset = 0;
  switch (aAppearance) {
    case StyleAppearance::Range:
      outlineOffset = kRangeOutlineOffset;
      break;
    case StyleAppearance::Radio:
    case StyleAppearance::Checkbox:
    case StyleAppearance::FocusOutline:
      break;
    case StyleAppearance::Textarea:
    case StyleAppearance::Listbox:
    case StyleAppearance::Textfield:
    case StyleAppearance::NumberInput:
    case StyleAppearance::PasswordInput:
      outlineOffset = -kTextFieldBorderWidth;
      break;
    case StyleAppearance::Menulist:
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton:
      outlineOffset = -kButtonBorderWidth;
      break;
    default:
      return false;
  }

  aOverflowRect->Inflate(CSSPixel::ToAppUnits(outlineWidth + outlineOffset));
  return true;
}

LayoutDeviceIntCoord Theme::GetScrollbarSize(const nsPresContext* aPresContext,
                                             StyleScrollbarWidth aWidth,
                                             Overlay aOverlay) {
  return GetScrollbarDrawing().GetScrollbarSize(aPresContext, aWidth, aOverlay);
}

CSSCoord Theme::GetCheckboxRadioPrefSize() { return kCheckboxRadioSize; }

/* static */
UniquePtr<ScrollbarDrawing> Theme::ScrollbarStyle() {
  switch (StaticPrefs::widget_non_native_theme_scrollbar_style()) {
    case 1:
      return MakeUnique<ScrollbarDrawingCocoa>();
    case 2:
      return MakeUnique<ScrollbarDrawingGTK>();
    case 3:
      return MakeUnique<ScrollbarDrawingAndroid>();
    case 4:
      return MakeUnique<ScrollbarDrawingWin>();
    case 5:
      return MakeUnique<ScrollbarDrawingWin11>();
    default:
      break;
  }
    // Default to native scrollbar style for each platform.
#ifdef XP_WIN
  if (IsWin11OrLater()) {
    return MakeUnique<ScrollbarDrawingWin11>();
  }
  return MakeUnique<ScrollbarDrawingWin>();
#elif defined(MOZ_WIDGET_COCOA) || defined(MOZ_WIDGET_UIKIT)
  return MakeUnique<ScrollbarDrawingCocoa>();
#elif MOZ_WIDGET_GTK
  return MakeUnique<ScrollbarDrawingGTK>();
#elif ANDROID
  return MakeUnique<ScrollbarDrawingAndroid>();
#else
#  error "Unknown platform, need scrollbar implementation."
#endif
}

LayoutDeviceIntSize Theme::GetMinimumWidgetSize(nsPresContext* aPresContext,
                                                nsIFrame* aFrame,
                                                StyleAppearance aAppearance) {
  DPIRatio dpiRatio = GetDPIRatio(aFrame, aAppearance);

  if (IsWidgetScrollbarPart(aAppearance)) {
    return GetScrollbarDrawing().GetMinimumWidgetSize(aPresContext, aAppearance,
                                                      aFrame);
  }

  LayoutDeviceIntSize result;
  switch (aAppearance) {
    case StyleAppearance::MozMenulistArrowButton:
      result.width = (kMinimumDropdownArrowButtonWidth * dpiRatio).Rounded();
      break;
    case StyleAppearance::SpinnerUpbutton:
    case StyleAppearance::SpinnerDownbutton:
      result.width = (kMinimumSpinnerButtonWidth * dpiRatio).Rounded();
      result.height = (kMinimumSpinnerButtonHeight * dpiRatio).Rounded();
      break;
    default:
      break;
  }
  return result;
}

nsITheme::Transparency Theme::GetWidgetTransparency(
    nsIFrame* aFrame, StyleAppearance aAppearance) {
  if (auto scrollbar = GetScrollbarDrawing().GetScrollbarPartTransparency(
          aFrame, aAppearance)) {
    return *scrollbar;
  }
  if (aAppearance == StyleAppearance::Tooltip) {
    // We draw a rounded rect, so we need transparency.
    return eTransparent;
  }
  return eUnknownTransparency;
}

bool Theme::WidgetAttributeChangeRequiresRepaint(StyleAppearance aAppearance,
                                                 nsAtom* aAttribute) {
  // Check the attribute to see if it's relevant.
  // TODO(emilio): The non-native theme doesn't use these attributes. Other
  // themes do, but not all of them (and not all of the ones they check are
  // here).
  return aAttribute == nsGkAtoms::disabled ||
         aAttribute == nsGkAtoms::checked ||
         aAttribute == nsGkAtoms::selected ||
         aAttribute == nsGkAtoms::visuallyselected ||
         aAttribute == nsGkAtoms::menuactive ||
         aAttribute == nsGkAtoms::sortDirection ||
         aAttribute == nsGkAtoms::focused ||
         aAttribute == nsGkAtoms::_default || aAttribute == nsGkAtoms::open;
}

bool Theme::WidgetAppearanceDependsOnWindowFocus(StyleAppearance aAppearance) {
  return IsWidgetScrollbarPart(aAppearance);
}

nsITheme::ThemeGeometryType Theme::ThemeGeometryTypeForWidget(
    nsIFrame* aFrame, StyleAppearance aAppearance) {
  return eThemeGeometryTypeUnknown;
}

bool Theme::ThemeSupportsWidget(nsPresContext* aPresContext, nsIFrame* aFrame,
                                StyleAppearance aAppearance) {
  switch (aAppearance) {
    case StyleAppearance::Radio:
    case StyleAppearance::Checkbox:
    case StyleAppearance::FocusOutline:
    case StyleAppearance::Textarea:
    case StyleAppearance::Textfield:
    case StyleAppearance::Range:
    case StyleAppearance::ProgressBar:
    case StyleAppearance::Meter:
    case StyleAppearance::ScrollbarbuttonUp:
    case StyleAppearance::ScrollbarbuttonDown:
    case StyleAppearance::ScrollbarbuttonLeft:
    case StyleAppearance::ScrollbarbuttonRight:
    case StyleAppearance::ScrollbarthumbHorizontal:
    case StyleAppearance::ScrollbarthumbVertical:
    case StyleAppearance::ScrollbarHorizontal:
    case StyleAppearance::ScrollbarVertical:
    case StyleAppearance::Scrollcorner:
    case StyleAppearance::Button:
    case StyleAppearance::Toolbarbutton:
    case StyleAppearance::Listbox:
    case StyleAppearance::Menulist:
    case StyleAppearance::NumberInput:
    case StyleAppearance::PasswordInput:
    case StyleAppearance::MozMenulistArrowButton:
    case StyleAppearance::SpinnerUpbutton:
    case StyleAppearance::SpinnerDownbutton:
    case StyleAppearance::Tooltip:
      return !IsWidgetStyled(aPresContext, aFrame, aAppearance);
    default:
      return false;
  }
}

bool Theme::WidgetIsContainer(StyleAppearance aAppearance) {
  switch (aAppearance) {
    case StyleAppearance::MozMenulistArrowButton:
    case StyleAppearance::Radio:
    case StyleAppearance::Checkbox:
      return false;
    default:
      return true;
  }
}

bool Theme::ThemeDrawsFocusForWidget(nsIFrame*, StyleAppearance) {
  return true;
}

bool Theme::ThemeNeedsComboboxDropmarker() { return true; }

bool Theme::ThemeSupportsScrollbarButtons() {
  return GetScrollbarDrawing().ShouldDrawScrollbarButtons();
}

}  // namespace mozilla::widget
