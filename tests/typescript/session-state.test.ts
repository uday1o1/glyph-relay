import assert from "node:assert/strict";
import test from "node:test";

import {
  ABSOLUTE_SESSION_LIFETIME_MS,
  deterministicEntropy,
  HEARTBEAT_INTERVAL_MS,
  ICE_DISCONNECTED_GRACE_MS,
  JOIN_RESERVATION_LIFETIME_MS,
  OWNER_ONLY_LIFETIME_MS,
  parseSignalClientMessage,
  PEER_ACTIVITY_TIMEOUT_MS,
  SIGNAL_PROTOCOL,
  SignalingSessionStore,
  type ConnectionBinding,
  type SignalAction,
  type SignalClientMessage,
  type SignalHandleResult,
  type SignalServerMessage,
} from "../../signaling/session-state.ts";

function message(value: Record<string, unknown>): SignalClientMessage {
  const parsed = parseSignalClientMessage({
    protocolVersion: SIGNAL_PROTOCOL,
    ...value,
  });
  assert.ok(parsed, `message did not parse: ${JSON.stringify(value)}`);
  return parsed;
}

function sent(
  actions: readonly SignalAction[],
  type: string,
): SignalServerMessage {
  const action = actions.find(
    (candidate) =>
      candidate.kind === "SEND" && candidate.message?.type === type,
  );
  assert.ok(action?.message, `${type} action missing`);
  return action.message;
}

function stringField(value: SignalServerMessage, field: string): string {
  const result = value[field];
  if (typeof result !== "string") {
    throw new Error(`${field} string field missing`);
  }
  return result;
}

class Harness {
  readonly clock = { now: 0 };
  readonly store = new SignalingSessionStore({
    entropy: deterministicEntropy("session-state-tests"),
    hashKey: Buffer.alloc(32, 0x5a),
    now: () => this.clock.now,
  });
  ownerBinding: ConnectionBinding;
  ownerCapability: string;
  ownerSequence = 1;
  receiverBinding?: ConnectionBinding;
  receiverSequence = 0;
  sessionId: string;

  constructor() {
    const created = this.store.handle(
      11,
      undefined,
      message({ sequence: 1, type: "CREATE_SESSION" }),
    );
    assert.equal(created.accepted, true);
    assert.ok(created.binding);
    this.ownerBinding = created.binding;
    const response = sent(created.actions, "SESSION_CREATED");
    this.sessionId = response.sessionId;
    this.ownerCapability = stringField(response, "ownerCapability");
  }

  owner(
    type: string,
    fields: Record<string, unknown> = {},
  ): SignalHandleResult {
    this.ownerSequence += 1;
    return this.store.handle(
      this.ownerBinding.connectionId,
      this.ownerBinding,
      message({
        ownerCapability: this.ownerCapability,
        sequence: this.ownerSequence,
        sessionId: this.sessionId,
        type,
        ...fields,
      }),
    );
  }

  openJoin(): string {
    const result = this.owner("CREATE_JOIN");
    assert.equal(result.accepted, true);
    return stringField(sent(result.actions, "JOIN_CREATED"), "joinCapability");
  }

  reserve(joinCapability: string, connectionId = 22): SignalHandleResult {
    const result = this.store.handle(
      connectionId,
      undefined,
      message({
        joinCapability,
        sequence: 1,
        sessionId: this.sessionId,
        type: "RESERVE_JOIN",
      }),
    );
    if (result.binding) {
      this.receiverBinding = result.binding;
      this.receiverSequence = 1;
    }
    return result;
  }

  receiver(
    type: string,
    fields: Record<string, unknown> = {},
  ): SignalHandleResult {
    assert.ok(this.receiverBinding);
    this.receiverSequence += 1;
    return this.store.handle(
      this.receiverBinding.connectionId,
      this.receiverBinding,
      message({
        sequence: this.receiverSequence,
        sessionId: this.sessionId,
        type,
        ...fields,
      }),
    );
  }

  connect(): void {
    const joinCapability = this.openJoin();
    assert.equal(this.reserve(joinCapability).accepted, true);
    assert.equal(
      this.receiver("RECEIVER_OFFER", { sdp: "v=0\r\n" }).accepted,
      true,
    );
    assert.equal(this.owner("OWNER_ANSWER", { sdp: "v=0\r\n" }).accepted, true);
  }

  tickAndAcknowledge(): SignalAction[] {
    const actions = this.store.tick();
    for (const action of actions) {
      if (action.kind !== "SEND" || action.message?.type !== "HEARTBEAT") {
        continue;
      }
      const heartbeatSequence = action.message.heartbeatSequence;
      assert.equal(typeof heartbeatSequence, "number");
      if (action.connectionId === this.ownerBinding.connectionId) {
        const result = this.owner("OWNER_HEARTBEAT_ACK", {
          heartbeatSequence,
        });
        assert.equal(result.accepted, true);
      } else if (
        this.receiverBinding &&
        action.connectionId === this.receiverBinding.connectionId
      ) {
        const result = this.receiver("RECEIVER_HEARTBEAT_ACK", {
          heartbeatSequence,
        });
        assert.equal(result.accepted, true);
      }
    }
    return actions;
  }

  keepAliveUntil(deadline: number): SignalAction[] {
    let latest: SignalAction[] = [];
    while (this.clock.now < deadline) {
      this.clock.now = Math.min(
        deadline,
        this.clock.now + HEARTBEAT_INTERVAL_MS,
      );
      latest = this.tickAndAcknowledge();
      if (!this.store.snapshot(this.sessionId)) {
        break;
      }
    }
    return latest;
  }
}

test("strict signaling parser rejects unknown fields, invalid bounds, and role confusion shapes", () => {
  assert.equal(
    parseSignalClientMessage({
      protocolVersion: SIGNAL_PROTOCOL,
      sequence: 1,
      type: "CREATE_SESSION",
      unknown: true,
    }),
    undefined,
  );
  assert.equal(
    parseSignalClientMessage({
      protocolVersion: SIGNAL_PROTOCOL,
      sequence: 0,
      type: "CREATE_SESSION",
    }),
    undefined,
  );
  assert.equal(
    parseSignalClientMessage({
      protocolVersion: SIGNAL_PROTOCOL,
      sequence: 1,
      sessionId: "x".repeat(22),
      sdp: "x".repeat(64 * 1024 + 1),
      type: "RECEIVER_OFFER",
    }),
    undefined,
  );
  assert.equal(
    parseSignalClientMessage({
      protocolVersion: SIGNAL_PROTOCOL,
      sequence: 1,
      sessionId: "x".repeat(22),
      type: "OWNER_STOP",
    }),
    undefined,
  );
});

test("capabilities are independent, hashed in state, role-bound, and single-use", () => {
  const harness = new Harness();
  const initial = harness.store.snapshot(harness.sessionId);
  assert.equal(initial?.phase, "OWNER_ONLY");
  assert.ok(initial?.ownerCapabilityHash);
  assert.doesNotMatch(
    initial?.ownerCapabilityHash ?? "",
    new RegExp(harness.ownerCapability),
  );
  assert.equal(initial?.joinCapabilityHash, undefined);

  const joinCapability = harness.openJoin();
  assert.notEqual(joinCapability, harness.ownerCapability);
  const opened = harness.store.snapshot(harness.sessionId);
  assert.equal(opened?.phase, "JOIN_OPEN");
  assert.ok(opened?.joinCapabilityHash);
  assert.doesNotMatch(
    opened?.joinCapabilityHash ?? "",
    new RegExp(joinCapability),
  );

  const ownerAsJoin = harness.reserve(harness.ownerCapability, 31);
  assert.equal(ownerAsJoin.accepted, false);
  assert.equal(harness.store.snapshot(harness.sessionId)?.phase, "JOIN_OPEN");
  const fixedSession = `${harness.sessionId.slice(0, -1)}${
    harness.sessionId.endsWith("A") ? "B" : "A"
  }`;
  assert.equal(
    harness.store.handle(
      32,
      undefined,
      message({
        joinCapability,
        sequence: 1,
        sessionId: fixedSession,
        type: "RESERVE_JOIN",
      }),
    ).accepted,
    false,
  );
  assert.equal(
    harness.store.handle(
      55,
      undefined,
      message({
        ownerCapability: harness.ownerCapability,
        sequence: 3,
        sessionId: harness.sessionId,
        type: "OWNER_STOP",
      }),
    ).accepted,
    false,
  );
  assert.equal(harness.store.snapshot(harness.sessionId)?.phase, "JOIN_OPEN");
  const joined = harness.reserve(joinCapability);
  assert.equal(joined.accepted, true);
  assert.equal(
    harness.store.snapshot(harness.sessionId)?.phase,
    "JOIN_RESERVED",
  );
  assert.equal(harness.reserve(joinCapability, 33).accepted, false);
});

test("offer, answer, ICE restart, and receiver cleanup follow the transition table", () => {
  const harness = new Harness();
  harness.connect();
  assert.equal(harness.store.snapshot(harness.sessionId)?.phase, "CONNECTED");

  assert.equal(
    harness.receiver("RECEIVER_ICE_RESTART_OFFER", {
      sdp: "v=0\r\na=ice-ufrag:new\r\n",
    }).accepted,
    true,
  );
  assert.equal(
    harness.owner("OWNER_ICE_RESTART_ANSWER", {
      sdp: "v=0\r\na=ice-ufrag:answer\r\n",
    }).accepted,
    true,
  );
  assert.equal(
    harness.store.snapshot(harness.sessionId)?.restartOfferPending,
    false,
  );

  assert.ok(harness.receiverBinding);
  const oldReceiver = harness.receiverBinding;
  const cleanup = harness.store.disconnect(oldReceiver);
  assert.equal(
    sent(cleanup, "RECEIVER_DISCONNECTED").sessionId,
    harness.sessionId,
  );
  assert.equal(harness.store.snapshot(harness.sessionId)?.phase, "OWNER_ONLY");
  assert.deepEqual(harness.store.disconnect(oldReceiver), []);
  const replacement = harness.openJoin();
  assert.notEqual(replacement.length, 0);
  assert.equal(harness.reserve(replacement, 44).accepted, true);
  assert.notEqual(harness.receiverBinding?.generation, oldReceiver.generation);
});

test("forged receiver owner action detaches only the receiver while owner failures revoke", () => {
  const harness = new Harness();
  const joinCapability = harness.openJoin();
  harness.reserve(joinCapability);
  assert.ok(harness.receiverBinding);
  const forged = harness.store.handle(
    harness.receiverBinding.connectionId,
    harness.receiverBinding,
    message({
      ownerCapability: "A".repeat(43),
      sequence: 2,
      sessionId: harness.sessionId,
      type: "OWNER_REVOKE",
    }),
  );
  assert.equal(forged.accepted, false);
  assert.equal(harness.store.snapshot(harness.sessionId)?.phase, "OWNER_ONLY");

  const joinAsOwner = harness.store.handle(
    harness.ownerBinding.connectionId,
    harness.ownerBinding,
    message({
      ownerCapability: joinCapability,
      sequence: 3,
      sessionId: harness.sessionId,
      type: "CREATE_JOIN",
    }),
  );
  assert.equal(joinAsOwner.accepted, false);
  assert.equal(harness.store.snapshot(harness.sessionId), undefined);
  assert.ok(joinAsOwner.actions.some((action) => action.kind === "CLOSE"));
});

test("connection identity and generation fence stale owners and disconnect is idempotent", () => {
  const harness = new Harness();
  const stale = { ...harness.ownerBinding, connectionId: 99 };
  const rejected = harness.store.handle(
    99,
    stale,
    message({
      ownerCapability: harness.ownerCapability,
      sequence: 2,
      sessionId: harness.sessionId,
      type: "CREATE_JOIN",
    }),
  );
  assert.equal(rejected.accepted, false);
  assert.equal(harness.store.snapshot(harness.sessionId), undefined);
  assert.deepEqual(harness.store.disconnect(harness.ownerBinding), []);
  assert.equal(
    harness.store.handle(
      77,
      undefined,
      message({
        ownerCapability: harness.ownerCapability,
        sequence: 2,
        sessionId: harness.sessionId,
        type: "CREATE_JOIN",
      }),
    ).accepted,
    false,
  );
});

test("owner and receiver activity deadlines fail closed at exactly five seconds", () => {
  const owner = new Harness();
  owner.clock.now = PEER_ACTIVITY_TIMEOUT_MS - 1;
  owner.store.tick();
  assert.ok(owner.store.snapshot(owner.sessionId));
  owner.clock.now = PEER_ACTIVITY_TIMEOUT_MS;
  const ownerActions = owner.store.tick();
  assert.equal(owner.store.snapshot(owner.sessionId), undefined);
  assert.equal(
    sent(ownerActions, "SESSION_REVOKED").reason,
    "OWNER_HEARTBEAT_TIMEOUT",
  );

  const receiver = new Harness();
  receiver.connect();
  assert.ok(receiver.receiverBinding);
  const receiverBinding = receiver.receiverBinding;
  receiver.clock.now = PEER_ACTIVITY_TIMEOUT_MS - 1;
  assert.equal(
    receiver.owner("OWNER_ICE_CANDIDATE", {
      candidate: '{"candidate":"candidate:owner"}',
    }).accepted,
    true,
  );
  receiver.clock.now = PEER_ACTIVITY_TIMEOUT_MS;
  const receiverActions = receiver.store.tick();
  assert.equal(
    receiver.store.snapshot(receiver.sessionId)?.phase,
    "OWNER_ONLY",
  );
  assert.ok(
    receiverActions.some(
      (action) =>
        action.kind === "CLOSE" &&
        action.connectionId === receiverBinding.connectionId,
    ),
  );
});

test("join, reservation, ICE, owner-only, and absolute deadlines are exact and bounded", () => {
  const join = new Harness();
  join.openJoin();
  const joinDeadline = join.store.snapshot(join.sessionId)?.joinExpiresAtMs;
  if (typeof joinDeadline !== "number") {
    throw new Error("join deadline missing");
  }
  join.keepAliveUntil(joinDeadline);
  assert.equal(join.store.snapshot(join.sessionId)?.phase, "OWNER_ONLY");
  assert.equal(
    join.store.snapshot(join.sessionId)?.joinCapabilityHash,
    undefined,
  );

  const reservation = new Harness();
  reservation.reserve(reservation.openJoin());
  const reservationDeadline =
    reservation.clock.now + JOIN_RESERVATION_LIFETIME_MS;
  reservation.keepAliveUntil(reservationDeadline - 1);
  assert.equal(
    reservation.store.snapshot(reservation.sessionId)?.phase,
    "JOIN_RESERVED",
  );
  reservation.keepAliveUntil(reservationDeadline);
  assert.equal(
    reservation.store.snapshot(reservation.sessionId)?.phase,
    "OWNER_ONLY",
  );

  const ice = new Harness();
  ice.connect();
  assert.equal(
    ice.receiver("RECEIVER_ICE_STATE", { iceState: "disconnected" }).accepted,
    true,
  );
  const iceDeadline = ice.clock.now + ICE_DISCONNECTED_GRACE_MS;
  ice.keepAliveUntil(iceDeadline - 1);
  assert.equal(ice.store.snapshot(ice.sessionId)?.phase, "CONNECTED");
  ice.keepAliveUntil(iceDeadline);
  assert.equal(ice.store.snapshot(ice.sessionId)?.phase, "OWNER_ONLY");

  const ownerOnly = new Harness();
  ownerOnly.keepAliveUntil(OWNER_ONLY_LIFETIME_MS - 1);
  assert.ok(ownerOnly.store.snapshot(ownerOnly.sessionId));
  const ownerOnlyActions = ownerOnly.keepAliveUntil(OWNER_ONLY_LIFETIME_MS);
  assert.equal(ownerOnly.store.snapshot(ownerOnly.sessionId), undefined);
  assert.equal(
    sent(ownerOnlyActions, "SESSION_EXPIRED").reason,
    "OWNER_ONLY_EXPIRY",
  );

  const absolute = new Harness();
  absolute.connect();
  const fixedDeadline = absolute.store.snapshot(
    absolute.sessionId,
  )?.absoluteDeadlineMs;
  assert.equal(fixedDeadline, ABSOLUTE_SESSION_LIFETIME_MS);
  absolute.keepAliveUntil(ABSOLUTE_SESSION_LIFETIME_MS - 1);
  assert.equal(
    absolute.store.snapshot(absolute.sessionId)?.absoluteDeadlineMs,
    fixedDeadline,
  );
  const absoluteActions = absolute.keepAliveUntil(ABSOLUTE_SESSION_LIFETIME_MS);
  assert.equal(absolute.store.snapshot(absolute.sessionId), undefined);
  assert.equal(
    sent(absoluteActions, "SESSION_EXPIRED").reason,
    "ABSOLUTE_EXPIRY",
  );
});
