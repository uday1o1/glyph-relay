from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from tools.replay_controller_trace import ReplayError, replay_trace  # noqa: E402

FIXTURES = [
    "stable_link",
    "emphasis_overshoot",
    "stale_remb",
    "missing_remb",
    "sudden_collapse",
    "high_rtt_without_loss",
    "recovery",
    "unusable",
]


def load_records(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        value = json.loads(line)
        if not isinstance(value, dict):
            raise RuntimeError("controller trace line is not an object")
        records.append(value)
    return records


def write_records(path: Path, records: list[dict[str, Any]]) -> None:
    path.write_text(
        "".join(
            json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n" for record in records
        ),
        encoding="utf-8",
    )


def require_fixture_semantics(fixture: str, records: list[dict[str, Any]]) -> None:
    if not records:
        raise RuntimeError(f"{fixture} emitted no decisions")
    actions = {record.get("action") for record in records}
    states = {record.get("resultingState") for record in records}
    if fixture == "stable_link" and (states != {"STABLE"} or actions != {"NONE"}):
        raise RuntimeError("stable fixture did not remain stable")
    if fixture == "emphasis_overshoot" and not any(
        record.get("roundingResults", {}).get("pinnedRegionViolationVisible") for record in records
    ):
        raise RuntimeError("emphasis fixture did not surface its pin violation")
    if fixture == "stale_remb":
        availability = [
            record.get("roundingResults", {}).get("feedbackRembAvailable") for record in records
        ]
        if True not in availability or availability[-1] is not False:
            raise RuntimeError("stale REMB fixture did not cross the freshness boundary")
    if fixture == "missing_remb" and any(
        record.get("roundingResults", {}).get("feedbackRembAvailable") for record in records
    ):
        raise RuntimeError("missing REMB fixture invented feedback")
    if fixture == "sudden_collapse" and "CONGESTED" not in states:
        raise RuntimeError("collapse fixture did not enter congestion")
    if fixture == "high_rtt_without_loss" and "RATE_PRESSURE" not in states:
        raise RuntimeError("high RTT fixture did not enter rate pressure")
    if fixture == "recovery" and not {
        "REQUEST_RECOVERY_IDR",
        "RESTORE_PRESENTATION_PROFILE",
    }.issubset(actions):
        raise RuntimeError("recovery fixture did not request an IDR and restore a profile")
    if fixture == "unusable" and records[-1].get("resultingState") != "UNUSABLE":
        raise RuntimeError("unusable fixture did not stop at the minimum stack")


def expect_replay_failure(path: Path, expected: str) -> None:
    try:
        replay_trace(path)
    except ReplayError as error:
        if expected not in str(error):
            raise RuntimeError(f"unexpected replay failure: {error}") from error
        return
    raise RuntimeError(f"seeded replay defect did not fail: {expected}")


def run_matrix(executable: Path) -> dict[str, str]:
    if not executable.is_file():
        raise RuntimeError("controller fixture executable is missing")
    digests: dict[str, str] = {}
    with tempfile.TemporaryDirectory(prefix="glyphrelay-controller-") as directory_name:
        directory = Path(directory_name)
        for fixture in FIXTURES:
            first = directory / f"{fixture}-first.jsonl"
            second = directory / f"{fixture}-second.jsonl"
            command = [str(executable), "--fixture", fixture, "--output"]
            subprocess.run(command + [str(first)], check=True)
            subprocess.run(command + [str(second)], check=True)
            if first.read_bytes() != second.read_bytes():
                raise RuntimeError(f"{fixture} production trace is nondeterministic")
            decisions, digest = replay_trace(first)
            if not decisions:
                raise RuntimeError(f"{fixture} replay produced no decisions")
            records = load_records(first)
            require_fixture_semantics(fixture, records)
            digests[fixture] = digest

        action_records = load_records(directory / "stable_link-first.jsonl")
        action_records[0]["action"] = "REDUCE_AUTOMATIC_EMPHASIS"
        action_defect = directory / "seeded-action-defect.jsonl"
        write_records(action_defect, action_records)
        expect_replay_failure(action_defect, "decision mismatch")

        future_records = load_records(directory / "stale_remb-first.jsonl")
        feedback = future_records[0]["consumedFeedback"][0]
        feedback["arrivalSequence"] = future_records[0]["arrivalSequence"]
        future_defect = directory / "seeded-future-feedback-defect.jsonl"
        write_records(future_defect, future_records)
        expect_replay_failure(future_defect, "consumed future feedback")

        no_clobber = subprocess.run(
            [
                str(executable),
                "--fixture",
                "stable_link",
                "--output",
                str(directory / "stable_link-first.jsonl"),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if no_clobber.returncode == 0 or "already exists" not in no_clobber.stderr:
            raise RuntimeError("controller fixture output did not fail closed on clobber")
    return digests


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run all frozen controller_v1 trace fixtures")
    parser.add_argument("--fixture-executable", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        digests = run_matrix(arguments.fixture_executable)
        print(json.dumps({"fixtures": digests, "status": "PASSED"}, sort_keys=True))
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps({"reason": str(error), "status": "FAILED"}, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
