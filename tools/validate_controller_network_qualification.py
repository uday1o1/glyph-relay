#!/usr/bin/env python3
"""Validate the frozen Milestone 5 namespace and netem qualification evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import subprocess
from bisect import bisect_left, bisect_right
from collections.abc import Callable, Sequence
from pathlib import Path, PurePath
from typing import Any, cast

from jsonschema import Draft202012Validator

from tools.replay_controller_trace import (
    PROJECTION_FIELDS,
    ControllerReplay,
    ReplayError,
    canonical,
)


class ControllerNetworkValidationError(RuntimeError):
    """Raised when target evidence cannot satisfy the Milestone 5 gate."""


ROOT = Path(__file__).resolve().parents[1]
MAXIMUM_TSHARK_OUTPUT_BYTES = 64 * 1024 * 1024
SENDER_ADDRESS = "10.77.1.2"


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ControllerNetworkValidationError(reason)


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ControllerNetworkValidationError(f"json_read_failed:{path.name}") from error
    require(isinstance(value, dict), f"json_object_required:{path.name}")
    return cast(dict[str, Any], value)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_artifact(root: Path, name: object, expected_sha256: object) -> Path:
    require(isinstance(name, str) and PurePath(name).name == name, "artifact_name_invalid")
    path = (root / cast(str, name)).resolve(strict=True)
    require(
        path.is_relative_to(root) and path.is_file() and not path.is_symlink(), "artifact_invalid"
    )
    require(
        isinstance(expected_sha256, str) and sha256_file(path) == expected_sha256,
        f"artifact_hash_invalid:{name}",
    )
    return path


def validate_schema(value: dict[str, Any], schema_path: Path) -> None:
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(value), key=lambda item: item.json_path
    )
    require(
        not errors,
        f"schema_validation_failed:{errors[0].json_path}:{errors[0].message}" if errors else "",
    )


def tshark_ip_totals(path: Path) -> tuple[int, int]:
    completed = subprocess.run(
        [
            "tshark",
            "-r",
            str(path),
            "-Y",
            f"udp && ip.src == {SENDER_ADDRESS}",
            "-T",
            "fields",
            "-E",
            "separator=/t",
            "-E",
            "occurrence=f",
            "-e",
            "ip.len",
            "-e",
            "ip.hdr_len",
            "-e",
            "ip.flags.mf",
            "-e",
            "ip.frag_offset",
            "-e",
            "ipv6.version",
        ],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=120,
        env={"PATH": os.environ.get("PATH", "")},
        check=False,
    )
    require(completed.returncode == 0, f"tshark_read_failed:{completed.returncode}")
    require(len(completed.stdout) <= MAXIMUM_TSHARK_OUTPUT_BYTES, "tshark_output_too_large")
    count = 0
    total = 0
    try:
        for line_number, line in enumerate(
            completed.stdout.decode("ascii", "strict").splitlines(), start=1
        ):
            fields = line.split("\t")
            require(len(fields) == 5, f"tshark_field_count_invalid:{line_number}")
            length, header, more_fragments, fragment_offset, ipv6 = fields
            require(
                header == "20"
                and more_fragments in {"", "0"}
                and fragment_offset in {"", "0"}
                and ipv6 == "",
                f"tshark_unverified_ip_feature:{line_number}",
            )
            parsed = int(length)
            require(parsed >= 28, f"tshark_ip_length_invalid:{line_number}")
            count += 1
            total += parsed
    except (UnicodeError, ValueError) as error:
        raise ControllerNetworkValidationError("tshark_output_invalid") from error
    require(count > 0 and total > 0, "packet_capture_empty")
    return count, total


def percentile(values: Sequence[float], fraction: float) -> float:
    require(bool(values), "percentile_input_empty")
    ordered = sorted(values)
    rank = max(1, math.ceil(fraction * len(ordered)))
    return ordered[rank - 1]


def interpolate(samples: Sequence[dict[str, Any]], field: str, timestamp: int) -> float:
    times = [cast(int, item["senderMilliseconds"]) for item in samples]
    right = bisect_left(times, timestamp)
    if right < len(times) and times[right] == timestamp:
        return float(samples[right][field])
    require(0 < right < len(samples), f"sample_coverage_missing:{field}:{timestamp}")
    left = right - 1
    left_time = times[left]
    right_time = times[right]
    require(right_time > left_time, "sample_time_not_strict")
    left_value = float(samples[left][field])
    right_value = float(samples[right][field])
    fraction = float(timestamp - left_time) / float(right_time - left_time)
    return left_value + fraction * (right_value - left_value)


def window_rates(
    samples: Sequence[dict[str, Any]], field: str, start: int, end: int
) -> list[float]:
    rates: list[float] = []
    for window_end in range(start + 1000, end + 1, 100):
        before = interpolate(samples, field, window_end - 1000)
        after = interpolate(samples, field, window_end)
        require(after >= before, f"window_counter_regressed:{field}")
        rates.append((after - before) * 8.0)
    return rates


def validate_trace_sample(sample: dict[str, Any]) -> None:
    trace = cast(dict[str, Any], sample["controllerTrace"])
    raw_value = trace.get("rawInput")
    require(isinstance(raw_value, dict), "trace_raw_input_missing")
    raw = cast(dict[str, Any], raw_value)
    counters_value = raw.get("counters")
    require(isinstance(counters_value, dict), "trace_counters_missing")
    counters = cast(dict[str, Any], counters_value)
    levels_value = trace.get("selectedLevelStack")
    require(isinstance(levels_value, dict), "trace_level_stack_missing")
    levels = cast(dict[str, Any], levels_value)
    require(
        trace.get("senderArrivalMilliseconds") == sample["senderMilliseconds"],
        "trace_time_mismatch",
    )
    require(counters.get("wireEgressBytes") == sample["wireEgressBytes"], "trace_wire_mismatch")
    require(
        counters.get("elementaryStreamBytes") == sample["elementaryStreamBytes"],
        "trace_elementary_mismatch",
    )
    require(
        counters.get("retransmissionBytes") == sample["retransmissionBytes"],
        "trace_retransmission_mismatch",
    )
    require(counters.get("deliveredFrames") == sample["deliveredFrames"], "trace_frames_mismatch")
    require(raw.get("pacerQueueBytes") == sample["pacerQueueBytes"], "trace_queue_mismatch")
    require(
        raw.get("pacerQueuePackets") == sample["pacerQueuePackets"],
        "trace_queue_packets_mismatch",
    )
    require(trace.get("dependencyEpoch") == sample["dependencyEpoch"], "trace_epoch_mismatch")
    require(trace.get("resultingState") == sample["controllerState"], "trace_state_mismatch")
    require(trace.get("action") == sample["action"], "trace_action_mismatch")
    for field, sample_field in (
        ("presentationProfile", "presentationProfile"),
        ("width", "width"),
        ("height", "height"),
        ("framesPerSecond", "framesPerSecond"),
    ):
        require(levels.get(field) == sample[sample_field], f"trace_level_mismatch:{field}")


def replay_samples(samples: Sequence[dict[str, Any]]) -> str:
    first_trace = cast(dict[str, Any], samples[0]["controllerTrace"])
    first_raw = cast(dict[str, Any], first_trace["rawInput"])
    replay = ControllerReplay(cast(dict[str, Any], first_raw["controllerConfig"]))
    decisions: list[bytes] = []
    try:
        for sample in samples:
            record = cast(dict[str, Any], sample["controllerTrace"])
            actual = replay.process(record)
            expected = {field: record.get(field) for field in PROJECTION_FIELDS}
            require(canonical(actual) == canonical(expected), "controller_trace_replay_mismatch")
            decisions.append(canonical(actual))
    except ReplayError as error:
        raise ControllerNetworkValidationError(f"controller_trace_replay_failed:{error}") from error
    return hashlib.sha256(b"\n".join(decisions) + b"\n").hexdigest()


def validate_samples(samples: Sequence[dict[str, Any]], acceptance: dict[str, Any]) -> None:
    require(bool(samples), "sender_samples_empty")
    prior_time: int | None = None
    prior_counters: dict[str, int] = {}
    for sample in samples:
        current_time = cast(int, sample["senderMilliseconds"])
        require(prior_time is None or current_time > prior_time, "sample_time_not_strict")
        if prior_time is not None:
            require(current_time - prior_time <= 500, "sample_gap_exceeds_500ms")
        prior_time = current_time
        for field in (
            "wireEgressBytes",
            "elementaryStreamBytes",
            "retransmissionBytes",
            "deliveredFrames",
            "transportedAccessUnits",
            "encodedAccessUnits",
        ):
            value = cast(int, sample[field])
            require(value >= prior_counters.get(field, 0), f"sample_counter_regressed:{field}")
            prior_counters[field] = value
        require(
            sample["pacerQueueBytes"] <= acceptance["maximumPacerQueueBytes"],
            "pacer_queue_byte_bound_exceeded",
        )
        require(
            sample["pacerOldestAgeMilliseconds"] <= acceptance["maximumPacerAgeMilliseconds"],
            "pacer_queue_age_bound_exceeded",
        )
        validate_trace_sample(sample)


def validate_sender_result(sender: dict[str, Any], samples: Sequence[dict[str, Any]]) -> None:
    result = cast(dict[str, Any], sender["result"])
    last = samples[-1]
    for result_field, sample_field in (
        ("encodedAccessUnits", "encodedAccessUnits"),
        ("transportedAccessUnits", "transportedAccessUnits"),
        ("wireEgressBytes", "wireEgressBytes"),
    ):
        require(result[result_field] == last[sample_field], f"final_{sample_field}_mismatch")
    require(result["controllerTicks"] == len(samples), "final_controller_tick_count_mismatch")
    require(
        result["controllerActions"] == sum(sample["action"] != "NONE" for sample in samples),
        "final_controller_action_count_mismatch",
    )


def matching_clock(samples: Sequence[dict[str, Any]], source_raw_ms: float) -> dict[str, Any]:
    raw_times = [float(item["senderMonotonicRawNanoseconds"]) / 1_000_000.0 for item in samples]
    index = bisect_right(raw_times, source_raw_ms)
    index = min(max(index - 1, 0), len(samples) - 1)
    clock = cast(dict[str, Any], samples[index]["clockCorrelation"])
    require(clock["valid"], "clock_correlation_unavailable")
    return clock


def observation_latencies(run: dict[str, Any]) -> list[tuple[float, float, float]]:
    sender = cast(dict[str, Any], run["sender"])
    base = cast(dict[str, Any], sender["rtpClockBase"])
    samples = cast(list[dict[str, Any]], sender["samples"])
    receiver = cast(dict[str, Any], run["receiver"])
    results: list[tuple[float, float, float]] = []
    for observation in cast(list[dict[str, Any]], receiver["observations"]):
        rtp = observation["rtpTimestamp"]
        expected = observation["expectedDisplayTimeMs"]
        if not isinstance(rtp, int) or not isinstance(expected, (int, float)):
            continue
        delta = (rtp - cast(int, base["wireRtpTimestamp"])) & 0xFFFF_FFFF
        source_raw_ms = (
            float(base["sourceMonotonicRawNanoseconds"]) / 1_000_000.0 + float(delta) / 90.0
        )
        clock = matching_clock(samples, source_raw_ms)
        sender_render_ms = float(expected) - float(clock["offsetMilliseconds"])
        latency = sender_render_ms - source_raw_ms
        require(math.isfinite(latency) and latency >= 0.0, "render_latency_invalid")
        results.append((source_raw_ms, latency, float(clock["uncertaintyMilliseconds"])))
    require(bool(results), "render_timing_metadata_unavailable")
    return results


def validate_network(run: dict[str, Any], matrix: dict[str, Any]) -> None:
    network = cast(dict[str, Any], run["network"])
    topology = cast(dict[str, Any], matrix["topology"])
    require(
        network["oneWayDelayMilliseconds"] == topology["oneWayDelayMilliseconds"], "delay_changed"
    )
    require(network["queueLimitPackets"] == topology["queueLimitPackets"], "queue_limit_changed")
    expected_rate = cast(int, run["capBitsPerSecond"]) // 1000
    require(network["baseRateKbit"] == expected_rate, "network_base_rate_mismatch")
    expected_base = topology["qdiscCommandTemplate"].format(
        device="{device}", rate_kbit=expected_rate
    )
    commands = cast(list[str], network["appliedCommands"])
    if run["kind"] == "collapse":
        collapse_rate = (
            cast(dict[str, Any], matrix["collapse"])["collapseRateBitsPerSecond"] // 1000
        )
        expected_collapse = topology["qdiscCommandTemplate"].format(
            device="{device}", rate_kbit=collapse_rate
        )
        require(network["collapseRateKbit"] == collapse_rate, "collapse_rate_mismatch")
        require(
            sum(command == expected_collapse for command in commands) == 2,
            "collapse_command_mismatch",
        )
        require(
            sum(command == expected_base for command in commands) == 4,
            "base_qdisc_command_mismatch",
        )
        require(len(commands) == 6, "collapse_command_count_invalid")
    else:
        require(
            sum(command == expected_base for command in commands) == 2,
            "base_qdisc_command_mismatch",
        )
        require(
            network["collapseRateKbit"] is None and len(commands) == 2,
            "steady_command_count_invalid",
        )
    for qdisc in (network["senderQdisc"], network["receiverQdisc"]):
        require(any(item.get("kind") == "netem" for item in qdisc), "netem_qdisc_missing")


def validate_capture(
    root: Path,
    run: dict[str, Any],
    packet_reader: Callable[[Path], tuple[int, int]],
) -> float:
    capture = cast(dict[str, Any], run["capture"])
    pcap = checked_artifact(root, capture["file"], capture["sha256"])
    packet_count, captured_bytes = packet_reader(pcap)
    require(packet_count == capture["packetCount"], "capture_packet_count_mismatch")
    require(captured_bytes == capture["ipTotalBytes"], "capture_ip_total_mismatch")
    result = cast(dict[str, Any], cast(dict[str, Any], run["sender"])["result"])
    production = cast(int, result["wireEgressBytes"])
    relative_error = abs(float(production - captured_bytes)) / float(max(captured_bytes, 1))
    return relative_error


def validate_steady_run(run: dict[str, Any], matrix: dict[str, Any]) -> dict[str, Any]:
    acceptance = cast(dict[str, Any], matrix["acceptance"])
    sender = cast(dict[str, Any], run["sender"])
    samples = cast(list[dict[str, Any]], sender["samples"])
    timing = cast(dict[str, Any], run["timing"])
    start = cast(int, timing["measurementStartSenderMilliseconds"])
    end = cast(int, timing["measurementEndSenderMilliseconds"])
    steady = cast(dict[str, Any], matrix["steady"])
    require(end - start == steady["measurementSeconds"] * 1000, "steady_duration_mismatch")
    wire_rates = window_rates(samples, "wireEgressBytes", start, end)
    wire_p95 = percentile(wire_rates, 0.95)
    cap = float(run["capBitsPerSecond"])
    require(wire_p95 <= cap * acceptance["wireP95MaximumCapFraction"], "steady_wire_p95_exceeded")
    delivered = interpolate(samples, "deliveredFrames", end) - interpolate(
        samples, "deliveredFrames", start
    )
    transported = interpolate(samples, "transportedAccessUnits", end) - interpolate(
        samples, "transportedAccessUnits", start
    )
    compositor_fps = delivered / float(steady["measurementSeconds"])
    delivery_fraction = delivered / max(transported, 1.0)
    require(
        compositor_fps >= acceptance["minimumCompositorFramesPerSecond"],
        "steady_compositor_fps_below_gate",
    )
    require(
        delivery_fraction >= acceptance["minimumDeliveryFraction"], "steady_delivery_below_gate"
    )
    in_window = [item for item in samples if start <= item["senderMilliseconds"] <= end]
    require(
        bool(in_window)
        and all(
            item["presentationProfile"] == acceptance["steadyPresentationProfile"]
            and item["width"] == 1280
            and item["height"] == 720
            and item["framesPerSecond"] == 30
            for item in in_window
        ),
        "steady_profile_reduced",
    )
    raw_start = float(in_window[0]["senderMonotonicRawNanoseconds"]) / 1_000_000.0
    raw_end = float(in_window[-1]["senderMonotonicRawNanoseconds"]) / 1_000_000.0
    latencies = [
        (latency, uncertainty)
        for raw, latency, uncertainty in observation_latencies(run)
        if raw_start <= raw <= raw_end
    ]
    require(bool(latencies), "steady_render_timing_empty")
    latency_p95 = percentile([item[0] for item in latencies], 0.95)
    maximum_uncertainty = max(item[1] for item in latencies)
    require(
        latency_p95 + maximum_uncertainty <= acceptance["maximumP95LatencyMilliseconds"],
        "steady_latency_p95_exceeded",
    )
    return {
        "wireP95BitsPerSecond": wire_p95,
        "compositorFramesPerSecond": compositor_fps,
        "deliveryFraction": delivery_fraction,
        "latencyP95Milliseconds": latency_p95,
        "maximumClockUncertaintyMilliseconds": maximum_uncertainty,
    }


def validate_collapse_run(run: dict[str, Any], matrix: dict[str, Any]) -> dict[str, Any]:
    acceptance = cast(dict[str, Any], matrix["acceptance"])
    collapse = cast(dict[str, Any], matrix["collapse"])
    sender = cast(dict[str, Any], run["sender"])
    samples = cast(list[dict[str, Any]], sender["samples"])
    timing = cast(dict[str, Any], run["timing"])
    collapse_start = cast(int, timing["collapseStartSenderMilliseconds"])
    restored = cast(int, timing["restoredAtSenderMilliseconds"])
    recovery_tick = cast(int, timing["recoveryTickSenderMilliseconds"])
    require(
        restored - collapse_start == collapse["durationSeconds"] * 1000,
        "collapse_duration_mismatch",
    )
    require(restored <= recovery_tick <= restored + 100, "recovery_tick_not_first_controller_tick")
    pre = [
        item
        for item in samples
        if collapse_start - 1000 <= item["senderMilliseconds"] < collapse_start
    ]
    require(bool(pre), "collapse_baseline_samples_missing")
    baseline_profile = cast(str, pre[-1]["presentationProfile"])
    profile_order = ["720p30", "720p24", "720p15"]
    sample_times = [cast(int, item["senderMilliseconds"]) for item in samples]
    restore_index = bisect_left(sample_times, restored)
    require(restore_index < len(samples), "collapse_restore_sample_missing")
    at_restore = samples[restore_index]
    outstanding = profile_order.index(cast(str, at_restore["presentationProfile"]))
    profile_deadline = min(
        2000 + 2000 * outstanding, acceptance["maximumProfileRecoveryMilliseconds"]
    )
    restored_profiles = [
        item
        for item in samples
        if restored <= item["senderMilliseconds"] <= restored + profile_deadline
    ]
    require(
        any(item["presentationProfile"] == baseline_profile for item in restored_profiles),
        "presentation_profile_recovery_deadline_missed",
    )
    recovery_index = bisect_left(sample_times, recovery_tick)
    require(recovery_index < len(samples), "recovery_tick_sample_missing")
    recovery_sample = samples[recovery_index]
    require(
        recovery_sample["senderMilliseconds"] == recovery_tick,
        "recovery_tick_sample_missing",
    )
    raw_tick = float(recovery_sample["senderMonotonicRawNanoseconds"]) / 1_000_000.0
    collapse_index = bisect_left(sample_times, collapse_start)
    require(collapse_index < len(samples), "collapse_start_sample_missing")
    collapse_raw = float(samples[collapse_index]["senderMonotonicRawNanoseconds"]) / 1_000_000.0
    recovery_points = cast(list[dict[str, Any]], sender["recoveryPoints"])
    request_samples = [
        item
        for item in samples
        if recovery_tick
        <= item["senderMilliseconds"]
        <= recovery_tick + acceptance["recoveryIdrAndFrameAgeDeadlineMilliseconds"]
        and item["action"] == "REQUEST_RECOVERY_IDR"
    ]
    require(bool(request_samples), "recovery_idr_not_requested")
    request_raw = float(request_samples[0]["senderMonotonicRawNanoseconds"]) / 1_000_000.0
    eligible_points = [
        point
        for point in recovery_points
        if float(point["sourceMonotonicRawNanoseconds"]) / 1_000_000.0 >= request_raw
    ]
    require(bool(eligible_points), "recovery_idr_missing")
    recovery_point = min(eligible_points, key=lambda item: item["sourceMonotonicRawNanoseconds"])
    recovery_raw = float(recovery_point["sourceMonotonicRawNanoseconds"]) / 1_000_000.0
    recovery_rtp = cast(int, recovery_point["wireRtpTimestamp"])
    rendered = [
        observation
        for observation in cast(
            list[dict[str, Any]], cast(dict[str, Any], run["receiver"])["observations"]
        )
        if isinstance(observation["rtpTimestamp"], int)
        and isinstance(observation["expectedDisplayTimeMs"], (int, float))
        and ((observation["rtpTimestamp"] - recovery_rtp) & 0xFFFF_FFFF) < 0x8000_0000
    ]
    require(bool(rendered), "recovery_idr_not_rendered")
    first_render = rendered[0]
    clock = matching_clock(samples, recovery_raw)
    recovery_render_raw = float(first_render["expectedDisplayTimeMs"]) - float(
        clock["offsetMilliseconds"]
    )
    recovery_idr_ms = recovery_render_raw - raw_tick
    require(
        recovery_idr_ms <= acceptance["recoveryIdrAndFrameAgeDeadlineMilliseconds"],
        "recovery_idr_deadline_missed",
    )
    latencies = observation_latencies(run)
    baseline_latencies = [
        latency
        for raw, latency, _ in latencies
        if collapse_raw - collapse["baselineSeconds"] * 1000 <= raw < collapse_raw
    ]
    recovered_latencies = [
        latency
        for raw, latency, _ in latencies
        if recovery_raw
        <= raw
        <= raw_tick + acceptance["recoveryIdrAndFrameAgeDeadlineMilliseconds"]
    ]
    require(
        bool(baseline_latencies) and bool(recovered_latencies), "recovery_latency_samples_missing"
    )
    baseline_p95 = percentile(baseline_latencies, 0.95)
    recovery_p95 = percentile(recovered_latencies, 0.95)
    require(recovery_p95 <= baseline_p95 * 1.10, "recovery_frame_age_deadline_missed")
    post_idr_expected = [
        float(item["expectedDisplayTimeMs"])
        for item in rendered
        if isinstance(item["expectedDisplayTimeMs"], (int, float))
    ]
    require(len(post_idr_expected) >= 2, "post_idr_frame_sequence_missing")
    maximum_freeze = max(
        right - left for left, right in zip(post_idr_expected, post_idr_expected[1:], strict=False)
    )
    require(
        maximum_freeze <= acceptance["maximumPostIdrFreezeMilliseconds"], "post_idr_freeze_exceeded"
    )
    receiver = cast(dict[str, Any], run["receiver"])
    require(not receiver["errors"], "receiver_reported_corruption_or_error")
    return {
        "outstandingProfileSteps": outstanding,
        "profileRecoveryDeadlineMilliseconds": profile_deadline,
        "recoveryIdrRenderMilliseconds": recovery_idr_ms,
        "baselineLatencyP95Milliseconds": baseline_p95,
        "recoveredLatencyP95Milliseconds": recovery_p95,
        "maximumPostIdrFreezeMilliseconds": maximum_freeze,
    }


def expected_runs(matrix: dict[str, Any]) -> dict[str, tuple[str, str, int]]:
    expected: dict[str, tuple[str, str, int]] = {}
    steady = cast(dict[str, Any], matrix["steady"])
    for browser in cast(list[dict[str, Any]], steady["browsers"]):
        for cap in cast(list[int], steady["capsBitsPerSecond"]):
            identifier = f"steady-{browser['name']}-{cap // 1000}k"
            expected[identifier] = ("steady", cast(str, browser["name"]), cap)
    collapse = cast(dict[str, Any], matrix["collapse"])
    for browser_name in cast(list[str], collapse["browsers"]):
        cap = cast(int, collapse["baseCapBitsPerSecond"])
        expected[f"collapse-{browser_name}-{cap // 1000}k"] = (
            "collapse",
            browser_name,
            cap,
        )
    return expected


def validate_controller_network_qualification(
    evidence_root: Path,
    schema_path: Path = ROOT / "schemas" / "controller-network-evidence-v1.schema.json",
    matrix_path: Path = ROOT / "qualification" / "controller-network-v1.json",
    packet_reader: Callable[[Path], tuple[int, int]] = tshark_ip_totals,
) -> dict[str, Any]:
    root = evidence_root.resolve(strict=True)
    require(root.is_dir(), "evidence_root_not_directory")
    evidence = load_object(root / "evidence.json")
    validate_schema(evidence, schema_path)
    matrix = load_object(matrix_path)
    require(evidence["matrixSha256"] == sha256_file(matrix_path), "matrix_hash_mismatch")
    require(
        evidence["controllerProtocolSha256"] == matrix["controllerProtocolSha256"],
        "controller_protocol_hash_mismatch",
    )
    deterministic = cast(dict[str, Any], evidence["deterministicTransportValidation"])
    deterministic_path = checked_artifact(root, deterministic["artifact"], deterministic["sha256"])
    deterministic_value = load_object(deterministic_path)
    require(deterministic_value.get("status") == "PASSED", "deterministic_transport_not_passed")

    expected = expected_runs(matrix)
    runs = cast(list[dict[str, Any]], evidence["runs"])
    require(len({run["runId"] for run in runs}) == len(runs), "duplicate_run_id")
    require({run["runId"] for run in runs} == set(expected), "qualification_matrix_incomplete")
    browser_versions = {
        item["name"]: item["version"] for item in cast(dict[str, Any], matrix["steady"])["browsers"]
    }
    acceptance = cast(dict[str, Any], matrix["acceptance"])
    summaries: list[dict[str, Any]] = []
    for run in sorted(runs, key=lambda item: cast(str, item["runId"])):
        kind, browser, cap = expected[cast(str, run["runId"])]
        require(
            run["kind"] == kind
            and cast(dict[str, Any], run["browser"])["name"] == browser
            and run["capBitsPerSecond"] == cap,
            f"run_identity_mismatch:{run['runId']}",
        )
        require(
            cast(dict[str, Any], run["browser"])["version"] == browser_versions[browser],
            f"browser_version_mismatch:{run['runId']}",
        )
        validate_network(run, matrix)
        samples = cast(list[dict[str, Any]], cast(dict[str, Any], run["sender"])["samples"])
        validate_samples(samples, acceptance)
        relative_error = validate_capture(root, run, packet_reader)
        require(
            relative_error <= acceptance["counterMaximumRelativeError"],
            f"counter_capture_error_exceeded:{run['runId']}",
        )
        metrics = (
            validate_steady_run(run, matrix)
            if kind == "steady"
            else validate_collapse_run(run, matrix)
        )
        trace_sha256 = replay_samples(samples)
        validate_sender_result(cast(dict[str, Any], run["sender"]), samples)
        summaries.append(
            {
                "runId": run["runId"],
                "kind": kind,
                "browser": browser,
                "capBitsPerSecond": cap,
                "controllerTraceSha256": trace_sha256,
                "counterCaptureRelativeError": relative_error,
                **metrics,
            }
        )
    return {
        "schemaVersion": 1,
        "protocol": "glyphrelay-controller-network-validation-v1",
        "status": "PASSED",
        "sourceCommit": evidence["sourceCommit"],
        "sourceBundleId": evidence["sourceBundleId"],
        "controllerProtocolSha256": evidence["controllerProtocolSha256"],
        "matrixSha256": evidence["matrixSha256"],
        "runs": summaries,
    }


def exclusive_write_json(path: Path, value: dict[str, Any]) -> None:
    content = (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode()
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
        0o600,
    )
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("evidence_root", type=Path)
    result.add_argument(
        "--schema", type=Path, default=ROOT / "schemas/controller-network-evidence-v1.schema.json"
    )
    result.add_argument(
        "--matrix", type=Path, default=ROOT / "qualification/controller-network-v1.json"
    )
    result.add_argument("--output", type=Path)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        result = validate_controller_network_qualification(
            arguments.evidence_root,
            schema_path=arguments.schema,
            matrix_path=arguments.matrix,
        )
        if arguments.output:
            exclusive_write_json(arguments.output, result)
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
        return 0
    except (
        ControllerNetworkValidationError,
        OSError,
        ValueError,
        json.JSONDecodeError,
    ) as error:
        print(json.dumps({"status": "FAILED", "reason": str(error)}, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
