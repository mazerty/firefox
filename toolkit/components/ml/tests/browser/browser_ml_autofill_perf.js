/* Any copyright is dedicated to the Public Domain.
http://creativecommons.org/publicdomain/zero/1.0/ */
"use strict";

const perfMetadata = {
  owner: "GenAI Team",
  name: "browser_ml_autofill_perf.js",
  description: "Template test for latency for ML Autofill model",
  options: {
    default: {
      perfherder: true,
      perfherder_metrics: [
        {
          name: "AUTOFILL-pipeline-ready-latency",
          unit: "ms",
          shouldAlert: false,
        },
        {
          name: "AUTOFILL-initialization-latency",
          unit: "ms",
          shouldAlert: false,
        },
        {
          name: "AUTOFILL-model-run-latency",
          unit: "ms",
          shouldAlert: false,
        },
        {
          name: "AUTOFILL-total-memory-usage",
          unit: "MiB",
          shouldAlert: false,
        },
        {
          name: "tokenSpeed",
          unit: "tokens/s",
          shouldAlert: false,
          lowerIsBetter: false,
        },
        {
          name: "charactersSpeed",
          unit: "chars/s",
          shouldAlert: false,
          lowerIsBetter: false,
        },
      ],
      verbose: true,
      manifest: "perftest.toml",
      manifest_flavor: "browser-chrome",
      try_platform: ["linux", "mac", "win"],
    },
  },
};

requestLongerTimeout(10);

/**
 * Tests local Autofill model
 */
add_task(async function test_ml_generic_pipeline() {
  const options = new PipelineOptions({
    taskName: "text-classification",
    modelId: "Mozilla/tinybert-uncased-autofill",
    modelHubUrlTemplate: "{model}/{revision}",
    modelRevision: "main",
    dtype: "int8",
    numThreads: 2,
    timeoutMS: -1,
  });

  const args = [
    "<input id='new-password' autocomplete='new-password' placeholder='Please enter a new password'>",
  ];

  const request = {
    args,
    options: { pooling: "mean", normalize: true },
  };

  await perfTest({ name: "autofill", options, request });
});
