# Recording profile qualification

GlyphRelay currently carries `recording_profile_candidate_v1` while Milestone 0 browser qualification remains deferred.

The candidate's canonical SHA-256 identity is `7a8d11250e043c52b7089a375485bef3916414c6d81d6f1350f53dbcc56b04e1`.

This identity is not the final `recording_profile_v1` hash.

The project must not freeze or advertise the final profile until the exact pinned Chromium and Firefox offers pass the predicate below and both browsers decode the corresponding NVENC stream.

## Candidate contract

The semantic pixel contract is 8-bit 4:2:0, limited range, with BT.709 color primaries, transfer characteristics, and matrix coefficients.

NVENC receives one contiguous NV12 surface.

The system OpenH264 adapter receives I420 produced by an exact NV12-to-I420 planarization.

The candidate encoder contract uses H.264 Constrained Baseline, a maximum encoded level of 4.0, no B frames, a maximum 60-frame GOP, and SPS plus PPS before the first IDR and every later IDR.

The declared presentations are 1920x1080 at 30 or 24 frames per second and 1280x720 at 24 or 15 frames per second.

Strict 1920x1080 at 30 frames per second requires Level 4.0 because it contains 8,160 macroblocks per frame and 244,800 macroblocks per second.

## SDP predicate

An offer passes only when its video media line advertises an explicit `H264/90000` payload with `packetization-mode=1`, `level-asymmetry-allowed=1`, the RFC 6184 Constrained Baseline subprofile, and Level 4.0 or higher.

The parser bounds total and per-line input, ties attributes to payload identifiers advertised by the active video media line, rejects duplicate or malformed fields, and fails closed when any required parameter is absent.

Profile compatibility is semantic rather than raw-byte equality.

Both `42c0xx` and `42e0xx` represent compatible Constrained Baseline constraint patterns under the RFC 6184 masks used by the predicate.

A typical `42e01f` Constrained Baseline Level 3.1 offer is rejected because it cannot authorize the candidate's 1080p presentations.

This unresolved browser-offer conflict is a Milestone 0 kill-gate risk, not a passing result.

## System OpenH264 evidence

The Ubuntu 24.04 CPU path dynamically links `libopenh264-7=2.4.1+dfsg-1` and does not redistribute the codec binary.

The adapter configures the screen-content real-time mode, bitrate control with bounded frame skipping, a single temporal and spatial layer, single-slice CAVLC, one reference frame, Constrained Baseline Level 4.0, limited-range BT.709 VUI, and SPS plus PPS emission on every IDR.

It rejects malformed output, inconsistent reported sizes, IDRs without parameter sets, incompatible SPS metadata, invalid input layout, and oversized access units before exposing encoded data.

The pinned Ubuntu amd64 integration test encodes a record-only stream without signaling or a browser offer.

The strict in-process parser verifies the first and forced recovery access units, and the independently installed FFmpeg 6.1.1 decoder accepts the resulting elementary stream.

FFprobe reports H.264 Constrained Baseline, Level 4.0, 1920x1080, `yuv420p`, limited range, and BT.709 color metadata for that fixture.

## Remaining acceptance evidence

The final profile requires captured offers from the exact pinned Chromium and Firefox binaries for every presentation, a compatible NVENC SPS and stream, 60-second playback in both browsers, PLI recovery, and independent NVENC record-only decode.

Those target-only checks remain `DEFERRED_HARDWARE` and will be run by the consolidated qualification workflow.
