from __future__ import annotations

import json
from pathlib import Path

from tools.check_controller_protocol import ROOT, load_object, validate_lock, validate_manifest


def copy_protocol(tmp_path: Path) -> Path:
    for relative in (
        "protocols/controller_v1/manifest.json",
        "protocols/controller_v1/manifest.lock",
        "schemas/controller-v1.schema.json",
        "tools/check_controller_protocol.py",
    ):
        source = ROOT / relative
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
    return tmp_path


def write_manifest(root: Path, manifest: dict[str, object]) -> None:
    path = root / "protocols/controller_v1/manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def test_frozen_controller_protocol_is_valid() -> None:
    lock_errors, protocol_sha256 = validate_lock()
    assert lock_errors == []
    assert protocol_sha256 is not None
    assert len(protocol_sha256) == 64
    assert validate_manifest() == []


def test_protocol_lock_rejects_manifest_drift(tmp_path: Path) -> None:
    root = copy_protocol(tmp_path)
    path = root / "protocols/controller_v1/manifest.json"
    path.write_bytes(path.read_bytes() + b"\n")
    errors, _ = validate_lock(root)
    assert "controller protocol hash changed: protocols/controller_v1/manifest.json" in errors


def test_semantics_reject_weakened_pacer_bound(tmp_path: Path) -> None:
    root = copy_protocol(tmp_path)
    manifest = load_object(root / "protocols/controller_v1/manifest.json")
    manifest["pacer"]["hardByteLimit"] = 8 * 1024 * 1024
    write_manifest(root, manifest)
    assert "controller pacer byte bound changed" in validate_manifest(root)


def test_semantics_reject_reordered_degradation_stack(tmp_path: Path) -> None:
    root = copy_protocol(tmp_path)
    manifest = load_object(root / "protocols/controller_v1/manifest.json")
    manifest["knobStack"]["degradationOrder"] = [
        "automatic_emphasis_cap",
        "payload_and_vbv_step",
        "protected_threshold_delta",
        "presentation_profile",
    ]
    write_manifest(root, manifest)
    assert "controller degradation order changed" in validate_manifest(root)


def test_semantics_reject_unverified_turn_transport(tmp_path: Path) -> None:
    root = copy_protocol(tmp_path)
    manifest = load_object(root / "protocols/controller_v1/manifest.json")
    manifest["bandwidth"]["verifiedTransports"].append("turn_tcp")
    write_manifest(root, manifest)
    assert "controller verified transport set changed" in validate_manifest(root)


def test_semantics_reject_shortened_measurement(tmp_path: Path) -> None:
    root = copy_protocol(tmp_path)
    manifest = load_object(root / "protocols/controller_v1/manifest.json")
    manifest["qualification"]["steady"]["measurementSeconds"] = 12
    write_manifest(root, manifest)
    assert "controller steady measurement window changed" in validate_manifest(root)


def test_semantics_reject_pre_pivot_live_profile(tmp_path: Path) -> None:
    root = copy_protocol(tmp_path)
    manifest = load_object(root / "protocols/controller_v1/manifest.json")
    manifest["qualification"]["steady"]["presentationProfile"] = "1080p30"
    write_manifest(root, manifest)
    assert "controller live steady profile changed" in validate_manifest(root)
