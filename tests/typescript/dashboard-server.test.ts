import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import {
  DASHBOARD_PROTOCOL,
  MAXIMUM_ACTION_BYTES,
  startDashboardServer,
  type DashboardActionResult,
  type DashboardBackend,
  type DashboardServer,
  type DashboardSnapshot,
} from "../../dashboard/server.ts";

interface RawResponse {
  body: string;
  headers: http.IncomingHttpHeaders;
  status: number;
}

async function rawRequest(
  origin: string,
  path: string,
  options: {
    body?: string;
    headers?: Record<string, string>;
    method?: string;
  } = {},
): Promise<RawResponse> {
  const url = new URL(path, origin);
  return await new Promise((resolveRequest, rejectRequest) => {
    const request = http.request(
      url,
      {
        headers: options.headers,
        method: options.method ?? "GET",
      },
      (response) => {
        const chunks: Buffer[] = [];
        response.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
        response.on("end", () => {
          resolveRequest({
            body: Buffer.concat(chunks).toString("utf8"),
            headers: response.headers,
            status: response.statusCode ?? 0,
          });
        });
      },
    );
    request.once("error", rejectRequest);
    request.end(options.body);
  });
}

function launchNonce(server: DashboardServer): string {
  const launch = new URL(server.launchUrl);
  assert.equal(launch.origin, server.origin);
  assert.equal(launch.pathname, "/");
  assert.equal(launch.search, "");
  const match = /^#nonce=([A-Za-z0-9_-]{43})$/.exec(launch.hash);
  assert.ok(match);
  return match[1]!;
}

function authorizedHeaders(
  server: DashboardServer,
  nonce: string,
): Record<string, string> {
  return {
    Origin: server.origin,
    "Sec-Fetch-Site": "same-origin",
    "X-GlyphRelay-Dashboard-Nonce": nonce,
  };
}

function actionBody(action: string): string {
  return JSON.stringify({
    action,
    protocolVersion: DASHBOARD_PROTOCOL,
  });
}

class StateBackend implements DashboardBackend {
  actions: string[] = [];
  state: DashboardSnapshot = {
    bitrateProfile: "2m",
    captureActive: true,
    connectionState: "CONNECTED",
    droppedFrames: 4,
    fallbackMode: "CPU_UNIFORM",
    protectedFraction: 0.125,
    queueDelayMs: 7,
    recordingActive: false,
    shareLinkAvailable: true,
  };

  perform(action: string): DashboardActionResult {
    this.actions.push(action);
    if (action === "PAUSE") {
      this.state = {
        ...this.state,
        captureActive: false,
        connectionState: "PAUSED",
      };
    }
    return {
      accepted: true,
      reason: "action_applied",
      snapshot: this.state,
    };
  }

  snapshot(): DashboardSnapshot {
    return this.state;
  }
}

test("binds only to an exact loopback address with a unique fragment nonce", async (context) => {
  await assert.rejects(
    startDashboardServer({ host: "0.0.0.0" }),
    /dashboard_non_loopback_bind_rejected/,
  );
  await assert.rejects(
    startDashboardServer({ host: "localhost" }),
    /dashboard_non_loopback_bind_rejected/,
  );
  await assert.rejects(
    startDashboardServer({ port: -1 }),
    /dashboard_port_invalid/,
  );

  const first = await startDashboardServer();
  const second = await startDashboardServer();
  context.after(async () => {
    await first.close();
    await second.close();
  });
  assert.equal(first.host, "127.0.0.1");
  assert.notEqual(launchNonce(first), launchNonce(second));
  assert.equal(new URL(first.launchUrl).username, "");
  assert.equal(new URL(first.launchUrl).password, "");
});

test("serves a self-contained shell without exposing launch authority", async (context) => {
  const server = await startDashboardServer();
  context.after(() => server.close());
  const nonce = launchNonce(server);

  const page = await rawRequest(server.origin, "/");
  assert.equal(page.status, 200);
  assert.doesNotMatch(page.body, new RegExp(nonce));
  assert.match(page.body, /GlyphRelay control room/);
  assert.equal(page.headers["cache-control"], "no-store");
  assert.equal(page.headers["referrer-policy"], "no-referrer");
  assert.equal(page.headers["cross-origin-resource-policy"], "same-origin");
  assert.match(
    String(page.headers["content-security-policy"]),
    /default-src 'none'/,
  );
  assert.equal(page.headers["access-control-allow-origin"], undefined);

  for (const path of ["/dashboard.js", "/dashboard.css"] as const) {
    const asset = await rawRequest(server.origin, path);
    assert.equal(asset.status, 200);
    assert.doesNotMatch(asset.body, /https?:\/\//i);
    assert.doesNotMatch(asset.body, new RegExp(nonce));
  }

  const health = await rawRequest(server.origin, "/healthz");
  assert.deepEqual(JSON.parse(health.body), {
    protocolVersion: DASHBOARD_PROTOCOL,
    status: "ok",
  });
  assert.equal(
    (await rawRequest(server.origin, `/?nonce=${nonce}`)).status,
    400,
  );
  assert.equal(
    (await rawRequest(server.origin, "/", { headers: { Cookie: "a=b" } }))
      .status,
    403,
  );
});

test("rejects Host, Origin, DNS-rebinding, nonce, and CSRF attacks before dispatch", async (context) => {
  const backend = new StateBackend();
  const server = await startDashboardServer({ backend });
  context.after(() => server.close());
  const nonce = launchNonce(server);

  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/state", {
        headers: { Host: "attacker.invalid" },
      })
    ).status,
    421,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/state", {
        headers: {
          Origin: "http://attacker.invalid",
          "X-GlyphRelay-Dashboard-Nonce": nonce,
        },
      })
    ).status,
    403,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/state", {
        headers: {
          "Sec-Fetch-Site": "cross-site",
          "X-GlyphRelay-Dashboard-Nonce": nonce,
        },
      })
    ).status,
    403,
  );
  assert.equal((await rawRequest(server.origin, "/api/v1/state")).status, 401);
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/state", {
        headers: { "X-GlyphRelay-Dashboard-Nonce": "A".repeat(43) },
      })
    ).status,
    401,
  );

  const state = await rawRequest(server.origin, "/api/v1/state", {
    headers: authorizedHeaders(server, nonce),
  });
  assert.equal(state.status, 200);
  assert.doesNotMatch(state.body, new RegExp(nonce));
  const stateMessage = JSON.parse(state.body) as {
    csrfToken: string;
    snapshot: DashboardSnapshot;
  };
  assert.match(stateMessage.csrfToken, /^[A-Za-z0-9_-]{43}$/);
  assert.equal(stateMessage.snapshot.connectionState, "CONNECTED");

  const baseAction = {
    body: actionBody("PAUSE"),
    headers: {
      ...authorizedHeaders(server, nonce),
      "Content-Type": "application/json",
      "X-GlyphRelay-CSRF": stateMessage.csrfToken,
    },
    method: "POST",
  };
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        ...baseAction,
        headers: {
          ...baseAction.headers,
          Origin: "http://attacker.invalid",
        },
      })
    ).status,
    403,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        ...baseAction,
        headers: {
          ...baseAction.headers,
          "X-GlyphRelay-CSRF": "A".repeat(43),
        },
      })
    ).status,
    401,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        ...baseAction,
        headers: {
          ...baseAction.headers,
          "X-GlyphRelay-Dashboard-Nonce": "A".repeat(43),
        },
      })
    ).status,
    401,
  );
  const preflight = await rawRequest(server.origin, "/api/v1/action", {
    headers: {
      "Access-Control-Request-Headers":
        "x-glyphrelay-dashboard-nonce,x-glyphrelay-csrf",
      "Access-Control-Request-Method": "POST",
      Origin: "http://attacker.invalid",
    },
    method: "OPTIONS",
  });
  assert.equal(preflight.status, 403);
  assert.equal(preflight.headers["access-control-allow-origin"], undefined);
  assert.deepEqual(backend.actions, []);

  const accepted = await rawRequest(
    server.origin,
    "/api/v1/action",
    baseAction,
  );
  assert.equal(accepted.status, 200);
  assert.deepEqual(backend.actions, ["PAUSE"]);
  assert.equal(
    (JSON.parse(accepted.body) as DashboardActionResult).snapshot
      .connectionState,
    "PAUSED",
  );
});

test("bounds and strictly validates the mutation body", async (context) => {
  const backend = new StateBackend();
  const server = await startDashboardServer({ backend });
  context.after(() => server.close());
  const nonce = launchNonce(server);
  const state = await rawRequest(server.origin, "/api/v1/state", {
    headers: authorizedHeaders(server, nonce),
  });
  const csrfToken = (JSON.parse(state.body) as { csrfToken: string }).csrfToken;
  const headers = {
    ...authorizedHeaders(server, nonce),
    "Content-Type": "application/json",
    "X-GlyphRelay-CSRF": csrfToken,
  };

  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        body: "not-json",
        headers,
        method: "POST",
      })
    ).status,
    400,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        body: `{"action":"STOP","action":"PAUSE","protocolVersion":"${DASHBOARD_PROTOCOL}"}`,
        headers,
        method: "POST",
      })
    ).status,
    400,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        body: ` ${actionBody("STOP")}`,
        headers,
        method: "POST",
      })
    ).status,
    400,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        body: JSON.stringify({
          action: "STOP",
          extra: true,
          protocolVersion: DASHBOARD_PROTOCOL,
        }),
        headers,
        method: "POST",
      })
    ).status,
    400,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        body: actionBody("REMOTE_INPUT"),
        headers,
        method: "POST",
      })
    ).status,
    400,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        body: actionBody("STOP"),
        headers: { ...headers, "Content-Type": "text/plain" },
        method: "POST",
      })
    ).status,
    415,
  );
  assert.equal(
    (
      await rawRequest(server.origin, "/api/v1/action", {
        body: "{}",
        headers: {
          ...headers,
          "Content-Length": String(MAXIMUM_ACTION_BYTES + 1),
        },
        method: "POST",
      })
    ).status,
    413,
  );
  assert.deepEqual(backend.actions, []);
});

test("standalone dashboard never reports a sender action as successful", async (context) => {
  const server = await startDashboardServer();
  context.after(() => server.close());
  const nonce = launchNonce(server);
  const state = await rawRequest(server.origin, "/api/v1/state", {
    headers: authorizedHeaders(server, nonce),
  });
  const message = JSON.parse(state.body) as {
    csrfToken: string;
    snapshot: DashboardSnapshot;
  };
  assert.equal(message.snapshot.connectionState, "UNAVAILABLE");
  const action = await rawRequest(server.origin, "/api/v1/action", {
    body: actionBody("START"),
    headers: {
      ...authorizedHeaders(server, nonce),
      "Content-Type": "application/json",
      "X-GlyphRelay-CSRF": message.csrfToken,
    },
    method: "POST",
  });
  assert.equal(action.status, 409);
  assert.equal(
    (JSON.parse(action.body) as DashboardActionResult).reason,
    "sender_backend_unavailable",
  );
});
