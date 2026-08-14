#!/usr/bin/env python3
"""Validate final-hook events against independent packet-capture IP lengths."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from jsonschema import Draft202012Validator


class TransportValidationError(RuntimeError):
    """Raised when transport evidence does not satisfy the Milestone 0 gate."""


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIRST_PORT = 41_000
SECOND_PORT = 41_001
MAXIMUM_TSHARK_OUTPUT_BYTES = 16 * 1024 * 1024


@dataclass(frozen=True)
class CapturedPacket:
    source_port: int
    family: str
    payload_bytes: int
    ip_total_bytes: int
    payload_sha256: str


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise TransportValidationError(reason)


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise TransportValidationError(f"json_read_failed:{path.name}") from error
    require(isinstance(value, dict), f"json_object_required:{path.name}")
    return cast(dict[str, Any], value)


def validate_schema(value: dict[str, Any], schema_path: Path) -> None:
    schema = load_object(schema_path)
    errors = sorted(
        Draft202012Validator(schema).iter_errors(value), key=lambda item: item.json_path
    )
    require(
        not errors,
        f"schema_validation_failed:{errors[0].json_path}:{errors[0].message}" if errors else "",
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_artifact(root: Path, raw_name: object, expected_sha256: object) -> Path:
    require(isinstance(raw_name, str), "transport_artifact_name_invalid")
    name = cast(str, raw_name)
    require(Path(name).name == name, "transport_artifact_name_invalid")
    artifact = (root / name).resolve()
    require(artifact.is_relative_to(root.resolve()), "transport_artifact_path_escape")
    require(
        artifact.is_file()
        and isinstance(expected_sha256, str)
        and sha256_file(artifact) == expected_sha256,
        f"transport_artifact_hash_invalid:{name}",
    )
    return artifact


def parse_tshark_output(output: bytes) -> list[CapturedPacket]:
    packets: list[CapturedPacket] = []
    for line_number, raw_line in enumerate(output.decode("ascii", "strict").splitlines(), start=1):
        fields = raw_line.split("\t")
        require(len(fields) == 7, f"tshark_field_count_invalid:{line_number}")
        (
            source_port_raw,
            udp_length_raw,
            ip_version,
            ip_length_raw,
            ipv6_version,
            ipv6_payload,
            raw,
        ) = fields
        try:
            source_port = int(source_port_raw)
            udp_length = int(udp_length_raw)
            payload = bytes.fromhex(raw.replace(":", ""))
            if ip_version == "4" and not ipv6_version:
                family = "IPV4"
                ip_total = int(ip_length_raw)
            elif ip_version in {"", "6"} and ipv6_version == "6":
                family = "IPV6"
                ip_total = 40 + int(ipv6_payload)
            else:
                raise ValueError("family")
        except (ValueError, OverflowError) as error:
            raise TransportValidationError(f"tshark_field_invalid:{line_number}") from error
        require(source_port in {FIRST_PORT, SECOND_PORT}, "tshark_source_port_invalid")
        require(udp_length >= 8 and len(payload) == udp_length - 8, "tshark_udp_length_invalid")
        require(
            ip_total == udp_length + (20 if family == "IPV4" else 40),
            "tshark_ip_length_invalid",
        )
        packets.append(
            CapturedPacket(
                source_port=source_port,
                family=family,
                payload_bytes=len(payload),
                ip_total_bytes=ip_total,
                payload_sha256=hashlib.sha256(payload).hexdigest(),
            )
        )
    require(bool(packets), "packet_capture_contains_no_eligible_datagrams")
    return packets


def tshark_packets(path: Path) -> list[CapturedPacket]:
    completed = subprocess.run(
        [
            "tshark",
            "-r",
            str(path),
            "-Y",
            f"udp.srcport == {FIRST_PORT} || udp.srcport == {SECOND_PORT}",
            "-T",
            "fields",
            "-E",
            "separator=/t",
            "-E",
            "occurrence=f",
            "-e",
            "udp.srcport",
            "-e",
            "udp.length",
            "-e",
            "ip.version",
            "-e",
            "ip.len",
            "-e",
            "ipv6.version",
            "-e",
            "ipv6.plen",
            "-e",
            "udp.payload",
        ],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
        env={"PATH": os.environ.get("PATH", "")},
        check=False,
    )
    require(completed.returncode == 0, f"tshark_read_failed:{completed.returncode}")
    require(len(completed.stdout) <= MAXIMUM_TSHARK_OUTPUT_BYTES, "tshark_output_limit_exceeded")
    return parse_tshark_output(completed.stdout)


def event_identity(event: dict[str, Any]) -> tuple[int, str, int, int, str]:
    return (
        event["sourcePort"],
        event["family"],
        event["payloadBytes"],
        event["ipTotalBytes"],
        event["payloadSha256"],
    )


def packet_identity(packet: CapturedPacket) -> tuple[int, str, int, int, str]:
    return (
        packet.source_port,
        packet.family,
        packet.payload_bytes,
        packet.ip_total_bytes,
        packet.payload_sha256,
    )


def validate_fixture(
    fixture: dict[str, Any], packets: list[CapturedPacket], scenario: str
) -> dict[str, Any]:
    require(fixture["scenario"] == scenario, f"transport_fixture_scenario_mismatch:{scenario}")
    expected_path = "TURN_UDP" if scenario == "turn-udp" else "DIRECT_UDP"
    expected_family = "IPV6" if scenario == "direct-ipv6" else "IPV4"
    relay = scenario == "turn-udp"
    first_relayed = fixture["firstSelectedCandidateType"] == "RELAYED"
    second_relayed = fixture["secondSelectedCandidateType"] == "RELAYED"
    require(
        (first_relayed and not second_relayed)
        if relay
        else (not first_relayed and not second_relayed),
        f"transport_selected_candidate_invalid:{scenario}",
    )
    events = fixture["events"]
    media = []
    control = []
    for event in events:
        require(
            event["sourcePort"] == (FIRST_PORT if event["agent"] == "first" else SECOND_PORT),
            f"transport_event_agent_port_mismatch:{scenario}",
        )
        require(
            (relay or event["path"] == expected_path)
            and event["family"] == expected_family
            and event["nativeResult"] == event["payloadBytes"],
            f"transport_event_metadata_invalid:{scenario}",
        )
        require(
            event["ipTotalBytes"]
            == event["payloadBytes"] + 8 + (40 if expected_family == "IPV6" else 20),
            f"transport_event_ip_length_invalid:{scenario}",
        )
        require(
            event["payloadBytes"] > 0
            or (
                event["class"] == "CONTROL"
                and event["path"] == "DIRECT_UDP"
                and event["protocol"] == "UNKNOWN_CONTROL"
            ),
            f"transport_zero_payload_classification_invalid:{scenario}",
        )
        if event["class"] == "MEDIA":
            media.append(event)
        else:
            control.append(event)
    require(
        len(media) == 1 and media[0]["agent"] == "first",
        f"transport_media_count_invalid:{scenario}",
    )
    require(
        {event["agent"] for event in control} == {"first", "second"},
        f"transport_control_coverage_invalid:{scenario}",
    )
    if relay:
        require(
            media[0]["path"] == "TURN_UDP"
            and media[0]["protocol"] in {"TURN_CHANNEL_DATA", "TURN_SEND_INDICATION"}
            and any(
                event["path"] == "TURN_UDP" and event["protocol"] == "TURN_CONTROL"
                for event in control
            )
            and all(
                event["path"] == "TURN_UDP"
                or event["protocol"] in {"STUN", "DTLS", "SRTCP", "UNKNOWN_CONTROL"}
                for event in control
            ),
            "transport_turn_protocol_invalid",
        )
    else:
        require(
            media[0]["protocol"] == "SRTP", f"transport_direct_media_protocol_invalid:{scenario}"
        )
        require(
            {event["protocol"] for event in control}.issubset(
                {"UNKNOWN_CONTROL", "SRTCP", "DTLS", "STUN"}
            ),
            f"transport_direct_control_protocol_invalid:{scenario}",
        )
    require(
        Counter(event_identity(event) for event in events)
        == Counter(packet_identity(packet) for packet in packets),
        f"transport_hook_packet_capture_mismatch:{scenario}",
    )
    hook_total = sum(event["ipTotalBytes"] for event in events)
    capture_total = sum(packet.ip_total_bytes for packet in packets)
    require(hook_total == capture_total, f"transport_ip_total_mismatch:{scenario}")
    return {
        "scenario": scenario,
        "support": "SUPPORTED",
        "hookDatagrams": len(events),
        "packetCaptureDatagrams": len(packets),
        "hookIpTotalBytes": hook_total,
        "packetCaptureIpTotalBytes": capture_total,
        "mediaDatagrams": len(media),
        "controlDatagrams": len(control),
        "protocols": sorted({event["protocol"] for event in events}),
    }


def validate_m0_transport_capture(
    capture_root: Path,
    schemas: Path,
    packet_reader: Callable[[Path], list[CapturedPacket]] = tshark_packets,
) -> dict[str, Any]:
    root = capture_root.resolve(strict=True)
    manifest = load_object(root / "capture-manifest.json")
    validate_schema(manifest, schemas / "m0-transport-capture-v1.schema.json")
    dependency_lock = load_object(REPOSITORY_ROOT / "dependencies.lock.json")
    expected_image = (
        f"{dependency_lock['coturn']['image']}@{dependency_lock['coturn']['linux_amd64_digest']}"
    )
    require(manifest["coturnImage"] == expected_image, "transport_coturn_image_mismatch")
    declarations = {item["scenario"]: item for item in manifest["scenarios"]}
    require(
        len(declarations) == 3 and set(declarations) == {"direct-ipv4", "direct-ipv6", "turn-udp"},
        "transport_scenario_set_invalid",
    )
    require(
        declarations["direct-ipv4"]["support"] == "SUPPORTED"
        and declarations["turn-udp"]["support"] == "SUPPORTED",
        "transport_required_scenario_unsupported",
    )
    results: list[dict[str, Any]] = []
    declared_files = {"capture-manifest.json"}
    for scenario in ("direct-ipv4", "direct-ipv6", "turn-udp"):
        declaration = declarations[scenario]
        if declaration["support"] == "UNSUPPORTED":
            require(scenario == "direct-ipv6", "transport_required_scenario_unsupported")
            require(
                all(
                    declaration[key] is None
                    for key in (
                        "fixtureFile",
                        "fixtureSha256",
                        "packetCaptureFile",
                        "packetCaptureSha256",
                    )
                )
                and declaration["supportReason"].startswith(
                    ("socket_module_reports_", "loopback_bind_unsupported_errno_")
                ),
                "transport_ipv6_unsupported_evidence_invalid",
            )
            results.append(
                {
                    "scenario": scenario,
                    "support": "UNSUPPORTED",
                    "hookDatagrams": 0,
                    "packetCaptureDatagrams": 0,
                    "hookIpTotalBytes": 0,
                    "packetCaptureIpTotalBytes": 0,
                    "mediaDatagrams": 0,
                    "controlDatagrams": 0,
                    "protocols": [],
                }
            )
            continue
        fixture_path = checked_artifact(
            root, declaration["fixtureFile"], declaration["fixtureSha256"]
        )
        capture_path = checked_artifact(
            root, declaration["packetCaptureFile"], declaration["packetCaptureSha256"]
        )
        declared_files.update({fixture_path.name, capture_path.name})
        fixture = load_object(fixture_path)
        validate_schema(fixture, schemas / "m0-transport-fixture-v1.schema.json")
        results.append(validate_fixture(fixture, packet_reader(capture_path), scenario))
    actual_files = {path.name for path in root.iterdir() if path.is_file()}
    require(actual_files == declared_files, "transport_unexpected_or_missing_artifact")
    validation = {
        "schemaVersion": 1,
        "protocol": "glyphrelay-m0-transport-validation-v1",
        "status": "PASSED",
        "coturnImage": expected_image,
        "supportedIpv6": declarations["direct-ipv6"]["support"] == "SUPPORTED",
        "scenarios": results,
    }
    validate_schema(validation, schemas / "m0-transport-validation-v1.schema.json")
    return validation


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--schemas", type=Path, default=Path("schemas"))
    arguments = parser.parse_args()
    try:
        validation = validate_m0_transport_capture(arguments.capture, arguments.schemas)
        descriptor = os.open(
            arguments.capture / "validation.json",
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        with os.fdopen(descriptor, "wb") as stream:
            stream.write((json.dumps(validation, indent=2, sort_keys=True) + "\n").encode())
            stream.flush()
            os.fsync(stream.fileno())
    except (OSError, TransportValidationError, subprocess.SubprocessError) as error:
        print(json.dumps({"reason": str(error), "status": "FAILED"}, sort_keys=True))
        return 1
    print(
        json.dumps({"scenarios": len(validation["scenarios"]), "status": "PASSED"}, sort_keys=True)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
