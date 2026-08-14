from __future__ import annotations

import copy
import json
import subprocess
from pathlib import Path

import pytest

from tools.check_saliency_validation_protocol import validate_protocol
from tools.corpus.evaluate_validation_ocr import evaluate_validation
from tools.corpus.prepare_saliency_validation import _resume
from tools.gpu.remote_qualification import load_phases
from tools.run_saliency_validation import (
    ValidationRunError,
    coverage,
    open_or_resume_ledger,
    validate_map_identity,
    validation_status,
)


def _manifest() -> dict[str, object]:
    value = json.loads(Path("corpus/manifests/validation.json").read_text(encoding="utf-8"))
    assert isinstance(value, dict)
    return value


def test_validation_ocr_reuses_frozen_metric_without_accepting_development_identity(
    tmp_path: Path,
) -> None:
    manifest = _manifest()
    sequences = manifest["sequences"]
    assert isinstance(sequences, list)
    for sequence in sequences:
        assert isinstance(sequence, dict)
        sequence_id = sequence["sequenceId"]
        for frame in sequence["sampleFrames"]:
            for region in frame["textRegions"]:
                path = tmp_path / str(sequence_id) / f"{frame['frameId']:03d}-{region['id']}.txt"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(region["truth"], encoding="utf-8")
    report = evaluate_validation(manifest, tmp_path)
    assert report["split"] == "validation"
    assert report["status"] == "PASSED"
    assert report["overallBoundedCer"] == 0
    assert report["smallGlyphBoundedCer"] == 0


def test_access_ledger_is_created_before_render_and_only_resumes_exact_identity(
    tmp_path: Path,
) -> None:
    identity = {
        "schemaVersion": 1,
        "protocol": "saliency_validation_v1",
        "accessOrdinal": 1,
        "repositoryCommit": "a" * 64,
    }
    ledger = open_or_resume_ledger(tmp_path, identity)
    assert ledger["accessOrdinal"] == 1
    assert (tmp_path / "access-ledger.json").is_file()
    assert open_or_resume_ledger(tmp_path, identity) == ledger
    with pytest.raises(ValidationRunError, match="ledger_identity_mismatch"):
        open_or_resume_ledger(tmp_path, {**identity, "repositoryCommit": "b" * 64})


def test_access_ledger_rejects_preopened_renderer(tmp_path: Path) -> None:
    (tmp_path / "render").mkdir()
    with pytest.raises(ValidationRunError, match="render_exists_without_access_ledger"):
        open_or_resume_ledger(tmp_path, {"schemaVersion": 1})


def test_validation_renderer_refuses_direct_access_without_ledger(tmp_path: Path) -> None:
    completed = subprocess.run(
        [
            "node",
            "tooling/corpus/render-validation.ts",
            "--manifest",
            "corpus/manifests/validation.json",
            "--output",
            str(tmp_path / "render"),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert completed.returncode != 0
    assert "--access-ledger-sha256" in completed.stderr
    assert not (tmp_path / "render").exists()


def test_validation_bundle_resume_recovers_metadata_after_publish_crash(
    tmp_path: Path,
) -> None:
    bundle = tmp_path / "validation.bundle"
    bundle.write_bytes(b"durable validation bundle")
    identity = {
        "schemaVersion": 1,
        "protocol": "saliency_validation_v1",
        "configurationSha256": "a" * 64,
    }
    resumed = _resume(tmp_path, identity)
    assert resumed is not None and resumed.resumed
    metadata = json.loads((tmp_path / "validation.bundle.json").read_text(encoding="utf-8"))
    assert metadata["bundleBytes"] == bundle.stat().st_size
    assert metadata["bundleSha256"] == resumed.sha256


def _passing_gate_inputs() -> tuple[dict[str, object], dict[str, object]]:
    return (
        {
            "status": "PASSED",
            "overallBoundedCer": 0.02,
            "smallGlyphBoundedCer": 0.05,
        },
        {
            "status": "PASSED",
            "metrics": {
                "overallGlyphRecall": 0.90,
                "smallGlyphRecall": 0.80,
                "protectedFraction": 0.35,
                "falseProtectedFraction": 0.15,
                "staticMapChangeFraction": 0.02,
            },
        },
    )


@pytest.mark.parametrize(
    ("section", "metric", "seeded_value"),
    [
        ("ocr", "overallBoundedCer", 0.0200001),
        ("ocr", "smallGlyphBoundedCer", 0.0500001),
        ("map", "overallGlyphRecall", 0.8999999),
        ("map", "smallGlyphRecall", 0.7999999),
        ("map", "protectedFraction", 0.3500001),
        ("map", "falseProtectedFraction", 0.1500001),
        ("map", "staticMapChangeFraction", 0.0200001),
    ],
)
def test_seeded_validation_gate_defects_cannot_retain_passed_status(
    section: str, metric: str, seeded_value: float
) -> None:
    ocr, map_evidence = _passing_gate_inputs()
    ocr = copy.deepcopy(ocr)
    map_evidence = copy.deepcopy(map_evidence)
    if section == "ocr":
        ocr[metric] = seeded_value
    else:
        metrics = map_evidence["metrics"]
        assert isinstance(metrics, dict)
        metrics[metric] = seeded_value
    with pytest.raises(ValidationRunError, match="status_does_not_match_measurements"):
        validation_status(ocr, map_evidence)


def test_validation_gate_boundary_controls_pass() -> None:
    ocr, map_evidence = _passing_gate_inputs()
    assert validation_status(ocr, map_evidence) == "PASSED"


def test_resumed_map_evidence_cannot_cross_validation_identity() -> None:
    identity = {
        "sourceBundleId": "a" * 64,
        "automaticMapImplementationSha256": "b" * 64,
        "processingPlatformSha256": "c" * 64,
        "corpusProtocolSha256": "d" * 64,
        "validationManifestSha256": "e" * 64,
        "saliencyConfigurationSha256": "f" * 64,
    }
    configuration = {"gradientWeight": 0.25}
    evidence = {
        **identity,
        "validationRenderIndexSha256": "1" * 64,
        "configurationSha256": identity["saliencyConfigurationSha256"],
        "configuration": configuration,
    }
    evidence.pop("saliencyConfigurationSha256")
    validate_map_identity(evidence, identity, "1" * 64, configuration)
    evidence["validationRenderIndexSha256"] = "2" * 64
    with pytest.raises(ValidationRunError, match="validationRenderIndexSha256"):
        validate_map_identity(evidence, identity, "1" * 64, configuration)


def test_validation_manifest_covers_required_scene_families() -> None:
    result = coverage(_manifest())
    assert result == {
        "themeIds": ["validation_dark_amber", "validation_light_violet"],
        "rapidScrollSequenceCount": 3,
        "cursorOrCaretSampleCount": 256,
        "embeddedVideoSequenceCount": 9,
        "smallGlyphCount": 5120,
    }


def test_validation_protocol_and_target_phase_are_frozen() -> None:
    errors, protocol_sha256 = validate_protocol()
    assert errors == []
    assert protocol_sha256 is not None
    phases = load_phases(Path("qualification/m0-phases.json"))
    phase = next(item for item in phases if item.identifier == "saliency-validation")
    flattened = [argument for command in phase.commands for argument in command]
    assert phase.dependencies == (
        "saliency-development-selection",
        "uniform-aq-development-selection",
    )
    assert phase.timeout_seconds == 7200
    assert "{phase_root}/validation-checkpoint" in flattened
