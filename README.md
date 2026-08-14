# GlyphRelay

GlyphRelay is an in-development local-first, text-aware screen-sharing and recording system for constrained networks.

The implementation follows the acceptance gates in [BUILD_PLAN.md](BUILD_PLAN.md).

No GPU, browser interoperability, quality, latency, or bitrate claim is accepted yet.

Current evidence and deferred gates are recorded in [docs/implementation-status.md](docs/implementation-status.md).

The Linux durable recording protocol, failure behavior, and verification commands are documented in [docs/recording.md](docs/recording.md).

The single-tenant receiver and signaling deployment, security model, and TLS workflow are documented in [docs/signaling.md](docs/signaling.md).

## Foundation workflow

The portable foundation requires Node.js 24, Corepack, `uv`, Apple Clang or Clang, and Make.

Project tools and dependencies remain local to the checkout.

```bash
corepack pnpm install --frozen-lockfile
uv sync --locked
make check
```

Run the real environment diagnostic with:

```bash
build/macos-local/glyphrelay doctor
build/macos-local/glyphrelay doctor --json
```

On Linux, the executable is under `build/linux-cpu` for the portable preset.

The command reports unsupported or unverified capabilities instead of fabricating a passing hardware result.

The stable fields, probe semantics, decision modes, and safe-attachment rules are documented in [docs/doctor.md](docs/doctor.md).

The frozen Milestone 0 benchmark input can be verified through the public workflow:

```bash
build/macos-local/glyphrelay benchmark \
  --manifest protocols/m0_fixed_map_v1/manifest.lock \
  --output build/m0-result
```

The command hashes every generated frame and every protocol component before checking hardware.

It exits with unsupported-capability code 3 on a host that cannot run the NVENC comparison and does not create a result directory there.

Run the pinned Linux x86-64 portable compile and test path with:

```bash
make linux-cpu-check
```

On a supported Linux desktop, start the portal-owned record-only path with:

```bash
build/linux-cpu/glyphrelay record --output recording.h264 --bitrate 2m
```

The portal dialog is the only source selector.

Press Ctrl-C to stop and durably publish the elementary stream, JSON sidecar, and completion marker.

Inspect either a completed or interrupted recording with:

```bash
build/linux-cpu/glyphrelay inspect --recording recording.h264 --json
```

The optional `--window-label` is displayed locally only after portal selection and is never written to the recording metadata.

The accepted bitrate profiles are `500k`, `1m`, `2m`, and `4m`.

This CPU path currently uses the unfrozen recording-profile candidate, so its final profile claim remains deferred with the Milestone 0 browser and NVENC qualification gates.

The Linux CPU preset also builds the exact patched WebRTC transport required by the public share path.

Configure the single-tenant signaling origin outside the command line, then start sharing through the portal-owned source selector:

```bash
export GLYPHRELAY_SIGNALING_ORIGIN=https://share.example.invalid
build/linux-cpu/glyphrelay share --bitrate 2m
```

A private development certificate authority can be supplied with `GLYPHRELAY_SIGNALING_CA_PATH`.

The command never accepts a source identifier or signaling-origin override.

It prints the URL-fragment join link only after the signaling service creates the single-use join capability.

Add `--record recording.h264` to start the durable 720p live-profile recording before a receiver joins.

Live-only sharing defers capture and CPU encoder initialization until a compatible receiver is ready.

The pinned Linux verification exercises portal selection ordering, system OpenH264 Level 3.1 output, immutable recorder and transport fanout, bounded queues, the native owner WSS client, real loopback DTLS-SRTP transport, durable publication, sanitizer cleanup, and independent FFmpeg decode.

The real portal-selected browser presentation remains a designated-target acceptance gate and is not inferred from those local integrations.

The local dashboard is a loopback-only, fragment-authorized control boundary with exact Host and Origin validation, a separate CSRF token, bounded canonical action messages, and no cookies or CORS.
The standalone process intentionally reports that no sender backend is available instead of simulating a live session.

```bash
corepack pnpm dashboard:start
make dashboard-browser-check
```

See [the local dashboard security contract](docs/dashboard.md) for endpoint and verification details.

The frozen generated screen-content corpus contains 64 development sequences, 64 unopened validation sequences, and a seedless final-test generation commitment.

Verify byte-for-byte manifest regeneration and the development lossless OCR floor with:

```bash
make corpus-regeneration-check
make corpus-lossless-check
```

The current development evidence covers 20,480 visible glyph instances, including 5,120 instances whose tight rendered height is 8 to 10 pixels, and measures 0.0 bounded CER for both overall and small-glyph aggregates.

No validation or final-test pixels were opened to obtain that result.

See [the corpus protocol](docs/corpus-protocol.md) for the renderer, fonts, truth ontology, OCR pipeline, metric, immutable split rules, and claim boundaries.

The portable `saliency_v1` correctness oracle and bounded preprocessing ownership rings can be exercised through the real preview path:

```bash
build/macos-local/glyphrelay_saliency_preview --output build/saliency-preview.ppm
```

The command writes a local protected-region overlay without overwriting an existing file.

See [the protected-region saliency contract](docs/saliency.md) for feature semantics, ownership transitions, verification, and current claim boundaries.

Exercise the static loopback receiver in the exact pinned Chromium build with:

```bash
make browser-harness-check
```

Capture and evaluate the unmodified pinned Chromium and Firefox offers with:

```bash
make browser-probe
```

The offer probe currently exits nonzero on the measured local platform because Chromium offers only Level 3.1 and Firefox exposes no H.264 payload.

That result is retained as a failed preflight rather than presented as interoperability.

The receiver, probe result states, and unfrozen browser-oracle workflow are documented in [docs/browser-interoperability.md](docs/browser-interoperability.md).

The target workflow generates its 720p30 browser input through the direct NVENC `browser-fixture` command and independently decodes all 2,100 access units before playback qualification begins.

The exact fixture and evidence contract is documented in [docs/browser-interoperability.md](docs/browser-interoperability.md#nvenc-playback-fixture).

Target qualification runs the exact two-browser zero-loss, PLI, and RTP-rollover matrix and validates four independently decoded oracle frames per run before accepting browser evidence.

CUDA, NVENC, XDG portal, PipeWire, browser, network, and performance acceptance remains deferred to the consolidated target qualification workflow required by the build plan.

The content-addressed source bundle and resumable qualification runner can be exercised without a GPU through:

```bash
make handoff-check
```

The final designated-workstation workflow uses one stable command after all local work is ready:

```bash
./scripts/gpu/qualify_cuda_pm.sh
```

That command performs safe synchronization, detached execution, polling, result retrieval, and hash verification.

It exits nonzero for `BLOCKED` or `FAILED` evidence and does not turn either state into an accepted hardware claim.

The workflow, security boundaries, resume behavior, artifacts, and exit codes are documented in [docs/qualification.md](docs/qualification.md).

## License

Original GlyphRelay source is available under the MIT License.

Pinned and patched third-party files retain their upstream licenses and notices.
