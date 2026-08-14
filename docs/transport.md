# Transport ownership, classification, and egress accounting

GlyphRelay owns one media-session UDP socket through the pinned libdatachannel and libjuice stack.

The selected libdatachannel revision is v0.24.1 at commit `a02b751917ac8afc8c58dc6f4461d25ff9465d48`.

The selected libjuice submodule is commit `5948a4162d37bc213d6051b67ee2876ccc5a99a6`.

The repository applies `patches/libdatachannel-v0.24.1/glyphrelay-final-egress.patch`, whose SHA-256 is `5679ddf6757fa58c1d4850c97a15349ec5e1dd5b9560138e70fe26c7d7af4731` and is locked in `dependencies.lock.json`.

The patch is isolated to MPL-2.0-covered libdatachannel and libjuice files and includes its loopback contract test as corresponding source.

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

SRTP and SRTCP classification is assigned only after libdatachannel's DTLS-SRTP transport selects and protects the packet.

DTLS records are classified as control by the DTLS transport after record construction.

TURN ChannelData and Send Indication wrapping preserves the authenticated inner media or control class while replacing the final protocol and path metadata with the outer TURN values.

The ordinary unclassified libjuice send API defaults to control, but GlyphRelay's patched libdatachannel path always calls the classified API.

Hook-enabled sessions force `JUICE_CONCURRENCY_MODE_THREAD` so each media session owns one socket and one gate.

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

A rejection, exception, negative result, or short send adds no wire-egress bytes.

The verified IPv4 total is the final UDP payload length plus 8 UDP header bytes plus 20 base IPv4 header bytes.

The verified IPv6 total is the final UDP payload length plus 8 UDP header bytes plus 40 base IPv6 header bytes.

The counter separates media and control totals and direct and TURN-over-UDP totals.

IPv4 options, IPv6 extension headers, fragmentation, non-UDP transport, missing provenance, inconsistent class, and invalid protocol metadata fail closed before the native send.

The local tests validate deterministic direct IPv4, direct IPv6, and synthetic final TURN payload arithmetic.

Packet-capture equality for direct IPv4, supported IPv6, and loopback coturn remains a target qualification gate and is not claimed by the local arithmetic tests.

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

## Verification

Run `make transport-check` to clone every exact submodule commit, verify and apply the locked patch, build libdatachannel with the frozen flags, run the strict packetization and bounded-recovery integration test, and run the patched libjuice loopback test.

The loopback test proves the hook-enabled mux rejection, control classification for generated ICE traffic, direct IPv4 metadata, classified media delivery, and exactly one media hook event.

Run `make check` for the portable packetization, rollover, cache, feedback, classifier, accounting, failure, epoch, control-bypass, and deterministic boundary-race tests.
