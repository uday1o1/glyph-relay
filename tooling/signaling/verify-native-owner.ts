import { execFileSync, spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";

import WebSocket from "ws";

import {
  SIGNAL_PROTOCOL,
  type SessionSnapshot,
} from "../../signaling/session-state.ts";
import { startV1SignalingServer } from "../../signaling/v1-server.ts";

interface FixtureEvent {
  type: string;
  value?: string;
}

interface TlsFixture {
  certificate: Buffer;
  certificatePath: string;
  key: Buffer;
}

function requiredArgument(name: string): string {
  const index = process.argv.indexOf(name);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  if (!value) {
    throw new Error(`missing_argument:${name}`);
  }
  return value;
}

function withTimeout<T>(promise: Promise<T>, milliseconds: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error("native_owner_timeout")),
      milliseconds,
    );
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error: unknown) => {
        clearTimeout(timer);
        reject(error);
      },
    );
  });
}

function parseFragment(joinUrl: string): {
  joinCapability: string;
  sessionId: string;
} {
  const url = new URL(joinUrl);
  const match = /^#join=([A-Za-z0-9_-]{22})\.([A-Za-z0-9_-]{22,128})$/.exec(
    url.hash,
  );
  if (!match) {
    throw new Error("native_owner_join_url_invalid");
  }
  return { sessionId: match[1]!, joinCapability: match[2]! };
}

async function waitFor(
  predicate: () => boolean,
  milliseconds = 2_000,
): Promise<void> {
  const deadline = performance.now() + milliseconds;
  while (!predicate()) {
    if (performance.now() >= deadline) {
      throw new Error("native_owner_state_timeout");
    }
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

async function runIntegration(
  executable: string,
  tls: TlsFixture | undefined,
): Promise<void> {
  const server = await startV1SignalingServer(
    tls ? { tls: { cert: tls.certificate, key: tls.key } } : {},
  );
  const fixtureArguments = ["--origin", server.origin];
  if (tls) {
    fixtureArguments.push("--ca", tls.certificatePath);
  }
  const fixture = spawn(executable, fixtureArguments, {
    stdio: ["ignore", "pipe", "inherit"],
  });
  if (!fixture.stdout) {
    throw new Error("native_owner_stdout_missing");
  }
  const fixtureExit = once(fixture, "exit");

  const pendingEvents: FixtureEvent[] = [];
  const eventWaiters: Array<() => void> = [];
  const lines = createInterface({ input: fixture.stdout });
  lines.on("line", (line) => {
    const decoded = JSON.parse(line) as FixtureEvent;
    if (!decoded || typeof decoded.type !== "string") {
      throw new Error("native_owner_event_invalid");
    }
    pendingEvents.push(decoded);
    eventWaiters.shift()?.();
  });

  const nextFixtureEvent = async (type: string): Promise<FixtureEvent> => {
    while (true) {
      const event = pendingEvents.shift();
      if (event) {
        if (event.type === "ERROR" || event.type === "TERMINAL") {
          throw new Error(
            `native_owner_${event.type.toLowerCase()}:${event.value}`,
          );
        }
        if (event.type === type) {
          return event;
        }
        continue;
      }
      await withTimeout(
        new Promise<void>((resolve) => eventWaiters.push(resolve)),
        5_000,
      );
    }
  };

  let receiver: WebSocket | undefined;
  try {
    const sessionCreated = await nextFixtureEvent("SESSION_CREATED");
    const joinCreated = await nextFixtureEvent("JOIN_CREATED");
    if (!sessionCreated.value || !joinCreated.value) {
      throw new Error("native_owner_initial_event_missing_value");
    }
    const join = parseFragment(joinCreated.value);
    if (join.sessionId !== sessionCreated.value) {
      throw new Error("native_owner_session_identity_mismatch");
    }

    receiver = new WebSocket(`${server.webSocketOrigin}/v1/signal`, {
      headers: { Origin: server.origin },
      perMessageDeflate: false,
      ...(tls ? { ca: tls.certificate } : {}),
    });
    await withTimeout(
      once(receiver, "open").then(() => undefined),
      2_000,
    );
    let receiverSequence = 0;
    const receiverMessages: Record<string, unknown>[] = [];
    const receiverWaiters: Array<() => void> = [];
    receiver.on("message", (data, isBinary) => {
      if (isBinary) {
        throw new Error("native_owner_receiver_binary_message");
      }
      const message = JSON.parse(data.toString()) as Record<string, unknown>;
      if (message.type === "HEARTBEAT") {
        receiver?.send(
          JSON.stringify({
            heartbeatSequence: message.heartbeatSequence,
            protocolVersion: SIGNAL_PROTOCOL,
            sequence: ++receiverSequence,
            sessionId: join.sessionId,
            type: "RECEIVER_HEARTBEAT_ACK",
          }),
        );
        return;
      }
      receiverMessages.push(message);
      receiverWaiters.shift()?.();
    });

    const receiveType = async (
      type: string,
    ): Promise<Record<string, unknown>> => {
      while (true) {
        const message = receiverMessages.shift();
        if (message) {
          if (message.type !== type) {
            throw new Error(
              `native_owner_receiver_message_unexpected:${message.type}`,
            );
          }
          return message;
        }
        await withTimeout(
          new Promise<void>((resolve) => receiverWaiters.push(resolve)),
          2_000,
        );
      }
    };

    const sendReceiver = (
      type: string,
      fields: Record<string, unknown>,
    ): void => {
      receiver?.send(
        JSON.stringify({
          protocolVersion: SIGNAL_PROTOCOL,
          sequence: ++receiverSequence,
          type,
          ...fields,
        }),
      );
    };

    sendReceiver("RESERVE_JOIN", {
      joinCapability: join.joinCapability,
      sessionId: join.sessionId,
    });
    await receiveType("JOIN_RESERVED");
    await nextFixtureEvent("RECEIVER_RESERVED");

    const offer = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\n";
    sendReceiver("RECEIVER_OFFER", { sdp: offer, sessionId: join.sessionId });
    await nextFixtureEvent("OFFER_ANSWERED");
    const answer = await receiveType("OWNER_ANSWER");
    if (answer.sdp !== offer) {
      throw new Error("native_owner_answer_mismatch");
    }

    const candidate = JSON.stringify({
      candidate: "candidate:1 1 UDP 1 127.0.0.1 9 typ host",
      sdpMid: "0",
    });
    sendReceiver("RECEIVER_ICE_CANDIDATE", {
      candidate,
      sessionId: join.sessionId,
    });
    await nextFixtureEvent("CANDIDATE_ECHOED");
    const echoed = await receiveType("OWNER_ICE_CANDIDATE");
    if (echoed.candidate !== candidate) {
      throw new Error("native_owner_candidate_mismatch");
    }

    await new Promise((resolve) => setTimeout(resolve, 5_250));
    const live: SessionSnapshot | undefined = server.sessions.snapshot(
      join.sessionId,
    );
    if (!live || live.phase !== "CONNECTED") {
      throw new Error("native_owner_heartbeat_liveness_failed");
    }

    receiver.close();
    await withTimeout(
      once(receiver, "close").then(() => undefined),
      2_000,
    );
    await nextFixtureEvent("RECEIVER_DISCONNECTED");
    const [exitCode] = (await withTimeout(fixtureExit, 3_000)) as [
      number | null,
      NodeJS.Signals | null,
    ];
    if (exitCode !== 0) {
      throw new Error(`native_owner_exit_failed:${exitCode}`);
    }
    await waitFor(() => server.sessions.snapshot(join.sessionId) === undefined);
    process.stdout.write(
      `native owner signaling ${tls ? "WSS" : "WS"} integration passed\n`,
    );
  } finally {
    receiver?.terminate();
    fixture.kill("SIGTERM");
    lines.close();
    await server.close();
  }
}

async function main(): Promise<void> {
  const executable = requiredArgument("--executable");
  const useTls = process.argv.includes("--tls");
  const temporaryDirectory = useTls
    ? await mkdtemp(join(tmpdir(), "glyphrelay-owner-signaling-"))
    : undefined;
  try {
    let tls: TlsFixture | undefined;
    if (temporaryDirectory) {
      const certificatePath = join(temporaryDirectory, "certificate.pem");
      const keyPath = join(temporaryDirectory, "private-key.pem");
      execFileSync(
        "openssl",
        [
          "req",
          "-x509",
          "-newkey",
          "rsa:2048",
          "-nodes",
          "-days",
          "1",
          "-subj",
          "/CN=127.0.0.1",
          "-addext",
          "subjectAltName=IP:127.0.0.1",
          "-keyout",
          keyPath,
          "-out",
          certificatePath,
        ],
        { stdio: "ignore" },
      );
      tls = {
        certificate: await readFile(certificatePath),
        certificatePath,
        key: await readFile(keyPath),
      };
    }
    await runIntegration(executable, tls);
  } finally {
    if (temporaryDirectory) {
      await rm(temporaryDirectory, { force: true, recursive: true });
    }
  }
}

await main();
