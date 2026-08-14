from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator

from tools.gpu.source_bundle import (
    BundleError,
    build_source_bundle,
    safe_extract_bundle,
    validate_extracted_source,
    verify_bundle_metadata,
    verify_result_checksums,
)


def git(repository: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def repository(tmp_path: Path) -> Path:
    root = tmp_path / "source"
    root.mkdir()
    git(root, "init", "--quiet")
    git(root, "config", "user.name", "Bundle Test")
    git(root, "config", "user.email", "bundle-test@example.invalid")
    (root / ".gitignore").write_text("/build/\n", encoding="utf-8")
    (root / "README.md").write_text("# Fixture\n", encoding="utf-8")
    script = root / "run.sh"
    script.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    script.chmod(0o755)
    git(root, "add", ".")
    git(root, "commit", "--quiet", "-m", "fixture")
    return root


def test_bundle_is_deterministic_and_extracts_only_tracked_files(tmp_path: Path) -> None:
    root = repository(tmp_path)
    output = root / "build" / "bundles"
    ignored = root / "build" / "private.log"
    ignored.parent.mkdir()
    ignored.write_text("not source\n", encoding="utf-8")

    first = build_source_bundle(root, output)
    first_archive = first.archive.read_bytes()
    second = build_source_bundle(root, output)
    assert first.bundle_id == second.bundle_id
    assert first_archive == second.archive.read_bytes()
    manifest = verify_bundle_metadata(second.archive, second.manifest, second.metadata)
    Draft202012Validator(
        json.loads(Path("schemas/source-manifest-v1.schema.json").read_text(encoding="utf-8"))
    ).validate(manifest)
    Draft202012Validator(
        json.loads(
            Path("schemas/source-bundle-metadata-v1.schema.json").read_text(encoding="utf-8")
        )
    ).validate(json.loads(second.metadata.read_text(encoding="utf-8")))
    assert manifest["git_commit"] == git(root, "rev-parse", "HEAD")
    assert {entry["path"] for entry in manifest["files"]} == {
        ".gitignore",
        "README.md",
        "run.sh",
    }

    extracted = tmp_path / "extracted"
    safe_extract_bundle(second.archive, second.manifest, extracted)
    validate_extracted_source(extracted, second.manifest)
    assert (extracted / "README.md").read_text(encoding="utf-8") == "# Fixture\n"
    assert os.access(extracted / "run.sh", os.X_OK)
    assert not (extracted / "build").exists()


def test_bundle_refuses_dirty_source_and_escaping_symlink(tmp_path: Path) -> None:
    root = repository(tmp_path)
    (root / "untracked.txt").write_text("dirty\n", encoding="utf-8")
    with pytest.raises(BundleError, match="source_worktree_dirty"):
        build_source_bundle(root, root / "build")
    (root / "untracked.txt").unlink()

    (root / "escape").symlink_to("../outside")
    git(root, "add", "escape")
    git(root, "commit", "--quiet", "-m", "escaping link")
    with pytest.raises(BundleError, match="source_symlink_escapes_root"):
        build_source_bundle(root, root / "build")


def test_bundle_hashes_and_destination_are_fail_closed(tmp_path: Path) -> None:
    root = repository(tmp_path)
    bundle = build_source_bundle(root, root / "build" / "bundles")
    tampered_directory = tmp_path / "tampered"
    tampered_directory.mkdir()
    tampered = tampered_directory / bundle.archive.name
    content = bytearray(bundle.archive.read_bytes())
    content[len(content) // 2] ^= 1
    tampered.write_bytes(content)
    with pytest.raises(BundleError, match="bundle_metadata_mismatch:archive_sha256"):
        verify_bundle_metadata(tampered, bundle.manifest, bundle.metadata)

    destination = tmp_path / "existing"
    destination.mkdir()
    with pytest.raises(BundleError, match="bundle_destination_exists"):
        safe_extract_bundle(bundle.archive, bundle.manifest, destination)


def test_result_verification_rejects_unmanifested_symlink(tmp_path: Path) -> None:
    result = tmp_path / "result"
    result.mkdir()
    artifact = result / "status.json"
    artifact.write_text("{}\n", encoding="utf-8")
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    (result / "SHA256SUMS").write_text(f"{digest}  status.json\n", encoding="utf-8")
    (result / "escape").symlink_to("status.json")
    with pytest.raises(BundleError, match="result_member_type_invalid"):
        verify_result_checksums(result)
