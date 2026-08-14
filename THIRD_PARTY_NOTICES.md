# Third-party notices

GlyphRelay source code is distributed under the repository's MIT license.

The following entries describe dependencies selected for V1 and the obligations that apply if they are fetched, linked, modified, or redistributed.

This file is not legal advice and does not replace the license text shipped by each dependency.

## NVIDIA NVENC headers

GlyphRelay pins `FFmpeg/nv-codec-headers` at tag `n13.1.15.0` and commit `0a6fba9a2820628b8103464f4c8753ee05838baa`.

The pinned header is distributed under its included MIT license and copyright notice.

Any redistributed copy must preserve that notice.

The repository does not redistribute the NVIDIA Video Codec SDK archive, NVIDIA driver, firmware, or binary codec components.

Use of NVIDIA software remains subject to the terms supplied by NVIDIA.

The local CUDA compile gate pulls the official NVIDIA CUDA 13.3.1 development container by its Linux amd64 manifest digest.

The repository does not redistribute that image or its CUDA toolkit binaries.

## libdatachannel and libjuice

GlyphRelay pins libdatachannel v0.24.1 at commit `a02b751917ac8afc8c58dc6f4461d25ff9465d48` under MPL-2.0.

The bundled libjuice dependency is also MPL-2.0.

GlyphRelay keeps required transport modifications isolated to MPL-covered source files so those modified files can be published with notices and corresponding source as required.

The corresponding modified source is retained as `patches/libdatachannel-v0.24.1/glyphrelay-final-egress.patch`, including the patch-owned loopback test and strict WebSocket Origin support.

The bounded packetization and NACK media handlers in `vendor/libdatachannel-v0.24.1` are separately published under MPL-2.0.

Unmodified bundled dependencies include nlohmann/json under MIT, libsrtp under BSD-3-Clause, plog under MIT, and usrsctp under BSD-3-Clause.

Exact commits and license-file hashes are recorded in `dependencies.lock.json`.

## OpenH264

GlyphRelay uses Ubuntu 24.04 system packages `libopenh264-dev` and `libopenh264-7` at version `2.4.1+dfsg-1` under BSD-2-Clause.

The application dynamically links the system runtime with SONAME `libopenh264.so.7`.

The repository does not redistribute an OpenH264 binary.

Package redistribution, if later introduced, requires a separate codec and distribution review.

## Playwright browsers

Browser interoperability tooling pins `@playwright/test` 1.62.1 and the browser revisions recorded in `dependencies.lock.json`.

Playwright is Apache-2.0.

Downloaded browser binaries retain their own upstream licenses and are qualification inputs, not repository artifacts.

Their binary hashes must be recorded before any browser oracle is frozen.

## coturn

Loopback TURN qualification pins the `coturn/coturn:4.15.0-r0` Linux amd64 image by digest.

coturn is BSD-3-Clause.

The image and its layers are pulled for testing and are not redistributed by this repository.

## ws

The self-hosted signaling bundle uses ws 8.21.3 under the MIT license.

The exact npm package version and integrity are retained in the package and dependency locks.

The TypeScript declarations from @types/ws 8.18.1 are also MIT licensed and are used only during development.

## Diagnostic tools

Ubuntu package versions for FFmpeg, Tesseract OCR, and tshark are recorded in `dependencies.lock.json` for the target qualification environment.

FFmpeg licensing depends on its exact build configuration and linked codecs.

GlyphRelay must capture `ffmpeg -version` and its configuration before using it for public evidence or redistributing any binary.

Tesseract is Apache-2.0 and tshark is GPL-2.0-or-later.

These tools are qualification dependencies and are not linked into or redistributed with the application.
