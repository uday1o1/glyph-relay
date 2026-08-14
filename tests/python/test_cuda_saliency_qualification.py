from __future__ import annotations

import json
from pathlib import Path

import pytest

from tools.validate_cuda_saliency_qualification import (
    CudaSaliencyValidationError,
    validate_cuda_saliency_evidence,
)

SCHEMA = Path("schemas/cuda-saliency-qualification-v1.schema.json")


def correctness() -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "protocol": "saliency_v1",
        "mode": "correctness",
        "status": "PASSED",
        "reason": "cuda_saliency_correctness_passed",
        "frameCount": 29,
        "comparedLumaChromaCodes": 1,
        "comparedFeatures": 1,
        "maximumCodeError": 1,
        "maximumFeatureError": 1e-12,
        "deterministic": True,
    }


def performance() -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "protocol": "saliency_v1",
        "mode": "performance",
        "status": "PASSED",
        "reason": "cuda_saliency_performance_passed",
        "warmupFrames": 300,
        "measuredFrames": 1_800,
        "measuredWallDurationNs": 10_000_000_000,
        "p95Ns": {
            "inputUpload": 1,
            "colorConversion": 1,
            "featureExtraction": 1,
            "temporalHysteresis": 1,
            "morphologyAndOverrides": 1,
            "macroblockReduction": 1,
            "hostMapCopy": 1,
            "totalPipeline": 5_000_000,
        },
    }


def write(path: Path, value: dict[str, object]) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def test_correctness_and_performance_boundary_controls_pass(tmp_path: Path) -> None:
    correctness_path = tmp_path / "correctness.json"
    performance_path = tmp_path / "performance.json"
    write(correctness_path, correctness())
    write(performance_path, performance())
    assert validate_cuda_saliency_evidence(correctness_path, SCHEMA)["mode"] == "correctness"
    assert validate_cuda_saliency_evidence(performance_path, SCHEMA)["mode"] == "performance"


def test_seeded_color_error_defect_fails_for_its_reason(tmp_path: Path) -> None:
    value = correctness()
    value["maximumCodeError"] = 2
    path = tmp_path / "bad-color.json"
    write(path, value)
    with pytest.raises(CudaSaliencyValidationError, match="color_code_error_exceeded"):
        validate_cuda_saliency_evidence(path, SCHEMA)


def test_seeded_nondeterminism_defect_fails_for_its_reason(tmp_path: Path) -> None:
    value = correctness()
    value["deterministic"] = False
    path = tmp_path / "nondeterministic.json"
    write(path, value)
    with pytest.raises(CudaSaliencyValidationError, match="determinism_not_proven"):
        validate_cuda_saliency_evidence(path, SCHEMA)


def test_seeded_performance_regression_fails_for_its_reason(tmp_path: Path) -> None:
    value = performance()
    timings = value["p95Ns"]
    assert isinstance(timings, dict)
    timings["totalPipeline"] = 5_000_001
    path = tmp_path / "slow.json"
    write(path, value)
    with pytest.raises(CudaSaliencyValidationError, match="performance_p95_exceeded"):
        validate_cuda_saliency_evidence(path, SCHEMA)


def test_seeded_short_performance_measurement_fails_for_its_reason(tmp_path: Path) -> None:
    value = performance()
    value["measuredWallDurationNs"] = 9_999_999_999
    path = tmp_path / "short.json"
    write(path, value)
    with pytest.raises(
        CudaSaliencyValidationError, match="performance_measurement_duration_invalid"
    ):
        validate_cuda_saliency_evidence(path, SCHEMA)
