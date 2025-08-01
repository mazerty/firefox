ChromeUtils.defineESModuleGetters(this, {
  AppProvidedConfigEngine:
    "moz-src:///toolkit/components/search/ConfigSearchEngine.sys.mjs",
  HttpServer: "resource://testing-common/httpd.sys.mjs",
  PlacesTestUtils: "resource://testing-common/PlacesTestUtils.sys.mjs",
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
  Preferences: "resource://gre/modules/Preferences.sys.mjs",
  sinon: "resource://testing-common/Sinon.sys.mjs",
  TopSites: "resource:///modules/topsites/TopSites.sys.mjs",
  UrlbarProvider: "resource:///modules/UrlbarUtils.sys.mjs",
  UrlbarProvidersManager: "resource:///modules/UrlbarProvidersManager.sys.mjs",
  UrlbarResult: "resource:///modules/UrlbarResult.sys.mjs",
  UrlbarTokenizer: "resource:///modules/UrlbarTokenizer.sys.mjs",
  UrlbarUtils: "resource:///modules/UrlbarUtils.sys.mjs",
});

ChromeUtils.defineLazyGetter(this, "TEST_BASE_URL", () =>
  getRootDirectory(gTestPath).replace(
    "chrome://mochitests/content",
    "https://example.com"
  )
);

XPCOMUtils.defineLazyServiceGetter(
  this,
  "clipboardHelper",
  "@mozilla.org/widget/clipboardhelper;1",
  "nsIClipboardHelper"
);

ChromeUtils.defineLazyGetter(this, "UrlbarTestUtils", () => {
  const { UrlbarTestUtils: module } = ChromeUtils.importESModule(
    "resource://testing-common/UrlbarTestUtils.sys.mjs"
  );
  module.init(this);
  return module;
});

ChromeUtils.defineLazyGetter(this, "SearchTestUtils", () => {
  const { SearchTestUtils: module } = ChromeUtils.importESModule(
    "resource://testing-common/SearchTestUtils.sys.mjs"
  );
  module.init(this);
  return module;
});

/**
 * Initializes an HTTP Server, and runs a task with it.
 *
 * @param {object} details {scheme, host, port}
 * @param {Function} taskFn The task to run, gets the server as argument.
 */
async function withHttpServer(
  details = { scheme: "http", host: "localhost", port: -1 },
  taskFn
) {
  let server = new HttpServer();
  let url = `${details.scheme}://${details.host}:${details.port}`;
  try {
    info(`starting HTTP Server for ${url}`);
    try {
      server.start(details.port);
      details.port = server.identity.primaryPort;
      server.identity.setPrimary(details.scheme, details.host, details.port);
    } catch (ex) {
      throw new Error("We can't launch our http server successfully. " + ex);
    }
    Assert.ok(
      server.identity.has(details.scheme, details.host, details.port),
      `${url} is listening.`
    );
    try {
      await taskFn(server);
    } catch (ex) {
      throw new Error("Exception in the task function " + ex);
    }
  } finally {
    server.identity.remove(details.scheme, details.host, details.port);
    try {
      await new Promise(resolve => server.stop(resolve));
    } catch (ex) {}
    server = null;
  }
}

/**
 * Updates the Top Sites feed.
 *
 * @param {Function} condition
 *   A callback that returns true after Top Sites are successfully updated.
 * @param {boolean} searchShortcuts
 *   True if Top Sites search shortcuts should be enabled.
 */
async function updateTopSites(condition, searchShortcuts = false) {
  // Toggle the pref to clear the feed cache and force an update.
  await SpecialPowers.pushPrefEnv({
    set: [
      [
        "browser.newtabpage.activity-stream.discoverystream.endpointSpocsClear",
        "",
      ],
      ["browser.newtabpage.activity-stream.feeds.system.topsites", false],
      ["browser.newtabpage.activity-stream.feeds.system.topsites", true],
      [
        "browser.newtabpage.activity-stream.improvesearch.topSiteSearchShortcuts",
        searchShortcuts,
      ],
    ],
  });

  if (Services.prefs.getBoolPref("browser.topsites.component.enabled")) {
    // The previous way of updating Top Sites was to toggle the preference which
    // removes the instance of the Top Sites Feed and re-creates it.
    TopSites.uninit();
    await TopSites.init();
  }

  // Wait for the feed to be updated.
  await TestUtils.waitForCondition(async () => {
    let sites;
    if (Services.prefs.getBoolPref("browser.topsites.component.enabled")) {
      sites = await TopSites.getSites();
    } else {
      sites = AboutNewTab.getTopSites();
    }
    return condition(sites);
  }, "Waiting for top sites to be updated");
}

async function installPersistTestEngines(globalDefault = "Example") {
  const CONFIG_V2 = [
    {
      recordType: "engine",
      identifier: "Example",
      base: {
        name: "Example",
        urls: {
          search: {
            base: "https://www.example.com/",
            searchTermParamName: "q",
          },
        },
      },
    },
    {
      recordType: "engine",
      identifier: "MochiSearch",
      base: {
        name: "MochiSearch",
        urls: {
          search: {
            base: "http://mochi.test:8888/",
            searchTermParamName: "q",
          },
        },
      },
    },
    {
      recordType: "defaultEngines",
      globalDefault,
      specificDefaults: [],
    },
  ];
  let persistSandbox = sinon.createSandbox();
  // Mostly to prevent warnings about missing icon urls for these engines.
  persistSandbox
    .stub(AppProvidedConfigEngine.prototype, "getIconURL")
    .returns(
      Promise.resolve(
        "data:image/x-icon;base64,R0lGODlhAQABAAAAACwAAAAAAQABAAA="
      )
    );
  info("Install Search Engines related to Persisted Search Tests");
  info(globalDefault);
  await SearchTestUtils.updateRemoteSettingsConfig(CONFIG_V2);
  return () => {
    persistSandbox.restore();
  };
}

async function resetApplicationProvidedEngines() {
  let settingsWritten = SearchTestUtils.promiseSearchNotification(
    "write-settings-to-disk-complete"
  );
  await SearchTestUtils.updateRemoteSettingsConfig();
  await settingsWritten;
}
