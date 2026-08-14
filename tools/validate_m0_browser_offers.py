#!/usr/bin/env python3
"""Independently validate exact-browser SDP evidence for Milestone 0."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, cast

from jsonschema import Draft202012Validator, FormatChecker


class BrowserOfferValidationError(RuntimeError):
    """Raised when browser-offer evidence cannot support the gate."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise BrowserOfferValidationError(reason)


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BrowserOfferValidationError(f"json_read_failed:{path.name}") from error
    require(isinstance(value, dict), f"json_object_required:{path.name}")
    return cast(dict[str, Any], value)


def constrained_baseline(profile_idc: int, profile_iop: int) -> bool:
    return bool(
        (profile_idc == 0x42 and profile_iop & 0x40)
        or (profile_idc == 0x4D and profile_iop & 0x80)
        or (profile_idc == 0x58 and profile_iop & 0xC0 == 0xC0)
    )


def validate_m0_browser_offers(
    report_path: Path, schema_path: Path, dependency_lock_path: Path
) -> dict[str, Any]:
    report = load_object(report_path)
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema, format_checker=FormatChecker()).iter_errors(report),
        key=lambda item: item.json_path,
    )
    require(
        not errors,
        f"schema_validation_failed:{errors[0].json_path}:{errors[0].message}" if errors else "",
    )
    require(report["status"] == "PASSED", f"browser_offer_gate_not_passed:{report['status']}")
    require(report["failures"] == [], "browser_offer_failures_not_empty")

    lock = load_object(dependency_lock_path)
    raw_playwright = lock.get("playwright")
    require(isinstance(raw_playwright, dict), "playwright_lock_missing")
    playwright = cast(dict[str, Any], raw_playwright)
    require(
        report["host"]["playwrightVersion"] == playwright.get("version"),
        "playwright_version_mismatch",
    )
    declared = {browser["browser"]: browser for browser in report["browsers"]}
    require(len(declared) == 2 and set(declared) == {"chromium", "firefox"}, "browser_set_invalid")
    for name in ("chromium", "firefox"):
        browser = declared[name]
        raw_expected = playwright.get(name)
        require(isinstance(raw_expected, dict), f"browser_lock_missing:{name}")
        expected = cast(dict[str, Any], raw_expected)
        require(
            browser["expectedVersion"] == expected.get("version")
            and browser["actualVersion"] == expected.get("version")
            and browser["expectedRevision"] == expected.get("revision"),
            f"browser_identity_mismatch:{name}",
        )
        require(
            not any(item.startswith("pageerror:") for item in browser["diagnostics"]),
            f"browser_page_error_observed:{name}",
        )
        compatibility = browser["compatibility"]
        require(
            compatibility["compatible"] is True
            and compatibility["reason"] == "sharing_profile_offer_compatible"
            and compatibility["presentation"] == "720p30"
            and compatibility["requiredLevelIdc"] == 31,
            f"browser_compatibility_invalid:{name}",
        )
        compatible_formats = []
        for format_ in compatibility["formats"]:
            profile = format_["profileLevelId"]
            require(
                int(profile[0:2], 16) == format_["profileIdc"]
                and int(profile[2:4], 16) == format_["profileIop"]
                and int(profile[4:6], 16) == format_["levelIdc"],
                f"browser_profile_identity_mismatch:{name}",
            )
            require(
                format_["payloadType"] in compatibility["videoPayloadTypes"],
                f"browser_payload_identity_mismatch:{name}",
            )
            if (
                constrained_baseline(format_["profileIdc"], format_["profileIop"])
                and format_["profileFamily"] == "constrained_baseline"
                and format_["levelIdc"] >= 31
                and format_["packetizationMode"] == 1
                and format_["levelAsymmetryAllowed"] is True
            ):
                compatible_formats.append(format_)
        require(bool(compatible_formats), f"browser_compatible_format_missing:{name}")
        require(
            any(
                "nack" in format_["feedback"] and "nack pli" in format_["feedback"]
                for format_ in compatible_formats
            ),
            f"browser_nack_pli_feedback_missing:{name}",
        )
        require(
            any(format_["profileLevelId"] in browser["offerSdp"] for format_ in compatible_formats),
            f"browser_profile_not_present_in_sdp:{name}",
        )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument(
        "--schema", type=Path, default=Path("schemas/m0-browser-offers-v1.schema.json")
    )
    parser.add_argument("--dependency-lock", type=Path, default=Path("dependencies.lock.json"))
    arguments = parser.parse_args()
    try:
        report = validate_m0_browser_offers(
            arguments.report, arguments.schema, arguments.dependency_lock
        )
    except BrowserOfferValidationError as error:
        print(json.dumps({"reason": str(error), "status": "FAILED"}, sort_keys=True))
        return 1
    print(json.dumps({"browsers": len(report["browsers"]), "status": "PASSED"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
