from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "protocols" / "saliency_validation_v1" / "manifest.lock"
EXPECTED_FILES = {
    "cmake/FrozenEvaluationTargets.cmake",
    "corpus/manifests/validation.json",
    "include/glyphrelay/cuda_preprocess.hpp",
    "include/glyphrelay/saliency.hpp",
    "include/glyphrelay/saliency_development.hpp",
    "protocols/saliency_validation_v1/execution-contract.json",
    "qualification/m0-phases.json",
    "schemas/saliency-selection-v1.schema.json",
    "schemas/saliency-validation-evidence-v1.schema.json",
    "schemas/uniform-aq-selection-v1.schema.json",
    "src/gpu/cuda_preprocess.cu",
    "src/gpu/saliency.cpp",
    "src/gpu/saliency_development.cpp",
    "tooling/corpus/corpus-model.ts",
    "tooling/corpus/render-validation.ts",
    "tools/check_saliency_validation_protocol.py",
    "tools/corpus/evaluate_ocr.py",
    "tools/corpus/evaluate_validation_ocr.py",
    "tools/corpus/prepare_saliency_development.py",
    "tools/corpus/prepare_saliency_validation.py",
    "tools/evaluate_saliency_validation.cpp",
    "tools/run_saliency_validation.py",
}


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object: {path}")
    return value


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_lock(root: Path = ROOT) -> tuple[list[str], str | None]:
    errors: list[str] = []
    path = root / "protocols" / "saliency_validation_v1" / "manifest.lock"
    if not path.is_file():
        return ["saliency validation protocol manifest lock is missing"], None
    lock = load_object(path)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "saliency_validation_v1":
        errors.append("saliency validation protocol lock identity changed")
    entries = lock.get("files")
    if not isinstance(entries, list):
        return errors + ["saliency validation protocol file list is invalid"], None
    paths: list[str] = []
    material = bytearray()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            errors.append(f"saliency validation protocol entry {index} is invalid")
            continue
        relative = entry.get("path")
        expected = entry.get("sha256")
        if not isinstance(relative, str) or not isinstance(expected, str):
            errors.append(f"saliency validation protocol entry {index} is invalid")
            continue
        parsed = PurePosixPath(relative)
        if parsed.is_absolute() or ".." in parsed.parts:
            errors.append(f"saliency validation protocol path is unsafe: {relative}")
            continue
        paths.append(relative)
        source = root / relative
        if not source.is_file():
            errors.append(f"saliency validation protocol file is missing: {relative}")
        elif sha256_file(source) != expected:
            errors.append(f"saliency validation protocol hash changed: {relative}")
        material.extend(f"{relative}\0{expected}\n".encode())
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        errors.append("saliency validation protocol paths must be unique and sorted")
    if set(paths) != EXPECTED_FILES:
        errors.append("saliency validation protocol file set changed")
    protocol_sha256 = hashlib.sha256(bytes(material)).hexdigest()
    if lock.get("protocol_sha256") != protocol_sha256:
        errors.append("saliency validation protocol aggregate hash changed")
    return errors, protocol_sha256


def validate_contract(root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    contract = load_object(
        root / "protocols" / "saliency_validation_v1" / "execution-contract.json"
    )
    if (
        contract.get("schemaVersion") != 1
        or contract.get("protocol") != "saliency_validation_v1"
        or contract.get("split") != "validation"
    ):
        errors.append("saliency validation execution identity changed")
    if contract.get("thresholds") != {
        "losslessOverallBoundedCerMaximum": 0.02,
        "losslessSmallGlyphBoundedCerMaximum": 0.05,
        "overallGlyphRecallMinimum": 0.9,
        "smallGlyphRecallMinimum": 0.8,
        "protectedFractionMaximum": 0.35,
        "falseProtectedFractionMaximum": 0.15,
        "staticMapChangeFractionMaximum": 0.02,
    }:
        errors.append("saliency validation thresholds changed")
    if contract.get("rendering") != {
        "nodeVersion": "v24.18.1",
        "chromiumVersion": "151.0.7922.34",
        "tesseractVersion": "5.3.4",
        "sampleFrameIds": [0, 60, 120, 180],
        "staticWarmupFrames": 5,
    }:
        errors.append("saliency validation rendering contract changed")
    corpus_lock = load_object(root / "protocols" / "corpus_protocol_v1" / "manifest.lock")
    validation_hash = next(
        (
            entry.get("sha256")
            for entry in corpus_lock.get("files", [])
            if entry.get("path") == "corpus/manifests/validation.json"
        ),
        None,
    )
    if validation_hash != sha256_file(root / "corpus" / "manifests" / "validation.json"):
        errors.append("frozen validation manifest identity changed")
    if (root / "corpus" / "generated" / "validation").exists():
        errors.append("validation renderer output is open in the repository")
    return errors


def validate_protocol(root: Path = ROOT) -> tuple[list[str], str | None]:
    lock_errors, protocol_sha256 = validate_lock(root)
    return lock_errors + validate_contract(root), protocol_sha256


def main() -> int:
    errors, protocol_sha256 = validate_protocol()
    if errors:
        for error in errors:
            print(f"saliency validation protocol check failed: {error}")
        return 1
    selections_present = all(
        path.is_file()
        for path in (
            ROOT / "protocols" / "saliency_v1" / "selected-configuration.json",
            ROOT / "protocols" / "uniform_aq_v1" / "selected-configuration.json",
        )
    )
    print(
        json.dumps(
            {
                "protocolSha256": protocol_sha256,
                "selectionStatus": "ready" if selections_present else "pending_target_selection",
                "status": "VALID",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
