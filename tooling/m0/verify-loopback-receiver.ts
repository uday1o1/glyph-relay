import assert from "node:assert/strict";

import { chromium } from "@playwright/test";

import { startM0LoopbackServer } from "../../signaling/m0-loopback-server.ts";

interface ReceiverState {
  state: string;
  error: string | null;
}

async function main(): Promise<void> {
  const server = await startM0LoopbackServer();
  const browser = await chromium.launch({ headless: true });
  const externalRequests: string[] = [];
  try {
    const page = await browser.newPage();
    page.on("request", (request) => {
      if (!request.url().startsWith(server.origin)) {
        externalRequests.push(request.url());
      }
    });
    const response = await page.goto(
      `${server.origin}/#single-use-secret-must-disappear`,
    );
    assert.equal(response?.status(), 200);
    assert.match(
      response?.headers()["content-security-policy"] ?? "",
      /default-src 'none'/,
    );
    assert.equal(response?.headers()["referrer-policy"], "no-referrer");
    await page.waitForFunction(() => location.hash === "");
    assert.equal(await page.title(), "GlyphRelay receiver");
    assert.equal(
      await page.locator("#status").innerText(),
      "Ready to create a receive-only offer.",
    );

    await page.locator("#connect").click();
    await page.waitForFunction(
      () => {
        const receiver = (
          window as unknown as { __glyphrelayReceiver?: ReceiverState }
        ).__glyphrelayReceiver;
        return (
          receiver?.state === "FAILED" || receiver?.state === "WAITING_ANSWER"
        );
      },
      undefined,
      { timeout: 15_000 },
    );
    const state = await page.evaluate(
      () =>
        (window as unknown as { __glyphrelayReceiver: ReceiverState })
          .__glyphrelayReceiver,
    );
    if (state.state === "FAILED") {
      assert.equal(state.error, "offer_publish_failed_422");
      assert.match(
        await page.locator("#status").innerText(),
        /Receiver failed/,
      );
      assert.equal(server.snapshot().offer, undefined);
    } else {
      assert.ok(server.snapshot().offer, "compatible offer was not stored");
    }
    assert.deepEqual(externalRequests, []);
    process.stdout.write(
      `${JSON.stringify({ browserVersion: browser.version(), receiverState: state.state })}\n`,
    );
  } finally {
    await browser.close();
    await server.close();
  }
}

await main();
