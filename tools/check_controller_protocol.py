from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
from typing import Any

import jsonschema

ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "protocols" / "controller_v1" / "manifest.lock"
MANIFEST_PATH = ROOT / "protocols" / "controller_v1" / "manifest.json"
SCHEMA_PATH = ROOT / "schemas" / "controller-v1.schema.json"
EXPECTED_FILES = {
    "protocols/controller_v1/manifest.json",
    "schemas/controller-v1.schema.json",
    "tools/check_controller_protocol.py",
}


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object: {path}")
    return value


def validate_lock(root: Path = ROOT) -> tuple[list[str], str | None]:
    errors: list[str] = []
    lock_path = root / "protocols" / "controller_v1" / "manifest.lock"
    if not lock_path.is_file():
        return ["controller protocol manifest lock is missing"], None
    lock = load_object(lock_path)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "controller_v1":
        errors.append("controller protocol lock identity changed")
    raw_files = lock.get("files")
    if not isinstance(raw_files, list):
        return errors + ["controller protocol file list is invalid"], None
    paths: list[str] = []
    material = bytearray()
    for index, entry in enumerate(raw_files):
        if not isinstance(entry, dict):
            errors.append(f"controller protocol entry {index} is invalid")
            continue
        path = entry.get("path")
        expected = entry.get("sha256")
        if not isinstance(path, str) or not isinstance(expected, str):
            errors.append(f"controller protocol entry {index} is invalid")
            continue
        parsed = PurePosixPath(path)
        if parsed.is_absolute() or ".." in parsed.parts:
            errors.append(f"controller protocol path is unsafe: {path}")
            continue
        paths.append(path)
        source = root / path
        if not source.is_file():
            errors.append(f"controller protocol file is missing: {path}")
        elif sha256_bytes(source.read_bytes()) != expected:
            errors.append(f"controller protocol hash changed: {path}")
        material.extend(f"{path}\0{expected}\n".encode())
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        errors.append("controller protocol paths must be unique and sorted")
    if set(paths) != EXPECTED_FILES:
        errors.append("controller protocol file set changed")
    protocol_sha256 = sha256_bytes(bytes(material))
    if lock.get("protocol_sha256") != protocol_sha256:
        errors.append("controller protocol aggregate hash changed")
    return errors, protocol_sha256


def validate_manifest(root: Path = ROOT) -> list[str]:
    manifest = load_object(root / "protocols" / "controller_v1" / "manifest.json")
    schema = load_object(root / "schemas" / "controller-v1.schema.json")
    schema_errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(manifest), key=str)
    if schema_errors:
        return [f"controller manifest schema failure: {schema_errors[0].message}"]

    errors: list[str] = []
    exact_sections: dict[str, Any] = {
        "arithmetic": {
            "numberFormat": "ieee754_binary64",
            "roundingMode": "round_to_nearest_ties_to_even",
            "rateUnit": "bits_per_second",
            "durationUnit": "milliseconds",
            "byteUnit": "octets",
        },
        "timing": {
            "tickMilliseconds": 100,
            "feedbackMaximumAgeMilliseconds": 2000,
            "knobStepMinimumIntervalMilliseconds": 500,
            "reversalObservationMilliseconds": 2000,
            "recoveryEntryMilliseconds": 1000,
            "recoveryStepIntervalMilliseconds": 2000,
            "stableReturnMilliseconds": 5000,
            "unusableViolationMilliseconds": 3000,
        },
    }
    for name, expected in exact_sections.items():
        if manifest.get(name) != expected:
            errors.append(f"controller {name} contract changed")

    estimators = manifest.get("estimators", {})
    if estimators.get("windowsMilliseconds") != [1000, 5000]:
        errors.append("controller estimator windows changed")
    if estimators.get("update") != (
        "alpha=1-exp(-dt/tau);estimate=alpha*sample+(1-alpha)*previous"
    ):
        errors.append("controller estimator arithmetic changed")

    bandwidth = manifest.get("bandwidth", {})
    if (
        bandwidth.get("effectiveWireCap")
        != ("min(user_cap_bps,0.90*fresh_remb_bps)_else_user_cap_bps")
        or bandwidth.get("controlReserve") != "max(0.10*effective_wire_cap_bps,64000)"
    ):
        errors.append("controller bandwidth allocation changed")
    if bandwidth.get("verifiedTransports") != ["direct_udp_ice", "turn_udp"]:
        errors.append("controller verified transport set changed")
    if bandwidth.get("unverifiedTransports") != ["turn_tcp", "turn_tls"]:
        errors.append("controller unverified transport set changed")
    if bandwidth.get("rejectedIpFeatures") != [
        "ipv4_options",
        "ipv6_extension_headers",
        "ip_fragmentation",
        "bypass_socket",
    ]:
        errors.append("controller rejected IP feature set changed")

    pacer = manifest.get("pacer", {})
    if pacer.get("burstDurationMilliseconds") != 100:
        errors.append("controller pacer burst changed")
    if pacer.get("maximumPacketAgeMilliseconds") != 100:
        errors.append("controller pacer age bound changed")
    if pacer.get("hardByteLimit") != 4 * 1024 * 1024:
        errors.append("controller pacer byte bound changed")
    if pacer.get("accessUnitAdmission") != "atomic":
        errors.append("controller pacer admission changed")

    knobs = manifest.get("knobStack", {})
    if knobs.get("degradationOrder") != [
        "automatic_emphasis_cap",
        "protected_threshold_delta",
        "payload_and_vbv_step",
        "presentation_profile",
    ]:
        errors.append("controller degradation order changed")
    if knobs.get("restorationOrder") != list(reversed(knobs.get("degradationOrder", []))):
        errors.append("controller restoration order is not the reverse degradation order")
    if knobs.get("initial") != {
        "automaticEmphasisCap": 4,
        "protectedThresholdDelta": 0.0,
        "payloadAndVbvStep": 0,
        "presentationProfile": "1080p30",
    }:
        errors.append("controller initial level stack changed")
    if knobs.get("minimum") != {
        "automaticEmphasisCap": 0,
        "protectedThresholdDelta": 0.15,
        "payloadTarget": "frozen_profile_minimum",
        "presentationProfile": "720p15",
    }:
        errors.append("controller minimum level stack changed")
    if knobs.get("automaticEmphasisCaps") != [4, 3, 2, 1, 0]:
        errors.append("controller automatic emphasis stack changed")
    if knobs.get("protectedThresholdDeltas") != [0.0, 0.05, 0.1, 0.15]:
        errors.append("controller threshold stack changed")
    if knobs.get("minimumPayloadBpsByProfile") != {
        "1080p30": 100000,
        "1080p24": 100000,
        "720p24": 100000,
        "720p15": 100000,
    }:
        errors.append("controller profile minimum payloads changed")

    expected_profiles = [
        {"name": "1080p30", "width": 1920, "height": 1080, "framesPerSecond": 30},
        {"name": "1080p24", "width": 1920, "height": 1080, "framesPerSecond": 24},
        {"name": "720p24", "width": 1280, "height": 720, "framesPerSecond": 24},
        {"name": "720p15", "width": 1280, "height": 720, "framesPerSecond": 15},
    ]
    if manifest.get("presentationProfiles") != expected_profiles:
        errors.append("controller presentation profiles changed")

    states = manifest.get("stateMachine", {})
    if states.get("states") != [
        "STABLE",
        "RATE_PRESSURE",
        "CONGESTED",
        "RECOVERY",
        "UNUSABLE",
    ]:
        errors.append("controller states changed")
    if states.get("ratePressure", {}).get("requiredConsecutiveTicks") != 3:
        errors.append("controller rate-pressure dwell changed")
    if states.get("congested", {}).get("requiredConsecutiveTicks") != 1:
        errors.append("controller congestion entry changed")

    trace = manifest.get("trace", {})
    if trace.get("fixtures") != [
        "stable_link",
        "emphasis_overshoot",
        "stale_remb",
        "missing_remb",
        "sudden_collapse",
        "high_rtt_without_loss",
        "recovery",
        "unusable",
    ]:
        errors.append("controller trace fixture set changed")
    if len(trace.get("requiredFields", [])) != 10:
        errors.append("controller trace field set changed")

    qualification = manifest.get("qualification", {})
    steady = qualification.get("steady", {})
    if steady.get("capsBps") != [500000, 1000000, 2000000, 4000000]:
        errors.append("controller steady cap matrix changed")
    if steady.get("warmupSeconds") != 10 or steady.get("measurementSeconds") != 120:
        errors.append("controller steady measurement window changed")
    network = qualification.get("networkFixture", {})
    if network.get("commandTemplate") != (
        "tc qdisc replace dev {device} root netem limit 1000 delay 25ms rate {rate_kbit}kbit"
    ):
        errors.append("controller network fixture command changed")
    return errors


def main() -> int:
    lock_errors, protocol_sha256 = validate_lock()
    errors = lock_errors + validate_manifest()
    if errors:
        for error in errors:
            print(f"controller protocol check failed: {error}")
        return 1
    print(
        json.dumps(
            {"protocolSha256": protocol_sha256, "protocol": "controller_v1", "status": "VALID"},
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
