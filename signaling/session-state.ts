import {
  createHmac,
  randomBytes,
  timingSafeEqual,
  type BinaryLike,
} from "node:crypto";
import { performance } from "node:perf_hooks";

export const SIGNAL_PROTOCOL = "glyphrelay-signal-v1" as const;
export const JOIN_CAPABILITY_LIFETIME_MS = 10 * 60 * 1_000;
export const OWNER_ONLY_LIFETIME_MS = 15 * 60 * 1_000;
export const JOIN_RESERVATION_LIFETIME_MS = 30 * 1_000;
export const ICE_DISCONNECTED_GRACE_MS = 5 * 1_000;
export const ABSOLUTE_SESSION_LIFETIME_MS = 8 * 60 * 60 * 1_000;
export const HEARTBEAT_INTERVAL_MS = 2 * 1_000;
export const PEER_ACTIVITY_TIMEOUT_MS = 5 * 1_000;

const MAXIMUM_SDP_BYTES = 64 * 1024;
const MAXIMUM_CANDIDATE_BYTES = 4 * 1024;
const CAPABILITY_PATTERN = /^[A-Za-z0-9_-]{22,128}$/;
const SESSION_PATTERN = /^[A-Za-z0-9_-]{22}$/;

export type SessionPhase =
  | "OWNER_ONLY"
  | "JOIN_OPEN"
  | "JOIN_RESERVED"
  | "CONNECTED"
  | "EXPIRED"
  | "REVOKED";
export type ConnectionRole = "OWNER" | "RECEIVER";

export interface ConnectionBinding {
  connectionId: number;
  role: ConnectionRole;
  sessionId: string;
  generation: number;
}

interface MessageBase {
  protocolVersion: typeof SIGNAL_PROTOCOL;
  sequence: number;
  type: string;
}

interface CreateSessionMessage extends MessageBase {
  type: "CREATE_SESSION";
}

interface ReserveJoinMessage extends MessageBase {
  type: "RESERVE_JOIN";
  sessionId: string;
  joinCapability: string;
}

interface OwnerMessageBase extends MessageBase {
  sessionId: string;
  ownerCapability: string;
}

interface CreateJoinMessage extends OwnerMessageBase {
  type: "CREATE_JOIN";
}

interface OwnerSdpMessage extends OwnerMessageBase {
  type: "OWNER_ANSWER" | "OWNER_ICE_RESTART_ANSWER";
  sdp: string;
}

interface OwnerCandidateMessage extends OwnerMessageBase {
  type: "OWNER_ICE_CANDIDATE";
  candidate: string;
}

interface OwnerStopMessage extends OwnerMessageBase {
  type: "OWNER_STOP" | "OWNER_REVOKE";
}

interface OwnerHeartbeatMessage extends OwnerMessageBase {
  type: "OWNER_HEARTBEAT_ACK";
  heartbeatSequence: number;
}

interface ReceiverIceStateMessage extends MessageBase {
  type: "RECEIVER_ICE_STATE";
  sessionId: string;
  iceState: "connected" | "disconnected" | "failed" | "closed";
}

interface ReceiverSdpMessage extends MessageBase {
  type: "RECEIVER_OFFER" | "RECEIVER_ICE_RESTART_OFFER";
  sessionId: string;
  sdp: string;
}

interface ReceiverCandidateMessage extends MessageBase {
  type: "RECEIVER_ICE_CANDIDATE";
  sessionId: string;
  candidate: string;
}

interface ReceiverHeartbeatMessage extends MessageBase {
  type: "RECEIVER_HEARTBEAT_ACK";
  sessionId: string;
  heartbeatSequence: number;
}

export type SignalClientMessage =
  | CreateSessionMessage
  | ReserveJoinMessage
  | CreateJoinMessage
  | OwnerSdpMessage
  | OwnerCandidateMessage
  | OwnerStopMessage
  | OwnerHeartbeatMessage
  | ReceiverIceStateMessage
  | ReceiverSdpMessage
  | ReceiverCandidateMessage
  | ReceiverHeartbeatMessage;

export interface SignalServerMessage {
  protocolVersion: typeof SIGNAL_PROTOCOL;
  sessionId: string;
  sequence: number;
  type: string;
  [field: string]: unknown;
}

export interface SignalAction {
  kind: "SEND" | "CLOSE";
  connectionId: number;
  message?: SignalServerMessage;
  reason?: string;
}

export interface SignalHandleResult {
  accepted: boolean;
  reason: string;
  binding?: ConnectionBinding;
  actions: SignalAction[];
}

export interface SessionSnapshot {
  sessionId: string;
  phase: SessionPhase;
  createdAtMs: number;
  absoluteDeadlineMs: number;
  phaseDeadlineMs?: number;
  joinExpiresAtMs?: number;
  ownerConnectionId: number;
  ownerGeneration: number;
  receiverConnectionId?: number;
  receiverGeneration?: number;
  ownerCapabilityHash: string;
  joinCapabilityHash?: string;
  offerSeen: boolean;
  answerSeen: boolean;
  restartOfferPending: boolean;
  iceDisconnectedDeadlineMs?: number;
}

interface PeerState {
  connectionId: number;
  generation: number;
  expectedClientSequence: number;
  nextServerSequence: number;
  lastActivityMs: number;
  nextHeartbeatMs: number;
  heartbeatSequence: number;
}

interface SessionRecord {
  sessionId: string;
  phase: SessionPhase;
  createdAtMs: number;
  absoluteDeadlineMs: number;
  phaseDeadlineMs?: number;
  joinExpiresAtMs?: number;
  ownerCapabilityHash: Buffer;
  joinCapabilityHash?: Buffer;
  owner: PeerState;
  receiver?: PeerState;
  offerSeen: boolean;
  answerSeen: boolean;
  restartOfferPending: boolean;
  iceDisconnectedDeadlineMs?: number;
}

export interface SignalingSessionStoreOptions {
  now?: () => number;
  entropy?: (bytes: number) => Buffer;
  hashKey?: Buffer;
  maximumSessions?: number;
}

function exactObject(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function exactKeys(
  value: Record<string, unknown>,
  expected: readonly string[],
): boolean {
  const actual = Object.keys(value).sort();
  const sortedExpected = [...expected].sort();
  return (
    actual.length === sortedExpected.length &&
    actual.every((key, index) => key === sortedExpected[index])
  );
}

function validSequence(value: unknown): value is number {
  return Number.isSafeInteger(value) && Number(value) > 0;
}

function validBoundedText(
  value: unknown,
  maximumBytes: number,
): value is string {
  return (
    typeof value === "string" &&
    value.length > 0 &&
    !value.includes("\0") &&
    Buffer.byteLength(value, "utf8") <= maximumBytes
  );
}

function fieldsForType(type: string): readonly string[] | undefined {
  const base = ["protocolVersion", "sequence", "type"];
  switch (type) {
    case "CREATE_SESSION":
      return base;
    case "RESERVE_JOIN":
      return [...base, "joinCapability", "sessionId"];
    case "CREATE_JOIN":
    case "OWNER_STOP":
    case "OWNER_REVOKE":
      return [...base, "ownerCapability", "sessionId"];
    case "OWNER_ANSWER":
    case "OWNER_ICE_RESTART_ANSWER":
      return [...base, "ownerCapability", "sdp", "sessionId"];
    case "OWNER_ICE_CANDIDATE":
      return [...base, "candidate", "ownerCapability", "sessionId"];
    case "OWNER_HEARTBEAT_ACK":
      return [...base, "heartbeatSequence", "ownerCapability", "sessionId"];
    case "RECEIVER_ICE_STATE":
      return [...base, "iceState", "sessionId"];
    case "RECEIVER_OFFER":
    case "RECEIVER_ICE_RESTART_OFFER":
      return [...base, "sdp", "sessionId"];
    case "RECEIVER_ICE_CANDIDATE":
      return [...base, "candidate", "sessionId"];
    case "RECEIVER_HEARTBEAT_ACK":
      return [...base, "heartbeatSequence", "sessionId"];
    default:
      return undefined;
  }
}

export function parseSignalClientMessage(
  value: unknown,
): SignalClientMessage | undefined {
  if (!exactObject(value) || typeof value.type !== "string") {
    return undefined;
  }
  const fields = fieldsForType(value.type);
  if (
    !fields ||
    !exactKeys(value, fields) ||
    value.protocolVersion !== SIGNAL_PROTOCOL ||
    !validSequence(value.sequence)
  ) {
    return undefined;
  }
  if (
    "sessionId" in value &&
    (typeof value.sessionId !== "string" ||
      !SESSION_PATTERN.test(value.sessionId))
  ) {
    return undefined;
  }
  if (
    "ownerCapability" in value &&
    (typeof value.ownerCapability !== "string" ||
      !CAPABILITY_PATTERN.test(value.ownerCapability))
  ) {
    return undefined;
  }
  if (
    "joinCapability" in value &&
    (typeof value.joinCapability !== "string" ||
      !CAPABILITY_PATTERN.test(value.joinCapability))
  ) {
    return undefined;
  }
  if ("sdp" in value && !validBoundedText(value.sdp, MAXIMUM_SDP_BYTES)) {
    return undefined;
  }
  if (
    "candidate" in value &&
    !validBoundedText(value.candidate, MAXIMUM_CANDIDATE_BYTES)
  ) {
    return undefined;
  }
  if ("heartbeatSequence" in value && !validSequence(value.heartbeatSequence)) {
    return undefined;
  }
  if (
    "iceState" in value &&
    value.iceState !== "connected" &&
    value.iceState !== "disconnected" &&
    value.iceState !== "failed" &&
    value.iceState !== "closed"
  ) {
    return undefined;
  }
  return value as unknown as SignalClientMessage;
}

function digestHex(value: Buffer): string {
  return value.toString("hex");
}

export class SignalingSessionStore {
  readonly #now: () => number;
  readonly #entropy: (bytes: number) => Buffer;
  readonly #hashKey: Buffer;
  readonly #maximumSessions: number;
  readonly #sessions = new Map<string, SessionRecord>();
  #nextGeneration = 1;

  constructor(options: SignalingSessionStoreOptions = {}) {
    this.#now = options.now ?? (() => performance.now());
    this.#entropy = options.entropy ?? randomBytes;
    this.#hashKey = Buffer.from(options.hashKey ?? this.#entropy(32));
    this.#maximumSessions = options.maximumSessions ?? 64;
    if (
      this.#hashKey.length < 32 ||
      !Number.isSafeInteger(this.#maximumSessions) ||
      this.#maximumSessions < 1 ||
      this.#maximumSessions > 1_024
    ) {
      throw new Error("signaling_store_configuration_invalid");
    }
  }

  get size(): number {
    return this.#sessions.size;
  }

  snapshot(sessionId: string): SessionSnapshot | undefined {
    const session = this.#sessions.get(sessionId);
    if (!session) {
      return undefined;
    }
    return {
      sessionId: session.sessionId,
      phase: session.phase,
      createdAtMs: session.createdAtMs,
      absoluteDeadlineMs: session.absoluteDeadlineMs,
      phaseDeadlineMs: session.phaseDeadlineMs,
      joinExpiresAtMs: session.joinExpiresAtMs,
      ownerConnectionId: session.owner.connectionId,
      ownerGeneration: session.owner.generation,
      receiverConnectionId: session.receiver?.connectionId,
      receiverGeneration: session.receiver?.generation,
      ownerCapabilityHash: digestHex(session.ownerCapabilityHash),
      joinCapabilityHash: session.joinCapabilityHash
        ? digestHex(session.joinCapabilityHash)
        : undefined,
      offerSeen: session.offerSeen,
      answerSeen: session.answerSeen,
      restartOfferPending: session.restartOfferPending,
      iceDisconnectedDeadlineMs: session.iceDisconnectedDeadlineMs,
    };
  }

  handle(
    connectionId: number,
    binding: ConnectionBinding | undefined,
    message: SignalClientMessage,
  ): SignalHandleResult {
    if (!binding) {
      if (message.type === "CREATE_SESSION") {
        return this.#createSession(connectionId, message);
      }
      if (message.type === "RESERVE_JOIN") {
        return this.#reserveJoin(connectionId, message);
      }
      return { accepted: false, reason: "AUTHENTICATION_FAILED", actions: [] };
    }
    return binding.role === "OWNER"
      ? this.#handleOwner(binding, message)
      : this.#handleReceiver(binding, message);
  }

  disconnect(
    binding: ConnectionBinding,
    reason = "SIGNALING_CLOSED",
  ): SignalAction[] {
    const session = this.#sessions.get(binding.sessionId);
    if (!session) {
      return [];
    }
    if (binding.role === "OWNER") {
      if (!this.#ownerBindingMatches(session, binding)) {
        return [];
      }
      return this.#revoke(session, reason, false);
    }
    if (!this.#receiverBindingMatches(session, binding)) {
      return [];
    }
    return this.#detachReceiver(session, reason, false);
  }

  tick(): SignalAction[] {
    const now = this.#now();
    const actions: SignalAction[] = [];
    for (const session of [...this.#sessions.values()]) {
      if (now >= session.absoluteDeadlineMs) {
        actions.push(...this.#expire(session, "ABSOLUTE_EXPIRY"));
        continue;
      }
      if (now - session.owner.lastActivityMs >= PEER_ACTIVITY_TIMEOUT_MS) {
        actions.push(...this.#revoke(session, "OWNER_HEARTBEAT_TIMEOUT", true));
        continue;
      }
      if (
        session.phase === "JOIN_OPEN" &&
        session.joinExpiresAtMs !== undefined &&
        now >= session.joinExpiresAtMs
      ) {
        this.#expireJoin(session, now);
      }
      if (
        session.phase === "OWNER_ONLY" &&
        session.phaseDeadlineMs !== undefined &&
        now >= session.phaseDeadlineMs
      ) {
        actions.push(...this.#expire(session, "OWNER_ONLY_EXPIRY"));
        continue;
      }
      if (
        session.phase === "JOIN_RESERVED" &&
        session.phaseDeadlineMs !== undefined &&
        now >= session.phaseDeadlineMs
      ) {
        actions.push(
          ...this.#detachReceiver(session, "JOIN_RESERVATION_TIMEOUT", true),
        );
        continue;
      }
      if (
        session.receiver &&
        now - session.receiver.lastActivityMs >= PEER_ACTIVITY_TIMEOUT_MS
      ) {
        actions.push(
          ...this.#detachReceiver(session, "RECEIVER_HEARTBEAT_TIMEOUT", true),
        );
        continue;
      }
      if (
        session.iceDisconnectedDeadlineMs !== undefined &&
        now >= session.iceDisconnectedDeadlineMs
      ) {
        actions.push(
          ...this.#detachReceiver(session, "ICE_DISCONNECTED_TIMEOUT", true),
        );
        continue;
      }
      actions.push(...this.#heartbeatActions(session, now));
    }
    return actions;
  }

  #createSession(
    connectionId: number,
    message: CreateSessionMessage,
  ): SignalHandleResult {
    if (
      message.sequence !== 1 ||
      this.#sessions.size >= this.#maximumSessions
    ) {
      return {
        accepted: false,
        reason: "SESSION_CREATION_REJECTED",
        actions: [],
      };
    }
    const now = this.#now();
    const sessionId = this.#uniqueSessionId();
    const ownerCapability = this.#capability();
    const generation = this.#generation();
    const session: SessionRecord = {
      sessionId,
      phase: "OWNER_ONLY",
      createdAtMs: now,
      absoluteDeadlineMs: now + ABSOLUTE_SESSION_LIFETIME_MS,
      phaseDeadlineMs: now + OWNER_ONLY_LIFETIME_MS,
      ownerCapabilityHash: this.#hash("OWNER", sessionId, ownerCapability),
      owner: this.#peer(connectionId, generation, 2, now),
      offerSeen: false,
      answerSeen: false,
      restartOfferPending: false,
    };
    this.#sessions.set(sessionId, session);
    const binding = {
      connectionId,
      role: "OWNER",
      sessionId,
      generation,
    } as const;
    const action = this.#send(session, "OWNER", "SESSION_CREATED", {
      ownerCapability,
      absoluteDeadlineMs: session.absoluteDeadlineMs,
    });
    return {
      accepted: true,
      reason: "SESSION_CREATED",
      binding,
      actions: action ? [action] : [],
    };
  }

  #reserveJoin(
    connectionId: number,
    message: ReserveJoinMessage,
  ): SignalHandleResult {
    const session = this.#sessions.get(message.sessionId);
    const now = this.#now();
    if (
      message.sequence !== 1 ||
      !session ||
      session.phase !== "JOIN_OPEN" ||
      session.receiver ||
      !session.joinCapabilityHash ||
      session.joinExpiresAtMs === undefined ||
      now >= session.joinExpiresAtMs ||
      now >= session.absoluteDeadlineMs ||
      (session.phaseDeadlineMs !== undefined &&
        now >= session.phaseDeadlineMs) ||
      !this.#verify(
        session.joinCapabilityHash,
        "JOIN",
        message.sessionId,
        message.joinCapability,
      )
    ) {
      return { accepted: false, reason: "AUTHENTICATION_FAILED", actions: [] };
    }
    session.joinCapabilityHash = undefined;
    session.joinExpiresAtMs = undefined;
    session.phase = "JOIN_RESERVED";
    session.phaseDeadlineMs = Math.min(
      now + JOIN_RESERVATION_LIFETIME_MS,
      session.absoluteDeadlineMs,
    );
    const generation = this.#generation();
    session.receiver = this.#peer(connectionId, generation, 2, now);
    session.offerSeen = false;
    session.answerSeen = false;
    session.restartOfferPending = false;
    const binding = {
      connectionId,
      role: "RECEIVER",
      sessionId: session.sessionId,
      generation,
    } as const;
    const actions = [
      this.#send(session, "RECEIVER", "JOIN_RESERVED", {
        reservationDeadlineMs: session.phaseDeadlineMs,
      }),
      this.#send(session, "OWNER", "RECEIVER_RESERVED", {}),
    ].filter((action): action is SignalAction => action !== undefined);
    return { accepted: true, reason: "JOIN_RESERVED", binding, actions };
  }

  #handleOwner(
    binding: ConnectionBinding,
    message: SignalClientMessage,
  ): SignalHandleResult {
    const session = this.#sessions.get(binding.sessionId);
    if (session) {
      const expiryActions = this.#expireBeforeMessage(session);
      if (expiryActions) {
        return {
          accepted: false,
          reason: "SESSION_EXPIRED",
          actions: expiryActions,
        };
      }
    }
    if (
      !session ||
      !this.#ownerBindingMatches(session, binding) ||
      !("ownerCapability" in message) ||
      message.sessionId !== session.sessionId ||
      !this.#verify(
        session.ownerCapabilityHash,
        "OWNER",
        session.sessionId,
        message.ownerCapability,
      ) ||
      message.sequence !== session.owner.expectedClientSequence
    ) {
      const actions = session
        ? this.#revoke(session, "OWNER_PROTOCOL_FAILURE", true)
        : [];
      return { accepted: false, reason: "AUTHENTICATION_FAILED", actions };
    }
    session.owner.expectedClientSequence += 1;
    session.owner.lastActivityMs = this.#now();
    switch (message.type) {
      case "CREATE_JOIN":
        return this.#createJoin(session);
      case "OWNER_ANSWER":
        return this.#ownerAnswer(session, message, false);
      case "OWNER_ICE_RESTART_ANSWER":
        return this.#ownerAnswer(session, message, true);
      case "OWNER_ICE_CANDIDATE":
        if (!session.receiver || !session.offerSeen) {
          return this.#ownerStateFailure(
            session,
            "OWNER_CANDIDATE_STATE_INVALID",
          );
        }
        return this.#forward(session, "RECEIVER", "OWNER_ICE_CANDIDATE", {
          candidate: message.candidate,
        });
      case "OWNER_HEARTBEAT_ACK":
        return this.#heartbeatAck(session, "OWNER", message.heartbeatSequence);
      case "OWNER_STOP":
      case "OWNER_REVOKE": {
        const actions = this.#revoke(session, message.type, true);
        return { accepted: true, reason: "SESSION_REVOKED", actions };
      }
      default: {
        const actions = this.#revoke(session, "OWNER_ROLE_CONFUSION", true);
        return { accepted: false, reason: "OWNER_MESSAGE_INVALID", actions };
      }
    }
  }

  #handleReceiver(
    binding: ConnectionBinding,
    message: SignalClientMessage,
  ): SignalHandleResult {
    const session = this.#sessions.get(binding.sessionId);
    if (session) {
      const expiryActions = this.#expireBeforeMessage(session);
      if (expiryActions) {
        return {
          accepted: false,
          reason: "SESSION_EXPIRED",
          actions: expiryActions,
        };
      }
    }
    if (
      !session ||
      !this.#receiverBindingMatches(session, binding) ||
      !("sessionId" in message) ||
      "ownerCapability" in message ||
      message.sessionId !== session.sessionId ||
      message.sequence !== session.receiver?.expectedClientSequence
    ) {
      const actions = session
        ? this.#detachReceiver(session, "RECEIVER_PROTOCOL_FAILURE", true)
        : [];
      return { accepted: false, reason: "AUTHENTICATION_FAILED", actions };
    }
    session.receiver.expectedClientSequence += 1;
    session.receiver.lastActivityMs = this.#now();
    switch (message.type) {
      case "RECEIVER_OFFER":
        if (session.phase !== "JOIN_RESERVED" || session.offerSeen) {
          break;
        }
        session.offerSeen = true;
        return this.#forward(session, "OWNER", "RECEIVER_OFFER", {
          sdp: message.sdp,
        });
      case "RECEIVER_ICE_RESTART_OFFER":
        if (session.phase !== "CONNECTED" || session.restartOfferPending) {
          break;
        }
        session.restartOfferPending = true;
        return this.#forward(session, "OWNER", "RECEIVER_ICE_RESTART_OFFER", {
          sdp: message.sdp,
        });
      case "RECEIVER_ICE_CANDIDATE":
        if (!session.offerSeen) {
          break;
        }
        return this.#forward(session, "OWNER", "RECEIVER_ICE_CANDIDATE", {
          candidate: message.candidate,
        });
      case "RECEIVER_HEARTBEAT_ACK":
        return this.#heartbeatAck(
          session,
          "RECEIVER",
          message.heartbeatSequence,
        );
      case "RECEIVER_ICE_STATE":
        return this.#receiverIceState(session, message.iceState);
      default:
        break;
    }
    const actions = this.#detachReceiver(
      session,
      "RECEIVER_ROLE_OR_STATE_INVALID",
      true,
    );
    return { accepted: false, reason: "RECEIVER_MESSAGE_INVALID", actions };
  }

  #createJoin(session: SessionRecord): SignalHandleResult {
    if (
      session.phase !== "OWNER_ONLY" ||
      session.receiver ||
      session.joinCapabilityHash
    ) {
      return this.#ownerStateFailure(session, "CREATE_JOIN_STATE_INVALID");
    }
    const joinCapability = this.#capability();
    session.joinCapabilityHash = this.#hash(
      "JOIN",
      session.sessionId,
      joinCapability,
    );
    session.joinExpiresAtMs = Math.min(
      this.#now() + JOIN_CAPABILITY_LIFETIME_MS,
      session.absoluteDeadlineMs,
    );
    session.phase = "JOIN_OPEN";
    session.phaseDeadlineMs = session.joinExpiresAtMs;
    const action = this.#send(session, "OWNER", "JOIN_CREATED", {
      joinCapability,
      joinExpiresAtMs: session.joinExpiresAtMs,
    });
    return {
      accepted: true,
      reason: "JOIN_CREATED",
      actions: action ? [action] : [],
    };
  }

  #ownerAnswer(
    session: SessionRecord,
    message: OwnerSdpMessage,
    restart: boolean,
  ): SignalHandleResult {
    if (
      !session.receiver ||
      (restart
        ? session.phase !== "CONNECTED" || !session.restartOfferPending
        : session.phase !== "JOIN_RESERVED" ||
          !session.offerSeen ||
          session.answerSeen)
    ) {
      return this.#ownerStateFailure(session, "OWNER_ANSWER_STATE_INVALID");
    }
    if (!restart) {
      session.answerSeen = true;
      session.phase = "CONNECTED";
      session.phaseDeadlineMs = undefined;
    } else {
      session.restartOfferPending = false;
    }
    return this.#forward(
      session,
      "RECEIVER",
      restart ? "OWNER_ICE_RESTART_ANSWER" : "OWNER_ANSWER",
      { sdp: message.sdp },
    );
  }

  #receiverIceState(
    session: SessionRecord,
    iceState: ReceiverIceStateMessage["iceState"],
  ): SignalHandleResult {
    if (!session.receiver || session.phase !== "CONNECTED") {
      return this.#ownerStateFailure(session, "RECEIVER_ICE_STATE_INVALID");
    }
    if (iceState === "connected") {
      session.iceDisconnectedDeadlineMs = undefined;
      return { accepted: true, reason: "ICE_CONNECTED", actions: [] };
    }
    if (iceState === "disconnected") {
      session.iceDisconnectedDeadlineMs = Math.min(
        this.#now() + ICE_DISCONNECTED_GRACE_MS,
        session.absoluteDeadlineMs,
      );
      return { accepted: true, reason: "ICE_DISCONNECTED_GRACE", actions: [] };
    }
    const actions = this.#detachReceiver(
      session,
      `ICE_${iceState.toUpperCase()}`,
      true,
    );
    return { accepted: true, reason: "RECEIVER_DISCONNECTED", actions };
  }

  #heartbeatAck(
    session: SessionRecord,
    role: ConnectionRole,
    heartbeatSequence: number,
  ): SignalHandleResult {
    const peer = role === "OWNER" ? session.owner : session.receiver;
    if (
      !peer ||
      heartbeatSequence !== peer.heartbeatSequence ||
      heartbeatSequence === 0
    ) {
      return role === "OWNER"
        ? this.#ownerStateFailure(session, "HEARTBEAT_ACK_INVALID")
        : {
            accepted: false,
            reason: "HEARTBEAT_ACK_INVALID",
            actions: this.#detachReceiver(
              session,
              "HEARTBEAT_ACK_INVALID",
              true,
            ),
          };
    }
    return { accepted: true, reason: "HEARTBEAT_ACKNOWLEDGED", actions: [] };
  }

  #heartbeatActions(session: SessionRecord, now: number): SignalAction[] {
    const actions: SignalAction[] = [];
    for (const role of ["OWNER", "RECEIVER"] as const) {
      const peer = role === "OWNER" ? session.owner : session.receiver;
      if (!peer || now < peer.nextHeartbeatMs) {
        continue;
      }
      peer.heartbeatSequence += 1;
      peer.nextHeartbeatMs = now + HEARTBEAT_INTERVAL_MS;
      const action = this.#send(session, role, "HEARTBEAT", {
        heartbeatSequence: peer.heartbeatSequence,
        deadlineMs: Math.min(
          now + PEER_ACTIVITY_TIMEOUT_MS,
          session.absoluteDeadlineMs,
        ),
      });
      if (action) {
        actions.push(action);
      }
    }
    return actions;
  }

  #forward(
    session: SessionRecord,
    role: ConnectionRole,
    type: string,
    payload: Record<string, unknown>,
  ): SignalHandleResult {
    const action = this.#send(session, role, type, payload);
    if (!action) {
      return role === "OWNER"
        ? this.#ownerStateFailure(session, "SIGNAL_DESTINATION_MISSING")
        : {
            accepted: false,
            reason: "SIGNAL_DESTINATION_MISSING",
            actions: this.#detachReceiver(
              session,
              "SIGNAL_DESTINATION_MISSING",
              true,
            ),
          };
    }
    return { accepted: true, reason: "SIGNAL_FORWARDED", actions: [action] };
  }

  #ownerStateFailure(
    session: SessionRecord,
    reason: string,
  ): SignalHandleResult {
    return {
      accepted: false,
      reason,
      actions: this.#revoke(session, reason, true),
    };
  }

  #detachReceiver(
    session: SessionRecord,
    reason: string,
    closeReceiver: boolean,
  ): SignalAction[] {
    const actions: SignalAction[] = [];
    const receiver = session.receiver;
    if (receiver && closeReceiver) {
      actions.push({
        kind: "CLOSE",
        connectionId: receiver.connectionId,
        reason,
      });
    }
    session.receiver = undefined;
    session.phase = "OWNER_ONLY";
    session.phaseDeadlineMs = Math.min(
      this.#now() + OWNER_ONLY_LIFETIME_MS,
      session.absoluteDeadlineMs,
    );
    session.joinCapabilityHash = undefined;
    session.joinExpiresAtMs = undefined;
    session.offerSeen = false;
    session.answerSeen = false;
    session.restartOfferPending = false;
    session.iceDisconnectedDeadlineMs = undefined;
    const ownerAction = this.#send(session, "OWNER", "RECEIVER_DISCONNECTED", {
      reason,
    });
    if (ownerAction) {
      actions.push(ownerAction);
    }
    return actions;
  }

  #revoke(
    session: SessionRecord,
    reason: string,
    closeOwner: boolean,
  ): SignalAction[] {
    const actions: SignalAction[] = [];
    if (session.receiver) {
      const receiverAction = this.#send(
        session,
        "RECEIVER",
        "SESSION_REVOKED",
        { reason },
      );
      if (receiverAction) {
        actions.push(receiverAction);
      }
      actions.push({
        kind: "CLOSE",
        connectionId: session.receiver.connectionId,
        reason,
      });
    }
    if (closeOwner) {
      const ownerAction = this.#send(session, "OWNER", "SESSION_REVOKED", {
        reason,
      });
      if (ownerAction) {
        actions.push(ownerAction);
      }
      actions.push({
        kind: "CLOSE",
        connectionId: session.owner.connectionId,
        reason,
      });
    }
    session.phase = "REVOKED";
    session.ownerCapabilityHash.fill(0);
    session.joinCapabilityHash?.fill(0);
    this.#sessions.delete(session.sessionId);
    return actions;
  }

  #expire(session: SessionRecord, reason: string): SignalAction[] {
    const actions: SignalAction[] = [];
    for (const role of ["RECEIVER", "OWNER"] as const) {
      const peer = role === "OWNER" ? session.owner : session.receiver;
      if (!peer) {
        continue;
      }
      const notification = this.#send(session, role, "SESSION_EXPIRED", {
        reason,
      });
      if (notification) {
        actions.push(notification);
      }
      actions.push({ kind: "CLOSE", connectionId: peer.connectionId, reason });
    }
    session.phase = "EXPIRED";
    session.ownerCapabilityHash.fill(0);
    session.joinCapabilityHash?.fill(0);
    this.#sessions.delete(session.sessionId);
    return actions;
  }

  #send(
    session: SessionRecord,
    role: ConnectionRole,
    type: string,
    payload: Record<string, unknown>,
  ): SignalAction | undefined {
    const peer = role === "OWNER" ? session.owner : session.receiver;
    if (!peer) {
      return undefined;
    }
    const message: SignalServerMessage = {
      protocolVersion: SIGNAL_PROTOCOL,
      sessionId: session.sessionId,
      sequence: peer.nextServerSequence++,
      type,
      ...payload,
    };
    return { kind: "SEND", connectionId: peer.connectionId, message };
  }

  #ownerBindingMatches(
    session: SessionRecord,
    binding: ConnectionBinding,
  ): boolean {
    return (
      binding.role === "OWNER" &&
      binding.connectionId === session.owner.connectionId &&
      binding.generation === session.owner.generation &&
      binding.sessionId === session.sessionId
    );
  }

  #receiverBindingMatches(
    session: SessionRecord,
    binding: ConnectionBinding,
  ): boolean {
    return (
      binding.role === "RECEIVER" &&
      binding.connectionId === session.receiver?.connectionId &&
      binding.generation === session.receiver?.generation &&
      binding.sessionId === session.sessionId
    );
  }

  #peer(
    connectionId: number,
    generation: number,
    expectedSequence: number,
    now: number,
  ): PeerState {
    return {
      connectionId,
      generation,
      expectedClientSequence: expectedSequence,
      nextServerSequence: 1,
      lastActivityMs: now,
      nextHeartbeatMs: now + HEARTBEAT_INTERVAL_MS,
      heartbeatSequence: 0,
    };
  }

  #expireBeforeMessage(session: SessionRecord): SignalAction[] | undefined {
    const now = this.#now();
    if (now >= session.absoluteDeadlineMs) {
      return this.#expire(session, "ABSOLUTE_EXPIRY");
    }
    if (now - session.owner.lastActivityMs >= PEER_ACTIVITY_TIMEOUT_MS) {
      return this.#revoke(session, "OWNER_HEARTBEAT_TIMEOUT", true);
    }
    if (
      session.phase === "JOIN_OPEN" &&
      session.joinExpiresAtMs !== undefined &&
      now >= session.joinExpiresAtMs
    ) {
      this.#expireJoin(session, now);
    }
    if (
      session.phase === "OWNER_ONLY" &&
      session.phaseDeadlineMs !== undefined &&
      now >= session.phaseDeadlineMs
    ) {
      return this.#expire(session, "OWNER_ONLY_EXPIRY");
    }
    if (
      session.phase === "JOIN_RESERVED" &&
      session.phaseDeadlineMs !== undefined &&
      now >= session.phaseDeadlineMs
    ) {
      return this.#detachReceiver(session, "JOIN_RESERVATION_TIMEOUT", true);
    }
    if (
      session.receiver &&
      now - session.receiver.lastActivityMs >= PEER_ACTIVITY_TIMEOUT_MS
    ) {
      return this.#detachReceiver(session, "RECEIVER_HEARTBEAT_TIMEOUT", true);
    }
    if (
      session.iceDisconnectedDeadlineMs !== undefined &&
      now >= session.iceDisconnectedDeadlineMs
    ) {
      return this.#detachReceiver(session, "ICE_DISCONNECTED_TIMEOUT", true);
    }
    return undefined;
  }

  #expireJoin(session: SessionRecord, now: number): void {
    session.joinCapabilityHash?.fill(0);
    session.joinCapabilityHash = undefined;
    session.joinExpiresAtMs = undefined;
    session.phase = "OWNER_ONLY";
    session.phaseDeadlineMs = Math.min(
      now + OWNER_ONLY_LIFETIME_MS,
      session.absoluteDeadlineMs,
    );
  }

  #generation(): number {
    if (this.#nextGeneration >= Number.MAX_SAFE_INTEGER) {
      throw new Error("signaling_generation_exhausted");
    }
    return this.#nextGeneration++;
  }

  #uniqueSessionId(): string {
    for (let attempt = 0; attempt < 16; attempt += 1) {
      const sessionId = this.#entropy(16).toString("base64url");
      if (SESSION_PATTERN.test(sessionId) && !this.#sessions.has(sessionId)) {
        return sessionId;
      }
    }
    throw new Error("session_identifier_generation_failed");
  }

  #capability(): string {
    const capability = this.#entropy(32).toString("base64url");
    if (!CAPABILITY_PATTERN.test(capability)) {
      throw new Error("capability_generation_failed");
    }
    return capability;
  }

  #hash(role: "OWNER" | "JOIN", sessionId: string, capability: string): Buffer {
    return createHmac("sha256", this.#hashKey)
      .update(SIGNAL_PROTOCOL)
      .update("\0")
      .update(role)
      .update("\0")
      .update(sessionId)
      .update("\0")
      .update(capability)
      .digest();
  }

  #verify(
    expected: Buffer,
    role: "OWNER" | "JOIN",
    sessionId: string,
    capability: string,
  ): boolean {
    const actual = this.#hash(role, sessionId, capability);
    return (
      actual.length === expected.length && timingSafeEqual(actual, expected)
    );
  }
}

export function deterministicEntropy(
  seed: BinaryLike,
): (bytes: number) => Buffer {
  let counter = 0;
  return (bytes: number) => {
    const chunks: Buffer[] = [];
    let length = 0;
    while (length < bytes) {
      const chunk = createHmac("sha256", seed)
        .update(String(counter++))
        .digest();
      chunks.push(chunk);
      length += chunk.length;
    }
    return Buffer.concat(chunks).subarray(0, bytes);
  };
}
