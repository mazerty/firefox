/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ASRouter: "resource:///modules/asrouter/ASRouter.sys.mjs",
  CustomizableUI: "resource:///modules/CustomizableUI.sys.mjs",
  IPProtectionPanel:
    "resource:///modules/ipprotection/IPProtectionPanel.sys.mjs",
  IPProtectionService:
    "resource:///modules/ipprotection/IPProtectionService.sys.mjs",
  requestIdleCallback: "resource://gre/modules/Timer.sys.mjs",
  cancelIdleCallback: "resource://gre/modules/Timer.sys.mjs",
});

const FXA_WIDGET_ID = "fxa-toolbar-menu-button";
const EXT_WIDGET_ID = "unified-extensions-button";

/**
 * IPProtectionWidget is the class for the singleton IPProtection, which
 * exposes init and uninit for app startup.
 *
 * It is a minimal manager for creating and removing a CustomizableUI widget
 * for IP protection features.
 *
 * It maintains the state of the panels and updates them when the
 * panel is shown or hidden.
 */
class IPProtectionWidget {
  static WIDGET_ID = "ipprotection-button";
  static PANEL_ID = "PanelUI-ipprotection";

  static ENABLED_PREF = "browser.ipProtection.enabled";

  #enabled = true;
  #created = false;
  #destroyed = false;
  #panels = new WeakMap();

  constructor() {
    this.updateEnabled = this.#updateEnabled.bind(this);
    this.sendReadyTrigger = this.#sendReadyTrigger.bind(this);
  }

  /**
   * Creates the widget if the feature is enabled and
   * the widget has not already been created.
   *
   * @param {Window} _window - new browser window.
   */
  init(_window) {
    if (!this.isEnabled) {
      return;
    }

    if (!this.#created) {
      this.#createWidget();
    }

    lazy.IPProtectionService.init();
  }

  /**
   * Destroys the widget and prevents any updates.
   */
  uninit() {
    this.#destroyWidget();
    this.#uninitPanels();
    lazy.IPProtectionService.uninit();
    this.#destroyed = true;
  }

  /**
   * Opens the panel in the given window.
   *
   * @param {Window} window - which window to open the panel in.
   * @returns {Promise<void>}
   */
  async openPanel(window) {
    if (!this.#created || !window?.PanelUI) {
      return;
    }

    let widget = lazy.CustomizableUI.getWidget(IPProtectionWidget.WIDGET_ID);
    let anchor = widget.forWindow(window).anchor;
    await window.PanelUI.showSubView(IPProtectionWidget.PANEL_ID, anchor);
  }

  /**
   * Updates the toolbar icon to reflect the VPN connection status
   *
   * @param {XULElement} toolbaritem - toolbaritem to update
   * @param {object} status - VPN connection status
   */
  updateIconStatus(toolbaritem, status = { isActive: false, isError: false }) {
    let isActive = status.isActive;
    let isError = status.isError;

    if (isError) {
      toolbaritem.classList.remove("ipprotection-on");
      toolbaritem.classList.add("ipprotection-error");
    } else if (isActive) {
      toolbaritem.classList.remove("ipprotection-error");
      toolbaritem.classList.add("ipprotection-on");
    } else {
      toolbaritem.classList.remove("ipprotection-error");
      toolbaritem.classList.remove("ipprotection-on");
    }
  }

  /**
   * Creates the CustomizableUI widget.
   */
  #createWidget() {
    const onViewShowing = this.#onViewShowing.bind(this);
    const onViewHiding = this.#onViewHiding.bind(this);
    const onBeforeCreated = this.#onBeforeCreated.bind(this);
    const onCreated = this.#onCreated.bind(this);
    lazy.CustomizableUI.createWidget({
      id: IPProtectionWidget.WIDGET_ID,
      l10nId: IPProtectionWidget.WIDGET_ID,
      type: "view",
      viewId: IPProtectionWidget.PANEL_ID,
      overflows: false,
      onViewShowing,
      onViewHiding,
      onBeforeCreated,
      onCreated,
    });

    this.#placeWidget();

    this.#created = true;
  }

  /**
   * Places the widget in the nav bar, next to the FxA widget.
   */
  #placeWidget() {
    let prevWidget = lazy.CustomizableUI.getPlacementOfWidget(FXA_WIDGET_ID);
    if (!prevWidget) {
      // Fallback to unremovable extensions button if fxa button isn't available.
      prevWidget = lazy.CustomizableUI.getPlacementOfWidget(EXT_WIDGET_ID);
    }

    lazy.CustomizableUI.addWidgetToArea(
      IPProtectionWidget.WIDGET_ID,
      lazy.CustomizableUI.AREA_NAVBAR,
      prevWidget.position - 1
    );
  }

  /**
   * Destroys the widget if it has been created.
   *
   * This will not remove the pref listeners, so the widget
   * can be recreated later.
   */
  #destroyWidget() {
    if (!this.#created) {
      return;
    }
    this.#destroyPanels();
    lazy.CustomizableUI.destroyWidget(IPProtectionWidget.WIDGET_ID);
    this.#created = false;
    if (this.readyTriggerIdleCallback) {
      lazy.cancelIdleCallback(this.readyTriggerIdleCallback);
    }
  }

  /**
   * Get the IPProtectionPanel for q given window.
   *
   * @param {Window} window - which window to get the panel for.
   * @returns {IPProtectionPanel}
   */
  getPanel(window) {
    if (!this.#created || !window?.PanelUI) {
      return null;
    }

    return this.#panels.get(window);
  }

  /**
   * Remove all panels content, but maintains state for if the widget is
   * re-enabled in the same window.
   *
   * Panels will only be removed from the WeakMap if their window is closed.
   */
  #destroyPanels() {
    let panels = ChromeUtils.nondeterministicGetWeakMapKeys(this.#panels);
    for (let panel of panels) {
      this.#panels.get(panel).destroy();
    }
  }

  /**
   * Uninit all panels and clear the WeakMap.
   */
  #uninitPanels() {
    let panels = ChromeUtils.nondeterministicGetWeakMapKeys(this.#panels);
    for (let panel of panels) {
      this.#panels.get(panel).uninit();
    }
    this.#panels = new WeakMap();
  }

  /**
   * Sets whether the feature pref is enabled and not destroyed.
   *
   * If enabled, creates the widget if it hasn't been created yet.
   * If not enabled, destroys the widget if it has been created.
   */
  #updateEnabled() {
    this.#enabled = this.isEnabled && !this.#destroyed;
    if (this.#enabled && !this.#created) {
      this.#createWidget();
      lazy.IPProtectionService.init();
    } else if (!this.#enabled && this.#created) {
      this.#destroyWidget();
      lazy.IPProtectionService.uninit();
    }
  }

  /**
   * Updates the state of the panel before it is shown.
   *
   * @param {Event} event - the panel shown.
   */
  #onViewShowing(event) {
    let { ownerGlobal } = event.target;
    if (this.#panels.has(ownerGlobal)) {
      let panel = this.#panels.get(ownerGlobal);
      panel.showing(event.target);
    }
  }

  /**
   * Updates the panels visibility.
   *
   * @param {Event} event - the panel hidden.
   */
  #onViewHiding(event) {
    let { ownerGlobal } = event.target;
    if (this.#panels.has(ownerGlobal)) {
      let panel = this.#panels.get(ownerGlobal);
      panel.hiding();
    }
  }

  /**
   * Creates a new IPProtectionPanel for a browser window.
   *
   * @param {Document} doc - the document containing the panel.
   */
  #onBeforeCreated(doc) {
    let { ownerGlobal } = doc;
    if (!this.#panels.has(ownerGlobal)) {
      let panel = new lazy.IPProtectionPanel(ownerGlobal);
      this.#panels.set(ownerGlobal, panel);
    }
  }

  /**
   * Gets the toolbaritem after the widget has been created and
   * adds content to the panel.
   *
   * @param {XULElement} _toolbaritem - the widget toolbaritem.
   */
  #onCreated(_toolbaritem) {
    this.readyTriggerIdleCallback = lazy.requestIdleCallback(
      this.sendReadyTrigger
    );
  }

  async #sendReadyTrigger() {
    await lazy.ASRouter.waitForInitialized;
    const win = Services.wm.getMostRecentBrowserWindow();
    const browser = win?.gBrowser?.selectedBrowser;
    await lazy.ASRouter.sendTriggerMessage({
      browser,
      id: "ipProtectionReady",
    });
  }
}

const IPProtection = new IPProtectionWidget();

XPCOMUtils.defineLazyPreferenceGetter(
  IPProtection,
  "isEnabled",
  IPProtectionWidget.ENABLED_PREF,
  false,
  IPProtection.updateEnabled
);

export { IPProtection, IPProtectionWidget };
