/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80 filetype=javascript: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Handles the Downloads panel shared methods and data access.
 *
 * This file includes the following constructors and global objects:
 *
 * DownloadsCommon
 * This object is exposed directly to the consumers of this JavaScript module,
 * and provides shared methods for all the instances of the user interface.
 *
 * DownloadsData
 * Retrieves the list of past and completed downloads from the underlying
 * Downloads API data, and provides asynchronous notifications allowing
 * to build a consistent view of the available data.
 *
 * DownloadsIndicatorData
 * This object registers itself with DownloadsData as a view, and transforms the
 * notifications it receives into overall status data, that is then broadcast to
 * the registered download status indicators.
 */

// Globals

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.sys.mjs",
  DownloadHistory: "resource://gre/modules/DownloadHistory.sys.mjs",
  DownloadUtils: "resource://gre/modules/DownloadUtils.sys.mjs",
  Downloads: "resource://gre/modules/Downloads.sys.mjs",
  NetUtil: "resource://gre/modules/NetUtil.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
});

XPCOMUtils.defineLazyServiceGetters(lazy, {
  gClipboardHelper: [
    "@mozilla.org/widget/clipboardhelper;1",
    "nsIClipboardHelper",
  ],
  gMIMEService: ["@mozilla.org/mime;1", "nsIMIMEService"],
});

ChromeUtils.defineLazyGetter(lazy, "DownloadsLogger", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  let consoleOptions = {
    maxLogLevelPref: "browser.download.loglevel",
    prefix: "Downloads",
  };
  return new ConsoleAPI(consoleOptions);
});

XPCOMUtils.defineLazyPreferenceGetter(
  lazy,
  "gAlwaysOpenPanel",
  "browser.download.alwaysOpenPanel",
  true
);

const kDownloadsStringBundleUrl =
  "chrome://browser/locale/downloads/downloads.properties";

const kDownloadsFluentStrings = new Localization(
  ["browser/downloads.ftl"],
  true
);

const kDownloadsStringsRequiringFormatting = {
  contentAnalysisNoAgentError: true,
  contentAnalysisInvalidAgentSignatureError: true,
  contentAnalysisUnspecifiedError: true,
  contentAnalysisTimeoutError: true,
  sizeWithUnits: true,
  statusSeparator: true,
  statusSeparatorBeforeNumber: true,
};

const kMaxHistoryResultsForLimitedView = 42;

const kPrefBranch = Services.prefs.getBranch("browser.download.");

const kGenericContentTypes = [
  "application/octet-stream",
  "binary/octet-stream",
  "application/unknown",
];

var PrefObserver = {
  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),
  getPref(name) {
    try {
      switch (typeof this.prefs[name]) {
        case "boolean":
          return kPrefBranch.getBoolPref(name);
      }
    } catch (ex) {}
    return this.prefs[name];
  },
  observe(aSubject, aTopic, aData) {
    if (this.prefs.hasOwnProperty(aData)) {
      delete this[aData];
      this[aData] = this.getPref(aData);
    }
  },
  register(prefs) {
    this.prefs = prefs;
    kPrefBranch.addObserver("", this);
    for (let key in prefs) {
      let name = key;
      ChromeUtils.defineLazyGetter(this, name, function () {
        return PrefObserver.getPref(name);
      });
    }
  },
};

PrefObserver.register({
  // prefName: defaultValue
  openInSystemViewerContextMenuItem: true,
  alwaysOpenInSystemViewerContextMenuItem: true,
});

// DownloadsCommon

/**
 * This object is exposed directly to the consumers of this JavaScript module,
 * and provides shared methods for all the instances of the user interface.
 */
export var DownloadsCommon = {
  // The following legacy constants are still returned by stateOfDownload, but
  // individual properties of the Download object should normally be used.
  DOWNLOAD_NOTSTARTED: -1,
  DOWNLOAD_DOWNLOADING: 0,
  DOWNLOAD_FINISHED: 1,
  DOWNLOAD_FAILED: 2,
  DOWNLOAD_CANCELED: 3,
  DOWNLOAD_PAUSED: 4,
  DOWNLOAD_BLOCKED_PARENTAL: 6,
  DOWNLOAD_DIRTY: 8,
  DOWNLOAD_BLOCKED_POLICY: 9,
  DOWNLOAD_BLOCKED_CONTENT_ANALYSIS: 10,

  // The following are the possible values of the "attention" property.
  ATTENTION_NONE: "",
  ATTENTION_SUCCESS: "success",
  ATTENTION_INFO: "info",
  ATTENTION_WARNING: "warning",
  ATTENTION_SEVERE: "severe",

  // Bit flags for the attentionSuppressed property.
  SUPPRESS_NONE: 0,
  SUPPRESS_PANEL_OPEN: 1,
  SUPPRESS_ALL_DOWNLOADS_OPEN: 2,
  SUPPRESS_CONTENT_AREA_DOWNLOADS_OPEN: 4,

  /**
   * Returns an object whose keys are the string names from the downloads string
   * bundle, and whose values are either the translated strings or functions
   * returning formatted strings.
   */
  get strings() {
    let strings = {};
    let sb = Services.strings.createBundle(kDownloadsStringBundleUrl);
    for (let string of sb.getSimpleEnumeration()) {
      let stringName = string.key;
      if (stringName in kDownloadsStringsRequiringFormatting) {
        strings[stringName] = function () {
          // Convert "arguments" to a real array before calling into XPCOM.
          return sb.formatStringFromName(stringName, Array.from(arguments));
        };
      } else {
        strings[stringName] = string.value;
      }
    }
    delete this.strings;
    return (this.strings = strings);
  },

  /**
   * Indicates whether or not to show the 'Open in system viewer' context menu item when appropriate
   */
  get openInSystemViewerItemEnabled() {
    return PrefObserver.openInSystemViewerContextMenuItem;
  },

  /**
   * Indicates whether or not to show the 'Always open...' context menu item when appropriate
   */
  get alwaysOpenInSystemViewerItemEnabled() {
    return PrefObserver.alwaysOpenInSystemViewerContextMenuItem;
  },

  /**
   * Get access to one of the DownloadsData, PrivateDownloadsData, or
   * HistoryDownloadsData objects, depending on the privacy status of the
   * specified window and on whether history downloads should be included.
   *
   * @param [optional] window
   *        The browser window which owns the download button.
   *        If not given, the privacy status will be assumed as non-private.
   * @param [optional] history
   *        True to include history downloads when the window is public.
   * @param [optional] privateAll
   *        Whether to force the public downloads data to be returned together
   *        with the private downloads data for a private window.
   * @param [optional] limited
   *        True to limit the amount of downloads returned to
   *        `kMaxHistoryResultsForLimitedView`.
   */
  getData(window, history = false, privateAll = false, limited = false) {
    let isPrivate =
      window && lazy.PrivateBrowsingUtils.isContentWindowPrivate(window);
    if (isPrivate && !privateAll) {
      return lazy.PrivateDownloadsData;
    }
    if (history) {
      if (isPrivate && privateAll) {
        return lazy.LimitedPrivateHistoryDownloadData;
      }
      return limited
        ? lazy.LimitedHistoryDownloadsData
        : lazy.HistoryDownloadsData;
    }
    return lazy.DownloadsData;
  },

  /**
   * Initializes the Downloads back-end and starts receiving events for both the
   * private and non-private downloads data objects.
   */
  initializeAllDataLinks() {
    lazy.DownloadsData.initializeDataLink();
    lazy.PrivateDownloadsData.initializeDataLink();
  },

  /**
   * Get access to one of the DownloadsIndicatorData or
   * PrivateDownloadsIndicatorData objects, depending on the privacy status of
   * the window in question.
   */
  getIndicatorData(aWindow) {
    if (lazy.PrivateBrowsingUtils.isContentWindowPrivate(aWindow)) {
      return lazy.PrivateDownloadsIndicatorData;
    }
    return lazy.DownloadsIndicatorData;
  },

  /**
   * Returns a reference to the DownloadsSummaryData singleton - creating one
   * in the process if one hasn't been instantiated yet.
   *
   * @param aWindow
   *        The browser window which owns the download button.
   * @param aNumToExclude
   *        The number of items on the top of the downloads list to exclude
   *        from the summary.
   */
  getSummary(aWindow, aNumToExclude) {
    if (lazy.PrivateBrowsingUtils.isContentWindowPrivate(aWindow)) {
      if (this._privateSummary) {
        return this._privateSummary;
      }
      return (this._privateSummary = new DownloadsSummaryData(
        true,
        aNumToExclude
      ));
    }
    if (this._summary) {
      return this._summary;
    }
    return (this._summary = new DownloadsSummaryData(false, aNumToExclude));
  },
  _summary: null,
  _privateSummary: null,

  /**
   * Returns the legacy state integer value for the provided Download object.
   */
  stateOfDownload(download) {
    // Collapse state using the correct priority.
    if (!download.stopped) {
      return DownloadsCommon.DOWNLOAD_DOWNLOADING;
    }
    if (download.succeeded) {
      return DownloadsCommon.DOWNLOAD_FINISHED;
    }
    if (download.error) {
      if (download.error.becauseBlockedByParentalControls) {
        return DownloadsCommon.DOWNLOAD_BLOCKED_PARENTAL;
      }
      if (download.error.becauseBlockedByReputationCheck) {
        return DownloadsCommon.DOWNLOAD_DIRTY;
      }
      if (download.error.becauseBlockedByContentAnalysis) {
        // BLOCK_VERDICT_MALWARE indicates that the download was
        // blocked by the content analysis service, so return
        // DOWNLOAD_BLOCKED_CONTENT_ANALYSIS to indicate this.
        // Otherwise, the content analysis service returned
        // WARN, so the user has a chance to unblock the download,
        // which corresponds with DOWNLOAD_DIRTY.
        return download.error.reputationCheckVerdict ===
          lazy.Downloads.Error.BLOCK_VERDICT_MALWARE
          ? DownloadsCommon.DOWNLOAD_BLOCKED_CONTENT_ANALYSIS
          : DownloadsCommon.DOWNLOAD_DIRTY;
      }
      return DownloadsCommon.DOWNLOAD_FAILED;
    }
    if (download.canceled) {
      if (download.hasPartialData) {
        return DownloadsCommon.DOWNLOAD_PAUSED;
      }
      return DownloadsCommon.DOWNLOAD_CANCELED;
    }
    return DownloadsCommon.DOWNLOAD_NOTSTARTED;
  },

  /**
   * Removes a Download object from both session and history downloads.
   */
  async deleteDownload(download) {
    // Check hasBlockedData to avoid double counting if you click the X button
    // in the Library view and then delete the download from the history.
    if (
      download.error?.becauseBlockedByReputationCheck &&
      download.hasBlockedData
    ) {
      Glean.downloads.userActionOnBlockedDownload[
        download.error.reputationCheckVerdict
      ].accumulateSingleSample(1); // confirm block
    }

    // Remove the associated history element first, if any, so that the views
    // that combine history and session downloads won't resurrect the history
    // download into the view just before it is deleted permanently.
    try {
      await lazy.PlacesUtils.history.remove(download.source.url);
    } catch (ex) {
      console.error(ex);
    }
    let list = await lazy.Downloads.getList(lazy.Downloads.ALL);
    await list.remove(download);
    if (download.error?.becauseBlockedByContentAnalysis) {
      await download.respondToContentAnalysisWarnWithBlock();
    }
    await download.finalize(true);
  },

  /**
   * Deletes all files associated with a download, with or without removing it
   * from the session downloads list and/or download history.
   *
   * @param download
   *        The download to delete and/or forget.
   * @param clearHistory
   *        Optional. Removes history from session downloads list or history.
   *        0 - Don't remove the download from session list or history.
   *        1 - Remove the download from session list, but not history.
   *        2 - Remove the download from both session list and history.
   */
  async deleteDownloadFiles(download, clearHistory = 0) {
    if (clearHistory > 1) {
      try {
        await lazy.PlacesUtils.history.remove(download.source.url);
      } catch (ex) {
        console.error(ex);
      }
    }
    if (clearHistory > 0) {
      let list = await lazy.Downloads.getList(lazy.Downloads.ALL);
      await list.remove(download);
    }
    await download.manuallyRemoveData();
    if (download.error?.becauseBlockedByContentAnalysis) {
      await download.respondToContentAnalysisWarnWithBlock();
    }
    if (clearHistory < 2) {
      lazy.DownloadHistory.updateMetaData(download).catch(console.error);
    }
  },

  /**
   * Get a nsIMIMEInfo object for a download
   */
  getMimeInfo(download) {
    if (!download.succeeded) {
      return null;
    }
    let contentType = download.contentType;
    let url = Cc["@mozilla.org/network/standard-url-mutator;1"]
      .createInstance(Ci.nsIURIMutator)
      .setSpec("http://example.com") // construct the URL
      .setFilePath(download.target.path)
      .finalize()
      .QueryInterface(Ci.nsIURL);
    let fileExtension = url.fileExtension;

    // look at file extension if there's no contentType or it is generic
    if (!contentType || kGenericContentTypes.includes(contentType)) {
      try {
        contentType = lazy.gMIMEService.getTypeFromExtension(fileExtension);
      } catch (ex) {
        DownloadsCommon.log(
          "Cant get mimeType from file extension: ",
          fileExtension
        );
      }
    }
    if (!(contentType || fileExtension)) {
      return null;
    }
    let mimeInfo = null;
    try {
      mimeInfo = lazy.gMIMEService.getFromTypeAndExtension(
        contentType || "",
        fileExtension || ""
      );
    } catch (ex) {
      DownloadsCommon.log(
        "Can't get nsIMIMEInfo for contentType: ",
        contentType,
        "and fileExtension:",
        fileExtension
      );
    }
    return mimeInfo;
  },

  /**
   * Confirm if the download exists on the filesystem and is a given mime-type
   */
  isFileOfType(download, mimeType) {
    if (!(download.succeeded && download.target?.exists)) {
      DownloadsCommon.log(
        `isFileOfType returning false for mimeType: ${mimeType}, succeeded: ${download.succeeded}, exists: ${download.target?.exists}`
      );
      return false;
    }
    let mimeInfo = DownloadsCommon.getMimeInfo(download);
    return mimeInfo?.type === mimeType.toLowerCase();
  },

  /**
   * Copies the source URI of the given Download object to the clipboard.
   */
  copyDownloadLink(download) {
    lazy.gClipboardHelper.copyString(
      download.source.originalUrl || download.source.url
    );
  },

  /**
   * Given an iterable collection of Download objects, generates and returns
   * statistics about that collection.
   *
   * @param downloads An iterable collection of Download objects.
   *
   * @return Object whose properties are the generated statistics. Currently,
   *         we return the following properties:
   *
   *         numActive       : The total number of downloads.
   *         numPaused       : The total number of paused downloads.
   *         numDownloading  : The total number of downloads being downloaded.
   *         totalSize       : The total size of all downloads once completed.
   *         totalTransferred: The total amount of transferred data for these
   *                           downloads.
   *         slowestSpeed    : The slowest download rate.
   *         rawTimeLeft     : The estimated time left for the downloads to
   *                           complete.
   *         percentComplete : The percentage of bytes successfully downloaded.
   */
  summarizeDownloads(downloads) {
    let summary = {
      numActive: 0,
      numPaused: 0,
      numDownloading: 0,
      totalSize: 0,
      totalTransferred: 0,
      // slowestSpeed is Infinity so that we can use Math.min to
      // find the slowest speed. We'll set this to 0 afterwards if
      // it's still at Infinity by the time we're done iterating all
      // download.
      slowestSpeed: Infinity,
      rawTimeLeft: -1,
      percentComplete: -1,
    };

    for (let download of downloads) {
      summary.numActive++;

      if (!download.stopped) {
        summary.numDownloading++;
        if (download.hasProgress && download.speed > 0) {
          let sizeLeft = download.totalBytes - download.currentBytes;
          summary.rawTimeLeft = Math.max(
            summary.rawTimeLeft,
            sizeLeft / download.speed
          );
          summary.slowestSpeed = Math.min(summary.slowestSpeed, download.speed);
        }
      } else if (download.canceled && download.hasPartialData) {
        summary.numPaused++;
      }

      // Only add to total values if we actually know the download size.
      if (download.succeeded) {
        summary.totalSize += download.target.size;
        summary.totalTransferred += download.target.size;
      } else if (download.hasProgress) {
        summary.totalSize += download.totalBytes;
        summary.totalTransferred += download.currentBytes;
      }
    }

    if (summary.totalSize != 0) {
      summary.percentComplete = Math.floor(
        (summary.totalTransferred / summary.totalSize) * 100
      );
    }

    if (summary.slowestSpeed == Infinity) {
      summary.slowestSpeed = 0;
    }

    return summary;
  },

  /**
   * If necessary, smooths the estimated number of seconds remaining for one
   * or more downloads to complete.
   *
   * @param aSeconds
   *        Current raw estimate on number of seconds left for one or more
   *        downloads. This is a floating point value to help get sub-second
   *        accuracy for current and future estimates.
   */
  smoothSeconds(aSeconds, aLastSeconds) {
    // We apply an algorithm similar to the DownloadUtils.getTimeLeft function,
    // though tailored to a single time estimation for all downloads.  We never
    // apply something if the new value is less than half the previous value.
    let shouldApplySmoothing = aLastSeconds >= 0 && aSeconds > aLastSeconds / 2;
    if (shouldApplySmoothing) {
      // Apply hysteresis to favor downward over upward swings.  Trust only 30%
      // of the new value if lower, and 10% if higher (exponential smoothing).
      let diff = aSeconds - aLastSeconds;
      aSeconds = aLastSeconds + (diff < 0 ? 0.3 : 0.1) * diff;

      // If the new time is similar, reuse something close to the last time
      // left, but subtract a little to provide forward progress.
      diff = aSeconds - aLastSeconds;
      let diffPercent = (diff / aLastSeconds) * 100;
      if (Math.abs(diff) < 5 || Math.abs(diffPercent) < 5) {
        aSeconds = aLastSeconds - (diff < 0 ? 0.4 : 0.2);
      }
    }

    // In the last few seconds of downloading, we are always subtracting and
    // never adding to the time left.  Ensure that we never fall below one
    // second left until all downloads are actually finished.
    return (aLastSeconds = Math.max(aSeconds, 1));
  },

  /**
   * Opens a downloaded file.
   *
   * @param downloadProperties
   *        A Download object or the initial properties of a serialized download
   * @param options.openWhere
   *        Optional string indicating how to handle opening a download target file URI.
   *        One of "window", "tab", "tabshifted".
   * @param options.useSystemDefault
   *        Optional value indicating how to handle launching this download,
   *        this call only. Will override the associated mimeInfo.preferredAction
   * @return {Promise}
   * @resolves When the instruction to launch the file has been
   *           successfully given to the operating system or handled internally
   * @rejects  JavaScript exception if there was an error trying to launch
   *           the file.
   */
  async openDownload(download, options) {
    // some download objects got serialized and need reconstituting
    if (typeof download.launch !== "function") {
      download = await lazy.Downloads.createDownload(download);
    }
    return download.launch(options).catch(ex => console.error(ex));
  },

  /**
   * Show a downloaded file in the system file manager.
   *
   * @param aFile
   *        a downloaded file.
   */
  showDownloadedFile(aFile) {
    if (!(aFile instanceof Ci.nsIFile)) {
      throw new Error("aFile must be a nsIFile object");
    }
    try {
      // Show the directory containing the file and select the file.
      aFile.reveal();
    } catch (ex) {
      // If reveal fails for some reason (e.g., it's not implemented on unix
      // or the file doesn't exist), try using the parent if we have it.
      let parent = aFile.parent;
      if (parent) {
        this.showDirectory(parent);
      }
    }
  },

  /**
   * Show the specified folder in the system file manager.
   *
   * @param aDirectory
   *        a directory to be opened with system file manager.
   */
  showDirectory(aDirectory) {
    if (!(aDirectory instanceof Ci.nsIFile)) {
      throw new Error("aDirectory must be a nsIFile object");
    }
    try {
      aDirectory.launch();
    } catch (ex) {
      // If launch fails (probably because it's not implemented), let
      // the OS handler try to open the directory.
      Cc["@mozilla.org/uriloader/external-protocol-service;1"]
        .getService(Ci.nsIExternalProtocolService)
        .loadURI(
          lazy.NetUtil.newURI(aDirectory),
          Services.scriptSecurityManager.getSystemPrincipal()
        );
    }
  },

  /**
   * Displays an alert message box which asks the user if they want to
   * unblock the downloaded file or not.
   *
   * @param options
   *        An object with the following properties:
   *        {
   *          verdict:
   *            The detailed reason why the download was blocked, according to
   *            the "Downloads.Error.BLOCK_VERDICT_" constants. If an unknown
   *            reason is specified, "Downloads.Error.BLOCK_VERDICT_MALWARE" is
   *            assumed.
   *          becauseBlockedByReputationCheck:
   *            Whether the the download was blocked by a reputation check.
   *          window:
   *            The window with which this action is associated.
   *          dialogType:
   *            String that determines which actions are available:
   *             - "unblock" to offer just "unblock".
   *             - "chooseUnblock" to offer "unblock" and "confirmBlock".
   *             - "chooseOpen" to offer "open" and "confirmBlock".
   *        }
   *
   * @return {Promise}
   * @resolves String representing the action that should be executed:
   *            - "open" to allow the download and open the file.
   *            - "unblock" to allow the download without opening the file.
   *            - "confirmBlock" to delete the blocked data permanently.
   *            - "cancel" to do nothing and cancel the operation.
   */
  async confirmUnblockDownload({
    verdict,
    becauseBlockedByReputationCheck,
    window,
    dialogType,
  }) {
    let s = DownloadsCommon.strings;

    // All the dialogs have an action button and a cancel button, while only
    // some of them have an additonal button to remove the file. The cancel
    // button must always be the one at BUTTON_POS_1 because this is the value
    // returned by confirmEx when using ESC or closing the dialog (bug 345067).
    let title = s.unblockHeaderUnblock;
    let firstButtonText = s.unblockButtonUnblock;
    let firstButtonAction = "unblock";
    let buttonFlags =
      Ci.nsIPrompt.BUTTON_TITLE_IS_STRING * Ci.nsIPrompt.BUTTON_POS_0 +
      Ci.nsIPrompt.BUTTON_TITLE_CANCEL * Ci.nsIPrompt.BUTTON_POS_1;

    switch (dialogType) {
      case "unblock":
        // Use only the unblock action. The default is to cancel.
        buttonFlags += Ci.nsIPrompt.BUTTON_POS_1_DEFAULT;
        break;
      case "chooseUnblock":
        // Use the unblock and remove file actions. The default is remove file.
        buttonFlags +=
          Ci.nsIPrompt.BUTTON_TITLE_IS_STRING * Ci.nsIPrompt.BUTTON_POS_2 +
          Ci.nsIPrompt.BUTTON_POS_2_DEFAULT;
        break;
      case "chooseOpen":
        // Use the unblock and open file actions. The default is open file.
        title = s.unblockHeaderOpen;
        firstButtonText = s.unblockButtonOpen;
        firstButtonAction = "open";
        buttonFlags +=
          Ci.nsIPrompt.BUTTON_TITLE_IS_STRING * Ci.nsIPrompt.BUTTON_POS_2 +
          Ci.nsIPrompt.BUTTON_POS_0_DEFAULT;
        break;
      default:
        console.error("Unexpected dialog type: " + dialogType);
        return "cancel";
    }

    let message;
    let tip = s.unblockTip2;
    switch (verdict) {
      case lazy.Downloads.Error.BLOCK_VERDICT_UNCOMMON:
        message = s.unblockTypeUncommon2;
        break;
      case lazy.Downloads.Error.BLOCK_VERDICT_POTENTIALLY_UNWANTED:
        if (becauseBlockedByReputationCheck) {
          message = s.unblockTypePotentiallyUnwanted2;
        } else {
          message = s.unblockTypeContentAnalysisWarn;
          tip = s.unblockContentAnalysisTip;
        }
        break;
      case lazy.Downloads.Error.BLOCK_VERDICT_INSECURE:
        message = s.unblockInsecure2;
        break;
      default:
        // Assume Downloads.Error.BLOCK_VERDICT_MALWARE
        message = s.unblockTypeMalware;
        break;
    }
    message += "\n\n" + tip;

    Services.ww.registerNotification(function onOpen(subj, topic) {
      if (topic == "domwindowopened" && subj instanceof Ci.nsIDOMWindow) {
        // Make sure to listen for "DOMContentLoaded" because it is fired
        // before the "load" event.
        subj.addEventListener(
          "DOMContentLoaded",
          function () {
            if (
              subj.document.documentURI ==
              "chrome://global/content/commonDialog.xhtml"
            ) {
              Services.ww.unregisterNotification(onOpen);
              let dialog = subj.document.getElementById("commonDialog");
              if (dialog) {
                // Change the dialog to use a warning icon.
                dialog.classList.add("alert-dialog");
              }
            }
          },
          { once: true }
        );
      }
    });

    let rv = Services.prompt.confirmEx(
      window,
      title,
      message,
      buttonFlags,
      firstButtonText,
      null,
      s.unblockButtonConfirmBlock,
      null,
      {}
    );
    return [firstButtonAction, "cancel", "confirmBlock"][rv];
  },
};

ChromeUtils.defineLazyGetter(DownloadsCommon, "log", () => {
  return lazy.DownloadsLogger.log.bind(lazy.DownloadsLogger);
});
ChromeUtils.defineLazyGetter(DownloadsCommon, "error", () => {
  return lazy.DownloadsLogger.error.bind(lazy.DownloadsLogger);
});

// DownloadsData

/**
 * Retrieves the list of past and completed downloads from the underlying
 * Downloads API data, and provides asynchronous notifications allowing to
 * build a consistent view of the available data.
 *
 * Note that using this object does not automatically initialize the list of
 * downloads. This is useful to display a neutral progress indicator in
 * the main browser window until the autostart timeout elapses.
 *
 * This powers the DownloadsData, PrivateDownloadsData, and HistoryDownloadsData
 * singleton objects.
 */
function DownloadsDataCtor({ isPrivate, isHistory, maxHistoryResults } = {}) {
  this._isPrivate = !!isPrivate;

  // Contains all the available Download objects and their integer state.
  this._oldDownloadStates = new WeakMap();

  // For the history downloads list we don't need to register this as a view,
  // but we have to ensure that the DownloadsData object is initialized before
  // we register more views. This ensures that the view methods of DownloadsData
  // are invoked before those of views registered on HistoryDownloadsData,
  // allowing the endTime property to be set correctly.
  if (isHistory) {
    if (isPrivate) {
      lazy.PrivateDownloadsData.initializeDataLink();
    }
    lazy.DownloadsData.initializeDataLink();
    this._promiseList = lazy.DownloadsData._promiseList.then(() => {
      // For history downloads in Private Browsing mode, we'll fetch the combined
      // list of public and private downloads.
      return lazy.DownloadHistory.getList({
        type: isPrivate ? lazy.Downloads.ALL : lazy.Downloads.PUBLIC,
        maxHistoryResults,
      });
    });
    return;
  }

  // This defines "initializeDataLink" and "_promiseList" synchronously, then
  // continues execution only when "initializeDataLink" is called, allowing the
  // underlying data to be loaded only when actually needed.
  this._promiseList = (async () => {
    await new Promise(resolve => (this.initializeDataLink = resolve));
    let list = await lazy.Downloads.getList(
      isPrivate ? lazy.Downloads.PRIVATE : lazy.Downloads.PUBLIC
    );
    await list.addView(this);
    return list;
  })();
}

DownloadsDataCtor.prototype = {
  /**
   * Starts receiving events for current downloads.
   */
  initializeDataLink() {},

  /**
   * Promise resolved with the underlying DownloadList object once we started
   * receiving events for current downloads.
   */
  _promiseList: null,

  /**
   * Iterator for all the available Download objects. This is empty until the
   * data has been loaded using the JavaScript API for downloads.
   */
  get _downloads() {
    return ChromeUtils.nondeterministicGetWeakMapKeys(this._oldDownloadStates);
  },

  /**
   * True if there are finished downloads that can be removed from the list.
   */
  get canRemoveFinished() {
    for (let download of this._downloads) {
      // Stopped, paused, and failed downloads with partial data are removed.
      if (download.stopped && !(download.canceled && download.hasPartialData)) {
        return true;
      }
    }
    return false;
  },

  /**
   * Asks the back-end to remove finished downloads from the list. This method
   * is only called after the data link has been initialized.
   */
  removeFinished() {
    lazy.Downloads.getList(
      this._isPrivate ? lazy.Downloads.PRIVATE : lazy.Downloads.PUBLIC
    )
      .then(list => list.removeFinished())
      .catch(console.error);
  },

  // Integration with the asynchronous Downloads back-end

  onDownloadAdded(download) {
    // Download objects do not store the end time of downloads, as the Downloads
    // API does not need to persist this information for all platforms. Once a
    // download terminates on a Desktop browser, it becomes a history download,
    // for which the end time is stored differently, as a Places annotation.
    download.endTime = Date.now();

    this._oldDownloadStates.set(
      download,
      DownloadsCommon.stateOfDownload(download)
    );
    if (
      download.error?.becauseBlockedByReputationCheck ||
      download.error?.becauseBlockedByContentAnalysis
    ) {
      this._notifyDownloadEvent("error");
    }
  },

  onDownloadChanged(download) {
    let oldState = this._oldDownloadStates.get(download);
    let newState = DownloadsCommon.stateOfDownload(download);
    this._oldDownloadStates.set(download, newState);

    if (oldState != newState) {
      if (
        download.succeeded ||
        (download.canceled && !download.hasPartialData) ||
        download.error
      ) {
        // Store the end time that may be displayed by the views.
        download.endTime = Date.now();

        // This state transition code should actually be located in a Downloads
        // API module (bug 941009).
        lazy.DownloadHistory.updateMetaData(download).catch(console.error);
      }

      if (
        download.succeeded ||
        (download.error && download.error.becauseBlocked)
      ) {
        this._notifyDownloadEvent("finish");
      }
    }

    if (!download.newDownloadNotified) {
      download.newDownloadNotified = true;
      this._notifyDownloadEvent("start", {
        openDownloadsListOnStart: download.openDownloadsListOnStart,
      });
    }
  },

  onDownloadRemoved(download) {
    this._oldDownloadStates.delete(download);
  },

  // Registration of views

  /**
   * Adds an object to be notified when the available download data changes.
   * The specified object is initialized with the currently available downloads.
   *
   * @param aView
   *        DownloadsView object to be added.  This reference must be passed to
   *        removeView before termination.
   */
  addView(aView) {
    this._promiseList.then(list => list.addView(aView)).catch(console.error);
  },

  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        DownloadsView object to be removed.
   */
  removeView(aView) {
    this._promiseList.then(list => list.removeView(aView)).catch(console.error);
  },

  // Notifications sent to the most recent browser window only

  /**
   * Set to true after the first download causes the downloads panel to be
   * displayed.
   */
  get panelHasShownBefore() {
    try {
      return Services.prefs.getBoolPref("browser.download.panel.shown");
    } catch (ex) {}
    return false;
  },

  set panelHasShownBefore(aValue) {
    Services.prefs.setBoolPref("browser.download.panel.shown", aValue);
  },

  /**
   * Displays a new or finished download notification in the most recent browser
   * window, if one is currently available with the required privacy type.
   * @param {string} aType
   *        Set to "start" for new downloads, "finish" for completed downloads,
   *        "error" for downloads that failed and need attention
   * @param {boolean} [openDownloadsListOnStart]
   *        (Only relevant when aType = "start")
   *        true (default) - open the downloads panel.
   *        false - only show an indicator notification.
   */
  _notifyDownloadEvent(aType, { openDownloadsListOnStart = true } = {}) {
    DownloadsCommon.log(
      "Attempting to notify that a new download has started or finished."
    );

    // Show the panel in the most recent browser window, if present.
    let browserWin = lazy.BrowserWindowTracker.getTopWindow({
      private: this._isPrivate,
    });
    if (!browserWin) {
      return;
    }

    let shouldOpenDownloadsPanel =
      aType == "start" &&
      DownloadsCommon.summarizeDownloads(this._downloads).numDownloading <= 1 &&
      lazy.gAlwaysOpenPanel;

    // For new downloads after the first one, don't show the panel
    // automatically, but provide a visible notification in the topmost browser
    // window, if the status indicator is already visible. Also ensure that if
    // openDownloadsListOnStart = false is passed, we always skip opening the
    // panel. That's because this will only be passed if the download is started
    // without user interaction or if a dialog was previously opened in the
    // process of the download (e.g. unknown content type dialog).
    if (
      aType != "error" &&
      ((this.panelHasShownBefore && !shouldOpenDownloadsPanel) ||
        !openDownloadsListOnStart ||
        browserWin != Services.focus.activeWindow)
    ) {
      DownloadsCommon.log("Showing new download notification.");
      browserWin.DownloadsIndicatorView.showEventNotification(aType);
      return;
    }
    this.panelHasShownBefore = true;
    browserWin.DownloadsPanel.showPanel();
  },
};

ChromeUtils.defineLazyGetter(lazy, "HistoryDownloadsData", function () {
  return new DownloadsDataCtor({ isHistory: true });
});

ChromeUtils.defineLazyGetter(lazy, "LimitedHistoryDownloadsData", function () {
  return new DownloadsDataCtor({
    isHistory: true,
    maxHistoryResults: kMaxHistoryResultsForLimitedView,
  });
});

ChromeUtils.defineLazyGetter(
  lazy,
  "LimitedPrivateHistoryDownloadData",
  function () {
    return new DownloadsDataCtor({
      isPrivate: true,
      isHistory: true,
      maxHistoryResults: kMaxHistoryResultsForLimitedView,
    });
  }
);

ChromeUtils.defineLazyGetter(lazy, "PrivateDownloadsData", function () {
  return new DownloadsDataCtor({ isPrivate: true });
});

ChromeUtils.defineLazyGetter(lazy, "DownloadsData", function () {
  return new DownloadsDataCtor();
});

// DownloadsViewPrototype

/**
 * A prototype for an object that registers itself with DownloadsData as soon
 * as a view is registered with it.
 */
const DownloadsViewPrototype = {
  /**
   * Contains all the available Download objects and their current state value.
   *
   * SUBCLASSES MUST OVERRIDE THIS PROPERTY.
   */
  _oldDownloadStates: null,

  // Registration of views

  /**
   * Array of view objects that should be notified when the available status
   * data changes.
   *
   * SUBCLASSES MUST OVERRIDE THIS PROPERTY.
   */
  _views: null,

  /**
   * Determines whether this view object is over the private or non-private
   * downloads.
   *
   * SUBCLASSES MUST OVERRIDE THIS PROPERTY.
   */
  _isPrivate: false,

  /**
   * Adds an object to be notified when the available status data changes.
   * The specified object is initialized with the currently available status.
   *
   * @param aView
   *        View object to be added.  This reference must be
   *        passed to removeView before termination.
   */
  addView(aView) {
    // Start receiving events when the first of our views is registered.
    if (!this._views.length) {
      if (this._isPrivate) {
        lazy.PrivateDownloadsData.addView(this);
      } else {
        lazy.DownloadsData.addView(this);
      }
    }

    this._views.push(aView);
    this.refreshView(aView);
  },

  /**
   * Updates the properties of an object previously added using addView.
   *
   * @param aView
   *        View object to be updated.
   */
  refreshView(aView) {
    // Update immediately even if we are still loading data asynchronously.
    // Subclasses must provide these two functions!
    this._refreshProperties();
    this._updateView(aView);
  },

  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        View object to be removed.
   */
  removeView(aView) {
    let index = this._views.indexOf(aView);
    if (index != -1) {
      this._views.splice(index, 1);
    }

    // Stop receiving events when the last of our views is unregistered.
    if (!this._views.length) {
      if (this._isPrivate) {
        lazy.PrivateDownloadsData.removeView(this);
      } else {
        lazy.DownloadsData.removeView(this);
      }
    }
  },

  // Callback functions from DownloadList

  /**
   * Indicates whether we are still loading downloads data asynchronously.
   */
  _loading: false,

  /**
   * Called before multiple downloads are about to be loaded.
   */
  onDownloadBatchStarting() {
    this._loading = true;
  },

  /**
   * Called after data loading finished.
   */
  onDownloadBatchEnded() {
    this._loading = false;
    this._updateViews();
  },

  /**
   * Called when a new download data item is available, either during the
   * asynchronous data load or when a new download is started.
   *
   * @param download
   *        Download object that was just added.
   *
   * @note Subclasses should override this and still call the base method.
   */
  onDownloadAdded(download) {
    this._oldDownloadStates.set(
      download,
      DownloadsCommon.stateOfDownload(download)
    );
  },

  /**
   * Called when the overall state of a Download has changed. In particular,
   * this is called only once when the download succeeds or is blocked
   * permanently, and is never called if only the current progress changed.
   *
   * The onDownloadChanged notification will always be sent afterwards.
   *
   * @note Subclasses should override this.
   */
  onDownloadStateChanged() {
    throw Components.Exception("", Cr.NS_ERROR_NOT_IMPLEMENTED);
  },

  /**
   * Called every time any state property of a Download may have changed,
   * including progress properties.
   *
   * Note that progress notification changes are throttled at the Downloads.sys.mjs
   * API level, and there is no throttling mechanism in the front-end.
   *
   * @note Subclasses should override this and still call the base method.
   */
  onDownloadChanged(download) {
    let oldState = this._oldDownloadStates.get(download);
    let newState = DownloadsCommon.stateOfDownload(download);
    this._oldDownloadStates.set(download, newState);

    if (oldState != newState) {
      this.onDownloadStateChanged(download);
    }
  },

  /**
   * Called when a data item is removed, ensures that the widget associated with
   * the view item is removed from the user interface.
   *
   * @param download
   *        Download object that is being removed.
   *
   * @note Subclasses should override this.
   */
  onDownloadRemoved(download) {
    this._oldDownloadStates.delete(download);
  },

  /**
   * Private function used to refresh the internal properties being sent to
   * each registered view.
   *
   * @note Subclasses should override this.
   */
  _refreshProperties() {
    throw Components.Exception("", Cr.NS_ERROR_NOT_IMPLEMENTED);
  },

  /**
   * Private function used to refresh an individual view.
   *
   * @note Subclasses should override this.
   */
  _updateView() {
    throw Components.Exception("", Cr.NS_ERROR_NOT_IMPLEMENTED);
  },

  /**
   * Computes aggregate values and propagates the changes to our views.
   */
  _updateViews() {
    // Do not update the status indicators during batch loads of download items.
    if (this._loading) {
      return;
    }

    this._refreshProperties();
    this._views.forEach(this._updateView, this);
  },
};

// DownloadsIndicatorData

/**
 * This object registers itself with DownloadsData as a view, and transforms the
 * notifications it receives into overall status data, that is then broadcast to
 * the registered download status indicators.
 *
 * Note that using this object does not automatically start the Download Manager
 * service.  Consumers will see an empty list of downloads until the service is
 * actually started.  This is useful to display a neutral progress indicator in
 * the main browser window until the autostart timeout elapses.
 */
function DownloadsIndicatorDataCtor(aPrivate) {
  this._oldDownloadStates = new WeakMap();
  this._isPrivate = aPrivate;
  this._views = [];
}
DownloadsIndicatorDataCtor.prototype = {
  /**
   * Map of the relative severities of different attention states.
   * Used in sorting the map of active downloads' attention states
   * to determine the attention state to be displayed.
   */
  _attentionPriority: new Map([
    [DownloadsCommon.ATTENTION_NONE, 0],
    [DownloadsCommon.ATTENTION_SUCCESS, 1],
    [DownloadsCommon.ATTENTION_INFO, 2],
    [DownloadsCommon.ATTENTION_WARNING, 3],
    [DownloadsCommon.ATTENTION_SEVERE, 4],
  ]),

  /**
   * Iterator for all the available Download objects. This is empty until the
   * data has been loaded using the JavaScript API for downloads.
   */
  get _downloads() {
    return ChromeUtils.nondeterministicGetWeakMapKeys(this._oldDownloadStates);
  },

  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        DownloadsIndicatorView object to be removed.
   */
  removeView(aView) {
    DownloadsViewPrototype.removeView.call(this, aView);

    if (!this._views.length) {
      this._itemCount = 0;
    }
  },

  onDownloadAdded(download) {
    DownloadsViewPrototype.onDownloadAdded.call(this, download);
    this._itemCount++;
    this._updateViews();
  },

  onDownloadStateChanged(download) {
    if (this._attentionSuppressed !== DownloadsCommon.SUPPRESS_NONE) {
      return;
    }
    let attention;
    if (
      !download.succeeded &&
      download.error &&
      download.error.reputationCheckVerdict
    ) {
      switch (download.error.reputationCheckVerdict) {
        case lazy.Downloads.Error.BLOCK_VERDICT_UNCOMMON:
          attention = DownloadsCommon.ATTENTION_INFO;
          break;
        case lazy.Downloads.Error.BLOCK_VERDICT_POTENTIALLY_UNWANTED: // fall-through
        case lazy.Downloads.Error.BLOCK_VERDICT_INSECURE:
        case lazy.Downloads.Error.BLOCK_VERDICT_DOWNLOAD_SPAM:
          attention = DownloadsCommon.ATTENTION_WARNING;
          break;
        case lazy.Downloads.Error.BLOCK_VERDICT_MALWARE:
          attention = DownloadsCommon.ATTENTION_SEVERE;
          break;
        default:
          attention = DownloadsCommon.ATTENTION_SEVERE;
          console.error(
            "Unknown reputation verdict: " +
              download.error.reputationCheckVerdict
          );
      }
    } else if (download.succeeded) {
      attention = DownloadsCommon.ATTENTION_SUCCESS;
    } else if (download.error) {
      attention = DownloadsCommon.ATTENTION_WARNING;
    }
    download.attention = attention;
    this.updateAttention();
  },

  onDownloadChanged(download) {
    DownloadsViewPrototype.onDownloadChanged.call(this, download);
    this._updateViews();
  },

  onDownloadRemoved(download) {
    DownloadsViewPrototype.onDownloadRemoved.call(this, download);
    this._itemCount--;
    this.updateAttention();
    this._updateViews();
  },

  // Propagation of properties to our views

  // The following properties are updated by _refreshProperties and are then
  // propagated to the views.  See _refreshProperties for details.
  _hasDownloads: false,
  _percentComplete: -1,

  /**
   * Indicates whether the download indicators should be highlighted.
   */
  set attention(aValue) {
    this._attention = aValue;
    this._updateViews();
  },
  _attention: DownloadsCommon.ATTENTION_NONE,

  /**
   * Indicates whether the user is interacting with downloads, thus the
   * attention indication should not be shown even if requested.
   */
  set attentionSuppressed(aFlags) {
    this._attentionSuppressed = aFlags;
    if (aFlags !== DownloadsCommon.SUPPRESS_NONE) {
      for (let download of this._downloads) {
        download.attention = DownloadsCommon.ATTENTION_NONE;
      }
      this.attention = DownloadsCommon.ATTENTION_NONE;
    }
  },
  get attentionSuppressed() {
    return this._attentionSuppressed;
  },
  _attentionSuppressed: DownloadsCommon.SUPPRESS_NONE,

  /**
   * Set the indicator's attention to the most severe attention state among the
   * unseen displayed downloads, or DownloadsCommon.ATTENTION_NONE if empty.
   */
  updateAttention() {
    let currentAttention = DownloadsCommon.ATTENTION_NONE;
    let currentPriority = 0;
    for (let download of this._downloads) {
      let { attention } = download;
      let priority = this._attentionPriority.get(attention);
      if (priority > currentPriority) {
        currentPriority = priority;
        currentAttention = attention;
      }
    }
    this.attention = currentAttention;
  },

  /**
   * Updates the specified view with the current aggregate values.
   *
   * @param aView
   *        DownloadsIndicatorView object to be updated.
   */
  _updateView(aView) {
    aView.hasDownloads = this._hasDownloads;
    aView.percentComplete = this._percentComplete;
    aView.attention =
      this.attentionSuppressed !== DownloadsCommon.SUPPRESS_NONE
        ? DownloadsCommon.ATTENTION_NONE
        : this._attention;
  },

  // Property updating based on current download status

  /**
   * Number of download items that are available to be displayed.
   */
  _itemCount: 0,

  /**
   * A generator function for the Download objects this summary is currently
   * interested in. This generator is passed off to summarizeDownloads in order
   * to generate statistics about the downloads we care about - in this case,
   * it's all active downloads.
   */
  *_activeDownloads() {
    let downloads = this._isPrivate
      ? lazy.PrivateDownloadsData._downloads
      : lazy.DownloadsData._downloads;
    for (let download of downloads) {
      if (
        download.isInCurrentBatch ||
        (download.canceled && download.hasPartialData)
      ) {
        yield download;
      }
    }
  },

  /**
   * Computes aggregate values based on the current state of downloads.
   */
  _refreshProperties() {
    let summary = DownloadsCommon.summarizeDownloads(this._activeDownloads());

    // Determine if the indicator should be shown or get attention.
    this._hasDownloads = this._itemCount > 0;

    // Always show a progress bar if there are downloads in progress.
    if (summary.percentComplete >= 0) {
      this._percentComplete = summary.percentComplete;
    } else if (summary.numDownloading > 0) {
      this._percentComplete = 0;
    } else {
      this._percentComplete = -1;
    }
  },
};
Object.setPrototypeOf(
  DownloadsIndicatorDataCtor.prototype,
  DownloadsViewPrototype
);

ChromeUtils.defineLazyGetter(
  lazy,
  "PrivateDownloadsIndicatorData",
  function () {
    return new DownloadsIndicatorDataCtor(true);
  }
);

ChromeUtils.defineLazyGetter(lazy, "DownloadsIndicatorData", function () {
  return new DownloadsIndicatorDataCtor(false);
});

// DownloadsSummaryData

/**
 * DownloadsSummaryData is a view for DownloadsData that produces a summary
 * of all downloads after a certain exclusion point aNumToExclude. For example,
 * if there were 5 downloads in progress, and a DownloadsSummaryData was
 * constructed with aNumToExclude equal to 3, then that DownloadsSummaryData
 * would produce a summary of the last 2 downloads.
 *
 * @param aIsPrivate
 *        True if the browser window which owns the download button is a private
 *        window.
 * @param aNumToExclude
 *        The number of items to exclude from the summary, starting from the
 *        top of the list.
 */
function DownloadsSummaryData(aIsPrivate, aNumToExclude) {
  this._numToExclude = aNumToExclude;
  // Since we can have multiple instances of DownloadsSummaryData, we
  // override these values from the prototype so that each instance can be
  // completely separated from one another.
  this._loading = false;

  this._downloads = [];

  // Floating point value indicating the last number of seconds estimated until
  // the longest download will finish.  We need to store this value so that we
  // don't continuously apply smoothing if the actual download state has not
  // changed.  This is set to -1 if the previous value is unknown.
  this._lastRawTimeLeft = -1;

  // Last number of seconds estimated until all in-progress downloads with a
  // known size and speed will finish.  This value is stored to allow smoothing
  // in case of small variations.  This is set to -1 if the previous value is
  // unknown.
  this._lastTimeLeft = -1;

  // The following properties are updated by _refreshProperties and are then
  // propagated to the views.
  this._showingProgress = false;
  this._details = "";
  this._description = "";
  this._numActive = 0;
  this._percentComplete = -1;

  this._oldDownloadStates = new WeakMap();
  this._isPrivate = aIsPrivate;
  this._views = [];
}

DownloadsSummaryData.prototype = {
  /**
   * Removes an object previously added using addView.
   *
   * @param aView
   *        DownloadsSummary view to be removed.
   */
  removeView(aView) {
    DownloadsViewPrototype.removeView.call(this, aView);

    if (!this._views.length) {
      // Clear out our collection of Download objects. If we ever have
      // another view registered with us, this will get re-populated.
      this._downloads = [];
    }
  },

  onDownloadAdded(download) {
    DownloadsViewPrototype.onDownloadAdded.call(this, download);
    this._downloads.unshift(download);
    this._updateViews();
  },

  onDownloadStateChanged() {
    // Since the state of a download changed, reset the estimated time left.
    this._lastRawTimeLeft = -1;
    this._lastTimeLeft = -1;
  },

  onDownloadChanged(download) {
    DownloadsViewPrototype.onDownloadChanged.call(this, download);
    this._updateViews();
  },

  onDownloadRemoved(download) {
    DownloadsViewPrototype.onDownloadRemoved.call(this, download);
    let itemIndex = this._downloads.indexOf(download);
    this._downloads.splice(itemIndex, 1);
    this._updateViews();
  },

  // Propagation of properties to our views

  /**
   * Updates the specified view with the current aggregate values.
   *
   * @param aView
   *        DownloadsIndicatorView object to be updated.
   */
  _updateView(aView) {
    aView.showingProgress = this._showingProgress;
    aView.percentComplete = this._percentComplete;
    aView.description = this._description;
    aView.details = this._details;
  },

  // Property updating based on current download status

  /**
   * A generator function for the Download objects this summary is currently
   * interested in. This generator is passed off to summarizeDownloads in order
   * to generate statistics about the downloads we care about - in this case,
   * it's the downloads in this._downloads after the first few to exclude,
   * which was set when constructing this DownloadsSummaryData instance.
   */
  *_downloadsForSummary() {
    if (this._downloads.length) {
      for (let i = this._numToExclude; i < this._downloads.length; ++i) {
        yield this._downloads[i];
      }
    }
  },

  /**
   * Computes aggregate values based on the current state of downloads.
   */
  _refreshProperties() {
    // Pre-load summary with default values.
    let summary = DownloadsCommon.summarizeDownloads(
      this._downloadsForSummary()
    );

    // Run sync to update view right away and get correct description.
    // See refreshView for more details.
    this._description = kDownloadsFluentStrings.formatValueSync(
      "downloads-more-downloading",
      {
        count: summary.numDownloading,
      }
    );
    this._percentComplete = summary.percentComplete;

    // Only show the downloading items.
    this._showingProgress = summary.numDownloading > 0;

    // Display the estimated time left, if present.
    if (summary.rawTimeLeft == -1) {
      // There are no downloads with a known time left.
      this._lastRawTimeLeft = -1;
      this._lastTimeLeft = -1;
      this._details = "";
    } else {
      // Compute the new time left only if state actually changed.
      if (this._lastRawTimeLeft != summary.rawTimeLeft) {
        this._lastRawTimeLeft = summary.rawTimeLeft;
        this._lastTimeLeft = DownloadsCommon.smoothSeconds(
          summary.rawTimeLeft,
          this._lastTimeLeft
        );
      }
      [this._details] = lazy.DownloadUtils.getDownloadStatusNoRate(
        summary.totalTransferred,
        summary.totalSize,
        summary.slowestSpeed,
        this._lastTimeLeft
      );
    }
  },
};
Object.setPrototypeOf(DownloadsSummaryData.prototype, DownloadsViewPrototype);
