from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from tools.corpus.evaluate_ocr import evaluate

ROOT = Path(__file__).resolve().parents[2]
VALIDATION_MANIFEST = (ROOT / "corpus" / "manifests" / "validation.json").resolve()


def _object(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label}_must_be_object")
    return value


def evaluate_validation(manifest: dict[str, Any], ocr_directory: Path) -> dict[str, Any]:
    if manifest.get("protocol") != "corpus_protocol_v1" or manifest.get("split") != "validation":
        raise ValueError("validation_evaluator_rejects_non_validation_manifest")
    development_view = dict(manifest)
    development_view["split"] = "development"
    result = evaluate(development_view, ocr_directory)
    result["split"] = "validation"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate frozen validation lossless OCR")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--ocr-results", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    manifest_path = arguments.manifest.resolve()
    output = arguments.output.resolve()
    if manifest_path != VALIDATION_MANIFEST:
        raise ValueError("validation_evaluator_manifest_path_rejected")
    if output.exists():
        raise FileExistsError(f"validation OCR report already exists: {output}")
    manifest = _object(json.loads(manifest_path.read_text(encoding="utf-8")), "manifest")
    report = evaluate_validation(manifest, arguments.ocr_results.resolve())
    output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, sort_keys=True, separators=(",", ":"))
        stream.write("\n")
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
