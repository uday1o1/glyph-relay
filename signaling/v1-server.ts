import { readFile } from "node:fs/promises";
import {
  createServer as createHttpServer,
  type IncomingMessage,
  type ServerResponse,
} from "node:http";
import {
  createServer as createHttpsServer,
  type ServerOptions as HttpsServerOptions,
} from "node:https";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import WebSocket, {
  WebSocketServer,
  type RawData,
  type ServerOptions as WebSocketServerOptions,
} from "ws";

import {
  parseSignalClientMessage,
  SIGNAL_PROTOCOL,
  SignalingSessionStore,
  type ConnectionBinding,
  type SignalAction,
  type SignalingSessionStoreOptions,
} from "./session-state.ts";

const SIGNAL_PATH = "/v1/signal";
const MAXIMUM_SIGNAL_BYTES = 64 * 1024;
const MAXIMUM_CONNECTIONS = 128;
const MAXIMUM_MESSAGES_PER_SECOND = 20;
const MAXIMUM_BUFFERED_SEND_BYTES = 256 * 1024;
const FIRST_MESSAGE_TIMEOUT_MS = 5_000;
const TICK_INTERVAL_MS = 250;

interface StaticAsset {
  body: Buffer;
  contentType: string;
}

interface ConnectionState {
  binding?: ConnectionBinding;
  firstMessageTimer: NodeJS.Timeout;
  id: number;
  messageTimes: number[];
  socket: WebSocket;
}

export interface SignalingTlsOptions {
  cert: NonNullable<HttpsServerOptions["cert"]>;
  key: NonNullable<HttpsServerOptions["key"]>;
}

export interface V1SignalingServerOptions {
  host?: string;
  maximumConnections?: number;
  port?: number;
  publicOrigin?: string;
  sessionStore?: SignalingSessionStoreOptions;
  tls?: SignalingTlsOptions;
}

export interface V1SignalingServer {
  readonly host: string;
  readonly origin: string;
  readonly port: number;
  readonly secure: boolean;
  readonly sessions: SignalingSessionStore;
  readonly webSocketOrigin: string;
  close(): Promise<void>;
}

function isLoopbackHost(host: string): boolean {
  return host === "127.0.0.1" || host === "::1";
}

function validatePort(port: number): void {
  if (!Number.isSafeInteger(port) || port < 0 || port > 65_535) {
    throw new Error("signaling_port_invalid");
  }
}

function validateMaximumConnections(value: number): void {
  if (!Number.isSafeInteger(value) || value < 1 || value > 4_096) {
    throw new Error("signaling_connection_limit_invalid");
  }
}

function parsePublicOrigin(value: string): URL {
  const origin = new URL(value);
  if (
    origin.origin !== value ||
    origin.username ||
    origin.password ||
    origin.pathname !== "/" ||
    origin.search ||
    origin.hash
  ) {
    throw new Error("signaling_public_origin_invalid");
  }
  return origin;
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
    "Permissions-Policy":
      "camera=(), microphone=(), geolocation=(), payment=(), usb=()",
    "Referrer-Policy": "no-referrer",
    "X-Content-Type-Options": "nosniff",
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

async function loadAssets(): Promise<ReadonlyMap<string, StaticAsset>> {
  const sourceDirectory = dirname(fileURLToPath(import.meta.url));
  const receiverDirectory = join(sourceDirectory, "..", "receiver");
  const definitions: ReadonlyArray<[string, string, string]> = [
    ["/", "v1-index.html", "text/html; charset=utf-8"],
    ["/v1-receiver.js", "v1-receiver.js", "text/javascript; charset=utf-8"],
    [
      "/control-protocol.js",
      "control-protocol.js",
      "text/javascript; charset=utf-8",
    ],
    ["/receiver.css", "receiver.css", "text/css; charset=utf-8"],
  ];
  const assets = new Map<string, StaticAsset>();
  for (const [path, file, contentType] of definitions) {
    assets.set(path, {
      body: await readFile(join(receiverDirectory, file)),
      contentType,
    });
  }
  return assets;
}

function rawDataBuffer(data: RawData): Buffer {
  if (Buffer.isBuffer(data)) {
    return data;
  }
  if (data instanceof ArrayBuffer) {
    return Buffer.from(data);
  }
  if (Array.isArray(data)) {
    return Buffer.concat(data);
  }
  throw new Error("unsupported_websocket_frame");
}

function safeClose(socket: WebSocket, code: number, reason: string): void {
  if (socket.readyState === WebSocket.OPEN) {
    socket.close(code, reason.slice(0, 100));
  }
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

function addressHost(host: string): string {
  return host.includes(":") ? `[${host}]` : host;
}

export async function startV1SignalingServer(
  options: V1SignalingServerOptions = {},
): Promise<V1SignalingServer> {
  const host = options.host ?? "127.0.0.1";
  const port = options.port ?? 0;
  const maximumConnections = options.maximumConnections ?? MAXIMUM_CONNECTIONS;
  validatePort(port);
  validateMaximumConnections(maximumConnections);

  const loopback = isLoopbackHost(host);
  if (!loopback && !options.tls) {
    throw new Error("insecure_non_loopback_bind_rejected");
  }
  const configuredOrigin = options.publicOrigin
    ? parsePublicOrigin(options.publicOrigin)
    : undefined;
  if (!loopback && !configuredOrigin) {
    throw new Error("non_loopback_public_origin_required");
  }
  if (options.tls && configuredOrigin?.protocol === "http:") {
    throw new Error("tls_origin_scheme_invalid");
  }
  if (!loopback && configuredOrigin?.protocol !== "https:") {
    throw new Error("non_loopback_https_origin_required");
  }
  if (!options.tls && configuredOrigin?.protocol === "https:") {
    throw new Error("https_origin_requires_tls");
  }

  const assets = await loadAssets();
  const sessions = new SignalingSessionStore(options.sessionStore);
  const connections = new Map<number, ConnectionState>();
  let expectedHost = "";
  let origin = "";
  let nextConnectionId = 1;
  let closing = false;

  const requestHandler = (
    request: IncomingMessage,
    response: ServerResponse,
  ): void => {
    if (request.headers.host !== expectedHost) {
      respondJson(response, 421, { error: "host_header_rejected" });
      return;
    }
    const url = validRequestTarget(request, origin);
    if (!url) {
      respondJson(response, 400, { error: "request_target_invalid" });
      return;
    }
    if (request.method !== "GET") {
      respondJson(response, 405, { error: "method_not_allowed" });
      return;
    }
    if (url.pathname === "/healthz") {
      respondJson(response, 200, {
        protocolVersion: SIGNAL_PROTOCOL,
        status: "ok",
      });
      return;
    }
    const asset = assets.get(url.pathname);
    if (!asset) {
      respondJson(response, 404, { error: "not_found" });
      return;
    }
    respond(response, 200, asset.contentType, asset.body);
  };

  const server = options.tls
    ? createHttpsServer(options.tls, requestHandler)
    : createHttpServer(requestHandler);
  const webSocketOptions: WebSocketServerOptions & {
    maxBufferedChunks: number;
    maxFragments: number;
  } = {
    clientTracking: false,
    maxBufferedChunks: 32,
    maxFragments: 32,
    maxPayload: MAXIMUM_SIGNAL_BYTES,
    noServer: true,
    perMessageDeflate: false,
    skipUTF8Validation: false,
  };
  const webSocketServer = new WebSocketServer(webSocketOptions);

  const dispatch = (actions: readonly SignalAction[]): void => {
    for (const action of actions) {
      const destination = connections.get(action.connectionId);
      if (!destination) {
        continue;
      }
      if (action.kind === "CLOSE") {
        safeClose(destination.socket, 1008, action.reason ?? "session_closed");
        continue;
      }
      if (!action.message || destination.socket.readyState !== WebSocket.OPEN) {
        continue;
      }
      if (destination.socket.bufferedAmount > MAXIMUM_BUFFERED_SEND_BYTES) {
        const cleanup = destination.binding
          ? sessions.disconnect(destination.binding, "SIGNAL_BACKPRESSURE")
          : [];
        safeClose(destination.socket, 1008, "signal_backpressure");
        dispatch(cleanup);
        continue;
      }
      const message = { ...action.message };
      if (
        message.type === "JOIN_CREATED" &&
        typeof message.joinCapability === "string"
      ) {
        message.joinUrl = `${origin}/#join=${message.sessionId}.${message.joinCapability}`;
        delete message.joinCapability;
      }
      destination.socket.send(JSON.stringify(message));
    }
  };

  const failConnection = (
    connection: ConnectionState,
    reason: string,
  ): void => {
    const actions = connection.binding
      ? sessions.disconnect(connection.binding, reason)
      : [];
    dispatch(actions);
    safeClose(connection.socket, 1008, reason);
  };

  webSocketServer.on(
    "connection",
    (
      socket: WebSocket,
      _request: IncomingMessage,
      connection: ConnectionState,
    ) => {
      connections.set(connection.id, connection);
      socket.on("message", (data, isBinary) => {
        clearTimeout(connection.firstMessageTimer);
        if (isBinary) {
          failConnection(connection, "binary_signal_rejected");
          return;
        }
        const now = performance.now();
        connection.messageTimes = connection.messageTimes.filter(
          (seenAt) => now - seenAt < 1_000,
        );
        if (connection.messageTimes.length >= MAXIMUM_MESSAGES_PER_SECOND) {
          failConnection(connection, "signal_rate_exceeded");
          return;
        }
        connection.messageTimes.push(now);
        const bytes = rawDataBuffer(data);
        if (bytes.length === 0 || bytes.length > MAXIMUM_SIGNAL_BYTES) {
          failConnection(connection, "signal_size_invalid");
          return;
        }
        let decoded: unknown;
        try {
          decoded = JSON.parse(bytes.toString("utf8"));
        } catch {
          failConnection(connection, "signal_json_invalid");
          return;
        }
        const message = parseSignalClientMessage(decoded);
        if (!message) {
          failConnection(connection, "signal_message_invalid");
          return;
        }
        const result = sessions.handle(
          connection.id,
          connection.binding,
          message,
        );
        if (result.binding) {
          connection.binding = result.binding;
        }
        dispatch(result.actions);
        if (!result.accepted) {
          safeClose(connection.socket, 1008, result.reason);
        }
      });
      socket.once("close", () => {
        clearTimeout(connection.firstMessageTimer);
        connections.delete(connection.id);
        if (connection.binding) {
          dispatch(sessions.disconnect(connection.binding));
        }
      });
      socket.once("error", () => {
        failConnection(connection, "SIGNALING_ERROR");
      });
    },
  );

  server.on("upgrade", (request, socket, head) => {
    if (
      closing ||
      connections.size >= maximumConnections ||
      request.headers.host !== expectedHost ||
      request.headers.origin !== origin
    ) {
      socket.end("HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
      return;
    }
    const url = validRequestTarget(request, origin);
    if (!url || url.pathname !== SIGNAL_PATH) {
      socket.end("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
      return;
    }
    const connection: ConnectionState = {
      firstMessageTimer: setTimeout(() => {
        safeClose(connection.socket, 1008, "first_message_timeout");
      }, FIRST_MESSAGE_TIMEOUT_MS),
      id: nextConnectionId++,
      messageTimes: [],
      socket: undefined as unknown as WebSocket,
    };
    webSocketServer.handleUpgrade(request, socket, head, (webSocket) => {
      connection.socket = webSocket;
      webSocketServer.emit("connection", webSocket, request, connection);
    });
  });

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen({ exclusive: true, host, port }, resolve);
  });
  const address = server.address();
  if (!address || typeof address === "string") {
    server.close();
    throw new Error("signaling_bind_verification_failed");
  }
  const secure = Boolean(options.tls);
  origin =
    configuredOrigin?.origin ??
    `${secure ? "https" : "http"}://${addressHost(host)}:${address.port}`;
  expectedHost = new URL(origin).host;
  const webSocketOrigin = origin.replace(/^http/, "ws");

  const tickTimer = setInterval(
    () => dispatch(sessions.tick()),
    TICK_INTERVAL_MS,
  );
  tickTimer.unref();

  return {
    host,
    origin,
    port: address.port,
    secure,
    sessions,
    webSocketOrigin,
    close: async () => {
      if (closing) {
        return;
      }
      closing = true;
      clearInterval(tickTimer);
      for (const connection of connections.values()) {
        clearTimeout(connection.firstMessageTimer);
        connection.socket.terminate();
      }
      connections.clear();
      await new Promise<void>((resolveClose, reject) => {
        webSocketServer.close(() => {
          server.close((error) => (error ? reject(error) : resolveClose()));
        });
      });
    },
  };
}
