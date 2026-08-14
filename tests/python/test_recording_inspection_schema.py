from __future__ import annotations

import json
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = json.loads(
    (ROOT / "schemas" / "recording-inspection-v1.schema.json").read_text(encoding="utf-8")
)


def report() -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "passed": True,
        "state": "COMPLETE",
        "reason": "RECORDING_COMPLETE",
        "sessionId": "record_only",
        "recordingId": "0123456789abcdef0123456789abcdef",
        "mediaPath": "recording.h264",
        "sidecarPath": "recording.h264.json",
        "markerPath": "recording.h264.complete",
        "committedAccessUnits": 30,
        "committedMediaBytes": 1024,
    }


def test_recording_inspection_contract_accepts_complete_and_incomplete() -> None:
    validator = Draft202012Validator(SCHEMA)
    assert not list(validator.iter_errors(report()))
    incomplete = report()
    incomplete.update(
        {
            "state": "PREPARED_INCOMPLETE",
            "reason": "RECORDING_PREPARED_INCOMPLETE",
            "recordingId": "",
            "committedAccessUnits": 0,
            "committedMediaBytes": 0,
        }
    )
    assert not list(validator.iter_errors(incomplete))


def test_recording_inspection_contract_rejects_unknown_state_and_fields() -> None:
    invalid = report()
    invalid["state"] = "RECOVERED"
    invalid["unexpected"] = True
    paths = {error.json_path for error in Draft202012Validator(SCHEMA).iter_errors(invalid)}
    assert "$.state" in paths
    assert "$" in paths
