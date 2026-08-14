from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[2]
GRID_PATH = ROOT / "corpus" / "saliency-grid-v1.json"
EVIDENCE_SCHEMA_PATH = ROOT / "schemas" / "saliency-development-evidence-v1.schema.json"
SELECTION_SCHEMA_PATH = ROOT / "schemas" / "saliency-selection-v1.schema.json"
PROTOCOL_LOCK_PATH = ROOT / "protocols" / "saliency_v1" / "manifest.lock"
FORBIDDEN_RENDER_OUTPUTS = (
    ROOT / "corpus" / "generated" / "validation",
    ROOT / "corpus" / "generated" / "final_test",
)
CORE_STRATA = (
    "animated_typing_scrolling",
    "browser_documentation",
    "code_editor",
    "mixed_video_text",
    "slide_diagram",
    "spreadsheet_table",
    "terminal",
)


class SaliencySelectionError(ValueError):
    """Raised when development evidence cannot produce a frozen selection."""


@dataclass(frozen=True, order=True)
class SaliencyConfiguration:
    gradient_weight: float
    contrast_weight: float
    edge_pair_weight: float
    small_structure_weight: float
    entry_threshold: float
    exit_threshold: float
    previous_score_coefficient: float
    dilation_radius_tiles: int

    def json(self) -> dict[str, float | int]:
        return {
            "contrastWeight": self.contrast_weight,
            "dilationRadiusTiles": self.dilation_radius_tiles,
            "edgePairWeight": self.edge_pair_weight,
            "entryThreshold": self.entry_threshold,
            "exitThreshold": self.exit_threshold,
            "gradientWeight": self.gradient_weight,
            "previousScoreCoefficient": self.previous_score_coefficient,
            "smallStructureWeight": self.small_structure_weight,
        }


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SaliencySelectionError(f"json_invalid:{path.name}") from error
    if not isinstance(value, dict):
        raise SaliencySelectionError(f"json_object_required:{path.name}")
    return value


def canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def validate_schema(value: object, schema_path: Path) -> None:
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(value), key=lambda item: list(item.path)
    )
    if errors:
        location = ".".join(str(part) for part in errors[0].path) or "root"
        raise SaliencySelectionError(f"schema_invalid:{location}:{errors[0].message}")


def verify_protocol_lock() -> dict[str, Any]:
    lock = load_object(PROTOCOL_LOCK_PATH)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "saliency_v1":
        raise SaliencySelectionError("saliency_protocol_lock_identity_invalid")
    files = lock.get("files")
    if not isinstance(files, list):
        raise SaliencySelectionError("saliency_protocol_lock_files_invalid")
    material = bytearray()
    for raw in files:
        if not isinstance(raw, dict):
            raise SaliencySelectionError("saliency_protocol_lock_entry_invalid")
        path = raw.get("path")
        expected = raw.get("sha256")
        if not isinstance(path, str) or not isinstance(expected, str):
            raise SaliencySelectionError("saliency_protocol_lock_entry_invalid")
        source = ROOT / path
        if not source.is_file() or sha256_file(source) != expected:
            raise SaliencySelectionError(f"saliency_protocol_hash_mismatch:{path}")
        material.extend(f"{path}\0{expected}\n".encode())
    if sha256_bytes(bytes(material)) != lock.get("protocol_sha256"):
        raise SaliencySelectionError("saliency_protocol_aggregate_hash_mismatch")
    return lock


def configuration_from_json(value: object) -> SaliencyConfiguration:
    if not isinstance(value, dict):
        raise SaliencySelectionError("candidate_configuration_invalid")
    expected = {
        "contrastWeight",
        "dilationRadiusTiles",
        "edgePairWeight",
        "entryThreshold",
        "exitThreshold",
        "gradientWeight",
        "previousScoreCoefficient",
        "smallStructureWeight",
    }
    if set(value) != expected:
        raise SaliencySelectionError("candidate_configuration_fields_invalid")
    numeric_names = expected - {"dilationRadiusTiles"}
    if any(
        not isinstance(value[name], (int, float))
        or isinstance(value[name], bool)
        or not math.isfinite(float(value[name]))
        for name in numeric_names
    ):
        raise SaliencySelectionError("candidate_configuration_number_invalid")
    radius = value["dilationRadiusTiles"]
    if not isinstance(radius, int) or isinstance(radius, bool):
        raise SaliencySelectionError("candidate_configuration_radius_invalid")
    return SaliencyConfiguration(
        gradient_weight=float(value["gradientWeight"]),
        contrast_weight=float(value["contrastWeight"]),
        edge_pair_weight=float(value["edgePairWeight"]),
        small_structure_weight=float(value["smallStructureWeight"]),
        entry_threshold=float(value["entryThreshold"]),
        exit_threshold=float(value["exitThreshold"]),
        previous_score_coefficient=float(value["previousScoreCoefficient"]),
        dilation_radius_tiles=radius,
    )


def enumerate_grid(grid: dict[str, Any]) -> tuple[SaliencyConfiguration, ...]:
    parameters = grid.get("parameters")
    if not isinstance(parameters, dict):
        raise SaliencySelectionError("saliency_grid_parameters_invalid")
    try:
        weights = tuple(float(value) for value in parameters["featureWeights"])
        entries = tuple(float(value) for value in parameters["entryThresholds"])
        exits = tuple(float(value) for value in parameters["exitThresholds"])
        coefficients = tuple(float(value) for value in parameters["previousScoreCoefficients"])
        radii = tuple(int(value) for value in parameters["dilationRadiusTiles"])
        required_sum = float(parameters["featureWeightSum"])
    except (KeyError, TypeError, ValueError) as error:
        raise SaliencySelectionError("saliency_grid_parameters_invalid") from error
    configurations: list[SaliencyConfiguration] = []
    for feature_weights in itertools.product(weights, repeat=4):
        if not math.isclose(sum(feature_weights), required_sum, rel_tol=0.0, abs_tol=1e-12):
            continue
        for entry, exit_threshold, coefficient, radius in itertools.product(
            entries, exits, coefficients, radii
        ):
            if exit_threshold >= entry:
                continue
            configurations.append(
                SaliencyConfiguration(
                    gradient_weight=feature_weights[0],
                    contrast_weight=feature_weights[1],
                    edge_pair_weight=feature_weights[2],
                    small_structure_weight=feature_weights[3],
                    entry_threshold=entry,
                    exit_threshold=exit_threshold,
                    previous_score_coefficient=coefficient,
                    dilation_radius_tiles=radius,
                )
            )
    unique = tuple(sorted(set(configurations)))
    if len(unique) != grid.get("expectedCandidateCount"):
        raise SaliencySelectionError("saliency_grid_candidate_count_invalid")
    return unique


def metric_number(metrics: dict[str, Any], name: str) -> float:
    value = metrics.get(name)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise SaliencySelectionError(f"candidate_metric_invalid:{name}")
    converted = float(value)
    if not math.isfinite(converted) or converted < 0.0 or converted > 1.0:
        raise SaliencySelectionError(f"candidate_metric_invalid:{name}")
    return converted


def processing_time(metrics: dict[str, Any]) -> int:
    value = metrics.get("processingP95Ns")
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise SaliencySelectionError("candidate_metric_invalid:processingP95Ns")
    return value


def verify_metric_detail(metrics: dict[str, Any]) -> None:
    per_stratum = metrics.get("perStratum")
    p95_sequence = metrics.get("p95Sequence")
    if not isinstance(per_stratum, dict) or set(per_stratum) != set(CORE_STRATA):
        raise SaliencySelectionError("candidate_per_stratum_incomplete")
    metric_names = (
        "overallGlyphRecall",
        "smallGlyphRecall",
        "protectedFraction",
        "falseProtectedFraction",
        "falseDiscoveryFraction",
        "staticMapChangeFraction",
    )
    for stratum in CORE_STRATA:
        detail = per_stratum[stratum]
        if not isinstance(detail, dict) or set(detail) != set(metric_names):
            raise SaliencySelectionError("candidate_per_stratum_metrics_invalid")
        for name in metric_names:
            metric_number(detail, name)
    if not isinstance(p95_sequence, dict) or set(p95_sequence) != set(metric_names):
        raise SaliencySelectionError("candidate_p95_sequence_metrics_invalid")
    for name in metric_names:
        metric_number(p95_sequence, name)


def passes_thresholds(metrics: dict[str, Any], thresholds: dict[str, Any]) -> bool:
    return (
        metric_number(metrics, "overallGlyphRecall")
        >= float(thresholds["overallGlyphRecallMinimum"])
        and metric_number(metrics, "smallGlyphRecall")
        >= float(thresholds["smallGlyphRecallMinimum"])
        and metric_number(metrics, "protectedFraction")
        <= float(thresholds["protectedFractionMaximum"])
        and metric_number(metrics, "falseProtectedFraction")
        <= float(thresholds["falseProtectedFractionMaximum"])
        and metric_number(metrics, "staticMapChangeFraction")
        <= float(thresholds["staticMapChangeFractionMaximum"])
    )


def candidate_key(candidate: dict[str, Any], grid: dict[str, Any]) -> tuple[object, ...]:
    metrics = candidate["metrics"]
    configuration = configuration_from_json(candidate["configuration"])
    if grid.get("lexicographicConfigurationSerialization") != "canonical_json_sorted_keys_utf8":
        raise SaliencySelectionError("lexicographic_configuration_serialization_invalid")
    return (
        -metric_number(metrics, "smallGlyphRecall"),
        metric_number(metrics, "falseProtectedFraction"),
        metric_number(metrics, "protectedFraction"),
        metric_number(metrics, "staticMapChangeFraction"),
        processing_time(metrics),
        canonical_json(configuration.json()),
    )


def select_development_evidence(
    evidence: dict[str, Any], grid: dict[str, Any], lock: dict[str, Any]
) -> dict[str, Any]:
    validate_schema(evidence, EVIDENCE_SCHEMA_PATH)
    if evidence.get("split") != "development":
        raise SaliencySelectionError("selector_rejects_non_development_split")
    identities = {
        "corpusProtocolSha256": grid.get("corpusProtocolSha256"),
        "developmentManifestSha256": grid.get("developmentManifestSha256"),
        "developmentRenderIndexSha256": grid.get("developmentRenderIndexSha256"),
        "gridSha256": sha256_file(GRID_PATH),
    }
    for name, expected in identities.items():
        if evidence.get(name) != expected:
            raise SaliencySelectionError(f"development_evidence_identity_mismatch:{name}")
    expected_grid = set(enumerate_grid(grid))
    candidates = evidence.get("candidates")
    if not isinstance(candidates, list):
        raise SaliencySelectionError("development_candidates_invalid")
    actual: dict[SaliencyConfiguration, dict[str, Any]] = {}
    for candidate in candidates:
        if not isinstance(candidate, dict):
            raise SaliencySelectionError("development_candidate_invalid")
        configuration = configuration_from_json(candidate.get("configuration"))
        if configuration in actual:
            raise SaliencySelectionError("development_candidate_duplicate")
        actual[configuration] = candidate
    if set(actual) != expected_grid:
        raise SaliencySelectionError("development_grid_coverage_incomplete")
    thresholds = grid.get("developmentThresholds")
    if not isinstance(thresholds, dict):
        raise SaliencySelectionError("development_thresholds_invalid")
    eligible: list[dict[str, Any]] = []
    for configuration in sorted(actual):
        candidate = actual[configuration]
        status = candidate.get("status")
        metrics = candidate.get("metrics")
        invalid_reason = candidate.get("invalidReason")
        if status == "INVALID":
            if not isinstance(invalid_reason, str) or not invalid_reason:
                raise SaliencySelectionError("invalid_candidate_reason_missing")
            if metrics is not None:
                raise SaliencySelectionError("invalid_candidate_has_metrics")
            continue
        if status != "PASSED" or invalid_reason is not None or not isinstance(metrics, dict):
            raise SaliencySelectionError("passed_candidate_shape_invalid")
        for name in (
            "overallGlyphRecall",
            "smallGlyphRecall",
            "protectedFraction",
            "falseProtectedFraction",
            "falseDiscoveryFraction",
            "staticMapChangeFraction",
        ):
            metric_number(metrics, name)
        processing_time(metrics)
        verify_metric_detail(metrics)
        if passes_thresholds(metrics, thresholds):
            eligible.append(candidate)
    if not eligible:
        raise SaliencySelectionError("no_development_candidate_passed_all_thresholds")
    winner = min(eligible, key=lambda candidate: candidate_key(candidate, grid))
    configuration_json = configuration_from_json(winner["configuration"]).json()
    configuration_sha256 = sha256_bytes(canonical_json(configuration_json))
    selector_entry = next(
        (
            entry
            for entry in lock["files"]
            if entry.get("path") == "tools/corpus/saliency_selector.py"
        ),
        None,
    )
    if not isinstance(selector_entry, dict):
        raise SaliencySelectionError("selector_hash_missing_from_protocol_lock")
    selection = {
        "schemaVersion": 1,
        "protocol": "saliency_v1",
        "split": "development",
        "status": "SELECTED",
        "sourceBundleId": evidence["sourceBundleId"],
        "automaticMapImplementationSha256": evidence["automaticMapImplementationSha256"],
        "processingPlatformSha256": evidence["processingPlatformSha256"],
        "corpusProtocolSha256": evidence["corpusProtocolSha256"],
        "developmentManifestSha256": evidence["developmentManifestSha256"],
        "developmentRenderIndexSha256": evidence["developmentRenderIndexSha256"],
        "gridSha256": evidence["gridSha256"],
        "selectorSha256": selector_entry["sha256"],
        "evidenceSha256": sha256_bytes(canonical_json(evidence)),
        "candidateCount": len(candidates),
        "eligibleCandidateCount": len(eligible),
        "configuration": configuration_json,
        "configurationSha256": configuration_sha256,
        "metrics": winner["metrics"],
        "thresholds": thresholds,
        "selectionObjective": grid["selectorObjective"],
    }
    validate_schema(selection, SELECTION_SCHEMA_PATH)
    return selection


def run_selection(evidence_path: Path, output_path: Path) -> dict[str, Any]:
    if any(path.exists() for path in FORBIDDEN_RENDER_OUTPUTS):
        raise SaliencySelectionError("selector_refuses_open_validation_or_final_test_output")
    if output_path.exists():
        raise SaliencySelectionError("selection_output_exists")
    if not output_path.parent.is_dir():
        raise SaliencySelectionError("selection_output_parent_missing")
    lock = verify_protocol_lock()
    grid = load_object(GRID_PATH)
    evidence = load_object(evidence_path)
    selection = select_development_evidence(evidence, grid, lock)
    with output_path.open("x", encoding="utf-8") as sink:
        json.dump(selection, sink, sort_keys=True, separators=(",", ":"), allow_nan=False)
        sink.write("\n")
        sink.flush()
        os.fsync(sink.fileno())
    directory = os.open(output_path.parent, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return selection


def main() -> int:
    parser = argparse.ArgumentParser(description="Freeze the saliency_v1 development selection")
    parser.add_argument("--development-evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        selection = run_selection(
            arguments.development_evidence.resolve(), arguments.output.resolve()
        )
    except SaliencySelectionError as error:
        print(f"saliency development selection failed: {error}")
        return 1
    print(
        json.dumps(
            {
                "configurationSha256": selection["configurationSha256"],
                "eligibleCandidateCount": selection["eligibleCandidateCount"],
                "status": selection["status"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
