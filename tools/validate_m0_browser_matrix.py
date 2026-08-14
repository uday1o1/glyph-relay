#!/usr/bin/env python3
"""Validate the complete two-browser NVENC playback and recovery matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, cast

from jsonschema import Draft202012Validator

from tools.validate_m0_browser_playback import (
    BrowserPlaybackValidationError,
    validate_browser_playback,
)


class BrowserMatrixValidationError(RuntimeError):
    """Raised when the matrix cannot support the Milestone 0 browser gate."""


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise BrowserMatrixValidationError(reason)


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BrowserMatrixValidationError(f"json_read_failed:{path.name}") from error
    require(isinstance(value, dict), f"json_object_required:{path.name}")
    return cast(dict[str, Any], value)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_schema(value: dict[str, Any], schema_path: Path) -> None:
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(value), key=lambda item: item.json_path
    )
    require(
        not errors,
        f"schema_validation_failed:{errors[0].json_path}:{errors[0].message}" if errors else "",
    )


def expected_run_contracts() -> dict[str, tuple[str, str, int | None, int | None]]:
    contracts: dict[str, tuple[str, str, int | None, int | None]] = {}
    for browser in ("chromium", "firefox"):
        for repeat in range(1, 6):
            run_id = f"zero-loss-{browser}-{repeat:02d}"
            contracts[run_id] = (browser, "ZERO_LOSS", None, None)
        contracts[f"pli-recovery-{browser}"] = (browser, "PLI_RECOVERY", None, 901)
        for sequence in (65_534, 65_535, 65_536):
            run_id = f"rollover-loss-{browser}-{sequence}"
            contracts[run_id] = (browser, "ROLLOVER_LOSS", sequence, None)
    return contracts


def comparison_passes(comparison: dict[str, Any], tolerance: dict[str, Any]) -> bool:
    return bool(
        comparison["maximumAbsoluteChannelError"] <= tolerance["maximumAbsoluteChannelError"]
        and comparison["differingPixelFraction"] <= tolerance["maximumDifferingPixelFraction"]
        and comparison["rootMeanSquareChannelError"]
        <= tolerance["maximumRootMeanSquareChannelError"]
    )


def canonical_source_digest(zero_runs: list[dict[str, Any]]) -> str:
    normalized: list[dict[str, Any]] = []
    for run in zero_runs:
        normalized.append(
            {
                "browser": run["browser"],
                "comparisons": sorted(run["comparisons"], key=lambda item: item["key"]),
                "decoderErrors": run["decoderErrors"],
                "infrastructureStatus": run["infrastructureStatus"],
                "runId": run["runId"],
                "zeroLoss": run["zeroLoss"],
            }
        )
    encoded = json.dumps(normalized, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
    return hashlib.sha256(encoded).hexdigest()


def validate_m0_browser_matrix(
    matrix_directory: Path,
    fixture_directory: Path,
    matrix_schema: Path,
    playback_schema: Path,
    oracle_schema: Path,
) -> dict[str, Any]:
    summary = load_object(matrix_directory / "browser-matrix-summary.json")
    validate_schema(summary, matrix_schema)
    fixture = load_object(fixture_directory / "browser-fixture-summary.json")
    require(
        fixture.get("stream_sha256") == summary["fixtureStreamSha256"],
        "matrix_fixture_stream_identity_mismatch",
    )
    contracts = expected_run_contracts()
    declared = {run["runId"]: run for run in summary["runs"]}
    require(len(declared) == len(summary["runs"]), "matrix_run_id_duplicate")
    require(set(declared) == set(contracts), "matrix_run_set_invalid")

    dependency_lock = load_object(REPOSITORY_ROOT / "dependencies.lock.json")
    expected_versions = {
        browser: dependency_lock["playwright"][browser]["version"]
        for browser in ("chromium", "firefox")
    }
    reports: dict[str, dict[str, Any]] = {}
    binary_identities: dict[str, tuple[str, str]] = {}
    root = matrix_directory.resolve()
    for run_id, declaration in declared.items():
        browser, scenario, fault, pli = contracts[run_id]
        require(
            declaration["browser"] == browser
            and declaration["scenario"] == scenario
            and declaration["faultLossExtendedSequence"] == fault
            and declaration["injectPliAfterFrame"] == pli
            and declaration["oraclePassed"] is True,
            f"matrix_run_contract_invalid:{run_id}",
        )
        report_path = (matrix_directory / declaration["reportPath"]).resolve()
        require(report_path.is_relative_to(root), f"matrix_report_path_escape:{run_id}")
        require(
            report_path.is_file() and sha256_file(report_path) == declaration["reportSha256"],
            f"matrix_report_hash_invalid:{run_id}",
        )
        try:
            report = validate_browser_playback(report_path.parent, playback_schema)
        except BrowserPlaybackValidationError as error:
            raise BrowserMatrixValidationError(
                f"matrix_playback_invalid:{run_id}:{error}"
            ) from error
        reports[run_id] = report
        require(
            report["browser"] == browser
            and report["browserVersion"] == expected_versions[browser]
            and report["streamSha256"] == summary["fixtureStreamSha256"],
            f"matrix_browser_or_stream_identity_invalid:{run_id}",
        )
        identity = (report["browserVersion"], report["browserExecutableSha256"])
        if browser in binary_identities:
            require(
                binary_identities[browser] == identity, f"matrix_browser_binary_changed:{browser}"
            )
        else:
            binary_identities[browser] = identity
        sender = report["sender"]
        require(
            sender["requested_frames"] == 1_800
            and sender["sent_frames"] == 1_800
            and report["receiver"]["presentedFrames"] >= 1_440,
            f"matrix_duration_gate_failed:{run_id}",
        )
        require(
            (scenario != "ZERO_LOSS" or (sender["start_frame"] == 300 and fault is None))
            and (scenario != "PLI_RECOVERY" or sender["start_frame"] == 240)
            and (scenario != "ROLLOVER_LOSS" or sender["start_frame"] == 300),
            f"matrix_source_range_invalid:{run_id}",
        )

    zero_input = load_object(matrix_directory / "browser-oracle-zero-loss.json")
    frozen = load_object(matrix_directory / "browser-oracle-frozen.json")
    validate_schema(frozen, oracle_schema)
    require(frozen == summary["oracleTolerance"], "matrix_frozen_oracle_identity_mismatch")
    zero_run_ids = [run["runId"] for run in summary["runs"] if run["scenario"] == "ZERO_LOSS"]
    require(frozen["sourceRunIds"] == zero_run_ids, "matrix_oracle_source_run_order_invalid")
    require(
        zero_input.get("schemaVersion") == 1
        and zero_input.get("protocol") == "browser_oracle_zero_loss_v1"
        and isinstance(zero_input.get("runs"), list)
        and len(zero_input["runs"]) == 10,
        "matrix_zero_loss_input_invalid",
    )
    zero_by_id = {run["runId"]: run for run in zero_input["runs"]}
    require(set(zero_by_id) == set(zero_run_ids), "matrix_zero_loss_run_set_invalid")
    required_keys = sorted(
        reports[zero_run_ids[0]]["oracleComparisons"], key=lambda item: item["key"]
    )
    required_key_names = [item["key"] for item in required_keys]
    require(
        zero_input["requiredFrameKeys"] == required_key_names, "matrix_oracle_frame_keys_invalid"
    )
    ordered_zero_runs: list[dict[str, Any]] = []
    for run_id in zero_run_ids:
        source = zero_by_id[run_id]
        report = reports[run_id]
        require(
            source["browser"] == report["browser"]
            and source["zeroLoss"] is True
            and source["infrastructureStatus"] == "COMPLETE"
            and source["decoderErrors"] == 0
            and source["comparisons"] == report["oracleComparisons"],
            f"matrix_zero_loss_source_mismatch:{run_id}",
        )
        require(
            sorted(item["key"] for item in source["comparisons"]) == required_key_names,
            f"matrix_zero_loss_frame_set_incomplete:{run_id}",
        )
        ordered_zero_runs.append(source)
    require(
        canonical_source_digest(ordered_zero_runs) == frozen["sourceDigestSha256"],
        "matrix_oracle_source_digest_invalid",
    )
    zero_comparisons = [
        comparison for run in ordered_zero_runs for comparison in run["comparisons"]
    ]
    require(
        max(item["maximumAbsoluteChannelError"] for item in zero_comparisons)
        == frozen["maximumAbsoluteChannelError"]
        and max(item["differingPixelFraction"] for item in zero_comparisons)
        == frozen["maximumDifferingPixelFraction"]
        and max(item["rootMeanSquareChannelError"] for item in zero_comparisons)
        == frozen["maximumRootMeanSquareChannelError"],
        "matrix_oracle_tolerance_not_exact_maximum",
    )
    for run_id, report in reports.items():
        require(
            all(comparison_passes(item, frozen) for item in report["oracleComparisons"]),
            f"matrix_oracle_tolerance_failed:{run_id}",
        )
    actual_report_paths = {
        path.resolve()
        for path in (matrix_directory / "runs").glob("*/browser-playback-summary.json")
    }
    declared_report_paths = {
        (matrix_directory / declaration["reportPath"]).resolve() for declaration in summary["runs"]
    }
    require(actual_report_paths == declared_report_paths, "matrix_unexpected_or_missing_report")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--schemas", type=Path, default=Path("schemas"))
    arguments = parser.parse_args()
    try:
        summary = validate_m0_browser_matrix(
            arguments.matrix,
            arguments.fixture,
            arguments.schemas / "m0-browser-matrix-v1.schema.json",
            arguments.schemas / "m0-browser-playback-v1.schema.json",
            arguments.schemas / "browser-oracle-v1.schema.json",
        )
    except BrowserMatrixValidationError as error:
        print(json.dumps({"reason": str(error), "status": "FAILED"}, sort_keys=True))
        return 1
    print(json.dumps({"runs": len(summary["runs"]), "status": "PASSED"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
