from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path, PurePosixPath
from typing import Any

PUBLIC_ARTIFACT_NAMES = {
    "capture-summary.json",
    "encoder-summary.json",
    "gate.json",
    "profile-summary.json",
    "profiler-summary.json",
    "resource-assessment.json",
    "validation.json",
}
IPV4_PATTERN = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
GPU_UUID_PATTERN = re.compile(r"GPU-[0-9a-fA-F-]{16,}")
CREDENTIAL_URI_PATTERN = re.compile(r"(?i)[a-z][a-z0-9+.-]*://[^/@\s:]+:[^/@\s]+@")
PRIVATE_KEY_PATTERN = re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")


class PublicEvidenceError(RuntimeError):
    """Raised when a public evidence export cannot be proven safe."""


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def exclusive_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
        0o600,
    )
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())


def public_string_is_safe(value: str, private_values: Sequence[str]) -> bool:
    if any(private and private != "/" and private in value for private in private_values):
        return False
    return not (
        IPV4_PATTERN.search(value)
        or GPU_UUID_PATTERN.search(value)
        or CREDENTIAL_URI_PATTERN.search(value)
        or PRIVATE_KEY_PATTERN.search(value)
    )


def public_value_is_safe(value: Any, private_values: Sequence[str]) -> bool:
    if isinstance(value, str):
        return public_string_is_safe(value, private_values)
    if isinstance(value, list):
        return all(public_value_is_safe(item, private_values) for item in value)
    if isinstance(value, dict):
        return all(
            isinstance(key, str)
            and public_string_is_safe(key, private_values)
            and public_value_is_safe(item, private_values)
            for key, item in value.items()
        )
    return value is None or isinstance(value, (bool, int, float))


def checked_result_artifact(run_root: Path, raw_path: object) -> Path:
    if not isinstance(raw_path, str):
        raise PublicEvidenceError("public_artifact_path_invalid")
    relative = PurePosixPath(raw_path)
    if relative.is_absolute() or ".." in relative.parts or str(relative) != raw_path:
        raise PublicEvidenceError("public_artifact_path_invalid")
    unresolved = run_root / Path(*relative.parts)
    details = unresolved.lstat()
    if not stat.S_ISREG(details.st_mode) or stat.S_ISLNK(details.st_mode):
        raise PublicEvidenceError("public_artifact_type_invalid")
    resolved = unresolved.resolve(strict=True)
    if not resolved.is_relative_to(run_root.resolve(strict=True)):
        raise PublicEvidenceError("public_artifact_path_invalid")
    return resolved


def public_platform(environment: Mapping[str, Any]) -> dict[str, Any]:
    tools = environment.get("tools", {})
    selected_tools = {
        name: value
        for name, value in tools.items()
        if name in {"compiler", "cuda_toolkit", "ffmpeg", "node", "tshark"}
        and isinstance(value, dict)
    }
    gpus = environment.get("gpus", [])
    return {
        "os": environment.get("os", {}),
        "kernel": environment.get("kernel", "unreported"),
        "architecture": environment.get("architecture", "unreported"),
        "gpu_models": sorted(
            {str(gpu.get("model", "unreported")) for gpu in gpus if isinstance(gpu, dict)}
        ),
        "driver_versions": sorted(
            {str(gpu.get("driver_version", "unreported")) for gpu in gpus if isinstance(gpu, dict)}
        ),
        "tools": selected_tools,
    }


def copy_public_artifacts(
    run_root: Path,
    public_root: Path,
    results: Mapping[str, Mapping[str, Any]],
    private_values: Sequence[str],
) -> list[dict[str, str]]:
    copied: list[dict[str, str]] = []
    for phase, result in sorted(results.items()):
        if result.get("status") != "PASSED":
            continue
        output_hashes = result.get("output_hashes", {})
        if not isinstance(output_hashes, dict):
            raise PublicEvidenceError("public_phase_outputs_invalid")
        for raw_path in sorted(output_hashes):
            if PurePosixPath(raw_path).name not in PUBLIC_ARTIFACT_NAMES:
                continue
            source = checked_result_artifact(run_root, raw_path)
            expected_hash = output_hashes[raw_path]
            if not isinstance(expected_hash, str) or sha256_file(source) != expected_hash:
                raise PublicEvidenceError("public_artifact_hash_mismatch")
            try:
                value = json.loads(source.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
                raise PublicEvidenceError("public_artifact_json_invalid") from error
            if not public_value_is_safe(value, private_values):
                raise PublicEvidenceError("public_artifact_contains_private_value")
            relative = Path("evidence") / phase / source.name
            destination = public_root / relative
            exclusive_write(destination, canonical_json(value))
            copied.append({"path": relative.as_posix(), "sha256": sha256_file(destination)})
    return copied


def create_public_evidence(
    run_root: Path,
    bundle_id: str,
    source_commit: str,
    status: str,
    environment: Mapping[str, Any],
    results: Mapping[str, Mapping[str, Any]],
    private_values: Sequence[str],
) -> Path:
    target_root = run_root / "public-evidence"
    public_root = Path(tempfile.mkdtemp(prefix=".public-evidence.", dir=run_root))
    os.chmod(public_root, 0o700)
    try:
        artifacts = copy_public_artifacts(
            run_root,
            public_root,
            results,
            private_values,
        )
        phases = [
            {
                "id": identifier,
                "status": result.get("status"),
                "reason": result.get("reason"),
                "duration_seconds": result.get("duration_seconds"),
            }
            for identifier, result in sorted(results.items())
        ]
        summary = {
            "schema_version": 1,
            "publication_review_required": True,
            "status": status,
            "bundle_id": bundle_id,
            "source_commit": source_commit,
            "platform": public_platform(environment),
            "phases": phases,
            "evidence_files": artifacts,
            "claim_consequence": (
                "eligible_for_milestone_review_after_publication_review"
                if status == "PASSED"
                else "no_hardware_performance_interoperability_or_release_claim_is_accepted"
            ),
        }
        if not public_value_is_safe(summary, private_values):
            raise PublicEvidenceError("public_summary_contains_private_value")
        exclusive_write(public_root / "summary.json", canonical_json(summary))
        report_lines = [
            "# GlyphRelay public evidence candidate",
            "",
            f"Status: `{status}`",
            "",
            f"Source commit: `{source_commit}`",
            "",
            "Publication review is required before any file in this directory is committed "
            "or published.",
            "",
            "## Phase results",
            "",
            *(f"- `{phase['id']}`: `{phase['status']}` ({phase['reason']})" for phase in phases),
            "",
            "## Claim consequence",
            "",
            str(summary["claim_consequence"]),
            "",
        ]
        exclusive_write(public_root / "REPORT.md", "\n".join(report_lines).encode())
        review_lines = [
            "# Publication review required",
            "",
            "This export has passed automated allowlisting and private-value rejection.",
            "",
            "A human must still compare every proposed public claim with the measured evidence "
            "before publication.",
            "",
            "Do not publish raw phase logs, packet captures, elementary streams, machine "
            "identifiers, user paths, credentials, or private addresses.",
            "",
        ]
        exclusive_write(
            public_root / "PUBLICATION_REVIEW_REQUIRED.md",
            "\n".join(review_lines).encode(),
        )
        checksum_lines = [
            f"{sha256_file(path)}  {path.relative_to(public_root).as_posix()}"
            for path in sorted(public_root.rglob("*"))
            if path.is_file() and not path.is_symlink() and path.name != "SHA256SUMS"
        ]
        exclusive_write(public_root / "SHA256SUMS", ("\n".join(checksum_lines) + "\n").encode())
        for directory, _, _ in os.walk(public_root, topdown=False):
            fsync_directory(Path(directory))
        if target_root.exists() or target_root.is_symlink():
            if target_root.is_symlink() or not target_root.is_dir():
                raise PublicEvidenceError("public_evidence_existing_type_invalid")
            for path in target_root.rglob("*"):
                mode = path.lstat().st_mode
                if not stat.S_ISREG(mode) and not stat.S_ISDIR(mode):
                    raise PublicEvidenceError("public_evidence_existing_type_invalid")
            existing = {
                path.relative_to(target_root).as_posix(): sha256_file(path)
                for path in target_root.rglob("*")
                if path.is_file() and not path.is_symlink()
            }
            replacement = {
                path.relative_to(public_root).as_posix(): sha256_file(path)
                for path in public_root.rglob("*")
                if path.is_file() and not path.is_symlink()
            }
            if existing != replacement:
                raise PublicEvidenceError("public_evidence_existing_content_mismatch")
            for path in sorted(
                public_root.rglob("*"), key=lambda item: len(item.parts), reverse=True
            ):
                if path.is_file() or path.is_symlink():
                    path.unlink()
                elif path.is_dir():
                    path.rmdir()
            public_root.rmdir()
            return target_root
        os.replace(public_root, target_root)
        fsync_directory(run_root)
        return target_root
    except Exception:
        if public_root.exists() and not public_root.is_symlink():
            for path in sorted(
                public_root.rglob("*"), key=lambda item: len(item.parts), reverse=True
            ):
                if path.is_file() or path.is_symlink():
                    path.unlink()
                elif path.is_dir():
                    path.rmdir()
            public_root.rmdir()
        raise
