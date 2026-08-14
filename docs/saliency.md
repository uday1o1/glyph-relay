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

Manual pins and exclusions are held in a bounded, revisioned correction set using source-visible coordinates.

Each mutation names its expected revision, and stale revisions fail without changing the set.

Changing the geometry epoch clears every correction atomically so a rectangle cannot be reused against a different crop or resolution.

When a pin and an exclusion overlap, the encoder-facing map remains level zero and the local preview overlays the affected tile in a distinct conflict color.

The public preview path exercises these controls with repeated `--pin X,Y,W,H` and `--exclude X,Y,W,H` arguments.

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

## Frozen development selection

The committed `saliency_v1` development grid contains exactly 2,511 configurations.

It is the product of 31 valid four-weight tuples, three entry thresholds, three exit thresholds, three previous-score coefficients, and three dilation radii.

Every trial must be preserved as either `PASSED` with complete aggregate, per-stratum, sequence-P95, and processing-P95 measurements or `INVALID` with a nonempty reason.

False-discovery fraction is retained alongside the selection metrics so downstream analysis can distinguish excess protected area from low precision.

The evidence binds the source bundle, automatic-map implementation, processing platform, corpus protocol, development manifest, development render index, and frozen grid by SHA-256.

The target evaluator renders only the pinned development split in the locked corpus container and rasterizes exact glyph, small-glyph, and typed UI macroblock truth from all 256 rendered frames.

Each configuration runs through the production CUDA preprocessing pipeline.

Every sequence contributes the fifth repeated initial frame plus source frames 60, 120, and 180, with gaps above 200 milliseconds and CUDA-event total-pipeline measurements attached to the same completed outputs.

Candidate checkpoints are checksum-protected, durably published, and bound to the complete evaluation identity so an interrupted 2,511-candidate run resumes without accepting stale work.

The selector rejects evidence unless all 2,511 configurations appear exactly once.

It first removes every configuration that misses any development map threshold.

It then maximizes small-glyph recall and minimizes false-protected fraction, protected fraction, static map-change fraction, and P95 processing time in that order.

An exact canonical JSON byte serialization supplies the final lexicographic tie-break.

The public selector refuses to run if validation or final-test renderer output exists and refuses to overwrite an existing selection.

The protocol identity is `9d11f1621b5174e985f520ee060c8e55371773131725cd0df65a7b8c96042cce`.

Verify the frozen grid and selector without opening either held-out split with:

```bash
make protocol-check
```

Once real designated-target development evidence exists, freeze and independently reproduce the one selected configuration with:

```bash
uv run python tools/corpus/saliency_selector.py \
  --development-evidence artifacts/private/saliency-development-evidence.json \
  --output protocols/saliency_v1/selected-configuration.json
uv run python tools/validate_saliency_selection.py \
  --development-evidence artifacts/private/saliency-development-evidence.json \
  --selection protocols/saliency_v1/selected-configuration.json
```

No selected configuration is currently committed because the local macOS host cannot produce the required CUDA development-map and processing-time evidence.

That target dependency is a deferred acceptance gate, not a passing result.

The consolidated target phase runs the full grid and returns a preserved `BLOCKED` freeze handoff after independently reproducing the selected configuration.

The selected artifact must then be committed before any held-out renderer output may be opened, after which the same consolidated command verifies the committed freeze and proceeds.

## One-shot validation

`saliency_validation_v1` is a validation-only execution package and is not a tuning path.

Its protocol identity is `df5df92b343772a4fa84763824df527805c1925bbedf5cbe6698ce23f6f4360e`.

It refuses to start until the selected `saliency_v1` configuration and selected `uniform_aq_v1` comparator artifact both exist byte-for-byte in the current Git commit.

The command rejects validation pixels that exist before its durable access ledger.

On first access, it writes and synchronizes the ledger before launching the pinned renderer, then seals the resulting render-index hash without modifying the ledger.

An interrupted first run may resume only when the repository commit, source bundle, processing platform, protocol, manifest, implementation, and both selection identities match exactly.

An ordinary rerun after final evidence exists is rejected.

Explicit reproduction mode verifies the original ledger, render seal, render index, and evidence without replacing the original result.

The lossless OCR gate reuses the frozen bounded-CER implementation and reports equal-stratum overall and small-glyph results before any later compressed readability claim is admissible.

The selected automatic configuration alone is evaluated through the production CUDA preprocessor on all 64 validation sequences.

Aggregate, per-stratum, nearest-rank P95 sequence, per-sequence, and processing-P95 values are preserved for recall, small-glyph recall, protected fraction, protected-truth precision, false-protected fraction, false-discovery fraction, and static map change.

The evidence records both frozen themes, rapid scrolling, caret samples, mixed video and text, and the exact small-glyph count.

The eight highest diagnostic failure scores are reported as failure scenes and cannot feed back into parameter selection.

Manual correction behavior remains the bounded pin, exclusion, cursor, geometry-reset, and conflict contract documented above.

The consolidated target phase is `saliency-validation`.

It owns the only authorized first-open path and runs after both development-selection phases.

The local host verifies the renderer, ledger, schemas, seeded threshold failures, frozen OCR reuse, native public help path, and protocol lock without rendering validation pixels.

The actual one-shot measurements remain a designated-target acceptance gate.

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
