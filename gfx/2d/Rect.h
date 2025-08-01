/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RECT_H_
#define MOZILLA_GFX_RECT_H_

#include "BaseRect.h"
#include "BaseMargin.h"
#include "NumericTools.h"
#include "Point.h"
#include "Tools.h"
#include "mozilla/Maybe.h"

#include <cmath>
#include <cstdint>

namespace mozilla {

template <typename>
struct IsPixel;

namespace gfx {

template <class Units, class F>
struct RectTyped;

template <class Units>
struct MOZ_EMPTY_BASES IntMarginTyped
    : public BaseMargin<int32_t, IntMarginTyped<Units>, IntCoordTyped<Units> >,
      public Units {
  static_assert(IsPixel<Units>::value,
                "'Units' must be a coordinate system tag");

  typedef IntCoordTyped<Units> Coord;
  typedef BaseMargin<int32_t, IntMarginTyped<Units>, Coord> Super;

  IntMarginTyped() : Super() {
    static_assert(sizeof(IntMarginTyped) == sizeof(int32_t) * 4,
                  "Would be unfortunate otherwise!");
  }
  constexpr IntMarginTyped(Coord aTop, Coord aRight, Coord aBottom, Coord aLeft)
      : Super(aTop, aRight, aBottom, aLeft) {}

  // XXX When all of the code is ported, the following functions to convert
  // to and from unknown types should be removed.

  static IntMarginTyped<Units> FromUnknownMargin(
      const IntMarginTyped<UnknownUnits>& aMargin) {
    return IntMarginTyped<Units>(aMargin.top.value, aMargin.right.value,
                                 aMargin.bottom.value, aMargin.left.value);
  }

  IntMarginTyped<UnknownUnits> ToUnknownMargin() const {
    return IntMarginTyped<UnknownUnits>(this->top, this->right, this->bottom,
                                        this->left);
  }
};
typedef IntMarginTyped<UnknownUnits> IntMargin;

template <class Units, class F = Float>
struct MarginTyped
    : public BaseMargin<F, MarginTyped<Units, F>, CoordTyped<Units, F> >,
      public Units {
  static_assert(IsPixel<Units>::value,
                "'Units' must be a coordinate system tag");

  typedef CoordTyped<Units, F> Coord;
  typedef BaseMargin<F, MarginTyped<Units, F>, Coord> Super;

  MarginTyped() : Super() {}
  MarginTyped(Coord aTop, Coord aRight, Coord aBottom, Coord aLeft)
      : Super(aTop, aRight, aBottom, aLeft) {}
  explicit MarginTyped(const IntMarginTyped<Units>& aMargin)
      : Super(F(aMargin.top), F(aMargin.right), F(aMargin.bottom),
              F(aMargin.left)) {}

  bool WithinEpsilonOf(const MarginTyped& aOther, F aEpsilon) const {
    return fabs(this->left - aOther.left) < aEpsilon &&
           fabs(this->top - aOther.top) < aEpsilon &&
           fabs(this->right - aOther.right) < aEpsilon &&
           fabs(this->bottom - aOther.bottom) < aEpsilon;
  }

  IntMarginTyped<Units> Rounded() const {
    return IntMarginTyped<Units>(int32_t(std::floor(this->top + 0.5f)),
                                 int32_t(std::floor(this->right + 0.5f)),
                                 int32_t(std::floor(this->bottom + 0.5f)),
                                 int32_t(std::floor(this->left + 0.5f)));
  }
};
typedef MarginTyped<UnknownUnits> Margin;
typedef MarginTyped<UnknownUnits, double> MarginDouble;

template <class Units>
IntMarginTyped<Units> RoundedToInt(const MarginTyped<Units>& aMargin) {
  return aMargin.Rounded();
}

template <class Units>
struct MOZ_EMPTY_BASES IntRectTyped
    : public BaseRect<int32_t, IntRectTyped<Units>, IntPointTyped<Units>,
                      IntSizeTyped<Units>, IntMarginTyped<Units> >,
      public Units {
  static_assert(IsPixel<Units>::value,
                "'Units' must be a coordinate system tag");

  typedef BaseRect<int32_t, IntRectTyped<Units>, IntPointTyped<Units>,
                   IntSizeTyped<Units>, IntMarginTyped<Units> >
      Super;
  typedef IntRectTyped<Units> Self;
  typedef IntParam<int32_t> ToInt;

  IntRectTyped() : Super() {
    static_assert(sizeof(IntRectTyped) == sizeof(int32_t) * 4,
                  "Would be unfortunate otherwise!");
  }
  IntRectTyped(const IntPointTyped<Units>& aPos,
               const IntSizeTyped<Units>& aSize)
      : Super(aPos, aSize) {}

  IntRectTyped(ToInt aX, ToInt aY, ToInt aWidth, ToInt aHeight)
      : Super(aX.value, aY.value, aWidth.value, aHeight.value) {}

  static IntRectTyped<Units> RoundIn(float aX, float aY, float aW, float aH) {
    return IntRectTyped<Units>::RoundIn(
        RectTyped<Units, float>(aX, aY, aW, aH));
  }

  static IntRectTyped<Units> RoundOut(float aX, float aY, float aW, float aH) {
    return IntRectTyped<Units>::RoundOut(
        RectTyped<Units, float>(aX, aY, aW, aH));
  }

  static IntRectTyped<Units> Round(float aX, float aY, float aW, float aH) {
    return IntRectTyped<Units>::Round(RectTyped<Units, float>(aX, aY, aW, aH));
  }

  static IntRectTyped<Units> Truncate(float aX, float aY, float aW, float aH) {
    return IntRectTyped<Units>(IntPointTyped<Units>::Truncate(aX, aY),
                               IntSizeTyped<Units>::Truncate(aW, aH));
  }

  static IntRectTyped<Units> RoundIn(const RectTyped<Units, float>& aRect) {
    auto tmp(aRect);
    tmp.RoundIn();
    return IntRectTyped(int32_t(tmp.X()), int32_t(tmp.Y()),
                        int32_t(tmp.Width()), int32_t(tmp.Height()));
  }

  static IntRectTyped<Units> RoundOut(const RectTyped<Units, float>& aRect) {
    auto tmp(aRect);
    tmp.RoundOut();
    return IntRectTyped(int32_t(tmp.X()), int32_t(tmp.Y()),
                        int32_t(tmp.Width()), int32_t(tmp.Height()));
  }

  static IntRectTyped<Units> Round(const RectTyped<Units, float>& aRect) {
    auto tmp(aRect);
    tmp.Round();
    return IntRectTyped(int32_t(tmp.X()), int32_t(tmp.Y()),
                        int32_t(tmp.Width()), int32_t(tmp.Height()));
  }

  static IntRectTyped<Units> Truncate(const RectTyped<Units, float>& aRect) {
    return IntRectTyped::Truncate(aRect.X(), aRect.Y(), aRect.Width(),
                                  aRect.Height());
  }

  // Rounding isn't meaningful on an integer rectangle.
  void Round() {}
  void RoundIn() {}
  void RoundOut() {}

  // XXX When all of the code is ported, the following functions to convert
  // to and from unknown types should be removed.

  static IntRectTyped<Units> FromUnknownRect(
      const IntRectTyped<UnknownUnits>& rect) {
    return IntRectTyped<Units>(rect.X(), rect.Y(), rect.Width(), rect.Height());
  }

  IntRectTyped<UnknownUnits> ToUnknownRect() const {
    return IntRectTyped<UnknownUnits>(this->X(), this->Y(), this->Width(),
                                      this->Height());
  }

  bool Overflows() const {
    CheckedInt<int32_t> xMost = this->X();
    xMost += this->Width();
    CheckedInt<int32_t> yMost = this->Y();
    yMost += this->Height();
    return !xMost.isValid() || !yMost.isValid();
  }

  // Same as Union(), but in the cases where aRect is non-empty, the union is
  // done while guarding against overflow. If an overflow is detected, Nothing
  // is returned.
  [[nodiscard]] Maybe<Self> SafeUnion(const Self& aRect) const {
    if (this->IsEmpty()) {
      return aRect.Overflows() ? Nothing() : Some(aRect);
    } else if (aRect.IsEmpty()) {
      return Some(*static_cast<const Self*>(this));
    } else {
      return this->SafeUnionEdges(aRect);
    }
  }

  // Same as UnionEdges, but guards against overflow. If an overflow is
  // detected, Nothing is returned.
  [[nodiscard]] Maybe<Self> SafeUnionEdges(const Self& aRect) const {
    if (this->Overflows() || aRect.Overflows()) {
      return Nothing();
    }
    // If neither |this| nor |aRect| overflow, then their XMost/YMost values
    // should be safe to use.
    CheckedInt<int32_t> newX = std::min(this->x, aRect.x);
    CheckedInt<int32_t> newY = std::min(this->y, aRect.y);
    CheckedInt<int32_t> newXMost = std::max(this->XMost(), aRect.XMost());
    CheckedInt<int32_t> newYMost = std::max(this->YMost(), aRect.YMost());
    CheckedInt<int32_t> newW = newXMost - newX;
    CheckedInt<int32_t> newH = newYMost - newY;
    if (!newW.isValid() || !newH.isValid()) {
      return Nothing();
    }
    return Some(Self(newX.value(), newY.value(), newW.value(), newH.value()));
  }

  // This is here only to keep IPDL-generated code happy. DO NOT USE.
  bool operator==(const IntRectTyped<Units>& aRect) const {
    return IntRectTyped<Units>::IsEqualEdges(aRect);
  }

  void InflateToMultiple(const IntSizeTyped<Units>& aTileSize) {
    if (this->IsEmpty()) {
      return;
    }

    int32_t yMost = this->YMost();
    int32_t xMost = this->XMost();

    this->x = mozilla::RoundDownToMultiple(this->x, aTileSize.width);
    this->y = mozilla::RoundDownToMultiple(this->y, aTileSize.height);
    xMost = mozilla::RoundUpToMultiple(xMost, aTileSize.width);
    yMost = mozilla::RoundUpToMultiple(yMost, aTileSize.height);

    this->SetWidth(xMost - this->x);
    this->SetHeight(yMost - this->y);
  }
};
typedef IntRectTyped<UnknownUnits> IntRect;

template <class Units, class F = Float>
struct MOZ_EMPTY_BASES RectTyped
    : public BaseRect<F, RectTyped<Units, F>, PointTyped<Units, F>,
                      SizeTyped<Units, F>, MarginTyped<Units, F> >,
      public Units {
  static_assert(IsPixel<Units>::value,
                "'Units' must be a coordinate system tag");

  typedef BaseRect<F, RectTyped<Units, F>, PointTyped<Units, F>,
                   SizeTyped<Units, F>, MarginTyped<Units, F> >
      Super;

  RectTyped() : Super() {
    static_assert(sizeof(RectTyped) == sizeof(F) * 4,
                  "Would be unfortunate otherwise!");
  }
  RectTyped(const PointTyped<Units, F>& aPos, const SizeTyped<Units, F>& aSize)
      : Super(aPos, aSize) {}
  RectTyped(F _x, F _y, F _width, F _height) : Super(_x, _y, _width, _height) {}
  explicit RectTyped(const IntRectTyped<Units>& rect)
      : Super(F(rect.X()), F(rect.Y()), F(rect.Width()), F(rect.Height())) {}

  void NudgeToIntegers() {
    NudgeToInteger(&(this->x));
    NudgeToInteger(&(this->y));
    NudgeToInteger(&(this->width));
    NudgeToInteger(&(this->height));
  }

  bool ToIntRect(IntRectTyped<Units>* aOut) const {
    *aOut =
        IntRectTyped<Units>(int32_t(this->X()), int32_t(this->Y()),
                            int32_t(this->Width()), int32_t(this->Height()));
    return RectTyped<Units, F>(F(aOut->X()), F(aOut->Y()), F(aOut->Width()),
                               F(aOut->Height()))
        .IsEqualEdges(*this);
  }

  // XXX When all of the code is ported, the following functions to convert to
  // and from unknown types should be removed.

  static RectTyped<Units, F> FromUnknownRect(
      const RectTyped<UnknownUnits, F>& rect) {
    return RectTyped<Units, F>(rect.X(), rect.Y(), rect.Width(), rect.Height());
  }

  RectTyped<UnknownUnits, F> ToUnknownRect() const {
    return RectTyped<UnknownUnits, F>(this->X(), this->Y(), this->Width(),
                                      this->Height());
  }

  // This is here only to keep IPDL-generated code happy. DO NOT USE.
  bool operator==(const RectTyped<Units, F>& aRect) const {
    return RectTyped<Units, F>::IsEqualEdges(aRect);
  }

  bool WithinEpsilonOf(const RectTyped& aOther, F aEpsilon) const {
    return fabs(this->x - aOther.x) < aEpsilon &&
           fabs(this->y - aOther.y) < aEpsilon &&
           fabs(this->width - aOther.width) < aEpsilon &&
           fabs(this->height - aOther.height) < aEpsilon;
  }
};
typedef RectTyped<UnknownUnits> Rect;
typedef RectTyped<UnknownUnits, double> RectDouble;

template <class Units, class D>
RectTyped<Units> NarrowToFloat(const RectTyped<Units, D>& aRect) {
  return RectTyped<Units>(float(aRect.x), float(aRect.y), float(aRect.width),
                          float(aRect.height));
}

template <class Units, class F>
RectTyped<Units, double> WidenToDouble(const RectTyped<Units, F>& aRect) {
  return RectTyped<Units, double>(double(aRect.x), double(aRect.y),
                                  double(aRect.width), double(aRect.height));
}

template <class Units>
IntRectTyped<Units> RoundedToInt(const RectTyped<Units>& aRect) {
  RectTyped<Units> copy(aRect);
  copy.Round();
  return IntRectTyped<Units>(int32_t(copy.X()), int32_t(copy.Y()),
                             int32_t(copy.Width()), int32_t(copy.Height()));
}

template <class Units>
bool RectIsInt32Safe(const RectTyped<Units>& aRect) {
  float min = (float)std::numeric_limits<std::int32_t>::min();
  float max = (float)std::numeric_limits<std::int32_t>::max();
  return aRect.x > min && aRect.y > min && aRect.width < max &&
         aRect.height < max && aRect.XMost() < max && aRect.YMost() < max;
}

template <class Units>
IntRectTyped<Units> RoundedIn(const RectTyped<Units>& aRect) {
  return IntRectTyped<Units>::RoundIn(aRect);
}

template <class Units>
IntRectTyped<Units> RoundedOut(const RectTyped<Units>& aRect) {
  return IntRectTyped<Units>::RoundOut(aRect);
}

template <class Units>
IntRectTyped<Units> TruncatedToInt(const RectTyped<Units>& aRect) {
  return IntRectTyped<Units>::Truncate(aRect);
}

template <class Units>
RectTyped<Units> IntRectToRect(const IntRectTyped<Units>& aRect) {
  return RectTyped<Units>(aRect.X(), aRect.Y(), aRect.Width(), aRect.Height());
}

// Convenience functions for intersecting and unioning two rectangles wrapped in
// Maybes.
template <typename Rect>
Maybe<Rect> IntersectMaybeRects(const Maybe<Rect>& a, const Maybe<Rect>& b) {
  if (!a) {
    return b;
  } else if (!b) {
    return a;
  } else {
    return Some(a->Intersect(*b));
  }
}
template <typename Rect>
Maybe<Rect> UnionMaybeRects(const Maybe<Rect>& a, const Maybe<Rect>& b) {
  if (!a) {
    return b;
  } else if (!b) {
    return a;
  } else {
    return Some(a->Union(*b));
  }
}

struct RectCornerRadii final {
  Size radii[eCornerCount];

  RectCornerRadii() = default;

  explicit RectCornerRadii(Float radius) {
    for (const auto i : mozilla::AllPhysicalCorners()) {
      radii[i].SizeTo(radius, radius);
    }
  }

  RectCornerRadii(Float radiusX, Float radiusY) {
    for (const auto i : mozilla::AllPhysicalCorners()) {
      radii[i].SizeTo(radiusX, radiusY);
    }
  }

  RectCornerRadii(Float tl, Float tr, Float br, Float bl) {
    radii[eCornerTopLeft].SizeTo(tl, tl);
    radii[eCornerTopRight].SizeTo(tr, tr);
    radii[eCornerBottomRight].SizeTo(br, br);
    radii[eCornerBottomLeft].SizeTo(bl, bl);
  }

  RectCornerRadii(const Size& tl, const Size& tr, const Size& br,
                  const Size& bl) {
    radii[eCornerTopLeft] = tl;
    radii[eCornerTopRight] = tr;
    radii[eCornerBottomRight] = br;
    radii[eCornerBottomLeft] = bl;
  }

  const Size& operator[](size_t aCorner) const { return radii[aCorner]; }

  Size& operator[](size_t aCorner) { return radii[aCorner]; }

  bool operator==(const RectCornerRadii& aOther) const {
    return TopLeft() == aOther.TopLeft() && TopRight() == aOther.TopRight() &&
           BottomRight() == aOther.BottomRight() &&
           BottomLeft() == aOther.BottomLeft();
  }

  bool AreRadiiSame() const {
    return TopLeft() == TopRight() && TopLeft() == BottomRight() &&
           TopLeft() == BottomLeft();
  }

  void Scale(Float aXScale, Float aYScale) {
    for (const auto i : mozilla::AllPhysicalCorners()) {
      radii[i].Scale(aXScale, aYScale);
    }
  }

  const Size TopLeft() const { return radii[eCornerTopLeft]; }
  Size& TopLeft() { return radii[eCornerTopLeft]; }

  const Size TopRight() const { return radii[eCornerTopRight]; }
  Size& TopRight() { return radii[eCornerTopRight]; }

  const Size BottomRight() const { return radii[eCornerBottomRight]; }
  Size& BottomRight() { return radii[eCornerBottomRight]; }

  const Size BottomLeft() const { return radii[eCornerBottomLeft]; }
  Size& BottomLeft() { return radii[eCornerBottomLeft]; }

  bool IsEmpty() const {
    return TopLeft().IsEmpty() && TopRight().IsEmpty() &&
           BottomRight().IsEmpty() && BottomLeft().IsEmpty();
  }
};

/* A rounded rectangle abstraction.
 *
 * This can represent a rectangle with a different pair of radii on each corner.
 *
 * Note: CoreGraphics and Direct2D only support rounded rectangle with the same
 * radii on all corners. However, supporting CSS's border-radius requires the
 * extra flexibility. */
struct RoundedRect {
  typedef mozilla::gfx::RectCornerRadii RectCornerRadii;

  RoundedRect() = default;

  RoundedRect(const Rect& aRect, const RectCornerRadii& aCorners)
      : rect(aRect), corners(aCorners) {}

  void Deflate(Float aTopWidth, Float aBottomWidth, Float aLeftWidth,
               Float aRightWidth) {
    // deflate the internal rect
    rect.SetRect(rect.X() + aLeftWidth, rect.Y() + aTopWidth,
                 std::max(0.f, rect.Width() - aLeftWidth - aRightWidth),
                 std::max(0.f, rect.Height() - aTopWidth - aBottomWidth));

    corners.radii[mozilla::eCornerTopLeft].width = std::max(
        0.f, corners.radii[mozilla::eCornerTopLeft].width - aLeftWidth);
    corners.radii[mozilla::eCornerTopLeft].height = std::max(
        0.f, corners.radii[mozilla::eCornerTopLeft].height - aTopWidth);

    corners.radii[mozilla::eCornerTopRight].width = std::max(
        0.f, corners.radii[mozilla::eCornerTopRight].width - aRightWidth);
    corners.radii[mozilla::eCornerTopRight].height = std::max(
        0.f, corners.radii[mozilla::eCornerTopRight].height - aTopWidth);

    corners.radii[mozilla::eCornerBottomLeft].width = std::max(
        0.f, corners.radii[mozilla::eCornerBottomLeft].width - aLeftWidth);
    corners.radii[mozilla::eCornerBottomLeft].height = std::max(
        0.f, corners.radii[mozilla::eCornerBottomLeft].height - aBottomWidth);

    corners.radii[mozilla::eCornerBottomRight].width = std::max(
        0.f, corners.radii[mozilla::eCornerBottomRight].width - aRightWidth);
    corners.radii[mozilla::eCornerBottomRight].height = std::max(
        0.f, corners.radii[mozilla::eCornerBottomRight].height - aBottomWidth);
  }

  bool operator==(const RoundedRect& aOther) const {
    return rect == aOther.rect && corners == aOther.corners;
  }

  Rect rect;
  RectCornerRadii corners;
};

}  // namespace gfx
}  // namespace mozilla

#endif /* MOZILLA_GFX_RECT_H_ */
