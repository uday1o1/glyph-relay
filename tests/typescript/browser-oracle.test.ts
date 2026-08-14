import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import test from "node:test";

import {
  compareOracleFrames,
  freezeOracleTolerance,
  oracleComparisonPasses,
  oracleFrameKey,
  parseOracleFreezeInput,
  type OracleComparison,
  type OracleFrame,
  type ZeroLossOracleRun,
} from "../../tooling/m0/browser-oracle.ts";

function frame(rgba: readonly number[], timestamp = "4294967297"): OracleFrame {
  return {
    dependencyEpoch: 3,
    extendedRtpTimestamp: timestamp,
    width: 2,
    height: 1,
    rgba: Uint8Array.from(rgba),
  };
}

function runs(comparison: OracleComparison): ZeroLossOracleRun[] {
  return Array.from({ length: 10 }, (_, index) => ({
    runId: `run-${index}`,
    browser: index < 5 ? "chromium" : "firefox",
    zeroLoss: true,
    infrastructureStatus: "COMPLETE",
    decoderErrors: 0,
    comparisons: [comparison],
  }));
}

test("compares exact epoch and extended RTP timestamp keyed RGBA frames", () => {
  const reference = frame([0, 10, 20, 255, 30, 40, 50, 255]);
  const presented = frame([0, 11, 20, 255, 33, 40, 50, 255]);
  const comparison = compareOracleFrames(reference, presented);
  assert.equal(comparison.key, "3:4294967297");
  assert.equal(comparison.maximumAbsoluteChannelError, 3);
  assert.equal(comparison.differingPixels, 2);
  assert.equal(comparison.differingPixelFraction, 1);
  assert.ok(
    Math.abs(comparison.rootMeanSquareChannelError - Math.sqrt(10 / 8)) < 1e-12,
  );
  assert.throws(
    () => compareOracleFrames(reference, frame([...presented.rgba], "7")),
    /key_mismatch/,
  );
  assert.equal(oracleFrameKey(reference), comparison.key);
});

test("freezes only ten complete zero-loss runs split equally across browsers", () => {
  const comparison = compareOracleFrames(
    frame([0, 10, 20, 255, 30, 40, 50, 255]),
    frame([0, 11, 20, 255, 33, 40, 50, 255]),
  );
  const observations = runs(comparison);
  const frozen = freezeOracleTolerance(observations, [comparison.key]);
  assert.equal(frozen.state, "FROZEN");
  assert.equal(frozen.maximumAbsoluteChannelError, 3);
  assert.equal(frozen.sourceDigestSha256.length, 64);
  assert.equal(oracleComparisonPasses(comparison, frozen), true);

  assert.throws(
    () => freezeOracleTolerance(observations.slice(1), [comparison.key]),
    /exactly_ten/,
  );
  const excluded = observations.map((run) => ({ ...run }));
  excluded[0] = {
    ...excluded[0]!,
    infrastructureStatus: "FAILED" as "COMPLETE",
  };
  assert.throws(
    () => freezeOracleTolerance(excluded, [comparison.key]),
    /inadmissible/,
  );
  const unbalanced = observations.map((run) => ({
    ...run,
    browser: "chromium" as const,
  }));
  assert.throws(
    () => freezeOracleTolerance(unbalanced, [comparison.key]),
    /five_runs_per_browser/,
  );
});

test("strictly parses the versioned zero-loss observation envelope", () => {
  const comparison = compareOracleFrames(
    frame([0, 10, 20, 255, 30, 40, 50, 255]),
    frame([0, 11, 20, 255, 33, 40, 50, 255]),
  );
  const input = {
    schemaVersion: 1,
    protocol: "browser_oracle_zero_loss_v1",
    requiredFrameKeys: [comparison.key],
    runs: runs(comparison),
  };
  assert.equal(parseOracleFreezeInput(input).runs.length, 10);
  assert.throws(
    () => parseOracleFreezeInput({ ...input, unreviewed: true }),
    /fields_invalid/,
  );
  assert.throws(
    () =>
      parseOracleFreezeInput({
        ...input,
        runs: [{ ...input.runs[0], decoderErrors: 1 }],
      }),
    /run_invalid/,
  );
});

test("freeze CLI writes one validated no-clobber artifact", async (context) => {
  const comparison = compareOracleFrames(
    frame([0, 10, 20, 255, 30, 40, 50, 255]),
    frame([0, 11, 20, 255, 33, 40, 50, 255]),
  );
  const directory = await mkdtemp(join(tmpdir(), "glyphrelay-oracle-"));
  context.after(() => rm(directory, { recursive: true, force: true }));
  const input = join(directory, "observations.json");
  const output = join(directory, "frozen.json");
  await writeFile(
    input,
    JSON.stringify({
      schemaVersion: 1,
      protocol: "browser_oracle_zero_loss_v1",
      requiredFrameKeys: [comparison.key],
      runs: runs(comparison),
    }),
  );
  const command = [
    resolve("tooling/m0/freeze-browser-oracle.ts"),
    "--input",
    input,
    "--output",
    output,
  ];
  const first = spawnSync(process.execPath, command, { encoding: "utf8" });
  assert.equal(first.status, 0, first.stderr);
  assert.equal(JSON.parse(await readFile(output, "utf8")).state, "FROZEN");
  const replay = spawnSync(process.execPath, command, { encoding: "utf8" });
  assert.notEqual(replay.status, 0);
  assert.match(replay.stderr, /EEXIST/);
});
