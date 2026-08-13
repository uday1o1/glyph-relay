# GlyphRelay Build Plan

Status: implementation-ready plan with a mandatory hardware feasibility gate.

Planning snapshot: 2026-08-13.

GlyphRelay is a local-first, text-aware screen-sharing and recording product for coding interviews, remote debugging, technical teaching, and support over constrained networks.
The sender selects one application window and a bandwidth cap.
GlyphRelay identifies text and user-interface regions locally, converts the frame on the GPU, creates an NVENC emphasis map, and spends more bits on important glyph regions while preserving interactive latency through bounded queues.
The receiver opens a short-lived browser link and decodes ordinary H.264 through WebRTC.

## 1. Product thesis

Natural-video encoders can make small code, terminal output, tables, and interface labels unreadable at the low bitrates common on constrained links.
Uniformly raising quality wastes bandwidth on backgrounds and embedded motion that may matter less to a technical collaboration task.

GlyphRelay asks:

> Can an automatic local saliency path preserve text readability at the same measured bitrate without adding material interactive latency or requiring a custom decoder?

The product contribution is:

- A visible end-user screen-sharing workflow.
- Original CUDA preprocessing and map-generation kernels.
- Direct use of the low-level NVIDIA NVENC API.
- Capability-aware fallbacks.
- A standard H.264 WebRTC receiver.
- A closed-loop bitrate and queue controller.
- A deterministic screen-content corpus with known glyph truth.
- Readability, latency, bitrate, and profiler evidence.

The project is not an NVENC wrapper or a benchmark dashboard.
Performance is the user outcome.

## 2. Target user and primary journey

The first user is a Linux developer, instructor, interviewer, or support engineer sharing one code, terminal, browser, slide, or spreadsheet window.

The primary journey is:

1. Launch GlyphRelay and run the automatic hardware and capture check.
2. Choose one application window through the desktop permission dialog.
3. Select a bandwidth profile such as 0.5, 1, 2, or 4 Mbps.
4. Inspect a local preview of automatically protected regions.
5. Pin or exclude a region if the automatic map is wrong.
6. Start a one-receiver session.
7. Copy a short-lived browser link.
8. Monitor payload bitrate, wire-egress bitrate, queue delay, dropped frames, feed quality, and connection state.
9. Pause or stop sharing immediately.
10. Optionally save the same stream as a local recording.

No account, remote-control channel, cloud OCR, audio, multi-party conference, or custom receiver plugin belongs in V1.

## 3. Job-description evidence

The project selection used current official descriptions from the active SimplifyJobs new-graduate corpus captured on 2026-08-13 as the primary design input.
The retained crawl contained 307 usable official descriptions.

A purposive 18-role systems, GPU, performance, embedded, and low-latency sample was manually coded.
It is not a random labor-market survey.

| Repeated responsibility or proof signal | Roles containing it |
| --- | ---: |
| Performance or efficiency | 17 of 18 |
| C++ | 14 of 18 |
| Hardware, drivers, memory, cache, or low-level systems | 15 of 18 |
| Debugging, profiling, benchmarking, latency, or throughput | 15 of 18 |
| Testing, validation, verification, or correctness | 14 of 18 |
| Ownership or end-to-end delivery | 16 of 18 |
| Collaboration or documentation | 16 of 18 |
| Reliability, robustness, maintainability, or production quality | 11 of 18 |
| Embedded or robotics | 10 of 18 |
| Networking | 10 of 18 |
| Linux | 9 of 18 |
| CUDA or GPU work | 7 of 18 |
| Distributed systems | 7 of 18 |
| Python | 7 of 18 |
| Compiler work | 5 of 18 |
| Rust | 5 of 18 |

Across the complete 307-description crawl, broad automated tags included C++ in 123, performance in 107, networking in 90, Linux in 52, embedded or robotics in 42, distributed systems in 35, CUDA or GPU in 31, Rust in 20, concurrency in 19, and compilers in 11.

GlyphRelay naturally demonstrates modern C++, Linux capture, CUDA kernels, GPU memory ownership, a low-level media API, concurrency, backpressure, WebRTC networking, browser interoperability, profiling, experimental design, capability handling, CPU fallback, privacy, and a polished user workflow.
It does not add TensorRT, DynamoDB, distributed storage, or compiler work without a product reason.

## 4. Novelty boundary

Screen-content coding, region-of-interest encoding, NVENC emphasis maps, and manual ROI tools already exist.
An OBS community plugin provides a manual ROI editor.
Academic and commercial systems have studied text-aware and screen-content codecs.

GlyphRelay must not claim to invent screen-content coding or ROI encoding.
Its defensible contribution is the combination of automatic per-frame text and UI protection, measured readability under a hard bandwidth budget, local-only saliency, a normal browser receiver, and an interactive product with explicit latency and rate control.
Before the public README is written, `docs/prior-art.md` must compare GlyphRelay with NVFBC classification maps, screen-content coding research, the OBS community ROI plugin, and named commercial screen-sharing screen-content modes.
The comparison must separate already-known mechanisms from the project's open implementation, product integration, controls, and reproducible evidence.

The runtime deliberately avoids OCR reconstruction and neural semantic codecs.
Those paths introduce privacy risk, custom-decoder lock-in, and a second ML project.

Truthful public claims may include:

- Automatic maps improve a named readability metric at matched measured bitrate on a frozen corpus.
- A standard browser decodes the H.264 stream.
- The system bounds queue growth and drops stale frames.
- The CUDA path reduces named preprocessing costs or CPU use on named hardware.
- Unsupported emphasis-map hardware falls back visibly.

Public claims may not include:

- Better quality than every screen-sharing product.
- A guaranteed configured bitrate when the encoder can overshoot.
- Zero-copy unless profiler and memory-path evidence proves it.
- End-to-end latency without a documented clock and render measurement method.
- Universal text detection.
- Legal or privacy compliance certification.
- Support for GPUs not present in the tested capability matrix.

## 5. Mandatory feasibility gate

The central product depends on `NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP` on the target GPU and driver.
The first executable is `glyphrelay doctor`.

Before building the product, Milestone 0 must prove:

- The target GPU supports H.264 NVENC.
- The target environment exposes emphasis maps.
- A fixed synthetic emphasis map meets the Milestone 0 protected-region effect and payload-cost gate.
- The encoded stream remains independently decodable.
- Bitrate overshoot can be measured.
- After a ten-second warmup at 1080p30, submit-to-bitstream-ready latency is at most 10 milliseconds at P95 and 16 milliseconds at P99 with no growth in pending submissions during a one-minute steady run.

If the target GPU lacks the capability, the original product cannot be claimed complete.
The project must either move to compatible hardware or explicitly pivot to a manual or uniform-encoding product with a new claim.

### 5.1 Local-first execution and consolidated GPU handoff

The private development-environment source of truth is `/Users/udaya/Documents/Unification Project/RESOURCE_ACCESS_ENVIRONMENT.md`.
That file currently identifies the SSH alias `cuda-pm` as the Linux GPU workstation and owns its mutable host, user, GPU, driver, CUDA, and remote-path facts.
Read that file again immediately before preparing a remote run because its facts may change.
Do not copy its private address, SSH identity details, or other machine-specific values into committed public project files.

The implementation chat must use the following local-first execution policy when it cannot directly execute commands on `cuda-pm`:

Track local work-package state separately from target-gate state.

- A local work package is `PENDING_LOCAL` or `IMPLEMENTED_LOCAL`.
- Each target-only gate is `DEFERRED_HARDWARE`, `FAILED`, `BLOCKED`, or `ACCEPTED`.
- A milestone is `IN_PROGRESS` while any local work package is pending, `WAITING_HARDWARE` when all local work is implemented and any target gate is deferred or blocked, `FAILED` when a nonaccepted result invalidates its gate, and `ACCEPTED` only when every local work package and acceptance item has passed.

Only `ACCEPTED` is milestone completion.
The scheduling exception permits later, explicitly hardware-independent work packages to reach `IMPLEMENTED_LOCAL` while an earlier milestone is `DEFERRED_HARDWARE`.
It never permits a deferred milestone, portfolio checkpoint, release gate, or public hardware claim to pass.
Each locally verified work package may receive a focused submilestone commit whose message and evidence identify the deferred target gates.

1. Implement every hardware-independent source file, test, fixture, deterministic corpus generator, analysis tool, browser component, security control, document, and packaging path locally before asking the user to touch the GPU workstation.
2. Run every test that is meaningful on the Mac or in an available CPU container, including pure C++, TypeScript, Python, schema, deterministic algorithm, parser, packetization, state-machine, security, statistical, and CPU fallback tests.
3. Compile-check platform-independent code wherever the available toolchain permits and keep Linux, CUDA, NVENC, PipeWire, portal, and GPU-only results explicitly `DEFERRED_HARDWARE`, never passed by assumption.
4. Continue only through later hardware-independent work packages when the logical Milestone 0 hardware gate remains `DEFERRED_HARDWARE`, accepting the rework risk created by this user-authorized scheduling exception.
5. Do not weaken, delete, or mark any hardware acceptance gate complete merely because implementation continued beyond it.
6. Do not interrupt the user for ordinary per-test SSH commands, repeated file copies, remote log retrieval, or one-off environment questions.
7. Consolidate GPU execution into one final qualification session after all locally executable work and the remote automation described below are complete.

The repository must provide these two scripts before the consolidated handoff:

- `scripts/gpu/qualify_cuda_pm.sh` is the single Mac-side entry point.
- `scripts/gpu/run_remote_qualification.sh` is the noninteractive and resumable Linux-side runner invoked by the entry point.

`scripts/gpu/qualify_cuda_pm.sh` must:

- Default `GLYPHRELAY_GPU_HOST` to `cuda-pm` without embedding an IP address.
- Resolve the remote home through the selected SSH alias and derive the default namespace from that value rather than hardcoding `/home/udaya`.
- Apply the same validation to the default namespace and any override.
- Accept a root only when it is a nonempty canonical absolute path below the canonical remote home, is not `/`, and contains no unresolved traversal.
- On first use, create the absent root with mode `0700`, verify it through `lstat`, then create its random GlyphRelay-owned sentinel atomically with exclusive creation and durable file and directory synchronization.
- On later use, require `lstat` to prove the root and sentinel are owned by the remote user, are not symbolic links, retain the expected types and modes, and match the locally recorded sentinel identifier.
- Refuse an unexplained dirty worktree and create a canonical source manifest containing the Git commit, recursive submodule commits, dependency locks and image digests, tracked file paths, modes, symbolic-link targets, file hashes, and qualification-script hash.
- Build the canonical bundle only from Git-tracked files and declared recursive submodule files, exclude ignored and untracked content, and reject absolute paths, `..` components, special files, or symbolic links that escape the extraction root.
- Verify every archive member against the manifest before and after safe extraction into a new content-addressed directory under the validated remote namespace.
- Transfer only the manifest-declared source bundle and never use `rsync --delete` against a broad or pre-existing remote directory.
- Resume an identical incomplete bundle without duplicating completed phases.
- Acquire one atomic remote idempotency lock per bundle and refuse a concurrent incompatible runner for that bundle.
- After GPU selection, acquire an exclusive qualification lock keyed by selected GPU UUID across every bundle, validate its owner PID, detached-session identity, bundle hash, and heartbeat before treating it as live, and refuse rather than break a live lock.
- Reclaim a stale GPU lock only through the recorded owner and session validation procedure, and record that action in the result bundle.
- Launch the remote runner in a detached `tmux` session, or a tested `nohup` fallback, so an SSH disconnect cannot terminate qualification.
- Reconnect, poll, and resume through the same stable entry point while preserving the complete remote log.
- Retrieve the immutable result bundle into a newly created no-clobber ignored directory under `artifacts/gpu-runs/<run-id>`.
- Verify remote and local `SHA256SUMS` before accepting any result.
- Exit nonzero unless the result status is exactly `PASSED`.
- Print one final summary containing passed phases, failed phases, deferred interactive phases, artifact paths, and truthful claim consequences.

`scripts/gpu/run_remote_qualification.sh` must:

- Run only inside the content-addressed project directory and refuse a source-manifest mismatch.
- Capture hostname class, OS, CPU, every visible GPU identity, selected GPU UUID, driver, CUDA runtime, toolkit, NVENC API, browsers, desktop-session facts, and dependency versions before building.
- Select the qualification GPU through a frozen noninteractive policy that prefers a declared compatible architecture and model, then the lexicographically smallest qualifying GPU UUID.
- Probe every candidate for the required CUDA and NVENC capabilities before selection, record the complete decision, and report one consolidated action when no candidate qualifies or the declared policy remains ambiguous.
- Use project-local build directories, dependency caches, virtual environments, and containers where appropriate.
- Never change the global driver, CUDA installation, Docker daemon, system packages, or shared environment without a separate explicit user approval.
- Run preflight and every Milestone 0 hardware gate before any dependent GPU phase, then run clean configure and build, formatting, static analysis, CPU tests, CUDA differential tests, sanitizer-compatible tests, `glyphrelay doctor`, NVENC ownership and failure tests, browser interoperability, transport accounting, controller replay, privacy revocation, recording durability, frozen benchmarks, and required Nsight captures in dependency order.
- Reuse completed phases only when their input hash, executable hash, environment fingerprint, and declared output hashes still match.
- Write phase transitions and top-level status through atomic temporary-file replacement and `fsync` the file and containing directory before considering a phase durable.
- Place each phase in a timestamped directory with its redacted command, named environment-variable allowlist without secret values, redacted stdout, redacted stderr, structured result, duration, and artifact hashes.
- Produce `status.json`, `environment.json`, `commands.jsonl`, `junit.xml`, `SHA256SUMS`, and a concise `REPORT.md` even when a phase fails.
- Mark absent hardware, unsupported emphasis maps, missing system capabilities, unavailable graphical sessions, and infrastructure errors as `BLOCKED` or `FAILED`, never as passing evidence.
- Continue all independent phases after a nonfatal failure so one remote session returns the complete problem list.
- Avoid interactive package installation and collect every unavoidable prerequisite into one generated `USER_ACTION_REQUIRED.md` rather than requesting fixes one at a time.
- Store private run artifacts with mode `0700`, redact tokens, credentials, private addresses, user paths, and unrelated environment values, and generate a separately reviewed redacted export for public evidence.
- Sample competing processes, per-GPU utilization, memory use, clocks, temperature, power, throttle reasons, and ECC or Xid errors before and throughout every performance phase.
- Invalidate a benchmark phase when its frozen contention, thermal, clock, throttling, or GPU-error limits are exceeded rather than publishing contaminated measurements.

The final handoff must minimize user involvement as follows:

- If the implementation agent is permitted to use SSH and file-copy commands, it must run the Mac-side entry point, retrieve results, fix in-scope defects, and repeat consolidated runs without asking the user to relay commands.
- If the implementation environment cannot open SSH, the agent must leave the complete source bundle and both scripts ready, then ask the user to use only `./scripts/gpu/qualify_cuda_pm.sh` from the repository root for the qualification and any resume cycle.
- That one entry point must perform sync, remote execution, result retrieval, and verification without requiring the user to copy individual commands or files.
- A real XDG portal selection that requires an active graphical desktop is the only expected human interaction inside the final qualification session.
- The runner must group every such portal or browser interaction into one clearly announced interactive phase rather than scattering prompts across the run.
- If the remote machine has no usable graphical desktop session, the run must still complete all synthetic, CUDA, NVENC, headless-browser, transport, recording, benchmark, and profiler phases and emit one precise remaining desktop-acceptance procedure.
- After retrieved failures are fixed locally, the agent must regenerate and rerun the same consolidated entry point rather than beginning manual SSH back-and-forth.
- A prerequisite or code correction may require another qualification cycle, but every cycle uses the same stable entry point and one consolidated action report rather than ad hoc commands.

Remote qualification artifacts are evidence, not source.
Keep generated bundles, logs, recordings, profiler reports, environment captures, and machine identifiers ignored by Git.
Commit only reusable scripts, redacted schemas, deterministic fixtures, and documentation that remains valid outside the private workstation.

## 6. V1 scope

### 6.1 Supported

- Linux x86-64.
- One tested NVIDIA GPU with a documented driver, CUDA, and NVENC capability set.
- One application-window capture through the XDG ScreenCast portal and PipeWire.
- Shared-memory capture fallback.
- DMA-BUF import only after explicit compatibility and profiler proof.
- BGRA or RGBA desktop frames.
- 1080p30 primary operating point.
- H.264 4:2:0 browser-compatible WebRTC.
- Four constrained-bandwidth profiles.
- Automatic non-neural text and UI saliency.
- User-pinned and user-excluded rectangles.
- Cursor emphasis.
- One sender and one receiver.
- Short-lived signaling.
- Optional TURN deployment using coturn.
- Local H.264 recording.
- CPU functional fallback with a system-provided compatible encoder.

### 6.2 Explicitly unsupported in V1

- Audio.
- Remote input or control.
- Clipboard sharing.
- File transfer.
- Multi-party calls.
- Multiple monitors or simultaneous windows.
- Windows and macOS sender support.
- Native receiver applications.
- 4K.
- HDR.
- H.264 4:4:4.
- HEVC and AV1 live transport.
- Custom codecs or decoders.
- Runtime OCR.
- Neural saliency.
- Cloud media processing.
- Accounts and persistent contact lists.
- Recording upload or cloud storage.

### 6.3 Release tiers

The core portfolio release requires Milestones 0 through 6, Milestone 8, and Milestone 9.
The core release uses the proven shared-memory capture path and requires functional local recording, Chromium and Firefox interoperability, privacy behavior, the frozen machine evaluation, and a self-hostable receiver and signaling bundle.
Milestone 7 DMA-BUF import is an optional measured optimization tier and cannot delay the first portfolio release when the shared-memory path meets its gates.
A deployed TURN service and the optional human-preference study are separate evidence tiers, and their absence must remove only their corresponding public claims.
Audio, remote control, multi-party transport, other operating systems, and alternative live codecs remain post-release extensions.

## 7. Portfolio acceptance definition

The project is portfolio-ready only when all of the following are true:

- `glyphrelay doctor` reports the exact environment and capability decision.
- A clean checkout builds the CPU path without proprietary artifacts committed to Git.
- A documented compatible environment builds and runs the GPU path.
- The real XDG portal and PipeWire window-selection path works.
- A Chromium and Firefox receiver decode the stream.
- The self-hostable HTTPS and WSS bundle completes the documented remote sender-to-browser signaling path.
- The release threat model has no unresolved critical or high-severity finding before that bundle is exposed on a non-loopback interface.
- All capture, processing, encode, transport, and render queues are bounded.
- The latest-frame-wins policy prevents latency growth under overload.
- CPU and CUDA color conversion pass golden checks.
- Macroblock maps pass dimensions, ordering, bounds, and lifetime tests.
- Automatic and deterministic oracle-pinned map modes are compared at matched measured bitrate, and interactive manual correction is evaluated separately as product behavior.
- The frozen corpus and evaluation-only OCR model are legally redistributable or reproducibly downloadable.
- Readability improvements include paired confidence intervals.
- Lossless OCR floors and retained-sample floors make the readability result admissible rather than `INSUFFICIENT_EVIDENCE`.
- Every latency claim uses its exact named start and end events and includes the measurement method and error limits.
- Nsight evidence identifies copies, overlap, custom-kernel cost, and bottlenecks.
- Privacy behaviors survive permission revocation, screen lock, disconnect, and shutdown.
- Stop, pause, lock, permission-loss, and revocation tests prove that the final media-egress boundary is linearizable against a deliberately stalled validation-to-final-UDP-send race and admits no late media datagram.
- Distinct owner and join capabilities pass signaling role-confusion, impersonation, replay, and invalid-transition tests.
- Owner WSS close, error, protocol failure, and liveness timeout revoke the remote session through a connection-generation-fenced fail-closed path, and no same-session owner reconnect is possible.
- The rootless wire counter has passed its packet-capture agreement gates on every transport used for a cap claim.
- A valid completion marker commits every complete recording, and every injected pre-marker crash remains inspectable as incomplete.
- Every record-only or mid-share recording begins at an IDR carrying SPS and PPS under the frozen local recording profile.
- Recording creation is exclusive and never overwrites an existing media, journal, sidecar, marker, temporary, or symbolic-link path.
- A ten-minute 1080p30 recording at the maximum V1 bitrate passes the frozen recorder-throughput and queue-age gate on the named release storage.
- User pause and resume follow a complete media-state protocol without changing the signaling role state or reviving an expired session.

## 8. Current NVIDIA SDK constraints

The current NVIDIA Video Codec SDK documentation defines emphasis maps at macroblock granularity.

The implementation must:

1. Call `NvEncodeAPIGetMaxSupportedVersion` before loading an API version.
2. Query `NvEncGetEncodeCaps` with `NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP`.
3. Refuse to label the session enhanced if capability probing fails.
4. Set `NV_ENC_RC_PARAMS::qpMapMode` to `NV_ENC_QP_MAP_EMPHASIS`.
5. Populate `NV_ENC_PIC_PARAMS::qpDeltaMap` for every enhanced frame.
6. Set `NV_ENC_PIC_PARAMS::qpDeltaMapSize` to the exact map size in bytes for every enhanced frame.
7. Store one signed-byte map entry per H.264 macroblock in raster-scan order.
8. Use values allowed by `NV_ENC_EMPHASIS_MAP_LEVEL` from the pinned header.
9. Pass an NVENC-supported host address to `qpDeltaMap` unless the pinned SDK explicitly documents and the feasibility spike proves another address space.
10. Keep each frame's NVENC-facing map slot alive until the corresponding encode operation is complete or the pinned SDK proves a shorter lifetime safe.
11. Disable spatial and temporal adaptive quantization because the SDK documents them as incompatible with emphasis maps.
12. Measure actual output size because emphasis adjustment occurs after rate control and may violate bitrate or VBV constraints.

The public plan follows current 13.1 documentation.
Milestone 0 selects one exact `nv-codec-headers` or SDK header release, records its hash and license, records the minimum driver API it requires, and commits that version to the build lock.
The runtime must compare that compiled API version with `NvEncodeAPIGetMaxSupportedVersion` and fail with an actionable mode decision rather than passing newer structures to an older driver.
Changing the pinned header after Milestone 0 requires a compatibility-matrix update and a rerun of the feasibility evidence.

On Linux, treat NVENC output as synchronous unless the pinned official documentation for the chosen environment proves otherwise.
Run blocking bitstream acquisition on a dedicated encode worker so capture and CUDA preprocessing can continue.

## 9. Architecture

```text
XDG ScreenCast portal
          |
          v
PipeWire window frames
          |
          +-----------------------+
          |                       |
          v                       v
SHM staging path          DMA-BUF import path
mandatory correctness     optional optimization
          |                       |
          +-----------+-----------+
                      v
                 CUDA stream
          BGRA/RGBA -> NV12 conversion
          luma gradient and contrast
          temporal stability
          tile reduction and dilation
                      |
                      v
       device protected-region map
          auto + pin + exclude + cursor
                      |
                      v
       pinned host emphasis-map ring
          exact byte size + CUDA event
                      |
                      v
        NVENC H.264 low-latency encoder
                      |
                      v
             bounded encoded queue
                      |
                      v
        RTP packetization and WebRTC
                      |
                      v
           Chromium or Firefox receiver
```

Control flow is separate from the media path.
A small signaling service exchanges short-lived session descriptions and ICE candidates.
The signaling service never receives decoded frames.
The core release ships the static receiver and ephemeral signaling service as one self-hostable HTTPS and WSS bundle.
The sender makes one outbound WSS connection to a user-configured instance, creates an ephemeral session, and receives a high-entropy owner capability that authenticates every later sender action on that exact connection generation.
The receiver uses the same public origin but receives a distinct single-use join capability that can authorize only the receiver role.
The reference deployment has no accounts or persistent session database, stores only keyed hashes of the separate owner and join capabilities plus bounded connection state, and deletes session state on expiry or revocation.
The signaling role state machine uses `OWNER_ONLY`, `JOIN_OPEN`, `JOIN_RESERVED`, `CONNECTED`, `REVOKED`, and `EXPIRED` states, and every transition identifies the only role and capability permitted to cause it.
Session creation enters `OWNER_ONLY`, an owner-issued link enters `JOIN_OPEN`, successful receiver authentication consumes that join capability and enters `JOIN_RESERVED`, and successful peer establishment enters `CONNECTED`.
Before peer establishment, closing the authenticated receiver signaling connection returns `JOIN_RESERVED` to `OWNER_ONLY` immediately, every old join or receiver binding remains invalid, and the owner must explicitly issue a new single-use link before another receiver can join.
After peer establishment, a closed or failed receiver signaling connection ends the peer immediately and returns the live owner session to `OWNER_ONLY`.
An ICE `disconnected` event while the authenticated receiver signaling connection remains open receives exactly five seconds to recover, while ICE `failed` or `closed` ends the peer immediately.
When that five-second timer expires, the sender closes the peer, clears its transport state, returns the live owner session to `OWNER_ONLY`, and requires a new join link.
The creating owner WSS connection is non-resumable and is fenced by a monotonically increasing server connection generation stored with the session.
The sender and signaling service exchange sequenced authenticated `OWNER_HEARTBEAT` and `OWNER_HEARTBEAT_ACK` messages every two seconds, and either side treats five seconds without valid peer activity as owner-signaling loss.
An owner WSS close, transport error, protocol failure, or liveness timeout makes the server atomically enter `REVOKED`, invalidate both capabilities and every receiver binding, expire TURN credentials, close the receiver signaling connection, and reject every message from the old connection generation.
The sender independently handles the same owner-signaling-loss event by closing media admission through the `MediaEgressGate`, ending the peer, destroying its in-memory owner capability and remote transport state, and requiring creation of a new session before remote sharing can resume.
The receiver treats owner-signaling revocation or closure of its authenticated signaling connection as a terminal peer event and clears media immediately.
V1 never rebinds an owner capability to another WSS connection, so an owner reconnect creates a new session identifier, owner capability, absolute lifetime, and join link rather than reviving the old session.
Every nonterminal state can enter `REVOKED` through authenticated owner stop, owner-signaling loss, or a privacy boundary and can enter `EXPIRED` through its fixed timer.
An owner capability can issue or rotate a join link, answer the selected receiver, relay owner ICE candidates, restart ICE on the existing authenticated session, stop, or revoke, while a join capability can submit one browser offer and receiver ICE candidates but cannot perform an owner action.
Neither capability may be accepted in the other role, echoed to the peer, placed in a URL visible to the other role, or retained after its session boundary.
Every entry into `OWNER_ONLY` starts a 15-minute join deadline bounded by a nonextendable eight-hour absolute session lifetime from creation, and a `JOIN_RESERVED` peer must reach `CONNECTED` within 30 seconds.
Receiver ICE restarts, pause, and resume never extend a deadline, while returning to `OWNER_ONLY` may start only the new bounded join deadline stated above.
Each `CONNECTED` signaling role state owns a separate media state machine with `NEGOTIATING`, `RUNNING`, `PAUSED`, and `ENDED` states.
Successful atomic admission of the first dependency-epoch IDR with SPS and PPS into the transport path changes `NEGOTIATING` to `RUNNING`, while receiver display remains a separately measured outcome.
Only the authenticated local owner may request `RUNNING -> PAUSED` or `PAUSED -> RUNNING`.
Stop, privacy revocation, signaling-role exit from `CONNECTED`, receiver-grace expiry, or absolute session expiry changes any media state to `ENDED` and cannot be resumed.
A pause never changes the signaling role state, never rotates either capability, and never extends a timer.
If no valid HTTPS signaling origin is configured, the sender offers local preview and recording but does not create a remotely usable link.
A project-operated multi-tenant public service is not part of V1.

## 10. Proposed technology stack

- C++20 for the sender core.
- CUDA C++ for original preprocessing kernels.
- CMake and Ninja for native builds.
- NVIDIA Video Codec SDK for direct NVENC access.
- XDG Desktop Portal and PipeWire client APIs for window capture.
- An exact libdatachannel release or commit with media support enabled, recorded build flags, and a compile-and-run probe for the H.264 packetizer plus required RTCP handlers in Milestone 0 for browser-compatible WebRTC transport.
- A Milestone 0 selected libdatachannel or lower transport source hook that exposes every final direct-UDP and TURN-over-UDP datagram plus its authenticated media or control classification before the system call.
- TypeScript for the receiver, local dashboard, and minimal signaling service.
- Node.js with a small audited WebSocket and HTTP dependency set for signaling.
- coturn as an optional deployment dependency rather than reimplemented TURN.
- Catch2 or GoogleTest for native unit and integration tests.
- Playwright for browser interoperability and workflow tests.
- Python only for corpus generation, experiment orchestration, statistical analysis, OCR evaluation, and plots.
- FFmpeg or GStreamer command-line tools for independent decode and diagnostic comparison where license terms permit.
- Nsight Systems for end-to-end timeline evidence.
- Nsight Compute for targeted custom-kernel analysis.
- Docker only for signaling, receiver, evaluation, and reproducible services that do not require portal capture.

The sender must run as a normal desktop process because portals and GPU devices do not map cleanly into the primary container workflow.

## 11. Repository layout

```text
glyph-relay/
  BUILD_PLAN.md
  README.md
  LICENSE
  SECURITY.md
  THIRD_PARTY_NOTICES.md
  CONTRIBUTING.md
  CMakeLists.txt
  CMakePresets.json
  package.json
  pnpm-lock.yaml
  pyproject.toml
  uv.lock
  Makefile
  .env.example
  cmake/
  include/glyphrelay/
    capture/
    frame/
    saliency/
    encode/
    transport/
    telemetry/
    session/
  src/
    app/
    capture/
    cuda/
    saliency/
    encode/
    transport/
    telemetry/
  apps/
    receiver/
    dashboard/
    signaling/
  cuda/
    colorspace.cu
    saliency.cu
    morphology.cu
    map_reduce.cu
  tests/
    native/
    cuda/
    integration/
    browser/
    network/
    privacy/
  corpus/
    generator/
    scenes/dev/
    scenes/validation/
    scenes/final_test_pool/
    fonts.lock
    manifests/
  evaluation/
    configs/
    metrics/
    analysis/
    expected/
  benchmarks/
    scripts/
    profiles/
  artifacts/
    gpu-runs/              # ignored local copies of verified remote evidence
  docs/
    architecture.md
    prior-art.md
    capture.md
    nvenc-contracts.md
    transport.md
    benchmark-methodology.md
    privacy.md
    troubleshooting.md
    limitations.md
    adr/
  scripts/
    gpu/
      qualify_cuda_pm.sh
      run_remote_qualification.sh
```

Do not commit NVIDIA SDK archives, restricted sample source, generated recordings, private screen captures, profiler reports containing sensitive paths, or unreviewed codec binaries.

## 12. Command-line contract

```text
glyphrelay doctor [--json]
glyphrelay share [--bitrate PROFILE] [--record PATH] [--json]
glyphrelay record --output PATH.h264 [--window-label LABEL] [--bitrate PROFILE]
glyphrelay benchmark --manifest FILE --output DIR
glyphrelay inspect --recording FILE --json
```

The graphical sender may invoke the same application services.
The CLI remains the stable automation and debugging interface.
Both `share` and interactive `record` always request one window through the XDG ScreenCast portal, and neither command accepts a window identifier that could bypass or preselect the portal source.
The optional `--window-label` is a local non-authoritative display label applied only after the user completes portal selection, never enters source selection, telemetry, logs, signaling, recording metadata, or a remote payload, and defaults to no label.
Deterministic benchmark input comes only from `glyphrelay benchmark --manifest FILE`, so automated corpus runs never masquerade as portal-selected desktop capture.
Milestone 0 freezes one `recording_profile_v1` H.264 profile, maximum encoded level, semantic 8-bit 4:2:0 colorimetry and range contract, encoder-specific NV12 and I420 memory layouts, GOP bound, parameter-set policy, and SDP compatibility predicate that both target browsers pass at every V1 presentation profile.
The record-only command uses `recording_profile_v1` without a browser offer.
A sharing session also uses that frozen profile and rejects a browser offer that does not pass its profile, maximum-level, level-asymmetry, and packetization compatibility predicate.
A share without recording may defer encoder initialization until the receiver offer arrives, while `share --record` initializes `recording_profile_v1` immediately and later admits only a compatible receiver offer.
When a receiver joins an already recording share, transport starts a new dependency epoch and sends that receiver an IDR with SPS and PPS, while the recorder keeps its existing recording epoch and accepts the same ordinary keyframe into its continuous stream.
V1 recording output is an Annex B `.h264` elementary stream whose first accepted access unit is an IDR preceded by SPS and PPS and whose every later IDR is also preceded by SPS and PPS.
Starting recording during an active share arms a new recording epoch, requests one coalesced IDR with SPS and PPS, and accepts no encoded access unit into that recorder until the complete recovery access unit arrives.
Stopping and restarting recording creates a new recording identifier, new files, and a new IDR boundary rather than appending to an earlier elementary stream.
The media, sidecar, and completion-marker temporary files are unpredictable sibling paths derived from the random recording identifier in the destination directory so every rename has same-filesystem atomicity.
The durable journal uses one deterministic no-clobber companion name derived from the requested output path and is the recovery anchor whose synchronized header records the journal protocol version, header length, session identifier, random recording identifier, every temporary and final companion name, a `PREPARED` state, and a checksum of the canonical header payload.
The canonical header encoding uses a fixed field order, unsigned fixed-width little-endian integers, UTF-8 names preceded by unsigned 32-bit byte lengths, and a final SHA-256 field computed over every preceding header byte.
Before capture or encoding begins, the recorder opens the destination directory once, rejects a non-directory or unsafe directory, and checks every final and temporary companion name relative to that directory descriptor.
It creates the journal and every temporary regular file with `openat`, `O_CREAT`, `O_EXCL`, `O_NOFOLLOW`, close-on-exec, and mode `0600`, then verifies the file type and ownership with `fstat`.
It writes the complete `PREPARED` journal header, calls `fdatasync` on the journal, calls `fsync` on every initially created empty temporary file, and then calls `fsync` on the already opened destination-directory descriptor.
No capture frame, encoded access unit, or recording-branch admission may begin until that directory synchronization succeeds, so every admitted recording has a durable external recovery anchor and durable companion directory entries.
Failure of the journal, temporary-file, or initial directory synchronization fails initialization before media admission and leaves any surviving no-clobber companions reserved for explicit inspection or removal outside GlyphRelay.
Any existing media, journal, sidecar, completion marker, temporary companion, or symbolic link returns `OUTPUT_EXISTS` or `OUTPUT_INCOMPLETE_EXISTS` without modifying it.
Final publication uses `renameat2` with `RENAME_NOREPLACE` on the tested Linux release, and lack of safe no-replace support fails recording initialization rather than falling back to an overwriting rename.
V1 has no replace flag.
An incomplete artifact continues to reserve its base path until the user explicitly moves or removes it outside GlyphRelay, so retrying the same path cannot erase crash evidence.
The writer flushes complete access units, so a crash can truncate the final access unit without invalidating earlier durable complete units.
Completed `EncodedFrame` objects are immutable and are reference-counted into independent transport and recorder branches before any transport dependency-epoch purge.
The recorder queue is bounded by the lower of two seconds of encoded media or 64 MiB, and it may never block capture, CUDA, encode, or transport workers.
Recorder overload, a short write, `ENOSPC`, permission loss, or sidecar failure stops recording at the last durable complete access unit, preserves live sharing, and displays a persistent recording error.
The record-only command exits with encoder runtime failure code 5 when the recorder cannot continue.
For each complete access unit, the writer appends media bytes and stages a journal record containing the session ID, recording ID, media epoch, dependency epoch, geometry and encoder-configuration epochs, configuration hash, access-unit index, byte range, source-frame identity, extended RTP timestamp, picture type, keyframe and parameter-set flags, presentation timestamp, and checksum.
The recorder group-commits at least every 250 milliseconds of sender monotonic time and immediately on stop, error, IDR, or encoder-configuration change.
One group commit writes every pending media byte, calls `fdatasync` on the media file, appends the corresponding staged journal records plus the highest committed access-unit index, and then calls `fdatasync` on the journal before declaring that batch durable.
Journal records may describe only media bytes covered by the preceding successful media synchronization.
Bytes or in-memory records after the highest synchronized journal commit are an uncommitted tail and are discarded logically after a crash.
If a pending batch remains uncommitted for one second, the recorder enters its persistent failure path rather than accumulating an unbounded durability window.
Clean finalization synchronizes and closes the media and journal files, derives and synchronizes the JSON sidecar temporary file, renames the media and sidecar to their final sibling names, and synchronizes the parent directory.
It then writes and synchronizes a temporary completion marker containing the session ID, final media and sidecar sizes, and their hashes, renames the marker into place, and synchronizes the parent directory again.
The valid completion marker is the only commit record for the multi-file recording, and a final-name media or sidecar file without that marker remains an incomplete artifact.
The durable journal remains available until the completion marker commits, and removing a redundant journal after commit requires one final parent-directory synchronization.
`glyphrelay inspect` resolves an incomplete recording only through the deterministic journal companion, validates its synchronized `PREPARED` header before following any directory-relative companion name, validates the marker, hashes, journal offsets, access-unit checksums, and every partial or final-name permutation, treats a valid header with no committed access unit as a prepared incomplete artifact, truncates any later incomplete artifact logically to its last durable journaled complete access unit, reports an incomplete status, and never guesses metadata for an unjournaled tail.
Crash-injection tests stop the process after every creation, header write, media write, file synchronization, journal write, sidecar creation, rename, marker write, and directory synchronization to prove that exactly one complete state or one recoverable incomplete state is reported.
A filesystem crash model that discards every unsynchronized file and directory update must independently prove the initial `PREPARED` barrier, every group-commit ordering point, and every publication ordering point.
Transport queue purges do not remove access units already accepted by the recorder branch, and recorder failure does not change the transport dependency epoch.

Exit codes are stable:

`OUTPUT_EXISTS`, `OUTPUT_INCOMPLETE_EXISTS`, an unsafe path, or unavailable no-replace rename support is an initialization failure with exit code 2 and never an encoder runtime failure.

```text
0 success
2 invalid configuration or arguments
3 unsupported capture or GPU capability
4 permission denied or revoked
5 encoder initialization or runtime failure
6 transport or signaling failure
7 benchmark gate failed
8 internal invariant violation
```

## 13. Doctor contract

`glyphrelay doctor` reports:

- Operating system and architecture.
- Desktop session type.
- XDG portal availability.
- XDG portal backend and advertised source types.
- Advertised hidden, embedded, and metadata cursor modes.
- PipeWire version.
- GPU model and architecture.
- NVIDIA driver version.
- CUDA driver and runtime version.
- CUDA toolkit version when building.
- Maximum supported NVENC API version.
- Pinned header and SDK version.
- Frozen `recording_profile_v1` hash and browser compatibility decision.
- Pinned header hash, license identifier, and minimum driver API version.
- H.264 encode support.
- Emphasis-map support.
- Supported input formats.
- Maximum supported width and height.
- Maximum session count where queryable.
- DMA-BUF import availability for the chosen capture format.
- Shared-memory fallback availability.
- CPU encoder availability.
- Screen-lock detection and capture-revocation hook availability.
- Browser and TURN configuration checks where requested.
- Direct UDP or TURN-over-UDP cap-accounting support, selected IP family, and any wire-cap-unverified transport reason.
- Final mode decision and reasons.

The JSON schema is versioned.
Doctor output is safe to attach to a bug report and must not contain usernames, full filesystem paths, window titles, or credentials.

## 14. Frame and ownership contracts

```text
FrameId
  session_id
  media_epoch
  monotonic_sequence
  dequeue_timestamp_ns
  producer_timestamp_optional
  producer_clock_domain_optional
  producer_timestamp_provenance

CapturedFrame
  frame_id
  width
  height
  pixel_format
  color_range
  color_matrix
  stride_bytes
  crop
  memory_kind: SHM | DMABUF
  memory_handle
  pipewire_stream_token
  release_on_pipewire_loop

DeviceSourceFrame
  frame_id
  geometry_epoch
  pixel_format
  source_width
  source_height
  source_pitch
  source_crop
  device_source_pointer_or_import
  source_read_complete_event
  pipewire_release_token_if_imported
  owner_token

EncoderSurfaceSlot
  frame_id
  geometry_epoch
  coded_width
  coded_height
  nv12_base_pointer
  nv12_pitch
  luma_offset
  chroma_offset
  registered_resource
  mapped_resource
  cuda_ready_event
  owner_token

EncoderSubmissionSlot
  submission_sequence
  frame_id
  dependency_epoch
  encoder_surface_slot_id
  host_emphasis_map_slot_id
  output_bitstream_buffer
  encode_return_status
  output_byte_count
  submission_state
  owner_token

DeviceEmphasisMap
  frame_id
  macroblock_width
  macroblock_height
  device_values_pointer
  generation_timestamp_ns
  protected_fraction
  owner_token

HostEmphasisMapSlot
  frame_id
  macroblock_width
  macroblock_height
  host_values_pointer
  byte_size
  device_to_host_ready_event
  owner_token

EncodedFrame
  frame_id
  media_epoch
  rtp_timestamp
  keyframe
  picture_type
  dependency_epoch
  parameter_sets_present
  encoded_bytes
  encode_start_ns
  encode_ready_ns
  owner_token

RtpPacket
  frame_id
  media_epoch
  dependency_epoch
  access_unit_id
  ssrc
  extended_sequence
  wire_sequence
  extended_rtp_timestamp
  wire_rtp_timestamp
  keyframe_or_parameter_set_role
  protected_packet_bytes
  owner_token
```

Ownership must be explicit and move-only across mutable stages.
After bitstream acquisition, an `EncodedFrame` payload is immutable and reference-counted, while each transport or recorder branch handle remains move-only.
The shared-memory path copies the visible pixels into an owned bounded staging slot before returning the PipeWire buffer.
The CPU path copies the shared-memory frame into an owned CPU slot before returning the PipeWire buffer.
The DMA-BUF path may retain the PipeWire buffer only until the CUDA import read completes and its completion event is observed.
PipeWire buffers are always requeued on the PipeWire processing loop through `release_on_pipewire_loop`.
The packed-RGB device source and the NV12 encoder surface are distinct allocations and may never alias.
The shared-memory GPU path copies pinned BGRA or RGBA staging bytes into a bounded `DeviceSourceFrame`, then converts from that source into a distinct `EncoderSurfaceSlot`.
The DMA-BUF path reads the imported packed-RGB source through CUDA and writes the result into the same distinct NV12 encoder-surface abstraction.
DMA-BUF source ownership ends when `source_read_complete_event` is observed and its PipeWire buffer is requeued on the PipeWire loop.
NV12 encoder-surface ownership continues independently until NVENC has completed its documented final use.
No device source, CUDA encoder surface, host emphasis-map slot, registered resource, submission slot, or output slot may be reused until its matching ownership boundary is reached.
The H.264 NV12 resource is one contiguous semi-planar allocation with a single registered base address, one pitch, and explicit luma and chroma offsets.
Every pool is bounded and its maximum in-flight count is configurable within safe limits.

The process retains one CUDA primary context for the selected GPU with `cuDevicePrimaryCtxRetain` before creating any runtime allocation, stream, event, imported object, or NVENC session.
The same retained `CUcontext` is passed when the NVENC session is opened with the CUDA device type.
Every host thread that calls a CUDA driver API, a CUDA runtime path, or an NVENC resource API enters through an RAII context guard that makes that context current for the call scope and restores the prior per-thread context on exit.
CUDA streams, events, device allocations, imported DMA-BUF objects, registered NVENC resources, and their owner tokens record the selected device and context identity.
A resource from another device or context is rejected before registration, mapping, event wait, or submission.
Shutdown first closes admission, joins work producers, observes or cancels all context-owned events, drains NVENC, unregisters and unmaps NVENC resources, destroys streams and allocations, joins every context-using worker, and releases the retained primary context last.
No PipeWire callback is moved onto a CUDA worker, and PipeWire requeue remains on its own processing loop.

Each shared-memory device-source slot follows `FREE -> HOST_TO_DEVICE_PENDING -> CUDA_SOURCE_READ_PENDING -> FREE`.
Each imported DMA-BUF source follows `PIPEWIRE_OWNED -> CUDA_SOURCE_READ_PENDING -> PIPEWIRE_REQUEUE_PENDING -> RELEASED`.
The conversion stream records `source_read_complete_event` only after its final packed-RGB read, and that event controls source reuse or PipeWire requeue independently of the NV12 encoder slot.

Each encoder slot follows this state machine:

```text
FREE
  -> CUDA_WRITING
  -> MAP_COPY_PENDING
  -> READY_TO_SUBMIT
  -> SUBMITTED
  -> ENCODER_INPUT_RELEASED
  -> FREE
```

Each encoder submission slot follows this state machine:

```text
FREE
  -> RESERVED
RESERVED
  -> SUBMIT_RETRY_PENDING
  -> RESERVED
RESERVED
  -> SUBMITTED_PENDING_OUTPUT
  -> BITSTREAM_LOCKABLE
  -> BITSTREAM_LOCKED
  -> FREE
```

Every call to `nvEncEncodePicture` passes the output buffer owned by that exact submission slot in `NV_ENC_PIC_PARAMS::outputBitstream`.
The encode worker maintains one FIFO of accepted submission slots ordered by `submission_sequence`.
An `ENCODER_BUSY` return keeps the slot in `SUBMIT_RETRY_PENDING` and retries the same picture parameters without inserting a second FIFO entry.
A `NEED_MORE_INPUT` return inserts the slot once as `SUBMITTED_PENDING_OUTPUT`, keeps its surface and host map alive, and forbids locking its output buffer until a later accepted submission or EOS permits FIFO output acquisition.
Each accepted submission or EOS drain makes the output worker acquire only the FIFO head, and a successful lock can release only the output buffer, surface, and host map named by that head.
The synchronous Linux output worker may block in bitstream acquisition, but capture, CUDA, and packet transport workers may not wait on that lock.
EOS drains the FIFO in order, and a fatal status transitions every affected slot through the documented abort path without reusing any input, map, or output handle that NVENC may still reference.
Only the owner of a state transition may mutate the slot.
Cancellation moves a slot through a documented drain or abort transition and never directly destroys a resource still owned by CUDA or NVENC.
The implementation must handle `NV_ENC_SUCCESS`, `NV_ENC_ERR_NEED_MORE_INPUT`, `NV_ENC_ERR_ENCODER_BUSY`, end-of-stream, and fatal statuses without losing submission order.

## 15. Queueing and backpressure

The media path uses bounded single-producer or multi-producer queues chosen for the actual stage topology.
Correctness is more important than selecting a lock-free structure prematurely.

The policy is latest-frame-wins:

- Never block the capture thread on network delivery.
- If preprocessing is behind, discard the oldest not-yet-processed frame.
- If encoding is behind, discard stale preprocessed frames before submission.
- Do not discard an arbitrary encoded H.264 access unit because a later P picture may depend on it.
- If the encoded queue reaches its hard bound, purge the queued dependency epoch, request a new IDR with SPS and PPS, and resume transport only from that recovery point.
- Post-encode dropping of an individual access unit is forbidden in V1 unless encoder output metadata proves that it is non-reference and a dedicated browser recovery test covers that exact configuration.
- Record every drop with stage and reason.
- Never grow a queue to preserve throughput at the cost of interactive latency.

The default end-to-end queue budget must be no more than two to three frame intervals per stage chain at 30 fps.
Exact capacities are tuned through overload tests and recorded in configuration.
The RTP pacer queue has a 100-millisecond maximum packet age and a 4 MiB hard byte cap, with the lower limit taking precedence.
The retransmission cache has a 500-millisecond maximum packet age, a 2,048-packet cap, and a 4 MiB hard byte cap, with the first reached limit evicting oldest packets.
Each RTP packet may be retransmitted at most twice during its cache lifetime.
The sender processes at most 100 distinct NACKed packet identifiers and ten RTCP recovery-feedback messages in any rolling one-second interval.
Duplicate NACK identifiers inside one feedback message or interval consume one identifier budget entry but never bypass the per-packet retransmission limit.
Every retransmission consumes the ordinary media-pacer budget and is included in controller retransmission and wire counters.
Excess authenticated feedback is ignored with a bounded diagnostic counter, and a sustained ten-second feedback flood ends the peer session visibly.
The retransmission cache is cleared on dependency-epoch reset and erased immediately when the session stops or is revoked.
Every pacer packet carries its frame ID, media epoch, access-unit ID, dependency epoch, SSRC, extended and wire RTP sequence values, extended and wire RTP timestamps, and IDR or parameter-set role.
The retransmission cache key is `(media_epoch, dependency_epoch, ssrc, extended_sequence)`, while a 16-bit Generic NACK resolves only to the unique active-epoch cached packet whose low 16 sequence bits match.
An absent or ambiguous NACK match cannot be retransmitted and instead enters the coalesced recovery-IDR path.
Milestone 0 selects one sequence-number owner in the pinned libdatachannel integration, records how its application-visible 64-bit extended sequence maps to the library's wire sequence and SRTP rollover state, and forbids an independent second sequence allocator.
The final UDP send wrapper is the sole owner of every nonblocking media-session UDP socket and one session-wide `MediaEgressGate` that serializes the final epoch validation with the pinned transport's final nonblocking UDP socket-send call across direct and TURN paths.
Milestone 0 must locate and lock the exact source-level datagram-emission hook in libdatachannel and its ICE dependency rather than assuming that a public high-level callback reaches this boundary.
The hook must expose direct IPv4, direct IPv6, TURN-over-UDP, SRTP, RTCP, SCTP, DTLS, STUN, and TURN datagrams to one classifier and counter without creating a second socket, sequence allocator, encryption path, or retransmission path.
If the pinned upstream APIs cannot provide that hook, the project must maintain a minimal isolated MPL-compatible source patch with tests and public source obligations, or select another transport integration before Milestone 1.
Runtime interposition such as `LD_PRELOAD`, process-wide syscall wrapping, packet sniffing, and destination-port guessing are not acceptable production ownership or classification mechanisms.
To send media, the wrapper acquires a short-lived gate permit, verifies the packet epoch while holding that permit, calls the pinned transport's final nonblocking UDP socket-send operation while the permit remains held, records a successful result, and releases the permit without performing pacing or encoding work inside the critical section.
For pinned libjuice v0.24.1 this operation is `sendto`, and any later transport change must relock the exact syscall boundary before qualification.
Stop, pause, lock, revocation, and permission loss acquire the same gate exclusively, close admission, advance the active epoch, and define the local media-revocation boundary at that linearization point after every earlier final UDP socket-send invocation has returned.
No media sender can pass validation before that boundary and invoke the final UDP socket-send operation after it.
A packet rejected by the gate or a failed or short final UDP socket-send call does not increment wire egress.
The pacer admits all packets for one access unit atomically only when the complete packet batch fits the current byte and age budgets.
If a queued media packet reaches the age bound or a new access unit would cross the hard byte bound, the sender marks that dependency epoch abandoned, stops accepting its media packets, purges all of its unsent pacer packets, and clears its retransmission entries.
The sender then discards every later encoded access unit from the abandoned epoch, requests a new IDR with SPS and PPS, and resumes media only when the complete recovery access unit can be admitted.
Feedback-triggered IDR requests are coalesced by dependency epoch and may force at most one new recovery IDR in any rolling one-second interval.
Startup, local resume, and an explicit controller dependency-epoch transition may force an IDR immediately, but they consume and reset the same recovery state so a simultaneous PLI or NACK miss cannot create a second IDR.
Arming a recorder or admitting a receiver to an already recording share may also force one immediate IDR and consumes the same coalescing state so simultaneous startup, PLI, NACK-miss, and recorder requests produce one keyframe.
If the complete recovery access unit cannot fit after the controller reaches `720p15` and its minimum encoder target, the session enters `UNUSABLE` instead of requesting an unbounded sequence of IDRs.
Control and authenticated stop traffic may bypass this media purge but remains subject to its independent message bounds and wire accounting.
No pacer capacity action may evict one fragment, one arbitrary access unit, or one P picture and then continue the same dependency epoch.

## 16. Capture path

Use the XDG ScreenCast portal for explicit user-mediated window selection.
Do not bypass the permission flow with unrestricted desktop scraping.
The portal dialog is the sole authority for selecting the captured window in both the graphical and CLI product paths.
A CLI source label may describe the already selected source locally but cannot identify, filter, preselect, reopen, or persist a window.

The capture implementation must handle:

- Portal request, response, and cancellation.
- CLI and graphical entry points that both reach the same portal-owned source-selection service.
- PipeWire node negotiation.
- Format and modifier negotiation.
- Portal source-type and cursor-mode capability negotiation.
- Shared-memory buffers.
- DMA-BUF buffers when supported.
- Stride, crop, orientation, and damage metadata.
- Resolution changes.
- Window closure.
- Permission revocation.
- Portal and PipeWire disconnect.
- Session teardown.

The mandatory GPU path copies shared-memory pixels into an owned pinned staging slot, schedules the PipeWire buffer for requeue after that host copy, uploads the staging slot into a private packed-RGB device source, and converts from that source into a distinct registered NV12 encoder surface.
The mandatory CPU path follows the same prompt-requeue rule with an owned CPU frame slot.
DMA-BUF import is an optimization only after the shared-memory path is correct.
Do not describe the pipeline as zero-copy if any full-frame host or device copy remains in the measured path.

The sender requests window sources only and sets `multiple=false`.
It uses `persist_mode=0` and never stores a portal restore token in V1.
It probes `AvailableCursorModes`, prefers metadata cursor mode, consumes PipeWire cursor position and bitmap metadata, and composites the cursor into the output when needed.
If the portal provides only embedded cursor pixels, the stream may include that cursor but cursor-halo emphasis is disabled and labeled unavailable.

All regions use source-visible pixel coordinates as the canonical coordinate space.
Capture crop, high-DPI scale, controller downscale, even-dimension padding, coded dimensions, and macroblock rounding are represented as explicit immutable transforms attached to each frame geometry epoch.
Pinned rectangles, exclusions, cursor coordinates, preview overlays, and evaluation truth must pass through the same tested transform.
Resolution or crop changes create a new geometry epoch, drain the old encoder dependency epoch, and regenerate all derived maps.

## 17. CUDA preprocessing

### 17.1 Color conversion

Implement pitch-aware BGRA or RGBA to NV12 conversion.
Support the exact color matrix and range negotiated from capture.
The first public operating mode uses BT.709 with an explicitly declared limited or full range.

The kernel must:

- Handle arbitrary valid pitch.
- Handle even and odd visible crops safely.
- Produce separate luma and interleaved chroma planes.
- Use correct chroma subsampling.
- Avoid out-of-bounds reads on partial edge blocks.
- Match the scalar reference with an absolute error of at most one code value for every Y, U, and V output sample.

### 17.2 Automatic saliency

The V1 algorithm is deterministic and non-neural.
Its initial protocol version is `saliency_v1`.
It operates on 8-by-8 visible-luma tiles before reducing them into H.264 macroblocks.
The feature protocol derives both a normalized luma value and a canonical integer code value from each declared full-range or limited-range sample without per-frame minimum-maximum normalization.
The canonical integer code value is from zero through 255 after declared range expansion with round-to-nearest and ties-to-even, and the normalized value is that integer divided by 255.
Scharr horizontal gradients use rows `[-3, 0, 3]`, `[-10, 0, 10]`, and `[-3, 0, 3]`, and vertical gradients use the transpose of that kernel.
Samples outside the visible image use clamp-to-edge extension, and no feature search crosses a tile boundary except the three-by-three Scharr footprint.
Normalized horizontal and vertical component magnitudes are `abs(Gx) / 4080` and `abs(Gy) / 4080`.
Normalized gradient magnitude is `min((abs(Gx) + abs(Gy)) / 4080, 1)` and a pixel is high-gradient only when that value is strictly greater than 0.12.
For a sorted tile of `N` normalized luma samples, percentile `p` is element `floor(p * (N - 1))`, and local contrast is percentile 0.90 minus percentile 0.10.
A high-gradient pixel belongs to a small structure when its maximal contiguous high-gradient run in at least one horizontal or vertical direction has length from one through three pixels.
Small-structure density is the number of distinct qualifying high-gradient pixels divided by `max(high_gradient_pixel_count, 1)`.
An edge-pair anchor is one tile pixel and one horizontal or vertical orientation for which at least one partner from one through twelve pixels away remains in the same tile.
A horizontal pair requires both horizontal component magnitudes to exceed 0.12 and their signed `Gx` values to have opposite signs, and a vertical pair applies the same rule to `Gy`.
An anchor qualifies when any allowed separation contains its matching pair, and edge-pair density is qualifying anchors divided by `max(eligible_anchor_count, 1)` with zero returned when no anchor is eligible.
Gradient density is high-gradient pixel count divided by visible tile pixel count, including partial edge tiles.
Clamp each resulting fraction or normalized contrast to the interval from zero to one without data-dependent rescaling.
The initial raw score is `0.35 * gradient_density + 0.25 * local_contrast + 0.25 * edge_pair_density + 0.15 * small_structure_density`.
Compute temporal stability as `1 - min(mean_absolute_normalized_luma_change / (32 / 255), 1)` against the most recent processed frame in the same geometry epoch.
The first processed frame of an epoch and a frame whose predecessor gap exceeds 200 milliseconds use temporal stability one and establish a new prior.
Dropped capture frames are not synthesized, and the next processed frame compares only with the most recent processed frame when its gap remains within 200 milliseconds.
Multiply the raw score by `0.75 + 0.25 * temporal_stability` so stable glyphs receive a modest preference without suppressing newly appearing text.
Update the tile score as `0.60 * previous_score + 0.40 * current_score` after the first frame of a geometry epoch.
A tile enters protection at a score of 0.55 and exits only below 0.40.
An inactive tile remains level zero until it reaches the entry threshold, and an active tile remains active until it falls strictly below the exit threshold.
Quantize an active automatic score below 0.55 to level one, a score from 0.55 through 0.69 to level two, a score from 0.70 through 0.84 to level three, and a score at or above 0.85 to level four.
Quantize every inactive tile to level zero regardless of its score below the entry threshold.
After quantization, automatic dilation assigns each destination tile the maximum automatic level among source tiles within the selected Chebyshev radius and clips the neighborhood to visible tile bounds.
Exclusions, pins, and cursor precedence are applied after automatic dilation.
The implementation validates those numeric levels against the enum values in the pinned header and refuses enhanced mode if they are not representable.
User-excluded rectangles have highest precedence and force level zero.
User-pinned rectangles have second precedence and force a configurable minimum that defaults to level four.
The cursor halo has third precedence and forces a configurable minimum that defaults to level three.
Automatic saliency has the lowest precedence.
If a user pin overlaps an exclusion, the exclusion wins and the preview displays the conflict.
Macroblock reduction takes the maximum surviving tile level inside each macroblock.
The development search grid permits each feature weight from `{0.15, 0.25, 0.35, 0.45}` subject to nonnegative weights summing to one, entry thresholds from `{0.50, 0.55, 0.60}`, exit thresholds from `{0.30, 0.35, 0.40}` strictly below entry, previous-frame temporal coefficients from `{0.40, 0.60, 0.80}`, and dilation radii from `{0, 1, 2}` tiles.
The development selector first discards configurations that miss any Milestone 4 map threshold on the development split, then maximizes recall for the defined 8-to-10-pixel rendered-glyph-height subset, and then breaks ties by lower false-protected fraction, lower protected fraction, lower map-change fraction, lower processing time, and lexicographic configuration order.
The selected development configuration replaces the stated initial weights, entry and exit thresholds, temporal coefficient, and dilation radius as one immutable configuration hash.
If no development configuration passes, the project stops before opening validation and creates a new protocol version rather than weakening the frozen validation gate.
All other operator choices are fixed for `saliency_v1`.
The protocol includes hand-calculated scalar feature vectors for uniform, single-edge, opposite-edge, isolated-pixel, border, partial-tile, dropped-frame, and geometry-reset fixtures.
Replacing an operator or introducing another glyph proxy requires a new protocol version, an ADR, fresh development and validation manifests, a fresh final-test generation commitment, and a rerun of every automatic-map gate.

Do not call the output a text detector unless evaluated as one.
The internal term is `protected-region saliency`.

### 17.3 Macroblock reduction

For H.264, calculate map dimensions from the actual coded width and coded height passed to NVENC.
The required shape is `ceil(coded_width / 16) * ceil(coded_height / 16)` entries and the required byte size is exactly that entry count because every entry is one signed byte.
Visible odd dimensions are padded to an even NV12 coded surface before macroblock rounding, and the H.264 crop or display geometry is recorded separately.

The reduction kernel must:

- Produce raster-scan order.
- Apply pinned and excluded regions deterministically.
- Clamp levels to the header-defined enum range.
- Report protected fraction and level histogram.
- Produce a device map and copy it asynchronously into a distinct pinned host map slot.
- Attach a CUDA event that proves the host map is ready before encoder submission.
- Preserve one device map and one host map slot per in-flight frame.
- Reject any size, geometry epoch, frame ID, or pointer-address-space mismatch before calling NVENC.

### 17.4 Optimization sequence

Start with separate kernels and correct intermediate buffers.
Profile before fusion.
Fuse colorspace, luma feature extraction, or tile reduction only when end-to-end evidence shows that global-memory traffic or launch overhead is material.

Every custom kernel has:

- A scalar CPU reference.
- Deterministic golden inputs.
- Boundary-size tests.
- Randomized differential tests.
- CUDA error checks.
- Sanitizer or compute-sanitizer coverage where available.

## 18. NVENC integration

### 18.1 Initialization

- Load the API through the official function table.
- Validate maximum supported API version.
- Select H.264.
- Select the low-latency preset and tuning information supported by the pinned SDK.
- Configure the frozen `recording_profile_v1` profile and maximum level, which must carry every V1 resolution and frame-rate profile and requires no browser offer in record-only mode.
- For sharing, parse the browser SDP offer and accept only an offered H.264 payload with `packetization-mode=1` whose profile, level, and level-asymmetry rules are compatible with `recording_profile_v1`.
- Configure NVENC so the emitted SPS profile and level remain within the accepted `profile-level-id` contract.
- Set H.264 video usability information for the same color matrix, range, and primaries used by preprocessing.
- Disable B frames, lookahead, hierarchical prediction, and other output reordering in V1.
- Configure a bounded GOP and keyframe request path.
- Emit SPS and PPS at startup, every IDR, and every dependency-epoch recovery point.
- Configure rate control and VBV explicitly.
- Disable spatial and temporal AQ in every emphasis-map session because the pinned SDK marks them incompatible.
- Use the same AQ-disabled configuration for the controlled uniform isolation baseline.
- Register input resources and allocate a bounded output ring.
- Register each private contiguous NV12 allocation as `NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR` with its actual coded width, coded height, pitch, and `NV_ENC_BUFFER_FORMAT_NV12`.

### 18.2 Per-frame submission

For every frame:

1. Wait on or synchronize with the CUDA-ready event without blocking unrelated stages.
2. Wait for the matching device-to-host emphasis-map event.
3. Map the registered contiguous NV12 input resource.
4. Reserve one free `EncoderSubmissionSlot` and attach its output buffer to the picture parameters.
5. Attach the matching pinned host emphasis-map pointer and exact `qpDeltaMapSize`.
6. Attach the frame timestamp, dependency epoch, submission sequence, and keyframe flags.
7. Submit with `nvEncEncodePicture`.
8. On `NV_ENC_ERR_ENCODER_BUSY`, retry the same fully owned submission after a bounded wait without adding another pending entry or releasing any resource.
9. On `NV_ENC_ERR_NEED_MORE_INPUT`, append that submission to the FIFO exactly once and do not lock its output buffer until a later accepted submission or EOS makes FIFO output acquisition valid.
10. On `NV_ENC_SUCCESS`, append the submission exactly once if it is not already pending and let the synchronous Linux output worker acquire completed bitstreams from the FIFO head.
11. Parse or record picture type, keyframe status, parameter-set presence, output bytes, and timing against the matching submission sequence.
12. Unlock the bitstream, unmap the input resource, and release input, map, submission, and output slots only after their documented final use.

An armed recorder begins only when this path produces an independently decoded IDR access unit with SPS and PPS under its new recording epoch.
If the keyframe request fails or the recovery access unit is malformed, recording fails before publishing any media byte while an existing share may continue visibly.

Flush and teardown must handle end-of-stream, encoder errors, and partial initialization.
No resource may be destroyed while the encoder can still reference it.
The EOS path drains every delayed submission, unlocks every output buffer, and proves all slots returned to `FREE` before destroying the encoder.

### 18.3 Reconfiguration

The controller may change bitrate, VBV, frame rate, and selected emphasis behavior only through supported reconfiguration paths.
Every reconfiguration records old and new parameters, return status, affected frame, and any forced keyframe.
Unsupported reconfiguration requires a safe encoder restart with a visible transport discontinuity.

## 19. Protected-region controller

Emphasis maps can violate the requested bitrate because QP adjustment occurs after rate control.
GlyphRelay therefore controls measured output rather than trusting configuration alone.

Inputs include:

- Encoded bytes per frame.
- One-second and five-second elementary-stream payload bitrate.
- Sender wire-egress bitrate.
- Negotiated RTCP feedback, loss, round-trip time, and receiver decode statistics.
- Queue age and depth.
- Drop rate.
- Protected fraction.
- Map level histogram.
- Encode latency.

Control knobs are applied in a documented order:

1. Reduce automatic emphasis level while preserving user-pinned regions.
2. Tighten the protected-area threshold and hysteresis.
3. Reduce the NVENC payload target and VBV in 10 percent steps through a supported reconfiguration path.
4. Move down one frozen presentation profile under sustained congestion.
5. Drop stale frames before encoder submission.

The numbered order is logical, and the stale-frame rule is continuously active rather than waiting for the other knobs to exhaust.
The automatic emphasis cap levels are exactly `4, 3, 2, 1, 0`, and the cap never lowers a user-pinned or cursor minimum.
For threshold delta `d` in `0.00, 0.05, 0.10, 0.15`, the controller entry threshold is `min(base_entry + d, 1)` and its exit threshold is `min(base_exit + d, entry_threshold - 0.10)`.
Encoder and VBV level `k` uses `max(base_payload_target * 0.90^k, frozen_profile_minimum)`.
The presentation profiles are exactly `1080p30`, `1080p24`, `720p24`, and `720p15` in degradation order.
Each presentation-profile transition starts a new geometry and dependency epoch and requires an IDR with SPS and PPS.
User-pinned regions remain at their declared minimum through every usable controller state, and a pinned-region violation is displayed rather than silently weakening the pin.
The complete initial level, minimum, and restoration stack is serialized into the `controller_v1` manifest before controller implementation begins.

The initial controller protocol is `controller_v1`.
It runs one control tick every 100 milliseconds from cumulative monotonic counters rather than from resettable interval counters.
It maintains one-second and five-second exponentially weighted estimates of elementary-stream bytes, wire-egress bytes, retransmission bytes, loss, round-trip time, queue age, and delivered frames.
For tick interval `dt`, estimate time constant `tau`, current sample `x`, and prior estimate `e`, every update uses `alpha = 1 - exp(-dt / tau)` and `e_next = alpha * x + (1 - alpha) * e` with IEEE 754 binary64 arithmetic.
Byte and frame samples are the nonnegative difference between consecutive cumulative counters divided by monotonic `dt`, and a counter decrease resets that estimator instead of producing a negative sample.
A feedback sample is eligible only after its sender arrival event and is ignored after two seconds without a newer valid sample.
A controller tick consumes only events whose sender monotonic arrival sequence precedes that tick, and equal-timestamp events are ordered by their recorded arrival sequence.
A stale or unavailable loss or round-trip estimate is marked unavailable rather than substituted with zero, and its predicate is omitted while local wire, pacer, and queue predicates remain active.
A REMB estimate is usable only when its negotiated payload type and RTCP source are valid and it is no more than two seconds old.
The effective wire cap is the lower of the user cap and 90 percent of fresh REMB when REMB is available.
When REMB is absent or stale, the effective cap remains the user cap and the state machine relies on local wire egress, RTCP loss and round-trip time, and queue age.
The controller reserves the greater of 10 percent of the effective cap or 64 kbps for RTCP, STUN, DTLS, SCTP control messages, and retransmission variability.
The remaining budget is the encoder-payload and RTP-pacer target.
The RTP pacer uses a token bucket refilled from monotonic time at that target rate with a burst capacity equal to 100 milliseconds of target bytes.
RTCP, STUN, DTLS, and authenticated stop traffic bypass the media pacer but remain in wire-egress accounting.

`controller_v1` has `STABLE`, `RATE_PRESSURE`, `CONGESTED`, `RECOVERY`, and `UNUSABLE` states.
It leaves `STABLE` for `RATE_PRESSURE` after three consecutive ticks with one-second wire egress above 95 percent of the effective cap, oldest pacer age above 50 milliseconds, fresh RTCP loss above 3 percent, fresh round-trip time above 250 milliseconds, or the configured encoder target above the newly computed payload budget.
It enters `CONGESTED` immediately when one-second wire egress exceeds 110 percent of the effective cap, oldest pacer age reaches 100 milliseconds, or the pacer reaches a hard byte limit.
In `RATE_PRESSURE` or `CONGESTED`, it applies at most one knob step every 500 milliseconds and waits for two full seconds of one-second-estimator observations before reversing that step.
It enters `RECOVERY` only after one continuous second below 85 percent of the effective cap with RTCP loss below 1 percent and oldest media age below one frame interval.
In `RECOVERY`, it restores one knob step every two seconds in the reverse order of degradation and returns to `STABLE` after five continuous compliant seconds.
Entry into `RECOVERY` requests an IDR with SPS and PPS so the browser receives a clean recovery point before quality restoration continues.
The frozen recovery profiles contain at most three outstanding presentation-profile steps from `720p15` to `1080p30`.
It enters `UNUSABLE` when the path is already at 720p15, automatic emphasis is at level zero, the minimum encoder target is active, and wire egress or queue age still violates its hard limit for three seconds.
The controller then stops media with a visible unusable-link state rather than accumulating delay.
Changes to resolution always start a new geometry and dependency epoch and require an IDR with SPS and PPS.
Every state transition, estimator input, knob step, and reversal is recorded in a machine-readable trace.
Every trace event records a monotonic arrival sequence, sender arrival time, source time when present, dependency epoch, prior state, estimator inputs, deterministic rounding results, selected level stack, action, and resulting state.
The offline controller simulator consumes that trace in arrival order without future events and must reproduce the production decision stream byte-for-byte.
The exact controller manifest is frozen before validation controller evaluation and may not change before final-test evaluation.
Deterministic trace fixtures cover a stable link, emphasis overshoot, stale REMB, missing REMB, sudden collapse, high RTT without loss, recovery, and a path that reaches `UNUSABLE`.

User-pinned regions may receive a minimum protected level, but the UI must warn when the bandwidth target cannot be respected.
The controller may not lie by reporting configured bitrate as measured bitrate.

The product records two distinct bitrate metrics:

- Elementary-stream payload bitrate counts all emitted Annex B access-unit bytes.
- Wire-egress bitrate counts every sender media-path byte for RTP, SRTP, RTCP, SCTP, DTLS, STUN, retransmission, and TURN traffic at the IP layer.

The user-selected bandwidth cap controls wire-egress bitrate.
Signaling HTTP and static asset downloads are reported separately and do not count toward the steady media cap.
TURN tests count sender egress to the relay and label the relay condition explicitly.
The capped V1 transport supports direct UDP ICE and TURN over UDP.
TURN over TCP or TLS may be diagnosed experimentally, but a session using it is labeled wire-cap-unverified and cannot contribute to the cap-compliance claim.
The pinned libdatachannel transport is wrapped at its sole final nonblocking UDP socket-send boundary by the linearizable `MediaEgressGate` and a rootless `DatagramEgressCounter` that observes every successful media-session datagram after ICE, DTLS, SRTP, STUN, and TURN framing.
For each successful datagram, the production counter adds the exact UDP payload length, eight UDP-header bytes, and either the 20-byte IPv4 base header or the 40-byte IPv6 base header selected by the connected socket.
The capped mode rejects IPv4 options, IPv6 extension-header configurations, IP fragmentation, and any transport path that bypasses this wrapper because their IP-layer size is not represented by that contract.
Failed or short final UDP socket-send calls add no egress bytes and enter the transport error path.
Ethernet, Wi-Fi, VPN, and other link-layer overhead is outside the named wire-egress metric and is reported as such.
The authoritative validation measurement is a packet capture or equivalent kernel counter filtered to the session sockets and cross-checked against the rootless production counter.
The production counter must agree with packet-capture IP total lengths exactly for deterministic fixtures and within 1 percent over every steady benchmark run before it may drive or display the cap claim.
One-second windows advance every 100 milliseconds after a ten-second warmup during a 120-second steady segment.

Stable-link gate at 1080p30 source resolution:

- P95 one-second wire-egress bitrate must remain within 110 percent of the selected cap on the frozen evaluation profiles.
- At least 24 frames per second must reach the browser compositor over the steady segment.
- At least 95 percent of frames submitted to transport must decode and reach the compositor.
- The controller may not reduce resolution in the stable-link primary matrix.
- P95 PipeWire-dequeue-to-render latency must remain at or below 250 milliseconds on the named 50-millisecond-RTT, zero-loss profile.

Overload and recovery gate:

- The controller may reduce to no less than 720p15 before declaring the connection unusable.
- Queues remain within their hard capacities during a 30-second bandwidth collapse.
- The recovery timer starts at the first 100-millisecond controller tick after the test harness restores the named link profile.
- A recovery IDR reaches the browser and P95 frame age returns within 10 percent of its pre-collapse value within two seconds of that timer start.
- Delivered frame rate and resolution return within 10 percent of their pre-collapse values within `2 + 2 * N` seconds, where `N` is the number of outstanding presentation-profile steps at restoration, with an absolute maximum of eight seconds.
- A dependency-epoch reset causes no browser corruption and no freeze longer than one second after the recovery IDR arrives.

If this gate is unattainable, revise the product claim or use QP delta and codec settings supported by a different defensible path.

## 20. WebRTC and signaling

Use libdatachannel for RTP packetization, ICE, DTLS, SRTP, and browser-compatible WebRTC session management.

The transport must:

- Parse the browser offer and negotiate an offered `H264/90000` payload with `packetization-mode=1` only when it passes the frozen `recording_profile_v1` compatibility predicate.
- Reject a profile, level, or packetization mismatch before media starts.
- Parse three-byte and four-byte Annex B start codes, retain access-unit boundaries, and reject a malformed or empty NAL unit.
- Send a NAL unit of at most 1,200 RTP payload bytes as a single-NAL packet.
- Send a larger NAL unit as ordered FU-A fragments with the original forbidden bit and NAL reference indicator, correct start and end bits, and no fragment larger than 1,200 payload bytes.
- Send SPS and PPS as individual single-NAL packets immediately before every IDR and do not generate STAP-A in V1.
- Set the RTP marker bit only on the final packet of each access unit.
- Send SPS and PPS with every IDR and verify that their profile and level match the negotiated SDP.
- Preserve a strictly increasing 64-bit extended RTP timestamp internally and send its low 32 bits on the wire.
- Generate 90 kHz RTP timestamps from sender monotonic dequeue time and test 32-bit wrap explicitly.
- Select exactly one RTP sequence allocator in the pinned transport, mirror every packet into a strictly increasing 64-bit extended sequence, and send its low 16 bits on the wire.
- Keep the library's SRTP rollover counter consistent with that same wire sequence and prove the mapping through packet capture around sequence 65,535.
- Resolve a Generic NACK PID and BLP only against unique active-epoch cached extended sequences, including across 16-bit wrap, and treat an absent or ambiguous match as a recovery-IDR request rather than guessing.
- Translate PLI feedback into an IDR request with SPS and PPS.
- Coalesce and rate-limit PLI and NACK-miss recovery through the single dependency-epoch recovery limiter.
- Treat FIR as optional unless the pinned libdatachannel release exposes and the browser spike verifies it.
- Use Generic NACK to retransmit the exact cached original RTP packet with its original payload type, SSRC, sequence number, timestamp, marker bit, and SRTP treatment.
- Do not negotiate an `rtx` payload, `apt`, RTX SSRC, or RFC 4588 packet format in V1.
- Ignore a NACK from an earlier dependency epoch.
- Request a fresh IDR with SPS and PPS when an active-epoch NACK names an absent or expired packet.
- Delegate repeated-index SRTP handling to exactly one verified path in the pinned libdatachannel build, and never pass cached plaintext through an unrelated second SRTP-protect path.
- Require a Milestone 0 packet-capture fixture to prove that a NACK retransmission emits a byte-identical protected UDP payload for the cached RTP packet and is accepted by both browsers.
- If the pinned built-in NACK responder cannot enforce the declared age, byte, packet, retransmission-count, feedback-rate, dependency-epoch, and explicit-clearing bounds, patch or replace that handler in a separately published MPL-2.0 source file before continuing beyond Milestone 0.
- If the pinned stack cannot expose the declared final datagram classifier and egress gate through public APIs, patch the exact transport source files under their applicable license and prove the patch before continuing beyond Milestone 0.
- Emit RTCP sender reports needed for timestamp correlation.
- Expose REMB when the browser and library negotiate it, plus packet loss, round-trip time, decode, and connection statistics.
- Use REMB as the V1 receiver-rate signal when available and a documented loss, RTT, and queue fallback when it is absent.
- Treat TWCC as unsupported unless the pinned libdatachannel build and browser spike prove a complete feedback implementation.
- Use one controller as the single rate authority for NVENC target rate, emphasis, frame rate, resolution, and RTP pacing.
- Bound packet, pacer, and retransmission queues by bytes, packets, and age.
- Bound RTCP feedback processing, retransmissions per packet, and feedback-triggered IDR frequency.
- Handle ICE restart or fail visibly.
- Close all tracks and credentials on sender stop.

The signaling service stores only ephemeral session state.
The supported remote workflow requires the sender owner to configure the shipped bundle on one publicly reachable HTTPS origin with a valid certificate before a link can be created.
The deployment guide covers one single-tenant container, bounded in-memory state, health checks that reveal no session identifiers, certificate termination, and sender and receiver WSS routing.
Automated tests use a loopback certificate authority and an isolated network namespace, while any real public deployment or portfolio demo waits for explicit authorization.
The project does not promise NAT traversal on paths where direct UDP ICE fails unless a tested TURN deployment is configured.

Session requirements include:

- At least 128 bits of unguessable entropy in each independently generated owner and join capability.
- An owner capability that is returned only over the creating sender's TLS-protected WSS connection, is accepted only on that connection generation, is stored only as a keyed hash by the signaling service, remains memory-only in the sender, and expires on owner-signaling loss, session revocation, or session expiry.
- A distinct join capability that expires after ten minutes, is stored only as a domain-separated keyed hash, is carried only in the receiver URL fragment, and is consumed by the first successful authenticated receiver reservation.
- Capability verification binds the session identifier, role, protocol version, and permitted transition so neither capability can authorize a message from the other role.
- The signaling server rejects a receiver attempt to answer, revoke, restart ICE, replace the sender, or create a second receiver, and rejects an owner attempt to consume or replay a receiver join capability.
- Session creation, owner authentication, join reservation, connected state, receiver ICE restart, owner-signaling loss, stop, expiry, and revocation have one versioned server-side transition table with idempotent disconnect cleanup and stale-connection-generation rejection.
- One receiver in V1.
- No browser-refresh token reuse or automatic second peer connection in V1.
- No owner WSS reconnection or same-session owner-capability rebinding in V1.
- Sequenced authenticated owner heartbeat and acknowledgment messages every two seconds with a fixed five-second peer-activity timeout on both endpoints.
- ICE restart is permitted only on the existing authenticated signaling connection.
- After the authenticated receiver signaling connection closes, the sender must explicitly create a new single-use join link.
- Immediate revocation when sharing stops.
- Origin and rate controls.
- No media payload logging.
- Short-lived TURN credentials when TURN is enabled.
- HTTPS and WSS for every non-loopback deployment.
- A bearer secret carried in the URL fragment so it is not sent in the initial HTTP request, access log, or referrer.
- Receiver JavaScript reads the fragment once, exchanges it over WSS, and immediately removes it with `history.replaceState` before joining media.
- `Referrer-Policy: no-referrer`, a restrictive content security policy, no third-party receiver assets, and no analytics.
- Exact Origin and Host validation on signaling and the local dashboard.

The receiver is a static browser application plus the minimal JavaScript needed to negotiate and display video.
It does not require a browser extension.
The Milestone 0 browser spike is authoritative for the exact Chromium, Firefox, NVENC, libdatachannel, profile, level, packetization, SPS/PPS, PLI, and timestamp combination.

The peer connection creates one ordered and reliable data channel named `glyphrelay-control-v1`.
This channel carries only versioned `HELLO`, `CLOCK_REQUEST`, `CLOCK_RESPONSE`, `RECEIVER_STATS`, `SESSION_PAUSED`, `SESSION_PAUSED_ACK`, `SESSION_RESUMED`, `SESSION_RESUMED_ACK`, `SESSION_ENDED`, `SESSION_ENDED_ACK`, and `PROTOCOL_ERROR` messages.
Each message contains a protocol version, session identifier, monotonically increasing message sequence, type, and type-specific bounded payload.
The receiver sends one `RECEIVER_STATS` message per second containing cumulative decoded-frame, dropped-frame, and `requestVideoFrameCallback` compositor counters plus its latest presented RTP timestamp.
The four-timestamp clock exchange uses `CLOCK_REQUEST` and `CLOCK_RESPONSE` no more often than once every five seconds after the initial five-sample burst.
Messages larger than 4 KiB, more than ten receiver-originated messages per second, invalid numbers, unknown required fields, stale sequences, a wrong session identifier, or a wrong protocol version close the control channel and fail the session visibly.
The sender never accepts keyboard, mouse, clipboard, file, command, or arbitrary application messages on this channel.
Receiver telemetry is treated as untrusted diagnostic input, range-checked, and never overrides local safety limits or the user wire cap.
Every control-channel byte is included in SCTP and wire-egress accounting.
`SESSION_PAUSED` contains the closed media epoch and causes the receiver to clear and detach the video element before returning `SESSION_PAUSED_ACK` for that epoch.
The sender changes its local media state and closes egress before sending `SESSION_PAUSED`, and it does not treat a missing acknowledgment as permission to send media.
Resume creates new media and dependency epochs, sends `SESSION_RESUMED` with those epochs, and keeps capture-to-transport admission closed until the receiver acknowledges them.
The receiver reattaches the existing authenticated track after `SESSION_RESUMED`, acknowledges the new epochs, and displays nothing until the matching recovery IDR is decoded.
After the acknowledgment, the sender requests an IDR with SPS and PPS and opens transport admission only for that recovery access unit and its successors.
Duplicate pause or resume messages for the current epoch are idempotent, a stale or future epoch is a protocol error, and data-channel closure while paused or resuming ends the peer rather than inferring a transition.
If a pause or resume acknowledgment does not arrive within two seconds, the sender ends the peer, returns an otherwise live owner session to `OWNER_ONLY`, and requires a new single-use join link.

## 21. CPU fallback

The CPU path must still capture, encode uniformly, share, record, and expose connection state.
It uses the same capture abstraction, frame timestamps, signaling path, WebRTC receiver, and queue policy.

The mandatory tested CPU adapter dynamically links a system-provided OpenH264 package on the documented Ubuntu release.
The repository does not redistribute an OpenH264 binary.
x264 and FFmpeg adapters are optional extensions and do not count toward the clean-checkout CPU acceptance gate.
Binary patent, package, and linking terms require a recorded review before packaging.

The CPU path is:

- A functional fallback.
- A correctness reference for selected preprocessing.
- A resource and quality baseline.

It is not required to reproduce automatic NVENC emphasis behavior.

## 22. Product interface

The local sender interface contains:

- Hardware and capture readiness.
- Window selection.
- Bandwidth profile.
- Automatic protected-region preview.
- Pin and exclude rectangle tools.
- Cursor-emphasis toggle.
- Start, pause, resume, and stop.
- Share-link copy and expiry.
- Recording toggle.
- Elementary-stream payload bitrate and wire-egress bitrate with an explicit cap label.
- Queue delay.
- Dropped frames by stage.
- Protected fraction.
- Connection state.
- Clear fallback and degraded-mode label.

The receiver contains:

- Join state.
- Video element.
- Connection quality.
- Fullscreen control.
- Session-ended state.
- No remote-control UI.

The sender must display a persistent operating-system and application capture indicator.

## 23. Privacy and security

Required privacy behavior includes:

- Select one window by default.
- Never default to the full desktop.
- Keep all saliency computation local.
- Never send source frames to the signaling service.
- Never log frames, thumbnails, glyph strings, window titles, filenames, clipboard contents, or OCR output.
- Bind the local dashboard to loopback only and require a per-launch random nonce plus strict Host and Origin checks.
- Stop and revoke the remote session on screen lock through the tested desktop or logind hook.
- Refuse to start a remotely shareable session on the tested release environment if screen-lock and permission-revocation hooks are unavailable.
- Stop immediately on permission revocation.
- Expire and revoke share links.
- Use DTLS-SRTP through WebRTC.
- Use short-lived TURN credentials.
- Store recordings only when the user opts in and chooses a path.
- Do not create a remote-control or input-event channel, and reject keyboard, mouse, clipboard, file, command, and arbitrary application messages on the bounded control channel.
- Disable or sanitize core dumps by default for the sender process and document the debugging opt-in.
- Never put a bearer secret in an HTTP query string, path, cookie, access log, analytics event, or referrer.

The threat model covers:

- Link guessing.
- Signaling replay.
- Sender impersonation and owner or receiver capability role confusion.
- Signaling session fixation and invalid state transitions.
- Owner signaling loss, asymmetric partition, stale connection reuse, and attempted same-session owner reconnection.
- Unauthorized second receiver.
- TURN credential leakage.
- Local dashboard exposure.
- Accidental full-desktop capture.
- Window switch or closure.
- Stale frame leakage after stop.
- Crash dumps and logs.
- Malformed network and signaling messages.
- Clipboard and browser-history exposure of share links.
- DNS rebinding, cross-site requests, and hostile origins against the local dashboard.

Stop, revoke, lock, and permission-loss follow the same privacy boundary:

1. Close new media admission, acquire the `MediaEgressGate` exclusively, transition the session to `REVOKED`, increment its monotonic media epoch, and close media egress for every older epoch at the gate's linearization point after all earlier final UDP socket-send calls have returned.
2. Purge encoded, packet, pacer, and retransmission queues without waiting for CUDA or NVENC completion.
3. Send an authenticated session-ended control event when the control path is usable, remove the media track immediately, and close the peer connection, TURN credentials, owner capability, receiver role binding, and signaling state without waiting for media-worker drain.
4. Cancel not-yet-submitted work and drain only GPU or NVENC work that cannot be cancelled safely into discard-only sinks.
5. Reject every late completion whose media epoch is no longer the active running epoch before it can enter an encoded queue, recorder branch created after the boundary, RTP packetizer, pacer, retransmission cache, or socket send.
6. Clear the receiver video element, detach its media stream, and replace it with the session-ended state on either the authenticated event or media-track end.

On the named local and 50-millisecond-RTT test profiles, the receiver must clear its last displayed frame within 250 milliseconds of receiving the authenticated stop event or connection close.
No RTP or SRTP media datagram may cross the final UDP socket-send boundary after the local revocation boundary, regardless of when its source frame was captured.
Control traffic needed to end the session may cross that boundary through its separate bounded control gate.
Screen lock always uses this revocation path and destroys the owner capability, consumed join record, receiver role binding, signaling state, peer connection, and TURN credentials rather than using the reversible user-pause media state.
User pause closes new media admission, takes the same egress gate exclusively, changes the connected peer's media state from `RUNNING` to `PAUSED`, increments the media epoch at the gate's linearization point, purges queued transport work, and sends the authenticated pause protocol without revoking the owner capability, changing the `CONNECTED` signaling role state, or closing the control channel.
Late pause-epoch completions are discarded before packetization or recording unless the recording branch had durably accepted that exact pre-pause access unit before the boundary.
An active recording remains open during pause, records no new access unit after its accepted pre-pause tail, and records a timestamp discontinuity plus the pause and resume media epochs in its journal.
Resume changes only `PAUSED` to `RUNNING` through the acknowledged control protocol, creates new media and dependency epochs, and begins transport and any continuing recording with an IDR carrying SPS and PPS.

## 24. Deterministic benchmark corpus

Build a redistributable generated screen-content corpus with known source text and protected-region truth.
The initial benchmark protocol is `corpus_protocol_v1`.

Scene families include:

- Light and dark code editors.
- Small monospace terminals.
- Typing, scrolling, selection, and cursor movement.
- Spreadsheets and dense tables.
- Slides and diagrams.
- Browser forms and documentation.
- Mixed content with an embedded moving-video pane.
- Font sizes from approximately 8 to 24 pixels.
- High-contrast and low-contrast themes.
- Static, slowly changing, and rapidly changing content.
- High-DPI scaling cases.

The generator records:

- Source HTML or scene description.
- Exact text.
- Per-glyph visible alpha masks or deterministic raster paths plus glyph and text-region bounding boxes.
- Typed non-glyph UI primitive annotations.
- Frame timestamps.
- Theme and font.
- Motion category.
- Scene seed.
- Rendering environment hash.

Use openly licensed fonts pinned by URL and hash.
Pin the browser or renderer used for corpus generation.
Do not use private desktop recordings in public evaluation.

The corpus is split before saliency tuning:

- The development split contains the scene families, fonts, themes, layouts, and seeds available for implementation and threshold tuning.
- The frozen validation split contains disjoint fonts, themes, layouts, and seeds and is used only for the Milestone 4 map gate and failure analysis.
- The sealed final-test generator pool contains fonts, themes, layouts, and at least one renderer or scene family disjoint from both development and validation, but its concrete scene seeds do not exist before the Milestone 8 claim test.

The development split contains at least 64 independent eight-second 1080p30 sequences.
The validation split contains at least 64 independent eight-second 1080p30 sequences.
The final-test generation commitment requires at least 64 independent eight-second 1080p30 sequences.
The development and validation splits and the final-test generation commitment each require at least eight independent sequences from each of code editor, terminal, spreadsheet or table, slide or diagram, browser form or documentation, mixed video and text, and animated typing or scrolling strata.
Every primary sequence contains at least 50 visible ground-truth characters, and each concrete split must contain at least 20,000 ground-truth visible character instances plus at least 5,000 instances in the defined 8-to-10-pixel rendered-glyph-height subset across its sampled evaluation frames.
Sequences, rather than frames from the same sequence, are assigned to splits.
The development split may be expanded after a pilot.
The validation sequence count and committed final-test sequence count may each be increased once from development-only variance estimates before any saliency parameter search begins, after which the validation manifest, final-test generation commitment, and counts are immutable for `corpus_protocol_v1`.
The development and validation manifests, final-test generator-pool hash, final-test seed-derivation rule, generator hash, metric formulas, OCR evaluator, sampling rule, label ontology, stratum weights, `uniform_aq_v1` grid, and AQ selector are committed before automatic-saliency implementation or tuning begins.
The validation renderer output is not opened by developers or accepted by tuning commands before the one-shot validation command in Milestone 4.
The development selector commits the selected automatic-map configuration hash before the validation command can open its renderer output.
The first validation run verifies the frozen manifest and selected configuration, emits an immutable execution record, and permanently closes automatic-map selection for `corpus_protocol_v1`.
Later validation runs require an explicit reproduction mode, preserve the original selection result, and may not mutate the protocol or replace the original artifact.
The final-test scene seeds, renderer output, metrics, thumbnails, and aggregate results do not exist before the one-shot Milestone 8 command.
After every protocol and analysis hash is frozen, the final-test seed is `SHA256(protocol_manifest_hash || randOut)` using the first verified NIST Randomness Beacon 2.0 pulse strictly after the frozen-manifest commit time plus 24 hours.
The final-test runner verifies the beacon certificate, signature, hash chain, `randOut`, and pulse time, derives every stratum seed deterministically from that root seed, generates the concrete manifest, renders lossless references, runs every condition, and writes results without an interactive pause or selective rerun.
If that exact pulse cannot be verified, the final run waits rather than selecting another pulse.
If the concrete final-test corpus violates a committed stratum, character-count, rendering, or truth invariant, the run records a protocol failure and cannot choose another pulse or seed under `corpus_protocol_v1`.
The first final-test command records the pulse, concrete manifest, every frozen hash, lossless checks, compressed results, and an immutable execution record.
Later final-test runs require an explicit reproduction mode with the recorded pulse, preserve the original result, and may not mutate the protocol or replace the original artifact.
Changing a validation asset, final-test generator pool, seed rule, generator, renderer, truth rule, comparator, operating point, or metric creates a new protocol version with new commitments and invalidates earlier gate results.
Every source frame and truth box passes a lossless render validation before compression evaluation.
The OCR evaluator's error floor is measured on lossless validation frames in Milestone 4 and on lossless final-test frames inside the sealed Milestone 8 command and is reported beside compressed results.
The frozen development evaluator must achieve an equal-stratum bounded character error rate no greater than 0.02 overall and no greater than 0.05 on the 8-to-10-pixel rendered-glyph-height subset before the validation renderer can be opened.
The same two limits are hard measurement-validity gates on lossless validation and final-test frames.
Failure of a lossless gate produces `INSUFFICIENT_EVIDENCE`, forbids a readability-improvement claim, and requires a new corpus or evaluator protocol version rather than compressed-result tuning.

The label ontology is part of `corpus_protocol_v1`.
A visible glyph pixel belongs to `G` when its post-clipping alpha is nonzero after the frozen renderer and coordinate transform.
A non-glyph UI pixel belongs to `U` only when it is part of an enumerated foreground border, icon, caret, selection outline, diagram stroke, or control affordance needed by a declared task.
Background fills, whitespace, broad panels, photographs, embedded-video pixels, and text-container rectangles do not belong to `U`.
Glyph pixels take precedence over UI pixels, so `G` and `U` are disjoint after rasterization.
A macroblock intersects `G` or `U` when at least one visible pixel in that macroblock belongs to the corresponding rasterized mask.
Clipped, occluded, and off-crop source pixels never contribute truth.
Rendered glyph height is the integer height of the tight nonzero-alpha glyph mask after renderer scale, clipping, crop, and conversion into source-visible pixel coordinates but before encoder downscale, even-dimension padding, chroma subsampling, or macroblock rounding.
The named 8-to-10-pixel subset contains only glyph instances whose rendered glyph height is 8, 9, or 10 source-visible pixels by that definition.
Punctuation and partially clipped glyphs participate only when their own tight visible mask meets the same height rule, and every subset assignment is stored in the immutable corpus manifest.

External screen-content datasets require a verified redistribution license before inclusion.

## 25. Evaluation conditions

Required paired conditions are:

1. Controlled uniform NVENC H.264 with AQ disabled and every non-map field identical to the emphasis conditions.
2. Best-supported uniform NVENC H.264 with one deterministic AQ configuration selected under the frozen development-only contract below and every other field held constant.
3. Deterministic oracle-pinned ROI with NVENC as a non-user quality ceiling.
4. Automatic GlyphRelay emphasis map with NVENC.
5. Uniform CPU H.264 as a functional and resource baseline.
6. Optional high-quality offline software encode as a quality ceiling.

For the controlled uniform, oracle-pinned, and automatic NVENC conditions, hold constant:

- Codec.
- Profile and level.
- Preset and tuning.
- GOP.
- Resolution.
- Frame rate.
- Rate-control mode.
- Requested bitrate.
- VBV configuration.
- Spatial and temporal AQ disabled state.
- Multipass state.
- Reference structure and entropy coding.
- Filler-data and repeat-parameter-set behavior.
- Source frames.
- Run order randomization protocol.

Serialize and hash every effective initialization and reconfiguration field after preset expansion for every run.
The best-supported uniform baseline may vary only the frozen AQ choice selected on the development split.
No validation or final-test result may change that choice.

The `uniform_aq_v1` development grid is the Cartesian product of `enableAQ` in `{false, true}`, `aqStrength` in `{1, 4, 8, 12, 15}` when `enableAQ` is true and canonical zero otherwise, and `enableTemporalAQ` in `{false, true}`, excluding the both-disabled controlled-uniform configuration.
Every candidate keeps B frames, lookahead, hierarchical prediction, profile, level, rate control, multipass, preset, tuning, GOP, resolution, frame rate, and every other effective encoder field at the controlled-uniform value.
An unsupported combination or a combination that fails independent decode, browser decode, the frozen latency margin, or the frozen submitted-frame and resolution contract is recorded as invalid and cannot be selected.
Development-only target search adjusts only the requested payload rate for each AQ candidate until its mean measured payload lies within 2 percent of each target in `0.5, 0.75, 1, 2, 4` Mbps or records that target as unmatchable.
A selectable AQ candidate must rate-match all five targets and pass the same systems admissibility margins used by the controlled uniform condition.
For every selectable candidate, apply the frozen pool-adjacent-violators and interpolation code to calculate equal-stratum fitted character error at exactly `0.5, 0.75, 1, 2, 4` Mbps.
The selector minimizes the unweighted mean of those five fitted character-error values, then breaks ties by lower fitted character error at 1 Mbps, lower fitted bitrate at 10 percent character error when estimable, lower P95 preprocessing-plus-encode latency, lower mean sender CPU use, and lexicographic serialization of the effective AQ fields.
An unestimable 10 percent crossing loses that tie break to an estimable crossing and otherwise advances to the next tie break.
The one selected AQ configuration is the only `best_supported_uniform` condition for both co-primary endpoints.
The protocol commits the complete grid, target-search records, invalid reasons, effective encoder-field hashes, all development results, selector code hash, and winning configuration before validation access.

The oracle-pinned condition expands each frozen visible glyph bounding box by two source pixels, clips it to the visible crop, transforms it through the frozen geometry path, and protects every intersecting macroblock at level four.
The oracle condition uses no developer-drawn rectangles, receives no validation or final-test feedback, and is never described as evidence of ordinary manual-user performance.
Interactive pin and correction usability is reported separately as product behavior rather than as a primary quantitative comparator.

Compare conditions using measured bitrate.
If automatic emphasis overshoots, either rate-match through controlled settings or report a bitrate-quality curve.
Two conditions count as directly rate-matched only when mean elementary-stream payload bitrate differs by at most 2 percent over the same source sequences.
Every primary condition records at least five measured operating points that bracket 1 Mbps and, when feasible, 10 percent character error on the development split.
The fifth curve-support point is 0.75 Mbps, while 0.5, 1, 2, and 4 Mbps remain the named primary matrix.
For each condition, pooled curve analysis applies weighted pool-adjacent-violators regression to make corpus character error nonincreasing in log measured payload bitrate and then linearly interpolates only between adjacent fitted log-bitrate points.
The character-error co-primary endpoint is the paired baseline-minus-automatic difference at exactly 1 Mbps measured elementary-stream payload bitrate.
The fixed character-error target for the bitrate-efficiency co-primary outcome is 10 percent sequence-weighted corpus character error under `corpus_protocol_v1`.
Interpolation may use only the common measured bitrate range in which both compared curves bracket that target.
If either curve does not bracket 10 percent, the bitrate-efficiency outcome is reported as not estimable and no extrapolation is allowed.
When a fitted plateau crosses the 10 percent target, the required bitrate is the lowest measured-range bitrate on that plateau.

Development-only data selects one comparator identity for each co-primary endpoint before validation is opened.
The character-error comparator is the uniform condition with lower fitted development character error at exactly 1 Mbps.
The bitrate-efficiency comparator is the uniform condition requiring less fitted development bitrate at 10 percent character error.
A numerical tie chooses controlled uniform by the fixed lexical order `controlled_uniform`, then `best_supported_uniform`.
The selected comparator identities, effective encoder configurations, curve-support settings, and interpolation code hash are committed into the protocol manifest and may not change after validation or final-test results.

Run the primary matrix at 1080p30 and 0.5, 1, 2, and 4 Mbps.
Use `tc netem` profiles for bandwidth, RTT, jitter, and packet loss.
The primary readability matrix holds resolution, submitted source frames, frame rate, and controller adaptation fixed so automatic emphasis cannot win by silently reducing motion or resolution.
The adaptive-controller matrix is separate and evaluates delivered frame rate, resolution, wire rate, freshness, recovery, and readability together.

## 26. Evaluation metrics

### 26.1 Primary user-outcome metrics

- Character error rate on known text regions using a pinned evaluation-only OCR model.
- Word or token accuracy where appropriate.
- Task-answer accuracy on predefined code, terminal, and table questions.
- Bitrate required to reach a fixed character-error target.
- Paired human readability preference from the optional predeclared study of at least 12 consenting adults.
- Glyph-truth macroblock recall.
- Protected macroblock fraction.
- False-protected macroblock fraction outside glyph and declared UI truth.
- Static-scene map-change fraction after hysteresis warmup.

The OCR model is an evaluator, not a runtime component.
The primary evaluator uses Tesseract 5 LSTM English mode with one exact binary build, `eng.traineddata` hash, OEM, page-segmentation mode, language, and preprocessing pipeline pinned in the protocol manifest.
Each declared text box is cropped from the decoded frame using the frozen coordinate transform, converted using the frozen color pipeline, and evaluated separately.
Every sampled source frame has an immutable `(sequence_id, frame_id, source_pts, geometry_epoch)` key in the corpus manifest.
The encoder sidecar maps each submitted source key to one submission sequence, output access-unit index, dependency epoch, and extended RTP timestamp.
Because primary evaluation disables B frames and output reordering, the independent decoder must produce exactly one candidate image for that mapped access-unit index and must verify its sidecar timestamp before OCR.
Browser evaluation matches compositor output only through the unambiguous extended RTP timestamp and dependency epoch established by the browser oracle.
Nearest-frame, last-presented-frame, duplicate-frame, and stale-frame substitution are forbidden for readability scoring.
If the exact expected candidate is missing, duplicated, ambiguously wrapped, or mapped to the wrong epoch, every declared text box for that sample receives character error rate one.
Truth and output are normalized to Unicode NFC, line endings are normalized, case is preserved, and no spelling correction is applied.
For a nonempty truth box, bounded character error rate is `min(Levenshtein insertions + deletions + substitutions, truth_character_count) / truth_character_count`.
The uncapped edit ratio is retained only as a secondary diagnostic.
The primary sequence character error rate is the sum of capped edit counts across its boxes divided by total truth characters in that sequence.
The primary corpus statistic first averages sequence character error rates within each declared scene stratum and then gives every stratum equal total weight.
Micro-averaged character error and every per-stratum result are reported as secondary statistics.
A decode failure or missing text-region frame receives the maximum bounded character error rate of one and is never silently excluded.
The OCR version, language, preprocessing, formulas, sampled frames, and license are locked before saliency work begins.

For map metrics, let `V` be visible macroblocks, `G` be macroblocks intersecting glyph truth, `U` be macroblocks intersecting declared non-glyph UI truth, and `P` be macroblocks assigned a nonzero emphasis level.
Glyph-truth recall is `|P intersect G| / |G|`.
Sampled frames with no glyph-truth macroblock are omitted only from recall denominators and remain included in protected, false-protected, and stability metrics.
Protected macroblock fraction is `|P| / |V|`.
False-protected macroblock fraction is `|P minus (G union U)| / |V|`.
False discovery fraction is `|P minus (G union U)| / max(|P|, 1)` and is reported even though it is not the Milestone 4 gate.
Static-scene map-change fraction is the number of visible macroblocks whose emphasis level changed divided by `|V|`.
Frame-level map counts use the frozen sampled frames after geometry clipping.
Sequence glyph recall is total protected glyph macroblocks divided by total glyph macroblocks across that sequence's sampled frames.
The 8-to-10-pixel rendered-glyph-height recall uses the same ratio after restricting `G` to glyph instances in that frozen subset.
Sequence protected and false-protected fractions are unweighted means of their sampled-frame fractions.
Sequence static-scene map change is the unweighted mean of adjacent-frame change fractions after the five-frame warmup.
Each map gate first averages sequence values within a stratum and then gives every declared stratum equal total weight.
The Milestone 4 numerical thresholds apply to those equal-stratum corpus statistics, and per-stratum values plus the P95 sequence value are always reported.

### 26.2 Secondary visual metrics

- Text-region SSIM.
- Edge-weighted similarity.
- Whole-frame SSIM.
- PSNR.
- VMAF when its screen-content limitations are stated.

Natural-video metrics remain secondary because they can underweight small text.

### 26.3 Systems metrics

- PipeWire-dequeue-to-send latency.
- Send-to-receiver-arrival latency.
- Receiver-arrival-to-render latency.
- PipeWire-dequeue-to-render latency.
- Per-stage p50, p95, and p99 duration.
- One-second bitrate p50, p95, and maximum.
- One-second wire-egress bitrate p50, p95, and maximum.
- Elementary-stream payload bitrate p50, p95, and maximum.
- Frame drops by stage and reason.
- Queue depth and frame age.
- CPU utilization.
- GPU SM utilization.
- NVENC utilization.
- VRAM use.
- Host-to-device and device-to-host bytes.
- Power or energy per streamed minute where the measurement is available and documented.

## 27. Latency measurement contract

Do not use the term glass-to-glass without a defensible measurement.

The required software method measures PipeWire-dequeue-to-render latency and does not include compositor capture or time already spent inside the portal and PipeWire before dequeue:

- Define the mandatory software start as the sender `CLOCK_MONOTONIC_RAW` timestamp taken immediately after dequeuing the PipeWire buffer.
- Stamp every captured frame with a sender monotonic sequence and time.
- At session start, choose a random 32-bit RTP base and record the sender `CLOCK_MONOTONIC_RAW` base paired with it.
- Derive each 64-bit extended 90 kHz timestamp as the base plus the rounded nonnegative dequeue-time delta in 90 kHz units, and send the low 32 bits on the wire.
- Record every wrap epoch and reject a receiver timestamp that cannot be mapped unambiguously into the active dependency epoch.
- Run an authenticated four-timestamp clock-correlation exchange over the data channel at session start and every five seconds.
- Each exchange records sender send time `S0`, receiver receive time `R1`, receiver send time `R2`, and sender receive time `S3` in their respective monotonic clocks.
- Estimate offset as `((R1 - S0) + (R2 - S3)) / 2` from the minimum-delay sample in the latest twelve valid exchanges.
- Bound uncertainty by half that sample's network delay plus the maximum offset variation among the three lowest-delay samples.
- Reset the estimator and suppress cross-clock metrics after a browser sleep, clock discontinuity, dependency-epoch ambiguity, or offset jump larger than the current bound.
- Use browser `requestVideoFrameCallback` metadata, including `rtpTimestamp`, `presentationTime`, and `expectedDisplayTime`, where exposed.
- Define the render endpoint as `expectedDisplayTime` and record whether the callback arrived before or after that point.
- Cross-check receiver `captureTime` and `receiveTime` metadata when the browser exposes them, but do not substitute them silently for the custom sender timestamp.
- Report PipeWire-dequeue-to-render together with clock uncertainty, display-refresh interval, missing-metadata rate, and browser version.
- Refuse cross-host PipeWire-dequeue-to-render claims when RTP correlation, clock correlation, or uncertainty bounds are unavailable.
- Evaluate the 10-millisecond automatic-versus-uniform latency noninferiority gate only in the same-host network-namespace setup validated before `tc netem` is enabled or with the optional physical method.
- Require the paired one-sided 95 percent upper confidence bound for added P95 latency plus the propagated worst-case clock and display uncertainty to be at most 10 milliseconds.
- Require propagated uncertainty alone to be at most 2 milliseconds for that noninferiority claim.
- Report the cross-host 50-millisecond-RTT results with their uncertainty but do not use them for the 10-millisecond claim when that admissibility rule fails.

An optional high-speed-camera test may measure glass-to-glass latency using a generated source-display event and the physical receiver display.
An optional producer-timestamp metric may be called capture-to-render only when the PipeWire timestamp's capture-event provenance is verified, its source clock is correlated to `CLOCK_MONOTONIC_RAW`, and the correlation error is included in the uncertainty bound.
Without one of those separately validated start events, call the mandatory metric PipeWire-dequeue-to-render and make no capture-to-render or glass-to-glass claim.
The software estimate must first agree with a same-host controlled validation within one display refresh interval plus the reported clock uncertainty.

## 28. Experimental protocol

- Warm the GPU and encoder before recording samples.
- Lock or record power and clock behavior where permitted.
- Record GPU model, driver, SDK, CUDA, OS, browser, and build commit.
- Randomize paired condition order.
- Run at least ten repetitions per primary condition.
- Use identical source frames and network profiles.
- Retain raw machine-readable frame and run metrics.
- Average repeated runs within each sequence and condition before calculating the primary paired sequence effect while retaining repetition-level systems metrics.
- Use 10,000 paired bootstrap resamples with scene sequence as the resampling unit, stratify resampling by the frozen scene stratum, and record the random seed.
- Give every scene stratum equal total weight in every primary bootstrap statistic.
- Each bootstrap draw samples the original number of sequences with replacement inside each stratum, keeps every condition for a selected sequence paired, and uses the percentile interval from the 1.25th through 98.75th percentiles of the 10,000 effects.
- At each operating point, calculate the equal-stratum corpus character error and payload bitrate first, and give each resulting operating point unit weight in pool-adjacent-violators regression.
- Treat character-error reduction and bitrate reduction at the fixed character-error target as two co-primary outcomes.
- Use two-sided 97.5 percent intervals for each co-primary outcome so success on either outcome controls the familywise error rate at 5 percent by Bonferroni adjustment.
- Refit the isotonic curve, perform interpolation, and calculate the effect inside every bootstrap resample while keeping each development-selected comparator identity fixed.
- Report ordinary 95 percent intervals as descriptive plots only and never use them for the pass decision.
- Report exclusions and failed runs.
- Exclude a paired sequence only for a predeclared source-generation or infrastructure invariant failure that invalidates every compared condition, and exclude the entire pair rather than one condition.
- Treat encode, decode, timeout, or missing-frame failures attributable to a condition as worst-case user outcomes rather than exclusions.
- Permit no more than 5 percent of committed final-test sequence pairs to be excluded for all-condition infrastructure failure.
- Retain at least seven valid sequence pairs and at least 90 percent of the committed sequence count rounded down in every declared scene stratum.
- Retain at least nine valid all-condition paired repetitions for every retained sequence and permit no more than 5 percent of all committed sequence-repetition pairs to be excluded for all-condition infrastructure failure.
- Count a failure isolated to one condition as a retained repetition with that condition's predeclared worst-case outcome.
- Produce `INSUFFICIENT_EVIDENCE` and forbid a pass decision when any total, stratum, or repetition floor is missed.
- Do not selectively rerun an excluded final-test sequence or repetition under the same protocol version after results exist.
- Separate functional failure from poor quality.
- Do not replace expected outputs after viewing results without versioning the protocol.
- Commit the development-selected comparator identities, corpus hashes, metric code hash, numerical gates, noninferiority margins, exclusion rules, and analysis configuration before the frozen final-test run.

The automatic condition is compared with the development-selected uniform comparator frozen for each co-primary endpoint.
The predeclared final practical-effect gate is satisfied when at least one of these paired final-test effects reaches its margin and the lower bound of its multiplicity-adjusted 97.5 percent confidence interval also reaches that same margin:

- At exactly 1 Mbps measured elementary-stream payload bitrate, baseline character error minus automatic character error is at least 0.05 and its adjusted lower confidence bound is at least 0.05.
- At the fixed 10 percent character-error target, `(baseline_payload_bitrate - automatic_payload_bitrate) / baseline_payload_bitrate` is at least 0.15 and its adjusted lower confidence bound is at least 0.15.

The 0.5, 2, and 4 Mbps character-error comparisons are secondary and cannot independently trigger the pass decision.
An endpoint that is not bracketed or otherwise not estimable cannot pass and does not transfer its unused error budget to the other endpoint.

For a directly measured passing endpoint, the one-sided 95 percent upper confidence bound for added P95 PipeWire-dequeue-to-render latency plus propagated timing uncertainty may be no more than 10 milliseconds, decoded-frame loss may increase by no more than two percentage points, and wire rate may not violate the stable-link gate at that operating point.
For an interpolated passing endpoint, both raw operating points adjacent to the interpolation interval in both compared conditions must independently pass those systems margins, and systems margins are never interpolated.

The optional human study uses at least 12 consenting adult participants, randomized blinded pairs, no collected identifiers beyond an anonymous study code, a documented withdrawal path, and a fixed deletion date for raw responses.

The main success claim is:

> Automatic emphasis beats its development-selected uniform NVENC comparator with an adjusted lower confidence bound of at least five absolute character-error points at exactly 1 Mbps or 15 percent payload-bitrate reduction at the fixed character-error target while staying inside the declared uncertainty-adjusted 10-millisecond latency, two-point decoded-loss, and wire-rate margins.

## 29. Profiling protocol

### Nsight Systems

Use NVTX ranges for:

- Capture acquisition.
- Host staging.
- GPU upload or import.
- Colorspace conversion.
- Saliency kernels.
- Map generation.
- Encoder submission.
- Bitstream acquisition.
- RTP packetization.
- Network send.

Nsight Systems must answer:

- Whether copies overlap with compute and NVENC.
- Whether capture or encode workers block the pipeline.
- Whether queue buildup precedes drops.
- Whether hidden synchronization exists.
- Whether DMA-BUF removes an actual full-frame copy.

### Nsight Compute

Profile only the custom kernels that materially affect end-to-end latency.
Inspect:

- Kernel duration.
- Memory throughput.
- Global load and store efficiency.
- Occupancy.
- Warp divergence.
- Launch overhead.
- Achieved versus relevant roofline limits.

Do not optimize a kernel solely because one utilization percentage appears low.
Tie every kernel change to the end-to-end bottleneck and rerun the product benchmark.

## 30. Testing strategy

### 30.1 CPU and CUDA correctness

- Primary-context identity, per-thread context guards, foreign-context rejection, worker-thread migration, and shutdown ordering.
- BT.709 limited-range goldens.
- BT.709 full-range goldens if supported.
- BGRA and RGBA channel order.
- Odd visible crop and padded pitch.
- Width and height boundary cases.
- Randomized CPU-versus-CUDA differential tests.
- Compute-sanitizer runs.
- Deterministic saliency fixtures.
- Hand-calculated Scharr, percentile, small-structure, edge-pair, hysteresis, dropped-frame, and reset feature vectors.
- Morphology boundary behavior.
- Macroblock map shape and raster order.
- Exact `qpDeltaMapSize` and coded-dimension padding.
- Device-map to pinned-host-map copy and event ordering.
- Wrong address-space, frame-ID, geometry-epoch, and byte-size rejection before NVENC.
- Packed-RGB device-source and NV12 encoder-surface address ranges never overlap.
- A source-read completion event releases a DMA-BUF without releasing or reusing the corresponding NV12 encoder surface.

### 30.2 NVENC tests

- Unsupported GPU capability.
- API version mismatch.
- Uniform map.
- Fixed center emphasis.
- Maximum and minimum allowed emphasis.
- Map lifetime under multiple in-flight frames.
- Contiguous pitched NV12 registration and chroma-offset correctness.
- `NV_ENC_ERR_NEED_MORE_INPUT` submission-order handling.
- `NV_ENC_ERR_ENCODER_BUSY` bounded retry.
- Multiple delayed submissions preserve the exact input, map, submission, and output-buffer association through FIFO acquisition and EOS.
- Bitrate overshoot.
- Keyframe request.
- Reconfiguration.
- Resolution change or safe restart.
- End-of-stream flush.
- Error injection and teardown.
- Independent H.264 decode.
- SPS profile, level, color VUI, and negotiated SDP consistency.
- SPS and PPS presence at startup, IDR, new receiver admission, and dependency-epoch recovery.

### 30.3 Capture tests

- User cancels portal selection.
- CLI recording opens the portal dialog, rejects any attempt at caller-selected window identity, and treats `--window-label` only as an ephemeral local label after successful selection.
- Shared-memory capture.
- DMA-BUF negotiation failure and fallback.
- Window resize.
- Window close.
- Permission revocation.
- PipeWire disconnect.
- Desktop lock.
- Hidden, embedded, and metadata cursor-mode handling.
- Prompt SHM requeue and capture-pool starvation.
- DMA-BUF completion-fence and PipeWire-loop requeue.
- Session shutdown with frames in flight.

### 30.4 Transport tests

- Chromium interoperability.
- Firefox interoperability.
- Strictly increasing extended RTP timestamps and correct modulo-32-bit wire values.
- RTP timestamp wrap and browser-frame correlation.
- Strictly increasing extended RTP packet sequences and correct modulo-16-bit wire values.
- RTP sequence wrap with loss immediately before, at, and after 65,535, including unambiguous Generic NACK resolution, SRTP rollover, retransmission, stale-feedback rejection, and browser recovery.
- Single-NAL and FU-A packetization, marker bits, 1,200-byte payload limit, and rejection of malformed Annex B input.
- SDP answers omit RTX and every Generic NACK retransmission preserves the original RTP identity.
- Packet loss and reordering.
- ICE restart on the same authenticated signaling connection and explicit-new-link behavior after connection closure.
- Immediate receiver-signaling-close cleanup, five-second receiver ICE-disconnected recovery, immediate ICE-failed cleanup, 30-second reservation timeout, 15-minute unjoined expiry, and nonextendable eight-hour absolute expiry under running and paused media states.
- Owner WSS close, transport error, protocol failure, missed heartbeat acknowledgment, one-way partition, stale connection generation, and attempted same-session owner reconnection all follow the fail-closed revocation contract.
- TURN relay path.
- Expired or reused share token.
- Unauthorized second receiver.
- Sender stop and immediate revocation.
- PLI to IDR plus SPS and PPS recovery.
- Encoded-queue dependency-epoch reset without corrupted presentation.
- Pacer access-unit atomic admission, packet-age expiry, partial-access-unit purge, and hard-byte-cap recovery through a new IDR.
- Retransmission-cache packet, byte, and age limits.
- NACK identifier rate, retransmissions per packet, feedback-triggered IDR coalescing, and sustained-feedback-flood termination.
- Byte-identical protected-packet retransmission without a second unrelated SRTP-protect operation.
- Rootless datagram egress accounting for direct IPv4, direct IPv6, and TURN over UDP cross-checked against packet-capture IP lengths.
- Control-channel schema, size, rate, sequence, session, replay, malformed-value, and unknown-message enforcement.
- Ordered idempotent pause and resume messages, acknowledgments, stale and future media-epoch rejection, acknowledgment loss, data-channel closure during transition, receiver clearing, and IDR-only resumed display.
- Receiver-stat freshness and range validation.

Browser recovery uses the versioned `browser_oracle_v1` deterministic sequence rather than browser console silence.
Every oracle frame contains a large high-contrast epoch identifier, frame identifier, and independently changing color regions.
The independent decoder produces a reference image keyed by RTP timestamp for every access unit that should be presented.
Playwright captures the receiver canvas on `requestVideoFrameCallback`, matches it by RTP timestamp, and compares it with the independent reference using a tolerance frozen from ten zero-loss runs before loss testing.
Loss, PLI, dependency-epoch purge, new receiver admission, and IDR tests fail on a mismatched epoch, a frame outside the frozen pixel tolerance, a stale presentation beyond the declared frame-age limit, or a freeze beyond the declared recovery bound.
Browser decoder counters and console errors are retained as secondary diagnostics only.

### 30.5 Backpressure tests

- Slow CUDA processing.
- Slow encoder acquisition.
- Slow network send.
- Bandwidth collapse.
- High RTT and jitter.
- Queue capacity enforcement.
- Latest-frame-wins behavior.
- Keyframe recovery after drops.
- No use-after-free during drop and teardown.
- No arbitrary encoded P-picture drop.
- Recovery IDR and frame-age recovery within two seconds after the declared collapse profile clears.
- Presentation-profile recovery within the frozen `2 + 2 * N` bound and eight-second absolute maximum.
- Production controller traces replay byte-for-byte without consuming future feedback events.

### 30.6 Browser and product tests

- Doctor and readiness display.
- Window selection.
- Map preview.
- Pin and exclude regions.
- Start and join.
- Pause and resume.
- Recording opt-in.
- Record-only initialization without a browser offer and mid-share recording start at an IDR carrying SPS and PPS.
- Receiver join after `share --record` has already started produces a fresh transport IDR without breaking the continuous recording dependency chain.
- Recording stop and restart create distinct recording epochs and never append to or overwrite an earlier artifact.
- Existing media, sidecar, journal, marker, temporary, symbolic-link, directory-swap, and no-replace-rename failure cases preserve every preexisting byte and fail before publication.
- Slow recorder, recorder-queue saturation, short write, full disk, permission loss, prepared-only recovery, corrupt prepared-header rejection, initial directory-barrier failure, crash-journal recovery, group-commit ordering, file and directory synchronization errors, every partial and final-name permutation, completion-marker validation, and clean committed finalization.
- Recorder failure preserves live sharing, and transport dependency-epoch purge preserves a valid recording dependency chain.
- Session expiry.
- Capture-revoked state.
- CPU fallback label.
- Complete keyboard flow.
- URL-fragment token handling with no token in requests, logs, history state, or referrers.
- Owner and join capability separation, domain-keyed hashes, role-confusion rejection, sender-impersonation rejection, session fixation rejection, receiver-forged owner action rejection, owner-connection-generation fencing, fail-closed owner-signaling loss, and state-machine cleanup.
- Receiver video clearing within the declared revocation bound.
- Forced CUDA and NVENC completions after stop, pause, screen lock, and permission revocation produce zero post-boundary RTP or SRTP media datagrams.
- A deterministic barrier between media epoch validation and the final nonblocking UDP socket-send call proves that stop, pause, screen lock, and revocation cannot linearize between the final check and the socket call.
- Pause clearing and resume from a new IDR dependency epoch.
- Local-dashboard Host, Origin, nonce, CSRF, and DNS-rebinding defenses.
- Development and tuning commands cannot read validation or final-test renderer output.
- Lossless development, validation, and final-test OCR-floor checks use the frozen bounded-CER implementation and abort compressed claim analysis when either overall or small-glyph threshold fails.
- Synthetic final-test exclusion fixtures produce `INSUFFICIENT_EVIDENCE` when total, per-stratum, or repetition floors are missed and never convert a condition-specific failure into an infrastructure exclusion.
- The validation command refuses to run without the committed automatic-map and comparator hashes.
- The final-test command refuses to run until every protocol and analysis hash is frozen and preserves the first immutable result against ordinary reruns.
- Reproduction mode verifies and retains the original validation and final-test execution records.

## 31. Failure behavior

### Unsupported emphasis map

Display uniform NVENC or CPU fallback explicitly.
Do not label it GlyphRelay enhanced mode.

### GPU lost or encoder error

Stop the media path, revoke the session, finalize or discard recording safely, and present a diagnostic identifier.
Do not continue sending stale frames.

### Capture revoked

Stop capture and media immediately.
Revoke the link and clear in-flight buffers.

### Network congestion

Reduce emphasis and protected area, then frame rate or resolution, and drop stale frames.
Never allow unbounded queue growth.
Drop before encoder submission whenever possible.
If the encoded queue reaches its hard bound, purge the current dependency epoch and resume only from an IDR carrying SPS and PPS.

### Signaling unavailable

Keep the local preview available but do not claim the session is shareable.
If an active owner's WSS connection closes, errors, fails protocol validation, or misses the fixed liveness deadline, invoke the remote-media revocation path immediately and require a new session before sharing again.
An explicitly selected local recording may continue only under its independent recorder contract after remote transport has been revoked.
Do not retry forever or silently rebind the old owner capability without a user-visible state.

### Recording unavailable

Stop the recorder at the last journaled complete access unit and preserve the partial artifact for `glyphrelay inspect`.
Continue an active share with a persistent recording-failed state, but terminate a record-only command with exit code 5.

### Receiver disappears

If authenticated receiver signaling closes or ICE becomes `failed` or `closed`, expire the peer connection and TURN credentials immediately.
If only ICE becomes `disconnected` while authenticated signaling remains open, expire them after the fixed five-second recovery grace period.
Return an unexpired live owner session to `OWNER_ONLY`, invalidate the consumed join binding, and require a new single-use join link.
Continue local recording only if the user explicitly selected it.

## 32. Milestone plan

### Milestone 0 - Capability and quality-shift kill gate

Deliverables:

- Initialize C++, CUDA, TypeScript, and Python tooling and lockfiles.
- Add formatting, static analysis, unit-test, secret-scan, and dependency-review CI.
- Implement `glyphrelay doctor` text and JSON output.
- Build a minimal NVENC H.264 encoder from deterministic synthetic frames.
- Before the first measured encode, commit the exact synthetic source generator, seeds, frame hashes, protected center mask, unprotected comparison mask, map values, metric code, and run configuration as `m0_fixed_map_v1`.
- Apply only that frozen fixed center emphasis map in the feasibility comparison.
- Decode with an independent decoder.
- Build the minimum libdatachannel sender, signaling exchange, and static browser receiver needed to carry the NVENC stream.
- Bind every Milestone 0 HTTP, WebSocket, and peer test endpoint to loopback and do not generate a remotely usable share link.
- Negotiate the exact H.264 SDP profile, level, and packetization mode from real Chromium and Firefox offers.
- Freeze `recording_profile_v1` only after both target browser offers pass its exact profile, maximum encoded level, level-asymmetry, and packetization compatibility predicate for every V1 presentation profile.
- Encode and independently decode one NVENC and one system-OpenH264 record-only `recording_profile_v1` stream without creating a browser offer.
- Exercise startup, PLI recovery, SPS and PPS repetition, RTP timestamp correlation, and session stop.
- Seed the RTP sequence immediately below 65,535 and prove sequence rollover, SRTP rollover, loss recovery, and active-epoch Generic NACK resolution in both browsers.
- Implement `browser_oracle_v1` with independent-decoder references and freeze its zero-loss pixel tolerance.
- Measure protected and unprotected quality, elementary-stream payload bitrate, wire-egress bitrate, and encode latency.
- Select and lock the exact NVENC header release and hash.
- Select and lock the exact libdatachannel release or commit and media build flags.
- Identify, implement, and lock the sole final datagram-emission hook and authenticated media or control classifier for direct IPv4, direct IPv6, and a loopback coturn TURN-over-UDP path.
- Cross-check the hook's IP-layer byte counts against packet capture and run the deterministic validation-to-final-UDP-send privacy race in the Milestone 0 transport spike.
- Select and lock the CUDA primary-context and per-thread context-guard contract.
- Prove the chosen Generic NACK path emits a byte-identical protected retransmission and supports every declared cache, rate, and recovery limit.
- Prove the pinned packetizer exposes or can be wrapped with one application-visible extended-sequence identity without creating a second allocator.
- Record SDK, header, driver, GPU, CUDA, OpenH264, libdatachannel, and license decisions.

Acceptance gate:

- The target reports H.264 and emphasis-map support.
- Across ten repeated 1080p30 one-minute synthetic runs, configure the fixed-map and AQ-disabled controlled-uniform conditions so each mean elementary-stream payload bitrate lies from 0.98 through 1.02 Mbps and the two means differ by no more than 2 percent.
- At that matched measured rate, the fixed map improves protected-region luma PSNR by at least 1.0 dB over controlled uniform and improves the protected-minus-unprotected PSNR difference by at least 0.75 dB.
- The run command rejects any source, mask, map, metric, or configuration hash that differs from the pre-measurement `m0_fixed_map_v1` manifest.
- Report the configured target needed by each condition, every measured payload point, whole-frame PSNR, and protected and unprotected region PSNR.
- The stream decodes independently.
- Chromium and Firefox each display the NVENC stream for 60 seconds, recover from a PLI through an IDR carrying SPS and PPS, remain within the frozen `browser_oracle_v1` pixel tolerance, and report no decoder error.
- The emitted SPS profile and level match the negotiated `profile-level-id` and packetization mode is 1.
- Chromium and Firefox offers both pass the frozen `recording_profile_v1` compatibility predicate, and both NVENC and system-OpenH264 record-only streams begin with SPS, PPS, and IDR without signaling.
- No Milestone 0 service accepts a non-loopback connection.
- Elementary-stream payload bitrate, wire-egress bitrate, and latency are recorded.
- After a ten-second warmup, matching-frame time from the start of `nvEncEncodePicture` to successful bitstream availability is at most 10 milliseconds at P95 and 16 milliseconds at P99 across every steady synthetic run.
- The number and age of pending submissions show no positive trend during each one-minute run, and no steady-state submission remains pending for more than one 33.34-millisecond frame interval.
- Wrong map byte size, stale frame ID, wrong geometry epoch, and unsupported map memory paths fail before encoder submission.
- Foreign CUDA context resources fail before registration or submission, and every context-using worker exits before primary-context release.
- A NACK retransmission is byte-identical at the protected UDP payload, and PLI or NACK floods cannot exceed the declared retransmission or feedback-triggered IDR limits.
- Loss and Generic NACK immediately around RTP sequence rollover resolve to the unique active-epoch extended packet, preserve the library's SRTP rollover state, and recover in both browsers.
- Every direct IPv4, direct IPv6 where the environment supports it, and loopback TURN-over-UDP datagram crosses the selected final hook exactly once with the correct media or control classification, and deterministic counter totals equal packet-capture IP lengths exactly.
- The deterministic stalled validation-to-final-UDP-send test proves the selected hook can linearize media revocation without blocking control traffic.
- Resource cleanup passes repeated and sanitizer runs.
- `make check` passes from a clean checkout.

Kill gate:

- Stop the original claim if capability is absent, the fixed-map effect gate fails, the encode-latency gate fails, pending work grows at 1080p30, either target browser cannot consume the exact NVENC-to-WebRTC path, or the pinned transport cannot expose a maintainable final datagram hook that passes both cap-accounting and revocation-race gates.

Commit expectation:

- One repository-foundation commit.
- One feasibility-spike and evidence commit.

### Milestone 1 - CPU end-to-end product path

Deliverables:

- Implement XDG portal window selection.
- Implement PipeWire shared-memory capture.
- Implement scalar and SIMD color conversion as available.
- Use the mandatory system-provided OpenH264 adapter for uniform CPU H.264.
- Implement libdatachannel sender and browser receiver.
- Implement short-lived signaling.
- Package the receiver and signaling service as the single-tenant self-hostable HTTPS and WSS bundle and document its required public-origin configuration.
- Implement single-use hashed join tokens, URL-fragment exchange and removal, exact Host and Origin validation, message size and rate limits, and one-receiver enforcement.
- Implement the separate owner and join capabilities, domain-separated keyed hashes, role-bound signaling transition table, and owner-authenticated stop, revocation, and ICE restart.
- Bind the owner capability to its creating WSS connection generation, implement the sequenced two-second owner heartbeat exchange and five-second fail-closed liveness deadline, and forbid same-session owner reconnection.
- Implement the fixed 15-minute `OWNER_ONLY`, 30-second `JOIN_RESERVED`, five-second ICE-disconnected, and eight-hour absolute timers with monotonic deadlines.
- Implement `glyphrelay-control-v1` with its bounded clock, receiver-stat, and session-ended messages.
- Require HTTPS and WSS before any non-loopback bind and refuse an insecure LAN or internet configuration.
- Add receiver CSP, `Referrer-Policy: no-referrer`, no-third-party-asset enforcement, and loopback-dashboard nonce and CSRF controls.
- Implement bounded queues, timestamps, drops, and local recording.
- Finalize `corpus_protocol_v1`, generate the development and frozen validation manifests, commit the final-test generator pool and seed rule, and commit their hashes before automatic-saliency implementation begins.
- Lock the OCR evaluator, metric formulas, sampling frames, stratum weights, generator, fonts, and rendering environments.
- Prove the frozen OCR evaluator passes both development lossless bounded-character-error gates before any validation asset can be opened.

Acceptance gate:

- A real selected window streams to Chromium and Firefox.
- The record-only command produces an independently decoded `recording_profile_v1` stream without creating a signaling session or browser offer.
- Capture cancel, close, revoke, and shutdown paths pass.
- Every queue has an asserted bound.
- Disconnects do not leak session or frame resources.
- Token replay, hostile Origin, hostile Host, oversized signaling message, control-channel flood, and insecure non-loopback-bind tests pass.
- Owner and join token swaps, receiver-forged owner messages, sender impersonation, session fixation, and every invalid signaling state transition fail closed without revealing a capability.
- Receiver signaling closure, owner WSS close or error, owner heartbeat timeout, one-way owner partition, ICE failure, ICE-disconnected recovery, reservation timeout, owner-only timeout, and absolute expiry follow their exact frozen transitions and deadlines without extending the absolute lifetime.
- Every message from a stale owner connection generation and every attempted same-session owner reconnect fails closed after the old session reaches `REVOKED`.
- The self-hostable bundle works through its documented sender-outbound-WSS and receiver-browser route, and an unconfigured sender cannot create a remote link.
- The development lossless OCR floor is at most 0.02 overall and at most 0.05 on the defined 8-to-10-pixel rendered-glyph-height subset.
- Corpus and evaluation protocol validation passes without opening validation or final-test renderer output.

### Milestone 2 - CUDA conversion and saliency

Deliverables:

- Implement separate bounded packed-RGB source and NV12 encoder-surface pools with the declared ownership state machines.
- Implement BGRA or RGBA to NV12.
- Implement luma, contrast, temporal, tile, morphology, and map kernels.
- Implement CPU references and differential tests.
- Add NVTX and per-stage timing.
- Add protected-region preview.
- Tune only the declared `saliency_v1` parameters on the development split using the frozen search grid.

Acceptance gate:

- All goldens, random differential tests, boundary cases, and compute-sanitizer runs pass.
- Saliency output is deterministic for the same frame sequence.
- Total CUDA conversion, saliency, morphology, macroblock reduction, and host-map-copy P95 stays at or below 5 milliseconds at 1080p30 on the target hardware.

### Milestone 3 - NVENC enhanced path

Deliverables:

- Implement API and capability negotiation.
- Retain one selected CUDA primary context, use the declared per-thread guards, and enforce context identity on every CUDA and NVENC resource path.
- Register only the distinct contiguous NV12 encoder surfaces as NVENC CUDA input resources.
- Implement per-frame emphasis maps and ownership rings.
- Implement synchronous Linux output worker.
- Implement the explicit slot state machine and normal `NEED_MORE_INPUT` and `ENCODER_BUSY` handling.
- Implement keyframes, flush, teardown, and safe errors.
- Implement uniform NVENC and fixed-map modes.

Acceptance gate:

- Browser and independent decoders accept the stream.
- Multiple in-flight frames show no map or surface corruption.
- Every injected normal and fatal NVENC status preserves slot ownership and submission order.
- Uniform and fixed-map conditions reproduce the Milestone 0 quality shift.
- Error and teardown stress tests pass.

### Milestone 4 - Automatic protection and user controls

Deliverables:

- Combine automatic saliency, pinned regions, exclusions, and cursor halo.
- Add threshold and temporal hysteresis.
- Add local map preview and correction controls.
- Run the complete frozen `uniform_aq_v1` development grid, preserve every valid and invalid trial, and select its one best-supported uniform configuration through the committed deterministic selector.
- Commit the development-selected automatic-map configuration and both endpoint-specific uniform comparator identities before validation access.
- Verify the previously frozen corpus manifests and run the one-shot validation map gate under `corpus_protocol_v1`.
- Measure protected-region precision, recall, stability, and processing cost.

Acceptance gate:

- The lossless validation bounded character error rate is at most 0.02 overall and at most 0.05 on the defined 8-to-10-pixel rendered-glyph-height subset before compressed readability evidence is admissible.
- The equal-stratum validation glyph-truth macroblock recall is at least 90 percent overall and at least 80 percent for the 8-to-10-pixel rendered-glyph-height subset.
- The equal-stratum validation protected macroblock fraction is at most 35 percent.
- The equal-stratum validation false-protected macroblock fraction outside glyph and declared UI truth is at most 15 percent.
- The equal-stratum validation static-scene map-change statistic after a five-frame hysteresis warmup is at most 2 percent.
- Per-stratum values and the P95 sequence value for every map metric are reported even when the corpus-level gate passes.
- Theme, scroll, cursor, embedded-video, and small-font cases are included.
- Failure scenes and manual-correction behavior are documented.
- The complete AQ grid, target-search records, invalid reasons, selector inputs, and winning effective configuration hash reproduce the best-supported uniform choice without validation data.

### Milestone 5 - Constrained-link controller

Deliverables:

- Ingest encoded sizes, queue state, and WebRTC feedback.
- Implement the measured-bitrate window.
- Implement the rootless final-UDP-boundary egress counter and packet-capture cross-check for direct IPv4, direct IPv6, and TURN over UDP.
- Implement protected-threshold, emphasis-level, payload, presentation-profile, and stale-frame controls.
- Implement configured `tc netem` profiles.
- Add controller traces, simulations, and overload tests.

Acceptance gate:

- P95 one-second wire-egress bitrate remains within 110 percent of the cap on the frozen primary matrix.
- The production egress counter matches deterministic packet-capture IP lengths exactly and every steady benchmark within 1 percent.
- A path using unsupported IP headers, fragmentation, a bypassing socket, or TURN over TCP or TLS cannot enter a wire-cap-verified state.
- The stable-link matrix maintains at least 24 compositor-presented frames per second, at least 95 percent transport-to-compositor delivery, 1080p resolution, and P95 PipeWire-dequeue-to-render at or below 250 milliseconds on the named 50-millisecond-RTT profile.
- Queues remain bounded under sustained collapse.
- A recovery IDR arrives and P95 frame age returns within 10 percent of baseline no later than two seconds after the frozen recovery timer starts.
- Delivered frame rate and resolution recover within the frozen `2 + 2 * N` bound and eight-second absolute maximum.
- Every production controller trace replays byte-for-byte without future feedback.
- Dependency-epoch recovery produces no corrupted browser frames and no freeze longer than one second after the recovery IDR arrives.
- Pinned regions and rate violations are surfaced honestly.

Milestone 5 is an end-to-end engineering checkpoint, not a portfolio-ready or readability-improvement checkpoint.
No readability-improvement claim is authorized until the frozen Milestone 8 comparison passes, and the core portfolio release still requires Milestones 6, 8, and 9.

### Milestone 6 - Privacy, reliability, and packaging

Deliverables:

- Add screen-lock revocation, permission-revocation handling, and immediate session invalidation while keeping explicit user pause as a separate reversible state.
- Verify owner-signaling-loss revocation, receiver clearing, heartbeat deadlines, connection-generation fencing, and no-rebind behavior established in Milestone 1.
- Implement the separate connected-peer media state machine, authenticated pause and resume acknowledgments, exact transition timeouts, receiver clearing and reattachment, and IDR-gated resume.
- Add authenticated receiver clearing and retransmission-cache purge on stop, lock, revoke, and permission loss.
- Add the linearizable media-epoch admission and `MediaEgressGate` contract plus discard-only handling for late CUDA and NVENC completions.
- Harden malformed signaling, token abuse, replay, control-channel validation, and denial-of-service limits established in Milestone 1.
- Audit URL-fragment bearer handling, HTTPS and WSS enforcement, CSP, referrer policy, Host and Origin validation, and loopback-dashboard nonce and CSRF controls established in Milestone 1.
- Add optional coturn configuration with ephemeral credentials.
- Add package discovery without redistributing restricted SDK or codec artifacts.
- Add troubleshooting and capability matrix documentation.
- Add the crash-safe recording journal, durable `PREPARED` directory barrier, file and directory synchronization protocol, completion marker, recovery matrix, operation-by-operation crash injection, and unsynchronized-update filesystem crash model.
- Add exclusive no-clobber recording creation, safe directory-relative path handling, random recording epochs, IDR-gated mid-share start, and the 250-millisecond durability group-commit protocol.
- Freeze a recorder-throughput manifest containing the named storage device and filesystem, 1080p30 source hash, maximum V1 payload profile, ten-minute duration, queue limits, group interval, and acceptance thresholds before the release recording run.

Acceptance gate:

- Privacy and threat-model tests pass.
- The release threat model has no unresolved critical or high-severity finding for the implemented capture, local dashboard, signaling, transport, TURN, recording, and packaging paths.
- No RTP or SRTP media datagram crosses the local revocation boundary, including one completed late from a pre-boundary frame, and the receiver clears its last frame within 250 milliseconds on the named test profiles.
- The deterministic validation-to-final-UDP-send race test proves the egress gate is linearizable for stop, pause, screen lock, revocation, and permission loss.
- Screen lock reaches `REVOKED`, invalidates the owner capability, receiver role binding, signaling state, and TURN credentials, and cannot resume without a newly created session and join link.
- Owner WSS close, error, protocol failure, or five-second liveness timeout reaches `REVOKED` on both sides, closes remote media through the same egress boundary, invalidates TURN and role state, clears the receiver, and rejects stale-generation or same-session reconnect attempts.
- Pause preserves `CONNECTED` signaling and its original expiry, sends no post-boundary media, clears the receiver, and resumes only after the matching acknowledgment and a new IDR with SPS and PPS.
- A missing pause or resume acknowledgment, data-channel closure during transition, stale epoch, or absolute expiry ends the peer without reviving media.
- Every injected initialization or recording-finalization crash yields either a marker-verified complete recording or an inspectable incomplete recording whose valid `PREPARED` journal anchors zero or more durable complete access units.
- The filesystem crash model proves that no recording byte is admitted before the journal, initial companion entries, and parent-directory entry are durable, and that discarding unsynchronized updates cannot produce an unanchored artifact.
- A ten-minute 1080p30 recording at the maximum V1 bitrate on the named release storage completes without recorder failure, never exceeds one second or 32 MiB of queued media, commits every batch within one second, and independently decodes from its initial IDR through its final durable access unit.
- Preexisting output and symbolic-link fixtures remain byte-identical across every initialization, crash, and retry test, and an incomplete artifact cannot be replaced through the GlyphRelay CLI.
- Unsupported environments fail with actionable doctor output.
- No log, crash artifact, or telemetry fixture contains window content or titles.
- Clean install instructions work on the tested environment.

### Milestone 7 - DMA-BUF and pipeline optimization

Milestone 7 is an optional optimization tier and is not a dependency of Milestone 8 or the first Milestone 9 portfolio release.
It begins only after the shared-memory core path passes Milestone 6 and a profile identifies copy cost as material.

Deliverables:

- Audit PipeWire format and modifier negotiation.
- Implement DMA-BUF import only for proven compatible paths.
- Profile copies, synchronization, overlap, and kernels.
- Optimize only measured bottlenecks.
- Keep shared-memory fallback fully supported.

Acceptance gate:

- Nsight evidence identifies whether the tested path removes a full-frame copy and whether synchronization replaces its cost.
- Image correctness remains unchanged for every retained DMA-BUF path.
- DMA-BUF is enabled in the release only if ten paired 1080p30 runs show at least a 5-millisecond reduction in P95 capture-to-submit latency or a 10 percent relative reduction in mean sender CPU use without worsening correctness, drop rate, or wire-rate compliance.
- If that improvement gate fails, the experiment and evidence remain documented but the release leaves DMA-BUF disabled and makes no optimization claim.
- Unsupported modifiers fall back safely.

This milestone may conclude that DMA-BUF does not help on the target stack.
That is a valid result if measured and documented.

### Milestone 8 - Frozen reproducible evaluation

Deliverables:

- Verify the immutable development and validation hashes plus the final-test generation commitment from Milestone 1, then freeze the remaining network profiles, exclusion rules, analysis configuration, and environment before the one-shot final-test run.
- Run controlled uniform, best-supported uniform, oracle-pinned, automatic, CPU, and optional offline-ceiling conditions.
- Run at least ten randomized repetitions.
- Run paired bootstrap intervals.
- Run latency, rate, resource, and profiler studies.
- Conduct the predeclared blinded readability task with at least 12 consenting adult participants or omit all human-preference claims.
- Publish raw machine-readable results, plots, profiler summaries, and failures.

Acceptance gate:

- Automatic mode passes only if the point estimate and multiplicity-adjusted paired 97.5 percent lower confidence bound both reach 0.05 baseline-minus-automatic character error at exactly 1 Mbps or both reach 0.15 relative payload reduction at the fixed 10 percent character-error target.
- The lossless final-test bounded character error rate is at most 0.02 overall and at most 0.05 on the defined 8-to-10-pixel rendered-glyph-height subset.
- The final analysis retains no less than the declared total, per-stratum, and paired-repetition floors and excludes no more than the declared infrastructure limits.
- The one-sided 95 percent upper confidence bound for added P95 PipeWire-dequeue-to-render plus propagated timing uncertainty is no more than 10 milliseconds, decoded-frame loss adds no more than two percentage points, and wire-egress rate remains within the declared stable-link limit.
- Failed and excluded runs remain visible.
- Every chart is reproducible from committed analysis code and a versioned result artifact.

Kill gate:

- If neither practical-effect point estimate and adjusted lower confidence bound reaches its declared margin, remove the improvement claim or pivot the product.
- If a lossless OCR floor, total-sample floor, per-stratum floor, or paired-repetition floor fails, label the run `INSUFFICIENT_EVIDENCE` and make no improvement claim regardless of any effect estimate.

### Milestone 9 - Portfolio release

Deliverables:

- Finish and test the single-command `scripts/gpu/qualify_cuda_pm.sh` handoff and its resumable `scripts/gpu/run_remote_qualification.sh` runner.
- Run the consolidated `cuda-pm` qualification directly when agent SSH access is available, or leave the user exactly one entry-point command when the agent environment cannot establish SSH.
- Retrieve and hash-verify the complete GPU result bundle before using any remote result in public evidence.
- Finish README, architecture, API contracts, privacy model, benchmark methodology, profiler explanation, capability matrix, and limitations.
- Produce a short sender-to-browser demonstration.
- Verify the CPU clean-checkout path and the documented GPU path.
- Audit licenses, SDK distribution, codec terms, secrets, generated media, stale claims, and untracked files.
- Verify the pinned NVENC header license and notices, dynamic OpenH264 packaging choice, libdatachannel obligations, font licenses, OCR evaluator license, and every shipped binary in `THIRD_PARTY_NOTICES.md`.
- Preserve MPL-2.0 notices and source-file obligations for the pinned libdatachannel version and keep any local modifications isolated and publishable as required by that license.
- Prepare personal resume language outside public project docs unless explicitly intended.

Acceptance gate:

- `HANDOFF_READY` requires the versioned source bundle, safe sync path, resumable runner, consolidated action report, and verified artifact-return path to pass their local and disposable-remote tests.
- A `BLOCKED` or `FAILED` qualification is a truthful pause with a complete handoff report and cannot complete Milestone 9, unlock the portfolio release, or support a hardware claim.
- `QUALIFICATION_PASSED` requires a `PASSED` consolidated qualification with a verified artifact bundle plus Milestones 0 through 6 and Milestone 8 in `ACCEPTED` state.
- Milestone 9 becomes `ACCEPTED` only after `QUALIFICATION_PASSED` and every other Milestone 9 acceptance bullet pass.
- `RELEASE_PASSED` is derived only after Milestones 0 through 6, Milestone 8, and Milestone 9 are all `ACCEPTED`.
- No hardware claim is accepted from hand-copied terminal output, an unverified partial artifact, or a remotely generated file whose source and environment manifests do not match the qualification bundle.
- A clean checkout reproduces the supported build and tests.
- A compatible GPU environment reproduces the named benchmark.
- Every public claim maps to measured evidence.
- No public non-loopback release is permitted while the release threat model contains an unresolved critical or high-severity finding.
- The final release threat model, security tests, HTTPS and WSS configuration, capability authorization, Host and Origin controls, transport revocation, TURN configuration when shipped, and sensitive-data review must all pass before any non-loopback release.
- The worktree is clean.
- Publication waits for explicit authorization.

### Milestone 10 - Extensions after release

Eligible extensions are:

- Windows capture.
- Audio.
- Annotations.
- Accessibility metadata for known text regions.
- AV1 or HEVC recording.
- H.264 4:4:4 recording.
- Dual-layer text and embedded-video streaming.
- Multiple receivers.
- A project-operated multi-tenant public signaling and TURN service.

Remote control, neural semantic codecs, runtime OCR, and multi-party conferencing require separate product and threat-model plans.

## 33. Commit discipline

Every milestone ends in a focused commit only after its acceptance gate passes.
Large milestones use submilestone commits for capture, CUDA, encode, transport, UI, tests, and evidence.
Every commit leaves relevant formatting, static analysis, unit tests, integration tests, and sanitizers passing.
Do not commit SDK archives, generated recordings, sensitive profiler captures, tokens, TURN secrets, or local build outputs.
Do not push, deploy, or publish without explicit authorization.

## 34. Kill gates and pivots

Stop or revise the central approach if:

- The target GPU does not expose emphasis-map support.
- The Milestone 0 fixed map misses either the matched-rate 1.0 dB protected-region effect gate or the 0.75 dB spatial-allocation gate.
- Automatic maps fail both adjusted-lower-bound practical-effect gates in the frozen final-test run.
- P95 one-second wire-egress bitrate cannot stay within 110 percent of the selected cap while the stable-link delivery, resolution, and latency floors hold.
- CUDA conversion, saliency, map reduction, and host-map copy exceed 5 milliseconds at P95 on the target hardware.
- Chromium or Firefox cannot reliably decode the exactly negotiated NVENC stream and recover through PLI, IDR, SPS, and PPS.
- The pinned WebRTC stack cannot expose one maintainable final datagram hook that passes direct and TURN cap accounting plus the privacy linearization race.
- Queueing cannot remain bounded under congestion.
- Acceptable quality requires cloud OCR, a custom decoder, or legally unclear datasets or binaries.

Valid pivots include:

- A manual pinned-region screen-sharing product.
- A local recording optimizer.
- A uniform low-latency NVENC sender with a narrower performance claim.

An invalid pivot is silently relabeling uniform NVENC as automatic GlyphRelay mode.

## 35. Major risks and mitigations

### Capability fragmentation

Probe every feature and publish the tested matrix.
Keep uniform NVENC and CPU paths.

### Bitrate overshoot

Measure actual bytes, control emphasis and protected area, and report violations.

### Map instability

Use temporal hysteresis, generated motion cases, stability metrics, and user corrections.

### Capture interoperability

Make shared-memory capture the correctness baseline.
Treat DMA-BUF as optional and modifier-specific.

### Browser H.264 differences

Test Chromium and Firefox continuously with a conservative negotiated profile.
Retain an independent decoder test.

### Latency measurement error

Version the timestamp path, estimate clock error, and avoid unsupported glass-to-glass claims.

### Codec and SDK licensing

Do not redistribute NVIDIA archives, sample source, or codec binaries without review.
Keep download and discovery steps explicit.

### Scope growth

Keep audio, remote control, multi-party calls, neural OCR, other operating systems, and 4K outside V1.

## 36. Public evidence artifacts

The final repository includes:

- Doctor JSON from the tested environment.
- Capability matrix.
- Pinned NVENC header, driver API, browser SDP, codec, and license matrix.
- Prior-art comparison with mechanism and claim boundaries.
- Deterministic development, validation, and sealed final-test corpus manifests.
- CPU and CUDA correctness tables.
- Automatic-region coverage and stability report.
- Matched-measured-rate readability curves.
- Character-error and task-accuracy results.
- Latency and queue distributions.
- Bitrate compliance table.
- CPU, GPU, NVENC, memory, and power measurements.
- Nsight Systems timeline summary.
- Targeted Nsight Compute kernel analysis.
- Browser interoperability report.
- Network-failure report.
- Controller causal-replay artifact.
- Recording recovery and disk-failure report.
- Recording no-clobber, random-access start, and sustained-throughput report.
- Privacy and threat model.
- Short product demonstration.
- Reproduction commands and environment lock.
- Frozen protocol manifest containing corpus, comparator, operating-point, metric, threshold, margin, and analysis hashes.
- Complete `uniform_aq_v1` development grid and deterministic selection artifact.

## 37. Authoritative technical references

- [NVIDIA Video Codec SDK 13.1](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/index.html)
- [NVENC Video Encoder API programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html)
- [NVIDIA Video Codec SDK requirements](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/read-me/index.html)
- [NVIDIA Video Codec SDK license](https://developer.nvidia.com/nvidia-video-codec-sdk-license-agreement)
- [NVIDIA encode and decode support matrix](https://developer.nvidia.com/video-encode-and-decode-support-matrix)
- [NIST Randomness Beacon 2.0](https://csrc.nist.gov/Projects/interoperable-randomness-beacons/beacon-20)
- [PipeWire DMA-BUF guidance](https://docs.pipewire.org/1.2/page_dma_buf.html)
- [XDG ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
- [WebRTC video requirements in RFC 7742](https://www.rfc-editor.org/info/rfc7742/)
- [RTP payload format for H.264 in RFC 6184](https://www.rfc-editor.org/info/rfc6184/)
- [Extended RTP feedback in RFC 4585](https://www.rfc-editor.org/info/rfc4585/)
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [libdatachannel C API media handlers](https://github.com/paullouisageneau/libdatachannel/blob/master/DOC.md)
- [Browser video-frame callback timing](https://developer.mozilla.org/en-US/docs/Web/API/HTMLVideoElement/requestVideoFrameCallback)
- [OpenH264](https://github.com/cisco/openh264)
- [FFmpeg legal guidance](https://www.ffmpeg.org/legal.html)
- [OBS encoder ROI editor comparator](https://obsproject.com/forum/resources/encoder-region-of-interest-editor.1904/)
- [Screen-content coding overview](https://arxiv.org/abs/2011.14068)
- [Screen Content Dataset](https://videoprocessing.github.io/screen-content-dataset)

## 38. Representative official role references

- [Amazon EFA Network Software Engineer I](https://amazon.jobs/en/jobs/10481932/efa-network-software-engineer-i-annapurna-labs)
- [Anduril Early Career Software Engineer](https://boards.greenhouse.io/andurilindustries/jobs/4802146007)
- [Ciena Embedded Software Developer, New Grad](https://ciena.wd5.myworkdayjobs.com/Careers/job/Ottawa/Embedded-Software-Developer---New-Grad_R031490)
- [General Motors Autonomous Vehicles Software Systems](https://generalmotors.wd5.myworkdayjobs.com/Careers_GM/job/Sunnyvale-California-United-States-of-America/Software-Engineer--Autonomous-Vehicles-Software-Systems---Early-Career_JR-202604759)
- [Freeform Software Engineer, New Grad](https://job-boards.greenhouse.io/freeformfuturecorp/jobs/7826634003)
- [Rocket Lab Flight Software Engineer I](https://job-boards.greenhouse.io/rocketlab/jobs/7830196003)
- [True Anomaly Flight Software Engineer I](https://job-boards.greenhouse.io/trueanomalyinc/jobs/5090441007)
- [Crusoe Software Engineer I, Storage](https://jobs.ashbyhq.com/Crusoe/4f5d34ed-0c05-4eec-b8f8-14663e114b02/application?embed=true)
- [Cerebras Software Engineer, New Grad](https://jobs.ashbyhq.com/cerebras/99c289fa-8fc6-49f7-b7e8-78ac4e9d99ac/application)
- [ByteDance Traffic Infrastructure, New Grad](https://jobs.bytedance.com/en/position/7665849950984194309/detail)
- [ByteDance Backend Inference Runtime, New Grad](https://jobs.bytedance.com/en/position/7669789046777940229/detail)
- [NVIDIA Backend Compiler Engineer, New College Grad](https://nvidia.wd5.myworkdayjobs.com/NVIDIAExternalCareerSite/job/US-CA-Santa-Clara/Backend-Compiler-Engineer---New-College-Grad-2026_JR2021242)
- [NVIDIA TensorRT Performance, New College Grad](https://nvidia.wd5.myworkdayjobs.com/NVIDIAExternalCareerSite/job/US-CA-Santa-Clara/Deep-Learning-Software-Engineer--TensorRT-Performance---New-College-Grad-2026_JR2015071)
- [NVIDIA Systems Software Engineer, New College Grad](https://nvidia.wd5.myworkdayjobs.com/NVIDIAExternalCareerSite/job/US-OR-Hillsboro/Systems-Software-Engineer---New-College-Grad-2026_JR2017083)
- [NXP NFC System Software Engineer, Entry Level](https://nxp.wd3.myworkdayjobs.com/en-US/careers/job/San-Jose-Holger-Way/NFC-System-Software-Engineer---Entry-Level_R-10064298)
- [Akuna Entry-Level C++ Software Engineer](https://www.akunacapital.com/careers/job/8013085/?gh_jid=8013085)

Role pages can close after the planning snapshot.
The project stays aligned to the repeated work signals rather than any single employer's exact stack.

## 39. Final positioning

The strongest interview story is:

> I built a low-latency screen-sharing product that moves desktop frames through a measured C++ and CUDA pipeline, controls NVENC at macroblock granularity, stays interoperable with standard browsers, and proves its text-readability benefit at matched measured bitrate under reproducible network conditions.

That story is credible only if the repository contains the capability gate, original kernels, bounded queues, standard receiver, matched-rate study, profiler evidence, failure tests, and honest hardware matrix required by this plan.
