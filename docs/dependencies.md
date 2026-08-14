# Dependency lock and update policy

`dependencies.lock.json` is the machine-readable authority for native media, browser, TURN, and target diagnostic dependencies.

Language package managers and the pinned Linux container retain their native lockfiles and digests.

`make dependency-check` cross-checks those sources so a version cannot drift in only one place.

## Selected native stack

The NVENC API contract is compiled against `nv-codec-headers` 13.1.15.0, API 13.1.

The exact header commit and file hash are locked, and runtime initialization must compare the compiled API against `NvEncodeAPIGetMaxSupportedVersion` before loading the function table.

The official compatibility table requires Linux driver 610 or newer for this header generation.

That driver number is a runtime prerequisite, while 13.1 is the compiled NVENC API version.

libdatachannel v0.24.1 is built statically with its locked bundled dependencies, OpenSSL, media, WebSocket support, and upstream tests enabled.

Examples, GnuTLS, Mbed TLS, libnice, shared libraries, and system substitutions for bundled dependencies are disabled.

The selected ICE implementation is the pinned libjuice submodule in one-thread-per-session mode.

Its final datagram boundary is `deps/libjuice/src/udp.c::udp_sendto`, which calls `sendto` in this revision.

The exact MPL-2.0 source patch and its SHA-256 are locked in `dependencies.lock.json`.

`make transport-check` verifies the patch against a clean exact checkout, builds with the frozen flags, and runs its loopback final-egress test.

The application must not use libdatachannel's built-in NACK responder because it cannot enforce the V1 age, byte, epoch, rate, and extended-sequence limits.

OpenH264 is dynamically linked from the exact Ubuntu 24.04 system packages recorded in the lock.

The amd64 package hashes and `libopenh264.so.7` SONAME were measured in the pinned Ubuntu base image.

The NVENC encoder consumes NV12 and OpenH264 consumes I420 under one semantic 8-bit 4:2:0 colorimetry and range contract.

## Browser inputs

Playwright 1.62.1 selects Chromium revision 1234 at version 151.0.7922.34 and Firefox revision 1538 at version 153.0.

Qualification installs those project-local browsers with `PLAYWRIGHT_BROWSERS_PATH=0`.

The qualification manifest records hashes of the installed executables before their SDP offers or decoded output can freeze `recording_profile_v1` or `browser_oracle_v1`.

The revision lock alone is not accepted as a binary identity.

## TURN and diagnostic inputs

Only the digest-pinned Linux amd64 coturn image may support the V1 TURN-over-UDP accounting claim.

TURN over TCP or TLS remains outside the verified-cap state.

Target qualification installs the exact Ubuntu package versions for FFmpeg, Tesseract OCR, and tshark in the lock and records their package hashes and runtime version output.

## Updating a dependency

Change the machine lock, native constant, package lock, container reference, notices, and relevant patches together.

Run `make check`, `make linux-cpu-check`, and the clean-source verification before committing.

Any change to NVENC, libdatachannel, libjuice, OpenH264, browser revisions, or coturn invalidates its affected Milestone 0 evidence.

It requires rerunning the corresponding compatibility, decode, transport, privacy, accounting, and license gates before an accepted claim can be restored.
