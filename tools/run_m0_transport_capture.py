#!/usr/bin/env python3
"""Capture final-hook transport fixtures for independent packet validation."""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import os
import secrets
import select
import shutil
import signal
import socket
import subprocess
import time
from pathlib import Path
from typing import Any


class TransportCaptureError(RuntimeError):
    """Raised when a capture cannot produce trustworthy target evidence."""


FIRST_PORT = 41_000
SECOND_PORT = 41_001
TURN_PORT = 34_780
TURN_RELAY_PORT_BEGIN = 49_160
TURN_RELAY_PORT_END = 49_190
MAXIMUM_DIAGNOSTIC_BYTES = 4_096
REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def exclusive_write(path: Path, content: bytes) -> None:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
        0o600,
    )
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())


def ipv6_loopback_support() -> tuple[bool, str]:
    if not socket.has_ipv6:
        return False, "socket_module_reports_ipv6_unavailable"
    probe = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    try:
        probe.bind(("::1", 0))
    except OSError as error:
        if error.errno in {errno.EAFNOSUPPORT, errno.EADDRNOTAVAIL, errno.ENODEV}:
            return False, f"loopback_bind_unsupported_errno_{error.errno}"
        raise TransportCaptureError(f"ipv6_probe_failed_errno_{error.errno}") from error
    finally:
        probe.close()
    return True, "loopback_bind_supported"


def wait_for_capture_ready(process: subprocess.Popen[bytes]) -> None:
    assert process.stderr is not None
    deadline = time.monotonic() + 10.0
    diagnostic = bytearray()
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        readable, _, _ = select.select([process.stderr], [], [], 0.1)
        if not readable:
            continue
        line = process.stderr.readline()
        diagnostic.extend(line[: max(0, MAXIMUM_DIAGNOSTIC_BYTES - len(diagnostic))])
        if b"Capturing on" in line:
            return
    message = bytes(diagnostic).decode("utf-8", "replace")
    raise TransportCaptureError(f"tshark_capture_start_failed:{message}")


def stop_capture(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        if process.returncode not in {0, 130}:
            raise TransportCaptureError(f"tshark_exited_early:{process.returncode}")
        return
    os.killpg(process.pid, signal.SIGINT)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired as error:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)
        raise TransportCaptureError("tshark_capture_stop_timeout") from error
    if process.returncode not in {0, 130}:
        raise TransportCaptureError(f"tshark_capture_stop_failed:{process.returncode}")


def start_capture(path: Path) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(
        [
            "tshark",
            "-q",
            "-i",
            "lo",
            "-f",
            f"udp and (src port {FIRST_PORT} or src port {SECOND_PORT})",
            "-w",
            str(path),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env={"PATH": os.environ.get("PATH", "")},
        start_new_session=True,
    )
    try:
        wait_for_capture_ready(process)
    except Exception:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        raise
    return process


def run_fixture(
    executable: Path,
    scenario: str,
    output: Path,
    environment: dict[str, str] | None = None,
) -> None:
    completed = subprocess.run(
        [str(executable), "--scenario", scenario, "--output", str(output)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=45,
        env={"PATH": os.environ.get("PATH", ""), **(environment or {})},
        check=False,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr[:MAXIMUM_DIAGNOSTIC_BYTES].decode("utf-8", "replace")
        raise TransportCaptureError(
            f"transport_fixture_failed:{scenario}:{completed.returncode}:{diagnostic}"
        )


def capture_scenario(
    executable: Path,
    output: Path,
    scenario: str,
    environment: dict[str, str] | None = None,
) -> dict[str, Any]:
    fixture = output / f"{scenario}-fixture.json"
    packet_capture = output / f"{scenario}.pcapng"
    capture = start_capture(packet_capture)
    fixture_error: Exception | None = None
    try:
        run_fixture(executable, scenario, fixture, environment)
    except Exception as error:  # noqa: BLE001 - capture must still be closed durably
        fixture_error = error
    capture_error: Exception | None = None
    try:
        stop_capture(capture)
    except Exception as error:  # noqa: BLE001 - both failures must remain visible
        capture_error = error
    if fixture_error is not None:
        raise fixture_error
    if capture_error is not None:
        raise capture_error
    if not fixture.is_file() or not packet_capture.is_file() or packet_capture.stat().st_size == 0:
        raise TransportCaptureError(f"transport_capture_artifact_missing:{scenario}")
    return {
        "scenario": scenario,
        "support": "SUPPORTED",
        "supportReason": "fixture_and_packet_capture_completed",
        "fixtureFile": fixture.name,
        "fixtureSha256": sha256_file(fixture),
        "packetCaptureFile": packet_capture.name,
        "packetCaptureSha256": sha256_file(packet_capture),
    }


def coturn_image() -> str:
    lock = json.loads((REPOSITORY_ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))
    image = lock["coturn"]["image"]
    digest = lock["coturn"]["linux_amd64_digest"]
    if (
        not isinstance(image, str)
        or not isinstance(digest, str)
        or not digest.startswith("sha256:")
    ):
        raise TransportCaptureError("coturn_lock_invalid")
    return f"{image}@{digest}"


def start_coturn(image: str) -> tuple[str, dict[str, str]]:
    container = f"glyphrelay-m0-turn-{secrets.token_hex(8)}"
    username = f"m0-{secrets.token_hex(8)}"
    password = secrets.token_hex(24)
    command = [
        "docker",
        "run",
        "--detach",
        "--rm",
        "--name",
        container,
        "--network",
        "host",
        image,
        "--listening-ip=127.0.0.1",
        "--relay-ip=127.0.0.1",
        f"--listening-port={TURN_PORT}",
        f"--min-port={TURN_RELAY_PORT_BEGIN}",
        f"--max-port={TURN_RELAY_PORT_END}",
        "--fingerprint",
        "--lt-cred-mech",
        "--realm=glyphrelay.invalid",
        f"--user={username}:{password}",
        "--no-cli",
        "--no-tls",
        "--no-dtls",
        "--allow-loopback-peers",
        "--no-software-attribute",
        "--log-file=stdout",
    ]
    completed = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
        env={"PATH": os.environ.get("PATH", "")},
        check=False,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr[:MAXIMUM_DIAGNOSTIC_BYTES].decode("utf-8", "replace")
        raise TransportCaptureError(f"coturn_start_failed:{completed.returncode}:{diagnostic}")
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        inspected = subprocess.run(
            ["docker", "inspect", "--format", "{{.State.Running}}", container],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=10,
            env={"PATH": os.environ.get("PATH", "")},
            check=False,
        )
        logs = subprocess.run(
            ["docker", "logs", container],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=10,
            env={"PATH": os.environ.get("PATH", "")},
            check=False,
        )
        if (
            inspected.returncode == 0
            and inspected.stdout.strip() == b"true"
            and logs.returncode == 0
            and b"Relay ports initialization done" in logs.stdout + logs.stderr
        ):
            return container, {
                "GLYPHRELAY_M0_TURN_HOST": "127.0.0.1",
                "GLYPHRELAY_M0_TURN_PORT": str(TURN_PORT),
                "GLYPHRELAY_M0_TURN_USERNAME": username,
                "GLYPHRELAY_M0_TURN_PASSWORD": password,
            }
        time.sleep(0.1)
    try:
        stop_coturn(container)
    except TransportCaptureError as error:
        raise TransportCaptureError("coturn_readiness_and_cleanup_failed") from error
    raise TransportCaptureError("coturn_readiness_timeout")


def stop_coturn(container: str) -> None:
    completed = subprocess.run(
        ["docker", "rm", "--force", container],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=30,
        env={"PATH": os.environ.get("PATH", "")},
        check=False,
    )
    if completed.returncode != 0 and b"No such container" not in completed.stderr:
        raise TransportCaptureError(f"coturn_cleanup_failed:{completed.returncode}")


def run(executable: Path, output: Path) -> dict[str, Any]:
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise TransportCaptureError("transport_fixture_executable_invalid")
    if output.exists() or output.is_symlink():
        raise TransportCaptureError("transport_capture_output_exists")
    if shutil.which("tshark") is None or shutil.which("docker") is None:
        raise TransportCaptureError("transport_capture_required_tool_missing")
    output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    output.mkdir(mode=0o700)
    scenarios = [capture_scenario(executable, output, "direct-ipv4")]
    ipv6_supported, ipv6_reason = ipv6_loopback_support()
    if ipv6_supported:
        scenarios.append(capture_scenario(executable, output, "direct-ipv6"))
    else:
        scenarios.append(
            {
                "scenario": "direct-ipv6",
                "support": "UNSUPPORTED",
                "supportReason": ipv6_reason,
                "fixtureFile": None,
                "fixtureSha256": None,
                "packetCaptureFile": None,
                "packetCaptureSha256": None,
            }
        )
    image = coturn_image()
    container = ""
    turn_error: Exception | None = None
    cleanup_error: Exception | None = None
    try:
        container, turn_environment = start_coturn(image)
        scenarios.append(capture_scenario(executable, output, "turn-udp", turn_environment))
    except Exception as error:  # noqa: BLE001 - cleanup result must also be retained
        turn_error = error
    finally:
        if container:
            try:
                stop_coturn(container)
            except Exception as error:  # noqa: BLE001 - combined below
                cleanup_error = error
    if turn_error is not None:
        raise turn_error
    if cleanup_error is not None:
        raise cleanup_error
    manifest = {
        "schemaVersion": 1,
        "protocol": "glyphrelay-m0-transport-capture-v1",
        "status": "CAPTURED",
        "coturnImage": image,
        "scenarios": scenarios,
    }
    exclusive_write(output / "capture-manifest.json", canonical_json(manifest))
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        manifest = run(arguments.fixture.resolve(), arguments.output.resolve())
    except (OSError, TransportCaptureError, subprocess.SubprocessError) as error:
        print(json.dumps({"reason": str(error), "status": "FAILED"}, sort_keys=True))
        return 1
    print(
        json.dumps(
            {"scenarios": len(manifest["scenarios"]), "status": "CAPTURED"},
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
