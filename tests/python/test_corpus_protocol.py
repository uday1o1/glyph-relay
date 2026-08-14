from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any, cast

from tools.check_corpus_protocol import ROOT, validate_manifest, validate_protocol


def development_manifest() -> dict[str, Any]:
    return cast(
        dict[str, Any],
        json.loads((ROOT / "corpus/manifests/development.json").read_text(encoding="utf-8")),
    )


def test_frozen_corpus_protocol_passes() -> None:
    assert validate_protocol() == []


def test_character_floor_defect_fails_for_intended_reason() -> None:
    manifest = development_manifest()
    manifest["sequences"] = manifest["sequences"][:1]
    errors = validate_manifest(manifest, "development")
    assert any("fewer than 64" in error for error in errors)
    assert any("character floor" in error for error in errors)


def test_duplicate_sequence_control_and_seeded_defect() -> None:
    control = development_manifest()
    assert not any("duplicated" in error for error in validate_manifest(control, "development"))
    defect = copy.deepcopy(control)
    defect["sequences"][1]["sequenceId"] = defect["sequences"][0]["sequenceId"]
    errors = validate_manifest(defect, "development")
    assert any("identity duplicated" in error for error in errors)


def test_small_glyph_height_defect_and_nearby_control() -> None:
    control = development_manifest()
    small = next(
        glyph
        for sequence in control["sequences"]
        for frame in sequence["sampleFrames"]
        for region in frame["textRegions"]
        for glyph in region["glyphs"]
        if glyph["smallGlyphSubset"]
    )
    raster_id = small["rasterId"]
    defect = copy.deepcopy(control)
    raster = next(entry for entry in defect["glyphRasterCatalog"] if entry["id"] == raster_id)
    raster["height"] = 11
    errors = validate_manifest(defect, "development")
    assert any("small glyph height" in error for error in errors)


def test_validation_and_final_renderer_outputs_do_not_exist() -> None:
    assert not (Path("corpus/generated/validation")).exists()
    assert not (Path("corpus/generated/final_test")).exists()
