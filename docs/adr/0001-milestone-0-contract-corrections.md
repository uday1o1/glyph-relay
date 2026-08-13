# ADR 0001: Milestone 0 contract corrections

Status: accepted on 2026-08-13 before implementation or measurement.

## Context

`BUILD_PLAN.md` is the implementation authority, but it permits the smallest defensible correction when current authoritative documentation proves a detail obsolete or impossible.

The Milestone 0 dependency audit selected the Ubuntu 24.04 system OpenH264 2.4.1 package and libdatachannel v0.24.1 with bundled libjuice.

OpenH264 2.4.1 exposes NV12 for decoder output but its encoder accepts I420 input.

The original command-line contract referred to one NV12 color contract for both the NVENC and OpenH264 paths.

Pinned libjuice v0.24.1 reaches the final UDP syscall through `src/udp.c::udp_sendto`, which invokes `sendto` rather than `sendmsg`.

The original privacy contract named `sendmsg`, although its required property is serialization around the actual final nonblocking UDP socket-send boundary.

## Decision

`recording_profile_v1` freezes semantic 8-bit 4:2:0 colorimetry and range independently from memory layout.

The NVENC adapter uses NV12 and the OpenH264 adapter uses I420.

Both layouts must be derived from the same declared color transform and must produce equivalent decoded color within their frozen tolerance.

The privacy and accounting contract names the final nonblocking UDP socket-send operation.

For libjuice v0.24.1 that operation is `sendto`.

The egress gate remains held from epoch validation through return from that call, and failed or short calls add no egress bytes.

Any transport upgrade must relock the exact source and syscall boundary before qualification.

## Consequences

The OpenH264 adapter requires a tested NV12-to-I420 planarization step or a common conversion path that emits both layouts.

The libdatachannel integration requires an isolated MPL-2.0 patch because its public API does not expose post-TURN final datagrams or transport-generated control datagrams.

No quality, privacy, accounting, or latency acceptance threshold is weakened.

The real-browser offer risk remains unresolved.

Current Chromium and Firefox WebRTC sources commonly represent Constrained Baseline Level 3.1, while strict 1080p30 requires Level 4.0.

Milestone 0 must capture exact real offers and apply the existing browser kill gate rather than assume compatibility or rewrite an offer.

## Evidence

- NVIDIA Video Codec SDK 13.1 programming guide, including emphasis maps and Linux synchronous output.
- OpenH264 v2.4.1 `CWelsH264SVCEncoder::EncodeFrame` input-format validation.
- libdatachannel v0.24.1 commit `a02b751917ac8afc8c58dc6f4461d25ff9465d48`.
- Bundled libjuice commit `5948a4162d37bc213d6051b67ee2876ccc5a99a6`, including `src/udp.c::udp_sendto`.
- RFC 6184 profile-level negotiation rules.
