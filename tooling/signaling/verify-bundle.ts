import { readFile } from "node:fs/promises";
import { request as httpRequest } from "node:http";
import { request as httpsRequest } from "node:https";

import WebSocket from "ws";

import { SIGNAL_PROTOCOL } from "../../signaling/session-state.ts";

interface Options {
  ca?: Buffer;
  origin: URL;
}

function parseArguments(argv: readonly string[]): {
  caPath?: string;
  origin: URL;
} {
  let caPath: string | undefined;
  let origin: URL | undefined;
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    const value = argv[index + 1];
    if (argument === "--") {
      continue;
    }
    if (argument === "--origin" && value) {
      origin = new URL(value);
      index += 1;
    } else if (argument === "--ca" && value) {
      caPath = value;
      index += 1;
    } else {
      throw new Error("usage: verify-bundle --origin URL [--ca PATH]");
    }
  }
  if (
    !origin ||
    !["http:", "https:"].includes(origin.protocol) ||
    origin.origin !== origin.href.replace(/\/$/, "")
  ) {
    throw new Error("bundle_origin_invalid");
  }
  return { caPath, origin };
}

async function request(
  options: Options,
  path: string,
): Promise<{ body: string; status: number }> {
  const makeRequest =
    options.origin.protocol === "https:" ? httpsRequest : httpRequest;
  return await new Promise((resolveRequest, reject) => {
    const request = makeRequest(
      new URL(path, options.origin),
      { ca: options.ca },
      (response) => {
        const chunks: Buffer[] = [];
        response.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
        response.on("end", () => {
          resolveRequest({
            body: Buffer.concat(chunks).toString("utf8"),
            status: response.statusCode ?? 0,
          });
        });
      },
    );
    request.once("error", reject);
    request.setTimeout(3_000, () =>
      request.destroy(new Error("request_timeout")),
    );
    request.end();
  });
}

async function connect(options: Options): Promise<WebSocket> {
  const webSocketOrigin = options.origin.origin.replace(/^http/, "ws");
  const socket = new WebSocket(`${webSocketOrigin}/v1/signal`, {
    ca: options.ca,
    origin: options.origin.origin,
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

async function receive(socket: WebSocket): Promise<Record<string, unknown>> {
  return await new Promise((resolveMessage, reject) => {
    const timeout = setTimeout(
      () => reject(new Error("signal_receive_timeout")),
      3_000,
    );
    socket.once("message", (raw, binary) => {
      clearTimeout(timeout);
      try {
        if (binary) {
          throw new Error("binary_response_rejected");
        }
        const decoded: unknown = JSON.parse(raw.toString());
        if (!decoded || typeof decoded !== "object" || Array.isArray(decoded)) {
          throw new Error("signal_response_invalid");
        }
        resolveMessage(decoded as Record<string, unknown>);
      } catch (error) {
        reject(error);
      }
    });
  });
}

function stringField(message: Record<string, unknown>, field: string): string {
  const value = message[field];
  if (typeof value !== "string") {
    throw new Error(`signal_response_${field}_missing`);
  }
  return value;
}

async function close(socket: WebSocket): Promise<void> {
  if (socket.readyState === WebSocket.CLOSED) {
    return;
  }
  const closed = new Promise<void>((resolveClose) => {
    socket.once("close", () => resolveClose());
  });
  socket.close();
  await closed;
}

async function main(): Promise<void> {
  const parsed = parseArguments(process.argv.slice(2));
  const options: Options = {
    ca: parsed.caPath ? await readFile(parsed.caPath) : undefined,
    origin: parsed.origin,
  };
  const health = await request(options, "/healthz");
  if (health.status !== 200 || JSON.parse(health.body).status !== "ok") {
    throw new Error("bundle_health_failed");
  }
  const owner = await connect(options);
  const receiver = await connect(options);
  try {
    send(owner, "CREATE_SESSION", 1);
    const created = await receive(owner);
    const sessionId = stringField(created, "sessionId");
    const ownerCapability = stringField(created, "ownerCapability");
    send(owner, "CREATE_JOIN", 2, { ownerCapability, sessionId });
    const joinUrl = new URL(stringField(await receive(owner), "joinUrl"));
    if (joinUrl.origin !== options.origin.origin || joinUrl.search) {
      throw new Error("bundle_join_url_invalid");
    }
    const match = /^#join=([A-Za-z0-9_-]{22})\.([A-Za-z0-9_-]{43})$/.exec(
      joinUrl.hash,
    );
    if (!match || match[1] !== sessionId || !match[2]) {
      throw new Error("bundle_join_fragment_invalid");
    }
    const page = await request(options, "/");
    if (page.status !== 200 || page.body.includes(match[2])) {
      throw new Error("bundle_fragment_disclosure");
    }
    send(receiver, "RESERVE_JOIN", 1, {
      joinCapability: match[2],
      sessionId,
    });
    if ((await receive(receiver)).type !== "JOIN_RESERVED") {
      throw new Error("bundle_receiver_reservation_failed");
    }
    if ((await receive(owner)).type !== "RECEIVER_RESERVED") {
      throw new Error("bundle_owner_notification_failed");
    }
    send(receiver, "RECEIVER_OFFER", 2, { sdp: "v=0\r\n", sessionId });
    if ((await receive(owner)).type !== "RECEIVER_OFFER") {
      throw new Error("bundle_offer_relay_failed");
    }
    send(owner, "OWNER_ANSWER", 3, {
      ownerCapability,
      sdp: "v=0\r\n",
      sessionId,
    });
    if ((await receive(receiver)).type !== "OWNER_ANSWER") {
      throw new Error("bundle_answer_relay_failed");
    }
  } finally {
    await close(receiver);
    await close(owner);
  }
  process.stdout.write("signaling bundle verification passed\n");
}

main().catch((error: unknown) => {
  process.stderr.write(
    `signaling bundle verification failed: ${error instanceof Error ? error.message : "unknown"}\n`,
  );
  process.exitCode = 1;
});
