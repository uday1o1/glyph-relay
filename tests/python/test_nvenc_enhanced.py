from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

import pytest

from tools.corpus.saliency_selector import canonical_json
from tools.gpu.remote_qualification import load_phases
from tools.run_nvenc_enhanced import EnhancedNvencRunError, native_command
from tools.validate_nvenc_enhanced import (
    EXPECTED_MODES,
    EnhancedNvencValidationError,
    validate,
)


def write_evidence(tmp_path: Path) -> Path:
    configuration = {
        "contrastWeight": 0.25,
        "dilationRadiusTiles": 1,
        "edgePairWeight": 0.25,
        "entryThreshold": 0.55,
        "exitThreshold": 0.4,
        "gradientWeight": 0.35,
        "previousScoreCoefficient": 0.6,
        "smallStructureWeight": 0.15,
    }
    configuration_sha256 = hashlib.sha256(canonical_json(configuration)).hexdigest()
    selection = tmp_path / "selected-configuration.json"
    selection.write_text(
        json.dumps(
            {
                "configuration": configuration,
                "configurationSha256": configuration_sha256,
            },
            sort_keys=True,
            separators=(",", ":"),
        ),
        encoding="utf-8",
    )
    sessions: list[dict[str, object]] = []
    for mode in EXPECTED_MODES:
        stream = tmp_path / f"{mode}.h264"
        stream.write_bytes(f"annex-b-{mode}".encode())
        sessions.append(
            {
                "mode": mode,
                "frames": 300,
                "peakInFlight": 3,
                "outputThreadDedicated": True,
                "firstAccessUnitKeyframe": True,
                "firstAccessUnitParameterSets": True,
                "resourcesReleased": True,
                "streamBytes": stream.stat().st_size,
                "streamSha256": hashlib.sha256(stream.read_bytes()).hexdigest(),
            }
        )
    evidence = tmp_path / "evidence.json"
    evidence.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "status": "PASSED",
                "width": 1920,
                "height": 1080,
                "teardownCycles": 10,
                "saliencyConfiguration": configuration,
                "saliencyConfigurationSha256": configuration_sha256,
                "saliencySelectionSha256": hashlib.sha256(selection.read_bytes()).hexdigest(),
                "sessions": sessions,
            }
        ),
        encoding="utf-8",
    )
    return evidence


def successful_tools(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
    if command[0] == "ffprobe":
        payload = {
            "streams": [
                {
                    "codec_name": "h264",
                    "width": 1920,
                    "height": 1080,
                    "level": 40,
                    "pix_fmt": "yuv420p",
                    "color_range": "tv",
                    "color_space": "bt709",
                    "color_transfer": "bt709",
                    "color_primaries": "bt709",
                    "nb_read_frames": "300",
                }
            ]
        }
        return subprocess.CompletedProcess(command, 0, json.dumps(payload), "")
    return subprocess.CompletedProcess(command, 0, "", "")


def test_validator_independently_decodes_all_exact_modes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    evidence = write_evidence(tmp_path)
    monkeypatch.setattr(subprocess, "run", successful_tools)
    result = validate(
        evidence,
        Path("schemas/nvenc-enhanced-qualification-v1.schema.json"),
        tmp_path / "selected-configuration.json",
    )
    assert result["status"] == "PASSED"
    assert [stream["mode"] for stream in result["streams"]] == list(EXPECTED_MODES)
    assert (
        result["saliencyConfigurationSha256"]
        == json.loads((tmp_path / "selected-configuration.json").read_text(encoding="utf-8"))[
            "configurationSha256"
        ]
    )


def test_validator_rejects_stream_tamper_before_decoder(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    evidence = write_evidence(tmp_path)
    (tmp_path / "fixed_emphasis.h264").write_bytes(b"tampered")
    monkeypatch.setattr(subprocess, "run", successful_tools)
    with pytest.raises(EnhancedNvencValidationError, match="stream_identity_invalid"):
        validate(
            evidence,
            Path("schemas/nvenc-enhanced-qualification-v1.schema.json"),
            tmp_path / "selected-configuration.json",
        )


def test_validator_rejects_seeded_decode_count_defect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    evidence = write_evidence(tmp_path)

    def wrong_count(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        result = successful_tools(command, **kwargs)
        if command[0] == "ffprobe":
            payload = json.loads(result.stdout)
            payload["streams"][0]["nb_read_frames"] = "299"
            result.stdout = json.dumps(payload)
        return result

    monkeypatch.setattr(subprocess, "run", wrong_count)
    with pytest.raises(EnhancedNvencValidationError, match="stream_contract_mismatch"):
        validate(
            evidence,
            Path("schemas/nvenc-enhanced-qualification-v1.schema.json"),
            tmp_path / "selected-configuration.json",
        )


def test_validator_rejects_missing_mode_even_with_three_sessions(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    evidence = write_evidence(tmp_path)
    payload = json.loads(evidence.read_text(encoding="utf-8"))
    payload["sessions"][1]["mode"] = "uniform"
    evidence.write_text(json.dumps(payload), encoding="utf-8")
    monkeypatch.setattr(subprocess, "run", successful_tools)
    with pytest.raises(EnhancedNvencValidationError, match="coverage_or_order_invalid"):
        validate(
            evidence,
            Path("schemas/nvenc-enhanced-qualification-v1.schema.json"),
            tmp_path / "selected-configuration.json",
        )


def test_runner_passes_exact_frozen_configuration_and_hashes(tmp_path: Path) -> None:
    write_evidence(tmp_path)
    selection = tmp_path / "selected-configuration.json"
    command, payload = native_command(Path("/native"), selection, tmp_path / "output")
    assert payload["configuration"]["gradientWeight"] == 0.35
    assert command[command.index("--gradient-weight") + 1] == "0.35"
    assert (
        command[command.index("--selection-sha256") + 1]
        == hashlib.sha256(selection.read_bytes()).hexdigest()
    )


def test_runner_rejects_seeded_configuration_hash_defect(tmp_path: Path) -> None:
    write_evidence(tmp_path)
    selection = tmp_path / "selected-configuration.json"
    payload = json.loads(selection.read_text(encoding="utf-8"))
    payload["configurationSha256"] = "0" * 64
    selection.write_text(json.dumps(payload), encoding="utf-8")
    with pytest.raises(EnhancedNvencRunError, match="selection_configuration_hash_invalid"):
        native_command(Path("/native"), selection, tmp_path / "output")


def test_validator_rejects_selection_identity_tamper(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    evidence = write_evidence(tmp_path)
    selection = tmp_path / "selected-configuration.json"
    selection.write_text(selection.read_text(encoding="utf-8") + "\n", encoding="utf-8")
    monkeypatch.setattr(subprocess, "run", successful_tools)
    with pytest.raises(EnhancedNvencValidationError, match="selection_identity_mismatch"):
        validate(
            evidence,
            Path("schemas/nvenc-enhanced-qualification-v1.schema.json"),
            selection,
        )


def test_target_phase_waits_for_prior_hardware_gates_and_is_resource_assessed() -> None:
    phases = load_phases(Path("qualification/m0-phases.json"))
    phase = next(item for item in phases if item.identifier == "nvenc-enhanced-runtime")
    assert phase.timeout_seconds == 1800
    assert phase.resource_policy == "nvenc-performance-v1"
    assert "saliency-development-selection" in phase.dependencies
    assert "m0-fixed-map-quality" in phase.dependencies
    assert "tools/run_nvenc_enhanced.py" in phase.commands[0]
