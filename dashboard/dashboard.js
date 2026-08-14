const DASHBOARD_PROTOCOL = "glyphrelay-dashboard-v1";
const NONCE_PATTERN = /^#nonce=([A-Za-z0-9_-]{43})$/;

const statusHeading = document.querySelector("#status-heading");
const statusDetail = document.querySelector("#status-detail");
const captureIndicator = document.querySelector("#capture-indicator");
const bitrate = document.querySelector("#bitrate");
const queueDelay = document.querySelector("#queue-delay");
const droppedFrames = document.querySelector("#dropped-frames");
const protectedFraction = document.querySelector("#protected-fraction");
const fallbackMode = document.querySelector("#fallback-mode");
const recordingState = document.querySelector("#recording-state");
const preview = document.querySelector(".preview");
const previewCanvas = document.querySelector("#protected-preview");
const previewState = document.querySelector("#preview-state");
const correctionForm = document.querySelector("#correction-form");
const correctionRegions = document.querySelector("#correction-regions");
const actionButtons = [...document.querySelectorAll("button[data-action]")];
const correctionButtons = [
  ...document.querySelectorAll("button[data-correction]"),
];

if (
  !(statusHeading instanceof HTMLElement) ||
  !(statusDetail instanceof HTMLElement) ||
  !(captureIndicator instanceof HTMLElement) ||
  !(bitrate instanceof HTMLElement) ||
  !(queueDelay instanceof HTMLElement) ||
  !(droppedFrames instanceof HTMLElement) ||
  !(protectedFraction instanceof HTMLElement) ||
  !(fallbackMode instanceof HTMLElement) ||
  !(recordingState instanceof HTMLElement) ||
  !(preview instanceof HTMLElement) ||
  !(previewCanvas instanceof HTMLCanvasElement) ||
  !(previewState instanceof HTMLElement) ||
  !(correctionForm instanceof HTMLFormElement) ||
  !(correctionRegions instanceof HTMLOListElement) ||
  correctionButtons.some((button) => !(button instanceof HTMLButtonElement)) ||
  actionButtons.some((button) => !(button instanceof HTMLButtonElement))
) {
  throw new Error("dashboard_document_invalid");
}

const launchNonce = consumeLaunchNonce();
let csrfToken;
let actionPending = false;

window.__glyphrelayDashboard = {
  state: "OPENING",
  error: null,
  snapshot: null,
};

function consumeLaunchNonce() {
  const match = NONCE_PATTERN.exec(location.hash);
  history.replaceState(null, "", location.pathname);
  return match?.[1];
}

function setStatus(state, heading, detail, error = null) {
  window.__glyphrelayDashboard.state = state;
  window.__glyphrelayDashboard.error = error;
  document.body.dataset.dashboardState = state.toLowerCase();
  statusHeading.textContent = heading;
  statusDetail.textContent = detail;
}

function controlsFor(snapshot) {
  const state = snapshot.connectionState;
  return {
    COPY_SHARE_LINK: !snapshot.shareLinkAvailable,
    PAUSE: state !== "CONNECTED",
    RESUME: state !== "PAUSED",
    START: state !== "IDLE",
    STOP: !["OWNER_ONLY", "JOIN_OPEN", "CONNECTED", "PAUSED"].includes(state),
  };
}

function updateControls(snapshot) {
  const disabled = controlsFor(snapshot);
  for (const button of actionButtons) {
    button.disabled =
      actionPending || disabled[button.dataset.action] !== false;
  }
  const correctionsUnavailable =
    actionPending ||
    snapshot.connectionState === "UNAVAILABLE" ||
    !snapshot.visibleGeometry;
  for (const button of correctionButtons) {
    button.disabled = correctionsUnavailable;
  }
  for (const input of correctionForm.elements) {
    if (input instanceof HTMLInputElement) {
      input.disabled = correctionsUnavailable;
    }
  }
}

const levelColors = [
  "rgba(0,0,0,0)",
  "rgba(64,128,255,0.55)",
  "rgba(40,200,255,0.62)",
  "rgba(255,220,32,0.68)",
  "rgba(255,128,24,0.76)",
  "rgba(255,32,32,0.82)",
];

function renderPreview(snapshot) {
  const map = snapshot.saliencyPreview;
  const geometry = snapshot.visibleGeometry;
  if (!map || !geometry) {
    preview.classList.remove("has-map");
    previewState.textContent = "No sender map is available.";
    return;
  }
  const tileCount = map.tileWidth * map.tileHeight;
  const valid =
    Number.isSafeInteger(map.tileWidth) &&
    Number.isSafeInteger(map.tileHeight) &&
    map.tileWidth > 0 &&
    map.tileHeight > 0 &&
    Array.isArray(map.levels) &&
    map.levels.length === tileCount &&
    map.levels.every(
      (level) => Number.isSafeInteger(level) && level >= 0 && level <= 5,
    ) &&
    Array.isArray(map.conflictTiles) &&
    map.conflictTiles.every(
      (tile) => Number.isSafeInteger(tile) && tile >= 0 && tile < tileCount,
    );
  if (!valid) {
    throw new Error("dashboard_saliency_preview_invalid");
  }
  const conflicts = new Set(map.conflictTiles);
  previewCanvas.width = map.tileWidth;
  previewCanvas.height = map.tileHeight;
  const context = previewCanvas.getContext("2d", { alpha: true });
  if (!context) {
    throw new Error("dashboard_preview_context_unavailable");
  }
  context.clearRect(0, 0, map.tileWidth, map.tileHeight);
  for (let tile = 0; tile < tileCount; tile += 1) {
    context.fillStyle = conflicts.has(tile)
      ? "rgba(224,48,255,0.9)"
      : levelColors[map.levels[tile]];
    context.fillRect(
      tile % map.tileWidth,
      Math.floor(tile / map.tileWidth),
      1,
      1,
    );
  }
  preview.classList.add("has-map");
  previewState.textContent = `${geometry.width} x ${geometry.height}, epoch ${geometry.geometryEpoch}`;
}

function renderCorrections(snapshot) {
  correctionRegions.replaceChildren();
  for (const region of snapshot.correctionRegions) {
    const item = document.createElement("li");
    item.classList.toggle("conflict", region.conflict);
    const label = document.createElement("span");
    label.textContent = `${region.kind.toLowerCase()} ${region.x},${region.y} ${region.width}x${region.height}${region.conflict ? " - conflict" : ""}`;
    const remove = document.createElement("button");
    remove.type = "button";
    remove.textContent = "Remove";
    remove.disabled = actionPending;
    remove.addEventListener(
      "click",
      () =>
        void performCommand({
          action: "REMOVE_CORRECTION",
          expectedRevision: snapshot.correctionRevision,
          regionId: region.id,
        }),
    );
    item.append(label, remove);
    correctionRegions.append(item);
  }
}

function render(snapshot) {
  window.__glyphrelayDashboard.snapshot = structuredClone(snapshot);
  const unavailable = snapshot.connectionState === "UNAVAILABLE";
  setStatus(
    snapshot.connectionState,
    unavailable ? "Sender connection unavailable" : snapshot.connectionState,
    unavailable
      ? "Start the dashboard from the GlyphRelay sender to enable controls."
      : "This dashboard is authenticated for the current local launch.",
  );
  captureIndicator.textContent = snapshot.captureActive ? "Capturing" : "Idle";
  captureIndicator.classList.toggle("active", snapshot.captureActive);
  bitrate.textContent = snapshot.bitrateProfile;
  queueDelay.textContent = `${snapshot.queueDelayMs} ms`;
  droppedFrames.textContent = String(snapshot.droppedFrames);
  protectedFraction.textContent = `${Math.round(snapshot.protectedFraction * 100)}%`;
  fallbackMode.textContent = snapshot.fallbackMode.replaceAll("_", " ");
  recordingState.textContent = snapshot.recordingActive ? "Recording" : "Off";

  renderPreview(snapshot);
  renderCorrections(snapshot);
  updateControls(snapshot);
}

async function responseJson(response) {
  const value = await response.json();
  if (
    !value ||
    typeof value !== "object" ||
    value.protocolVersion !== DASHBOARD_PROTOCOL
  ) {
    throw new Error("dashboard_response_invalid");
  }
  return value;
}

async function loadState() {
  if (!launchNonce) {
    throw new Error("dashboard_launch_nonce_missing");
  }
  const response = await fetch("/api/v1/state", {
    cache: "no-store",
    credentials: "omit",
    headers: {
      Accept: "application/json",
      "X-GlyphRelay-Dashboard-Nonce": launchNonce,
    },
    redirect: "error",
  });
  if (!response.ok) {
    throw new Error(`dashboard_state_rejected_${response.status}`);
  }
  const message = await responseJson(response);
  if (typeof message.csrfToken !== "string" || !message.snapshot) {
    throw new Error("dashboard_state_invalid");
  }
  csrfToken = message.csrfToken;
  render(message.snapshot);
}

async function performCommand(command) {
  if (!launchNonce || !csrfToken || actionPending) {
    return;
  }
  actionPending = true;
  if (window.__glyphrelayDashboard.snapshot) {
    render(window.__glyphrelayDashboard.snapshot);
  }
  try {
    const response = await fetch("/api/v1/action", {
      body: JSON.stringify({ ...command, protocolVersion: DASHBOARD_PROTOCOL }),
      cache: "no-store",
      credentials: "omit",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json",
        "X-GlyphRelay-CSRF": csrfToken,
        "X-GlyphRelay-Dashboard-Nonce": launchNonce,
      },
      method: "POST",
      redirect: "error",
    });
    const message = await responseJson(response);
    if (!message.snapshot) {
      throw new Error("dashboard_action_response_invalid");
    }
    render(message.snapshot);
    if (!response.ok) {
      setStatus(
        message.snapshot.connectionState,
        "Action was not applied",
        message.reason ?? "The local sender rejected this action.",
        message.reason ?? "dashboard_action_rejected",
      );
    }
  } catch (error) {
    setStatus(
      "ERROR",
      "Dashboard action failed",
      "The local sender did not accept the request.",
      error instanceof Error ? error.message : "dashboard_action_failed",
    );
  } finally {
    actionPending = false;
    if (window.__glyphrelayDashboard.snapshot) {
      updateControls(window.__glyphrelayDashboard.snapshot);
    }
  }
}

for (const button of actionButtons) {
  button.addEventListener(
    "click",
    () => void performCommand({ action: button.dataset.action }),
  );
}

correctionForm.addEventListener("submit", (event) => {
  event.preventDefault();
  const submitter = event.submitter;
  const snapshot = window.__glyphrelayDashboard.snapshot;
  if (!(submitter instanceof HTMLButtonElement) || !snapshot) {
    return;
  }
  const form = new FormData(correctionForm);
  const rectangle = {
    height: Number(form.get("height")),
    width: Number(form.get("width")),
    x: Number(form.get("x")),
    y: Number(form.get("y")),
  };
  void performCommand({
    action: submitter.dataset.correction,
    expectedRevision: snapshot.correctionRevision,
    rectangle,
  });
});

loadState().catch((error) => {
  for (const button of actionButtons) {
    button.disabled = true;
  }
  for (const button of correctionButtons) {
    button.disabled = true;
  }
  setStatus(
    "REJECTED",
    "This dashboard link is not valid",
    "Close this tab and launch a new dashboard from GlyphRelay.",
    error instanceof Error ? error.message : "dashboard_startup_failed",
  );
});
