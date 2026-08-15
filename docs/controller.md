# Protected-region controller

`controller_v1` is the frozen protocol for adapting emphasis, payload rate, and presentation profile from measured sender behavior.
The machine-readable source of truth is `protocols/controller_v1/manifest.json`, and `make protocol-check` verifies its schema, semantics, and content-addressed lock.

## Measurement contract

The controller ticks every 100 milliseconds from cumulative monotonic counters.
It computes one-second and five-second exponentially weighted estimates with IEEE 754 binary64 arithmetic using the exact equation in the manifest.
A decreasing cumulative counter resets only its own estimator and never creates a negative sample.
Feedback is eligible only after sender arrival and becomes unavailable after two seconds without a newer valid sample.
Unavailable loss or round-trip time is not treated as zero.

The named wire metric counts each successful final UDP datagram as its payload, the eight-byte UDP header, and the connected socket's IPv4 or IPv6 base header.
Direct UDP ICE and TURN over UDP can enter the verified-cap state.
TURN over TCP or TLS, IPv4 options, IPv6 extension headers, fragmentation, and any bypass socket cannot enter that state.

## Control stack

Degradation lowers automatic emphasis first, tightens protected thresholds second, lowers payload and VBV in ten-percent steps third, and lowers the presentation profile last.
Restoration uses the exact reverse order.
Stale-frame dropping remains active continuously before encoder submission.
The four presentation profiles are 1080p30, 1080p24, 720p24, and 720p15.
A profile transition starts new geometry and dependency epochs and requires an IDR containing SPS and PPS.

User-pinned and cursor minima are never weakened by automatic control.
If those minima prevent cap compliance, the product must display the violation rather than claim compliance.

The native `ProtectedRegionController` consumes cumulative counters and causally ordered feedback, and it emits one canonical JSON decision record for every tick.
The native `MediaPacerQueue` applies the controller's payload budget with a monotonic token bucket whose burst is exactly 100 milliseconds of target bytes.
It admits every access unit atomically and purges the complete dependency epoch plus retransmission cache when the 100-millisecond age bound or hard byte bound is reached.

## Production media path

The patched libdatachannel media chain places the bounded pacer after strict RTP packetization and before the single DTLS-SRTP transport.
New packets and Generic NACK retransmissions therefore share one token budget, and a retransmission reuses the cached plaintext RTP identity through the same SRTP context.
Only packets released by the pacer enter the bounded retransmission cache.
Deferred packets retain their original byte sequence, extended sequence, access-unit identity, dependency epoch, and marker bit.
An epoch purge clears both the pacer and retransmission cache before a new IDR carrying SPS and PPS is admitted.
The public share bitrate selects the initial pacing target, while authenticated REMB applies the frozen effective-cap and control-reserve calculation without lowering the packet-safe 100 kbps V1 floor.
Matching RTCP receiver reports surface fractional loss and round-trip time, and the receiver control channel separately supplies compositor, decoded-frame, and dropped-frame counters.
Peer diagnostics expose the current pacer bounds and target, retransmission bytes, final wire counter, latest REMB, latest loss, and latest round-trip time without substituting configured values for measured values.

## Deterministic replay

Run `glyphrelay_controller_trace_fixture --fixture stable_link --output trace.jsonl` to exercise the production controller path without target hardware.
Run `python tools/replay_controller_trace.py --trace trace.jsonl` to replay the trace from raw cumulative counters and causally consumed feedback.
The replay reconstructs estimator values, state transitions, ordered knob actions, profile epochs, and rounding results before comparing the canonical decision stream byte-for-byte.
The repository integration matrix covers `stable_link`, `emphasis_overshoot`, `stale_remb`, `missing_remb`, `sudden_collapse`, `high_rtt_without_loss`, `recovery`, and `unusable`.
It also proves that action tampering and future-feedback consumption fail rather than producing a passing replay.

## Network qualification

The frozen stable-link matrix uses 0.5, 1, 2, and 4 Mbps caps with 50 milliseconds of round-trip delay and zero configured loss.
Each direction uses a namespace-scoped `netem` qdisc with 25 milliseconds of one-way delay.
The target harness records `tc -V` and the installed package before applying the exact command template in the manifest.
The upstream [`tc-netem` manual](https://github.com/iproute2/iproute2/blob/main/man/man8/tc-netem.8) defines `limit`, `delay`, `rate`, and optional `seed`; a seed is required only when a random impairment is configured.
The steady measurement follows a ten-second warmup and a 120-second segment with one-second windows advanced every 100 milliseconds.

Target qualification is still required for every browser, latency, rate, recovery, and packet-capture acceptance claim.
