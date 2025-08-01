/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CssAltContent.h"
#include "DocAccessible-inl.h"
#include "mozilla/a11y/DocManager.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "nsCoreUtils.h"
#include "nsIContent.h"
#include "nsIFrame.h"
#include "nsLayoutUtils.h"
#include "nsNameSpaceManager.h"

namespace mozilla::a11y {

CssAltContent::CssAltContent(nsIContent* aContent) {
  nsIFrame* frame = aContent->GetPrimaryFrame();
  if (!frame) {
    return;
  }
  // Check if this is for a pseudo-element.
  if (nsCoreUtils::IsPseudoElement(aContent)) {
    // If there are children, we want to expose the alt text on those instead,
    // so ignore it for the pseudo-element itself.
    if (aContent->HasChildren()) {
      return;
    }
    // No children only happens when there is alt text with an empty content
    // string; e.g. content: "" / "alt"
    // In this case, we need to expose the alt text on the pseudo-element
    // itself.
    mPseudoElement = aContent->AsElement();
  } else if (aContent->IsInNativeAnonymousSubtree()) {
    if (!frame->IsReplaced()) {
      return;
    }
    dom::Element* parent = aContent->GetParentElement();
    if (parent && nsCoreUtils::IsPseudoElement(parent)) {
      // aContent is a child of a pseudo-element.
      mPseudoElement = parent;
      // We need the frame from the pseudo-element to get the content style.
      frame = parent->GetPrimaryFrame();
      if (!frame) {
        return;
      }
    }
  }
  if (mPseudoElement) {
    // We need the real element to get any attributes.
    mRealElement = mPseudoElement->GetParentElement();
    if (!mRealElement) {
      return;
    }
  }
  if (!mRealElement) {
    // This isn't for a pseudo-element. It might be an element which has its
    // content replaced using CSS content.
    if (aContent->IsElement()) {
      if (!frame->IsReplaced()) {
        return;
      }
      mRealElement = aContent->AsElement();
    } else {
      return;
    }
  }
  mItems = frame->StyleContent()->AltContentItems();
}

void CssAltContent::AppendToString(nsAString& aOut) {
  // There can be multiple alt text items.
  for (const auto& item : mItems) {
    if (item.IsString()) {
      aOut.Append(NS_ConvertUTF8toUTF16(item.AsString().AsString()));
    } else if (item.IsAttr()) {
      // This item gets its value from an attribute on the element or from
      // fallback text.
      MOZ_ASSERT(mRealElement);
      const auto& attr = item.AsAttr();
      RefPtr<nsAtom> name = attr.attribute.AsAtom();
      int32_t nsId = kNameSpaceID_None;
      RefPtr<nsAtom> ns = attr.namespace_url.AsAtom();
      if (!ns->IsEmpty()) {
        nsresult rv = nsNameSpaceManager::GetInstance()->RegisterNameSpace(
            ns.forget(), nsId);
        if (NS_FAILED(rv)) {
          continue;
        }
      }
      if (mRealElement->IsHTMLElement() &&
          mRealElement->OwnerDoc()->IsHTMLDocument()) {
        ToLowerCaseASCII(name);
      }
      nsAutoString val;
      if (!mRealElement->GetAttr(nsId, name, val)) {
        if (RefPtr<nsAtom> fallback = attr.fallback.AsAtom()) {
          fallback->ToString(val);
        }
      }
      aOut.Append(val);
    }
  }
}

/* static */
bool CssAltContent::HandleAttributeChange(nsIContent* aContent,
                                          int32_t aNameSpaceID,
                                          nsAtom* aAttribute) {
  // Handle CSS content which replaces the content of aContent itself.
  if (CssAltContent(aContent).HandleAttributeChange(aNameSpaceID, aAttribute)) {
    return true;
  }
  // Handle any pseudo-elements with CSS alt content.
  for (dom::Element* pseudo : {nsLayoutUtils::GetBeforePseudo(aContent),
                               nsLayoutUtils::GetAfterPseudo(aContent),
                               nsLayoutUtils::GetMarkerPseudo(aContent)}) {
    // CssAltContent wants a child of a pseudo-element if there is one.
    nsIContent* content = pseudo ? pseudo->GetFirstChild() : nullptr;
    if (!content) {
      content = pseudo;
    }
    if (content && CssAltContent(content).HandleAttributeChange(aNameSpaceID,
                                                                aAttribute)) {
      return true;
    }
  }
  return false;
}

bool CssAltContent::HandleAttributeChange(int32_t aNameSpaceID,
                                          nsAtom* aAttribute) {
  for (const auto& item : mItems) {
    if (!item.IsAttr()) {
      continue;
    }
    MOZ_ASSERT(mRealElement);
    const auto& attr = item.AsAttr();
    RefPtr<nsAtom> name = attr.attribute.AsAtom();
    if (mRealElement->IsHTMLElement() &&
        mRealElement->OwnerDoc()->IsHTMLDocument()) {
      ToLowerCaseASCII(name);
    }
    if (name != aAttribute) {
      continue;
    }
    int32_t nsId = kNameSpaceID_None;
    RefPtr<nsAtom> ns = attr.namespace_url.AsAtom();
    if (!ns->IsEmpty()) {
      nsresult rv = nsNameSpaceManager::GetInstance()->RegisterNameSpace(
          ns.forget(), nsId);
      if (NS_FAILED(rv)) {
        continue;
      }
    }
    if (nsId != aNameSpaceID) {
      continue;
    }
    // The CSS alt content references this attribute which has just changed.
    DocAccessible* docAcc = GetExistingDocAccessible(mRealElement->OwnerDoc());
    MOZ_ASSERT(docAcc);
    if (mPseudoElement) {
      // For simplicity, we just recreate the pseudo-element subtree. If this
      // becomes a performance problem, we can probably do better. For images,
      // we can just fire a name change event. Text is a bit more complicated,
      // as we need to update the text leaf with the new alt text and fire the
      // appropriate text change events. Mixed content gets even messier.
      docAcc->RecreateAccessible(mPseudoElement);
    } else {
      // This is CSS content replacing an element's content.
      MOZ_ASSERT(mRealElement->GetPrimaryFrame());
      MOZ_ASSERT(mRealElement->GetPrimaryFrame()->IsReplaced());
      LocalAccessible* acc = docAcc->GetAccessible(mRealElement);
      MOZ_ASSERT(acc);
      docAcc->FireDelayedEvent(nsIAccessibleEvent::EVENT_NAME_CHANGE, acc);
    }
    return true;
  }
  return false;
}

}  // namespace mozilla::a11y
