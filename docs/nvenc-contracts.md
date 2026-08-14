# CUDA and NVENC ownership contracts

GlyphRelay's Linux GPU path is built around one retained CUDA primary context and the pinned NVENC 13.1 API.

The target-only capability and encode gates remain deferred until the consolidated GPU qualification runs.

Portable state-machine tests are implementation evidence only and do not claim that local macOS or CPU-container hardware supports CUDA or NVENC.

## Reproducible header boundary

Run `scripts/bootstrap_nvcodec_headers.sh` before configuring the `linux-gpu` preset.

The script checks out `nv-codec-headers` tag `n13.1.15.0`, verifies commit `0a6fba9a2820628b8103464f4c8753ee05838baa`, and verifies the selected `nvEncodeAPI.h` SHA-256 digest `8776fddcb8febc6aec4d73989b1f21831eb30306bc583da55b4bf0c14a1dc228`.

The checkout lives under the ignored `.deps` directory.

The Linux GPU configure fails closed if the header is absent or its hash differs.

The compiled source statically checks that emphasis levels zero through five have the exact values locked by the protocol.

## Primary-context lifetime

`CudaPrimaryContext` initializes the CUDA Driver API, resolves the selected device, and calls `cuDevicePrimaryCtxRetain` before any later resource or NVENC session can be created.

Retaining a primary context does not make it current on a CPU thread.

Every CUDA or NVENC call site therefore creates a move-only `ScopedCudaContext`, which pushes the retained context and pops the same identity on scope exit so the prior per-thread context is restored.

Each retained context receives a process-local generation, and every resource contract records the device ordinal, opaque context handle, and generation.

The capability probe passes that same retained context to `nvEncOpenEncodeSessionEx` with `NV_ENC_DEVICE_TYPE_CUDA`.

Shutdown cannot release the primary context while a guard is active.

The required teardown order is admission close, producer joins, event observation or cancellation, NVENC drain, resource unregister and unmap, stream and allocation destruction, context-worker joins, and primary-context release.

The portable shutdown model rejects every skipped or reordered phase.

## Frame ownership

Shared-memory device sources follow `FREE -> HOST_TO_DEVICE_PENDING -> CUDA_SOURCE_READ_PENDING -> FREE`.

Imported DMA-BUF sources follow `PIPEWIRE_OWNED -> CUDA_SOURCE_READ_PENDING -> PIPEWIRE_REQUEUE_PENDING -> RELEASED`.

NV12 encoder surfaces follow `FREE -> CUDA_WRITING -> MAP_COPY_PENDING -> READY_TO_SUBMIT -> SUBMITTED -> ENCODER_INPUT_RELEASED -> FREE`.

The transition predicates reject direct reuse while CUDA, PipeWire, or NVENC still owns a resource.

One encoded surface is a private contiguous CUDA-device NV12 allocation with one base pointer, one pitch, a luma extent, and the following interleaved chroma extent.

The packed source and NV12 encoder surface are separate allocations.

## Pre-submit gate

`validate_nvenc_submission` is the only portable admission point before the driver call.

It rejects an absent output buffer, a foreign or invalid CUDA context, a stale frame identifier, a stale geometry epoch, an invalid or noncontiguous NV12 allocation, an unfinished CUDA event, a wrong macroblock shape or byte size, pageable or mismatched map memory, an unfinished map-copy event, and emphasis values outside zero through five.

For H.264, the required map shape is `ceil(coded_width / 16) * ceil(coded_height / 16)` signed bytes in raster order.

Tests count driver invocations and prove every seeded preflight defect returns before that boundary.

Uniform submissions carry no emphasis descriptor or backing storage.

Fixed-emphasis submissions use a configuration-owned pinned map copied into each submission slot, while automatic-emphasis submissions use the exact pinned map produced beside that frame's NV12 surface.

The retry fingerprint snapshots every surface field, mode, force-IDR flag, map descriptor, and map byte, so even an in-place map mutation is rejected before another driver call.

## Submission and output ownership

The single encode owner constructs `NvencSubmissionCoordinator` with fixed slot and busy-retry bounds.

Every live slot owns exactly one immutable submission fingerprint and one unique output bitstream handle.

`ENCODER_BUSY` retains the same request in `SUBMIT_RETRY_PENDING`, adds no FIFO entry, rejects any mutated retry, and enters the explicit fatal path if the configured retry bound is exceeded.

`NEED_MORE_INPUT` appends the submission exactly once but does not make that submission lockable until a later `SUCCESS` or end-of-stream drain permits FIFO output acquisition.

`SUCCESS` appends exactly once and makes every earlier delayed FIFO entry and the current entry available in submission order.

Only the FIFO head can enter `BITSTREAM_LOCKED` or complete.

Completing the head releases its slot and advances only the next ready head.

End of stream makes every delayed FIFO entry drainable in order.

A fatal driver result, missing callback, or thrown callback moves live non-free slots to `ABORT_PENDING` and requires explicit driver cleanup confirmation before reuse.

If the FIFO head is already `BITSTREAM_LOCKED`, fatal transition preserves that ownership until the output worker finishes the actual unlock operation.

The production `NvencEncoder` retains the same primary CUDA context as its `CudaPreprocessor`, registers each private device NV12 surface at most once, maps it only for an owned submission, and uses one bitstream buffer per slot.

A dedicated output thread performs the only blocking bitstream lock, parses each Annex B access unit, checks frame identity, invokes the output callback, and releases the corresponding preprocessing ticket in FIFO order.

Close sends end of stream, drains accepted frames, joins the worker, unmaps and unregisters resources, destroys every bitstream buffer, destroys the encoder session, and leaves primary-context release to the last shared owner.

The target `glyphrelay_nvenc_encoder_qualify` command exercises 300 frames in each of uniform, fixed-emphasis, and automatic-emphasis mode, requires multiple simultaneous owned submissions, and performs ten additional create-submit-drain-destroy cycles with a foreign-generation rejection control.

The repository-owned runner loads the committed `saliency_v1` selection, verifies its canonical configuration hash, passes every selected parameter explicitly, and binds both the selection-file hash and configuration hash into the native evidence.

Its independent validator verifies exact stream identities and uses FFmpeg and FFprobe to decode and inspect every complete H.264 elementary stream.

## Target probe

The `glyphrelay_probe_nvenc` target exists only in a verified `linux-gpu` build.

It loads `libnvidia-encode.so.1`, resolves the official entry points, compares the driver API major and minor version with the compiled header, creates the official function table, opens one CUDA encode session, enumerates H.264 and NV12 support, queries emphasis-map support and maximum geometry, closes the encoder, and emits bounded JSON without raw context handles.

The command succeeds only when H.264, NV12, the emphasis map, and API compatibility all pass.

Capability presence, foreign-context rejection at the real registration boundary, full NVENC submission behavior, and worker-before-context-release remain target-only gates.
