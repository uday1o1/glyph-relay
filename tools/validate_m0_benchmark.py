from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator
from jsonschema.exceptions import ValidationError

CONDITIONS = ("controlled_uniform", "fixed_emphasis_level_4")
REPEATS = 10
FRAME_COUNT = 2_100
WARMUP_FRAMES = 300
MEASUREMENT_FRAMES = 1_800


class BenchmarkValidationError(RuntimeError):
    """Raised when a benchmark artifact cannot support the declared gate."""


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise BenchmarkValidationError(reason)


def finite_number(value: object, reason: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise BenchmarkValidationError(reason)
    converted = float(value)
    require(math.isfinite(converted), reason)
    return converted


def close(left: float, right: float) -> bool:
    return math.isclose(left, right, rel_tol=1e-12, abs_tol=1e-9)


def read_manifest_identity(path: Path) -> str:
    require(path.is_file() and not path.is_symlink(), "manifest_path_invalid")
    lines = path.read_bytes().splitlines(keepends=True)
    require(bool(lines) and lines[0] == b"glyphrelay-protocol-lock-v1\n", "manifest_magic_invalid")
    require(bool(lines) and lines[-1].endswith(b"\n"), "manifest_termination_invalid")
    final = lines[-1].removesuffix(b"\n").split(b"\t")
    require(len(final) == 2 and final[0] == b"manifest_sha256", "manifest_identity_invalid")
    try:
        identity = final[1].decode("ascii")
    except UnicodeDecodeError as error:
        raise BenchmarkValidationError("manifest_identity_invalid") from error
    require(
        len(identity) == 64 and all(character in "0123456789abcdef" for character in identity),
        "manifest_identity_invalid",
    )
    require(
        hashlib.sha256(b"".join(lines[:-1])).hexdigest() == identity,
        "manifest_self_hash_invalid",
    )
    return identity


def nearest_rank(values: list[float], probability: float) -> float:
    require(bool(values) and 0.0 < probability <= 1.0, "percentile_input_invalid")
    require(
        all(math.isfinite(value) and value >= 0.0 for value in values), "percentile_value_invalid"
    )
    ordered = sorted(values)
    rank = math.ceil(probability * len(ordered))
    return ordered[max(1, rank) - 1]


def read_tsv(path: Path, header: str, expected_rows: int) -> list[list[str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    require(bool(lines) and lines[0] == header, f"tsv_header_invalid:{path.name}")
    require(len(lines) == expected_rows + 1, f"tsv_row_count_invalid:{path.name}")
    return [line.split("\t") for line in lines[1:]]


def validate_configuration(path: Path, expected_sha256: str) -> None:
    value = load_json(path)
    require(isinstance(value, dict) and value.get("schema_version") == 1, "configuration_invalid")
    require(value.get("configuration_sha256") == expected_sha256, "configuration_identity_mismatch")
    initialize_hex = value.get("initialize_struct_hex")
    config_hex = value.get("config_struct_hex")
    require(
        isinstance(initialize_hex, str)
        and isinstance(config_hex, str)
        and len(initialize_hex) % 2 == 0
        and len(config_hex) % 2 == 0,
        "configuration_struct_encoding_invalid",
    )
    try:
        encoded = bytes.fromhex(initialize_hex) + bytes.fromhex(config_hex)
    except ValueError as error:
        raise BenchmarkValidationError("configuration_struct_encoding_invalid") from error
    require(hashlib.sha256(encoded).hexdigest() == expected_sha256, "configuration_hash_mismatch")
    require(value.get("filler_data_insertion") is True, "configuration_filler_disabled")
    require(
        value.get("vbv_initial_delay_bits") == value.get("vbv_buffer_bits"),
        "configuration_vbv_not_full",
    )


def validate_run(
    root: Path,
    condition: str,
    repeat: int,
    summary: dict[str, Any],
) -> dict[str, float]:
    stem = f"{condition}-repeat-{repeat:02d}"
    frame_rows = read_tsv(
        root / f"{stem}-frames.tsv",
        "frame_index\tbytes\tlatency_ms\tpending_count\toldest_pending_ms",
        FRAME_COUNT,
    )
    all_bytes = 0
    measurement_bytes = 0
    latencies: list[float] = []
    pending_counts: list[int] = []
    pending_ages: list[float] = []
    for frame_index, fields in enumerate(frame_rows):
        require(len(fields) == 5 and fields[0] == str(frame_index), f"frame_row_invalid:{stem}")
        try:
            byte_count = int(fields[1])
            latency = float(fields[2])
            pending_count = int(fields[3])
            pending_age = float(fields[4])
        except ValueError as error:
            raise BenchmarkValidationError(f"frame_row_invalid:{stem}") from error
        require(
            byte_count > 0
            and math.isfinite(latency)
            and latency >= 0.0
            and pending_count >= 0
            and math.isfinite(pending_age)
            and pending_age >= 0.0,
            f"frame_value_invalid:{stem}",
        )
        all_bytes += byte_count
        if frame_index >= WARMUP_FRAMES:
            measurement_bytes += byte_count
            latencies.append(latency)
            pending_counts.append(pending_count)
            pending_ages.append(pending_age)
    stream = root / f"{stem}.h264"
    require(stream.is_file() and not stream.is_symlink(), f"stream_missing:{stem}")
    require(stream.stat().st_size == all_bytes, f"stream_size_mismatch:{stem}")
    payload_bps = measurement_bytes * 8.0 * 30.0 / MEASUREMENT_FRAMES
    require(summary.get("measurement_bytes") == measurement_bytes, f"summary_bytes_mismatch:{stem}")
    require(
        close(finite_number(summary.get("payload_bps"), "summary_payload_invalid"), payload_bps),
        f"summary_payload_mismatch:{stem}",
    )

    quality_rows = read_tsv(
        root / f"{stem}-quality.tsv",
        "frame_index\twhole_squared_error\twhole_psnr_db\tprotected_squared_error\t"
        "protected_psnr_db\tcomparison_squared_error\tcomparison_psnr_db\t"
        "protected_minus_comparison_db",
        MEASUREMENT_FRAMES,
    )
    quality_values: list[tuple[float, float, float, float]] = []
    for offset, fields in enumerate(quality_rows):
        require(
            len(fields) == 8 and fields[0] == str(WARMUP_FRAMES + offset),
            f"quality_row_invalid:{stem}",
        )
        try:
            squared_errors = (int(fields[1]), int(fields[3]), int(fields[5]))
            values = (float(fields[2]), float(fields[4]), float(fields[6]), float(fields[7]))
        except ValueError as error:
            raise BenchmarkValidationError(f"quality_row_invalid:{stem}") from error
        require(
            all(error >= 0 for error in squared_errors)
            and all(math.isfinite(value) for value in values),
            f"quality_value_invalid:{stem}",
        )
        require(close(values[3], values[1] - values[2]), f"quality_allocation_mismatch:{stem}")
        quality_values.append(values)
    quality_means = tuple(
        sum(values[index] for values in quality_values) / MEASUREMENT_FRAMES for index in range(4)
    )
    summary_names = (
        "whole_frame_psnr_db",
        "protected_psnr_db",
        "comparison_psnr_db",
        "protected_minus_comparison_db",
    )
    for name, value in zip(summary_names, quality_means, strict=True):
        require(
            close(finite_number(summary.get(name), f"summary_quality_invalid:{name}"), value),
            f"summary_quality_mismatch:{stem}:{name}",
        )

    configuration_sha256 = summary.get("configuration_sha256")
    if not isinstance(configuration_sha256, str) or len(configuration_sha256) != 64:
        raise BenchmarkValidationError(f"summary_configuration_invalid:{stem}")
    configuration_path = root / f"{stem}-configuration.json"
    validate_configuration(configuration_path, configuration_sha256)
    configuration = load_json(configuration_path)
    expected_mode = "emphasis" if condition == "fixed_emphasis_level_4" else "disabled"
    require(
        configuration.get("qp_map_mode") == expected_mode, f"configuration_mode_mismatch:{stem}"
    )
    quarter = MEASUREMENT_FRAMES // 4
    return {
        "payload_bps": payload_bps,
        "latency_p95_ms": nearest_rank(latencies, 0.95),
        "latency_p99_ms": nearest_rank(latencies, 0.99),
        "maximum_pending_age_ms": max(pending_ages),
        "first_quarter_pending_mean": sum(pending_counts[:quarter]) / quarter,
        "last_quarter_pending_mean": sum(pending_counts[-quarter:]) / quarter,
        "whole_frame_psnr_db": quality_means[0],
        "protected_psnr_db": quality_means[1],
        "comparison_psnr_db": quality_means[2],
        "protected_minus_comparison_db": quality_means[3],
    }


def expected_files() -> set[str]:
    files = {"encoder-summary.json", "gate.json", "PASSED"}
    for condition in CONDITIONS:
        for repeat in range(1, REPEATS + 1):
            stem = f"{condition}-repeat-{repeat:02d}"
            files.update(
                {
                    f"{stem}.h264",
                    f"{stem}-frames.tsv",
                    f"{stem}-quality.tsv",
                    f"{stem}-configuration.json",
                }
            )
    return files


def validate_benchmark(root: Path, schemas: Path, expected_manifest_sha256: str) -> dict[str, Any]:
    require(not root.is_symlink(), "benchmark_root_invalid")
    directory = root.resolve(strict=True)
    require(directory.is_dir(), "benchmark_root_invalid")
    members = list(directory.iterdir())
    actual = {path.name for path in members}
    require(actual == expected_files(), "benchmark_member_set_mismatch")
    require(
        all(path.is_file() and not path.is_symlink() for path in members),
        "benchmark_member_type_invalid",
    )
    summary = load_json(directory / "encoder-summary.json")
    gate = load_json(directory / "gate.json")
    Draft202012Validator(load_json(schemas / "m0-benchmark-summary-v1.schema.json")).validate(
        summary
    )
    Draft202012Validator(load_json(schemas / "m0-benchmark-gate-v1.schema.json")).validate(gate)
    require(
        gate.get("status") == "PASSED" and gate.get("failures") == [], "benchmark_gate_not_passed"
    )
    require(
        summary.get("manifest_sha256") == expected_manifest_sha256, "benchmark_manifest_mismatch"
    )
    marker = (directory / "PASSED").read_text(encoding="utf-8")
    require(marker == f"{summary['manifest_sha256']}\n", "benchmark_completion_marker_mismatch")
    summary_conditions = summary.get("conditions")
    require(
        isinstance(summary_conditions, list)
        and [condition.get("name") for condition in summary_conditions] == list(CONDITIONS),
        "benchmark_condition_order_invalid",
    )
    observed: dict[str, list[dict[str, float]]] = {}
    for condition_name, condition in zip(CONDITIONS, summary_conditions, strict=True):
        calibration = condition.get("calibration")
        runs = condition.get("runs")
        require(
            isinstance(calibration, list) and len(calibration) == 8, "calibration_count_invalid"
        )
        require(isinstance(runs, list) and len(runs) == REPEATS, "benchmark_repeat_count_invalid")
        selected = condition.get("selected_requested_bps")
        require(
            isinstance(selected, int) and not isinstance(selected, bool), "selected_bitrate_invalid"
        )
        expected_selection = min(
            calibration,
            key=lambda point: (
                abs(
                    finite_number(point.get("measured_bps"), "calibration_measurement_invalid")
                    - 1_000_000.0
                ),
                point.get("requested_bps"),
            ),
        )
        require(
            expected_selection.get("requested_bps") == selected, "calibration_selection_invalid"
        )
        selected_hash = expected_selection.get("configuration_sha256")
        condition_runs: list[dict[str, float]] = []
        for repeat, run_summary in enumerate(runs, start=1):
            require(run_summary.get("repeat") == repeat, "benchmark_repeat_identity_invalid")
            require(
                run_summary.get("configuration_sha256") == selected_hash,
                "benchmark_configuration_drift",
            )
            condition_runs.append(validate_run(directory, condition_name, repeat, run_summary))
        observed[condition_name] = condition_runs

    uniform = observed[CONDITIONS[0]]
    fixed = observed[CONDITIONS[1]]
    gate_runs = gate.get("runs")
    require(isinstance(gate_runs, dict), "gate_runs_invalid")
    for condition, runs in observed.items():
        declared_runs = gate_runs.get(condition)
        require(isinstance(declared_runs, list), f"gate_runs_invalid:{condition}")
        for repeat, (run, declared) in enumerate(zip(runs, declared_runs, strict=True), start=1):
            require(declared.get("repeat") == repeat, f"gate_repeat_identity_invalid:{condition}")
            for name in (
                "payload_bps",
                "latency_p95_ms",
                "latency_p99_ms",
                "maximum_pending_age_ms",
                "first_quarter_pending_mean",
                "last_quarter_pending_mean",
            ):
                declared_value = finite_number(
                    declared.get(name), f"gate_run_value_invalid:{condition}:{repeat}:{name}"
                )
                require(
                    close(declared_value, run[name]),
                    f"gate_run_recalculation_mismatch:{condition}:{repeat}:{name}",
                )
    mean_payload = {
        CONDITIONS[0]: sum(run["payload_bps"] for run in uniform) / REPEATS,
        CONDITIONS[1]: sum(run["payload_bps"] for run in fixed) / REPEATS,
    }
    declared_mean_payload = gate.get("mean_payload_bps")
    require(isinstance(declared_mean_payload, dict), "gate_mean_payload_invalid")
    for condition, mean in mean_payload.items():
        require(
            close(
                finite_number(
                    declared_mean_payload.get(condition), f"gate_mean_payload_invalid:{condition}"
                ),
                mean,
            ),
            f"gate_mean_payload_recalculation_mismatch:{condition}",
        )
    for condition, runs in observed.items():
        for repeat, run in enumerate(runs, start=1):
            prefix = f"{condition}:repeat_{repeat}"
            require(run["latency_p95_ms"] <= 10.0, f"latency_p95_gate_failed:{prefix}")
            require(run["latency_p99_ms"] <= 16.0, f"latency_p99_gate_failed:{prefix}")
            require(run["maximum_pending_age_ms"] <= 33.34, f"pending_age_gate_failed:{prefix}")
            require(
                run["last_quarter_pending_mean"] <= run["first_quarter_pending_mean"],
                f"pending_trend_gate_failed:{prefix}",
            )
    payload_denominator = sum(mean_payload.values()) / 2.0
    payload_difference = (
        abs(mean_payload[CONDITIONS[1]] - mean_payload[CONDITIONS[0]]) / payload_denominator
    )
    protected_improvement = (
        sum(run["protected_psnr_db"] for run in fixed) / REPEATS
        - sum(run["protected_psnr_db"] for run in uniform) / REPEATS
    )
    allocation_improvement = (
        sum(run["protected_minus_comparison_db"] for run in fixed) / REPEATS
        - sum(run["protected_minus_comparison_db"] for run in uniform) / REPEATS
    )
    for condition, payload_bps in mean_payload.items():
        require(980_000.0 <= payload_bps <= 1_020_000.0, f"mean_payload_gate_failed:{condition}")
    require(payload_difference <= 0.02, "between_condition_payload_gate_failed")
    require(protected_improvement >= 1.0, "protected_psnr_gate_failed")
    require(allocation_improvement >= 0.75, "spatial_allocation_gate_failed")
    require(
        close(
            finite_number(
                gate.get("between_condition_payload_difference_fraction"), "gate_payload_invalid"
            ),
            payload_difference,
        ),
        "gate_payload_recalculation_mismatch",
    )
    require(
        close(
            finite_number(gate.get("protected_psnr_improvement_db"), "gate_quality_invalid"),
            protected_improvement,
        ),
        "gate_protected_recalculation_mismatch",
    )
    require(
        close(
            finite_number(gate.get("spatial_allocation_improvement_db"), "gate_allocation_invalid"),
            allocation_improvement,
        ),
        "gate_allocation_recalculation_mismatch",
    )
    return {
        "schema_version": 1,
        "status": "PASSED",
        "manifest_sha256": summary["manifest_sha256"],
        "mean_payload_bps": mean_payload,
        "between_condition_payload_difference_fraction": payload_difference,
        "protected_psnr_improvement_db": protected_improvement,
        "spatial_allocation_improvement_db": allocation_improvement,
    }


def write_exclusive(path: Path, value: dict[str, Any]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def main() -> int:
    parser = argparse.ArgumentParser(description="Independently validate m0_fixed_map_v1 results")
    parser.add_argument("benchmark", type=Path)
    parser.add_argument("--schemas", type=Path, default=Path("schemas"))
    parser.add_argument("--manifest", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        result = validate_benchmark(
            arguments.benchmark,
            arguments.schemas.resolve(strict=True),
            read_manifest_identity(arguments.manifest),
        )
        write_exclusive(arguments.benchmark / "validation.json", result)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (
        BenchmarkValidationError,
        OSError,
        ValueError,
        json.JSONDecodeError,
        ValidationError,
    ) as error:
        print(f"benchmark validation failed: {error}")
        return 8


if __name__ == "__main__":
    raise SystemExit(main())
