import assert from "node:assert/strict";
import { glob, readFile } from "node:fs/promises";
import test from "node:test";

import { unpinnedActions } from "../../tooling/ci/verify-pins.ts";

test("rejects mutable action references", () => {
  assert.deepEqual(unpinnedActions("- uses: actions/checkout@v7"), [
    "actions/checkout@v7",
  ]);
});

test("accepts immutable and local action references", () => {
  const sha = "3d3c42e5aac5ba805825da76410c181273ba90b1";
  assert.deepEqual(
    unpinnedActions(`- uses: actions/checkout@${sha}\n- uses: ./local`),
    [],
  );
});

test("repository workflows pin every external action", async () => {
  for await (const path of glob(".github/workflows/*.{yml,yaml}")) {
    const workflow = await readFile(path, "utf8");
    assert.deepEqual(
      unpinnedActions(workflow),
      [],
      `${path} contains a mutable action reference`,
    );
  }
});
