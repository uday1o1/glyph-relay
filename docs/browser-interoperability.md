# Browser interoperability

GlyphRelay treats browser interoperability as a measured gate and never infers it from a codec name or a Playwright revision.

The machine-readable dependency lock pins Playwright 1.62.1, Chromium revision 1234 at version 151.0.7922.34, and Firefox revision 1538 at version 153.0.

The qualification report also hashes each executable because a revision label alone is not a binary identity.

## Loopback receiver

The Milestone 0 receiver and signaling exchange bind to the literal address `127.0.0.1` and do not create a remotely usable link.

The HTTP server validates the actual bound address, loopback peer address, exact Host header, and exact Origin on every signaling write.

It serves only three repository-owned static assets with a restrictive content security policy, `Referrer-Policy: no-referrer`, no analytics, and no third-party requests.

An offer is stored only after the strict `recording_profile_candidate_v1` SDP predicate accepts it for the selected 720p30 sharing presentation.

An incompatible offer receives status 422 with a stable reason and is never rewritten.

The receiver uses a receive-only transceiver, packetization-mode 1 H.264 preferences, no ICE servers, a bounded 30-second answer poll, and the ordered `glyphrelay-control-v1` data channel.

Presented frames are captured only from `requestVideoFrameCallback`, keyed by RTP timestamp, and retained in a 16 MiB ring so 1080p capture cannot grow without bound.

Receiver statistics are emitted at most once per second and messages larger than 4 KiB are not sent.

Run the real Chromium receiver workflow with:

```bash
make browser-harness-check
```

The check opens the actual static page, verifies the security headers and fragment removal, exercises the connect button, observes the user-visible terminal state, and rejects any request outside the loopback origin.

## Offer probe

Run the exact two-browser offer gate with:

```bash
make browser-probe
```

The command writes `build/m0-browser-offers.json`, which is intentionally ignored because it contains host paths and local binary identities.

Exit code 0 means both exact binaries match their locks and both unmodified offers pass the candidate predicate.

Exit code 4 means the capture succeeded but at least one offer is incompatible.

Exit code 5 means browser launch, version identity, or capture infrastructure failed.

An infrastructure failure is never converted into compatibility evidence.

The target phase passes the captured report through `tools/validate_m0_browser_offers.py` before any NVENC playback phase starts.

That independent validator applies the strict schema, rechecks the locked package and browser identities, requires exactly Chromium and Firefox, verifies the derived RFC 6184 profile bytes, and requires NACK plus PLI feedback on a compatible packetization-mode 1 format.

## Current local preflight observation

The 2026-08-13 macOS arm64 preflight launched both exact pinned browser versions and produced a valid report with status `INCOMPATIBLE`.

Chromium advertised Constrained Baseline `42e01f` with packetization mode 1 and level asymmetry, which is Level 3.1 rather than the Level 4.0 required for the strict 1920x1080 at 30 frames per second candidate.

Firefox advertised no explicit H.264 payload in its offer.

The static Chromium receiver therefore reached its visible failure state after the server rejected the incompatible offer with status 422.

This preflight is a real failed observation, but it does not substitute for the required Linux GPU target run because Firefox H.264 availability depends on its separately delivered OpenH264 GMP and platform configuration.

Mozilla's current WebRTC debugging documentation describes H.264 support as a third-party GMP that is downloaded on first request and may not be available in every environment.

The current Firefox source also defaults its H.264 level preference to 31.

See the [Firefox WebRTC debugging documentation](https://firefox-source-docs.mozilla.org/contributing/debugging/debugging_webrtc_calls.html) and the [current Firefox H.264 preference source](https://searchfox.org/mozilla-central/search?q=media.navigator.video.h264.level&path=).

ADR 0002 retains 1080p30 for record-only and evaluation use, caps V1 browser sharing at 720p30, and requires a fresh target offer and decode run before freezing the profile.

The post-pivot local rerun accepted Chromium's exact unmodified offer for 720p30 and still found no explicit H.264 payload in Firefox.

The report therefore remains truthfully `INCOMPATIBLE` until target-side Firefox GMP qualification passes.

No profile has been frozen and no two-browser compatibility claim is accepted from either local result.

## NVENC playback fixture

The target qualification creates its browser input with the public `glyphrelay browser-fixture` command instead of substituting an unrelated software-encoded stream.

The producer derives a deterministic 1280 by 720 NV12 sequence from the immutable `m0_fixed_map_v1` source through an exact 3:2 nearest-neighbor transform.

The three retained source identities are frames 0, 300, and 2099 so a changed transform fails before browser evidence is accepted.

The direct NVENC session uses H.264 Constrained Baseline Level 3.1, 30 frames per second, a 60-frame GOP, repeated SPS and PPS, limited-range BT.709 colorimetry, no B frames, no AQ, and no emphasis map.

The first shared access unit is frame 300, which is an IDR carrying SPS and PPS because the warmup and GOP boundaries coincide.

The producer writes all 2,100 access units and their exact byte boundaries before FFmpeg independently decodes the complete elementary stream.

The validator then rehashes the stream, frame table, and effective NVENC configuration, recalculates elementary-stream payload bitrate, checks every frame identity, and uses FFprobe to require exactly 2,100 decoded 720p Level 3.1 limited-range BT.709 frames.

On the qualified Linux GPU target, the standalone workflow is:

```bash
build/linux-gpu/glyphrelay browser-fixture \
  --manifest protocols/m0_fixed_map_v1/manifest.lock \
  --output build/m0-nvenc-browser-fixture
uv run python tools/validate_m0_browser_fixture.py \
  build/m0-nvenc-browser-fixture \
  schemas/m0-browser-fixture-v1.schema.json
```

An unsupported local host verifies the frozen manifest, exits with code 3, and does not create the requested output directory.

Passing local source, schema, and compile-contract tests do not substitute for the target NVENC encode and decode phase.

## Browser oracle

`protocols/browser_oracle_v1/candidate.json` defines the unfrozen oracle protocol.

The frame identity is the pair of dependency epoch and unambiguous extended RTP timestamp.

The comparator rejects mismatched identity, geometry, or RGBA size and reports maximum channel error, differing-pixel fraction, and channel RMSE against independent-decoder output.

The freeze tool requires exactly ten complete zero-loss runs, split as five Chromium and five Firefox runs, with identical complete frame sets and zero decoder errors.

It permits no infrastructure exclusions and writes the frozen tolerance with no-clobber semantics.

Run it only after target playback has produced the complete observation bundle:

```bash
corepack pnpm run browser:oracle:freeze -- \
  --input artifacts/gpu-runs/browser-oracle-zero-loss.json \
  --output artifacts/gpu-runs/browser-oracle-frozen.json
```

Loss, PLI, rollover, and recovery tests must consume that frozen artifact and may not update it after seeing their results.

The playback harness retains exactly four target frames at 40, 50, 60, and 70 percent of the sent-frame sequence.
Each browser frame is joined to the sender trace by its low 32-bit RTP timestamp and then identified by the unambiguous extended RTP timestamp and dependency epoch.
The harness independently decodes the corresponding elementary-stream source frames with the pinned FFmpeg input contract and compares browser RGBA output against those references.
The sender must remain alive until all four target frames are retained so a relaxed recovery frame-count threshold cannot produce a premature or incomplete oracle sample.

The complete target matrix contains exactly eighteen runs.
It runs five zero-loss observations in each exact browser, freezes the tolerance from those ten runs, and then evaluates one PLI recovery and loss at extended sequences 65,534, 65,535, and 65,536 in each browser against the frozen tolerance.
Every run sends 1,800 frames at 30 fps and requires all four oracle comparisons, zero corrupted browser frames, the exact locked browser binary, and scenario-specific sender evidence.
The PLI runs begin at source frame 240 and inject feedback after 901 sent frames so the next SPS, PPS, and IDR access unit remains within the fixed 2,100-frame fixture.
The zero-loss and rollover runs begin at source frame 300.

Run the complete matrix only with the target-generated NVENC fixture:

```bash
corepack pnpm run browser:matrix -- \
  --answerer build/transport-contract/glyphrelay_m0_webrtc_sender \
  --fixture build/m0-nvenc-browser-fixture \
  --output artifacts/gpu-runs/browser-matrix
uv run python tools/validate_m0_browser_matrix.py \
  artifacts/gpu-runs/browser-matrix \
  build/m0-nvenc-browser-fixture \
  --schemas schemas
```

The matrix tool creates its output directory with no-clobber semantics and stops on the first failed or incomplete run.
The target qualification runner retains every completed run and its failure state in the private evidence archive.
