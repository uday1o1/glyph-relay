const PROTOCOL_VERSION = "glyphrelay-m0-loopback-v1";
const SESSION_ID = "m0-loopback-session";
const MAXIMUM_ORACLE_BYTES = 16 * 1024 * 1024;

const statusElement = document.querySelector("#status");
const connectButton = document.querySelector("#connect");
const codecElement = document.querySelector("#codec");
const framesElement = document.querySelector("#frames");
const timestampElement = document.querySelector("#timestamp");
const video = document.querySelector("#video");
const canvas = document.querySelector("#oracle");
const shell = document.querySelector(".video-shell");

if (
  !(statusElement instanceof HTMLElement) ||
  !(connectButton instanceof HTMLButtonElement) ||
  !(codecElement instanceof HTMLElement) ||
  !(framesElement instanceof HTMLElement) ||
  !(timestampElement instanceof HTMLElement) ||
  !(video instanceof HTMLVideoElement) ||
  !(canvas instanceof HTMLCanvasElement) ||
  !(shell instanceof HTMLElement)
) {
  throw new Error("receiver_document_invalid");
}

history.replaceState(null, "", `${location.pathname}${location.search}`);

let peerConnection;
let controlChannel;
let presentedFrames = 0;
let controlSequence = 0;
let oracleBytes = 0;
let lastStatsSentAt = Number.NEGATIVE_INFINITY;
const oracleFrames = [];
let oracleTargets = null;

window.__glyphrelayReceiver = {
  state: "READY",
  error: null,
  oracleFrames,
  setOracleTargets,
  snapshot: receiverSnapshot,
};

function setOracleTargets(values) {
  if (
    window.__glyphrelayReceiver.state !== "READY" ||
    !Array.isArray(values) ||
    values.length !== 4 ||
    new Set(values).size !== values.length ||
    !values.every(
      (value) =>
        Number.isSafeInteger(value) && value >= 0 && value <= 0xffffffff,
    )
  ) {
    throw new Error("oracle_targets_invalid");
  }
  oracleTargets = new Set(values);
}

function rgbaBase64(bytes) {
  let binary = "";
  const chunkSize = 32 * 1024;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    binary += String.fromCharCode(
      ...bytes.subarray(offset, offset + chunkSize),
    );
  }
  return btoa(binary);
}

async function receiverSnapshot(includeRgba = false) {
  const inboundVideo = [];
  if (peerConnection) {
    const stats = await peerConnection.getStats();
    for (const report of stats.values()) {
      if (report.type === "inbound-rtp" && report.kind === "video") {
        inboundVideo.push({
          framesDecoded: report.framesDecoded ?? null,
          framesDropped: report.framesDropped ?? null,
          keyFramesDecoded: report.keyFramesDecoded ?? null,
          nackCount: report.nackCount ?? null,
          packetsLost: report.packetsLost ?? null,
          pliCount: report.pliCount ?? null,
        });
      }
    }
  }
  const oracle = [];
  const retainedFrames = oracleFrames.slice(-4);
  for (const frame of retainedFrames) {
    const digest = await crypto.subtle.digest("SHA-256", frame.rgba);
    const retained = {
      expectedDisplayTime: frame.expectedDisplayTime,
      height: frame.height,
      presentationTime: frame.presentationTime,
      rgbaSha256: Array.from(new Uint8Array(digest), (value) =>
        value.toString(16).padStart(2, "0"),
      ).join(""),
      rtpTimestamp: frame.rtpTimestamp,
      width: frame.width,
    };
    if (includeRgba) {
      retained.rgbaBase64 = rgbaBase64(frame.rgba);
    }
    oracle.push(retained);
  }
  return {
    inboundVideo,
    oracle,
    playbackQuality: video.getVideoPlaybackQuality
      ? {
          corruptedVideoFrames:
            video.getVideoPlaybackQuality().corruptedVideoFrames,
          droppedVideoFrames:
            video.getVideoPlaybackQuality().droppedVideoFrames,
          totalVideoFrames: video.getVideoPlaybackQuality().totalVideoFrames,
        }
      : null,
    presentedFrames,
    state: window.__glyphrelayReceiver.state,
    videoHeight: video.videoHeight,
    videoWidth: video.videoWidth,
  };
}

function updateStatus(state, message, error = null) {
  window.__glyphrelayReceiver.state = state;
  window.__glyphrelayReceiver.error = error;
  statusElement.textContent = message;
}

function waitForIceGathering(connection) {
  if (connection.iceGatheringState === "complete") {
    return Promise.resolve();
  }
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      connection.removeEventListener("icegatheringstatechange", changed);
      reject(new Error("ice_gathering_timeout"));
    }, 10_000);
    const changed = () => {
      if (connection.iceGatheringState === "complete") {
        clearTimeout(timeout);
        connection.removeEventListener("icegatheringstatechange", changed);
        resolve();
      }
    };
    connection.addEventListener("icegatheringstatechange", changed);
  });
}

async function publishOffer(sdp) {
  const response = await fetch("/api/m0/offer", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      protocolVersion: PROTOCOL_VERSION,
      sessionId: SESSION_ID,
      type: "offer",
      sdp,
    }),
  });
  if (!response.ok) {
    throw new Error(`offer_publish_failed_${response.status}`);
  }
}

async function waitForAnswer() {
  const deadline = performance.now() + 30_000;
  while (performance.now() < deadline) {
    const response = await fetch("/api/m0/answer", { cache: "no-store" });
    if (response.status === 200) {
      const answer = await response.json();
      if (
        answer.protocolVersion !== PROTOCOL_VERSION ||
        answer.sessionId !== SESSION_ID ||
        answer.type !== "answer" ||
        typeof answer.sdp !== "string"
      ) {
        throw new Error("answer_envelope_invalid");
      }
      return answer.sdp;
    }
    if (response.status !== 204) {
      throw new Error(`answer_poll_failed_${response.status}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("answer_timeout");
}

function sendReceiverStats(metadata) {
  const now = performance.now();
  if (
    !controlChannel ||
    controlChannel.readyState !== "open" ||
    now - lastStatsSentAt < 1_000
  ) {
    return;
  }
  const message = {
    protocolVersion: "glyphrelay-control-v1",
    sessionId: SESSION_ID,
    sequence: ++controlSequence,
    type: "RECEIVER_STATS",
    decodedFrames: presentedFrames,
    droppedFrames: video.getVideoPlaybackQuality?.().droppedVideoFrames ?? 0,
    compositorFrames: presentedFrames,
    latestPresentedRtpTimestamp: metadata.rtpTimestamp ?? null,
  };
  const encoded = JSON.stringify(message);
  if (encoded.length <= 4_096) {
    controlChannel.send(encoded);
    lastStatsSentAt = now;
  }
}

function capturePresentedFrame(_now, metadata) {
  if (video.videoWidth > 0 && video.videoHeight > 0) {
    canvas.width = video.videoWidth;
    canvas.height = video.videoHeight;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (context) {
      context.drawImage(video, 0, 0, canvas.width, canvas.height);
      const pixels = context.getImageData(
        0,
        0,
        canvas.width,
        canvas.height,
      ).data;
      const frame = {
        rtpTimestamp: metadata.rtpTimestamp ?? null,
        presentationTime: metadata.presentationTime,
        expectedDisplayTime: metadata.expectedDisplayTime,
        width: canvas.width,
        height: canvas.height,
        rgba: new Uint8ClampedArray(pixels),
      };
      const targetSelected =
        oracleTargets === null || oracleTargets.has(frame.rtpTimestamp);
      const alreadyRetained = oracleFrames.some(
        (retained) => retained.rtpTimestamp === frame.rtpTimestamp,
      );
      if (targetSelected && !alreadyRetained) {
        while (
          oracleTargets === null &&
          oracleFrames.length > 0 &&
          oracleBytes + frame.rgba.byteLength > MAXIMUM_ORACLE_BYTES
        ) {
          const removed = oracleFrames.shift();
          oracleBytes -= removed.rgba.byteLength;
        }
        if (oracleBytes + frame.rgba.byteLength <= MAXIMUM_ORACLE_BYTES) {
          oracleFrames.push(frame);
          oracleBytes += frame.rgba.byteLength;
        }
      }
    }
  }
  presentedFrames += 1;
  framesElement.textContent = String(presentedFrames);
  timestampElement.textContent =
    metadata.rtpTimestamp === undefined
      ? "Unavailable"
      : String(metadata.rtpTimestamp);
  sendReceiverStats(metadata);
  if (window.__glyphrelayReceiver.state === "RUNNING") {
    video.requestVideoFrameCallback(capturePresentedFrame);
  }
}

async function connect() {
  connectButton.disabled = true;
  updateStatus("NEGOTIATING", "Inspecting browser H.264 receive capabilities.");
  if (typeof video.requestVideoFrameCallback !== "function") {
    throw new Error("request_video_frame_callback_unavailable");
  }
  const capabilities = RTCRtpReceiver.getCapabilities("video");
  const h264 =
    capabilities?.codecs.filter(
      (codec) =>
        codec.mimeType.toLowerCase() === "video/h264" &&
        /(?:^|;)\s*packetization-mode=1(?:;|$)/i.test(codec.sdpFmtpLine ?? ""),
    ) ?? [];
  if (h264.length === 0) {
    throw new Error("browser_h264_packetization_mode_1_unavailable");
  }

  peerConnection = new RTCPeerConnection({ iceServers: [] });
  const transceiver = peerConnection.addTransceiver("video", {
    direction: "recvonly",
  });
  transceiver.setCodecPreferences(h264);
  codecElement.textContent = h264
    .map((codec) => codec.sdpFmtpLine ?? "H264/90000")
    .join(" | ");
  controlChannel = peerConnection.createDataChannel("glyphrelay-control-v1", {
    ordered: true,
  });
  controlChannel.addEventListener("close", () => {
    if (window.__glyphrelayReceiver.state === "RUNNING") {
      clearReceiver("control_channel_closed");
    }
  });
  controlChannel.addEventListener("message", (event) => {
    if (typeof event.data !== "string" || event.data.length > 4_096) {
      clearReceiver("control_message_invalid");
      return;
    }
    try {
      const message = JSON.parse(event.data);
      if (
        message.protocolVersion === "glyphrelay-control-v1" &&
        message.sessionId === SESSION_ID &&
        message.type === "SESSION_ENDED"
      ) {
        clearReceiver("session_ended");
      }
    } catch {
      clearReceiver("control_message_invalid");
    }
  });
  peerConnection.addEventListener("track", (event) => {
    const [stream] = event.streams;
    video.srcObject = stream ?? new MediaStream([event.track]);
    shell.classList.add("has-video");
    updateStatus("RUNNING", "Receiving the loopback H.264 stream.");
    video.requestVideoFrameCallback(capturePresentedFrame);
  });
  peerConnection.addEventListener("connectionstatechange", () => {
    if (
      peerConnection &&
      ["failed", "closed"].includes(peerConnection.connectionState)
    ) {
      clearReceiver(`peer_${peerConnection.connectionState}`);
    }
  });

  const offer = await peerConnection.createOffer();
  await peerConnection.setLocalDescription(offer);
  await waitForIceGathering(peerConnection);
  const localSdp = peerConnection.localDescription?.sdp;
  if (!localSdp) {
    throw new Error("local_offer_missing");
  }
  await publishOffer(localSdp);
  updateStatus(
    "WAITING_ANSWER",
    "Loopback offer published. Waiting for the sender answer.",
  );
  const answer = await waitForAnswer();
  await peerConnection.setRemoteDescription({ type: "answer", sdp: answer });
}

function clearReceiver(reason) {
  if (video.srcObject instanceof MediaStream) {
    for (const track of video.srcObject.getTracks()) {
      track.stop();
    }
  }
  video.srcObject = null;
  oracleFrames.splice(0);
  oracleBytes = 0;
  oracleTargets = null;
  shell.classList.remove("has-video");
  peerConnection?.close();
  peerConnection = undefined;
  controlChannel = undefined;
  updateStatus("ENDED", "The receiver has cleared its remote media.", reason);
}

connectButton.addEventListener("click", () => {
  connect()
    .catch((error) => {
      const reason = error instanceof Error ? error.message : "receiver_failed";
      clearReceiver(reason);
      updateStatus("FAILED", `Receiver failed: ${reason}`, reason);
    })
    .finally(() => {
      connectButton.disabled = false;
    });
});
