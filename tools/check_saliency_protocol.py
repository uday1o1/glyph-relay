from __future__ import annotations

import hashlib
import itertools
import json
import math
from pathlib import Path, PurePosixPath
from typing import Any

import jsonschema

ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "protocols" / "saliency_v1" / "manifest.lock"
GRID_PATH = ROOT / "corpus" / "saliency-grid-v1.json"
SELECTION_PATH = ROOT / "protocols" / "saliency_v1" / "selected-configuration.json"
EXPECTED_FILES = {
    "corpus/saliency-grid-v1.json",
    "schemas/saliency-development-evidence-v1.schema.json",
    "schemas/saliency-selection-v1.schema.json",
    "tools/check_saliency_protocol.py",
    "tools/corpus/saliency_selector.py",
    "tools/validate_saliency_selection.py",
}
EXPECTED_PARAMETERS = {
    "featureWeights": [0.15, 0.25, 0.35, 0.45],
    "featureWeightSum": 1,
    "entryThresholds": [0.5, 0.55, 0.6],
    "exitThresholds": [0.3, 0.35, 0.4],
    "previousScoreCoefficients": [0.4, 0.6, 0.8],
    "dilationRadiusTiles": [0, 1, 2],
}
EXPECTED_THRESHOLDS = {
    "overallGlyphRecallMinimum": 0.9,
    "smallGlyphRecallMinimum": 0.8,
    "protectedFractionMaximum": 0.35,
    "falseProtectedFractionMaximum": 0.15,
    "staticMapChangeFractionMaximum": 0.02,
}
EXPECTED_OBJECTIVE = [
    "discard_candidates_missing_any_development_threshold",
    "highest_small_glyph_recall",
    "lowest_false_protected_fraction",
    "lowest_protected_fraction",
    "lowest_static_map_change_fraction",
    "lowest_p95_processing_time_ns",
    "lexicographic_configuration",
]


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object: {path}")
    return value


def validate_lock(root: Path = ROOT) -> tuple[list[str], str | None]:
    errors: list[str] = []
    lock_path = root / "protocols" / "saliency_v1" / "manifest.lock"
    if not lock_path.is_file():
        return ["saliency protocol manifest lock is missing"], None
    lock = load_object(lock_path)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "saliency_v1":
        errors.append("saliency protocol lock identity changed")
    raw_files = lock.get("files")
    if not isinstance(raw_files, list):
        return errors + ["saliency protocol file list is invalid"], None
    paths: list[str] = []
    material = bytearray()
    for index, entry in enumerate(raw_files):
        if not isinstance(entry, dict):
            errors.append(f"saliency protocol entry {index} is invalid")
            continue
        path = entry.get("path")
        expected = entry.get("sha256")
        if not isinstance(path, str) or not isinstance(expected, str):
            errors.append(f"saliency protocol entry {index} is invalid")
            continue
        parsed = PurePosixPath(path)
        if parsed.is_absolute() or ".." in parsed.parts:
            errors.append(f"saliency protocol path is unsafe: {path}")
            continue
        paths.append(path)
        source = root / path
        if not source.is_file():
            errors.append(f"saliency protocol file is missing: {path}")
        elif sha256_bytes(source.read_bytes()) != expected:
            errors.append(f"saliency protocol hash changed: {path}")
        material.extend(f"{path}\0{expected}\n".encode())
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        errors.append("saliency protocol paths must be unique and sorted")
    if set(paths) != EXPECTED_FILES:
        errors.append("saliency protocol file set changed")
    protocol_sha256 = sha256_bytes(bytes(material))
    if lock.get("protocol_sha256") != protocol_sha256:
        errors.append("saliency protocol aggregate hash changed")
    return errors, protocol_sha256


def validate_grid(root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    grid = load_object(root / "corpus" / "saliency-grid-v1.json")
    if (
        grid.get("schemaVersion") != 1
        or grid.get("protocol") != "saliency_v1"
        or grid.get("corpusProtocol") != "corpus_protocol_v1"
    ):
        errors.append("saliency grid identity changed")
    if grid.get("parameters") != EXPECTED_PARAMETERS:
        errors.append("saliency grid parameters changed")
    if grid.get("developmentThresholds") != EXPECTED_THRESHOLDS:
        errors.append("saliency development thresholds changed")
    if grid.get("selectorObjective") != EXPECTED_OBJECTIVE:
        errors.append("saliency selector objective changed")
    if grid.get("lexicographicConfigurationSerialization") != "canonical_json_sorted_keys_utf8":
        errors.append("saliency lexicographic serialization changed")
    if grid.get("samplingFrameIds") != [0, 60, 120, 180]:
        errors.append("saliency sampling frames changed")
    if grid.get("staticWarmupFrames") != 5:
        errors.append("saliency static warmup changed")
    parameters = grid.get("parameters", {})
    weight_tuples = {
        weights
        for weights in itertools.product(parameters.get("featureWeights", []), repeat=4)
        if math.isclose(sum(weights), 1.0, rel_tol=0.0, abs_tol=1e-12)
    }
    entries = parameters.get("entryThresholds", [])
    exits = parameters.get("exitThresholds", [])
    valid_threshold_pairs = sum(exit_value < entry for entry in entries for exit_value in exits)
    candidate_count = (
        len(weight_tuples)
        * valid_threshold_pairs
        * len(parameters.get("previousScoreCoefficients", []))
        * len(parameters.get("dilationRadiusTiles", []))
    )
    if len(weight_tuples) != 31 or candidate_count != 2511:
        errors.append("saliency grid does not enumerate exactly 2,511 candidates")
    if grid.get("expectedCandidateCount") != candidate_count:
        errors.append("saliency grid stored candidate count changed")
    development_manifest = root / "corpus" / "manifests" / "development.json"
    if sha256_bytes(development_manifest.read_bytes()) != grid.get("developmentManifestSha256"):
        errors.append("saliency grid development manifest identity changed")
    corpus_lock = load_object(root / "protocols" / "corpus_protocol_v1" / "manifest.lock")
    if grid.get("corpusProtocolSha256") != corpus_lock.get("protocol_sha256"):
        errors.append("saliency grid corpus protocol identity changed")
    render_index = root / "corpus" / "generated" / "development" / "index.json"
    if render_index.exists() and sha256_bytes(render_index.read_bytes()) != grid.get(
        "developmentRenderIndexSha256"
    ):
        errors.append("saliency development render index identity changed")
    for split in ("validation", "final_test"):
        if (root / "corpus" / "generated" / split).exists():
            errors.append(f"forbidden renderer output is open: {split}")
    return errors


def validate_selection_if_present(root: Path = ROOT) -> list[str]:
    selection_path = root / "protocols" / "saliency_v1" / "selected-configuration.json"
    if not selection_path.exists():
        return []
    schema = load_object(root / "schemas" / "saliency-selection-v1.schema.json")
    selection = load_object(selection_path)
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(selection), key=str)
    return [f"selected saliency configuration is invalid: {error.message}" for error in errors]


def validate_protocol(root: Path = ROOT) -> tuple[list[str], str | None]:
    lock_errors, protocol_sha256 = validate_lock(root)
    return lock_errors + validate_grid(root) + validate_selection_if_present(root), protocol_sha256


def main() -> int:
    errors, protocol_sha256 = validate_protocol()
    if errors:
        for error in errors:
            print(f"saliency protocol check failed: {error}")
        return 1
    selection_status = (
        "selected" if SELECTION_PATH.exists() else "pending_target_development_evidence"
    )
    print(
        json.dumps(
            {
                "candidateCount": 2511,
                "protocolSha256": protocol_sha256,
                "selectionStatus": selection_status,
                "status": "VALID",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
