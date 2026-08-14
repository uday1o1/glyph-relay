from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator

from tools.corpus.aq_selector import CurvePoint, bitrate_at_error, interpolate_log_bitrate

ROOT = Path(__file__).resolve().parents[2]
GRID_PATH = ROOT / "corpus" / "uniform-aq-v1.json"
EVIDENCE_SCHEMA_PATH = ROOT / "schemas" / "uniform-aq-development-evidence-v1.schema.json"
SELECTION_SCHEMA_PATH = ROOT / "schemas" / "uniform-aq-selection-v1.schema.json"
PROTOCOL_LOCK_PATH = ROOT / "protocols" / "uniform_aq_v1" / "manifest.lock"
SELECTION_PATH = ROOT / "protocols" / "uniform_aq_v1" / "selected-configuration.json"
DEVELOPMENT_MANIFEST_PATH = ROOT / "corpus" / "manifests" / "development.json"
CORPUS_LOCK_PATH = ROOT / "protocols" / "corpus_protocol_v1" / "manifest.lock"
FORBIDDEN_RENDER_OUTPUTS = (
    ROOT / "corpus" / "generated" / "validation",
    ROOT / "corpus" / "generated" / "final_test",
)
TARGET_PAYLOAD_MBPS = (0.5, 0.75, 1.0, 2.0, 4.0)
CORE_STRATA = (
    "animated_typing_scrolling",
    "browser_documentation",
    "code_editor",
    "mixed_video_text",
    "slide_diagram",
    "spreadsheet_table",
    "terminal",
)
RATE_MATCH_TOLERANCE = 0.02
MAXIMUM_PREPROCESS_P95_MS = 5.0
MAXIMUM_ENCODE_P95_MS = 10.0
MAXIMUM_ENCODE_P99_MS = 16.0
MAXIMUM_PENDING_AGE_MS = 33.34
MINIMUM_SUBMITTED_FRAMES = 300


class UniformAqSelectionError(ValueError):
    """Raised when development evidence cannot produce a frozen uniform AQ selection."""


@dataclass(frozen=True, order=True)
class AqFields:
    enable_aq: bool
    aq_strength: int
    enable_temporal_aq: bool

    def json(self) -> dict[str, bool | int]:
        return {
            "aqStrength": self.aq_strength,
            "enableAQ": self.enable_aq,
            "enableTemporalAQ": self.enable_temporal_aq,
        }

    def candidate_id(self) -> str:
        if not self.enable_aq:
            return "aq-disabled-temporal-on"
        temporal = "on" if self.enable_temporal_aq else "off"
        return f"aq-strength-{self.aq_strength:02d}-temporal-{temporal}"


@dataclass(frozen=True)
class SelectorInputs:
    mean_fitted_cer: float
    fitted_cer_at_1_mbps: float
    bitrate_at_ten_percent_cer_mbps: float | None
    p95_preprocess_encode_ms: float
    mean_sender_cpu_percent: float
    lexicographic_effective_aq_fields: bytes

    def json(self) -> dict[str, float | str | None]:
        return {
            "bitrateAtTenPercentCerMbps": self.bitrate_at_ten_percent_cer_mbps,
            "fittedCerAt1Mbps": self.fitted_cer_at_1_mbps,
            "lexicographicEffectiveAqFields": self.lexicographic_effective_aq_fields.decode(),
            "meanFittedCer": self.mean_fitted_cer,
            "meanSenderCpuPercent": self.mean_sender_cpu_percent,
            "p95PreprocessEncodeMs": self.p95_preprocess_encode_ms,
        }


def canonical_json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise UniformAqSelectionError(f"json_invalid:{path.name}") from error
    if not isinstance(value, dict):
        raise UniformAqSelectionError(f"json_object_required:{path.name}")
    return value


def validate_schema(value: object, schema_path: Path) -> None:
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(value), key=lambda item: list(item.path)
    )
    if errors:
        location = ".".join(str(part) for part in errors[0].path) or "root"
        raise UniformAqSelectionError(f"schema_invalid:{location}:{errors[0].message}")


def expected_aq_fields() -> tuple[AqFields, ...]:
    candidates = [AqFields(False, 0, True)]
    candidates.extend(
        AqFields(True, strength, temporal)
        for strength in (1, 4, 8, 12, 15)
        for temporal in (False, True)
    )
    return tuple(sorted(candidates))


def aq_fields_from_json(value: object) -> AqFields:
    if not isinstance(value, dict) or set(value) != {
        "aqStrength",
        "enableAQ",
        "enableTemporalAQ",
    }:
        raise UniformAqSelectionError("effective_aq_fields_invalid")
    enable_aq = value["enableAQ"]
    strength = value["aqStrength"]
    temporal = value["enableTemporalAQ"]
    if (
        not isinstance(enable_aq, bool)
        or not isinstance(temporal, bool)
        or not isinstance(strength, int)
        or isinstance(strength, bool)
        or (enable_aq and strength not in (1, 4, 8, 12, 15))
        or (not enable_aq and strength != 0)
    ):
        raise UniformAqSelectionError("effective_aq_fields_invalid")
    return AqFields(enable_aq, strength, temporal)


def finite_number(value: object, label: str, minimum: float = 0.0) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise UniformAqSelectionError(f"number_invalid:{label}")
    result = float(value)
    if not math.isfinite(result) or result < minimum:
        raise UniformAqSelectionError(f"number_invalid:{label}")
    return result


def verify_protocol_lock() -> dict[str, Any]:
    lock = load_object(PROTOCOL_LOCK_PATH)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "uniform_aq_v1":
        raise UniformAqSelectionError("uniform_aq_protocol_lock_identity_invalid")
    files = lock.get("files")
    if not isinstance(files, list):
        raise UniformAqSelectionError("uniform_aq_protocol_lock_files_invalid")
    material = bytearray()
    for raw in files:
        if not isinstance(raw, dict):
            raise UniformAqSelectionError("uniform_aq_protocol_lock_entry_invalid")
        path = raw.get("path")
        expected = raw.get("sha256")
        if not isinstance(path, str) or not isinstance(expected, str):
            raise UniformAqSelectionError("uniform_aq_protocol_lock_entry_invalid")
        source = ROOT / path
        if not source.is_file() or sha256_file(source) != expected:
            raise UniformAqSelectionError(f"uniform_aq_protocol_hash_mismatch:{path}")
        material.extend(f"{path}\0{expected}\n".encode())
    if sha256_bytes(bytes(material)) != lock.get("protocol_sha256"):
        raise UniformAqSelectionError("uniform_aq_protocol_aggregate_hash_mismatch")
    return lock


def _validate_attempt(attempt: object, label: str) -> None:
    if not isinstance(attempt, dict):
        raise UniformAqSelectionError(f"target_attempt_invalid:{label}")
    requested = attempt.get("requestedPayloadBps")
    status = attempt.get("status")
    measured = attempt.get("measuredPayloadMbps")
    reason = attempt.get("invalidReason")
    if not isinstance(requested, int) or isinstance(requested, bool) or requested <= 0:
        raise UniformAqSelectionError(f"target_attempt_requested_rate_invalid:{label}")
    if status == "PASSED":
        finite_number(measured, f"{label}.measuredPayloadMbps", minimum=1e-12)
        if reason is not None:
            raise UniformAqSelectionError(f"target_attempt_passed_with_reason:{label}")
    elif status == "INVALID":
        if measured is not None:
            finite_number(measured, f"{label}.measuredPayloadMbps", minimum=1e-12)
        if not isinstance(reason, str) or not reason:
            raise UniformAqSelectionError(f"target_attempt_invalid_reason_missing:{label}")
    else:
        raise UniformAqSelectionError(f"target_attempt_status_invalid:{label}")


def _selected_measurement(target: dict[str, Any], label: str) -> float | None:
    attempts = target.get("attempts")
    if not isinstance(attempts, list) or not attempts:
        raise UniformAqSelectionError(f"target_attempts_missing:{label}")
    for index, attempt in enumerate(attempts):
        _validate_attempt(attempt, f"{label}.{index}")
    selected_index = target.get("selectedAttemptIndex")
    if selected_index is None:
        return None
    if (
        not isinstance(selected_index, int)
        or isinstance(selected_index, bool)
        or selected_index < 0
        or selected_index >= len(attempts)
        or attempts[selected_index].get("status") != "PASSED"
    ):
        raise UniformAqSelectionError(f"target_selected_attempt_invalid:{label}")
    return finite_number(
        attempts[selected_index].get("measuredPayloadMbps"),
        f"{label}.selectedMeasuredPayloadMbps",
        minimum=1e-12,
    )


def _validate_passed_target(target: dict[str, Any], measured: float, label: str) -> CurvePoint:
    target_mbps = finite_number(target.get("targetPayloadMbps"), f"{label}.targetPayloadMbps")
    if abs(measured - target_mbps) / target_mbps > RATE_MATCH_TOLERANCE + 1e-12:
        raise UniformAqSelectionError(f"target_rate_match_failed:{label}")
    invalid_reasons = target.get("invalidReasons")
    if invalid_reasons != []:
        raise UniformAqSelectionError(f"passed_target_has_invalid_reasons:{label}")
    per_stratum = target.get("perStratumCer")
    if not isinstance(per_stratum, dict) or set(per_stratum) != set(CORE_STRATA):
        raise UniformAqSelectionError(f"target_per_stratum_incomplete:{label}")
    stratum_values = [
        finite_number(per_stratum[stratum], f"{label}.perStratumCer.{stratum}")
        for stratum in CORE_STRATA
    ]
    if any(value > 1.0 for value in stratum_values):
        raise UniformAqSelectionError(f"target_per_stratum_cer_invalid:{label}")
    equal_stratum = finite_number(target.get("equalStratumCer"), f"{label}.equalStratumCer")
    if equal_stratum > 1.0 or not math.isclose(
        equal_stratum,
        sum(stratum_values) / len(stratum_values),
        rel_tol=0.0,
        abs_tol=1e-12,
    ):
        raise UniformAqSelectionError(f"target_equal_stratum_cer_invalid:{label}")
    preprocess = finite_number(target.get("p95PreprocessMs"), f"{label}.p95PreprocessMs")
    combined_p95 = finite_number(
        target.get("p95PreprocessEncodeMs"), f"{label}.p95PreprocessEncodeMs"
    )
    encode_p95 = finite_number(target.get("p95EncodeMs"), f"{label}.p95EncodeMs")
    encode_p99 = finite_number(target.get("p99EncodeMs"), f"{label}.p99EncodeMs")
    finite_number(target.get("meanSenderCpuPercent"), f"{label}.meanSenderCpuPercent")
    pending_age = finite_number(target.get("maximumPendingAgeMs"), f"{label}.maximumPendingAgeMs")
    submitted = target.get("submittedFrames")
    decoded = target.get("decodedFrames")
    if (
        preprocess > MAXIMUM_PREPROCESS_P95_MS
        or combined_p95 > MAXIMUM_PREPROCESS_P95_MS + MAXIMUM_ENCODE_P95_MS
        or encode_p95 > MAXIMUM_ENCODE_P95_MS
        or encode_p99 > MAXIMUM_ENCODE_P99_MS
        or pending_age > MAXIMUM_PENDING_AGE_MS
        or not isinstance(submitted, int)
        or isinstance(submitted, bool)
        or submitted < MINIMUM_SUBMITTED_FRAMES
        or decoded != submitted
        or target.get("width") != 1920
        or target.get("height") != 1080
        or target.get("pendingPositiveTrend") is not False
        or target.get("independentDecodePassed") is not True
        or target.get("browserDecodePassed") is not True
    ):
        raise UniformAqSelectionError(f"target_systems_admissibility_failed:{label}")
    return CurvePoint(measured, equal_stratum)


def condition_curve(condition: dict[str, Any], label: str) -> list[CurvePoint]:
    target_search = condition.get("targetSearch")
    if not isinstance(target_search, list) or len(target_search) != len(TARGET_PAYLOAD_MBPS):
        raise UniformAqSelectionError(f"condition_target_search_incomplete:{label}")
    points: list[CurvePoint] = []
    for target, expected_target in zip(target_search, TARGET_PAYLOAD_MBPS, strict=True):
        target_label = f"{label}.{expected_target:g}"
        if not isinstance(target, dict) or target.get("targetPayloadMbps") != expected_target:
            raise UniformAqSelectionError(f"target_order_or_identity_invalid:{target_label}")
        measured = _selected_measurement(target, target_label)
        status = target.get("status")
        reasons = target.get("invalidReasons")
        if status == "INVALID":
            if (
                not isinstance(reasons, list)
                or not reasons
                or any(not isinstance(reason, str) or not reason for reason in reasons)
            ):
                raise UniformAqSelectionError(f"invalid_target_reason_missing:{target_label}")
            continue
        if status != "PASSED" or measured is None:
            raise UniformAqSelectionError(f"target_status_invalid:{target_label}")
        points.append(_validate_passed_target(target, measured, target_label))
    return points


def _validate_condition_identity(condition: dict[str, Any], fields: AqFields, label: str) -> None:
    if condition.get("candidateId") != (
        "controlled_uniform" if label == "controlled_uniform" else fields.candidate_id()
    ):
        raise UniformAqSelectionError(f"candidate_identity_invalid:{label}")
    effective_hash = condition.get("effectiveEncoderFieldsSha256")
    if not isinstance(effective_hash, str) or len(effective_hash) != 64:
        raise UniformAqSelectionError(f"effective_encoder_hash_invalid:{label}")


def validate_condition(
    condition: object, expected_fields: AqFields, label: str
) -> list[CurvePoint]:
    if not isinstance(condition, dict):
        raise UniformAqSelectionError(f"condition_invalid:{label}")
    fields = aq_fields_from_json(condition.get("effectiveAqFields"))
    if fields != expected_fields:
        raise UniformAqSelectionError(f"candidate_fields_mismatch:{label}")
    _validate_condition_identity(condition, fields, label)
    points = condition_curve(condition, label)
    status = condition.get("status")
    reasons = condition.get("invalidReasons")
    if status == "PASSED":
        if reasons != [] or len(points) != len(TARGET_PAYLOAD_MBPS):
            raise UniformAqSelectionError(f"passed_condition_incomplete:{label}")
        p95 = finite_number(
            condition.get("p95PreprocessEncodeMs"), f"{label}.p95PreprocessEncodeMs"
        )
        if p95 > MAXIMUM_PREPROCESS_P95_MS + MAXIMUM_ENCODE_P95_MS:
            raise UniformAqSelectionError(f"condition_latency_margin_failed:{label}")
        finite_number(condition.get("meanSenderCpuPercent"), f"{label}.meanSenderCpuPercent")
        if (
            min(point.measured_mbps for point in points) > TARGET_PAYLOAD_MBPS[0]
            or max(point.measured_mbps for point in points) < TARGET_PAYLOAD_MBPS[-1]
        ):
            raise UniformAqSelectionError(f"condition_exact_target_range_not_bracketed:{label}")
    elif status == "INVALID":
        if not isinstance(reasons, list) or not reasons:
            raise UniformAqSelectionError(f"invalid_condition_reason_missing:{label}")
        if len(points) == len(TARGET_PAYLOAD_MBPS):
            raise UniformAqSelectionError(f"invalid_condition_has_complete_passed_targets:{label}")
        if (
            condition.get("p95PreprocessEncodeMs") is not None
            or condition.get("meanSenderCpuPercent") is not None
        ):
            raise UniformAqSelectionError(f"invalid_condition_has_selector_metrics:{label}")
    else:
        raise UniformAqSelectionError(f"condition_status_invalid:{label}")
    return points


def selector_inputs(condition: dict[str, Any], points: list[CurvePoint]) -> SelectorInputs:
    fitted_targets = [interpolate_log_bitrate(points, target) for target in TARGET_PAYLOAD_MBPS]
    fields = aq_fields_from_json(condition["effectiveAqFields"])
    return SelectorInputs(
        mean_fitted_cer=sum(fitted_targets) / len(fitted_targets),
        fitted_cer_at_1_mbps=fitted_targets[2],
        bitrate_at_ten_percent_cer_mbps=bitrate_at_error(points, 0.10),
        p95_preprocess_encode_ms=finite_number(
            condition["p95PreprocessEncodeMs"], "selector.p95PreprocessEncodeMs"
        ),
        mean_sender_cpu_percent=finite_number(
            condition["meanSenderCpuPercent"], "selector.meanSenderCpuPercent"
        ),
        lexicographic_effective_aq_fields=canonical_json(fields.json()),
    )


def selector_key(inputs: SelectorInputs) -> tuple[object, ...]:
    crossing = inputs.bitrate_at_ten_percent_cer_mbps
    return (
        inputs.mean_fitted_cer,
        inputs.fitted_cer_at_1_mbps,
        crossing is None,
        math.inf if crossing is None else crossing,
        inputs.p95_preprocess_encode_ms,
        inputs.mean_sender_cpu_percent,
        inputs.lexicographic_effective_aq_fields,
    )


def comparator_identity(controlled: SelectorInputs, selected: SelectorInputs, metric: str) -> str:
    if metric == "character_error":
        return (
            "best_supported_uniform"
            if selected.fitted_cer_at_1_mbps < controlled.fitted_cer_at_1_mbps
            else "controlled_uniform"
        )
    controlled_crossing = controlled.bitrate_at_ten_percent_cer_mbps
    selected_crossing = selected.bitrate_at_ten_percent_cer_mbps
    if selected_crossing is not None and (
        controlled_crossing is None or selected_crossing < controlled_crossing
    ):
        return "best_supported_uniform"
    return "controlled_uniform"


def select_development_evidence(
    evidence: dict[str, Any], grid: dict[str, Any], lock: dict[str, Any]
) -> dict[str, Any]:
    validate_schema(evidence, EVIDENCE_SCHEMA_PATH)
    corpus_lock = load_object(CORPUS_LOCK_PATH)
    identities = {
        "corpusProtocolSha256": corpus_lock.get("protocol_sha256"),
        "developmentManifestSha256": sha256_file(DEVELOPMENT_MANIFEST_PATH),
        "gridSha256": sha256_file(GRID_PATH),
        "protocolSha256": lock.get("protocol_sha256"),
    }
    for name, expected in identities.items():
        if evidence.get(name) != expected:
            raise UniformAqSelectionError(f"development_evidence_identity_mismatch:{name}")
    if (
        grid.get("targetPayloadMbps") != list(TARGET_PAYLOAD_MBPS)
        or grid.get("rateMatchToleranceFraction") != RATE_MATCH_TOLERANCE
    ):
        raise UniformAqSelectionError("uniform_aq_grid_contract_invalid")

    controlled = evidence.get("controlledUniform")
    controlled_points = validate_condition(
        controlled, AqFields(False, 0, False), "controlled_uniform"
    )
    if not isinstance(controlled, dict) or controlled.get("status") != "PASSED":
        raise UniformAqSelectionError("controlled_uniform_not_admissible")
    controlled_inputs = selector_inputs(controlled, controlled_points)

    raw_candidates = evidence.get("candidates")
    if not isinstance(raw_candidates, list):
        raise UniformAqSelectionError("uniform_aq_candidates_invalid")
    expected = set(expected_aq_fields())
    actual: dict[AqFields, tuple[dict[str, Any], list[CurvePoint]]] = {}
    encoder_hashes = {controlled["effectiveEncoderFieldsSha256"]}
    for raw_candidate in raw_candidates:
        if not isinstance(raw_candidate, dict):
            raise UniformAqSelectionError("uniform_aq_candidate_invalid")
        fields = aq_fields_from_json(raw_candidate.get("effectiveAqFields"))
        if fields in actual:
            raise UniformAqSelectionError("uniform_aq_candidate_duplicate")
        points = validate_condition(raw_candidate, fields, fields.candidate_id())
        effective_hash = raw_candidate["effectiveEncoderFieldsSha256"]
        if effective_hash in encoder_hashes:
            raise UniformAqSelectionError("effective_encoder_hash_duplicate")
        encoder_hashes.add(effective_hash)
        actual[fields] = (raw_candidate, points)
    if set(actual) != expected:
        raise UniformAqSelectionError("uniform_aq_grid_coverage_incomplete")

    selectable: list[tuple[dict[str, Any], SelectorInputs]] = []
    for fields in sorted(actual):
        candidate, points = actual[fields]
        if candidate.get("status") == "PASSED":
            selectable.append((candidate, selector_inputs(candidate, points)))
    if not selectable:
        raise UniformAqSelectionError("no_uniform_aq_candidate_selectable")
    winner, winning_inputs = min(selectable, key=lambda item: selector_key(item[1]))
    selector_entry = next(
        (
            entry
            for entry in lock["files"]
            if entry.get("path") == "tools/corpus/uniform_aq_selector.py"
        ),
        None,
    )
    if not isinstance(selector_entry, dict) or not isinstance(selector_entry.get("sha256"), str):
        raise UniformAqSelectionError("uniform_aq_selector_lock_entry_missing")
    selection = {
        "schemaVersion": 1,
        "protocol": "uniform_aq_v1",
        "status": "SELECTED",
        "developmentEvidenceSha256": sha256_bytes(canonical_json(evidence)),
        "protocolSha256": lock["protocol_sha256"],
        "selectorSha256": selector_entry["sha256"],
        "bestSupportedUniform": {
            "candidateId": winner["candidateId"],
            "effectiveAqFields": winner["effectiveAqFields"],
            "effectiveEncoderFieldsSha256": winner["effectiveEncoderFieldsSha256"],
            "selectorInputs": winning_inputs.json(),
        },
        "comparators": {
            "characterError": comparator_identity(
                controlled_inputs, winning_inputs, "character_error"
            ),
            "bitrateEfficiency": comparator_identity(
                controlled_inputs, winning_inputs, "bitrate_efficiency"
            ),
        },
    }
    validate_schema(selection, SELECTION_SCHEMA_PATH)
    return selection


def write_exclusive(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as sink:
            sink.write(canonical_json(value) + b"\n")
            sink.flush()
            os.fsync(sink.fileno())
    except Exception:
        path.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Select the frozen development-only best-supported uniform AQ configuration"
    )
    parser.add_argument("--development-evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        if any(path.exists() for path in FORBIDDEN_RENDER_OUTPUTS):
            raise UniformAqSelectionError("held_out_renderer_output_already_open")
        output = arguments.output.resolve()
        if output != SELECTION_PATH:
            raise UniformAqSelectionError("uniform_aq_selection_output_path_rejected")
        evidence = load_object(arguments.development_evidence.resolve(strict=True))
        grid = load_object(GRID_PATH)
        lock = verify_protocol_lock()
        selection = select_development_evidence(evidence, grid, lock)
        write_exclusive(output, selection)
        print(
            json.dumps(
                {
                    "candidateId": selection["bestSupportedUniform"]["candidateId"],
                    "comparators": selection["comparators"],
                    "status": "SELECTED",
                },
                sort_keys=True,
            )
        )
    except (OSError, UniformAqSelectionError) as error:
        print(f"uniform AQ selection failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
