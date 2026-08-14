from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from tools.validate_m0_browser_offers import (
    BrowserOfferValidationError,
    validate_m0_browser_offers,
)

SCHEMA = Path("schemas/m0-browser-offers-v1.schema.json").resolve(strict=True)
LOCK = Path("dependencies.lock.json").resolve(strict=True)


def browser(name: str, version: str, revision: str) -> dict[str, Any]:
    profile = "42e01f"
    return {
        "browser": name,
        "expectedVersion": version,
        "expectedRevision": revision,
        "actualVersion": version,
        "executablePath": f"/qualification/{name}",
        "executableSha256": "a" * 64 if name == "chromium" else "b" * 64,
        "capabilities": {"codecs": []},
        "offerSdp": (
            "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102 103\r\n"
            "a=rtpmap:102 H264/90000\r\n"
            f"a=fmtp:102 profile-level-id={profile};packetization-mode=1;"
            "level-asymmetry-allowed=1\r\n"
            "a=rtcp-fb:102 nack\r\na=rtcp-fb:102 nack pli\r\n"
            "a=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n"
        ),
        "compatibility": {
            "compatible": True,
            "reason": "sharing_profile_offer_compatible",
            "presentation": "720p30",
            "requiredLevelIdc": 31,
            "formats": [
                {
                    "payloadType": 102,
                    "profileLevelId": profile,
                    "profileIdc": 66,
                    "profileIop": 224,
                    "levelIdc": 31,
                    "profileFamily": "constrained_baseline",
                    "packetizationMode": 1,
                    "levelAsymmetryAllowed": True,
                    "feedback": ["nack", "nack pli"],
                }
            ],
            "videoPayloadTypes": [102, 103],
            "rtxPayloadTypes": [103],
        },
        "diagnostics": [],
    }


def valid_report() -> dict[str, Any]:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))["playwright"]
    return {
        "schemaVersion": 1,
        "protocol": "glyphrelay-browser-offers-v1",
        "status": "PASSED",
        "generatedAtUtc": "2026-08-14T12:00:00Z",
        "host": {
            "platform": "linux",
            "release": "6.8.0",
            "architecture": "x64",
            "nodeVersion": "v24.6.0",
            "playwrightVersion": lock["version"],
        },
        "browsers": [
            browser("chromium", lock["chromium"]["version"], lock["chromium"]["revision"]),
            browser("firefox", lock["firefox"]["version"], lock["firefox"]["revision"]),
        ],
        "failures": [],
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.write_text(json.dumps(report) + "\n", encoding="utf-8")


def test_complete_exact_browser_offer_set_passes(tmp_path: Path) -> None:
    report_path = tmp_path / "offers.json"
    write_report(report_path, valid_report())
    result = validate_m0_browser_offers(report_path, SCHEMA, LOCK)
    assert result["status"] == "PASSED"


def test_duplicate_browser_cannot_satisfy_two_browser_gate(tmp_path: Path) -> None:
    report = valid_report()
    report["browsers"][1] = report["browsers"][0]
    report_path = tmp_path / "offers.json"
    write_report(report_path, report)
    with pytest.raises(BrowserOfferValidationError, match="browser_set_invalid"):
        validate_m0_browser_offers(report_path, SCHEMA, LOCK)


def test_spoofed_derived_profile_identity_fails(tmp_path: Path) -> None:
    report = valid_report()
    report["browsers"][0]["compatibility"]["formats"][0]["profileIdc"] = 77
    report_path = tmp_path / "offers.json"
    write_report(report_path, report)
    with pytest.raises(BrowserOfferValidationError, match="browser_profile_identity_mismatch"):
        validate_m0_browser_offers(report_path, SCHEMA, LOCK)


def test_offer_without_nack_pli_feedback_fails(tmp_path: Path) -> None:
    report = valid_report()
    report["browsers"][1]["compatibility"]["formats"][0]["feedback"] = ["nack"]
    report_path = tmp_path / "offers.json"
    write_report(report_path, report)
    with pytest.raises(BrowserOfferValidationError, match="browser_nack_pli_feedback_missing"):
        validate_m0_browser_offers(report_path, SCHEMA, LOCK)
