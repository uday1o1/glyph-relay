# Self-hosted signaling

GlyphRelay ships the static browser receiver and the ephemeral signaling service as one single-tenant Node.js bundle.

The service relays only bounded SDP, ICE, heartbeat, and lifecycle messages.

It never accepts source frames or media payloads.

## Security model

Each session has an independently generated owner capability and one single-use join capability.

The service stores keyed, domain-separated hashes instead of the raw capabilities.

The owner capability is valid only on the WebSocket connection generation that created it.

The join capability expires after ten minutes, authorizes only a receiver reservation, and is destroyed on first successful use.

The browser receives the join capability in the URL fragment.

Fragments are not sent in HTTP request targets.

The receiver removes the fragment with `history.replaceState` before it opens the signaling WebSocket.

The service validates the exact configured Host and Origin on every WebSocket upgrade.

It rejects insecure non-loopback binds, binary messages, payloads larger than 64 KiB, more than 20 signaling messages per connection per second, additional receivers, stale sequences, stale connection generations, and unknown fields.

Health responses contain only a status and protocol version.

They never contain session identifiers.

## Required configuration

A remote deployment requires one public HTTPS origin with a certificate trusted by sender and receiver devices.

The Node service terminates TLS directly in V1.

Set these variables without a trailing slash on the public origin:

```text
GLYPHRELAY_BIND_HOST=0.0.0.0
GLYPHRELAY_PORT=8443
GLYPHRELAY_SIGNALING_ORIGIN=https://share.example.com:8443
GLYPHRELAY_TLS_CERT_PATH=/run/secrets/tls.crt
GLYPHRELAY_TLS_KEY_PATH=/run/secrets/tls.key
```

Set `GLYPHRELAY_HEALTHCHECK_CA_PATH` to a read-only CA certificate path when the public certificate chains to a private CA that is not in the Node.js system trust store.

`GLYPHRELAY_CAPABILITY_HASH_KEY` may contain a canonical base64url value of at least 32 random bytes.

If it is omitted, the service generates an ephemeral key at startup.

An ephemeral key is safe because all sessions are memory-only and are intentionally lost on restart.

`GLYPHRELAY_MAX_SESSIONS` defaults to 64.

`GLYPHRELAY_MAX_CONNECTIONS` defaults to 128.

## Container workflow

Build the pinned Linux amd64 image from the repository root:

```bash
docker build --platform linux/amd64 --file containers/signaling.Dockerfile --tag glyphrelay-signaling:local .
```

Run it with a read-only filesystem, a dropped capability set, and read-only certificate mounts:

```bash
docker run --rm --read-only --cap-drop ALL --security-opt no-new-privileges \
  --publish 8443:8443 \
  --env GLYPHRELAY_BIND_HOST=0.0.0.0 \
  --env GLYPHRELAY_PORT=8443 \
  --env GLYPHRELAY_SIGNALING_ORIGIN=https://share.example.com:8443 \
  --env GLYPHRELAY_TLS_CERT_PATH=/run/secrets/tls.crt \
  --env GLYPHRELAY_TLS_KEY_PATH=/run/secrets/tls.key \
  --mount type=bind,src=/absolute/path/fullchain.pem,dst=/run/secrets/tls.crt,readonly \
  --mount type=bind,src=/absolute/path/privkey.pem,dst=/run/secrets/tls.key,readonly \
  glyphrelay-signaling:local
```

The image runs as the unprivileged `node` user and has a container health check for `/healthz`.

The primary browser URL is the exact public origin.

The sender creates the fragment-bearing join URL only after it has established the authenticated owner WebSocket and explicitly requested a new join capability.

## Native owner client

The native sender client uses the exact configured HTTPS origin as the WebSocket `Origin` header and connects to its corresponding WSS endpoint.

Unencrypted WS is accepted only for canonical literal IPv4 or IPv6 loopback origins.

Remote origins require HTTPS, certificate verification is always enabled, and a private CA file may be configured without disabling hostname verification.

The client rejects noncanonical origins, credentials, paths, query strings, fragments, header injection characters, binary messages, messages larger than 64 KiB, JSON comments, duplicate JSON keys, unknown fields, stale sequences, session swaps, and forged join URLs.

The owner capability stays in native process memory, is excluded from events and diagnostics, and is overwritten when the session stops or fails.

Serialized authenticated messages and received signaling buffers are also overwritten after use.

The client responds to server heartbeats and independently closes the transport after five seconds without valid server activity.

It does not reconnect or rebind an owner session because the service binds the capability to the WebSocket connection generation that created it.

After a receiver disconnects, the live owner must explicitly request a new single-use link.

## Receiver control channel

The peer connection uses one ordered and reliable data channel labeled `glyphrelay-control-v1`.

Both directions use exact versioned JSON schemas, the signaling session identifier, and a strictly increasing per-direction sequence.

Receiver-origin messages are limited to 4 KiB and ten messages per rolling second.

The sender accepts only bounded clock responses, cumulative receiver statistics, matching lifecycle acknowledgments, and protocol errors.

Keyboard, mouse, clipboard, file, command, arbitrary application, stale-sequence, wrong-session, duplicate-key, comment, oversized, flood, and regressing-telemetry inputs fail the control session closed.

The clock protocol permits an initial five-sample burst and then no more than one sender request every five seconds.

Pause, resume, and end acknowledgments must name the exact pending sender message sequence.

Resume binds both a new media epoch and a new dependency epoch before media can be admitted.

## Local verification

Loopback HTTP and WS are allowed only on the literal loopback bind addresses for automated tests and development.

Run the signaling state and live server suite with:

```bash
corepack pnpm test
```

The live tests exercise the public HTTP and WebSocket routes, capability consumption, receiver replacement, connection cleanup, hostile Host and Origin values, oversized messages, and rate floods.

The pinned native transport verification exercises the owner state machine against the real service over both loopback WS and certificate-verified WSS:

```bash
make transport-check
```

Verify a running HTTPS bundle through its real public route with:

```bash
corepack pnpm run signaling:verify -- --origin https://share.example.com:8443
```

Add `--ca /path/to/private-ca.crt` only when the deployment uses a private CA.

No real public deployment, DNS change, certificate issuance, or external publication is performed by repository verification.

## Lifecycle limits

The service uses monotonic deadlines.

An owner-only session expires after 15 minutes.

A join capability expires after ten minutes.

A receiver reservation must establish the peer within 30 seconds.

An ICE-disconnected receiver has exactly five seconds to recover while its authenticated signaling connection remains open.

Both roles exchange heartbeats every two seconds and fail after five seconds without valid peer activity.

The absolute session lifetime is eight hours and cannot be extended by a join rotation, receiver replacement, ICE restart, pause, or heartbeat.

An owner WebSocket close or protocol failure revokes the entire session.

A receiver signaling close ends that peer and requires the live owner to create a new single-use link.
