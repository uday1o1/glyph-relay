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
const actionButtons = [...document.querySelectorAll("button[data-action]")];

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

async function performAction(action) {
  if (!launchNonce || !csrfToken || actionPending) {
    return;
  }
  actionPending = true;
  if (window.__glyphrelayDashboard.snapshot) {
    render(window.__glyphrelayDashboard.snapshot);
  }
  try {
    const response = await fetch("/api/v1/action", {
      body: JSON.stringify({ action, protocolVersion: DASHBOARD_PROTOCOL }),
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
    () => void performAction(button.dataset.action),
  );
}

loadState().catch((error) => {
  for (const button of actionButtons) {
    button.disabled = true;
  }
  setStatus(
    "REJECTED",
    "This dashboard link is not valid",
    "Close this tab and launch a new dashboard from GlyphRelay.",
    error instanceof Error ? error.message : "dashboard_startup_failed",
  );
});
