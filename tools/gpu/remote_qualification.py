from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path, PurePosixPath
from typing import Any, TextIO

from tools.gpu.source_bundle import BundleError, load_manifest, validate_extracted_source

RUN_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
BUNDLE_ID_PATTERN = re.compile(r"^[0-9a-f]{64}$")
GPU_UUID_PATTERN = re.compile(r"^GPU-[0-9a-fA-F-]{16,}$")
FINAL_PHASE_STATES = {"PASSED", "FAILED", "BLOCKED", "DEFERRED_INTERACTIVE"}
TERMINAL_RUN_STATES = {"PASSED", "FAILED", "BLOCKED"}
BASE_COMMAND_ENVIRONMENT = {
    "DISPLAY",
    "HOME",
    "LANG",
    "LC_ALL",
    "PATH",
    "PYTHONDONTWRITEBYTECODE",
    "TMPDIR",
    "WAYLAND_DISPLAY",
    "XDG_RUNTIME_DIR",
    "XDG_SESSION_TYPE",
}
MAXIMUM_PHASE_LOG_CHARACTERS = 32 * 1024 * 1024
MAXIMUM_PHASE_LOG_LINE_CHARACTERS = 64 * 1024
SECRET_PATTERN = re.compile(
    r"(?i)(authorization|password|private[_-]?key|secret|token)(\s*[:=]\s*)([^\s,;]+)"
)
IPV4_PATTERN = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
CREDENTIAL_URI_PATTERN = re.compile(r"(?i)([a-z][a-z0-9+.-]*://)[^/@\s:]+:[^/@\s]+@")


class QualificationError(RuntimeError):
    """Raised when qualification infrastructure cannot proceed safely."""


@dataclass(frozen=True)
class PhaseDefinition:
    identifier: str
    title: str
    kind: str
    required: bool
    dependencies: tuple[str, ...]
    commands: tuple[tuple[str, ...], ...]
    environment_names: tuple[str, ...] = ()
    environment_values: tuple[tuple[str, str], ...] = ()


@dataclass
class RunContext:
    source_root: Path
    namespace: Path
    bundle_id: str
    run_id: str
    run_root: Path
    manifest_path: Path
    environment: dict[str, Any] = field(default_factory=dict)
    environment_fingerprint: str = ""
    selected_gpu: dict[str, Any] | None = None
    gpu_lock: Path | None = None
    reclaimed_locks: list[dict[str, Any]] = field(default_factory=list)
    sampler: ResourceSampler | None = None


def utc_now() -> str:
    return datetime.now(tz=UTC).isoformat().replace("+00:00", "Z")


def canonical_json(value: Any) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"
    ).encode()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


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


def atomic_write(path: Path, content: bytes, mode: int = 0o600) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_json(path: Path, value: Any) -> None:
    atomic_write(path, json.dumps(value, indent=2, sort_keys=True).encode() + b"\n")


def append_durable(path: Path, line: str) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(descriptor, line.encode())
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def redact(text: str, context: RunContext) -> str:
    replacements = {
        str(context.source_root): "<SOURCE_ROOT>",
        str(context.namespace): "<QUALIFICATION_ROOT>",
        str(Path.home()): "<HOME>",
    }
    redacted = text
    for value, replacement in sorted(
        replacements.items(), key=lambda item: len(item[0]), reverse=True
    ):
        if value and value != "/":
            redacted = redacted.replace(value, replacement)
    redacted = SECRET_PATTERN.sub(
        lambda match: f"{match.group(1)}{match.group(2)}<REDACTED>", redacted
    )
    redacted = CREDENTIAL_URI_PATTERN.sub(r"\1<REDACTED_CREDENTIALS>@", redacted)
    return IPV4_PATTERN.sub("<IP_ADDRESS>", redacted)


def validate_private_directory(path: Path, label: str) -> Path:
    if not path.is_absolute() or path == Path("/") or ".." in path.parts:
        raise QualificationError(f"{label}_path_invalid")
    details = path.lstat()
    if not stat.S_ISDIR(details.st_mode) or stat.S_ISLNK(details.st_mode):
        raise QualificationError(f"{label}_not_real_directory")
    if details.st_uid != os.getuid() or stat.S_IMODE(details.st_mode) != 0o700:
        raise QualificationError(f"{label}_ownership_or_mode_invalid")
    return path.resolve(strict=True)


def validate_context(arguments: argparse.Namespace) -> RunContext:
    if not RUN_ID_PATTERN.fullmatch(arguments.run_id):
        raise QualificationError("qualification_run_id_invalid")
    if not BUNDLE_ID_PATTERN.fullmatch(arguments.bundle_id):
        raise QualificationError("qualification_bundle_id_invalid")
    source_root = Path.cwd().resolve(strict=True)
    namespace = validate_private_directory(Path(arguments.namespace), "qualification_namespace")
    expected_source = namespace / "sources" / arguments.bundle_id
    validated_source = validate_private_directory(expected_source, "qualification_source")
    if source_root != validated_source:
        raise QualificationError("qualification_source_directory_mismatch")
    manifest_path = source_root / "SOURCE_MANIFEST.json"
    manifest = load_manifest(manifest_path)
    if manifest["bundle_id"] != arguments.bundle_id:
        raise QualificationError("qualification_manifest_bundle_mismatch")
    validate_extracted_source(source_root, manifest_path)
    runs = namespace / "runs"
    runs.mkdir(mode=0o700, exist_ok=True)
    os.chmod(runs, 0o700)
    run_root = runs / arguments.run_id
    if run_root.exists():
        validate_private_directory(run_root, "qualification_run")
    else:
        run_root.mkdir(mode=0o700)
        fsync_directory(runs)
    return RunContext(
        source_root=source_root,
        namespace=namespace,
        bundle_id=arguments.bundle_id,
        run_id=arguments.run_id,
        run_root=run_root,
        manifest_path=manifest_path,
    )


def command_output(arguments: Sequence[str], timeout: float = 15.0) -> tuple[int, str, str]:
    try:
        completed = subprocess.run(
            list(arguments),
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        return completed.returncode, completed.stdout, completed.stderr
    except (OSError, subprocess.TimeoutExpired) as error:
        return 127, "", str(error)


def executable_identity(command: str, source_root: Path) -> dict[str, str]:
    candidate = Path(command)
    if "/" in command:
        path = candidate if candidate.is_absolute() else source_root / candidate
        resolved = path.resolve(strict=False)
    else:
        located = shutil.which(command)
        resolved = Path(located).resolve() if located else Path(command)
    if not resolved.is_file():
        return {"command": command, "path": str(resolved), "sha256": "MISSING"}
    return {"command": command, "path": str(resolved), "sha256": sha256_file(resolved)}


def capture_environment(context: RunContext) -> dict[str, Any]:
    versions: dict[str, dict[str, Any]] = {}
    for name, arguments in {
        "cmake": ["cmake", "--version"],
        "ninja": ["ninja", "--version"],
        "compiler": ["c++", "--version"],
        "cuda_toolkit": ["nvcc", "--version"],
        "node": ["node", "--version"],
        "corepack": ["corepack", "--version"],
        "uv": ["uv", "--version"],
        "ffmpeg": ["ffmpeg", "-version"],
        "tshark": ["tshark", "--version"],
        "tmux": ["tmux", "-V"],
    }.items():
        returncode, stdout, stderr = command_output(arguments)
        versions[name] = {
            "available": returncode == 0,
            "version": (stdout or stderr).splitlines()[0][:512]
            if stdout or stderr
            else "unavailable",
        }
    os_release: dict[str, str] = {}
    release_path = Path("/etc/os-release")
    if release_path.is_file():
        for line in release_path.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if separator and key in {"ID", "VERSION_ID", "VERSION_CODENAME"}:
                os_release[key.lower()] = value.strip('"')
    returncode, stdout, stderr = command_output(
        [
            "nvidia-smi",
            "--query-gpu=index,uuid,name,driver_version,memory.total",
            "--format=csv,noheader,nounits",
        ]
    )
    gpus = [] if returncode else parse_gpu_inventory(stdout)
    hostname_digest = sha256_bytes(socket.gethostname().encode())
    environment = {
        "schema_version": 1,
        "captured_at_utc": utc_now(),
        "hostname_class": "linux_gpu_workstation",
        "hostname_sha256": hostname_digest,
        "os": os_release,
        "kernel": platform.release(),
        "architecture": platform.machine(),
        "cpu": platform.processor() or "unreported",
        "desktop": {
            "session_type": os.environ.get("XDG_SESSION_TYPE", "unavailable"),
            "display_present": bool(os.environ.get("DISPLAY")),
            "wayland_display_present": bool(os.environ.get("WAYLAND_DISPLAY")),
            "runtime_directory_present": bool(os.environ.get("XDG_RUNTIME_DIR")),
        },
        "tools": versions,
        "gpus": gpus,
        "gpu_inventory_error": stderr[:1_024] if returncode else "",
        "bundle_id": context.bundle_id,
    }
    fingerprint_value = dict(environment)
    fingerprint_value.pop("captured_at_utc", None)
    environment["fingerprint_sha256"] = sha256_bytes(canonical_json(fingerprint_value))
    return environment


def parse_gpu_inventory(output: str) -> list[dict[str, Any]]:
    gpus: list[dict[str, Any]] = []
    for line in output.splitlines():
        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 5 or not fields[0].isdigit() or not GPU_UUID_PATTERN.fullmatch(fields[1]):
            raise QualificationError("nvidia_smi_inventory_invalid")
        gpus.append(
            {
                "index": int(fields[0]),
                "uuid": fields[1],
                "model": fields[2],
                "driver_version": fields[3],
                "memory_mib": int(fields[4]) if fields[4].isdigit() else 0,
            }
        )
    return gpus


def model_priority(model: str) -> int:
    priorities = (
        "RTX PRO 6000 Blackwell Workstation Edition",
        "RTX 6000 Ada Generation",
    )
    try:
        return priorities.index(model)
    except ValueError:
        return len(priorities)


def select_gpu(context: RunContext) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    decisions: list[dict[str, Any]] = []
    probe = context.source_root / "build/linux-gpu/glyphrelay_probe_nvenc"
    if not probe.is_file():
        return None, [{"reason": "nvenc_probe_executable_missing"}]
    for gpu in context.environment.get("gpus", []):
        environment = os.environ.copy()
        environment["CUDA_VISIBLE_DEVICES"] = str(gpu["uuid"])
        try:
            completed = subprocess.run(
                [str(probe)],
                cwd=context.source_root,
                env=environment,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            report = json.loads(completed.stdout) if completed.stdout else {}
            passed = completed.returncode == 0 and report.get("passed") is True
            decisions.append({**gpu, "probe": report, "qualified": passed})
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
            decisions.append({**gpu, "probe_error": str(error), "qualified": False})
    qualified = [decision for decision in decisions if decision.get("qualified")]
    qualified.sort(key=lambda item: (model_priority(str(item["model"])), str(item["uuid"])))
    return (qualified[0] if qualified else None), decisions


def process_alive(process_id: int) -> bool:
    if process_id <= 1:
        return False
    try:
        os.kill(process_id, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def session_alive(session: str) -> bool:
    if session.startswith("tmux:"):
        name = session.removeprefix("tmux:")
        return command_output(["tmux", "has-session", "-t", name], timeout=5)[0] == 0
    return False


def acquire_lock(context: RunContext, name: str, *, gpu_uuid: str | None = None) -> Path:
    locks = context.namespace / "locks"
    locks.mkdir(mode=0o700, exist_ok=True)
    os.chmod(locks, 0o700)
    lock = locks / name
    owner = {
        "schema_version": 1,
        "pid": os.getpid(),
        "run_id": context.run_id,
        "bundle_id": context.bundle_id,
        "session": os.environ.get("GLYPHRELAY_DETACHED_SESSION", "unknown"),
        "gpu_uuid": gpu_uuid,
        "created_at_utc": utc_now(),
        "heartbeat_unix_seconds": time.time(),
    }
    try:
        lock.mkdir(mode=0o700)
    except FileExistsError:
        details = lock.lstat()
        if (
            not stat.S_ISDIR(details.st_mode)
            or stat.S_ISLNK(details.st_mode)
            or details.st_uid != os.getuid()
            or stat.S_IMODE(details.st_mode) != 0o700
        ):
            raise QualificationError(f"qualification_lock_directory_invalid:{name}") from None
        owner_path = lock / "owner.json"
        try:
            existing = json.loads(owner_path.read_text(encoding="utf-8"))
            heartbeat = float(existing.get("heartbeat_unix_seconds", 0))
            pid = int(existing.get("pid", -1))
            session = str(existing.get("session", "unknown"))
        except (OSError, ValueError, TypeError, json.JSONDecodeError) as error:
            raise QualificationError(f"qualification_lock_unreadable:{name}:{error}") from error
        heartbeat_live = time.time() - heartbeat <= 120
        process_is_live = process_alive(pid)
        session_is_live = session_alive(session)
        live = heartbeat_live or process_is_live or session_is_live
        if live:
            raise QualificationError(f"qualification_lock_live:{name}") from None
        stale = locks / f"{name}.stale-{int(time.time())}-{os.getpid()}"
        os.replace(lock, stale)
        fsync_directory(locks)
        context.reclaimed_locks.append(
            {
                "lock": name,
                "prior_owner": existing,
                "reclaimed_at_utc": utc_now(),
                "reason": "heartbeat_stale_and_owner_not_live",
            }
        )
        lock.mkdir(mode=0o700)
    atomic_json(lock / "owner.json", owner)
    fsync_directory(locks)
    return lock


def refresh_lock(lock: Path) -> None:
    owner_path = lock / "owner.json"
    owner = json.loads(owner_path.read_text(encoding="utf-8"))
    owner["heartbeat_unix_seconds"] = time.time()
    atomic_json(owner_path, owner)


def release_lock(lock: Path) -> None:
    owner = lock / "owner.json"
    value = json.loads(owner.read_text(encoding="utf-8"))
    if value.get("pid") != os.getpid():
        raise QualificationError("qualification_lock_release_owner_mismatch")
    owner.unlink(missing_ok=True)
    lock.rmdir()
    fsync_directory(lock.parent)


class ResourceSampler:
    def __init__(self, context: RunContext, gpu_uuid: str, lock: Path) -> None:
        self.context = context
        self.gpu_uuid = gpu_uuid
        self.lock = lock
        self.stop_event = threading.Event()
        self.failure: str | None = None
        self.thread = threading.Thread(
            target=self._run, name="glyphrelay-gpu-sampler", daemon=False
        )

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=15)
        if self.thread.is_alive():
            raise QualificationError("resource_sampler_did_not_stop")
        if self.failure:
            raise QualificationError(self.failure)

    def _run(self) -> None:
        sample_path = self.context.run_root / "resource-samples.jsonl"
        while not self.stop_event.is_set():
            command = [
                "nvidia-smi",
                f"--id={self.gpu_uuid}",
                "--query-gpu=timestamp,uuid,utilization.gpu,memory.used,clocks.sm,temperature.gpu,power.draw,clocks_throttle_reasons.active,ecc.errors.uncorrected.volatile.total",
                "--format=csv,noheader,nounits",
            ]
            returncode, stdout, stderr = command_output(command, timeout=10)
            process_code, process_stdout, process_stderr = command_output(
                [
                    "nvidia-smi",
                    "--query-compute-apps=pid,process_name,used_memory,gpu_uuid",
                    "--format=csv,noheader,nounits",
                ],
                timeout=10,
            )
            sample = {
                "captured_at_utc": utc_now(),
                "returncode": returncode,
                "gpu": stdout.strip()[:2_048],
                "gpu_error": stderr.strip()[:1_024],
                "process_returncode": process_code,
                "processes": process_stdout.strip().splitlines()[:128],
                "process_error": process_stderr.strip()[:1_024],
            }
            append_durable(sample_path, json.dumps(sample, sort_keys=True) + "\n")
            try:
                refresh_lock(self.lock)
            except (OSError, ValueError, json.JSONDecodeError) as error:
                self.failure = f"resource_sampler_lock_refresh_failed:{type(error).__name__}"
                self.stop_event.set()
                return
            self.stop_event.wait(5)


class LockHeartbeat:
    def __init__(self, lock: Path) -> None:
        self.lock = lock
        self.stop_event = threading.Event()
        self.failure: str | None = None
        self.thread = threading.Thread(
            target=self._run,
            name="glyphrelay-bundle-lock-heartbeat",
            daemon=False,
        )

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=15)
        if self.thread.is_alive():
            raise QualificationError("bundle_lock_heartbeat_did_not_stop")
        if self.failure:
            raise QualificationError(self.failure)

    def _run(self) -> None:
        while not self.stop_event.wait(5):
            try:
                refresh_lock(self.lock)
            except (OSError, ValueError, json.JSONDecodeError) as error:
                self.failure = f"bundle_lock_heartbeat_failed:{type(error).__name__}"
                self.stop_event.set()
                return


def load_phases(path: Path) -> tuple[PhaseDefinition, ...]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise QualificationError("qualification_phase_manifest_invalid")
    raw_phases = value.get("phases")
    if not isinstance(raw_phases, list):
        raise QualificationError("qualification_phase_manifest_invalid")
    phases: list[PhaseDefinition] = []
    seen: set[str] = set()
    for raw in raw_phases:
        if not isinstance(raw, dict):
            raise QualificationError("qualification_phase_invalid")
        required_fields = {
            "id",
            "title",
            "kind",
            "required",
            "dependencies",
            "commands",
            "environment_names",
        }
        if not required_fields.issubset(raw) or not set(raw).issubset(
            required_fields | {"environment_values"}
        ):
            raise QualificationError("qualification_phase_fields_invalid")
        identifier = raw.get("id")
        if (
            not isinstance(identifier, str)
            or not RUN_ID_PATTERN.fullmatch(identifier)
            or identifier in seen
        ):
            raise QualificationError("qualification_phase_id_invalid")
        seen.add(identifier)
        dependencies = raw.get("dependencies", [])
        commands = raw.get("commands", [])
        environment_names = raw.get("environment_names", [])
        environment_values = raw.get("environment_values", {})
        if (
            not isinstance(raw.get("title"), str)
            or raw.get("kind") not in {"command", "gpu_selection", "interactive", "preflight"}
            or not isinstance(raw.get("required"), bool)
            or not isinstance(dependencies, list)
            or not all(isinstance(item, str) for item in dependencies)
            or not isinstance(commands, list)
            or not all(
                isinstance(command, list)
                and command
                and all(isinstance(argument, str) and argument for argument in command)
                for command in commands
            )
            or not isinstance(environment_names, list)
            or not all(isinstance(item, str) for item in environment_names)
            or not isinstance(environment_values, dict)
            or not all(
                isinstance(key, str) and isinstance(item, str)
                for key, item in environment_values.items()
            )
            or not set(environment_values).issubset(environment_names)
        ):
            raise QualificationError(f"qualification_phase_invalid:{identifier}")
        phases.append(
            PhaseDefinition(
                identifier=identifier,
                title=raw["title"],
                kind=raw["kind"],
                required=raw["required"],
                dependencies=tuple(dependencies),
                commands=tuple(tuple(command) for command in commands),
                environment_names=tuple(environment_names),
                environment_values=tuple(sorted(environment_values.items())),
            )
        )
    ordered: set[str] = set()
    for phase in phases:
        if any(dependency not in ordered for dependency in phase.dependencies):
            raise QualificationError(f"qualification_phase_dependency_unknown:{phase.identifier}")
        ordered.add(phase.identifier)
    return tuple(phases)


def expand_argument(argument: str, context: RunContext, attempt: Path | None) -> str:
    selected_uuid = str(context.selected_gpu["uuid"]) if context.selected_gpu else ""
    replacements = {
        "{source_root}": str(context.source_root),
        "{run_root}": str(context.run_root),
        "{phase_dir}": str(attempt) if attempt else "<PHASE_DIR>",
        "{selected_gpu_uuid}": selected_uuid,
    }
    value = argument
    for token, replacement in replacements.items():
        value = value.replace(token, replacement)
    if "{" in value or "}" in value:
        raise QualificationError("qualification_command_placeholder_unknown")
    return value


def phase_input_identity(
    phase: PhaseDefinition, context: RunContext, commands: Sequence[Sequence[str]]
) -> str:
    identity = {
        "bundle_id": context.bundle_id,
        "environment_fingerprint": context.environment_fingerprint,
        "selected_gpu_uuid": context.selected_gpu.get("uuid") if context.selected_gpu else None,
        "phase": phase.identifier,
        "commands": commands,
        "executables": [
            executable_identity(command[0], context.source_root) for command in commands
        ],
        "environment_names": sorted(phase.environment_names),
        "environment_values_sha256": sha256_bytes(canonical_json(dict(phase.environment_values))),
    }
    return sha256_bytes(canonical_json(identity))


def reusable_phase(
    phase: PhaseDefinition, context: RunContext, input_identity: str
) -> dict[str, Any] | None:
    state_path = context.run_root / "phases" / phase.identifier / "state.json"
    if not state_path.is_file() or state_path.is_symlink():
        return None
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
        result_path = safe_run_artifact(context.run_root, state["result_path"])
        if state.get("result_sha256") != sha256_file(result_path):
            return None
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if not isinstance(result, dict):
            return None
        if result.get("status") != "PASSED" or result.get("input_sha256") != input_identity:
            return None
        for relative, expected in result.get("output_hashes", {}).items():
            artifact = safe_run_artifact(context.run_root, relative)
            if sha256_file(artifact) != expected:
                return None
        return result
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
        return None


def safe_run_artifact(root: Path, raw_path: object) -> Path:
    if not isinstance(raw_path, str):
        raise ValueError("run_artifact_path_invalid")
    relative = PurePosixPath(raw_path)
    if relative.is_absolute() or ".." in relative.parts or str(relative) != raw_path:
        raise ValueError("run_artifact_path_invalid")
    path = (root / Path(*relative.parts)).resolve(strict=True)
    if not path.is_relative_to(root) or not path.is_file() or path.is_symlink():
        raise ValueError("run_artifact_path_invalid")
    return path


def run_bounded_command(
    command: Sequence[str],
    context: RunContext,
    environment: Mapping[str, str],
    stdout_stream: TextIO,
    stderr_stream: TextIO,
) -> int:
    process = subprocess.Popen(  # noqa: S603 - arguments are a validated array without a shell
        list(command),
        cwd=context.source_root,
        env=dict(environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    if process.stdout is None or process.stderr is None:
        process.kill()
        raise QualificationError("phase_subprocess_pipe_missing")
    failures: list[Exception] = []

    def drain(source: TextIO, destination: TextIO) -> None:
        retained = 0
        truncated = False
        discarding_long_line = False
        try:
            while chunk := source.readline(MAXIMUM_PHASE_LOG_LINE_CHARACTERS + 1):
                if discarding_long_line or len(chunk) > MAXIMUM_PHASE_LOG_LINE_CHARACTERS:
                    value = "<OVERSIZED_LOG_LINE_REDACTED>\n" if not discarding_long_line else ""
                    discarding_long_line = not chunk.endswith("\n")
                else:
                    value = redact(chunk, context)
                remaining = MAXIMUM_PHASE_LOG_CHARACTERS - retained
                if remaining > 0:
                    destination.write(value[:remaining])
                    retained += min(len(value), remaining)
                if len(value) > remaining and not truncated:
                    destination.write("\n<LOG_TRUNCATED_AT_BOUND>\n")
                    truncated = True
        except Exception as error:  # noqa: BLE001 - propagated after both pipes drain
            failures.append(error)
        finally:
            source.close()

    stdout_thread = threading.Thread(target=drain, args=(process.stdout, stdout_stream))
    stderr_thread = threading.Thread(target=drain, args=(process.stderr, stderr_stream))
    stdout_thread.start()
    stderr_thread.start()
    returncode = process.wait()
    stdout_thread.join()
    stderr_thread.join()
    if failures:
        raise QualificationError("phase_log_redaction_failed") from failures[0]
    stdout_stream.flush()
    stderr_stream.flush()
    os.fsync(stdout_stream.fileno())
    os.fsync(stderr_stream.fileno())
    return returncode


def run_command_phase(
    phase: PhaseDefinition,
    context: RunContext,
    attempt: Path,
    input_identity: str,
) -> dict[str, Any]:
    commands = [
        tuple(expand_argument(argument, context, attempt) for argument in command)
        for command in phase.commands
    ]

    stdout_path = attempt / "stdout.log"
    stderr_path = attempt / "stderr.log"
    started = time.monotonic()
    returncodes: list[int] = []
    with (
        stdout_path.open("w", encoding="utf-8") as stdout_stream,
        stderr_path.open("w", encoding="utf-8") as stderr_stream,
    ):
        os.chmod(stdout_path, 0o600)
        os.chmod(stderr_path, 0o600)
        for index, command in enumerate(commands):
            command_record = {
                "phase": phase.identifier,
                "command_index": index,
                "started_at_utc": utc_now(),
                "arguments": [redact(argument, context) for argument in command],
                "environment_names": sorted(phase.environment_names),
            }
            append_durable(context.run_root / "commands.jsonl", json.dumps(command_record) + "\n")
            allowed_environment = BASE_COMMAND_ENVIRONMENT | set(phase.environment_names)
            environment = {
                name: value for name, value in os.environ.items() if name in allowed_environment
            }
            environment.update(phase.environment_values)
            if context.selected_gpu:
                environment["CUDA_VISIBLE_DEVICES"] = str(context.selected_gpu["uuid"])
            returncode = run_bounded_command(
                command,
                context,
                environment,
                stdout_stream,
                stderr_stream,
            )
            returncodes.append(returncode)
            if returncode != 0:
                break
    duration = time.monotonic() - started
    status = "PASSED" if returncodes and all(code == 0 for code in returncodes) else "FAILED"
    reason = (
        "commands_passed"
        if status == "PASSED"
        else f"command_exit_{returncodes[-1] if returncodes else 127}"
    )
    output_hashes = {
        stdout_path.relative_to(context.run_root).as_posix(): sha256_file(stdout_path),
        stderr_path.relative_to(context.run_root).as_posix(): sha256_file(stderr_path),
    }
    for artifact in attempt.rglob("*"):
        if (
            artifact.is_file()
            and not artifact.is_symlink()
            and artifact not in {stdout_path, stderr_path}
        ):
            output_hashes[artifact.relative_to(context.run_root).as_posix()] = sha256_file(artifact)
    return {
        "schema_version": 1,
        "phase": phase.identifier,
        "title": phase.title,
        "status": status,
        "reason": reason,
        "required": phase.required,
        "input_sha256": input_identity,
        "commands": [[redact(argument, context) for argument in command] for command in commands],
        "environment_names": sorted(phase.environment_names),
        "returncodes": returncodes,
        "duration_seconds": duration,
        "output_hashes": output_hashes,
        "reused": False,
    }


def run_gpu_selection_phase(
    phase: PhaseDefinition, context: RunContext, attempt: Path
) -> dict[str, Any]:
    started = time.monotonic()
    selected, decisions = select_gpu(context)
    atomic_json(
        attempt / "gpu-selection.json",
        {"policy": "model_priority_then_uuid", "candidates": decisions},
    )
    status = "PASSED" if selected else "BLOCKED"
    reason = "gpu_selected" if selected else "no_gpu_passed_nvenc_capability_probe"
    if selected:
        context.selected_gpu = selected
        lock_name = f"gpu-{sha256_bytes(str(selected['uuid']).encode())[:32]}"
        context.gpu_lock = acquire_lock(context, lock_name, gpu_uuid=str(selected["uuid"]))
        context.sampler = ResourceSampler(context, str(selected["uuid"]), context.gpu_lock)
        context.sampler.start()
    selection_path = attempt / "gpu-selection.json"
    return {
        "schema_version": 1,
        "phase": phase.identifier,
        "title": phase.title,
        "status": status,
        "reason": reason,
        "required": phase.required,
        "input_sha256": sha256_bytes(
            canonical_json({"bundle_id": context.bundle_id, "candidates": decisions})
        ),
        "commands": [],
        "environment_names": [],
        "returncodes": [],
        "duration_seconds": time.monotonic() - started,
        "output_hashes": {
            selection_path.relative_to(context.run_root).as_posix(): sha256_file(selection_path)
        },
        "reused": False,
    }


def run_preflight_phase(
    phase: PhaseDefinition, context: RunContext, attempt: Path
) -> dict[str, Any]:
    required_commands = [
        "bash",
        "cmake",
        "corepack",
        "ctest",
        "c++",
        "ffmpeg",
        "git",
        "make",
        "ninja",
        "node",
        "nvcc",
        "nvidia-smi",
        "python3",
        "tshark",
        "uv",
    ]
    missing = [command for command in required_commands if shutil.which(command) is None]
    facts = {
        "schema_version": 1,
        "required_commands": required_commands,
        "missing_commands": missing,
        "linux": sys.platform.startswith("linux"),
        "x86_64": platform.machine() == "x86_64",
        "python": list(sys.version_info[:3]),
    }
    artifact = attempt / "preflight.json"
    atomic_json(artifact, facts)
    passed = not missing and facts["linux"] and facts["x86_64"] and sys.version_info >= (3, 12)
    return {
        "schema_version": 1,
        "phase": phase.identifier,
        "title": phase.title,
        "status": "PASSED" if passed else "BLOCKED",
        "reason": "target_prerequisites_available" if passed else "target_prerequisites_missing",
        "required": phase.required,
        "input_sha256": sha256_bytes(canonical_json(facts)),
        "commands": [],
        "environment_names": [],
        "returncodes": [],
        "duration_seconds": 0.0,
        "output_hashes": {artifact.relative_to(context.run_root).as_posix(): sha256_file(artifact)},
        "reused": False,
    }


def write_phase_result(
    context: RunContext, phase: PhaseDefinition, attempt: Path, result: dict[str, Any]
) -> None:
    result["completed_at_utc"] = utc_now()
    result_path = attempt / "result.json"
    atomic_json(result_path, result)
    relative = result_path.relative_to(context.run_root).as_posix()
    atomic_json(
        context.run_root / "phases" / phase.identifier / "state.json",
        {
            "schema_version": 1,
            "phase": phase.identifier,
            "status": result["status"],
            "result_path": relative,
            "result_sha256": sha256_file(result_path),
        },
    )


def phase_attempt_directory(context: RunContext, phase: PhaseDefinition) -> Path:
    parent = context.run_root / "phases" / phase.identifier
    parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    attempt = parent / f"{datetime.now(tz=UTC).strftime('%Y%m%dT%H%M%S.%fZ')}-{os.getpid()}"
    attempt.mkdir(mode=0o700)
    fsync_directory(parent)
    return attempt


def blocked_result(phase: PhaseDefinition, reason: str) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "phase": phase.identifier,
        "title": phase.title,
        "status": "BLOCKED",
        "reason": reason,
        "required": phase.required,
        "input_sha256": "",
        "commands": [],
        "environment_names": [],
        "returncodes": [],
        "duration_seconds": 0.0,
        "output_hashes": {},
        "reused": False,
    }


def interactive_result(phase: PhaseDefinition, context: RunContext) -> dict[str, Any]:
    desktop = context.environment.get("desktop", {})
    available = bool(desktop.get("runtime_directory_present")) and (
        bool(desktop.get("display_present")) or bool(desktop.get("wayland_display_present"))
    )
    return {
        **blocked_result(phase, "interactive_desktop_phase_deferred"),
        "status": "DEFERRED_INTERACTIVE",
        "reason": "interactive_desktop_ready" if available else "interactive_desktop_unavailable",
    }


def top_level_status(
    results: Mapping[str, Mapping[str, Any]], phases: Sequence[PhaseDefinition]
) -> str:
    required_states = [
        str(results[phase.identifier]["status"]) for phase in phases if phase.required
    ]
    if any(state == "FAILED" for state in required_states):
        return "FAILED"
    if any(state in {"BLOCKED", "DEFERRED_INTERACTIVE"} for state in required_states):
        return "BLOCKED"
    return (
        "PASSED"
        if required_states and all(state == "PASSED" for state in required_states)
        else "FAILED"
    )


def write_status(
    context: RunContext, status: str, results: Mapping[str, Mapping[str, Any]]
) -> None:
    atomic_json(
        context.run_root / "status.json",
        {
            "schema_version": 1,
            "run_id": context.run_id,
            "bundle_id": context.bundle_id,
            "status": status,
            "updated_at_utc": utc_now(),
            "selected_gpu_uuid": context.selected_gpu.get("uuid") if context.selected_gpu else None,
            "phases": {identifier: result["status"] for identifier, result in results.items()},
            "reclaimed_locks": context.reclaimed_locks,
        },
    )


def write_junit(context: RunContext, results: Mapping[str, Mapping[str, Any]]) -> None:
    failures = sum(result["status"] == "FAILED" for result in results.values())
    skipped = sum(
        result["status"] in {"BLOCKED", "DEFERRED_INTERACTIVE"} for result in results.values()
    )
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        (
            f'<testsuite name="glyphrelay-qualification" tests="{len(results)}" '
            f'failures="{failures}" skipped="{skipped}">'
        ),
    ]
    for identifier, result in results.items():
        lines.append(f'  <testcase classname="qualification" name="{identifier}">')
        reason = str(result.get("reason", "unknown")).replace("&", "&amp;").replace('"', "&quot;")
        if result["status"] == "FAILED":
            lines.append(f'    <failure message="{reason}" />')
        elif result["status"] in {"BLOCKED", "DEFERRED_INTERACTIVE"}:
            lines.append(f'    <skipped message="{reason}" />')
        lines.append("  </testcase>")
    lines.append("</testsuite>")
    atomic_write(context.run_root / "junit.xml", ("\n".join(lines) + "\n").encode())


def write_report(
    context: RunContext,
    status: str,
    results: Mapping[str, Mapping[str, Any]],
) -> None:
    passed = [identifier for identifier, result in results.items() if result["status"] == "PASSED"]
    failed = [identifier for identifier, result in results.items() if result["status"] == "FAILED"]
    blocked = [
        identifier
        for identifier, result in results.items()
        if result["status"] in {"BLOCKED", "DEFERRED_INTERACTIVE"}
    ]
    consequences = (
        "All required phases passed, but milestone acceptance still requires repository state "
        "validation."
        if status == "PASSED"
        else "No hardware, interoperability, performance, or release claim may be made from this "
        "run."
    )
    lines = [
        "# GlyphRelay qualification report",
        "",
        f"Status: `{status}`",
        "",
        f"Bundle: `{context.bundle_id}`",
        "",
        f"Run: `{context.run_id}`",
        "",
        "## Passed phases",
        "",
        *(f"- `{identifier}`" for identifier in passed),
        "",
        "## Failed phases",
        "",
        *(f"- `{identifier}`" for identifier in failed),
        "",
        "## Blocked or interactive phases",
        "",
        *(f"- `{identifier}`" for identifier in blocked),
        "",
        "## Claim consequence",
        "",
        consequences,
        "",
    ]
    atomic_write(context.run_root / "REPORT.md", "\n".join(lines).encode())
    if blocked:
        action_lines = [
            "# User action required",
            "",
            "The following phases could not complete in the consolidated run:",
            "",
        ]
        for identifier in blocked:
            action_lines.append(f"- `{identifier}`: {results[identifier]['reason']}")
        action_lines.extend(
            [
                "",
                "Resume with the same Mac-side entry point after satisfying these prerequisites: "
                "`./scripts/gpu/qualify_cuda_pm.sh`.",
                "",
            ]
        )
        atomic_write(context.run_root / "USER_ACTION_REQUIRED.md", "\n".join(action_lines).encode())


def write_checksums(context: RunContext) -> None:
    entries: list[str] = []
    for path in sorted(context.run_root.rglob("*")):
        if path.is_dir() or path.name == "SHA256SUMS" or path.is_symlink():
            continue
        entries.append(f"{sha256_file(path)}  {path.relative_to(context.run_root).as_posix()}")
    atomic_write(context.run_root / "SHA256SUMS", ("\n".join(entries) + "\n").encode())


def export_result_bundle(context: RunContext) -> tuple[Path, Path]:
    exports = context.namespace / "exports"
    exports.mkdir(mode=0o700, exist_ok=True)
    os.chmod(exports, 0o700)
    archive = exports / f"{context.run_id}.tar"
    checksum = exports / f"{context.run_id}.tar.sha256"
    if archive.exists() or checksum.exists():
        raise QualificationError("qualification_export_already_exists")
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{context.run_id}.", dir=exports)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with tarfile.open(temporary, "w", format=tarfile.PAX_FORMAT) as output:
            for path in sorted(context.run_root.rglob("*")):
                if path.is_dir():
                    continue
                if path.is_symlink() or not path.is_file():
                    raise QualificationError("qualification_result_special_file")
                relative = path.relative_to(context.run_root).as_posix()
                information = output.gettarinfo(str(path), arcname=relative)
                information.uid = 0
                information.gid = 0
                information.uname = ""
                information.gname = ""
                with path.open("rb") as stream:
                    output.addfile(information, stream)
        os.replace(temporary, archive)
        os.chmod(archive, 0o600)
        digest = sha256_file(archive)
        atomic_write(checksum, f"{digest}  {archive.name}\n".encode())
        fsync_directory(exports)
        return archive, checksum
    finally:
        temporary.unlink(missing_ok=True)


def execute(context: RunContext, phase_manifest: Path) -> str:
    phases = load_phases(phase_manifest)
    results: dict[str, dict[str, Any]] = {}
    bundle_lock = acquire_lock(context, f"bundle-{context.bundle_id}")
    bundle_heartbeat = LockHeartbeat(bundle_lock)
    heartbeat_stopped = False
    try:
        bundle_heartbeat.start()
        context.environment = capture_environment(context)
        context.environment_fingerprint = str(context.environment["fingerprint_sha256"])
        atomic_json(context.run_root / "environment.json", context.environment)
        write_status(context, "RUNNING", results)
        for phase in phases:
            dependency_failure = next(
                (
                    dependency
                    for dependency in phase.dependencies
                    if results.get(dependency, {}).get("status") != "PASSED"
                ),
                None,
            )
            stable_commands = [
                tuple(expand_argument(argument, context, None) for argument in command)
                for command in phase.commands
            ]
            input_identity = phase_input_identity(phase, context, stable_commands)
            if phase.kind == "command" and not dependency_failure:
                reusable = reusable_phase(phase, context, input_identity)
                if reusable is not None:
                    results[phase.identifier] = {**reusable, "reused": True}
                    write_status(context, "RUNNING", results)
                    continue
            attempt = phase_attempt_directory(context, phase)
            try:
                if dependency_failure:
                    result = blocked_result(phase, f"dependency_not_passed:{dependency_failure}")
                elif phase.kind == "preflight":
                    result = run_preflight_phase(phase, context, attempt)
                elif phase.kind == "gpu_selection":
                    result = run_gpu_selection_phase(phase, context, attempt)
                elif phase.kind == "interactive":
                    result = interactive_result(phase, context)
                else:
                    result = run_command_phase(phase, context, attempt, input_identity)
            except (OSError, subprocess.SubprocessError, QualificationError) as error:
                result = blocked_result(phase, f"phase_infrastructure_error:{type(error).__name__}")
            write_phase_result(context, phase, attempt, result)
            results[phase.identifier] = result
            write_status(context, "RUNNING", results)
        status = top_level_status(results, phases)
        if context.sampler:
            context.sampler.stop()
            context.sampler = None
        bundle_heartbeat.stop()
        heartbeat_stopped = True
        write_junit(context, results)
        write_report(context, status, results)
        write_status(context, status, results)
        write_checksums(context)
        export_result_bundle(context)
        return status
    finally:
        cleanup_error: Exception | None = None
        if context.sampler:
            try:
                context.sampler.stop()
            except Exception as error:  # noqa: BLE001 - cleanup must release every lock
                cleanup_error = error
        if not heartbeat_stopped:
            try:
                bundle_heartbeat.stop()
            except Exception as error:  # noqa: BLE001 - cleanup must release every lock
                cleanup_error = cleanup_error or error
        if context.gpu_lock:
            try:
                release_lock(context.gpu_lock)
            except Exception as error:  # noqa: BLE001 - continue with bundle lock cleanup
                cleanup_error = cleanup_error or error
        try:
            release_lock(bundle_lock)
        except Exception as error:  # noqa: BLE001 - report after all cleanup attempts
            cleanup_error = cleanup_error or error
        if cleanup_error:
            raise cleanup_error


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="Run GlyphRelay remote qualification")
    root.add_argument("--namespace", required=True)
    root.add_argument("--bundle-id", required=True)
    root.add_argument("--run-id", required=True)
    root.add_argument("--phase-manifest", default="qualification/m0-phases.json")
    return root


def main() -> int:
    arguments = parser().parse_args()
    context: RunContext | None = None
    try:
        context = validate_context(arguments)
        phase_manifest = (context.source_root / arguments.phase_manifest).resolve(strict=True)
        if not phase_manifest.is_relative_to(context.source_root):
            raise QualificationError("qualification_phase_manifest_outside_source")
        status = execute(context, phase_manifest)
        return 0 if status == "PASSED" else 4 if status == "FAILED" else 5
    except (QualificationError, BundleError, OSError, ValueError, json.JSONDecodeError) as error:
        if context:
            atomic_json(
                context.run_root / "status.json",
                {
                    "schema_version": 1,
                    "run_id": context.run_id,
                    "bundle_id": context.bundle_id,
                    "status": "BLOCKED",
                    "updated_at_utc": utc_now(),
                    "selected_gpu_uuid": (
                        context.selected_gpu.get("uuid") if context.selected_gpu else None
                    ),
                    "phases": {},
                    "reclaimed_locks": context.reclaimed_locks,
                    "reason": type(error).__name__,
                },
            )
        reason = (
            str(error)
            if isinstance(error, (QualificationError, BundleError))
            else type(error).__name__
        )
        print(f"qualification blocked: {reason}", file=sys.stderr)
        return 5


if __name__ == "__main__":
    raise SystemExit(main())
