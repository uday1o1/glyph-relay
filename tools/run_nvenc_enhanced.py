from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from tools.corpus.saliency_selector import (  # noqa: E402
    SaliencySelectionError,
    canonical_json,
    configuration_from_json,
    load_object,
)


class EnhancedNvencRunError(RuntimeError):
    """Raised when the enhanced encoder cannot consume the frozen selection."""


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native_command(
    native: Path, selection_path: Path, output: Path
) -> tuple[list[str], dict[str, Any]]:
    selection = load_object(selection_path)
    configuration = configuration_from_json(selection.get("configuration"))
    configuration_json = configuration.json()
    configuration_sha256 = hashlib.sha256(canonical_json(configuration_json)).hexdigest()
    if configuration_sha256 != selection.get("configurationSha256"):
        raise EnhancedNvencRunError("nvenc_selection_configuration_hash_invalid")
    command = [
        str(native),
        "--output",
        str(output),
        "--selection-sha256",
        sha256_file(selection_path),
        "--configuration-sha256",
        configuration_sha256,
        "--gradient-weight",
        str(configuration.gradient_weight),
        "--contrast-weight",
        str(configuration.contrast_weight),
        "--edge-pair-weight",
        str(configuration.edge_pair_weight),
        "--small-structure-weight",
        str(configuration.small_structure_weight),
        "--entry-threshold",
        str(configuration.entry_threshold),
        "--exit-threshold",
        str(configuration.exit_threshold),
        "--previous-score-coefficient",
        str(configuration.previous_score_coefficient),
        "--dilation-radius-tiles",
        str(configuration.dilation_radius_tiles),
    ]
    return command, selection


def run(native: Path, selection_path: Path, output: Path) -> None:
    command, _ = native_command(native, selection_path, output)
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        raise EnhancedNvencRunError(f"nvenc_enhanced_native_exit_{completed.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run enhanced NVENC qualification with the frozen saliency selection"
    )
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--selection", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        run(
            arguments.native.resolve(strict=True),
            arguments.selection.resolve(strict=True),
            arguments.output.resolve(),
        )
    except (EnhancedNvencRunError, SaliencySelectionError, OSError) as error:
        print(f"enhanced NVENC run failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
