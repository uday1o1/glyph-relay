import {
  createServer,
  type IncomingMessage,
  type ServerResponse,
} from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { evaluateRecordingProfileOffer } from "./sdp.ts";

const PROTOCOL_VERSION = "glyphrelay-m0-loopback-v1";
const SESSION_ID = "m0-loopback-session";
const LIVE_PRESENTATION = "720p30";
const MAXIMUM_ENVELOPE_BYTES = 68 * 1024;
const BODY_TIMEOUT_MILLISECONDS = 5_000;

interface SessionDescriptionEnvelope {
  protocolVersion: typeof PROTOCOL_VERSION;
  sessionId: typeof SESSION_ID;
  type: "offer" | "answer";
  sdp: string;
}

interface ExchangeSnapshot {
  offer?: SessionDescriptionEnvelope;
  answer?: SessionDescriptionEnvelope;
}

export interface M0LoopbackServer {
  readonly host: "127.0.0.1";
  readonly port: number;
  readonly origin: string;
  snapshot(): Readonly<ExchangeSnapshot>;
  close(): Promise<void>;
}

interface StaticAsset {
  contentType: string;
  body: Buffer;
}

function isLoopbackPeer(address: string | undefined): boolean {
  return (
    address === "127.0.0.1" ||
    address === "::1" ||
    address === "::ffff:127.0.0.1"
  );
}

function securityHeaders(contentType: string): Record<string, string> {
  return {
    "Cache-Control": "no-store",
    "Content-Security-Policy":
      "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; " +
      "media-src 'self' blob:; img-src 'self' data:; base-uri 'none'; form-action 'none'; " +
      "frame-ancestors 'none'; object-src 'none'",
    "Content-Type": contentType,
    "Cross-Origin-Opener-Policy": "same-origin",
    "Referrer-Policy": "no-referrer",
    "X-Content-Type-Options": "nosniff",
  };
}

function respond(
  response: ServerResponse,
  status: number,
  contentType: string,
  body = "",
): void {
  response.writeHead(status, securityHeaders(contentType));
  response.end(body);
}

function respondJson(
  response: ServerResponse,
  status: number,
  value: unknown,
): void {
  respond(
    response,
    status,
    "application/json; charset=utf-8",
    `${JSON.stringify(value)}\n`,
  );
}

async function readJson(request: IncomingMessage): Promise<unknown> {
  const contentType = request.headers["content-type"]
    ?.split(";", 1)[0]
    ?.trim()
    .toLowerCase();
  if (contentType !== "application/json") {
    throw new Error("content_type_invalid");
  }
  const chunks: Buffer[] = [];
  let bytes = 0;
  const timer = setTimeout(
    () => request.destroy(new Error("request_body_timeout")),
    BODY_TIMEOUT_MILLISECONDS,
  );
  try {
    for await (const rawChunk of request) {
      const chunk = Buffer.isBuffer(rawChunk)
        ? rawChunk
        : Buffer.from(rawChunk);
      bytes += chunk.length;
      if (bytes > MAXIMUM_ENVELOPE_BYTES) {
        throw new Error("request_body_too_large");
      }
      chunks.push(chunk);
    }
  } finally {
    clearTimeout(timer);
  }
  try {
    return JSON.parse(Buffer.concat(chunks).toString("utf8"));
  } catch {
    throw new Error("request_json_invalid");
  }
}

function parseEnvelope(
  value: unknown,
  expectedType: "offer" | "answer",
): SessionDescriptionEnvelope | undefined {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  const keys = Object.keys(candidate).sort();
  const expectedKeys = ["protocolVersion", "sdp", "sessionId", "type"];
  if (
    keys.length !== expectedKeys.length ||
    keys.some((key, index) => key !== expectedKeys[index])
  ) {
    return undefined;
  }
  if (
    candidate.protocolVersion !== PROTOCOL_VERSION ||
    candidate.sessionId !== SESSION_ID ||
    candidate.type !== expectedType ||
    typeof candidate.sdp !== "string" ||
    candidate.sdp.length === 0 ||
    Buffer.byteLength(candidate.sdp, "utf8") > 64 * 1024 ||
    candidate.sdp.includes("\0")
  ) {
    return undefined;
  }
  return candidate as unknown as SessionDescriptionEnvelope;
}

async function loadAssets(): Promise<ReadonlyMap<string, StaticAsset>> {
  const sourceDirectory = dirname(fileURLToPath(import.meta.url));
  const receiverDirectory = join(sourceDirectory, "..", "receiver");
  const definitions: ReadonlyArray<[string, string, string]> = [
    ["/", "index.html", "text/html; charset=utf-8"],
    ["/receiver.js", "receiver.js", "text/javascript; charset=utf-8"],
    ["/receiver.css", "receiver.css", "text/css; charset=utf-8"],
  ];
  const assets = new Map<string, StaticAsset>();
  for (const [path, file, contentType] of definitions) {
    assets.set(path, {
      contentType,
      body: await readFile(join(receiverDirectory, file)),
    });
  }
  return assets;
}

export async function startM0LoopbackServer(
  port = 0,
): Promise<M0LoopbackServer> {
  if (!Number.isSafeInteger(port) || port < 0 || port > 65_535) {
    throw new Error("loopback_port_invalid");
  }
  const host = "127.0.0.1" as const;
  const assets = await loadAssets();
  const exchange: ExchangeSnapshot = {};
  let expectedHost = "";
  let origin = "";

  const server = createServer(async (request, response) => {
    try {
      if (!isLoopbackPeer(request.socket.remoteAddress)) {
        respondJson(response, 403, { error: "non_loopback_peer_rejected" });
        return;
      }
      if (request.headers.host !== expectedHost) {
        respondJson(response, 421, { error: "host_header_rejected" });
        return;
      }
      const url = new URL(request.url ?? "/", origin);
      if (url.origin !== origin || url.search || url.hash) {
        respondJson(response, 400, { error: "request_target_invalid" });
        return;
      }

      if (request.method === "GET") {
        const asset = assets.get(url.pathname);
        if (asset) {
          response.writeHead(200, securityHeaders(asset.contentType));
          response.end(asset.body);
          return;
        }
        if (url.pathname === "/healthz") {
          respondJson(response, 200, {
            status: "ok",
            protocolVersion: PROTOCOL_VERSION,
          });
          return;
        }
        if (
          url.pathname === "/api/m0/offer" ||
          url.pathname === "/api/m0/answer"
        ) {
          const value = url.pathname.endsWith("offer")
            ? exchange.offer
            : exchange.answer;
          if (!value) {
            respond(response, 204, "application/json; charset=utf-8");
          } else {
            respondJson(response, 200, value);
          }
          return;
        }
        respondJson(response, 404, { error: "not_found" });
        return;
      }

      if (
        request.method === "POST" &&
        (url.pathname === "/api/m0/offer" || url.pathname === "/api/m0/answer")
      ) {
        if (request.headers.origin !== origin) {
          respondJson(response, 403, { error: "origin_rejected" });
          return;
        }
        const expectedType = url.pathname.endsWith("offer")
          ? "offer"
          : "answer";
        const envelope = parseEnvelope(await readJson(request), expectedType);
        if (!envelope) {
          respondJson(response, 400, { error: "signaling_envelope_invalid" });
          return;
        }
        if (expectedType === "offer") {
          const compatibility = evaluateRecordingProfileOffer(
            envelope.sdp,
            LIVE_PRESENTATION,
          );
          if (!compatibility.compatible) {
            respondJson(response, 422, {
              error: "recording_profile_offer_incompatible",
              reason: compatibility.reason,
            });
            return;
          }
          if (exchange.offer) {
            respondJson(response, 409, { error: "offer_already_published" });
            return;
          }
          exchange.offer = envelope;
        } else {
          if (!exchange.offer) {
            respondJson(response, 409, { error: "answer_before_offer" });
            return;
          }
          if (exchange.answer) {
            respondJson(response, 409, { error: "answer_already_published" });
            return;
          }
          exchange.answer = envelope;
        }
        respondJson(response, 201, { accepted: true });
        return;
      }

      respondJson(response, 405, { error: "method_not_allowed" });
    } catch (error) {
      const reason = error instanceof Error ? error.message : "request_failed";
      const status = reason === "request_body_too_large" ? 413 : 400;
      if (!response.headersSent) {
        respondJson(response, status, { error: reason });
      } else {
        response.destroy();
      }
    }
  });

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen({ host, port, exclusive: true }, () => resolve());
  });
  const address = server.address();
  if (!address || typeof address === "string" || address.address !== host) {
    server.close();
    throw new Error("loopback_bind_verification_failed");
  }
  expectedHost = `${host}:${address.port}`;
  origin = `http://${expectedHost}`;

  return {
    host,
    port: address.port,
    origin,
    snapshot: () => structuredClone(exchange),
    close: () =>
      new Promise<void>((resolve, reject) =>
        server.close((error) => (error ? reject(error) : resolve())),
      ),
  };
}

async function main(): Promise<void> {
  const rawPort = process.argv[2] ?? "0";
  if (!/^\d{1,5}$/.test(rawPort)) {
    throw new Error("usage: node signaling/m0-loopback-server.ts [PORT]");
  }
  const server = await startM0LoopbackServer(Number(rawPort));
  process.stdout.write(
    `${JSON.stringify({ origin: server.origin, protocolVersion: PROTOCOL_VERSION })}\n`,
  );
  const stop = async () => {
    await server.close();
    process.exitCode = 0;
  };
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
}

const invokedPath = process.argv[1];
if (invokedPath && fileURLToPath(import.meta.url) === invokedPath) {
  await main();
}

export const m0LoopbackProtocolVersion = PROTOCOL_VERSION;
export const m0LoopbackSessionId = SESSION_ID;
