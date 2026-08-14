from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from tools.validate_m0_browser_playback import (
    BrowserPlaybackValidationError,
    validate_browser_playback,
)

SCHEMA = Path("schemas/m0-browser-playback-v1.schema.json")


def report() -> dict[str, object]:
    oracle_ordinals = [36, 45, 54, 63]
    return {
        "schemaVersion": 1,
        "protocol": "glyphrelay-m0-browser-playback-v1",
        "status": "PASSED",
        "browser": "chromium",
        "browserVersion": "151.0.7922.34",
        "browserExecutableSha256": "a" * 64,
        "diagnostics": [],
        "minimumPresentedFrames": 72,
        "oracleFrameOrdinals": oracle_ordinals,
        "oracleComparisons": [
            {
                "key": f"2:{2**32 - 3_000 + ordinal * 3_000}",
                "width": 1280,
                "height": 720,
                "maximumAbsoluteChannelError": 1,
                "differingPixels": 10,
                "differingPixelFraction": 10 / (1280 * 720),
                "rootMeanSquareChannelError": 0.01,
            }
            for ordinal in oracle_ordinals
        ],
        "receiver": {
            "inboundVideo": [
                {
                    "framesDecoded": 72,
                    "framesDropped": 0,
                    "keyFramesDecoded": 3,
                    "nackCount": 1,
                    "packetsLost": 0,
                    "pliCount": 0,
                }
            ],
            "oracle": [
                {
                    "expectedDisplayTime": 3000.0,
                    "height": 720,
                    "presentationTime": 3000.0,
                    "rgbaSha256": "b" * 64,
                    "rtpTimestamp": (2**32 - 3_000 + ordinal * 3_000) % 2**32,
                    "width": 1280,
                }
                for ordinal in oracle_ordinals
            ],
            "playbackQuality": {
                "corruptedVideoFrames": 0,
                "droppedVideoFrames": 0,
                "totalVideoFrames": 72,
            },
            "presentedFrames": 72,
            "state": "RUNNING",
            "videoHeight": 720,
            "videoWidth": 1280,
        },
        "sender": {
            "schema_version": 1,
            "protocol": "glyphrelay-m0-webrtc-sender-v1",
            "status": "PASSED",
            "presentation": "720p30",
            "start_frame": 0,
            "requested_frames": 90,
            "sent_frames": 90,
            "recovery_frames": 1,
            "initial_extended_sequence": 65_534,
            "next_extended_sequence": 66_949,
            "last_extended_timestamp": 4_295_231_296,
            "wire_datagrams": 10,
            "wire_ip_total_bytes": 100,
            "wire_media_ip_total_bytes": 80,
            "wire_control_ip_total_bytes": 20,
            "wire_direct_ip_total_bytes": 100,
            "wire_turn_ip_total_bytes": 0,
            "rejected_datagrams": 0,
            "failed_datagrams": 0,
            "short_datagrams": 0,
            "accounting_overflowed": False,
            "feedback_messages": 2,
            "distinct_nack_identifiers": 1,
            "idr_requests": 3,
            "cache_retransmissions": 1,
            "inject_pli_after_frame": 30,
            "fault_loss_extended_sequence": 65_535,
            "fault_datagram_suppressed": True,
            "protected_retransmission_observed": True,
            "protected_retransmission_identical": True,
            "sent_frame_trace": [
                {
                    "frame_index": index if index < 30 else index + 30,
                    "dependency_epoch": 1 if index < 30 else 2,
                    "extended_timestamp": 2**32 - 3_000 + index * 3_000,
                }
                for index in range(90)
            ],
        },
        "streamSha256": "c" * 64,
    }


def write_report(root: Path, value: dict[str, object]) -> None:
    (root / "browser-playback-summary.json").write_text(json.dumps(value), encoding="utf-8")


def test_browser_playback_validator_accepts_complete_recovery_evidence(
    tmp_path: Path,
) -> None:
    write_report(tmp_path, report())
    validated = validate_browser_playback(tmp_path, SCHEMA)
    assert validated["browser"] == "chromium"


def test_browser_playback_validator_rejects_seeded_protected_replay_defect(
    tmp_path: Path,
) -> None:
    seeded = report()
    sender = seeded["sender"]
    assert isinstance(sender, dict)
    sender["protected_retransmission_identical"] = False
    write_report(tmp_path, seeded)
    with pytest.raises(
        BrowserPlaybackValidationError, match="protected_retransmission_not_identical"
    ):
        validate_browser_playback(tmp_path, SCHEMA)


def test_browser_playback_validator_nearby_zero_loss_control_passes(
    tmp_path: Path,
) -> None:
    control = copy.deepcopy(report())
    sender = control["sender"]
    assert isinstance(sender, dict)
    sender["fault_loss_extended_sequence"] = None
    sender["fault_datagram_suppressed"] = False
    sender["protected_retransmission_observed"] = False
    sender["protected_retransmission_identical"] = False
    sender["cache_retransmissions"] = 0
    sender["distinct_nack_identifiers"] = 0
    write_report(tmp_path, control)
    validate_browser_playback(tmp_path, SCHEMA)


def test_browser_playback_validator_rejects_corrupted_browser_frame(
    tmp_path: Path,
) -> None:
    seeded = report()
    receiver = seeded["receiver"]
    assert isinstance(receiver, dict)
    playback = receiver["playbackQuality"]
    assert isinstance(playback, dict)
    playback["corruptedVideoFrames"] = 1
    write_report(tmp_path, seeded)
    with pytest.raises(BrowserPlaybackValidationError, match="corrupted_browser_frame"):
        validate_browser_playback(tmp_path, SCHEMA)
