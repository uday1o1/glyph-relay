from pathlib import Path

from tools.check_repository import scan
from tools.validate_doctor import validate_report


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
        "required": ["schema_version", "decision"],
        "properties": {"schema_version": {"const": 1}, "decision": {}},
    }
    try:
        validate_report({"schema_version": 1}, schema)
    except ValueError as error:
        assert "missing required keys" in str(error)
    else:
        raise AssertionError("missing doctor fields must fail validation")
