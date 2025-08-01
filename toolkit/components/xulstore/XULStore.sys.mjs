/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Enables logging and shorter save intervals.
const debugMode = false;

// Delay when a change is made to when the file is saved.
// 30 seconds normally, or 3 seconds for testing
const WRITE_DELAY_MS = (debugMode ? 3 : 30) * 1000;

const XULSTORE_CID = Components.ID("{6f46b6f4-c8b1-4bd4-a4fa-9ebbed0753ea}");
const STOREDB_FILENAME = "xulstore.json";

/**
 * The XULStore retains XULElement attributes such as the sizing and styling. These are
 * stored in the user's profile directory inside of "xulstore.json". The JSON is
 * stored based on the chrome URL of the resource.
 *
 * For instance the "chrome://browser/content/browser.xhtml" main window sizing could be
 * stored like so:
 *
 * {
 *   "chrome://browser/content/browser.xhtml": {
 *     "main-window": {
 *       "screenX": "1104",
 *       "screenY": "25",
 *       "width": "1904",
 *       "height": "1612",
 *       "sizemode": "normal"
 *     },
 *     ...
 *   }
 * }
 *
 * The XULStore can only be loaded in the parent process. See the XULStore
 * and XULPersist C++ classes for how this is integrated from the C++ side.
 */
export function XULStore() {
  if (!Services.appinfo.inSafeMode) {
    this.load();
  }
}

XULStore.prototype = {
  classID: XULSTORE_CID,
  name: "XULStore",
  QueryInterface: ChromeUtils.generateQI([
    "nsINamed",
    "nsIObserver",
    "nsIXULStore",
    "nsISupportsWeakReference",
  ]),

  /* ---------- private members ---------- */

  /*
   * The format of _data is _data[docuri][elementid][attribute]. For example:
   *  {
   *      "chrome://blah/foo.xul" : {
   *                                    "main-window" : { aaa : 1, bbb : "c" },
   *                                    "barColumn"   : { ddd : 9, eee : "f" },
   *                                },
   *
   *      "chrome://foopy/b.xul" :  { ... },
   *      ...
   *  }
   */
  _data: {},
  _storeFile: null,
  _needsSaving: false,
  _saveAllowed: true,
  _writeTimer: Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer),

  load() {
    Services.obs.addObserver(this, "profile-before-change", true);

    try {
      this._storeFile = Services.dirsvc.get("ProfD", Ci.nsIFile);
    } catch (ex) {
      try {
        this._storeFile = Services.dirsvc.get("ProfDS", Ci.nsIFile);
      } catch (ex) {
        if (Services.startup.startingUp) {
          return;
        }
        throw new Error("Can't find profile directory.");
      }
    }
    this._storeFile.append(STOREDB_FILENAME);

    this.readFile();
  },

  observe(subject, topic) {
    this.writeFile();
    if (topic == "profile-before-change") {
      this._saveAllowed = false;
    }
  },

  /*
   * Internal function for logging debug messages to the Error Console window
   */
  log(message) {
    if (!debugMode) {
      return;
    }
    console.log("XULStore: " + message);
  },

  readFile() {
    try {
      this._data = JSON.parse(Cu.readUTF8File(this._storeFile));
    } catch (e) {
      this.log("Error reading JSON: " + e);
      // This exception could mean that the file didn't exist.
      // We'll just ignore the error and start with a blank slate.
    }
  },

  async writeFile() {
    if (!this._needsSaving) {
      return;
    }

    this._needsSaving = false;

    this.log("Writing to xulstore.json");

    try {
      await IOUtils.writeJSON(this._storeFile.path, this._data, {
        tmpPath: this._storeFile.path + ".tmp",
      });
    } catch (e) {
      this.log("Failed to write xulstore.json: " + e);
      throw e;
    }
  },

  markAsChanged() {
    if (this._needsSaving || !this._storeFile) {
      return;
    }

    // Don't write the file more than once every 30 seconds.
    this._needsSaving = true;
    this._writeTimer.init(this, WRITE_DELAY_MS, Ci.nsITimer.TYPE_ONE_SHOT);
  },

  /* ---------- interface implementation ---------- */

  persist(node, attr) {
    if (!node.id) {
      throw new Error("Node without ID passed into persist()");
    }

    if (node.localName == "window") {
      // TODO(emilio): This doesn't deal with <html>, and it probably should.
      this.log("Persisting attributes to windows is handled by AppWindow.");
      return;
    }

    const uri = node.ownerDocument.documentURI;
    const value = node.getAttribute(attr);

    // This matches XULPersist::Persist.
    // FIXME(emilio): Somehow expose that to script rather than copying it here?
    const persistedValue = value === null ? "-moz-missing\n" : value;
    this.setValue(uri, node.id, attr, persistedValue);
  },

  setValue(docURI, id, attr, value) {
    this.log(
      "Saving " + attr + "=" + value + " for id=" + id + ", doc=" + docURI
    );

    if (!this._saveAllowed) {
      Services.console.logStringMessage(
        "XULStore: Changes after profile-before-change are ignored!"
      );
      return;
    }

    // bug 319846 -- don't save really long attributes or values.
    if (id.length > 512 || attr.length > 512) {
      throw Components.Exception(
        "id or attribute name too long",
        Cr.NS_ERROR_ILLEGAL_VALUE
      );
    }

    if (value.length > 4096) {
      Services.console.logStringMessage(
        "XULStore: Warning, truncating long attribute value"
      );
      value = value.substr(0, 4096);
    }

    let obj = this._data;
    if (!(docURI in obj)) {
      obj[docURI] = {};
    }
    obj = obj[docURI];
    if (!(id in obj)) {
      obj[id] = {};
    }
    obj = obj[id];

    // Don't set the value if it is already set to avoid saving the file.
    if (attr in obj && obj[attr] == value) {
      return;
    }

    obj[attr] = value; // IE, this._data[docURI][id][attr] = value;

    this.markAsChanged();
  },

  hasValue(docURI, id, attr) {
    this.log(
      "has store value for id=" + id + ", attr=" + attr + ", doc=" + docURI
    );

    let ids = this._data[docURI];
    if (ids) {
      let attrs = ids[id];
      if (attrs) {
        return attr in attrs;
      }
    }

    return false;
  },

  getValue(docURI, id, attr) {
    this.log(
      "get store value for id=" + id + ", attr=" + attr + ", doc=" + docURI
    );

    let ids = this._data[docURI];
    if (ids) {
      let attrs = ids[id];
      if (attrs) {
        return attrs[attr] || "";
      }
    }

    return "";
  },

  removeValue(docURI, id, attr) {
    this.log(
      "remove store value for id=" + id + ", attr=" + attr + ", doc=" + docURI
    );

    if (!this._saveAllowed) {
      Services.console.logStringMessage(
        "XULStore: Changes after profile-before-change are ignored!"
      );
      return;
    }

    let ids = this._data[docURI];
    if (ids) {
      let attrs = ids[id];
      if (attrs && attr in attrs) {
        delete attrs[attr];

        if (!Object.getOwnPropertyNames(attrs).length) {
          delete ids[id];

          if (!Object.getOwnPropertyNames(ids).length) {
            delete this._data[docURI];
          }
        }

        this.markAsChanged();
      }
    }
  },

  removeDocument(docURI) {
    this.log("remove store values for doc=" + docURI);

    if (!this._saveAllowed) {
      Services.console.logStringMessage(
        "XULStore: Changes after profile-before-change are ignored!"
      );
      return;
    }

    if (this._data[docURI]) {
      delete this._data[docURI];
      this.markAsChanged();
    }
  },

  getIDsEnumerator(docURI) {
    this.log("Getting ID enumerator for doc=" + docURI);

    if (!(docURI in this._data)) {
      return new nsStringEnumerator([]);
    }

    let result = [];
    let ids = this._data[docURI];
    if (ids) {
      for (let id in this._data[docURI]) {
        result.push(id);
      }
    }

    return new nsStringEnumerator(result);
  },

  getAttributeEnumerator(docURI, id) {
    this.log("Getting attribute enumerator for id=" + id + ", doc=" + docURI);

    if (!(docURI in this._data) || !(id in this._data[docURI])) {
      return new nsStringEnumerator([]);
    }

    let attrs = [];
    for (let attr in this._data[docURI][id]) {
      attrs.push(attr);
    }

    return new nsStringEnumerator(attrs);
  },
};

function nsStringEnumerator(items) {
  this._items = items;
}

nsStringEnumerator.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIStringEnumerator"]),
  _nextIndex: 0,
  [Symbol.iterator]() {
    return this._items.values();
  },
  hasMore() {
    return this._nextIndex < this._items.length;
  },
  getNext() {
    if (!this.hasMore()) {
      throw Components.Exception("", Cr.NS_ERROR_NOT_AVAILABLE);
    }
    return this._items[this._nextIndex++];
  },
};
