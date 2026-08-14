import { readFile } from "node:fs/promises";

import { startV1SignalingServer } from "./v1-server.ts";

function integerEnvironment(
  name: string,
  fallback: number,
  minimum: number,
  maximum: number,
): number {
  const raw = process.env[name];
  if (raw === undefined) {
    return fallback;
  }
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new Error(`${name.toLowerCase()}_invalid`);
  }
  return value;
}

async function optionalTls(): Promise<
  { cert: Buffer; key: Buffer } | undefined
> {
  const certPath = process.env.GLYPHRELAY_TLS_CERT_PATH;
  const keyPath = process.env.GLYPHRELAY_TLS_KEY_PATH;
  if (!certPath && !keyPath) {
    return undefined;
  }
  if (!certPath || !keyPath) {
    throw new Error("tls_certificate_pair_incomplete");
  }
  return {
    cert: await readFile(certPath),
    key: await readFile(keyPath),
  };
}

function optionalHashKey(): Buffer | undefined {
  const encoded = process.env.GLYPHRELAY_CAPABILITY_HASH_KEY;
  if (!encoded) {
    return undefined;
  }
  const key = Buffer.from(encoded, "base64url");
  if (key.length < 32 || key.toString("base64url") !== encoded) {
    throw new Error("capability_hash_key_invalid");
  }
  return key;
}

async function main(): Promise<void> {
  const server = await startV1SignalingServer({
    host: process.env.GLYPHRELAY_BIND_HOST ?? "127.0.0.1",
    maximumConnections: integerEnvironment(
      "GLYPHRELAY_MAX_CONNECTIONS",
      128,
      1,
      4_096,
    ),
    port: integerEnvironment("GLYPHRELAY_PORT", 8443, 1, 65_535),
    publicOrigin: process.env.GLYPHRELAY_SIGNALING_ORIGIN,
    sessionStore: {
      hashKey: optionalHashKey(),
      maximumSessions: integerEnvironment(
        "GLYPHRELAY_MAX_SESSIONS",
        64,
        1,
        1_024,
      ),
    },
    tls: await optionalTls(),
  });
  process.stdout.write(
    `GlyphRelay signaling ready at ${server.origin} (${server.secure ? "TLS" : "loopback"})\n`,
  );

  let stopping = false;
  const stop = (): void => {
    if (stopping) {
      return;
    }
    stopping = true;
    server
      .close()
      .then(() => process.exit(0))
      .catch((error: unknown) => {
        process.stderr.write(
          `signaling shutdown failed: ${error instanceof Error ? error.message : "unknown"}\n`,
        );
        process.exit(1);
      });
  };
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
}

main().catch((error: unknown) => {
  process.stderr.write(
    `signaling startup failed: ${error instanceof Error ? error.message : "unknown"}\n`,
  );
  process.exitCode = 1;
});
