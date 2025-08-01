/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/*
 * Test initializing with an engine that's a duplicate of an app-provided
 * engine.
 */

"use strict";

const { getAppInfo } = ChromeUtils.importESModule(
  "resource://testing-common/AppInfo.sys.mjs"
);

const DUPLICATE_ENGINE_ID = "f3094ab7-3302-4d5b-9f79-ea92c9a49f87";

const enginesSettings = {
  version: SearchUtils.SETTINGS_VERSION,
  buildID: "TBD",
  appVersion: "TBD",
  locale: "en-US",
  metaData: {
    searchDefault: "Test search engine",
    searchDefaultHash: "TBD",
    // Intentionally in the past, but shouldn't actually matter for this test.
    searchDefaultExpir: 1567694909002,
    current: "",
    hash: "TBD",
    visibleDefaultEngines:
      "engine,engine-pref,engine-rel-searchform-purpose,engine-chromeicon,engine-resourceicon,engine-reordered",
    visibleDefaultEnginesHash: "TBD",
  },
  engines: [
    {
      id: "appDefault",
      _metaData: { alias: null },
      _isConfigEngine: true,
      _name: "appDefault",
    },
    {
      id: "other",
      _metaData: { alias: null },
      _isConfigEngine: true,
      _name: "other",
    },
    // This is a user-installed engine - the only one that was listed due to the
    // original issue.
    {
      id: DUPLICATE_ENGINE_ID,
      _name: "appDefault",
      _shortName: "appDefault",
      _loadPath: "[https]oldduplicateversion",
      description: "An old near duplicate version of appDefault",
      _iconURL:
        "data:image/x-icon;base64,AAABAAEAEBAQAAEABAAoAQAAFgAAACgAAAAQAAAAIAAAAAEABAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAEAgQAhIOEAMjHyABIR0gA6ejpAGlqaQCpqKkAKCgoAPz9/AAZGBkAmJiYANjZ2ABXWFcAent6ALm6uQA8OjwAiIiIiIiIiIiIiI4oiL6IiIiIgzuIV4iIiIhndo53KIiIiB/WvXoYiIiIfEZfWBSIiIEGi/foqoiIgzuL84i9iIjpGIoMiEHoiMkos3FojmiLlUipYliEWIF+iDe0GoRa7D6GPbjcu1yIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      _iconMapObj: {
        '{"width":16,"height":16}':
          "data:image/x-icon;base64,AAABAAEAEBAQAAEABAAoAQAAFgAAACgAAAAQAAAAIAAAAAEABAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAEAgQAhIOEAMjHyABIR0gA6ejpAGlqaQCpqKkAKCgoAPz9/AAZGBkAmJiYANjZ2ABXWFcAent6ALm6uQA8OjwAiIiIiIiIiIiIiI4oiL6IiIiIgzuIV4iIiIhndo53KIiIiB/WvXoYiIiIfEZfWBSIiIEGi/foqoiIgzuL84i9iIjpGIoMiEHoiMkos3FojmiLlUipYliEWIF+iDe0GoRa7D6GPbjcu1yIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      },
      _metaData: {
        order: 1,
      },
      _urls: [
        {
          template: "https://example.com/?myquery={searchTerms}",
          rels: [],
          resultDomain: "example.com",
          params: [],
        },
      ],
      queryCharset: "UTF-8",
      filePath: "TBD",
    },
  ],
};

add_setup(async function () {
  // Allow telemetry probes which may otherwise be disabled for some applications (e.g. Thunderbird)
  Services.prefs.setBoolPref(
    "toolkit.telemetry.testing.overrideProductsCheck",
    true
  );

  SearchTestUtils.setRemoteSettingsConfig([
    {
      identifier: "appDefault",
      base: {
        urls: {
          search: { base: "https://1.example.com", searchTermParamName: "q" },
        },
      },
    },
    { identifier: "other" },
  ]);
  Services.prefs.setCharPref(SearchUtils.BROWSER_SEARCH_PREF + "region", "US");
  Services.locale.availableLocales = ["en-US"];
  Services.locale.requestedLocales = ["en-US"];

  // We dynamically generate the hashes because these depend on the profile.
  enginesSettings.metaData.searchDefaultHash = SearchUtils.getVerificationHash(
    enginesSettings.metaData.searchDefault
  );
  enginesSettings.metaData.hash = SearchUtils.getVerificationHash(
    enginesSettings.metaData.current
  );
  enginesSettings.metaData.visibleDefaultEnginesHash =
    SearchUtils.getVerificationHash(
      enginesSettings.metaData.visibleDefaultEngines
    );
  let appInfo = getAppInfo();
  enginesSettings.buildID = appInfo.platformBuildID;
  enginesSettings.appVersion = appInfo.version;

  await IOUtils.write(
    PathUtils.join(PathUtils.profileDir, SETTINGS_FILENAME),
    new TextEncoder().encode(JSON.stringify(enginesSettings)),
    { compress: true }
  );

  consoleAllowList.push("Failed to load");
});

add_task(async function test_cached_duplicate() {
  info("init search service");

  let initResult = await Services.search.init();

  info("init'd search service");
  Assert.ok(
    Components.isSuccessCode(initResult),
    "Should have successfully created the search service"
  );

  let engine = Services.search.getEngineByName("appDefault");
  let submission = engine.getSubmission("foo");
  Assert.equal(
    submission.uri.spec,
    "https://1.example.com/?q=foo",
    "Should have not changed the app provided engine."
  );

  Assert.ok(
    !(await Services.search.getEngineById(DUPLICATE_ENGINE_ID)),
    "Should not have added the duplicate engine"
  );

  let engines = await Services.search.getEngines();

  Assert.deepEqual(
    engines.map(e => e.name),
    ["appDefault", "other"],
    "Should have the expected default engines"
  );
});
