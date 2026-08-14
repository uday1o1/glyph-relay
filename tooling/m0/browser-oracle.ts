import { createHash } from "node:crypto";

export interface OracleFrame {
  dependencyEpoch: number;
  extendedRtpTimestamp: string;
  width: number;
  height: number;
  rgba: Uint8Array;
}

export interface OracleComparison {
  key: string;
  width: number;
  height: number;
  maximumAbsoluteChannelError: number;
  differingPixels: number;
  differingPixelFraction: number;
  rootMeanSquareChannelError: number;
}

export interface ZeroLossOracleRun {
  runId: string;
  browser: "chromium" | "firefox";
  zeroLoss: true;
  infrastructureStatus: "COMPLETE";
  decoderErrors: 0;
  comparisons: readonly OracleComparison[];
}

export interface FrozenOracleTolerance {
  schemaVersion: 1;
  protocol: "browser_oracle_v1";
  state: "FROZEN";
  requiredZeroLossRuns: 10;
  frameKey: "dependency_epoch:extended_rtp_timestamp";
  sourceRunIds: readonly string[];
  sourceDigestSha256: string;
  maximumAbsoluteChannelError: number;
  maximumDifferingPixelFraction: number;
  maximumRootMeanSquareChannelError: number;
}

export interface OracleFreezeInput {
  schemaVersion: 1;
  protocol: "browser_oracle_zero_loss_v1";
  requiredFrameKeys: readonly string[];
  runs: readonly ZeroLossOracleRun[];
}

function record(value: unknown, name: string): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${name}_invalid`);
  }
  return value as Record<string, unknown>;
}

function exactKeys(
  value: Record<string, unknown>,
  expected: readonly string[],
  name: string,
): void {
  const actual = Object.keys(value).sort();
  const sortedExpected = [...expected].sort();
  if (
    actual.length !== sortedExpected.length ||
    actual.some((key, index) => key !== sortedExpected[index])
  ) {
    throw new Error(`${name}_fields_invalid`);
  }
}

function assertSafeNonnegativeInteger(value: number, name: string): void {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error(`${name}_invalid`);
  }
}

function assertFiniteRange(
  value: number,
  minimum: number,
  maximum: number,
  name: string,
): void {
  if (!Number.isFinite(value) || value < minimum || value > maximum) {
    throw new Error(`${name}_invalid`);
  }
}

export function oracleFrameKey(
  frame: Pick<OracleFrame, "dependencyEpoch" | "extendedRtpTimestamp">,
): string {
  assertSafeNonnegativeInteger(frame.dependencyEpoch, "dependency_epoch");
  if (!/^(0|[1-9]\d*)$/.test(frame.extendedRtpTimestamp)) {
    throw new Error("extended_rtp_timestamp_invalid");
  }
  return `${frame.dependencyEpoch}:${frame.extendedRtpTimestamp}`;
}

export function compareOracleFrames(
  reference: OracleFrame,
  presented: OracleFrame,
): OracleComparison {
  const referenceKey = oracleFrameKey(reference);
  const presentedKey = oracleFrameKey(presented);
  if (referenceKey !== presentedKey) {
    throw new Error("oracle_frame_key_mismatch");
  }
  assertSafeNonnegativeInteger(reference.width, "reference_width");
  assertSafeNonnegativeInteger(reference.height, "reference_height");
  assertSafeNonnegativeInteger(presented.width, "presented_width");
  assertSafeNonnegativeInteger(presented.height, "presented_height");
  if (
    reference.width === 0 ||
    reference.height === 0 ||
    reference.width !== presented.width ||
    reference.height !== presented.height
  ) {
    throw new Error("oracle_frame_geometry_mismatch");
  }
  const expectedBytes = reference.width * reference.height * 4;
  if (
    !Number.isSafeInteger(expectedBytes) ||
    reference.rgba.length !== expectedBytes ||
    presented.rgba.length !== expectedBytes
  ) {
    throw new Error("oracle_frame_rgba_size_invalid");
  }

  let maximumAbsoluteChannelError = 0;
  let differingPixels = 0;
  let squaredError = 0;
  for (let pixelOffset = 0; pixelOffset < expectedBytes; pixelOffset += 4) {
    let pixelDiffers = false;
    for (let channel = 0; channel < 4; channel += 1) {
      const referenceValue = reference.rgba[pixelOffset + channel];
      const presentedValue = presented.rgba[pixelOffset + channel];
      if (referenceValue === undefined || presentedValue === undefined) {
        throw new Error("oracle_frame_rgba_size_invalid");
      }
      const error = Math.abs(referenceValue - presentedValue);
      maximumAbsoluteChannelError = Math.max(
        maximumAbsoluteChannelError,
        error,
      );
      squaredError += error * error;
      pixelDiffers ||= error !== 0;
    }
    differingPixels += pixelDiffers ? 1 : 0;
  }
  const pixels = reference.width * reference.height;
  return {
    key: referenceKey,
    width: reference.width,
    height: reference.height,
    maximumAbsoluteChannelError,
    differingPixels,
    differingPixelFraction: differingPixels / pixels,
    rootMeanSquareChannelError: Math.sqrt(squaredError / expectedBytes),
  };
}

function canonicalRunSummary(runs: readonly ZeroLossOracleRun[]): string {
  const normalized = runs.map((run) => ({
    browser: run.browser,
    comparisons: [...run.comparisons]
      .sort((left, right) => left.key.localeCompare(right.key))
      .map((comparison) => ({ ...comparison })),
    decoderErrors: run.decoderErrors,
    infrastructureStatus: run.infrastructureStatus,
    runId: run.runId,
    zeroLoss: run.zeroLoss,
  }));
  return `${JSON.stringify(normalized)}\n`;
}

export function freezeOracleTolerance(
  runs: readonly ZeroLossOracleRun[],
  requiredFrameKeys: readonly string[],
): FrozenOracleTolerance {
  if (runs.length !== 10) {
    throw new Error("oracle_freeze_requires_exactly_ten_runs");
  }
  if (new Set(runs.map((run) => run.runId)).size !== runs.length) {
    throw new Error("oracle_freeze_run_id_duplicate");
  }
  const expectedKeys = [...requiredFrameKeys].sort();
  if (
    expectedKeys.length === 0 ||
    new Set(expectedKeys).size !== expectedKeys.length
  ) {
    throw new Error("oracle_freeze_frame_keys_invalid");
  }
  const browserCounts = { chromium: 0, firefox: 0 };
  let maximumAbsoluteChannelError = 0;
  let maximumDifferingPixelFraction = 0;
  let maximumRootMeanSquareChannelError = 0;

  for (const run of runs) {
    if (
      !run.runId ||
      run.zeroLoss !== true ||
      run.infrastructureStatus !== "COMPLETE" ||
      run.decoderErrors !== 0
    ) {
      throw new Error("oracle_freeze_run_inadmissible");
    }
    browserCounts[run.browser] += 1;
    const actualKeys = run.comparisons
      .map((comparison) => comparison.key)
      .sort();
    if (
      actualKeys.length !== expectedKeys.length ||
      actualKeys.some((key, index) => key !== expectedKeys[index])
    ) {
      throw new Error("oracle_freeze_frame_set_incomplete");
    }
    for (const comparison of run.comparisons) {
      assertSafeNonnegativeInteger(comparison.width, "comparison_width");
      assertSafeNonnegativeInteger(comparison.height, "comparison_height");
      assertSafeNonnegativeInteger(
        comparison.maximumAbsoluteChannelError,
        "maximum_channel_error",
      );
      assertSafeNonnegativeInteger(
        comparison.differingPixels,
        "differing_pixels",
      );
      assertFiniteRange(
        comparison.differingPixelFraction,
        0,
        1,
        "differing_pixel_fraction",
      );
      assertFiniteRange(
        comparison.rootMeanSquareChannelError,
        0,
        255,
        "root_mean_square_channel_error",
      );
      if (
        comparison.width === 0 ||
        comparison.height === 0 ||
        comparison.maximumAbsoluteChannelError > 255 ||
        comparison.differingPixels > comparison.width * comparison.height
      ) {
        throw new Error("oracle_freeze_comparison_invalid");
      }
      maximumAbsoluteChannelError = Math.max(
        maximumAbsoluteChannelError,
        comparison.maximumAbsoluteChannelError,
      );
      maximumDifferingPixelFraction = Math.max(
        maximumDifferingPixelFraction,
        comparison.differingPixelFraction,
      );
      maximumRootMeanSquareChannelError = Math.max(
        maximumRootMeanSquareChannelError,
        comparison.rootMeanSquareChannelError,
      );
    }
  }
  if (browserCounts.chromium !== 5 || browserCounts.firefox !== 5) {
    throw new Error("oracle_freeze_requires_five_runs_per_browser");
  }

  const canonical = canonicalRunSummary(runs);
  return {
    schemaVersion: 1,
    protocol: "browser_oracle_v1",
    state: "FROZEN",
    requiredZeroLossRuns: 10,
    frameKey: "dependency_epoch:extended_rtp_timestamp",
    sourceRunIds: runs.map((run) => run.runId),
    sourceDigestSha256: createHash("sha256").update(canonical).digest("hex"),
    maximumAbsoluteChannelError,
    maximumDifferingPixelFraction,
    maximumRootMeanSquareChannelError,
  };
}

export function oracleComparisonPasses(
  comparison: OracleComparison,
  tolerance: FrozenOracleTolerance,
): boolean {
  return (
    comparison.maximumAbsoluteChannelError <=
      tolerance.maximumAbsoluteChannelError &&
    comparison.differingPixelFraction <=
      tolerance.maximumDifferingPixelFraction &&
    comparison.rootMeanSquareChannelError <=
      tolerance.maximumRootMeanSquareChannelError
  );
}

export function parseOracleFreezeInput(value: unknown): OracleFreezeInput {
  const root = record(value, "oracle_freeze_input");
  exactKeys(
    root,
    ["schemaVersion", "protocol", "requiredFrameKeys", "runs"],
    "oracle_freeze_input",
  );
  if (
    root.schemaVersion !== 1 ||
    root.protocol !== "browser_oracle_zero_loss_v1" ||
    !Array.isArray(root.requiredFrameKeys) ||
    !root.requiredFrameKeys.every((key) => typeof key === "string") ||
    !Array.isArray(root.runs)
  ) {
    throw new Error("oracle_freeze_input_invalid");
  }
  const runs = root.runs.map((rawRun) => {
    const run = record(rawRun, "oracle_freeze_run");
    exactKeys(
      run,
      [
        "runId",
        "browser",
        "zeroLoss",
        "infrastructureStatus",
        "decoderErrors",
        "comparisons",
      ],
      "oracle_freeze_run",
    );
    if (
      typeof run.runId !== "string" ||
      (run.browser !== "chromium" && run.browser !== "firefox") ||
      run.zeroLoss !== true ||
      run.infrastructureStatus !== "COMPLETE" ||
      run.decoderErrors !== 0 ||
      !Array.isArray(run.comparisons)
    ) {
      throw new Error("oracle_freeze_run_invalid");
    }
    const comparisons = run.comparisons.map((rawComparison) => {
      const comparison = record(rawComparison, "oracle_comparison");
      exactKeys(
        comparison,
        [
          "key",
          "width",
          "height",
          "maximumAbsoluteChannelError",
          "differingPixels",
          "differingPixelFraction",
          "rootMeanSquareChannelError",
        ],
        "oracle_comparison",
      );
      if (
        typeof comparison.key !== "string" ||
        typeof comparison.width !== "number" ||
        typeof comparison.height !== "number" ||
        typeof comparison.maximumAbsoluteChannelError !== "number" ||
        typeof comparison.differingPixels !== "number" ||
        typeof comparison.differingPixelFraction !== "number" ||
        typeof comparison.rootMeanSquareChannelError !== "number"
      ) {
        throw new Error("oracle_comparison_invalid");
      }
      return comparison as unknown as OracleComparison;
    });
    return {
      runId: run.runId,
      browser: run.browser,
      zeroLoss: true,
      infrastructureStatus: "COMPLETE",
      decoderErrors: 0,
      comparisons,
    } satisfies ZeroLossOracleRun;
  });
  return {
    schemaVersion: 1,
    protocol: "browser_oracle_zero_loss_v1",
    requiredFrameKeys: root.requiredFrameKeys as string[],
    runs,
  };
}
