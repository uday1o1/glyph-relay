from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = Path("protocols/m0_fixed_map_v1")
SHA256 = re.compile(r"[0-9a-f]{64}")


def load_json(root: Path, name: str) -> dict[str, Any]:
    value = json.loads((root / PROTOCOL / name).read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{name} must contain an object")
    return value


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(64 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def expect(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def parse_manifest(root: Path, errors: list[str]) -> dict[str, str]:
    path = root / PROTOCOL / "manifest.lock"
    content = path.read_bytes()
    lines = content.decode("utf-8").splitlines(keepends=True)
    expect(
        errors, bool(lines) and lines[0] == "glyphrelay-protocol-lock-v1\n", "bad manifest magic"
    )
    expect(errors, len(lines) >= 4, "manifest is too short")
    if not lines:
        return {}
    final = lines[-1].removesuffix("\n").split("\t")
    expect(
        errors,
        len(final) == 2
        and final[0] == "manifest_sha256"
        and SHA256.fullmatch(final[1]) is not None,
        "bad manifest digest record",
    )
    if len(final) == 2:
        expect(
            errors,
            hashlib.sha256("".join(lines[:-1]).encode()).hexdigest() == final[1],
            "manifest self-hash mismatch",
        )
    components: dict[str, str] = {}
    for line in lines[2:-1]:
        fields = line.removesuffix("\n").split("\t")
        if len(fields) != 3 or fields[0] != "component":
            errors.append("bad manifest component record")
            continue
        relative, expected_hash = fields[1:]
        path_value = Path(relative)
        expect(
            errors,
            not path_value.is_absolute()
            and path_value == Path(*path_value.parts)
            and ".." not in path_value.parts
            and "." not in path_value.parts,
            f"unsafe component path: {relative}",
        )
        expect(errors, relative not in components, f"duplicate component: {relative}")
        components[relative] = expected_hash
        candidate = root / path_value
        expect(
            errors, candidate.is_file() and not candidate.is_symlink(), f"bad component: {relative}"
        )
        if candidate.is_file():
            expect(
                errors,
                file_sha256(candidate) == expected_hash,
                f"component hash mismatch: {relative}",
            )
    return components


def parse_emphasis_map(root: Path, errors: list[str]) -> list[int]:
    lines = (root / PROTOCOL / "emphasis-map.rle").read_text(encoding="utf-8").splitlines()
    expect(errors, bool(lines) and lines[0] == "glyphrelay-emphasis-map-rle-v1", "bad map magic")
    values: list[int] = []
    for line in lines:
        fields = line.split("\t")
        if not fields or fields[0] != "run":
            continue
        if len(fields) != 4:
            errors.append("bad map run")
            continue
        start, count, level = map(int, fields[1:])
        expect(errors, start == len(values), "non-contiguous map run")
        expect(errors, count > 0 and 0 <= level <= 5, "invalid map count or level")
        values.extend([level] * count)
    return values


def validate_protocol(root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    components = parse_manifest(root, errors)
    required = {
        str(PROTOCOL / name)
        for name in (
            "source.json",
            "protected.mask.json",
            "comparison.mask.json",
            "emphasis-map.rle",
            "metric.json",
            "run-config.json",
            "frame-hashes.tsv",
        )
    }
    required.update(
        {
            "include/glyphrelay/synthetic_source.hpp",
            "src/core/synthetic_source.cpp",
            "include/glyphrelay/quality_metrics.hpp",
            "src/core/quality_metrics.cpp",
        }
    )
    expect(errors, required <= components.keys(), "manifest omits a required protocol component")

    source = load_json(root, "source.json")
    expect(errors, source.get("protocol") == "m0_fixed_map_v1", "source protocol mismatch")
    expect(errors, source.get("seed_hex") == "6d305f6669786564", "source seed mismatch")
    expect(errors, source.get("visible_width") == 1920, "visible width mismatch")
    expect(errors, source.get("visible_height") == 1080, "visible height mismatch")
    expect(errors, source.get("coded_width") == 1920, "coded width mismatch")
    expect(errors, source.get("coded_height") == 1088, "coded height mismatch")
    expect(errors, source.get("frame_rate_numerator") == 30, "frame rate mismatch")
    expect(errors, source.get("frame_rate_denominator") == 1, "frame rate denominator mismatch")
    expect(errors, source.get("warmup_seconds") == 10, "warmup mismatch")
    expect(errors, source.get("measurement_seconds") == 60, "measurement duration mismatch")
    expect(errors, source.get("frame_count") == 2100, "frame count mismatch")
    expect(errors, source.get("pixel_format") == "nv12", "pixel format mismatch")
    expect(errors, source.get("range") == "limited", "color range mismatch")

    protected = load_json(root, "protected.mask.json")
    comparison = load_json(root, "comparison.mask.json")
    protected_rectangle = protected.get("rectangle", {})
    comparison_rectangle = comparison.get("rectangle", {})
    expect(
        errors,
        protected_rectangle == {"x": 640, "y": 384, "width": 640, "height": 320},
        "protected rectangle mismatch",
    )
    expect(
        errors,
        comparison_rectangle == {"x": 640, "y": 32, "width": 640, "height": 320},
        "comparison rectangle mismatch",
    )
    expect(
        errors,
        protected_rectangle.get("width") == comparison_rectangle.get("width")
        and protected_rectangle.get("height") == comparison_rectangle.get("height"),
        "quality regions must have equal area",
    )

    actual_map = parse_emphasis_map(root, errors)
    expected_map = [0] * (120 * 68)
    for y in range(24, 44):
        for x in range(40, 80):
            expected_map[y * 120 + x] = 4
    expect(
        errors, actual_map == expected_map, "emphasis map does not match the protected rectangle"
    )

    metric = load_json(root, "metric.json")
    expect(errors, metric.get("peak") == 255, "metric peak mismatch")
    expect(errors, metric.get("lossless_psnr") == "positive_infinity", "lossless PSNR mismatch")
    expect(
        errors,
        metric.get("aggregation")
        == "per_frame_then_arithmetic_mean_with_every_measurement_frame_retained",
        "metric aggregation mismatch",
    )

    run = load_json(root, "run-config.json")
    expect(
        errors,
        run.get("conditions") == ["controlled_uniform", "fixed_emphasis_level_4"],
        "condition set mismatch",
    )
    expect(errors, run.get("spatial_aq") is False, "spatial AQ must be disabled")
    expect(errors, run.get("temporal_aq") is False, "temporal AQ must be disabled")
    measurement = run.get("measurement", {})
    expect(errors, measurement.get("repeats_per_condition") == 10, "repeat count mismatch")
    expect(errors, measurement.get("mean_payload_bps_minimum") == 980000, "bitrate floor mismatch")
    expect(
        errors, measurement.get("mean_payload_bps_maximum") == 1020000, "bitrate ceiling mismatch"
    )
    expect(
        errors,
        measurement.get("minimum_protected_psnr_improvement_db") == 1.0,
        "protected PSNR gate mismatch",
    )
    expect(
        errors,
        measurement.get("minimum_spatial_allocation_improvement_db") == 0.75,
        "spatial allocation gate mismatch",
    )

    frame_lines = (root / PROTOCOL / "frame-hashes.tsv").read_text(encoding="utf-8").splitlines()
    expect(errors, len(frame_lines) == 2101, "frame hash count mismatch")
    expect(
        errors,
        bool(frame_lines) and frame_lines[0] == "glyphrelay-frame-hashes-v1",
        "frame hash magic mismatch",
    )
    for index, line in enumerate(frame_lines[1:]):
        fields = line.split("\t")
        expect(
            errors,
            len(fields) == 3
            and fields[0] == "frame"
            and fields[1] == str(index)
            and SHA256.fullmatch(fields[2]) is not None,
            f"malformed frame hash at index {index}",
        )
    return errors


def main() -> int:
    errors = validate_protocol()
    if errors:
        print("\n".join(errors))
        return 1
    print("m0_fixed_map_v1 semantic and hash lock passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
