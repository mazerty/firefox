/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

/**
 * Redux actions for the sources state
 * @module actions/sources
 */
import { prettyPrintSource, prettyPrintAndSelectSource } from "./prettyPrint";
import { addTab, closeTab } from "../tabs";
import { loadSourceText } from "./loadSourceText";
import { setBreakableLines } from "./breakableLines";

import { prefs } from "../../utils/prefs";
import { isMinified } from "../../utils/source";
import { createLocation } from "../../utils/location";
import {
  getRelatedMapLocation,
  getOriginalLocation,
} from "../../utils/source-maps";

import {
  getSource,
  getFirstSourceActorForGeneratedSource,
  getSourceByURL,
  getPrettySource,
  getSelectedLocation,
  getShouldSelectOriginalLocation,
  canPrettyPrintSource,
  getSourceTextContent,
  tabExists,
  hasSource,
  hasSourceActor,
  hasPrettyTab,
  isSourceActorWithSourceMap,
} from "../../selectors/index";

// This is only used by jest tests (and within this module)
export const setSelectedLocation = (
  location,
  shouldSelectOriginalLocation,
  shouldHighlightSelectedLocation,
  shouldScrollToSelectedLocation
) => ({
  type: "SET_SELECTED_LOCATION",
  location,
  shouldSelectOriginalLocation,
  shouldHighlightSelectedLocation,
  shouldScrollToSelectedLocation,
});

// This is only used by jest tests (and within this module)
export const setPendingSelectedLocation = (url, options) => ({
  type: "SET_PENDING_SELECTED_LOCATION",
  url,
  line: options?.line,
  column: options?.column,
});

// This is only used by jest tests (and within this module)
export const clearSelectedLocation = () => ({
  type: "CLEAR_SELECTED_LOCATION",
});

export const setDefaultSelectedLocation = shouldSelectOriginalLocation => ({
  type: "SET_DEFAULT_SELECTED_LOCATION",
  shouldSelectOriginalLocation,
});

/**
 * Deterministically select a source that has a given URL. This will
 * work regardless of the connection status or if the source exists
 * yet.
 *
 * This exists mostly for external things to interact with the
 * debugger.
 */
export function selectSourceURL(url, options) {
  return async ({ dispatch, getState }) => {
    const source = getSourceByURL(getState(), url);
    if (!source) {
      return dispatch(setPendingSelectedLocation(url, options));
    }

    const location = createLocation({ ...options, source });
    return dispatch(selectLocation(location));
  };
}

/**
 * Function dedicated to the Source Tree.
 *
 * This would automatically select the pretty printed source
 * if one exists for the passed source.
 *
 * We aren't relying on selectLocation's mayBeSelectMappedSource logic
 * as the (0,0) location (line 0, column 0) may not be mapped
 * and wouldn't be resolved to the pretty printed source.
 */
export function selectMayBePrettyPrintedLocation(location) {
  return async ({ dispatch, getState }) => {
    const prettySource = getPrettySource(getState(), location.source.id);
    if (prettySource) {
      location = createLocation({ source: prettySource });
    }
    await dispatch(selectSpecificLocation(location));
  };
}

/**
 * Wrapper around selectLocation, which creates the location object for us.
 * Note that it ignores the currently selected source and will select
 * the precise generated/original source passed as argument.
 *
 * @param {String} source
 *        The precise source to select.
 * @param {String} sourceActor
 *        The specific source actor of the source to
 *        select the source text. This is optional.
 */
export function selectSource(source, sourceActor) {
  return async ({ dispatch }) => {
    // `createLocation` requires a source object, but we may use selectSource to close the last tab,
    // where source will be null and the location will be an empty object.
    const location = source ? createLocation({ source, sourceActor }) : {};

    return dispatch(selectSpecificLocation(location));
  };
}

/**
 * Helper for `selectLocation`.
 * Based on `keepContext` argument passed to `selectLocation`,
 * this will automatically select the related mapped source (original or generated).
 *
 * @param {Object} location
 *        The location to select.
 * @param {Boolean} keepContext
 *        If true, will try to select a mapped source.
 * @param {Object} thunkArgs
 * @return {Object}
 *        Object with two attributes:
 *         - `shouldSelectOriginalLocation`, to know if we should keep trying to select the original location
 *         - `newLocation`, for the final location to select
 */
async function mayBeSelectMappedSource(location, keepContext, thunkArgs) {
  const { getState, dispatch } = thunkArgs;

  // Preserve the current source map context (original / generated)
  // when navigating to a new location.
  // i.e. if keepContext isn't manually overriden to false,
  // we will convert the source we want to select to either
  // original/generated in order to match the currently selected one.
  // If the currently selected source is original, we will
  // automatically map `location` to refer to the original source,
  // even if that used to refer only to the generated source.
  let shouldSelectOriginalLocation =
    getShouldSelectOriginalLocation(getState());

  // Pretty print source may not be registered yet and getRelatedMapLocation may not return it.
  // Wait for the pretty print source to be fully processed.
  //
  // In this case we don't follow the "should select original location",
  // we solely follow user decision to have pretty printed the source.
  const sourceHasPrettyTab = hasPrettyTab(getState(), location.source);
  if (!location.source.isOriginal && sourceHasPrettyTab) {
    // Note that prettyPrintSource has already been called a bit before when this generated source has been added
    // but it is a slow operation and is most likely not resolved yet.
    // prettyPrintSource uses memoization to avoid doing the operation more than once, while waiting from both callsites.
    const prettyPrintedSource = await dispatch(
      prettyPrintSource(location.source)
    );
    // If we aren't selecting a particular location line will be 0 and column be undefined,
    // avoid calling getRelatedMapLocation which may not map to any original location.
    if (location.line == 0 && !location.column) {
      return {
        shouldSelectOriginalLocation,
        newLocation: createLocation({
          ...location,
          source: prettyPrintedSource,
          line: 1,
          column: 0,
        }),
      };
    }
    location = await getRelatedMapLocation(location, thunkArgs);
    return { shouldSelectOriginalLocation, newLocation: location };
  }

  if (keepContext) {
    if (shouldSelectOriginalLocation != location.source.isOriginal) {
      // Only try to map the location if the source is mapped:
      // - mapping from original to generated, if this is original source
      // - mapping from generated to original, if the generated source has a source map URL comment
      // - mapping from compressed to pretty print, if the compressed source has a matching pretty print tab opened
      if (
        location.source.isOriginal ||
        isSourceActorWithSourceMap(getState(), location.sourceActor.id) ||
        sourceHasPrettyTab
      ) {
        // getRelatedMapLocation will convert to the related generated/original location.
        // i.e if the original location is passed, the related generated location will be returned and vice versa.
        location = await getRelatedMapLocation(location, thunkArgs);
      }
      // Note that getRelatedMapLocation may return the exact same location.
      // For example, if the source-map is half broken, it may return a generated location
      // while we were selecting original locations. So we may be seeing bundles intermittently
      // when stepping through broken source maps. And we will see original sources when stepping
      // through functional original sources.
    }
  } else if (
    location.source.isOriginal ||
    isSourceActorWithSourceMap(getState(), location.sourceActor.id)
  ) {
    // Only update this setting if the source is mapped. i.e. don't update if we select a regular source.
    // The source is mapped when it is either:
    //  - an original source,
    //  - a bundle with a source map comment referencing a source map URL.
    shouldSelectOriginalLocation = location.source.isOriginal;
  }
  return { shouldSelectOriginalLocation, newLocation: location };
}

/**
 * Select a new location.
 * This will automatically select the source in the source tree (if visible)
 * and open the source (a new tab and the source editor)
 * as well as highlight a precise line in the editor.
 *
 * Note that by default, this may map your passed location to the original
 * or generated location based on the selected source state. (see keepContext)
 *
 * @param {Object} location
 * @param {Object} options
 * @param {boolean} options.keepContext
 *        If false, this will ignore the currently selected source
 *        and select the generated or original location, even if we
 *        were currently selecting the other source type.
 * @param {boolean} options.highlight
 *        True by default. To be set to false in order to preveng highlighting the selected location in the editor.
 *        We will only show the location, but do not put a special background on the line.
 * @param {boolean} options.scroll
 *        True by default. Is set to false to stop the editor from scrolling to the location that has been selected.
 *        e.g is when clicking in the editor to just show the selected line / column in the footer
 */
export function selectLocation(
  location,
  { keepContext = true, highlight = true, scroll = true } = {}
) {
  // eslint-disable-next-line complexity
  return async thunkArgs => {
    const { dispatch, getState, client } = thunkArgs;

    if (!client) {
      // No connection, do nothing. This happens when the debugger is
      // shut down too fast and it tries to display a default source.
      return;
    }

    let source = location.source;

    if (!source) {
      // If there is no source we deselect the current selected source
      dispatch(clearSelectedLocation());
      return;
    }

    let sourceActor = location.sourceActor;
    if (!sourceActor) {
      sourceActor = getFirstSourceActorForGeneratedSource(
        getState(),
        source.id
      );
      location = createLocation({ ...location, sourceActor });
    }

    const lastSelectedLocation = getSelectedLocation(getState());
    const { shouldSelectOriginalLocation, newLocation } =
      await mayBeSelectMappedSource(location, keepContext, thunkArgs);

    // Ignore the request if another location was selected while we were waiting for mayBeSelectMappedSource async completion
    if (getSelectedLocation(getState()) != lastSelectedLocation) {
      return;
    }

    // Update all local variables after mapping
    location = newLocation;
    source = location.source;
    sourceActor = location.sourceActor;
    if (!sourceActor) {
      sourceActor = getFirstSourceActorForGeneratedSource(
        getState(),
        source.id
      );
      location = createLocation({ ...location, sourceActor });
    }

    if (!tabExists(getState(), source)) {
      dispatch(addTab(source, sourceActor));
    }
    dispatch(
      setSelectedLocation(
        location,
        shouldSelectOriginalLocation,
        highlight,
        scroll
      )
    );

    await dispatch(loadSourceText(source, sourceActor));

    // Stop the async work if we started selecting another location
    if (getSelectedLocation(getState()) != location) {
      return;
    }

    await dispatch(setBreakableLines(location));

    // Stop the async work if we started selecting another location
    if (getSelectedLocation(getState()) != location) {
      return;
    }

    const loadedSource = getSource(getState(), source.id);

    if (!loadedSource) {
      // If there was a navigation while we were loading the loadedSource
      return;
    }

    const sourceTextContent = getSourceTextContent(getState(), location);

    if (
      keepContext &&
      prefs.autoPrettyPrint &&
      !getPrettySource(getState(), loadedSource.id) &&
      canPrettyPrintSource(getState(), location) &&
      isMinified(source, sourceTextContent)
    ) {
      await dispatch(prettyPrintAndSelectSource(loadedSource));
      dispatch(closeTab(loadedSource));
    }

    // When we select a generated source which has a sourcemap,
    // asynchronously fetch the related original location in order to display
    // the mapped location in the editor's footer.
    if (
      !location.source.isOriginal &&
      isSourceActorWithSourceMap(getState(), sourceActor.id)
    ) {
      let originalLocation = await getOriginalLocation(location, thunkArgs, {
        looseSearch: true,
      });
      // We pass a null original location when the location doesn't map
      // in order to know when we are done processing the source map.
      // * `getOriginalLocation` would return the exact same location if it doesn't map
      // * `getOriginalLocation` may also return a distinct location object,
      //   but refering to the same `source` object (which is the bundle) when it doesn't
      //   map to any known original location.
      if (originalLocation.source === location.source) {
        originalLocation = null;
      }
      dispatch({
        type: "SET_ORIGINAL_SELECTED_LOCATION",
        location,
        originalLocation,
      });
    }
  };
}

/**
 * Select a location while ignoring the currently selected source.
 * This will select the generated location even if the currently
 * select source is an original source. And the other way around.
 *
 * @param {Object} location
 *        The location to select, object which includes enough
 *        information to specify a precise source, line and column.
 */
export function selectSpecificLocation(location) {
  return selectLocation(location, { keepContext: false });
}

/**
 * Similar to `selectSpecificLocation`, but if the precise Source object
 * is missing, this will fallback to select any source having the same URL.
 * In this fallback scenario, sources without a URL will be ignored.
 *
 * This is typically used when trying to select a source (e.g. in project search result)
 * after reload, because the source objects are new on each new page load, but source
 * with the same URL may still exist.
 *
 * @param {Object} location
 *        The location to select.
 * @return {function}
 *        The action will return true if a matching source was found.
 */
export function selectSpecificLocationOrSameUrl(location) {
  return async ({ dispatch, getState }) => {
    // If this particular source no longer exists, open any matching URL.
    // This will typically happen on reload.
    if (!hasSource(getState(), location.source.id)) {
      // Some sources, like evaled script won't have a URL attribute
      // and can't be re-selected if we don't find the exact same source object.
      if (!location.source.url) {
        return false;
      }
      const source = getSourceByURL(getState(), location.source.url);
      if (!source) {
        return false;
      }
      // Also reset the sourceActor, as it won't match the same source.
      const sourceActor = getFirstSourceActorForGeneratedSource(
        getState(),
        location.source.id
      );
      location = createLocation({ ...location, source, sourceActor });
    } else if (!hasSourceActor(getState(), location.sourceActor.id)) {
      // If the specific source actor no longer exists, match any still available.
      const sourceActor = getFirstSourceActorForGeneratedSource(
        getState(),
        location.source.id
      );
      location = createLocation({ ...location, sourceActor });
    }
    await dispatch(selectSpecificLocation(location));
    return true;
  };
}

/**
 * Select the "mapped location".
 *
 * If the passed location is on a generated source, select the
 * related location in the original source.
 * If the passed location is on an original source, select the
 * related location in the generated source.
 */
export function jumpToMappedLocation(location) {
  return async function (thunkArgs) {
    const { client, dispatch } = thunkArgs;
    if (!client) {
      return null;
    }

    // Map to either an original or a generated source location
    const pairedLocation = await getRelatedMapLocation(location, thunkArgs);

    // If we are on a non-mapped source, this will return the same location
    // so ignore the request.
    if (pairedLocation == location) {
      return null;
    }

    return dispatch(selectSpecificLocation(pairedLocation));
  };
}

export function jumpToMappedSelectedLocation() {
  return async function ({ dispatch, getState }) {
    const location = getSelectedLocation(getState());
    if (!location) {
      return;
    }

    await dispatch(jumpToMappedLocation(location));
  };
}
