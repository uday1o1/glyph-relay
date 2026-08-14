from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from tools.validate_m0_browser_fixture import (
    BrowserFixtureValidationError,
    validate_browser_fixture,
)

SCHEMA = Path("schemas/m0-browser-fixture-v1.schema.json")
MANIFEST = "5443417595e3ccb88c89adc3a2d22842fde3206c736d3069042b62cd1c8ab708"


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def write_fixture(root: Path) -> dict[str, object]:
    stream = b"x" * 21_000
    table_lines = ["frame_index\tbytes\tlatency_ms\tpending_count\toldest_pending_ms"]
    table_lines.extend(f"{index}\t10\t0.5\t0\t0" for index in range(2_100))
    table = ("\n".join(table_lines) + "\n").encode()
    configuration = json.dumps(
        {"configuration_sha256": "a" * 64}, sort_keys=True, separators=(",", ":")
    ).encode()
    (root / "nvenc-browser-720p30.h264").write_bytes(stream)
    (root / "nvenc-browser-720p30-frames.tsv").write_bytes(table)
    (root / "nvenc-browser-720p30-configuration.json").write_bytes(configuration)
    (root / "PASSED").write_text(MANIFEST + "\n", encoding="utf-8")
    summary: dict[str, object] = {
        "schema_version": 1,
        "protocol": "m0_nvenc_browser_fixture_v1",
        "status": "PASSED",
        "manifest_sha256": MANIFEST,
        "presentation": "sharing_720p30",
        "width": 1280,
        "height": 720,
        "frames_per_second": 30,
        "level_idc": 31,
        "warmup_frames": 300,
        "measurement_frames": 1800,
        "frame_count": 2100,
        "requested_bps": 2_000_000,
        "measurement_bytes": 18_000,
        "payload_bps": 2_400.0,
        "configuration_sha256": "a" * 64,
        "stream_sha256": digest(stream),
        "frame_table_sha256": digest(table),
        "configuration_file_sha256": digest(configuration),
        "source_sha256": {
            "frame_0": "858b32ab999e6193daf9035ec26b9d3e0893d1faa3bc3404a7721fcc4b09c6e6",
            "frame_300": "35ea1743773a43b1f785121e864bcf66c8c510591d1d17e015fd9be597a6d3f3",
            "frame_2099": "52443d6562be5e66cfc716a5f8c05df96f18e79a4663f0f4dbb0845542594abd",
        },
        "independent_decoder": "ffmpeg_raw_yuv420p_exact_frame_count",
    }
    (root / "browser-fixture-summary.json").write_text(json.dumps(summary), encoding="utf-8")
    return summary


def rewrite_summary(root: Path, summary: dict[str, object]) -> None:
    (root / "browser-fixture-summary.json").write_text(json.dumps(summary), encoding="utf-8")


def test_complete_nvenc_browser_fixture_passes_without_replaying_decoder(tmp_path: Path) -> None:
    write_fixture(tmp_path)
    result = validate_browser_fixture(tmp_path, SCHEMA, run_independent_decoder=False)
    assert result["frame_count"] == 2_100


def test_seeded_pending_ring_overflow_fails_for_its_reason(tmp_path: Path) -> None:
    summary = write_fixture(tmp_path)
    table_path = tmp_path / "nvenc-browser-720p30-frames.tsv"
    lines = table_path.read_text(encoding="utf-8").splitlines()
    lines[1] = "0\t10\t0.5\t4\t0"
    table_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    summary["frame_table_sha256"] = digest(table_path.read_bytes())
    rewrite_summary(tmp_path, summary)
    with pytest.raises(BrowserFixtureValidationError, match="frame_table_pending_count_invalid"):
        validate_browser_fixture(tmp_path, SCHEMA, run_independent_decoder=False)


def test_nearby_bounded_pending_control_passes(tmp_path: Path) -> None:
    summary = write_fixture(tmp_path)
    table_path = tmp_path / "nvenc-browser-720p30-frames.tsv"
    lines = table_path.read_text(encoding="utf-8").splitlines()
    lines[1] = "0\t10\t0.5\t3\t0.25"
    table_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    summary["frame_table_sha256"] = digest(table_path.read_bytes())
    rewrite_summary(tmp_path, summary)
    validate_browser_fixture(tmp_path, SCHEMA, run_independent_decoder=False)


def test_seeded_stream_corruption_fails_hash_verification(tmp_path: Path) -> None:
    write_fixture(tmp_path)
    with (tmp_path / "nvenc-browser-720p30.h264").open("ab") as stream:
        stream.write(b"corrupt")
    with pytest.raises(BrowserFixtureValidationError, match="fixture_stream_hash_mismatch"):
        validate_browser_fixture(tmp_path, SCHEMA, run_independent_decoder=False)


def test_seeded_source_identity_change_fails_schema(tmp_path: Path) -> None:
    summary = write_fixture(tmp_path)
    source = summary["source_sha256"]
    assert isinstance(source, dict)
    source["frame_300"] = "b" * 64
    rewrite_summary(tmp_path, summary)
    with pytest.raises(BrowserFixtureValidationError, match="summary_schema_invalid"):
        validate_browser_fixture(tmp_path, SCHEMA, run_independent_decoder=False)
