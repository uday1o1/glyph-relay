from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from tools.corpus.prepare_saliency_development import (  # noqa: E402
    GRID_PATH,
    DevelopmentPreparationError,
    load_object,
    prepare_bundle,
    sha256_file,
    validate_render_index,
)
from tools.corpus.saliency_selector import (  # noqa: E402
    SaliencySelectionError,
    run_selection,
)
from tools.validate_saliency_selection import validate_artifacts  # noqa: E402

IMPLEMENTATION_PATHS = (
    "CMakeLists.txt",
    "include/glyphrelay/cuda_preprocess.hpp",
    "include/glyphrelay/saliency.hpp",
    "include/glyphrelay/saliency_development.hpp",
    "src/core/color_conversion.cpp",
    "src/gpu/cuda_preprocess.cu",
    "src/gpu/preprocess_pool.cpp",
    "src/gpu/saliency.cpp",
    "src/gpu/saliency_development.cpp",
    "tools/corpus/prepare_saliency_development.py",
    "tools/evaluate_saliency_development.cpp",
    "tools/run_saliency_development.py",
)


class DevelopmentRunError(RuntimeError):
    """Raised when target development selection cannot produce trusted evidence."""


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def implementation_sha256(root: Path = ROOT) -> str:
    digest = hashlib.sha256()
    for relative in IMPLEMENTATION_PATHS:
        source = root / relative
        if not source.is_file():
            raise DevelopmentRunError(f"automatic_map_implementation_file_missing:{relative}")
        digest.update(relative.encode())
        digest.update(b"\0")
        digest.update(sha256_file(source).encode())
        digest.update(b"\n")
    return digest.hexdigest()


def render_development(renderer_directory: Path) -> None:
    grid = load_object(GRID_PATH)
    manifest = load_object(ROOT / "corpus" / "manifests" / "development.json")
    if renderer_directory.exists():
        validate_render_index(renderer_directory, manifest, grid)
        return
    renderer_directory.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    built = subprocess.run(
        ["bash", "scripts/build_corpus_image.sh"],
        cwd=ROOT,
        check=False,
    )
    if built.returncode != 0:
        raise DevelopmentRunError("development_renderer_image_build_failed")
    command = [
        "docker",
        "run",
        "--rm",
        "--init",
        "--ipc=host",
        "--platform",
        "linux/amd64",
        "--volume",
        f"{ROOT}:/workspace",
        "--volume",
        f"{renderer_directory.parent}:/glyph-output",
        "--workdir",
        "/workspace",
        "--env",
        "HOME=/tmp",
        "--env",
        "PLAYWRIGHT_BROWSERS_PATH=/ms-playwright",
        "glyphrelay-corpus:protocol-v1",
        "node",
        "tooling/corpus/render-development.ts",
        "--manifest",
        "corpus/manifests/development.json",
        "--output",
        f"/glyph-output/{renderer_directory.name}",
    ]
    rendered = subprocess.run(command, cwd=ROOT, check=False)
    if rendered.returncode != 0:
        raise DevelopmentRunError("development_renderer_failed")
    validate_render_index(renderer_directory, manifest, grid)


def exact_sha256(value: str, label: str) -> str:
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise DevelopmentRunError(f"{label}_invalid")
    return value


def write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    with path.open("x", encoding="utf-8") as stream:
        stream.write(canonical_json(value).decode())
        stream.flush()
        os.fsync(stream.fileno())
    descriptor = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def require_private_directory(path: Path, label: str) -> None:
    details = path.lstat()
    if (
        not stat.S_ISDIR(details.st_mode)
        or stat.S_ISLNK(details.st_mode)
        or details.st_uid != os.getuid()
        or stat.S_IMODE(details.st_mode) & 0o077
    ):
        raise DevelopmentRunError(f"{label}_must_be_private_owned_directory")


def run_native(
    native: Path,
    bundle_path: Path,
    bundle_sha256: str,
    checkpoint: Path,
    evidence_path: Path,
    identities: dict[str, str],
) -> None:
    command = [
        str(native),
        "--bundle",
        str(bundle_path),
        "--bundle-sha256",
        bundle_sha256,
        "--checkpoint",
        str(checkpoint),
        "--output",
        str(evidence_path),
        "--evaluation-identity",
        identities["evaluationIdentity"],
        "--source-bundle-id",
        identities["sourceBundleId"],
        "--automatic-map-implementation-sha256",
        identities["automaticMapImplementationSha256"],
        "--processing-platform-sha256",
        identities["processingPlatformSha256"],
        "--corpus-protocol-sha256",
        identities["corpusProtocolSha256"],
        "--development-manifest-sha256",
        identities["developmentManifestSha256"],
        "--development-render-index-sha256",
        identities["developmentRenderIndexSha256"],
        "--grid-sha256",
        identities["gridSha256"],
    ]
    completed = subprocess.run(command, cwd=ROOT, check=False)
    if completed.returncode != 0:
        raise DevelopmentRunError(f"development_native_exit_{completed.returncode}")


def compare_committed_selection(generated: dict[str, Any], committed: dict[str, Any]) -> None:
    required_equal = (
        "protocol",
        "status",
        "corpusProtocolSha256",
        "developmentManifestSha256",
        "developmentRenderIndexSha256",
        "gridSha256",
        "selectorSha256",
        "automaticMapImplementationSha256",
        "configuration",
        "configurationSha256",
    )
    for name in required_equal:
        if committed.get(name) != generated.get(name):
            raise DevelopmentRunError(f"committed_saliency_selection_mismatch:{name}")


def execute(
    *,
    native: Path,
    renderer_directory: Path,
    checkpoint_root: Path,
    phase_output: Path,
    source_bundle_id: str,
    processing_platform_sha256: str,
) -> int:
    source_bundle_id = exact_sha256(source_bundle_id, "source_bundle_id")
    processing_platform_sha256 = exact_sha256(
        processing_platform_sha256, "processing_platform_sha256"
    )
    expected_native = ROOT / "build" / "linux-gpu" / "glyphrelay_saliency_development"
    if native != expected_native or not native.is_file() or not os.access(native, os.X_OK):
        raise DevelopmentRunError("development_native_missing_or_not_executable")
    require_private_directory(phase_output, "development_phase_output")
    if renderer_directory.parent != phase_output or renderer_directory.name != "render":
        raise DevelopmentRunError("development_renderer_path_invalid")
    if checkpoint_root.name != "checkpoint" or checkpoint_root.parent == Path(
        checkpoint_root.anchor
    ):
        raise DevelopmentRunError("development_checkpoint_path_invalid")
    require_private_directory(checkpoint_root.parent, "development_phase_root")
    render_development(renderer_directory)
    grid = load_object(GRID_PATH)
    automatic_sha256 = implementation_sha256()
    prepared = prepare_bundle(
        renderer_directory,
        checkpoint_root / "input",
    )
    identities = {
        "sourceBundleId": source_bundle_id,
        "automaticMapImplementationSha256": automatic_sha256,
        "processingPlatformSha256": processing_platform_sha256,
        "corpusProtocolSha256": exact_sha256(
            grid["corpusProtocolSha256"], "corpus_protocol_sha256"
        ),
        "developmentManifestSha256": exact_sha256(
            grid["developmentManifestSha256"], "development_manifest_sha256"
        ),
        "developmentRenderIndexSha256": exact_sha256(
            grid["developmentRenderIndexSha256"], "development_render_index_sha256"
        ),
        "gridSha256": sha256_file(GRID_PATH),
    }
    identities["evaluationIdentity"] = hashlib.sha256(
        canonical_json({**identities, "bundleSha256": prepared.sha256})
    ).hexdigest()
    evaluation_checkpoint = checkpoint_root / "results" / identities["evaluationIdentity"]
    evidence_path = phase_output / "development-evidence.json"
    selection_path = phase_output / "selected-configuration.json"
    if not evidence_path.exists():
        run_native(
            native,
            prepared.path,
            prepared.sha256,
            evaluation_checkpoint,
            evidence_path,
            identities,
        )
    evidence = load_object(evidence_path)
    if not selection_path.exists():
        run_selection(evidence_path, selection_path)
    selection = load_object(selection_path)
    validate_artifacts(evidence, selection)
    committed_path = ROOT / "protocols" / "saliency_v1" / "selected-configuration.json"
    if not committed_path.exists():
        handoff_path = phase_output / "freeze-handoff.json"
        if not handoff_path.exists():
            write_json_exclusive(
                handoff_path,
                {
                    "schemaVersion": 1,
                    "status": "BLOCKED",
                    "reason": "repository_selection_freeze_required",
                    "selectionArtifact": "selected-configuration.json",
                    "configurationSha256": selection["configurationSha256"],
                    "resumeCommand": "./scripts/gpu/qualify_cuda_pm.sh",
                },
            )
        return 75
    committed = load_object(committed_path)
    compare_committed_selection(selection, committed)
    accepted_path = phase_output / "selection-verification.json"
    if not accepted_path.exists():
        write_json_exclusive(
            accepted_path,
            {
                "schemaVersion": 1,
                "status": "PASSED",
                "configurationSha256": selection["configurationSha256"],
                "developmentEvidenceSha256": sha256_file(evidence_path),
                "committedSelectionSha256": sha256_file(committed_path),
            },
        )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run and freeze the complete target saliency development grid"
    )
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--renderer-directory", type=Path, required=True)
    parser.add_argument("--checkpoint-root", type=Path, required=True)
    parser.add_argument("--phase-output", type=Path, required=True)
    parser.add_argument("--source-bundle-id", required=True)
    parser.add_argument("--processing-platform-sha256", required=True)
    arguments = parser.parse_args()
    try:
        return execute(
            native=arguments.native.resolve(),
            renderer_directory=arguments.renderer_directory.resolve(),
            checkpoint_root=arguments.checkpoint_root.resolve(),
            phase_output=arguments.phase_output.resolve(),
            source_bundle_id=arguments.source_bundle_id,
            processing_platform_sha256=arguments.processing_platform_sha256,
        )
    except (
        DevelopmentPreparationError,
        DevelopmentRunError,
        OSError,
        SaliencySelectionError,
        subprocess.SubprocessError,
    ) as error:
        print(f"saliency development run failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
