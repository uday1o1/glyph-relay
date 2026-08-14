from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

import pytest

from tools.corpus.uniform_aq_selector import AqFields
from tools.run_uniform_aq_development import (
    canonical_json,
    condition_id,
    effective_encoder_fields,
    effective_encoder_fields_sha256,
    next_requested_rate,
    run_target_search,
)


def _native(measured: float) -> dict[str, Any]:
    return {
        "height": 1080,
        "maximumPendingAgeMs": 6.0,
        "meanSenderCpuPercent": 15.0,
        "measuredPayloadMbps": measured,
        "p95EncodeMs": 4.0,
        "p95PreprocessEncodeMs": 6.0,
        "p95PreprocessMs": 2.0,
        "p99EncodeMs": 5.0,
        "pendingPositiveTrend": False,
        "submittedFrames": 15_360,
        "width": 1920,
    }


def _verification() -> dict[str, Any]:
    per_stratum = {
        "animated_typing_scrolling": 0.1,
        "browser_documentation": 0.1,
        "code_editor": 0.1,
        "mixed_video_text": 0.1,
        "slide_diagram": 0.1,
        "spreadsheet_table": 0.1,
        "terminal": 0.1,
    }
    return {
        "browserDecodePassed": True,
        "decodedFrames": 15_360,
        "equalStratumCer": 0.1,
        "independentDecodePassed": True,
        "perStratumCer": per_stratum,
    }


def test_effective_encoder_identity_is_canonical_and_rate_independent() -> None:
    fields = AqFields(True, 12, True)
    expanded = effective_encoder_fields(fields)
    assert expanded["aqFields"] == fields.json()
    assert expanded["rateControl"] == "cbr_development_search_variable"
    assert (
        effective_encoder_fields_sha256(fields)
        == hashlib.sha256(canonical_json(expanded)).hexdigest()
    )
    assert condition_id(AqFields(False, 0, False)) == "controlled_uniform"


def test_rate_search_scales_request_and_avoids_an_existing_rate() -> None:
    assert next_requested_rate(1_000_000, 0.8, 1.0, {1_000_000}) == 1_250_000
    assert next_requested_rate(1_000_000, 1.0, 1.0, {1_000_000}) == 999_999


def test_target_search_rate_matches_then_reuses_checkpoint(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    observed_requests: list[int] = []

    def fake_native(**kwargs: Any) -> tuple[dict[str, Any], None, Path]:
        requested = int(kwargs["requested_bps"])
        observed_requests.append(requested)
        native_output = Path(kwargs["trial_root"]) / "native-fake"
        native_output.mkdir(parents=True)
        return _native(0.8 if len(observed_requests) == 1 else 1.0), None, native_output

    monkeypatch.setattr("tools.run_uniform_aq_development.run_native_trial", fake_native)
    monkeypatch.setattr(
        "tools.run_uniform_aq_development.verify_matched_trial",
        lambda *_args, **_kwargs: _verification(),
    )
    root = tmp_path / "target"
    result = run_target_search(
        native=tmp_path / "native",
        bundle=tmp_path / "bundle",
        bundle_sha256="a" * 64,
        root=root,
        fields=AqFields(True, 4, False),
        target=1.0,
        identities={},
    )
    assert observed_requests == [1_000_000, 1_250_000]
    assert result["status"] == "PASSED"
    assert result["selectedAttemptIndex"] == 1
    assert (root / "target.json").is_file()

    monkeypatch.setattr(
        "tools.run_uniform_aq_development.run_native_trial",
        lambda **_kwargs: pytest.fail("checkpoint was not reused"),
    )
    assert (
        run_target_search(
            native=tmp_path / "native",
            bundle=tmp_path / "bundle",
            bundle_sha256="a" * 64,
            root=root,
            fields=AqFields(True, 4, False),
            target=1.0,
            identities={},
        )
        == result
    )


def test_invalid_native_trial_retries_same_requested_rate(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    calls = 0

    def fake_native(**kwargs: Any) -> tuple[dict[str, Any] | None, str | None, Path]:
        nonlocal calls
        calls += 1
        native_output = Path(kwargs["trial_root"]) / "native-fake"
        native_output.mkdir(parents=True)
        if calls == 1:
            return None, "native_exit_8", native_output
        return _native(0.5), None, native_output

    monkeypatch.setattr("tools.run_uniform_aq_development.run_native_trial", fake_native)
    monkeypatch.setattr(
        "tools.run_uniform_aq_development.verify_matched_trial",
        lambda *_args, **_kwargs: _verification(),
    )
    result = run_target_search(
        native=tmp_path / "native",
        bundle=tmp_path / "bundle",
        bundle_sha256="a" * 64,
        root=tmp_path / "retry-target",
        fields=AqFields(False, 0, False),
        target=0.5,
        identities={},
    )
    assert [attempt["requestedPayloadBps"] for attempt in result["attempts"]] == [
        500_000,
        500_000,
    ]
    assert [attempt["status"] for attempt in result["attempts"]] == [
        "INVALID",
        "PASSED",
    ]
    assert result["selectedAttemptIndex"] == 1
