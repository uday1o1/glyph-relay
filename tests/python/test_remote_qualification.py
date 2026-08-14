from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from tools.gpu import handoff, remote_qualification
from tools.gpu.handoff import (
    HandoffError,
    complete_run_record,
    qualification_run_record,
    validate_namespace,
)
from tools.gpu.public_evidence import PublicEvidenceError, create_public_evidence
from tools.gpu.remote_qualification import (
    QualificationError,
    RunContext,
    acquire_lock,
    execute,
    expand_argument,
    load_phases,
    phase_input_identity,
    release_lock,
    reusable_phase,
    run_bounded_command,
    validate_context,
)
from tools.gpu.source_bundle import (
    build_source_bundle,
    safe_extract_bundle,
    verify_result_archive,
)
from tools.validate_public_evidence import validate_public_evidence
from tools.validate_qualification import validate_qualification


def git(repository: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def rewrite_result_checksums(root: Path) -> None:
    lines = [
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root).as_posix()}"
        for path in sorted(root.rglob("*"))
        if path.is_file() and not path.is_symlink() and path.name != "SHA256SUMS"
    ]
    (root / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")


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
                            ],
                            [
                                "python3",
                                "-c",
                                "import json; from pathlib import Path; "
                                "Path(r'{phase_dir}/gate.json').write_text("
                                "json.dumps(dict(status='PASSED')) + '\\n')",
                            ],
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
                    {
                        "id": "saliency-development-selection",
                        "title": "Repository freeze handoff",
                        "kind": "command",
                        "required": True,
                        "dependencies": [],
                        "commands": [["python3", "-c", "raise SystemExit(75)"]],
                        "environment_names": [],
                    },
                    {
                        "id": "unrelated-tempfail",
                        "title": "Unrelated exit 75",
                        "kind": "command",
                        "required": True,
                        "dependencies": [],
                        "commands": [["python3", "-c", "raise SystemExit(75)"]],
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
        "runner-integrity": "PASSED",
        "saliency-development-selection": "BLOCKED",
        "unrelated-tempfail": "FAILED",
    }
    assert (context.run_root / "REPORT.md").is_file()
    assert (context.run_root / "commands.jsonl").is_file()
    assert (context.run_root / "junit.xml").is_file()
    freeze_state = json.loads(
        (context.run_root / "phases/saliency-development-selection/state.json").read_text(
            encoding="utf-8"
        )
    )
    freeze_result = json.loads(
        (context.run_root / freeze_state["result_path"]).read_text(encoding="utf-8")
    )
    assert freeze_result["reason"] == "repository_freeze_required"
    unrelated_state = json.loads(
        (context.run_root / "phases/unrelated-tempfail/state.json").read_text(encoding="utf-8")
    )
    unrelated_result = json.loads(
        (context.run_root / unrelated_state["result_path"]).read_text(encoding="utf-8")
    )
    assert unrelated_result["reason"] == "command_exit_75"
    passing_logs = list(context.run_root.glob("phases/passing/*/stdout.log"))
    assert len(passing_logs) == 1
    redacted_log = passing_logs[0].read_text(encoding="utf-8")
    assert "synthetic" not in redacted_log
    assert "user:pass" not in redacted_log
    assert "10.0.0.1" not in redacted_log

    passing_phase = load_phases(source / "qualification/test-phases.json")[0]
    stable_commands = [
        tuple(expand_argument(argument, context, None) for argument in command)
        for command in passing_phase.commands
    ]
    identity = phase_input_identity(passing_phase, context, stable_commands)
    assert reusable_phase(passing_phase, context, identity) is not None
    assert expand_argument("{source_bundle_id}", context, None) == bundle_id
    assert expand_argument("{phase_root}", context, None) == "<PHASE_ROOT>"
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
    public_archive = namespace / "exports/test-run-public.tar"
    public_checksum = namespace / "exports/test-run-public.tar.sha256"
    extracted = tmp_path / "verified-result"
    public = tmp_path / "verified-public"
    verify_result_archive(archive, checksum, extracted)
    verify_result_archive(public_archive, public_checksum, public)
    assert json.loads((extracted / "status.json").read_text())["status"] == "FAILED"
    validate_qualification(extracted, Path("schemas").resolve())
    validate_public_evidence(public, Path("schemas").resolve())
    public_content = "\n".join(
        path.read_text(encoding="utf-8")
        for path in public.rglob("*")
        if path.is_file() and path.name != "SHA256SUMS"
    )
    assert str(source) not in public_content
    assert str(namespace) not in public_content
    assert "publication_review_required" in public_content
    assert (public / "evidence/passing/gate.json").is_file()

    tampered_output = next(extracted.glob("phases/passing/*/gate.json"))
    tampered_output.write_text('{"status":"TAMPERED"}\n', encoding="utf-8")
    rewrite_result_checksums(extracted)
    with pytest.raises(ValueError, match="phase output hash does not match"):
        validate_qualification(extracted, Path("schemas").resolve())


def test_runner_exports_complete_blocked_result_when_environment_capture_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source, namespace, bundle_id = extracted_source(tmp_path)
    context = context_for(source, namespace, bundle_id)

    def fail_capture(_context: RunContext) -> dict[str, object]:
        raise OSError("seeded environment failure")

    monkeypatch.setattr(remote_qualification, "capture_environment", fail_capture)
    previous = Path.cwd()
    os.chdir(source)
    try:
        status = execute(context, source / "qualification/test-phases.json")
    finally:
        os.chdir(previous)
    assert status == "BLOCKED"
    result = json.loads((context.run_root / "status.json").read_text(encoding="utf-8"))
    assert result["phases"]["runner-integrity"] == "BLOCKED"
    assert all(state == "BLOCKED" for state in result["phases"].values())
    assert (namespace / "exports/test-run.tar").is_file()
    assert (namespace / "exports/test-run-public.tar").is_file()
    extracted = tmp_path / "blocked-result"
    verify_result_archive(
        namespace / "exports/test-run.tar",
        namespace / "exports/test-run.tar.sha256",
        extracted,
    )
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


def test_phase_manifest_rejects_unbounded_timeout(tmp_path: Path) -> None:
    manifest = tmp_path / "phases.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "phases": [
                    {
                        "id": "timeout",
                        "title": "Invalid timeout",
                        "kind": "command",
                        "required": True,
                        "dependencies": [],
                        "commands": [["true"]],
                        "environment_names": [],
                        "timeout_seconds": 0,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(QualificationError, match="qualification_phase_invalid"):
        load_phases(manifest)


def test_bounded_command_terminates_the_process_group_on_timeout(tmp_path: Path) -> None:
    context = RunContext(
        source_root=tmp_path,
        namespace=tmp_path,
        bundle_id="a" * 64,
        run_id="timeout-test",
        run_root=tmp_path,
        manifest_path=tmp_path / "SOURCE_MANIFEST.json",
    )
    with (
        (tmp_path / "stdout.log").open("w", encoding="utf-8") as stdout,
        (tmp_path / "stderr.log").open("w", encoding="utf-8") as stderr,
    ):
        returncode, timed_out = run_bounded_command(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            context,
            os.environ,
            stdout,
            stderr,
            0.05,
        )
    assert timed_out is True
    assert returncode != 0


def test_public_evidence_rejects_allowlisted_json_with_private_values(tmp_path: Path) -> None:
    run_root = tmp_path / "run"
    artifact = run_root / "phases/benchmark/attempt/gate.json"
    artifact.parent.mkdir(parents=True)
    artifact.write_text(json.dumps({"status": "PASSED", "path": str(tmp_path)}), encoding="utf-8")
    results = {
        "benchmark": {
            "status": "PASSED",
            "reason": "commands_passed",
            "duration_seconds": 1.0,
            "output_hashes": {
                artifact.relative_to(run_root).as_posix(): hashlib.sha256(
                    artifact.read_bytes()
                ).hexdigest()
            },
        }
    }
    with pytest.raises(PublicEvidenceError, match="public_artifact_contains_private_value"):
        create_public_evidence(
            run_root,
            "a" * 64,
            "b" * 40,
            "PASSED",
            {"os": {}, "tools": {}, "gpus": []},
            results,
            (str(tmp_path),),
        )
    assert not (run_root / "public-evidence").exists()


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
