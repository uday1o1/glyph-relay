from copy import deepcopy

import pytest

from tools.gpu.resource_policy import (
    ResourcePolicyError,
    evaluate_cuda_performance_samples,
    evaluate_nvenc_performance_samples,
    parse_gpu_metrics,
)

GPU_UUID = "GPU-00000000-0000-0000-0000-000000000000"


def sample(*, active: bool) -> dict[str, object]:
    return {
        "gpu_query_passed": True,
        "process_query_passed": True,
        "xid_query_passed": True,
        "foreign_compute_processes": [],
        "xid_events": [],
        "metrics": {
            "gpu_utilization_percent": 5.0,
            "encoder_utilization_percent": 40.0 if active else 0.0,
            "memory_used_mib": 512.0,
            "sm_clock_mhz": 1_200.0,
            "video_clock_mhz": 900.0,
            "temperature_c": 70.0,
            "power_watts": 100.0,
            "throttle_mask": 0,
            "volatile_uncorrected_ecc_errors": 5,
        },
    }


def test_parses_complete_nvidia_smi_resource_row() -> None:
    metrics = parse_gpu_metrics(
        "2026/08/13 12:00:00.000, "
        f"{GPU_UUID}, 5, 40, 512, 1200, 900, 70, 100.5, 0x0000000000000000, 5\n",
        GPU_UUID,
    )
    assert metrics.encoder_utilization_percent == 40.0
    assert metrics.throttle_mask == 0
    assert metrics.volatile_uncorrected_ecc_errors == 5


def test_rejects_incomplete_nvidia_smi_resource_row() -> None:
    with pytest.raises(ResourcePolicyError, match="resource_gpu_row_invalid"):
        parse_gpu_metrics(f"timestamp, {GPU_UUID}, 5\n", GPU_UUID)


def test_clean_nvenc_performance_samples_pass_frozen_limits() -> None:
    result = evaluate_nvenc_performance_samples([sample(active=False), sample(active=True)])
    assert result["status"] == "PASSED"
    assert result["violations"] == []


def test_seeded_resource_contamination_fails_for_every_intended_reason() -> None:
    samples = [sample(active=True), sample(active=True)]
    samples[0]["foreign_compute_processes"] = [{"pid": 9}]
    samples[0]["xid_events"] = ["NVRM: Xid 31"]
    first_metrics = samples[0]["metrics"]
    second_metrics = samples[1]["metrics"]
    assert isinstance(first_metrics, dict) and isinstance(second_metrics, dict)
    first_metrics["temperature_c"] = 84.0
    first_metrics["throttle_mask"] = 4
    first_metrics["video_clock_mhz"] = 1_000.0
    second_metrics["video_clock_mhz"] = 700.0
    second_metrics["volatile_uncorrected_ecc_errors"] = 6
    result = evaluate_nvenc_performance_samples(samples)
    assert result["status"] == "FAILED"
    assert set(result["violations"]) == {
        "active_clock_throttle_detected",
        "active_video_clock_unstable",
        "foreign_compute_process_detected",
        "temperature_limit_exceeded",
        "volatile_uncorrected_ecc_increased",
        "xid_event_detected",
    }


def test_missing_telemetry_and_absent_nvenc_activity_fail_closed() -> None:
    samples = [sample(active=False), sample(active=False)]
    incomplete = deepcopy(samples[1])
    incomplete["gpu_query_passed"] = False
    incomplete["process_query_passed"] = False
    incomplete["xid_query_passed"] = False
    result = evaluate_nvenc_performance_samples([samples[0], incomplete])
    assert result["status"] == "FAILED"
    assert set(result["violations"]) == {
        "gpu_telemetry_incomplete",
        "nvenc_activity_not_observed",
        "process_telemetry_incomplete",
        "xid_telemetry_incomplete",
    }


def test_clean_cuda_performance_samples_pass_frozen_limits() -> None:
    samples = [sample(active=False), sample(active=False)]
    metrics = samples[1]["metrics"]
    assert isinstance(metrics, dict)
    metrics["gpu_utilization_percent"] = 80.0
    result = evaluate_cuda_performance_samples(samples)
    assert result["status"] == "PASSED"
    assert result["policy"] == "cuda-performance-v1"
    assert result["violations"] == []


def test_seeded_cuda_activity_and_sm_clock_defects_fail_closed() -> None:
    samples = [sample(active=False), sample(active=False)]
    for item in samples:
        metrics = item["metrics"]
        assert isinstance(metrics, dict)
        metrics["gpu_utilization_percent"] = 0.0
    absent = evaluate_cuda_performance_samples(samples)
    assert absent["status"] == "FAILED"
    assert "cuda_activity_not_observed" in absent["violations"]

    first_metrics = samples[0]["metrics"]
    second_metrics = samples[1]["metrics"]
    assert isinstance(first_metrics, dict) and isinstance(second_metrics, dict)
    first_metrics["gpu_utilization_percent"] = 80.0
    second_metrics["gpu_utilization_percent"] = 80.0
    first_metrics["sm_clock_mhz"] = 1_500.0
    second_metrics["sm_clock_mhz"] = 1_000.0
    unstable = evaluate_cuda_performance_samples(samples)
    assert unstable["status"] == "FAILED"
    assert unstable["violations"] == ["active_sm_clock_unstable"]
