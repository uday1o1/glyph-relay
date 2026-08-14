import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

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

interface ReceiverSnapshot {
  inboundVideo: readonly Record<string, number | null>[];
  oracle: readonly {
    expectedDisplayTime: number;
    height: number;
    presentationTime: number;
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
          snapshot(): Promise<ReceiverSnapshot>;
        };
      }
    ).__glyphrelayReceiver.snapshot(),
  );
}

async function run(): Promise<void> {
  const arguments_ = parseArguments(process.argv.slice(2));
  if (arguments_.framesPerSecond > 60) {
    throw new Error("frames_per_second_invalid");
  }
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
      receiver: snapshot,
      sender: senderSummary,
      streamSha256: await sha256File(arguments_.stream),
    };
    if (
      snapshot.presentedFrames < minimumPresentedFrames ||
      snapshot.videoWidth !== 1280 ||
      snapshot.videoHeight !== 720 ||
      snapshot.oracle.length === 0 ||
      snapshot.inboundVideo.length !== 1 ||
      snapshot.playbackQuality?.corruptedVideoFrames !== 0 ||
      !lossEvidencePassed ||
      !pliEvidencePassed
    ) {
      throw new Error("browser_playback_acceptance_failed");
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

await run();
