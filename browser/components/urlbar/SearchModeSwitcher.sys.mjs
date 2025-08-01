/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  OpenSearchManager:
    "moz-src:///browser/components/search/OpenSearchManager.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  SearchUIUtils: "moz-src:///browser/components/search/SearchUIUtils.sys.mjs",
  UrlbarPrefs: "resource:///modules/UrlbarPrefs.sys.mjs",
  UrlbarSearchUtils: "resource:///modules/UrlbarSearchUtils.sys.mjs",
  UrlbarUtils: "resource:///modules/UrlbarUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "SearchModeSwitcherL10n", () => {
  return new Localization(["browser/browser.ftl"]);
});
ChromeUtils.defineLazyGetter(lazy, "searchModeNewBadge", () => {
  return lazy.SearchModeSwitcherL10n.formatValue("urlbar-searchmode-new");
});

// The maximum number of openSearch engines available to install
// to display.
const MAX_OPENSEARCH_ENGINES = 3;

// Default icon used for engines that do not have icons loaded.
const DEFAULT_ENGINE_ICON =
  "chrome://browser/skin/search-engine-placeholder@2x.png";

/**
 * Implements the SearchModeSwitcher in the urlbar.
 */
export class SearchModeSwitcher {
  static DEFAULT_ICON = lazy.UrlbarUtils.ICON.SEARCH_GLASS;
  #popup;
  #input;
  #toolbarbutton;

  /**
   * @param {UrlbarInput} input
   */
  constructor(input) {
    this.#input = input;

    this.QueryInterface = ChromeUtils.generateQI([
      "nsIObserver",
      "nsISupportsWeakReference",
    ]);

    lazy.UrlbarPrefs.addObserver(this);

    this.#popup = input.document.getElementById("searchmode-switcher-popup");

    this.#toolbarbutton = input.document.querySelector(
      "#urlbar-searchmode-switcher"
    );

    if (lazy.UrlbarPrefs.get("scotchBonnet.enableOverride")) {
      this.#enableObservers();
    }
  }

  async #onPopupShowing() {
    await this.#buildSearchModeList();

    this.#input.view.close({ showFocusBorder: false });

    // This moves the focus to the urlbar when the popup is closed.
    this.#input.document.commandDispatcher.focusedElement =
      this.#input.inputField;

    Glean.urlbarUnifiedsearchbutton.opened.add(1);
  }

  /**
   * Close the SearchSwitcher popup.
   */
  closePanel() {
    this.#popup.hidePopup();
  }

  #openPreferences(event) {
    if (
      (event.type == "click" && event.button != 0) ||
      (event.type == "keypress" &&
        event.charCode != KeyEvent.DOM_VK_SPACE &&
        event.keyCode != KeyEvent.DOM_VK_RETURN)
    ) {
      return; // Left click, space or enter only
    }

    event.preventDefault();
    event.stopPropagation();

    this.#input.window.openPreferences("paneSearch");
    this.#popup.hidePopup();

    Glean.urlbarUnifiedsearchbutton.picked.settings.add(1);
  }

  /**
   * Exit the engine specific searchMode.
   *
   * @param {Event} event
   *        The event that triggered the searchMode exit.
   */
  exitSearchMode(event) {
    event.preventDefault();
    this.#input.searchMode = null;
    // Update the result by the default engine.
    this.#input.startQuery();
  }

  /**
   * Called when the value of the searchMode attribute on UrlbarInput is changed.
   */
  onSearchModeChanged() {
    if (!this.#input.window || this.#input.window.closed) {
      return;
    }

    if (lazy.UrlbarPrefs.get("scotchBonnet.enableOverride")) {
      this.updateSearchIcon();

      if (
        this.#input.searchMode?.engineName == "Perplexity" &&
        !lazy.UrlbarPrefs.get("perplexity.hasBeenInSearchMode")
      ) {
        lazy.UrlbarPrefs.set("perplexity.hasBeenInSearchMode", true);
      }
    }
  }

  handleEvent(event) {
    if (event.type == "focus") {
      this.#input.setUnifiedSearchButtonAvailability(true);
      return;
    }
    if (event.type == "popupshowing") {
      this.#toolbarbutton.setAttribute("aria-expanded", true);
      this.#onPopupShowing();
      return;
    }
    if (event.type == "popuphiding") {
      this.#toolbarbutton.setAttribute("aria-expanded", false);
      return;
    }
    if (event.type == "keydown") {
      if (this.#input.view.isOpen) {
        // The urlbar view is open, which means the unified search button got
        // focus by tab key from urlbar.
        switch (event.keyCode) {
          case KeyEvent.DOM_VK_TAB: {
            // Move the focus to urlbar view to make cyclable.
            this.#input.focus();
            this.#input.view.selectBy(1, {
              reverse: event.shiftKey,
              userPressedTab: true,
            });
            event.preventDefault();
            return;
          }
          case KeyEvent.DOM_VK_ESCAPE: {
            this.#input.view.close();
            this.#input.focus();
            event.preventDefault();
            return;
          }
        }
      }

      // Manually open the popup on down.
      if (event.keyCode == KeyEvent.DOM_VK_DOWN) {
        this.#popup.openPopup(null, {
          triggerEvent: event,
        });
      }

      return;
    }

    let action = event.currentTarget.dataset.action ?? event.type;

    switch (action) {
      case "exitsearchmode": {
        this.exitSearchMode(event);
        break;
      }
      case "openpreferences": {
        this.#openPreferences(event);
        break;
      }
    }
  }

  observe(_subject, topic, data) {
    if (!this.#input.window || this.#input.window.closed) {
      return;
    }

    switch (topic) {
      case "browser-search-engine-modified": {
        if (
          data === "engine-default" ||
          data === "engine-default-private" ||
          data === "engine-icon-changed"
        ) {
          this.updateSearchIcon();
        }
        break;
      }
    }
  }

  /**
   * Called when a urlbar pref changes.
   *
   * @param {string} pref
   *   The name of the pref relative to `browser.urlbar`.
   */
  onPrefChanged(pref) {
    if (!this.#input.window || this.#input.window.closed) {
      return;
    }

    switch (pref) {
      case "scotchBonnet.enableOverride": {
        if (lazy.UrlbarPrefs.get("scotchBonnet.enableOverride")) {
          this.#enableObservers();
          this.updateSearchIcon();
        } else {
          this.#disableObservers();
        }
        break;
      }
      case "keyword.enabled": {
        if (lazy.UrlbarPrefs.get("scotchBonnet.enableOverride")) {
          this.updateSearchIcon();
        }
        break;
      }
    }
  }

  async updateSearchIcon() {
    let searchMode = this.#input.searchMode;

    try {
      await lazy.UrlbarSearchUtils.init();
    } catch {
      console.error("Search service failed to init");
    }

    let { label, icon } = await this.#getDisplayedEngineDetails(
      this.#input.searchMode
    );

    if (searchMode?.source != this.#input.searchMode?.source) {
      return;
    }

    const inSearchMode = this.#input.searchMode;
    if (!lazy.UrlbarPrefs.get("unifiedSearchButton.always")) {
      const keywordEnabled = lazy.UrlbarPrefs.get("keyword.enabled");
      if (!keywordEnabled && !inSearchMode) {
        icon = SearchModeSwitcher.DEFAULT_ICON;
      }
    } else if (!inSearchMode) {
      // Use default icon set in CSS.
      icon = null;
    }

    let iconUrl = icon ? `url(${icon})` : null;
    this.#input.document.getElementById(
      "searchmode-switcher-icon"
    ).style.listStyleImage = iconUrl;

    if (label) {
      this.#input.document.l10n.setAttributes(
        this.#toolbarbutton,
        "urlbar-searchmode-button2",
        { engine: label }
      );
    } else {
      this.#input.document.l10n.setAttributes(
        this.#toolbarbutton,
        "urlbar-searchmode-button-no-engine"
      );
    }

    let labelEl = this.#input.document.getElementById(
      "searchmode-switcher-title"
    );

    if (!inSearchMode) {
      labelEl.replaceChildren();
    } else {
      labelEl.textContent = label;
    }
  }

  async #getSearchModeLabel(source) {
    let mode = lazy.UrlbarUtils.LOCAL_SEARCH_MODES.find(
      m => m.source == source
    );
    let [str] = await lazy.SearchModeSwitcherL10n.formatMessages([
      { id: mode.uiLabel },
    ]);
    return str.attributes[0].value;
  }

  async #getDisplayedEngineDetails(searchMode = null) {
    if (!Services.search.hasSuccessfullyInitialized) {
      return { label: null, icon: SearchModeSwitcher.DEFAULT_ICON };
    }

    if (!searchMode || searchMode.engineName) {
      let engine = searchMode
        ? lazy.UrlbarSearchUtils.getEngineByName(searchMode.engineName)
        : lazy.UrlbarSearchUtils.getDefaultEngine(
            lazy.PrivateBrowsingUtils.isWindowPrivate(this.#input.window)
          );
      let icon = (await engine.getIconURL()) ?? SearchModeSwitcher.DEFAULT_ICON;
      return { label: engine.name, icon };
    }

    let mode = lazy.UrlbarUtils.LOCAL_SEARCH_MODES.find(
      m => m.source == searchMode.source
    );
    return {
      label: await this.#getSearchModeLabel(searchMode.source),
      icon: mode.icon,
    };
  }

  /**
   * Builds the popup and dispatches a rebuild event on the popup when finished.
   */
  async #buildSearchModeList() {
    // Remove all menuitems added.
    for (let item of this.#popup.querySelectorAll(
      ".searchmode-switcher-addEngine, .searchmode-switcher-installed, .searchmode-switcher-local"
    )) {
      item.remove();
    }

    let browser = this.#input.window.gBrowser;
    let separator = this.#popup.querySelector(
      "#searchmode-switcher-popup-footer-separator"
    );

    let openSearchEngines = lazy.OpenSearchManager.getEngines(
      browser.selectedBrowser
    );
    openSearchEngines = openSearchEngines.slice(0, MAX_OPENSEARCH_ENGINES);

    for (let engine of openSearchEngines) {
      let menuitem = this.#createButton(engine.title, engine.icon);
      menuitem.classList.add("searchmode-switcher-addEngine");
      menuitem.addEventListener("command", e => {
        this.#installOpenSearchEngine(e, engine);
      });
      this.#popup.insertBefore(menuitem, separator);
    }

    // Add engines installed.
    let engines = [];
    try {
      engines = await Services.search.getVisibleEngines();
    } catch {
      console.error("Failed to fetch engines");
    }

    let today = new Date().toISOString().slice(0, 10); // "YYYY-MM-DD" format
    for (let engine of engines) {
      if (engine.hideOneOffButton) {
        continue;
      }
      let icon = await engine.getIconURL();
      let menuitem = this.#createButton(engine.name, icon);
      menuitem.classList.add("searchmode-switcher-installed");
      menuitem.setAttribute("label", engine.name);

      let isNew = engine.isNewUntil && today <= engine.isNewUntil;
      if (isNew && engine.isAppProvided) {
        menuitem.setAttribute("badge", await lazy.searchModeNewBadge);
      }

      menuitem.addEventListener("command", e => {
        this.search({ engine, openEngineHomePage: e.shiftKey });
      });
      this.#popup.insertBefore(menuitem, separator);
    }

    // Add local options.
    for (let { source, pref, restrict } of lazy.UrlbarUtils
      .LOCAL_SEARCH_MODES) {
      if (!lazy.UrlbarPrefs.get(pref)) {
        continue;
      }
      let name = lazy.UrlbarUtils.getResultSourceName(source);
      let { icon } = await this.#getDisplayedEngineDetails({
        source,
        pref,
        restrict,
      });
      let menuitem = this.#createButton(name, icon);
      menuitem.id = `search-button-${name}`;
      menuitem.classList.add("searchmode-switcher-local");
      menuitem.addEventListener("command", () => {
        this.search({ restrict });
      });

      this.#input.document.l10n.setAttributes(
        menuitem,
        `urlbar-searchmode-${name}`,
        {
          restrict,
        }
      );

      menuitem.restrict = restrict;
      this.#popup.insertBefore(menuitem, separator);
    }

    this.#popup.dispatchEvent(new Event("rebuild"));
  }

  search({ engine = null, restrict = null, openEngineHomePage = false } = {}) {
    let search = "";
    /** @type {Parameters<UrlbarInput["search"]>[1]} */
    let opts = null;
    if (engine) {
      search = this.#input.value;
      opts = {
        searchEngine: engine,
        searchModeEntry: "searchbutton",
      };
    } else if (restrict) {
      search = restrict + " " + this.#input.value;
      opts = { searchModeEntry: "searchbutton" };
    }

    if (openEngineHomePage) {
      opts.focus = false;
      opts.startQuery = false;
    }

    this.#input.search(search, opts);

    if (openEngineHomePage) {
      this.#input.openEngineHomePage(search, {
        searchEngine: opts.searchEngine,
      });
    }

    this.#popup.hidePopup();

    if (engine) {
      // TODO do we really need to distinguish here?
      Glean.urlbarUnifiedsearchbutton.picked[
        engine.isConfigEngine ? "builtin_search" : "addon_search"
      ].add(1);
    } else if (restrict) {
      Glean.urlbarUnifiedsearchbutton.picked.local_search.add(1);
    } else {
      console.warn(
        `Unexpected search: ${JSON.stringify({ engine, restrict, openEngineHomePage })}`
      );
    }
  }

  #enableObservers() {
    Services.obs.addObserver(this, "browser-search-engine-modified", true);

    this.#toolbarbutton.addEventListener("focus", this);
    this.#toolbarbutton.addEventListener("keydown", this);

    this.#popup.addEventListener("popupshowing", this);
    this.#popup.addEventListener("popuphiding", this);

    let closebutton = this.#input.document.querySelector(
      "#searchmode-switcher-close"
    );
    closebutton.addEventListener("command", this);

    let prefsbutton = this.#input.document.querySelector(
      "#searchmode-switcher-popup-search-settings-button"
    );
    prefsbutton.addEventListener("command", this);
  }

  #disableObservers() {
    Services.obs.removeObserver(this, "browser-search-engine-modified");

    this.#toolbarbutton.removeEventListener("focus", this);
    this.#toolbarbutton.removeEventListener("keydown", this);

    this.#popup.removeEventListener("popupshowing", this);
    this.#popup.removeEventListener("popuphiding", this);

    let closebutton = this.#input.document.querySelector(
      "#searchmode-switcher-close"
    );
    closebutton.removeEventListener("command", this);

    let prefsbutton = this.#input.document.querySelector(
      "#searchmode-switcher-popup-search-settings-button"
    );
    prefsbutton.removeEventListener("command", this);
  }

  #createButton(label, icon) {
    let menuitem = this.#input.window.document.createXULElement("menuitem");
    menuitem.setAttribute("label", label);
    menuitem.setAttribute("class", "menuitem-iconic");
    menuitem.setAttribute("image", icon ?? DEFAULT_ENGINE_ICON);
    return menuitem;
  }

  async #installOpenSearchEngine(e, engine) {
    let topic = "browser-search-engine-modified";

    let observer = engineObj => {
      Services.obs.removeObserver(observer, topic);
      let eng = Services.search.getEngineByName(engineObj.wrappedJSObject.name);
      this.search({
        engine: eng,
        openEngineHomePage: e.shiftKey,
      });
    };
    Services.obs.addObserver(observer, topic);

    await lazy.SearchUIUtils.addOpenSearchEngine(
      engine.uri,
      engine.icon,
      this.#input.window.gBrowser.selectedBrowser.browsingContext
    );
  }
}
