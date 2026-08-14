from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import pytest

from tools.validate_m0_browser_matrix import (
    BrowserMatrixValidationError,
    canonical_source_digest,
    expected_run_contracts,
    validate_m0_browser_matrix,
)

MATRIX_SCHEMA = Path("schemas/m0-browser-matrix-v1.schema.json")
PLAYBACK_SCHEMA = Path("schemas/m0-browser-playback-v1.schema.json")
ORACLE_SCHEMA = Path("schemas/browser-oracle-v1.schema.json")
STREAM_SHA256 = "c" * 64
ORACLE_ORDINALS = [720, 900, 1080, 1260]


def trace(start_frame: int, pli: int | None) -> list[dict[str, int]]:
    values = []
    for index in range(1_800):
        recovered = pli is not None and index >= pli
        values.append(
            {
                "frame_index": start_frame + index + (59 if recovered else 0),
                "dependency_epoch": 2 if recovered else 1,
                "extended_timestamp": 2**32 - 3_000 + index * 3_000,
            }
        )
    return values


def playback_report(
    browser: str,
    scenario: str,
    fault: int | None,
    pli: int | None,
) -> dict[str, Any]:
    start_frame = 240 if scenario == "PLI_RECOVERY" else 300
    sent_trace = trace(start_frame, pli)
    comparisons = []
    oracle = []
    for ordinal in ORACLE_ORDINALS:
        frame = sent_trace[ordinal]
        comparisons.append(
            {
                "key": f"{frame['dependency_epoch']}:{frame['extended_timestamp']}",
                "width": 1280,
                "height": 720,
                "maximumAbsoluteChannelError": 1,
                "differingPixels": 10,
                "differingPixelFraction": 10 / (1280 * 720),
                "rootMeanSquareChannelError": 0.01,
            }
        )
        oracle.append(
            {
                "expectedDisplayTime": 60_000.0,
                "height": 720,
                "presentationTime": 60_000.0,
                "rgbaSha256": "d" * 64,
                "rtpTimestamp": frame["extended_timestamp"] % 2**32,
                "width": 1280,
            }
        )
    loss = scenario == "ROLLOVER_LOSS"
    recovery = scenario == "PLI_RECOVERY"
    return {
        "schemaVersion": 1,
        "protocol": "glyphrelay-m0-browser-playback-v1",
        "status": "PASSED",
        "browser": browser,
        "browserVersion": "151.0.7922.34" if browser == "chromium" else "153.0",
        "browserExecutableSha256": ("a" if browser == "chromium" else "b") * 64,
        "diagnostics": [],
        "minimumPresentedFrames": 1_440,
        "oracleFrameOrdinals": ORACLE_ORDINALS,
        "oracleComparisons": comparisons,
        "receiver": {
            "inboundVideo": [
                {
                    "framesDecoded": 1_780,
                    "framesDropped": 0,
                    "keyFramesDecoded": 30,
                    "nackCount": 1 if loss else 0,
                    "packetsLost": 0,
                    "pliCount": 1 if recovery else 0,
                }
            ],
            "oracle": oracle,
            "playbackQuality": {
                "corruptedVideoFrames": 0,
                "droppedVideoFrames": 0,
                "totalVideoFrames": 1_780,
            },
            "presentedFrames": 1_780,
            "state": "RUNNING",
            "videoHeight": 720,
            "videoWidth": 1280,
        },
        "sender": {
            "schema_version": 1,
            "protocol": "glyphrelay-m0-webrtc-sender-v1",
            "status": "PASSED",
            "presentation": "720p30",
            "start_frame": start_frame,
            "requested_frames": 1_800,
            "sent_frames": 1_800,
            "recovery_frames": 1 if recovery else 0,
            "initial_extended_sequence": 65_534,
            "next_extended_sequence": 80_000,
            "last_extended_timestamp": sent_trace[-1]["extended_timestamp"],
            "wire_datagrams": 2_000,
            "wire_ip_total_bytes": 2_000_000,
            "wire_media_ip_total_bytes": 1_900_000,
            "wire_control_ip_total_bytes": 100_000,
            "wire_direct_ip_total_bytes": 2_000_000,
            "wire_turn_ip_total_bytes": 0,
            "rejected_datagrams": 0,
            "failed_datagrams": 0,
            "short_datagrams": 0,
            "accounting_overflowed": False,
            "feedback_messages": 2 if (loss or recovery) else 0,
            "distinct_nack_identifiers": 1 if loss else 0,
            "idr_requests": 2 if recovery else 1,
            "cache_retransmissions": 1 if loss else 0,
            "inject_pli_after_frame": pli,
            "fault_loss_extended_sequence": fault,
            "fault_datagram_suppressed": loss,
            "protected_retransmission_observed": loss,
            "protected_retransmission_identical": loss,
            "sent_frame_trace": sent_trace,
        },
        "streamSha256": STREAM_SHA256,
    }


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_matrix(root: Path, fixture: Path) -> dict[str, Any]:
    write_json(fixture / "browser-fixture-summary.json", {"stream_sha256": STREAM_SHA256})
    declarations: list[dict[str, Any]] = []
    reports: dict[str, dict[str, Any]] = {}
    for run_id, (browser, scenario, fault, pli) in expected_run_contracts().items():
        report = playback_report(browser, scenario, fault, pli)
        report_path = root / "runs" / run_id / "browser-playback-summary.json"
        write_json(report_path, report)
        reports[run_id] = report
        declarations.append(
            {
                "browser": browser,
                "faultLossExtendedSequence": fault,
                "injectPliAfterFrame": pli,
                "oraclePassed": True,
                "presentedFrames": 1_780,
                "reportPath": report_path.relative_to(root).as_posix(),
                "reportSha256": digest(report_path),
                "runId": run_id,
                "scenario": scenario,
            }
        )
    zero_ids = [str(item["runId"]) for item in declarations if item["scenario"] == "ZERO_LOSS"]
    zero_runs = [
        {
            "browser": reports[run_id]["browser"],
            "comparisons": reports[run_id]["oracleComparisons"],
            "decoderErrors": 0,
            "infrastructureStatus": "COMPLETE",
            "runId": run_id,
            "zeroLoss": True,
        }
        for run_id in zero_ids
    ]
    comparisons = [item for run in zero_runs for item in run["comparisons"]]
    tolerance = {
        "schemaVersion": 1,
        "protocol": "browser_oracle_v1",
        "state": "FROZEN",
        "requiredZeroLossRuns": 10,
        "frameKey": "dependency_epoch:extended_rtp_timestamp",
        "sourceRunIds": zero_ids,
        "sourceDigestSha256": canonical_source_digest(zero_runs),
        "maximumAbsoluteChannelError": max(
            item["maximumAbsoluteChannelError"] for item in comparisons
        ),
        "maximumDifferingPixelFraction": max(
            item["differingPixelFraction"] for item in comparisons
        ),
        "maximumRootMeanSquareChannelError": max(
            item["rootMeanSquareChannelError"] for item in comparisons
        ),
    }
    zero_input = {
        "schemaVersion": 1,
        "protocol": "browser_oracle_zero_loss_v1",
        "requiredFrameKeys": sorted(item["key"] for item in zero_runs[0]["comparisons"]),
        "runs": zero_runs,
    }
    summary = {
        "schemaVersion": 1,
        "protocol": "glyphrelay-m0-browser-matrix-v1",
        "status": "PASSED",
        "fixtureStreamSha256": STREAM_SHA256,
        "oracleTolerance": tolerance,
        "runs": declarations,
    }
    write_json(root / "browser-oracle-zero-loss.json", zero_input)
    write_json(root / "browser-oracle-frozen.json", tolerance)
    write_json(root / "browser-matrix-summary.json", summary)
    return summary


def validate(root: Path, fixture: Path) -> dict[str, Any]:
    return validate_m0_browser_matrix(
        root,
        fixture,
        MATRIX_SCHEMA,
        PLAYBACK_SCHEMA,
        ORACLE_SCHEMA,
    )


def test_complete_browser_matrix_and_oracle_freeze_pass(tmp_path: Path) -> None:
    root = tmp_path / "matrix"
    fixture = tmp_path / "fixture"
    write_matrix(root, fixture)
    assert len(validate(root, fixture)["runs"]) == 18


def test_seeded_recovery_oracle_regression_fails_for_its_reason(tmp_path: Path) -> None:
    root = tmp_path / "matrix"
    fixture = tmp_path / "fixture"
    summary = write_matrix(root, fixture)
    run_id = "rollover-loss-firefox-65535"
    report_path = root / "runs" / run_id / "browser-playback-summary.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    report["oracleComparisons"][0]["maximumAbsoluteChannelError"] = 2
    write_json(report_path, report)
    declaration = next(item for item in summary["runs"] if item["runId"] == run_id)
    declaration["reportSha256"] = digest(report_path)
    write_json(root / "browser-matrix-summary.json", summary)
    with pytest.raises(BrowserMatrixValidationError, match="matrix_oracle_tolerance_failed"):
        validate(root, fixture)


def test_nearby_recovery_oracle_control_passes(tmp_path: Path) -> None:
    root = tmp_path / "matrix"
    fixture = tmp_path / "fixture"
    write_matrix(root, fixture)
    validate(root, fixture)


def test_seeded_browser_binary_change_fails_consistency_gate(tmp_path: Path) -> None:
    root = tmp_path / "matrix"
    fixture = tmp_path / "fixture"
    summary = write_matrix(root, fixture)
    run_id = "zero-loss-chromium-02"
    report_path = root / "runs" / run_id / "browser-playback-summary.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    report["browserExecutableSha256"] = "f" * 64
    write_json(report_path, report)
    declaration = next(item for item in summary["runs"] if item["runId"] == run_id)
    declaration["reportSha256"] = digest(report_path)
    write_json(root / "browser-matrix-summary.json", summary)
    with pytest.raises(BrowserMatrixValidationError, match="matrix_browser_binary_changed"):
        validate(root, fixture)
