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

Run the pinned Linux x86-64 portable compile and test path with:

```bash
make linux-cpu-check
```

CUDA, NVENC, XDG portal, PipeWire, browser, network, and performance acceptance remains deferred to the consolidated target qualification workflow required by the build plan.

## License

Original GlyphRelay source is available under the MIT License.

Pinned and patched third-party files retain their upstream licenses and notices.
