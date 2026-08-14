# ADR 0002: Browser level compatibility pivot

Status: accepted on 2026-08-13 after the exact pinned browser preflight.

## Context

The original plan required one Level 4.0 H.264 profile to cover both 1080p30 recording and every browser-sharing presentation.

The exact pinned Chromium 151.0.7922.34 binary produced an unmodified receive offer whose Constrained Baseline packetization-mode 1 format was `42e01f`, or Level 3.1.

The exact pinned Firefox 153.0 binary exposed no H.264 payload on the local preflight, and the target qualification must still determine whether its system OpenH264 GMP makes the selected live profile available there.

Level 3.1 permits 108,000 decoded macroblocks per second.

Strict 1920x1080 at 30 frames per second requires 244,800 macroblocks per second and therefore requires Level 4.0.

Rewriting the browser offer or claiming 1080p30 under a Level 3.1 SPS would not be valid negotiation evidence.

The Milestone 0 kill gate requires a product pivot when either target browser cannot consume the exact declared path.

## Decision

GlyphRelay retains 1080p30 and 1080p24 as record-only and deterministic evaluation presentations under Constrained Baseline Level 4.0.

Browser sharing uses only 720p30, 720p24, and 720p15 under Constrained Baseline Level 3.1 or a higher offered level.

The SDP predicate evaluates the exact selected sharing presentation instead of requiring a browser offer to authorize the highest record-only presentation.

Every sharing offer must still advertise packetization mode 1, level asymmetry, and the Constrained Baseline RFC 6184 subprofile.

A combined share-and-record session records the selected live presentation unless a future separately qualified dual-encoder design is introduced.

The 1080p30 NVENC capability, fixed-map quality, latency, recorder-throughput, and CUDA preprocessing gates remain unchanged.

## Consequences

The public live-sharing maximum is 1280x720 at 30 frames per second for V1.

No live 1080p claim is permitted.

The target Firefox offer and real 60-second decode path remain mandatory gates.

The profile candidate receives a new canonical identity and remains unfrozen until both target browsers pass the selected 720p30 contract and decode the emitted stream.

## Evidence

- `build/m0-browser-offers.json` captured the exact pinned Chromium and Firefox offers without SDP rewriting.
- Chromium advertised Constrained Baseline Level 3.1 with packetization mode 1 and level asymmetry.
- RFC 6184 level limits and H.264 macroblock arithmetic make the original 1080p30 receive contract incompatible with that offer.
- ADR 0001 had already classified this exact conflict as an unresolved Milestone 0 kill-gate risk.
