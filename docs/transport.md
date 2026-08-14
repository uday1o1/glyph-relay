# Transport ownership, classification, and egress accounting

GlyphRelay owns one media-session UDP socket through the pinned libdatachannel and libjuice stack.

The selected libdatachannel revision is v0.24.1 at commit `a02b751917ac8afc8c58dc6f4461d25ff9465d48`.

The selected libjuice submodule is commit `5948a4162d37bc213d6051b67ee2876ccc5a99a6`.

The repository applies `patches/libdatachannel-v0.24.1/glyphrelay-final-egress.patch`, whose SHA-256 is `252f2e30d8cbd62c0bc60620d5621d9e43f3ae7f2e11354b1613c44e9a99117a` and is locked in `dependencies.lock.json`.

The patch is isolated to MPL-2.0-covered libdatachannel and libjuice files and includes its loopback contract test as corresponding source.

The same isolated patch adds an optional exact client `Origin` header to the pinned libdatachannel WebSocket configuration so the native sender can satisfy the signaling server's strict upgrade policy.

An empty Origin or any value containing a carriage return or line feed is rejected before the client handshake is generated.

## Final datagram boundary

The upstream `IceTransport::outgoing` callback is not a final datagram boundary.

It cannot see libjuice-generated STUN traffic, and it observes application bytes before TURN ChannelData or Send Indication wrapping.

The patch therefore carries trusted egress class and protocol metadata through libdatachannel and libjuice to the connection backend.

The hook wraps the exact `udp_sendto` call that immediately invokes the platform `sendto` syscall in the pinned libjuice source.

The hook receives the final UDP payload, its authenticated media or control class, the direct or TURN-over-UDP path, the outer protocol, and the destination IP family.

The hook also receives a synchronous one-shot native-send callback.

The callback and native-send pointer are valid only for the duration of the hook call.

The native-send callback rejects a second invocation so one hook event cannot emit the same final datagram twice.

Generated ICE, STUN, and TURN maintenance messages are classified as control inside libjuice.

Libjuice's zero-payload UDP self-interrupt also crosses the hook as direct authenticated unknown control.
It is counted because it reaches the same final socket-send boundary, even though its destination is the session socket itself.

SRTP and SRTCP classification is assigned only after libdatachannel's DTLS-SRTP transport selects and protects the packet.

DTLS records are classified as control by the DTLS transport after record construction.

TURN ChannelData and Send Indication wrapping preserves the authenticated inner media or control class while replacing the final protocol and path metadata with the outer TURN values.

The ordinary unclassified libjuice send API defaults to control, but GlyphRelay's patched libdatachannel path always calls the classified API.

Hook-enabled sessions force `JUICE_CONCURRENCY_MODE_THREAD` so each media session owns one socket and one gate.

The patch carries libdatachannel's relay-only policy into libjuice and prevents construction of direct local candidate pairs for that agent.
This closes the upstream same-network behavior where filtering signaled candidates alone can still allow the internal host candidate to win.

The patch also resolves numeric ICE candidates without `AI_ADDRCONFIG` in both libdatachannel and libjuice.
That flag incorrectly rejects a literal `::1` candidate in an isolated Linux network namespace where IPv6 loopback is available but no non-loopback IPv6 address exists.

The patched configuration rejects UDP mux, ICE TCP, TURN TCP, and TURN TLS when the final UDP hook is installed.

These transports cannot enter the verified wire-cap state.

## Media egress gate

`MediaEgressGate` serializes epoch validation and the final native UDP send.

A media sender acquires a shared permit, validates that admission is open and its epoch is current, calls the synchronous native send, records a successful complete result, and then releases the permit.

Stop, pause, screen lock, capture revocation, and permission loss acquire the same gate exclusively.

The exclusive acquisition waits for every earlier admitted native send to return, closes media admission, and advances the epoch.

No sender can validate an old epoch before that boundary and start its native send after the boundary.

Pause is the only resumable boundary and resumes in the advanced epoch.

Every other boundary is terminal for that gate instance.

Authenticated control traffic bypasses the media permit and remains available while an exclusive media boundary waits.

The deterministic native test stalls each boundary reason after media validation and before the native send.

It proves the exclusive close cannot complete early and proves a stale sender never reaches the native callback after closure.

## Exact accounting contract

The counter records a datagram only when the native send returns the complete UDP payload length.

A zero-length UDP payload is accepted only for the authenticated libjuice self-interrupt classification.
All zero-length media and every other zero-length control classification fail closed.

A rejection, exception, negative result, or short send adds no wire-egress bytes.

The verified IPv4 total is the final UDP payload length plus 8 UDP header bytes plus 20 base IPv4 header bytes.

The verified IPv6 total is the final UDP payload length plus 8 UDP header bytes plus 40 base IPv6 header bytes.

The counter separates media and control totals and direct and TURN-over-UDP totals.

IPv4 options, IPv6 extension headers, fragmentation, non-UDP transport, missing provenance, inconsistent class, and invalid protocol metadata fail closed before the native send.

The portable tests validate deterministic direct IPv4, direct IPv6, and final TURN payload arithmetic.

The `glyphrelay_m0_transport_fixture` executable creates two real libdatachannel peers on fixed loopback ports and sends one RTP packet through DTLS-SRTP.
It records every successful final-hook payload hash, authenticated class, outer protocol, direct or TURN path, address family, native-send result, and computed IP-layer length.
Direct IPv4 and direct IPv6 fixture runs pass locally on the supported macOS loopback stack.

Target qualification runs the same fixture for direct IPv4, supported direct IPv6, and TURN over the exact digest-pinned coturn image.
The TURN scenario uses the pinned upstream asymmetric topology: the first peer that sends media is relay-only, while the receiving peer is direct.
The relay-only peer completes candidate gathering before its SDP is delivered so an early host candidate cannot win before the relayed candidate exists.
The gate requires the first selected local candidate to be relayed, the second selected local candidate not to be relayed, and the protected media datagram to use final TURN framing.
Tshark captures outbound agent datagrams from the two fixed source ports.
The independent validator parses each UDP payload and IP length from the capture, compares the complete payload-identity multiset against the hook events, and requires both datagram counts and aggregate IP-layer byte totals to be exactly equal.
IPv6 may be reported unsupported only when an actual `::1` UDP bind fails with an address-family or address-availability error.
The raw packet captures remain private qualification artifacts, while the reduced `validation.json` contains only counts, totals, protocols, and support state.

## RTP identity and packetization

The patched `RtpPacketizationConfig::extendedSequenceNumber` is the sole sequence allocator used by the pinned library packetizer.

The library writes its low 16 bits on wire, advances the 64-bit value once for each emitted RTP packet, and exposes both identities on the resulting message.

The patched configuration also carries a 64-bit extended timestamp and writes its low 32 bits on wire.

The GlyphRelay adapter serializes packetization so concurrent callers cannot race either extended value.

It rejects zero epochs, zero access-unit identity, missing extended timestamps, nonincreasing extended timestamps, malformed or empty Annex B, unsupported NAL types, and an IDR that is not preceded by individual SPS and PPS NAL units.

Only validated access units enter the pinned `H264RtpPacketizer` with a 1,200-byte maximum RTP payload.

The library emits SPS and PPS as individual packets, does not emit STAP-A, fragments larger NAL units as ordered FU-A, and marks only the final packet of the access unit.

The portable packetizer is an independent contract oracle for deterministic tests and is not a second production allocator.

## Bounded loss recovery

The built-in `RtcpNackResponder` is not used because it keys only on 16-bit sequence number and cannot enforce the declared byte, age, epoch, retransmission, rate, or clearing limits.

`vendor/libdatachannel-v0.24.1/glyphrelay_media_handlers.cpp` is the separately published MPL-2.0 replacement.

It accepts authenticated RTCP Generic NACK and PLI only for the configured media SSRC after the library has removed SRTCP protection.

PID and BLP identifiers are expanded modulo 16 bits and deduplicated before lookup.

The cache key carries media epoch, dependency epoch, SSRC, and extended sequence, while feedback resolves only a unique active-epoch packet matching the 16-bit wire sequence.

An absent, expired, retransmission-limited, or ambiguous match enters the one coalesced IDR-with-SPS-and-PPS recovery limiter instead of guessing.

The cache hard limits are 500 milliseconds, 2,048 packets, 4 MiB, and two retransmissions per packet.

The feedback hard limits are 100 distinct NACK identifiers and ten recovery messages in any rolling second.

Continuous overload for ten seconds terminates the session through one visible callback.

Epoch reset and session stop synchronously erase the retransmission cache.

An admitted retransmission recreates the exact original plaintext RTP bytes and metadata and re-enters the same libdatachannel DTLS-SRTP path.

The pinned libsrtp outbound policy enables repeated transmission of an existing RTP identity.

Byte-identical protected UDP replay, SRTP rollover, and browser recovery remain empirical target qualification gates and are not inferred from the local plaintext test.

## Native peer service

The native `PeerSender` accepts one compatible 720p30 H.264 receiver offer under the frozen Constrained Baseline Level 3.1 and packetization-mode 1 predicate.

It creates one send-only video track from that offer, retains the selected browser payload type, and rejects extra tracks or incompatible directions.

The browser creates one ordered and reliable `glyphrelay-control-v1` data channel in its offer.

The sender rejects a second channel, an incorrect label or protocol, an unreliable channel, binary control input, and an unexpected control close.

Trickle ICE candidates use a bounded exact JSON envelope, reject duplicate or unknown fields, and are admitted only after the offer installs the remote description.

ICE TCP, UDP mux, TURN TCP, and TURN TLS remain disabled because they cannot enter the verified final-UDP accounting path.

Each access unit retains its source frame, media epoch, dependency epoch, access-unit identity, and extended RTP timestamp through the strict packetizer.

A new dependency epoch is admitted only by an IDR carrying SPS and PPS.

The service chains the existing sender report, bounded NACK and PLI recovery, and REMB handlers on the same track.

Stop closes the media egress gate first, clears the retransmission cache, sends the bounded session-end control message, waits at most two seconds for its exact acknowledgment, and then clears channel, track, peer, and handler ownership.

The browser detaches its last displayed stream on pause, rejects a second data channel, reattaches only the existing authenticated stream on resume, and stops every retained track on terminal cleanup.

## Public share orchestration

The stable `glyphrelay share` command reads its signaling origin from `GLYPHRELAY_SIGNALING_ORIGIN` and never accepts an origin or capture-source override.

An empty or invalid configuration fails before portal selection, capture, encoder initialization, or owner-session creation.

The optional `GLYPHRELAY_SIGNALING_CA_PATH` provides a private certificate authority file to the certificate-verifying WSS client.

The production transport composes the owner-signaling client and one `PeerSender` behind a bounded 32-event application queue.

It forwards the exact answer and trickle candidates through the owner-authenticated signaling connection and declares the receiver ready only after the peer, media track, and reliable control channel are all open.

The application creates no more than one three-access-unit, 8 MiB, 100-millisecond encoded transport queue.

Live-only sharing waits for receiver readiness before capture and OpenH264 initialization.

Combined sharing and recording starts the 720p30 Level 3.1 encoder, durable recorder preparation barrier, and portal capture before creating a remote join link.

The recording and transport branches receive the same immutable encoded access unit after the receiver joins.

Receiver admission, geometry change, queue purge, NACK, and PLI recovery advance the dependency epoch and require an IDR carrying SPS and PPS before transport resumes.

Signaling or peer failure revokes the remote transport immediately, while an explicitly selected healthy recording may continue under its independent durability contract.

Recorder failure remains visible, disables only that branch, and preserves an active remote share.

Stop drains no stale encoded work, closes transport before capture teardown, finalizes an opted-in recorder, and reports capture, recorder, transport, and queue diagnostics without exposing the owner capability.

## Verification

Run `make transport-check` to clone every exact submodule commit, verify and apply the locked patch, build libdatachannel with the frozen flags, compile the real transport fixtures, run the native owner, control, peer, strict packetization, and bounded-recovery integration tests, exercise WS and certificate-verified WSS, and run the patched libjuice loopback test.

The loopback test proves the hook-enabled mux rejection, control classification for generated ICE traffic, direct IPv4 metadata, classified media delivery, and exactly one media hook event.

The peer test negotiates two real loopback DTLS-SRTP peers, exchanges trickle ICE and control traffic, sends one recovery access unit through the final UDP hook, and proves bounded-resource cleanup after the acknowledged session end.

Run `make check` for the portable packetization, rollover, cache, feedback, classifier, accounting, failure, epoch, control-bypass, and deterministic boundary-race tests.

The consolidated designated-target workflow runs packet capture through the `transport-packet-capture` phase:

```bash
./scripts/gpu/qualify_cuda_pm.sh
```

An absent Docker daemon, inaccessible loopback capture interface, unavailable tshark permission, failed coturn start, or incomplete capture is a nonpassing prerequisite or phase result.
