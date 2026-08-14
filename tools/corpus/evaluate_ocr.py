from __future__ import annotations

import argparse
import json
import math
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEVELOPMENT_MANIFEST = (ROOT / "corpus" / "manifests" / "development.json").resolve()
DEVELOPMENT_OUTPUT_ROOT = (ROOT / "corpus" / "generated" / "development").resolve()
OVERALL_MAXIMUM = 0.02
SMALL_GLYPH_MAXIMUM = 0.05


@dataclass(frozen=True)
class RegionScore:
    capped_edits: int
    characters: int
    missing: bool


@dataclass(frozen=True)
class SequenceScore:
    bounded_cer: float
    sequence_id: str
    small_glyph_bounded_cer: float
    stratum: str


def normalize_text(value: str, *, tesseract_output: bool) -> str:
    normalized = unicodedata.normalize("NFC", value.replace("\r\n", "\n").replace("\r", "\n"))
    if tesseract_output and normalized.endswith("\n"):
        normalized = normalized[:-1]
    return normalized


def levenshtein(left: str, right: str) -> int:
    if len(left) < len(right):
        left, right = right, left
    previous = list(range(len(right) + 1))
    for left_index, left_character in enumerate(left, start=1):
        current = [left_index]
        for right_index, right_character in enumerate(right, start=1):
            current.append(
                min(
                    current[-1] + 1,
                    previous[right_index] + 1,
                    previous[right_index - 1] + (left_character != right_character),
                )
            )
        previous = current
    return previous[-1]


def score_region(truth: str, prediction: str | None) -> RegionScore:
    normalized_truth = normalize_text(truth, tesseract_output=False)
    if not normalized_truth:
        raise ValueError("truth_region_empty")
    if prediction is None:
        return RegionScore(len(normalized_truth), len(normalized_truth), True)
    normalized_prediction = normalize_text(prediction, tesseract_output=True)
    distance = levenshtein(normalized_truth, normalized_prediction)
    return RegionScore(min(distance, len(normalized_truth)), len(normalized_truth), False)


def _object(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label}_must_be_object")
    return value


def _list(value: object, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise ValueError(f"{label}_must_be_array")
    return value


def evaluate(manifest: dict[str, Any], ocr_directory: Path) -> dict[str, Any]:
    if manifest.get("protocol") != "corpus_protocol_v1" or manifest.get("split") != "development":
        raise ValueError("development_evaluator_rejects_non_development_manifest")
    sequences = _list(manifest.get("sequences"), "sequences")
    sequence_scores: list[SequenceScore] = []
    missing_regions = 0
    for raw_sequence in sequences:
        sequence = _object(raw_sequence, "sequence")
        sequence_id = sequence.get("sequenceId")
        stratum = sequence.get("stratum")
        if not isinstance(sequence_id, str) or not isinstance(stratum, str):
            raise ValueError("sequence_identity_invalid")
        total_edits = 0
        total_characters = 0
        small_edits = 0
        small_characters = 0
        for raw_frame in _list(sequence.get("sampleFrames"), "sampleFrames"):
            frame = _object(raw_frame, "sampleFrame")
            frame_id = frame.get("frameId")
            if not isinstance(frame_id, int):
                raise ValueError("sample_frame_id_invalid")
            for raw_region in _list(frame.get("textRegions"), "textRegions"):
                region = _object(raw_region, "textRegion")
                region_id = region.get("id")
                truth = region.get("truth")
                glyphs = _list(region.get("glyphs"), "glyphs")
                if not isinstance(region_id, str) or not isinstance(truth, str):
                    raise ValueError("text_region_invalid")
                path = ocr_directory / sequence_id / f"{frame_id:03d}-{region_id}.txt"
                prediction = path.read_text(encoding="utf-8") if path.is_file() else None
                score = score_region(truth, prediction)
                total_edits += score.capped_edits
                total_characters += score.characters
                missing_regions += int(score.missing)
                small_region = bool(glyphs) and all(
                    _object(glyph, "glyph").get("smallGlyphSubset") is True for glyph in glyphs
                )
                if small_region:
                    small_edits += score.capped_edits
                    small_characters += score.characters
        if total_characters == 0 or small_characters == 0:
            raise ValueError("sequence_truth_floor_missing")
        sequence_scores.append(
            SequenceScore(
                bounded_cer=total_edits / total_characters,
                sequence_id=sequence_id,
                small_glyph_bounded_cer=small_edits / small_characters,
                stratum=stratum,
            )
        )

    weights = _object(
        _object(manifest.get("splitContract"), "splitContract").get("stratumWeights"),
        "stratumWeights",
    )
    per_stratum: dict[str, dict[str, float | int]] = {}
    for stratum in sorted(weights):
        members = [score for score in sequence_scores if score.stratum == stratum]
        if not members:
            raise ValueError(f"stratum_has_no_sequences:{stratum}")
        per_stratum[stratum] = {
            "boundedCer": sum(score.bounded_cer for score in members) / len(members),
            "sequences": len(members),
            "smallGlyphBoundedCer": sum(score.small_glyph_bounded_cer for score in members)
            / len(members),
        }
    if not math.isclose(
        sum(float(weight) for weight in weights.values()), 1.0, rel_tol=0, abs_tol=1e-12
    ):
        raise ValueError("stratum_weights_do_not_sum_to_one")
    overall = sum(
        float(weights[stratum]) * float(result["boundedCer"])
        for stratum, result in per_stratum.items()
    )
    small_glyph_bounded_cer = sum(
        float(weights[stratum]) * float(result["smallGlyphBoundedCer"])
        for stratum, result in per_stratum.items()
    )
    passed = overall <= OVERALL_MAXIMUM and small_glyph_bounded_cer <= SMALL_GLYPH_MAXIMUM
    return {
        "schemaVersion": 1,
        "protocol": "corpus_protocol_v1",
        "split": "development",
        "status": "PASSED" if passed else "INSUFFICIENT_EVIDENCE",
        "overallBoundedCer": overall,
        "smallGlyphBoundedCer": small_glyph_bounded_cer,
        "thresholds": {
            "overallBoundedCerMaximum": OVERALL_MAXIMUM,
            "smallGlyphBoundedCerMaximum": SMALL_GLYPH_MAXIMUM,
        },
        "missingRegions": missing_regions,
        "perStratum": per_stratum,
        "perSequence": [
            {
                "boundedCer": score.bounded_cer,
                "sequenceId": score.sequence_id,
                "smallGlyphBoundedCer": score.small_glyph_bounded_cer,
                "stratum": score.stratum,
            }
            for score in sequence_scores
        ],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Evaluate frozen development lossless OCR")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--ocr-results", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.resolve()
    ocr_results = args.ocr_results.resolve()
    output = args.output.resolve()
    if manifest_path != DEVELOPMENT_MANIFEST:
        raise ValueError("development_evaluator_manifest_path_rejected")
    if not ocr_results.is_relative_to(DEVELOPMENT_OUTPUT_ROOT):
        raise ValueError("development_evaluator_ocr_path_rejected")
    if not output.is_relative_to(DEVELOPMENT_OUTPUT_ROOT):
        raise ValueError("development_evaluator_output_path_rejected")
    if output.exists():
        raise FileExistsError(f"OCR report already exists: {output}")
    manifest = _object(json.loads(manifest_path.read_text(encoding="utf-8")), "manifest")
    report = evaluate(manifest, ocr_results)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as sink:
        json.dump(report, sink, sort_keys=True, separators=(",", ":"))
        sink.write("\n")
    print(
        json.dumps(
            {
                "overallBoundedCer": report["overallBoundedCer"],
                "smallGlyphBoundedCer": report["smallGlyphBoundedCer"],
                "status": report["status"],
            },
            sort_keys=True,
        )
    )
    return 0 if report["status"] == "PASSED" else 9


if __name__ == "__main__":
    raise SystemExit(main())
