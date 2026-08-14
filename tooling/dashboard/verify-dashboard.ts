import assert from "node:assert/strict";

import { chromium } from "@playwright/test";

import {
  startDashboardServer,
  type DashboardActionResult,
  type DashboardBackend,
  type DashboardSnapshot,
} from "../../dashboard/server.ts";

interface BrowserDashboardState {
  error: string | null;
  snapshot: DashboardSnapshot | null;
  state: string;
}

class BrowserBackend implements DashboardBackend {
  actions: string[] = [];
  state: DashboardSnapshot = {
    bitrateProfile: "2m",
    captureActive: true,
    connectionState: "CONNECTED",
    droppedFrames: 2,
    fallbackMode: "CPU_UNIFORM",
    protectedFraction: 0.25,
    queueDelayMs: 8,
    recordingActive: true,
    shareLinkAvailable: true,
  };

  perform(action: string): DashboardActionResult {
    this.actions.push(action);
    if (action !== "PAUSE") {
      return {
        accepted: false,
        reason: "unexpected_browser_action",
        snapshot: this.state,
      };
    }
    this.state = {
      ...this.state,
      captureActive: false,
      connectionState: "PAUSED",
    };
    return {
      accepted: true,
      reason: "paused",
      snapshot: this.state,
    };
  }

  snapshot(): DashboardSnapshot {
    return this.state;
  }
}

const backend = new BrowserBackend();
const server = await startDashboardServer({ backend });
const browser = await chromium.launch({ headless: true });
const externalRequests: string[] = [];
const requestUrls: string[] = [];

try {
  const context = await browser.newContext({
    viewport: { height: 900, width: 1440 },
  });
  const page = await context.newPage();
  page.on("request", (request) => {
    requestUrls.push(request.url());
    if (!request.url().startsWith(server.origin)) {
      externalRequests.push(request.url());
    }
  });

  const response = await page.goto(server.launchUrl);
  assert.equal(response?.status(), 200);
  assert.match(
    response?.headers()["content-security-policy"] ?? "",
    /default-src 'none'/,
  );
  assert.equal(response?.headers()["referrer-policy"], "no-referrer");
  await page.waitForFunction(() => location.hash === "");
  await page.waitForFunction(
    () =>
      (
        window as unknown as {
          __glyphrelayDashboard?: BrowserDashboardState;
        }
      ).__glyphrelayDashboard?.state === "CONNECTED",
  );
  assert.equal(await page.title(), "GlyphRelay control room");
  assert.equal(await page.locator("#status-heading").innerText(), "CONNECTED");
  assert.equal(await page.locator("#recording-state").innerText(), "Recording");
  assert.equal(await page.locator("#queue-delay").innerText(), "8 ms");
  assert.equal(new URL(page.url()).hash, "");

  const pause = page.locator('button[data-action="PAUSE"]');
  assert.equal(await pause.isEnabled(), true);
  await pause.click();
  await page.waitForFunction(
    () =>
      (
        window as unknown as {
          __glyphrelayDashboard?: BrowserDashboardState;
        }
      ).__glyphrelayDashboard?.state === "PAUSED",
  );
  assert.deepEqual(backend.actions, ["PAUSE"]);
  assert.equal(await page.locator("#capture-indicator").innerText(), "Idle");
  assert.equal(
    await page.locator('button[data-action="RESUME"]').isEnabled(),
    true,
  );
  assert.deepEqual(externalRequests, []);

  const nonce = new URL(server.launchUrl).hash.slice("#nonce=".length);
  assert.ok(nonce);
  assert.equal(
    requestUrls.some((url) => url.includes(nonce)),
    false,
  );
  assert.equal((await context.cookies()).length, 0);
  const screenshotPath = process.env.GLYPHRELAY_DASHBOARD_SCREENSHOT;
  if (screenshotPath) {
    await page.screenshot({ fullPage: true, path: screenshotPath });
  }
  process.stdout.write(
    `${JSON.stringify({ browserVersion: browser.version(), state: "PAUSED" })}\n`,
  );
} finally {
  await browser.close();
  await server.close();
}
