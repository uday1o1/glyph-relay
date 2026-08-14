from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class ReplayError(RuntimeError):
    pass


@dataclass
class Estimate:
    one: float | None = None
    five: float | None = None

    def trace(self) -> dict[str, float | None]:
        return {"fiveSecond": self.five, "oneSecond": self.one}


@dataclass
class RateEstimator:
    previous_counter: int | None = None
    previous_milliseconds: int | None = None
    estimate: Estimate | None = None

    def update(self, counter: int, now: int, scale: float) -> bool:
        if self.previous_counter is None or self.previous_milliseconds is None:
            self.previous_counter = counter
            self.previous_milliseconds = now
            return False
        if now <= self.previous_milliseconds:
            raise ReplayError("counter time did not strictly increase")
        if counter < self.previous_counter:
            self.previous_counter = counter
            self.previous_milliseconds = now
            self.estimate = None
            return True
        delta = counter - self.previous_counter
        dt = now - self.previous_milliseconds
        sample = float(delta) * scale * 1000.0 / float(dt)
        if self.estimate is None:
            self.estimate = Estimate(sample, sample)
        else:
            self.estimate.one = ewma(self.estimate.one, sample, float(dt), 1000.0)
            self.estimate.five = ewma(self.estimate.five, sample, float(dt), 5000.0)
        self.previous_counter = counter
        self.previous_milliseconds = now
        return False

    def trace(self) -> dict[str, float | None]:
        return self.estimate.trace() if self.estimate else Estimate().trace()


@dataclass
class GaugeEstimator:
    previous_milliseconds: int | None = None
    estimate: Estimate | None = None

    def update(self, sample: float, now: int) -> None:
        finite_nonnegative(sample, "gauge")
        if self.previous_milliseconds is None:
            self.previous_milliseconds = now
            self.estimate = Estimate(sample, sample)
            return
        if now < self.previous_milliseconds:
            raise ReplayError("gauge time regressed")
        assert self.estimate is not None
        dt = now - self.previous_milliseconds
        self.estimate.one = ewma(self.estimate.one, sample, float(dt), 1000.0)
        self.estimate.five = ewma(self.estimate.five, sample, float(dt), 5000.0)
        self.previous_milliseconds = now

    def trace(self, available: bool = True) -> dict[str, float | None]:
        if not available or self.estimate is None:
            return Estimate().trace()
        return self.estimate.trace()


def ewma(previous: float | None, sample: float, dt: float, tau: float) -> float:
    if previous is None:
        raise ReplayError("EWMA previous value is unavailable")
    alpha = 1.0 - math.exp(-dt / tau)
    return alpha * sample + (1.0 - alpha) * previous


def finite_nonnegative(value: float, field: str) -> None:
    if not math.isfinite(value) or value < 0.0:
        raise ReplayError(f"{field} must be finite and nonnegative")


def canonical(value: Any) -> bytes:
    return json.dumps(value, allow_nan=False, separators=(",", ":"), sort_keys=True).encode()


AUTOMATIC_CAPS = [4, 3, 2, 1, 0]
THRESHOLD_DELTAS = [0.0, 0.05, 0.10, 0.15]
PROFILES = [
    ("1080p30", 1920, 1080, 30),
    ("1080p24", 1920, 1080, 24),
    ("720p24", 1280, 720, 24),
    ("720p15", 1280, 720, 15),
]
MINIMUM_PAYLOAD_BPS = 100_000
PROJECTION_FIELDS = (
    "action",
    "estimatorInputs",
    "priorState",
    "reason",
    "resultingState",
    "roundingResults",
    "selectedLevelStack",
)


class ControllerReplay:
    def __init__(self, config: dict[str, Any]) -> None:
        self.base_target = integer(config, "basePayloadTargetBps")
        self.base_entry = number(config, "baseEntryThreshold")
        self.base_exit = number(config, "baseExitThreshold")
        if self.base_target < MINIMUM_PAYLOAD_BPS:
            raise ReplayError("base target is below the frozen minimum")
        self.state = "STABLE"
        self.auto_index = 0
        self.threshold_index = 0
        self.payload_step = 0
        self.profile_index = 0
        self.pressure_ticks = 0
        self.recovery_since: int | None = None
        self.stable_since: int | None = None
        self.unusable_since: int | None = None
        self.last_action: int | None = None
        self.last_restoration: int | None = None
        self.history: list[tuple[str, int]] = []
        self.last_tick_sequence: int | None = None
        self.last_tick_milliseconds: int | None = None
        self.last_feedback_sequence: int | None = None
        self.last_feedback_milliseconds: int | None = None
        self.elementary = RateEstimator()
        self.wire = RateEstimator()
        self.retransmission = RateEstimator()
        self.delivered = RateEstimator()
        self.queue_age = GaugeEstimator()
        self.loss = GaugeEstimator()
        self.rtt = GaugeEstimator()
        self.latest_loss: tuple[float, int] | None = None
        self.latest_rtt: tuple[float, int] | None = None
        self.latest_remb: tuple[float, int] | None = None

    def payload_target(self) -> int:
        reduced = float(self.base_target) * math.pow(0.90, float(self.payload_step))
        return max(MINIMUM_PAYLOAD_BPS, round(reduced))

    def levels(self) -> dict[str, Any]:
        name, width, height, fps = PROFILES[self.profile_index]
        return {
            "automaticEmphasisCap": AUTOMATIC_CAPS[self.auto_index],
            "framesPerSecond": fps,
            "height": height,
            "payloadAndVbvStep": self.payload_step,
            "payloadTargetBps": self.payload_target(),
            "presentationProfile": name,
            "protectedThresholdDelta": THRESHOLD_DELTAS[self.threshold_index],
            "width": width,
        }

    def at_unusable_levels(self) -> bool:
        return (
            self.auto_index == len(AUTOMATIC_CAPS) - 1
            and self.payload_target() == MINIMUM_PAYLOAD_BPS
            and self.profile_index == len(PROFILES) - 1
        )

    def degrade(self, now: int) -> str:
        action = "NONE"
        if self.auto_index + 1 < len(AUTOMATIC_CAPS):
            self.auto_index += 1
            action = "REDUCE_AUTOMATIC_EMPHASIS"
        elif self.threshold_index + 1 < len(THRESHOLD_DELTAS):
            self.threshold_index += 1
            action = "TIGHTEN_PROTECTED_THRESHOLD"
        elif self.payload_target() != MINIMUM_PAYLOAD_BPS:
            self.payload_step += 1
            action = "REDUCE_PAYLOAD_AND_VBV"
        elif self.profile_index + 1 < len(PROFILES):
            self.profile_index += 1
            action = "REDUCE_PRESENTATION_PROFILE"
        if action != "NONE":
            self.history.append((action, now))
            self.last_action = now
        return action

    def restore(self, now: int) -> str:
        if not self.history:
            return "NONE"
        applied, applied_at = self.history[-1]
        if now - applied_at < 2000:
            return "NONE"
        if self.last_restoration is not None and now - self.last_restoration < 2000:
            return "NONE"
        self.history.pop()
        restoration = {
            "REDUCE_PRESENTATION_PROFILE": "RESTORE_PRESENTATION_PROFILE",
            "REDUCE_PAYLOAD_AND_VBV": "RESTORE_PAYLOAD_AND_VBV",
            "TIGHTEN_PROTECTED_THRESHOLD": "RESTORE_PROTECTED_THRESHOLD",
            "REDUCE_AUTOMATIC_EMPHASIS": "RESTORE_AUTOMATIC_EMPHASIS",
        }[applied]
        if applied == "REDUCE_PRESENTATION_PROFILE":
            self.profile_index -= 1
        elif applied == "REDUCE_PAYLOAD_AND_VBV":
            self.payload_step -= 1
        elif applied == "TIGHTEN_PROTECTED_THRESHOLD":
            self.threshold_index -= 1
        else:
            self.auto_index -= 1
        self.last_action = now
        self.last_restoration = now
        return restoration

    def process(self, record: dict[str, Any]) -> dict[str, Any]:
        raw = mapping(record, "rawInput")
        config = mapping(raw, "controllerConfig")
        if config != {
            "baseEntryThreshold": self.base_entry,
            "baseExitThreshold": self.base_exit,
            "basePayloadTargetBps": self.base_target,
        }:
            raise ReplayError("controller config changed within one trace")
        sequence = integer(record, "arrivalSequence")
        now = integer(record, "senderArrivalMilliseconds")
        if self.last_tick_sequence is not None and sequence <= self.last_tick_sequence:
            raise ReplayError("tick arrival sequence did not strictly increase")
        if self.last_tick_milliseconds is not None and now <= self.last_tick_milliseconds:
            raise ReplayError("tick sender time did not strictly increase")
        self.last_tick_sequence = sequence
        self.last_tick_milliseconds = now

        for feedback_value in array(record, "consumedFeedback"):
            if not isinstance(feedback_value, dict):
                raise ReplayError("feedback event must be an object")
            feedback_sequence = integer(feedback_value, "arrivalSequence")
            feedback_time = integer(feedback_value, "senderArrivalMilliseconds")
            if feedback_sequence >= sequence or feedback_time > now:
                raise ReplayError("trace consumed future feedback")
            if (
                self.last_feedback_sequence is not None
                and feedback_sequence <= self.last_feedback_sequence
            ):
                raise ReplayError("feedback arrival sequence did not strictly increase")
            if (
                self.last_feedback_milliseconds is not None
                and feedback_time < self.last_feedback_milliseconds
            ):
                raise ReplayError("feedback sender time regressed")
            self.last_feedback_sequence = feedback_sequence
            self.last_feedback_milliseconds = feedback_time
            loss = optional_number(feedback_value, "lossFraction")
            rtt = optional_number(feedback_value, "roundTripTimeMilliseconds")
            remb = optional_number(feedback_value, "rembBitsPerSecond")
            if loss is not None:
                self.loss.update(loss, feedback_time)
                self.latest_loss = (loss, feedback_time)
            if rtt is not None:
                self.rtt.update(rtt, feedback_time)
                self.latest_rtt = (rtt, feedback_time)
            if (
                remb is not None
                and boolean(feedback_value, "rembPayloadTypeValid")
                and boolean(feedback_value, "rembRtcpSourceValid")
            ):
                self.latest_remb = (remb, feedback_time)

        counters = mapping(raw, "counters")
        resets = [
            self.elementary.update(integer(counters, "elementaryStreamBytes"), now, 8.0),
            self.wire.update(integer(counters, "wireEgressBytes"), now, 8.0),
            self.retransmission.update(integer(counters, "retransmissionBytes"), now, 8.0),
            self.delivered.update(integer(counters, "deliveredFrames"), now, 1.0),
        ]
        oldest = number(raw, "oldestMediaAgeMilliseconds")
        self.queue_age.update(oldest, now)

        loss_available = fresh(self.latest_loss, now)
        rtt_available = fresh(self.latest_rtt, now)
        remb_available = fresh(self.latest_remb, now)
        estimators = {
            "deliveredFramesPerSecond": self.delivered.trace(),
            "elementaryStreamBps": self.elementary.trace(),
            "lossFraction": self.loss.trace(loss_available),
            "oldestMediaAgeMilliseconds": self.queue_age.trace(),
            "retransmissionBps": self.retransmission.trace(),
            "roundTripTimeMilliseconds": self.rtt.trace(rtt_available),
            "wireEgressBps": self.wire.trace(),
        }
        effective_cap = float(integer(raw, "userWireCapBps"))
        if remb_available:
            assert self.latest_remb is not None
            effective_cap = min(effective_cap, 0.90 * self.latest_remb[0])
        reserve = max(0.10 * effective_cap, 64_000.0)
        payload_budget = max(effective_cap - reserve, 0.0)
        wire_one = self.wire.estimate.one if self.wire.estimate else None
        loss_one = self.loss.estimate.one if loss_available and self.loss.estimate else None
        rtt_one = self.rtt.estimate.one if rtt_available and self.rtt.estimate else None
        pressure = (
            (wire_one is not None and wire_one > 0.95 * effective_cap)
            or oldest > 50.0
            or (loss_one is not None and loss_one > 0.03)
            or (rtt_one is not None and rtt_one > 250.0)
            or float(self.payload_target()) > payload_budget
        )
        hard = (
            (wire_one is not None and wire_one > 1.10 * effective_cap)
            or oldest >= 100.0
            or integer(raw, "pacerQueueBytes") >= 4 * 1024 * 1024
        )
        recovery_compliant = (
            wire_one is not None
            and wire_one < 0.85 * effective_cap
            and oldest < 1000.0 / float(PROFILES[self.profile_index][3])
            and (loss_one is None or loss_one < 0.01)
        )
        self.pressure_ticks = self.pressure_ticks + 1 if pressure else 0
        if recovery_compliant:
            if self.recovery_since is None:
                self.recovery_since = now
        else:
            self.recovery_since = None
            self.stable_since = None

        prior_state = self.state
        action = "NONE"
        request_idr = False
        reason = ""
        if self.state != "UNUSABLE":
            if hard:
                self.state = "CONGESTED"
                self.stable_since = None
                reason = "hard_congestion_predicate"
            elif self.state in ("STABLE", "RECOVERY") and self.pressure_ticks >= 3:
                self.state = "RATE_PRESSURE"
                self.stable_since = None
                reason = "three_consecutive_pressure_ticks"
            elif (
                self.state in ("RATE_PRESSURE", "CONGESTED")
                and self.recovery_since is not None
                and now - self.recovery_since >= 1000
            ):
                self.state = "RECOVERY"
                self.stable_since = self.recovery_since
                self.last_restoration = now
                action = "REQUEST_RECOVERY_IDR"
                request_idr = True
                reason = "continuous_recovery_entry"
            elif (
                self.state == "RECOVERY"
                and self.stable_since is not None
                and now - self.stable_since >= 5000
            ):
                self.state = "STABLE"
                reason = "five_seconds_continuously_compliant"

        if self.state != "UNUSABLE" and self.at_unusable_levels() and hard:
            if self.unusable_since is None:
                self.unusable_since = now
            if now - self.unusable_since >= 3000:
                self.state = "UNUSABLE"
                action = "STOP_UNUSABLE_LINK"
                reason = "minimum_stack_hard_violation_for_three_seconds"
        else:
            self.unusable_since = None

        action_interval = self.last_action is None or now - self.last_action >= 500
        if action == "NONE" and self.state in ("RATE_PRESSURE", "CONGESTED") and action_interval:
            action = self.degrade(now)
            if action != "NONE":
                reason = "ordered_degradation_step"
        elif action == "NONE" and recovery_compliant and self.state in ("RECOVERY", "STABLE"):
            action = self.restore(now)
            if action != "NONE":
                reason = "reverse_order_restoration_step"

        starts_profile_epoch = action in (
            "REDUCE_PRESENTATION_PROFILE",
            "RESTORE_PRESENTATION_PROFILE",
        )
        if starts_profile_epoch:
            request_idr = True
        if not reason:
            reason = "no_state_or_level_change"
        levels = self.levels()
        entry = min(self.base_entry + levels["protectedThresholdDelta"], 1.0)
        exit_threshold = min(self.base_exit + levels["protectedThresholdDelta"], entry - 0.10)
        return {
            "action": action,
            "estimatorInputs": estimators,
            "priorState": prior_state,
            "reason": reason,
            "resultingState": self.state,
            "roundingResults": {
                "controlReserveBps": reserve,
                "counterReset": any(resets),
                "effectiveWireCapBps": effective_cap,
                "entryThreshold": entry,
                "exitThreshold": exit_threshold,
                "feedbackLossAvailable": loss_available,
                "feedbackRembAvailable": remb_available,
                "feedbackRttAvailable": rtt_available,
                "payloadBudgetBps": payload_budget,
                "pinnedRegionViolationVisible": boolean(raw, "pinnedRegionViolation"),
                "requestIdrWithParameterSets": request_idr,
                "startsDependencyEpoch": starts_profile_epoch,
                "startsGeometryEpoch": starts_profile_epoch,
            },
            "selectedLevelStack": levels,
        }


def fresh(value: tuple[float, int] | None, now: int) -> bool:
    return value is not None and now >= value[1] and now - value[1] <= 2000


def mapping(value: dict[str, Any], field: str) -> dict[str, Any]:
    result = value.get(field)
    if not isinstance(result, dict):
        raise ReplayError(f"{field} must be an object")
    return result


def array(value: dict[str, Any], field: str) -> list[Any]:
    result = value.get(field)
    if not isinstance(result, list):
        raise ReplayError(f"{field} must be an array")
    return result


def integer(value: dict[str, Any], field: str) -> int:
    result = value.get(field)
    if isinstance(result, bool) or not isinstance(result, int) or result < 0:
        raise ReplayError(f"{field} must be a nonnegative integer")
    return result


def number(value: dict[str, Any], field: str) -> float:
    result = value.get(field)
    if isinstance(result, bool) or not isinstance(result, (int, float)):
        raise ReplayError(f"{field} must be a number")
    converted = float(result)
    finite_nonnegative(converted, field)
    return converted


def optional_number(value: dict[str, Any], field: str) -> float | None:
    if value.get(field) is None:
        return None
    return number(value, field)


def boolean(value: dict[str, Any], field: str) -> bool:
    result = value.get(field)
    if not isinstance(result, bool):
        raise ReplayError(f"{field} must be a boolean")
    return result


def replay_trace(path: Path) -> tuple[list[bytes], str]:
    if not path.is_file():
        raise ReplayError("trace path is not a file")
    records: list[dict[str, Any]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ReplayError(f"trace line {line_number} is invalid JSON") from error
        if not isinstance(value, dict):
            raise ReplayError(f"trace line {line_number} must be an object")
        records.append(value)
    if not records:
        raise ReplayError("trace is empty")
    first_raw = mapping(records[0], "rawInput")
    replay = ControllerReplay(mapping(first_raw, "controllerConfig"))
    decision_stream: list[bytes] = []
    for index, record in enumerate(records, start=1):
        actual = replay.process(record)
        expected = {field: record.get(field) for field in PROJECTION_FIELDS}
        actual_bytes = canonical(actual)
        expected_bytes = canonical(expected)
        if actual_bytes != expected_bytes:
            mismatched = [
                field
                for field in PROJECTION_FIELDS
                if canonical(actual[field]) != canonical(expected[field])
            ]
            detail = ",".join(mismatched)
            first = mismatched[0] if mismatched else "unknown"
            raise ReplayError(
                f"decision mismatch at trace line {index}: {detail}; "
                f"actual_{first}={canonical(actual.get(first))[:512].decode()}; "
                f"expected_{first}={canonical(expected.get(first))[:512].decode()}"
            )
        decision_stream.append(actual_bytes)
    import hashlib

    digest = hashlib.sha256(b"\n".join(decision_stream) + b"\n").hexdigest()
    return decision_stream, digest


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Replay a controller_v1 production JSONL trace")
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        decisions, digest = replay_trace(arguments.trace)
        if arguments.output:
            if arguments.output.exists():
                raise ReplayError("decision output already exists")
            arguments.output.write_bytes(b"\n".join(decisions) + b"\n")
        print(
            json.dumps(
                {
                    "decisionCount": len(decisions),
                    "decisionStreamSha256": digest,
                    "status": "REPLAYED",
                },
                sort_keys=True,
            )
        )
        return 0
    except (OSError, UnicodeError, ReplayError) as error:
        print(json.dumps({"reason": str(error), "status": "FAILED"}, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
