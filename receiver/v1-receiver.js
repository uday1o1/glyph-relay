import {
  CONTROL_PROTOCOL,
  decodeSenderControlMessage,
  MAXIMUM_CONTROL_BYTES,
  SlidingControlRateLimit,
} from "/control-protocol.js";

const SIGNAL_PROTOCOL = "glyphrelay-signal-v1";

const statusElement = document.querySelector("#status");
const connectionElement = document.querySelector("#connection");
const framesElement = document.querySelector("#frames");
const timestampElement = document.querySelector("#timestamp");
const video = document.querySelector("#video");
const shell = document.querySelector(".video-shell");

if (
  !(statusElement instanceof HTMLElement) ||
  !(connectionElement instanceof HTMLElement) ||
  !(framesElement instanceof HTMLElement) ||
  !(timestampElement instanceof HTMLElement) ||
  !(video instanceof HTMLVideoElement) ||
  !(shell instanceof HTMLElement)
) {
  throw new Error("receiver_document_invalid");
}

let signalSocket;
let peerConnection;
let controlChannel;
let sessionId;
let joinCapability;
let clientSequence = 0;
let serverSequence = 0;
let controlSendSequence = 0;
let controlReceiveSequence = 0;
let presentedFrames = 0;
let lastStatsSentAt = Number.NEGATIVE_INFINITY;
let offerSent = false;
let ended = false;
let pendingCandidates = [];
const controlRateLimit = new SlidingControlRateLimit();
const hasInitialJoin = consumeInitialFragment();

window.__glyphrelayReceiver = {
  error: null,
  state: "READY",
  snapshot: receiverSnapshot,
};

function receiverSnapshot() {
  return {
    error: window.__glyphrelayReceiver.error,
    presentedFrames,
    sessionId,
    signalingState: signalSocket?.readyState ?? WebSocket.CLOSED,
    state: window.__glyphrelayReceiver.state,
    videoHeight: video.videoHeight,
    videoWidth: video.videoWidth,
  };
}

function updateStatus(state, message, error = null) {
  window.__glyphrelayReceiver.state = state;
  window.__glyphrelayReceiver.error = error;
  statusElement.textContent = message;
  connectionElement.textContent = state;
}

function parseJoinFragment(value) {
  const match = /^#join=([A-Za-z0-9_-]{22})\.([A-Za-z0-9_-]{22,128})$/.exec(
    value,
  );
  return match ? { sessionId: match[1], joinCapability: match[2] } : undefined;
}

function consumeInitialFragment() {
  const join = parseJoinFragment(location.hash);
  history.replaceState(null, "", location.pathname);
  if (!join) {
    return false;
  }
  sessionId = join.sessionId;
  joinCapability = join.joinCapability;
  return true;
}

function sendSignal(type, fields = {}) {
  if (!signalSocket || signalSocket.readyState !== WebSocket.OPEN || ended) {
    throw new Error("signaling_not_open");
  }
  signalSocket.send(
    JSON.stringify({
      protocolVersion: SIGNAL_PROTOCOL,
      sequence: ++clientSequence,
      type,
      ...fields,
    }),
  );
}

function validServerMessage(message) {
  return (
    message !== null &&
    typeof message === "object" &&
    !Array.isArray(message) &&
    message.protocolVersion === SIGNAL_PROTOCOL &&
    message.sessionId === sessionId &&
    Number.isSafeInteger(message.sequence) &&
    message.sequence === serverSequence + 1 &&
    typeof message.type === "string"
  );
}

function h264Codecs() {
  return (
    RTCRtpReceiver.getCapabilities("video")?.codecs.filter(
      (codec) =>
        codec.mimeType.toLowerCase() === "video/h264" &&
        /(?:^|;)\s*packetization-mode=1(?:;|$)/i.test(codec.sdpFmtpLine ?? ""),
    ) ?? []
  );
}

function signalIceState() {
  if (!peerConnection || ended) {
    return;
  }
  const state = peerConnection.iceConnectionState;
  if (["connected", "disconnected", "failed", "closed"].includes(state)) {
    sendSignal("RECEIVER_ICE_STATE", { iceState: state, sessionId });
  }
}

function capturePresentedFrame(_now, metadata) {
  if (ended) {
    return;
  }
  presentedFrames += 1;
  framesElement.textContent = String(presentedFrames);
  timestampElement.textContent =
    metadata.rtpTimestamp === undefined
      ? "Unavailable"
      : String(metadata.rtpTimestamp);
  sendReceiverStats(metadata);
  video.requestVideoFrameCallback(capturePresentedFrame);
}

async function sendReceiverStats(metadata) {
  const now = performance.now();
  if (
    !controlChannel ||
    controlChannel.readyState !== "open" ||
    ended ||
    now - lastStatsSentAt < 1_000
  ) {
    return;
  }
  lastStatsSentAt = now;
  const stats = await peerConnection?.getStats();
  let decodedFrames = presentedFrames;
  let droppedFrames = video.getVideoPlaybackQuality?.().droppedVideoFrames ?? 0;
  if (stats) {
    for (const report of stats.values()) {
      if (report.type === "inbound-rtp" && report.kind === "video") {
        decodedFrames = report.framesDecoded ?? decodedFrames;
        droppedFrames = report.framesDropped ?? droppedFrames;
      }
    }
  }
  sendControl("RECEIVER_STATS", {
    compositorFrames: presentedFrames,
    decodedFrames,
    droppedFrames,
    latestPresentedRtpTimestamp: metadata.rtpTimestamp ?? null,
  });
}

function sendControl(type, fields = {}) {
  if (!controlChannel || controlChannel.readyState !== "open" || ended) {
    return;
  }
  const encoded = JSON.stringify({
    protocolVersion: CONTROL_PROTOCOL,
    sequence: ++controlSendSequence,
    sessionId,
    type,
    ...fields,
  });
  if (new TextEncoder().encode(encoded).length > MAXIMUM_CONTROL_BYTES) {
    clearReceiver("control_message_too_large");
    return;
  }
  controlChannel.send(encoded);
}

function handleControlMessage(event) {
  const now = performance.now();
  if (!controlRateLimit.admit(now)) {
    clearReceiver("control_rate_exceeded");
    return;
  }
  const message = decodeSenderControlMessage(
    event.data,
    sessionId,
    controlReceiveSequence + 1,
  );
  if (!message) {
    clearReceiver("control_message_invalid");
    return;
  }
  controlReceiveSequence = message.sequence;
  switch (message.type) {
    case "HELLO":
      return;
    case "CLOCK_REQUEST":
      sendControl("CLOCK_RESPONSE", {
        receiverReceiveTimeMs: now,
        receiverSendTimeMs: performance.now(),
        requestSequence: message.sequence,
        senderSendTimeMs: message.senderSendTimeMs,
      });
      return;
    case "SESSION_PAUSED":
      updateStatus("PAUSED", "The sender paused this share.");
      sendControl("SESSION_PAUSED_ACK", { requestSequence: message.sequence });
      return;
    case "SESSION_RESUMED":
      updateStatus("RUNNING", "Receiving the shared screen.");
      sendControl("SESSION_RESUMED_ACK", { requestSequence: message.sequence });
      return;
    case "SESSION_ENDED":
      sendControl("SESSION_ENDED_ACK", { requestSequence: message.sequence });
      clearReceiver("session_ended");
      return;
    case "PROTOCOL_ERROR":
      clearReceiver("sender_protocol_error");
      return;
    default:
      clearReceiver("control_type_invalid");
  }
}

function acceptControlChannel(channel) {
  if (
    controlChannel ||
    channel.label !== CONTROL_PROTOCOL ||
    !channel.ordered ||
    channel.maxPacketLifeTime !== null ||
    channel.maxRetransmits !== null
  ) {
    channel.close();
    clearReceiver("control_channel_invalid");
    return;
  }
  controlChannel = channel;
  controlChannel.addEventListener("message", handleControlMessage);
  controlChannel.addEventListener("close", () => {
    if (!ended) {
      clearReceiver("control_channel_closed");
    }
  });
}

async function createReceiverOffer() {
  if (typeof video.requestVideoFrameCallback !== "function") {
    throw new Error("request_video_frame_callback_unavailable");
  }
  const codecs = h264Codecs();
  if (codecs.length === 0) {
    throw new Error("browser_h264_packetization_mode_1_unavailable");
  }
  peerConnection = new RTCPeerConnection({ iceServers: [] });
  const transceiver = peerConnection.addTransceiver("video", {
    direction: "recvonly",
  });
  transceiver.setCodecPreferences(codecs);
  peerConnection.addEventListener("datachannel", (event) => {
    acceptControlChannel(event.channel);
  });
  peerConnection.addEventListener("icecandidate", (event) => {
    if (!event.candidate) {
      return;
    }
    const candidate = event.candidate.toJSON();
    const serialized = JSON.stringify(candidate);
    if (offerSent) {
      sendSignal("RECEIVER_ICE_CANDIDATE", {
        candidate: serialized,
        sessionId,
      });
    } else {
      pendingCandidates.push(serialized);
    }
  });
  peerConnection.addEventListener("iceconnectionstatechange", signalIceState);
  peerConnection.addEventListener("track", (event) => {
    const [stream] = event.streams;
    video.srcObject = stream ?? new MediaStream([event.track]);
    shell.classList.add("has-video");
    updateStatus("RUNNING", "Receiving the shared screen.");
    video.requestVideoFrameCallback(capturePresentedFrame);
  });
  const offer = await peerConnection.createOffer();
  await peerConnection.setLocalDescription(offer);
  const sdp = peerConnection.localDescription?.sdp;
  if (!sdp) {
    throw new Error("receiver_offer_missing");
  }
  sendSignal("RECEIVER_OFFER", { sdp, sessionId });
  offerSent = true;
  for (const candidate of pendingCandidates) {
    sendSignal("RECEIVER_ICE_CANDIDATE", { candidate, sessionId });
  }
  pendingCandidates = [];
  updateStatus("NEGOTIATING", "Waiting for the sender to answer.");
}

async function handleSignalMessage(event) {
  if (typeof event.data !== "string" || event.data.length > 64 * 1024) {
    clearReceiver("signal_message_invalid");
    return;
  }
  let message;
  try {
    message = JSON.parse(event.data);
  } catch {
    clearReceiver("signal_message_invalid");
    return;
  }
  if (!validServerMessage(message)) {
    clearReceiver("signal_message_invalid");
    return;
  }
  serverSequence = message.sequence;
  switch (message.type) {
    case "JOIN_RESERVED":
      await createReceiverOffer();
      return;
    case "OWNER_ANSWER":
    case "OWNER_ICE_RESTART_ANSWER":
      if (typeof message.sdp !== "string" || !peerConnection) {
        throw new Error("sender_answer_invalid");
      }
      await peerConnection.setRemoteDescription({
        sdp: message.sdp,
        type: "answer",
      });
      return;
    case "OWNER_ICE_CANDIDATE":
      if (typeof message.candidate !== "string" || !peerConnection) {
        throw new Error("sender_candidate_invalid");
      }
      await peerConnection.addIceCandidate(JSON.parse(message.candidate));
      return;
    case "HEARTBEAT":
      if (!Number.isSafeInteger(message.heartbeatSequence)) {
        throw new Error("signal_heartbeat_invalid");
      }
      sendSignal("RECEIVER_HEARTBEAT_ACK", {
        heartbeatSequence: message.heartbeatSequence,
        sessionId,
      });
      return;
    case "SESSION_EXPIRED":
    case "SESSION_REVOKED":
      clearReceiver(message.type.toLowerCase());
      return;
    default:
      clearReceiver("signal_type_invalid");
  }
}

function clearReceiver(reason) {
  if (ended) {
    return;
  }
  ended = true;
  if (video.srcObject instanceof MediaStream) {
    for (const track of video.srcObject.getTracks()) {
      track.stop();
    }
  }
  video.srcObject = null;
  shell.classList.remove("has-video");
  controlChannel?.close();
  peerConnection?.close();
  signalSocket?.close();
  joinCapability = undefined;
  pendingCandidates = [];
  updateStatus("ENDED", "This sharing session has ended.", reason);
}

async function begin() {
  if (!hasInitialJoin) {
    updateStatus(
      "NO_LINK",
      "Open a current GlyphRelay sharing link to connect.",
    );
    return;
  }
  const webSocketScheme = location.protocol === "https:" ? "wss:" : "ws:";
  signalSocket = new WebSocket(
    `${webSocketScheme}//${location.host}/v1/signal`,
  );
  signalSocket.addEventListener("open", () => {
    sendSignal("RESERVE_JOIN", { joinCapability, sessionId });
    joinCapability = undefined;
    updateStatus("RESERVING", "Authenticating the one-time sharing link.");
  });
  signalSocket.addEventListener("message", (event) => {
    handleSignalMessage(event).catch((error) => {
      clearReceiver(error instanceof Error ? error.message : "signal_failed");
    });
  });
  signalSocket.addEventListener("close", () => {
    if (!ended) {
      clearReceiver("signaling_closed");
    }
  });
  signalSocket.addEventListener("error", () => {
    clearReceiver("signaling_error");
  });
}

begin().catch((error) => {
  clearReceiver(error instanceof Error ? error.message : "receiver_failed");
});
