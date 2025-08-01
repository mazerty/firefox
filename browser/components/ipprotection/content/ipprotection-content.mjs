/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { MozLitElement } from "chrome://global/content/lit-utils.mjs";
import { html, classMap } from "chrome://global/content/vendor/lit.all.mjs";

// eslint-disable-next-line import/no-unassigned-import
import "chrome://browser/content/ipprotection/ipprotection-header.mjs";
// eslint-disable-next-line import/no-unassigned-import
import "chrome://browser/content/ipprotection/ipprotection-signedout.mjs";
// eslint-disable-next-line import/no-unassigned-import
import "chrome://global/content/elements/moz-toggle.mjs";

const DEFAULT_TIME_CONNECTED = "00:00:00";

export default class IPProtectionContentElement extends MozLitElement {
  static queries = {
    headerEl: "ipprotection-header",
    signedOutEl: "ipprotection-signedout",
    statusCardEl: "#status-card",
    connectionTitleEl: "#connection-title",
    connectionToggleEl: "#connection-toggle",
    upgradeEl: "#upgrade-vpn-content",
    supportLinkEl: "#vpn-support-link",
  };

  static properties = {
    state: { type: Object },
  };

  constructor() {
    super();

    this.state = {};

    this.keyListener = this.#keyListener.bind(this);
  }

  connectedCallback() {
    super.connectedCallback();
    this.dispatchEvent(new CustomEvent("IPProtection:Init", { bubbles: true }));

    this.addEventListener("keydown", this.keyListener, { capture: true });
  }

  disconnectedCallback() {
    super.disconnectedCallback();

    this.removeEventListener("keydown", this.keyListener, { capture: true });
  }

  handleClickSupportLink(event) {
    if (event.target === this.supportLinkEl) {
      this.dispatchEvent(
        new CustomEvent("IPProtection:Close", { bubbles: true })
      );
    }
  }

  handleToggleConnect(event) {
    let isEnabled = event.target.pressed;

    if (isEnabled) {
      this.dispatchEvent(
        new CustomEvent("IPProtection:UserEnable", { bubbles: true })
      );
    } else {
      this.dispatchEvent(
        new CustomEvent("IPProtection:UserDisable", { bubbles: true })
      );
    }
  }

  handleUpgrade() {
    // TODO: Handle click of Upgrade button - Bug 1975317
  }

  focus() {
    if (!this.state.isSignedIn) {
      this.signedOutEl?.focus();
    } else {
      this.connectionToggleEl?.focus();
    }
  }

  #keyListener(event) {
    let keyCode = event.code;
    switch (keyCode) {
      case "ArrowUp":
      // Intentional fall-through
      case "ArrowDown": {
        event.stopPropagation();
        event.preventDefault();

        let direction =
          keyCode == "ArrowDown"
            ? Services.focus.MOVEFOCUS_FORWARD
            : Services.focus.MOVEFOCUS_BACKWARD;
        Services.focus.moveFocus(
          window,
          null,
          direction,
          Services.focus.FLAG_BYKEY
        );
        break;
      }
    }
  }

  statusCardTemplate() {
    const statusCardL10nId = this.state.isProtectionEnabled
      ? "ipprotection-connection-status-on"
      : "ipprotection-connection-status-off";
    const toggleL10nId = this.state.isProtectionEnabled
      ? "ipprotection-toggle-active"
      : "ipprotection-toggle-inactive";

    // TODO: update timer and its starting value according to the protectionEnabledSince property (Bug 1972460)
    const timeConnected = DEFAULT_TIME_CONNECTED;

    return html` <div
      id="status-card"
      class=${classMap({
        "is-enabled": this.state.isProtectionEnabled,
      })}
    >
      <div id="connection-wrapper" class="status-card-section">
        <span
          id="connection-icon"
          class=${classMap({
            "is-enabled": this.state.isProtectionEnabled,
            "status-card-icon": true,
          })}
        ></span>
        <span id="connection-details-and-toggle">
          <div id="connection-details">
            <span
              id="connection-title"
              class="status-card-section-title heading-medium"
              data-l10n-id=${statusCardL10nId}
            ></span>
            <span id="connection-time" class="text-deemphasized"
              >${timeConnected}</span
            >
          </div>
          <div>
            <moz-toggle
              id="connection-toggle"
              data-l10n-id=${toggleL10nId}
              @click=${this.handleToggleConnect}
              ?pressed=${this.state.isProtectionEnabled}
            ></moz-toggle>
          </div>
        </span>
      </div>
      <div class="status-card-divider"></div>
      <div id="location-wrapper" class="status-card-section">
        <div
          id="location-title-wrapper"
          class="status-card-section-title status-card-section-item"
        >
          <span id="location-title-icon" class="status-card-icon"></span>
          <span
            id="location-title"
            class="heading-medium"
            data-l10n-id="ipprotection-location-title"
          ></span>
        </div>
        <div id="location-label" class="status-card-section-item">
          <!--TODO: add location flag icon (Bug 1976769)-->
          <span id="location-flag"></span>
          <span
            id="location-name"
            class="text-deemphasized status-card-section-item"
            >${this.state.location}</span
          >
        </div>
      </div>
    </div>`;
  }

  mainContentTemplate() {
    // TODO: Update support-page with new SUMO link for Mozilla VPN - Bug 1975474
    if (!this.state.isSignedIn) {
      return html` <ipprotection-signedout></ipprotection-signedout> `;
    }
    return html`
      ${this.statusCardTemplate()}
      <div id="upgrade-vpn-content">
        <h2 id="upgrade-vpn-title" data-l10n-id="upgrade-vpn-title"></h2>
        <p
          id="upgrade-vpn-paragraph"
          data-l10n-id="upgrade-vpn-paragraph"
          @click=${this.handleClickSupportLink}
        >
          <a
            id="vpn-support-link"
            is="moz-support-link"
            data-l10n-name="learn-more-vpn"
            support-page="test"
          ></a>
        </p>
        <moz-button
          id="upgrade-vpn-button"
          @click=${this.handleUpgrade}
          type="secondary"
          data-l10n-id="upgrade-vpn-button"
        ></moz-button>
      </div>
    `;
  }

  render() {
    // TODO: Conditionally render post-upgrade subview within #ipprotection-content-wrapper - Bug 1973813
    return html`
      <link
        rel="stylesheet"
        href="chrome://browser/content/ipprotection/ipprotection-content.css"
      />
      <ipprotection-header titleId="ipprotection-title"></ipprotection-header>
      <hr />
      <div id="ipprotection-content-wrapper">${this.mainContentTemplate()}</div>
    `;
  }
}

customElements.define("ipprotection-content", IPProtectionContentElement);
