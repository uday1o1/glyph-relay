from __future__ import annotations

import math
from collections.abc import Iterable
from dataclasses import dataclass


@dataclass(frozen=True)
class CurvePoint:
    measured_mbps: float
    character_error: float


def fit_nonincreasing(points: Iterable[CurvePoint]) -> list[CurvePoint]:
    ordered = sorted(points, key=lambda point: point.measured_mbps)
    if not ordered:
        raise ValueError("curve_requires_points")
    if any(
        not math.isfinite(point.measured_mbps)
        or point.measured_mbps <= 0
        or not math.isfinite(point.character_error)
        or point.character_error < 0
        or point.character_error > 1
        for point in ordered
    ):
        raise ValueError("curve_point_invalid")
    if any(
        left.measured_mbps == right.measured_mbps
        for left, right in zip(ordered, ordered[1:], strict=False)
    ):
        raise ValueError("curve_bitrate_duplicate")

    blocks: list[tuple[list[CurvePoint], float]] = []
    for point in ordered:
        blocks.append(([point], point.character_error))
        while len(blocks) >= 2 and blocks[-2][1] < blocks[-1][1]:
            right_points, _ = blocks.pop()
            left_points, _ = blocks.pop()
            merged = left_points + right_points
            blocks.append((merged, sum(item.character_error for item in merged) / len(merged)))
    fitted: list[CurvePoint] = []
    for block_points, value in blocks:
        fitted.extend(CurvePoint(point.measured_mbps, value) for point in block_points)
    return fitted


def interpolate_log_bitrate(points: Iterable[CurvePoint], target_mbps: float) -> float:
    fitted = fit_nonincreasing(points)
    if not math.isfinite(target_mbps) or target_mbps <= 0:
        raise ValueError("target_bitrate_invalid")
    for point in fitted:
        if point.measured_mbps == target_mbps:
            return point.character_error
    for left, right in zip(fitted, fitted[1:], strict=False):
        if left.measured_mbps < target_mbps < right.measured_mbps:
            fraction = (math.log(target_mbps) - math.log(left.measured_mbps)) / (
                math.log(right.measured_mbps) - math.log(left.measured_mbps)
            )
            return left.character_error + fraction * (right.character_error - left.character_error)
    raise ValueError("target_outside_measured_range")


def bitrate_at_error(points: Iterable[CurvePoint], target_error: float) -> float | None:
    fitted = fit_nonincreasing(points)
    if not math.isfinite(target_error) or target_error < 0 or target_error > 1:
        raise ValueError("target_error_invalid")
    for point in fitted:
        if point.character_error == target_error:
            return point.measured_mbps
    for left, right in zip(fitted, fitted[1:], strict=False):
        if left.character_error > target_error > right.character_error:
            fraction = (target_error - left.character_error) / (
                right.character_error - left.character_error
            )
            return math.exp(
                math.log(left.measured_mbps)
                + fraction * (math.log(right.measured_mbps) - math.log(left.measured_mbps))
            )
    return None
