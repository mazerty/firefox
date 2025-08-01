/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* import-globals-from extensionControlled.js */
/* import-globals-from preferences.js */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AddonSearchEngine:
    "moz-src:///toolkit/components/search/AddonSearchEngine.sys.mjs",
  CustomizableUI: "resource:///modules/CustomizableUI.sys.mjs",
  SearchUIUtils: "moz-src:///browser/components/search/SearchUIUtils.sys.mjs",
  SearchUtils: "moz-src:///toolkit/components/search/SearchUtils.sys.mjs",
  UserSearchEngine:
    "moz-src:///toolkit/components/search/UserSearchEngine.sys.mjs",
});

Preferences.addAll([
  { id: "browser.search.suggest.enabled", type: "bool" },
  { id: "browser.urlbar.suggest.searches", type: "bool" },
  { id: "browser.search.suggest.enabled.private", type: "bool" },
  { id: "browser.urlbar.showSearchSuggestionsFirst", type: "bool" },
  { id: "browser.urlbar.showSearchTerms.enabled", type: "bool" },
  { id: "browser.search.separatePrivateDefault", type: "bool" },
  { id: "browser.search.separatePrivateDefault.ui.enabled", type: "bool" },
  { id: "browser.urlbar.suggest.trending", type: "bool" },
  { id: "browser.urlbar.trending.featureGate", type: "bool" },
  { id: "browser.urlbar.recentsearches.featureGate", type: "bool" },
  { id: "browser.urlbar.suggest.recentsearches", type: "bool" },
  { id: "browser.urlbar.scotchBonnet.enableOverride", type: "bool" },
  { id: "browser.urlbar.update2.engineAliasRefresh", type: "bool" },
]);

const ENGINE_FLAVOR = "text/x-moz-search-engine";
const SEARCH_TYPE = "default_search";
const SEARCH_KEY = "defaultSearch";

var gEngineView = null;

var gSearchPane = {
  _engineStore: null,
  _engineDropDown: null,
  _engineDropDownPrivate: null,

  init() {
    this._engineStore = new EngineStore();
    gEngineView = new EngineView(this._engineStore);

    this._engineDropDown = new DefaultEngineDropDown(
      "normal",
      this._engineStore
    );
    this._engineDropDownPrivate = new DefaultEngineDropDown(
      "private",
      this._engineStore
    );

    this._engineStore.init().catch(console.error);

    if (
      Services.policies &&
      !Services.policies.isAllowed("installSearchEngine")
    ) {
      document.getElementById("addEnginesBox").hidden = true;
    } else {
      let addEnginesLink = document.getElementById("addEngines");
      addEnginesLink.setAttribute("href", lazy.SearchUIUtils.searchEnginesURL);
    }

    window.addEventListener("command", this);

    Services.obs.addObserver(this, "browser-search-engine-modified");
    Services.obs.addObserver(this, "intl:app-locales-changed");
    Services.obs.addObserver(this, "quicksuggest-dismissals-changed");
    window.addEventListener("unload", () => {
      Services.obs.removeObserver(this, "browser-search-engine-modified");
      Services.obs.removeObserver(this, "intl:app-locales-changed");
      Services.obs.removeObserver(this, "quicksuggest-dismissals-changed");
    });

    let suggestsPref = Preferences.get("browser.search.suggest.enabled");
    let urlbarSuggestsPref = Preferences.get("browser.urlbar.suggest.searches");
    let privateSuggestsPref = Preferences.get(
      "browser.search.suggest.enabled.private"
    );

    Preferences.get("browser.urlbar.update2.engineAliasRefresh").on(
      "change",
      () => gEngineView.updateUserEngineButtonVisibility()
    );

    let updateSuggestionCheckboxes =
      this._updateSuggestionCheckboxes.bind(this);
    suggestsPref.on("change", updateSuggestionCheckboxes);
    urlbarSuggestsPref.on("change", updateSuggestionCheckboxes);
    let customizableUIListener = {
      onWidgetAfterDOMChange: node => {
        if (node.id == "search-container") {
          updateSuggestionCheckboxes();
        }
      },
    };
    lazy.CustomizableUI.addListener(customizableUIListener);
    window.addEventListener("unload", () => {
      lazy.CustomizableUI.removeListener(customizableUIListener);
    });

    let urlbarSuggests = document.getElementById("urlBarSuggestion");
    urlbarSuggests.addEventListener("command", () => {
      urlbarSuggestsPref.value = urlbarSuggests.checked;
    });
    let suggestionsInSearchFieldsCheckbox = document.getElementById(
      "suggestionsInSearchFieldsCheckbox"
    );
    // We only want to call _updateSuggestionCheckboxes once after updating
    // all prefs.
    suggestionsInSearchFieldsCheckbox.addEventListener("command", () => {
      this._skipUpdateSuggestionCheckboxesFromPrefChanges = true;
      if (!lazy.CustomizableUI.getPlacementOfWidget("search-container")) {
        urlbarSuggestsPref.value = suggestionsInSearchFieldsCheckbox.checked;
      }
      suggestsPref.value = suggestionsInSearchFieldsCheckbox.checked;
      this._skipUpdateSuggestionCheckboxesFromPrefChanges = false;
      this._updateSuggestionCheckboxes();
    });
    let privateWindowCheckbox = document.getElementById(
      "showSearchSuggestionsPrivateWindows"
    );
    privateWindowCheckbox.addEventListener("command", () => {
      privateSuggestsPref.value = privateWindowCheckbox.checked;
    });

    setEventListener(
      "browserSeparateDefaultEngine",
      "command",
      this._onBrowserSeparateDefaultEngineChange.bind(this)
    );

    this._initDefaultEngines();
    this._initShowSearchTermsCheckbox();
    this._updateSuggestionCheckboxes();
    this._initRecentSeachesCheckbox();
    this._initAddressBar();
  },

  /**
   * Initialize the default engine handling. This will hide the private default
   * options if they are not enabled yet.
   */
  _initDefaultEngines() {
    this._separatePrivateDefaultEnabledPref = Preferences.get(
      "browser.search.separatePrivateDefault.ui.enabled"
    );

    this._separatePrivateDefaultPref = Preferences.get(
      "browser.search.separatePrivateDefault"
    );

    const checkbox = document.getElementById("browserSeparateDefaultEngine");
    checkbox.checked = !this._separatePrivateDefaultPref.value;

    this._updatePrivateEngineDisplayBoxes();

    const listener = () => {
      this._updatePrivateEngineDisplayBoxes();
      this._engineStore.notifyRebuildViews();
    };

    this._separatePrivateDefaultEnabledPref.on("change", listener);
    this._separatePrivateDefaultPref.on("change", listener);
  },

  _initShowSearchTermsCheckbox() {
    let checkbox = document.getElementById("searchShowSearchTermCheckbox");
    let updateCheckboxHidden = () => {
      checkbox.hidden =
        !UrlbarPrefs.getScotchBonnetPref("showSearchTerms.featureGate") ||
        !!lazy.CustomizableUI.getPlacementOfWidget("search-container");
    };

    // Add observer of CustomizableUI as showSearchTerms checkbox
    // should be hidden while Search Bar is enabled.
    let customizableUIListener = {
      onWidgetAfterDOMChange: node => {
        if (node.id == "search-container") {
          updateCheckboxHidden();
        }
      },
    };
    lazy.CustomizableUI.addListener(customizableUIListener);

    // Fire once to initialize.
    updateCheckboxHidden();

    window.addEventListener("unload", () => {
      lazy.CustomizableUI.removeListener(customizableUIListener);
    });
  },

  _updatePrivateEngineDisplayBoxes() {
    const separateEnabled = this._separatePrivateDefaultEnabledPref.value;
    document.getElementById("browserSeparateDefaultEngine").hidden =
      !separateEnabled;

    const separateDefault = this._separatePrivateDefaultPref.value;

    const vbox = document.getElementById("browserPrivateEngineSelection");
    vbox.hidden = !separateEnabled || !separateDefault;
  },

  _onBrowserSeparateDefaultEngineChange(event) {
    this._separatePrivateDefaultPref.value = !event.target.checked;
  },

  _updateSuggestionCheckboxes() {
    if (this._skipUpdateSuggestionCheckboxesFromPrefChanges) {
      return;
    }
    let suggestsPref = Preferences.get("browser.search.suggest.enabled");
    let permanentPB = Services.prefs.getBoolPref(
      "browser.privatebrowsing.autostart"
    );
    let urlbarSuggests = document.getElementById("urlBarSuggestion");
    let suggestionsInSearchFieldsCheckbox = document.getElementById(
      "suggestionsInSearchFieldsCheckbox"
    );
    let positionCheckbox = document.getElementById(
      "showSearchSuggestionsFirstCheckbox"
    );
    let privateWindowCheckbox = document.getElementById(
      "showSearchSuggestionsPrivateWindows"
    );
    let urlbarSuggestsPref = Preferences.get("browser.urlbar.suggest.searches");
    let searchBarVisible =
      !!lazy.CustomizableUI.getPlacementOfWidget("search-container");

    suggestionsInSearchFieldsCheckbox.checked =
      suggestsPref.value && (searchBarVisible || urlbarSuggestsPref.value);

    urlbarSuggests.disabled = !suggestsPref.value || permanentPB;
    urlbarSuggests.hidden = !searchBarVisible;

    privateWindowCheckbox.disabled = !suggestsPref.value;
    privateWindowCheckbox.checked = Preferences.get(
      "browser.search.suggest.enabled.private"
    ).value;
    if (privateWindowCheckbox.disabled) {
      privateWindowCheckbox.checked = false;
    }

    urlbarSuggests.checked = urlbarSuggestsPref.value;
    if (urlbarSuggests.disabled) {
      urlbarSuggests.checked = false;
    }
    if (urlbarSuggests.checked) {
      positionCheckbox.disabled = false;
      // Update the checked state of the show-suggestions-first checkbox.  Note
      // that this does *not* also update its pref, it only checks the box.
      positionCheckbox.checked = Preferences.get(
        positionCheckbox.getAttribute("preference")
      ).value;
    } else {
      positionCheckbox.disabled = true;
      positionCheckbox.checked = false;
    }
    if (
      suggestionsInSearchFieldsCheckbox.checked &&
      !searchBarVisible &&
      !urlbarSuggests.checked
    ) {
      urlbarSuggestsPref.value = true;
    }

    let permanentPBLabel = document.getElementById(
      "urlBarSuggestionPermanentPBLabel"
    );
    permanentPBLabel.hidden = urlbarSuggests.hidden || !permanentPB;

    this._updateTrendingCheckbox(!suggestsPref.value || permanentPB);
  },

  _initRecentSeachesCheckbox() {
    this._recentSearchesEnabledPref = Preferences.get(
      "browser.urlbar.recentsearches.featureGate"
    );
    let recentSearchesCheckBox = document.getElementById(
      "enableRecentSearches"
    );
    const listener = () => {
      recentSearchesCheckBox.hidden = !this._recentSearchesEnabledPref.value;
    };

    this._recentSearchesEnabledPref.on("change", listener);
    listener();
  },

  async _updateTrendingCheckbox(suggestDisabled) {
    let trendingBox = document.getElementById("showTrendingSuggestionsBox");
    let trendingCheckBox = document.getElementById("showTrendingSuggestions");
    let trendingSupported = (
      await Services.search.getDefault()
    ).supportsResponseType(lazy.SearchUtils.URL_TYPE.TRENDING_JSON);
    trendingBox.hidden = !Preferences.get("browser.urlbar.trending.featureGate")
      .value;
    trendingCheckBox.disabled = suggestDisabled || !trendingSupported;
  },

  // ADDRESS BAR

  /**
   * Initializes the address bar section.
   */
  _initAddressBar() {
    // Update the Firefox Suggest section when its Nimbus config changes.
    let onNimbus = () => this._updateFirefoxSuggestSection();
    NimbusFeatures.urlbar.onUpdate(onNimbus);
    window.addEventListener("unload", () => {
      NimbusFeatures.urlbar.offUpdate(onNimbus);
    });

    document.getElementById("clipboardSuggestion").hidden = !UrlbarPrefs.get(
      "clipboard.featureGate"
    );

    this._updateFirefoxSuggestSection(true);
    this._initQuickActionsSection();
  },

  /**
   * Updates the Firefox Suggest section (in the address bar section) depending
   * on whether the user is enrolled in a Firefox Suggest rollout.
   *
   * @param {boolean} [onInit]
   *   Pass true when calling this when initializing the pane.
   */
  _updateFirefoxSuggestSection(onInit = false) {
    let container = document.getElementById("firefoxSuggestContainer");

    if (
      UrlbarPrefs.get("quickSuggestEnabled") &&
      UrlbarPrefs.get("quickSuggestSettingsUi") != QuickSuggest.SETTINGS_UI.NONE
    ) {
      // Update the l10n IDs of text elements.
      let l10nIdByElementId = {
        locationBarGroupHeader: "addressbar-header-firefox-suggest",
        locationBarSuggestionLabel: "addressbar-suggest-firefox-suggest",
      };
      for (let [elementId, l10nId] of Object.entries(l10nIdByElementId)) {
        let element = document.getElementById(elementId);
        element.dataset.l10nIdOriginal ??= element.dataset.l10nId;
        element.dataset.l10nId = l10nId;
      }

      // Update the learn more link in the section's description.
      document
        .getElementById("locationBarSuggestionLabel")
        .classList.add("tail-with-learn-more");
      document.getElementById("firefoxSuggestLearnMore").hidden = false;

      document.getElementById(
        "firefoxSuggestDataCollectionSearchToggle"
      ).hidden =
        UrlbarPrefs.get("quickSuggestSettingsUi") !=
        QuickSuggest.SETTINGS_UI.FULL;

      this._updateDismissedSuggestionsStatus();
      setEventListener("restoreDismissedSuggestions", "command", () =>
        QuickSuggest.clearDismissedSuggestions()
      );

      container.hidden = false;
    } else if (!onInit) {
      // Firefox Suggest is not enabled. This is the default, so to avoid
      // accidentally messing anything up, only modify the doc if we're being
      // called due to a change in the rollout-enabled status (!onInit).
      document
        .getElementById("locationBarSuggestionLabel")
        .classList.remove("tail-with-learn-more");
      document.getElementById("firefoxSuggestLearnMore").hidden = true;
      container.hidden = true;
      let elementIds = ["locationBarGroupHeader", "locationBarSuggestionLabel"];
      for (let id of elementIds) {
        let element = document.getElementById(id);
        if (element.dataset.l10nIdOriginal) {
          document.l10n.setAttributes(element, element.dataset.l10nIdOriginal);
          delete element.dataset.l10nIdOriginal;
        }
      }
    }
  },

  _initQuickActionsSection() {
    let showPref = Preferences.get("browser.urlbar.quickactions.showPrefs");
    let scotchBonnet = Preferences.get(
      "browser.urlbar.scotchBonnet.enableOverride"
    );
    let showQuickActionsGroup = () => {
      document.getElementById("quickActionsBox").hidden = !(
        showPref.value || scotchBonnet.value
      );
    };
    showPref.on("change", showQuickActionsGroup);
    showQuickActionsGroup();
  },

  /**
   * Enables/disables the "Restore" button for dismissed Firefox Suggest
   * suggestions.
   */
  async _updateDismissedSuggestionsStatus() {
    document.getElementById("restoreDismissedSuggestions").disabled =
      !(await QuickSuggest.canClearDismissedSuggestions());
  },

  handleEvent(aEvent) {
    if (aEvent.type != "command") {
      return;
    }
    switch (aEvent.target.id) {
      case "":
        if (aEvent.target.parentNode && aEvent.target.parentNode.parentNode) {
          if (aEvent.target.parentNode.parentNode.id == "defaultEngine") {
            gSearchPane.setDefaultEngine();
          } else if (
            aEvent.target.parentNode.parentNode.id == "defaultPrivateEngine"
          ) {
            gSearchPane.setDefaultPrivateEngine();
          }
        }
        break;
      default:
        gEngineView.handleEvent(aEvent);
    }
  },

  /**
   * Handle when the app locale is changed.
   */
  async appLocalesChanged() {
    await document.l10n.ready;
    await gEngineView.loadL10nNames();
  },

  /**
   * nsIObserver implementation.
   */
  observe(subject, topic, data) {
    switch (topic) {
      case "intl:app-locales-changed": {
        this.appLocalesChanged();
        break;
      }
      case "browser-search-engine-modified": {
        let engine = subject.QueryInterface(Ci.nsISearchEngine);
        switch (data) {
          case "engine-default": {
            // Pass through to the engine store to handle updates.
            this._engineStore.browserSearchEngineModified(engine, data);
            gSearchPane._updateSuggestionCheckboxes();
            break;
          }
          default:
            this._engineStore.browserSearchEngineModified(engine, data);
        }
        break;
      }
      case "quicksuggest-dismissals-changed":
        this._updateDismissedSuggestionsStatus();
        break;
    }
  },

  showRestoreDefaults(aEnable) {
    document.getElementById("restoreDefaultSearchEngines").disabled = !aEnable;
  },

  async setDefaultEngine() {
    await Services.search.setDefault(
      document.getElementById("defaultEngine").selectedItem.engine,
      Ci.nsISearchService.CHANGE_REASON_USER
    );
    if (ExtensionSettingsStore.getSetting(SEARCH_TYPE, SEARCH_KEY) !== null) {
      ExtensionSettingsStore.select(
        ExtensionSettingsStore.SETTING_USER_SET,
        SEARCH_TYPE,
        SEARCH_KEY
      );
    }
  },

  async setDefaultPrivateEngine() {
    await Services.search.setDefaultPrivate(
      document.getElementById("defaultPrivateEngine").selectedItem.engine,
      Ci.nsISearchService.CHANGE_REASON_USER
    );
  },
};

/**
 * Keeps track of the search engine objects and notifies the views for updates.
 */
class EngineStore {
  /**
   * A list of engines that are currently visible in the UI.
   *
   * @type {Object[]}
   */
  engines = [];

  /**
   * A list of listeners to be notified when the engine list changes.
   *
   * @type {Object[]}
   */
  #listeners = [];

  async init() {
    let engines = await Services.search.getEngines();

    let visibleEngines = engines.filter(e => !e.hidden);
    for (let engine of visibleEngines) {
      this.addEngine(engine);
    }
    this.notifyRowCountChanged(0, visibleEngines.length);

    gSearchPane.showRestoreDefaults(
      engines.some(e => e.isAppProvided && e.hidden)
    );
  }

  /**
   * Adds a listener to be notified when the engine list changes.
   *
   * @param {object} aListener
   */
  addListener(aListener) {
    this.#listeners.push(aListener);
  }

  /**
   * Notifies all listeners that the engine list has changed and they should
   * rebuild.
   */
  notifyRebuildViews() {
    for (let listener of this.#listeners) {
      try {
        listener.rebuild(this.engines);
      } catch (ex) {
        console.error("Error notifying EngineStore listener", ex);
      }
    }
  }

  /**
   * Notifies all listeners that the number of engines in the list has changed.
   *
   * @param {number} index
   * @param {number} count
   */
  notifyRowCountChanged(index, count) {
    for (let listener of this.#listeners) {
      listener.rowCountChanged(index, count, this.engines);
    }
  }

  /**
   * Notifies all listeners that the default engine has changed.
   *
   * @param {string} type
   * @param {object} engine
   */
  notifyDefaultEngineChanged(type, engine) {
    for (let listener of this.#listeners) {
      if ("defaultEngineChanged" in listener) {
        listener.defaultEngineChanged(type, engine, this.engines);
      }
    }
  }

  notifyEngineIconUpdated(engine) {
    // Check the engine is still in the list.
    let index = this._getIndexForEngine(engine);
    if (index != -1) {
      for (let listener of this.#listeners) {
        listener.engineIconUpdated(index, this.engines);
      }
    }
  }

  _getIndexForEngine(aEngine) {
    return this.engines.indexOf(aEngine);
  }

  _getEngineByName(aName) {
    return this.engines.find(engine => engine.name == aName);
  }

  /**
   * Converts an nsISearchEngine object into an Engine Store
   * search engine object.
   *
   * @param {nsISearchEngine} aEngine
   *   The search engine to convert.
   * @returns {object}
   *   The EngineStore search engine object.
   */
  _cloneEngine(aEngine) {
    var clonedObj = {
      iconURL: null,
    };
    for (let i of ["id", "name", "alias", "hidden", "isAppProvided"]) {
      clonedObj[i] = aEngine[i];
    }
    clonedObj.isAddonEngine =
      aEngine.wrappedJSObject instanceof lazy.AddonSearchEngine;
    clonedObj.isUserEngine =
      aEngine.wrappedJSObject instanceof lazy.UserSearchEngine;
    clonedObj.originalEngine = aEngine;

    // Trigger getting the iconURL for this engine.
    aEngine.getIconURL().then(iconURL => {
      if (iconURL) {
        clonedObj.iconURL = iconURL;
      } else if (window.devicePixelRatio > 1) {
        clonedObj.iconURL =
          "chrome://browser/skin/search-engine-placeholder@2x.png";
      } else {
        clonedObj.iconURL =
          "chrome://browser/skin/search-engine-placeholder.png";
      }

      this.notifyEngineIconUpdated(clonedObj);
    });

    return clonedObj;
  }

  // Callback for Array's some(). A thisObj must be passed to some()
  _isSameEngine(aEngineClone) {
    return aEngineClone.originalEngine.id == this.originalEngine.id;
  }

  addEngine(aEngine) {
    this.engines.push(this._cloneEngine(aEngine));
  }

  updateEngine(newEngine) {
    let engineToUpdate = this.engines.findIndex(
      e => e.originalEngine.id == newEngine.id
    );
    if (engineToUpdate != -1) {
      this.engines[engineToUpdate] = this._cloneEngine(newEngine);
    }
  }

  moveEngine(aEngine, aNewIndex) {
    if (aNewIndex < 0 || aNewIndex > this.engines.length - 1) {
      throw new Error("ES_moveEngine: invalid aNewIndex!");
    }
    var index = this._getIndexForEngine(aEngine);
    if (index == -1) {
      throw new Error("ES_moveEngine: invalid engine?");
    }

    if (index == aNewIndex) {
      return Promise.resolve();
    } // nothing to do

    // Move the engine in our internal store
    var removedEngine = this.engines.splice(index, 1)[0];
    this.engines.splice(aNewIndex, 0, removedEngine);

    return Services.search.moveEngine(aEngine.originalEngine, aNewIndex);
  }

  /**
   * Called when a search engine is removed.
   *
   * @param {nsISearchEngine} aEngine
   *   The Engine being removed. Note that this is an nsISearchEngine object.
   */
  removeEngine(aEngine) {
    if (this.engines.length == 1) {
      throw new Error("Cannot remove last engine!");
    }

    let engineId = aEngine.id;
    let index = this.engines.findIndex(element => element.id == engineId);

    if (index == -1) {
      throw new Error("invalid engine?");
    }

    this.engines.splice(index, 1)[0];

    if (aEngine.isAppProvided) {
      gSearchPane.showRestoreDefaults(true);
    }

    this.notifyRowCountChanged(index, -1);

    document.getElementById("engineList").focus();
  }

  /**
   * Update the default engine UI and engine tree view as appropriate when engine changes
   * or locale changes occur.
   *
   * @param {nsISearchEngine} engine
   * @param {string} data
   */
  browserSearchEngineModified(engine, data) {
    engine.QueryInterface(Ci.nsISearchEngine);
    switch (data) {
      case "engine-added":
        this.addEngine(engine);
        this.notifyRowCountChanged(gEngineView.lastEngineIndex, 1);
        break;
      case "engine-changed":
      case "engine-icon-changed":
        this.updateEngine(engine);
        this.notifyRebuildViews();
        break;
      case "engine-removed":
        this.removeEngine(engine);
        break;
      case "engine-default":
        this.notifyDefaultEngineChanged("normal", engine);
        break;
      case "engine-default-private":
        this.notifyDefaultEngineChanged("private", engine);
        break;
    }
  }

  async restoreDefaultEngines() {
    var added = 0;
    // _cloneEngine is necessary here because all functions in
    // this file work on EngineStore search engine objects.
    let appProvidedEngines = (
      await Services.search.getAppProvidedEngines()
    ).map(this._cloneEngine, this);

    for (var i = 0; i < appProvidedEngines.length; ++i) {
      var e = appProvidedEngines[i];

      // If the engine is already in the list, just move it.
      if (this.engines.some(this._isSameEngine, e)) {
        await this.moveEngine(this._getEngineByName(e.name), i);
      } else {
        // Otherwise, add it back to our internal store

        // The search service removes the alias when an engine is hidden,
        // so clear any alias we may have cached before unhiding the engine.
        e.alias = "";

        this.engines.splice(i, 0, e);
        let engine = e.originalEngine;
        engine.hidden = false;
        await Services.search.moveEngine(engine, i);
        added++;
      }
    }

    // We can't do this as part of the loop above because the indices are
    // used for moving engines.
    let policyRemovedEngineNames =
      Services.policies.getActivePolicies()?.SearchEngines?.Remove || [];
    for (let engineName of policyRemovedEngineNames) {
      let engine = Services.search.getEngineByName(engineName);
      if (engine) {
        try {
          await Services.search.removeEngine(
            engine,
            Ci.nsISearchService.CHANGE_REASON_ENTERPRISE
          );
        } catch (ex) {
          // Engine might not exist
        }
      }
    }

    Services.search.resetToAppDefaultEngine();
    gSearchPane.showRestoreDefaults(false);
    this.notifyRebuildViews();
    return added;
  }

  changeEngine(aEngine, aProp, aNewValue) {
    var index = this._getIndexForEngine(aEngine);
    if (index == -1) {
      throw new Error("invalid engine?");
    }

    this.engines[index][aProp] = aNewValue;
    aEngine.originalEngine[aProp] = aNewValue;
  }
}

/**
 * Manages the view of the Search Shortcuts tree on the search pane of preferences.
 */
class EngineView {
  _engineStore;
  _engineList = null;
  tree = null;

  /**
   * @param {EngineStore} aEngineStore
   */
  constructor(aEngineStore) {
    this._engineStore = aEngineStore;
    this._engineList = document.getElementById("engineList");
    this._engineList.view = this;

    UrlbarPrefs.addObserver(this);
    aEngineStore.addListener(this);

    this.loadL10nNames();
    this.#addListeners();
    this.updateUserEngineButtonVisibility();
  }

  async loadL10nNames() {
    // This maps local shortcut sources to their l10n names.  The names are needed
    // by getCellText.  Getting the names is async but getCellText is not, so we
    // cache them here to retrieve them syncronously in getCellText.
    this._localShortcutL10nNames = new Map();

    let getIDs = (suffix = "") =>
      UrlbarUtils.LOCAL_SEARCH_MODES.map(mode => {
        let name = UrlbarUtils.getResultSourceName(mode.source);
        return { id: `urlbar-search-mode-${name}${suffix}` };
      });

    try {
      let localizedIDs = getIDs();
      let englishIDs = getIDs("-en");

      let englishSearchStrings = new Localization([
        "preview/enUS-searchFeatures.ftl",
      ]);
      let localizedNames = await document.l10n.formatValues(localizedIDs);
      let englishNames = await englishSearchStrings.formatValues(englishIDs);

      UrlbarUtils.LOCAL_SEARCH_MODES.forEach(({ source }, index) => {
        let localizedName = localizedNames[index];
        let englishName = englishNames[index];

        // Add only the English name if localized and English are the same
        let names =
          localizedName === englishName
            ? [englishName]
            : [localizedName, englishName];

        this._localShortcutL10nNames.set(source, names);

        // Invalidate the tree now that we have the names in case getCellText was
        // called before name retrieval finished.
        this.invalidate();
      });
    } catch (ex) {
      console.error("Error loading l10n names", ex);
    }
  }

  #addListeners() {
    this._engineList.addEventListener("click", this);
    this._engineList.addEventListener("dragstart", this);
    this._engineList.addEventListener("keypress", this);
    this._engineList.addEventListener("select", this);
    this._engineList.addEventListener("dblclick", this);
  }

  /**
   * Shows the Add and Edit Search Engines buttons if the pref is enabled.
   */
  updateUserEngineButtonVisibility() {
    let aliasRefresh = Services.prefs.getBoolPref(
      "browser.urlbar.update2.engineAliasRefresh",
      false
    );
    document.getElementById("addEngineButton").hidden = !aliasRefresh;
    document.getElementById("editEngineButton").hidden = !aliasRefresh;
  }

  get lastEngineIndex() {
    return this._engineStore.engines.length - 1;
  }

  get selectedIndex() {
    var seln = this.selection;
    if (seln.getRangeCount() > 0) {
      var min = {};
      seln.getRangeAt(0, min, {});
      return min.value;
    }
    return -1;
  }

  get selectedEngine() {
    return this._engineStore.engines[this.selectedIndex];
  }

  // Helpers
  rebuild() {
    this.invalidate();
  }

  rowCountChanged(index, count) {
    if (!this.tree) {
      return;
    }
    this.tree.rowCountChanged(index, count);

    // If we're removing elements, ensure that we still have a selection.
    if (count < 0) {
      this.selection.select(Math.min(index, this.rowCount - 1));
      this.ensureRowIsVisible(this.currentIndex);
    }
  }

  engineIconUpdated(index) {
    this.tree?.invalidateCell(
      index,
      this.tree.columns.getNamedColumn("engineName")
    );
  }

  invalidate() {
    this.tree?.invalidate();
  }

  ensureRowIsVisible(index) {
    this.tree.ensureRowIsVisible(index);
  }

  getSourceIndexFromDrag(dataTransfer) {
    return parseInt(dataTransfer.getData(ENGINE_FLAVOR));
  }

  isCheckBox(index, column) {
    return column.id == "engineShown";
  }

  isEngineSelectedAndRemovable() {
    let defaultEngine = Services.search.defaultEngine;
    let defaultPrivateEngine = Services.search.defaultPrivateEngine;
    // We don't allow the last remaining engine to be removed, thus the
    // `this.lastEngineIndex != 0` check.
    // We don't allow the default engine to be removed.
    return (
      this.selectedIndex != -1 &&
      this.lastEngineIndex != 0 &&
      !this._getLocalShortcut(this.selectedIndex) &&
      this.selectedEngine.name != defaultEngine.name &&
      this.selectedEngine.name != defaultPrivateEngine.name
    );
  }

  /**
   * Removes a search engine from the search service.
   *
   * Application provided engines are removed without confirmation since they
   * can easily be restored. Addon engines are not removed (see comment).
   * For other engine types, the user is prompted for confirmation.
   *
   * @param {object} engine
   *   The search engine object from EngineStore to remove.
   */
  async promptAndRemoveEngine(engine) {
    if (engine.isAppProvided) {
      Services.search.removeEngine(
        this.selectedEngine.originalEngine,
        Ci.nsISearchService.CHANGE_REASON_USER
      );
      return;
    }

    if (engine.isAddonEngine) {
      // Addon engines will re-appear after restarting, see Bug 1546652.
      // This should ideally prompt the user if they want to remove the addon.
      let msg = await document.l10n.formatValue("remove-addon-engine-alert");
      alert(msg);
      return;
    }

    let [body, removeLabel] = await document.l10n.formatValues([
      "remove-engine-confirmation",
      "remove-engine-remove",
    ]);

    let button = Services.prompt.confirmExBC(
      window.browsingContext,
      Services.prompt.MODAL_TYPE_CONTENT,
      null,
      body,
      (Services.prompt.BUTTON_TITLE_IS_STRING * Services.prompt.BUTTON_POS_0) |
        (Services.prompt.BUTTON_TITLE_CANCEL * Services.prompt.BUTTON_POS_1),
      removeLabel,
      null,
      null,
      null,
      {}
    );

    // Button 0 is the remove button.
    if (button == 0) {
      Services.search.removeEngine(
        this.selectedEngine.originalEngine,
        Ci.nsISearchService.CHANGE_REASON_USER
      );
    }
  }

  /**
   * Returns the local shortcut corresponding to a tree row, or null if the row
   * is not a local shortcut.
   *
   * @param {number} index
   *   The tree row index.
   * @returns {object}
   *   The local shortcut object or null if the row is not a local shortcut.
   */
  _getLocalShortcut(index) {
    let engineCount = this._engineStore.engines.length;
    if (index < engineCount) {
      return null;
    }
    return UrlbarUtils.LOCAL_SEARCH_MODES[index - engineCount];
  }

  /**
   * Called by UrlbarPrefs when a urlbar pref changes.
   *
   * @param {string} pref
   *   The name of the pref relative to the browser.urlbar branch.
   */
  onPrefChanged(pref) {
    // If one of the local shortcut prefs was toggled, toggle its row's
    // checkbox.
    let parts = pref.split(".");
    if (parts[0] == "shortcuts" && parts[1] && parts.length == 2) {
      this.invalidate();
    }
  }

  handleEvent(aEvent) {
    switch (aEvent.type) {
      case "dblclick":
        if (aEvent.target.id == "engineChildren") {
          let cell = aEvent.target.parentNode.getCellAt(
            aEvent.clientX,
            aEvent.clientY
          );
          if (cell.col?.id == "engineKeyword") {
            this.#startEditingAlias(this.selectedIndex);
          }
        }
        break;
      case "click":
        if (
          aEvent.target.id != "engineChildren" &&
          !aEvent.target.classList.contains("searchEngineAction")
        ) {
          // We don't want to toggle off selection while editing keyword
          // so proceed only when the input field is hidden.
          // We need to check that engineList.view is defined here
          // because the "click" event listener is on <window> and the
          // view might have been destroyed if the pane has been navigated
          // away from.
          if (this._engineList.inputField.hidden && this._engineList.view) {
            let selection = this._engineList.view.selection;
            if (selection?.count > 0) {
              selection.toggleSelect(selection.currentIndex);
            }
            this._engineList.blur();
          }
        }
        break;
      case "command":
        switch (aEvent.target.id) {
          case "restoreDefaultSearchEngines":
            this.#onRestoreDefaults();
            break;
          case "removeEngineButton":
            if (this.isEngineSelectedAndRemovable()) {
              this.promptAndRemoveEngine(this.selectedEngine);
            }
            break;
          case "editEngineButton":
            if (this.selectedEngine.isUserEngine) {
              let engine = this.selectedEngine.originalEngine.wrappedJSObject;
              gSubDialog.open(
                "chrome://browser/content/search/addEngine.xhtml",
                { features: "resizable=no, modal=yes" },
                { engine, mode: "EDIT" }
              );
            }
            break;
          case "addEngineButton":
            gSubDialog.open(
              "chrome://browser/content/search/addEngine.xhtml",
              { features: "resizable=no, modal=yes" },
              { mode: "NEW" }
            );
            break;
        }
        break;
      case "dragstart":
        if (aEvent.target.id == "engineChildren") {
          this.#onDragEngineStart(aEvent);
        }
        break;
      case "keypress":
        if (aEvent.target.id == "engineList") {
          this.#onTreeKeyPress(aEvent);
        }
        break;
      case "select":
        if (aEvent.target.id == "engineList") {
          this.#onTreeSelect();
        }
        break;
    }
  }

  /**
   * Called when the restore default engines button is clicked to reset the
   * list of engines to their defaults.
   */
  async #onRestoreDefaults() {
    let num = await this._engineStore.restoreDefaultEngines();
    this.rowCountChanged(0, num);
  }

  #onDragEngineStart(event) {
    let selectedIndex = this.selectedIndex;

    // Local shortcut rows can't be dragged or re-ordered.
    if (this._getLocalShortcut(selectedIndex)) {
      event.preventDefault();
      return;
    }

    let tree = document.getElementById("engineList");
    let cell = tree.getCellAt(event.clientX, event.clientY);
    if (selectedIndex >= 0 && !this.isCheckBox(cell.row, cell.col)) {
      event.dataTransfer.setData(ENGINE_FLAVOR, selectedIndex.toString());
      event.dataTransfer.effectAllowed = "move";
    }
  }

  #onTreeSelect() {
    document.getElementById("removeEngineButton").disabled =
      !this.isEngineSelectedAndRemovable();
    document.getElementById("editEngineButton").disabled =
      !this.selectedEngine?.isUserEngine;
  }

  #onTreeKeyPress(aEvent) {
    let index = this.selectedIndex;
    let tree = document.getElementById("engineList");
    if (tree.hasAttribute("editing")) {
      return;
    }

    if (aEvent.charCode == KeyEvent.DOM_VK_SPACE) {
      // Space toggles the checkbox.
      let newValue = !this.getCellValue(
        index,
        tree.columns.getNamedColumn("engineShown")
      );
      this.setCellValue(
        index,
        tree.columns.getFirstColumn(),
        newValue.toString()
      );
      // Prevent page from scrolling on the space key.
      aEvent.preventDefault();
    } else {
      let isMac = Services.appinfo.OS == "Darwin";
      if (
        (isMac && aEvent.keyCode == KeyEvent.DOM_VK_RETURN) ||
        (!isMac && aEvent.keyCode == KeyEvent.DOM_VK_F2)
      ) {
        this.#startEditingAlias(index);
      } else if (
        aEvent.keyCode == KeyEvent.DOM_VK_DELETE ||
        (isMac &&
          aEvent.shiftKey &&
          aEvent.keyCode == KeyEvent.DOM_VK_BACK_SPACE)
      ) {
        // Delete and Shift+Backspace (Mac) removes selected engine.
        if (this.isEngineSelectedAndRemovable()) {
          this.promptAndRemoveEngine(this.selectedEngine);
        }
      }
    }
  }

  /**
   * Triggers editing of an alias in the tree.
   *
   * @param {number} index
   */
  #startEditingAlias(index) {
    // Local shortcut aliases can't be edited.
    if (this._getLocalShortcut(index)) {
      return;
    }

    let engine = this._engineStore.engines[index];
    this.tree.startEditing(index, this.tree.columns.getLastColumn());
    this.tree.inputField.value = engine.alias || "";
    this.tree.inputField.select();
  }

  /**
   * Triggers editing of an engine name in the tree.
   *
   * @param {number} index
   */
  #startEditingName(index) {
    let engine = this._engineStore.engines[index];
    if (!engine.isUserEngine) {
      return;
    }

    this.tree.startEditing(
      index,
      this.tree.columns.getNamedColumn("engineName")
    );
    this.tree.inputField.value = engine.name;
    this.tree.inputField.select();
  }

  // nsITreeView
  get rowCount() {
    let localModes = UrlbarUtils.LOCAL_SEARCH_MODES;
    if (!UrlbarPrefs.get("scotchBonnet.enableOverride")) {
      localModes = localModes.filter(
        mode => mode.source != UrlbarUtils.RESULT_SOURCE.ACTIONS
      );
    }
    return this._engineStore.engines.length + localModes.length;
  }

  getImageSrc(index, column) {
    if (column.id == "engineName") {
      let shortcut = this._getLocalShortcut(index);
      if (shortcut) {
        return shortcut.icon;
      }

      return this._engineStore.engines[index].iconURL;
    }

    return "";
  }

  getCellText(index, column) {
    if (column.id == "engineName") {
      let shortcut = this._getLocalShortcut(index);
      if (shortcut) {
        return this._localShortcutL10nNames.get(shortcut.source)[0] || "";
      }
      return this._engineStore.engines[index].name;
    } else if (column.id == "engineKeyword") {
      let shortcut = this._getLocalShortcut(index);
      if (shortcut) {
        if (
          UrlbarPrefs.getScotchBonnetPref("searchRestrictKeywords.featureGate")
        ) {
          let keywords = this._localShortcutL10nNames
            .get(shortcut.source)
            .map(keyword => `@${keyword.toLowerCase()}`)
            .join(", ");

          return `${keywords}, ${shortcut.restrict}`;
        }

        return shortcut.restrict;
      }
      return this._engineStore.engines[index].originalEngine.aliases.join(", ");
    }
    return "";
  }

  setTree(tree) {
    this.tree = tree;
  }

  canDrop(targetIndex, orientation, dataTransfer) {
    var sourceIndex = this.getSourceIndexFromDrag(dataTransfer);
    return (
      sourceIndex != -1 &&
      sourceIndex != targetIndex &&
      sourceIndex != targetIndex + orientation &&
      // Local shortcut rows can't be dragged or dropped on.
      targetIndex < this._engineStore.engines.length
    );
  }

  async drop(dropIndex, orientation, dataTransfer) {
    // Local shortcut rows can't be dragged or dropped on.  This can sometimes
    // be reached even though canDrop returns false for these rows.
    if (this._engineStore.engines.length <= dropIndex) {
      return;
    }

    var sourceIndex = this.getSourceIndexFromDrag(dataTransfer);
    var sourceEngine = this._engineStore.engines[sourceIndex];

    const nsITreeView = Ci.nsITreeView;
    if (dropIndex > sourceIndex) {
      if (orientation == nsITreeView.DROP_BEFORE) {
        dropIndex--;
      }
    } else if (orientation == nsITreeView.DROP_AFTER) {
      dropIndex++;
    }

    await this._engineStore.moveEngine(sourceEngine, dropIndex);
    gSearchPane.showRestoreDefaults(true);

    // Redraw, and adjust selection
    this.invalidate();
    this.selection.select(dropIndex);
  }

  selection = null;
  getRowProperties() {
    return "";
  }
  getCellProperties(index, column) {
    if (column.id == "engineName") {
      // For local shortcut rows, return the result source name so we can style
      // the icons in CSS.
      let shortcut = this._getLocalShortcut(index);
      if (shortcut) {
        return UrlbarUtils.getResultSourceName(shortcut.source);
      }
    }
    return "";
  }
  getColumnProperties() {
    return "";
  }
  isContainer() {
    return false;
  }
  isContainerOpen() {
    return false;
  }
  isContainerEmpty() {
    return false;
  }
  isSeparator() {
    return false;
  }
  isSorted() {
    return false;
  }
  getParentIndex() {
    return -1;
  }
  hasNextSibling() {
    return false;
  }
  getLevel() {
    return 0;
  }
  getCellValue(index, column) {
    if (column.id == "engineShown") {
      let shortcut = this._getLocalShortcut(index);
      if (shortcut) {
        return UrlbarPrefs.get(shortcut.pref);
      }
      return !this._engineStore.engines[index].originalEngine.hideOneOffButton;
    }
    return undefined;
  }
  toggleOpenState() {}
  cycleHeader() {}
  selectionChanged() {}
  cycleCell() {}
  isEditable(index, column) {
    return (
      column.id == "engineShown" ||
      (column.id == "engineKeyword" && !this._getLocalShortcut(index)) ||
      (column.id == "engineName" &&
        this._engineStore.engines[index].isUserEngine)
    );
  }
  setCellValue(index, column, value) {
    if (column.id == "engineShown") {
      let shortcut = this._getLocalShortcut(index);
      if (shortcut) {
        UrlbarPrefs.set(shortcut.pref, value == "true");
        this.invalidate();
        return;
      }
      this._engineStore.engines[index].originalEngine.hideOneOffButton =
        value != "true";
      this.invalidate();
    }
  }
  async setCellText(index, column, value) {
    let engine = this._engineStore.engines[index];
    if (column.id == "engineKeyword") {
      let valid = await this.#changeKeyword(engine, value);
      if (!valid) {
        this.#startEditingAlias(index);
      }
    } else if (column.id == "engineName" && engine.isUserEngine) {
      let valid = await this.#changeName(engine, value);
      if (!valid) {
        this.#startEditingName(index);
      }
    }
  }

  /**
   * Handles changing the keyword for an engine. This will check for potentially
   * duplicate keywords and prompt the user if necessary.
   *
   * @param {object} aEngine
   *   The engine to change.
   * @param {string} aNewKeyword
   *   The new keyword.
   * @returns {Promise<boolean>}
   *   Resolves to true if the keyword was changed.
   */
  async #changeKeyword(aEngine, aNewKeyword) {
    let keyword = aNewKeyword.trim();
    if (keyword) {
      let isBookmarkDuplicate = !!(await PlacesUtils.keywords.fetch(keyword));

      let dupEngine = await Services.search.getEngineByAlias(keyword);
      let isEngineDuplicate = dupEngine !== null && dupEngine.id != aEngine.id;

      // Notify the user if they have chosen an existing engine/bookmark keyword
      if (isEngineDuplicate || isBookmarkDuplicate) {
        let msgid;
        if (isEngineDuplicate) {
          msgid = {
            id: "search-keyword-warning-engine",
            args: { name: dupEngine.name },
          };
        } else {
          msgid = { id: "search-keyword-warning-bookmark" };
        }

        let msg = await document.l10n.formatValue(msgid.id, msgid.args);
        alert(msg);
        return false;
      }
    }

    this._engineStore.changeEngine(aEngine, "alias", keyword);
    this.invalidate();
    return true;
  }

  /**
   * Handles changing the name for a user engine. This will check for
   * duplicate names and warn the user if necessary.
   *
   * @param {object} aEngine
   *   The user search engine to change.
   * @param {string} aNewName
   *   The new name.
   * @returns {Promise<boolean>}
   *   Resolves to true if the name was changed.
   */
  async #changeName(aEngine, aNewName) {
    let valid = aEngine.originalEngine.wrappedJSObject.rename(aNewName);
    if (!valid) {
      let msg = await document.l10n.formatValue(
        "edit-engine-name-warning-duplicate",
        { name: aNewName }
      );
      alert(msg);
      return false;
    }
    return true;
  }
}

/**
 * Manages the default engine dropdown buttons in the search pane of preferences.
 */
class DefaultEngineDropDown {
  #element = null;
  #type = null;

  constructor(type, engineStore) {
    this.#type = type;
    this.#element = document.getElementById(
      type == "private" ? "defaultPrivateEngine" : "defaultEngine"
    );

    engineStore.addListener(this);
  }

  rowCountChanged(index, count, enginesList) {
    // Simply rebuild the menulist, rather than trying to update the changed row.
    this.rebuild(enginesList);
  }

  defaultEngineChanged(type, engine, enginesList) {
    if (type != this.#type) {
      return;
    }
    // If the user is going through the drop down using up/down keys, the
    // dropdown may still be open (eg. on Windows) when engine-default is
    // fired, so rebuilding the list unconditionally would get in the way.
    let selectedEngineName = this.#element.selectedItem?.engine?.name;
    if (selectedEngineName != engine.name) {
      this.rebuild(enginesList);
    }
  }

  engineIconUpdated(index, enginesList) {
    let item = this.#element.getItemAtIndex(index);
    // Check this is the right item.
    if (item?.label == enginesList[index].name) {
      item.setAttribute("image", enginesList[index].iconURL);
    }
  }

  async rebuild(enginesList) {
    if (
      this.#type == "private" &&
      !gSearchPane._separatePrivateDefaultPref.value
    ) {
      return;
    }
    let defaultEngine =
      await Services.search[
        this.#type == "normal" ? "getDefault" : "getDefaultPrivate"
      ]();

    this.#element.removeAllItems();
    for (let engine of enginesList) {
      let item = this.#element.appendItem(engine.name);
      item.setAttribute(
        "class",
        "menuitem-iconic searchengine-menuitem menuitem-with-favicon"
      );
      if (engine.iconURL) {
        item.setAttribute("image", engine.iconURL);
      }
      item.engine = engine;
      if (engine.name == defaultEngine.name) {
        this.#element.selectedItem = item;
      }
    }
    // This should never happen, but try and make sure we have at least one
    // selected item.
    if (!this.#element.selectedItem) {
      this.#element.selectedIndex = 0;
    }
  }
}
