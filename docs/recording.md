# Durable recording storage

GlyphRelay records an Annex B H.264 elementary stream together with a JSON access-unit sidecar and a completion marker.

The recorder is available only on Linux because its safety contract requires `openat`, `fdatasync`, directory `fsync`, and `renameat2(RENAME_NOREPLACE)`.

Other platforms fail closed instead of emulating weaker publication semantics.

## Output set

A requested `example.h264` recording owns these deterministic final names:

- `example.h264` contains the elementary stream.
- `example.h264.json` contains the access-unit identities, epochs, byte ranges, timestamps, and payload hashes.
- `example.h264.complete` commits the final media and sidecar sizes and SHA-256 hashes.
- `example.h264.journal` reserves the base path and anchors recovery until the completion marker is durable.

Temporary media, sidecar, and marker files use unpredictable sibling names bound into the checksummed journal header.

All files are created relative to one already validated destination-directory descriptor with exclusive creation, no symbolic-link following, close-on-exec, and mode `0600`.

An existing final name, journal, temporary companion, or symbolic link is never replaced.

## Admission and queue bounds

Initialization creates and synchronizes the journal and all empty temporary companions before synchronizing their parent directory.

No access unit is admitted until that prepared barrier succeeds.

The first accepted access unit, and every later IDR, must begin with SPS, PPS, and IDR NAL units whose parsed contents agree with the supplied metadata.

The writer queue is nonblocking and is bounded by at most two seconds of sender time, 64 MiB of encoded bytes, and 4,096 access units.

A deployment can select smaller time and byte limits, but cannot raise those ceilings.

Queue overload fails the recording branch without blocking a capture, encoder, or transport worker.

## Durable group commits

Each group first appends complete media access units and synchronizes the media file.

It then appends the corresponding checksummed journal records and commit record before synchronizing the journal.

The sidecar is staged and synchronized incrementally so recorder memory does not grow with recording duration.

A group is committed immediately for an IDR, an encoder-configuration epoch change, stop, or a recording error with a writable pending group.

Ordinary groups are committed at least every 250 milliseconds of sender monotonic time, with a wall-clock guard as an independent backstop.

A pending group older than one second fails the recorder rather than widening the durability window.

## Publication and inspection

Clean finalization synchronizes the remaining group and sidecar, publishes media and sidecar with no-replace renames, and synchronizes the parent directory.

It then synchronizes and publishes the completion marker, synchronizes the directory again, and removes the now redundant journal only after one final directory synchronization.

The completion marker is the only commit record for the multi-file output.

A final-name media or sidecar without a valid marker remains incomplete.

Inspection validates completed companion hashes directly from the marker.

For an incomplete artifact, inspection trusts names only after validating the deterministic journal's checksummed prepared header.

It parses the journal as a bounded stream, validates every committed byte range and access-unit payload hash against the media file, and ignores an uncommitted torn tail.

Long incomplete recordings therefore do not require loading the journal or sidecar into memory.

## Verification

The Linux contract test records real system-OpenH264 access units, exercises every declared persistence event in a child process, validates complete and incomplete states, checks no-clobber behavior, and independently decodes the completed stream with FFmpeg.

The independent filesystem model discards unsynchronized file contents and directory entries after every protocol operation.

Its seeded journal-before-media, admission-before-preparation, and marker-before-publication defects must fail for their intended ordering invariant.

Run the complete portable Linux verification with:

```bash
make linux-cpu-check
```

Run only the independent filesystem model with:

```bash
uv run pytest -q tests/python/test_recording_crash_model.py
```
