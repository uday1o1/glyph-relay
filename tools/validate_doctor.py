from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any


def validate_report(report: dict[str, Any], schema: dict[str, Any]) -> None:
    required = schema["required"]
    missing = sorted(set(required) - report.keys())
    if missing:
        raise ValueError(f"doctor report is missing required keys: {missing}")
    expected_version = schema["properties"]["schema_version"]["const"]
    if report.get("schema_version") != expected_version:
        raise ValueError("doctor schema version does not match the committed schema")
    unexpected = sorted(set(report) - set(schema["properties"]))
    if unexpected:
        raise ValueError(f"doctor report has unexpected top-level keys: {unexpected}")


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
