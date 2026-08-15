import assert from "node:assert/strict";
import test from "node:test";

import {
  CONTROL_PROTOCOL,
  decodeReceiverControlMessage,
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
      encoded("SESSION_RESUMED", 4, {
        dependencyEpoch: 8,
        mediaEpoch: 2,
      }),
      sessionId,
      4,
    )?.type,
    "SESSION_RESUMED",
  );
  assert.equal(
    decodeSenderControlMessage(
      encoded("PROTOCOL_ERROR", 5, { code: "SEQUENCE_INVALID" }),
      sessionId,
      5,
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
      encoded("SESSION_RESUMED", 1, { mediaEpoch: 2 }),
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

test("validates the exact receiver-to-sender control vocabulary", () => {
  assert.equal(
    decodeReceiverControlMessage(
      encoded("CLOCK_RESPONSE", 1, {
        receiverReceiveTimeMs: 10,
        receiverSendTimeMs: 11,
        requestSequence: 2,
        senderSendTimeMs: 1,
      }),
      sessionId,
      1,
    )?.type,
    "CLOCK_RESPONSE",
  );
  assert.equal(
    decodeReceiverControlMessage(
      encoded("RECEIVER_STATS", 2, {
        compositorFrames: 30,
        decodedFrames: 31,
        droppedFrames: 1,
        latestCallbackTimeMs: 20,
        latestCaptureTimeMs: 12,
        latestExpectedDisplayTimeMs: 21,
        latestPresentationTimeMs: 19,
        latestPresentedRtpTimestamp: 0xfffffff0,
        latestReceiveTimeMs: 14,
      }),
      sessionId,
      2,
    )?.type,
    "RECEIVER_STATS",
  );
  assert.equal(
    decodeReceiverControlMessage(
      encoded("SESSION_PAUSED_ACK", 3, { requestSequence: 7 }),
      sessionId,
      3,
    )?.type,
    "SESSION_PAUSED_ACK",
  );
});

test("rejects receiver control commands, unknown fields, and invalid telemetry", () => {
  assert.equal(
    decodeReceiverControlMessage(
      encoded("KEYBOARD_INPUT", 1, { key: "Enter" }),
      sessionId,
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeReceiverControlMessage(
      encoded("SESSION_ENDED_ACK", 1, {
        requestSequence: 2,
        unknown: true,
      }),
      sessionId,
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeReceiverControlMessage(
      encoded("RECEIVER_STATS", 1, {
        compositorFrames: -1,
        decodedFrames: 0,
        droppedFrames: 0,
        latestCallbackTimeMs: null,
        latestCaptureTimeMs: null,
        latestExpectedDisplayTimeMs: null,
        latestPresentationTimeMs: null,
        latestPresentedRtpTimestamp: null,
        latestReceiveTimeMs: null,
      }),
      sessionId,
      1,
    ),
    undefined,
  );
  assert.equal(
    decodeReceiverControlMessage(
      encoded("RECEIVER_STATS", 1, {
        compositorFrames: 1,
        decodedFrames: 1,
        droppedFrames: 0,
        latestCallbackTimeMs: 20,
        latestCaptureTimeMs: null,
        latestExpectedDisplayTimeMs: -1,
        latestPresentationTimeMs: 19,
        latestPresentedRtpTimestamp: 1,
        latestReceiveTimeMs: 14,
      }),
      sessionId,
      1,
    ),
    undefined,
  );
});
