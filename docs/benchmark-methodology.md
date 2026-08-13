# Benchmark methodology

This document describes the committed Milestone 0 feasibility protocol.

It does not report a GPU result, quality improvement, bitrate result, or latency result.

Those claims remain deferred until the target qualification bundle passes every acceptance gate.

## Frozen input

`m0_fixed_map_v1` generates 2,100 deterministic 1920 by 1088 coded NV12 frames with a 1920 by 1080 visible crop at 30 frames per second.

The first 300 frames are the ten-second warmup and the remaining 1,800 frames are the one-minute measurement interval.

The generator uses 8-bit limited-range BT.709 luma with neutral chroma and repeats the final visible luma row into the coded padding.

Two equal-area 640 by 320 text regions provide the protected center and unprotected comparison samples.

The source includes deterministic cursor, line-selection, and lower-panel motion so the encoder does not receive a static still frame.

`frame-hashes.tsv` commits the SHA-256 of the complete coded NV12 byte sequence for every source frame.

The public benchmark command regenerates and verifies all 2,100 frames before it probes hardware or creates output.

## Frozen emphasis map

The coded geometry contains 120 by 68 H.264 macroblocks.

The map contains exactly 8,160 signed byte entries in row-major raster order.

The 40 by 20 macroblocks intersecting the protected rectangle receive emphasis level four and all other macroblocks receive level zero.

The committed RLE encoding expands to exactly the map produced by the independent geometry implementation.

Spatial AQ, temporal AQ, lookahead, B frames, and multipass are disabled in both measured conditions.

The fixed condition differs from the controlled-uniform condition only by enabling and supplying the frozen map.

## Rate matching and runs

Each condition calibrates its requested payload target independently through at most eight bounded integer-bisection iterations from 0.8 through 1.2 Mbps.

Calibration selects the observation closest to 1 Mbps measured elementary-stream payload, breaking a tie toward the lower configured target.

The configured target is then held constant for all ten measured repeats of that condition.

Every measured mean payload must fall from 0.98 through 1.02 Mbps and the two condition means may differ by at most two percent.

Every effective NVENC initialization and reconfiguration field is serialized after preset expansion and hashed before submission.

## Quality metric

An independent decoder provides visible-frame 8-bit luma matched by source-frame index.

Squared error is accumulated exactly in an unsigned 64-bit integer.

Mean squared error divides that sum by the number of visible samples in the declared region.

PSNR is `10 * log10(255^2 / MSE)`, with a lossless region represented as positive infinity.

The report retains every per-frame measurement and reports arithmetic means for the whole visible frame, protected region, unprotected comparison region, and protected-minus-unprotected difference.

The fixed condition must improve protected-region PSNR by at least 1.0 dB and the protected-minus-unprotected allocation by at least 0.75 dB at the matched measured rate.

## Protocol identity and failure behavior

The manifest hashes the source declaration, both masks, map, run configuration, metric declaration, all frame hashes, the generator, the metric implementation, the SHA-256 implementation, the verifier, and the freeze tool.

Its immutable identity is `3428958bf30b487e34c106614f83b59fe2526cfd46e1beb9a9249b70f2b1c717`.

Changing any locked byte produces benchmark-gate exit code 7 before hardware use.

An unsupported host exits with capability code 3 only after the complete lock passes and does not create a benchmark result directory.

An infrastructure error, timeout, unsupported encoder feature, failed independent decode, or incomplete run never counts as passing evidence.
