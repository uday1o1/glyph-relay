from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, cast

from jsonschema import Draft202012Validator


class BrowserPlaybackValidationError(RuntimeError):
    pass


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise BrowserPlaybackValidationError(reason)


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BrowserPlaybackValidationError(f"json_read_failed:{path.name}") from error
    require(isinstance(value, dict), f"json_object_required:{path.name}")
    return cast(dict[str, Any], value)


def validate_browser_playback(evidence: Path, schema_path: Path) -> dict[str, Any]:
    report = load_object(evidence / "browser-playback-summary.json")
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(report), key=lambda item: item.json_path
    )
    require(
        not errors,
        f"schema_validation_failed:{errors[0].json_path}:{errors[0].message}" if errors else "",
    )

    receiver = report["receiver"]
    sender = report["sender"]
    frame_count = sender["requested_frames"]
    require(
        report["oracleFrameOrdinals"]
        == [
            frame_count * 2 // 5,
            frame_count // 2,
            frame_count * 3 // 5,
            frame_count * 7 // 10,
        ],
        "oracle_frame_ordinals_invalid",
    )
    require(
        receiver["presentedFrames"] >= report["minimumPresentedFrames"],
        "presented_frame_gate_failed",
    )
    require(receiver["playbackQuality"]["corruptedVideoFrames"] == 0, "corrupted_browser_frame")
    require(sender["sent_frames"] == sender["requested_frames"], "sender_frame_count_mismatch")
    trace = sender["sent_frame_trace"]
    require(len(trace) == sender["sent_frames"], "sender_frame_trace_count_mismatch")
    for index, frame in enumerate(trace):
        require(
            frame["frame_index"] >= sender["start_frame"]
            and (index == 0 or frame["frame_index"] > trace[index - 1]["frame_index"]),
            "sender_frame_trace_source_order_invalid",
        )
        require(
            frame["extended_timestamp"] == (2**32 - 3_000) + index * 3_000,
            "sender_frame_trace_timestamp_invalid",
        )
        require(
            index == 0 or frame["dependency_epoch"] >= trace[index - 1]["dependency_epoch"],
            "sender_frame_trace_epoch_invalid",
        )
    require(
        trace[-1]["extended_timestamp"] == sender["last_extended_timestamp"],
        "sender_frame_trace_last_timestamp_mismatch",
    )
    require(sender["initial_extended_sequence"] == 65_534, "rtp_rollover_seed_invalid")
    require(sender["next_extended_sequence"] > 65_536, "rtp_sequence_did_not_roll_over")
    require(sender["last_extended_timestamp"] >= 2**32, "rtp_timestamp_did_not_roll_over")
    require(
        sender["rejected_datagrams"] == 0
        and sender["failed_datagrams"] == 0
        and sender["short_datagrams"] == 0
        and sender["accounting_overflowed"] is False,
        "wire_egress_accounting_failed",
    )
    require(
        sender["wire_ip_total_bytes"]
        == sender["wire_media_ip_total_bytes"] + sender["wire_control_ip_total_bytes"],
        "wire_class_total_mismatch",
    )
    require(
        sender["wire_ip_total_bytes"]
        == sender["wire_direct_ip_total_bytes"] + sender["wire_turn_ip_total_bytes"],
        "wire_path_total_mismatch",
    )
    require(
        not any(item.startswith("pageerror:") for item in report["diagnostics"]),
        "browser_page_error_observed",
    )
    require(
        len(report["oracleComparisons"]) == len(receiver["oracle"]),
        "oracle_comparison_count_mismatch",
    )
    trace_by_key = {
        f"{frame['dependency_epoch']}:{frame['extended_timestamp']}": frame for frame in trace
    }
    for comparison, presented in zip(report["oracleComparisons"], receiver["oracle"], strict=True):
        require(comparison["key"] in trace_by_key, "oracle_comparison_trace_identity_missing")
        require(presented["rtpTimestamp"] is not None, "oracle_presented_timestamp_missing")
        require(
            trace_by_key[comparison["key"]]["extended_timestamp"] % 2**32
            == presented["rtpTimestamp"],
            "oracle_comparison_presented_timestamp_mismatch",
        )
    if sender["inject_pli_after_frame"] is not None:
        require(sender["recovery_frames"] >= 1, "pli_recovery_frame_missing")
        require(sender["idr_requests"] >= 2, "pli_recovery_idr_not_requested")
        require(
            any(frame["dependency_epoch"] > trace[0]["dependency_epoch"] for frame in trace),
            "pli_recovery_dependency_epoch_missing",
        )
        require(
            receiver["inboundVideo"][0]["keyFramesDecoded"] >= 2, "browser_recovery_idr_not_decoded"
        )
    if sender["fault_loss_extended_sequence"] is not None:
        require(sender["fault_datagram_suppressed"] is True, "seeded_loss_not_injected")
        require(sender["cache_retransmissions"] >= 1, "nack_retransmission_missing")
        require(sender["distinct_nack_identifiers"] >= 1, "generic_nack_not_observed")
        require(
            sender["protected_retransmission_observed"] is True
            and sender["protected_retransmission_identical"] is True,
            "protected_retransmission_not_identical",
        )
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument(
        "--schema",
        type=Path,
        default=Path("schemas/m0-browser-playback-v1.schema.json"),
    )
    arguments = parser.parse_args()
    try:
        report = validate_browser_playback(arguments.evidence, arguments.schema)
    except BrowserPlaybackValidationError as error:
        print(json.dumps({"status": "FAILED", "reason": str(error)}, sort_keys=True))
        return 1
    print(
        json.dumps(
            {
                "browser": report["browser"],
                "presented_frames": report["receiver"]["presentedFrames"],
                "status": "PASSED",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
