from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
from typing import Any

import pytest

from tools.replay_controller_trace import ControllerReplay
from tools.validate_controller_network_qualification import (
    ControllerNetworkValidationError,
    sha256_file,
    validate_controller_network_qualification,
)

MATRIX_PATH = Path("qualification/controller-network-v1.json")
SCHEMA_PATH = Path("schemas/controller-network-evidence-v1.schema.json")
SOURCE_RAW_BASE_MS = 1_000_000.0
CLOCK_OFFSET_MS = -SOURCE_RAW_BASE_MS


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def feedback(sequence: int, now: int) -> dict[str, Any]:
    return {
        "arrivalSequence": sequence,
        "lossFraction": 0.0,
        "receiverDecodedFrames": None,
        "receiverDroppedFrames": None,
        "rembBitsPerSecond": None,
        "rembPayloadTypeValid": False,
        "rembRtcpSourceValid": False,
        "roundTripTimeMilliseconds": 50.0,
        "senderArrivalMilliseconds": now,
        "sourceTimeMilliseconds": now,
    }


def trace_record(
    replay: ControllerReplay,
    *,
    tick: int,
    now: int,
    cap: int,
    wire: int,
    elementary: int,
    delivered: int,
    queue_age: float,
    queue_bytes: int,
) -> dict[str, Any]:
    record: dict[str, Any] = {
        "arrivalSequence": tick * 2 + 2,
        "consumedFeedback": [feedback(tick * 2 + 1, now)],
        "dependencyEpoch": 1,
        "rawInput": {
            "controllerConfig": {
                "baseEntryThreshold": 0.5,
                "baseExitThreshold": 0.3,
                "basePayloadTargetBps": cap,
            },
            "counters": {
                "deliveredFrames": delivered,
                "elementaryStreamBytes": elementary,
                "retransmissionBytes": 0,
                "wireEgressBytes": wire,
            },
            "dropFraction": 0.0,
            "encodeLatencyMilliseconds": 2.0,
            "mapLevelHistogram": [0, 0, 0, 0, 0, 0],
            "oldestMediaAgeMilliseconds": queue_age,
            "pacerQueueBytes": queue_bytes,
            "pacerQueuePackets": 0 if queue_bytes == 0 else 10,
            "pinnedRegionViolation": False,
            "protectedFraction": 0.2,
            "userWireCapBps": cap,
        },
        "senderArrivalMilliseconds": now,
        "sourceTimeMilliseconds": now,
    }
    record.update(replay.process(record))
    return record


def sender_samples(kind: str, cap: int) -> list[dict[str, Any]]:
    total_ms = 130_000 if kind == "steady" else 58_000
    replay = ControllerReplay(
        {
            "baseEntryThreshold": 0.5,
            "baseExitThreshold": 0.3,
            "basePayloadTargetBps": cap,
        }
    )
    samples: list[dict[str, Any]] = []
    wire = 0
    elementary = 0
    delivered = 0
    for tick, now in enumerate(range(0, total_ms + 1, 100)):
        collapse_active = kind == "collapse" and 20_000 <= now < 50_000
        wire_bps = 80_000 if collapse_active else cap * 0.80
        wire += round(wire_bps / 80.0)
        elementary += round(cap * 0.70 / 80.0)
        delivered += 1 if collapse_active else 3
        queue_age = 75.0 if collapse_active else 0.0
        queue_bytes = 2 * 1024 * 1024 if collapse_active else 0
        trace = trace_record(
            replay,
            tick=tick,
            now=now,
            cap=cap,
            wire=wire,
            elementary=elementary,
            delivered=delivered,
            queue_age=queue_age,
            queue_bytes=queue_bytes,
        )
        levels = trace["selectedLevelStack"]
        samples.append(
            {
                "senderMilliseconds": now,
                "senderMonotonicRawNanoseconds": int((SOURCE_RAW_BASE_MS + now) * 1_000_000),
                "wireEgressBytes": wire,
                "elementaryStreamBytes": elementary,
                "retransmissionBytes": 0,
                "deliveredFrames": delivered,
                "transportedAccessUnits": tick * 3,
                "encodedAccessUnits": tick * 3,
                "pacerQueueBytes": queue_bytes,
                "pacerQueuePackets": 0 if queue_bytes == 0 else 10,
                "pacerOldestAgeMilliseconds": queue_age,
                "presentationProfile": levels["presentationProfile"],
                "width": levels["width"],
                "height": levels["height"],
                "framesPerSecond": levels["framesPerSecond"],
                "dependencyEpoch": trace["dependencyEpoch"],
                "controllerState": trace["resultingState"],
                "action": trace["action"],
                "controllerTrace": trace,
                "clockCorrelation": {
                    "valid": True,
                    "offsetMilliseconds": CLOCK_OFFSET_MS,
                    "uncertaintyMilliseconds": 0.5,
                    "networkDelayMilliseconds": 1.0,
                    "requestSequence": tick + 1,
                },
            }
        )
    return samples


def observations(total_ms: int) -> list[dict[str, Any]]:
    result = []
    for source_ms in range(0, total_ms, 33):
        result.append(
            {
                "callbackTimeMs": source_ms + 99.0,
                "captureTimeMs": None,
                "expectedDisplayTimeMs": source_ms + 100.0,
                "presentationTimeMs": source_ms + 98.0,
                "receiveTimeMs": source_ms + 75.0,
                "rtpTimestamp": 90_000 + source_ms * 90,
            }
        )
    return result


def run_fixture(kind: str, browser: str, cap: int, capture_name: str) -> dict[str, Any]:
    samples = sender_samples(kind, cap)
    total_ms = samples[-1]["senderMilliseconds"]
    collapse = kind == "collapse"
    recovery_requests = [
        item
        for item in samples
        if item["senderMilliseconds"] >= 50_000 and item["action"] == "REQUEST_RECOVERY_IDR"
    ]
    recovery_tick = 50_100 if collapse else None
    recovery_source = recovery_requests[0]["senderMilliseconds"] + 500 if recovery_requests else 0
    recovery_points = [
        {
            "sourceFrameId": 1,
            "sourceMonotonicRawNanoseconds": int(SOURCE_RAW_BASE_MS * 1_000_000),
            "extendedRtpTimestamp": 90_000,
            "wireRtpTimestamp": 90_000,
            "dependencyEpoch": 1,
            "geometryEpoch": 1,
        }
    ]
    if collapse:
        recovery_points.append(
            {
                "sourceFrameId": recovery_source // 33 + 1,
                "sourceMonotonicRawNanoseconds": int(
                    (SOURCE_RAW_BASE_MS + recovery_source) * 1_000_000
                ),
                "extendedRtpTimestamp": 90_000 + recovery_source * 90,
                "wireRtpTimestamp": 90_000 + recovery_source * 90,
                "dependencyEpoch": 2,
                "geometryEpoch": 1,
            }
        )
    base_command = (
        f"tc qdisc replace dev {{device}} root netem limit 1000 delay 25ms rate {cap // 1000}kbit"
    )
    commands = [base_command, base_command]
    if collapse:
        collapse_command = (
            "tc qdisc replace dev {device} root netem limit 1000 delay 25ms rate 100kbit"
        )
        commands.extend([collapse_command, collapse_command, base_command, base_command])
    return {
        "runId": f"{kind}-{browser}-{cap // 1000}k",
        "kind": kind,
        "browser": {
            "name": browser,
            "version": "151.0.7922.34" if browser == "chromium" else "153.0",
            "executableSha256": hashlib.sha256(browser.encode()).hexdigest(),
        },
        "transport": "direct_ipv4",
        "capBitsPerSecond": cap,
        "network": {
            "oneWayDelayMilliseconds": 25,
            "queueLimitPackets": 1000,
            "baseRateKbit": cap // 1000,
            "collapseRateKbit": 100 if collapse else None,
            "appliedCommands": commands,
            "senderQdisc": [{"kind": "netem"}],
            "receiverQdisc": [{"kind": "netem"}],
        },
        "timing": {
            "measurementStartSenderMilliseconds": 10_000,
            "measurementEndSenderMilliseconds": 130_000 if not collapse else 58_000,
            "collapseStartSenderMilliseconds": 20_000 if collapse else None,
            "restoredAtSenderMilliseconds": 50_000 if collapse else None,
            "recoveryTickSenderMilliseconds": recovery_tick,
        },
        "sender": {
            "result": {
                "exitCode": 0,
                "reason": "share_stopped",
                "capturedFrames": len(samples) * 3,
                "encodedAccessUnits": samples[-1]["encodedAccessUnits"],
                "transportedAccessUnits": samples[-1]["transportedAccessUnits"],
                "wireEgressBytes": samples[-1]["wireEgressBytes"],
                "controllerTicks": len(samples),
                "controllerActions": sum(item["action"] != "NONE" for item in samples),
            },
            "samples": samples,
            "rtpClockBase": recovery_points[0],
            "recoveryPoints": recovery_points,
        },
        "receiver": {
            "finalSnapshot": {
                "state": "ENDED",
                "presentedFrames": samples[-1]["deliveredFrames"],
                "videoWidth": 1280,
                "videoHeight": 720,
            },
            "observations": observations(total_ms),
            "droppedFrameObservations": 0,
            "errors": [],
        },
        "capture": {
            "file": capture_name,
            "sha256": "",
            "packetCount": 100,
            "ipTotalBytes": samples[-1]["wireEgressBytes"],
        },
    }


def complete_evidence(tmp_path: Path) -> tuple[Path, dict[str, Any], dict[str, tuple[int, int]]]:
    root = tmp_path / "evidence"
    root.mkdir()
    deterministic = root / "deterministic-transport-validation.json"
    write_json(deterministic, {"status": "PASSED"})
    matrix = json.loads(MATRIX_PATH.read_text(encoding="utf-8"))
    runs = []
    packet_totals: dict[str, tuple[int, int]] = {}
    for browser in ("chromium", "firefox"):
        for cap in (500_000, 1_000_000, 2_000_000, 4_000_000):
            name = f"steady-{browser}-{cap // 1000}k.pcapng"
            run = run_fixture("steady", browser, cap, name)
            (root / name).write_bytes(name.encode())
            run["capture"]["sha256"] = sha256_file(root / name)
            packet_totals[name] = (run["capture"]["packetCount"], run["capture"]["ipTotalBytes"])
            runs.append(run)
        name = f"collapse-{browser}-1000k.pcapng"
        run = run_fixture("collapse", browser, 1_000_000, name)
        (root / name).write_bytes(name.encode())
        run["capture"]["sha256"] = sha256_file(root / name)
        packet_totals[name] = (run["capture"]["packetCount"], run["capture"]["ipTotalBytes"])
        runs.append(run)
    evidence = {
        "schemaVersion": 1,
        "protocol": "glyphrelay-controller-network-evidence-v1",
        "status": "CAPTURED",
        "sourceCommit": "a" * 40,
        "sourceBundleId": "b" * 64,
        "controllerProtocolSha256": matrix["controllerProtocolSha256"],
        "matrixSha256": sha256_file(MATRIX_PATH),
        "deterministicTransportValidation": {
            "status": "PASSED",
            "artifact": deterministic.name,
            "sha256": sha256_file(deterministic),
        },
        "environment": {
            "kernelRelease": "6.8.0-test",
            "ipVersion": "ip utility, iproute2-6.1.0",
            "tcVersion": "tc utility, iproute2-6.1.0",
            "tsharkVersion": "TShark 4.2.0",
            "iproute2Package": {
                "manager": "dpkg",
                "name": "iproute2",
                "version": "6.1.0-3",
                "architecture": "amd64",
            },
        },
        "runs": runs,
    }
    write_json(root / "evidence.json", evidence)
    return root, evidence, packet_totals


def validate(root: Path, packet_totals: dict[str, tuple[int, int]]) -> dict[str, Any]:
    return validate_controller_network_qualification(
        root,
        schema_path=SCHEMA_PATH,
        matrix_path=MATRIX_PATH,
        packet_reader=lambda path: packet_totals[path.name],
    )


def rewrite(root: Path, evidence: dict[str, Any]) -> None:
    write_json(root / "evidence.json", evidence)


def test_complete_controller_network_matrix_passes(tmp_path: Path) -> None:
    root, _, packets = complete_evidence(tmp_path)
    result = validate(root, packets)
    assert result["status"] == "PASSED"
    assert len(result["runs"]) == 10


@pytest.mark.parametrize(
    ("mutation", "reason"),
    [
        ("wire", "steady_wire_p95_exceeded"),
        ("capture", "counter_capture_error_exceeded"),
        ("profile", "steady_profile_reduced"),
        ("latency", "steady_latency_p95_exceeded"),
        ("queue", "pacer_queue_byte_bound_exceeded"),
        ("trace", "trace_wire_mismatch"),
    ],
)
def test_seeded_network_defects_fail_for_their_reason(
    tmp_path: Path, mutation: str, reason: str
) -> None:
    root, evidence, packets = complete_evidence(tmp_path)
    run = copy.deepcopy(evidence["runs"][0])
    evidence["runs"][0] = run
    if mutation == "wire":
        for sample in run["sender"]["samples"]:
            sample["wireEgressBytes"] *= 2
            sample["controllerTrace"]["rawInput"]["counters"]["wireEgressBytes"] *= 2
    elif mutation == "capture":
        packets[run["capture"]["file"]] = (
            run["capture"]["packetCount"],
            round(run["capture"]["ipTotalBytes"] * 0.90),
        )
        run["capture"]["ipTotalBytes"] = packets[run["capture"]["file"]][1]
    elif mutation == "profile":
        sample = run["sender"]["samples"][200]
        sample["presentationProfile"] = "720p24"
        sample["framesPerSecond"] = 24
        sample["controllerTrace"]["selectedLevelStack"]["presentationProfile"] = "720p24"
        sample["controllerTrace"]["selectedLevelStack"]["framesPerSecond"] = 24
    elif mutation == "latency":
        for observation in run["receiver"]["observations"]:
            observation["expectedDisplayTimeMs"] += 300
    elif mutation == "queue":
        sample = run["sender"]["samples"][1]
        sample["pacerQueueBytes"] = 4 * 1024 * 1024 + 1
        sample["controllerTrace"]["rawInput"]["pacerQueueBytes"] = 4 * 1024 * 1024 + 1
    else:
        run["sender"]["samples"][1]["wireEgressBytes"] += 1
    rewrite(root, evidence)
    with pytest.raises(ControllerNetworkValidationError, match=reason):
        validate(root, packets)


def test_missing_matrix_run_cannot_pass(tmp_path: Path) -> None:
    root, evidence, packets = complete_evidence(tmp_path)
    evidence["runs"].pop()
    rewrite(root, evidence)
    with pytest.raises(ControllerNetworkValidationError, match="schema_validation_failed"):
        validate(root, packets)
