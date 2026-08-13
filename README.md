# GlyphRelay

GlyphRelay is an in-development local-first, text-aware screen-sharing and recording system for constrained networks.

The implementation follows the acceptance gates in [BUILD_PLAN.md](BUILD_PLAN.md).

No GPU, browser interoperability, quality, latency, or bitrate claim is accepted yet.

Current evidence and deferred gates are recorded in [docs/implementation-status.md](docs/implementation-status.md).

## Foundation workflow

The portable foundation requires Node.js 24, Corepack, `uv`, Apple Clang or Clang, and Make.

Project tools and dependencies remain local to the checkout.

```bash
corepack pnpm install --frozen-lockfile
uv sync --locked
make check
```

Run the real environment diagnostic with:

```bash
build/macos-local/glyphrelay doctor
build/macos-local/glyphrelay doctor --json
```

On Linux, the executable is under `build/linux-cpu` for the portable preset.

The command reports unsupported or unverified capabilities instead of fabricating a passing hardware result.

The stable fields, probe semantics, decision modes, and safe-attachment rules are documented in [docs/doctor.md](docs/doctor.md).

The frozen Milestone 0 benchmark input can be verified through the public workflow:

```bash
build/macos-local/glyphrelay benchmark \
  --manifest protocols/m0_fixed_map_v1/manifest.lock \
  --output build/m0-result
```

The command hashes every generated frame and every protocol component before checking hardware.

It exits with unsupported-capability code 3 on a host that cannot run the NVENC comparison and does not create a result directory there.

Run the pinned Linux x86-64 portable compile and test path with:

```bash
make linux-cpu-check
```

CUDA, NVENC, XDG portal, PipeWire, browser, network, and performance acceptance remains deferred to the consolidated target qualification workflow required by the build plan.

## License

Original GlyphRelay source is available under the MIT License.

Pinned and patched third-party files retain their upstream licenses and notices.
