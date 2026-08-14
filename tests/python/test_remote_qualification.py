from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path

import pytest

from tools.gpu import handoff
from tools.gpu.handoff import (
    HandoffError,
    complete_run_record,
    qualification_run_record,
    validate_namespace,
)
from tools.gpu.remote_qualification import (
    QualificationError,
    RunContext,
    acquire_lock,
    execute,
    load_phases,
    phase_input_identity,
    release_lock,
    reusable_phase,
    validate_context,
)
from tools.gpu.source_bundle import (
    build_source_bundle,
    safe_extract_bundle,
    verify_result_archive,
)
from tools.validate_qualification import validate_qualification


def git(repository: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def extracted_source(tmp_path: Path) -> tuple[Path, Path, str]:
    repository = tmp_path / "repository"
    repository.mkdir()
    git(repository, "init", "--quiet")
    git(repository, "config", "user.name", "Qualification Test")
    git(repository, "config", "user.email", "qualification-test@example.invalid")
    (repository / ".gitignore").write_text("/build/\n", encoding="utf-8")
    phases = repository / "qualification" / "test-phases.json"
    phases.parent.mkdir()
    phases.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "phases": [
                    {
                        "id": "passing",
                        "title": "Passing phase",
                        "kind": "command",
                        "required": True,
                        "dependencies": [],
                        "commands": [
                            [
                                "python3",
                                "-c",
                                "print('token=synthetic http://user:pass@10.0.0.1')",
                            ]
                        ],
                        "environment_names": [],
                    },
                    {
                        "id": "failing",
                        "title": "Failing phase",
                        "kind": "command",
                        "required": True,
                        "dependencies": ["passing"],
                        "commands": [["python3", "-c", "raise SystemExit(7)"]],
                        "environment_names": [],
                    },
                    {
                        "id": "dependent",
                        "title": "Blocked dependent phase",
                        "kind": "command",
                        "required": True,
                        "dependencies": ["failing"],
                        "commands": [["python3", "-c", "raise SystemExit(0)"]],
                        "environment_names": [],
                    },
                    {
                        "id": "independent",
                        "title": "Independent phase after failure",
                        "kind": "command",
                        "required": True,
                        "dependencies": [],
                        "commands": [["python3", "-c", "print('still ran')"]],
                        "environment_names": [],
                    },
                ],
            }
        )
        + "\n",
        encoding="utf-8",
    )
    git(repository, "add", ".")
    git(repository, "commit", "--quiet", "-m", "qualification fixture")
    bundle = build_source_bundle(repository, repository / "build" / "bundles")
    namespace = tmp_path / "namespace"
    namespace.mkdir(mode=0o700)
    sources = namespace / "sources"
    sources.mkdir(mode=0o700)
    source = sources / bundle.bundle_id
    safe_extract_bundle(bundle.archive, bundle.manifest, source)
    return source, namespace, bundle.bundle_id


def context_for(source: Path, namespace: Path, bundle_id: str) -> RunContext:
    previous = Path.cwd()
    os.chdir(source)
    try:
        return validate_context(
            argparse.Namespace(namespace=str(namespace), bundle_id=bundle_id, run_id="test-run")
        )
    finally:
        os.chdir(previous)


def test_runner_continues_independent_phases_and_exports_complete_failure(
    tmp_path: Path,
) -> None:
    source, namespace, bundle_id = extracted_source(tmp_path)
    context = context_for(source, namespace, bundle_id)
    previous = Path.cwd()
    os.chdir(source)
    try:
        status = execute(context, source / "qualification/test-phases.json")
    finally:
        os.chdir(previous)
    assert status == "FAILED"
    result = json.loads((context.run_root / "status.json").read_text(encoding="utf-8"))
    assert result["phases"] == {
        "dependent": "BLOCKED",
        "failing": "FAILED",
        "independent": "PASSED",
        "passing": "PASSED",
    }
    assert (context.run_root / "REPORT.md").is_file()
    assert (context.run_root / "commands.jsonl").is_file()
    assert (context.run_root / "junit.xml").is_file()
    passing_logs = list(context.run_root.glob("phases/passing/*/stdout.log"))
    assert len(passing_logs) == 1
    redacted_log = passing_logs[0].read_text(encoding="utf-8")
    assert "synthetic" not in redacted_log
    assert "user:pass" not in redacted_log
    assert "10.0.0.1" not in redacted_log

    passing_phase = load_phases(source / "qualification/test-phases.json")[0]
    stable_commands = [tuple(command) for command in passing_phase.commands]
    identity = phase_input_identity(passing_phase, context, stable_commands)
    assert reusable_phase(passing_phase, context, identity) is not None
    state = json.loads((context.run_root / "phases/passing/state.json").read_text(encoding="utf-8"))
    result_path = context.run_root / state["result_path"]
    original_result = result_path.read_bytes()
    result_path.write_text("{}\n", encoding="utf-8")
    assert reusable_phase(passing_phase, context, identity) is None
    result_path.write_bytes(original_result)
    assert reusable_phase(passing_phase, context, identity) is not None
    passing_logs[0].write_text("tampered\n", encoding="utf-8")
    assert reusable_phase(passing_phase, context, identity) is None
    archive = namespace / "exports/test-run.tar"
    checksum = namespace / "exports/test-run.tar.sha256"
    extracted = tmp_path / "verified-result"
    verify_result_archive(archive, checksum, extracted)
    assert json.loads((extracted / "status.json").read_text())["status"] == "FAILED"
    validate_qualification(extracted, Path("schemas").resolve())


def test_bundle_and_gpu_locks_refuse_live_owners_and_record_stale_reclamation(
    tmp_path: Path,
) -> None:
    source, namespace, bundle_id = extracted_source(tmp_path)
    first = context_for(source, namespace, bundle_id)
    lock = acquire_lock(first, "gpu-test", gpu_uuid="GPU-00000000-0000-0000-0000-000000000000")
    second = context_for(source, namespace, bundle_id)
    with pytest.raises(QualificationError, match="qualification_lock_live"):
        acquire_lock(second, "gpu-test", gpu_uuid="GPU-00000000-0000-0000-0000-000000000000")
    owner_path = lock / "owner.json"
    owner = json.loads(owner_path.read_text(encoding="utf-8"))
    owner["heartbeat_unix_seconds"] = 0
    owner_path.write_text(json.dumps(owner), encoding="utf-8")
    with pytest.raises(QualificationError, match="qualification_lock_live"):
        acquire_lock(second, "gpu-test", gpu_uuid="GPU-00000000-0000-0000-0000-000000000000")
    owner["pid"] = 999_999_999
    owner["session"] = "unknown"
    owner_path.write_text(json.dumps(owner), encoding="utf-8")
    reclaimed = acquire_lock(
        second,
        "gpu-test",
        gpu_uuid="GPU-00000000-0000-0000-0000-000000000000",
    )
    assert second.reclaimed_locks[0]["reason"] == "heartbeat_stale_and_owner_not_live"
    release_lock(reclaimed)


def test_handoff_paths_and_cycles_are_bounded_and_resumable(tmp_path: Path) -> None:
    assert validate_namespace("/home/tester", None) == "/home/tester/glyph-relay-qualification"
    with pytest.raises(HandoffError, match="remote_namespace_invalid"):
        validate_namespace("/home/tester", "/home/tester/../other")
    bundle_id = "a" * 64
    record, first = qualification_run_record(tmp_path, bundle_id)
    assert qualification_run_record(tmp_path, bundle_id)[1] == first
    complete_run_record(record, "BLOCKED")
    _, second = qualification_run_record(tmp_path, bundle_id)
    assert first.endswith("c001")
    assert second.endswith("c002")
    complete_run_record(record, "PASSED")
    assert qualification_run_record(tmp_path, bundle_id)[1] == second


def test_phase_manifest_rejects_forward_dependencies(tmp_path: Path) -> None:
    manifest = tmp_path / "phases.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "phases": [
                    {
                        "id": "first",
                        "title": "First",
                        "kind": "command",
                        "required": True,
                        "dependencies": ["later"],
                        "commands": [["true"]],
                        "environment_names": [],
                    },
                    {
                        "id": "later",
                        "title": "Later",
                        "kind": "command",
                        "required": True,
                        "dependencies": [],
                        "commands": [["true"]],
                        "environment_names": [],
                    },
                ],
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(QualificationError, match="qualification_phase_dependency_unknown"):
        load_phases(manifest)


def test_terminal_run_without_export_is_relaunched(monkeypatch: pytest.MonkeyPatch) -> None:
    states = iter(
        [
            {"status": "BLOCKED", "export_ready": False, "runner_alive": False},
            {"status": "BLOCKED", "export_ready": True, "runner_alive": False},
        ]
    )
    launches: list[str] = []

    def launch(_host: str, _namespace: str, _source: str, _bundle: str, run_id: str) -> str:
        launches.append(run_id)
        return ""

    monkeypatch.setattr(handoff, "remote_status", lambda *_arguments: next(states))
    monkeypatch.setattr(handoff, "tmux_session_alive", lambda *_arguments: False)
    monkeypatch.setattr(handoff, "launch_runner", launch)
    result = handoff.wait_for_result(
        "gpu-host",
        "/home/tester/qualification",
        "/home/tester/qualification/source",
        "a" * 64,
        "test-run",
        0,
        1,
    )
    assert result["export_ready"] is True
    assert launches == ["test-run"]
