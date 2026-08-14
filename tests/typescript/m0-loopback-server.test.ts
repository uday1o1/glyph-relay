import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import http from "node:http";
import { resolve } from "node:path";
import test from "node:test";

import { startM0LoopbackServer } from "../../signaling/m0-loopback-server.ts";

const compatibleOffer = [
  "v=0",
  "m=video 9 UDP/TLS/RTP/SAVPF 102 103",
  "a=rtpmap:102 H264/90000",
  "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
  "a=rtpmap:103 rtx/90000",
  "a=fmtp:103 apt=102",
  "",
].join("\r\n");

function envelope(type: "offer" | "answer", sdp = compatibleOffer): string {
  return JSON.stringify({
    protocolVersion: "glyphrelay-m0-loopback-v1",
    sessionId: "m0-loopback-session",
    type,
    sdp,
  });
}

async function rawRequest(
  origin: string,
  path: string,
  options: {
    method?: string;
    headers?: Record<string, string>;
    body?: string;
  } = {},
): Promise<{
  status: number;
  headers: http.IncomingHttpHeaders;
  body: string;
}> {
  const url = new URL(path, origin);
  return await new Promise((resolveRequest, reject) => {
    const request = http.request(
      url,
      { method: options.method ?? "GET", headers: options.headers },
      (response) => {
        const chunks: Buffer[] = [];
        response.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
        response.on("end", () =>
          resolveRequest({
            status: response.statusCode ?? 0,
            headers: response.headers,
            body: Buffer.concat(chunks).toString("utf8"),
          }),
        );
      },
    );
    request.on("error", reject);
    request.end(options.body);
  });
}

test("serves only explicit local assets with restrictive response headers", async (context) => {
  const server = await startM0LoopbackServer();
  context.after(() => server.close());
  assert.equal(server.host, "127.0.0.1");

  const page = await rawRequest(server.origin, "/");
  assert.equal(page.status, 200);
  assert.match(
    String(page.headers["content-security-policy"] ?? ""),
    /default-src 'none'/,
  );
  assert.equal(page.headers["referrer-policy"], "no-referrer");
  assert.equal(page.headers["cross-origin-opener-policy"], "same-origin");
  assert.doesNotMatch(page.body, /https?:\/\//i);
  assert.equal((await rawRequest(server.origin, "/healthz")).status, 200);
  assert.equal((await rawRequest(server.origin, "/missing")).status, 404);
});

test("rejects hostile Host, Origin, incompatible offers, replays, and invalid ordering", async (context) => {
  const server = await startM0LoopbackServer();
  context.after(() => server.close());
  const contentHeaders = {
    "Content-Type": "application/json",
    Origin: server.origin,
  };

  const hostileHost = await rawRequest(server.origin, "/healthz", {
    headers: { Host: "attacker.invalid" },
  });
  assert.equal(hostileHost.status, 421);
  const hostileOrigin = await rawRequest(server.origin, "/api/m0/offer", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Origin: "http://attacker.invalid",
    },
    body: envelope("offer"),
  });
  assert.equal(hostileOrigin.status, 403);
  const beforeOffer = await rawRequest(server.origin, "/api/m0/answer", {
    method: "POST",
    headers: contentHeaders,
    body: envelope("answer"),
  });
  assert.equal(beforeOffer.status, 409);
  const incompatible = await rawRequest(server.origin, "/api/m0/offer", {
    method: "POST",
    headers: contentHeaders,
    body: envelope("offer", compatibleOffer.replace("42e01f", "42e00d")),
  });
  assert.equal(incompatible.status, 422);
  assert.match(incompatible.body, /sharing_profile_offer_lacks_selected_level/);

  const accepted = await rawRequest(server.origin, "/api/m0/offer", {
    method: "POST",
    headers: contentHeaders,
    body: envelope("offer"),
  });
  assert.equal(accepted.status, 201);
  assert.equal(server.snapshot().offer?.sdp, compatibleOffer);
  assert.equal(
    (
      await rawRequest(server.origin, "/api/m0/offer", {
        method: "POST",
        headers: contentHeaders,
        body: envelope("offer"),
      })
    ).status,
    409,
  );
});

test("rejects oversized envelopes and static receiver contains no third-party references", async (context) => {
  const server = await startM0LoopbackServer();
  context.after(() => server.close());
  const oversized = await rawRequest(server.origin, "/api/m0/offer", {
    method: "POST",
    headers: { "Content-Type": "application/json", Origin: server.origin },
    body: `{"padding":"${"x".repeat(70 * 1024)}"}`,
  });
  assert.equal(oversized.status, 413);

  for (const file of ["index.html", "receiver.css", "receiver.js"]) {
    const source = await readFile(resolve("receiver", file), "utf8");
    assert.doesNotMatch(
      source,
      /https?:\/\//i,
      `${file} contains a third-party URL`,
    );
  }
});
