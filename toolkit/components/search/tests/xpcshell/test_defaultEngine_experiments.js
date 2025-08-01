/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/*
 * Test that defaultEngine property can be set and yields the proper events and
 * behavior (search results)
 */

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  NimbusFeatures: "resource://nimbus/ExperimentAPI.sys.mjs",
});

const CONFIG = [
  { identifier: "engine1" },
  { identifier: "engine2" },
  {
    identifier: "exp2",
    variants: [
      {
        environment: { allRegionsAndLocales: true, experiment: "exp2" },
      },
    ],
  },
  {
    identifier: "exp3",
    variants: [
      {
        environment: { allRegionsAndLocales: true, experiment: "exp3" },
      },
    ],
  },
  {
    recordType: "defaultEngines",
    globalDefault: "engine1",
    globalDefaultPrivate: "engine1",
    specificDefaults: [
      {
        environment: { experiment: "exp1" },
        default: "engine2",
      },
      {
        environment: { experiment: "exp2" },
        defaultPrivate: "exp2",
      },
      {
        environment: { experiment: "exp3" },
        default: "exp3",
      },
    ],
  },
];

let getVariableStub;

let defaultGetVariable = name => {
  if (name == "separatePrivateDefaultUIEnabled") {
    return true;
  }
  if (name == "separatePrivateDefaultUrlbarResultEnabled") {
    return false;
  }
  return undefined;
};

add_setup(async () => {
  Services.prefs.setBoolPref(
    SearchUtils.BROWSER_SEARCH_PREF + "separatePrivateDefault.ui.enabled",
    true
  );
  Services.prefs.setBoolPref(
    SearchUtils.BROWSER_SEARCH_PREF + "separatePrivateDefault",
    true
  );

  sinon.spy(NimbusFeatures.searchConfiguration, "onUpdate");
  sinon.stub(NimbusFeatures.searchConfiguration, "ready").resolves();
  getVariableStub = sinon.stub(
    NimbusFeatures.searchConfiguration,
    "getVariable"
  );
  getVariableStub.callsFake(defaultGetVariable);

  do_get_profile();
  Services.fog.initializeFOG();

  SearchTestUtils.setRemoteSettingsConfig(CONFIG);

  let promiseSaved = promiseAfterSettings();
  await Services.search.init();
  await promiseSaved;

  registerCleanupFunction(async () => {
    sinon.restore();
  });
});

async function switchExperiment(newExperiment) {
  let promiseReloaded =
    SearchTestUtils.promiseSearchNotification("engines-reloaded");
  let promiseSaved = promiseAfterSettings();

  if (newExperiment) {
    Services.prefs.setStringPref("browser.search.experiment", newExperiment);
  } else {
    Services.prefs.clearUserPref("browser.search.experiment");
  }

  await promiseReloaded;
  await promiseSaved;
}

function getSettingsAttribute(setting, engine) {
  return Services.search.wrappedJSObject._settings.getVerifiedMetaDataAttribute(
    setting,
    engine.isConfigEngine
  );
}

add_task(async function test_experiment_setting() {
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have the application default engine as default"
  );

  // Start the experiment.
  await switchExperiment("exp1");

  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have set the experiment engine as default"
  );

  // End the experiment.
  await switchExperiment("");

  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have reset the default engine to the application default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should have kept the saved attribute as empty"
  );
});

add_task(async function test_experiment_setting_to_same_as_user() {
  await Services.search.setDefault(
    Services.search.getEngineByName("engine2"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );

  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have the user selected engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "engine2"
  );

  // Start the experiment, ensure user default is maintained.
  await switchExperiment("exp1");
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "engine2"
  );

  Assert.equal(
    Services.search.appDefaultEngine.name,
    "engine2",
    "Should have set the experiment engine as default"
  );
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have set the experiment engine as default"
  );

  // End the experiment.
  await switchExperiment("");

  Assert.equal(
    Services.search.appDefaultEngine.name,
    "engine1",
    "Should have set the app default engine correctly"
  );
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have kept the engine the same "
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "engine2",
    "Should have kept the saved attribute as the user's preference"
  );
});

add_task(async function test_experiment_setting_user_changed_back_during() {
  await Services.search.setDefault(
    Services.search.getEngineByName("engine1"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );

  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have the application default engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should have an empty settings attribute"
  );

  // Start the experiment.
  await switchExperiment("exp1");

  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have set the experiment engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should still have an empty settings attribute"
  );

  // User resets to the original default engine.
  await Services.search.setDefault(
    Services.search.getEngineByName("engine1"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have the user selected engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "engine1"
  );

  // Ending the experiment should keep the original default and reset the
  // saved attribute.
  await switchExperiment("");

  Assert.equal(
    Services.search.appDefaultEngine.name,
    "engine1",
    "Should have set the app default engine correctly"
  );
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have kept the engine the same"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should have reset the saved attribute to empty after the experiment ended"
  );
});

add_task(async function test_experiment_setting_user_changed_back_private() {
  await Services.search.setDefaultPrivate(
    Services.search.getEngineByName("engine1"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );

  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "engine1",
    "Should have the user selected engine as default"
  );
  Assert.equal(
    getSettingsAttribute(
      "privateDefaultEngineId",
      Services.search.defaultPrivateEngine
    ),
    "",
    "Should have an empty settings attribute"
  );

  // Start the experiment.
  await switchExperiment("exp2");

  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "exp2",
    "Should have set the experiment engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should still have an empty settings attribute"
  );

  // User resets to the original default engine.
  await Services.search.setDefaultPrivate(
    Services.search.getEngineByName("engine1"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );
  Assert.equal(
    Services.search.defaultPrivateEngine.name,
    "engine1",
    "Should have the user selected engine as default"
  );
  Assert.equal(
    getSettingsAttribute(
      "privateDefaultEngineId",
      Services.search.defaultPrivateEngine
    ),
    "engine1"
  );

  // Ending the experiment should keep the original default and reset the
  // saved attribute.
  await switchExperiment("");

  Assert.equal(Services.search.appPrivateDefaultEngine.name, "engine1");
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have kept the engine the same "
  );
  Assert.equal(
    getSettingsAttribute(
      "privateDefaultEngineId",
      Services.search.defaultPrivateEngine
    ),
    "",
    "Should have reset the saved attribute to empty after the experiment ended"
  );
});

add_task(async function test_experiment_setting_user_changed_to_other_during() {
  await Services.search.setDefault(
    Services.search.getEngineByName("engine1"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );

  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have the application default engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should have an empty settings attribute"
  );

  // Start the experiment.
  await switchExperiment("exp3");

  Assert.equal(
    Services.search.defaultEngine.name,
    "exp3",
    "Should have set the experiment engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should still have an empty settings attribute"
  );

  // User changes to a different default engine
  await Services.search.setDefault(
    Services.search.getEngineByName("engine2"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have the user selected engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "engine2",
    "Should have correctly set the user's default in settings"
  );

  // Ending the experiment should keep the original default and reset the
  // saved attribute.
  await switchExperiment("");

  Assert.equal(
    Services.search.appDefaultEngine.name,
    "engine1",
    "Should have set the app default engine correctly"
  );
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have kept the user's choice of engine"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "engine2",
    "Should have kept the user's choice in settings"
  );
});

add_task(async function test_experiment_setting_user_hid_app_default_during() {
  Services.prefs.setBoolPref(
    SearchUtils.BROWSER_SEARCH_PREF + "separatePrivateDefault",
    false
  );
  await Services.search.setDefault(
    Services.search.getEngineByName("engine1"),
    Ci.nsISearchService.CHANGE_REASON_UNKNOWN
  );

  Assert.equal(
    Services.search.defaultEngine.name,
    "engine1",
    "Should have the application default engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should have an empty settings attribute"
  );

  // Start the experiment.
  await switchExperiment("exp3");

  Assert.equal(
    Services.search.defaultEngine.name,
    "exp3",
    "Should have set the experiment engine as default"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "",
    "Should still have an empty settings attribute"
  );

  // User hides the original application engine
  await Services.search.removeEngine(
    Services.search.getEngineByName("engine1")
  );
  Assert.equal(
    Services.search.getEngineByName("engine1").hidden,
    true,
    "Should have hid the selected engine"
  );

  // Ending the experiment should keep the original default and reset the
  // saved attribute.
  await switchExperiment("");

  Assert.equal(
    Services.search.appDefaultEngine.name,
    "engine1",
    "Should have set the app default engine correctly"
  );
  Assert.equal(
    Services.search.defaultEngine.hidden,
    false,
    "Should not have set default engine to an engine that is hidden"
  );
  Assert.equal(
    Services.search.defaultEngine.name,
    "engine2",
    "Should have reset the user's engine to the next available engine"
  );
  Assert.equal(
    getSettingsAttribute("defaultEngineId", Services.search.defaultEngine),
    "engine2",
    "Should have saved the choice in settings"
  );
});
