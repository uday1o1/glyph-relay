from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path, PurePosixPath
from typing import Any

import jsonschema

ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from tools.corpus.uniform_aq_selector import AqFields, expected_aq_fields  # noqa: E402

LOCK_PATH = ROOT / "protocols" / "uniform_aq_v1" / "manifest.lock"
SELECTION_PATH = ROOT / "protocols" / "uniform_aq_v1" / "selected-configuration.json"
EXPECTED_FILES = {
    "cmake/FrozenEvaluationTargets.cmake",
    "corpus/uniform-aq-v1.json",
    "include/glyphrelay/nvenc_encoder.hpp",
    "protocols/uniform_aq_v1/execution-contract.json",
    "qualification/m0-phases.json",
    "schemas/uniform-aq-development-evidence-v1.schema.json",
    "schemas/uniform-aq-selection-v1.schema.json",
    "src/gpu/cuda_preprocess.cu",
    "src/gpu/nvenc_encoder.cpp",
    "tooling/corpus/prepare-decoded-ocr.ts",
    "tooling/corpus/verify-browser-decode.ts",
    "tools/check_uniform_aq_protocol.py",
    "tools/corpus/aq_selector.py",
    "tools/corpus/evaluate_ocr.py",
    "tools/corpus/run_tesseract.sh",
    "tools/corpus/uniform_aq_selector.py",
    "tools/evaluate_uniform_aq.cpp",
    "tools/run_uniform_aq_development.py",
}
EXPECTED_TARGETS = [0.5, 0.75, 1, 2, 4]
EXPECTED_OBJECTIVE = [
    "lowest_unweighted_mean_equal_stratum_fitted_cer_at_five_targets",
    "lowest_fitted_cer_at_1_mbps",
    "lowest_estimable_fitted_bitrate_at_10_percent_cer",
    "lowest_p95_preprocess_plus_encode_latency",
    "lowest_mean_sender_cpu",
    "lexicographic_effective_fields",
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
    lock_path = root / "protocols" / "uniform_aq_v1" / "manifest.lock"
    if not lock_path.is_file():
        return ["uniform AQ protocol manifest lock is missing"], None
    lock = load_object(lock_path)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "uniform_aq_v1":
        errors.append("uniform AQ protocol lock identity changed")
    raw_files = lock.get("files")
    if not isinstance(raw_files, list):
        return errors + ["uniform AQ protocol file list is invalid"], None
    paths: list[str] = []
    material = bytearray()
    for index, entry in enumerate(raw_files):
        if not isinstance(entry, dict):
            errors.append(f"uniform AQ protocol entry {index} is invalid")
            continue
        path = entry.get("path")
        expected = entry.get("sha256")
        if not isinstance(path, str) or not isinstance(expected, str):
            errors.append(f"uniform AQ protocol entry {index} is invalid")
            continue
        parsed = PurePosixPath(path)
        if parsed.is_absolute() or ".." in parsed.parts:
            errors.append(f"uniform AQ protocol path is unsafe: {path}")
            continue
        paths.append(path)
        source = root / path
        if not source.is_file():
            errors.append(f"uniform AQ protocol file is missing: {path}")
        elif sha256_bytes(source.read_bytes()) != expected:
            errors.append(f"uniform AQ protocol hash changed: {path}")
        material.extend(f"{path}\0{expected}\n".encode())
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        errors.append("uniform AQ protocol paths must be unique and sorted")
    if set(paths) != EXPECTED_FILES:
        errors.append("uniform AQ protocol file set changed")
    protocol_sha256 = sha256_bytes(bytes(material))
    if lock.get("protocol_sha256") != protocol_sha256:
        errors.append("uniform AQ protocol aggregate hash changed")
    return errors, protocol_sha256


def validate_grid(root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    grid = load_object(root / "corpus" / "uniform-aq-v1.json")
    if (
        grid.get("schemaVersion") != 1
        or grid.get("protocol") != "corpus_protocol_v1"
        or grid.get("name") != "uniform_aq_v1"
    ):
        errors.append("uniform AQ grid identity changed")
    if grid.get("targetPayloadMbps") != EXPECTED_TARGETS:
        errors.append("uniform AQ target matrix changed")
    if grid.get("rateMatchToleranceFraction") != 0.02:
        errors.append("uniform AQ rate-match tolerance changed")
    execution = load_object(root / "protocols" / "uniform_aq_v1" / "execution-contract.json")
    if execution.get("schemaVersion") != 1 or execution.get("protocol") != "uniform_aq_v1":
        errors.append("uniform AQ execution identity changed")
    if execution.get("targetSearch") != {
        "initialRequestedPayloadBps": "round_target_mbps_times_1000000",
        "maximumAttempts": 4,
        "minimumRequestedPayloadBps": 100000,
        "maximumRequestedPayloadBps": 20000000,
        "update": "round_previous_request_times_target_divided_by_measured",
        "invalidTrialRetry": "same_requested_rate_until_attempt_budget_exhausted",
    }:
        errors.append("uniform AQ target-search contract changed")
    if execution.get(
        "sourceSchedule"
    ) != "each_frozen_sample_frame_held_for_its_exact_60_frame_interval" or execution.get(
        "systemsWindow"
    ) != {
        "warmupFrames": 300,
        "measuredRealtimeFrames": 300,
        "remainingFrames": ("accelerated_with_30fps_timestamps_and_serial_output_completion"),
    }:
        errors.append("uniform AQ source or systems window changed")
    expected_grid = {
        "enableAQ": [False, True],
        "aqStrengthWhenEnabled": [1, 4, 8, 12, 15],
        "canonicalAqStrengthWhenDisabled": 0,
        "enableTemporalAQ": [False, True],
        "exclude": [
            {
                "enableAQ": False,
                "aqStrength": 0,
                "enableTemporalAQ": False,
                "reason": "controlled_uniform_identity",
            }
        ],
    }
    if grid.get("grid") != expected_grid:
        errors.append("uniform AQ candidate grid changed")
    if grid.get("selectorObjective") != EXPECTED_OBJECTIVE:
        errors.append("uniform AQ selector objective changed")
    expected_fields = expected_aq_fields()
    if len(expected_fields) != 11 or AqFields(False, 0, False) in expected_fields:
        errors.append("uniform AQ selector does not enumerate exactly 11 candidates")
    return errors


def validate_selection_if_present(root: Path = ROOT) -> list[str]:
    selection_path = root / "protocols" / "uniform_aq_v1" / "selected-configuration.json"
    if not selection_path.exists():
        return []
    schema = load_object(root / "schemas" / "uniform-aq-selection-v1.schema.json")
    selection = load_object(selection_path)
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(selection), key=str)
    if errors:
        return [f"selected uniform AQ configuration is invalid: {errors[0].message}"]
    lock = load_object(root / "protocols" / "uniform_aq_v1" / "manifest.lock")
    if selection.get("protocolSha256") != lock.get("protocol_sha256"):
        return ["selected uniform AQ configuration has the wrong protocol identity"]
    selector_hash = next(
        (
            entry.get("sha256")
            for entry in lock.get("files", [])
            if entry.get("path") == "tools/corpus/uniform_aq_selector.py"
        ),
        None,
    )
    if selection.get("selectorSha256") != selector_hash:
        return ["selected uniform AQ configuration has the wrong selector identity"]
    return []


def main() -> int:
    lock_errors, protocol_sha256 = validate_lock()
    errors = lock_errors + validate_grid() + validate_selection_if_present()
    if errors:
        for error in errors:
            print(f"uniform AQ protocol check failed: {error}")
        return 1
    print(
        json.dumps(
            {
                "candidateCount": 11,
                "protocolSha256": protocol_sha256,
                "selectionStatus": "selected"
                if SELECTION_PATH.exists()
                else "pending_target_development_evidence",
                "status": "VALID",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
