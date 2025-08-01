/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HTMLEditUtils_h
#define HTMLEditUtils_h

/**
 * This header declares/defines static helper methods as members of
 * HTMLEditUtils.  If you want to create or look for helper trivial classes for
 * HTMLEditor, see HTMLEditHelpers.h.
 */

#include "EditorBase.h"
#include "EditorDOMPoint.h"
#include "EditorForwards.h"
#include "EditorLineBreak.h"
#include "EditorUtils.h"
#include "HTMLEditHelpers.h"

#include "mozilla/Attributes.h"
#include "mozilla/EnumSet.h"
#include "mozilla/IntegerRange.h"
#include "mozilla/Maybe.h"
#include "mozilla/Result.h"
#include "mozilla/dom/AbstractRange.h"
#include "mozilla/dom/AncestorIterator.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLBRElement.h"
#include "mozilla/dom/Selection.h"
#include "mozilla/dom/Text.h"

#include "nsContentUtils.h"
#include "nsCRT.h"
#include "nsGkAtoms.h"
#include "nsHTMLTags.h"
#include "nsTArray.h"
#include "nsTextFragment.h"

class nsAtom;
class nsPresContext;

namespace mozilla {

enum class CollectChildrenOption {
  // Ignore non-editable nodes
  IgnoreNonEditableChildren,
  // Ignore invisible text nodes
  IgnoreInvisibleTextNodes,
  // Collect list children too.
  CollectListChildren,
  // Collect table children too.
  CollectTableChildren,
};

class HTMLEditUtils final {
  using AbstractRange = dom::AbstractRange;
  using Element = dom::Element;
  using Selection = dom::Selection;
  using Text = dom::Text;
  using WhitespaceOption = nsTextFragment::WhitespaceOption;
  using WhitespaceOptions = nsTextFragment::WhitespaceOptions;

 public:
  static constexpr char16_t kNewLine = '\n';
  static constexpr char16_t kCarriageReturn = '\r';
  static constexpr char16_t kTab = '\t';
  static constexpr char16_t kSpace = ' ';
  static constexpr char16_t kNBSP = 0x00A0;
  static constexpr char16_t kGreaterThan = '>';

  /**
   * IsSimplyEditableNode() returns true when aNode is simply editable.
   * This does NOT means that aNode can be removed from current parent nor
   * aNode's data is editable.
   */
  static bool IsSimplyEditableNode(const nsINode& aNode) {
    return aNode.IsEditable();
  }

  /**
   * Return true if aNode is editable or not in a composed doc.  This is useful
   * if the caller may modify document fragment before inserting it into a
   * Document.
   */
  static bool NodeIsEditableOrNotInComposedDoc(const nsINode& aNode) {
    return MOZ_UNLIKELY(!aNode.IsInComposedDoc()) || aNode.IsEditable();
  }

  /**
   * Return true if aElement is an editing host which is either:
   * - the root element
   * - parent is not editable
   * - the <body> element of the document
   */
  [[nodiscard]] static bool ElementIsEditableRoot(const Element& aElement);

  /**
   * Return true if inclusive flat tree ancestor has `inert` state.
   */
  static bool ContentIsInert(const nsIContent& aContent);

  /**
   * IsNeverContentEditableElementByUser() returns true if the element's content
   * is never editable by user.  E.g., the content is always replaced by
   * native anonymous node or something.
   */
  static bool IsNeverElementContentsEditableByUser(const nsIContent& aContent) {
    return aContent.IsElement() &&
           (!HTMLEditUtils::IsContainerNode(aContent) ||
            aContent.IsAnyOfHTMLElements(
                nsGkAtoms::applet, nsGkAtoms::colgroup, nsGkAtoms::frameset,
                nsGkAtoms::head, nsGkAtoms::html, nsGkAtoms::iframe,
                nsGkAtoms::meter, nsGkAtoms::progress, nsGkAtoms::select,
                nsGkAtoms::textarea));
  }

  /**
   * IsNonEditableReplacedContent() returns true when aContent is an inclusive
   * descendant of a replaced element whose content shouldn't be editable by
   * user's operation.
   */
  static bool IsNonEditableReplacedContent(const nsIContent& aContent) {
    for (Element* element : aContent.InclusiveAncestorsOfType<Element>()) {
      if (element->IsAnyOfHTMLElements(nsGkAtoms::select, nsGkAtoms::option,
                                       nsGkAtoms::optgroup)) {
        return true;
      }
    }
    return false;
  }

  /*
   * IsRemovalNode() returns true when parent of aContent is editable even
   * if aContent isn't editable.
   * This is a valid method to check it if you find the content from point
   * of view of siblings or parents of aContent.
   * Note that padding `<br>` element for empty editor and manual native
   * anonymous content should be deletable even after `HTMLEditor` is destroyed
   * because they are owned/managed by `HTMLEditor`.
   */
  static bool IsRemovableNode(const nsIContent& aContent) {
    return EditorUtils::IsPaddingBRElementForEmptyEditor(aContent) ||
           aContent.IsRootOfNativeAnonymousSubtree() ||
           (aContent.GetParentNode() &&
            aContent.GetParentNode()->IsEditable() &&
            &aContent != aContent.OwnerDoc()->GetBody() &&
            &aContent != aContent.OwnerDoc()->GetDocumentElement());
  }

  /**
   * IsRemovableFromParentNode() returns true when aContent is editable, has a
   * parent node and the parent node is also editable.
   * This is a valid method to check it if you find the content from point
   * of view of descendants of aContent.
   * Note that padding `<br>` element for empty editor and manual native
   * anonymous content should be deletable even after `HTMLEditor` is destroyed
   * because they are owned/managed by `HTMLEditor`.
   */
  static bool IsRemovableFromParentNode(const nsIContent& aContent) {
    return EditorUtils::IsPaddingBRElementForEmptyEditor(aContent) ||
           aContent.IsRootOfNativeAnonymousSubtree() ||
           (aContent.IsEditable() && aContent.GetParentNode() &&
            aContent.GetParentNode()->IsEditable() &&
            &aContent != aContent.OwnerDoc()->GetBody() &&
            &aContent != aContent.OwnerDoc()->GetDocumentElement());
  }

  /**
   * CanContentsBeJoined() returns true if aLeftContent and aRightContent can be
   * joined.
   */
  static bool CanContentsBeJoined(const nsIContent& aLeftContent,
                                  const nsIContent& aRightContent);

  /**
   * Returns true if aContent is an element and it should be treated as a block.
   *
   * @param aBlockInlineCheck
   *  - If UseHTMLDefaultStyle, this returns true only for HTML elements which
   * are defined as a block by the default style.  I.e., non-HTML elements are
   * always treated as inline.
   *  - If UseComputedDisplayOutsideStyle, this returns true for element nodes
   * whose display-outside is not inline nor ruby.  This is useful to get
   * inclusive ancestor block element.
   *  - If UseComputedDisplayStyle, this returns true for element nodes whose
   * display-outside is not inline or whose display-inside is flow-root and they
   * do not appear as a form control.  This is useful to check whether
   * collapsible white-spaces at the element edges are visible or invisible or
   * whether <br> element at end of the element is visible or invisible.
   */
  [[nodiscard]] static bool IsBlockElement(const nsIContent& aContent,
                                           BlockInlineCheck aBlockInlineCheck);

  /**
   * This is designed to check elements or non-element nodes which are layed out
   * as inline.  Therefore, inline-block etc and ruby are treated as inline.
   * Note that invisible non-element nodes like comment nodes are also treated
   * as inline.
   *
   * @param aBlockInlineCheck   UseComputedDisplayOutsideStyle and
   *                            UseComputedDisplayStyle return same result for
   *                            any elements.
   */
  [[nodiscard]] static bool IsInlineContent(const nsIContent& aContent,
                                            BlockInlineCheck aBlockInlineCheck);

  /**
   * IsVisibleElementEvenIfLeafNode() returns true if aContent is an empty block
   * element, a visible replaced element such as a form control.  This does not
   * check the layout information.
   */
  static bool IsVisibleElementEvenIfLeafNode(const nsIContent& aContent);

  static bool IsInlineStyle(nsINode* aNode);

  /**
   * IsDisplayOutsideInline() returns true if display-outside value is
   * "inside".  This does NOT flush the layout.
   */
  [[nodiscard]] static bool IsDisplayOutsideInline(const Element& aElement);

  /**
   * IsDisplayInsideFlowRoot() returns true if display-inline value of aElement
   * is "flow-root".  This does NOT flush the layout.
   */
  [[nodiscard]] static bool IsDisplayInsideFlowRoot(const Element& aElement);

  /**
   * Return true if aElement is a flex item or a grid item.  This works only
   * when aElement has a primary frame.
   */
  [[nodiscard]] static bool IsFlexOrGridItem(const Element& aElement);

  /**
   * IsRemovableInlineStyleElement() returns true if aElement is an inline
   * element and can be removed or split to in order to modifying inline
   * styles.
   */
  static bool IsRemovableInlineStyleElement(Element& aElement);

  /**
   * Return true if aTagName is one of the format element name of
   * Document.execCommand("formatBlock").
   */
  [[nodiscard]] static bool IsFormatTagForFormatBlockCommand(
      const nsStaticAtom& aTagName) {
    return
        // clang-format off
        &aTagName == nsGkAtoms::address ||
        &aTagName == nsGkAtoms::article ||
        &aTagName == nsGkAtoms::aside ||
        &aTagName == nsGkAtoms::blockquote ||
        &aTagName == nsGkAtoms::dd ||
        &aTagName == nsGkAtoms::div ||
        &aTagName == nsGkAtoms::dl ||
        &aTagName == nsGkAtoms::dt ||
        &aTagName == nsGkAtoms::footer ||
        &aTagName == nsGkAtoms::h1 ||
        &aTagName == nsGkAtoms::h2 ||
        &aTagName == nsGkAtoms::h3 ||
        &aTagName == nsGkAtoms::h4 ||
        &aTagName == nsGkAtoms::h5 ||
        &aTagName == nsGkAtoms::h6 ||
        &aTagName == nsGkAtoms::header ||
        &aTagName == nsGkAtoms::hgroup ||
        &aTagName == nsGkAtoms::main ||
        &aTagName == nsGkAtoms::nav ||
        &aTagName == nsGkAtoms::p ||
        &aTagName == nsGkAtoms::pre ||
        &aTagName == nsGkAtoms::section;
    // clang-format on
  }

  /**
   * Return true if aContent is a format element of
   * Document.execCommand("formatBlock").
   */
  [[nodiscard]] static bool IsFormatElementForFormatBlockCommand(
      const nsIContent& aContent) {
    if (!aContent.IsHTMLElement() ||
        !aContent.NodeInfo()->NameAtom()->IsStatic()) {
      return false;
    }
    const nsStaticAtom* tagName = aContent.NodeInfo()->NameAtom()->AsStatic();
    return IsFormatTagForFormatBlockCommand(*tagName);
  }

  /**
   * Return true if aTagName is one of the format element name of
   * cmd_paragraphState.
   */
  [[nodiscard]] static bool IsFormatTagForParagraphStateCommand(
      const nsStaticAtom& aTagName) {
    return
        // clang-format off
        &aTagName == nsGkAtoms::address ||
        &aTagName == nsGkAtoms::dd ||
        &aTagName == nsGkAtoms::dl ||
        &aTagName == nsGkAtoms::dt ||
        &aTagName == nsGkAtoms::h1 ||
        &aTagName == nsGkAtoms::h2 ||
        &aTagName == nsGkAtoms::h3 ||
        &aTagName == nsGkAtoms::h4 ||
        &aTagName == nsGkAtoms::h5 ||
        &aTagName == nsGkAtoms::h6 ||
        &aTagName == nsGkAtoms::p ||
        &aTagName == nsGkAtoms::pre;
    // clang-format on
  }

  /**
   * Return true if aContent is a format element of cmd_paragraphState.
   */
  [[nodiscard]] static bool IsFormatElementForParagraphStateCommand(
      const nsIContent& aContent) {
    if (!aContent.IsHTMLElement() ||
        !aContent.NodeInfo()->NameAtom()->IsStatic()) {
      return false;
    }
    const nsStaticAtom* tagName = aContent.NodeInfo()->NameAtom()->AsStatic();
    return IsFormatTagForParagraphStateCommand(*tagName);
  }

  static bool IsNodeThatCanOutdent(nsINode* aNode);
  static bool IsHeader(nsINode& aNode);
  static bool IsListItem(const nsINode* aNode);
  static bool IsTable(const nsINode* aNode);
  static bool IsTableRow(nsINode* aNode);
  static bool IsAnyTableElement(const nsINode* aNode);
  static bool IsAnyTableElementButNotTable(nsINode* aNode);
  static bool IsTableCell(const nsINode* aNode);
  static bool IsTableCellOrCaption(nsINode& aNode);
  static bool IsAnyListElement(const nsINode* aNode);
  static bool IsPre(const nsINode* aNode);
  static bool IsImage(nsINode* aNode);
  static bool IsLink(const nsINode* aNode);
  static bool IsNamedAnchor(const nsINode* aNode);
  static bool IsMozDiv(nsINode* aNode);
  static bool IsMailCite(const Element& aElement);
  static bool IsFormWidget(const nsINode* aNode);
  static bool SupportsAlignAttr(nsINode& aNode);

  static bool CanNodeContain(const nsINode& aParent, const nsIContent& aChild) {
    switch (aParent.NodeType()) {
      case nsINode::ELEMENT_NODE:
      case nsINode::DOCUMENT_FRAGMENT_NODE:
        return HTMLEditUtils::CanNodeContain(*aParent.NodeInfo()->NameAtom(),
                                             aChild);
    }
    return false;
  }

  static bool CanNodeContain(const nsINode& aParent,
                             const nsAtom& aChildNodeName) {
    switch (aParent.NodeType()) {
      case nsINode::ELEMENT_NODE:
      case nsINode::DOCUMENT_FRAGMENT_NODE:
        return HTMLEditUtils::CanNodeContain(*aParent.NodeInfo()->NameAtom(),
                                             aChildNodeName);
    }
    return false;
  }

  static bool CanNodeContain(const nsAtom& aParentNodeName,
                             const nsIContent& aChild) {
    switch (aChild.NodeType()) {
      case nsINode::TEXT_NODE:
      case nsINode::COMMENT_NODE:
      case nsINode::CDATA_SECTION_NODE:
      case nsINode::ELEMENT_NODE:
      case nsINode::DOCUMENT_FRAGMENT_NODE:
        return HTMLEditUtils::CanNodeContain(aParentNodeName,
                                             *aChild.NodeInfo()->NameAtom());
    }
    return false;
  }

  // XXX Only this overload does not check the node type.  Therefore, only this
  //     handle Document and ProcessingInstructionTagName.
  static bool CanNodeContain(const nsAtom& aParentNodeName,
                             const nsAtom& aChildNodeName) {
    nsHTMLTag childTagEnum;
    if (&aChildNodeName == nsGkAtoms::textTagName) {
      childTagEnum = eHTMLTag_text;
    } else if (&aChildNodeName == nsGkAtoms::commentTagName ||
               &aChildNodeName == nsGkAtoms::cdataTagName) {
      childTagEnum = eHTMLTag_comment;
    } else {
      childTagEnum =
          nsHTMLTags::AtomTagToId(const_cast<nsAtom*>(&aChildNodeName));
    }

    nsHTMLTag parentTagEnum =
        nsHTMLTags::AtomTagToId(const_cast<nsAtom*>(&aParentNodeName));
    return HTMLEditUtils::CanNodeContain(parentTagEnum, childTagEnum);
  }

  /**
   * CanElementContainParagraph() returns true if aElement can have a <p>
   * element as its child or its descendant.
   */
  static bool CanElementContainParagraph(const Element& aElement) {
    if (HTMLEditUtils::CanNodeContain(aElement, *nsGkAtoms::p)) {
      return true;
    }

    // Even if the element cannot have a <p> element as a child, it can contain
    // <p> element as a descendant if it's one of the following elements.
    if (aElement.IsAnyOfHTMLElements(nsGkAtoms::ol, nsGkAtoms::ul,
                                     nsGkAtoms::dl, nsGkAtoms::table,
                                     nsGkAtoms::thead, nsGkAtoms::tbody,
                                     nsGkAtoms::tfoot, nsGkAtoms::tr)) {
      return true;
    }

    // XXX Otherwise, Chromium checks the CSS box is a block, but we don't do it
    //     for now.
    return false;
  }

  /**
   * Return a point which can insert a node whose name is aTagName scanning
   * from aPoint to its ancestor points.
   */
  template <typename EditorDOMPointType>
  static EditorDOMPoint GetInsertionPointInInclusiveAncestor(
      const nsAtom& aTagName, const EditorDOMPointType& aPoint,
      const Element* aAncestorLimit = nullptr) {
    if (MOZ_UNLIKELY(!aPoint.IsInContentNode())) {
      return EditorDOMPoint();
    }
    Element* lastChild = nullptr;
    for (Element* containerElement :
         aPoint.template ContainerAs<nsIContent>()
             ->template InclusiveAncestorsOfType<Element>()) {
      if (!HTMLEditUtils::IsSimplyEditableNode(*containerElement)) {
        return EditorDOMPoint();
      }
      if (HTMLEditUtils::CanNodeContain(*containerElement, aTagName)) {
        return lastChild ? EditorDOMPoint(lastChild)
                         : aPoint.template To<EditorDOMPoint>();
      }
      if (containerElement == aAncestorLimit) {
        return EditorDOMPoint();
      }
      lastChild = containerElement;
    }
    return EditorDOMPoint();
  }

  /**
   * IsContainerNode() returns true if aContent is a container node.
   */
  static bool IsContainerNode(const nsIContent& aContent) {
    nsHTMLTag tagEnum;
    // XXX Should this handle #cdata-section too?
    if (aContent.IsText()) {
      tagEnum = eHTMLTag_text;
    } else {
      // XXX Why don't we use nsHTMLTags::AtomTagToId?  Are there some
      //     difference?
      tagEnum = nsHTMLTags::StringTagToId(aContent.NodeName());
    }
    return HTMLEditUtils::IsContainerNode(tagEnum);
  }

  /**
   * IsSplittableNode() returns true if aContent can split.
   */
  static bool IsSplittableNode(const nsIContent& aContent) {
    if (!EditorUtils::IsEditableContent(aContent,
                                        EditorUtils::EditorType::HTML) ||
        !HTMLEditUtils::IsRemovableFromParentNode(aContent)) {
      return false;
    }
    if (aContent.IsElement()) {
      // XXX Perhaps, instead of using container, we should have "splittable"
      //     information in the DB.  E.g., `<template>`, `<script>` elements
      //     can have children, but shouldn't be split.
      return HTMLEditUtils::IsContainerNode(aContent) &&
             !aContent.IsAnyOfHTMLElements(nsGkAtoms::body, nsGkAtoms::button,
                                           nsGkAtoms::caption, nsGkAtoms::table,
                                           nsGkAtoms::tbody, nsGkAtoms::tfoot,
                                           nsGkAtoms::thead, nsGkAtoms::tr) &&
             !HTMLEditUtils::IsNeverElementContentsEditableByUser(aContent) &&
             !HTMLEditUtils::IsNonEditableReplacedContent(aContent);
    }
    return aContent.IsText() && aContent.Length() > 0;
  }

  /**
   * See execCommand spec:
   * https://w3c.github.io/editing/execCommand.html#non-list-single-line-container
   * https://w3c.github.io/editing/execCommand.html#single-line-container
   */
  static bool IsNonListSingleLineContainer(const nsINode& aNode);
  static bool IsSingleLineContainer(const nsINode& aNode);

  /**
   * Return true if aText has only a linefeed and it's preformatted.
   */
  [[nodiscard]] static bool TextHasOnlyOnePreformattedLinefeed(
      const Text& aText) {
    return aText.TextDataLength() == 1u &&
           aText.TextFragment().CharAt(0u) == kNewLine &&
           EditorUtils::IsNewLinePreformatted(aText);
  }

  /**
   * IsVisibleTextNode() returns true if aText has visible text.  If it has
   * only white-spaces and they are collapsed, returns false.
   */
  [[nodiscard]] static bool IsVisibleTextNode(const Text& aText);

  /**
   * IsInVisibleTextFrames() returns true if any text in aText is in visible
   * text frames.  Callers have to guarantee that there is no pending reflow.
   */
  static bool IsInVisibleTextFrames(nsPresContext* aPresContext,
                                    const Text& aText);

  /**
   * IsVisibleBRElement() and IsInvisibleBRElement() return true if aContent is
   * a visible HTML <br> element, i.e., not a padding <br> element for making
   * last line in a block element visible, or an invisible <br> element.
   */
  static bool IsVisibleBRElement(const nsIContent& aContent) {
    if (const dom::HTMLBRElement* brElement =
            dom::HTMLBRElement::FromNode(&aContent)) {
      return IsVisibleBRElement(*brElement);
    }
    return false;
  }
  static bool IsVisibleBRElement(const dom::HTMLBRElement& aBRElement) {
    // If followed by a block boundary without visible content, it's invisible
    // <br> element.
    return !HTMLEditUtils::GetElementOfImmediateBlockBoundary(
        aBRElement, WalkTreeDirection::Forward);
  }
  static bool IsInvisibleBRElement(const nsIContent& aContent) {
    if (const dom::HTMLBRElement* brElement =
            dom::HTMLBRElement::FromNode(&aContent)) {
      return IsInvisibleBRElement(*brElement);
    }
    return false;
  }
  static bool IsInvisibleBRElement(const dom::HTMLBRElement& aBRElement) {
    return !HTMLEditUtils::IsVisibleBRElement(aBRElement);
  }

  enum class IgnoreInvisibleLineBreak { No, Yes };

  /**
   * Return true if aPoint is immediately before current block boundary.  If
   * aIgnoreInvisibleLineBreak is "Yes", this returns true if aPoint is before
   * invisible line break before a block boundary.
   */
  template <typename PT, typename CT>
  [[nodiscard]] static bool PointIsImmediatelyBeforeCurrentBlockBoundary(
      const EditorDOMPointBase<PT, CT>& aPoint,
      IgnoreInvisibleLineBreak aIgnoreInvisibleLineBreak);

  /**
   * Return true if aRange crosses the inclusive ancestor block element at
   * start boundary, in other words, if aRange ends outside of the inclusive
   * ancestor block of the start boundary.
   */
  template <typename EditorDOMPointType>
  [[nodiscard]] static bool RangeIsAcrossStartBlockBoundary(
      const EditorDOMRangeBase<EditorDOMPointType>& aRange) {
    MOZ_ASSERT(aRange.IsPositionedAndValid());
    if (MOZ_UNLIKELY(!aRange.StartRef().IsInContentNode())) {
      return false;
    }
    const Element* const startBlockElement =
        HTMLEditUtils::GetInclusiveAncestorElement(
            *aRange.StartRef().template ContainerAs<nsIContent>(),
            ClosestBlockElement,
            BlockInlineCheck::UseComputedDisplayOutsideStyle);
    if (MOZ_UNLIKELY(!startBlockElement)) {
      return false;
    }
    return EditorRawDOMPoint::After(*startBlockElement)
        .EqualsOrIsBefore(aRange.EndRef());
  }

  /**
   * Return true if `display` of inclusive ancestor of aContent is `none`.
   */
  static bool IsInclusiveAncestorCSSDisplayNone(const nsIContent& aContent);

  /**
   * IsVisiblePreformattedNewLine() and IsInvisiblePreformattedNewLine() return
   * true if the point is preformatted linefeed and it's visible or invisible.
   * If linefeed is immediately before a block boundary, it's invisible.
   *
   * @param aFollowingBlockElement  [out] If the node is followed by a block
   *                                boundary, this is set to the element
   *                                creating the block boundary.
   */
  template <typename EditorDOMPointType>
  [[nodiscard]] static bool IsVisiblePreformattedNewLine(
      const EditorDOMPointType& aPoint,
      Element** aFollowingBlockElement = nullptr) {
    if (aFollowingBlockElement) {
      *aFollowingBlockElement = nullptr;
    }
    if (!aPoint.IsInTextNode() || aPoint.IsEndOfContainer() ||
        !aPoint.IsCharPreformattedNewLine()) {
      return false;
    }
    // If there are some other characters in the text node, it's a visible
    // linefeed.
    if (!aPoint.IsAtLastContent()) {
      if (EditorUtils::IsWhiteSpacePreformatted(
              *aPoint.template ContainerAs<Text>())) {
        return true;
      }
      const nsTextFragment& textFragment =
          aPoint.template ContainerAs<Text>()->TextFragment();
      const uint32_t nextVisibleCharOffset = textFragment.FindNonWhitespaceChar(
          EditorUtils::IsNewLinePreformatted(
              *aPoint.template ContainerAs<Text>())
              ? WhitespaceOptions{WhitespaceOption::FormFeedIsSignificant,
                                  WhitespaceOption::NewLineIsSignificant}
              : WhitespaceOptions{WhitespaceOption::FormFeedIsSignificant},
          aPoint.Offset() + 1);
      if (nextVisibleCharOffset != nsTextFragment::kNotFound) {
        return true;  // There is a visible character after the point.
      }
    }
    // If followed by a block boundary without visible content, it's invisible
    // linefeed.
    Element* followingBlockElement =
        HTMLEditUtils::GetElementOfImmediateBlockBoundary(
            *aPoint.template ContainerAs<Text>(), WalkTreeDirection::Forward);
    if (aFollowingBlockElement) {
      *aFollowingBlockElement = followingBlockElement;
    }
    return !followingBlockElement;
  }
  template <typename EditorDOMPointType>
  static bool IsInvisiblePreformattedNewLine(
      const EditorDOMPointType& aPoint,
      Element** aFollowingBlockElement = nullptr) {
    if (!aPoint.IsInTextNode() || aPoint.IsEndOfContainer() ||
        !aPoint.IsCharPreformattedNewLine()) {
      if (aFollowingBlockElement) {
        *aFollowingBlockElement = nullptr;
      }
      return false;
    }
    return !IsVisiblePreformattedNewLine(aPoint, aFollowingBlockElement);
  }

  /**
   * Return a point to insert a padding line break if aPoint is following a
   * block boundary and the line containing aPoint requires a following padding
   * line break to make the line visible.
   */
  template <typename PT, typename CT>
  static EditorDOMPoint LineRequiresPaddingLineBreakToBeVisible(
      const EditorDOMPointBase<PT, CT>& aPoint, const Element& aEditingHost);

  /**
   * ShouldInsertLinefeedCharacter() returns true if the caller should insert
   * a linefeed character instead of <br> element.
   */
  static bool ShouldInsertLinefeedCharacter(
      const EditorDOMPoint& aPointToInsert, const Element& aEditingHost);

  /**
   * IsEmptyNode() returns false if aNode has some visible content nodes,
   * list elements or table elements.
   *
   * @param aPresContext    Must not be nullptr if
   *                        EmptyCheckOption::SafeToAskLayout is set.
   * @param aNode           The node to check whether it's empty.
   * @param aOptions        You can specify which type of elements are visible
   *                        and/or whether this can access layout information.
   * @param aSeenBR         [Out] Set to true if this meets an <br> element
   *                        before meeting visible things.
   */
  enum class EmptyCheckOption {
    TreatSingleBRElementAsVisible,
    TreatBlockAsVisible,
    TreatListItemAsVisible,
    TreatTableCellAsVisible,
    TreatNonEditableContentAsInvisible,
    SafeToAskLayout,
  };
  using EmptyCheckOptions = EnumSet<EmptyCheckOption, uint32_t>;
  static bool IsEmptyNode(nsPresContext* aPresContext, const nsINode& aNode,
                          const EmptyCheckOptions& aOptions = {},
                          bool* aSeenBR = nullptr);
  static bool IsEmptyNode(const nsINode& aNode,
                          const EmptyCheckOptions& aOptions = {},
                          bool* aSeenBR = nullptr) {
    MOZ_ASSERT(!aOptions.contains(EmptyCheckOption::SafeToAskLayout));
    return IsEmptyNode(nullptr, aNode, aOptions, aSeenBR);
  }

  /**
   * IsEmptyInlineContainer() returns true if aContent is an inline element
   * which can have children and does not have meaningful content.
   */
  static bool IsEmptyInlineContainer(const nsIContent& aContent,
                                     const EmptyCheckOptions& aOptions,
                                     BlockInlineCheck aBlockInlineCheck) {
    return HTMLEditUtils::IsInlineContent(aContent, aBlockInlineCheck) &&
           HTMLEditUtils::IsContainerNode(aContent) &&
           HTMLEditUtils::IsEmptyNode(aContent, aOptions);
  }

  /**
   * IsEmptyBlockElement() returns true if aElement is a block level element
   * and it doesn't have any visible content.
   */
  static bool IsEmptyBlockElement(const Element& aElement,
                                  const EmptyCheckOptions& aOptions,
                                  BlockInlineCheck aBlockInlineCheck) {
    return HTMLEditUtils::IsBlockElement(aElement, aBlockInlineCheck) &&
           HTMLEditUtils::IsEmptyNode(aElement, aOptions);
  }

  /**
   * Return true if aListElement is completely empty or it has only one list
   * item element which is empty.
   */
  [[nodiscard]] static bool IsEmptyAnyListElement(const Element& aListElement) {
    MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(&aListElement));
    bool foundListItem = false;
    for (nsIContent* child = aListElement.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      if (HTMLEditUtils::IsListItem(child)) {
        if (foundListItem) {
          return false;  // 2 list items found.
        }
        if (!IsEmptyNode(*child, {})) {
          return false;  // found non-empty list item.
        }
        foundListItem = true;
        continue;
      }
      if (child->IsElement()) {
        return false;  // found sublist or illegal child.
      }
      if (child->IsText() &&
          HTMLEditUtils::IsVisibleTextNode(*child->AsText())) {
        return false;  // found illegal visible text node.
      }
    }
    return true;
  }

  /**
   * Return true if aListElement does not have invalid child.
   */
  enum class TreatSubListElementAs { Invalid, Valid };
  [[nodiscard]] static bool IsValidListElement(
      const Element& aListElement,
      TreatSubListElementAs aTreatSubListElementAs) {
    MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(&aListElement));
    for (nsIContent* child = aListElement.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      if (HTMLEditUtils::IsAnyListElement(child)) {
        if (aTreatSubListElementAs == TreatSubListElementAs::Invalid) {
          return false;
        }
        continue;
      }
      if (child->IsHTMLElement(nsGkAtoms::li)) {
        if (MOZ_UNLIKELY(!aListElement.IsAnyOfHTMLElements(nsGkAtoms::ol,
                                                           nsGkAtoms::ul))) {
          return false;
        }
        continue;
      }
      if (child->IsAnyOfHTMLElements(nsGkAtoms::dt, nsGkAtoms::dd)) {
        if (MOZ_UNLIKELY(!aListElement.IsAnyOfHTMLElements(nsGkAtoms::dl))) {
          return false;
        }
        continue;
      }
      if (MOZ_UNLIKELY(child->IsElement())) {
        return false;
      }
      if (MOZ_LIKELY(child->IsText())) {
        if (MOZ_UNLIKELY(HTMLEditUtils::IsVisibleTextNode(*child->AsText()))) {
          return false;
        }
      }
    }
    return true;
  }

  /**
   * IsEmptyOneHardLine() returns true if aArrayOfContents does not represent
   * 2 or more lines and have meaningful content.
   */
  static bool IsEmptyOneHardLine(
      nsTArray<OwningNonNull<nsIContent>>& aArrayOfContents,
      BlockInlineCheck aBlockInlineCheck) {
    if (NS_WARN_IF(aArrayOfContents.IsEmpty())) {
      return true;
    }

    bool brElementHasFound = false;
    for (OwningNonNull<nsIContent>& content : aArrayOfContents) {
      if (!EditorUtils::IsEditableContent(content,
                                          EditorUtils::EditorType::HTML)) {
        continue;
      }
      if (content->IsHTMLElement(nsGkAtoms::br)) {
        // If there are 2 or more `<br>` elements, it's not empty line since
        // there may be only one `<br>` element in a hard line.
        if (brElementHasFound) {
          return false;
        }
        brElementHasFound = true;
        continue;
      }
      if (!HTMLEditUtils::IsEmptyInlineContainer(
              content,
              {EmptyCheckOption::TreatSingleBRElementAsVisible,
               EmptyCheckOption::TreatNonEditableContentAsInvisible},
              aBlockInlineCheck)) {
        return false;
      }
    }
    return true;
  }

  /**
   * IsPointAtEdgeOfLink() returns true if aPoint is at start or end of a
   * link.
   */
  template <typename PT, typename CT>
  static bool IsPointAtEdgeOfLink(const EditorDOMPointBase<PT, CT>& aPoint,
                                  Element** aFoundLinkElement = nullptr) {
    if (aFoundLinkElement) {
      *aFoundLinkElement = nullptr;
    }
    if (!aPoint.IsInContentNode()) {
      return false;
    }
    if (!aPoint.IsStartOfContainer() && !aPoint.IsEndOfContainer()) {
      return false;
    }
    // XXX Assuming it's not in an empty text node because it's unrealistic edge
    //     case.
    bool maybeStartOfAnchor = aPoint.IsStartOfContainer();
    for (EditorRawDOMPoint point(aPoint.template ContainerAs<nsIContent>());
         point.IsSet() && (maybeStartOfAnchor ? point.IsStartOfContainer()
                                              : point.IsAtLastContent());
         point = point.ParentPoint()) {
      if (HTMLEditUtils::IsLink(point.GetContainer())) {
        // Now, we're at start or end of <a href>.
        if (aFoundLinkElement) {
          *aFoundLinkElement =
              do_AddRef(point.template ContainerAs<Element>()).take();
        }
        return true;
      }
    }
    return false;
  }

  /**
   * IsContentInclusiveDescendantOfLink() returns true if aContent is a
   * descendant of a link element.
   * Note that this returns true even if editing host of aContent is in a link
   * element.
   */
  static bool IsContentInclusiveDescendantOfLink(
      nsIContent& aContent, Element** aFoundLinkElement = nullptr) {
    if (aFoundLinkElement) {
      *aFoundLinkElement = nullptr;
    }
    for (Element* element : aContent.InclusiveAncestorsOfType<Element>()) {
      if (HTMLEditUtils::IsLink(element)) {
        if (aFoundLinkElement) {
          *aFoundLinkElement = do_AddRef(element).take();
        }
        return true;
      }
    }
    return false;
  }

  /**
   * IsRangeEntirelyInLink() returns true if aRange is entirely in a link
   * element.
   * Note that this returns true even if editing host of the range is in a link
   * element.
   */
  template <typename EditorDOMRangeType>
  static bool IsRangeEntirelyInLink(const EditorDOMRangeType& aRange,
                                    Element** aFoundLinkElement = nullptr) {
    MOZ_ASSERT(aRange.IsPositionedAndValid());
    if (aFoundLinkElement) {
      *aFoundLinkElement = nullptr;
    }
    nsINode* commonAncestorNode =
        nsContentUtils::GetClosestCommonInclusiveAncestor(
            aRange.StartRef().GetContainer(), aRange.EndRef().GetContainer());
    if (NS_WARN_IF(!commonAncestorNode) || !commonAncestorNode->IsContent()) {
      return false;
    }
    return IsContentInclusiveDescendantOfLink(*commonAncestorNode->AsContent(),
                                              aFoundLinkElement);
  }

  /**
   * Get adjacent content node of aNode if there is (even if one is in different
   * parent element).
   *
   * @param aNode               The node from which we start to walk the DOM
   *                            tree.
   * @param aOptions            See WalkTreeOption for the detail.
   * @param aBlockInlineCheck   Whether considering block vs. inline with the
   *                            computed style or the HTML default style.
   * @param aAncestorLimiter    Ancestor limiter element which these methods
   *                            never cross its boundary.  This is typically
   *                            the editing host.
   */
  enum class WalkTreeOption {
    IgnoreNonEditableNode,     // Ignore non-editable nodes and their children.
    IgnoreDataNodeExceptText,  // Ignore data nodes which are not text node.
    IgnoreWhiteSpaceOnlyText,  // Ignore text nodes having only white-spaces.
    StopAtBlockBoundary,       // Stop waking the tree at a block boundary.
  };
  using WalkTreeOptions = EnumSet<WalkTreeOption>;
  static nsIContent* GetPreviousContent(
      const nsINode& aNode, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr) {
    if (&aNode == aAncestorLimiter ||
        (aAncestorLimiter &&
         !aNode.IsInclusiveDescendantOf(aAncestorLimiter))) {
      return nullptr;
    }
    return HTMLEditUtils::GetAdjacentContent(aNode, WalkTreeDirection::Backward,
                                             aOptions, aBlockInlineCheck,
                                             aAncestorLimiter);
  }
  static nsIContent* GetNextContent(const nsINode& aNode,
                                    const WalkTreeOptions& aOptions,
                                    BlockInlineCheck aBlockInlineCheck,
                                    const Element* aAncestorLimiter = nullptr) {
    if (&aNode == aAncestorLimiter ||
        (aAncestorLimiter &&
         !aNode.IsInclusiveDescendantOf(aAncestorLimiter))) {
      return nullptr;
    }
    return HTMLEditUtils::GetAdjacentContent(aNode, WalkTreeDirection::Forward,
                                             aOptions, aBlockInlineCheck,
                                             aAncestorLimiter);
  }

  /**
   * And another version that takes a point in DOM tree rather than a node.
   */
  template <typename PT, typename CT>
  static nsIContent* GetPreviousContent(
      const EditorDOMPointBase<PT, CT>& aPoint, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr);

  /**
   * And another version that takes a point in DOM tree rather than a node.
   *
   * Note that this may return the child at the offset.  E.g., following code
   * causes infinite loop.
   *
   * EditorRawDOMPoint point(aEditableNode);
   * while (nsIContent* content =
   *          GetNextContent(point, {WalkTreeOption::IgnoreNonEditableNode})) {
   *   // Do something...
   *   point.Set(content);
   * }
   *
   * Following code must be you expected:
   *
   * while (nsIContent* content =
   *          GetNextContent(point, {WalkTreeOption::IgnoreNonEditableNode}) {
   *   // Do something...
   *   DebugOnly<bool> advanced = point.Advanced();
   *   MOZ_ASSERT(advanced);
   *   point.Set(point.GetChild());
   * }
   */
  template <typename PT, typename CT>
  static nsIContent* GetNextContent(const EditorDOMPointBase<PT, CT>& aPoint,
                                    const WalkTreeOptions& aOptions,
                                    BlockInlineCheck aBlockInlineCheck,
                                    const Element* aAncestorLimiter = nullptr);

  /**
   * GetPreviousSibling() return the preceding sibling of aContent which matches
   * with aOption.
   *
   * @param aBlockInlineCheck   Can be Unused if aOptions does not contain
   *                            StopAtBlockBoundary.
   */
  static nsIContent* GetPreviousSibling(
      const nsIContent& aContent, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused) {
    for (nsIContent* sibling = aContent.GetPreviousSibling(); sibling;
         sibling = sibling->GetPreviousSibling()) {
      if (HTMLEditUtils::IsContentIgnored(*sibling, aOptions)) {
        continue;
      }
      if (aOptions.contains(WalkTreeOption::StopAtBlockBoundary) &&
          HTMLEditUtils::IsBlockElement(*sibling, aBlockInlineCheck)) {
        return nullptr;
      }
      return sibling;
    }
    return nullptr;
  }

  /**
   * GetNextSibling() return the following sibling of aContent which matches
   * with aOption.
   *
   * @param aBlockInlineCheck   Can be Unused if aOptions does not contain
   *                            StopAtBlockBoundary.
   */
  static nsIContent* GetNextSibling(
      const nsIContent& aContent, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused) {
    for (nsIContent* sibling = aContent.GetNextSibling(); sibling;
         sibling = sibling->GetNextSibling()) {
      if (HTMLEditUtils::IsContentIgnored(*sibling, aOptions)) {
        continue;
      }
      if (aOptions.contains(WalkTreeOption::StopAtBlockBoundary) &&
          HTMLEditUtils::IsBlockElement(*sibling, aBlockInlineCheck)) {
        return nullptr;
      }
      return sibling;
    }
    return nullptr;
  }

  /**
   * Return the last child of aNode which matches with aOption.
   *
   * @param aBlockInlineCheck   Can be unused if aOptions does not contain
   *                            StopAtBlockBoundary.
   */
  static nsIContent* GetLastChild(
      const nsINode& aNode, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused) {
    for (nsIContent* child = aNode.GetLastChild(); child;
         child = child->GetPreviousSibling()) {
      if (HTMLEditUtils::IsContentIgnored(*child, aOptions)) {
        continue;
      }
      if (aOptions.contains(WalkTreeOption::StopAtBlockBoundary) &&
          HTMLEditUtils::IsBlockElement(*child, aBlockInlineCheck)) {
        return nullptr;
      }
      return child;
    }
    return nullptr;
  }

  /**
   * Return the first child of aNode which matches with aOption.
   *
   * @param aBlockInlineCheck   Can be unused if aOptions does not contain
   *                            StopAtBlockBoundary.
   */
  static nsIContent* GetFirstChild(
      const nsINode& aNode, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused) {
    for (nsIContent* child = aNode.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      if (HTMLEditUtils::IsContentIgnored(*child, aOptions)) {
        continue;
      }
      if (aOptions.contains(WalkTreeOption::StopAtBlockBoundary) &&
          HTMLEditUtils::IsBlockElement(*child, aBlockInlineCheck)) {
        return nullptr;
      }
      return child;
    }
    return nullptr;
  }

  /**
   * Return true if aContent is the last child of aNode with ignoring all
   * children which do not match with aOption.
   *
   * @param aBlockInlineCheck   Can be unused if aOptions does not contain
   *                            StopAtBlockBoundary.
   */
  static bool IsLastChild(
      const nsIContent& aContent, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused) {
    nsINode* parentNode = aContent.GetParentNode();
    if (!parentNode) {
      return false;
    }
    return HTMLEditUtils::GetLastChild(*parentNode, aOptions,
                                       aBlockInlineCheck) == &aContent;
  }

  /**
   * Return true if aContent is the first child of aNode with ignoring all
   * children which do not match with aOption.
   *
   * @param aBlockInlineCheck   Can be unused if aOptions does not contain
   *                            StopAtBlockBoundary.
   */
  static bool IsFirstChild(
      const nsIContent& aContent, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused) {
    nsINode* parentNode = aContent.GetParentNode();
    if (!parentNode) {
      return false;
    }
    return HTMLEditUtils::GetFirstChild(*parentNode, aOptions,
                                        aBlockInlineCheck) == &aContent;
  }

  /**
   * GetAdjacentContentToPutCaret() walks the DOM tree to find an editable node
   * near aPoint where may be a good point to put caret and keep typing or
   * deleting.
   *
   * @param aPoint      The DOM point where to start to search from.
   * @return            If found, returns non-nullptr.  Otherwise, nullptr.
   *                    Note that if found node is in different table structure
   *                    element, this returns nullptr.
   */
  enum class WalkTreeDirection { Forward, Backward };
  template <typename PT, typename CT>
  static nsIContent* GetAdjacentContentToPutCaret(
      const EditorDOMPointBase<PT, CT>& aPoint,
      WalkTreeDirection aWalkTreeDirection, const Element& aEditingHost) {
    MOZ_ASSERT(aPoint.IsSetAndValid());

    nsIContent* editableContent = nullptr;
    if (aWalkTreeDirection == WalkTreeDirection::Backward) {
      editableContent = HTMLEditUtils::GetPreviousContent(
          aPoint, {WalkTreeOption::IgnoreNonEditableNode},
          BlockInlineCheck::UseComputedDisplayStyle, &aEditingHost);
      if (!editableContent) {
        return nullptr;  // Not illegal.
      }
    } else {
      editableContent = HTMLEditUtils::GetNextContent(
          aPoint, {WalkTreeOption::IgnoreNonEditableNode},
          BlockInlineCheck::UseComputedDisplayStyle, &aEditingHost);
      if (NS_WARN_IF(!editableContent)) {
        // Perhaps, illegal because the node pointed by aPoint isn't editable
        // and nobody of previous nodes is editable.
        return nullptr;
      }
    }

    // scan in the right direction until we find an eligible text node,
    // but don't cross any breaks, images, or table elements.
    // XXX This comment sounds odd.  editableContent may have already crossed
    //     breaks and/or images if they are non-editable.
    while (editableContent && !editableContent->IsText() &&
           !editableContent->IsHTMLElement(nsGkAtoms::br) &&
           !HTMLEditUtils::IsImage(editableContent)) {
      if (aWalkTreeDirection == WalkTreeDirection::Backward) {
        editableContent = HTMLEditUtils::GetPreviousContent(
            *editableContent, {WalkTreeOption::IgnoreNonEditableNode},
            BlockInlineCheck::UseComputedDisplayStyle, &aEditingHost);
        if (NS_WARN_IF(!editableContent)) {
          return nullptr;
        }
      } else {
        editableContent = HTMLEditUtils::GetNextContent(
            *editableContent, {WalkTreeOption::IgnoreNonEditableNode},
            BlockInlineCheck::UseComputedDisplayStyle, &aEditingHost);
        if (NS_WARN_IF(!editableContent)) {
          return nullptr;
        }
      }
    }

    // don't cross any table elements
    if ((!aPoint.IsInContentNode() &&
         !!HTMLEditUtils::GetInclusiveAncestorAnyTableElement(
             *editableContent)) ||
        (HTMLEditUtils::GetInclusiveAncestorAnyTableElement(*editableContent) !=
         HTMLEditUtils::GetInclusiveAncestorAnyTableElement(
             *aPoint.template ContainerAs<nsIContent>()))) {
      return nullptr;
    }

    // otherwise, ok, we have found a good spot to put the selection
    return editableContent;
  }

  enum class LeafNodeType {
    // Even if there is a child block, keep scanning a leaf content in it.
    OnlyLeafNode,
    // If there is a child block, return it too.  Note that this does not
    // mean that block siblings are not treated as leaf nodes.
    LeafNodeOrChildBlock,
    // If there is a non-editable element if and only if scanning from editable
    // node, return it too.
    LeafNodeOrNonEditableNode,
    // Ignore non-editable content at walking the tree.
    OnlyEditableLeafNode,
  };
  using LeafNodeTypes = EnumSet<LeafNodeType>;

  /**
   * GetLastLeafContent() returns rightmost leaf content in aNode.  It depends
   * on aLeafNodeTypes whether this which types of nodes are treated as leaf
   * nodes.
   *
   * @param aBlockInlineCheck   Can be Unused if aLeafNodeTypes does not contain
   *                            LeafNodeOrCHildBlock.
   */
  static nsIContent* GetLastLeafContent(
      const nsINode& aNode, const LeafNodeTypes& aLeafNodeTypes,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused,
      const Element* aAncestorLimiter = nullptr) {
    MOZ_ASSERT_IF(
        aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
        !aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode));
    // editor shouldn't touch child nodes which are replaced with native
    // anonymous nodes.
    if (aNode.IsElement() &&
        HTMLEditUtils::IsNeverElementContentsEditableByUser(
            *aNode.AsElement())) {
      return nullptr;
    }
    for (nsIContent* content = aNode.GetLastChild(); content;) {
      if (aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode) &&
          !EditorUtils::IsEditableContent(*content,
                                          EditorUtils::EditorType::HTML)) {
        content = HTMLEditUtils::GetPreviousContent(
            *content, {WalkTreeOption::IgnoreNonEditableNode},
            aBlockInlineCheck, aAncestorLimiter);
        continue;
      }
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrChildBlock) &&
          HTMLEditUtils::IsBlockElement(*content, aBlockInlineCheck)) {
        return content;
      }
      if (!content->HasChildren() ||
          HTMLEditUtils::IsNeverElementContentsEditableByUser(*content)) {
        return content;
      }
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
          !HTMLEditUtils::IsSimplyEditableNode(*content)) {
        return content;
      }
      content = content->GetLastChild();
    }
    return nullptr;
  }

  /**
   * GetFirstLeafContent() returns leftmost leaf content in aNode.  It depends
   * on aLeafNodeTypes whether this scans into a block child or treat block as a
   * leaf.
   *
   * @param aBlockInlineCheck   Can be Unused if aLeafNodeTypes does not contain
   *                            LeafNodeOrCHildBlock.
   */
  static nsIContent* GetFirstLeafContent(
      const nsINode& aNode, const LeafNodeTypes& aLeafNodeTypes,
      BlockInlineCheck aBlockInlineCheck = BlockInlineCheck::Unused,
      const Element* aAncestorLimiter = nullptr) {
    MOZ_ASSERT_IF(
        aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
        !aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode));
    // editor shouldn't touch child nodes which are replaced with native
    // anonymous nodes.
    if (aNode.IsElement() &&
        HTMLEditUtils::IsNeverElementContentsEditableByUser(
            *aNode.AsElement())) {
      return nullptr;
    }
    for (nsIContent* content = aNode.GetFirstChild(); content;) {
      if (aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode) &&
          !EditorUtils::IsEditableContent(*content,
                                          EditorUtils::EditorType::HTML)) {
        content = HTMLEditUtils::GetNextContent(
            *content, {WalkTreeOption::IgnoreNonEditableNode},
            aBlockInlineCheck, aAncestorLimiter);
        continue;
      }
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrChildBlock) &&
          HTMLEditUtils::IsBlockElement(*content, aBlockInlineCheck)) {
        return content;
      }
      if (!content->HasChildren() ||
          HTMLEditUtils::IsNeverElementContentsEditableByUser(*content)) {
        return content;
      }
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
          !HTMLEditUtils::IsSimplyEditableNode(*content)) {
        return content;
      }
      content = content->GetFirstChild();
    }
    return nullptr;
  }

  /**
   * GetNextLeafContentOrNextBlockElement() returns next leaf content or
   * next block element of aStartContent inside aAncestorLimiter.
   *
   * @param aStartContent       The start content to scan next content.
   * @param aLeafNodeTypes      See LeafNodeType.
   * @param aAncestorLimiter    Optional, if you set this, it must be an
   *                            inclusive ancestor of aStartContent.
   */
  static nsIContent* GetNextLeafContentOrNextBlockElement(
      const nsIContent& aStartContent, const LeafNodeTypes& aLeafNodeTypes,
      BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr) {
    MOZ_ASSERT_IF(
        aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
        !aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode));

    if (&aStartContent == aAncestorLimiter) {
      return nullptr;
    }

    nsIContent* nextContent = aStartContent.GetNextSibling();
    if (!nextContent) {
      if (!aStartContent.GetParentElement()) {
        NS_WARNING("Reached orphan node while climbing up the DOM tree");
        return nullptr;
      }
      for (Element* parentElement : aStartContent.AncestorsOfType<Element>()) {
        if (parentElement == aAncestorLimiter ||
            HTMLEditUtils::IsBlockElement(*parentElement, aBlockInlineCheck)) {
          return nullptr;
        }
        if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
            !parentElement->IsEditable()) {
          return nullptr;
        }
        nextContent = parentElement->GetNextSibling();
        if (nextContent) {
          break;
        }
        if (!parentElement->GetParentElement()) {
          NS_WARNING("Reached orphan node while climbing up the DOM tree");
          return nullptr;
        }
      }
      MOZ_ASSERT(nextContent);
      aBlockInlineCheck = IgnoreInsideBlockBoundary(aBlockInlineCheck);
    }

    // We have a next content.  If it's a block, return it.
    if (HTMLEditUtils::IsBlockElement(*nextContent, aBlockInlineCheck)) {
      return nextContent;
    }
    if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
        !nextContent->IsEditable()) {
      return nextContent;
    }
    if (HTMLEditUtils::IsContainerNode(*nextContent)) {
      // Else if it's a container, get deep leftmost child
      if (nsIContent* child = HTMLEditUtils::GetFirstLeafContent(
              *nextContent, aLeafNodeTypes, aBlockInlineCheck)) {
        return child;
      }
    }
    // Else return the next content itself.
    return nextContent;
  }

  /**
   * Similar to the above method, but take a DOM point to specify scan start
   * point.
   */
  template <typename PT, typename CT>
  static nsIContent* GetNextLeafContentOrNextBlockElement(
      const EditorDOMPointBase<PT, CT>& aStartPoint,
      const LeafNodeTypes& aLeafNodeTypes, BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr) {
    MOZ_ASSERT(aStartPoint.IsSet());
    MOZ_ASSERT_IF(
        aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
        !aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode));
    NS_ASSERTION(!aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
                 "Not implemented yet");

    if (!aStartPoint.IsInContentNode()) {
      return nullptr;
    }
    if (aStartPoint.IsInTextNode()) {
      return HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
          *aStartPoint.template ContainerAs<Text>(), aLeafNodeTypes,
          aBlockInlineCheck, aAncestorLimiter);
    }
    if (!HTMLEditUtils::IsContainerNode(
            *aStartPoint.template ContainerAs<nsIContent>())) {
      return HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
          *aStartPoint.template ContainerAs<nsIContent>(), aLeafNodeTypes,
          aBlockInlineCheck, aAncestorLimiter);
    }

    nsCOMPtr<nsIContent> nextContent = aStartPoint.GetChild();
    if (!nextContent) {
      if (aStartPoint.GetContainer() == aAncestorLimiter ||
          HTMLEditUtils::IsBlockElement(
              *aStartPoint.template ContainerAs<nsIContent>(),
              aBlockInlineCheck)) {
        // We are at end of the block.
        return nullptr;
      }

      // We are at end of non-block container
      return HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
          *aStartPoint.template ContainerAs<nsIContent>(), aLeafNodeTypes,
          IgnoreInsideBlockBoundary(aBlockInlineCheck), aAncestorLimiter);
    }

    // We have a next node.  If it's a block, return it.
    if (HTMLEditUtils::IsBlockElement(*nextContent, aBlockInlineCheck)) {
      return nextContent;
    }
    if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
        !HTMLEditUtils::IsSimplyEditableNode(*nextContent)) {
      return nextContent;
    }
    if (HTMLEditUtils::IsContainerNode(*nextContent)) {
      // else if it's a container, get deep leftmost child
      if (nsIContent* child = HTMLEditUtils::GetFirstLeafContent(
              *nextContent, aLeafNodeTypes,
              IgnoreInsideBlockBoundary(aBlockInlineCheck))) {
        return child;
      }
    }
    // Else return the node itself
    return nextContent;
  }

  /**
   * GetPreviousLeafContentOrPreviousBlockElement() returns previous leaf
   * content or previous block element of aStartContent inside
   * aAncestorLimiter.
   *
   * @param aStartContent       The start content to scan previous content.
   * @param aLeafNodeTypes      See LeafNodeType.
   * @param aAncestorLimiter    Optional, if you set this, it must be an
   *                            inclusive ancestor of aStartContent.
   */
  static nsIContent* GetPreviousLeafContentOrPreviousBlockElement(
      const nsIContent& aStartContent, const LeafNodeTypes& aLeafNodeTypes,
      BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr) {
    MOZ_ASSERT_IF(
        aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
        !aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode));
    NS_ASSERTION(!aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
                 "Not implemented yet");

    if (&aStartContent == aAncestorLimiter) {
      return nullptr;
    }

    nsIContent* previousContent = aStartContent.GetPreviousSibling();
    if (!previousContent) {
      if (!aStartContent.GetParentElement()) {
        NS_WARNING("Reached orphan node while climbing up the DOM tree");
        return nullptr;
      }
      for (Element* parentElement : aStartContent.AncestorsOfType<Element>()) {
        if (parentElement == aAncestorLimiter ||
            HTMLEditUtils::IsBlockElement(*parentElement, aBlockInlineCheck)) {
          return nullptr;
        }
        if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
            !parentElement->IsEditable()) {
          return nullptr;
        }
        previousContent = parentElement->GetPreviousSibling();
        if (previousContent) {
          break;
        }
        if (!parentElement->GetParentElement()) {
          NS_WARNING("Reached orphan node while climbing up the DOM tree");
          return nullptr;
        }
      }
      MOZ_ASSERT(previousContent);
      aBlockInlineCheck = IgnoreInsideBlockBoundary(aBlockInlineCheck);
    }

    // We have a next content.  If it's a block, return it.
    if (HTMLEditUtils::IsBlockElement(*previousContent, aBlockInlineCheck)) {
      return previousContent;
    }
    if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
        !HTMLEditUtils::IsSimplyEditableNode(*previousContent)) {
      return previousContent;
    }
    if (HTMLEditUtils::IsContainerNode(*previousContent)) {
      // Else if it's a container, get deep rightmost child
      if (nsIContent* child = HTMLEditUtils::GetLastLeafContent(
              *previousContent, aLeafNodeTypes, aBlockInlineCheck)) {
        return child;
      }
    }
    // Else return the next content itself.
    return previousContent;
  }

  /**
   * Similar to the above method, but take a DOM point to specify scan start
   * point.
   */
  template <typename PT, typename CT>
  static nsIContent* GetPreviousLeafContentOrPreviousBlockElement(
      const EditorDOMPointBase<PT, CT>& aStartPoint,
      const LeafNodeTypes& aLeafNodeTypes, BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr) {
    MOZ_ASSERT(aStartPoint.IsSet());
    MOZ_ASSERT_IF(
        aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
        !aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode));
    NS_ASSERTION(!aLeafNodeTypes.contains(LeafNodeType::OnlyEditableLeafNode),
                 "Not implemented yet");

    if (!aStartPoint.IsInContentNode()) {
      return nullptr;
    }
    if (aStartPoint.IsInTextNode()) {
      return HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
          *aStartPoint.template ContainerAs<Text>(), aLeafNodeTypes,
          aBlockInlineCheck, aAncestorLimiter);
    }
    if (!HTMLEditUtils::IsContainerNode(
            *aStartPoint.template ContainerAs<nsIContent>())) {
      return HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
          *aStartPoint.template ContainerAs<nsIContent>(), aLeafNodeTypes,
          aBlockInlineCheck, aAncestorLimiter);
    }

    if (aStartPoint.IsStartOfContainer()) {
      if (aStartPoint.GetContainer() == aAncestorLimiter ||
          HTMLEditUtils::IsBlockElement(
              *aStartPoint.template ContainerAs<nsIContent>(),
              aBlockInlineCheck)) {
        // We are at start of the block.
        return nullptr;
      }

      // We are at start of non-block container
      return HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
          *aStartPoint.template ContainerAs<nsIContent>(), aLeafNodeTypes,
          IgnoreInsideBlockBoundary(aBlockInlineCheck), aAncestorLimiter);
    }

    nsCOMPtr<nsIContent> previousContent =
        aStartPoint.GetPreviousSiblingOfChild();
    if (NS_WARN_IF(!previousContent)) {
      return nullptr;
    }

    // We have a prior node.  If it's a block, return it.
    if (HTMLEditUtils::IsBlockElement(*previousContent, aBlockInlineCheck)) {
      return previousContent;
    }
    if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
        !HTMLEditUtils::IsSimplyEditableNode(*previousContent)) {
      return previousContent;
    }
    if (HTMLEditUtils::IsContainerNode(*previousContent)) {
      // Else if it's a container, get deep rightmost child
      if (nsIContent* child = HTMLEditUtils::GetLastLeafContent(
              *previousContent, aLeafNodeTypes,
              IgnoreInsideBlockBoundary(aBlockInlineCheck))) {
        return child;
      }
    }
    // Else return the node itself
    return previousContent;
  }

  /**
   * Return previous non-empty leaf content or child block or non-editable
   * content (depending on aLeafNodeTypes).  This ignores invisible inline leaf
   * element like `<b></b>` and empty `Text` nodes.  So, this may return
   * invisible `Text` node, but it may be useful to consider whether we need to
   * insert a padding <br> element.
   */
  [[nodiscard]] static nsIContent*
  GetPreviousNonEmptyLeafContentOrPreviousBlockElement(
      const nsIContent& aContent, const LeafNodeTypes& aLeafNodeTypes,
      BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr) {
    for (nsIContent* previousContent =
             HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
                 aContent, aLeafNodeTypes, aBlockInlineCheck, aAncestorLimiter);
         previousContent;
         previousContent =
             HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
                 *previousContent, aLeafNodeTypes, aBlockInlineCheck,
                 aAncestorLimiter)) {
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrChildBlock) &&
          HTMLEditUtils::IsBlockElement(
              *previousContent,
              BlockInlineCheck::UseComputedDisplayOutsideStyle)) {
        return previousContent;  // Reached block element
      }
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
          HTMLEditUtils::IsSimplyEditableNode(*previousContent)) {
        return previousContent;  // Reached non-editable content
      }
      Text* const previousText = Text::FromNode(previousContent);
      if (!previousText) {
        if (!HTMLEditUtils::IsVisibleElementEvenIfLeafNode(*previousContent)) {
          continue;  // Ignore invisible inline elements
        }
        return previousContent;  // Reached visible inline element
      }
      if (!previousText->TextDataLength()) {
        continue;  // Ignore empty Text nodes.
      }
      return previousText;  // Reached non-empty text
    }
    return nullptr;
  }

  /**
   * Return previous visible leaf content or child block or non-editable content
   * (depending on aLeafNodeTypes).  This ignores invisible inline leaf element
   * like `<b></b>` and empty `Text` nodes.  So, this may return invisible
   * `Text` node, but it may be useful to consider whether we need to insert a
   * padding <br> element.
   */
  template <typename PT, typename CT>
  [[nodiscard]] static nsIContent*
  GetPreviousNonEmptyLeafContentOrPreviousBlockElement(
      const EditorDOMPointBase<PT, CT>& aPoint,
      const LeafNodeTypes& aLeafNodeTypes, BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr) {
    for (nsIContent* previousContent =
             HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
                 aPoint, aLeafNodeTypes, aBlockInlineCheck, aAncestorLimiter);
         previousContent;
         previousContent =
             HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
                 *previousContent, aLeafNodeTypes, aBlockInlineCheck,
                 aAncestorLimiter)) {
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrChildBlock) &&
          HTMLEditUtils::IsBlockElement(
              *previousContent,
              BlockInlineCheck::UseComputedDisplayOutsideStyle)) {
        return previousContent;  // Reached block element
      }
      if (aLeafNodeTypes.contains(LeafNodeType::LeafNodeOrNonEditableNode) &&
          HTMLEditUtils::IsSimplyEditableNode(*previousContent)) {
        return previousContent;  // Reached non-editable content
      }
      Text* const previousText = Text::FromNode(previousContent);
      if (!previousText) {
        if (!HTMLEditUtils::IsVisibleElementEvenIfLeafNode(*previousContent)) {
          continue;  // Ignore invisible inline elements
        }
        return previousContent;  // Reached visible inline element
      }
      if (!previousText->TextDataLength()) {
        continue;  // Ignore empty Text nodes.
      }
      return previousText;  // Reached non-empty text
    }
    return nullptr;
  }
  /**
   * Returns a content node whose inline styles should be preserved after
   * deleting content in a range.  Typically, you should set aPoint to start
   * boundary of the range to delete.
   */
  template <typename EditorDOMPointType>
  static nsIContent* GetContentToPreserveInlineStyles(
      const EditorDOMPointType& aPoint, const Element& aEditingHost);

  /**
   * Get previous/next editable point from start or end of aContent.
   */
  enum class InvisibleWhiteSpaces {
    Ignore,    // Ignore invisible white-spaces, i.e., don't return middle of
               // them.
    Preserve,  // Preserve invisible white-spaces, i.e., result may be start or
               // end of a text node even if it begins or ends with invisible
               // white-spaces.
  };
  enum class TableBoundary {
    Ignore,                  // May cross any table element boundary.
    NoCrossTableElement,     // Won't cross `<table>` element boundary.
    NoCrossAnyTableElement,  // Won't cross any table element boundary.
  };
  template <typename EditorDOMPointType>
  static EditorDOMPointType GetPreviousEditablePoint(
      nsIContent& aContent, const Element* aAncestorLimiter,
      InvisibleWhiteSpaces aInvisibleWhiteSpaces,
      TableBoundary aHowToTreatTableBoundary);
  template <typename EditorDOMPointType>
  static EditorDOMPointType GetNextEditablePoint(
      nsIContent& aContent, const Element* aAncestorLimiter,
      InvisibleWhiteSpaces aInvisibleWhiteSpaces,
      TableBoundary aHowToTreatTableBoundary);

  /**
   * GetAncestorElement() and GetInclusiveAncestorElement() return
   * (inclusive) block ancestor element of aContent whose time matches
   * aAncestorTypes.
   */
  enum class AncestorType {
    // If there is an ancestor block, it's a limiter of the scan.
    ClosestBlockElement,
    // If there is an ancestor container element, it's a limiter of the scan.
    ClosestContainerElement,
    // If there is no ancestor block in the range, the topmost inline element is
    // a limiter of the scan.
    MostDistantInlineElementInBlock,
    // Ignore ancestor <hr> elements to check whether a block.
    IgnoreHRElement,
    // If there is an ancestor <button> element, it's also a limiter of the
    // scan.
    ButtonElement,
    // The root element of the scan start node or the ancestor limiter may be
    // return if there is no proper element.
    AllowRootOrAncestorLimiterElement,

    // Limit to editable elements.  If it reaches an non-editable element,
    // return its child element.
    EditableElement,
  };
  using AncestorTypes = EnumSet<AncestorType>;
  constexpr static AncestorTypes
      ClosestEditableBlockElementOrInlineEditingHost = {
          AncestorType::ClosestBlockElement,
          AncestorType::MostDistantInlineElementInBlock,
          AncestorType::EditableElement};
  constexpr static AncestorTypes ClosestBlockElement = {
      AncestorType::ClosestBlockElement};
  constexpr static AncestorTypes ClosestEditableBlockElement = {
      AncestorType::ClosestBlockElement, AncestorType::EditableElement};
  constexpr static AncestorTypes ClosestBlockElementExceptHRElement = {
      AncestorType::ClosestBlockElement, AncestorType::IgnoreHRElement};
  constexpr static AncestorTypes ClosestEditableBlockElementExceptHRElement = {
      AncestorType::ClosestBlockElement, AncestorType::IgnoreHRElement,
      AncestorType::EditableElement};
  constexpr static AncestorTypes ClosestEditableBlockElementOrButtonElement = {
      AncestorType::ClosestBlockElement, AncestorType::EditableElement,
      AncestorType::ButtonElement};
  static Element* GetAncestorElement(const nsIContent& aContent,
                                     const AncestorTypes& aAncestorTypes,
                                     BlockInlineCheck aBlockInlineCheck,
                                     const Element* aAncestorLimiter = nullptr);
  static Element* GetInclusiveAncestorElement(
      const nsIContent& aContent, const AncestorTypes& aAncestorTypes,
      BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr);

  /**
   * GetClosestAncestorTableElement() returns the nearest inclusive ancestor
   * <table> element of aContent.
   */
  static Element* GetClosestAncestorTableElement(const nsIContent& aContent) {
    // TODO: the method name and its documentation clash with the
    // implementation. Split this method into
    // `GetClosestAncestorTableElement` and
    // `GetClosestInclusiveAncestorTableElement`.
    if (!aContent.GetParent()) {
      return nullptr;
    }
    for (Element* element : aContent.InclusiveAncestorsOfType<Element>()) {
      if (HTMLEditUtils::IsTable(element)) {
        return element;
      }
    }
    return nullptr;
  }

  static Element* GetInclusiveAncestorAnyTableElement(
      const nsIContent& aContent) {
    for (Element* parent : aContent.InclusiveAncestorsOfType<Element>()) {
      if (HTMLEditUtils::IsAnyTableElement(parent)) {
        return parent;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static Element* GetClosestAncestorAnyListElement(
      const nsIContent& aContent);
  [[nodiscard]] static Element* GetClosestInclusiveAncestorAnyListElement(
      const nsIContent& aContent);

  /**
   * GetClosestAncestorListItemElement() returns a list item element if
   * aContent or its ancestor in editing host is one.  However, this won't
   * cross table related element.
   */
  static Element* GetClosestAncestorListItemElement(
      const nsIContent& aContent, const Element* aAncestorLimit = nullptr) {
    MOZ_ASSERT_IF(aAncestorLimit,
                  aContent.IsInclusiveDescendantOf(aAncestorLimit));

    if (HTMLEditUtils::IsListItem(&aContent)) {
      return const_cast<Element*>(aContent.AsElement());
    }

    for (Element* parentElement : aContent.AncestorsOfType<Element>()) {
      if (HTMLEditUtils::IsAnyTableElement(parentElement)) {
        return nullptr;
      }
      if (HTMLEditUtils::IsListItem(parentElement)) {
        return parentElement;
      }
      if (parentElement == aAncestorLimit) {
        return nullptr;
      }
    }
    return nullptr;
  }

  /**
   * GetRangeSelectingAllContentInAllListItems() returns a range which selects
   * from start of the first list item to end of the last list item of
   * aListElement.  Note that the result may be in different list element if
   * aListElement has child list element(s) directly.
   */
  template <typename EditorDOMRangeType>
  static EditorDOMRangeType GetRangeSelectingAllContentInAllListItems(
      const Element& aListElement) {
    MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(&aListElement));
    Element* firstListItem =
        HTMLEditUtils::GetFirstListItemElement(aListElement);
    Element* lastListItem = HTMLEditUtils::GetLastListItemElement(aListElement);
    MOZ_ASSERT_IF(firstListItem, lastListItem);
    MOZ_ASSERT_IF(!firstListItem, !lastListItem);
    if (!firstListItem || !lastListItem) {
      return EditorDOMRangeType();
    }
    return EditorDOMRangeType(
        typename EditorDOMRangeType::PointType(firstListItem, 0u),
        EditorDOMRangeType::PointType::AtEndOf(*lastListItem));
  }

  /**
   * GetFirstListItemElement() returns the first list item element in the
   * pre-order tree traversal of the DOM.
   */
  static Element* GetFirstListItemElement(const Element& aListElement) {
    MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(&aListElement));
    for (nsIContent* maybeFirstListItem = aListElement.GetFirstChild();
         maybeFirstListItem;
         maybeFirstListItem = maybeFirstListItem->GetNextNode(&aListElement)) {
      if (HTMLEditUtils::IsListItem(maybeFirstListItem)) {
        return maybeFirstListItem->AsElement();
      }
    }
    return nullptr;
  }

  /**
   * GetLastListItemElement() returns the last list item element in the
   * post-order tree traversal of the DOM.  I.e., returns the last list
   * element whose close tag appears at last.
   */
  static Element* GetLastListItemElement(const Element& aListElement) {
    MOZ_ASSERT(HTMLEditUtils::IsAnyListElement(&aListElement));
    for (nsIContent* maybeLastListItem = aListElement.GetLastChild();
         maybeLastListItem;) {
      if (HTMLEditUtils::IsListItem(maybeLastListItem)) {
        return maybeLastListItem->AsElement();
      }
      if (maybeLastListItem->HasChildren()) {
        maybeLastListItem = maybeLastListItem->GetLastChild();
        continue;
      }
      if (maybeLastListItem->GetPreviousSibling()) {
        maybeLastListItem = maybeLastListItem->GetPreviousSibling();
        continue;
      }
      for (Element* parent = maybeLastListItem->GetParentElement(); parent;
           parent = parent->GetParentElement()) {
        maybeLastListItem = nullptr;
        if (parent == &aListElement) {
          return nullptr;
        }
        if (parent->GetPreviousSibling()) {
          maybeLastListItem = parent->GetPreviousSibling();
          break;
        }
      }
    }
    return nullptr;
  }

  /**
   * GetFirstTableCellElementChild() and GetLastTableCellElementChild()
   * return the first/last element child of <tr> element if it's a table
   * cell element.
   */
  static Element* GetFirstTableCellElementChild(
      const Element& aTableRowElement) {
    MOZ_ASSERT(aTableRowElement.IsHTMLElement(nsGkAtoms::tr));
    Element* firstElementChild = aTableRowElement.GetFirstElementChild();
    return firstElementChild && HTMLEditUtils::IsTableCell(firstElementChild)
               ? firstElementChild
               : nullptr;
  }
  static Element* GetLastTableCellElementChild(
      const Element& aTableRowElement) {
    MOZ_ASSERT(aTableRowElement.IsHTMLElement(nsGkAtoms::tr));
    Element* lastElementChild = aTableRowElement.GetLastElementChild();
    return lastElementChild && HTMLEditUtils::IsTableCell(lastElementChild)
               ? lastElementChild
               : nullptr;
  }

  /**
   * GetPreviousTableCellElementSibling() and GetNextTableCellElementSibling()
   * return a table cell element of previous/next element sibling of given
   * content node if and only if the element sibling is a table cell element.
   */
  static Element* GetPreviousTableCellElementSibling(
      const nsIContent& aChildOfTableRow) {
    MOZ_ASSERT(aChildOfTableRow.GetParentNode());
    MOZ_ASSERT(aChildOfTableRow.GetParentNode()->IsHTMLElement(nsGkAtoms::tr));
    Element* previousElementSibling =
        aChildOfTableRow.GetPreviousElementSibling();
    return previousElementSibling &&
                   HTMLEditUtils::IsTableCell(previousElementSibling)
               ? previousElementSibling
               : nullptr;
  }
  static Element* GetNextTableCellElementSibling(
      const nsIContent& aChildOfTableRow) {
    MOZ_ASSERT(aChildOfTableRow.GetParentNode());
    MOZ_ASSERT(aChildOfTableRow.GetParentNode()->IsHTMLElement(nsGkAtoms::tr));
    Element* nextElementSibling = aChildOfTableRow.GetNextElementSibling();
    return nextElementSibling && HTMLEditUtils::IsTableCell(nextElementSibling)
               ? nextElementSibling
               : nullptr;
  }

  /**
   * GetMostDistantAncestorInlineElement() returns the most distant ancestor
   * inline element between aContent and the aEditingHost.  Even if aEditingHost
   * is an inline element, this method never returns aEditingHost as the result.
   * Optionally, you can specify ancestor limiter content node.  This guarantees
   * that the result is a descendant of aAncestorLimiter if aContent is a
   * descendant of aAncestorLimiter.
   */
  static nsIContent* GetMostDistantAncestorInlineElement(
      const nsIContent& aContent, BlockInlineCheck aBlockInlineCheck,
      const Element* aEditingHost = nullptr,
      const nsIContent* aAncestorLimiter = nullptr) {
    if (HTMLEditUtils::IsBlockElement(aContent, aBlockInlineCheck)) {
      return nullptr;
    }

    // If aNode is the editing host itself, there is no modifiable inline
    // parent.
    if (&aContent == aEditingHost || &aContent == aAncestorLimiter) {
      return nullptr;
    }

    // If aNode is outside of the <body> element, we don't support to edit
    // such elements for now.
    // XXX This should be MOZ_ASSERT after fixing bug 1413131 for avoiding
    //     calling this expensive method.
    if (aEditingHost && !aContent.IsInclusiveDescendantOf(aEditingHost)) {
      return nullptr;
    }

    if (!aContent.GetParent()) {
      return const_cast<nsIContent*>(&aContent);
    }

    // Looks for the highest inline parent in the editing host.
    nsIContent* topMostInlineContent = const_cast<nsIContent*>(&aContent);
    for (Element* element : aContent.AncestorsOfType<Element>()) {
      if (element == aEditingHost || element == aAncestorLimiter ||
          HTMLEditUtils::IsBlockElement(*element, aBlockInlineCheck)) {
        break;
      }
      topMostInlineContent = element;
    }
    return topMostInlineContent;
  }

  /**
   * GetMostDistantAncestorEditableEmptyInlineElement() returns most distant
   * ancestor which only has aEmptyContent or its ancestor, editable and
   * inline element.
   */
  static Element* GetMostDistantAncestorEditableEmptyInlineElement(
      const nsIContent& aEmptyContent, BlockInlineCheck aBlockInlineCheck,
      const Element* aEditingHost = nullptr,
      const nsIContent* aAncestorLimiter = nullptr) {
    if (&aEmptyContent == aEditingHost || &aEmptyContent == aAncestorLimiter) {
      return nullptr;
    }
    nsIContent* lastEmptyContent = const_cast<nsIContent*>(&aEmptyContent);
    for (Element* element : aEmptyContent.AncestorsOfType<Element>()) {
      if (element == aEditingHost || element == aAncestorLimiter) {
        break;
      }
      if (!HTMLEditUtils::IsInlineContent(*element, aBlockInlineCheck) ||
          !HTMLEditUtils::IsSimplyEditableNode(*element)) {
        break;
      }
      if (element->GetChildCount() > 1) {
        for (const nsIContent* child = element->GetFirstChild(); child;
             child = child->GetNextSibling()) {
          if (child == lastEmptyContent || child->IsComment()) {
            continue;
          }
          return lastEmptyContent != &aEmptyContent
                     ? Element::FromNode(lastEmptyContent)
                     : nullptr;
        }
      }
      lastEmptyContent = element;
    }
    return lastEmptyContent != &aEmptyContent
               ? Element::FromNode(lastEmptyContent)
               : nullptr;
  }

  /**
   * GetElementIfOnlyOneSelected() returns an element if aRange selects only
   * the element node (and its descendants).
   */
  static Element* GetElementIfOnlyOneSelected(const AbstractRange& aRange) {
    return GetElementIfOnlyOneSelected(EditorRawDOMRange(aRange));
  }
  template <typename EditorDOMPointType>
  static Element* GetElementIfOnlyOneSelected(
      const EditorDOMRangeBase<EditorDOMPointType>& aRange) {
    if (!aRange.IsPositioned() || aRange.Collapsed()) {
      return nullptr;
    }
    const auto& start = aRange.StartRef();
    const auto& end = aRange.EndRef();
    if (NS_WARN_IF(!start.IsSetAndValid()) ||
        NS_WARN_IF(!end.IsSetAndValid()) ||
        start.GetContainer() != end.GetContainer()) {
      return nullptr;
    }
    nsIContent* childAtStart = start.GetChild();
    if (!childAtStart || !childAtStart->IsElement()) {
      return nullptr;
    }
    // If start child is not the last sibling and only if end child is its
    // next sibling, the start child is selected.
    if (childAtStart->GetNextSibling()) {
      return childAtStart->GetNextSibling() == end.GetChild()
                 ? childAtStart->AsElement()
                 : nullptr;
    }
    // If start child is the last sibling and only if no child at the end,
    // the start child is selected.
    return !end.GetChild() ? childAtStart->AsElement() : nullptr;
  }

  static Element* GetTableCellElementIfOnlyOneSelected(
      const AbstractRange& aRange) {
    Element* element = HTMLEditUtils::GetElementIfOnlyOneSelected(aRange);
    return element && HTMLEditUtils::IsTableCell(element) ? element : nullptr;
  }

  /**
   * GetFirstSelectedTableCellElement() returns a table cell element (i.e.,
   * `<td>` or `<th>` if and only if first selection range selects only a
   * table cell element.
   */
  static Element* GetFirstSelectedTableCellElement(
      const Selection& aSelection) {
    if (!aSelection.RangeCount()) {
      return nullptr;
    }
    const nsRange* firstRange = aSelection.GetRangeAt(0);
    if (NS_WARN_IF(!firstRange) || NS_WARN_IF(!firstRange->IsPositioned())) {
      return nullptr;
    }
    return GetTableCellElementIfOnlyOneSelected(*firstRange);
  }

  /**
   * GetInclusiveFirstChildWhichHasOneChild() returns the deepest element whose
   * tag name is one of `aFirstElementName` and `aOtherElementNames...` if and
   * only if the elements have only one child node.   In other words, when
   * this method meets an element which does not matches any of the tag name
   * or it has no children or 2+ children.
   *
   * XXX This method must be implemented without treating edge cases.  So, the
   *     behavior is odd.  E.g., why can we ignore non-editable node at counting
   *     each children?  Why do we dig non-editable aNode or first child of its
   *     descendants?
   */
  template <typename FirstElementName, typename... OtherElementNames>
  static Element* GetInclusiveDeepestFirstChildWhichHasOneChild(
      const nsINode& aNode, const WalkTreeOptions& aOptions,
      BlockInlineCheck aBlockInlineCheck, FirstElementName aFirstElementName,
      OtherElementNames... aOtherElementNames) {
    if (!aNode.IsElement()) {
      return nullptr;
    }
    Element* parentElement = nullptr;
    for (nsIContent* content = const_cast<nsIContent*>(aNode.AsContent());
         content && content->IsElement() &&
         content->IsAnyOfHTMLElements(aFirstElementName, aOtherElementNames...);
         // XXX Why do we scan only the first child of every element?  If it's
         //     not editable, why do we ignore it when aOptions specifies so.
         content = content->GetFirstChild()) {
      if (HTMLEditUtils::CountChildren(*content, aOptions, aBlockInlineCheck) !=
          1) {
        return content->AsElement();
      }
      parentElement = content->AsElement();
    }
    return parentElement;
  }

  /**
   * Get the first line break in aElement.  This scans only leaf nodes so
   * if a <br> element has children illegally, it'll be ignored.
   *
   * @param aElement    The element which may have a <br> element or a
   *                    preformatted linefeed.
   */
  template <typename EditorLineBreakType>
  static Maybe<EditorLineBreakType> GetFirstLineBreak(
      const dom::Element& aElement) {
    for (nsIContent* content = HTMLEditUtils::GetFirstLeafContent(
             aElement, {LeafNodeType::OnlyLeafNode});
         content; content = HTMLEditUtils::GetNextContent(
                      *content,
                      {WalkTreeOption::IgnoreDataNodeExceptText,
                       WalkTreeOption::IgnoreWhiteSpaceOnlyText},
                      BlockInlineCheck::Unused, &aElement)) {
      if (auto* brElement = dom::HTMLBRElement::FromNode(*content)) {
        return Some(EditorLineBreakType(*brElement));
      }
      if (auto* textNode = Text::FromNode(*content)) {
        if (EditorUtils::IsNewLinePreformatted(*textNode)) {
          uint32_t offset = textNode->TextFragment().FindChar(kNewLine);
          if (offset != nsTextFragment::kNotFound) {
            return Some(EditorLineBreakType(*textNode, offset));
          }
        }
      }
    }
    return Nothing();
  }

  enum class ScanLineBreak {
    AtEndOfBlock,
    BeforeBlock,
  };
  /**
   * Return last <br> element or last text node ending with a preserved line
   * break of/before aBlockElement.
   * Note that the result may be non-editable and/or non-removable.
   */
  template <typename EditorLineBreakType>
  static Maybe<EditorLineBreakType> GetUnnecessaryLineBreak(
      const Element& aBlockElement, ScanLineBreak aScanLineBreak);

  /**
   * Return following <br> element from aPoint if and only if it's immediately
   * before a block boundary but it's not necessary to make the preceding
   * empty line of the block boundary visible anymore.
   * Note that the result may be non-editable and/or non-removable linebreak.
   */
  template <typename EditorLineBreakType, typename EditorDOMPointType>
  [[nodiscard]] static Maybe<EditorLineBreakType>
  GetFollowingUnnecessaryLineBreak(const EditorDOMPointType& aPoint);

  /**
   * IsInTableCellSelectionMode() returns true when Gecko's editor thinks that
   * selection is in a table cell selection mode.
   * Note that Gecko's editor traditionally treats selection as in table cell
   * selection mode when first range selects a table cell element.  I.e., even
   * if `nsFrameSelection` is not in table cell selection mode, this may return
   * true.
   */
  static bool IsInTableCellSelectionMode(const Selection& aSelection) {
    return GetFirstSelectedTableCellElement(aSelection) != nullptr;
  }

  static EditAction GetEditActionForInsert(const nsAtom& aTagName);
  static EditAction GetEditActionForRemoveList(const nsAtom& aTagName);
  static EditAction GetEditActionForInsert(const Element& aElement);
  static EditAction GetEditActionForFormatText(const nsAtom& aProperty,
                                               const nsAtom* aAttribute,
                                               bool aToSetStyle);
  static EditAction GetEditActionForAlignment(const nsAString& aAlignType);

  /**
   * GetPreviousNonCollapsibleCharOffset() returns offset of previous
   * character which is not collapsible white-space characters.
   */
  enum class WalkTextOption {
    TreatNBSPsCollapsible,
  };
  using WalkTextOptions = EnumSet<WalkTextOption>;
  static Maybe<uint32_t> GetPreviousNonCollapsibleCharOffset(
      const EditorDOMPointInText& aPoint,
      const WalkTextOptions& aWalkTextOptions = {}) {
    MOZ_ASSERT(aPoint.IsSetAndValid());
    return GetPreviousNonCollapsibleCharOffset(
        *aPoint.ContainerAs<Text>(), aPoint.Offset(), aWalkTextOptions);
  }
  static Maybe<uint32_t> GetPreviousNonCollapsibleCharOffset(
      const Text& aTextNode, uint32_t aOffset,
      const WalkTextOptions& aWalkTextOptions = {}) {
    if (MOZ_UNLIKELY(!aOffset)) {
      return Nothing{};
    }
    MOZ_ASSERT(aOffset <= aTextNode.TextDataLength());
    if (EditorUtils::IsWhiteSpacePreformatted(aTextNode)) {
      return Some(aOffset - 1);
    }
    WhitespaceOptions whitespaceOptions{
        WhitespaceOption::FormFeedIsSignificant};
    if (EditorUtils::IsNewLinePreformatted(aTextNode)) {
      whitespaceOptions += WhitespaceOption::NewLineIsSignificant;
    }
    if (aWalkTextOptions.contains(WalkTextOption::TreatNBSPsCollapsible)) {
      whitespaceOptions += WhitespaceOption::TreatNBSPAsCollapsible;
    }
    const uint32_t prevVisibleCharOffset =
        aTextNode.TextFragment().RFindNonWhitespaceChar(whitespaceOptions,
                                                        aOffset - 1);
    return prevVisibleCharOffset != nsTextFragment::kNotFound
               ? Some(prevVisibleCharOffset)
               : Nothing();
  }

  /**
   * GetNextNonCollapsibleCharOffset() returns offset of next character which is
   * not collapsible white-space characters.
   */
  static Maybe<uint32_t> GetNextNonCollapsibleCharOffset(
      const EditorDOMPointInText& aPoint,
      const WalkTextOptions& aWalkTextOptions = {}) {
    MOZ_ASSERT(aPoint.IsSetAndValid());
    return GetNextNonCollapsibleCharOffset(*aPoint.ContainerAs<Text>(),
                                           aPoint.Offset(), aWalkTextOptions);
  }
  static Maybe<uint32_t> GetNextNonCollapsibleCharOffset(
      const Text& aTextNode, uint32_t aOffset,
      const WalkTextOptions& aWalkTextOptions = {}) {
    return GetInclusiveNextNonCollapsibleCharOffset(aTextNode, aOffset + 1,
                                                    aWalkTextOptions);
  }

  /**
   * GetInclusiveNextNonCollapsibleCharOffset() returns offset of inclusive next
   * character which is not collapsible white-space characters.
   */
  template <typename PT, typename CT>
  static Maybe<uint32_t> GetInclusiveNextNonCollapsibleCharOffset(
      const EditorDOMPointBase<PT, CT>& aPoint,
      const WalkTextOptions& aWalkTextOptions = {}) {
    static_assert(std::is_same<PT, RefPtr<Text>>::value ||
                  std::is_same<PT, Text*>::value);
    MOZ_ASSERT(aPoint.IsSetAndValid());
    return GetInclusiveNextNonCollapsibleCharOffset(
        *aPoint.template ContainerAs<Text>(), aPoint.Offset(),
        aWalkTextOptions);
  }
  static Maybe<uint32_t> GetInclusiveNextNonCollapsibleCharOffset(
      const Text& aTextNode, uint32_t aOffset,
      const WalkTextOptions& aWalkTextOptions = {}) {
    if (MOZ_UNLIKELY(aOffset >= aTextNode.TextDataLength())) {
      return Nothing();
    }
    MOZ_ASSERT(aOffset <= aTextNode.TextDataLength());
    if (EditorUtils::IsWhiteSpacePreformatted(aTextNode)) {
      return Some(aOffset);
    }
    WhitespaceOptions whitespaceOptions{
        WhitespaceOption::FormFeedIsSignificant};
    if (EditorUtils::IsNewLinePreformatted(aTextNode)) {
      whitespaceOptions += WhitespaceOption::NewLineIsSignificant;
    }
    if (aWalkTextOptions.contains(WalkTextOption::TreatNBSPsCollapsible)) {
      whitespaceOptions += WhitespaceOption::TreatNBSPAsCollapsible;
    }
    const uint32_t inclusiveNextVisibleCharOffset =
        aTextNode.TextFragment().FindNonWhitespaceChar(whitespaceOptions,
                                                       aOffset);
    if (inclusiveNextVisibleCharOffset != nsTextFragment::kNotFound) {
      return Some(inclusiveNextVisibleCharOffset);
    }
    return Nothing();
  }

  /**
   * GetFirstWhiteSpaceOffsetCollapsedWith() returns first collapsible
   * white-space offset which is collapsed with a white-space at the given
   * position.  I.e., the character at the position must be a collapsible
   * white-space.
   */
  template <typename PT, typename CT>
  static uint32_t GetFirstWhiteSpaceOffsetCollapsedWith(
      const EditorDOMPointBase<PT, CT>& aPoint,
      const WalkTextOptions& aWalkTextOptions = {}) {
    static_assert(std::is_same<PT, RefPtr<Text>>::value ||
                  std::is_same<PT, Text*>::value);
    MOZ_ASSERT(aPoint.IsSetAndValid());
    MOZ_ASSERT(!aPoint.IsEndOfContainer());
    MOZ_ASSERT_IF(
        aWalkTextOptions.contains(WalkTextOption::TreatNBSPsCollapsible),
        aPoint.IsCharCollapsibleASCIISpaceOrNBSP());
    MOZ_ASSERT_IF(
        !aWalkTextOptions.contains(WalkTextOption::TreatNBSPsCollapsible),
        aPoint.IsCharCollapsibleASCIISpace());
    return GetFirstWhiteSpaceOffsetCollapsedWith(
        *aPoint.template ContainerAs<Text>(), aPoint.Offset(),
        aWalkTextOptions);
  }
  static uint32_t GetFirstWhiteSpaceOffsetCollapsedWith(
      const Text& aTextNode, uint32_t aOffset,
      const WalkTextOptions& aWalkTextOptions = {}) {
    MOZ_ASSERT(aOffset < aTextNode.TextLength());
    MOZ_ASSERT_IF(
        aWalkTextOptions.contains(WalkTextOption::TreatNBSPsCollapsible),
        EditorRawDOMPoint(&aTextNode, aOffset)
            .IsCharCollapsibleASCIISpaceOrNBSP());
    MOZ_ASSERT_IF(
        !aWalkTextOptions.contains(WalkTextOption::TreatNBSPsCollapsible),
        EditorRawDOMPoint(&aTextNode, aOffset).IsCharCollapsibleASCIISpace());
    if (!aOffset) {
      return 0;
    }
    Maybe<uint32_t> previousVisibleCharOffset =
        GetPreviousNonCollapsibleCharOffset(aTextNode, aOffset,
                                            aWalkTextOptions);
    return previousVisibleCharOffset.isSome()
               ? previousVisibleCharOffset.value() + 1
               : 0;
  }

  /**
   * GetPreviousPreformattedNewLineInTextNode() returns a point which points
   * previous preformatted linefeed if there is and aPoint is in a text node.
   * If the node's linefeed characters are not preformatted or aPoint is not
   * in a text node, this returns unset DOM point.
   */
  template <typename EditorDOMPointType, typename ArgEditorDOMPointType>
  static EditorDOMPointType GetPreviousPreformattedNewLineInTextNode(
      const ArgEditorDOMPointType& aPoint) {
    if (!aPoint.IsInTextNode() || aPoint.IsStartOfContainer() ||
        !EditorUtils::IsNewLinePreformatted(
            *aPoint.template ContainerAs<Text>())) {
      return EditorDOMPointType();
    }
    const Text& textNode = *aPoint.template ContainerAs<Text>();
    MOZ_ASSERT(aPoint.Offset() <= textNode.TextFragment().GetLength());
    const uint32_t previousLineBreakOffset =
        textNode.TextFragment().RFindChar('\n', aPoint.Offset() - 1u);
    return previousLineBreakOffset != nsTextFragment::kNotFound
               ? EditorDOMPointType(&textNode, previousLineBreakOffset)
               : EditorDOMPointType();
  }

  /**
   * GetInclusiveNextPreformattedNewLineInTextNode() returns a point which
   * points inclusive next preformatted linefeed if there is and aPoint is in a
   * text node. If the node's linefeed characters are not preformatted or aPoint
   * is not in a text node, this returns unset DOM point.
   */
  template <typename EditorDOMPointType, typename ArgEditorDOMPointType>
  static EditorDOMPointType GetInclusiveNextPreformattedNewLineInTextNode(
      const ArgEditorDOMPointType& aPoint) {
    if (!aPoint.IsInTextNode() || aPoint.IsEndOfContainer() ||
        !EditorUtils::IsNewLinePreformatted(
            *aPoint.template ContainerAs<Text>())) {
      return EditorDOMPointType();
    }
    const Text& textNode = *aPoint.template ContainerAs<Text>();
    MOZ_ASSERT(aPoint.Offset() <= textNode.TextFragment().GetLength());
    const uint32_t inclusiveNextVisibleCharOffset =
        textNode.TextFragment().FindChar('\n', aPoint.Offset());
    return inclusiveNextVisibleCharOffset != nsTextFragment::kNotFound
               ? EditorDOMPointType(&textNode, inclusiveNextVisibleCharOffset)
               : EditorDOMPointType();
  }

  /**
   * Get the first visible char offset in aText.  I.e., this returns invisible
   * white-space length at start of aText.  If there is no visible char in
   * aText, this returns the text data length.
   * Note that WSRunScanner::GetFirstVisiblePoint() may return different `Text`
   * node point, but this does not scan following `Text` nodes even if aText
   * is completely invisible.
   */
  [[nodiscard]] static uint32_t GetFirstVisibleCharOffset(const Text& aText);

  /**
   * Get next offset of the last visible char in aText.  I.e., this returns
   * the first offset of invisible trailing white-spaces.  If there is no
   * invisible trailing white-spaces in aText, this returns 0.
   * Note that WSRunScanner::GetAfterLastVisiblePoint() may return different
   * `Text` node point, but this does not scan preceding `Text` nodes even if
   * aText is completely invisible.
   */
  [[nodiscard]] static uint32_t GetOffsetAfterLastVisibleChar(
      const Text& aText);

  /**
   * Get the number of invisible white-spaces in the white-space sequence.  Note
   * that some invisible white-spaces may be after the first visible character.
   * E.g., "SP SP NBSP SP SP NBSP".  If this Text follows a block boundary, the
   * first SPs are the leading invisible white-spaces, and the first NBSP is the
   * first visible character.  However, following 2 SPs are collapsed to one.
   * Therefore, one of them is counted as an invisible white-space.
   *
   * Note that this assumes that all white-spaces starting from aOffset and
   * ending by aOffset + aLength are collapsible white-spaces including NBSPs.
   */
  [[nodiscard]] static uint32_t GetInvisibleWhiteSpaceCount(
      const Text& aText, uint32_t aOffset = 0u, uint32_t aLength = UINT32_MAX);

  /**
   * GetGoodCaretPointFor() returns a good point to collapse `Selection`
   * after handling edit action with aDirectionAndAmount.
   *
   * @param aContent            The content where you want to put caret
   *                            around.
   * @param aDirectionAndAmount Muse be one of eNext, eNextWord, eToEndOfLine,
   *                            ePrevious, ePreviousWord and eToBeggingOfLine.
   *                            Set the direction of handled edit action.
   */
  template <typename EditorDOMPointType>
  static EditorDOMPointType GetGoodCaretPointFor(
      nsIContent& aContent, nsIEditor::EDirection aDirectionAndAmount) {
    MOZ_ASSERT(nsIEditor::EDirectionIsValidExceptNone(aDirectionAndAmount));

    // XXX Why don't we check whether the candidate position is enable or not?
    //     When the result is not editable point, caret will be enclosed in
    //     the non-editable content.

    // If we can put caret in aContent, return start or end in it.
    if (aContent.IsText() || HTMLEditUtils::IsContainerNode(aContent) ||
        NS_WARN_IF(!aContent.GetParentNode())) {
      return EditorDOMPointType(
          &aContent, nsIEditor::DirectionIsDelete(aDirectionAndAmount)
                         ? 0
                         : aContent.Length());
    }

    // If we are going forward, put caret at aContent itself.
    if (nsIEditor::DirectionIsDelete(aDirectionAndAmount)) {
      return EditorDOMPointType(&aContent);
    }

    // If we are going backward, put caret to next node unless aContent is an
    // invisible `<br>` element.
    // XXX Shouldn't we put caret to first leaf of the next node?
    if (!HTMLEditUtils::IsInvisibleBRElement(aContent)) {
      EditorDOMPointType ret(EditorDOMPointType::After(aContent));
      NS_WARNING_ASSERTION(ret.IsSet(), "Failed to set after aContent");
      return ret;
    }

    // Otherwise, we should put caret at the invisible `<br>` element.
    return EditorDOMPointType(&aContent);
  }

  /**
   * GetBetterInsertionPointFor() returns better insertion point to insert
   * aContentToInsert.
   *
   * @param aContentToInsert    The content to insert.
   * @param aPointToInsert      A candidate point to insert the node.
   * @return                    Better insertion point if next visible node
   *                            is a <br> element and previous visible node
   *                            is neither none, another <br> element nor
   *                            different block level element.
   */
  template <typename EditorDOMPointType, typename EditorDOMPointTypeInput>
  static EditorDOMPointType GetBetterInsertionPointFor(
      const nsIContent& aContentToInsert,
      const EditorDOMPointTypeInput& aPointToInsert);

  /**
   * GetBetterCaretPositionToInsertText() returns better point to put caret
   * if aPoint is near a text node or in non-container node.
   */
  template <typename EditorDOMPointType, typename EditorDOMPointTypeInput>
  static EditorDOMPointType GetBetterCaretPositionToInsertText(
      const EditorDOMPointTypeInput& aPoint);

  /**
   * ComputePointToPutCaretInElementIfOutside() returns a good point in aElement
   * to put caret if aCurrentPoint is outside of aElement.
   *
   * @param aElement        The result is a point in aElement.
   * @param aCurrentPoint   The current (candidate) caret point.  Only if this
   *                        is outside aElement, returns a point in aElement.
   */
  template <typename EditorDOMPointType, typename EditorDOMPointTypeInput>
  static Result<EditorDOMPointType, nsresult>
  ComputePointToPutCaretInElementIfOutside(
      const Element& aElement, const EditorDOMPointTypeInput& aCurrentPoint);

  /**
   * Content-based query returns true if
   * <mHTMLProperty mAttribute=mAttributeValue> effects aContent.  If there is
   * such a element, but another element whose attribute value does not match
   * with mAttributeValue is closer ancestor of aContent, then the distant
   * ancestor does not effect aContent.
   *
   * @param aContent    The target of the query
   * @param aStyle      The style which queries a representing element.
   * @param aValue      Optional, the value of aStyle.mAttribute, example: blue
   *                    in <font color="blue"> May be null.  Ignored if
   *                    aStyle.mAttribute is null.
   * @param aOutValue   [OUT] the value of the attribute, if returns true
   * @return            true if <mHTMLProperty mAttribute=mAttributeValue>
   *                    effects aContent.
   */
  [[nodiscard]] static bool IsInlineStyleSetByElement(
      const nsIContent& aContent, const EditorInlineStyle& aStyle,
      const nsAString* aValue, nsAString* aOutValue = nullptr);

  /**
   * CollectAllChildren() collects all child nodes of aParentNode.
   */
  static void CollectAllChildren(
      const nsINode& aParentNode,
      nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents) {
    MOZ_ASSERT(aOutArrayOfContents.IsEmpty());
    aOutArrayOfContents.SetCapacity(aParentNode.GetChildCount());
    for (nsIContent* childContent = aParentNode.GetFirstChild(); childContent;
         childContent = childContent->GetNextSibling()) {
      aOutArrayOfContents.AppendElement(*childContent);
    }
  }

  /**
   * CollectChildren() collects child nodes of aNode (starting from
   * first editable child, but may return non-editable children after it).
   *
   * @param aNode               Parent node of retrieving children.
   * @param aOutArrayOfContents [out] This method will inserts found children
   *                            into this array.
   * @param aIndexToInsertChildren      Starting from this index, found
   *                                    children will be inserted to the array.
   * @param aOptions            Options to scan the children.
   * @return                    Number of found children.
   */
  static size_t CollectChildren(
      const nsINode& aNode,
      nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents,
      const CollectChildrenOptions& aOptions) {
    return HTMLEditUtils::CollectChildren(aNode, aOutArrayOfContents, 0u,
                                          aOptions);
  }
  static size_t CollectChildren(
      const nsINode& aNode,
      nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents,
      size_t aIndexToInsertChildren, const CollectChildrenOptions& aOptions);

  /**
   * CollectEmptyInlineContainerDescendants() appends empty inline elements in
   * aNode to aOutArrayOfContents.  Although it's array of nsIContent, the
   * instance will be elements.
   *
   * @param aNode               The node whose descendants may have empty inline
   *                            elements.
   * @param aOutArrayOfContents [out] This method will append found descendants
   *                            into this array.
   * @param aOptions            The option which element should be treated as
   *                            empty.
   * @param aBlockInlineCheck   Whether use computed style or HTML default style
   *                            when consider block vs. inline.
   * @return                    Number of found elements.
   */
  static size_t CollectEmptyInlineContainerDescendants(
      const nsINode& aNode,
      nsTArray<OwningNonNull<nsIContent>>& aOutArrayOfContents,
      const EmptyCheckOptions& aOptions, BlockInlineCheck aBlockInlineCheck);

  /**
   * Check whether aElement has attributes except the name aAttribute and
   * "_moz_*" attributes.
   */
  [[nodiscard]] static bool ElementHasAttribute(const Element& aElement) {
    return ElementHasAttributeExcept(aElement, *nsGkAtoms::_empty,
                                     *nsGkAtoms::empty, *nsGkAtoms::_empty);
  }
  [[nodiscard]] static bool ElementHasAttributeExcept(
      const Element& aElement, const nsAtom& aAttribute) {
    return ElementHasAttributeExcept(aElement, aAttribute, *nsGkAtoms::_empty,
                                     *nsGkAtoms::empty);
  }
  [[nodiscard]] static bool ElementHasAttributeExcept(
      const Element& aElement, const nsAtom& aAttribute1,
      const nsAtom& aAttribute2) {
    return ElementHasAttributeExcept(aElement, aAttribute1, aAttribute2,
                                     *nsGkAtoms::empty);
  }
  [[nodiscard]] static bool ElementHasAttributeExcept(
      const Element& aElement, const nsAtom& aAttribute1,
      const nsAtom& aAttribute2, const nsAtom& aAttribute3);

  enum class EditablePointOption {
    // Do not ignore invisible collapsible white-spaces which are next to a
    // block boundary.
    RecognizeInvisibleWhiteSpaces,
    // Stop at Comment node.
    StopAtComment,
    // Stop at List element.
    StopAtListElement,
    // Stop at ListItem element.
    StopAtListItemElement,
    // Stop at Table element.
    StopAtTableElement,
    // Stop at any table element.
    StopAtAnyTableElement,
  };
  using EditablePointOptions = EnumSet<EditablePointOption>;

  friend std::ostream& operator<<(std::ostream& aStream,
                                  const EditablePointOption& aOption);
  friend std::ostream& operator<<(std::ostream& aStream,
                                  const EditablePointOptions& aOptions);

 private:
  class MOZ_STACK_CLASS AutoEditablePointChecker final {
   public:
    explicit AutoEditablePointChecker(const EditablePointOptions& aOptions)
        : mIgnoreInvisibleText(!aOptions.contains(
              EditablePointOption::RecognizeInvisibleWhiteSpaces)),
          mIgnoreComment(
              !aOptions.contains(EditablePointOption::StopAtComment)),
          mStopAtListElement(
              aOptions.contains(EditablePointOption::StopAtListElement)),
          mStopAtListItemElement(
              aOptions.contains(EditablePointOption::StopAtListItemElement)),
          mStopAtTableElement(
              aOptions.contains(EditablePointOption::StopAtTableElement)),
          mStopAtAnyTableElement(
              aOptions.contains(EditablePointOption::StopAtAnyTableElement)) {}

    [[nodiscard]] bool IgnoreInvisibleWhiteSpaces() const {
      return mIgnoreInvisibleText;
    }

    [[nodiscard]] bool NodeShouldBeIgnored(const nsIContent& aContent) const {
      if (mIgnoreInvisibleText && aContent.IsText() &&
          HTMLEditUtils::IsSimplyEditableNode(aContent) &&
          !HTMLEditUtils::IsVisibleTextNode(*aContent.AsText())) {
        return true;
      }
      if (mIgnoreComment && aContent.IsComment()) {
        return true;
      }
      return false;
    }

    [[nodiscard]] bool ShouldStopScanningAt(const nsIContent& aContent) const {
      if (HTMLEditUtils::IsAnyListElement(&aContent)) {
        return mStopAtListElement;
      }
      if (HTMLEditUtils::IsListItem(&aContent)) {
        return mStopAtListItemElement;
      }
      if (HTMLEditUtils::IsAnyTableElement(&aContent)) {
        return mStopAtAnyTableElement ||
               (mStopAtTableElement && HTMLEditUtils::IsTable(&aContent));
      }
      return false;
    }

   private:
    const bool mIgnoreInvisibleText;
    const bool mIgnoreComment;
    const bool mStopAtListElement;
    const bool mStopAtListItemElement;
    const bool mStopAtTableElement;
    const bool mStopAtAnyTableElement;
  };

 public:
  /**
   * Return a point which points deepest editable start point of aContent.  This
   * walks the DOM tree in aContent to search meaningful first descendant.  If
   * EditablePointOption::IgnoreInvisibleText is specified, this returns first
   * visible char offset if this reaches a visible `Text` first.  If there is an
   * empty inline element such as <span>, this returns start of the inline
   * element.  If this reaches non-editable element or non-container element
   * like <img>, this returns the position.
   */
  template <typename EditorDOMPointType>
  [[nodiscard]] static EditorDOMPointType GetDeepestEditableStartPointOf(
      const nsIContent& aContent, const EditablePointOptions& aOptions) {
    if (NS_WARN_IF(!EditorUtils::IsEditableContent(
            aContent, EditorBase::EditorType::HTML))) {
      return EditorDOMPointType();
    }
    const AutoEditablePointChecker checker(aOptions);
    EditorRawDOMPoint result(&aContent, 0u);
    while (true) {
      nsIContent* firstChild = result.GetContainer()->GetFirstChild();
      if (!firstChild) {
        break;
      }
      // If the caller wants to skip invisible white-spaces, we should skip
      // invisible text nodes.
      nsIContent* meaningfulFirstChild = nullptr;
      if (checker.NodeShouldBeIgnored(*firstChild)) {
        // If we ignored a non-empty `Text`, it means that we're next to a block
        // boundary.
        for (nsIContent* nextSibling = firstChild->GetNextSibling();
             nextSibling; nextSibling = nextSibling->GetNextSibling()) {
          if (!checker.NodeShouldBeIgnored(*nextSibling) ||
              checker.ShouldStopScanningAt(*nextSibling)) {
            meaningfulFirstChild = nextSibling;
            break;
          }
        }
        if (!meaningfulFirstChild) {
          break;
        }
      } else {
        meaningfulFirstChild = firstChild;
      }
      if (meaningfulFirstChild->IsText()) {
        if (checker.IgnoreInvisibleWhiteSpaces()) {
          result.Set(meaningfulFirstChild,
                     HTMLEditUtils::GetInclusiveNextNonCollapsibleCharOffset(
                         *meaningfulFirstChild->AsText(), 0u)
                         .valueOr(0u));
        } else {
          result.Set(meaningfulFirstChild, 0u);
        }
        break;
      }
      if (checker.ShouldStopScanningAt(*meaningfulFirstChild) ||
          !HTMLEditUtils::IsContainerNode(*meaningfulFirstChild) ||
          !EditorUtils::IsEditableContent(*meaningfulFirstChild,
                                          EditorBase::EditorType::HTML)) {
        // FIXME: If the node is at middle of invisible white-spaces, we should
        // ignore the node.
        result.Set(meaningfulFirstChild);
        break;
      }
      result.Set(meaningfulFirstChild, 0u);
    }
    return result.To<EditorDOMPointType>();
  }

  /**
   * Return a point which points deepest editable last point of aContent.  This
   * walks the DOM tree in aContent to search meaningful last descendant.  If
   * EditablePointOption::IgnoreInvisibleText is specified, this returns next
   * offset of the last visible char if this reaches a visible `Text` first.  If
   * there is an empty inline element such as <span>, this returns end of the
   * inline element.  If this reaches non-editable element or non-container
   * element like <img>, this returns the position after that.
   */
  template <typename EditorDOMPointType>
  [[nodiscard]] static EditorDOMPointType GetDeepestEditableEndPointOf(
      const nsIContent& aContent, const EditablePointOptions& aOptions) {
    if (NS_WARN_IF(!EditorUtils::IsEditableContent(
            aContent, EditorBase::EditorType::HTML))) {
      return EditorDOMPointType();
    }
    const AutoEditablePointChecker checker(aOptions);
    auto result = EditorRawDOMPoint::AtEndOf(aContent);
    while (true) {
      nsIContent* lastChild = result.GetContainer()->GetLastChild();
      if (!lastChild) {
        break;
      }
      // If the caller wants to skip invisible white-spaces, we should skip
      // invisible text nodes.
      nsIContent* meaningfulLastChild = nullptr;
      // XXX Should we skip the lastChild if it's an invisible line break?
      if (checker.NodeShouldBeIgnored(*lastChild)) {
        for (nsIContent* nextSibling = lastChild->GetPreviousSibling();
             nextSibling; nextSibling = nextSibling->GetPreviousSibling()) {
          if (!checker.NodeShouldBeIgnored(*nextSibling) ||
              checker.ShouldStopScanningAt(*nextSibling)) {
            meaningfulLastChild = nextSibling;
            break;
          }
        }
        if (!meaningfulLastChild) {
          break;
        }
      } else {
        meaningfulLastChild = lastChild;
      }
      if (meaningfulLastChild->IsText()) {
        if (checker.IgnoreInvisibleWhiteSpaces()) {
          const Maybe<uint32_t> visibleCharOffset =
              HTMLEditUtils::GetPreviousNonCollapsibleCharOffset(
                  *meaningfulLastChild->AsText(),
                  meaningfulLastChild->AsText()->TextDataLength());
          if (visibleCharOffset.isNothing()) {
            result = EditorRawDOMPoint::AtEndOf(*meaningfulLastChild);
          } else {
            result.Set(meaningfulLastChild, visibleCharOffset.value() + 1u);
          }
        } else {
          result = EditorRawDOMPoint::AtEndOf(*meaningfulLastChild);
        }
        break;
      }
      if (checker.ShouldStopScanningAt(*meaningfulLastChild) ||
          !HTMLEditUtils::IsContainerNode(*meaningfulLastChild) ||
          !EditorUtils::IsEditableContent(*meaningfulLastChild,
                                          EditorBase::EditorType::HTML)) {
        // FIXME: If the node is at middle of invisible white-spaces, we should
        // ignore the node.
        result.SetAfter(meaningfulLastChild);
        break;
      }
      result = EditorRawDOMPoint::AtEndOf(*lastChild);
    }
    return result.To<EditorDOMPointType>();
  }

  /**
   * Get `#[0-9a-f]{6}` style HTML color value if aColorValue is valid value
   * for color-specifying attribute. The result is useful to set attributes
   * of HTML elements which take a color value.
   *
   * @param aColorValue         [in] Should be one of `#[0-9a-fA-Z]{3}`,
   *                            `#[0-9a-fA-Z]{3}` or a color name.
   * @param aNormalizedValue    [out] Set to `#[0-9a-f]{6}` style color code
   *                            if this returns true.  Otherwise, returns
   *                            aColorValue as-is.
   * @return                    true if aColorValue is valid.  Otherwise, false.
   */
  static bool GetNormalizedHTMLColorValue(const nsAString& aColorValue,
                                          nsAString& aNormalizedValue);

  /**
   * Return true if aColorValue may be a CSS specific color value or general
   * keywords of CSS.
   */
  [[nodiscard]] static bool MaybeCSSSpecificColorValue(
      const nsAString& aColorValue);

  /**
   * Return true if aColorValue can be specified to `color` value of <font>.
   */
  [[nodiscard]] static bool CanConvertToHTMLColorValue(
      const nsAString& aColorValue);

  /**
   * Convert aColorValue to `#[0-9a-f]{6}` style HTML color value.
   */
  static bool ConvertToNormalizedHTMLColorValue(const nsAString& aColorValue,
                                                nsAString& aNormalizedValue);

  /**
   * Get serialized color value (`rgb(...)` or `rgba(...)`) or "currentcolor"
   * if aColorValue is valid. The result is useful to set CSS color property.
   *
   * @param aColorValue         [in] Should be valid CSS color value.
   * @param aZeroAlphaColor     [in] If TransparentKeyword, aNormalizedValue is
   *                            set to "transparent" if the alpha value is 0.
   *                            Otherwise, `rgba(...)` value is set.
   * @param aNormalizedValue    [out] Serialized color value or "currentcolor".
   * @return                    true if aColorValue is valid.  Otherwise, false.
   */
  enum class ZeroAlphaColor { RGBAValue, TransparentKeyword };
  static bool GetNormalizedCSSColorValue(const nsAString& aColorValue,
                                         ZeroAlphaColor aZeroAlphaColor,
                                         nsAString& aNormalizedValue);

  /**
   * Check whether aColorA and aColorB are same color.
   *
   * @param aTransparentKeyword Whether allow to treat "transparent" keyword
   *                            as a valid value or an invalid value.
   * @return                    If aColorA and aColorB are valid values and
   *                            mean same color, returns true.
   */
  enum class TransparentKeyword { Invalid, Allowed };
  static bool IsSameHTMLColorValue(const nsAString& aColorA,
                                   const nsAString& aColorB,
                                   TransparentKeyword aTransparentKeyword);

  /**
   * Check whether aColorA and aColorB are same color.
   *
   * @return                    If aColorA and aColorB are valid values and
   *                            mean same color, returns true.
   */
  template <typename CharType>
  static bool IsSameCSSColorValue(const nsTSubstring<CharType>& aColorA,
                                  const nsTSubstring<CharType>& aColorB);

  /**
   * Return true if aColor is completely transparent.
   */
  [[nodiscard]] static bool IsTransparentCSSColor(const nsAString& aColor);

 private:
  static bool CanNodeContain(nsHTMLTag aParentTagId, nsHTMLTag aChildTagId);
  static bool IsContainerNode(nsHTMLTag aTagId);

  static bool CanCrossContentBoundary(nsIContent& aContent,
                                      TableBoundary aHowToTreatTableBoundary) {
    const bool cannotCrossBoundary =
        (aHowToTreatTableBoundary == TableBoundary::NoCrossAnyTableElement &&
         HTMLEditUtils::IsAnyTableElement(&aContent)) ||
        (aHowToTreatTableBoundary == TableBoundary::NoCrossTableElement &&
         aContent.IsHTMLElement(nsGkAtoms::table));
    return !cannotCrossBoundary;
  }

  static bool IsContentIgnored(const nsIContent& aContent,
                               const WalkTreeOptions& aOptions) {
    if (aOptions.contains(WalkTreeOption::IgnoreNonEditableNode) &&
        !EditorUtils::IsEditableContent(aContent,
                                        EditorUtils::EditorType::HTML)) {
      return true;
    }
    if (aOptions.contains(WalkTreeOption::IgnoreDataNodeExceptText) &&
        !EditorUtils::IsElementOrText(aContent)) {
      return true;
    }
    if (aOptions.contains(WalkTreeOption::IgnoreWhiteSpaceOnlyText) &&
        aContent.IsText() &&
        const_cast<Text*>(aContent.AsText())->TextIsOnlyWhitespace()) {
      return true;
    }
    return false;
  }

  static uint32_t CountChildren(const nsINode& aNode,
                                const WalkTreeOptions& aOptions,
                                BlockInlineCheck aBlockInlineCheck) {
    uint32_t count = 0;
    for (nsIContent* child = aNode.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      if (HTMLEditUtils::IsContentIgnored(*child, aOptions)) {
        continue;
      }
      if (aOptions.contains(WalkTreeOption::StopAtBlockBoundary) &&
          HTMLEditUtils::IsBlockElement(*child, aBlockInlineCheck)) {
        break;
      }
      ++count;
    }
    return count;
  }

  /**
   * Helper for GetPreviousContent() and GetNextContent().
   */
  static nsIContent* GetAdjacentLeafContent(
      const nsINode& aNode, WalkTreeDirection aWalkTreeDirection,
      const WalkTreeOptions& aOptions, BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr);
  static nsIContent* GetAdjacentContent(
      const nsINode& aNode, WalkTreeDirection aWalkTreeDirection,
      const WalkTreeOptions& aOptions, BlockInlineCheck aBlockInlineCheck,
      const Element* aAncestorLimiter = nullptr);

  /**
   * GetElementOfImmediateBlockBoundary() returns a block element if its
   * block boundary and aContent may be first visible thing before/after the
   * boundary.  And it may return a <br> element only when aContent is a
   * text node and follows a <br> element because only in this case, the
   * start white-spaces are invisible.  So the <br> element works same as
   * a block boundary.
   */
  static Element* GetElementOfImmediateBlockBoundary(
      const nsIContent& aContent, const WalkTreeDirection aDirection);
};

/**
 * DefinitionListItemScanner() scans given `<dl>` element's children.
 * Then, you can check whether `<dt>` and/or `<dd>` elements are in it.
 */
class MOZ_STACK_CLASS DefinitionListItemScanner final {
  using Element = dom::Element;

 public:
  DefinitionListItemScanner() = delete;
  explicit DefinitionListItemScanner(Element& aDLElement) {
    MOZ_ASSERT(aDLElement.IsHTMLElement(nsGkAtoms::dl));
    for (nsIContent* child = aDLElement.GetFirstChild(); child;
         child = child->GetNextSibling()) {
      if (child->IsHTMLElement(nsGkAtoms::dt)) {
        mDTFound = true;
        if (mDDFound) {
          break;
        }
        continue;
      }
      if (child->IsHTMLElement(nsGkAtoms::dd)) {
        mDDFound = true;
        if (mDTFound) {
          break;
        }
        continue;
      }
    }
  }

  bool DTElementFound() const { return mDTFound; }
  bool DDElementFound() const { return mDDFound; }

 private:
  bool mDTFound = false;
  bool mDDFound = false;
};

/**
 * SelectedTableCellScanner() scans all table cell elements which are selected
 * by each selection range.  Note that if 2nd or later ranges do not select
 * only one table cell element, the ranges are just ignored.
 */
class MOZ_STACK_CLASS SelectedTableCellScanner final {
  using Element = dom::Element;
  using Selection = dom::Selection;

 public:
  SelectedTableCellScanner() = delete;
  explicit SelectedTableCellScanner(const Selection& aSelection) {
    Element* firstSelectedCellElement =
        HTMLEditUtils::GetFirstSelectedTableCellElement(aSelection);
    if (!firstSelectedCellElement) {
      return;  // We're not in table cell selection mode.
    }
    mSelectedCellElements.SetCapacity(aSelection.RangeCount());
    mSelectedCellElements.AppendElement(*firstSelectedCellElement);
    const uint32_t rangeCount = aSelection.RangeCount();
    for (const uint32_t i : IntegerRange(1u, rangeCount)) {
      MOZ_ASSERT(aSelection.RangeCount() == rangeCount);
      nsRange* range = aSelection.GetRangeAt(i);
      if (MOZ_UNLIKELY(NS_WARN_IF(!range)) ||
          MOZ_UNLIKELY(NS_WARN_IF(!range->IsPositioned()))) {
        continue;  // Shouldn't occur in normal conditions.
      }
      // Just ignore selection ranges which do not select only one table
      // cell element.  This is possible case if web apps sets multiple
      // selections and first range selects a table cell element.
      if (Element* selectedCellElement =
              HTMLEditUtils::GetTableCellElementIfOnlyOneSelected(*range)) {
        mSelectedCellElements.AppendElement(*selectedCellElement);
      }
    }
  }

  explicit SelectedTableCellScanner(const AutoClonedRangeArray& aRanges);

  bool IsInTableCellSelectionMode() const {
    return !mSelectedCellElements.IsEmpty();
  }

  const nsTArray<OwningNonNull<Element>>& ElementsRef() const {
    return mSelectedCellElements;
  }

  /**
   * GetFirstElement() and GetNextElement() are stateful iterator methods.
   * This is useful to port legacy code which used old `nsITableEditor` API.
   */
  Element* GetFirstElement() const {
    MOZ_ASSERT(!mSelectedCellElements.IsEmpty());
    mIndex = 0;
    return !mSelectedCellElements.IsEmpty() ? mSelectedCellElements[0].get()
                                            : nullptr;
  }
  Element* GetNextElement() const {
    MOZ_ASSERT(mIndex < mSelectedCellElements.Length());
    return ++mIndex < mSelectedCellElements.Length()
               ? mSelectedCellElements[mIndex].get()
               : nullptr;
  }

 private:
  AutoTArray<OwningNonNull<Element>, 16> mSelectedCellElements;
  mutable size_t mIndex = 0;
};

}  // namespace mozilla

#endif  // #ifndef HTMLEditUtils_h
