export const CONTROL_PROTOCOL = "glyphrelay-control-v1";
export const MAXIMUM_CONTROL_BYTES = 4 * 1024;
export const MAXIMUM_CONTROL_MESSAGES_PER_SECOND = 10;

const SESSION_PATTERN = /^[A-Za-z0-9_-]{22}$/;
const REASON_PATTERN = /^[A-Z0-9_]{1,64}$/;

function exactObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function exactKeys(value, expected) {
  const actual = Object.keys(value).sort();
  const sortedExpected = [...expected].sort();
  return (
    actual.length === sortedExpected.length &&
    actual.every((key, index) => key === sortedExpected[index])
  );
}

function validSequence(value) {
  return Number.isSafeInteger(value) && value > 0;
}

function validTime(value) {
  return Number.isFinite(value) && value >= 0;
}

function validEpoch(value) {
  return Number.isSafeInteger(value) && value >= 0;
}

function fieldsForType(type) {
  const base = ["protocolVersion", "sequence", "sessionId", "type"];
  switch (type) {
    case "HELLO":
    case "SESSION_PAUSED":
      return [...base, "mediaEpoch"];
    case "SESSION_RESUMED":
      return [...base, "dependencyEpoch", "mediaEpoch"];
    case "CLOCK_REQUEST":
      return [...base, "senderSendTimeMs"];
    case "SESSION_ENDED":
      return [...base, "mediaEpoch", "reason"];
    case "PROTOCOL_ERROR":
      return [...base, "code"];
    default:
      return undefined;
  }
}

export function decodeSenderControlMessage(
  encoded,
  expectedSessionId,
  expectedSequence,
) {
  if (
    typeof encoded !== "string" ||
    new TextEncoder().encode(encoded).length > MAXIMUM_CONTROL_BYTES
  ) {
    return undefined;
  }
  let value;
  try {
    value = JSON.parse(encoded);
  } catch {
    return undefined;
  }
  if (!exactObject(value) || typeof value.type !== "string") {
    return undefined;
  }
  const fields = fieldsForType(value.type);
  if (
    !fields ||
    !exactKeys(value, fields) ||
    value.protocolVersion !== CONTROL_PROTOCOL ||
    value.sessionId !== expectedSessionId ||
    !SESSION_PATTERN.test(value.sessionId) ||
    !validSequence(value.sequence) ||
    value.sequence !== expectedSequence
  ) {
    return undefined;
  }
  if ("mediaEpoch" in value && !validEpoch(value.mediaEpoch)) {
    return undefined;
  }
  if ("dependencyEpoch" in value && !validEpoch(value.dependencyEpoch)) {
    return undefined;
  }
  if (
    value.type === "SESSION_RESUMED" &&
    (value.mediaEpoch < 1 || value.dependencyEpoch < 1)
  ) {
    return undefined;
  }
  if ("senderSendTimeMs" in value && !validTime(value.senderSendTimeMs)) {
    return undefined;
  }
  if (
    "reason" in value &&
    (typeof value.reason !== "string" || !REASON_PATTERN.test(value.reason))
  ) {
    return undefined;
  }
  if (
    "code" in value &&
    (typeof value.code !== "string" || !REASON_PATTERN.test(value.code))
  ) {
    return undefined;
  }
  return value;
}

function receiverFieldsForType(type) {
  const base = ["protocolVersion", "sequence", "sessionId", "type"];
  switch (type) {
    case "CLOCK_RESPONSE":
      return [
        ...base,
        "receiverReceiveTimeMs",
        "receiverSendTimeMs",
        "requestSequence",
        "senderSendTimeMs",
      ];
    case "RECEIVER_STATS":
      return [
        ...base,
        "compositorFrames",
        "decodedFrames",
        "droppedFrames",
        "latestCallbackTimeMs",
        "latestCaptureTimeMs",
        "latestExpectedDisplayTimeMs",
        "latestPresentationTimeMs",
        "latestPresentedRtpTimestamp",
        "latestReceiveTimeMs",
      ];
    case "SESSION_PAUSED_ACK":
    case "SESSION_RESUMED_ACK":
    case "SESSION_ENDED_ACK":
      return [...base, "requestSequence"];
    case "PROTOCOL_ERROR":
      return [...base, "code"];
    default:
      return undefined;
  }
}

export function decodeReceiverControlMessage(
  encoded,
  expectedSessionId,
  expectedSequence,
) {
  if (
    typeof encoded !== "string" ||
    new TextEncoder().encode(encoded).length > MAXIMUM_CONTROL_BYTES
  ) {
    return undefined;
  }
  let value;
  try {
    value = JSON.parse(encoded);
  } catch {
    return undefined;
  }
  if (!exactObject(value) || typeof value.type !== "string") {
    return undefined;
  }
  const fields = receiverFieldsForType(value.type);
  if (
    !fields ||
    !exactKeys(value, fields) ||
    value.protocolVersion !== CONTROL_PROTOCOL ||
    value.sessionId !== expectedSessionId ||
    !SESSION_PATTERN.test(value.sessionId) ||
    !validSequence(value.sequence) ||
    value.sequence !== expectedSequence
  ) {
    return undefined;
  }
  if ("requestSequence" in value && !validSequence(value.requestSequence)) {
    return undefined;
  }
  if ("senderSendTimeMs" in value && !validTime(value.senderSendTimeMs)) {
    return undefined;
  }
  if (
    "receiverReceiveTimeMs" in value &&
    !validTime(value.receiverReceiveTimeMs)
  ) {
    return undefined;
  }
  if (
    "receiverSendTimeMs" in value &&
    (!validTime(value.receiverSendTimeMs) ||
      value.receiverSendTimeMs < value.receiverReceiveTimeMs)
  ) {
    return undefined;
  }
  for (const field of ["compositorFrames", "decodedFrames", "droppedFrames"]) {
    if (
      field in value &&
      (!Number.isSafeInteger(value[field]) || value[field] < 0)
    ) {
      return undefined;
    }
  }
  if (
    "latestPresentedRtpTimestamp" in value &&
    value.latestPresentedRtpTimestamp !== null &&
    (!Number.isSafeInteger(value.latestPresentedRtpTimestamp) ||
      value.latestPresentedRtpTimestamp < 0 ||
      value.latestPresentedRtpTimestamp > 0xffffffff)
  ) {
    return undefined;
  }
  for (const field of [
    "latestCallbackTimeMs",
    "latestCaptureTimeMs",
    "latestExpectedDisplayTimeMs",
    "latestPresentationTimeMs",
    "latestReceiveTimeMs",
  ]) {
    if (field in value && value[field] !== null && !validTime(value[field])) {
      return undefined;
    }
  }
  if (
    "code" in value &&
    (typeof value.code !== "string" || !REASON_PATTERN.test(value.code))
  ) {
    return undefined;
  }
  return value;
}

export class SlidingControlRateLimit {
  #maximum;
  #times = [];

  constructor(maximum = MAXIMUM_CONTROL_MESSAGES_PER_SECOND) {
    if (!Number.isSafeInteger(maximum) || maximum < 1 || maximum > 100) {
      throw new Error("control_rate_limit_invalid");
    }
    this.#maximum = maximum;
  }

  admit(now) {
    if (!validTime(now)) {
      return false;
    }
    this.#times = this.#times.filter((seenAt) => now - seenAt < 1_000);
    if (this.#times.length >= this.#maximum) {
      return false;
    }
    this.#times.push(now);
    return true;
  }
}
