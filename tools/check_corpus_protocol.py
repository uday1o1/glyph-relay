from __future__ import annotations

import base64
import hashlib
import json
import math
from collections import Counter
from pathlib import Path
from typing import Any

import jsonschema

ROOT = Path(__file__).resolve().parents[1]
CORE_STRATA = {
    "code_editor",
    "terminal",
    "spreadsheet_table",
    "slide_diagram",
    "browser_documentation",
    "mixed_video_text",
    "animated_typing_scrolling",
}
SAMPLE_FRAMES = [0, 60, 120, 180]


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object: {path}")
    return value


def validate_manifest(manifest: dict[str, Any], expected_split: str) -> list[str]:
    errors: list[str] = []
    if manifest.get("protocol") != "corpus_protocol_v1":
        errors.append("protocol identity changed")
    if manifest.get("schemaVersion") != 1 or manifest.get("split") != expected_split:
        errors.append(f"{expected_split} manifest identity changed")
    if manifest.get("rendererOutputOpened") is not False:
        errors.append(f"{expected_split} renderer output must remain unopened")
    frame_contract = manifest.get("frameContract", {})
    expected_frame = {
        "durationSeconds": 8,
        "frameCount": 240,
        "frameIntervalNs": 33_333_333,
        "frameRate": 30,
        "geometryEpoch": 1,
        "height": 1080,
        "sampleFrames": SAMPLE_FRAMES,
        "width": 1920,
    }
    if frame_contract != expected_frame:
        errors.append(f"{expected_split} frame contract changed")

    raw_catalog = manifest.get("glyphRasterCatalog", [])
    catalog: dict[str, dict[str, Any]] = {}
    if not isinstance(raw_catalog, list):
        errors.append(f"{expected_split} glyph catalog must be an array")
        raw_catalog = []
    for raw in raw_catalog:
        if not isinstance(raw, dict) or not isinstance(raw.get("id"), str):
            errors.append(f"{expected_split} glyph catalog entry invalid")
            continue
        glyph_id = raw["id"]
        if glyph_id in catalog:
            errors.append(f"{expected_split} duplicate glyph raster {glyph_id}")
            continue
        catalog[glyph_id] = raw
        try:
            alpha = base64.b64decode(raw["alphaBase64"], validate=True)
            if len(alpha) != raw["width"] * raw["height"]:
                errors.append(f"{expected_split} glyph raster size mismatch {glyph_id}")
            if sha256_bytes(alpha) != raw["alphaSha256"]:
                errors.append(f"{expected_split} glyph raster hash mismatch {glyph_id}")
        except (KeyError, TypeError, ValueError):
            errors.append(f"{expected_split} glyph raster encoding invalid {glyph_id}")

    raw_sequences = manifest.get("sequences", [])
    if not isinstance(raw_sequences, list):
        errors.append(f"{expected_split} sequences must be an array")
        raw_sequences = []
    sequence_ids: set[str] = set()
    seeds: set[str] = set()
    strata: Counter[str] = Counter()
    character_instances = 0
    small_instances = 0
    sampled_frames = 0
    scale_cases: set[int] = set()
    for raw_sequence in raw_sequences:
        if not isinstance(raw_sequence, dict):
            errors.append(f"{expected_split} sequence entry invalid")
            continue
        sequence_id = raw_sequence.get("sequenceId")
        seed = raw_sequence.get("seed")
        stratum = raw_sequence.get("stratum")
        if not isinstance(sequence_id, str) or sequence_id in sequence_ids:
            errors.append(f"{expected_split} sequence identity duplicated or invalid")
        else:
            sequence_ids.add(sequence_id)
        if not isinstance(seed, str) or len(seed) != 64 or seed in seeds:
            errors.append(f"{expected_split} sequence seed duplicated or invalid")
        else:
            seeds.add(seed)
        if not isinstance(stratum, str):
            errors.append(f"{expected_split} sequence stratum invalid")
            continue
        strata[stratum] += 1
        if raw_sequence.get("durationFrames") != 240:
            errors.append(f"{expected_split} sequence duration changed")
        scale = raw_sequence.get("logicalDeviceScale")
        if scale not in (1, 2):
            errors.append(f"{expected_split} logical device scale invalid")
        else:
            scale_cases.add(scale)
        frames = raw_sequence.get("sampleFrames")
        if (
            not isinstance(frames, list)
            or [frame.get("frameId") for frame in frames] != SAMPLE_FRAMES
        ):
            errors.append(f"{expected_split} sample frame identities changed")
            continue
        sampled_frames += len(frames)
        sequence_characters = 0
        prior_signature: tuple[object, ...] | None = None
        for frame_index, frame in enumerate(frames):
            if not isinstance(frame, dict):
                errors.append(f"{expected_split} sample frame invalid")
                continue
            if frame.get("sourcePtsNs") != frame["frameId"] * 33_333_333:
                errors.append(f"{expected_split} sample timestamp changed")
            if frame.get("geometryEpoch") != 1:
                errors.append(f"{expected_split} geometry epoch changed")
            regions = frame.get("textRegions")
            if not isinstance(regions, list) or len(regions) != 2:
                errors.append(f"{expected_split} text region contract changed")
                continue
            signature = tuple(
                (region.get("truth"), tuple(region.get("boundingBox", []))) for region in regions
            )
            motion = raw_sequence.get("motionCategory")
            if motion == "static" and prior_signature is not None and signature != prior_signature:
                errors.append(f"{expected_split} static sequence changed between samples")
            if motion == "slow" and frame_index % 2 == 1 and signature != prior_signature:
                errors.append(f"{expected_split} slow sequence changed before two samples")
            prior_signature = signature
            for region in regions:
                if not isinstance(region, dict) or not isinstance(region.get("truth"), str):
                    errors.append(f"{expected_split} text region invalid")
                    continue
                glyphs = region.get("glyphs")
                if not isinstance(glyphs, list):
                    errors.append(f"{expected_split} glyph instances invalid")
                    continue
                character_instances += len(glyphs)
                sequence_characters += len(glyphs)
                for glyph in glyphs:
                    if not isinstance(glyph, dict) or glyph.get("rasterId") not in catalog:
                        errors.append(f"{expected_split} glyph reference invalid")
                        continue
                    raster = catalog[glyph["rasterId"]]
                    small = glyph.get("smallGlyphSubset") is True
                    small_instances += int(small)
                    if small and raster.get("height") not in (8, 9, 10):
                        errors.append(f"{expected_split} small glyph height invalid")
                    x = glyph.get("x")
                    y = glyph.get("y")
                    if (
                        not isinstance(x, int)
                        or not isinstance(y, int)
                        or x < 0
                        or y < 0
                        or x + raster.get("width", 0) > 1920
                        or y + raster.get("height", 0) > 1080
                    ):
                        errors.append(f"{expected_split} glyph lies outside visible frame")
        if sequence_characters < 50:
            errors.append(f"{expected_split} sequence visible truth floor failed")

    if len(raw_sequences) < 64:
        errors.append(f"{expected_split} has fewer than 64 sequences")
    if set(strata) != CORE_STRATA or any(strata[stratum] < 8 for stratum in CORE_STRATA):
        errors.append(f"{expected_split} stratum floor failed")
    if scale_cases != {1, 2}:
        errors.append(f"{expected_split} high-DPI scale cases missing")
    if character_instances < 20_000 or small_instances < 5_000:
        errors.append(f"{expected_split} character floor failed")
    counts = manifest.get("counts")
    expected_counts = {
        "characterInstances": character_instances,
        "sampledFrames": sampled_frames,
        "sequences": len(raw_sequences),
        "smallGlyphInstances": small_instances,
    }
    if counts != expected_counts:
        errors.append(f"{expected_split} stored counts do not match truth")
    weights = manifest.get("splitContract", {}).get("stratumWeights", {})
    if set(weights) != CORE_STRATA or not math.isclose(
        sum(weights.values()), 1.0, rel_tol=0, abs_tol=1e-12
    ):
        errors.append(f"{expected_split} stratum weights invalid")
    return errors


def validate_protocol(root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    lock_path = root / "protocols" / "corpus_protocol_v1" / "manifest.lock"
    if not lock_path.is_file():
        return ["corpus protocol manifest lock is missing"]
    lock = load_object(lock_path)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "corpus_protocol_v1":
        errors.append("corpus protocol lock identity changed")
    files = lock.get("files")
    if not isinstance(files, list):
        return errors + ["corpus protocol file list is invalid"]
    protocol_material = bytearray()
    for entry in files:
        if not isinstance(entry, dict):
            errors.append("corpus protocol file entry is invalid")
            continue
        path = entry.get("path")
        expected = entry.get("sha256")
        if not isinstance(path, str) or not isinstance(expected, str):
            errors.append("corpus protocol file lock is incomplete")
            continue
        source = root / path
        if not source.is_file():
            errors.append(f"corpus protocol file is missing: {path}")
            continue
        actual = sha256_bytes(source.read_bytes())
        if actual != expected:
            errors.append(f"corpus protocol hash mismatch: {path}")
        protocol_material.extend(f"{path}\0{expected}\n".encode())
    if sha256_bytes(bytes(protocol_material)) != lock.get("protocol_sha256"):
        errors.append("corpus protocol aggregate hash mismatch")

    schema = load_object(root / "schemas" / "corpus-manifest-v1.schema.json")
    development = load_object(root / "corpus" / "manifests" / "development.json")
    validation = load_object(root / "corpus" / "manifests" / "validation.json")
    for name, manifest in [("development", development), ("validation", validation)]:
        try:
            jsonschema.validate(manifest, schema)
        except jsonschema.ValidationError as error:
            errors.append(f"{name} schema validation failed: {error.message}")
        errors.extend(validate_manifest(manifest, name))
    development_ids = {sequence["sequenceId"] for sequence in development["sequences"]}
    validation_ids = {sequence["sequenceId"] for sequence in validation["sequences"]}
    development_seeds = {sequence["seed"] for sequence in development["sequences"]}
    validation_seeds = {sequence["seed"] for sequence in validation["sequences"]}
    if development_ids & validation_ids or development_seeds & validation_seeds:
        errors.append("development and validation sequence assignments overlap")
    for field in ["fontId", "layoutIds", "themeIds"]:
        left = development["splitContract"][field]
        right = validation["splitContract"][field]
        left_values = {left} if isinstance(left, str) else set(left)
        right_values = {right} if isinstance(right, str) else set(right)
        if left_values & right_values:
            errors.append(f"development and validation {field} overlap")

    final_pool = load_object(root / "corpus" / "final-test-pool.json")
    final_sequence_count = final_pool.get("sequenceCount")
    final_strata = final_pool.get("strata")
    if (
        final_pool.get("concreteSeedsExist") is not False
        or not isinstance(final_sequence_count, int)
        or final_sequence_count < 64
        or not isinstance(final_strata, dict)
        or any(not isinstance(count, int) or count < 8 for count in final_strata.values())
        or "svg_log_viewer" not in final_strata
    ):
        errors.append("final-test generator pool contract invalid")
    if any(key in final_pool for key in ("seed", "seeds", "concreteSeed", "concreteSeeds")):
        errors.append("concrete final-test seed material exists before the claim test")
    if (root / "corpus" / "generated" / "validation").exists():
        errors.append("validation renderer output exists before one-shot validation")
    if (root / "corpus" / "generated" / "final_test").exists():
        errors.append("final-test renderer output exists before the claim test")
    return errors


def main() -> int:
    errors = validate_protocol()
    if errors:
        print("\n".join(errors))
        return 1
    print("corpus_protocol_v1 semantic and hash lock passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
