from __future__ import annotations

import re
import subprocess
from pathlib import Path

SECRET_PATTERNS = {
    "private key": re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "GitHub token": re.compile(rb"\bgh[pousr]_[A-Za-z0-9_]{20,}\b"),
    "AWS access key": re.compile(rb"\bAKIA[0-9A-Z]{16}\b"),
    "assigned password": re.compile(rb"(?i)password\s*[:=]\s*['\"][^'\"\r\n]{8,}['\"]"),
}

FORBIDDEN_SUFFIXES = {
    ".h264",
    ".key",
    ".ncu-rep",
    ".nsys-rep",
    ".p12",
    ".pcap",
    ".pcapng",
}


def repository_files() -> list[Path]:
    completed = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        check=True,
        capture_output=True,
    )
    return [Path(raw.decode()) for raw in completed.stdout.split(b"\0") if raw]


def scan(paths: list[Path]) -> list[str]:
    failures: list[str] = []
    for path in paths:
        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            failures.append(f"forbidden generated or sensitive artifact: {path}")
            continue
        if not path.is_file() or path.stat().st_size > 5 * 1024 * 1024:
            continue
        content = path.read_bytes()
        for label, pattern in SECRET_PATTERNS.items():
            if pattern.search(content):
                failures.append(f"possible {label}: {path}")
    return failures


def main() -> int:
    failures = scan(repository_files())
    if failures:
        print("\n".join(failures))
        return 1
    print("repository secret and artifact scan passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
