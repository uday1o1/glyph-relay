import hashlib
import json
from pathlib import Path
from typing import Any

import pytest

from tools.validate_m0_benchmark import BenchmarkValidationError, validate_benchmark

CONDITIONS = ("controlled_uniform", "fixed_emphasis_level_4")


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def create_valid_benchmark(root: Path) -> None:
    root.mkdir()
    configuration_bytes = b"\x00\x01"
    configuration_sha256 = hashlib.sha256(configuration_bytes).hexdigest()
    conditions: list[dict[str, Any]] = []
    gate_runs: dict[str, list[dict[str, Any]]] = {}
    requested_values = [
        800_000,
        900_000,
        950_000,
        975_000,
        1_000_000,
        1_025_000,
        1_050_000,
        1_100_000,
    ]
    for condition in CONDITIONS:
        is_emphasis = condition == "fixed_emphasis_level_4"
        protected = 31.5 if is_emphasis else 30.0
        comparison = 30.0
        allocation = protected - comparison
        runs: list[dict[str, Any]] = []
        declared_gate_runs: list[dict[str, Any]] = []
        for repeat in range(1, 11):
            stem = f"{condition}-repeat-{repeat:02d}"
            frame_lines = ["frame_index\tbytes\tlatency_ms\tpending_count\toldest_pending_ms"]
            total_bytes = 0
            measurement_bytes = 0
            for frame_index in range(2_100):
                if frame_index < 300:
                    byte_count = 1
                elif frame_index < 1_500:
                    byte_count = 4_167
                else:
                    byte_count = 4_166
                total_bytes += byte_count
                if frame_index >= 300:
                    measurement_bytes += byte_count
                frame_lines.append(f"{frame_index}\t{byte_count}\t1.0\t1\t0.0")
            (root / f"{stem}-frames.tsv").write_text(
                "\n".join(frame_lines) + "\n", encoding="utf-8"
            )
            with (root / f"{stem}.h264").open("wb") as stream:
                stream.truncate(total_bytes)
            quality_lines = [
                "frame_index\twhole_squared_error\twhole_psnr_db\tprotected_squared_error\t"
                "protected_psnr_db\tcomparison_squared_error\tcomparison_psnr_db\t"
                "protected_minus_comparison_db"
            ]
            for frame_index in range(300, 2_100):
                quality_lines.append(
                    f"{frame_index}\t1\t30.5\t1\t{protected}\t1\t{comparison}\t{allocation}"
                )
            (root / f"{stem}-quality.tsv").write_text(
                "\n".join(quality_lines) + "\n", encoding="utf-8"
            )
            write_json(
                root / f"{stem}-configuration.json",
                {
                    "schema_version": 1,
                    "requested_bps": 1_000_000,
                    "vbv_buffer_bits": 33_333,
                    "vbv_initial_delay_bits": 33_333,
                    "filler_data_insertion": True,
                    "qp_map_mode": "emphasis" if is_emphasis else "disabled",
                    "initialize_struct_hex": "00",
                    "config_struct_hex": "01",
                    "configuration_sha256": configuration_sha256,
                },
            )
            runs.append(
                {
                    "repeat": repeat,
                    "measurement_bytes": measurement_bytes,
                    "payload_bps": 1_000_000.0,
                    "configuration_sha256": configuration_sha256,
                    "whole_frame_psnr_db": 30.5,
                    "protected_psnr_db": protected,
                    "comparison_psnr_db": comparison,
                    "protected_minus_comparison_db": allocation,
                }
            )
            declared_gate_runs.append(
                {
                    "repeat": repeat,
                    "payload_bps": 1_000_000.0,
                    "latency_p95_ms": 1.0,
                    "latency_p99_ms": 1.0,
                    "maximum_pending_age_ms": 0.0,
                    "first_quarter_pending_mean": 1.0,
                    "last_quarter_pending_mean": 1.0,
                }
            )
        conditions.append(
            {
                "name": condition,
                "selected_requested_bps": 1_000_000,
                "calibration": [
                    {
                        "requested_bps": requested,
                        "measured_bps": float(requested),
                        "configuration_sha256": configuration_sha256,
                    }
                    for requested in requested_values
                ],
                "runs": runs,
            }
        )
        gate_runs[condition] = declared_gate_runs
    manifest_sha256 = "a" * 64
    write_json(
        root / "encoder-summary.json",
        {
            "schema_version": 1,
            "protocol": "m0_fixed_map_v1",
            "manifest_sha256": manifest_sha256,
            "conditions": conditions,
        },
    )
    write_json(
        root / "gate.json",
        {
            "schema_version": 1,
            "status": "PASSED",
            "mean_payload_bps": dict.fromkeys(CONDITIONS, 1000000.0),
            "between_condition_payload_difference_fraction": 0.0,
            "protected_psnr_improvement_db": 1.5,
            "spatial_allocation_improvement_db": 1.5,
            "failures": [],
            "runs": gate_runs,
        },
    )
    (root / "PASSED").write_text(manifest_sha256 + "\n", encoding="utf-8")


def test_full_benchmark_artifact_set_passes_independent_recalculation(tmp_path: Path) -> None:
    benchmark = tmp_path / "benchmark"
    create_valid_benchmark(benchmark)
    result = validate_benchmark(benchmark, Path("schemas").resolve(strict=True), "a" * 64)
    assert result["status"] == "PASSED"
    assert result["protected_psnr_improvement_db"] == 1.5


def test_tampered_stream_size_fails_for_intended_reason(tmp_path: Path) -> None:
    benchmark = tmp_path / "benchmark"
    create_valid_benchmark(benchmark)
    (benchmark / "controlled_uniform-repeat-01.h264").write_bytes(b"bad")
    with pytest.raises(BenchmarkValidationError, match="stream_size_mismatch"):
        validate_benchmark(benchmark, Path("schemas").resolve(strict=True), "a" * 64)


def test_false_gate_measurement_fails_for_intended_reason(tmp_path: Path) -> None:
    benchmark = tmp_path / "benchmark"
    create_valid_benchmark(benchmark)
    gate_path = benchmark / "gate.json"
    gate = json.loads(gate_path.read_text(encoding="utf-8"))
    gate["runs"]["controlled_uniform"][0]["latency_p99_ms"] = 0.5
    write_json(gate_path, gate)
    with pytest.raises(BenchmarkValidationError, match="gate_run_recalculation_mismatch"):
        validate_benchmark(benchmark, Path("schemas").resolve(strict=True), "a" * 64)


def test_wrong_condition_map_mode_fails_for_intended_reason(tmp_path: Path) -> None:
    benchmark = tmp_path / "benchmark"
    create_valid_benchmark(benchmark)
    configuration_path = benchmark / "fixed_emphasis_level_4-repeat-01-configuration.json"
    configuration = json.loads(configuration_path.read_text(encoding="utf-8"))
    configuration["qp_map_mode"] = "disabled"
    write_json(configuration_path, configuration)
    with pytest.raises(BenchmarkValidationError, match="configuration_mode_mismatch"):
        validate_benchmark(benchmark, Path("schemas").resolve(strict=True), "a" * 64)
