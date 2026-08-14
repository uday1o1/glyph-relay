from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import pytest

from tools.validate_m0_transport_capture import (
    CapturedPacket,
    TransportValidationError,
    parse_tshark_output,
    validate_m0_transport_capture,
)

SCHEMAS = Path("schemas")


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def event(
    agent: str,
    source_port: int,
    egress_class: str,
    path: str,
    protocol: str,
    family: str,
    payload: bytes,
) -> dict[str, Any]:
    ip_total = len(payload) + 8 + (40 if family == "IPV6" else 20)
    return {
        "agent": agent,
        "sourcePort": source_port,
        "class": egress_class,
        "path": path,
        "protocol": protocol,
        "family": family,
        "payloadBytes": len(payload),
        "ipTotalBytes": ip_total,
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "nativeResult": len(payload),
    }


def fixture(scenario: str) -> dict[str, Any]:
    relay = scenario == "turn-udp"
    path = "TURN_UDP" if relay else "DIRECT_UDP"
    family = "IPV6" if scenario == "direct-ipv6" else "IPV4"
    events = [
        event(
            "first",
            41_000,
            "CONTROL",
            "DIRECT_UDP",
            "UNKNOWN_CONTROL",
            family,
            b"",
        ),
        event(
            "first",
            41_000,
            "CONTROL",
            path,
            "TURN_CONTROL" if relay else "STUN",
            family,
            f"{scenario}-first-control".encode(),
        ),
        event(
            "second",
            41_001,
            "CONTROL",
            "DIRECT_UDP" if relay else path,
            "DTLS",
            family,
            f"{scenario}-second-control".encode(),
        ),
        event(
            "first",
            41_000,
            "MEDIA",
            path,
            "TURN_CHANNEL_DATA" if relay else "SRTP",
            family,
            f"{scenario}-protected-media".encode(),
        ),
    ]
    if relay:
        events.append(
            event(
                "first",
                41_000,
                "CONTROL",
                "DIRECT_UDP",
                "STUN",
                family,
                b"turn-gathering-direct-stun",
            )
        )
    return {
        "schemaVersion": 1,
        "protocol": "glyphrelay-m0-transport-fixture-v1",
        "status": "PASSED",
        "scenario": scenario,
        "firstSourcePort": 41_000,
        "secondSourcePort": 41_001,
        "firstSelectedCandidateType": "RELAYED" if relay else "HOST",
        "secondSelectedCandidateType": "HOST",
        "events": events,
    }


def packets(value: dict[str, Any]) -> list[CapturedPacket]:
    return [
        CapturedPacket(
            source_port=item["sourcePort"],
            family=item["family"],
            payload_bytes=item["payloadBytes"],
            ip_total_bytes=item["ipTotalBytes"],
            payload_sha256=item["payloadSha256"],
        )
        for item in value["events"]
    ]


def write_capture(root: Path, *, ipv6_supported: bool = True) -> dict[str, list[CapturedPacket]]:
    root.mkdir()
    packet_sets: dict[str, list[CapturedPacket]] = {}
    declarations = []
    for scenario in ("direct-ipv4", "direct-ipv6", "turn-udp"):
        if scenario == "direct-ipv6" and not ipv6_supported:
            declarations.append(
                {
                    "scenario": scenario,
                    "support": "UNSUPPORTED",
                    "supportReason": "loopback_bind_unsupported_errno_99",
                    "fixtureFile": None,
                    "fixtureSha256": None,
                    "packetCaptureFile": None,
                    "packetCaptureSha256": None,
                }
            )
            continue
        value = fixture(scenario)
        fixture_path = root / f"{scenario}-fixture.json"
        pcap_path = root / f"{scenario}.pcapng"
        write_json(fixture_path, value)
        pcap_path.write_bytes(f"pcap:{scenario}".encode())
        packet_sets[pcap_path.name] = packets(value)
        declarations.append(
            {
                "scenario": scenario,
                "support": "SUPPORTED",
                "supportReason": "fixture_and_packet_capture_completed",
                "fixtureFile": fixture_path.name,
                "fixtureSha256": sha256_file(fixture_path),
                "packetCaptureFile": pcap_path.name,
                "packetCaptureSha256": sha256_file(pcap_path),
            }
        )
    lock = json.loads(Path("dependencies.lock.json").read_text(encoding="utf-8"))
    manifest = {
        "schemaVersion": 1,
        "protocol": "glyphrelay-m0-transport-capture-v1",
        "status": "CAPTURED",
        "coturnImage": (f"{lock['coturn']['image']}@{lock['coturn']['linux_amd64_digest']}"),
        "scenarios": declarations,
    }
    write_json(root / "capture-manifest.json", manifest)
    return packet_sets


def validate(root: Path, packet_sets: dict[str, list[CapturedPacket]]) -> dict[str, Any]:
    return validate_m0_transport_capture(
        root,
        SCHEMAS,
        packet_reader=lambda path: packet_sets[path.name],
    )


def test_complete_transport_capture_matches_every_hook_datagram(tmp_path: Path) -> None:
    root = tmp_path / "capture"
    packet_sets = write_capture(root)
    result = validate(root, packet_sets)
    assert result["status"] == "PASSED"
    assert result["supportedIpv6"] is True
    assert all(
        item["hookDatagrams"] == item["packetCaptureDatagrams"] for item in result["scenarios"]
    )
    assert all(
        item["hookIpTotalBytes"] == item["packetCaptureIpTotalBytes"]
        for item in result["scenarios"]
    )


def test_seeded_packet_payload_mismatch_fails_for_its_reason(tmp_path: Path) -> None:
    root = tmp_path / "capture"
    packet_sets = write_capture(root)
    original = packet_sets["turn-udp.pcapng"][0]
    packet_sets["turn-udp.pcapng"][0] = CapturedPacket(
        source_port=original.source_port,
        family=original.family,
        payload_bytes=original.payload_bytes,
        ip_total_bytes=original.ip_total_bytes,
        payload_sha256="f" * 64,
    )
    with pytest.raises(TransportValidationError, match="transport_hook_packet_capture_mismatch"):
        validate(root, packet_sets)


def test_nearby_packet_capture_control_passes(tmp_path: Path) -> None:
    root = tmp_path / "capture"
    packet_sets = write_capture(root)
    validate(root, packet_sets)


def test_proven_unsupported_ipv6_does_not_weaken_required_paths(tmp_path: Path) -> None:
    root = tmp_path / "capture"
    packet_sets = write_capture(root, ipv6_supported=False)
    result = validate(root, packet_sets)
    assert result["supportedIpv6"] is False
    ipv6 = next(item for item in result["scenarios"] if item["scenario"] == "direct-ipv6")
    assert ipv6["support"] == "UNSUPPORTED"
    assert ipv6["hookDatagrams"] == 0


def test_seeded_turn_classification_regression_fails(tmp_path: Path) -> None:
    root = tmp_path / "capture"
    packet_sets = write_capture(root)
    fixture_path = root / "turn-udp-fixture.json"
    value = json.loads(fixture_path.read_text(encoding="utf-8"))
    value["events"][-1]["protocol"] = "SRTP"
    write_json(fixture_path, value)
    manifest_path = root / "capture-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    declaration = next(item for item in manifest["scenarios"] if item["scenario"] == "turn-udp")
    declaration["fixtureSha256"] = sha256_file(fixture_path)
    write_json(manifest_path, manifest)
    with pytest.raises(TransportValidationError, match="transport_turn_protocol_invalid"):
        validate(root, packet_sets)


def test_tshark_parser_preserves_ipv4_and_ipv6_ip_lengths() -> None:
    first = b"glyphrelay-v4"
    second = b"glyphrelay-v6"
    output = (
        f"41000\t{len(first) + 8}\t4\t{len(first) + 28}\t\t\t{first.hex()}\n"
        f"41001\t{len(second) + 8}\t6\t\t6\t{len(second) + 8}\t{second.hex()}\n"
    ).encode()
    parsed = parse_tshark_output(output)
    assert parsed[0].ip_total_bytes == len(first) + 28
    assert parsed[1].ip_total_bytes == len(second) + 48
