# Local dashboard security boundary

The GlyphRelay dashboard is a local sender control surface, not a remotely hosted administration page.
It binds to an exact loopback IP address and refuses wildcard, hostname, LAN, and internet binds.
The default standalone command uses `127.0.0.1` and an ephemeral port.

Run the standalone boundary with:

```console
corepack pnpm dashboard:start
```

The command prints one launch URL whose authorization value is stored only in the URL fragment.
The page consumes that fragment into module-private memory and immediately replaces the current history entry with `/`.
The nonce never appears in an HTTP path, query, cookie, response body, or browser request URL.

The standalone process deliberately reports `UNAVAILABLE` and rejects sender actions because it has no native sender backend.
An embedding sender must supply the `DashboardBackend` interface before the controls can report success.
This prevents a detached dashboard from presenting a mocked sharing state.

## Request boundary

Every request must use the exact numeric loopback `Host` value and ephemeral port selected by the server.
Requests carrying an `Origin` must match the dashboard origin exactly.
State-changing requests require that exact Origin, a 256-bit launch nonce header, and a separate 256-bit CSRF header.
Cross-site Fetch Metadata values, ambient cookies, CORS preflights, and DNS-rebinding Host values fail closed.
The server emits no CORS allow headers.

The read-only state endpoint also requires the launch nonce.
The mutation endpoint accepts only canonical JSON envelopes, five local session actions, three bounded correction actions, and at most 4 KiB.
Correction creation carries an exact source-visible rectangle and the last rendered correction revision.
Correction removal carries a stable region identifier and that same expected revision.
Coordinates are nonnegative safe integers, dimensions are positive safe integers, and each right or bottom edge may not exceed 16,384 pixels before the native backend applies the tighter current-geometry bound.
A stale correction revision is rejected by the native backend so concurrent local tabs cannot silently overwrite one another.
Keyboard, mouse, clipboard ingestion, file transfer, commands, arbitrary messages, unknown fields, duplicate keys, and noncanonical JSON are not part of the dashboard protocol.

All responses use `no-store`, `nosniff`, same-origin resource policy, frame denial, a no-referrer policy, restrictive permissions, and a CSP that permits only same-origin scripts, styles, and connections.
The page has no third-party assets, analytics, service workers, forms, cookies, or remote requests.

## Verification

Run the protocol and hostile-request tests with:

```console
node --test tests/typescript/dashboard-server.test.ts
```

Run the pinned Chromium user workflow with:

```console
make dashboard-browser-check
```

The browser verifier loads the launch URL, confirms the fragment disappears, renders the real tile-map contract, adds a bounded pinned region, performs an authorized pause, observes the updated controls, and asserts that no request contains the nonce or leaves the loopback origin.

## Correction and preview contract

The dashboard receives only the sender's local protected-level tile map, conflict tile indices, visible geometry, correction metadata, and operational measurements.
It does not receive or render captured screen pixels.
Levels zero through five use the same qualitative overlay palette as the native preview, while a pin-exclusion overlap uses a distinct magenta conflict color.
The exclusion still wins in the encoder-facing map.

Pins and exclusions are source-visible rectangles and share the same native coordinate contract used by saliency, cursor halos, and evaluation truth.
A geometry-epoch change atomically clears the native correction set instead of reusing stale coordinates against a different crop or resolution.
The native set has a fixed capacity and rejects overflow, out-of-bounds rectangles, identity exhaustion, stale revisions, and removal of an unknown identity without mutating state.
