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
| Architectural correction | ADR 0002 retains 1080p30 for record-only and evaluation use and caps V1 browser sharing at 720p30 under Level 3.1 |

## Milestone summary

| Milestone | State | Reason |
| --- | --- | --- |
| 0 | `WAITING_HARDWARE` | M0-L01 through M0-L10 are locally implemented and all target gates are deferred |
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
| M0-L04 | `IMPLEMENTED_LOCAL` | Canonical `m0_fixed_map_v1` generator, manifests, metrics, strict hash enforcement, target-capable NVENC runner, and independent evidence validator | Full regeneration and SHA-256 verification of 2,100 coded NV12 frames; manifest `5443417595e3ccb88c89adc3a2d22842fde3206c736d3069042b62cd1c8ab708`; portable gate and seeded-defect tests; public benchmark CLI exits 3 after verified lock on unsupported Mac; target encode and measured acceptance remain deferred |
| M0-L05 | `IMPLEMENTED_LOCAL` | Annex B, SPS, SDP, recording-profile, OpenH264, and independent-decode contracts | Strict malformed-input, RFC 6184 profile, Level 3.1 rejection, SPS VUI, and NV12-to-I420 tests; Ubuntu 24.04 system OpenH264 2.4.1 record-only encode; SPS/PPS on startup and forced IDR; independent FFmpeg 6.1.1 decode; FFprobe confirms 1080p Constrained Baseline Level 4.0 limited-range BT.709; final profile remains correctly unfrozen pending exact browser and NVENC target gates |
| M0-L06 | `IMPLEMENTED_LOCAL` | CUDA context and NVENC feasibility source, ownership state machines, pre-submit rejection, and cleanup | Exact header bootstrap and configure hash gate; primary-context RAII and per-thread push/pop guard; real NVENC function-table capability probe source compiled against the locked 13.1 header; bounded portable source, surface, submission, delayed-output, busy-retry, EOS, abort, and shutdown models; seeded foreign-context, stale-frame, stale-geometry, map-size, pointer-space, event, range, FIFO, retry-mutation, output-alias, retry-limit, and fatal-path tests; target capability and encode gates remain deferred |
| M0-L07 | `IMPLEMENTED_LOCAL` | Final datagram hook, classifier, egress gate, counter, and deterministic revocation race | Locked MPL source patch at the final `sendto` boundary; exact patched-stack build; loopback generated-control and classified-media hook test; mux, ICE TCP, and TURN TCP or TLS rejection; direct IPv4, direct IPv6, TURN arithmetic, failure, stale-epoch, control-bypass, and all-reason linearization tests; packet-capture equality remains deferred |
| M0-L08 | `IMPLEMENTED_LOCAL` | RTP packetization, sole sequence owner, bounded NACK and PLI recovery, and rollover contracts | Strict portable and exact pinned-library packetization tests; mixed 3-byte and 4-byte Annex B; single NAL and ordered FU-A; 1,200-byte payload and marker bounds; 64-bit timestamp and sequence wrap; MPL bounded responder; exact plaintext RTP replay; PID and BLP deduplication; active-epoch absent and ambiguous recovery; 500 ms, 2,048 packet, 4 MiB, and two-retransmission cache bounds; 100-identifier and ten-message rolling limits; sustained-flood termination; patched-stack build; protected-payload and browser recovery remain deferred |
| M0-L09 | `IMPLEMENTED_LOCAL` | Loopback signaling, receiver, browser oracle, and browser pins | Strict Host and Origin, replay, ordering, size, SDP, and static-asset tests; exact Chromium 151.0.7922.34 accepts the ADR 0002 live 720p30 Level 3.1 contract and reaches the answer wait; exact Firefox 153.0 locally exposes no H.264 payload; strict ten-run no-clobber oracle freeze tool; profile and oracle remain unfrozen pending target qualification |
| M0-L10 | `IMPLEMENTED_LOCAL` | Local qualification phases, structured result schemas, documentation, and complete local verification | Content-addressed bundle; bounded and durable failure-complete runner; frozen performance-contamination policy; separate private and public-evidence archives; disposable-remote and seeded-fault tests; strict independent browser-offer identity and SDP validator; repeated target pre-submit and ownership phase; all-stream independent benchmark decode; direct NVENC 720p30 browser fixture producer; exact eighteen-run browser matrix and deterministic four-frame independent oracle; real DTLS-SRTP direct IPv4 and IPv6 fixtures; digest-pinned TURN, tshark payload-identity, and IP-length target phase with seeded validators; `make check`; `make linux-cpu-check`; `make handoff-check` |

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
| M0-H04 | `DEFERRED_HARDWARE` | Both real offers pass `recording_profile_v1` for every selected V1 sharing presentation | None |
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

### Local work packages

| ID | State | Deliverable | Evidence |
| --- | --- | --- | --- |
| M1-L01A | `IMPLEMENTED_LOCAL` | Portable portal lifecycle, shared-memory ownership, bounded latest-frame capture pool, cursor composition, orientation transforms, geometry epochs, and scalar plus available SIMD BT.709 conversion | `native.capture_contracts`; hand-calculated limited- and full-range goldens; BGRA and RGBA; pitch, odd-edge, and all eight SPA orientation cases; 100 deterministic randomized scalar-to-SIMD comparisons; prompt-requeue and starvation controls |
| M1-L01B | `IMPLEMENTED_LOCAL` | Real Linux GDBus ScreenCast portal and PipeWire shared-memory adapter | Locked Ubuntu GLib/GIO/PipeWire/SPA build; `integration.linux_capture_contracts`; real SPA crop, damage, reflected transform, inline cursor, prompt-requeue, and DMA-BUF-fallback fixtures; Linux capture analyzer and sanitizer lanes; interactive portal gate remains deferred |
| M1-L02A | `IMPLEMENTED_LOCAL` | Bounded nonblocking recorder branch, crash-safe journal, durable publication, streaming inspection, and independent filesystem crash model | Real system-OpenH264 access units; independent FFmpeg decode; all 27 recorder persistence-event process crashes; 28-operation filesystem crash model; three seeded ordering defects; Linux analyzer; ASAN and UBSAN; no-clobber and corruption fixtures |
| M1-L02B | `IMPLEMENTED_LOCAL` | Public CPU record-only and inspect paths, three-frame latest capture pool, frame-rate drops, fixed-profile conversion, and user-facing durable recording | Exact CLI source-override rejection; injected full record service; six real OpenH264 access units; public JSON inspection; independent FFmpeg decode; label non-disclosure; Linux analyzer; ASAN and UBSAN |
| M1-L02C1 | `IMPLEMENTED_LOCAL` | Immutable encoded-access-unit fanout, three-access-unit and eight-MiB transport bounds, 100-ms age bound, whole-epoch overflow purge, stale-epoch fencing, recovery coalescing, branch isolation, and stop cleanup | `native.encoded_fanout` on macOS and pinned Ubuntu 24.04; ASAN and UBSAN; exact age and capacity boundaries; immutable allocation identity; stale and future epochs; time regression; recorder failure and exception isolation; transport failure and recorder survival |
| M1-L02C2 | `PENDING_LOCAL` | Public share path, native peer transport, sender signaling, and end-to-end disconnect cleanup | None |
| M1-L03A | `IMPLEMENTED_LOCAL` | Self-hosted HTTPS and WSS receiver bundle, separate hashed capabilities, role state machine, monotonic timers, strict receiver control protocol, and adversarial service tests | 14 signaling and control tests within the 31-test TypeScript and JavaScript suite; exact timer simulation through eight hours; live HTTP and WebSocket Host, Origin, replay, rotation, size, and flood tests; digest-pinned production image build; hardened TLS container health check; public-route HTTPS and WSS verifier |
| M1-L03B1 | `IMPLEMENTED_LOCAL` | Exact native WebSocket Origin support in the pinned libdatachannel client without weakening signaling upgrade validation | Patched-source SHA-256 lock; `integration.libdatachannel_contract`; exact single-header generation; absent-header control; empty, CR, and LF injection rejection; complete patched transport rebuild |
| M1-L03B2 | `PENDING_LOCAL` | Sender owner-signaling and control-channel integration plus loopback dashboard nonce and CSRF defenses | None |
| M1-L04 | `PENDING_LOCAL` | Frozen corpus protocol, renderer inputs, manifests, OCR evaluator, metrics, and lossless development gates | None |

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | A real portal-selected window streams to Chromium and Firefox | None |
| `DEFERRED_HARDWARE` | Record-only produces an independently decoded profile stream without signaling or a browser offer | The complete local CLI service produces and independently decodes `recording_profile_candidate_v1`; final acceptance waits for target-only `recording_profile_v1` freeze evidence |
| `DEFERRED_HARDWARE` | Capture cancel, close, revoke, and shutdown paths pass | None |
| `PENDING_LOCAL` | Every queue has an asserted bound and disconnects leak no session or frame resource | None |
| `IMPLEMENTED_LOCAL` | Token replay, hostile Origin and Host, oversized messages, control floods, and insecure non-loopback binds fail closed | Live loopback service tests plus strict receiver control parser and ten-message rolling-window flood test |
| `IMPLEMENTED_LOCAL` | Capability swaps, forged actions, impersonation, fixation, and every invalid transition fail closed without revealing capabilities | Domain-separated keyed-hash state tests, exact-field parser tests, one-receiver reservation tests, and role-confusion transition tests |
| `IMPLEMENTED_LOCAL` | Signaling closure, heartbeat, partition, ICE, reservation, join, and absolute timers follow exact transitions without extending absolute lifetime | Deterministic monotonic-clock tests cover exact five-second, ten-minute, 15-minute, 30-second, and eight-hour boundaries plus live disconnect cleanup |
| `IMPLEMENTED_LOCAL` | Stale owner generations and same-session owner reconnects fail closed after revocation | Connection-identity and generation-fencing tests plus idempotent post-revocation cleanup |
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
| `IMPLEMENTED_LOCAL` | Every injected recorder crash yields a marker-verified complete or journal-anchored inspectable incomplete recording | `integration.durable_recording`; one child-process crash at every declared persistence event; completion and journal inspection |
| `IMPLEMENTED_LOCAL` | Filesystem crash model proves initial preparation and publication durability order | `tests/python/test_recording_crash_model.py`; all 28 crash cuts; seeded journal, admission, and marker ordering defects |
| `DEFERRED_HARDWARE` | Ten-minute 1080p30 maximum-profile recording meets queue, commit, and decode gates on named storage | None |
| `IMPLEMENTED_LOCAL` | Existing and symbolic-link outputs remain unchanged and incomplete outputs cannot be replaced | `integration.durable_recording`; deterministic incomplete journal reservation; `O_EXCL`, `O_NOFOLLOW`, and `RENAME_NOREPLACE` fixtures |
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
