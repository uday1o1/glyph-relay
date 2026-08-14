import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import {
  chromium,
  firefox,
  type BrowserType,
  type Page,
} from "@playwright/test";

import {
  m0LoopbackProtocolVersion,
  m0LoopbackSessionId,
  startM0LoopbackServer,
} from "../../signaling/m0-loopback-server.ts";
import {
  compareOracleFrames,
  type OracleComparison,
} from "./browser-oracle.ts";

type BrowserName = "chromium" | "firefox";

interface Arguments {
  answerer: string;
  browser: BrowserName;
  faultLossExtendedSequence?: string;
  frameCount: number;
  frames: string;
  framesPerSecond: number;
  injectPliAfterFrame?: number;
  output: string;
  startFrame: number;
  stream: string;
}

interface ReceiverState {
  error: string | null;
  state: string;
}

interface ReceiverControl extends ReceiverState {
  setOracleTargets(values: readonly number[]): void;
}

interface ReceiverSnapshot {
  inboundVideo: readonly Record<string, number | null>[];
  oracle: readonly {
    expectedDisplayTime: number;
    height: number;
    presentationTime: number;
    rgbaBase64?: string;
    rgbaSha256: string;
    rtpTimestamp: number | null;
    width: number;
  }[];
  playbackQuality: {
    corruptedVideoFrames: number;
    droppedVideoFrames: number;
    totalVideoFrames: number;
  } | null;
  presentedFrames: number;
  state: string;
  videoHeight: number;
  videoWidth: number;
}

interface ChildResult {
  code: number | null;
  signal: NodeJS.Signals | null;
  stderr: string;
  stdout: string;
}

const MAXIMUM_CHILD_OUTPUT_BYTES = 256 * 1024;
const RGBA_FRAME_BYTES = 1280 * 720 * 4;
const RTP_MODULUS = 2 ** 32;
const INITIAL_EXTENDED_RTP_TIMESTAMP = RTP_MODULUS - 3000;

function parsePositiveInteger(value: string, name: string): number {
  if (!/^[1-9]\d*$/.test(value)) {
    throw new Error(`${name}_invalid`);
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed)) {
    throw new Error(`${name}_invalid`);
  }
  return parsed;
}

function parseNonnegativeInteger(value: string, name: string): number {
  if (!/^(0|[1-9]\d*)$/.test(value)) {
    throw new Error(`${name}_invalid`);
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed)) {
    throw new Error(`${name}_invalid`);
  }
  return parsed;
}

function parseArguments(values: readonly string[]): Arguments {
  if (values[0] === "--") {
    values = values.slice(1);
  }
  const parsed = new Map<string, string>();
  for (let index = 0; index < values.length; index += 2) {
    const option = values[index];
    const value = values[index + 1];
    if (!option?.startsWith("--") || !value || parsed.has(option)) {
      throw new Error("browser_playback_arguments_invalid");
    }
    parsed.set(option, value);
  }
  const required = [
    "--answerer",
    "--browser",
    "--frames",
    "--output",
    "--stream",
  ] as const;
  if (required.some((name) => !parsed.has(name))) {
    throw new Error("browser_playback_required_argument_missing");
  }
  const browser = parsed.get("--browser");
  if (browser !== "chromium" && browser !== "firefox") {
    throw new Error("browser_playback_browser_invalid");
  }
  const fault = parsed.get("--fault-loss-extended-sequence");
  if (fault !== undefined && !/^(0|[1-9]\d*)$/.test(fault)) {
    throw new Error("fault_loss_extended_sequence_invalid");
  }
  return {
    answerer: resolve(parsed.get("--answerer")!),
    browser,
    faultLossExtendedSequence: fault,
    frameCount: parsePositiveInteger(
      parsed.get("--frame-count") ?? "1800",
      "frame_count",
    ),
    frames: resolve(parsed.get("--frames")!),
    framesPerSecond: parsePositiveInteger(
      parsed.get("--fps") ?? "30",
      "frames_per_second",
    ),
    injectPliAfterFrame: parsed.has("--inject-pli-after-frame")
      ? parsePositiveInteger(
          parsed.get("--inject-pli-after-frame")!,
          "inject_pli_after_frame",
        )
      : undefined,
    output: resolve(parsed.get("--output")!),
    startFrame: parseNonnegativeInteger(
      parsed.get("--start-frame") ?? "300",
      "start_frame",
    ),
    stream: resolve(parsed.get("--stream")!),
  };
}

async function sha256File(path: string): Promise<string> {
  return createHash("sha256")
    .update(await readFile(path))
    .digest("hex");
}

export function oracleFrameOrdinals(frameCount: number): number[] {
  if (!Number.isSafeInteger(frameCount) || frameCount < 10) {
    throw new Error("oracle_frame_count_invalid");
  }
  const ordinals = [
    Math.floor((frameCount * 2) / 5),
    Math.floor(frameCount / 2),
    Math.floor((frameCount * 3) / 5),
    Math.floor((frameCount * 7) / 10),
  ];
  if (
    new Set(ordinals).size !== 4 ||
    ordinals.some((value) => value >= frameCount)
  ) {
    throw new Error("oracle_frame_ordinals_invalid");
  }
  return ordinals;
}

export function oracleRtpTimestamp(frameOrdinal: number): number {
  if (!Number.isSafeInteger(frameOrdinal) || frameOrdinal < 0) {
    throw new Error("oracle_frame_ordinal_invalid");
  }
  return (INITIAL_EXTENDED_RTP_TIMESTAMP + frameOrdinal * 3000) % RTP_MODULUS;
}

async function waitForFile(
  path: string,
  timeoutMilliseconds: number,
): Promise<void> {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    try {
      if ((await stat(path)).size > 0) {
        return;
      }
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") {
        throw error;
      }
    }
    await new Promise((resolveDelay) => setTimeout(resolveDelay, 50));
  }
  throw new Error("sender_answer_timeout");
}

function appendBounded(current: string, chunk: Buffer): string {
  const combined = current + chunk.toString("utf8");
  if (Buffer.byteLength(combined) > MAXIMUM_CHILD_OUTPUT_BYTES) {
    throw new Error("sender_output_limit_exceeded");
  }
  return combined;
}

function launchSender(
  arguments_: Arguments,
  offer: string,
  answer: string,
  summary: string,
) {
  const childArguments = [
    "--offer",
    offer,
    "--stream",
    arguments_.stream,
    "--frames",
    arguments_.frames,
    "--answer",
    answer,
    "--summary",
    summary,
    "--start-frame",
    String(arguments_.startFrame),
    "--frame-count",
    String(arguments_.frameCount),
    "--fps",
    String(arguments_.framesPerSecond),
  ];
  if (arguments_.faultLossExtendedSequence !== undefined) {
    childArguments.push(
      "--fault-loss-extended-sequence",
      arguments_.faultLossExtendedSequence,
    );
  }
  if (arguments_.injectPliAfterFrame !== undefined) {
    childArguments.push(
      "--inject-pli-after-frame",
      String(arguments_.injectPliAfterFrame),
    );
  }
  const child = spawn(arguments_.answerer, childArguments, {
    env: { PATH: process.env.PATH ?? "" },
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
  const completion = new Promise<ChildResult>((resolveChild, rejectChild) => {
    child.once("error", rejectChild);
    child.once("exit", (code, signal) => {
      if (outputFailure) {
        rejectChild(outputFailure);
      } else {
        resolveChild({ code, signal, stderr, stdout });
      }
    });
  });
  return { child, completion };
}

async function receiverState(page: Page): Promise<ReceiverState> {
  return page.evaluate(
    () =>
      (
        window as unknown as {
          __glyphrelayReceiver: ReceiverState;
        }
      ).__glyphrelayReceiver,
  );
}

async function receiverSnapshot(page: Page): Promise<ReceiverSnapshot> {
  return page.evaluate(() =>
    (
      window as unknown as {
        __glyphrelayReceiver: {
          snapshot(includeRgba: boolean): Promise<ReceiverSnapshot>;
        };
      }
    ).__glyphrelayReceiver.snapshot(true),
  );
}

interface SentFrameTrace {
  dependency_epoch: number;
  extended_timestamp: number;
  frame_index: number;
}

function parseSentFrameTrace(
  sender: Record<string, unknown>,
): SentFrameTrace[] {
  const rawTrace = sender.sent_frame_trace;
  if (
    !Array.isArray(rawTrace) ||
    rawTrace.length === 0 ||
    rawTrace.length > 1800
  ) {
    throw new Error("sender_frame_trace_invalid");
  }
  let previousExtendedTimestamp: number | undefined;
  return rawTrace.map((raw) => {
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) {
      throw new Error("sender_frame_trace_invalid");
    }
    const frame = raw as Record<string, unknown>;
    const frameIndex = frame.frame_index;
    const dependencyEpoch = frame.dependency_epoch;
    const extendedTimestamp = frame.extended_timestamp;
    if (
      !Number.isSafeInteger(frameIndex) ||
      !Number.isSafeInteger(dependencyEpoch) ||
      !Number.isSafeInteger(extendedTimestamp) ||
      (frameIndex as number) < 0 ||
      (dependencyEpoch as number) < 1 ||
      (extendedTimestamp as number) < 0 ||
      (previousExtendedTimestamp !== undefined &&
        (extendedTimestamp as number) !== previousExtendedTimestamp + 3000)
    ) {
      throw new Error("sender_frame_trace_invalid");
    }
    previousExtendedTimestamp = extendedTimestamp as number;
    return {
      dependency_epoch: dependencyEpoch as number,
      extended_timestamp: extendedTimestamp as number,
      frame_index: frameIndex as number,
    };
  });
}

async function decodeReferenceFrames(
  stream: string,
  frameIndices: readonly number[],
): Promise<Map<number, Uint8Array>> {
  const unique = [...new Set(frameIndices)].sort((left, right) => left - right);
  if (
    unique.length === 0 ||
    unique.length > 4 ||
    unique.some(
      (index) => !Number.isSafeInteger(index) || index < 0 || index >= 10_000,
    )
  ) {
    throw new Error(
      `oracle_reference_frame_selection_invalid:${unique.join(",")}`,
    );
  }
  const filter = `select=${unique.map((index) => `eq(n\\,${index})`).join("+")}`;
  const child = spawn(
    "ffmpeg",
    [
      "-v",
      "error",
      "-f",
      "h264",
      "-i",
      stream,
      "-an",
      "-sn",
      "-dn",
      "-vf",
      filter,
      "-fps_mode",
      "passthrough",
      "-f",
      "rawvideo",
      "-pix_fmt",
      "rgba",
      "pipe:1",
    ],
    {
      env: { PATH: process.env.PATH ?? "" },
      shell: false,
      stdio: ["ignore", "pipe", "pipe"],
    },
  );
  const output: Buffer[] = [];
  let outputBytes = 0;
  let stderr = "";
  let outputFailure: Error | undefined;
  child.stdout.on("data", (chunk: Buffer) => {
    outputBytes += chunk.length;
    if (outputBytes > unique.length * RGBA_FRAME_BYTES) {
      outputFailure = new Error("oracle_reference_output_limit_exceeded");
      child.kill("SIGTERM");
      return;
    }
    output.push(chunk);
  });
  child.stderr.on("data", (chunk: Buffer) => {
    try {
      stderr = appendBounded(stderr, chunk);
    } catch (error) {
      outputFailure = error as Error;
      child.kill("SIGTERM");
    }
  });
  const result = await new Promise<ChildResult>((resolveChild, rejectChild) => {
    child.once("error", rejectChild);
    child.once("exit", (code, signal) => {
      if (outputFailure) {
        rejectChild(outputFailure);
      } else {
        resolveChild({ code, signal, stderr, stdout: "" });
      }
    });
  });
  if (result.code !== 0 || result.signal !== null) {
    throw new Error(
      `oracle_reference_decode_failed:${result.code}:${result.signal}:${stderr}`,
    );
  }
  const decoded = Buffer.concat(output);
  if (decoded.length !== unique.length * RGBA_FRAME_BYTES) {
    throw new Error("oracle_reference_frame_count_invalid");
  }
  const frames = new Map<number, Uint8Array>();
  for (const [position, frameIndex] of unique.entries()) {
    const start = position * RGBA_FRAME_BYTES;
    frames.set(
      frameIndex,
      new Uint8Array(decoded.subarray(start, start + RGBA_FRAME_BYTES)),
    );
  }
  return frames;
}

async function comparePresentedFrames(
  stream: string,
  snapshot: ReceiverSnapshot,
  sender: Record<string, unknown>,
): Promise<OracleComparison[]> {
  const trace = parseSentFrameTrace(sender);
  const retained = snapshot.oracle.map((frame) => {
    if (frame.rtpTimestamp === null || frame.rgbaBase64 === undefined) {
      throw new Error("oracle_presented_frame_identity_missing");
    }
    const candidates = trace.filter(
      (item) => item.extended_timestamp % RTP_MODULUS === frame.rtpTimestamp,
    );
    if (candidates.length !== 1) {
      throw new Error("oracle_presented_frame_trace_ambiguous");
    }
    const rgba = new Uint8Array(Buffer.from(frame.rgbaBase64, "base64"));
    if (
      rgba.length !== RGBA_FRAME_BYTES ||
      createHash("sha256").update(rgba).digest("hex") !== frame.rgbaSha256
    ) {
      throw new Error("oracle_presented_frame_payload_invalid");
    }
    return { frame, rgba, trace: candidates[0]! };
  });
  const references = await decodeReferenceFrames(
    stream,
    retained.map((item) => item.trace.frame_index),
  );
  return retained.map((item) => {
    const reference = references.get(item.trace.frame_index);
    if (!reference) {
      throw new Error("oracle_reference_frame_missing");
    }
    const identity = {
      dependencyEpoch: item.trace.dependency_epoch,
      extendedRtpTimestamp: String(item.trace.extended_timestamp),
      height: item.frame.height,
      width: item.frame.width,
    };
    return compareOracleFrames(
      { ...identity, rgba: reference },
      { ...identity, rgba: item.rgba },
    );
  });
}

export async function runBrowserPlayback(): Promise<void> {
  const arguments_ = parseArguments(process.argv.slice(2));
  if (arguments_.framesPerSecond > 60) {
    throw new Error("frames_per_second_invalid");
  }
  const oracleOrdinals = oracleFrameOrdinals(arguments_.frameCount);
  const oracleTargets = oracleOrdinals.map(oracleRtpTimestamp);
  await stat(arguments_.answerer);
  await stat(arguments_.stream);
  await stat(arguments_.frames);
  await mkdir(dirname(arguments_.output), { recursive: true });
  await mkdir(arguments_.output, { mode: 0o700 });

  const server = await startM0LoopbackServer();
  const browserType: BrowserType =
    arguments_.browser === "chromium" ? chromium : firefox;
  const browser = await browserType.launch({ headless: true });
  const diagnostics: string[] = [];
  let sender: ReturnType<typeof launchSender> | undefined;
  try {
    const repositoryRoot = resolve(
      dirname(fileURLToPath(import.meta.url)),
      "../..",
    );
    const lock = JSON.parse(
      await readFile(resolve(repositoryRoot, "dependencies.lock.json"), "utf8"),
    ) as {
      playwright: Record<BrowserName, { version: string }>;
    };
    if (browser.version() !== lock.playwright[arguments_.browser].version) {
      throw new Error("browser_playback_version_mismatch");
    }
    const page = await browser.newPage();
    page.on("console", (message) => {
      if (diagnostics.length < 100) {
        diagnostics.push(
          `console:${message.type()}:${message.text().slice(0, 1024)}`,
        );
      }
    });
    page.on("pageerror", (error) => {
      if (diagnostics.length < 100) {
        diagnostics.push(`pageerror:${error.message.slice(0, 1024)}`);
      }
    });
    await page.goto(server.origin);
    await page.evaluate((targets) => {
      const receiver = (
        window as unknown as {
          __glyphrelayReceiver: ReceiverControl;
        }
      ).__glyphrelayReceiver;
      receiver.setOracleTargets(targets);
    }, oracleTargets);
    await page.locator("#connect").click();
    await page.waitForFunction(
      () => {
        const receiver = (
          window as unknown as {
            __glyphrelayReceiver?: ReceiverState;
          }
        ).__glyphrelayReceiver;
        return (
          receiver?.state === "FAILED" || receiver?.state === "WAITING_ANSWER"
        );
      },
      undefined,
      { timeout: 15_000 },
    );
    const initialState = await receiverState(page);
    if (initialState.state !== "WAITING_ANSWER") {
      throw new Error(
        `receiver_offer_failed:${initialState.error ?? "unknown"}`,
      );
    }
    const offer = server.snapshot().offer;
    assert.ok(offer, "receiver_offer_missing");
    const offerPath = resolve(arguments_.output, "offer.sdp");
    const answerPath = resolve(arguments_.output, "answer.sdp");
    const senderSummaryPath = resolve(arguments_.output, "sender-summary.json");
    await writeFile(offerPath, offer.sdp, {
      encoding: "utf8",
      flag: "wx",
      mode: 0o600,
    });
    sender = launchSender(arguments_, offerPath, answerPath, senderSummaryPath);
    await waitForFile(answerPath, 15_000);
    const answerSdp = await readFile(answerPath, "utf8");
    const answerResponse = await fetch(`${server.origin}/api/m0/answer`, {
      body: JSON.stringify({
        protocolVersion: m0LoopbackProtocolVersion,
        sdp: answerSdp,
        sessionId: m0LoopbackSessionId,
        type: "answer",
      }),
      headers: {
        "Content-Type": "application/json",
        Origin: server.origin,
      },
      method: "POST",
    });
    assert.equal(answerResponse.status, 201);
    await page.waitForFunction(
      () =>
        (
          window as unknown as {
            __glyphrelayReceiver: ReceiverState;
          }
        ).__glyphrelayReceiver.state === "RUNNING",
      undefined,
      { timeout: 30_000 },
    );
    const durationSeconds = arguments_.frameCount / arguments_.framesPerSecond;
    const steadyDurationSeconds =
      arguments_.faultLossExtendedSequence === undefined
        ? durationSeconds
        : Math.max(0, durationSeconds - 2);
    const minimumPresentedFrames = Math.max(
      1,
      Math.floor(steadyDurationSeconds * 24),
    );
    await page.waitForFunction(
      (minimum) =>
        Number(document.querySelector("#frames")?.textContent ?? "0") >=
        minimum,
      minimumPresentedFrames,
      { timeout: Math.ceil((durationSeconds + 30) * 1_000) },
    );
    await page.waitForFunction(
      () =>
        (
          window as unknown as {
            __glyphrelayReceiver: { oracleFrames: unknown[] };
          }
        ).__glyphrelayReceiver.oracleFrames.length === 4,
      undefined,
      { timeout: Math.ceil((durationSeconds + 30) * 1_000) },
    );
    const snapshot = await receiverSnapshot(page);
    const timeout = setTimeout(
      () => sender?.child.kill("SIGTERM"),
      Math.ceil((durationSeconds + 10) * 1_000),
    );
    const senderResult = await sender.completion.finally(() =>
      clearTimeout(timeout),
    );
    if (senderResult.code !== 0 || senderResult.signal !== null) {
      throw new Error(
        `sender_failed:${senderResult.code}:${senderResult.signal}:${senderResult.stderr.slice(0, 2048)}`,
      );
    }
    const senderSummary = JSON.parse(
      await readFile(senderSummaryPath, "utf8"),
    ) as Record<string, unknown>;
    const oracleComparisons = await comparePresentedFrames(
      arguments_.stream,
      snapshot,
      senderSummary,
    );
    const publicSnapshot = {
      ...snapshot,
      oracle: snapshot.oracle.map(
        ({ rgbaBase64: _rgbaBase64, ...frame }) => frame,
      ),
    };
    const lossEvidencePassed =
      arguments_.faultLossExtendedSequence === undefined ||
      (senderSummary.fault_datagram_suppressed === true &&
        senderSummary.protected_retransmission_observed === true &&
        senderSummary.protected_retransmission_identical === true &&
        typeof senderSummary.cache_retransmissions === "number" &&
        senderSummary.cache_retransmissions >= 1);
    const pliEvidencePassed =
      arguments_.injectPliAfterFrame === undefined ||
      (typeof senderSummary.recovery_frames === "number" &&
        senderSummary.recovery_frames >= 1 &&
        typeof senderSummary.idr_requests === "number" &&
        senderSummary.idr_requests >= 2 &&
        snapshot.inboundVideo.some(
          (inbound) =>
            typeof inbound.keyFramesDecoded === "number" &&
            inbound.keyFramesDecoded >= 2,
        ));
    const report = {
      schemaVersion: 1,
      protocol: "glyphrelay-m0-browser-playback-v1",
      status: "PASSED",
      browser: arguments_.browser,
      browserVersion: browser.version(),
      browserExecutableSha256: await sha256File(browserType.executablePath()),
      diagnostics,
      minimumPresentedFrames,
      oracleFrameOrdinals: oracleOrdinals,
      oracleComparisons,
      receiver: publicSnapshot,
      sender: senderSummary,
      streamSha256: await sha256File(arguments_.stream),
    };
    const acceptanceFailures: string[] = [];
    if (snapshot.presentedFrames < minimumPresentedFrames) {
      acceptanceFailures.push("presented_frame_gate_failed");
    }
    if (snapshot.videoWidth !== 1280 || snapshot.videoHeight !== 720) {
      acceptanceFailures.push("presented_geometry_invalid");
    }
    if (snapshot.oracle.length !== 4) {
      acceptanceFailures.push(
        `oracle_frame_count_${snapshot.oracle.length}_targets_${oracleTargets.join("-")}_actual_${snapshot.oracle.map((frame) => frame.rtpTimestamp).join("-")}`,
      );
    }
    if (oracleComparisons.length !== snapshot.oracle.length) {
      acceptanceFailures.push("oracle_comparison_count_mismatch");
    }
    if (snapshot.inboundVideo.length !== 1) {
      acceptanceFailures.push("inbound_video_report_count_invalid");
    }
    if (snapshot.playbackQuality?.corruptedVideoFrames !== 0) {
      acceptanceFailures.push("corrupted_browser_frame");
    }
    if (!lossEvidencePassed) {
      acceptanceFailures.push("loss_recovery_evidence_failed");
    }
    if (!pliEvidencePassed) {
      acceptanceFailures.push("pli_recovery_evidence_failed");
    }
    if (acceptanceFailures.length > 0) {
      throw new Error(
        `browser_playback_acceptance_failed:${acceptanceFailures.join(",")}`,
      );
    }
    await writeFile(
      resolve(arguments_.output, "browser-playback-summary.json"),
      `${JSON.stringify(report, null, 2)}\n`,
      { encoding: "utf8", flag: "wx", mode: 0o600 },
    );
    process.stdout.write(
      `${JSON.stringify({ browser: arguments_.browser, status: "PASSED", presentedFrames: snapshot.presentedFrames })}\n`,
    );
  } finally {
    if (sender) {
      sender.child.kill("SIGTERM");
    }
    await browser.close();
    await server.close();
  }
}

const invokedPath = process.argv[1]
  ? pathToFileURL(resolve(process.argv[1])).href
  : undefined;
if (invokedPath === import.meta.url) {
  await runBrowserPlayback();
}
