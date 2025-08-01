/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_STYLE_TYPEDOM_CSSUNITVALUE_H_
#define LAYOUT_STYLE_TYPEDOM_CSSUNITVALUE_H_

#include "js/TypeDecls.h"
#include "mozilla/dom/CSSNumericValue.h"
#include "nsStringFwd.h"

template <class T>
struct already_AddRefed;
template <class T>
class nsCOMPtr;
class nsISupports;

namespace mozilla {

class ErrorResult;

namespace dom {

class GlobalObject;

class CSSUnitValue final : public CSSNumericValue {
 public:
  explicit CSSUnitValue(nsCOMPtr<nsISupports> aParent);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  // start of CSSUnitValue Web IDL declarations

  static already_AddRefed<CSSUnitValue> Constructor(const GlobalObject& aGlobal,
                                                    double aValue,
                                                    const nsACString& aUnit,
                                                    ErrorResult& aRv);

  double Value() const;

  void SetValue(double aArg);

  void GetUnit(nsCString& aRetVal) const;

  // end of CSSUnitValue Web IDL declarations

 private:
  virtual ~CSSUnitValue() = default;
};

}  // namespace dom
}  // namespace mozilla

#endif  // LAYOUT_STYLE_TYPEDOM_CSSUNITVALUE_H_
