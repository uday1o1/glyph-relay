from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator


def validate_report(report: dict[str, Any], schema: dict[str, Any]) -> None:
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(report), key=lambda error: list(error.absolute_path))
    if errors:
        error = errors[0]
        location = ".".join(str(part) for part in error.absolute_path) or "<root>"
        raise ValueError(f"doctor report violates schema at {location}: {error.message}")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: validate_doctor.py GLYPHRELAY SCHEMA", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1])
    schema_path = Path(sys.argv[2])
    completed = subprocess.run(
        [str(executable), "doctor", "--json"],
        check=True,
        capture_output=True,
        text=True,
    )
    report = json.loads(completed.stdout)
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    validate_report(report, schema)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
