#!/usr/bin/env python3
"""Validate one no-clobber NVENC browser fixture and its independent decode evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator


class BrowserFixtureValidationError(ValueError):
    """Raised when browser fixture evidence is incomplete or inconsistent."""


STREAM_NAME = "nvenc-browser-720p30.h264"
TABLE_NAME = "nvenc-browser-720p30-frames.tsv"
CONFIGURATION_NAME = "nvenc-browser-720p30-configuration.json"
SUMMARY_NAME = "browser-fixture-summary.json"
EXPECTED_MANIFEST_SHA256 = "5443417595e3ccb88c89adc3a2d22842fde3206c736d3069042b62cd1c8ab708"
FRAME_COUNT = 2_100
WARMUP_FRAMES = 300
FPS = 30


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BrowserFixtureValidationError(f"invalid_json:{path.name}") from error
    if not isinstance(value, dict):
        raise BrowserFixtureValidationError(f"json_object_required:{path.name}")
    return value


def validate_schema(value: dict[str, Any], schema_path: Path) -> None:
    schema = load_json(schema_path)
    validator = Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(value), key=lambda error: list(error.path))
    if errors:
        location = ".".join(str(part) for part in errors[0].path) or "root"
        raise BrowserFixtureValidationError(
            f"summary_schema_invalid:{location}:{errors[0].message}"
        )


def validate_table(path: Path) -> tuple[int, int]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise BrowserFixtureValidationError("frame_table_unreadable") from error
    expected_header = "frame_index\tbytes\tlatency_ms\tpending_count\toldest_pending_ms"
    if not lines or lines[0] != expected_header:
        raise BrowserFixtureValidationError("frame_table_header_invalid")
    if len(lines) != FRAME_COUNT + 1:
        raise BrowserFixtureValidationError("frame_table_count_invalid")
    stream_bytes = 0
    measurement_bytes = 0
    for expected_index, line in enumerate(lines[1:]):
        fields = line.split("\t")
        if len(fields) != 5:
            raise BrowserFixtureValidationError("frame_table_row_width_invalid")
        try:
            frame_index = int(fields[0])
            encoded_bytes = int(fields[1])
            latency_ms = float(fields[2])
            pending_count = int(fields[3])
            oldest_pending_ms = float(fields[4])
        except ValueError as error:
            raise BrowserFixtureValidationError("frame_table_value_invalid") from error
        if frame_index != expected_index:
            raise BrowserFixtureValidationError("frame_table_identity_invalid")
        if not 0 < encoded_bytes <= 16 * 1024 * 1024:
            raise BrowserFixtureValidationError("frame_table_payload_size_invalid")
        if not math.isfinite(latency_ms) or latency_ms < 0:
            raise BrowserFixtureValidationError("frame_table_latency_invalid")
        if not 0 <= pending_count < 4:
            raise BrowserFixtureValidationError("frame_table_pending_count_invalid")
        if not math.isfinite(oldest_pending_ms) or oldest_pending_ms < 0:
            raise BrowserFixtureValidationError("frame_table_pending_age_invalid")
        stream_bytes += encoded_bytes
        if frame_index >= WARMUP_FRAMES:
            measurement_bytes += encoded_bytes
    return stream_bytes, measurement_bytes


def validate_independent_decoder(stream: Path) -> None:
    command = [
        "ffprobe",
        "-v",
        "error",
        "-count_frames",
        "-select_streams",
        "v:0",
        "-show_entries",
        (
            "stream=codec_name,profile,width,height,pix_fmt,level,color_range,color_space,"
            "color_transfer,color_primaries,nb_read_frames"
        ),
        "-of",
        "json",
        str(stream),
    ]
    try:
        completed = subprocess.run(
            command, check=False, capture_output=True, text=True, timeout=180
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise BrowserFixtureValidationError("ffprobe_execution_failed") from error
    if completed.returncode != 0:
        raise BrowserFixtureValidationError("ffprobe_decode_failed")
    try:
        payload = json.loads(completed.stdout)
        streams = payload["streams"]
        facts = streams[0]
    except (json.JSONDecodeError, KeyError, IndexError, TypeError) as error:
        raise BrowserFixtureValidationError("ffprobe_output_invalid") from error
    expected = {
        "codec_name": "h264",
        "profile": "Constrained Baseline",
        "width": 1280,
        "height": 720,
        "pix_fmt": "yuv420p",
        "level": 31,
        "color_range": "tv",
        "color_space": "bt709",
        "color_transfer": "bt709",
        "color_primaries": "bt709",
        "nb_read_frames": str(FRAME_COUNT),
    }
    for name, expected_value in expected.items():
        if facts.get(name) != expected_value:
            raise BrowserFixtureValidationError(f"ffprobe_contract_mismatch:{name}")


def validate_browser_fixture(
    directory: Path, schema_path: Path, *, run_independent_decoder: bool = True
) -> dict[str, Any]:
    if not directory.is_dir():
        raise BrowserFixtureValidationError("fixture_directory_missing")
    if (directory / "FAILED.json").exists():
        raise BrowserFixtureValidationError("fixture_failed_marker_present")
    paths = {
        "stream": directory / STREAM_NAME,
        "table": directory / TABLE_NAME,
        "configuration": directory / CONFIGURATION_NAME,
        "summary": directory / SUMMARY_NAME,
        "passed": directory / "PASSED",
    }
    if any(not path.is_file() for path in paths.values()):
        raise BrowserFixtureValidationError("fixture_required_file_missing")
    summary = load_json(paths["summary"])
    validate_schema(summary, schema_path)
    if summary["manifest_sha256"] != EXPECTED_MANIFEST_SHA256:
        raise BrowserFixtureValidationError("fixture_manifest_identity_invalid")
    if paths["passed"].read_text(encoding="utf-8") != EXPECTED_MANIFEST_SHA256 + "\n":
        raise BrowserFixtureValidationError("fixture_passed_marker_invalid")
    if sha256_file(paths["stream"]) != summary["stream_sha256"]:
        raise BrowserFixtureValidationError("fixture_stream_hash_mismatch")
    if sha256_file(paths["table"]) != summary["frame_table_sha256"]:
        raise BrowserFixtureValidationError("fixture_frame_table_hash_mismatch")
    if sha256_file(paths["configuration"]) != summary["configuration_file_sha256"]:
        raise BrowserFixtureValidationError("fixture_configuration_file_hash_mismatch")

    configuration = load_json(paths["configuration"])
    if configuration.get("configuration_sha256") != summary["configuration_sha256"]:
        raise BrowserFixtureValidationError("fixture_configuration_identity_mismatch")
    stream_bytes, measurement_bytes = validate_table(paths["table"])
    if paths["stream"].stat().st_size != stream_bytes:
        raise BrowserFixtureValidationError("fixture_stream_size_mismatch")
    if summary["measurement_bytes"] != measurement_bytes:
        raise BrowserFixtureValidationError("fixture_measurement_bytes_mismatch")
    expected_payload_bps = measurement_bytes * 8 * FPS / (FRAME_COUNT - WARMUP_FRAMES)
    if not math.isclose(summary["payload_bps"], expected_payload_bps, rel_tol=1e-12):
        raise BrowserFixtureValidationError("fixture_payload_rate_mismatch")
    if run_independent_decoder:
        validate_independent_decoder(paths["stream"])
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("schema", type=Path)
    arguments = parser.parse_args()
    try:
        summary = validate_browser_fixture(arguments.directory, arguments.schema)
    except BrowserFixtureValidationError as error:
        print(f"M0 NVENC browser fixture validation failed: {error}")
        return 1
    print(
        "M0 NVENC browser fixture passed: "
        f"{summary['frame_count']} frames at {summary['payload_bps']:.3f} payload bps"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
