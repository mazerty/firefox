/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* storage of the frame tree and information about it */

#include "nsFrameManager.h"

#include "ChildIterator.h"
#include "GeckoProfiler.h"
#include "mozilla/ComputedStyle.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PresShell.h"
#include "mozilla/PresState.h"
#include "mozilla/ViewportFrame.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "nsAbsoluteContainingBlock.h"
#include "nsCOMPtr.h"
#include "nsContainerFrame.h"
#include "nsError.h"
#include "nsGkAtoms.h"
#include "nsILayoutHistoryState.h"
#include "nsIStatefulFrame.h"
#include "nsPlaceholderFrame.h"
#include "nsWindowSizes.h"
#include "nscore.h"
#include "plhash.h"

using namespace mozilla;
using namespace mozilla::dom;

//----------------------------------------------------------------------

nsFrameManager::~nsFrameManager() {
  NS_ASSERTION(!mPresShell, "nsFrameManager::Destroy never called");
}

void nsFrameManager::SetRootFrame(ViewportFrame* aRootFrame) {
  MOZ_ASSERT(aRootFrame, "The root frame should be valid!");
  MOZ_ASSERT(!mRootFrame, "We should set a root frame only once!");
  mRootFrame = aRootFrame;
}

void nsFrameManager::Destroy() {
  NS_ASSERTION(mPresShell, "Frame manager already shut down.");

  // Destroy the frame hierarchy.
  mPresShell->SetIgnoreFrameDestruction(true);

  if (mRootFrame) {
    FrameDestroyContext context(mPresShell);
    mRootFrame->Destroy(context);
    mRootFrame = nullptr;
  }

  mPresShell = nullptr;
}

//----------------------------------------------------------------------
void nsFrameManager::AppendFrames(nsContainerFrame* aParentFrame,
                                  FrameChildListID aListID,
                                  nsFrameList&& aFrameList) {
  if (aParentFrame->IsAbsoluteContainer() &&
      aListID == aParentFrame->GetAbsoluteListID()) {
    aParentFrame->GetAbsoluteContainingBlock()->AppendFrames(
        aParentFrame, aListID, std::move(aFrameList));
  } else {
    aParentFrame->AppendFrames(aListID, std::move(aFrameList));
  }
}

void nsFrameManager::InsertFrames(nsContainerFrame* aParentFrame,
                                  FrameChildListID aListID,
                                  nsIFrame* aPrevFrame,
                                  nsFrameList&& aFrameList) {
  MOZ_ASSERT(
      !aPrevFrame ||
          (!aPrevFrame->GetNextContinuation() ||
           (aPrevFrame->GetNextContinuation()->HasAnyStateBits(
                NS_FRAME_IS_OVERFLOW_CONTAINER) &&
            !aPrevFrame->HasAnyStateBits(NS_FRAME_IS_OVERFLOW_CONTAINER))),
      "aPrevFrame must be the last continuation in its chain!");

  if (aParentFrame->IsAbsoluteContainer() &&
      aListID == aParentFrame->GetAbsoluteListID()) {
    aParentFrame->GetAbsoluteContainingBlock()->InsertFrames(
        aParentFrame, aListID, aPrevFrame, std::move(aFrameList));
  } else {
    aParentFrame->InsertFrames(aListID, aPrevFrame, nullptr,
                               std::move(aFrameList));
  }
}

void nsFrameManager::RemoveFrame(DestroyContext& aContext,
                                 FrameChildListID aListID,
                                 nsIFrame* aOldFrame) {
  // In case the reflow doesn't invalidate anything since it just leaves
  // a gap where the old frame was, we invalidate it here.  (This is
  // reasonably likely to happen when removing a last child in a way
  // that doesn't change the size of the parent.)
  // This has to sure to invalidate the entire overflow rect; this
  // is important in the presence of absolute positioning
  aOldFrame->InvalidateFrameForRemoval();

  NS_ASSERTION(!aOldFrame->GetPrevContinuation() ||
                   // exception for
                   // nsCSSFrameConstructor::RemoveFloatingFirstLetterFrames
                   aOldFrame->IsTextFrame(),
               "Must remove first continuation.");
  NS_ASSERTION(!(aOldFrame->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW) &&
                 aOldFrame->GetPlaceholderFrame()),
               "Must call RemoveFrame on placeholder for out-of-flows.");
  nsContainerFrame* parentFrame = aOldFrame->GetParent();
  if (parentFrame->IsAbsoluteContainer() &&
      aListID == parentFrame->GetAbsoluteListID()) {
    parentFrame->GetAbsoluteContainingBlock()->RemoveFrame(aContext, aListID,
                                                           aOldFrame);
  } else {
    parentFrame->RemoveFrame(aContext, aListID, aOldFrame);
  }
}

//----------------------------------------------------------------------

// Capture state for a given frame.
// Accept a content id here, in some cases we may not have content (scroll
// position)
void nsFrameManager::CaptureFrameStateFor(nsIFrame* aFrame,
                                          nsILayoutHistoryState* aState) {
  if (!aFrame || !aState) {
    NS_WARNING("null frame, or state");
    return;
  }

  // Only capture state for stateful frames
  nsIStatefulFrame* statefulFrame = do_QueryFrame(aFrame);
  if (!statefulFrame) {
    return;
  }

  // Capture the state, exit early if we get null (nothing to save)
  UniquePtr<PresState> frameState = statefulFrame->SaveState();
  if (!frameState) {
    return;
  }

  // Generate the hash key to store the state under
  // Exit early if we get empty key
  nsAutoCString stateKey;
  nsIContent* content = aFrame->GetContent();
  Document* doc = content ? content->GetUncomposedDoc() : nullptr;
  statefulFrame->GenerateStateKey(content, doc, stateKey);
  if (stateKey.IsEmpty()) {
    return;
  }

  // Store the state. aState owns frameState now.
  aState->AddState(stateKey, std::move(frameState));
}

void nsFrameManager::CaptureFrameState(nsIFrame* aFrame,
                                       nsILayoutHistoryState* aState) {
  MOZ_ASSERT(nullptr != aFrame && nullptr != aState,
             "null parameters passed in");

  CaptureFrameStateFor(aFrame, aState);

  // Now capture state recursively for the frame hierarchy rooted at aFrame
  for (const auto& childList : aFrame->ChildLists()) {
    for (nsIFrame* child : childList.mList) {
      if (child->HasAnyStateBits(NS_FRAME_OUT_OF_FLOW)) {
        // We'll pick it up when we get to its placeholder
        continue;
      }
      // Make sure to walk through placeholders as needed, so that we
      // save state for out-of-flows which may not be our descendants
      // themselves but whose placeholders are our descendants.
      nsIFrame* realChild = nsPlaceholderFrame::GetRealFrameFor(child);
      // GetRealFrameFor should theoretically never return null here (and its
      // helper has an assertion to enforce this); but we've got known fuzzer
      // testcases where it does return null (in non-debug builds that make it
      // past the aforementioned assertion) due to weird situations with
      // out-of-flows and fragmentation. We handle that unexpected situation by
      // silently skipping this frame, rather than crashing.
      if (MOZ_LIKELY(realChild)) {
        CaptureFrameState(realChild, aState);
      }
    }
  }
}

// Restore state for a given frame.
// Accept a content id here, in some cases we may not have content (scroll
// position)
void nsFrameManager::RestoreFrameStateFor(nsIFrame* aFrame,
                                          nsILayoutHistoryState* aState) {
  if (!aFrame || !aState) {
    NS_WARNING("null frame or state");
    return;
  }

  // Only restore state for stateful frames
  nsIStatefulFrame* statefulFrame = do_QueryFrame(aFrame);
  if (!statefulFrame) {
    return;
  }

  // Generate the hash key the state was stored under
  // Exit early if we get empty key
  nsIContent* content = aFrame->GetContent();
  // If we don't have content, we can't generate a hash
  // key and there's probably no state information for us.
  if (!content) {
    return;
  }

  nsAutoCString stateKey;
  Document* doc = content->GetUncomposedDoc();
  statefulFrame->GenerateStateKey(content, doc, stateKey);
  if (stateKey.IsEmpty()) {
    return;
  }

  // Get the state from the hash
  PresState* frameState = aState->GetState(stateKey);
  if (!frameState) {
    return;
  }

  // Restore it
  nsresult rv = statefulFrame->RestoreState(frameState);
  if (NS_FAILED(rv)) {
    return;
  }

  // If we restore ok, remove the state from the state table
  aState->RemoveState(stateKey);
}

void nsFrameManager::RestoreFrameState(nsIFrame* aFrame,
                                       nsILayoutHistoryState* aState) {
  MOZ_ASSERT(nullptr != aFrame && nullptr != aState,
             "null parameters passed in");

  RestoreFrameStateFor(aFrame, aState);

  // Now restore state recursively for the frame hierarchy rooted at aFrame
  for (const auto& childList : aFrame->ChildLists()) {
    for (nsIFrame* child : childList.mList) {
      RestoreFrameState(child, aState);
    }
  }
}

void nsFrameManager::AddSizeOfIncludingThis(nsWindowSizes& aSizes) const {
  aSizes.mLayoutPresShellSize += aSizes.mState.mMallocSizeOf(this);
}
