import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { mkdir, open } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { chromium, firefox, type BrowserType } from "@playwright/test";

type BrowserName = "chromium" | "firefox";

interface Arguments {
  browser: BrowserName;
  durationMilliseconds: number;
  joinUrl: string;
  output: string;
}

interface FrameObservation {
  callbackTimeMs: number | null;
  captureTimeMs: number | null;
  expectedDisplayTimeMs: number | null;
  presentationTimeMs: number | null;
  receiveTimeMs: number | null;
  rtpTimestamp: number | null;
}

interface ReceiverState {
  drainFrameObservations(): {
    droppedFrameObservations: number;
    observations: FrameObservation[];
  };
  error: string | null;
  snapshot(): Record<string, unknown>;
  state: string;
  transportSnapshot(): Promise<Record<string, unknown> | null>;
}

const MAXIMUM_DIAGNOSTICS = 128;
const MAXIMUM_DIAGNOSTIC_CHARACTERS = 2_048;

function parsePositiveInteger(value: string | undefined, name: string): number {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < 1 || parsed > 300_000) {
    throw new Error(`${name}_invalid`);
  }
  return parsed;
}

export function parseArguments(values: readonly string[]): Arguments {
  if (values[0] === "--") {
    values = values.slice(1);
  }
  const options = new Map<string, string>();
  for (let index = 0; index < values.length; index += 2) {
    const option = values[index];
    const value = values[index + 1];
    if (!option?.startsWith("--") || !value || options.has(option)) {
      throw new Error("controller_receiver_arguments_invalid");
    }
    options.set(option, value);
  }
  if (
    options.size !== 4 ||
    !options.has("--browser") ||
    !options.has("--duration-ms") ||
    !options.has("--join-url") ||
    !options.has("--output")
  ) {
    throw new Error("controller_receiver_arguments_invalid");
  }
  const browser = options.get("--browser");
  if (browser !== "chromium" && browser !== "firefox") {
    throw new Error("controller_receiver_browser_invalid");
  }
  const joinUrl = new URL(options.get("--join-url")!);
  if (
    joinUrl.origin !== "http://127.0.0.1:8443" ||
    joinUrl.pathname !== "/" ||
    joinUrl.search ||
    !/^#join=[A-Za-z0-9_-]{22}\.[A-Za-z0-9_-]{22,128}$/.test(joinUrl.hash)
  ) {
    throw new Error("controller_receiver_join_url_invalid");
  }
  return {
    browser,
    durationMilliseconds: parsePositiveInteger(
      options.get("--duration-ms"),
      "controller_receiver_duration",
    ),
    joinUrl: joinUrl.href,
    output: resolve(options.get("--output")!),
  };
}

async function sha256File(path: string): Promise<string> {
  const digest = createHash("sha256");
  for await (const chunk of createReadStream(path)) {
    digest.update(chunk);
  }
  return digest.digest("hex");
}

function boundedDiagnostic(value: string): string {
  return value.slice(0, MAXIMUM_DIAGNOSTIC_CHARACTERS);
}

async function durableExclusiveJson(
  path: string,
  value: unknown,
): Promise<void> {
  await mkdir(dirname(path), { recursive: true });
  const file = await open(path, "wx", 0o600);
  try {
    await file.writeFile(`${JSON.stringify(value, null, 2)}\n`, "utf8");
    await file.sync();
  } finally {
    await file.close();
  }
}

export async function qualifyLiveReceiver(
  arguments_: Arguments,
): Promise<void> {
  const browserType: BrowserType =
    arguments_.browser === "chromium" ? chromium : firefox;
  const executablePath = browserType.executablePath();
  const executableSha256 = await sha256File(executablePath);
  const browser = await browserType.launch({ headless: true });
  const diagnostics: string[] = [];
  const observations: FrameObservation[] = [];
  let droppedFrameObservations = 0;
  try {
    const context = await browser.newContext({
      viewport: { height: 720, width: 1280 },
    });
    const page = await context.newPage();
    const record = (value: string): void => {
      if (diagnostics.length < MAXIMUM_DIAGNOSTICS) {
        diagnostics.push(boundedDiagnostic(value));
      }
    };
    page.on("pageerror", (error) => record(`pageerror:${error.message}`));
    page.on("console", (message) => {
      if (message.type() === "error") {
        record(`console:${message.text()}`);
      }
    });
    const response = await page.goto(arguments_.joinUrl, {
      waitUntil: "domcontentloaded",
      timeout: 30_000,
    });
    if (response?.status() !== 200) {
      throw new Error(`controller_receiver_http_${response?.status() ?? 0}`);
    }
    await page.waitForFunction(
      () =>
        (window as unknown as { __glyphrelayReceiver?: ReceiverState })
          .__glyphrelayReceiver?.state === "RECEIVING",
      undefined,
      { timeout: 60_000 },
    );
    const deadline = Date.now() + arguments_.durationMilliseconds;
    while (Date.now() < deadline) {
      await page.waitForTimeout(Math.min(250, deadline - Date.now()));
      const drained = await page.evaluate(() =>
        (
          window as unknown as { __glyphrelayReceiver: ReceiverState }
        ).__glyphrelayReceiver.drainFrameObservations(),
      );
      observations.push(...drained.observations);
      droppedFrameObservations = drained.droppedFrameObservations;
    }
    const drained = await page.evaluate(() =>
      (
        window as unknown as { __glyphrelayReceiver: ReceiverState }
      ).__glyphrelayReceiver.drainFrameObservations(),
    );
    observations.push(...drained.observations);
    droppedFrameObservations = drained.droppedFrameObservations;
    const finalSnapshot = await page.evaluate(() =>
      (
        window as unknown as { __glyphrelayReceiver: ReceiverState }
      ).__glyphrelayReceiver.snapshot(),
    );
    const transportSnapshot = await page.evaluate(() =>
      (
        window as unknown as { __glyphrelayReceiver: ReceiverState }
      ).__glyphrelayReceiver.transportSnapshot(),
    );
    await durableExclusiveJson(arguments_.output, {
      browser: {
        executableSha256,
        name: arguments_.browser,
        version: browser.version(),
      },
      droppedFrameObservations,
      errors: diagnostics,
      finalSnapshot,
      observations,
      transportSnapshot,
    });
  } finally {
    await browser.close();
  }
}

async function main(): Promise<void> {
  const arguments_ = parseArguments(process.argv.slice(2));
  await qualifyLiveReceiver(arguments_);
  process.stdout.write("controller live receiver qualification completed\n");
}

if (
  process.argv[1] &&
  import.meta.url === pathToFileURL(process.argv[1]).href
) {
  main().catch((error: unknown) => {
    process.stderr.write(
      `controller live receiver qualification failed: ${error instanceof Error ? error.message : "unknown"}\n`,
    );
    process.exitCode = 1;
  });
}
