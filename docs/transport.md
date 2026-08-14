# Transport ownership, classification, and egress accounting

GlyphRelay owns one media-session UDP socket through the pinned libdatachannel and libjuice stack.

The selected libdatachannel revision is v0.24.1 at commit `a02b751917ac8afc8c58dc6f4461d25ff9465d48`.

The selected libjuice submodule is commit `5948a4162d37bc213d6051b67ee2876ccc5a99a6`.

The repository applies `patches/libdatachannel-v0.24.1/glyphrelay-final-egress.patch`, whose SHA-256 is locked in `dependencies.lock.json`.

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

## Verification

Run `make transport-check` to clone every exact submodule commit, verify and apply the locked patch, build libdatachannel with the frozen flags, and run the patched libjuice loopback test.

The loopback test proves the hook-enabled mux rejection, control classification for generated ICE traffic, direct IPv4 metadata, classified media delivery, and exactly one media hook event.

Run `make check` for the portable classifier, accounting, failure, epoch, control-bypass, and deterministic boundary-race tests.
