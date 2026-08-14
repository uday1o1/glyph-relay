from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.corpus.saliency_selector import (
    GRID_PATH,
    SELECTION_SCHEMA_PATH,
    SaliencySelectionError,
    canonical_json,
    load_object,
    select_development_evidence,
    validate_schema,
    verify_protocol_lock,
)  # noqa: E402


def validate_artifacts(evidence: dict[str, Any], selection: dict[str, Any]) -> None:
    lock = verify_protocol_lock()
    grid = load_object(GRID_PATH)
    validate_schema(selection, SELECTION_SCHEMA_PATH)
    expected = select_development_evidence(evidence, grid, lock)
    if canonical_json(selection) != canonical_json(expected):
        raise SaliencySelectionError("selection_does_not_reproduce_from_development_evidence")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reproduce and validate a frozen saliency_v1 development selection"
    )
    parser.add_argument("--development-evidence", type=Path, required=True)
    parser.add_argument("--selection", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        evidence = load_object(arguments.development_evidence.resolve())
        selection = load_object(arguments.selection.resolve())
        validate_artifacts(evidence, selection)
    except SaliencySelectionError as error:
        print(f"saliency selection validation failed: {error}")
        return 1
    print(
        json.dumps(
            {
                "candidateCount": selection["candidateCount"],
                "configurationSha256": selection["configurationSha256"],
                "status": "VALID",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
