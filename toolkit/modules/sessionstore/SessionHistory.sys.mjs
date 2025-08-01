/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  E10SUtils: "resource://gre/modules/E10SUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "PolicyContainer", () => {
  return Components.Constructor(
    "@mozilla.org/policycontainer;1",
    "nsIPolicyContainer",
    "initFromCSP"
  );
});

/**
 * The external API exported by this module.
 */
export var SessionHistory = Object.freeze({
  isEmpty(docShell) {
    return SessionHistoryInternal.isEmpty(docShell);
  },

  collect(docShell, aFromIdx = -1) {
    if (Services.appinfo.sessionHistoryInParent) {
      throw new Error("Use SessionHistory.collectFromParent instead");
    }
    return SessionHistoryInternal.collect(docShell, aFromIdx);
  },

  collectFromParent(uri, documentHasChildNodes, history, aFromIdx = -1) {
    return SessionHistoryInternal.collectCommon(
      uri,
      documentHasChildNodes,
      history,
      aFromIdx
    );
  },

  collectNonWebControlledBlankLoadingSession(browsingContext) {
    return SessionHistoryInternal.collectNonWebControlledBlankLoadingSession(
      browsingContext
    );
  },

  restore(docShell, tabData) {
    if (Services.appinfo.sessionHistoryInParent) {
      throw new Error("Use SessionHistory.restoreFromParent instead");
    }
    return SessionHistoryInternal.restore(
      docShell.QueryInterface(Ci.nsIWebNavigation).sessionHistory
        .legacySHistory,
      tabData
    );
  },

  restoreFromParent(history, tabData) {
    return SessionHistoryInternal.restore(history, tabData);
  },
});

/**
 * The internal API for the SessionHistory module.
 */
var SessionHistoryInternal = {
  /**
   * Mapping from legacy docshellIDs to docshellUUIDs.
   */
  _docshellUUIDMap: new Map(),

  /**
   * Returns whether the given docShell's session history is empty.
   *
   * @param docShell
   *        The docShell that owns the session history.
   */
  isEmpty(docShell) {
    let webNavigation = docShell.QueryInterface(Ci.nsIWebNavigation);
    let history = webNavigation.sessionHistory;
    if (!webNavigation.currentURI) {
      return true;
    }
    let uri = webNavigation.currentURI.spec;
    return uri == "about:blank" && history.count == 0;
  },

  /**
   * Collects session history data for a given docShell.
   *
   * @param docShell
   *        The docShell that owns the session history.
   * @param aFromIdx
   *        The starting local index to collect the history from.
   * @return An object reprereseting a partial global history update.
   */
  collect(docShell, aFromIdx = -1) {
    let webNavigation = docShell.QueryInterface(Ci.nsIWebNavigation);
    let uri = webNavigation.currentURI.displaySpec;
    let body = webNavigation.document.body;
    let history = webNavigation.sessionHistory;
    return this.collectCommon(
      uri,
      body && body.hasChildNodes(),
      history.legacySHistory,
      aFromIdx
    );
  },

  collectCommon(uri, documentHasChildNodes, shistory, aFromIdx) {
    let data = {
      entries: [],
      requestedIndex: shistory.requestedIndex + 1,
    };

    // We want to keep track how many entries we *could* have collected and
    // how many we skipped, so we can sanitiy-check the current history index
    // and also determine whether we need to get any fallback data or not.
    let skippedCount = 0,
      entryCount = 0;

    if (shistory && shistory.count > 0) {
      let count = shistory.count;
      for (; entryCount < count; entryCount++) {
        let shEntry = shistory.getEntryAtIndex(entryCount);
        if (entryCount <= aFromIdx) {
          skippedCount++;
          continue;
        }
        let entry = this.serializeEntry(shEntry);
        data.entries.push(entry);
      }

      // Ensure the index isn't out of bounds if an exception was thrown above.
      data.index = Math.min(shistory.index + 1, entryCount);
    }

    // If either the session history isn't available yet or doesn't have any
    // valid entries, make sure we at least include the current page,
    // unless of course we just skipped all entries because aFromIdx was big enough.
    if (!data.entries.length && (skippedCount != entryCount || aFromIdx < 0)) {
      // We landed here because the history is inaccessible or there are no
      // history entries. In that case we should at least record the docShell's
      // current URL as a single history entry. If the URL is not about:blank
      // or it's a blank tab that was modified (like a custom newtab page),
      // record it. For about:blank we explicitly want an empty array without
      // an 'index' property to denote that there are no history entries.
      if (uri != "about:blank" || documentHasChildNodes) {
        data.entries.push({
          url: uri,
          triggeringPrincipal_base64: lazy.E10SUtils.SERIALIZED_SYSTEMPRINCIPAL,
        });
        data.index = 1;
      }
    }

    data.fromIdx = aFromIdx;

    return data;
  },

  collectNonWebControlledBlankLoadingSession(browsingContext) {
    if (
      browsingContext.sessionHistory?.count === 0 &&
      browsingContext.nonWebControlledBlankURI &&
      browsingContext.mostRecentLoadingSessionHistoryEntry
    ) {
      return {
        entries: [
          this.serializeEntry(
            browsingContext.mostRecentLoadingSessionHistoryEntry
          ),
        ],
        // Set 1 to the index, as the array of session entries is 1-based.
        index: 1,
        fromIdx: -1,
        requestedIndex: browsingContext.sessionHistory.requestedIndex + 1,
      };
    }

    return null;
  },

  /**
   * Get an object that is a serialized representation of a History entry.
   *
   * @param shEntry
   *        nsISHEntry instance
   * @return object
   */
  serializeEntry(shEntry) {
    let entry = { url: shEntry.URI.displaySpec, title: shEntry.title };

    if (shEntry.isSubFrame) {
      entry.subframe = true;
    }

    entry.cacheKey = shEntry.cacheKey;
    entry.ID = shEntry.ID;
    entry.docshellUUID = shEntry.docshellID.toString();

    // We will include the property only if it's truthy to save a couple of
    // bytes when the resulting object is stringified and saved to disk.
    if (shEntry.referrerInfo) {
      entry.referrerInfo = lazy.E10SUtils.serializeReferrerInfo(
        shEntry.referrerInfo
      );
    }

    if (shEntry.originalURI) {
      entry.originalURI = shEntry.originalURI.spec;
    }

    if (shEntry.resultPrincipalURI) {
      entry.resultPrincipalURI = shEntry.resultPrincipalURI.spec;

      // For downgrade compatibility we store the loadReplace property as it
      // would be stored before result principal URI introduction so that
      // the old code can still create URL based principals for channels
      // correctly.  When resultPrincipalURI is non-null and not equal to
      // channel's orignalURI in the new code, it's equal to setting
      // LOAD_REPLACE in the old code.  Note that we only do 'the best we can'
      // here to derivate the 'old' loadReplace flag value.
      entry.loadReplace = entry.resultPrincipalURI != entry.originalURI;
    } else {
      // We want to store the property to let the backward compatibility code,
      // when reading the stored session, work. When this property is undefined
      // that code will derive the result principal URI from the load replace
      // flag.
      entry.resultPrincipalURI = null;
    }

    if (shEntry.loadReplace) {
      // Storing under a new property name, since it has changed its meaning
      // with the result principal URI introduction.
      entry.loadReplace2 = shEntry.loadReplace;
    }

    if (shEntry.isSrcdocEntry) {
      entry.srcdocData = shEntry.srcdocData;
      entry.isSrcdocEntry = shEntry.isSrcdocEntry;
    }

    if (shEntry.baseURI) {
      entry.baseURI = shEntry.baseURI.spec;
    }

    if (shEntry.contentType) {
      entry.contentType = shEntry.contentType;
    }

    if (shEntry.scrollRestorationIsManual) {
      entry.scrollRestorationIsManual = true;
    } else {
      let x = {},
        y = {};
      shEntry.getScrollPosition(x, y);
      if (x.value !== 0 || y.value !== 0) {
        entry.scroll = x.value + "," + y.value;
      }

      let layoutHistoryState = shEntry.layoutHistoryState;
      if (layoutHistoryState && layoutHistoryState.hasStates) {
        let presStates = layoutHistoryState
          .getKeys()
          .map(key => this._getSerializablePresState(layoutHistoryState, key))
          .filter(
            presState =>
              // Only keep presState entries that contain more than the key itself.
              Object.getOwnPropertyNames(presState).length > 1
          );

        if (presStates.length) {
          entry.presState = presStates;
        }
      }
    }

    // Collect triggeringPrincipal data for the current history entry.
    if (shEntry.principalToInherit) {
      entry.principalToInherit_base64 = lazy.E10SUtils.serializePrincipal(
        shEntry.principalToInherit
      );
    }

    entry.hasUserInteraction = shEntry.hasUserInteraction;

    if (shEntry.triggeringPrincipal) {
      entry.triggeringPrincipal_base64 = lazy.E10SUtils.serializePrincipal(
        shEntry.triggeringPrincipal
      );
    }

    if (shEntry.policyContainer) {
      entry.policyContainer = lazy.E10SUtils.serializePolicyContainer(
        shEntry.policyContainer
      );
    }

    entry.docIdentifier = shEntry.bfcacheID;

    if (shEntry.stateData != null) {
      let stateData = shEntry.stateData;
      entry.structuredCloneState = stateData.getDataAsBase64();
      entry.structuredCloneVersion = stateData.formatVersion;
    }

    if (shEntry.wireframe != null) {
      entry.wireframe = shEntry.wireframe;
    }

    if (shEntry.childCount > 0 && !shEntry.hasDynamicallyAddedChild()) {
      let children = [];
      for (let i = 0; i < shEntry.childCount; i++) {
        let child = shEntry.GetChildAt(i);

        if (child) {
          children.push(this.serializeEntry(child));
        }
      }

      if (children.length) {
        entry.children = children;
      }
    }

    entry.transient = shEntry.isTransient();

    return entry;
  },

  /**
   * Get an object that is a serializable representation of a PresState.
   *
   * @param layoutHistoryState
   *        nsILayoutHistoryState instance
   * @param stateKey
   *        The state key of the presState to be retrieved.
   * @return object
   */
  _getSerializablePresState(layoutHistoryState, stateKey) {
    let presState = { stateKey };
    let x = {},
      y = {},
      scrollOriginDowngrade = {},
      res = {};

    layoutHistoryState.getPresState(stateKey, x, y, scrollOriginDowngrade, res);
    if (x.value !== 0 || y.value !== 0) {
      presState.scroll = x.value + "," + y.value;
    }
    if (scrollOriginDowngrade.value === false) {
      presState.scrollOriginDowngrade = scrollOriginDowngrade.value;
    }
    if (res.value != 1.0) {
      presState.res = res.value;
    }

    return presState;
  },

  /**
   * Restores session history data for a given docShell.
   *
   * @param history
   *        The session history object.
   * @param tabData
   *        The tabdata including all history entries.
   * @return A reference to the docShell's nsISHistory interface.
   */
  restore(history, tabData) {
    if (history.count > 0) {
      history.purgeHistory(history.count);
    }

    let idMap = { used: {} };
    let docIdentMap = {};
    for (let i = 0; i < tabData.entries.length; i++) {
      let entry = tabData.entries[i];
      // XXXzpao Wallpaper patch for bug 514751
      if (!entry.url) {
        continue;
      }
      let shEntry = this.deserializeEntry(entry, idMap, docIdentMap, history);

      // To enable a smooth migration, we treat values of null/undefined as having
      // user interaction (because we don't want to hide all session history that was
      // added before we started recording user interaction).
      //
      // This attribute is only set on top-level SH history entries, so we set it
      // outside of deserializeEntry since that is called recursively.
      if (entry.hasUserInteraction == undefined) {
        shEntry.hasUserInteraction = true;
      } else {
        shEntry.hasUserInteraction = entry.hasUserInteraction;
      }

      history.addEntry(shEntry);
    }

    // Select the right history entry.
    let index = tabData.index - 1;
    if (index < history.count && history.index != index) {
      history.index = index;
    }
    return history;
  },

  /**
   * Expands serialized history data into a session-history-entry instance.
   *
   * @param entry
   *        Object containing serialized history data for a URL
   * @param idMap
   *        Hash for ensuring unique frame IDs
   * @param docIdentMap
   *        Hash to ensure reuse of BFCache entries
   * @returns nsISHEntry
   */
  deserializeEntry(entry, idMap, docIdentMap, shistory) {
    var shEntry = shistory.createEntry();

    shEntry.URI = Services.io.newURI(entry.url);
    shEntry.title = entry.title || entry.url;
    if (entry.subframe) {
      shEntry.isSubFrame = entry.subframe || false;
    }
    shEntry.setLoadTypeAsHistory();
    if (entry.contentType) {
      shEntry.contentType = entry.contentType;
    }
    // Referrer information is now stored as a referrerInfo property. We should
    // also cope with the old format of passing `referrer` and `referrerPolicy`
    // separately.
    if (entry.referrerInfo) {
      shEntry.referrerInfo = lazy.E10SUtils.deserializeReferrerInfo(
        entry.referrerInfo
      );
    } else if (entry.referrer) {
      let ReferrerInfo = Components.Constructor(
        "@mozilla.org/referrer-info;1",
        "nsIReferrerInfo",
        "init"
      );
      shEntry.referrerInfo = new ReferrerInfo(
        entry.referrerPolicy,
        true,
        Services.io.newURI(entry.referrer)
      );
    }

    if (entry.originalURI) {
      shEntry.originalURI = Services.io.newURI(entry.originalURI);
    }
    if (typeof entry.resultPrincipalURI === "undefined" && entry.loadReplace) {
      // This is backward compatibility code for stored sessions saved prior to
      // introduction of the resultPrincipalURI property.  The equivalent of this
      // property non-null value used to be the URL while the LOAD_REPLACE flag
      // was set.
      shEntry.resultPrincipalURI = shEntry.URI;
    } else if (entry.resultPrincipalURI) {
      shEntry.resultPrincipalURI = Services.io.newURI(entry.resultPrincipalURI);
    }
    if (entry.loadReplace2) {
      shEntry.loadReplace = entry.loadReplace2;
    }
    if (entry.isSrcdocEntry) {
      shEntry.srcdocData = entry.srcdocData;
    }
    if (entry.baseURI) {
      shEntry.baseURI = Services.io.newURI(entry.baseURI);
    }

    if (entry.cacheKey) {
      shEntry.cacheKey = entry.cacheKey;
    }

    // The persist attribute was replaced by SetTransient() and IsTransient().
    // But existing session storage might contain entries with the persist attribute,
    // so we translate them to transient.
    // Bug 1971274 tracks removing this.
    if ("persist" in entry) {
      entry.transient = !entry.persist;
    }

    if (entry.transient) {
      shEntry.setTransient();
    }

    if (entry.ID) {
      // get a new unique ID for this frame (since the one from the last
      // start might already be in use)
      var id = idMap[entry.ID] || 0;
      if (!id) {
        // eslint-disable-next-line no-empty
        for (id = Date.now(); id in idMap.used; id++) {}
        idMap[entry.ID] = id;
        idMap.used[id] = true;
      }
      shEntry.ID = id;
    }

    // If we have the legacy docshellID on our entry, upgrade it to a
    // docshellUUID by going through the mapping.
    if (entry.docshellID) {
      if (!this._docshellUUIDMap.has(entry.docshellID)) {
        // Convert the nsID to a string so that the docshellUUID property
        // is correctly stored as a string.
        this._docshellUUIDMap.set(
          entry.docshellID,
          Services.uuid.generateUUID().toString()
        );
      }
      entry.docshellUUID = this._docshellUUIDMap.get(entry.docshellID);
      delete entry.docshellID;
    }

    if (entry.docshellUUID) {
      shEntry.docshellID = Components.ID(entry.docshellUUID);
    }

    if (entry.structuredCloneState && entry.structuredCloneVersion) {
      var stateData = Cc[
        "@mozilla.org/docshell/structured-clone-container;1"
      ].createInstance(Ci.nsIStructuredCloneContainer);

      stateData.initFromBase64(
        entry.structuredCloneState,
        entry.structuredCloneVersion
      );
      shEntry.stateData = stateData;
    }

    if (entry.scrollRestorationIsManual) {
      shEntry.scrollRestorationIsManual = true;
    } else {
      if (entry.scroll) {
        shEntry.setScrollPosition(
          ...this._deserializeScrollPosition(entry.scroll)
        );
      }

      if (entry.presState) {
        let layoutHistoryState = shEntry.initLayoutHistoryState();

        for (let presState of entry.presState) {
          this._deserializePresState(layoutHistoryState, presState);
        }
      }
    }

    let childDocIdents = {};
    if (entry.docIdentifier) {
      // If we have a serialized document identifier, try to find an SHEntry
      // which matches that doc identifier and adopt that SHEntry's
      // BFCacheEntry.  If we don't find a match, insert shEntry as the match
      // for the document identifier.
      let matchingEntry = docIdentMap[entry.docIdentifier];
      if (!matchingEntry) {
        matchingEntry = { shEntry, childDocIdents };
        docIdentMap[entry.docIdentifier] = matchingEntry;
      } else {
        shEntry.adoptBFCacheEntry(matchingEntry.shEntry);
        childDocIdents = matchingEntry.childDocIdents;
      }
    }

    // Every load must have a triggeringPrincipal to load otherwise we prevent it,
    // this code *must* always return a valid principal:
    shEntry.triggeringPrincipal = lazy.E10SUtils.deserializePrincipal(
      entry.triggeringPrincipal_base64,
      () => {
        // This callback fires when we failed to deserialize the principal (or we don't have one)
        // and this ensures we always have a principal returned from this function.
        // We must always have a triggering principal for a load to work.
        // A null principal won't always work however is safe to use.
        console.warn(
          "Couldn't deserialize the triggeringPrincipal, falling back to NullPrincipal"
        );
        return Services.scriptSecurityManager.createNullPrincipal({});
      }
    );
    // As both partitionedPrincipal and principalToInherit are both not required to load
    // it's ok to keep these undefined when we don't have a previously defined principal.
    if (entry.principalToInherit_base64) {
      shEntry.principalToInherit = lazy.E10SUtils.deserializePrincipal(
        entry.principalToInherit_base64
      );
    }
    if (entry.policyContainer) {
      // Firefox 143 and later writes to policyContainer (bug 1974070).
      shEntry.policyContainer = lazy.E10SUtils.deserializePolicyContainer(
        entry.policyContainer
      );
    } else if (entry.csp) {
      // Firefox 142 and earlier writes entry.csp;
      const csp = lazy.E10SUtils.deserializeCSP(entry.csp);
      shEntry.policyContainer = new lazy.PolicyContainer(csp);
    }
    if (entry.wireframe) {
      shEntry.wireframe = entry.wireframe;
    }

    if (entry.children) {
      for (var i = 0; i < entry.children.length; i++) {
        // XXXzpao Wallpaper patch for bug 514751
        if (!entry.children[i].url) {
          continue;
        }

        // We're getting sessionrestore.js files with a cycle in the
        // doc-identifier graph, likely due to bug 698656.  (That is, we have
        // an entry where doc identifier A is an ancestor of doc identifier B,
        // and another entry where doc identifier B is an ancestor of A.)
        //
        // If we were to respect these doc identifiers, we'd create a cycle in
        // the SHEntries themselves, which causes the docshell to loop forever
        // when it looks for the root SHEntry.
        //
        // So as a hack to fix this, we restrict the scope of a doc identifier
        // to be a node's siblings and cousins, and pass childDocIdents, not
        // aDocIdents, to _deserializeHistoryEntry.  That is, we say that two
        // SHEntries with the same doc identifier have the same document iff
        // they have the same parent or their parents have the same document.

        shEntry.AddChild(
          this.deserializeEntry(
            entry.children[i],
            idMap,
            childDocIdents,
            shistory
          ),
          i
        );
      }
    }

    return shEntry;
  },

  /**
   * Expands serialized PresState data and adds it to the given nsILayoutHistoryState.
   *
   * @param layoutHistoryState
   *        nsILayoutHistoryState instance
   * @param presState
   *        Object containing serialized PresState data.
   */
  _deserializePresState(layoutHistoryState, presState) {
    let stateKey = presState.stateKey;
    let scrollOriginDowngrade =
      typeof presState.scrollOriginDowngrade == "boolean"
        ? presState.scrollOriginDowngrade
        : true;
    let res = presState.res || 1.0;

    layoutHistoryState.addNewPresState(
      stateKey,
      ...this._deserializeScrollPosition(presState.scroll),
      scrollOriginDowngrade,
      res
    );
  },

  /**
   * Expands serialized scroll position data into an array containing the x and y coordinates,
   * defaulting to 0,0 if no scroll position was found.
   *
   * @param scroll
   *        Object containing serialized scroll position data.
   * @return An array containing the scroll position's x and y coordinates.
   */
  _deserializeScrollPosition(scroll = "0,0") {
    return scroll.split(",").map(pos => parseInt(pos, 10) || 0);
  },
};
