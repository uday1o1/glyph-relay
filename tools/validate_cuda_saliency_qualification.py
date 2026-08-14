#!/usr/bin/env python3
"""Validate CUDA saliency correctness or performance qualification evidence."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator


class CudaSaliencyValidationError(ValueError):
    """Raised when CUDA saliency evidence is invalid or below its frozen gate."""


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CudaSaliencyValidationError(f"invalid_json:{path.name}") from error
    if not isinstance(value, dict):
        raise CudaSaliencyValidationError(f"json_object_required:{path.name}")
    return value


def validate_cuda_saliency_evidence(path: Path, schema_path: Path) -> dict[str, Any]:
    value = load_object(path)
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(value), key=lambda item: list(item.path)
    )
    if errors:
        location = ".".join(str(part) for part in errors[0].path) or "root"
        raise CudaSaliencyValidationError(f"schema_invalid:{location}:{errors[0].message}")
    if value["status"] != "PASSED":
        raise CudaSaliencyValidationError("qualification_status_not_passed")
    if value["mode"] == "correctness":
        if value["frameCount"] != 29:
            raise CudaSaliencyValidationError("correctness_frame_count_invalid")
        if value["comparedLumaChromaCodes"] <= 0 or value["comparedFeatures"] <= 0:
            raise CudaSaliencyValidationError("correctness_comparison_count_invalid")
        if value["maximumCodeError"] > 1:
            raise CudaSaliencyValidationError("color_code_error_exceeded")
        feature_error = value["maximumFeatureError"]
        if not isinstance(feature_error, (int, float)) or not math.isfinite(feature_error):
            raise CudaSaliencyValidationError("feature_error_not_finite")
        if feature_error > 1e-10:
            raise CudaSaliencyValidationError("feature_error_exceeded")
        if value["deterministic"] is not True:
            raise CudaSaliencyValidationError("determinism_not_proven")
    else:
        if value["warmupFrames"] != 300 or value["measuredFrames"] < 1_800:
            raise CudaSaliencyValidationError("performance_sample_count_invalid")
        if value["measuredWallDurationNs"] < 10_000_000_000:
            raise CudaSaliencyValidationError("performance_measurement_duration_invalid")
        if value["p95Ns"]["totalPipeline"] > 5_000_000:
            raise CudaSaliencyValidationError("performance_p95_exceeded")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("schema", type=Path)
    arguments = parser.parse_args()
    try:
        value = validate_cuda_saliency_evidence(arguments.evidence, arguments.schema)
    except CudaSaliencyValidationError as error:
        print(f"CUDA saliency qualification validation failed: {error}")
        return 1
    print(f"CUDA saliency {value['mode']} evidence passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
