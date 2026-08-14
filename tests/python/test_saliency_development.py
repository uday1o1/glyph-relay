from __future__ import annotations

import base64
import copy
import hashlib
import json
from pathlib import Path

import pytest

from tools.corpus.prepare_saliency_development import (
    BUNDLE_MAGIC,
    MACROBLOCK_COUNT,
    DevelopmentPreparationError,
    metadata_identity,
    resume_bundle,
    truth_masks,
)
from tools.corpus.saliency_selector import GRID_PATH, load_object
from tools.gpu.remote_qualification import load_phases
from tools.run_saliency_development import (
    IMPLEMENTATION_PATHS,
    DevelopmentRunError,
    compare_committed_selection,
    exact_sha256,
    implementation_sha256,
)


def synthetic_manifest() -> dict[str, object]:
    alpha = b"\xff"
    frame = {
        "frameId": 0,
        "textRegions": [
            {
                "glyphs": [
                    {
                        "rasterId": "glyph",
                        "smallGlyphSubset": True,
                        "x": 17,
                        "y": 33,
                    }
                ]
            }
        ],
        "uiPrimitives": [{"boundingBox": [48, 64, 1, 1], "type": "caret"}],
    }
    return {
        "schemaVersion": 1,
        "protocol": "corpus_protocol_v1",
        "split": "development",
        "glyphRasterCatalog": [
            {
                "id": "glyph",
                "width": 1,
                "height": 1,
                "alphaBase64": base64.b64encode(alpha).decode(),
                "alphaSha256": hashlib.sha256(alpha).hexdigest(),
            }
        ],
        "sequences": [
            {
                "sequenceId": "development-code_editor-01",
                "sampleFrames": [
                    frame,
                    {**copy.deepcopy(frame), "frameId": 60},
                    {**copy.deepcopy(frame), "frameId": 120},
                    {**copy.deepcopy(frame), "frameId": 180},
                ],
            }
        ],
    }


def test_truth_rasterization_uses_nonzero_glyph_alpha_and_declared_ui() -> None:
    masks = truth_masks(synthetic_manifest())
    glyph, small, ui = masks[("development-code_editor-01", 0)]
    glyph_index = (33 // 16) * 120 + 17 // 16
    ui_index = (64 // 16) * 120 + 48 // 16
    assert len(glyph) == len(small) == len(ui) == MACROBLOCK_COUNT
    assert glyph[glyph_index] == small[glyph_index] == 1
    assert ui[ui_index] == 1
    assert sum(glyph) == sum(small) == sum(ui) == 1


def test_frozen_development_truth_produces_complete_macroblock_masks() -> None:
    manifest = load_object(Path("corpus/manifests/development.json"))
    masks = truth_masks(manifest)
    assert len(masks) == 256
    for glyph, small, ui in masks.values():
        assert len(glyph) == len(small) == len(ui) == MACROBLOCK_COUNT
        assert any(glyph) and any(small) and any(ui)
        assert all(not small[index] or glyph[index] for index in range(MACROBLOCK_COUNT))


def test_truth_rasterization_rejects_small_truth_outside_catalog_identity() -> None:
    manifest = synthetic_manifest()
    catalog = manifest["glyphRasterCatalog"]
    assert isinstance(catalog, list) and isinstance(catalog[0], dict)
    catalog[0]["alphaSha256"] = "0" * 64
    with pytest.raises(DevelopmentPreparationError, match="glyph_alpha_identity_invalid"):
        truth_masks(manifest)


def test_bundle_resume_recovers_durable_bundle_after_metadata_publish_crash(
    tmp_path: Path,
) -> None:
    grid = load_object(GRID_PATH)
    identity = metadata_identity(grid)
    bundle = tmp_path / "development.bundle"
    bundle.write_bytes(BUNDLE_MAGIC + b"durable")
    resumed = resume_bundle(tmp_path, identity)
    assert resumed is not None and resumed.resumed
    metadata = json.loads((tmp_path / "development.bundle.json").read_text(encoding="utf-8"))
    assert metadata["bundleBytes"] == bundle.stat().st_size
    assert metadata["bundleSha256"] == hashlib.sha256(bundle.read_bytes()).hexdigest()


def test_automatic_map_identity_covers_every_declared_source(tmp_path: Path) -> None:
    for relative in IMPLEMENTATION_PATHS:
        source = tmp_path / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(f"{relative}\n", encoding="utf-8")
    before = implementation_sha256(tmp_path)
    changed = tmp_path / IMPLEMENTATION_PATHS[-1]
    changed.write_text("seeded implementation defect\n", encoding="utf-8")
    assert implementation_sha256(tmp_path) != before


def test_selection_freeze_comparison_ignores_only_run_specific_evidence_identity() -> None:
    base = {
        "protocol": "saliency_v1",
        "status": "SELECTED",
        "corpusProtocolSha256": "a" * 64,
        "developmentManifestSha256": "b" * 64,
        "developmentRenderIndexSha256": "c" * 64,
        "gridSha256": "d" * 64,
        "selectorSha256": "e" * 64,
        "automaticMapImplementationSha256": "f" * 64,
        "configuration": {"gradientWeight": 0.35},
        "configurationSha256": "1" * 64,
        "sourceBundleId": "2" * 64,
        "evidenceSha256": "3" * 64,
    }
    committed = {**base, "sourceBundleId": "4" * 64, "evidenceSha256": "5" * 64}
    compare_committed_selection(base, committed)
    committed["configurationSha256"] = "6" * 64
    with pytest.raises(DevelopmentRunError, match="configurationSha256"):
        compare_committed_selection(base, committed)


def test_target_phase_is_bounded_resumable_and_uses_frozen_handoff_placeholders() -> None:
    phases = load_phases(Path("qualification/m0-phases.json"))
    phase = next(item for item in phases if item.identifier == "saliency-development-selection")
    flattened = [argument for command in phase.commands for argument in command]
    assert phase.timeout_seconds == 86_400
    assert phase.resource_policy == "cuda-performance-v1"
    assert "{phase_root}/checkpoint" in flattened
    assert "{source_bundle_id}" in flattened
    assert "{environment_fingerprint}" in flattened


def test_identity_parser_rejects_noncanonical_hash() -> None:
    assert exact_sha256("a" * 64, "fixture") == "a" * 64
    with pytest.raises(DevelopmentRunError, match="fixture_invalid"):
        exact_sha256("A" * 64, "fixture")
