import assert from "node:assert/strict";
import test from "node:test";

import {
  CONTROL_PROTOCOL,
  decodeSenderControlMessage,
  MAXIMUM_CONTROL_BYTES,
  SlidingControlRateLimit,
} from "../../receiver/control-protocol.js";

const sessionId = "A".repeat(22);

function encoded(type, sequence, fields = {}) {
  return JSON.stringify({
    protocolVersion: CONTROL_PROTOCOL,
    sequence,
    sessionId,
    type,
    ...fields,
  });
}

test("accepts only the bounded sender-to-receiver control vocabulary", () => {
  assert.equal(
    decodeSenderControlMessage(
      encoded("HELLO", 1, { mediaEpoch: 0 }),
      sessionId,
      1,
    )?.type,
    "HELLO",
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("CLOCK_REQUEST", 2, { senderSendTimeMs: 12.5 }),
      sessionId,
      2,
    )?.type,
    "CLOCK_REQUEST",
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("SESSION_ENDED", 3, {
        mediaEpoch: 4,
        reason: "OWNER_STOP",
      }),
      sessionId,
      3,
    )?.type,
    "SESSION_ENDED",
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("PROTOCOL_ERROR", 4, { code: "SEQUENCE_INVALID" }),
      sessionId,
      4,
    )?.type,
    "PROTOCOL_ERROR",
  );
});

test("rejects stale sequences, wrong sessions, unknown fields, commands, and invalid numbers", () => {
  assert.equal(
    decodeSenderControlMessage(
      encoded("HELLO", 1, { mediaEpoch: 0 }),
      sessionId,
      2,
    ),
    undefined,
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("HELLO", 1, { mediaEpoch: 0 }),
      "B".repeat(22),
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("HELLO", 1, { mediaEpoch: 0, unknown: true }),
      sessionId,
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("KEYBOARD_INPUT", 1, { key: "Enter" }),
      sessionId,
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("CLOCK_REQUEST", 1, { senderSendTimeMs: -1 }),
      sessionId,
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("SESSION_ENDED", 1, { mediaEpoch: 1, reason: "bad reason" }),
      sessionId,
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeSenderControlMessage(
      "x".repeat(MAXIMUM_CONTROL_BYTES + 1),
      sessionId,
      1,
    ),
    undefined,
  );
});

test("control flood limiter admits ten messages per rolling second and rejects the next", () => {
  const limiter = new SlidingControlRateLimit();
  for (let index = 0; index < 10; index += 1) {
    assert.equal(limiter.admit(index), true);
  }
  assert.equal(limiter.admit(999), false);
  assert.equal(limiter.admit(1_000), true);
  assert.equal(limiter.admit(Number.NaN), false);
});
