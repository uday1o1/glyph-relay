from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path, PurePosixPath
from typing import Any

from tools.gpu.source_bundle import (
    BundleError,
    BundlePaths,
    build_source_bundle,
    load_manifest,
    verify_result_archive,
    verify_result_checksums,
)

HOST_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.@-]{0,127}$")
REMOTE_PATH_PATTERN = re.compile(r"^/[A-Za-z0-9._/-]+$")
RUN_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
TERMINAL_STATES = {"PASSED", "FAILED", "BLOCKED"}
REMOTE_SHA256 = (
    "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())"
)

REMOTE_BOOTSTRAP = r"""
import json, os, stat, sys
root, sentinel_id = sys.argv[1:3]
home = os.path.realpath(os.path.expanduser("~"))
if not os.path.isabs(root) or os.path.normpath(root) != root or root in {"/", home}:
    raise SystemExit("namespace_invalid")
if os.path.commonpath([home, root]) != home:
    raise SystemExit("namespace_outside_home")
created = False
if not os.path.lexists(root):
    os.mkdir(root, 0o700)
    created = True
details = os.lstat(root)
if not stat.S_ISDIR(details.st_mode) or stat.S_ISLNK(details.st_mode):
    raise SystemExit("namespace_type_invalid")
if details.st_uid != os.getuid() or stat.S_IMODE(details.st_mode) != 0o700:
    raise SystemExit("namespace_owner_mode_invalid")
sentinel = os.path.join(root, ".glyphrelay-owner")
expected = ("glyphrelay-sentinel-v1:" + sentinel_id + "\n").encode()
if not os.path.lexists(sentinel):
    if not created:
        raise SystemExit("namespace_sentinel_missing")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(sentinel, flags, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(expected)
        stream.flush()
        os.fsync(stream.fileno())
sentinel_details = os.lstat(sentinel)
if not stat.S_ISREG(sentinel_details.st_mode) or stat.S_ISLNK(sentinel_details.st_mode):
    raise SystemExit("namespace_sentinel_type_invalid")
if sentinel_details.st_uid != os.getuid() or stat.S_IMODE(sentinel_details.st_mode) != 0o600:
    raise SystemExit("namespace_sentinel_owner_mode_invalid")
with open(sentinel, "rb") as stream:
    if stream.read(256) != expected or stream.read(1):
        raise SystemExit("namespace_sentinel_identity_mismatch")
for name in ("exports", "incoming", "locks", "logs", "runs", "sources"):
    path = os.path.join(root, name)
    if not os.path.lexists(path):
        os.mkdir(path, 0o700)
    child = os.lstat(path)
    if not stat.S_ISDIR(child.st_mode) or stat.S_ISLNK(child.st_mode):
        raise SystemExit("namespace_child_type_invalid")
    if child.st_uid != os.getuid() or stat.S_IMODE(child.st_mode) != 0o700:
        raise SystemExit("namespace_child_owner_mode_invalid")
directory = os.open(root, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
os.fsync(directory)
os.close(directory)
if created:
    home_directory = os.open(home, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    os.fsync(home_directory)
    os.close(home_directory)
print(json.dumps({"created": created, "home": home, "namespace": os.path.realpath(root)}))
"""

REMOTE_STATUS = r"""
import json, os, sys
root, run_id, bundle_id = sys.argv[1:4]
status_path = os.path.join(root, "runs", run_id, "status.json")
archive = os.path.join(root, "exports", run_id + ".tar")
checksum = archive + ".sha256"
owner_path = os.path.join(root, "locks", "bundle-" + bundle_id, "owner.json")
runner_alive = False
if os.path.isfile(owner_path):
    try:
        with open(owner_path, encoding="utf-8") as stream:
            owner = json.load(stream)
        os.kill(int(owner.get("pid", -1)), 0)
        runner_alive = True
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        pass
value = {"status": "NOT_STARTED", "export_ready": False, "runner_alive": runner_alive}
if os.path.isfile(status_path):
    with open(status_path, encoding="utf-8") as stream:
        status = json.load(stream)
    value["status"] = status.get("status", "INVALID")
value["export_ready"] = os.path.isfile(archive) and os.path.isfile(checksum)
print(json.dumps(value, sort_keys=True))
"""


class HandoffError(RuntimeError):
    """Raised when the consolidated handoff cannot continue safely."""


def run(
    arguments: list[str],
    *,
    input_text: str | None = None,
    timeout: float | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            arguments,
            input=input_text,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=check,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise HandoffError(
            f"external_command_failed:{arguments[0]}:{type(error).__name__}"
        ) from error


def ssh(host: str, arguments: list[str], *, timeout: float | None = None) -> str:
    command = shlex.join(arguments)
    completed = run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, command],
        timeout=timeout,
    )
    return completed.stdout.strip()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8", closefd=True) as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def remote_home(host: str) -> str:
    value = ssh(
        host,
        ["python3", "-c", "import os; print(os.path.realpath(os.path.expanduser('~')))"],
        timeout=20,
    )
    if (
        not REMOTE_PATH_PATTERN.fullmatch(value)
        or value == "/"
        or posixpath.normpath(value) != value
    ):
        raise HandoffError("remote_home_invalid")
    return value


def validate_namespace(home: str, override: str | None) -> str:
    namespace = override or posixpath.join(home, "glyph-relay-qualification")
    if (
        not REMOTE_PATH_PATTERN.fullmatch(namespace)
        or posixpath.normpath(namespace) != namespace
        or namespace in {"/", home}
        or not namespace.startswith(f"{home}/")
        or ".." in PurePosixPath(namespace).parts
    ):
        raise HandoffError("remote_namespace_invalid")
    return namespace


def sentinel_record(repository_root: Path, host: str, namespace: str) -> tuple[Path, str]:
    host_key = hashlib.sha256(host.encode()).hexdigest()[:24]
    record_path = repository_root / "build" / "qualification" / f"sentinel-{host_key}.json"
    if record_path.exists():
        value = json.loads(record_path.read_text(encoding="utf-8"))
        if (
            not isinstance(value, dict)
            or value.get("schema_version") != 1
            or value.get("namespace") != namespace
            or not isinstance(value.get("sentinel_id"), str)
            or not re.fullmatch(r"[0-9a-f]{32}", value["sentinel_id"])
        ):
            raise HandoffError("local_sentinel_record_invalid")
        return record_path, str(value["sentinel_id"])
    sentinel_id = uuid.uuid4().hex
    atomic_json(
        record_path,
        {
            "schema_version": 1,
            "state": "PENDING",
            "host_key": host_key,
            "namespace": namespace,
            "sentinel_id": sentinel_id,
        },
    )
    return record_path, sentinel_id


def bootstrap_namespace(repository_root: Path, host: str, namespace: str) -> tuple[Path, str]:
    record_path, sentinel_id = sentinel_record(repository_root, host, namespace)
    output = ssh(host, ["python3", "-c", REMOTE_BOOTSTRAP, namespace, sentinel_id], timeout=30)
    value = json.loads(output)
    if value.get("namespace") != namespace:
        raise HandoffError("remote_namespace_canonical_mismatch")
    atomic_json(
        record_path,
        {
            "schema_version": 1,
            "state": "ACTIVE",
            "host_key": hashlib.sha256(host.encode()).hexdigest()[:24],
            "namespace": namespace,
            "sentinel_id": sentinel_id,
        },
    )
    return record_path, sentinel_id


def scp_to(host: str, local: Path, remote: str) -> None:
    run(
        [
            "scp",
            "-q",
            "-p",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=10",
            str(local),
            f"{host}:{remote}",
        ],
        timeout=600,
    )


def scp_from(host: str, remote: str, local: Path) -> None:
    run(
        [
            "scp",
            "-q",
            "-p",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=10",
            f"{host}:{remote}",
            str(local),
        ],
        timeout=3_600,
    )


def sync_bundle(
    repository_root: Path,
    host: str,
    namespace: str,
    bundle: BundlePaths,
) -> str:
    manifest = load_manifest(bundle.manifest)
    source_tool = repository_root / "tools/gpu/source_bundle.py"
    expected_tool_hash = next(
        entry["sha256"]
        for entry in manifest["files"]
        if entry["path"] == "tools/gpu/source_bundle.py"
    )
    transfer_id = f"transfer-{uuid.uuid4().hex}"
    incoming = f"{namespace}/incoming/{transfer_id}"
    ssh(host, ["mkdir", "-m", "700", incoming], timeout=20)
    try:
        for path in (bundle.archive, bundle.manifest, bundle.metadata, source_tool):
            scp_to(host, path, f"{incoming}/{path.name}")
        remote_tool = f"{incoming}/{source_tool.name}"
        actual_hash = ssh(
            host,
            [
                "python3",
                "-c",
                REMOTE_SHA256,
                remote_tool,
            ],
            timeout=30,
        )
        if actual_hash != expected_tool_hash:
            raise HandoffError("remote_bootstrap_tool_hash_mismatch")
        destination = f"{namespace}/sources/{bundle.bundle_id}"
        exists = ssh(
            host,
            [
                "python3",
                "-c",
                "import os,sys;print('yes' if os.path.lexists(sys.argv[1]) else 'no')",
                destination,
            ],
            timeout=20,
        )
        if exists == "yes":
            trusted_tool = f"{destination}/tools/gpu/source_bundle.py"
            trusted_hash = ssh(
                host,
                [
                    "python3",
                    "-c",
                    REMOTE_SHA256,
                    trusted_tool,
                ],
                timeout=30,
            )
            if trusted_hash != expected_tool_hash:
                raise HandoffError("existing_remote_source_tool_hash_mismatch")
            ssh(
                host,
                [
                    "python3",
                    trusted_tool,
                    "validate-source",
                    "--root",
                    destination,
                    "--manifest",
                    f"{destination}/SOURCE_MANIFEST.json",
                ],
                timeout=300,
            )
        elif exists == "no":
            ssh(
                host,
                [
                    "python3",
                    remote_tool,
                    "safe-extract",
                    "--archive",
                    f"{incoming}/{bundle.archive.name}",
                    "--manifest",
                    f"{incoming}/{bundle.manifest.name}",
                    "--metadata",
                    f"{incoming}/{bundle.metadata.name}",
                    "--destination",
                    destination,
                ],
                timeout=1_800,
            )
        else:
            raise HandoffError("remote_source_existence_probe_invalid")
        return destination
    finally:
        ssh(host, ["rm", "-rf", incoming], timeout=30)


def remote_status(host: str, namespace: str, run_id: str, bundle_id: str) -> dict[str, Any]:
    value = json.loads(
        ssh(
            host,
            ["python3", "-c", REMOTE_STATUS, namespace, run_id, bundle_id],
            timeout=30,
        )
    )
    if not isinstance(value, dict) or value.get("status") not in TERMINAL_STATES | {
        "NOT_STARTED",
        "RUNNING",
    }:
        raise HandoffError("remote_status_invalid")
    return value


def tmux_session_alive(host: str, name: str) -> bool:
    completed = run(
        [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=10",
            host,
            shlex.join(["tmux", "has-session", "-t", name]),
        ],
        timeout=30,
        check=False,
    )
    return completed.returncode == 0


def launch_runner(host: str, namespace: str, source: str, bundle_id: str, run_id: str) -> str:
    session = f"glyphrelay-{bundle_id[:12]}"
    runner_arguments = [
        "./scripts/gpu/run_remote_qualification.sh",
        "--namespace",
        namespace,
        "--bundle-id",
        bundle_id,
        "--run-id",
        run_id,
    ]
    log = f"{namespace}/logs/{run_id}.log"
    command = f"exec {shlex.join(runner_arguments)} >{shlex.quote(log)} 2>&1"
    tmux_available = (
        run(
            ["ssh", "-o", "BatchMode=yes", host, "command -v tmux"],
            timeout=30,
            check=False,
        ).returncode
        == 0
    )
    if tmux_available:
        ssh(
            host,
            [
                "tmux",
                "new-session",
                "-d",
                "-s",
                session,
                "-c",
                source,
                "env",
                f"GLYPHRELAY_DETACHED_SESSION=tmux:{session}",
                "bash",
                "-c",
                command,
            ],
            timeout=30,
        )
        return session
    detached = f"nohup:{run_id}"
    shell_command = (
        f"cd {shlex.quote(source)} && "
        f"nohup env GLYPHRELAY_DETACHED_SESSION={shlex.quote(detached)} "
        f"bash -c {shlex.quote(command)} </dev/null >/dev/null 2>&1 &"
    )
    ssh(host, ["bash", "-c", shell_command], timeout=30)
    return ""


def wait_for_result(
    host: str,
    namespace: str,
    source: str,
    bundle_id: str,
    run_id: str,
    poll_seconds: int,
    maximum_wait_seconds: int,
) -> dict[str, Any]:
    session = f"glyphrelay-{bundle_id[:12]}"
    started = time.monotonic()
    launches = 0
    while True:
        state = remote_status(host, namespace, run_id, bundle_id)
        if state["status"] in TERMINAL_STATES and state["export_ready"] is True:
            return state
        alive = bool(state.get("runner_alive")) or tmux_session_alive(host, session)
        if not alive and not state["export_ready"]:
            if launches >= 3:
                raise HandoffError("remote_runner_repeatedly_exited_without_terminal_status")
            launch_runner(host, namespace, source, bundle_id, run_id)
            launches += 1
        if maximum_wait_seconds and time.monotonic() - started >= maximum_wait_seconds:
            raise HandoffError("remote_qualification_wait_timeout")
        time.sleep(poll_seconds)


def retrieve_result(
    repository_root: Path,
    host: str,
    namespace: str,
    run_id: str,
) -> tuple[Path, dict[str, Any]]:
    artifacts = repository_root / "artifacts" / "gpu-runs"
    artifacts.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(repository_root / "artifacts", 0o700)
    os.chmod(artifacts, 0o700)
    destination = artifacts / run_id
    if destination.exists():
        verify_result_checksums(destination / "result")
        status = json.loads((destination / "result/status.json").read_text(encoding="utf-8"))
        return destination, status
    incoming = artifacts / f".incoming-{run_id}-{uuid.uuid4().hex}"
    incoming.mkdir(mode=0o700)
    try:
        archive = incoming / f"{run_id}.tar"
        checksum = incoming / f"{run_id}.tar.sha256"
        scp_from(host, f"{namespace}/exports/{archive.name}", archive)
        scp_from(host, f"{namespace}/exports/{checksum.name}", checksum)
        destination.mkdir(mode=0o700)
        shutil.move(archive, destination / archive.name)
        shutil.move(checksum, destination / checksum.name)
        verify_result_archive(
            destination / archive.name,
            destination / checksum.name,
            destination / "result",
        )
        status = json.loads((destination / "result/status.json").read_text(encoding="utf-8"))
        return destination, status
    except Exception:
        if destination.exists():
            shutil.rmtree(destination)
        raise
    finally:
        shutil.rmtree(incoming, ignore_errors=True)


def parse_positive_integer(name: str, default: int, *, allow_zero: bool = False) -> int:
    raw = os.environ.get(name, str(default))
    if not raw.isdigit():
        raise HandoffError(f"{name.lower()}_invalid")
    value = int(raw)
    if value < (0 if allow_zero else 1) or value > 86_400:
        raise HandoffError(f"{name.lower()}_invalid")
    return value


def qualification_run_record(repository_root: Path, bundle_id: str) -> tuple[Path, str]:
    path = repository_root / "build" / "qualification" / f"run-{bundle_id}.json"
    cycle = 1
    if path.exists():
        value = json.loads(path.read_text(encoding="utf-8"))
        if (
            not isinstance(value, dict)
            or value.get("schema_version") != 1
            or value.get("bundle_id") != bundle_id
            or not isinstance(value.get("cycle"), int)
            or not isinstance(value.get("run_id"), str)
            or not RUN_ID_PATTERN.fullmatch(value["run_id"])
            or value.get("state") not in {"ACTIVE", "COMPLETED"}
        ):
            raise HandoffError("local_qualification_run_record_invalid")
        if value["state"] == "ACTIVE" or value.get("status") == "PASSED":
            return path, str(value["run_id"])
        cycle = int(value["cycle"]) + 1
    if cycle > 999:
        raise HandoffError("qualification_cycle_limit_exceeded")
    run_id = f"q-{bundle_id[:16]}-c{cycle:03d}"
    atomic_json(
        path,
        {
            "schema_version": 1,
            "bundle_id": bundle_id,
            "cycle": cycle,
            "run_id": run_id,
            "state": "ACTIVE",
            "status": None,
        },
    )
    return path, run_id


def complete_run_record(path: Path, status: str) -> None:
    value = json.loads(path.read_text(encoding="utf-8"))
    value["state"] = "COMPLETED"
    value["status"] = status
    atomic_json(path, value)


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Consolidated GlyphRelay GPU qualification handoff"
    )
    parser.add_argument("--repository", default=".")
    return parser


def main() -> int:
    try:
        arguments = argument_parser().parse_args()
        repository_root = Path(arguments.repository).resolve(strict=True)
        host = os.environ.get("GLYPHRELAY_GPU_HOST", "cuda-pm")
        if not HOST_PATTERN.fullmatch(host):
            raise HandoffError("gpu_host_alias_invalid")
        home = remote_home(host)
        namespace = validate_namespace(home, os.environ.get("GLYPHRELAY_GPU_NAMESPACE"))
        bootstrap_namespace(repository_root, host, namespace)
        bundle = build_source_bundle(repository_root, repository_root / "build/qualification")
        source = sync_bundle(repository_root, host, namespace, bundle)
        run_record, run_id = qualification_run_record(repository_root, bundle.bundle_id)
        poll_seconds = parse_positive_integer("GLYPHRELAY_POLL_SECONDS", 15)
        maximum_wait = parse_positive_integer("GLYPHRELAY_MAX_WAIT_SECONDS", 0, allow_zero=True)
        wait_for_result(
            host,
            namespace,
            source,
            bundle.bundle_id,
            run_id,
            poll_seconds,
            maximum_wait,
        )
        destination, status = retrieve_result(repository_root, host, namespace, run_id)
        if status.get("bundle_id") != bundle.bundle_id or status.get("run_id") != run_id:
            raise HandoffError("retrieved_result_identity_mismatch")
        state = status.get("status")
        if state not in TERMINAL_STATES:
            raise HandoffError("retrieved_result_status_not_terminal")
        complete_run_record(run_record, str(state))
        summary = {
            "status": state,
            "run_id": run_id,
            "bundle_id": bundle.bundle_id,
            "artifact_path": str(destination),
            "passed_phases": sorted(
                name
                for name, phase_state in status.get("phases", {}).items()
                if phase_state == "PASSED"
            ),
            "failed_phases": sorted(
                name
                for name, phase_state in status.get("phases", {}).items()
                if phase_state == "FAILED"
            ),
            "deferred_or_blocked_phases": sorted(
                name
                for name, phase_state in status.get("phases", {}).items()
                if phase_state in {"BLOCKED", "DEFERRED_INTERACTIVE"}
            ),
            "claim_consequence": (
                "qualification evidence is eligible for milestone review"
                if state == "PASSED"
                else "no hardware or release claim is accepted"
            ),
        }
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0 if state == "PASSED" else 4 if state == "FAILED" else 5
    except (HandoffError, BundleError, OSError, ValueError, json.JSONDecodeError) as error:
        reason = (
            str(error) if isinstance(error, (HandoffError, BundleError)) else type(error).__name__
        )
        print(f"qualification handoff blocked: {reason}", file=sys.stderr)
        return 5


if __name__ == "__main__":
    raise SystemExit(main())
