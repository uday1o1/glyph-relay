from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
from typing import Any

import pytest

from tools.corpus.uniform_aq_selector import (
    CORPUS_LOCK_PATH,
    DEVELOPMENT_MANIFEST_PATH,
    GRID_PATH,
    AqFields,
    SelectorInputs,
    UniformAqSelectionError,
    canonical_json,
    comparator_identity,
    expected_aq_fields,
    load_object,
    select_development_evidence,
    sha256_file,
    write_exclusive,
)


def _target(target: float, error: float) -> dict[str, Any]:
    requested = int(target * 1_000_000)
    return {
        "targetPayloadMbps": target,
        "attempts": [
            {
                "requestedPayloadBps": requested,
                "measuredPayloadMbps": target,
                "status": "PASSED",
                "invalidReason": None,
            }
        ],
        "selectedAttemptIndex": 0,
        "status": "PASSED",
        "invalidReasons": [],
        "perStratumCer": {
            "animated_typing_scrolling": error,
            "browser_documentation": error,
            "code_editor": error,
            "mixed_video_text": error,
            "slide_diagram": error,
            "spreadsheet_table": error,
            "terminal": error,
        },
        "equalStratumCer": error,
        "p95PreprocessMs": 2.0,
        "p95PreprocessEncodeMs": 7.0,
        "p95EncodeMs": 5.0,
        "p99EncodeMs": 8.0,
        "meanSenderCpuPercent": 12.0,
        "submittedFrames": 300,
        "decodedFrames": 300,
        "width": 1920,
        "height": 1080,
        "pendingPositiveTrend": False,
        "maximumPendingAgeMs": 9.0,
        "independentDecodePassed": True,
        "browserDecodePassed": True,
    }


def _condition(fields: AqFields, errors: list[float], index: int) -> dict[str, Any]:
    identity = (
        "controlled_uniform" if fields == AqFields(False, 0, False) else fields.candidate_id()
    )
    effective = {
        "aq": fields.json(),
        "fixedEncoderFields": "recording_profile_v1",
        "testIndex": index,
    }
    return {
        "candidateId": identity,
        "effectiveAqFields": fields.json(),
        "effectiveEncoderFieldsSha256": hashlib.sha256(canonical_json(effective)).hexdigest(),
        "status": "PASSED",
        "invalidReasons": [],
        "targetSearch": [
            _target(target, error)
            for target, error in zip((0.5, 0.75, 1.0, 2.0, 4.0), errors, strict=True)
        ],
        "p95PreprocessEncodeMs": 7.0 + index / 100.0,
        "meanSenderCpuPercent": 12.0 + index / 100.0,
    }


def _evidence() -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    grid = load_object(GRID_PATH)
    lock = {
        "schema_version": 1,
        "protocol": "uniform_aq_v1",
        "protocol_sha256": "a" * 64,
        "files": [
            {
                "path": "tools/corpus/uniform_aq_selector.py",
                "sha256": "b" * 64,
            }
        ],
    }
    candidates = []
    selected = AqFields(True, 4, False)
    for index, fields in enumerate(expected_aq_fields(), start=1):
        errors = [0.46, 0.38, 0.31, 0.19, 0.09]
        if fields == selected:
            errors = [0.39, 0.29, 0.20, 0.10, 0.05]
        candidates.append(_condition(fields, errors, index))
    corpus_lock = load_object(CORPUS_LOCK_PATH)
    evidence = {
        "schemaVersion": 1,
        "protocol": "uniform_aq_v1",
        "split": "development",
        "corpusProtocolSha256": corpus_lock["protocol_sha256"],
        "developmentManifestSha256": sha256_file(DEVELOPMENT_MANIFEST_PATH),
        "gridSha256": sha256_file(GRID_PATH),
        "protocolSha256": lock["protocol_sha256"],
        "processingPlatformSha256": "c" * 64,
        "implementationSha256": "d" * 64,
        "controlledUniform": _condition(
            AqFields(False, 0, False), [0.55, 0.47, 0.40, 0.22, 0.10], 0
        ),
        "candidates": candidates,
    }
    return evidence, grid, lock


def test_grid_enumeration_and_selection_are_exact_and_deterministic() -> None:
    fields = expected_aq_fields()
    assert len(fields) == 11
    assert AqFields(False, 0, True) in fields
    assert AqFields(False, 0, False) not in fields

    evidence, grid, lock = _evidence()
    first = select_development_evidence(evidence, grid, lock)
    second = select_development_evidence(copy.deepcopy(evidence), grid, lock)
    assert canonical_json(first) == canonical_json(second)
    assert first["bestSupportedUniform"]["candidateId"] == "aq-strength-04-temporal-off"
    assert first["bestSupportedUniform"]["effectiveAqFields"] == {
        "aqStrength": 4,
        "enableAQ": True,
        "enableTemporalAQ": False,
    }
    assert first["comparators"] == {
        "bitrateEfficiency": "best_supported_uniform",
        "characterError": "best_supported_uniform",
    }


def test_invalid_trial_is_preserved_but_never_selected() -> None:
    evidence, grid, lock = _evidence()
    candidate = next(
        item
        for item in evidence["candidates"]
        if item["candidateId"] == "aq-strength-04-temporal-off"
    )
    failed_target = candidate["targetSearch"][2]
    failed_target.update(
        {
            "attempts": [
                {
                    "requestedPayloadBps": 1_000_000,
                    "measuredPayloadMbps": None,
                    "status": "INVALID",
                    "invalidReason": "browser_decode_failed",
                }
            ],
            "selectedAttemptIndex": None,
            "status": "INVALID",
            "invalidReasons": ["browser_decode_failed"],
            "perStratumCer": None,
            "equalStratumCer": None,
            "p95PreprocessMs": None,
            "p95PreprocessEncodeMs": None,
            "p95EncodeMs": None,
            "p99EncodeMs": None,
            "meanSenderCpuPercent": None,
            "submittedFrames": None,
            "decodedFrames": None,
            "width": None,
            "height": None,
            "pendingPositiveTrend": None,
            "maximumPendingAgeMs": None,
            "independentDecodePassed": None,
            "browserDecodePassed": None,
        }
    )
    candidate.update(
        {
            "status": "INVALID",
            "invalidReasons": ["browser_decode_failed"],
            "p95PreprocessEncodeMs": None,
            "meanSenderCpuPercent": None,
        }
    )
    result = select_development_evidence(evidence, grid, lock)
    assert result["bestSupportedUniform"]["candidateId"] != candidate["candidateId"]


def test_invalid_same_rate_retry_is_preserved_before_selected_attempt() -> None:
    evidence, grid, lock = _evidence()
    target = evidence["controlledUniform"]["targetSearch"][0]
    target["attempts"].insert(
        0,
        {
            "requestedPayloadBps": 500_000,
            "measuredPayloadMbps": None,
            "status": "INVALID",
            "invalidReason": "interrupted_native_trial",
        },
    )
    target["selectedAttemptIndex"] = 1
    selected = select_development_evidence(evidence, grid, lock)
    assert selected["status"] == "SELECTED"


def test_selector_rejects_grid_gaps_rate_mismatch_and_hidden_failures() -> None:
    evidence, grid, lock = _evidence()
    evidence["candidates"].pop()
    with pytest.raises(UniformAqSelectionError, match="schema_invalid|coverage"):
        select_development_evidence(evidence, grid, lock)

    evidence, grid, lock = _evidence()
    candidate = evidence["candidates"][0]
    candidate["targetSearch"][2]["attempts"][0]["measuredPayloadMbps"] = 1.03
    with pytest.raises(UniformAqSelectionError, match="rate_match_failed"):
        select_development_evidence(evidence, grid, lock)

    evidence, grid, lock = _evidence()
    candidate = evidence["candidates"][0]
    candidate["targetSearch"][0]["browserDecodePassed"] = False
    with pytest.raises(UniformAqSelectionError, match="systems_admissibility"):
        select_development_evidence(evidence, grid, lock)


def test_comparator_ties_and_unestimable_crossings_choose_controlled() -> None:
    controlled = SelectorInputs(
        mean_fitted_cer=0.2,
        fitted_cer_at_1_mbps=0.2,
        bitrate_at_ten_percent_cer_mbps=None,
        p95_preprocess_encode_ms=5.0,
        mean_sender_cpu_percent=10.0,
        lexicographic_effective_aq_fields=b"{}",
    )
    selected = SelectorInputs(
        mean_fitted_cer=0.2,
        fitted_cer_at_1_mbps=0.2,
        bitrate_at_ten_percent_cer_mbps=None,
        p95_preprocess_encode_ms=5.0,
        mean_sender_cpu_percent=10.0,
        lexicographic_effective_aq_fields=b"{}",
    )
    assert comparator_identity(controlled, selected, "character_error") == "controlled_uniform"
    assert comparator_identity(controlled, selected, "bitrate_efficiency") == "controlled_uniform"


def test_selection_write_is_no_clobber(tmp_path: Path) -> None:
    output = tmp_path / "selection.json"
    write_exclusive(output, {"status": "SELECTED"})
    assert json.loads(output.read_text(encoding="utf-8")) == {"status": "SELECTED"}
    with pytest.raises(FileExistsError):
        write_exclusive(output, {"status": "REPLACED"})
    assert json.loads(output.read_text(encoding="utf-8")) == {"status": "SELECTED"}
