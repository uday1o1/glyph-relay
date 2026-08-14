import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { arch, platform, release } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { chromium, firefox, type BrowserType } from "@playwright/test";

import {
  evaluateRecordingProfileOffer,
  type RecordingProfileOfferResult,
} from "../../signaling/sdp.ts";

interface BrowserLock {
  revision: string;
  version: string;
}

interface DependencyLock {
  playwright: {
    version: string;
    chromium: BrowserLock;
    firefox: BrowserLock;
  };
}

interface BrowserOfferEvidence {
  browser: "chromium" | "firefox";
  expectedVersion: string;
  expectedRevision: string;
  actualVersion: string;
  executablePath: string;
  executableSha256: string;
  capabilities: unknown;
  offerSdp: string;
  compatibility: RecordingProfileOfferResult;
  diagnostics: readonly string[];
}

interface BrowserOfferReport {
  schemaVersion: 1;
  protocol: "glyphrelay-browser-offers-v1";
  status: "PASSED" | "INCOMPATIBLE" | "INFRASTRUCTURE_FAILURE";
  generatedAtUtc: string;
  host: {
    platform: string;
    release: string;
    architecture: string;
    nodeVersion: string;
    playwrightVersion: string;
  };
  browsers: readonly BrowserOfferEvidence[];
  failures: readonly string[];
}

async function sha256File(path: string): Promise<string> {
  const digest = createHash("sha256");
  for await (const chunk of createReadStream(path)) {
    digest.update(chunk);
  }
  return digest.digest("hex");
}

async function captureBrowserOffer(
  browserName: "chromium" | "firefox",
  browserType: BrowserType,
  expected: BrowserLock,
): Promise<BrowserOfferEvidence> {
  const executablePath = browserType.executablePath();
  const executableSha256 = await sha256File(executablePath);
  const browser = await browserType.launch({ headless: true });
  const diagnostics: string[] = [];
  try {
    const actualVersion = browser.version();
    const page = await browser.newPage();
    page.on("console", (message) => {
      if (diagnostics.length < 100) {
        diagnostics.push(
          `console:${message.type()}:${message.text().slice(0, 1_024)}`,
        );
      }
    });
    page.on("pageerror", (error) => {
      if (diagnostics.length < 100) {
        diagnostics.push(`pageerror:${error.message.slice(0, 1_024)}`);
      }
    });
    const capture = await page.evaluate(async () => {
      const capabilities = RTCRtpReceiver.getCapabilities("video") ?? null;
      const connection = new RTCPeerConnection({ iceServers: [] });
      try {
        connection.addTransceiver("video", { direction: "recvonly" });
        const offer = await connection.createOffer();
        if (!offer.sdp) {
          throw new Error("browser_offer_sdp_missing");
        }
        return { capabilities, offerSdp: offer.sdp };
      } finally {
        connection.close();
      }
    });
    await page.close();
    return {
      browser: browserName,
      expectedVersion: expected.version,
      expectedRevision: expected.revision,
      actualVersion,
      executablePath,
      executableSha256,
      capabilities: capture.capabilities,
      offerSdp: capture.offerSdp,
      compatibility: evaluateRecordingProfileOffer(capture.offerSdp, "720p30"),
      diagnostics,
    };
  } finally {
    await browser.close();
  }
}

export async function captureBrowserOfferReport(
  repositoryRoot: string,
): Promise<BrowserOfferReport> {
  const packageJson = JSON.parse(
    await readFile(resolve(repositoryRoot, "package.json"), "utf8"),
  ) as { devDependencies?: Record<string, string> };
  const lock = JSON.parse(
    await readFile(resolve(repositoryRoot, "dependencies.lock.json"), "utf8"),
  ) as DependencyLock;
  const installedPlaywright = packageJson.devDependencies?.["@playwright/test"];
  const failures: string[] = [];
  const browsers: BrowserOfferEvidence[] = [];

  if (installedPlaywright !== lock.playwright.version) {
    failures.push("playwright_package_version_mismatch");
  }
  for (const [name, type, expected] of [
    ["chromium", chromium, lock.playwright.chromium],
    ["firefox", firefox, lock.playwright.firefox],
  ] as const) {
    try {
      const evidence = await captureBrowserOffer(name, type, expected);
      browsers.push(evidence);
      if (evidence.actualVersion !== evidence.expectedVersion) {
        failures.push(`${name}_version_mismatch`);
      }
    } catch (error) {
      const reason =
        error instanceof Error
          ? error.message
          : "unknown_browser_capture_error";
      failures.push(`${name}_capture_failed:${reason}`);
    }
  }

  const infrastructureFailed =
    failures.length > 0 ||
    browsers.length !== 2 ||
    browsers.some((browser) => browser.executableSha256.length !== 64);
  const compatible =
    browsers.length === 2 &&
    browsers.every((browser) => browser.compatibility.compatible);
  return {
    schemaVersion: 1,
    protocol: "glyphrelay-browser-offers-v1",
    status: infrastructureFailed
      ? "INFRASTRUCTURE_FAILURE"
      : compatible
        ? "PASSED"
        : "INCOMPATIBLE",
    generatedAtUtc: new Date().toISOString(),
    host: {
      platform: platform(),
      release: release(),
      architecture: arch(),
      nodeVersion: process.version,
      playwrightVersion: lock.playwright.version,
    },
    browsers,
    failures,
  };
}

function parseOutputArgument(arguments_: readonly string[]): string {
  if (
    arguments_.length !== 2 ||
    arguments_[0] !== "--output" ||
    !arguments_[1]
  ) {
    throw new Error(
      "usage: node tooling/m0/capture-browser-offers.ts --output FILE",
    );
  }
  return resolve(arguments_[1]);
}

async function main(): Promise<void> {
  const output = parseOutputArgument(process.argv.slice(2));
  const repositoryRoot = resolve(
    dirname(fileURLToPath(import.meta.url)),
    "../..",
  );
  const report = await captureBrowserOfferReport(repositoryRoot);
  await mkdir(dirname(output), { recursive: true });
  await writeFile(output, `${JSON.stringify(report, null, 2)}\n`, {
    encoding: "utf8",
    mode: 0o600,
  });
  process.stdout.write(
    `${JSON.stringify({ output, status: report.status })}\n`,
  );
  process.exitCode =
    report.status === "PASSED" ? 0 : report.status === "INCOMPATIBLE" ? 4 : 5;
}

const invokedPath = process.argv[1]
  ? pathToFileURL(resolve(process.argv[1])).href
  : undefined;
if (invokedPath === import.meta.url) {
  await main();
}
