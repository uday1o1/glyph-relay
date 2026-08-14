from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from typing import Any
from xml.etree import ElementTree

from jsonschema import Draft202012Validator, FormatChecker

from tools.gpu.source_bundle import verify_result_checksums
from tools.validate_public_evidence import validate_public_evidence


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def validate_schema(instance: Any, schema_path: Path) -> None:
    Draft202012Validator(
        load_json(schema_path),
        format_checker=FormatChecker(),
    ).validate(instance)


def validate_qualification(result_root: Path, schema_root: Path) -> None:
    root = result_root.resolve(strict=True)
    verify_result_checksums(root)
    status = load_json(root / "status.json")
    environment = load_json(root / "environment.json")
    validate_schema(status, schema_root / "qualification-status-v1.schema.json")
    validate_schema(environment, schema_root / "qualification-environment-v1.schema.json")
    phase_results: dict[str, dict[str, Any]] = {}
    for state_path in sorted((root / "phases").glob("*/state.json")):
        state = load_json(state_path)
        raw_result_path = state["result_path"]
        if not isinstance(raw_result_path, str):
            raise ValueError("qualification phase result path is invalid")
        relative_posix = PurePosixPath(raw_result_path)
        if (
            relative_posix.is_absolute()
            or ".." in relative_posix.parts
            or str(relative_posix) != raw_result_path
        ):
            raise ValueError("qualification phase result path is invalid")
        relative = Path(*relative_posix.parts)
        result_path = (root / relative).resolve(strict=True)
        if (
            not result_path.is_relative_to(root)
            or not result_path.is_file()
            or result_path.is_symlink()
        ):
            raise ValueError("qualification phase result path is invalid")
        result = load_json(result_path)
        validate_schema(result, schema_root / "qualification-phase-v1.schema.json")
        for raw_output, expected_hash in result["output_hashes"].items():
            output_posix = PurePosixPath(raw_output)
            if (
                output_posix.is_absolute()
                or ".." in output_posix.parts
                or str(output_posix) != raw_output
            ):
                raise ValueError("qualification phase output path is invalid")
            output = (root / Path(*output_posix.parts)).resolve(strict=True)
            if (
                not output.is_relative_to(root)
                or not output.is_file()
                or output.is_symlink()
                or hashlib.sha256(output.read_bytes()).hexdigest() != expected_hash
            ):
                raise ValueError("qualification phase output hash does not match")
        assessment = result_path.parent / "resource-assessment.json"
        if assessment.exists():
            if assessment.is_symlink() or not assessment.is_file():
                raise ValueError("qualification resource assessment path is invalid")
            validate_schema(
                load_json(assessment),
                schema_root / "qualification-resource-assessment-v1.schema.json",
            )
        if state != {
            "schema_version": 1,
            "phase": result["phase"],
            "status": result["status"],
            "result_path": result_path.relative_to(root).as_posix(),
            "result_sha256": hashlib.sha256(result_path.read_bytes()).hexdigest(),
        }:
            raise ValueError("qualification phase state does not match its result")
        phase_results[result["phase"]] = result
    if not phase_results:
        raise ValueError("qualification result contains no phase results")
    expected_states = {name: result["status"] for name, result in phase_results.items()}
    if status["phases"] != expected_states:
        raise ValueError("qualification top-level phase states do not match results")
    required = [result["status"] for result in phase_results.values() if result["required"]]
    expected_top = (
        "FAILED"
        if "FAILED" in required
        else "BLOCKED"
        if any(state in {"BLOCKED", "DEFERRED_INTERACTIVE"} for state in required)
        else "PASSED"
    )
    if status["status"] != expected_top:
        raise ValueError("qualification top-level status is not derived from required phases")
    if environment["bundle_id"] != status["bundle_id"]:
        raise ValueError("qualification environment and status bundle identities differ")
    if not (root / "REPORT.md").is_file() or not (root / "commands.jsonl").is_file():
        raise ValueError("qualification report or command log is missing")
    validate_public_evidence(root / "public-evidence", schema_root)
    public_summary = load_json(root / "public-evidence/summary.json")
    if (
        public_summary["status"] != status["status"]
        or public_summary["bundle_id"] != status["bundle_id"]
    ):
        raise ValueError("qualification public evidence identity differs from private result")
    document = ElementTree.parse(root / "junit.xml")
    if document.getroot().tag != "testsuite":
        raise ValueError("qualification JUnit document is invalid")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a GlyphRelay qualification result")
    parser.add_argument("result_root", type=Path)
    parser.add_argument("--schemas", type=Path, default=Path("schemas"))
    arguments = parser.parse_args()
    validate_qualification(arguments.result_root, arguments.schemas)
    print("qualification result validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
