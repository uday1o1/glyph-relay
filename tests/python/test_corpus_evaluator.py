from __future__ import annotations

import json
from pathlib import Path

import pytest

from tools.corpus.evaluate_ocr import evaluate, levenshtein, normalize_text, score_region


def test_levenshtein_and_bounded_region_score() -> None:
    assert levenshtein("kitten", "sitting") == 3
    assert levenshtein("", "abc") == 3
    assert score_region("ABC", "XYZXYZ\n").capped_edits == 3
    assert score_region("ABC", None).missing
    assert normalize_text("e\u0301\r\n", tesseract_output=True) == "é"


def manifest() -> dict[str, object]:
    return {
        "protocol": "corpus_protocol_v1",
        "split": "development",
        "splitContract": {"stratumWeights": {"alpha": 0.5, "beta": 0.5}},
        "sequences": [
            {
                "sequenceId": stratum,
                "stratum": stratum,
                "sampleFrames": [
                    {
                        "frameId": 0,
                        "textRegions": [
                            {
                                "id": "normal",
                                "truth": "CLEAR TEXT",
                                "glyphs": [{"smallGlyphSubset": False}],
                            },
                            {
                                "id": "small",
                                "truth": "SMALL",
                                "glyphs": [{"smallGlyphSubset": True}],
                            },
                        ],
                    }
                ],
            }
            for stratum in ["alpha", "beta"]
        ],
    }


def test_equal_stratum_aggregation_and_missing_maximum(tmp_path: Path) -> None:
    for stratum in ["alpha", "beta"]:
        directory = tmp_path / stratum
        directory.mkdir()
        (directory / "000-normal.txt").write_text("CLEAR TEXT\n", encoding="utf-8")
        (directory / "000-small.txt").write_text(
            "SMALL\n" if stratum == "alpha" else "SMALX\n", encoding="utf-8"
        )
    report = evaluate(manifest(), tmp_path)
    assert report["overallBoundedCer"] == pytest.approx(1 / 30)
    assert report["smallGlyphBoundedCer"] == pytest.approx(0.1)
    assert report["status"] == "INSUFFICIENT_EVIDENCE"

    (tmp_path / "beta" / "000-small.txt").unlink()
    missing = evaluate(manifest(), tmp_path)
    assert missing["missingRegions"] == 1
    assert missing["smallGlyphBoundedCer"] == pytest.approx(0.5)


def test_development_evaluator_rejects_validation_manifest(tmp_path: Path) -> None:
    value = manifest()
    value["split"] = "validation"
    with pytest.raises(ValueError, match="rejects_non_development"):
        evaluate(value, tmp_path)


def test_manifest_fixture_is_json_serializable() -> None:
    assert json.loads(json.dumps(manifest()))["protocol"] == "corpus_protocol_v1"
