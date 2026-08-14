# Protected-region saliency

`saliency_v1` is GlyphRelay's deterministic, non-neural protected-region saliency protocol.

The scalar implementation is the correctness oracle for the CUDA implementation and for development-only parameter selection.

It does not claim to detect text.

## Input and output contract

The input is a pitch-aware visible luma plane with an explicit full or limited range, frame identity, geometry epoch, and monotonic timestamp.

Limited-range samples are expanded to canonical 8-bit codes with round-to-nearest and ties-to-even.

No per-frame minimum-to-maximum normalization is permitted.

The algorithm emits 8-by-8 tile features, final protected levels, a raster-order signed-byte macroblock map, a level histogram, and the protected macroblock fraction.

Every partial visible tile is included, and clamp-to-edge extension is used only for the three-by-three Scharr footprint.

The macroblock dimensions equal `ceil(coded_width / 16)` by `ceil(coded_height / 16)` because even padding of an odd visible dimension cannot cross an additional macroblock boundary.

## Edge-pair interpretation

The plan defines one edge-pair anchor as one pixel and one horizontal or vertical orientation but leaves the scan direction implicit.

`saliency_v1` resolves that ambiguity by searching only increasing x for horizontal anchors and increasing y for vertical anchors.

This canonical forward direction counts an unordered pixel pair once and makes the eligible-anchor denominator exact.

Changing that direction rule requires a new protocol version and fresh corpus gates.

## Temporal and precedence behavior

Temporal comparison uses only the most recent processed frame in the same geometry epoch when its timestamp gap is at most 200 milliseconds.

A geometry change requires a new epoch and resets luma, score, and hysteresis history.

A larger timestamp gap resets the luma-change comparison while retaining score hysteresis in the unchanged epoch.

Dilation applies only to automatic levels.

Cursor halos then raise the selected level, pins raise it next, and exclusions finally force level zero.

The preview uses the same final tile levels as macroblock reduction and never leaves the local process.

## Ownership contract

Packed RGB sources and NV12 encoder surfaces use separate fixed-capacity rings.

Each reservation binds both slots to one frame identity and geometry epoch.

The packed source becomes reusable only after its CUDA read-completion event.

The NV12 surface remains owned until map copy, submit readiness, NVENC submission, encoder input release, and final surface release complete in order.

Registered source and surface allocation ranges may never alias, and stale tokens fail closed.

Fatal preprocessing cleanup requires an exact matching token and atomically returns only that frame's owned slots.

## CUDA execution

The Linux GPU implementation retains the selected CUDA primary context and enters it through a scoped guard for allocation, enqueue, completion, debug-copy, and cleanup operations.

One nonblocking stream orders input upload, pitch-aware BGRA or RGBA conversion, canonical luma expansion, Scharr gradients, tile features, temporal hysteresis, morphology, macroblock reduction, and the asynchronous device-to-pinned-host map copy.

Each packed source has a distinct device allocation and source-read event.

Each encoder surface has a distinct contiguous NV12 allocation, device feature storage, device map, pinned host map, timing events, and explicit NVENC lifetime.

CUDA events fence every stage and produce both per-stage durations and a directly ranked total-pipeline duration.

NVTX ranges name the same stages for target profiling without using profiler time as acceptance evidence.

The correctness executable compares both color planes and every saliency feature against the scalar reference across frozen goldens, randomized temporal input, partial geometry, both pixel orders, both range contracts, overrides, and a 1920x1080 frame.

An independent replay requires byte-identical NV12 and map output plus exactly identical feature state.

The target performance run uses 300 warmup frames followed by at least 1,800 measured 1920x1080 frames and accepts only a directly measured total P95 at or below 5 milliseconds.
The measured interval extends when needed to span at least ten seconds so the performance-environment sampler can observe the active compute workload.

The target runs memcheck, initcheck, racecheck, and synccheck before the performance gate.

## Local verification

Build and run the scalar goldens, randomized determinism checks, boundary cases, ownership checks, and the real preview command with:

```bash
uvx --from cmake==4.1.0 cmake --preset macos-local
uvx --from cmake==4.1.0 cmake --build --preset macos-local
uvx --from cmake==4.1.0 ctest --preset macos-local -R 'native.saliency|native.preprocess_pool|cli.saliency_preview'
build/macos-local/glyphrelay_saliency_preview --output build/saliency-preview.ppm
```

The preview command creates a new PPM file and refuses to overwrite an existing path.

The preview is qualitative inspection evidence only and does not satisfy map-recall or readability gates.

CUDA differential, compute-sanitizer, and 1080p30 P95 evidence remain designated-target gates.

The CUDA translation unit and qualification executable compile locally through a pinned official NVIDIA CUDA image:

```bash
make cuda-compile-check
```

That command is compile evidence only because the local Docker host exposes no NVIDIA GPU.

The consolidated target entry point owns runtime qualification:

```bash
./scripts/gpu/qualify_cuda_pm.sh
```
