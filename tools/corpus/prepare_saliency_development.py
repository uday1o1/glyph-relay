from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO

ROOT = Path(__file__).resolve().parents[2]
GRID_PATH = ROOT / "corpus" / "saliency-grid-v1.json"
MANIFEST_PATH = ROOT / "corpus" / "manifests" / "development.json"
BUNDLE_MAGIC = b"GLYPH_SAL_DEV1\0\0"
BUNDLE_VERSION = 1
WIDTH = 1920
HEIGHT = 1080
FRAME_COUNT = 4
MACROBLOCK_WIDTH = 120
MACROBLOCK_HEIGHT = 68
MACROBLOCK_COUNT = MACROBLOCK_WIDTH * MACROBLOCK_HEIGHT
FRAME_BYTES = WIDTH * HEIGHT * 4
SEQUENCE_ID = re.compile(r"[a-z0-9_-]{1,96}")
STRATA = (
    "animated_typing_scrolling",
    "browser_documentation",
    "code_editor",
    "mixed_video_text",
    "slide_diagram",
    "spreadsheet_table",
    "terminal",
)


class DevelopmentPreparationError(RuntimeError):
    """Raised when the frozen development input cannot be prepared safely."""


@dataclass(frozen=True)
class PreparedBundle:
    path: Path
    metadata_path: Path
    sha256: str
    bytes: int
    resumed: bool


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DevelopmentPreparationError(f"json_invalid:{path.name}") from error
    if not isinstance(value, dict):
        raise DevelopmentPreparationError(f"json_object_required:{path.name}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def macroblock_mask_from_pixels(pixels: set[tuple[int, int]]) -> bytes:
    mask = bytearray(MACROBLOCK_COUNT)
    for x, y in pixels:
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            mask[(y // 16) * MACROBLOCK_WIDTH + x // 16] = 1
    return bytes(mask)


def mark_rectangle(mask: bytearray, box: object) -> None:
    if (
        not isinstance(box, list)
        or len(box) != 4
        or any(not isinstance(value, int) or isinstance(value, bool) for value in box)
    ):
        raise DevelopmentPreparationError("truth_rectangle_invalid")
    x, y, width, height = box
    if width <= 0 or height <= 0:
        raise DevelopmentPreparationError("truth_rectangle_empty")
    left = max(x, 0)
    top = max(y, 0)
    right = min(x + width, WIDTH)
    bottom = min(y + height, HEIGHT)
    if left >= right or top >= bottom:
        return
    for macroblock_y in range(top // 16, (bottom - 1) // 16 + 1):
        for macroblock_x in range(left // 16, (right - 1) // 16 + 1):
            mask[macroblock_y * MACROBLOCK_WIDTH + macroblock_x] = 1


def glyph_pixels(
    glyph: dict[str, Any], catalog: dict[str, tuple[int, int, bytes]]
) -> set[tuple[int, int]]:
    raster_id = glyph.get("rasterId")
    if not isinstance(raster_id, str) or raster_id not in catalog:
        raise DevelopmentPreparationError("glyph_raster_reference_invalid")
    x = glyph.get("x")
    y = glyph.get("y")
    if not isinstance(x, int) or not isinstance(y, int):
        raise DevelopmentPreparationError("glyph_position_invalid")
    width, height, alpha = catalog[raster_id]
    return {
        (x + local_x, y + local_y)
        for local_y in range(height)
        for local_x in range(width)
        if alpha[local_y * width + local_x] != 0
    }


def truth_masks(
    manifest: dict[str, Any], *, expected_split: str = "development"
) -> dict[tuple[str, int], tuple[bytes, bytes, bytes]]:
    if manifest.get("split") != expected_split or manifest.get("protocol") != "corpus_protocol_v1":
        raise DevelopmentPreparationError(f"{expected_split}_manifest_identity_invalid")
    raw_catalog = manifest.get("glyphRasterCatalog")
    if not isinstance(raw_catalog, list):
        raise DevelopmentPreparationError("glyph_catalog_invalid")
    catalog: dict[str, tuple[int, int, bytes]] = {}
    for raw in raw_catalog:
        if not isinstance(raw, dict):
            raise DevelopmentPreparationError("glyph_catalog_entry_invalid")
        raster_id = raw.get("id")
        width = raw.get("width")
        height = raw.get("height")
        encoded = raw.get("alphaBase64")
        expected_sha256 = raw.get("alphaSha256")
        if (
            not isinstance(raster_id, str)
            or raster_id in catalog
            or not isinstance(width, int)
            or not isinstance(height, int)
            or width <= 0
            or height <= 0
            or not isinstance(encoded, str)
            or not isinstance(expected_sha256, str)
        ):
            raise DevelopmentPreparationError("glyph_catalog_entry_invalid")
        try:
            alpha = base64.b64decode(encoded, validate=True)
        except ValueError as error:
            raise DevelopmentPreparationError("glyph_alpha_invalid") from error
        if len(alpha) != width * height or hashlib.sha256(alpha).hexdigest() != expected_sha256:
            raise DevelopmentPreparationError("glyph_alpha_identity_invalid")
        catalog[raster_id] = (width, height, alpha)

    masks: dict[tuple[str, int], tuple[bytes, bytes, bytes]] = {}
    sequences = manifest.get("sequences")
    if not isinstance(sequences, list):
        raise DevelopmentPreparationError("development_sequences_invalid")
    for sequence in sequences:
        if not isinstance(sequence, dict):
            raise DevelopmentPreparationError("development_sequence_invalid")
        sequence_id = sequence.get("sequenceId")
        if not isinstance(sequence_id, str) or not SEQUENCE_ID.fullmatch(sequence_id):
            raise DevelopmentPreparationError("development_sequence_id_invalid")
        frames = sequence.get("sampleFrames")
        if not isinstance(frames, list) or len(frames) != FRAME_COUNT:
            raise DevelopmentPreparationError("development_sample_frames_invalid")
        for frame in frames:
            if not isinstance(frame, dict) or not isinstance(frame.get("frameId"), int):
                raise DevelopmentPreparationError("development_sample_frame_invalid")
            glyph_mask = bytearray(MACROBLOCK_COUNT)
            small_mask = bytearray(MACROBLOCK_COUNT)
            ui_mask = bytearray(MACROBLOCK_COUNT)
            regions = frame.get("textRegions")
            if not isinstance(regions, list):
                raise DevelopmentPreparationError("development_text_regions_invalid")
            for region in regions:
                if not isinstance(region, dict) or not isinstance(region.get("glyphs"), list):
                    raise DevelopmentPreparationError("development_text_region_invalid")
                for raw_glyph in region["glyphs"]:
                    if not isinstance(raw_glyph, dict):
                        raise DevelopmentPreparationError("development_glyph_invalid")
                    pixels = glyph_pixels(raw_glyph, catalog)
                    rendered = macroblock_mask_from_pixels(pixels)
                    for index, value in enumerate(rendered):
                        if value:
                            glyph_mask[index] = 1
                            if raw_glyph.get("smallGlyphSubset") is True:
                                small_mask[index] = 1
            primitives = frame.get("uiPrimitives")
            if not isinstance(primitives, list):
                raise DevelopmentPreparationError("development_ui_primitives_invalid")
            for primitive in primitives:
                if not isinstance(primitive, dict):
                    raise DevelopmentPreparationError("development_ui_primitive_invalid")
                mark_rectangle(ui_mask, primitive.get("boundingBox"))
            key = (sequence_id, frame["frameId"])
            if key in masks:
                raise DevelopmentPreparationError("development_truth_key_duplicate")
            masks[key] = (bytes(glyph_mask), bytes(small_mask), bytes(ui_mask))
    return masks


def validate_render_index(
    renderer_directory: Path, manifest: dict[str, Any], grid: dict[str, Any]
) -> dict[tuple[str, int], dict[str, Any]]:
    index_path = renderer_directory / "render-index.json"
    if not index_path.is_file() or sha256_file(index_path) != grid.get(
        "developmentRenderIndexSha256"
    ):
        raise DevelopmentPreparationError("development_render_index_identity_invalid")
    index = load_object(index_path)
    if (
        index.get("protocol") != "corpus_protocol_v1"
        or index.get("split") != "development"
        or index.get("browserVersion") != "151.0.7922.34"
    ):
        raise DevelopmentPreparationError("development_render_index_contract_invalid")
    raw_frames = index.get("frames")
    if not isinstance(raw_frames, list):
        raise DevelopmentPreparationError("development_render_frames_invalid")
    indexed: dict[tuple[str, int], dict[str, Any]] = {}
    for raw in raw_frames:
        if not isinstance(raw, dict):
            raise DevelopmentPreparationError("development_render_frame_invalid")
        sequence_id = raw.get("sequenceId")
        frame_id = raw.get("frameId")
        if (
            not isinstance(sequence_id, str)
            or not SEQUENCE_ID.fullmatch(sequence_id)
            or not isinstance(frame_id, int)
            or isinstance(frame_id, bool)
            or not isinstance(raw.get("frameSha256"), str)
            or not isinstance(raw.get("sourcePtsNs"), int)
        ):
            raise DevelopmentPreparationError("development_render_frame_invalid")
        key = (sequence_id, frame_id)
        if key in indexed:
            raise DevelopmentPreparationError("development_render_frame_duplicate")
        indexed[key] = raw
    expected = {
        (sequence["sequenceId"], frame["frameId"])
        for sequence in manifest["sequences"]
        for frame in sequence["sampleFrames"]
    }
    if set(indexed) != expected or len(indexed) != 256:
        raise DevelopmentPreparationError("development_render_coverage_invalid")
    return indexed


class HashingWriter:
    def __init__(self, stream: BinaryIO) -> None:
        self.stream = stream
        self.digest = hashlib.sha256()
        self.bytes = 0

    def write(self, value: bytes) -> None:
        if self.stream.write(value) != len(value):
            raise DevelopmentPreparationError("development_bundle_short_write")
        self.digest.update(value)
        self.bytes += len(value)


def decode_bgra(ffmpeg: str, image: Path, expected_sha256: str) -> bytes:
    encoded = image.read_bytes()
    if hashlib.sha256(encoded).hexdigest() != expected_sha256:
        raise DevelopmentPreparationError("development_frame_identity_invalid")
    completed = subprocess.run(
        [
            ffmpeg,
            "-v",
            "error",
            "-i",
            str(image),
            "-frames:v",
            "1",
            "-pix_fmt",
            "bgra",
            "-f",
            "rawvideo",
            "pipe:1",
        ],
        check=False,
        capture_output=True,
    )
    if completed.returncode != 0 or completed.stderr or len(completed.stdout) != FRAME_BYTES:
        raise DevelopmentPreparationError("development_frame_decode_failed")
    return completed.stdout


def metadata_identity(grid: dict[str, Any]) -> dict[str, Any]:
    return {
        "schemaVersion": 1,
        "protocol": "saliency_v1",
        "width": WIDTH,
        "height": HEIGHT,
        "sequenceCount": 64,
        "frameCount": 256,
        "macroblockCount": MACROBLOCK_COUNT,
        "corpusProtocolSha256": grid["corpusProtocolSha256"],
        "developmentManifestSha256": grid["developmentManifestSha256"],
        "developmentRenderIndexSha256": grid["developmentRenderIndexSha256"],
        "gridSha256": sha256_file(GRID_PATH),
    }


def resume_bundle(output_directory: Path, identity: dict[str, Any]) -> PreparedBundle | None:
    bundle = output_directory / "development.bundle"
    metadata_path = output_directory / "development.bundle.json"
    bundle_exists = bundle.is_file()
    metadata_exists = metadata_path.is_file()
    if not bundle.exists() and not metadata_path.exists():
        return None
    if bundle_exists and not metadata_path.exists():
        recovered = {
            **identity,
            "bundleBytes": bundle.stat().st_size,
            "bundleSha256": sha256_file(bundle),
        }
        with metadata_path.open("x", encoding="utf-8") as stream:
            stream.write(canonical_json(recovered).decode())
            stream.flush()
            os.fsync(stream.fileno())
        directory = os.open(output_directory, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        metadata_exists = True
    if not bundle_exists or not metadata_exists:
        raise DevelopmentPreparationError("development_bundle_checkpoint_incomplete")
    metadata = load_object(metadata_path)
    expected = {
        **identity,
        "bundleBytes": bundle.stat().st_size,
        "bundleSha256": sha256_file(bundle),
    }
    if metadata != expected:
        raise DevelopmentPreparationError("development_bundle_checkpoint_identity_invalid")
    return PreparedBundle(
        path=bundle,
        metadata_path=metadata_path,
        sha256=metadata["bundleSha256"],
        bytes=metadata["bundleBytes"],
        resumed=True,
    )


def prepare_bundle(
    renderer_directory: Path,
    output_directory: Path,
    *,
    ffmpeg: str = "ffmpeg",
) -> PreparedBundle:
    for split in ("validation", "final_test"):
        if (ROOT / "corpus" / "generated" / split).exists():
            raise DevelopmentPreparationError(f"forbidden_renderer_output_open:{split}")
    grid = load_object(GRID_PATH)
    manifest = load_object(MANIFEST_PATH)
    if sha256_file(MANIFEST_PATH) != grid.get("developmentManifestSha256"):
        raise DevelopmentPreparationError("development_manifest_identity_invalid")
    identity = metadata_identity(grid)
    output_directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    resumed = resume_bundle(output_directory, identity)
    if resumed is not None:
        return resumed
    indexed = validate_render_index(renderer_directory, manifest, grid)
    masks = truth_masks(manifest)
    temporary = output_directory / f".development.bundle.{os.getpid()}.tmp"
    bundle = output_directory / "development.bundle"
    metadata_path = output_directory / "development.bundle.json"
    if temporary.exists() or bundle.exists() or metadata_path.exists():
        raise DevelopmentPreparationError("development_bundle_output_exists")
    try:
        with temporary.open("xb") as raw_stream:
            writer = HashingWriter(raw_stream)
            writer.write(BUNDLE_MAGIC)
            writer.write(
                struct.pack(
                    "<7I",
                    BUNDLE_VERSION,
                    WIDTH,
                    HEIGHT,
                    len(manifest["sequences"]),
                    FRAME_COUNT,
                    MACROBLOCK_COUNT,
                    FRAME_BYTES,
                )
            )
            writer.write(bytes.fromhex(identity["corpusProtocolSha256"]))
            writer.write(bytes.fromhex(identity["developmentManifestSha256"]))
            writer.write(bytes.fromhex(identity["developmentRenderIndexSha256"]))
            writer.write(bytes.fromhex(identity["gridSha256"]))
            for sequence in manifest["sequences"]:
                sequence_id = sequence["sequenceId"]
                encoded_id = sequence_id.encode("ascii")
                stratum = sequence.get("stratum")
                if stratum not in STRATA or len(encoded_id) > 96:
                    raise DevelopmentPreparationError("development_sequence_contract_invalid")
                writer.write(
                    struct.pack(
                        "<HBB",
                        len(encoded_id),
                        STRATA.index(stratum),
                        int(sequence.get("motionCategory") == "static"),
                    )
                )
                writer.write(encoded_id)
                for frame in sequence["sampleFrames"]:
                    key = (sequence_id, frame["frameId"])
                    indexed_frame = indexed[key]
                    if indexed_frame["sourcePtsNs"] != frame["sourcePtsNs"]:
                        raise DevelopmentPreparationError("development_frame_timestamp_mismatch")
                    writer.write(struct.pack("<QQ", frame["frameId"], frame["sourcePtsNs"]))
                    writer.write(bytes.fromhex(indexed_frame["frameSha256"]))
                    for mask in masks[key]:
                        writer.write(mask)
                    image = (
                        renderer_directory / "frames" / sequence_id / f"{frame['frameId']:03d}.png"
                    )
                    writer.write(decode_bgra(ffmpeg, image, indexed_frame["frameSha256"]))
            raw_stream.flush()
            os.fsync(raw_stream.fileno())
        temporary.replace(bundle)
        directory = os.open(output_directory, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        metadata = {
            **identity,
            "bundleBytes": writer.bytes,
            "bundleSha256": writer.digest.hexdigest(),
        }
        with metadata_path.open("x", encoding="utf-8") as stream:
            stream.write(canonical_json(metadata).decode())
            stream.flush()
            os.fsync(stream.fileno())
        directory = os.open(output_directory, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return PreparedBundle(
        path=bundle,
        metadata_path=metadata_path,
        sha256=metadata["bundleSha256"],
        bytes=metadata["bundleBytes"],
        resumed=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare frozen saliency development input")
    parser.add_argument("--renderer-directory", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    arguments = parser.parse_args()
    try:
        prepared = prepare_bundle(
            arguments.renderer_directory.resolve(),
            arguments.output_directory.resolve(),
            ffmpeg=arguments.ffmpeg,
        )
    except (DevelopmentPreparationError, OSError) as error:
        print(f"saliency development preparation failed: {error}")
        return 1
    print(
        json.dumps(
            {
                "bundleBytes": prepared.bytes,
                "bundleSha256": prepared.sha256,
                "resumed": prepared.resumed,
                "status": "PASSED",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
