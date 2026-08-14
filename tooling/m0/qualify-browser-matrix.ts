import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import {
  freezeOracleTolerance,
  oracleComparisonPasses,
  parseOracleFreezeInput,
  type FrozenOracleTolerance,
  type OracleComparison,
  type OracleFreezeInput,
} from "./browser-oracle.ts";

type BrowserName = "chromium" | "firefox";
type Scenario = "ZERO_LOSS" | "PLI_RECOVERY" | "ROLLOVER_LOSS";

interface Arguments {
  answerer: string;
  fixture: string;
  output: string;
}

export interface BrowserMatrixRunDefinition {
  browser: BrowserName;
  faultLossExtendedSequence?: number;
  injectPliAfterFrame?: number;
  runId: string;
  scenario: Scenario;
  startFrame: number;
}

interface ChildResult {
  code: number | null;
  signal: NodeJS.Signals | null;
  stderr: string;
  stdout: string;
}

interface MatrixRunResult {
  browser: BrowserName;
  faultLossExtendedSequence: number | null;
  injectPliAfterFrame: number | null;
  oraclePassed: boolean;
  presentedFrames: number;
  reportPath: string;
  reportSha256: string;
  runId: string;
  scenario: Scenario;
}

const FRAME_COUNT = 1800;
const FPS = 30;
const MAXIMUM_CHILD_OUTPUT_BYTES = 256 * 1024;
const CHILD_TIMEOUT_MILLISECONDS = 150_000;
const PLAYBACK_SCRIPT = resolve(
  dirname(fileURLToPath(import.meta.url)),
  "qualify-browser-playback.ts",
);

function parseArguments(values: readonly string[]): Arguments {
  if (values[0] === "--") {
    values = values.slice(1);
  }
  const parsed = new Map<string, string>();
  for (let index = 0; index < values.length; index += 2) {
    const option = values[index];
    const value = values[index + 1];
    if (!option?.startsWith("--") || !value || parsed.has(option)) {
      throw new Error("browser_matrix_arguments_invalid");
    }
    parsed.set(option, value);
  }
  if (
    parsed.size !== 3 ||
    !parsed.has("--answerer") ||
    !parsed.has("--fixture") ||
    !parsed.has("--output")
  ) {
    throw new Error(
      "usage: node tooling/m0/qualify-browser-matrix.ts --answerer FILE --fixture DIR --output DIR",
    );
  }
  return {
    answerer: resolve(parsed.get("--answerer")!),
    fixture: resolve(parsed.get("--fixture")!),
    output: resolve(parsed.get("--output")!),
  };
}

export function browserMatrixRunDefinitions(): BrowserMatrixRunDefinition[] {
  const runs: BrowserMatrixRunDefinition[] = [];
  for (const browser of ["chromium", "firefox"] as const) {
    for (let repeat = 1; repeat <= 5; repeat += 1) {
      runs.push({
        browser,
        runId: `zero-loss-${browser}-${String(repeat).padStart(2, "0")}`,
        scenario: "ZERO_LOSS",
        startFrame: 300,
      });
    }
  }
  for (const browser of ["chromium", "firefox"] as const) {
    runs.push({
      browser,
      injectPliAfterFrame: 901,
      runId: `pli-recovery-${browser}`,
      scenario: "PLI_RECOVERY",
      startFrame: 240,
    });
    for (const sequence of [65_534, 65_535, 65_536]) {
      runs.push({
        browser,
        faultLossExtendedSequence: sequence,
        runId: `rollover-loss-${browser}-${sequence}`,
        scenario: "ROLLOVER_LOSS",
        startFrame: 300,
      });
    }
  }
  return runs;
}

function appendBounded(current: string, chunk: Buffer): string {
  const combined = current + chunk.toString("utf8");
  if (Buffer.byteLength(combined) > MAXIMUM_CHILD_OUTPUT_BYTES) {
    throw new Error("browser_matrix_child_output_limit_exceeded");
  }
  return combined;
}

async function launchPlayback(
  arguments_: Arguments,
  definition: BrowserMatrixRunDefinition,
  stream: string,
  frames: string,
  output: string,
): Promise<ChildResult> {
  const childArguments = [
    PLAYBACK_SCRIPT,
    "--answerer",
    arguments_.answerer,
    "--browser",
    definition.browser,
    "--stream",
    stream,
    "--frames",
    frames,
    "--start-frame",
    String(definition.startFrame),
    "--frame-count",
    String(FRAME_COUNT),
    "--fps",
    String(FPS),
    "--output",
    output,
  ];
  if (definition.injectPliAfterFrame !== undefined) {
    childArguments.push(
      "--inject-pli-after-frame",
      String(definition.injectPliAfterFrame),
    );
  }
  if (definition.faultLossExtendedSequence !== undefined) {
    childArguments.push(
      "--fault-loss-extended-sequence",
      String(definition.faultLossExtendedSequence),
    );
  }
  const child = spawn(process.execPath, childArguments, {
    env: {
      PATH: process.env.PATH ?? "",
      ...(process.env.PLAYWRIGHT_BROWSERS_PATH
        ? { PLAYWRIGHT_BROWSERS_PATH: process.env.PLAYWRIGHT_BROWSERS_PATH }
        : {}),
    },
    shell: false,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  let outputFailure: Error | undefined;
  child.stdout.on("data", (chunk: Buffer) => {
    try {
      stdout = appendBounded(stdout, chunk);
    } catch (error) {
      outputFailure = error as Error;
      child.kill("SIGTERM");
    }
  });
  child.stderr.on("data", (chunk: Buffer) => {
    try {
      stderr = appendBounded(stderr, chunk);
    } catch (error) {
      outputFailure = error as Error;
      child.kill("SIGTERM");
    }
  });
  return new Promise<ChildResult>((resolveChild, rejectChild) => {
    const timeout = setTimeout(
      () => child.kill("SIGTERM"),
      CHILD_TIMEOUT_MILLISECONDS,
    );
    child.once("error", (error) => {
      clearTimeout(timeout);
      rejectChild(error);
    });
    child.once("exit", (code, signal) => {
      clearTimeout(timeout);
      if (outputFailure) {
        rejectChild(outputFailure);
      } else if (code === null && signal === "SIGTERM") {
        rejectChild(new Error("browser_matrix_child_timeout"));
      } else {
        resolveChild({ code, signal, stderr, stdout });
      }
    });
  });
}

function object(value: unknown, name: string): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${name}_invalid`);
  }
  return value as Record<string, unknown>;
}

function comparisons(report: Record<string, unknown>): OracleComparison[] {
  if (
    !Array.isArray(report.oracleComparisons) ||
    report.oracleComparisons.length !== 4
  ) {
    throw new Error("browser_matrix_oracle_comparisons_invalid");
  }
  return report.oracleComparisons.map((value) => {
    const comparison = object(value, "browser_matrix_oracle_comparison");
    if (
      typeof comparison.key !== "string" ||
      typeof comparison.width !== "number" ||
      typeof comparison.height !== "number" ||
      typeof comparison.maximumAbsoluteChannelError !== "number" ||
      typeof comparison.differingPixels !== "number" ||
      typeof comparison.differingPixelFraction !== "number" ||
      typeof comparison.rootMeanSquareChannelError !== "number"
    ) {
      throw new Error("browser_matrix_oracle_comparison_invalid");
    }
    return comparison as unknown as OracleComparison;
  });
}

function decoderErrors(report: Record<string, unknown>): number {
  const receiver = object(report.receiver, "browser_matrix_receiver");
  const playback = object(
    receiver.playbackQuality,
    "browser_matrix_playback_quality",
  );
  const corrupted = playback.corruptedVideoFrames;
  const diagnostics = report.diagnostics;
  if (
    !Number.isSafeInteger(corrupted) ||
    (corrupted as number) < 0 ||
    !Array.isArray(diagnostics) ||
    !diagnostics.every((value) => typeof value === "string")
  ) {
    throw new Error("browser_matrix_decoder_diagnostics_invalid");
  }
  return (
    (corrupted as number) +
    diagnostics.filter((value) => (value as string).startsWith("pageerror:"))
      .length
  );
}

async function sha256File(path: string): Promise<string> {
  return createHash("sha256")
    .update(await readFile(path))
    .digest("hex");
}

async function runOne(
  arguments_: Arguments,
  definition: BrowserMatrixRunDefinition,
  stream: string,
  frames: string,
): Promise<{ report: Record<string, unknown>; result: MatrixRunResult }> {
  const output = resolve(arguments_.output, "runs", definition.runId);
  const child = await launchPlayback(
    arguments_,
    definition,
    stream,
    frames,
    output,
  );
  if (child.code !== 0 || child.signal !== null) {
    throw new Error(
      `browser_matrix_run_failed:${definition.runId}:${child.code}:${child.signal}:${child.stderr.slice(0, 2048)}`,
    );
  }
  const reportPath = resolve(output, "browser-playback-summary.json");
  const report = object(
    JSON.parse(await readFile(reportPath, "utf8")),
    "browser_matrix_playback_report",
  );
  const receiver = object(report.receiver, "browser_matrix_receiver");
  if (
    report.status !== "PASSED" ||
    report.browser !== definition.browser ||
    decoderErrors(report) !== 0 ||
    !Number.isSafeInteger(receiver.presentedFrames)
  ) {
    throw new Error(`browser_matrix_report_invalid:${definition.runId}`);
  }
  return {
    report,
    result: {
      browser: definition.browser,
      faultLossExtendedSequence: definition.faultLossExtendedSequence ?? null,
      injectPliAfterFrame: definition.injectPliAfterFrame ?? null,
      oraclePassed: false,
      presentedFrames: receiver.presentedFrames as number,
      reportPath: relative(arguments_.output, reportPath),
      reportSha256: await sha256File(reportPath),
      runId: definition.runId,
      scenario: definition.scenario,
    },
  };
}

async function writeJsonExclusive(path: string, value: unknown): Promise<void> {
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`, {
    encoding: "utf8",
    flag: "wx",
    mode: 0o600,
  });
}

export async function runBrowserMatrix(arguments_: Arguments): Promise<void> {
  await stat(arguments_.answerer);
  const fixtureSummaryPath = resolve(
    arguments_.fixture,
    "browser-fixture-summary.json",
  );
  const stream = resolve(arguments_.fixture, "nvenc-browser-720p30.h264");
  const frames = resolve(arguments_.fixture, "nvenc-browser-720p30-frames.tsv");
  const fixtureSummary = object(
    JSON.parse(await readFile(fixtureSummaryPath, "utf8")),
    "browser_matrix_fixture_summary",
  );
  await stat(stream);
  await stat(frames);
  if (
    fixtureSummary.status !== "PASSED" ||
    fixtureSummary.stream_sha256 !== (await sha256File(stream))
  ) {
    throw new Error("browser_matrix_fixture_invalid");
  }
  await mkdir(dirname(arguments_.output), { recursive: true });
  await mkdir(arguments_.output, { mode: 0o700 });
  await mkdir(resolve(arguments_.output, "runs"), { mode: 0o700 });

  const definitions = browserMatrixRunDefinitions();
  const zeroDefinitions = definitions.filter(
    (run) => run.scenario === "ZERO_LOSS",
  );
  const recoveryDefinitions = definitions.filter(
    (run) => run.scenario !== "ZERO_LOSS",
  );
  const reports = new Map<string, Record<string, unknown>>();
  const results: MatrixRunResult[] = [];
  for (const definition of zeroDefinitions) {
    const completed = await runOne(arguments_, definition, stream, frames);
    reports.set(definition.runId, completed.report);
    results.push(completed.result);
  }

  const firstComparisons = comparisons(reports.get(zeroDefinitions[0]!.runId)!);
  const requiredFrameKeys = firstComparisons
    .map((comparison) => comparison.key)
    .sort();
  const freezeInput: OracleFreezeInput = {
    schemaVersion: 1,
    protocol: "browser_oracle_zero_loss_v1",
    requiredFrameKeys,
    runs: zeroDefinitions.map((definition) => ({
      browser: definition.browser,
      comparisons: comparisons(reports.get(definition.runId)!),
      decoderErrors: 0,
      infrastructureStatus: "COMPLETE",
      runId: definition.runId,
      zeroLoss: true,
    })),
  };
  const parsedFreezeInput = parseOracleFreezeInput(freezeInput);
  const tolerance: FrozenOracleTolerance = freezeOracleTolerance(
    parsedFreezeInput.runs,
    parsedFreezeInput.requiredFrameKeys,
  );
  await writeJsonExclusive(
    resolve(arguments_.output, "browser-oracle-zero-loss.json"),
    freezeInput,
  );
  await writeJsonExclusive(
    resolve(arguments_.output, "browser-oracle-frozen.json"),
    tolerance,
  );
  for (const result of results) {
    result.oraclePassed = comparisons(reports.get(result.runId)!).every(
      (comparison) => oracleComparisonPasses(comparison, tolerance),
    );
  }

  for (const definition of recoveryDefinitions) {
    const completed = await runOne(arguments_, definition, stream, frames);
    completed.result.oraclePassed = comparisons(completed.report).every(
      (comparison) => oracleComparisonPasses(comparison, tolerance),
    );
    if (!completed.result.oraclePassed) {
      throw new Error(
        `browser_matrix_oracle_tolerance_failed:${definition.runId}`,
      );
    }
    reports.set(definition.runId, completed.report);
    results.push(completed.result);
  }
  if (!results.every((result) => result.oraclePassed)) {
    throw new Error("browser_matrix_zero_loss_oracle_tolerance_failed");
  }

  const summary = {
    schemaVersion: 1,
    protocol: "glyphrelay-m0-browser-matrix-v1",
    status: "PASSED",
    fixtureStreamSha256: fixtureSummary.stream_sha256,
    oracleTolerance: tolerance,
    runs: results,
  };
  await writeJsonExclusive(
    resolve(arguments_.output, "browser-matrix-summary.json"),
    summary,
  );
  process.stdout.write(
    `${JSON.stringify({ runs: results.length, status: "PASSED", tolerance: tolerance.sourceDigestSha256 })}\n`,
  );
}

const invokedPath = process.argv[1]
  ? pathToFileURL(resolve(process.argv[1])).href
  : undefined;
if (invokedPath === import.meta.url) {
  await runBrowserMatrix(parseArguments(process.argv.slice(2)));
}
