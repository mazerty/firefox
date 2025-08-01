/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Tests to ensure that when a user installs or uninstalls an add-on,
 * we correctly handle the overriding of default and/or parameters
 * according to the allowlist.
 */

"use strict";

const kBaseURL = "https://example.com/";
const kSearchEngineURL = `${kBaseURL}?q={searchTerms}&foo=myparams`;
const kOverriddenEngineName = "Simple Engine";

const allowlist = [
  {
    thirdPartyId: "test@thirdparty.example.com",
    overridesAppIdv2: "simple",
    urls: [],
  },
];

const tests = [
  {
    title: "test_not_changing_anything",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: "MozParamsTest2",
      keyword: "MozSearch",
      search_url: kSearchEngineURL,
    },
    expected: {
      switchToDefaultAllowed: false,
      canInstallEngine: true,
      overridesEngine: false,
    },
  },
  {
    title: "test_changing_default_engine",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kSearchEngineURL,
    },
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: false,
    },
  },
  {
    title: "test_changing_default_engine",
    startupReason: "ADDON_ENABLE",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kSearchEngineURL,
    },
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: false,
    },
  },
  {
    title: "test_overriding_default_engine",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kSearchEngineURL,
    },
    allowlistUrls: [
      {
        search_url: kSearchEngineURL,
      },
    ],
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: true,
      searchUrl: kSearchEngineURL,
    },
  },
  {
    title: "test_overriding_default_engine_enable",
    startupReason: "ADDON_ENABLE",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kSearchEngineURL,
    },
    allowlistUrls: [
      {
        search_url: kSearchEngineURL,
      },
    ],
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: true,
      searchUrl: kSearchEngineURL,
    },
  },
  {
    title: "test_overriding_default_engine_different_url",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kSearchEngineURL + "a",
    },
    allowlistUrls: [
      {
        search_url: kSearchEngineURL,
      },
    ],
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: false,
    },
  },
  {
    title: "test_overriding_default_engine_get_params",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kBaseURL,
      search_url_get_params: "q={searchTerms}&enc=UTF-8",
    },
    allowlistUrls: [
      {
        search_url: kBaseURL,
        search_url_get_params: "q={searchTerms}&enc=UTF-8",
      },
    ],
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: true,
      searchUrl: `${kBaseURL}?q={searchTerms}&enc=UTF-8`,
    },
  },
  {
    title: "test_overriding_default_engine_different_get_params",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kBaseURL,
      search_url_get_params: "q={searchTerms}&enc=UTF-8a",
    },
    allowlistUrls: [
      {
        search_url: kBaseURL,
        search_url_get_params: "q={searchTerms}&enc=UTF-8",
      },
    ],
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: false,
    },
  },
  {
    title: "test_overriding_default_engine_post_params",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kBaseURL,
      search_url_post_params: "q={searchTerms}&enc=UTF-8",
    },
    allowlistUrls: [
      {
        search_url: kBaseURL,
        search_url_post_params: "q={searchTerms}&enc=UTF-8",
      },
    ],
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: true,
      searchUrl: `${kBaseURL}`,
      postData: "q={searchTerms}&enc=UTF-8",
    },
  },
  {
    title: "test_overriding_default_engine_different_post_params",
    startupReason: "ADDON_INSTALL",
    search_provider: {
      is_default: true,
      name: kOverriddenEngineName,
      keyword: "MozSearch",
      search_url: kBaseURL,
      search_url_post_params: "q={searchTerms}&enc=UTF-8a",
    },
    allowlistUrls: [
      {
        search_url: kBaseURL,
        search_url_post_params: "q={searchTerms}&enc=UTF-8",
      },
    ],
    expected: {
      switchToDefaultAllowed: true,
      canInstallEngine: false,
      overridesEngine: false,
    },
  },
];

let baseExtension;
let remoteSettingsStub;

add_setup(async function () {
  SearchTestUtils.setRemoteSettingsConfig([
    { identifier: "originalDefault" },
    {
      identifier: "simple",
      base: { name: kOverriddenEngineName },
    },
  ]);
  await SearchTestUtils.initXPCShellAddonManager();
  await Services.search.init();

  baseExtension = ExtensionTestUtils.loadExtension({
    manifest: {
      browser_specific_settings: {
        gecko: {
          id: "test@thirdparty.example.com",
        },
      },
    },
    useAddonManager: "permanent",
  });
  await baseExtension.startup();

  const settings = await RemoteSettings(SearchUtils.SETTINGS_ALLOWLIST_KEY);
  remoteSettingsStub = sinon.stub(settings, "get").returns([]);

  registerCleanupFunction(async () => {
    await baseExtension.unload();
    sinon.restore();
  });
});

for (const test of tests) {
  add_task(async () => {
    info(test.title);

    let extension = {
      ...baseExtension,
      startupReason: test.startupReason,
      manifest: {
        chrome_settings_overrides: {
          search_provider: test.search_provider,
        },
      },
    };

    if (test.expected.overridesEngine) {
      remoteSettingsStub.returns([
        { ...allowlist[0], urls: test.allowlistUrls },
      ]);
    }

    let result = await Services.search.maybeSetAndOverrideDefault(extension);
    Assert.equal(
      result.canChangeToConfigEngine,
      test.expected.switchToDefaultAllowed,
      "Should have returned the correct value for allowing switch to default or not."
    );
    Assert.equal(
      result.canInstallEngine,
      test.expected.canInstallEngine,
      "Should have returned the correct value for allowing to install the engine or not."
    );

    let engine = await Services.search.getEngineByName(kOverriddenEngineName);
    Assert.equal(
      !!engine.wrappedJSObject.getAttr("overriddenBy"),
      test.expected.overridesEngine,
      "Should have correctly overridden or not."
    );

    Assert.equal(
      engine.telemetryId,
      "simple" + (test.expected.overridesEngine ? "-addon" : ""),
      "Should set the correct telemetry Id"
    );

    if (test.expected.overridesEngine) {
      let submission = engine.getSubmission("{searchTerms}");
      Assert.equal(
        decodeURI(submission.uri.spec),
        test.expected.searchUrl,
        "Should have set the correct url on an overriden engine"
      );

      if (test.expected.postData) {
        let sis = Cc["@mozilla.org/scriptableinputstream;1"].createInstance(
          Ci.nsIScriptableInputStream
        );
        sis.init(submission.postData);
        let data = sis.read(submission.postData.available());
        Assert.equal(
          decodeURIComponent(data),
          test.expected.postData,
          "Should have overridden the postData"
        );
      }

      // As we're not testing the WebExtension manager as well,
      // set this engine as default so we can check the telemetry data.
      let oldDefaultEngine = Services.search.defaultEngine;
      await Services.search.setDefault(
        engine,
        Ci.nsISearchService.CHANGE_REASON_UNKNOWN
      );

      let engineInfo = Services.search.getDefaultEngineInfo();
      Assert.deepEqual(
        engineInfo,
        {
          defaultSearchEngine: "simple-addon",
          defaultSearchEngineData: {
            loadPath: "[app]simple",
            name: "Simple Engine",
            submissionURL: test.expected.searchUrl.replace("{searchTerms}", ""),
          },
        },
        "Should return the extended identifier and alternate submission url to telemetry"
      );
      await Services.search.setDefault(
        oldDefaultEngine,
        Ci.nsISearchService.CHANGE_REASON_UNKNOWN
      );

      engine.wrappedJSObject.removeExtensionOverride();
    }
  });
}
