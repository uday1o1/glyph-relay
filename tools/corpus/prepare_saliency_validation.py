from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO

from tools.corpus.prepare_saliency_development import (
    FRAME_BYTES,
    FRAME_COUNT,
    HEIGHT,
    MACROBLOCK_COUNT,
    STRATA,
    WIDTH,
    decode_bgra,
    truth_masks,
)

ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "corpus" / "manifests" / "validation.json"
BUNDLE_MAGIC = b"GLYPH_SAL_VAL1\0\0"
BUNDLE_VERSION = 1
SEQUENCE_ID = re.compile(r"[a-z0-9_-]{1,96}")


class ValidationPreparationError(RuntimeError):
    """Raised when immutable validation input cannot be prepared safely."""


@dataclass(frozen=True)
class PreparedValidationBundle:
    path: Path
    metadata_path: Path
    sha256: str
    bytes: int
    resumed: bool


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValidationPreparationError(f"json_object_required:{path.name}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def exact_sha256(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValidationPreparationError(f"{label}_invalid")
    return value


def validate_render_index(
    renderer_directory: Path, manifest: dict[str, Any]
) -> tuple[dict[tuple[str, int], dict[str, Any]], str]:
    index_path = renderer_directory / "render-index.json"
    index = load_object(index_path)
    if (
        index.get("protocol") != "corpus_protocol_v1"
        or index.get("split") != "validation"
        or index.get("browserVersion") != "151.0.7922.34"
    ):
        raise ValidationPreparationError("validation_render_index_contract_invalid")
    raw_frames = index.get("frames")
    if not isinstance(raw_frames, list):
        raise ValidationPreparationError("validation_render_frames_invalid")
    indexed: dict[tuple[str, int], dict[str, Any]] = {}
    for raw in raw_frames:
        if not isinstance(raw, dict):
            raise ValidationPreparationError("validation_render_frame_invalid")
        sequence_id = raw.get("sequenceId")
        frame_id = raw.get("frameId")
        if (
            not isinstance(sequence_id, str)
            or not SEQUENCE_ID.fullmatch(sequence_id)
            or not isinstance(frame_id, int)
            or isinstance(frame_id, bool)
            or not isinstance(raw.get("frameSha256"), str)
            or not isinstance(raw.get("sourcePtsNs"), int)
            or not isinstance(raw.get("ocrInputs"), dict)
        ):
            raise ValidationPreparationError("validation_render_frame_invalid")
        key = (sequence_id, frame_id)
        if key in indexed:
            raise ValidationPreparationError("validation_render_frame_duplicate")
        indexed[key] = raw
    expected = {
        (sequence["sequenceId"], frame["frameId"])
        for sequence in manifest["sequences"]
        for frame in sequence["sampleFrames"]
    }
    if set(indexed) != expected or len(indexed) != 256:
        raise ValidationPreparationError("validation_render_coverage_invalid")
    for sequence in manifest["sequences"]:
        sequence_id = sequence["sequenceId"]
        for frame in sequence["sampleFrames"]:
            entry = indexed[(sequence_id, frame["frameId"])]
            expected_regions = {region["id"] for region in frame["textRegions"]}
            if set(entry["ocrInputs"]) != expected_regions:
                raise ValidationPreparationError("validation_ocr_render_coverage_invalid")
            image = renderer_directory / "frames" / sequence_id / f"{frame['frameId']:03d}.png"
            if not image.is_file() or sha256_file(image) != entry["frameSha256"]:
                raise ValidationPreparationError("validation_render_frame_identity_invalid")
            for region_id, expected_sha256 in entry["ocrInputs"].items():
                ocr_image = (
                    renderer_directory
                    / "ocr-inputs"
                    / sequence_id
                    / f"{frame['frameId']:03d}-{region_id}.png"
                )
                if not ocr_image.is_file() or sha256_file(ocr_image) != expected_sha256:
                    raise ValidationPreparationError("validation_ocr_input_identity_invalid")
    return indexed, sha256_file(index_path)


class HashingWriter:
    def __init__(self, stream: BinaryIO) -> None:
        self.stream = stream
        self.digest = hashlib.sha256()
        self.bytes = 0

    def write(self, value: bytes) -> None:
        if self.stream.write(value) != len(value):
            raise ValidationPreparationError("validation_bundle_short_write")
        self.digest.update(value)
        self.bytes += len(value)


def identity(
    *,
    corpus_protocol_sha256: str,
    manifest_sha256: str,
    render_index_sha256: str,
    configuration_sha256: str,
) -> dict[str, Any]:
    return {
        "schemaVersion": 1,
        "protocol": "saliency_validation_v1",
        "width": WIDTH,
        "height": HEIGHT,
        "sequenceCount": 64,
        "frameCount": 256,
        "macroblockCount": MACROBLOCK_COUNT,
        "corpusProtocolSha256": exact_sha256(corpus_protocol_sha256, "corpus_protocol_sha256"),
        "validationManifestSha256": exact_sha256(manifest_sha256, "validation_manifest_sha256"),
        "validationRenderIndexSha256": exact_sha256(
            render_index_sha256, "validation_render_index_sha256"
        ),
        "configurationSha256": exact_sha256(configuration_sha256, "configuration_sha256"),
    }


def _resume(
    output_directory: Path, expected_identity: dict[str, Any]
) -> PreparedValidationBundle | None:
    bundle = output_directory / "validation.bundle"
    metadata_path = output_directory / "validation.bundle.json"
    if not bundle.exists() and not metadata_path.exists():
        return None
    if bundle.is_file() and not metadata_path.exists():
        recovered = {
            **expected_identity,
            "bundleBytes": bundle.stat().st_size,
            "bundleSha256": sha256_file(bundle),
        }
        with metadata_path.open("x", encoding="utf-8") as stream:
            stream.write(canonical_json(recovered).decode())
            stream.flush()
            os.fsync(stream.fileno())
    if not bundle.is_file() or not metadata_path.is_file():
        raise ValidationPreparationError("validation_bundle_checkpoint_incomplete")
    metadata = load_object(metadata_path)
    expected = {
        **expected_identity,
        "bundleBytes": bundle.stat().st_size,
        "bundleSha256": sha256_file(bundle),
    }
    if metadata != expected:
        raise ValidationPreparationError("validation_bundle_checkpoint_identity_invalid")
    return PreparedValidationBundle(
        bundle,
        metadata_path,
        metadata["bundleSha256"],
        metadata["bundleBytes"],
        True,
    )


def prepare_bundle(
    renderer_directory: Path,
    output_directory: Path,
    *,
    corpus_protocol_sha256: str,
    configuration_sha256: str,
    ffmpeg: str = "ffmpeg",
) -> PreparedValidationBundle:
    manifest = load_object(MANIFEST_PATH)
    manifest_sha256 = sha256_file(MANIFEST_PATH)
    indexed, render_index_sha256 = validate_render_index(renderer_directory, manifest)
    expected_identity = identity(
        corpus_protocol_sha256=corpus_protocol_sha256,
        manifest_sha256=manifest_sha256,
        render_index_sha256=render_index_sha256,
        configuration_sha256=configuration_sha256,
    )
    output_directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    resumed = _resume(output_directory, expected_identity)
    if resumed is not None:
        return resumed
    masks = truth_masks(manifest, expected_split="validation")
    temporary = output_directory / f".validation.bundle.{os.getpid()}.tmp"
    bundle = output_directory / "validation.bundle"
    metadata_path = output_directory / "validation.bundle.json"
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
            for name in (
                "corpusProtocolSha256",
                "validationManifestSha256",
                "validationRenderIndexSha256",
                "configurationSha256",
            ):
                writer.write(bytes.fromhex(expected_identity[name]))
            for sequence in manifest["sequences"]:
                sequence_id = sequence["sequenceId"]
                encoded_id = sequence_id.encode("ascii")
                stratum = sequence.get("stratum")
                if stratum not in STRATA or len(encoded_id) > 96:
                    raise ValidationPreparationError("validation_sequence_contract_invalid")
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
                        raise ValidationPreparationError("validation_frame_timestamp_mismatch")
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
            **expected_identity,
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
    return PreparedValidationBundle(
        bundle,
        metadata_path,
        metadata["bundleSha256"],
        metadata["bundleBytes"],
        False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare immutable saliency validation input")
    parser.add_argument("--renderer-directory", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--corpus-protocol-sha256", required=True)
    parser.add_argument("--configuration-sha256", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    arguments = parser.parse_args()
    try:
        prepared = prepare_bundle(
            arguments.renderer_directory.resolve(),
            arguments.output_directory.resolve(),
            corpus_protocol_sha256=arguments.corpus_protocol_sha256,
            configuration_sha256=arguments.configuration_sha256,
            ffmpeg=arguments.ffmpeg,
        )
    except (OSError, ValidationPreparationError, ValueError) as error:
        print(f"saliency validation preparation failed: {error}")
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
