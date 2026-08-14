from __future__ import annotations

import pytest

from tools.corpus.aq_selector import (
    CurvePoint,
    bitrate_at_error,
    fit_nonincreasing,
    interpolate_log_bitrate,
)


def test_pool_adjacent_violators_is_nonincreasing() -> None:
    fitted = fit_nonincreasing(
        [
            CurvePoint(0.5, 0.5),
            CurvePoint(1.0, 0.3),
            CurvePoint(2.0, 0.35),
            CurvePoint(4.0, 0.1),
        ]
    )
    assert [point.character_error for point in fitted] == pytest.approx([0.5, 0.325, 0.325, 0.1])


def test_interpolation_uses_log_bitrate_and_never_extrapolates() -> None:
    points = [CurvePoint(1.0, 0.4), CurvePoint(4.0, 0.0)]
    assert interpolate_log_bitrate(points, 2.0) == pytest.approx(0.2)
    assert bitrate_at_error(points, 0.2) == pytest.approx(2.0)
    with pytest.raises(ValueError, match="outside"):
        interpolate_log_bitrate(points, 0.5)
    assert bitrate_at_error(points, 0.8) is None


def test_curve_rejects_duplicate_or_invalid_points() -> None:
    with pytest.raises(ValueError, match="duplicate"):
        fit_nonincreasing([CurvePoint(1.0, 0.2), CurvePoint(1.0, 0.1)])
    with pytest.raises(ValueError, match="invalid"):
        fit_nonincreasing([CurvePoint(0.0, 0.2)])
