from __future__ import annotations

import argparse
import json
import stat
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator

from tools.gpu.public_evidence import public_value_is_safe, sha256_file
from tools.gpu.source_bundle import verify_result_checksums


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def validate_public_evidence(root: Path, schemas: Path) -> None:
    if root.is_symlink():
        raise ValueError("public_evidence_root_invalid")
    directory = root.resolve(strict=True)
    for path in directory.rglob("*"):
        mode = path.lstat().st_mode
        if not stat.S_ISREG(mode) and not stat.S_ISDIR(mode):
            raise ValueError("public_evidence_member_type_invalid")
    verify_result_checksums(directory)
    summary = load_json(directory / "summary.json")
    Draft202012Validator(
        load_json(schemas / "qualification-public-evidence-v1.schema.json")
    ).validate(summary)
    if (
        not (directory / "REPORT.md").is_file()
        or not (directory / "PUBLICATION_REVIEW_REQUIRED.md").is_file()
    ):
        raise ValueError("public_evidence_review_files_missing")
    if not public_value_is_safe(summary, (str(Path.home()),)):
        raise ValueError("public_evidence_contains_private_value")
    declared = {
        item["path"]: item["sha256"] for item in summary["evidence_files"] if isinstance(item, dict)
    }
    actual = {
        path.relative_to(directory).as_posix(): sha256_file(path)
        for path in directory.glob("evidence/*/*.json")
        if path.is_file() and not path.is_symlink()
    }
    if actual != declared:
        raise ValueError("public_evidence_artifact_manifest_mismatch")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a GlyphRelay public evidence candidate")
    parser.add_argument("root", type=Path)
    parser.add_argument("--schemas", type=Path, default=Path("schemas"))
    arguments = parser.parse_args()
    validate_public_evidence(arguments.root, arguments.schemas.resolve(strict=True))
    print("public evidence candidate validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
