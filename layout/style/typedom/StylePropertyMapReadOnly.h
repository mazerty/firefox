/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_STYLE_TYPEDOM_STYLEPROPERTYMAPREADONLY_H_
#define LAYOUT_STYLE_TYPEDOM_STYLEPROPERTYMAPREADONLY_H_

#include <stdint.h>

#include "js/TypeDecls.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/dom/CSSStyleValueBindingFwd.h"
#include "nsCOMPtr.h"
#include "nsISupports.h"
#include "nsISupportsImpl.h"
#include "nsStringFwd.h"
#include "nsTArrayForwardDeclare.h"
#include "nsWrapperCache.h"

template <class T>
class RefPtr;

namespace mozilla {

class ErrorResult;

namespace dom {

class OwningUndefinedOrCSSStyleValue;

class StylePropertyMapReadOnly : public nsISupports, public nsWrapperCache {
 public:
  explicit StylePropertyMapReadOnly(nsCOMPtr<nsISupports> aParent);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(StylePropertyMapReadOnly)

  nsISupports* GetParentObject() const;

  JSObject* WrapObject(JSContext*, JS::Handle<JSObject*> aGivenProto) override;

  // start of StylePropertyMapReadOnly Web IDL declarations

  void Get(const nsACString& aProperty, OwningUndefinedOrCSSStyleValue& aRetVal,
           ErrorResult& aRv) const;

  void GetAll(const nsACString& aProperty,
              nsTArray<RefPtr<CSSStyleValue>>& aRetVal, ErrorResult& aRv) const;

  bool Has(const nsACString& aProperty, ErrorResult& aRv) const;

  uint32_t Size() const;

  uint32_t GetIterableLength() const;

  const nsACString& GetKeyAtIndex(uint32_t aIndex) const;

  nsTArray<RefPtr<CSSStyleValue>> GetValueAtIndex(uint32_t aIndex) const;

  // end of StylePropertyMapReadOnly Web IDL declarations

  size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const;

  size_t SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const;

 protected:
  virtual ~StylePropertyMapReadOnly() = default;

  nsCOMPtr<nsISupports> mParent;
};

}  // namespace dom
}  // namespace mozilla

#endif  // LAYOUT_STYLE_TYPEDOM_STYLEPROPERTYMAPREADONLY_H_
