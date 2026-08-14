from __future__ import annotations

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

from tools.gpu.source_bundle import (
    build_source_bundle,
    safe_extract_bundle,
    verify_result_archive,
)


def run_disposable_remote(repository_root: Path) -> None:
    repository = repository_root.resolve(strict=True)
    bundle = build_source_bundle(repository, repository / "build" / "disposable-handoff")
    with tempfile.TemporaryDirectory(prefix="glyphrelay-disposable-remote-") as temporary:
        namespace = Path(temporary) / "namespace"
        namespace.mkdir(mode=0o700)
        for name in ("exports", "locks", "runs", "sources"):
            (namespace / name).mkdir(mode=0o700)
        source = namespace / "sources" / bundle.bundle_id
        safe_extract_bundle(bundle.archive, bundle.manifest, source)
        run_id = "disposable-remote"
        completed = subprocess.run(
            [
                "python3",
                "-m",
                "tools.gpu.remote_qualification",
                "--namespace",
                str(namespace),
                "--bundle-id",
                bundle.bundle_id,
                "--run-id",
                run_id,
                "--phase-manifest",
                "qualification/disposable-phases.json",
            ],
            cwd=source,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()[-2_048:]
            raise RuntimeError(f"disposable_remote_runner_failed:{completed.returncode}:{detail}")
        extracted_result = Path(temporary) / "verified-result"
        verify_result_archive(
            namespace / f"exports/{run_id}.tar",
            namespace / f"exports/{run_id}.tar.sha256",
            extracted_result,
        )
        status = json.loads((extracted_result / "status.json").read_text(encoding="utf-8"))
        if status.get("status") != "PASSED" or status.get("bundle_id") != bundle.bundle_id:
            raise RuntimeError("disposable_remote_result_invalid")
        if not any(
            path.read_text(encoding="utf-8") == "verified\n"
            for path in extracted_result.glob("phases/resumable-command/*/evidence.txt")
        ):
            raise RuntimeError("disposable_remote_phase_artifact_missing")


def main() -> int:
    parser = argparse.ArgumentParser(description="Exercise a disposable local qualification remote")
    parser.add_argument("--repository", default=".")
    arguments = parser.parse_args()
    run_disposable_remote(Path(arguments.repository))
    print(json.dumps({"status": "PASSED", "workflow": "disposable_remote"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
