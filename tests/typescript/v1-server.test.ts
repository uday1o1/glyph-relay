import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import http from "node:http";
import { resolve } from "node:path";
import test from "node:test";

import WebSocket from "ws";

import { SIGNAL_PROTOCOL } from "../../signaling/session-state.ts";
import {
  startV1SignalingServer,
  type V1SignalingServer,
} from "../../signaling/v1-server.ts";

async function rawRequest(
  origin: string,
  path: string,
  headers: Record<string, string> = {},
): Promise<{
  body: string;
  headers: http.IncomingHttpHeaders;
  status: number;
}> {
  const url = new URL(path, origin);
  return await new Promise((resolveRequest, reject) => {
    const request = http.request(url, { headers }, (response) => {
      const chunks: Buffer[] = [];
      response.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
      response.on("end", () => {
        resolveRequest({
          body: Buffer.concat(chunks).toString("utf8"),
          headers: response.headers,
          status: response.statusCode ?? 0,
        });
      });
    });
    request.once("error", reject);
    request.end();
  });
}

async function connect(
  server: V1SignalingServer,
  origin = server.origin,
  host?: string,
): Promise<WebSocket> {
  const socket = new WebSocket(`${server.webSocketOrigin}/v1/signal`, {
    headers: host ? { Host: host } : undefined,
    origin,
  });
  await new Promise<void>((resolveOpen, reject) => {
    socket.once("open", resolveOpen);
    socket.once("error", reject);
    socket.once("unexpected-response", (_request, response) => {
      reject(new Error(`upgrade_rejected_${response.statusCode}`));
    });
  });
  return socket;
}

async function rejectedUpgrade(
  server: V1SignalingServer,
  origin: string,
  host?: string,
): Promise<number> {
  const socket = new WebSocket(`${server.webSocketOrigin}/v1/signal`, {
    headers: host ? { Host: host } : undefined,
    origin,
  });
  return await new Promise<number>((resolveStatus, reject) => {
    socket.once("unexpected-response", (_request, response) => {
      resolveStatus(response.statusCode ?? 0);
      response.resume();
    });
    socket.once("open", () => reject(new Error("hostile_upgrade_accepted")));
    socket.once("error", () => undefined);
  });
}

async function receive(socket: WebSocket): Promise<Record<string, unknown>> {
  return await new Promise((resolveMessage, reject) => {
    const timer = setTimeout(
      () => reject(new Error("websocket_message_timeout")),
      2_000,
    );
    socket.once("message", (data, isBinary) => {
      clearTimeout(timer);
      try {
        assert.equal(isBinary, false);
        const parsed: unknown = JSON.parse(data.toString());
        assert.ok(
          parsed && typeof parsed === "object" && !Array.isArray(parsed),
        );
        resolveMessage(parsed as Record<string, unknown>);
      } catch (error) {
        reject(error);
      }
    });
  });
}

async function closed(socket: WebSocket): Promise<number> {
  if (socket.readyState === WebSocket.CLOSED) {
    return 0;
  }
  return await new Promise((resolveClose, reject) => {
    const timer = setTimeout(
      () => reject(new Error("websocket_close_timeout")),
      2_000,
    );
    socket.once("close", (code) => {
      clearTimeout(timer);
      resolveClose(code);
    });
  });
}

function send(
  socket: WebSocket,
  type: string,
  sequence: number,
  fields: Record<string, unknown> = {},
): void {
  socket.send(
    JSON.stringify({
      protocolVersion: SIGNAL_PROTOCOL,
      sequence,
      type,
      ...fields,
    }),
  );
}

function requiredString(
  message: Record<string, unknown>,
  field: string,
): string {
  const value = message[field];
  if (typeof value !== "string") {
    throw new Error(`${field} missing`);
  }
  return value;
}

async function waitFor(predicate: () => boolean): Promise<void> {
  const deadline = performance.now() + 1_000;
  while (!predicate()) {
    if (performance.now() >= deadline) {
      throw new Error("condition_timeout");
    }
    await new Promise((resolveWait) => setTimeout(resolveWait, 5));
  }
}

test("serves a self-contained receiver without exposing fragments or session state", async (context) => {
  const server = await startV1SignalingServer();
  context.after(() => server.close());

  const page = await rawRequest(server.origin, "/#join=not-sent");
  assert.equal(page.status, 200);
  assert.doesNotMatch(page.body, /not-sent/);
  assert.match(
    String(page.headers["content-security-policy"]),
    /default-src 'none'/,
  );
  assert.equal(page.headers["referrer-policy"], "no-referrer");
  assert.match(String(page.headers["permissions-policy"]), /camera=\(\)/);
  const health = await rawRequest(server.origin, "/healthz");
  assert.equal(health.status, 200);
  assert.deepEqual(JSON.parse(health.body), {
    protocolVersion: SIGNAL_PROTOCOL,
    status: "ok",
  });

  for (const file of [
    "v1-index.html",
    "v1-receiver.js",
    "control-protocol.js",
    "receiver.css",
  ]) {
    const source = await readFile(resolve("receiver", file), "utf8");
    assert.doesNotMatch(source, /https?:\/\//i);
    assert.doesNotMatch(source, /analytics|telemetry/i);
    if (file === "v1-receiver.js") {
      assert.match(source, /history\.replaceState/);
      assert.doesNotMatch(source, /const\s+fragment\s*=/);
    }
  }
});

test("rejects insecure non-loopback configuration, hostile Host, and hostile Origin", async (context) => {
  await assert.rejects(
    startV1SignalingServer({ host: "0.0.0.0" }),
    /insecure_non_loopback_bind_rejected/,
  );
  await assert.rejects(
    startV1SignalingServer({
      host: "0.0.0.0",
      publicOrigin: "http://example.invalid",
    }),
    /insecure_non_loopback_bind_rejected/,
  );

  const server = await startV1SignalingServer();
  context.after(() => server.close());
  assert.equal(
    (await rawRequest(server.origin, "/healthz", { Host: "attacker.invalid" }))
      .status,
    421,
  );
  assert.equal(await rejectedUpgrade(server, "http://attacker.invalid"), 403);
  assert.equal(
    await rejectedUpgrade(server, server.origin, "attacker.invalid"),
    403,
  );
});

test("owner creates an explicit single-use fragment link and receiver closure requires rotation", async (context) => {
  const server = await startV1SignalingServer();
  context.after(() => server.close());
  const owner = await connect(server);
  context.after(() => owner.terminate());

  send(owner, "CREATE_SESSION", 1);
  const created = await receive(owner);
  assert.equal(created.type, "SESSION_CREATED");
  const sessionId = requiredString(created, "sessionId");
  const ownerCapability = requiredString(created, "ownerCapability");
  assert.equal(created.joinCapability, undefined);

  send(owner, "CREATE_JOIN", 2, { ownerCapability, sessionId });
  const linkCreated = await receive(owner);
  assert.equal(linkCreated.type, "JOIN_CREATED");
  assert.equal(linkCreated.joinCapability, undefined);
  const joinUrl = new URL(requiredString(linkCreated, "joinUrl"));
  assert.equal(joinUrl.origin, server.origin);
  assert.equal(joinUrl.pathname, "/");
  assert.equal(joinUrl.search, "");
  const match = /^#join=([A-Za-z0-9_-]{22})\.([A-Za-z0-9_-]{43})$/.exec(
    joinUrl.hash,
  );
  assert.ok(match);
  assert.equal(match[1], sessionId);
  const joinCapability = match[2];
  assert.ok(joinCapability);

  const receiver = await connect(server);
  context.after(() => receiver.terminate());
  send(receiver, "RESERVE_JOIN", 1, { joinCapability, sessionId });
  assert.equal((await receive(receiver)).type, "JOIN_RESERVED");
  assert.equal((await receive(owner)).type, "RECEIVER_RESERVED");

  const replay = await connect(server);
  send(replay, "RESERVE_JOIN", 1, { joinCapability, sessionId });
  assert.equal(await closed(replay), 1008);
  assert.equal(server.sessions.snapshot(sessionId)?.phase, "JOIN_RESERVED");

  send(receiver, "RECEIVER_OFFER", 2, { sdp: "v=0\r\n", sessionId });
  assert.equal((await receive(owner)).type, "RECEIVER_OFFER");
  send(owner, "OWNER_ANSWER", 3, {
    ownerCapability,
    sdp: "v=0\r\n",
    sessionId,
  });
  assert.equal((await receive(receiver)).type, "OWNER_ANSWER");
  assert.equal(server.sessions.snapshot(sessionId)?.phase, "CONNECTED");

  receiver.close();
  await closed(receiver);
  assert.equal((await receive(owner)).type, "RECEIVER_DISCONNECTED");
  assert.equal(server.sessions.snapshot(sessionId)?.phase, "OWNER_ONLY");
  send(owner, "CREATE_JOIN", 4, { ownerCapability, sessionId });
  const replacement = await receive(owner);
  assert.equal(replacement.type, "JOIN_CREATED");
  assert.notEqual(replacement.joinUrl, linkCreated.joinUrl);

  owner.close();
  await closed(owner);
  await waitFor(() => server.sessions.snapshot(sessionId) === undefined);
  assert.equal(server.sessions.snapshot(sessionId), undefined);
});

test("oversized signaling and per-connection floods close and revoke the owner", async (context) => {
  const oversizedServer = await startV1SignalingServer();
  context.after(() => oversizedServer.close());
  const oversizedOwner = await connect(oversizedServer);
  send(oversizedOwner, "CREATE_SESSION", 1);
  const oversizedCreated = await receive(oversizedOwner);
  const oversizedSession = requiredString(oversizedCreated, "sessionId");
  oversizedOwner.send("x".repeat(64 * 1024 + 1));
  assert.ok([1008, 1009].includes(await closed(oversizedOwner)));
  await waitFor(
    () => oversizedServer.sessions.snapshot(oversizedSession) === undefined,
  );
  assert.equal(oversizedServer.sessions.snapshot(oversizedSession), undefined);

  const floodServer = await startV1SignalingServer();
  context.after(() => floodServer.close());
  const owner = await connect(floodServer);
  const receiver = await connect(floodServer);
  context.after(() => owner.terminate());
  context.after(() => receiver.terminate());
  send(owner, "CREATE_SESSION", 1);
  const created = await receive(owner);
  const sessionId = requiredString(created, "sessionId");
  const ownerCapability = requiredString(created, "ownerCapability");
  send(owner, "CREATE_JOIN", 2, { ownerCapability, sessionId });
  const link = new URL(requiredString(await receive(owner), "joinUrl"));
  const joinCapability = link.hash.split(".")[1];
  assert.ok(joinCapability);
  send(receiver, "RESERVE_JOIN", 1, { joinCapability, sessionId });
  await receive(receiver);
  await receive(owner);
  send(receiver, "RECEIVER_OFFER", 2, { sdp: "v=0\r\n", sessionId });
  await receive(owner);
  send(owner, "OWNER_ANSWER", 3, {
    ownerCapability,
    sdp: "v=0\r\n",
    sessionId,
  });
  await receive(receiver);
  for (let sequence = 4; sequence <= 24; sequence += 1) {
    send(owner, "OWNER_ICE_CANDIDATE", sequence, {
      candidate: `{"candidate":"candidate:${sequence}"}`,
      ownerCapability,
      sessionId,
    });
  }
  assert.equal(await closed(owner), 1008);
  assert.equal(floodServer.sessions.snapshot(sessionId), undefined);
});
