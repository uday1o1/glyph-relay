# Capture correctness baseline

GlyphRelay uses the XDG ScreenCast portal as the sole authority for selecting one application window.

The frozen V1 request contract selects window sources only, sets `multiple=false`, sets `persist_mode=0`, stores no restore token, and prefers metadata, embedded, then hidden cursor modes.

The portable `PortalSelectionStateMachine` fails closed on stale request handles, invalid response order, zero PipeWire node identifiers, cancellation after streaming, and repeated terminal transitions.

It clears every request, session, and node handle when capture is closed, revoked, or disconnected.

This state machine is the policy boundary consumed by the Linux portal adapter.

It is not evidence that a real portal dialog or PipeWire node succeeded on the current macOS host.

## Shared-memory ownership

The mandatory shared-memory path copies the selected visible crop into one owned packed-RGB slot before invoking the PipeWire-loop requeue callback.

The owned copy normalizes row pitch while preserving the source dimensions, source crop, pixel order, cursor mode, damage rectangles, monotonic dequeue timestamp, and immutable geometry epoch.

Upright and 90-, 180-, and 270-degree source orientations are normalized into source-visible coordinates before downstream use.

The same immutable transform rotates damage rectangles, cursor pixels, and cursor positions with the frame.

Resolution or crop changes increment the geometry epoch and prevent an older concurrent copy from re-entering the ready queue.

Metadata cursor bitmaps are alpha-composited into the owned BGRA or RGBA frame before the source buffer is returned.

The pool has a fixed capacity from one through 64 frames.

Consumption is latest-frame-wins, evicts only unleased ready frames, and drops a new frame with a bounded diagnostic when every slot is leased.

Capture stop closes admission, clears ready frames, permits existing leases to drain, and still requeues every later PipeWire buffer.

Malformed crop, pitch, buffer, damage, or cursor metadata fails closed and still invokes the requeue callback exactly once.

## CPU color conversion

The CPU baseline converts BGRA or RGBA to one contiguous NV12 image with explicit visible dimensions, even coded dimensions, pitch, and chroma offset.

The conversion uses BT.709 coefficients with an explicitly selected limited or full range and deterministic round-to-nearest, ties-to-even arithmetic.

Odd visible edges are clamped before 2-by-2 chroma subsampling, and input row padding is never read as a visible pixel.

The scalar implementation is the correctness reference.

On ARM NEON and x86 SSE2 builds, the SIMD-assisted backend performs bounded packed-pixel loads and channel extraction while retaining the exact scalar code-value arithmetic.

The automatic backend must remain byte-identical to the scalar reference.

Native tests cover hand-calculated black, white, red, green, and blue limited- and full-range values, BGRA and RGBA ordering, padded pitch, odd geometry, every supported orientation, malformed inputs, bounded pool starvation, prompt requeue, cursor composition, portal cancellation and revocation, and 100 deterministic randomized scalar-to-SIMD comparisons.

Run the portable contract with:

```bash
ctest --test-dir build/macos-local --output-on-failure \
  -R '^native\.capture_contracts$'
```

The real GDBus and PipeWire adapter, interactive portal selection, and Linux desktop teardown evidence remain separate target work.
