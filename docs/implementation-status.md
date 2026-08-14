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
| 1 | `WAITING_HARDWARE` | M1-L01A through M1-L04 are locally implemented and all remaining acceptance items require the designated target |
| 2 | `WAITING_HARDWARE` | M2-L01 through M2-L03B are locally implemented, while the real development-grid selection and CUDA runtime gates require the designated target |
| 3 | `WAITING_HARDWARE` | M3-L01 through M3-L03 are locally implemented, while browser, real multi-flight, quality, and target teardown gates require the designated target |
| 4 | `IN_PROGRESS` | Correction controls and the frozen uniform-AQ selector are locally implemented, while target development evidence and the one-shot validation gate remain pending |
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
| M1-L02C2 | `IMPLEMENTED_LOCAL` | Public share path, native peer transport, sender signaling, bounded CPU encode and fanout, optional durable recording, and ordered cleanup | Public CLI parsing and unconfigured no-side-effect test; pinned Ubuntu system OpenH264 720p30 Level 3.1 SPS; portal-selection and capture-start ordering; three-access-unit, 8 MiB, 100-millisecond queue assertions; combined share-record and deferred live-only pipelines; recovery IDR and dependency-epoch assertion; signaling-failure no-link control; peer-disconnect transport drain with recording continuity; durable inspection; independent FFmpeg decode; Linux static analysis; ASAN and UBSAN; production CLI linkage against the exact patched transport; real owner WS and WSS and peer DTLS-SRTP integrations |
| M1-L03A | `IMPLEMENTED_LOCAL` | Self-hosted HTTPS and WSS receiver bundle, separate hashed capabilities, role state machine, monotonic timers, strict receiver control protocol, and adversarial service tests | 14 signaling and control tests within the 31-test TypeScript and JavaScript suite; exact timer simulation through eight hours; live HTTP and WebSocket Host, Origin, replay, rotation, size, and flood tests; digest-pinned production image build; hardened TLS container health check; public-route HTTPS and WSS verifier |
| M1-L03B1 | `IMPLEMENTED_LOCAL` | Exact native WebSocket Origin support in the pinned libdatachannel client without weakening signaling upgrade validation | Patched-source SHA-256 lock; `integration.libdatachannel_contract`; exact single-header generation; absent-header control; empty, CR, and LF injection rejection; complete patched transport rebuild |
| M1-L03B2a | `IMPLEMENTED_LOCAL` | Native owner-signaling protocol and client with exact origin binding, strict schemas, bounded queues, authenticated lifecycle transitions, heartbeat liveness, memory-only capability cleanup, and verified WS and WSS routing | `integration.owner_signaling_protocol`; real loopback service integration over WS and certificate-verified WSS; session creation; single-use join; reservation; offer and answer; candidates; independent heartbeat survival; disconnect; owner stop; server cleanup; seeded comments, duplicate keys, stale sequence, session swap, forged link, and unknown-field failures |
| M1-L03B2b1 | `IMPLEMENTED_LOCAL` | Exact bidirectional `glyphrelay-control-v1` state machine and browser schema contract with bounded telemetry, sequence and session fencing, five-sample clock burst, five-second clock cadence, transition acknowledgments, and receiver flood limits | `integration.control_protocol`; browser control tests; exact lifecycle, clock identity, cumulative statistics, pause, resume, end, duplicate-key, comment, remote-command, telemetry-regression, 4 KiB, and ten-message rolling-limit fixtures |
| M1-L03B2b2a | `IMPLEMENTED_LOCAL` | Native peer service with strict 720p30 H.264 offer admission, trickle ICE, one reliable control channel, sole RTP allocator, bounded NACK and PLI recovery, REMB observation, final UDP egress accounting, and ordered stop cleanup | `integration.peer_sender` over two real loopback DTLS-SRTP peers; five repeated functional runs; ASAN and UBSAN; offer and answer; bidirectional candidates; five clock responses; receiver statistics; recovery Annex B through RTP; final media egress; session-end acknowledgment; cache, channel, track, and peer cleanup; duplicate-candidate and unsupported TURN transport failures |
| M1-L03B2b2b | `IMPLEMENTED_LOCAL` | Loopback dashboard binding, fragment nonce, exact Host and Origin, Fetch Metadata, cookie rejection, CSRF, canonical bounded actions, and restrictive browser boundary | Five live HTTP adversarial tests; hostile Host, Origin, DNS rebinding, cross-site Fetch Metadata, cookie, nonce, CSRF, CORS preflight, oversized body, duplicate key, noncanonical JSON, unknown field, and arbitrary-action controls; Chromium 151.0.7922.34 launch, fragment removal, state render, authorized pause, no-cookie, no-external-request, and no-nonce-request workflow |
| M1-L04 | `IMPLEMENTED_LOCAL` | Frozen corpus protocol, renderer inputs, manifests, OCR evaluator, metrics, and lossless development gates | `make corpus-regeneration-check`; `make corpus-lossless-check`; 64 development and 64 validation sequences; 20,480 visible and 5,120 small-glyph instances in each split; 256 development frames and 512 OCR boxes; overall bounded CER 0.0; small-glyph bounded CER 0.0; validation and final-test outputs absent; aggregate protocol hash `81f95f221b3a9671e8547dfc3352a0766902e4a0a719baf5c50583049e6125dc` |

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | A real portal-selected window streams to Chromium and Firefox | None |
| `DEFERRED_HARDWARE` | Record-only produces an independently decoded profile stream without signaling or a browser offer | The complete local CLI service produces and independently decodes `recording_profile_candidate_v1`; final acceptance waits for target-only `recording_profile_v1` freeze evidence |
| `DEFERRED_HARDWARE` | Capture cancel, close, revoke, and shutdown paths pass | None |
| `IMPLEMENTED_LOCAL` | Every queue has an asserted bound and disconnects leak no session or frame resource | Three-frame capture pool; three-access-unit, 8 MiB, and 100 ms transport bounds; bounded recorder, signaling, control, RTP recovery, and telemetry paths; real peer, owner signaling, and service disconnect tests assert empty queues, released frames, closed tracks and channels, and deleted session state |
| `IMPLEMENTED_LOCAL` | Token replay, hostile Origin and Host, oversized messages, control floods, and insecure non-loopback binds fail closed | Live loopback service tests plus strict receiver control parser and ten-message rolling-window flood test |
| `IMPLEMENTED_LOCAL` | Capability swaps, forged actions, impersonation, fixation, and every invalid transition fail closed without revealing capabilities | Domain-separated keyed-hash state tests, exact-field parser tests, one-receiver reservation tests, and role-confusion transition tests |
| `IMPLEMENTED_LOCAL` | Signaling closure, heartbeat, partition, ICE, reservation, join, and absolute timers follow exact transitions without extending absolute lifetime | Deterministic monotonic-clock tests cover exact five-second, ten-minute, 15-minute, 30-second, and eight-hour boundaries plus live disconnect cleanup |
| `IMPLEMENTED_LOCAL` | Stale owner generations and same-session owner reconnects fail closed after revocation | Connection-identity and generation-fencing tests plus idempotent post-revocation cleanup |
| `IMPLEMENTED_LOCAL` | Self-hosted HTTPS and WSS routing works and an unconfigured sender creates no remote link | Public origin route verifier; certificate-verified native WSS integration; production share CLI transport linkage; exact CLI test proving missing configuration performs no portal, capture, signaling, or link side effect |
| `IMPLEMENTED_LOCAL` | Development lossless OCR bounded error is at most 0.02 overall and 0.05 for 8-to-10-pixel glyphs | Exact Tesseract 5.3.4 image evaluates 512 boxes from 256 lossless sampled frames at 0.0 overall and 0.0 small-glyph bounded CER |
| `IMPLEMENTED_LOCAL` | Corpus and protocol validation passes without opening validation or final-test renderer output | Frozen manifest schema and semantic checker; byte-for-byte regeneration of both 64-sequence manifests; validation and final-test output directories absent; sealed final-test pool contains no concrete seed |

## Milestone 2 acceptance gates

### Local work packages

| ID | State | Deliverable | Evidence |
| --- | --- | --- | --- |
| M2-L01 | `IMPLEMENTED_LOCAL` | Scalar `saliency_v1`, separate bounded packed-source and NV12-surface ownership rings, deterministic map reduction, protected-region preview, and timing oracle | Hand-calculated uniform, edge, opposite-edge, isolated-pixel, border, partial-tile, dropped-frame, and geometry-reset fixtures; one twenty-frame seeded randomized sequence; ownership, alias, exhaustion, stale-token, and shutdown tests; real no-clobber preview CLI inspection |
| M2-L02 | `IMPLEMENTED_LOCAL` | CUDA conversion and saliency kernels, asynchronous map copy, CUDA event fencing, NVTX ranges, differential harness, and target qualification phases | `make cuda-compile-check` builds the complete Linux CUDA target with NVCC 13.3.73 in the digest-pinned official CUDA 13.3.1 image; portable unsupported-adapter test; independent evidence schema and seeded validator controls; runtime gates remain deferred |
| M2-L03A | `IMPLEMENTED_LOCAL` | Frozen development grid, exact evidence schemas, leak-proof deterministic selector, immutable protocol identity, and independent selection reproduction | Exact 2,511-candidate enumeration from 31 valid feature-weight tuples; all threshold and tie-break controls; complete-grid, duplicate, missing-stratum, split-identity, validation-access, no-clobber, and seeded-selection-defect tests; protocol aggregate `9d11f1621b5174e985f520ee060c8e55371773131725cd0df65a7b8c96042cce` |
| M2-L03B | `IMPLEMENTED_LOCAL` | Designated-target development-grid evaluator, exact glyph and UI truth rasterization, CUDA-event timing provenance, durable per-candidate resume, and consolidated qualification freeze handoff | Pinned Linux CUDA image builds the 47-target graph including `glyphrelay_saliency_development`; portable map-formula goldens cover recall, protection, false protection, false discovery, and static level change; all 256 frozen truth frames rasterize completely; source, platform, renderer, corpus, implementation, and grid hashes bind every result; exit 75 becomes a preserved `BLOCKED` repository-freeze handoff rather than acceptance |

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `IMPLEMENTED_LOCAL` | Scalar goldens and portable boundary tests pass | `native.saliency`; exact frozen feature vectors and partial 1-by-1, 17-by-17, and 23-by-17 geometry fixtures |
| `DEFERRED_HARDWARE` | CUDA goldens, differential tests, boundary tests, and compute-sanitizer pass | None |
| `IMPLEMENTED_LOCAL` | Saliency output is deterministic for the same frame sequence | Twenty seeded randomized frames produce exact feature, hysteresis, level, and macroblock-map equality across independent instances |
| `DEFERRED_HARDWARE` | The complete frozen development grid selects one configuration before validation access | The exact evaluator and selector are locally verified, but no selected configuration is committed because the CUDA maps and CUDA-event processing P95 values require the designated target |
| `DEFERRED_HARDWARE` | Complete CUDA preprocessing and map copy is at most 5 ms P95 at 1080p30 | None |

## Milestone 3 acceptance gates

### Local work packages

| ID | State | Deliverable | Evidence |
| --- | --- | --- | --- |
| M3-L01 | `IMPLEMENTED_LOCAL` | Exact per-frame uniform, fixed-emphasis, and automatic-emphasis submission contracts with bounded busy retry, delayed output, FIFO ownership, EOS, and fatal cleanup | Exhaustive `native.gpu_contracts` status transitions; complete immutable retry fingerprint including map bytes; consecutive delayed-output and locked-head fatal controls; seeded mutation, alias, order, and retry-limit defects |
| M3-L02 | `IMPLEMENTED_LOCAL` | Production synchronous-output NVENC session sharing one retained CUDA primary context with the CUDA preprocessor | Exact 13.1 API build; one registered NV12 surface and bitstream buffer per slot; dedicated output thread; single map and surface ownership chain; strict IDR and Annex B callback identity; pinned CUDA image compiles the complete 50-target graph |
| M3-L03 | `IMPLEMENTED_LOCAL` | Target enhanced-encoder qualification, independent decode validator, and repeated teardown harness | Exact committed saliency-selection and configuration hash binding; three exact 300-frame modes; at least two simultaneous owned submissions; ten create-submit-drain-destroy cycles; foreign-generation rejection control; hash-bound streams; independent FFmpeg and FFprobe validation; target runtime remains deferred |

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | Browser and independent decoders accept the enhanced stream | None |
| `DEFERRED_HARDWARE` | Multiple in-flight frames show no map or surface corruption | None |
| `IMPLEMENTED_LOCAL` | Injected normal and fatal status simulations preserve slot ownership and order | `native.gpu_contracts`; success, consecutive need-more-input, encoder-busy, retry mutation, fatal, locked-head fatal, EOS, and cleanup controls |
| `DEFERRED_HARDWARE` | Uniform and fixed maps reproduce the Milestone 0 quality shift | None |
| `IMPLEMENTED_LOCAL` | Portable error and teardown stress tests pass | `native.gpu_contracts`; `native.nvenc_encoder`; repeated shutdown and exact release-order models |
| `DEFERRED_HARDWARE` | Target error and teardown stress tests pass | None |

## Milestone 4 acceptance gates

### Local work packages

| ID | State | Deliverable | Evidence |
| --- | --- | --- | --- |
| M4-L01 | `IMPLEMENTED_LOCAL` | Bounded source-visible pins and exclusions with stable identifiers, optimistic revisions, geometry reset, cursor composition, conflict visualization, and loopback dashboard controls | `native.saliency_corrections`; public preview CLI; hostile dashboard command tests; exact Chromium correction, conflict, pause, and resume workflow |
| M4-L02 | `IMPLEMENTED_LOCAL` | Exact production NVENC uniform-AQ configuration surface and emphasis-map incompatibility guard | `native.nvenc_encoder`; canonical disabled fields; temporal-only AQ; spatial strengths 1, 4, 8, 12, and 15 with temporal AQ on and off; invalid strength and emphasis-map controls |
| M4-L03 | `IMPLEMENTED_LOCAL` | Frozen 11-candidate `uniform_aq_v1` schema, lock, deterministic PAV selector, comparator selection, and no-validation-access guard | `make protocol-check`; AQ selector, retry, orchestration, and interpolation tests; exact five-target coverage, two-percent rate matching, seven-stratum arithmetic, systems margins, unestimable crossing, and tie-break controls; protocol SHA-256 `fabcba7efe362d2f4c8359441527b9b728e165b56d7f933cfc89950a2e766a3c` |
| M4-L04 | `IMPLEMENTED_LOCAL` | Resumable target corpus encoder, independent decoder, pinned OCR, exact-browser verifier, evidence assembler, and freeze handoff | Portable native build and public help test; canonical native-to-Python encoder identity parity; AQ orchestration and resume tests; strict schema and protocol checks; pinned-browser TypeScript checks; consolidated `uniform-aq-development-selection` phase; runtime remains deferred |
| M4-L05 | `IMPLEMENTED_LOCAL` | One-shot validation renderer, pre-access durable ledger, exact-resume and reproduction controls, lossless OCR gate, selected-map CUDA evaluator, strict evidence schema, failure-scene report, and consolidated target phase | Portable native build and help path; frozen OCR reuse test; preopened-render, mismatched-ledger, ordinary-rerun, and exact-resume controls; seven seeded threshold defects and boundary controls; exact validation theme, scrolling, caret, embedded-video, and 5,120-small-glyph coverage; runtime remains deferred |

| State | Acceptance item | Evidence |
| --- | --- | --- |
| `DEFERRED_HARDWARE` | Lossless validation bounded error is at most 0.02 overall and 0.05 for 8-to-10-pixel glyphs before compressed evidence | No validation renderer output has been opened |
| `DEFERRED_HARDWARE` | Equal-stratum glyph macroblock recall is at least 90 percent overall and 80 percent for the small-glyph subset | No validation map measurement exists |
| `DEFERRED_HARDWARE` | Equal-stratum protected macroblock fraction is at most 35 percent | No validation map measurement exists |
| `DEFERRED_HARDWARE` | Equal-stratum false-protected fraction is at most 15 percent | No validation map measurement exists |
| `DEFERRED_HARDWARE` | Equal-stratum static-scene map change after warmup is at most two percent | No validation map measurement exists |
| `IMPLEMENTED_LOCAL` | Every map metric reports per-stratum values and P95 sequence values | Strict schema plus native aggregate, per-stratum, per-sequence, and nearest-rank P95 serialization |
| `IMPLEMENTED_LOCAL` | Theme, scroll, cursor, embedded-video, and small-font cases are included | Frozen manifest coverage test records two themes, three rapid-scroll sequences, 256 caret samples, nine mixed-video sequences, and 5,120 small glyphs |
| `IMPLEMENTED_LOCAL` | Failure scenes and manual correction are documented | Eight-scene diagnostic-only ranking plus bounded pin, exclusion, cursor, geometry-reset, and conflict behavior in `docs/saliency.md` |
| `DEFERRED_HARDWARE` | Complete AQ grid and deterministic selector reproduce the winning target configuration without validation data | None |

## Milestone 5 acceptance gates

### Local work packages

| ID | State | Deliverable | Evidence |
| --- | --- | --- | --- |
| M5-L01A | `IMPLEMENTED_LOCAL` | Frozen `controller_v1` arithmetic, timing, complete degradation and restoration stack, profiles, transport eligibility, trace contract, qualification matrix, and network fixture | JSON Schema validation; semantic mutation controls; content-addressed protocol lock `0088578978411a9f1705880ca6122e0896037b0f115a298f48376e2caca96701`; `make protocol-check` |

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
