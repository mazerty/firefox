"use strict";

ChromeUtils.defineESModuleGetters(this, {
  MockRegistry: "resource://testing-common/MockRegistry.sys.mjs",
});

const MANIFEST = {
  name: "test-storage-managed@mozilla.com",
  description: "",
  type: "storage",
  data: {
    null: null,
    str: "hello",
    obj: {
      a: [2, 3],
      b: true,
    },
  },
};

AddonTestUtils.init(this);

const server = createHttpServer({ hosts: ["example.com"] });
server.registerDirectory("/data/", do_get_file("data"));

add_task(async function setup() {
  await ExtensionTestUtils.startAddonManager();

  let tmpDir = FileUtils.getDir("TmpD", ["native-manifests"]);
  tmpDir.createUnique(Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);

  let dirProvider = {
    getFile(property) {
      if (property.endsWith("NativeManifests")) {
        return tmpDir.clone();
      }
    },
  };
  Services.dirsvc.registerProvider(dirProvider);

  let typeSlug =
    AppConstants.platform === "linux" ? "managed-storage" : "ManagedStorage";
  await IOUtils.makeDirectory(PathUtils.join(tmpDir.path, typeSlug));

  let path = PathUtils.join(tmpDir.path, typeSlug, `${MANIFEST.name}.json`);
  await IOUtils.writeJSON(path, MANIFEST);

  let registry;
  if (AppConstants.platform === "win") {
    registry = new MockRegistry();
    registry.setValue(
      Ci.nsIWindowsRegKey.ROOT_KEY_CURRENT_USER,
      `Software\\\Mozilla\\\ManagedStorage\\${MANIFEST.name}`,
      "",
      path
    );
  }

  registerCleanupFunction(() => {
    Services.dirsvc.unregisterProvider(dirProvider);
    tmpDir.remove(true);
    if (registry) {
      registry.shutdown();
    }
  });
});

add_task(async function test_storage_managed() {
  let extension = ExtensionTestUtils.loadExtension({
    manifest: {
      browser_specific_settings: { gecko: { id: MANIFEST.name } },
      permissions: ["storage"],
    },

    async background() {
      await browser.test.assertRejects(
        browser.storage.managed.set({ a: 1 }),
        /storage.managed is read-only/,
        "browser.storage.managed.set() rejects because it's read only"
      );

      await browser.test.assertRejects(
        browser.storage.managed.remove("str"),
        /storage.managed is read-only/,
        "browser.storage.managed.remove() rejects because it's read only"
      );

      await browser.test.assertRejects(
        browser.storage.managed.clear(),
        /storage.managed is read-only/,
        "browser.storage.managed.clear() rejects because it's read only"
      );

      browser.test.sendMessage(
        "results",
        await Promise.all([
          browser.storage.managed.get(),
          browser.storage.managed.get("str"),
          browser.storage.managed.get(["null", "obj"]),
          browser.storage.managed.get({ str: "a", num: 2 }),
        ])
      );

      browser.test.sendMessage(
        "getKeysResults",
        await browser.storage.managed.getKeys()
      );
    },
  });

  await extension.startup();
  deepEqual(await extension.awaitMessage("results"), [
    MANIFEST.data,
    { str: "hello" },
    { null: null, obj: MANIFEST.data.obj },
    { str: "hello", num: 2 },
  ]);
  deepEqual(
    await extension.awaitMessage("getKeysResults"),
    Object.keys(MANIFEST.data)
  );
  await extension.unload();
});

add_task(async function test_storage_managed_from_content_script() {
  let extension = ExtensionTestUtils.loadExtension({
    manifest: {
      browser_specific_settings: { gecko: { id: MANIFEST.name } },
      permissions: ["storage"],
      content_scripts: [
        {
          js: ["contentscript.js"],
          matches: ["*://*/*"],
          run_at: "document_end",
        },
      ],
    },

    files: {
      "contentscript.js": async function () {
        browser.test.sendMessage(
          "results",
          await browser.storage.managed.get()
        );
      },
    },
  });

  await extension.startup();
  let contentPage = await ExtensionTestUtils.loadContentPage(
    "http://example.com/data/file_sample.html"
  );
  deepEqual(await extension.awaitMessage("results"), MANIFEST.data);
  await contentPage.close();
  await extension.unload();
});

add_task(async function test_manifest_not_found() {
  let extension = ExtensionTestUtils.loadExtension({
    manifest: {
      permissions: ["storage"],
    },

    async background() {
      await browser.test.assertRejects(
        browser.storage.managed.get({ a: 1 }),
        /Managed storage manifest not found/,
        "browser.storage.managed.get() rejects when without manifest"
      );

      browser.test.notifyPass();
    },
  });

  await extension.startup();
  await extension.awaitFinish();
  await extension.unload();
});

add_task(async function test_manifest_not_found() {
  let extension = ExtensionTestUtils.loadExtension({
    manifest: {
      permissions: ["storage"],
    },

    async background() {
      const dummyListener = () => {};
      browser.storage.managed.onChanged.addListener(dummyListener);
      browser.test.assertTrue(
        browser.storage.managed.onChanged.hasListener(dummyListener),
        "addListener works according to hasListener"
      );
      browser.storage.managed.onChanged.removeListener(dummyListener);

      // We should get a warning for each registration.
      browser.storage.managed.onChanged.addListener(() => {});
      browser.storage.managed.onChanged.addListener(() => {});
      browser.storage.managed.onChanged.addListener(() => {});

      // Invoke the storage.managed API to make sure that we have made a
      // round trip to the parent process and back. This is because event
      // registration is async but we cannot await (bug 1300234).
      await browser.test.assertRejects(
        browser.storage.managed.get({ a: 1 }),
        /Managed storage manifest not found/,
        "browser.storage.managed.get() rejects when without manifest"
      );

      browser.test.notifyPass();
    },
  });

  let { messages } = await promiseConsoleOutput(async () => {
    await extension.startup();
    await extension.awaitFinish();
    await extension.unload();
  });
  const UNSUP_EVENT_WARNING = `attempting to use listener "storage.managed.onChanged", which is unimplemented`;
  messages = messages.filter(msg => msg.message.includes(UNSUP_EVENT_WARNING));
  Assert.equal(messages.length, 4, "Expected msg for each addListener call");
});
