/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/InspectorUtils.h"

#include "ChildIterator.h"
#include "gfxTextRun.h"
#include "inLayoutUtils.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/DeclarationBlock.h"
#include "mozilla/EventStateManager.h"
#include "mozilla/PresShell.h"
#include "mozilla/PresShellInlines.h"
#include "mozilla/RefPtr.h"
#include "mozilla/ScrollContainerFrame.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoCSSParser.h"
#include "mozilla/ServoStyleRuleMap.h"
#include "mozilla/ServoStyleSet.h"
#include "mozilla/StaticPrefs_layout.h"
#include "mozilla/StyleSheetInlines.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/CSS2PropertiesBinding.h"
#include "mozilla/dom/CSSBinding.h"
#include "mozilla/dom/CSSKeyframesRule.h"
#include "mozilla/dom/CSSStyleRule.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/CharacterData.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLSlotElement.h"
#include "mozilla/dom/HTMLTemplateElement.h"
#include "mozilla/dom/Highlight.h"
#include "mozilla/dom/HighlightRegistry.h"
#include "mozilla/dom/InspectorFontFace.h"
#include "mozilla/dom/InspectorUtilsBinding.h"
#include "mozilla/dom/LinkStyle.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/gfx/Matrix.h"
#include "nsArray.h"
#include "nsAtom.h"
#include "nsBlockFrame.h"
#include "nsCSSProps.h"
#include "nsCSSValue.h"
#include "nsColor.h"
#include "nsComputedDOMStyle.h"
#include "nsContentList.h"
#include "nsGlobalWindowInner.h"
#include "nsIContentInlines.h"
#include "nsLayoutUtils.h"
#include "nsNameSpaceManager.h"
#include "nsPresContext.h"
#include "nsQueryObject.h"
#include "nsRange.h"
#include "nsString.h"
#include "nsStyleUtil.h"

using namespace mozilla;
using namespace mozilla::css;
using namespace mozilla::dom;

namespace mozilla::dom {

static nsPresContext* EnsureSafeToHandOutRules(Element& aElement) {
  Document* doc = aElement.GetComposedDoc();
  if (!doc) {
    return nullptr;
  }
  const PresShell* presShell = doc->GetPresShell();
  if (!presShell) {
    return nullptr;
  }
  nsPresContext* presContext = presShell->GetPresContext();
  if (!presContext) {
    return nullptr;
  }
  presContext->EnsureSafeToHandOutCSSRules();
  return presContext;
}

static already_AddRefed<const ComputedStyle> GetStartingStyle(
    Element& aElement) {
  // If this element is unstyled, or it doesn't have matched rules in
  // @starting-style, we return.
  if (!Servo_Element_MayHaveStartingStyle(&aElement)) {
    return nullptr;
  }
  if (!EnsureSafeToHandOutRules(aElement)) {
    return nullptr;
  }
  RefPtr<Document> doc = aElement.GetComposedDoc();
  if (!doc) {
    return nullptr;
  }
  doc->FlushPendingNotifications(FlushType::Style);
  RefPtr<PresShell> ps = doc->GetPresShell();
  if (!ps) {
    return nullptr;
  }
  return ps->StyleSet()->ResolveStartingStyle(aElement);
}

static already_AddRefed<const ComputedStyle> GetCleanComputedStyleForElement(
    dom::Element* aElement, const PseudoStyleRequest& aPseudo) {
  MOZ_ASSERT(aElement);
  nsPresContext* pc = EnsureSafeToHandOutRules(*aElement);
  if (!pc) {
    return nullptr;
  }
  return nsComputedDOMStyle::GetComputedStyle(aElement, aPseudo);
}

/* static */
void InspectorUtils::GetAllStyleSheets(GlobalObject& aGlobalObject,
                                       Document& aDocument, bool aDocumentOnly,
                                       nsTArray<RefPtr<StyleSheet>>& aResult) {
  // Get the agent, then user and finally xbl sheets in the style set.
  PresShell* presShell = aDocument.GetPresShell();
  nsTHashSet<StyleSheet*> sheetSet;

  if (presShell) {
    ServoStyleSet* styleSet = presShell->StyleSet();

    if (!aDocumentOnly) {
      const StyleOrigin kOrigins[] = {StyleOrigin::UserAgent,
                                      StyleOrigin::User};
      for (const auto origin : kOrigins) {
        for (size_t i = 0, count = styleSet->SheetCount(origin); i < count;
             i++) {
          aResult.AppendElement(styleSet->SheetAt(origin, i));
        }
      }
    }

    AutoTArray<StyleSheet*, 32> nonDocumentSheets;
    styleSet->AppendAllNonDocumentAuthorSheets(nonDocumentSheets);

    // The non-document stylesheet array can have duplicates due to adopted
    // stylesheets.
    nsTHashSet<StyleSheet*> sheetSet;
    for (StyleSheet* sheet : nonDocumentSheets) {
      if (sheetSet.EnsureInserted(sheet)) {
        aResult.AppendElement(sheet);
      }
    }
  }

  // Get the document sheets.
  for (size_t i = 0; i < aDocument.SheetCount(); i++) {
    aResult.AppendElement(aDocument.SheetAt(i));
  }

  for (auto& sheet : aDocument.AdoptedStyleSheets()) {
    if (sheetSet.EnsureInserted(sheet)) {
      aResult.AppendElement(sheet);
    }
  }
}

bool InspectorUtils::IsIgnorableWhitespace(CharacterData& aDataNode) {
  if (!aDataNode.TextIsOnlyWhitespace()) {
    return false;
  }

  // Okay.  We have only white space.  Let's check the white-space
  // property now and make sure that this isn't preformatted text...
  if (nsIFrame* frame = aDataNode.GetPrimaryFrame()) {
    return !frame->StyleText()->WhiteSpaceIsSignificant();
  }

  // empty inter-tag text node without frame, e.g., in between <table>\n<tr>
  return true;
}

/* static */
nsINode* InspectorUtils::GetParentForNode(nsINode& aNode,
                                          bool aShowingAnonymousContent) {
  if (nsINode* parent = aNode.GetParentNode()) {
    return parent;
  }
  if (aNode.IsDocument()) {
    return inLayoutUtils::GetContainerFor(*aNode.AsDocument());
  }
  if (aShowingAnonymousContent) {
    if (auto* frag = DocumentFragment::FromNode(aNode)) {
      // This deals with shadow roots and HTMLTemplateElement.content.
      return frag->GetHost();
    }
  }
  return nullptr;
}

/* static */
void InspectorUtils::GetChildrenForNode(nsINode& aNode,
                                        bool aShowingAnonymousContent,
                                        bool aIncludeAssignedNodes,
                                        bool aIncludeSubdocuments,
                                        nsTArray<RefPtr<nsINode>>& aResult) {
  if (aIncludeSubdocuments) {
    if (auto* doc = inLayoutUtils::GetSubDocumentFor(&aNode)) {
      aResult.AppendElement(doc);
      // XXX Do we really want to early-return?
      return;
    }
  }

  if (!aShowingAnonymousContent || !aNode.IsContent()) {
    for (nsINode* child = aNode.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      aResult.AppendElement(child);
    }
    return;
  }

  if (auto* tmpl = HTMLTemplateElement::FromNode(aNode)) {
    aResult.AppendElement(tmpl->Content());
    // XXX Do we really want to early-return?
    return;
  }

  if (auto* element = Element::FromNode(aNode)) {
    if (auto* shadow = element->GetShadowRoot()) {
      aResult.AppendElement(shadow);
    }
  }
  nsIContent* parent = aNode.AsContent();
  if (auto* node = nsLayoutUtils::GetMarkerPseudo(parent)) {
    aResult.AppendElement(node);
  }
  if (auto* node = nsLayoutUtils::GetBeforePseudo(parent)) {
    aResult.AppendElement(node);
  }
  if (aIncludeAssignedNodes) {
    if (auto* slot = HTMLSlotElement::FromNode(aNode)) {
      for (nsINode* node : slot->AssignedNodes()) {
        aResult.AppendElement(node);
      }
    }
  }
  for (nsIContent* node = parent->GetFirstChild(); node;
       node = node->GetNextSibling()) {
    aResult.AppendElement(node);
  }
  AutoTArray<nsIContent*, 4> anonKids;
  nsContentUtils::AppendNativeAnonymousChildren(parent, anonKids,
                                                nsIContent::eAllChildren);
  for (nsIContent* node : anonKids) {
    aResult.AppendElement(node);
  }
  if (auto* node = nsLayoutUtils::GetAfterPseudo(parent)) {
    aResult.AppendElement(node);
  }
}

class ReadOnlyInspectorDeclaration final : public nsDOMCSSDeclaration {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(ReadOnlyInspectorDeclaration)

  explicit ReadOnlyInspectorDeclaration(const StyleLockedDeclarationBlock* aRaw)
      : mRaw(aRaw) {}

  nsINode* GetAssociatedNode() const final { return nullptr; }
  nsISupports* GetParentObject() const final { return nullptr; }
  void GetPropertyValue(const nsACString& aPropName, nsACString& aValue) final {
    Servo_DeclarationBlock_GetPropertyValue(mRaw, &aPropName, &aValue);
  }
  void GetPropertyValue(nsCSSPropertyID aId, nsACString& aValue) final {
    Servo_DeclarationBlock_GetPropertyValueById(mRaw, aId, &aValue);
  }
  void IndexedGetter(uint32_t aIndex, bool& aFound,
                     nsACString& aPropName) final {
    aFound = Servo_DeclarationBlock_GetNthProperty(mRaw, aIndex, &aPropName);
  }
  void RemoveProperty(const nsACString& aPropertyName, nsACString& aValue,
                      ErrorResult& aRv) final {
    aRv.ThrowInvalidModificationError("Can't mutate this declaration");
  }
  void SetProperty(const nsACString& aPropertyName, const nsACString& aValue,
                   const nsACString& aPriority, nsIPrincipal* aSubjectPrincipal,
                   ErrorResult& aRv) final {
    aRv.ThrowInvalidModificationError("Can't mutate this declaration");
  }
  void SetPropertyValue(nsCSSPropertyID aId, const nsACString& aValue,
                        nsIPrincipal* aSubjectPrincipal,
                        ErrorResult& aRv) final {
    aRv.ThrowInvalidModificationError("Can't mutate this declaration");
  }
  void SetCssText(const nsACString& aString, nsIPrincipal* aSubjectPrincipal,
                  ErrorResult& aRv) final {
    aRv.ThrowInvalidModificationError("Can't mutate this declaration");
  }
  void GetCssText(nsACString& aString) final {
    Servo_DeclarationBlock_GetCssText(mRaw, &aString);
  }
  uint32_t Length() final { return Servo_DeclarationBlock_Count(mRaw); }
  void GetPropertyPriority(const nsACString& aPropName,
                           nsACString& aPriority) final {
    if (Servo_DeclarationBlock_GetPropertyIsImportant(mRaw, &aPropName)) {
      aPriority.AssignLiteral("important");
    }
  }
  css::Rule* GetParentRule() final { return nullptr; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) final {
    return CSS2Properties_Binding::Wrap(aCx, this, aGivenProto);
  }
  // These ones are a bit sad, but matches e.g. nsComputedDOMStyle.
  nsresult SetCSSDeclaration(DeclarationBlock* aDecl,
                             MutationClosureData*) final {
    MOZ_CRASH("called ReadOnlyInspectorDeclaration::SetCSSDeclaration");
  }
  DeclarationBlock* GetOrCreateCSSDeclaration(Operation,
                                              DeclarationBlock**) override {
    MOZ_CRASH("called ReadOnlyInspectorDeclaration::GetOrCreateCSSDeclaration");
  }
  ParsingEnvironment GetParsingEnvironment(nsIPrincipal*) const final {
    MOZ_CRASH("called ReadOnlyInspectorDeclaration::GetParsingEnvironment");
  }

 private:
  ~ReadOnlyInspectorDeclaration() = default;

  RefPtr<const StyleLockedDeclarationBlock> mRaw;
};

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ReadOnlyInspectorDeclaration)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsICSSDeclaration)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(ReadOnlyInspectorDeclaration)
NS_IMPL_CYCLE_COLLECTING_RELEASE(ReadOnlyInspectorDeclaration)
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_0(ReadOnlyInspectorDeclaration)

static void GetCSSRulesFromComputedValues(
    Element& aElement, const ComputedStyle* aComputedStyle,
    nsTArray<OwningCSSRuleOrInspectorDeclaration>& aResult) {
  const PresShell* presShell = aElement.OwnerDoc()->GetPresShell();
  if (!presShell) {
    return;
  }

  AutoTArray<StyleMatchingDeclarationBlock, 8> rawDecls;
  Servo_ComputedValues_GetMatchingDeclarations(aComputedStyle, &rawDecls);

  AutoTArray<ServoStyleRuleMap*, 8> maps;
  {
    ServoStyleSet* styleSet = presShell->StyleSet();
    ServoStyleRuleMap* map = styleSet->StyleRuleMap();
    maps.AppendElement(map);
  }

  // Now shadow DOM stuff...
  if (auto* shadow = aElement.GetShadowRoot()) {
    maps.AppendElement(&shadow->ServoStyleRuleMap());
  }

  // Now NAC:
  for (auto* el = aElement.GetClosestNativeAnonymousSubtreeRootParentOrHost();
       el; el = el->GetClosestNativeAnonymousSubtreeRootParentOrHost()) {
    if (auto* shadow = el->GetShadowRoot()) {
      maps.AppendElement(&shadow->ServoStyleRuleMap());
    }
  }

  for (auto* shadow = aElement.GetContainingShadow(); shadow;
       shadow = shadow->Host()->GetContainingShadow()) {
    maps.AppendElement(&shadow->ServoStyleRuleMap());
  }

  // Rules from the assigned slot.
  for (auto* slot = aElement.GetAssignedSlot(); slot;
       slot = slot->GetAssignedSlot()) {
    if (auto* shadow = slot->GetContainingShadow()) {
      maps.AppendElement(&shadow->ServoStyleRuleMap());
    }
  }

  // Find matching rules in the table.
  for (const StyleMatchingDeclarationBlock& block : Reversed(rawDecls)) {
    bool found = false;
    for (ServoStyleRuleMap* map : maps) {
      if (css::Rule* rule = map->Lookup(block.block)) {
        aResult.AppendElement()->SetAsCSSRule() = rule;
        found = true;
        break;
      }
    }
    if (!found) {
      auto& declaration = aResult.AppendElement()->SetAsInspectorDeclaration();
      declaration.mStyle = OwningNonNull<ReadOnlyInspectorDeclaration>(
          *new ReadOnlyInspectorDeclaration(block.block));
      declaration.mDeclarationOrigin = [&] {
        switch (block.origin) {
          case StyleMatchingDeclarationBlockOrigin::Author:
            return DeclarationOrigin::Style_attribute;
          case StyleMatchingDeclarationBlockOrigin::User:
            MOZ_ASSERT_UNREACHABLE(
                "Where did this user agent declaration come from?");
            return DeclarationOrigin::User;
          case StyleMatchingDeclarationBlockOrigin::UserAgent:
            return DeclarationOrigin::User_agent;
          case StyleMatchingDeclarationBlockOrigin::Animations:
            return DeclarationOrigin::Animations;
          case StyleMatchingDeclarationBlockOrigin::Transitions:
            return DeclarationOrigin::Transitions;
          case StyleMatchingDeclarationBlockOrigin::SMIL:
            return DeclarationOrigin::Smil;
          case StyleMatchingDeclarationBlockOrigin::PresHints:
            return DeclarationOrigin::Pres_hints;
        }
        MOZ_ASSERT_UNREACHABLE("Unkown origin?");
        return DeclarationOrigin::Pres_hints;
      }();
    }
  }
}

/* static */
void InspectorUtils::GetMatchingCSSRules(
    GlobalObject& aGlobalObject, Element& aElement, const nsAString& aPseudo,
    bool aIncludeVisitedStyle, bool aWithStartingStyle,
    nsTArray<OwningCSSRuleOrInspectorDeclaration>& aResult) {
  auto pseudo = nsCSSPseudoElements::ParsePseudoElement(
      aPseudo, CSSEnabledState::ForAllContent);
  if (!pseudo) {
    return;
  }

  RefPtr<const ComputedStyle> computedStyle;
  if (aWithStartingStyle) {
    computedStyle = GetStartingStyle(aElement);
  }

  // Note: GetStartingStyle() return nullptr if this element doesn't have rules
  // inside @starting-style. For this case, we would like to return the primay
  // rules of this element.
  if (!computedStyle) {
    computedStyle = GetCleanComputedStyleForElement(&aElement, *pseudo);
  }

  if (!computedStyle) {
    // This can fail for elements that are not in the document or
    // if the document they're in doesn't have a presshell.  Bail out.
    return;
  }

  if (aIncludeVisitedStyle) {
    if (const auto* styleIfVisited = computedStyle->GetStyleIfVisited()) {
      computedStyle = styleIfVisited;
    }
  }

  GetCSSRulesFromComputedValues(aElement, computedStyle, aResult);
}

/* static */
uint32_t InspectorUtils::GetRuleLine(GlobalObject& aGlobal, css::Rule& aRule) {
  uint32_t line = aRule.GetLineNumber();
  if (StyleSheet* sheet = aRule.GetStyleSheet()) {
    if (auto* link = LinkStyle::FromNodeOrNull(sheet->GetOwnerNode())) {
      line += link->GetLineNumber();
    }
  }
  return line;
}

/* static */
uint32_t InspectorUtils::GetRuleColumn(GlobalObject& aGlobal,
                                       css::Rule& aRule) {
  return aRule.GetColumnNumber();
}

/* static */
uint32_t InspectorUtils::GetRelativeRuleLine(GlobalObject& aGlobal,
                                             css::Rule& aRule) {
  // Rule lines are 0-based, but inspector wants 1-based.
  return aRule.GetLineNumber() + 1;
}

void InspectorUtils::GetRuleIndex(GlobalObject& aGlobal, css::Rule& aRule,
                                  nsTArray<uint32_t>& aResult) {
  css::Rule* currentRule = &aRule;

  do {
    css::Rule* parentRule = currentRule->GetParentRule();
    dom::CSSRuleList* ruleList = nullptr;

    if (parentRule) {
      if (parentRule->IsGroupRule()) {
        ruleList = static_cast<css::GroupRule*>(parentRule)->CssRules();
      } else if (parentRule->Type() == StyleCssRuleType::Keyframes) {
        ruleList = static_cast<CSSKeyframesRule*>(parentRule)->CssRules();
      } else {
        MOZ_ASSERT_UNREACHABLE("Unknown parent rule type?");
      }
    } else if (StyleSheet* sheet = currentRule->GetStyleSheet()) {
      ruleList = sheet->GetCssRulesInternal();
    }

    if (!ruleList) {
      return;
    }

    bool found = false;
    for (uint32_t i = 0, len = ruleList->Length(); i < len; ++i) {
      css::Rule* rule = ruleList->Item(i);
      if (currentRule == rule) {
        found = true;
        aResult.InsertElementAt(0, i);
        break;
      }
    }

    if (!found) {
      return;
    }

    currentRule = parentRule;
  } while (currentRule);
}

/* static */
bool InspectorUtils::HasRulesModifiedByCSSOM(GlobalObject& aGlobal,
                                             StyleSheet& aSheet) {
  return aSheet.HasModifiedRulesForDevtools();
}

static uint32_t CollectAtRules(ServoCSSRuleList& aRuleList,
                               Sequence<OwningNonNull<css::Rule>>& aResult) {
  uint32_t len = aRuleList.Length();
  uint32_t ruleCount = len;
  for (uint32_t i = 0; i < len; ++i) {
    css::Rule* rule = aRuleList.GetRule(i);
    // This collect rules we want to display in Devtools Style Editor toolbar.
    // When adding a new StyleCssRuleType, put it in the "default" list, and
    // file a new bug with
    // https://bugzilla.mozilla.org/enter_bug.cgi?product=DevTools&component=Style%20Editor&short_desc=Consider%20displaying%20new%20XXX%20rule%20type%20in%20at-rules%20sidebar
    // so the DevTools team gets notified and can decide if it should be
    // displayed.
    switch (rule->Type()) {
      case StyleCssRuleType::Media:
      case StyleCssRuleType::Supports:
      case StyleCssRuleType::LayerBlock:
      case StyleCssRuleType::Property:
      case StyleCssRuleType::Container: {
        Unused << aResult.AppendElement(OwningNonNull(*rule), fallible);
        break;
      }
      case StyleCssRuleType::Style:
      case StyleCssRuleType::Import:
      case StyleCssRuleType::Document:
      case StyleCssRuleType::LayerStatement:
      case StyleCssRuleType::FontFace:
      case StyleCssRuleType::Page:
      case StyleCssRuleType::Keyframes:
      case StyleCssRuleType::Keyframe:
      case StyleCssRuleType::Margin:
      case StyleCssRuleType::Namespace:
      case StyleCssRuleType::CounterStyle:
      case StyleCssRuleType::FontFeatureValues:
      case StyleCssRuleType::FontPaletteValues:
      case StyleCssRuleType::Scope:
      case StyleCssRuleType::StartingStyle:
      case StyleCssRuleType::PositionTry:
      case StyleCssRuleType::NestedDeclarations:
        break;
    }

    if (rule->IsGroupRule()) {
      ruleCount += CollectAtRules(
          *static_cast<css::GroupRule*>(rule)->CssRules(), aResult);
    }
  }
  return ruleCount;
}

void InspectorUtils::GetStyleSheetRuleCountAndAtRules(
    GlobalObject& aGlobal, StyleSheet& aSheet,
    InspectorStyleSheetRuleCountAndAtRulesResult& aResult) {
  aResult.mRuleCount =
      CollectAtRules(*aSheet.GetCssRulesInternal(), aResult.mAtRules);
}

/* static */
bool InspectorUtils::IsInheritedProperty(GlobalObject& aGlobalObject,
                                         Document& aDocument,
                                         const nsACString& aPropertyName) {
  return Servo_Property_IsInherited(aDocument.EnsureStyleSet().RawData(),
                                    &aPropertyName);
}

/* static */
void InspectorUtils::GetCSSPropertyNames(GlobalObject& aGlobalObject,
                                         const PropertyNamesOptions& aOptions,
                                         nsTArray<nsString>& aResult) {
  CSSEnabledState enabledState = aOptions.mIncludeExperimentals
                                     ? CSSEnabledState::IgnoreEnabledState
                                     : CSSEnabledState::ForAllContent;

  auto appendProperty = [enabledState, &aResult](uint32_t prop) {
    nsCSSPropertyID cssProp = nsCSSPropertyID(prop);
    if (nsCSSProps::IsEnabled(cssProp, enabledState)) {
      aResult.AppendElement(
          NS_ConvertASCIItoUTF16(nsCSSProps::GetStringValue(cssProp)));
    }
  };

  uint32_t prop = 0;
  for (; prop < eCSSProperty_COUNT_no_shorthands; ++prop) {
    if (!nsCSSProps::PropHasFlags(nsCSSPropertyID(prop),
                                  CSSPropFlags::Inaccessible)) {
      appendProperty(prop);
    }
  }

  if (aOptions.mIncludeShorthands) {
    for (; prop < eCSSProperty_COUNT; ++prop) {
      appendProperty(prop);
    }
  }

  if (aOptions.mIncludeAliases) {
    for (prop = eCSSProperty_COUNT; prop < eCSSProperty_COUNT_with_aliases;
         ++prop) {
      appendProperty(prop);
    }
  }
}

/* static */
void InspectorUtils::GetCSSPropertyPrefs(GlobalObject& aGlobalObject,
                                         nsTArray<PropertyPref>& aResult) {
  for (const auto* src = nsCSSProps::kPropertyPrefTable;
       src->mPropID != eCSSProperty_UNKNOWN; src++) {
    PropertyPref& dest = *aResult.AppendElement();
    dest.mName.Assign(
        NS_ConvertASCIItoUTF16(nsCSSProps::GetStringValue(src->mPropID)));
    dest.mPref.AssignASCII(src->mPref);
  }
}

/* static */
void InspectorUtils::GetSubpropertiesForCSSProperty(GlobalObject& aGlobal,
                                                    const nsACString& aProperty,
                                                    nsTArray<nsString>& aResult,
                                                    ErrorResult& aRv) {
  nsCSSPropertyID propertyID = nsCSSProps::LookupProperty(aProperty);

  if (propertyID == eCSSProperty_UNKNOWN) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  if (propertyID == eCSSPropertyExtra_variable) {
    aResult.AppendElement(NS_ConvertUTF8toUTF16(aProperty));
    return;
  }

  if (!nsCSSProps::IsShorthand(propertyID)) {
    nsString* name = aResult.AppendElement();
    CopyASCIItoUTF16(nsCSSProps::GetStringValue(propertyID), *name);
    return;
  }

  for (const nsCSSPropertyID* props =
           nsCSSProps::SubpropertyEntryFor(propertyID);
       *props != eCSSProperty_UNKNOWN; ++props) {
    nsString* name = aResult.AppendElement();
    CopyASCIItoUTF16(nsCSSProps::GetStringValue(*props), *name);
  }
}

/* static */
bool InspectorUtils::CssPropertyIsShorthand(GlobalObject& aGlobalObject,
                                            const nsACString& aProperty,
                                            ErrorResult& aRv) {
  bool found;
  bool isShorthand = Servo_Property_IsShorthand(&aProperty, &found);
  if (!found) {
    aRv.Throw(NS_ERROR_FAILURE);
  }
  return isShorthand;
}

// This should match the constants in specified_value_info.rs
//
// Once we can use bitflags in consts, we can also cbindgen that and use them
// here instead.
static uint8_t ToServoCssType(InspectorPropertyType aType) {
  switch (aType) {
    case InspectorPropertyType::Color:
      return 1;
    case InspectorPropertyType::Gradient:
      return 1 << 1;
    case InspectorPropertyType::Timing_function:
      return 1 << 2;
    default:
      MOZ_ASSERT_UNREACHABLE("Unknown property type?");
      return 0;
  }
}

bool InspectorUtils::Supports(GlobalObject&, const nsACString& aDeclaration,
                              const SupportsOptions& aOptions) {
  return Servo_CSSSupports(&aDeclaration, aOptions.mUserAgent, aOptions.mChrome,
                           aOptions.mQuirks);
}

bool InspectorUtils::CssPropertySupportsType(GlobalObject& aGlobalObject,
                                             const nsACString& aProperty,
                                             InspectorPropertyType aType,
                                             ErrorResult& aRv) {
  bool found;
  bool result =
      Servo_Property_SupportsType(&aProperty, ToServoCssType(aType), &found);
  if (!found) {
    aRv.Throw(NS_ERROR_FAILURE);
    return false;
  }
  return result;
}

/* static */
void InspectorUtils::GetCSSValuesForProperty(GlobalObject& aGlobalObject,
                                             const nsACString& aProperty,
                                             nsTArray<nsString>& aResult,
                                             ErrorResult& aRv) {
  bool found;
  Servo_Property_GetCSSValuesForProperty(&aProperty, &found, &aResult);
  if (!found) {
    aRv.Throw(NS_ERROR_FAILURE);
  }
}

/* static */
void InspectorUtils::RgbToColorName(GlobalObject&, uint8_t aR, uint8_t aG,
                                    uint8_t aB, nsACString& aColorName) {
  Servo_SlowRgbToColorName(aR, aG, aB, &aColorName);
}

/* static */
void InspectorUtils::ColorToRGBA(GlobalObject& aGlobal,
                                 const nsACString& aColorString,
                                 Nullable<InspectorRGBATuple>& aResult) {
  const auto* styleData = [&]() -> const StylePerDocumentStyleData* {
    nsCOMPtr<nsIGlobalObject> global =
        do_QueryInterface(aGlobal.GetAsSupports());
    if (!global) {
      return nullptr;
    }
    auto* win = global->GetAsInnerWindow();
    if (!win) {
      return nullptr;
    }
    Document* doc = win->GetExtantDoc();
    if (!doc) {
      return nullptr;
    }
    PresShell* ps = doc->GetPresShell();
    if (!ps) {
      return nullptr;
    }
    return ps->StyleSet()->RawData();
  }();

  nscolor color = NS_RGB(0, 0, 0);
  if (!ServoCSSParser::ComputeColor(styleData, NS_RGB(0, 0, 0), aColorString,
                                    &color)) {
    aResult.SetNull();
    return;
  }

  InspectorRGBATuple& tuple = aResult.SetValue();
  tuple.mR = NS_GET_R(color);
  tuple.mG = NS_GET_G(color);
  tuple.mB = NS_GET_B(color);
  tuple.mA = nsStyleUtil::ColorComponentToFloat(NS_GET_A(color));
}

/* static */
void InspectorUtils::ColorTo(GlobalObject&, const nsACString& aFromColor,
                             const nsACString& aToColorSpace,
                             Nullable<InspectorColorToResult>& aResult) {
  nsCString resultColor;
  nsTArray<float> resultComponents;
  bool resultAdjusted = false;

  if (!ServoCSSParser::ColorTo(aFromColor, aToColorSpace, &resultColor,
                               &resultComponents, &resultAdjusted)) {
    aResult.SetNull();
    return;
  }

  auto& result = aResult.SetValue();
  result.mColor.AssignASCII(resultColor);
  result.mComponents = std::move(resultComponents);
  result.mAdjusted = resultAdjusted;
}

/* static */
bool InspectorUtils::IsValidCSSColor(GlobalObject& aGlobalObject,
                                     const nsACString& aColorString) {
  return ServoCSSParser::IsValidCSSColor(aColorString);
}

/* static */
bool InspectorUtils::SetContentState(GlobalObject& aGlobalObject,
                                     Element& aElement, uint64_t aState,
                                     ErrorResult& aRv) {
  RefPtr<EventStateManager> esm =
      inLayoutUtils::GetEventStateManagerFor(aElement);
  ElementState state(aState);
  if (!esm || !EventStateManager::ManagesState(state)) {
    aRv.Throw(NS_ERROR_INVALID_ARG);
    return false;
  }
  return esm->SetContentState(&aElement, state);
}

/* static */
bool InspectorUtils::RemoveContentState(GlobalObject& aGlobalObject,
                                        Element& aElement, uint64_t aState,
                                        bool aClearActiveDocument,
                                        ErrorResult& aRv) {
  RefPtr<EventStateManager> esm =
      inLayoutUtils::GetEventStateManagerFor(aElement);
  ElementState state(aState);
  if (!esm || !EventStateManager::ManagesState(state)) {
    aRv.Throw(NS_ERROR_INVALID_ARG);
    return false;
  }

  bool result = esm->SetContentState(nullptr, state);

  if (aClearActiveDocument && state == ElementState::ACTIVE) {
    EventStateManager* activeESM = static_cast<EventStateManager*>(
        EventStateManager::GetActiveEventStateManager());
    if (activeESM == esm) {
      EventStateManager::ClearGlobalActiveContent(nullptr);
    }
  }

  return result;
}

/* static */
uint64_t InspectorUtils::GetContentState(GlobalObject& aGlobalObject,
                                         Element& aElement) {
  // NOTE: if this method is removed,
  // please remove GetInternalValue from ElementState
  return aElement.State().GetInternalValue();
}

/* static */
void InspectorUtils::GetUsedFontFaces(GlobalObject& aGlobalObject,
                                      nsRange& aRange, uint32_t aMaxRanges,
                                      bool aSkipCollapsedWhitespace,
                                      nsLayoutUtils::UsedFontFaceList& aResult,
                                      ErrorResult& aRv) {
  nsresult rv =
      aRange.GetUsedFontFaces(aResult, aMaxRanges, aSkipCollapsedWhitespace);

  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }
}

static ElementState GetStatesForPseudoClass(const nsAString& aStatePseudo) {
  if (aStatePseudo.IsEmpty() || aStatePseudo[0] != u':') {
    return ElementState();
  }
  NS_ConvertUTF16toUTF8 statePseudo(Substring(aStatePseudo, 1));
  return ElementState(Servo_PseudoClass_GetStates(&statePseudo));
}

/* static */
void InspectorUtils::GetCSSPseudoElementNames(GlobalObject& aGlobalObject,
                                              nsTArray<nsString>& aResult) {
  const auto kPseudoCount =
      static_cast<size_t>(PseudoStyleType::CSSPseudoElementsEnd);
  for (size_t i = 0; i < kPseudoCount; ++i) {
    PseudoStyleType type = static_cast<PseudoStyleType>(i);
    if (!nsCSSPseudoElements::IsEnabled(type, CSSEnabledState::ForAllContent)) {
      continue;
    }
    auto& string = *aResult.AppendElement();
    // Use two semi-colons (though internally we use one).
    string.Append(u':');
    nsAtom* atom = nsCSSPseudoElements::GetPseudoAtom(type);
    string.Append(nsDependentAtomString(atom));
  }
}

/* static */
void InspectorUtils::AddPseudoClassLock(GlobalObject& aGlobalObject,
                                        Element& aElement,
                                        const nsAString& aPseudoClass,
                                        bool aEnabled) {
  ElementState state = GetStatesForPseudoClass(aPseudoClass);
  if (state.IsEmpty()) {
    return;
  }

  aElement.LockStyleStates(state, aEnabled);
}

/* static */
void InspectorUtils::RemovePseudoClassLock(GlobalObject& aGlobal,
                                           Element& aElement,
                                           const nsAString& aPseudoClass) {
  ElementState state = GetStatesForPseudoClass(aPseudoClass);
  if (state.IsEmpty()) {
    return;
  }

  aElement.UnlockStyleStates(state);
}

/* static */
bool InspectorUtils::HasPseudoClassLock(GlobalObject& aGlobalObject,
                                        Element& aElement,
                                        const nsAString& aPseudoClass) {
  ElementState state = GetStatesForPseudoClass(aPseudoClass);
  if (state.IsEmpty()) {
    return false;
  }

  ElementState locks = aElement.LockedStyleStates().mLocks;
  return locks.HasAllStates(state);
}

/* static */
void InspectorUtils::ClearPseudoClassLocks(GlobalObject& aGlobalObject,
                                           Element& aElement) {
  aElement.ClearStyleStateLocks();
}

/* static */
void InspectorUtils::ParseStyleSheet(GlobalObject& aGlobalObject,
                                     StyleSheet& aSheet,
                                     const nsACString& aInput,
                                     ErrorResult& aRv) {
  aSheet.ReparseSheet(aInput, aRv);
}

bool InspectorUtils::IsCustomElementName(GlobalObject&, const nsAString& aName,
                                         const nsAString& aNamespaceURI) {
  if (aName.IsEmpty()) {
    return false;
  }

  int32_t namespaceID;
  nsNameSpaceManager::GetInstance()->RegisterNameSpace(aNamespaceURI,
                                                       namespaceID);

  RefPtr<nsAtom> nameElt = NS_Atomize(aName);
  return nsContentUtils::IsCustomElementName(nameElt, namespaceID);
}

bool InspectorUtils::IsElementThemed(GlobalObject&, Element& aElement) {
  // IsThemed will check if the native theme supports the widget using
  // ThemeSupportsWidget which in turn will check that the widget is not
  // already styled by content through nsNativeTheme::IsWidgetStyled. We
  // assume that if the native theme styles the widget and the author did not
  // override the appropriate styles, the theme will provide focus styling.
  nsIFrame* frame = aElement.GetPrimaryFrame(FlushType::Frames);
  return frame && frame->IsThemed();
}

bool InspectorUtils::IsUsedColorSchemeDark(GlobalObject&, Element& aElement) {
  nsIFrame* frame = aElement.GetPrimaryFrame(FlushType::Frames);
  return frame && LookAndFeel::ColorSchemeForFrame(frame) == ColorScheme::Dark;
}

Element* InspectorUtils::ContainingBlockOf(GlobalObject&, Element& aElement) {
  nsIFrame* frame = aElement.GetPrimaryFrame(FlushType::Frames);
  if (!frame) {
    return nullptr;
  }
  nsIFrame* cb = frame->GetContainingBlock();
  if (!cb) {
    return nullptr;
  }
  return Element::FromNodeOrNull(cb->GetContent());
}

void InspectorUtils::GetBlockLineCounts(GlobalObject& aGlobal,
                                        Element& aElement,
                                        Nullable<nsTArray<uint32_t>>& aResult) {
  nsBlockFrame* block =
      do_QueryFrame(aElement.GetPrimaryFrame(FlushType::Layout));
  if (!block) {
    aResult.SetNull();
    return;
  }

  // If CSS columns were specified on the actual block element (rather than an
  // ancestor block, GetPrimaryFrame will return its ColumnSetWrapperFrame, and
  // we need to drill down to the actual block that contains the lines.
  if (block->IsColumnSetWrapperFrame()) {
    nsIFrame* firstChild = block->PrincipalChildList().FirstChild();
    if (!firstChild->IsColumnSetFrame()) {
      aResult.SetNull();
      return;
    }
    block = do_QueryFrame(firstChild->PrincipalChildList().FirstChild());
    if (!block || block->GetContent() != &aElement) {
      aResult.SetNull();
      return;
    }
  }

  nsTArray<uint32_t> result;
  do {
    result.AppendElement(block->Lines().size());
    block = static_cast<nsBlockFrame*>(block->GetNextInFlow());
  } while (block);

  aResult.SetValue(std::move(result));
}

static bool FrameHasSpecifiedSize(const nsIFrame* aFrame) {
  auto wm = aFrame->GetWritingMode();

  const nsStylePosition* stylePos = aFrame->StylePosition();
  const auto anchorResolutionParams = AnchorPosResolutionParams::From(aFrame);

  return stylePos->ISize(wm, anchorResolutionParams)->IsLengthPercentage() ||
         stylePos->BSize(wm, anchorResolutionParams)->IsLengthPercentage();
}

static bool IsFrameOutsideOfAncestor(const nsIFrame* aFrame,
                                     const nsIFrame* aAncestorFrame,
                                     const nsRect& aAncestorRect) {
  nsRect frameRectInAncestorSpace = nsLayoutUtils::TransformFrameRectToAncestor(
      aFrame, aFrame->ScrollableOverflowRect(), RelativeTo{aAncestorFrame},
      nullptr, nullptr, false, nullptr);

  // We use nsRect::SaturatingUnionEdges because it correctly handles the case
  // of a zero-width or zero-height frame, which we still want to consider as
  // contributing to the union.
  nsRect unionizedRect =
      frameRectInAncestorSpace.SaturatingUnionEdges(aAncestorRect);

  // If frameRectInAncestorSpace is inside aAncestorRect then union of
  // frameRectInAncestorSpace and aAncestorRect should be equal to aAncestorRect
  // hence if it is equal, then false should be returned.

  return !(unionizedRect == aAncestorRect);
}

static void AddOverflowingChildrenOfElement(const nsIFrame* aFrame,
                                            const nsIFrame* aAncestorFrame,
                                            const nsRect& aRect,
                                            nsSimpleContentList& aList) {
  MOZ_ASSERT(aFrame, "we assume the passed-in frame is non-null");
  for (const auto& childList : aFrame->ChildLists()) {
    for (const nsIFrame* child : childList.mList) {
      // We want to identify if the child or any of its children have a
      // frame that is outside of aAncestorFrame. Ideally, child would have
      // a frame rect that encompasses all of its children, but this is not
      // guaranteed by the frame tree. So instead we first check other
      // conditions that indicate child is an interesting frame:
      //
      // 1) child has a specified size
      // 2) none of child's children are implicated
      //
      // If either of these conditions are true, we *then* check if child's
      // frame is outside of aAncestorFrame, and if so, we add child's content
      // to aList.

      if (FrameHasSpecifiedSize(child) &&
          IsFrameOutsideOfAncestor(child, aAncestorFrame, aRect)) {
        aList.MaybeAppendElement(child->GetContent());
        continue;
      }

      uint32_t currListLength = aList.Length();
      AddOverflowingChildrenOfElement(child, aAncestorFrame, aRect, aList);

      // If child is a leaf node, length of aList should remain same after
      // calling AddOverflowingChildrenOfElement on it.
      if (currListLength == aList.Length() &&
          IsFrameOutsideOfAncestor(child, aAncestorFrame, aRect)) {
        aList.MaybeAppendElement(child->GetContent());
      }
    }
  }
}

already_AddRefed<nsINodeList> InspectorUtils::GetOverflowingChildrenOfElement(
    GlobalObject& aGlobal, Element& aElement) {
  auto list = MakeRefPtr<nsSimpleContentList>(&aElement);
  const ScrollContainerFrame* scrollContainerFrame =
      aElement.GetScrollContainerFrame();
  // Element must be a ScrollContainerFrame.
  if (!scrollContainerFrame) {
    return list.forget();
  }

  auto scrollPortRect = scrollContainerFrame->GetScrollPortRect();
  const nsIFrame* scrolledFrame = scrollContainerFrame->GetScrolledFrame();
  AddOverflowingChildrenOfElement(scrolledFrame, scrollContainerFrame,
                                  scrollPortRect, *list);
  return list.forget();
}

/* static */
void InspectorUtils::GetRegisteredCssHighlights(GlobalObject& aGlobalObject,
                                                Document& aDocument,
                                                bool aActiveOnly,
                                                nsTArray<nsString>& aResult) {
  for (auto const& iter : aDocument.HighlightRegistry().HighlightsOrdered()) {
    const RefPtr<nsAtom>& highlightName = iter.first();
    const RefPtr<Highlight>& highlight = iter.second();
    if (!aActiveOnly || highlight->Size() > 0) {
      aResult.AppendElement(highlightName->GetUTF16String());
    }
  }
}

/* static */
void InspectorUtils::GetCSSRegisteredProperties(
    GlobalObject& aGlobalObject, Document& aDocument,
    nsTArray<InspectorCSSPropertyDefinition>& aResult) {
  nsTArray<StylePropDef> result;

  ServoStyleSet& styleSet = aDocument.EnsureStyleSet();
  // Update the rules before looking up @property rules.
  styleSet.UpdateStylistIfNeeded();

  Servo_GetRegisteredCustomProperties(styleSet.RawData(), &result);
  for (const auto& propDef : result) {
    InspectorCSSPropertyDefinition& property = *aResult.AppendElement();

    // Servo does not include the "--" prefix in the property definition name.
    // Add it back as it's easier for DevTools to handle them _with_ "--".
    property.mName.AssignLiteral("--");
    property.mName.Append(nsAtomCString(propDef.name.AsAtom()));
    property.mSyntax.Append(propDef.syntax);
    property.mInherits = propDef.inherits;
    if (propDef.has_initial_value) {
      property.mInitialValue.Append(propDef.initial_value);
    } else {
      property.mInitialValue.SetIsVoid(true);
    }
    property.mFromJS = propDef.from_js;
  }
}

/* static */
void InspectorUtils::GetCSSRegisteredProperty(
    GlobalObject& aGlobalObject, Document& aDocument, const nsACString& aName,
    Nullable<InspectorCSSPropertyDefinition>& aResult) {
  StylePropDef result{StyleAtom(NS_Atomize(aName))};

  // Update the rules before looking up @property rules.
  ServoStyleSet& styleSet = aDocument.EnsureStyleSet();
  styleSet.UpdateStylistIfNeeded();

  if (!Servo_GetRegisteredCustomProperty(styleSet.RawData(), &aName, &result)) {
    aResult.SetNull();
    return;
  }

  InspectorCSSPropertyDefinition& propDef = aResult.SetValue();

  // Servo does not include the "--" prefix in the property definition name.
  // Add it back as it's easier for DevTools to handle them _with_ "--".
  propDef.mName.AssignLiteral("--");
  propDef.mName.Append(nsAtomCString(result.name.AsAtom()));
  propDef.mSyntax.Append(result.syntax);
  propDef.mInherits = result.inherits;
  if (result.has_initial_value) {
    propDef.mInitialValue.Append(result.initial_value);
  } else {
    propDef.mInitialValue.SetIsVoid(true);
  }
  propDef.mFromJS = result.from_js;
}

/* static */
bool InspectorUtils::ValueMatchesSyntax(GlobalObject&, Document& aDocument,
                                        const nsACString& aValue,
                                        const nsACString& aSyntax) {
  return Servo_Value_Matches_Syntax(&aValue, &aSyntax,
                                    aDocument.DefaultStyleAttrURLData());
}

/* static */
void InspectorUtils::GetRuleBodyText(GlobalObject&,
                                     const nsACString& aInitialText,
                                     nsACString& aBodyText) {
  Servo_GetRuleBodyText(&aInitialText, &aBodyText);
}

/* static */
void InspectorUtils::ReplaceBlockRuleBodyTextInStylesheet(
    GlobalObject&, const nsACString& aStyleSheetText, uint32_t aLine,
    uint32_t aColumn, const nsACString& aNewBodyText,
    nsACString& aNewStyleSheetText) {
  Servo_ReplaceBlockRuleBodyTextInStylesheetText(
      &aStyleSheetText, aLine, aColumn, &aNewBodyText, &aNewStyleSheetText);
}

void InspectorUtils::SetVerticalClipping(GlobalObject&,
                                         BrowsingContext* aContext,
                                         mozilla::ScreenIntCoord aOffset) {
  MOZ_ASSERT(XRE_IsParentProcess());
  if (!aContext) {
    return;
  }

  CanonicalBrowsingContext* canonical = aContext->Canonical();
  if (!canonical) {
    return;
  }

  BrowserParent* parent = canonical->GetBrowserParent();
  if (!parent) {
    return;
  }
  parent->DynamicToolbarOffsetChanged(aOffset);

  RefPtr<nsIWidget> widget = canonical->GetParentProcessWidgetContaining();
  if (!widget) {
    return;
  }
  widget->DynamicToolbarOffsetChanged(aOffset);
}

void InspectorUtils::SetDynamicToolbarMaxHeight(
    GlobalObject&, BrowsingContext* aContext, mozilla::ScreenIntCoord aHeight) {
  MOZ_ASSERT(XRE_IsParentProcess());
  if (!aContext) {
    return;
  }

  CanonicalBrowsingContext* canonical = aContext->Canonical();
  if (!canonical) {
    return;
  }

  BrowserParent* parent = canonical->GetBrowserParent();
  if (!parent) {
    return;
  }

  parent->DynamicToolbarMaxHeightChanged(aHeight);
}
}  // namespace mozilla::dom
