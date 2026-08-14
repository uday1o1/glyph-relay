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
The mutation endpoint accepts only a canonical two-field JSON envelope, five local session actions, and at most 4 KiB.
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

The browser verifier loads the launch URL, confirms the fragment disappears, renders sender state, performs an authorized pause, observes the updated controls, and asserts that no request contains the nonce or leaves the loopback origin.
