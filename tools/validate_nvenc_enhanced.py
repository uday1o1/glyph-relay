from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path
from typing import Any

import jsonschema

from tools.corpus.saliency_selector import canonical_json, configuration_from_json

EXPECTED_MODES = ("uniform", "fixed_emphasis", "automatic_emphasis")


class EnhancedNvencValidationError(RuntimeError):
    """Raised when target NVENC evidence cannot support the milestone gate."""


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EnhancedNvencValidationError("nvenc_evidence_json_invalid") from error
    if not isinstance(value, dict):
        raise EnhancedNvencValidationError("nvenc_evidence_object_required")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def probe_stream(path: Path, expected_frames: int) -> dict[str, Any]:
    decoded = subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "h264", "-i", str(path), "-f", "null", "-"],
        check=False,
        capture_output=True,
        text=True,
    )
    if decoded.returncode != 0 or decoded.stderr:
        raise EnhancedNvencValidationError("nvenc_independent_decode_failed")
    probed = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-count_frames",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,width,height,level,pix_fmt,color_range,color_space,"
            "color_transfer,color_primaries,nb_read_frames",
            "-of",
            "json",
            str(path),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if probed.returncode != 0 or probed.stderr:
        raise EnhancedNvencValidationError("nvenc_independent_probe_failed")
    try:
        payload = json.loads(probed.stdout)
        stream = payload["streams"][0]
    except (json.JSONDecodeError, KeyError, IndexError, TypeError) as error:
        raise EnhancedNvencValidationError("nvenc_independent_probe_invalid") from error
    expected = {
        "codec_name": "h264",
        "width": 1920,
        "height": 1080,
        "level": 40,
        "pix_fmt": "yuv420p",
        "color_range": "tv",
        "color_space": "bt709",
        "color_transfer": "bt709",
        "color_primaries": "bt709",
        "nb_read_frames": str(expected_frames),
    }
    if any(stream.get(name) != value for name, value in expected.items()):
        raise EnhancedNvencValidationError("nvenc_independent_stream_contract_mismatch")
    return expected


def validate(evidence_path: Path, schema_path: Path, selection_path: Path) -> dict[str, Any]:
    evidence = load_object(evidence_path)
    schema = load_object(schema_path)
    try:
        jsonschema.Draft202012Validator(schema).validate(evidence)
    except jsonschema.ValidationError as error:
        raise EnhancedNvencValidationError("nvenc_evidence_schema_invalid") from error
    sessions = evidence["sessions"]
    if [session["mode"] for session in sessions] != list(EXPECTED_MODES):
        raise EnhancedNvencValidationError("nvenc_mode_coverage_or_order_invalid")
    selection = load_object(selection_path)
    try:
        configuration = configuration_from_json(selection.get("configuration")).json()
    except ValueError as error:
        raise EnhancedNvencValidationError("nvenc_selection_configuration_invalid") from error
    configuration_sha256 = hashlib.sha256(canonical_json(configuration)).hexdigest()
    if (
        configuration_sha256 != selection.get("configurationSha256")
        or evidence["saliencyConfiguration"] != configuration
        or evidence["saliencyConfigurationSha256"] != configuration_sha256
        or evidence["saliencySelectionSha256"] != sha256_file(selection_path)
    ):
        raise EnhancedNvencValidationError("nvenc_selection_identity_mismatch")
    streams: list[dict[str, Any]] = []
    for session in sessions:
        mode = session["mode"]
        path = evidence_path.parent / f"{mode}.h264"
        if (
            not path.is_file()
            or path.stat().st_size != session["streamBytes"]
            or sha256_file(path) != session["streamSha256"]
        ):
            raise EnhancedNvencValidationError("nvenc_stream_identity_invalid")
        probe = probe_stream(path, session["frames"])
        streams.append(
            {
                "mode": mode,
                "frames": session["frames"],
                "streamSha256": session["streamSha256"],
                "probe": probe,
            }
        )
    return {
        "schemaVersion": 1,
        "status": "PASSED",
        "decoder": "ffmpeg",
        "probe": "ffprobe",
        "saliencyConfigurationSha256": configuration_sha256,
        "saliencySelectionSha256": evidence["saliencySelectionSha256"],
        "streams": streams,
    }


def write_exclusive(path: Path, value: dict[str, Any]) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, sort_keys=True, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    descriptor = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Independently decode and validate enhanced NVENC target evidence"
    )
    parser.add_argument("evidence", type=Path)
    parser.add_argument("schema", type=Path)
    parser.add_argument("selection", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    try:
        result = validate(
            arguments.evidence.resolve(),
            arguments.schema.resolve(),
            arguments.selection.resolve(),
        )
        write_exclusive(arguments.output.resolve(), result)
    except (EnhancedNvencValidationError, OSError, subprocess.SubprocessError) as error:
        print(f"enhanced NVENC validation failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
