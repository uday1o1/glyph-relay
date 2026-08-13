import json
import shutil
from pathlib import Path

from tools.check_m0_protocol import PROTOCOL, ROOT, validate_protocol


def copy_protocol_repository(destination: Path) -> Path:
    root = destination / "repository"
    manifest = ROOT / PROTOCOL / "manifest.lock"
    for line in manifest.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 3 or fields[0] != "component":
            continue
        source = ROOT / fields[1]
        target = root / fields[1]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    copied_manifest = root / PROTOCOL / "manifest.lock"
    shutil.copyfile(manifest, copied_manifest)
    return root


def test_committed_m0_protocol_is_semantically_consistent() -> None:
    assert validate_protocol() == []


def test_m0_protocol_rejects_source_drift(tmp_path: Path) -> None:
    root = copy_protocol_repository(tmp_path)
    source_path = root / PROTOCOL / "source.json"
    source = json.loads(source_path.read_text(encoding="utf-8"))
    source["frame_count"] = 2099
    source_path.write_text(json.dumps(source), encoding="utf-8")
    errors = validate_protocol(root)
    assert "component hash mismatch: protocols/m0_fixed_map_v1/source.json" in errors
    assert "frame count mismatch" in errors


def test_m0_protocol_rejects_aq_drift(tmp_path: Path) -> None:
    root = copy_protocol_repository(tmp_path)
    run_path = root / PROTOCOL / "run-config.json"
    run = json.loads(run_path.read_text(encoding="utf-8"))
    run["spatial_aq"] = True
    run_path.write_text(json.dumps(run), encoding="utf-8")
    errors = validate_protocol(root)
    assert "component hash mismatch: protocols/m0_fixed_map_v1/run-config.json" in errors
    assert "spatial AQ must be disabled" in errors
