/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * The panel is initialized based on data given in the js object passed
 * as window.arguments[0]. The object must have the following fields set:
 *   @ action (String). Possible values:
 *     - "add" - for adding a new item.
 *       @ type (String). Possible values:
 *         - "bookmark"
 *         - "folder"
 *           @ URIList (Array of nsIURI objects) - optional, list of uris to
 *             be bookmarked under the new folder.
 *       @ uri (nsIURI object) - optional, the default uri for the new item.
 *         The property is not used for the "folder with items" type.
 *       @ title (String) - optional, the default title for the new item.
 *       @ defaultInsertionPoint (InsertionPoint JS object) - optional, the
 *         default insertion point for the new item.
 *       @ keyword (String) - optional, the default keyword for the new item.
 *       @ postData (String) - optional, POST data to accompany the keyword.
 *       @ charSet (String) - optional, character-set to accompany the keyword.
 *      Notes:
 *        1) If |uri| is set for a bookmark and |title| isn't,
 *           the dialog will query the history tables for the title associated
 *           with the given uri. If the dialog is set to adding a folder with
 *           bookmark items under it (see URIList), a default static title is
 *           used ("[Folder Name]").
 *        2) The index field of the default insertion point is ignored if
 *           the folder picker is shown.
 *     - "edit" - for editing a bookmark item or a folder.
 *       @ type (String). Possible values:
 *         - "bookmark"
 *           @ node (an nsINavHistoryResultNode object) - a node representing
 *             the bookmark.
 *         - "folder"
 *           @ node (an nsINavHistoryResultNode object) - a node representing
 *             the folder.
 *   @ hiddenRows (Strings array) - optional, list of rows to be hidden
 *     regardless of the item edited or added by the dialog.
 *     Possible values:
 *     - "title"
 *     - "location"
 *     - "keyword"
 *     - "tags"
 *     - "folderPicker" - hides both the tree and the menu.
 *
 * window.arguments[0].bookmarkGuid is set to the guid of the item, if the
 * dialog is accepted.
 */

/* import-globals-from editBookmark.js */

/* Shared Places Import - change other consumers if you change this: */
var { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);
ChromeUtils.defineESModuleGetters(this, {
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});
XPCOMUtils.defineLazyScriptGetter(
  this,
  "PlacesTreeView",
  "chrome://browser/content/places/treeView.js"
);
XPCOMUtils.defineLazyScriptGetter(
  this,
  ["PlacesInsertionPoint", "PlacesController", "PlacesControllerDragHelper"],
  "chrome://browser/content/places/controller.js"
);
/* End Shared Places Import */

const BOOKMARK_ITEM = 0;
const BOOKMARK_FOLDER = 1;

const ACTION_EDIT = 0;
const ACTION_ADD = 1;

var BookmarkPropertiesPanel = {
  /** UI Text Strings */
  __strings: null,
  get _strings() {
    if (!this.__strings) {
      this.__strings = document.getElementById("stringBundle");
    }
    return this.__strings;
  },

  _action: null,
  _itemType: null,
  _uri: null,
  _title: "",
  _URIs: [],
  _keyword: "",
  _postData: null,
  _charSet: "",

  _defaultInsertionPoint: null,
  _hiddenRows: [],

  /**
   * @returns {string}
   *   This method returns the correct label for the dialog's "accept"
   *   button based on the variant of the dialog.
   */
  _getAcceptLabel: function BPP__getAcceptLabel() {
    return this._strings.getString("dialogAcceptLabelSaveItem");
  },

  /**
   * @returns {string}
   *   This method returns the correct title for the current variant
   *   of this dialog.
   */
  _getDialogTitle: function BPP__getDialogTitle() {
    if (this._action == ACTION_ADD) {
      if (this._itemType == BOOKMARK_ITEM) {
        return this._strings.getString("dialogTitleAddNewBookmark2");
      }

      // add folder
      if (this._itemType != BOOKMARK_FOLDER) {
        throw new Error("Unknown item type");
      }
      if (this._URIs.length) {
        return this._strings.getString("dialogTitleAddMulti");
      }

      return this._strings.getString("dialogTitleAddBookmarkFolder");
    }
    if (this._action == ACTION_EDIT) {
      if (this._itemType === BOOKMARK_ITEM) {
        return this._strings.getString("dialogTitleEditBookmark2");
      }

      return this._strings.getString("dialogTitleEditBookmarkFolder");
    }
    return "";
  },

  /**
   * Determines the initial data for the item edited or added by this dialog
   */
  async _determineItemInfo() {
    let dialogInfo = window.arguments[0];
    this._action = dialogInfo.action == "add" ? ACTION_ADD : ACTION_EDIT;
    this._hiddenRows = dialogInfo.hiddenRows ? dialogInfo.hiddenRows : [];
    if (this._action == ACTION_ADD) {
      if (!("type" in dialogInfo)) {
        throw new Error("missing type property for add action");
      }

      if ("title" in dialogInfo) {
        this._title = dialogInfo.title;
      }

      if ("defaultInsertionPoint" in dialogInfo) {
        this._defaultInsertionPoint = dialogInfo.defaultInsertionPoint;
      } else {
        let parentGuid = await PlacesUIUtils.defaultParentGuid;
        this._defaultInsertionPoint = new PlacesInsertionPoint({
          parentGuid,
        });
      }

      switch (dialogInfo.type) {
        case "bookmark":
          this._itemType = BOOKMARK_ITEM;
          if ("uri" in dialogInfo) {
            if (!(dialogInfo.uri instanceof Ci.nsIURI)) {
              throw new Error("uri property should be a uri object");
            }
            this._uri = dialogInfo.uri;
            if (typeof this._title != "string") {
              this._title =
                (await PlacesUtils.history.fetch(this._uri)) || this._uri.spec;
            }
          } else {
            this._uri = Services.io.newURI("about:blank");
            this._title = this._strings.getString("newBookmarkDefault");
            this._dummyItem = true;
          }

          if ("keyword" in dialogInfo) {
            this._keyword = dialogInfo.keyword;
            this._isAddKeywordDialog = true;
            if ("postData" in dialogInfo) {
              this._postData = dialogInfo.postData;
            }
            if ("charSet" in dialogInfo) {
              this._charSet = dialogInfo.charSet;
            }
          }
          break;

        case "folder":
          this._itemType = BOOKMARK_FOLDER;
          if (!this._title) {
            if ("URIList" in dialogInfo) {
              this._title = this._strings.getString("bookmarkAllTabsDefault");
              this._URIs = dialogInfo.URIList;
            } else {
              this._title = this._strings.getString("newFolderDefault");
              this._dummyItem = true;
            }
          }
          break;
      }
    } else {
      // edit
      this._node = dialogInfo.node;
      this._title = this._node.title;
      if (PlacesUtils.nodeIsFolderOrShortcut(this._node)) {
        this._itemType = BOOKMARK_FOLDER;
      } else if (PlacesUtils.nodeIsURI(this._node)) {
        this._itemType = BOOKMARK_ITEM;
      }
    }
  },

  /**
   * This method should be called by the onload of the Bookmark Properties
   * dialog to initialize the state of the panel.
   */
  async onDialogLoad() {
    document.addEventListener("dialogaccept", () => this.onDialogAccept());
    document.addEventListener("dialogcancel", () => this.onDialogCancel());
    window.addEventListener("unload", () => this.onDialogUnload());

    // Disable the buttons until we have all the information required.
    let acceptButton = document
      .getElementById("bookmarkpropertiesdialog")
      .getButton("accept");
    acceptButton.disabled = true;
    await this._determineItemInfo();
    document.title = this._getDialogTitle();

    // Set adjustable title
    let title = { raw: document.title };
    document.documentElement.setAttribute("headertitle", JSON.stringify(title));

    let iconUrl = this._getIconUrl();
    if (iconUrl) {
      document.documentElement.style.setProperty(
        "--icon-url",
        `url(${iconUrl})`
      );
    }

    await this._initDialog();
  },

  _getIconUrl() {
    let url = "chrome://browser/skin/bookmark-hollow.svg";

    if (this._action === ACTION_EDIT && this._itemType === BOOKMARK_ITEM) {
      url = window.arguments[0]?.node?.icon;
    }

    return url;
  },

  /**
   * Initializes the dialog, gathering the required bookmark data. This function
   * will enable the accept button (if appropraite) when it is complete.
   */
  async _initDialog() {
    let acceptButton = document
      .getElementById("bookmarkpropertiesdialog")
      .getButton("accept");
    acceptButton.label = this._getAcceptLabel();
    let acceptButtonDisabled = false;

    // Since elements can be unhidden asynchronously, we must observe their
    // mutations and resize the dialog accordingly.
    this._mutationObserver = new MutationObserver(mutations => {
      for (let { target, oldValue } of mutations) {
        let hidden = target.hasAttribute("hidden");
        let wasHidden = oldValue !== null;
        if (target.classList.contains("hideable") && hidden != wasHidden) {
          // To support both kind of dialogs (window and dialog-box) we need
          // both resizeBy and sizeToContent, otherwise either the dialog
          // doesn't resize, or it gets empty unused space.
          if (hidden) {
            let diff = this._mutationObserver._heightsById.get(target.id);
            window.resizeBy(0, -diff);
          } else {
            let diff = target.getBoundingClientRect().height;
            this._mutationObserver._heightsById.set(target.id, diff);
            window.resizeBy(0, diff);
          }
          window.sizeToContent();
        }
      }
    });
    this._mutationObserver._heightsById = new Map();
    this._mutationObserver.observe(document, {
      subtree: true,
      attributeOldValue: true,
      attributeFilter: ["hidden"],
    });

    switch (this._action) {
      case ACTION_EDIT: {
        await gEditItemOverlay.initPanel({
          node: this._node,
          hiddenRows: this._hiddenRows,
          focusedElement: "first",
        });
        acceptButtonDisabled = gEditItemOverlay.readOnly;
        break;
      }
      case ACTION_ADD: {
        this._node = await this._promiseNewItem();

        // Edit the new item
        await gEditItemOverlay.initPanel({
          node: this._node,
          hiddenRows: this._hiddenRows,
          postData: this._postData,
          focusedElement: "first",
          addedMultipleBookmarks: this._node.children?.length > 1,
        });

        // Empty location field if the uri is about:blank, this way inserting a new
        // url will be easier for the user, Accept button will be automatically
        // disabled by the input listener until the user fills the field.
        let locationField = this._element("locationField");
        if (locationField.value == "about:blank") {
          locationField.value = "";
        }

        // if this is an uri related dialog disable accept button until
        // the user fills an uri value.
        if (this._itemType == BOOKMARK_ITEM) {
          acceptButtonDisabled = !this._inputIsValid();
        }
        break;
      }
    }

    if (!gEditItemOverlay.readOnly) {
      // Listen on uri fields to enable accept button if input is valid
      if (this._itemType == BOOKMARK_ITEM) {
        this._element("locationField").addEventListener("input", this);
        if (this._isAddKeywordDialog) {
          this._element("keywordField").addEventListener("input", this);
        }
      }
    }
    // Only enable the accept button once we've finished everything.
    acceptButton.disabled = acceptButtonDisabled;
  },

  // EventListener
  handleEvent: function BPP_handleEvent(aEvent) {
    var target = aEvent.target;
    switch (aEvent.type) {
      case "input":
        if (
          target.id == "editBMPanel_locationField" ||
          target.id == "editBMPanel_keywordField"
        ) {
          // Check uri fields to enable accept button if input is valid
          document
            .getElementById("bookmarkpropertiesdialog")
            .getButton("accept").disabled = !this._inputIsValid();
        }
        break;
    }
  },

  // nsISupports
  QueryInterface: ChromeUtils.generateQI([]),

  _element: function BPP__element(aID) {
    return document.getElementById("editBMPanel_" + aID);
  },

  onDialogUnload() {
    // gEditItemOverlay does not exist anymore here, so don't rely on it.
    this._mutationObserver.disconnect();
    delete this._mutationObserver;

    // Calling removeEventListener with arguments which do not identify any
    // currently registered EventListener on the EventTarget has no effect.
    this._element("locationField").removeEventListener("input", this);
    this._element("keywordField").removeEventListener("input", this);
  },

  onDialogAccept() {
    // We must blur current focused element to save its changes correctly
    document.commandDispatcher.focusedElement?.blur();

    // Get the states to compare bookmark and editedBookmark
    window.arguments[0].bookmarkState = gEditItemOverlay._bookmarkState;

    // We have to uninit the panel first, otherwise late changes could force it
    // to commit more transactions.
    gEditItemOverlay.uninitPanel(true);

    window.arguments[0].bookmarkGuid = this._node.bookmarkGuid;
  },

  onDialogCancel() {
    // We have to uninit the panel first, otherwise late changes could force it
    // to commit more transactions.
    gEditItemOverlay.uninitPanel(true);
  },

  /**
   * This method checks to see if the input fields are in a valid state.
   *
   * @returns {boolean} true if the input is valid, false otherwise
   */
  _inputIsValid: function BPP__inputIsValid() {
    if (
      this._itemType == BOOKMARK_ITEM &&
      !this._containsValidURI("locationField")
    ) {
      return false;
    }
    if (
      this._isAddKeywordDialog &&
      !this._element("keywordField").value.length
    ) {
      return false;
    }

    return true;
  },

  /**
   * Determines whether the input with the given ID contains a
   * string that can be converted into an nsIURI.
   *
   * @param {number} aTextboxID
   *        the ID of the textbox element whose contents we'll test
   *
   * @returns {boolean} true if the textbox contains a valid URI string, false otherwise
   */
  _containsValidURI: function BPP__containsValidURI(aTextboxID) {
    try {
      var value = this._element(aTextboxID).value;
      if (value) {
        Services.uriFixup.getFixupURIInfo(value);
        return true;
      }
    } catch (e) {}
    return false;
  },

  /**
   * [New Item Mode] Get the insertion point details for the new item, given
   * dialog state and opening arguments.
   *
   * @returns {Array}
   *   The container-identifier and insertion-index are returned separately in
   *   the form of [containerIdentifier, insertionIndex]
   */
  async _getInsertionPointDetails() {
    return [
      await this._defaultInsertionPoint.getIndex(),
      this._defaultInsertionPoint.guid,
    ];
  },

  async _promiseNewItem() {
    let [index, parentGuid] = await this._getInsertionPointDetails();

    let info = { parentGuid, index, title: this._title };
    if (this._itemType == BOOKMARK_ITEM) {
      info.url = this._uri;
      if (this._keyword) {
        info.keyword = this._keyword;
      }
      if (this._postData) {
        info.postData = this._postData;
      }

      if (this._charSet) {
        PlacesUIUtils.setCharsetForPage(this._uri, this._charSet, window).catch(
          console.error
        );
      }
    } else if (this._itemType == BOOKMARK_FOLDER) {
      // NewFolder requires a url rather than uri.
      info.children = this._URIs.map(item => {
        return { url: item.uri, title: item.title };
      });
    } else {
      throw new Error(`unexpected value for _itemType:  ${this._itemType}`);
    }
    return Object.freeze({
      index,
      bookmarkGuid: PlacesUtils.bookmarks.unsavedGuid,
      title: this._title,
      uri: this._uri ? this._uri.spec : "",
      type:
        this._itemType == BOOKMARK_ITEM
          ? Ci.nsINavHistoryResultNode.RESULT_TYPE_URI
          : Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER,
      parent: {
        bookmarkGuid: parentGuid,
        type: Ci.nsINavHistoryResultNode.RESULT_TYPE_FOLDER,
      },
      children: info.children,
    });
  },
};

document.addEventListener("DOMContentLoaded", function () {
  // Content initialization is asynchronous, thus set mozSubdialogReady
  // immediately to properly wait for it.
  document.mozSubdialogReady = BookmarkPropertiesPanel.onDialogLoad()
    .catch(ex => console.error(`Failed to initialize dialog: ${ex}`))
    .then(() => window.sizeToContent());
});
