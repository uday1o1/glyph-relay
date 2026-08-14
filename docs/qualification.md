# Consolidated qualification

GlyphRelay uses a content-addressed source bundle and a resumable remote runner for every designated-GPU acceptance run.

This workflow keeps a hardware result tied to the exact committed source, dependency locks, qualification scripts, environment fingerprint, executable hashes, and phase outputs that produced it.

It does not accept copied terminal output or an unverified partial artifact as evidence.

## Local verification

Run the disposable workflow from the repository root:

```bash
make handoff-check
```

The check requires a clean Git worktree because the production bundler intentionally refuses staged, unstaged, or untracked source changes.

It builds a canonical archive from Git-tracked files, extracts it into a temporary private namespace, runs a passing two-phase qualification, retrieves the result archive, verifies both checksum layers, and inspects the declared phase artifact.

The full clean-source check also exercises this path:

```bash
make clean-tree-check
```

The production phase graph includes an exact final-datagram packet-capture phase.
It requires an accessible local Docker daemon, tshark access to the Linux loopback interface, the locked coturn image, and the built DTLS-SRTP fixture.
The phase retains raw captures privately and exports only the safe reduced validation summary after exact per-payload and aggregate byte agreement.

## Designated GPU workflow

After every locally executable milestone package is ready, run one command from the repository root:

```bash
./scripts/gpu/qualify_cuda_pm.sh
```

The entry point defaults to the configured `cuda-pm` SSH alias and never embeds a host address or remote user path.

It resolves the remote home through SSH, validates a private namespace beneath that home, establishes a durable random ownership sentinel, transfers only the manifest-declared bundle, verifies the bootstrap tool hash, and safely extracts into a new directory named by the bundle hash.

The entry point then launches `scripts/gpu/run_remote_qualification.sh` in a detached `tmux` session or its tested `nohup` fallback.

Repeated invocations resume the active run and reuse only successful phases whose input identity and output hashes still match.

A failed or blocked completed cycle receives a new bounded cycle identifier on the next invocation.

Set `GLYPHRELAY_GPU_HOST` only to select another configured SSH alias.

Set `GLYPHRELAY_GPU_NAMESPACE` only to select a canonical absolute directory strictly below the resolved remote home.

Set `GLYPHRELAY_POLL_SECONDS` to change the polling interval or `GLYPHRELAY_MAX_WAIT_SECONDS` to impose a bounded local wait.

The default maximum wait is unlimited because disconnect-safe qualification can include long benchmarks.

## Remote safety and durability

The remote namespace and its children must be real directories owned by the remote user with mode `0700`.

The ownership sentinel must be a real file owned by the remote user with mode `0600` and must match the locally retained identifier.

Archive paths, modes, hashes, symbolic-link targets, member sets, and the embedded manifest are checked before and after extraction.

The transfer path never uses a broad synchronization delete.

One atomic lock protects each source bundle.

After deterministic GPU selection, a second lock protects the selected GPU across bundles.

A lock is reclaimed only after its heartbeat is stale and both its recorded process and detached session are no longer live.

Every phase writes into a timestamped private directory and durably records redacted commands, named environment variables, redacted bounded logs, duration, result state, and output hashes.

Every command phase has a frozen wall-clock bound from one second through 24 hours.

A timeout terminates the complete process group, records `BLOCKED`, and can never become passing evidence.

An interrupted successful phase is reusable only when its result hash, input hash, executable identity, environment fingerprint, and every declared output hash still match.

Independent phases continue after a nonfatal failure so one cycle returns a consolidated action report.

The required `runner-integrity` phase records environment-capture, sampler, GPU-lock, heartbeat, and bundle-lock cleanup failures before final status derivation.

An environment-capture or runner fault produces a complete nonpassing result with blocked remaining phases whenever the private run directory remains writable.

### Performance contamination policy

The GPU sampler begins after deterministic device selection and records structured five-second samples plus synchronous phase-boundary samples.

Each sample contains GPU and NVENC utilization, memory use, SM and video clocks, temperature, power, active throttle reasons, volatile uncorrected ECC totals, competing compute processes, and kernel-journal NVIDIA Xid events.

Process ancestry distinguishes qualification subprocesses from foreign compute workloads on the selected GPU.

The frozen `nvenc-performance-v1` policy requires at least two complete samples, readable GPU, process, and Xid telemetry, observed NVENC activity, no foreign compute process, no Xid event, no increase in volatile uncorrected ECC errors, temperature at or below 83 C, no active throttle reason while NVENC is busy, and an active video-clock minimum at least 75 percent of its active maximum.

A command success with any resource-policy violation becomes `FAILED` with a structured `resource-assessment.json` artifact.

## Results

The runner always treats absent hardware, missing tools, unsupported NVENC capabilities, unavailable desktop interaction, and infrastructure faults as `BLOCKED` or `FAILED`.

A normal completed result contains `status.json`, `environment.json`, `commands.jsonl`, `runner.json`, `junit.xml`, `REPORT.md`, phase records, artifacts, a `public-evidence` candidate, and `SHA256SUMS`.

Blocked runs also contain `USER_ACTION_REQUIRED.md` with the consolidated prerequisites and the stable resume command.

The remote private result archive and the separate public-evidence archive each have an external SHA-256 file.

The Mac-side entry point downloads both archives and both checksum files into a newly created ignored directory under `artifacts/gpu-runs/<run-id>`, safely extracts them, verifies both complete internal member sets, and checks their run, source, status, and bundle identities.

The public-evidence candidate includes only allowlisted JSON summaries from passing phases plus a reduced platform and phase summary.

Generation rejects user paths, qualification paths, private IPv4 addresses, GPU UUIDs, credential-bearing URIs, and private-key markers.

Automated redaction does not authorize publication.

`PUBLICATION_REVIEW_REQUIRED.md` and the schema-level `publication_review_required` flag require a separate human claim-to-evidence review before any public use.

The entry point prints one JSON summary and returns:

- `0` only when the qualification status is exactly `PASSED`.

- `4` when a required phase is `FAILED`.

- `5` when qualification is `BLOCKED` or the handoff infrastructure cannot establish trustworthy evidence.

A `PASSED` runner result is eligible for milestone review but does not by itself accept a milestone or release.

Milestone acceptance remains governed by every gate in `BUILD_PLAN.md` and the state recorded in `docs/implementation-status.md`.
