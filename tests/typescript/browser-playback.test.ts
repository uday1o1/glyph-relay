import assert from "node:assert/strict";
import test from "node:test";

import {
  oracleFrameOrdinals,
  oracleRtpTimestamp,
} from "../../tooling/m0/qualify-browser-playback.ts";
import { browserMatrixRunDefinitions } from "../../tooling/m0/qualify-browser-matrix.ts";

test("selects four deterministic steady-state oracle frames", () => {
  const ordinals = oracleFrameOrdinals(1800);
  assert.deepEqual(ordinals, [720, 900, 1080, 1260]);
  assert.deepEqual(
    ordinals.map(oracleRtpTimestamp),
    [2157000, 2697000, 3237000, 3777000],
  );
});

test("rejects an undersized oracle sequence and invalid ordinal", () => {
  assert.throws(() => oracleFrameOrdinals(9), /oracle_frame_count_invalid/);
  assert.throws(() => oracleRtpTimestamp(-1), /oracle_frame_ordinal_invalid/);
});

test("freezes the complete two-browser playback and recovery matrix", () => {
  const runs = browserMatrixRunDefinitions();
  assert.equal(runs.length, 18);
  for (const browser of ["chromium", "firefox"]) {
    const browserRuns = runs.filter((run) => run.browser === browser);
    assert.equal(
      browserRuns.filter((run) => run.scenario === "ZERO_LOSS").length,
      5,
    );
    assert.deepEqual(
      browserRuns
        .filter((run) => run.scenario === "ROLLOVER_LOSS")
        .map((run) => run.faultLossExtendedSequence),
      [65_534, 65_535, 65_536],
    );
    const pli = browserRuns.find((run) => run.scenario === "PLI_RECOVERY");
    assert.equal(pli?.startFrame, 240);
    assert.equal(pli?.injectPliAfterFrame, 901);
  }
});
