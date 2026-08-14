import { randomBytes, timingSafeEqual } from "node:crypto";
import { readFile } from "node:fs/promises";
import {
  createServer,
  type IncomingMessage,
  type ServerResponse,
} from "node:http";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const DASHBOARD_PROTOCOL = "glyphrelay-dashboard-v1";
const MAXIMUM_ACTION_BYTES = 4 * 1024;
const NONCE_BYTES = 32;
const REQUEST_TIMEOUT_MS = 5_000;

const ACTIONS = new Set([
  "START",
  "PAUSE",
  "RESUME",
  "STOP",
  "COPY_SHARE_LINK",
]);

interface StaticAsset {
  body: Buffer;
  contentType: string;
}

export interface DashboardSnapshot {
  bitrateProfile: "500k" | "1m" | "2m" | "4m";
  captureActive: boolean;
  connectionState:
    | "IDLE"
    | "OWNER_ONLY"
    | "JOIN_OPEN"
    | "CONNECTED"
    | "PAUSED"
    | "REVOKED"
    | "UNAVAILABLE";
  droppedFrames: number;
  fallbackMode: "CPU_UNIFORM" | "GPU_ENHANCED" | "UNAVAILABLE";
  protectedFraction: number;
  queueDelayMs: number;
  recordingActive: boolean;
  shareLinkAvailable: boolean;
}

export interface DashboardActionResult {
  accepted: boolean;
  reason: string;
  snapshot: DashboardSnapshot;
}

export interface DashboardBackend {
  perform(
    action: string,
  ): DashboardActionResult | Promise<DashboardActionResult>;
  snapshot(): DashboardSnapshot | Promise<DashboardSnapshot>;
}

export interface DashboardServerOptions {
  backend?: DashboardBackend;
  host?: string;
  port?: number;
}

export interface DashboardServer {
  readonly host: string;
  readonly launchUrl: string;
  readonly origin: string;
  readonly port: number;
  close(): Promise<void>;
}

class UnavailableBackend implements DashboardBackend {
  readonly #snapshot: DashboardSnapshot = {
    bitrateProfile: "2m",
    captureActive: false,
    connectionState: "UNAVAILABLE",
    droppedFrames: 0,
    fallbackMode: "UNAVAILABLE",
    protectedFraction: 0,
    queueDelayMs: 0,
    recordingActive: false,
    shareLinkAvailable: false,
  };

  perform(_action: string): DashboardActionResult {
    return {
      accepted: false,
      reason: "sender_backend_unavailable",
      snapshot: this.#snapshot,
    };
  }

  snapshot(): DashboardSnapshot {
    return this.#snapshot;
  }
}

function isLoopbackHost(host: string): boolean {
  return host === "127.0.0.1" || host === "::1";
}

function validatePort(port: number): void {
  if (!Number.isSafeInteger(port) || port < 0 || port > 65_535) {
    throw new Error("dashboard_port_invalid");
  }
}

function addressHost(host: string): string {
  return host.includes(":") ? `[${host}]` : host;
}

function randomToken(): string {
  return randomBytes(NONCE_BYTES).toString("base64url");
}

function exactSecret(actual: string | undefined, expected: string): boolean {
  const actualBytes = Buffer.from(actual ?? "", "utf8");
  const expectedBytes = Buffer.from(expected, "utf8");
  if (actualBytes.length !== expectedBytes.length) {
    timingSafeEqual(expectedBytes, Buffer.alloc(expectedBytes.length));
    return false;
  }
  return timingSafeEqual(actualBytes, expectedBytes);
}

function singleHeader(
  value: string | string[] | undefined,
): string | undefined {
  return typeof value === "string" ? value : undefined;
}

function securityHeaders(contentType: string): Record<string, string> {
  return {
    "Cache-Control": "no-store",
    "Content-Security-Policy":
      "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; " +
      "base-uri 'none'; form-action 'none'; frame-ancestors 'none'; object-src 'none'",
    "Content-Type": contentType,
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Permissions-Policy":
      "camera=(), microphone=(), geolocation=(), payment=(), usb=()",
    "Referrer-Policy": "no-referrer",
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
  };
}

function respond(
  response: ServerResponse,
  status: number,
  contentType: string,
  body: string | Buffer,
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

function reject(response: ServerResponse, status: number): void {
  respondJson(response, status, {
    error: "request_rejected",
    protocolVersion: DASHBOARD_PROTOCOL,
  });
}

function validRequestTarget(
  request: IncomingMessage,
  origin: string,
): URL | undefined {
  try {
    const url = new URL(request.url ?? "", origin);
    if (url.origin !== origin || url.search || url.hash) {
      return undefined;
    }
    return url;
  } catch {
    return undefined;
  }
}

function requestContextAllowed(
  request: IncomingMessage,
  origin: string,
  mutation: boolean,
): boolean {
  if (request.headers.cookie !== undefined) {
    return false;
  }
  const requestOrigin = request.headers.origin;
  if (mutation && requestOrigin !== origin) {
    return false;
  }
  if (requestOrigin !== undefined && requestOrigin !== origin) {
    return false;
  }
  const fetchSite = request.headers["sec-fetch-site"];
  if (
    fetchSite !== undefined &&
    fetchSite !== "same-origin" &&
    !(fetchSite === "none" && !mutation)
  ) {
    return false;
  }
  return true;
}

async function loadAssets(): Promise<ReadonlyMap<string, StaticAsset>> {
  const directory = dirname(fileURLToPath(import.meta.url));
  const definitions: ReadonlyArray<[string, string, string]> = [
    ["/", "index.html", "text/html; charset=utf-8"],
    ["/dashboard.js", "dashboard.js", "text/javascript; charset=utf-8"],
    ["/dashboard.css", "dashboard.css", "text/css; charset=utf-8"],
  ];
  const assets = new Map<string, StaticAsset>();
  for (const [path, file, contentType] of definitions) {
    assets.set(path, {
      body: await readFile(join(directory, file)),
      contentType,
    });
  }
  return assets;
}

type BodyResult = { ok: true; value: unknown } | { ok: false; status: number };

async function readActionBody(request: IncomingMessage): Promise<BodyResult> {
  if (request.headers["content-type"] !== "application/json") {
    request.resume();
    return { ok: false, status: 415 };
  }
  const declared = request.headers["content-length"];
  if (declared !== undefined) {
    const length = Number(declared);
    if (
      !Number.isSafeInteger(length) ||
      length < 1 ||
      length > MAXIMUM_ACTION_BYTES
    ) {
      request.resume();
      return { ok: false, status: length > MAXIMUM_ACTION_BYTES ? 413 : 400 };
    }
  }

  const chunks: Buffer[] = [];
  let bytes = 0;
  let tooLarge = false;
  for await (const chunk of request) {
    const buffer = Buffer.from(chunk);
    bytes += buffer.length;
    if (bytes > MAXIMUM_ACTION_BYTES) {
      tooLarge = true;
      continue;
    }
    chunks.push(buffer);
  }
  if (tooLarge) {
    return { ok: false, status: 413 };
  }
  if (bytes === 0) {
    return { ok: false, status: 400 };
  }
  try {
    const encoded = Buffer.concat(chunks).toString("utf8");
    const value = JSON.parse(encoded) as unknown;
    if (encoded !== JSON.stringify(value)) {
      return { ok: false, status: 400 };
    }
    return {
      ok: true,
      value,
    };
  } catch {
    return { ok: false, status: 400 };
  }
}

function parseAction(value: unknown): string | undefined {
  if (
    !value ||
    typeof value !== "object" ||
    Array.isArray(value) ||
    Object.getPrototypeOf(value) !== Object.prototype
  ) {
    return undefined;
  }
  const record = value as Record<string, unknown>;
  if (Object.keys(record).length !== 2) {
    return undefined;
  }
  if (
    record.protocolVersion !== DASHBOARD_PROTOCOL ||
    typeof record.action !== "string" ||
    !ACTIONS.has(record.action)
  ) {
    return undefined;
  }
  return record.action;
}

export async function startDashboardServer(
  options: DashboardServerOptions = {},
): Promise<DashboardServer> {
  const host = options.host ?? "127.0.0.1";
  const port = options.port ?? 0;
  if (!isLoopbackHost(host)) {
    throw new Error("dashboard_non_loopback_bind_rejected");
  }
  validatePort(port);

  const backend = options.backend ?? new UnavailableBackend();
  const assets = await loadAssets();
  const launchNonce = randomToken();
  const csrfToken = randomToken();
  let expectedHost = "";
  let origin = "";
  let closing = false;

  const requestHandler = async (
    request: IncomingMessage,
    response: ServerResponse,
  ): Promise<void> => {
    try {
      if (closing) {
        reject(response, 503);
        return;
      }
      if (request.headers.host !== expectedHost) {
        reject(response, 421);
        return;
      }
      const url = validRequestTarget(request, origin);
      if (!url) {
        reject(response, 400);
        return;
      }
      const mutation = request.method === "POST";
      if (!requestContextAllowed(request, origin, mutation)) {
        reject(response, 403);
        return;
      }

      if (request.method === "GET" && url.pathname === "/healthz") {
        respondJson(response, 200, {
          protocolVersion: DASHBOARD_PROTOCOL,
          status: "ok",
        });
        return;
      }
      if (request.method === "GET") {
        const asset = assets.get(url.pathname);
        if (asset) {
          respond(response, 200, asset.contentType, asset.body);
          return;
        }
      }
      if (request.method === "GET" && url.pathname === "/api/v1/state") {
        if (
          !exactSecret(
            singleHeader(request.headers["x-glyphrelay-dashboard-nonce"]),
            launchNonce,
          )
        ) {
          reject(response, 401);
          return;
        }
        respondJson(response, 200, {
          csrfToken,
          protocolVersion: DASHBOARD_PROTOCOL,
          snapshot: await backend.snapshot(),
        });
        return;
      }
      if (request.method === "POST" && url.pathname === "/api/v1/action") {
        if (
          !exactSecret(
            singleHeader(request.headers["x-glyphrelay-dashboard-nonce"]),
            launchNonce,
          ) ||
          !exactSecret(
            singleHeader(request.headers["x-glyphrelay-csrf"]),
            csrfToken,
          )
        ) {
          request.resume();
          reject(response, 401);
          return;
        }
        const body = await readActionBody(request);
        if (!body.ok) {
          reject(response, body.status);
          return;
        }
        const action = parseAction(body.value);
        if (!action) {
          reject(response, 400);
          return;
        }
        const result = await backend.perform(action);
        respondJson(response, result.accepted ? 200 : 409, {
          ...result,
          protocolVersion: DASHBOARD_PROTOCOL,
        });
        return;
      }

      if (request.method === "OPTIONS") {
        reject(response, 405);
        return;
      }
      if (request.method !== "GET" && request.method !== "POST") {
        request.resume();
        reject(response, 405);
        return;
      }
      reject(response, 404);
    } catch {
      if (!response.headersSent) {
        reject(response, 500);
      } else {
        response.destroy();
      }
    }
  };

  const server = createServer(
    {
      headersTimeout: REQUEST_TIMEOUT_MS,
      keepAliveTimeout: 1_000,
      maxHeaderSize: 8 * 1024,
      requestTimeout: REQUEST_TIMEOUT_MS,
      requireHostHeader: true,
    },
    (request, response) => void requestHandler(request, response),
  );
  server.maxRequestsPerSocket = 100;

  await new Promise<void>((resolveListen, rejectListen) => {
    server.once("error", rejectListen);
    server.listen({ exclusive: true, host, port }, resolveListen);
  });
  const address = server.address();
  if (!address || typeof address === "string" || address.address !== host) {
    server.close();
    throw new Error("dashboard_bind_verification_failed");
  }
  expectedHost = `${addressHost(host)}:${address.port}`;
  origin = `http://${expectedHost}`;

  return {
    host,
    launchUrl: `${origin}/#nonce=${launchNonce}`,
    origin,
    port: address.port,
    close: async () => {
      if (closing) {
        return;
      }
      closing = true;
      await new Promise<void>((resolveClose, rejectClose) => {
        server.close((error) => (error ? rejectClose(error) : resolveClose()));
      });
    },
  };
}

export { DASHBOARD_PROTOCOL, MAXIMUM_ACTION_BYTES };
