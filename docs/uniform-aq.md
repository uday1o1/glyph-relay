# Uniform AQ development protocol

`uniform_aq_v1` selects one best-supported uniform NVENC adaptive-quantization configuration before validation data can be opened.

The workflow is development-only and does not claim that any candidate passes until the designated target produces the complete evidence artifact.

## Frozen candidates

Controlled uniform disables spatial AQ, temporal AQ, and emphasis maps.

The candidate grid contains temporal-only AQ plus spatial strengths 1, 4, 8, 12, and 15 with temporal AQ both disabled and enabled.

Every candidate uses the same Baseline profile, Level 4.0, P4 low-latency preset, CBR mode, one-frame VBV, 60-frame GOP, no B frames, no lookahead, no multipass, one reference frame, 8-bit 4:2:0 output, repeated SPS and PPS, and filler-data behavior.

The native runner reconstructs and hashes the complete declared effective-field object before it opens the source bundle or the encoder.

The production encoder rejects spatial or temporal AQ in either fixed-emphasis or automatic-emphasis mode.

## Source and systems schedule

Every trial encodes all 64 development sequences as one 15,360-frame 1920 by 1080 stream at 30 frames per second.

Each frozen sample image supplies the exact 60-frame interval beginning at its declared frame identity, so frames 0, 60, 120, and 180 remain the immutable OCR samples for every sequence.

The first 300 frames warm the pipeline.

The next 300 frames run in real time and provide preprocessing, encode, combined-latency, pending-work, and sender-CPU measurements.

The remaining frames retain their 30 fps timestamps but wait only for serial output completion, which keeps the complete development grid bounded without admitting accelerated-tail timing samples.

Every trial requires 15,360 submitted and encoded frames, 1920 by 1080 geometry, no positive pending-window trend, at most 5 ms preprocessing P95, at most 10 ms encode P95, at most 16 ms encode P99, at most 15 ms combined P95, and at most 33.34 ms maximum measured pending age.

## Rate search and decode checks

Each condition starts at exactly 0.5, 0.75, 1, 2, and 4 million requested payload bits per second.

At most four trials may be used for each target.

After a valid mismatched trial, the next integer request is the rounded previous request multiplied by target rate and divided by measured rate.

The requested range is bounded from 100,000 through 20,000,000 bits per second.

Interrupted or invalid native trials remain visible and retry the same requested rate within the fixed attempt budget.

A selected trial must measure within two percent of its target.

FFprobe independently decodes and counts all 15,360 frames, while FFmpeg extracts exactly the 256 immutable OCR samples.

The frozen Chromium renderer applies the same rounded BT.709 luma, polarity normalization, nearest-neighbor enlargement, and border pipeline used by lossless corpus evaluation.

The digest-pinned Tesseract container evaluates all 512 text regions and reports every sequence and stratum.

The actual selected H.264 stream is remuxed without re-encoding, served only from an ephemeral loopback endpoint, and seek-decoded at five distributed positions by Chromium 151.0.7922.34 and Firefox 153.0.

Any native, independent-decoder, OCR, browser, geometry, frame-count, latency, pending-work, or identity failure makes that target invalid.

## Selection and freeze

Pool-adjacent-violators regression makes character error nonincreasing in log measured bitrate, and interpolation occurs only between adjacent fitted points.

The selector minimizes the equal-weight mean of fitted character error at the five target rates, then uses the frozen 1 Mbps error, estimable 10 percent-error crossing, combined latency, CPU, and lexical field tie breaks.

The same winner supplies both co-primary uniform endpoints, while controlled uniform wins an exact endpoint comparator tie.

The complete execution path, schemas, source schedule, encoder configuration, OCR tools, browser tools, qualification phase, and selector are bound by protocol SHA-256 `fabcba7efe362d2f4c8359441527b9b728e165b56d7f933cfc89950a2e766a3c`.

Run the local lock and algorithm checks with:

```bash
make protocol-check
uv run pytest -q tests/python/test_uniform_aq_selector.py tests/python/test_uniform_aq_development.py
```

Run the target workflow only through the consolidated entry point:

```bash
./scripts/gpu/qualify_cuda_pm.sh
```

The first successful development evaluation returns a preserved repository-freeze handoff and does not open validation data.

The selected artifact must be committed before the same entry point can reproduce the result and advance.

No selected AQ artifact or target result is currently committed because the local macOS host has no NVIDIA GPU.
