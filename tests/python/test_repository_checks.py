import json
from copy import deepcopy
from pathlib import Path

from tools.check_repository import scan
from tools.validate_doctor import validate_report

ROOT = Path(__file__).resolve().parents[2]


def test_secret_scanner_rejects_private_key(tmp_path: Path) -> None:
    candidate = tmp_path / "credential.txt"
    marker = "-----BEGIN " + "PRIVATE KEY-----"
    candidate.write_text(marker, encoding="utf-8")
    assert scan([candidate]) == [f"possible private key: {candidate}"]


def test_secret_scanner_rejects_generated_media(tmp_path: Path) -> None:
    candidate = tmp_path / "capture.h264"
    candidate.write_bytes(b"content")
    assert scan([candidate]) == [f"forbidden generated or sensitive artifact: {candidate}"]


def test_doctor_validator_rejects_missing_keys() -> None:
    schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "required": ["schema_version", "decision"],
        "properties": {"schema_version": {"const": 1}, "decision": {"type": "object"}},
    }
    try:
        validate_report({"schema_version": 1}, schema)
    except ValueError as error:
        assert "'decision' is a required property" in str(error)
    else:
        raise AssertionError("missing doctor fields must fail validation")


def test_doctor_fixture_satisfies_complete_schema() -> None:
    schema = json.loads((ROOT / "schemas/doctor-v1.schema.json").read_text(encoding="utf-8"))
    report = json.loads(
        (ROOT / "tests/fixtures/doctor/enhanced-v1.json").read_text(encoding="utf-8")
    )
    validate_report(report, schema)


def test_doctor_schema_rejects_malformed_nested_probe() -> None:
    schema = json.loads((ROOT / "schemas/doctor-v1.schema.json").read_text(encoding="utf-8"))
    report = json.loads(
        (ROOT / "tests/fixtures/doctor/enhanced-v1.json").read_text(encoding="utf-8")
    )
    malformed = deepcopy(report)
    malformed["nvenc"]["emphasis_map"]["status"] = "probably"
    try:
        validate_report(malformed, schema)
    except ValueError as error:
        assert "nvenc.emphasis_map.status" in str(error)
    else:
        raise AssertionError("malformed nested probe must fail validation")
