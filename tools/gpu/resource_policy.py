from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

MAXIMUM_TEMPERATURE_C = 83.0
MINIMUM_ACTIVE_VIDEO_CLOCK_FRACTION = 0.75
MINIMUM_ACTIVE_SM_CLOCK_FRACTION = 0.75


class ResourcePolicyError(ValueError):
    """Raised when target telemetry cannot support a performance result."""


@dataclass(frozen=True)
class GpuMetrics:
    gpu_utilization_percent: float
    encoder_utilization_percent: float
    memory_used_mib: float
    sm_clock_mhz: float
    video_clock_mhz: float
    temperature_c: float
    power_watts: float
    throttle_mask: int
    volatile_uncorrected_ecc_errors: int


def finite_nonnegative(raw: str, label: str) -> float:
    try:
        value = float(raw.strip())
    except ValueError as error:
        raise ResourcePolicyError(f"resource_metric_invalid:{label}") from error
    if not math.isfinite(value) or value < 0.0:
        raise ResourcePolicyError(f"resource_metric_invalid:{label}")
    return value


def parse_gpu_metrics(output: str, expected_uuid: str) -> GpuMetrics:
    lines = [line for line in output.splitlines() if line.strip()]
    if len(lines) != 1:
        raise ResourcePolicyError("resource_gpu_row_count_invalid")
    fields = [field.strip() for field in lines[0].split(",")]
    if len(fields) != 11 or fields[1] != expected_uuid:
        raise ResourcePolicyError("resource_gpu_row_invalid")
    try:
        throttle_mask = int(fields[9], 0)
        ecc_errors = int(fields[10])
    except ValueError as error:
        raise ResourcePolicyError("resource_gpu_integer_metric_invalid") from error
    if throttle_mask < 0 or ecc_errors < 0:
        raise ResourcePolicyError("resource_gpu_integer_metric_invalid")
    return GpuMetrics(
        gpu_utilization_percent=finite_nonnegative(fields[2], "gpu_utilization"),
        encoder_utilization_percent=finite_nonnegative(fields[3], "encoder_utilization"),
        memory_used_mib=finite_nonnegative(fields[4], "memory_used"),
        sm_clock_mhz=finite_nonnegative(fields[5], "sm_clock"),
        video_clock_mhz=finite_nonnegative(fields[6], "video_clock"),
        temperature_c=finite_nonnegative(fields[7], "temperature"),
        power_watts=finite_nonnegative(fields[8], "power"),
        throttle_mask=throttle_mask,
        volatile_uncorrected_ecc_errors=ecc_errors,
    )


def metrics_json(metrics: GpuMetrics) -> dict[str, float | int]:
    return {
        "gpu_utilization_percent": metrics.gpu_utilization_percent,
        "encoder_utilization_percent": metrics.encoder_utilization_percent,
        "memory_used_mib": metrics.memory_used_mib,
        "sm_clock_mhz": metrics.sm_clock_mhz,
        "video_clock_mhz": metrics.video_clock_mhz,
        "temperature_c": metrics.temperature_c,
        "power_watts": metrics.power_watts,
        "throttle_mask": metrics.throttle_mask,
        "volatile_uncorrected_ecc_errors": metrics.volatile_uncorrected_ecc_errors,
    }


def sample_number(sample: dict[str, Any], name: str) -> float:
    value = sample.get("metrics", {}).get(name)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ResourcePolicyError(f"resource_sample_metric_invalid:{name}")
    converted = float(value)
    if not math.isfinite(converted) or converted < 0.0:
        raise ResourcePolicyError(f"resource_sample_metric_invalid:{name}")
    return converted


def evaluate_nvenc_performance_samples(samples: Sequence[dict[str, Any]]) -> dict[str, Any]:
    return evaluate_performance_samples(
        samples,
        policy="nvenc-performance-v1",
        utilization_name="encoder_utilization_percent",
        clock_name="video_clock_mhz",
        clock_fraction_name="minimum_active_video_clock_fraction",
        minimum_clock_fraction=MINIMUM_ACTIVE_VIDEO_CLOCK_FRACTION,
        activity_violation="nvenc_activity_not_observed",
        clock_violation="active_video_clock_unstable",
    )


def evaluate_cuda_performance_samples(samples: Sequence[dict[str, Any]]) -> dict[str, Any]:
    return evaluate_performance_samples(
        samples,
        policy="cuda-performance-v1",
        utilization_name="gpu_utilization_percent",
        clock_name="sm_clock_mhz",
        clock_fraction_name="minimum_active_sm_clock_fraction",
        minimum_clock_fraction=MINIMUM_ACTIVE_SM_CLOCK_FRACTION,
        activity_violation="cuda_activity_not_observed",
        clock_violation="active_sm_clock_unstable",
    )


def evaluate_performance_samples(
    samples: Sequence[dict[str, Any]],
    *,
    policy: str,
    utilization_name: str,
    clock_name: str,
    clock_fraction_name: str,
    minimum_clock_fraction: float,
    activity_violation: str,
    clock_violation: str,
) -> dict[str, Any]:
    violations: list[str] = []
    if len(samples) < 2:
        violations.append("resource_sample_count_below_two")
    valid_samples = [
        sample
        for sample in samples
        if sample.get("gpu_query_passed") is True and isinstance(sample.get("metrics"), dict)
    ]
    if len(valid_samples) != len(samples):
        violations.append("gpu_telemetry_incomplete")
    if any(sample.get("process_query_passed") is not True for sample in samples):
        violations.append("process_telemetry_incomplete")
    if any(sample.get("xid_query_passed") is not True for sample in samples):
        violations.append("xid_telemetry_incomplete")
    if any(sample.get("foreign_compute_processes") for sample in samples):
        violations.append("foreign_compute_process_detected")
    if any(sample.get("xid_events") for sample in samples):
        violations.append("xid_event_detected")

    temperatures: list[float] = []
    ecc_values: list[int] = []
    active_clocks: list[float] = []
    for sample in valid_samples:
        temperatures.append(sample_number(sample, "temperature_c"))
        ecc_value = sample_number(sample, "volatile_uncorrected_ecc_errors")
        ecc_values.append(int(ecc_value))
        utilization = sample_number(sample, utilization_name)
        if utilization >= 1.0:
            active_clocks.append(sample_number(sample, clock_name))
            if int(sample_number(sample, "throttle_mask")) != 0:
                violations.append("active_clock_throttle_detected")
    if temperatures and max(temperatures) > MAXIMUM_TEMPERATURE_C:
        violations.append("temperature_limit_exceeded")
    if ecc_values and max(ecc_values) != min(ecc_values):
        violations.append("volatile_uncorrected_ecc_increased")
    if not active_clocks:
        violations.append(activity_violation)
    elif min(active_clocks) <= 0.0 or min(active_clocks) < (
        max(active_clocks) * minimum_clock_fraction
    ):
        violations.append(clock_violation)
    unique_violations = sorted(set(violations))
    return {
        "schema_version": 1,
        "policy": policy,
        "status": "PASSED" if not unique_violations else "FAILED",
        "sample_count": len(samples),
        "active_sample_count": len(active_clocks),
        "limits": {
            "maximum_temperature_c": MAXIMUM_TEMPERATURE_C,
            clock_fraction_name: minimum_clock_fraction,
            "allowed_active_throttle_mask": 0,
            "maximum_foreign_compute_processes": 0,
            "maximum_xid_events": 0,
            "maximum_volatile_uncorrected_ecc_increase": 0,
        },
        "observed": {
            "maximum_temperature_c": max(temperatures) if temperatures else None,
            f"minimum_active_{clock_name}": min(active_clocks) if active_clocks else None,
            f"maximum_active_{clock_name}": max(active_clocks) if active_clocks else None,
            "minimum_volatile_uncorrected_ecc_errors": min(ecc_values) if ecc_values else None,
            "maximum_volatile_uncorrected_ecc_errors": max(ecc_values) if ecc_values else None,
        },
        "violations": unique_violations,
    }
