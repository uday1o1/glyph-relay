from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import stat
import subprocess
import sys
import tarfile
import tempfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path, PurePosixPath
from typing import Any

SCHEMA_VERSION = 1
MANIFEST_MEMBER = "SOURCE_MANIFEST.json"
MAXIMUM_SOURCE_FILE_BYTES = 64 * 1024 * 1024
ALLOWED_FILE_MODES = {"100644", "100755", "120000"}
GENERATED_SOURCE_ROOTS = {".deps", ".venv", "artifacts", "build", "node_modules", "out"}


class BundleError(RuntimeError):
    """Raised when a source bundle cannot be trusted."""


@dataclass(frozen=True)
class BundlePaths:
    bundle_id: str
    archive: Path
    manifest: Path
    metadata: Path


def _run_git(root: Path, arguments: Sequence[str], *, binary: bool = False) -> str | bytes:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=True,
        capture_output=True,
    )
    return completed.stdout if binary else completed.stdout.decode("utf-8", "strict").strip()


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical_json(value: Mapping[str, Any]) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"
    ).encode()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_relative_path(raw_path: str) -> str:
    if not raw_path or "\x00" in raw_path or any(ord(character) < 32 for character in raw_path):
        raise BundleError("source_path_invalid")
    path = PurePosixPath(raw_path)
    if path.is_absolute() or any(component in {"", ".", ".."} for component in path.parts):
        raise BundleError("source_path_invalid")
    normalized = str(path)
    if normalized != raw_path:
        raise BundleError("source_path_not_canonical")
    return normalized


def _validate_symlink(path: str, target: str) -> None:
    if not target or "\x00" in target or any(ord(character) < 32 for character in target):
        raise BundleError("source_symlink_target_invalid")
    if PurePosixPath(target).is_absolute():
        raise BundleError("source_symlink_target_absolute")
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(path), target))
    if resolved == ".." or resolved.startswith("../") or resolved.startswith("/"):
        raise BundleError("source_symlink_escapes_root")


def _indexed_entries(repository: Path) -> list[tuple[str, str, str]]:
    raw = _run_git(repository, ["ls-files", "--stage", "-z"], binary=True)
    assert isinstance(raw, bytes)
    entries: list[tuple[str, str, str]] = []
    for record in raw.split(b"\0"):
        if not record:
            continue
        metadata, separator, raw_path = record.partition(b"\t")
        if not separator:
            raise BundleError("git_index_record_invalid")
        fields = metadata.decode("ascii", "strict").split(" ")
        if len(fields) != 3 or fields[2] != "0":
            raise BundleError("git_index_stage_invalid")
        path = raw_path.decode("utf-8", "strict")
        entries.append((fields[0], fields[1], _validate_relative_path(path)))
    return entries


def _blob(repository: Path, object_id: str) -> bytes:
    raw = _run_git(repository, ["cat-file", "blob", object_id], binary=True)
    assert isinstance(raw, bytes)
    if len(raw) > MAXIMUM_SOURCE_FILE_BYTES:
        raise BundleError("source_file_too_large")
    return raw


def _collect_repository(
    repository: Path,
    prefix: str,
    files: list[dict[str, Any]],
    contents: dict[str, bytes],
    submodules: list[dict[str, str]],
) -> None:
    for mode, object_id, local_path in _indexed_entries(repository):
        path = _validate_relative_path(posixpath.join(prefix, local_path) if prefix else local_path)
        if mode == "160000":
            submodule_root = repository / local_path
            if not submodule_root.is_dir():
                raise BundleError(f"submodule_missing:{path}")
            commit = str(_run_git(submodule_root, ["rev-parse", "HEAD"])).strip()
            if commit != object_id:
                raise BundleError(f"submodule_commit_mismatch:{path}")
            if _run_git(submodule_root, ["status", "--porcelain", "--untracked-files=all"]):
                raise BundleError(f"submodule_dirty:{path}")
            submodules.append({"path": path, "commit": commit})
            _collect_repository(submodule_root, path, files, contents, submodules)
            continue
        if mode not in ALLOWED_FILE_MODES:
            raise BundleError(f"source_mode_unsupported:{path}:{mode}")
        data = _blob(repository, object_id)
        entry: dict[str, Any] = {
            "path": path,
            "mode": mode,
            "bytes": len(data),
            "sha256": _sha256(data),
        }
        if mode == "120000":
            target = data.decode("utf-8", "strict")
            _validate_symlink(path, target)
            entry["symlink_target"] = target
        files.append(entry)
        contents[path] = data


def _reject_symlink_ancestors(files: Sequence[Mapping[str, Any]]) -> None:
    symlinks = {str(entry["path"]) for entry in files if entry["mode"] == "120000"}
    for entry in files:
        parts = PurePosixPath(str(entry["path"])).parts
        ancestors = {str(PurePosixPath(*parts[:index])) for index in range(1, len(parts))}
        if ancestors & symlinks:
            raise BundleError(f"source_path_below_symlink:{entry['path']}")


def _lock_summary(root: Path, contents: Mapping[str, bytes]) -> dict[str, Any]:
    lock_names = ["dependencies.lock.json", "pnpm-lock.yaml", "uv.lock"]
    locks = {name: _sha256(contents[name]) for name in lock_names if name in contents}
    dependency_lock = json.loads(contents.get("dependencies.lock.json", b"{}").decode())
    image_digests: dict[str, str] = {}
    if isinstance(dependency_lock, dict):
        for name in ("coturn", "linux_cpu_container"):
            value = dependency_lock.get(name)
            if isinstance(value, dict):
                for key, item in value.items():
                    if isinstance(item, str) and "sha256:" in item:
                        image_digests[f"{name}.{key}"] = item
    script_paths = [
        "scripts/gpu/qualify_cuda_pm.sh",
        "scripts/gpu/run_remote_qualification.sh",
        "tools/gpu/source_bundle.py",
        "tools/gpu/remote_qualification.py",
    ]
    scripts = {path: _sha256(contents[path]) for path in script_paths if path in contents}
    return {"lockfiles": locks, "image_digests": image_digests, "qualification_scripts": scripts}


def _prepare_output_directory(repository_root: Path, output_directory: Path) -> Path:
    build_root = repository_root / "build"
    build_root.mkdir(mode=0o700, exist_ok=True)
    build_details = build_root.lstat()
    if (
        not stat.S_ISDIR(build_details.st_mode)
        or stat.S_ISLNK(build_details.st_mode)
        or build_details.st_uid != os.getuid()
    ):
        raise BundleError("repository_build_root_invalid")
    output = output_directory.absolute()
    if output == build_root or not output.is_relative_to(build_root):
        raise BundleError("bundle_output_must_be_below_repository_build")
    current = build_root
    for component in output.relative_to(build_root).parts:
        current /= component
        if current.exists() or current.is_symlink():
            details = current.lstat()
            if not stat.S_ISDIR(details.st_mode) or stat.S_ISLNK(details.st_mode):
                raise BundleError("bundle_output_component_invalid")
        else:
            current.mkdir(mode=0o700)
        if current.lstat().st_uid != os.getuid():
            raise BundleError("bundle_output_owner_invalid")
        os.chmod(current, 0o700)
    return output.resolve(strict=True)


def build_source_bundle(repository_root: Path, output_directory: Path) -> BundlePaths:
    root = repository_root.resolve(strict=True)
    if _run_git(root, ["status", "--porcelain", "--untracked-files=all"]):
        raise BundleError("source_worktree_dirty")
    commit = str(_run_git(root, ["rev-parse", "HEAD"])).strip()
    files: list[dict[str, Any]] = []
    contents: dict[str, bytes] = {}
    submodules: list[dict[str, str]] = []
    _collect_repository(root, "", files, contents, submodules)
    files.sort(key=lambda entry: str(entry["path"]))
    submodules.sort(key=lambda entry: entry["path"])
    _reject_symlink_ancestors(files)
    payload: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "git_commit": commit,
        "submodules": submodules,
        "dependencies": _lock_summary(root, contents),
        "files": files,
    }
    bundle_id = _sha256(_canonical_json(payload))
    manifest = {**payload, "bundle_id": bundle_id}
    manifest_bytes = _canonical_json(manifest)

    output_directory = _prepare_output_directory(root, output_directory)
    archive_path = output_directory / f"glyphrelay-source-{bundle_id}.tar"
    manifest_path = output_directory / f"glyphrelay-source-{bundle_id}.manifest.json"
    metadata_path = output_directory / f"glyphrelay-source-{bundle_id}.metadata.json"
    temporary_descriptor, temporary_name = tempfile.mkstemp(prefix=".bundle-", dir=output_directory)
    os.close(temporary_descriptor)
    temporary_path = Path(temporary_name)
    try:
        with tarfile.open(temporary_path, "w", format=tarfile.PAX_FORMAT) as archive:
            for entry in files:
                path = str(entry["path"])
                information = tarfile.TarInfo(path)
                information.mtime = 0
                information.uid = 0
                information.gid = 0
                information.uname = ""
                information.gname = ""
                information.mode = 0o755 if entry["mode"] == "100755" else 0o644
                if entry["mode"] == "120000":
                    information.type = tarfile.SYMTYPE
                    information.mode = 0o777
                    information.linkname = str(entry["symlink_target"])
                    archive.addfile(information)
                else:
                    information.size = len(contents[path])
                    archive.addfile(information, BytesIO(contents[path]))
            information = tarfile.TarInfo(MANIFEST_MEMBER)
            information.mtime = 0
            information.uid = 0
            information.gid = 0
            information.uname = ""
            information.gname = ""
            information.mode = 0o644
            information.size = len(manifest_bytes)
            archive.addfile(information, BytesIO(manifest_bytes))
        archive_sha256 = _sha256_file(temporary_path)
        metadata = {
            "schema_version": SCHEMA_VERSION,
            "bundle_id": bundle_id,
            "archive_name": archive_path.name,
            "archive_sha256": archive_sha256,
            "archive_bytes": temporary_path.stat().st_size,
            "manifest_name": manifest_path.name,
            "manifest_sha256": _sha256(manifest_bytes),
        }
        metadata_bytes = _canonical_json(metadata)
        existing = [archive_path.exists(), manifest_path.exists(), metadata_path.exists()]
        if any(existing):
            if not all(existing):
                raise BundleError("bundle_output_incomplete_conflict")
            if (
                _sha256_file(archive_path) != archive_sha256
                or manifest_path.read_bytes() != manifest_bytes
                or metadata_path.read_bytes() != metadata_bytes
            ):
                raise BundleError("bundle_output_content_conflict")
            return BundlePaths(bundle_id, archive_path, manifest_path, metadata_path)
        _atomic_write(manifest_path, manifest_bytes, 0o600)
        _atomic_write(metadata_path, metadata_bytes, 0o600)
        os.replace(temporary_path, archive_path)
        os.chmod(archive_path, 0o600)
        _fsync_directory(output_directory)
    finally:
        temporary_path.unlink(missing_ok=True)
    if _run_git(root, ["status", "--porcelain", "--untracked-files=all"]):
        raise BundleError("source_worktree_changed_during_bundle")
    return BundlePaths(bundle_id, archive_path, manifest_path, metadata_path)


def _atomic_write(path: Path, content: bytes, mode: int) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        _fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def load_manifest(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or value.get("schema_version") != SCHEMA_VERSION:
        raise BundleError("source_manifest_invalid")
    payload = {key: item for key, item in value.items() if key != "bundle_id"}
    if value.get("bundle_id") != _sha256(_canonical_json(payload)):
        raise BundleError("source_manifest_identity_mismatch")
    return value


def verify_bundle_metadata(archive: Path, manifest: Path, metadata_path: Path) -> dict[str, Any]:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if not isinstance(metadata, dict) or metadata.get("schema_version") != SCHEMA_VERSION:
        raise BundleError("bundle_metadata_invalid")
    parsed_manifest = load_manifest(manifest)
    checks = {
        "bundle_id": parsed_manifest["bundle_id"],
        "archive_name": archive.name,
        "archive_sha256": _sha256_file(archive),
        "archive_bytes": archive.stat().st_size,
        "manifest_name": manifest.name,
        "manifest_sha256": _sha256(manifest.read_bytes()),
    }
    for key, expected in checks.items():
        if metadata.get(key) != expected:
            raise BundleError(f"bundle_metadata_mismatch:{key}")
    return parsed_manifest


def safe_extract_bundle(archive: Path, manifest_path: Path, destination: Path) -> None:
    manifest = load_manifest(manifest_path)
    expected = {entry["path"]: entry for entry in manifest["files"]}
    if destination.exists():
        raise BundleError("bundle_destination_exists")
    destination.mkdir(mode=0o700, parents=False)
    try:
        with tarfile.open(archive, "r:") as source:
            members = source.getmembers()
            names = [member.name for member in members]
            if len(names) != len(set(names)) or set(names) != set(expected) | {MANIFEST_MEMBER}:
                raise BundleError("bundle_member_set_mismatch")
            for member in members:
                _validate_relative_path(member.name)
                if member.name == MANIFEST_MEMBER:
                    if not member.isfile():
                        raise BundleError("bundle_manifest_member_invalid")
                    extracted = source.extractfile(member)
                    if extracted is None or extracted.read() != manifest_path.read_bytes():
                        raise BundleError("bundle_embedded_manifest_mismatch")
                    continue
                entry = expected[member.name]
                if member.issym():
                    if entry["mode"] != "120000" or member.linkname != entry["symlink_target"]:
                        raise BundleError("bundle_symlink_mismatch")
                    _validate_symlink(member.name, member.linkname)
                elif not member.isfile() or entry["mode"] not in {"100644", "100755"}:
                    raise BundleError("bundle_member_type_mismatch")

            for member in members:
                target = destination / member.name
                target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                if member.name == MANIFEST_MEMBER:
                    _write_extracted_file(
                        source,
                        member,
                        target,
                        0o600,
                        _sha256(manifest_path.read_bytes()),
                    )
                elif member.issym():
                    os.symlink(member.linkname, target)
                else:
                    entry = expected[member.name]
                    mode = 0o755 if entry["mode"] == "100755" else 0o644
                    _write_extracted_file(source, member, target, mode, str(entry["sha256"]))
        validate_extracted_source(destination, manifest_path)
        for directory, _, _ in os.walk(destination, topdown=False):
            _fsync_directory(Path(directory))
        _fsync_directory(destination.parent)
    except Exception:
        _remove_failed_destination(destination)
        raise


def _write_extracted_file(
    archive: tarfile.TarFile,
    member: tarfile.TarInfo,
    target: Path,
    mode: int,
    expected_sha256: str,
) -> None:
    source = archive.extractfile(member)
    if source is None:
        raise BundleError("bundle_regular_file_missing")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(target, flags, mode)
    digest = hashlib.sha256()
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as destination:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
                destination.write(chunk)
            destination.flush()
            os.fsync(destination.fileno())
    except Exception:
        target.unlink(missing_ok=True)
        raise
    if digest.hexdigest() != expected_sha256:
        target.unlink(missing_ok=True)
        raise BundleError(f"bundle_file_hash_mismatch:{member.name}")


def validate_extracted_source(root: Path, manifest_path: Path) -> None:
    manifest = load_manifest(manifest_path)
    expected = {entry["path"]: entry for entry in manifest["files"]}
    for name in GENERATED_SOURCE_ROOTS:
        generated = root / name
        if generated.exists() or generated.is_symlink():
            details = generated.lstat()
            if not stat.S_ISDIR(details.st_mode) or stat.S_ISLNK(details.st_mode):
                raise BundleError(f"generated_source_root_invalid:{name}")
    actual: set[str] = set()
    for directory, directory_names, file_names in os.walk(root, followlinks=False):
        parent = Path(directory)
        if parent == root:
            directory_names[:] = [
                name for name in directory_names if name not in GENERATED_SOURCE_ROOTS
            ]
        for name in tuple(directory_names):
            path = parent / name
            if path.is_symlink():
                actual.add(path.relative_to(root).as_posix())
                directory_names.remove(name)
        for name in file_names:
            actual.add((parent / name).relative_to(root).as_posix())
    if actual != set(expected) | {MANIFEST_MEMBER}:
        raise BundleError("extracted_source_member_set_mismatch")
    embedded = root / MANIFEST_MEMBER
    if not embedded.is_file() or embedded.read_bytes() != manifest_path.read_bytes():
        raise BundleError("extracted_source_manifest_mismatch")
    for path, entry in expected.items():
        target = root / path
        details = target.lstat()
        if entry["mode"] == "120000":
            if not stat.S_ISLNK(details.st_mode) or os.readlink(target) != entry["symlink_target"]:
                raise BundleError(f"extracted_source_symlink_mismatch:{path}")
        else:
            if not stat.S_ISREG(details.st_mode):
                raise BundleError(f"extracted_source_type_mismatch:{path}")
            if _sha256(target.read_bytes()) != entry["sha256"]:
                raise BundleError(f"extracted_source_hash_mismatch:{path}")
            expected_mode = 0o755 if entry["mode"] == "100755" else 0o644
            if stat.S_IMODE(details.st_mode) != expected_mode:
                raise BundleError(f"extracted_source_mode_mismatch:{path}")


def _remove_failed_destination(root: Path) -> None:
    if not root.exists() or root.is_symlink():
        return
    for path in sorted(root.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            path.rmdir()
    root.rmdir()


def verify_result_archive(archive: Path, checksum_path: Path, destination: Path) -> None:
    checksum_fields = checksum_path.read_text(encoding="utf-8").strip().split("  ")
    if (
        len(checksum_fields) != 2
        or checksum_fields[1] != archive.name
        or checksum_fields[0] != _sha256_file(archive)
    ):
        raise BundleError("result_archive_checksum_mismatch")
    if destination.exists():
        raise BundleError("result_destination_exists")
    destination.mkdir(mode=0o700, parents=False)
    try:
        with tarfile.open(archive, "r:") as source:
            members = source.getmembers()
            names = [_validate_relative_path(member.name) for member in members]
            if len(names) != len(set(names)):
                raise BundleError("result_archive_member_duplicate")
            for member in members:
                if member.isdir():
                    continue
                if not member.isfile() or member.size > 2 * 1024 * 1024 * 1024:
                    raise BundleError("result_archive_member_invalid")
            for member in members:
                target = destination / member.name
                if member.isdir():
                    target.mkdir(mode=0o700, parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                _write_extracted_file(source, member, target, 0o600, _member_sha256(source, member))
        verify_result_checksums(destination)
        _fsync_directory(destination)
        _fsync_directory(destination.parent)
    except Exception:
        _remove_failed_destination(destination)
        raise


def _member_sha256(archive: tarfile.TarFile, member: tarfile.TarInfo) -> str:
    stream = archive.extractfile(member)
    if stream is None:
        raise BundleError("result_archive_member_missing")
    digest = hashlib.sha256()
    while chunk := stream.read(1024 * 1024):
        digest.update(chunk)
    return digest.hexdigest()


def verify_result_checksums(root: Path) -> None:
    checksum_path = root / "SHA256SUMS"
    if not checksum_path.is_file() or checksum_path.is_symlink():
        raise BundleError("result_checksums_missing")
    declared: set[str] = set()
    for line in checksum_path.read_text(encoding="utf-8").splitlines():
        digest, separator, raw_path = line.partition("  ")
        path = _validate_relative_path(raw_path)
        if (
            not separator
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise BundleError("result_checksum_record_invalid")
        if path == "SHA256SUMS" or path in declared:
            raise BundleError("result_checksum_path_invalid")
        declared.add(path)
        target = root / path
        if not target.is_file() or target.is_symlink() or _sha256_file(target) != digest:
            raise BundleError(f"result_file_hash_mismatch:{path}")
    actual = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and not path.is_symlink() and path.name != "SHA256SUMS"
    }
    if actual != declared:
        raise BundleError("result_checksum_member_set_mismatch")


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build and verify GlyphRelay source bundles")
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build")
    build.add_argument("--repository", required=True)
    build.add_argument("--output-directory", required=True)
    verify = commands.add_parser("verify-metadata")
    verify.add_argument("--archive", required=True)
    verify.add_argument("--manifest", required=True)
    verify.add_argument("--metadata", required=True)
    extract = commands.add_parser("safe-extract")
    extract.add_argument("--archive", required=True)
    extract.add_argument("--manifest", required=True)
    extract.add_argument("--metadata", required=True)
    extract.add_argument("--destination", required=True)
    validate = commands.add_parser("validate-source")
    validate.add_argument("--root", required=True)
    validate.add_argument("--manifest", required=True)
    results = commands.add_parser("verify-results")
    results.add_argument("--archive", required=True)
    results.add_argument("--checksum", required=True)
    results.add_argument("--destination", required=True)
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    parsed = _argument_parser().parse_args(arguments)
    try:
        if parsed.command == "build":
            result = build_source_bundle(Path(parsed.repository), Path(parsed.output_directory))
            print(
                json.dumps(
                    {
                        "bundle_id": result.bundle_id,
                        "archive": str(result.archive),
                        "manifest": str(result.manifest),
                        "metadata": str(result.metadata),
                    },
                    sort_keys=True,
                )
            )
        elif parsed.command == "verify-metadata":
            manifest = verify_bundle_metadata(
                Path(parsed.archive), Path(parsed.manifest), Path(parsed.metadata)
            )
            print(json.dumps({"bundle_id": manifest["bundle_id"], "verified": True}))
        elif parsed.command == "safe-extract":
            verify_bundle_metadata(
                Path(parsed.archive), Path(parsed.manifest), Path(parsed.metadata)
            )
            safe_extract_bundle(
                Path(parsed.archive), Path(parsed.manifest), Path(parsed.destination)
            )
            print(json.dumps({"destination": parsed.destination, "verified": True}))
        elif parsed.command == "validate-source":
            validate_extracted_source(Path(parsed.root), Path(parsed.manifest))
            print(json.dumps({"root": parsed.root, "verified": True}))
        else:
            verify_result_archive(
                Path(parsed.archive), Path(parsed.checksum), Path(parsed.destination)
            )
            print(json.dumps({"destination": parsed.destination, "verified": True}))
        return 0
    except (BundleError, OSError, ValueError, json.JSONDecodeError, tarfile.TarError) as error:
        print(f"bundle operation failed: {type(error).__name__}", file=sys.stderr)
        return 5


if __name__ == "__main__":
    raise SystemExit(main())
