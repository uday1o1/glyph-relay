# Implementation status

This file is the repository-owned acceptance and evidence tracker for `BUILD_PLAN.md`.

It records implementation separately from qualification so locally verified work is never mistaken for accepted target evidence.

## State model

A local work package is `PENDING_LOCAL` or `IMPLEMENTED_LOCAL`.

A target-only gate is `DEFERRED_HARDWARE`, `BLOCKED`, `FAILED`, or `ACCEPTED`.

A milestone is `IN_PROGRESS`, `WAITING_HARDWARE`, `FAILED`, or `ACCEPTED`.

Only `ACCEPTED` completes a milestone.

Milestone 7 remains outside the required portfolio release set and cannot be labeled skipped or accepted without its measured gate.

Every verified commit must pass the repository audit, be pushed immediately, and have its local hash matched against the remote branch before later work starts.

## Initial evidence snapshot

| Evidence | Observation |
| --- | --- |
| Repository | Unborn `main` containing only the authoritative plan before foundation work |
| Original plan SHA-256 | `22838301958a0513fd32a74b1edffab8da18824856faa680b3d7dc94993e8a44` before ADR 0001 corrections |
| Local host | macOS arm64 with Apple Clang 21 |
| Local NVIDIA support | No CUDA compiler, NVIDIA management tool, or NVIDIA GPU |
| Local Docker | Available Linux arm64 daemon with tested `linux/amd64` emulation |
| Local language tools | Node 24, Corepack, uv, Python, and FFmpeg available |
| Missing host tools | CMake, Ninja, pnpm, pkg-config, Tesseract, OpenH264, Firefox, clang-format, clang-tidy, ShellCheck, gitleaks, coturn, and packet capture are absent from `PATH` |
| Target state | All target-only gates are `DEFERRED_HARDWARE` pending the consolidated qualification workflow |
| Architectural risk | Exact Chromium and Firefox offers may not authorize the Level 4.0 required by strict 1080p30 |

## Milestone summary

| Milestone | State | Reason |
| --- | --- | --- |
| 0 | `IN_PROGRESS` | M0-L01 is locally implemented and all target gates are deferred |
| 1 | `IN_PROGRESS` | Local work packages remain pending |
| 2 | `IN_PROGRESS` | Local work packages remain pending |
| 3 | `IN_PROGRESS` | Local work packages remain pending |
| 4 | `IN_PROGRESS` | Local work packages and irreversible freezes remain pending |
| 5 | `IN_PROGRESS` | Local work packages remain pending |
| 6 | `IN_PROGRESS` | Local work packages remain pending |
| 7 | `IN_PROGRESS` | Profiling precondition and local experiment remain pending, but this milestone does not block the core release |
| 8 | `IN_PROGRESS` | Frozen evaluation work and target gates remain pending |
| 9 | `IN_PROGRESS` | Qualification, documentation, audit, and release gates remain pending |

## Milestone 0 local work packages

| ID | State | Deliverable | Evidence |
| --- | --- | --- | --- |
| M0-L01 | `IMPLEMENTED_LOCAL` | Reproducible repository foundation, CI, truthful doctor, lockfiles, checks, and clean-check workflow | `make check`; `make linux-cpu-check`; `scripts/ci/check_clean_tree.sh`; real text and JSON doctor invocation |
| M0-L02 | `IMPLEMENTED_LOCAL` | Exact dependency, API, build-flag, ABI, and license locks | `make dependency-check`; pinned Ubuntu amd64 package hash and SONAME inspection; `THIRD_PARTY_NOTICES.md` |
| M0-L03 | `IMPLEMENTED_LOCAL` | Complete versioned doctor adapters, schema, redaction, and decision fixtures | Real text and JSON CLI inspection; full Draft 2020-12 validation; enhanced, uniform, CPU, API-incompatible, pending, unsupported, and redaction fixtures; macOS sanitizers; Linux amd64 compile and tests |
| M0-L04 | `PENDING_LOCAL` | Canonical `m0_fixed_map_v1` generator, manifests, metrics, strict hash enforcement, and runner | None |
| M0-L05 | `PENDING_LOCAL` | Annex B, SPS, SDP, recording-profile, OpenH264, and independent-decode contracts | None |
| M0-L06 | `PENDING_LOCAL` | CUDA context and NVENC feasibility source, ownership state machines, pre-submit rejection, and cleanup | None |
| M0-L07 | `PENDING_LOCAL` | Final datagram hook, classifier, egress gate, counter, and deterministic revocation race | Source audit only |
| M0-L08 | `PENDING_LOCAL` | RTP packetization, sole sequence owner, bounded NACK and PLI recovery, and rollover contracts | Source audit only |
| M0-L09 | `PENDING_LOCAL` | Loopback signaling, receiver, browser oracle, and browser pins | None |
| M0-L10 | `PENDING_LOCAL` | Local qualification phases, structured result schemas, documentation, and complete local verification | None |

## Milestone 0 target gates

| ID | State | Acceptance item | Evidence |
| --- | --- | --- | --- |
| M0-H01 | `DEFERRED_HARDWARE` | Target reports H.264 NVENC and emphasis-map support | None |
| M0-H01 | `DEFERRED_HARDWARE` | Compiled NVENC API is compatible with the driver's maximum API | None |
| M0-H01 | `DEFERRED_HARDWARE` | Wrong map size, frame, geometry, memory space, and foreign CUDA context fail before NVENC submission | None |
| M0-H01 | `DEFERRED_HARDWARE` | Every context worker exits before primary-context release | None |
| M0-H02 | `DEFERRED_HARDWARE` | Ten repeated 1080p30 one-minute fixed and controlled-uniform runs each measure 0.98 through 1.02 Mbps mean payload | None |
| M0-H02 | `DEFERRED_HARDWARE` | Fixed and controlled-uniform mean payload rates differ by no more than two percent | None |
| M0-H02 | `DEFERRED_HARDWARE` | Fixed map improves protected-region luma PSNR by at least 1.0 dB | None |
| M0-H02 | `DEFERRED_HARDWARE` | Fixed map improves protected-minus-unprotected PSNR difference by at least 0.75 dB | None |
| M0-H02 | `DEFERRED_HARDWARE` | Frozen source, mask, map, metric, and configuration hashes are enforced | None |
| M0-H02 | `DEFERRED_HARDWARE` | Configured targets, every measured point, and whole-frame and regional PSNR are retained | None |
| M0-H02 | `DEFERRED_HARDWARE` | Every stream decodes independently | None |
| M0-H03 | `DEFERRED_HARDWARE` | Post-warmup encode-to-bitstream latency is at most 10 ms P95 and 16 ms P99 in every steady run | None |
| M0-H03 | `DEFERRED_HARDWARE` | Pending submission count and age show no positive trend | None |
| M0-H03 | `DEFERRED_HARDWARE` | No steady submission remains pending longer than 33.34 ms | None |
| M0-H04 | `DEFERRED_HARDWARE` | Chromium and Firefox each display the NVENC stream for 60 seconds inside the frozen oracle tolerance | None |
| M0-H04 | `DEFERRED_HARDWARE` | Both browsers recover from PLI through an IDR carrying SPS and PPS without decoder error | None |
| M0-H04 | `DEFERRED_HARDWARE` | Emitted SPS profile and level match negotiated SDP with packetization mode 1 | None |
| M0-H04 | `DEFERRED_HARDWARE` | Both real offers pass `recording_profile_v1` for every V1 presentation profile | None |
| M0-H04 | `DEFERRED_HARDWARE` | NVENC and system OpenH264 record-only streams begin with SPS, PPS, and IDR without signaling | None |
| M0-H04 | `DEFERRED_HARDWARE` | No Milestone 0 service accepts a non-loopback connection | None |
| M0-H05 | `DEFERRED_HARDWARE` | NACK around sequence 65,535 resolves uniquely in the active epoch and preserves SRTP rollover | None |
| M0-H05 | `DEFERRED_HARDWARE` | Protected retransmission UDP payload is byte-identical and both browsers recover | None |
| M0-H05 | `DEFERRED_HARDWARE` | PLI and NACK floods remain within every cache, feedback, retransmission, and IDR limit | None |
| M0-H06 | `DEFERRED_HARDWARE` | Direct IPv4, supported IPv6, and loopback TURN-over-UDP datagrams cross the final hook exactly once with correct classification | None |
| M0-H06 | `DEFERRED_HARDWARE` | Deterministic counter totals equal packet-capture IP lengths exactly | None |
| M0-H06 | `DEFERRED_HARDWARE` | The stalled validation-to-final-UDP-send test proves linearizable media revocation without blocking control | None |
| M0-H07 | `DEFERRED_HARDWARE` | Elementary payload bitrate, wire egress, latency, cleanup, and sanitizer evidence is complete | None |
| M0-H07 | `DEFERRED_HARDWARE` | Target `make check` passes from a clean checkout | None |

## Milestone 1 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | A real portal-selected window streams to Chromium and Firefox | None |
| `PENDING_LOCAL` | Record-only produces an independently decoded profile stream without signaling or a browser offer | None |
| `DEFERRED_HARDWARE` | Capture cancel, close, revoke, and shutdown paths pass | None |
| `PENDING_LOCAL` | Every queue has an asserted bound and disconnects leak no session or frame resource | None |
| `PENDING_LOCAL` | Token replay, hostile Origin and Host, oversized messages, control floods, and insecure non-loopback binds fail closed | None |
| `PENDING_LOCAL` | Capability swaps, forged actions, impersonation, fixation, and every invalid transition fail closed without revealing capabilities | None |
| `PENDING_LOCAL` | Signaling closure, heartbeat, partition, ICE, reservation, join, and absolute timers follow exact transitions without extending absolute lifetime | None |
| `PENDING_LOCAL` | Stale owner generations and same-session owner reconnects fail closed after revocation | None |
| `PENDING_LOCAL` | Self-hosted HTTPS and WSS routing works and an unconfigured sender creates no remote link | None |
| `PENDING_LOCAL` | Development lossless OCR bounded error is at most 0.02 overall and 0.05 for 8-to-10-pixel glyphs | None |
| `PENDING_LOCAL` | Corpus and protocol validation passes without opening validation or final-test renderer output | None |

## Milestone 2 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `PENDING_LOCAL` | Scalar goldens and portable boundary and randomized differential tests pass | None |
| `DEFERRED_HARDWARE` | CUDA goldens, differential tests, boundary tests, and compute-sanitizer pass | None |
| `PENDING_LOCAL` | Saliency output is deterministic for the same frame sequence | None |
| `DEFERRED_HARDWARE` | Complete CUDA preprocessing and map copy is at most 5 ms P95 at 1080p30 | None |

## Milestone 3 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | Browser and independent decoders accept the enhanced stream | None |
| `DEFERRED_HARDWARE` | Multiple in-flight frames show no map or surface corruption | None |
| `PENDING_LOCAL` | Injected normal and fatal status simulations preserve slot ownership and order | None |
| `DEFERRED_HARDWARE` | Uniform and fixed maps reproduce the Milestone 0 quality shift | None |
| `PENDING_LOCAL` | Portable error and teardown stress tests pass | None |
| `DEFERRED_HARDWARE` | Target error and teardown stress tests pass | None |

## Milestone 4 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `PENDING_LOCAL` | Lossless validation bounded error is at most 0.02 overall and 0.05 for 8-to-10-pixel glyphs before compressed evidence | None |
| `PENDING_LOCAL` | Equal-stratum glyph macroblock recall is at least 90 percent overall and 80 percent for the small-glyph subset | None |
| `PENDING_LOCAL` | Equal-stratum protected macroblock fraction is at most 35 percent | None |
| `PENDING_LOCAL` | Equal-stratum false-protected fraction is at most 15 percent | None |
| `PENDING_LOCAL` | Equal-stratum static-scene map change after warmup is at most two percent | None |
| `PENDING_LOCAL` | Every map metric reports per-stratum values and P95 sequence values | None |
| `PENDING_LOCAL` | Theme, scroll, cursor, embedded-video, and small-font cases are included | None |
| `PENDING_LOCAL` | Failure scenes and manual correction are documented | None |
| `DEFERRED_HARDWARE` | Complete AQ grid and deterministic selector reproduce the winning target configuration without validation data | None |

## Milestone 5 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | P95 one-second wire egress remains within 110 percent of cap on the frozen primary matrix | None |
| `DEFERRED_HARDWARE` | Production egress count matches deterministic capture exactly and every steady run within one percent | None |
| `PENDING_LOCAL` | Unsupported IP headers, fragmentation, bypass sockets, and TURN TCP or TLS cannot enter verified-cap state | None |
| `DEFERRED_HARDWARE` | Stable link sustains at least 24 compositor fps, 95 percent delivery, 1080p, and at most 250 ms P95 latency | None |
| `PENDING_LOCAL` | Queues remain within hard bounds under sustained collapse | None |
| `DEFERRED_HARDWARE` | Recovery IDR and frame-age recovery complete within two seconds | None |
| `DEFERRED_HARDWARE` | Frame rate and resolution recover within `2 + 2 * N` and at most eight seconds | None |
| `PENDING_LOCAL` | Every production controller trace replays byte-for-byte without future feedback | None |
| `DEFERRED_HARDWARE` | Dependency reset produces no corruption or post-IDR freeze longer than one second | None |
| `PENDING_LOCAL` | Pin preservation and rate violations are surfaced truthfully | None |

## Milestone 6 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `PENDING_LOCAL` | Privacy and threat-model tests pass with no unresolved critical or high finding | None |
| `DEFERRED_HARDWARE` | No RTP or SRTP crosses revocation and receiver clearing completes within 250 ms | None |
| `PENDING_LOCAL` | Deterministic egress race is linearizable for stop, pause, lock, revocation, and permission loss | None |
| `DEFERRED_HARDWARE` | Screen lock revokes every capability and transport resource and cannot resume the session | None |
| `PENDING_LOCAL` | Owner signaling loss revokes both sides, closes media, clears receiver, and rejects stale or rebound owners | None |
| `PENDING_LOCAL` | Pause preserves signaling and expiry, emits no late media, clears receiver, and resumes only after acknowledgment and recovery IDR | None |
| `PENDING_LOCAL` | Missing acknowledgments, transition closure, bad epochs, and expiry end the peer without reviving media | None |
| `PENDING_LOCAL` | Every injected recorder crash yields a marker-verified complete or journal-anchored inspectable incomplete recording | None |
| `PENDING_LOCAL` | Filesystem crash model proves initial preparation and publication durability order | None |
| `DEFERRED_HARDWARE` | Ten-minute 1080p30 maximum-profile recording meets queue, commit, and decode gates on named storage | None |
| `PENDING_LOCAL` | Existing and symbolic-link outputs remain unchanged and incomplete outputs cannot be replaced | None |
| `PENDING_LOCAL` | Unsupported environments fail with actionable doctor output | None |
| `PENDING_LOCAL` | Logs, crash fixtures, and telemetry contain no window content or titles | None |
| `DEFERRED_HARDWARE` | Clean install instructions work on the tested environment | None |

## Milestone 7 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | Nsight proves whether DMA-BUF removes a full-frame copy and whether synchronization replaces its cost | None |
| `DEFERRED_HARDWARE` | Correctness remains unchanged for each retained DMA-BUF path | None |
| `DEFERRED_HARDWARE` | Ten paired runs show at least 5 ms P95 latency reduction or 10 percent CPU reduction without regressions before release enablement | None |
| `PENDING_LOCAL` | Failed improvement leaves DMA-BUF disabled with an honest documented result | None |
| `DEFERRED_HARDWARE` | Unsupported modifiers fall back safely | None |

## Milestone 8 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | An adjusted 97.5 percent lower confidence bound reaches the 0.05 error margin or 0.15 bitrate margin with its point estimate | None |
| `PENDING_LOCAL` | Final lossless bounded error is at most 0.02 overall and 0.05 for the small-glyph subset | None |
| `PENDING_LOCAL` | Total, stratum, repetition, and infrastructure exclusion floors remain admissible | None |
| `DEFERRED_HARDWARE` | Added P95 latency bound is at most 10 ms, decoded loss adds at most two points, and wire rate stays compliant | None |
| `PENDING_LOCAL` | Failed and excluded runs remain visible | None |
| `PENDING_LOCAL` | Every chart reproduces from committed analysis code and a versioned result artifact | None |

## Milestone 9 acceptance gates

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `PENDING_LOCAL` | `HANDOFF_READY` safe source bundle, sync path, runner, action report, and return path pass local and disposable-remote tests | None |
| `DEFERRED_HARDWARE` | A blocked or failed qualification remains a truthful incomplete state | None |
| `DEFERRED_HARDWARE` | `QUALIFICATION_PASSED` has a verified passed bundle and Milestones 0 through 6 and 8 accepted | None |
| `DEFERRED_HARDWARE` | Milestone 9 is accepted only after qualification and every other Milestone 9 gate | None |
| `DEFERRED_HARDWARE` | `RELEASE_PASSED` derives only from accepted Milestones 0 through 6, 8, and 9 | None |
| `PENDING_LOCAL` | No hardware claim can derive from copied output or mismatched manifests | None |
| `PENDING_LOCAL` | Clean checkout reproduces the supported build and tests | None |
| `DEFERRED_HARDWARE` | A compatible GPU environment reproduces the named benchmark | None |
| `PENDING_LOCAL` | Every public claim maps to measured evidence | None |
| `PENDING_LOCAL` | No public non-loopback release occurs with unresolved high or critical threat-model findings | None |
| `PENDING_LOCAL` | Security, HTTPS and WSS, authorization, Host and Origin, revocation, TURN, and sensitive-data reviews pass | None |
| `PENDING_LOCAL` | Worktree is clean and publication waits for explicit authorization | None |
