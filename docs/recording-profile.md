# Recording profile qualification

GlyphRelay currently carries `recording_profile_candidate_v1` while Milestone 0 browser qualification remains deferred.

The candidate's canonical SHA-256 identity is `d5e05bae2645f774665675bca1230c9320f5978b3f11fe0e3b6be1c8f1746d63`.

This identity is not the final `recording_profile_v1` hash.

The project must not freeze or advertise the final profile until the exact pinned Chromium and Firefox offers pass the predicate below and both browsers decode the corresponding NVENC stream.

## Candidate contract

The semantic pixel contract is 8-bit 4:2:0, limited range, with BT.709 color primaries, transfer characteristics, and matrix coefficients.

NVENC receives one contiguous NV12 surface.

The system OpenH264 adapter receives I420 produced by an exact NV12-to-I420 planarization.

The candidate encoder contract uses H.264 Constrained Baseline, no B frames, a maximum 60-frame GOP, and SPS plus PPS before the first IDR and every later IDR.

Record-only and deterministic evaluation presentations include 1920x1080 at 30 or 24 frames per second under Level 4.0 and 1280x720 at 30, 24, or 15 frames per second under Level 3.1.

Browser-sharing presentations are 1280x720 at 30, 24, or 15 frames per second under Level 3.1.

Strict 1920x1080 at 30 frames per second requires Level 4.0 because it contains 8,160 macroblocks per frame and 244,800 macroblocks per second.

## SDP predicate

An offer passes only when its video media line advertises an explicit `H264/90000` payload with `packetization-mode=1`, `level-asymmetry-allowed=1`, the RFC 6184 Constrained Baseline subprofile, and a level sufficient for the selected browser-sharing presentation.

The parser bounds total and per-line input, ties attributes to payload identifiers advertised by the active video media line, rejects duplicate or malformed fields, and fails closed when any required parameter is absent.

Profile compatibility is semantic rather than raw-byte equality.

Both `42c0xx` and `42e0xx` represent compatible Constrained Baseline constraint patterns under the RFC 6184 masks used by the predicate.

A typical `42e01f` Constrained Baseline Level 3.1 offer is accepted for the selected 720p sharing presentations and cannot authorize a 1080p share.

ADR 0002 records the browser-level kill-gate pivot and forbids a live 1080p V1 claim.

The exact pinned local-browser preflight on 2026-08-13 observed Chromium Constrained Baseline only at Level 3.1 and no explicit H.264 format from Firefox.

The post-pivot local probe accepts Chromium's exact Level 3.1 offer for 720p30 and remains truthfully `INCOMPATIBLE` because Firefox exposed no explicit H.264 format.

The corrected candidate remains unfrozen until the target browser run passes.

See [browser-interoperability.md](browser-interoperability.md) for the reproducible workflow and the boundary between local preflight and target evidence.

## System OpenH264 evidence

The Ubuntu 24.04 CPU path dynamically links `libopenh264-7=2.4.1+dfsg-1` and does not redistribute the codec binary.

The adapter configures the screen-content real-time mode, bitrate control with bounded frame skipping, a single temporal and spatial layer, single-slice CAVLC, one reference frame, Constrained Baseline Level 4.0, limited-range BT.709 VUI, and SPS plus PPS emission on every IDR.

It rejects malformed output, inconsistent reported sizes, IDRs without parameter sets, incompatible SPS metadata, invalid input layout, and oversized access units before exposing encoded data.

The pinned Ubuntu amd64 integration test encodes a record-only stream without signaling or a browser offer.

The strict in-process parser verifies the first and forced recovery access units, and the independently installed FFmpeg 6.1.1 decoder accepts the resulting elementary stream.

FFprobe reports H.264 Constrained Baseline, Level 4.0, 1920x1080, `yuv420p`, limited range, and BT.709 color metadata for that fixture.

## Remaining acceptance evidence

The final profile requires captured offers from the exact pinned Chromium and Firefox binaries for the selected sharing presentations, a compatible NVENC SPS and stream, 60-second playback in both browsers, PLI recovery, and independent NVENC record-only decode.

Those target-only checks remain `DEFERRED_HARDWARE` and will be run by the consolidated qualification workflow.
