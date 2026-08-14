# ruff: noqa: E402

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any

import jsonschema

ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from tools.check_saliency_protocol import (
    validate_protocol as validate_saliency_protocol,  # noqa: E402
)
from tools.check_uniform_aq_protocol import (  # noqa: E402
    validate_grid as validate_aq_grid,
)
from tools.check_uniform_aq_protocol import (
    validate_lock as validate_aq_lock,
)
from tools.check_uniform_aq_protocol import (
    validate_selection_if_present as validate_aq_selection,
)
from tools.corpus.prepare_saliency_validation import (  # noqa: E402
    MANIFEST_PATH,
    ValidationPreparationError,
    canonical_json,
    load_object,
    prepare_bundle,
    sha256_file,
    validate_render_index,
)

SALIENCY_SELECTION_PATH = ROOT / "protocols" / "saliency_v1" / "selected-configuration.json"
AQ_SELECTION_PATH = ROOT / "protocols" / "uniform_aq_v1" / "selected-configuration.json"
VALIDATION_LOCK_PATH = ROOT / "protocols" / "saliency_validation_v1" / "manifest.lock"
IMPLEMENTATION_PATHS = (
    "include/glyphrelay/cuda_preprocess.hpp",
    "include/glyphrelay/saliency.hpp",
    "include/glyphrelay/saliency_development.hpp",
    "src/gpu/cuda_preprocess.cu",
    "src/gpu/saliency.cpp",
    "src/gpu/saliency_development.cpp",
    "tools/evaluate_saliency_validation.cpp",
)


class ValidationRunError(RuntimeError):
    """Raised when the one-shot validation contract cannot be satisfied."""


def exact_sha256(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValidationRunError(f"{label}_invalid")
    return value


def write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.parent / f".{path.name}.{os.getpid()}.tmp"
    with temporary.open("x", encoding="utf-8") as stream:
        stream.write(canonical_json(value).decode())
        stream.flush()
        os.fsync(stream.fileno())
    try:
        os.link(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
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
        raise ValidationRunError(f"{label}_must_be_private_owned_directory")


def committed_bytes(relative: str) -> bytes:
    completed = subprocess.run(
        ["git", "show", f"HEAD:{relative}"],
        cwd=ROOT,
        check=False,
        capture_output=True,
    )
    path = ROOT / relative
    if completed.returncode != 0 or not path.is_file() or completed.stdout != path.read_bytes():
        raise ValidationRunError(f"selection_not_committed:{relative}")
    return completed.stdout


def validate_schema(value: dict[str, Any], path: Path, label: str) -> None:
    schema = load_object(path)
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(value), key=str)
    if errors:
        raise ValidationRunError(f"{label}_schema_invalid:{errors[0].message}")


def validate_selections() -> tuple[dict[str, Any], dict[str, Any]]:
    saliency_bytes = committed_bytes("protocols/saliency_v1/selected-configuration.json")
    aq_bytes = committed_bytes("protocols/uniform_aq_v1/selected-configuration.json")
    saliency = load_object(SALIENCY_SELECTION_PATH)
    aq = load_object(AQ_SELECTION_PATH)
    validate_schema(saliency, ROOT / "schemas" / "saliency-selection-v1.schema.json", "saliency")
    validate_schema(aq, ROOT / "schemas" / "uniform-aq-selection-v1.schema.json", "uniform_aq")
    configuration = saliency["configuration"]
    if hashlib.sha256(canonical_json(configuration)).hexdigest() != saliency["configurationSha256"]:
        raise ValidationRunError("saliency_configuration_sha256_invalid")
    saliency_errors, _saliency_protocol_sha256 = validate_saliency_protocol()
    if saliency_errors:
        raise ValidationRunError(f"saliency_protocol_invalid:{saliency_errors[0]}")
    aq_lock_errors, _aq_protocol_sha256 = validate_aq_lock()
    aq_errors = aq_lock_errors + validate_aq_grid() + validate_aq_selection()
    if aq_errors:
        raise ValidationRunError(f"uniform_aq_protocol_invalid:{aq_errors[0]}")
    if hashlib.sha256(saliency_bytes).hexdigest() != sha256_file(SALIENCY_SELECTION_PATH):
        raise ValidationRunError("saliency_selection_worktree_changed")
    if hashlib.sha256(aq_bytes).hexdigest() != sha256_file(AQ_SELECTION_PATH):
        raise ValidationRunError("uniform_aq_selection_worktree_changed")
    return saliency, aq


def validate_protocol_lock() -> str:
    lock = load_object(VALIDATION_LOCK_PATH)
    if lock.get("schema_version") != 1 or lock.get("protocol") != "saliency_validation_v1":
        raise ValidationRunError("validation_protocol_lock_identity_invalid")
    material = bytearray()
    paths: list[str] = []
    for entry in lock.get("files", []):
        if not isinstance(entry, dict):
            raise ValidationRunError("validation_protocol_lock_entry_invalid")
        relative = entry.get("path")
        expected = entry.get("sha256")
        if not isinstance(relative, str) or not isinstance(expected, str):
            raise ValidationRunError("validation_protocol_lock_entry_invalid")
        source = ROOT / relative
        if not source.is_file() or sha256_file(source) != expected:
            raise ValidationRunError(f"validation_protocol_file_changed:{relative}")
        paths.append(relative)
        material.extend(f"{relative}\0{expected}\n".encode())
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise ValidationRunError("validation_protocol_paths_invalid")
    actual = hashlib.sha256(bytes(material)).hexdigest()
    if lock.get("protocol_sha256") != actual:
        raise ValidationRunError("validation_protocol_aggregate_invalid")
    return actual


def implementation_sha256() -> str:
    digest = hashlib.sha256()
    for relative in IMPLEMENTATION_PATHS:
        digest.update(relative.encode())
        digest.update(b"\0")
        digest.update(sha256_file(ROOT / relative).encode())
        digest.update(b"\n")
    return digest.hexdigest()


def head_commit() -> str:
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True, capture_output=True, text=True
    )
    return exact_sha256(completed.stdout.strip(), "repository_commit")


def access_identity(
    *,
    protocol_sha256: str,
    saliency: dict[str, Any],
    aq: dict[str, Any],
    source_bundle_id: str,
    processing_platform_sha256: str,
) -> dict[str, Any]:
    corpus_lock = load_object(ROOT / "protocols" / "corpus_protocol_v1" / "manifest.lock")
    return {
        "schemaVersion": 1,
        "protocol": "saliency_validation_v1",
        "accessOrdinal": 1,
        "repositoryCommit": head_commit(),
        "sourceBundleId": source_bundle_id,
        "processingPlatformSha256": processing_platform_sha256,
        "validationProtocolSha256": protocol_sha256,
        "corpusProtocolSha256": exact_sha256(
            corpus_lock.get("protocol_sha256"), "corpus_protocol_sha256"
        ),
        "validationManifestSha256": sha256_file(MANIFEST_PATH),
        "saliencySelectionSha256": sha256_file(SALIENCY_SELECTION_PATH),
        "saliencyConfigurationSha256": exact_sha256(
            saliency.get("configurationSha256"), "saliency_configuration_sha256"
        ),
        "uniformAqSelectionSha256": sha256_file(AQ_SELECTION_PATH),
        "uniformAqEffectiveEncoderFieldsSha256": exact_sha256(
            aq.get("bestSupportedUniform", {}).get("effectiveEncoderFieldsSha256"),
            "uniform_aq_effective_fields_sha256",
        ),
        "automaticMapImplementationSha256": implementation_sha256(),
    }


def open_or_resume_ledger(checkpoint_root: Path, identity: dict[str, Any]) -> dict[str, Any]:
    ledger_path = checkpoint_root / "access-ledger.json"
    renderer = checkpoint_root / "render"
    if not ledger_path.exists():
        if renderer.exists():
            raise ValidationRunError("validation_render_exists_without_access_ledger")
        ledger = {
            **identity,
            "openedAtUtc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
        }
        write_json_exclusive(ledger_path, ledger)
        return ledger
    ledger = load_object(ledger_path)
    opened = ledger.get("openedAtUtc")
    if (
        not isinstance(opened, str)
        or {key: value for key, value in ledger.items() if key != "openedAtUtc"} != identity
    ):
        raise ValidationRunError("validation_access_ledger_identity_mismatch")
    return ledger


def render_validation(
    checkpoint_root: Path, access_ledger_sha256: str
) -> tuple[dict[str, Any], str]:
    renderer = checkpoint_root / "render"
    manifest = load_object(MANIFEST_PATH)
    if not renderer.exists():
        built = subprocess.run(["bash", "scripts/build_corpus_image.sh"], cwd=ROOT, check=False)
        if built.returncode != 0:
            raise ValidationRunError("validation_renderer_image_build_failed")
        command = [
            "docker",
            "run",
            "--rm",
            "--init",
            "--ipc=host",
            "--platform",
            "linux/amd64",
            "--volume",
            f"{ROOT}:/workspace:ro",
            "--volume",
            f"{checkpoint_root}:/validation",
            "--workdir",
            "/workspace",
            "--env",
            "HOME=/tmp",
            "--env",
            "PLAYWRIGHT_BROWSERS_PATH=/ms-playwright",
            "glyphrelay-corpus:protocol-v1",
            "node",
            "tooling/corpus/render-validation.ts",
            "--access-ledger",
            "/validation/access-ledger.json",
            "--access-ledger-sha256",
            access_ledger_sha256,
            "--manifest",
            "corpus/manifests/validation.json",
            "--output",
            "/validation/render",
        ]
        if subprocess.run(command, cwd=ROOT, check=False).returncode != 0:
            raise ValidationRunError("validation_renderer_failed")
    indexed, render_sha256 = validate_render_index(renderer, manifest)
    return {"frames": len(indexed)}, render_sha256


def seal_render(checkpoint_root: Path, ledger_sha256: str, render_sha256: str) -> dict[str, Any]:
    path = checkpoint_root / "render-seal.json"
    expected = {
        "schemaVersion": 1,
        "protocol": "saliency_validation_v1",
        "accessLedgerSha256": ledger_sha256,
        "validationRenderIndexSha256": render_sha256,
    }
    if not path.exists():
        write_json_exclusive(path, expected)
    elif load_object(path) != expected:
        raise ValidationRunError("validation_render_seal_identity_mismatch")
    return expected


def run_lossless_ocr(checkpoint_root: Path) -> dict[str, Any]:
    output_directory = checkpoint_root / "ocr-results"
    temporary_output = checkpoint_root / ".ocr-results.tmp"
    report_path = checkpoint_root / "lossless-ocr.json"
    if not output_directory.exists():
        if temporary_output.exists():
            shutil.rmtree(temporary_output)
        command = [
            "docker",
            "run",
            "--rm",
            "--init",
            "--platform",
            "linux/amd64",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "--volume",
            f"{ROOT}:/workspace:ro",
            "--volume",
            f"{checkpoint_root}:/validation",
            "--workdir",
            "/workspace",
            "glyphrelay-corpus:protocol-v1",
            "bash",
            "tools/corpus/run_tesseract.sh",
            "/validation/render/ocr-inputs",
            "/validation/.ocr-results.tmp",
        ]
        if subprocess.run(command, cwd=ROOT, check=False).returncode != 0:
            raise ValidationRunError("validation_tesseract_failed")
        temporary_output.replace(output_directory)
    if not report_path.exists():
        command = [
            sys.executable,
            "tools/corpus/evaluate_validation_ocr.py",
            "--manifest",
            str(MANIFEST_PATH),
            "--ocr-results",
            str(output_directory),
            "--output",
            str(report_path),
        ]
        completed = subprocess.run(command, cwd=ROOT, check=False)
        if completed.returncode not in (0, 9):
            raise ValidationRunError("validation_ocr_evaluator_failed")
    return load_object(report_path)


def run_native(
    native: Path,
    bundle: Path,
    bundle_sha256: str,
    output: Path,
    identity: dict[str, Any],
    render_sha256: str,
    configuration: dict[str, Any],
) -> None:
    options = (
        ("gradient-weight", "gradientWeight"),
        ("contrast-weight", "contrastWeight"),
        ("edge-pair-weight", "edgePairWeight"),
        ("small-structure-weight", "smallStructureWeight"),
        ("entry-threshold", "entryThreshold"),
        ("exit-threshold", "exitThreshold"),
        ("previous-score-coefficient", "previousScoreCoefficient"),
        ("dilation-radius-tiles", "dilationRadiusTiles"),
    )
    command = [
        str(native),
        "--bundle",
        str(bundle),
        "--bundle-sha256",
        bundle_sha256,
        "--output",
        str(output),
        "--source-bundle-id",
        identity["sourceBundleId"],
        "--automatic-map-implementation-sha256",
        identity["automaticMapImplementationSha256"],
        "--processing-platform-sha256",
        identity["processingPlatformSha256"],
        "--corpus-protocol-sha256",
        identity["corpusProtocolSha256"],
        "--validation-manifest-sha256",
        identity["validationManifestSha256"],
        "--validation-render-index-sha256",
        render_sha256,
        "--configuration-sha256",
        identity["saliencyConfigurationSha256"],
    ]
    for option, name in options:
        command.extend((f"--{option}", str(configuration[name])))
    if subprocess.run(command, cwd=ROOT, check=False).returncode != 0:
        raise ValidationRunError("validation_native_failed")


def coverage(manifest: dict[str, Any]) -> dict[str, Any]:
    sequences = manifest["sequences"]
    return {
        "themeIds": sorted({sequence["themeId"] for sequence in sequences}),
        "rapidScrollSequenceCount": sum(
            sequence["stratum"] == "animated_typing_scrolling"
            and sequence["motionCategory"] == "rapid"
            for sequence in sequences
        ),
        "cursorOrCaretSampleCount": sum(
            primitive["type"] == "caret"
            for sequence in sequences
            for frame in sequence["sampleFrames"]
            for primitive in frame["uiPrimitives"]
        ),
        "embeddedVideoSequenceCount": sum(
            sequence["stratum"] == "mixed_video_text" for sequence in sequences
        ),
        "smallGlyphCount": sum(
            glyph.get("smallGlyphSubset") is True
            for sequence in sequences
            for frame in sequence["sampleFrames"]
            for region in frame["textRegions"]
            for glyph in region["glyphs"]
        ),
    }


def failure_scenes(map_evidence: dict[str, Any]) -> list[dict[str, Any]]:
    sequences = map_evidence["metrics"]["perSequence"]
    ranked = sorted(
        sequences,
        key=lambda item: (
            -(1 - item["metrics"]["overallGlyphRecall"])
            - (1 - item["metrics"]["smallGlyphRecall"])
            - item["metrics"]["falseProtectedFraction"]
            - item["metrics"]["protectedFraction"]
            - item["metrics"]["staticMapChangeFraction"],
            item["sequenceId"],
        ),
    )
    return ranked[:8]


def validation_status(ocr: dict[str, Any], map_evidence: dict[str, Any]) -> str:
    try:
        ocr_passed = ocr["overallBoundedCer"] <= 0.02 and ocr["smallGlyphBoundedCer"] <= 0.05
        metrics = map_evidence["metrics"]
        map_passed = (
            metrics["overallGlyphRecall"] >= 0.90
            and metrics["smallGlyphRecall"] >= 0.80
            and metrics["protectedFraction"] <= 0.35
            and metrics["falseProtectedFraction"] <= 0.15
            and metrics["staticMapChangeFraction"] <= 0.02
        )
    except (KeyError, TypeError) as error:
        raise ValidationRunError("validation_gate_measurement_missing") from error
    expected_ocr = "PASSED" if ocr_passed else "INSUFFICIENT_EVIDENCE"
    expected_map = "PASSED" if map_passed else "INSUFFICIENT_EVIDENCE"
    if ocr.get("status") != expected_ocr:
        raise ValidationRunError("validation_ocr_status_does_not_match_measurements")
    if map_evidence.get("status") != expected_map:
        raise ValidationRunError("validation_map_status_does_not_match_measurements")
    return "PASSED" if ocr_passed and map_passed else "INSUFFICIENT_EVIDENCE"


def validate_map_identity(
    map_evidence: dict[str, Any],
    identity: dict[str, Any],
    render_sha256: str,
    configuration: dict[str, Any],
) -> None:
    expected = {
        "sourceBundleId": identity["sourceBundleId"],
        "automaticMapImplementationSha256": identity["automaticMapImplementationSha256"],
        "processingPlatformSha256": identity["processingPlatformSha256"],
        "corpusProtocolSha256": identity["corpusProtocolSha256"],
        "validationManifestSha256": identity["validationManifestSha256"],
        "validationRenderIndexSha256": render_sha256,
        "configurationSha256": identity["saliencyConfigurationSha256"],
        "configuration": configuration,
    }
    for name, value in expected.items():
        if map_evidence.get(name) != value:
            raise ValidationRunError(f"validation_map_identity_mismatch:{name}")


def assemble_evidence(
    *,
    phase_output: Path,
    identity: dict[str, Any],
    ledger_sha256: str,
    seal_sha256: str,
    render_sha256: str,
    saliency: dict[str, Any],
    ocr: dict[str, Any],
    map_evidence: dict[str, Any],
) -> dict[str, Any]:
    manifest = load_object(MANIFEST_PATH)
    validate_map_identity(map_evidence, identity, render_sha256, saliency["configuration"])
    status = validation_status(ocr, map_evidence)
    result = {
        "schemaVersion": 1,
        "protocol": "saliency_validation_v1",
        "split": "validation",
        "status": status,
        "sourceBundleId": identity["sourceBundleId"],
        "processingPlatformSha256": identity["processingPlatformSha256"],
        "validationProtocolSha256": identity["validationProtocolSha256"],
        "corpusProtocolSha256": identity["corpusProtocolSha256"],
        "validationManifestSha256": identity["validationManifestSha256"],
        "validationRenderIndexSha256": render_sha256,
        "accessLedgerSha256": ledger_sha256,
        "renderSealSha256": seal_sha256,
        "saliencySelectionSha256": identity["saliencySelectionSha256"],
        "saliencyConfigurationSha256": saliency["configurationSha256"],
        "uniformAqSelectionSha256": identity["uniformAqSelectionSha256"],
        "uniformAqEffectiveEncoderFieldsSha256": identity["uniformAqEffectiveEncoderFieldsSha256"],
        "losslessOcr": ocr,
        "mapEvaluation": map_evidence,
        "coverage": coverage(manifest),
        "failureScenes": failure_scenes(map_evidence),
    }
    validate_schema(
        result,
        ROOT / "schemas" / "saliency-validation-evidence-v1.schema.json",
        "validation_evidence",
    )
    evidence_path = phase_output / "validation-evidence.json"
    if not evidence_path.exists():
        write_json_exclusive(evidence_path, result)
    elif canonical_json(load_object(evidence_path)) != canonical_json(result):
        raise ValidationRunError("validation_evidence_immutable_mismatch")
    return result


def reproduce(checkpoint_root: Path, phase_output: Path) -> int:
    required = (
        checkpoint_root / "access-ledger.json",
        checkpoint_root / "render-seal.json",
        checkpoint_root / "render" / "render-index.json",
        checkpoint_root / "lossless-ocr.json",
        phase_output / "map-evidence.json",
        phase_output / "validation-evidence.json",
    )
    if any(not path.is_file() for path in required):
        raise ValidationRunError("validation_reproduction_artifact_missing")
    evidence = load_object(required[-1])
    expected_status = validation_status(evidence["losslessOcr"], evidence["mapEvaluation"])
    if evidence.get("status") != expected_status:
        raise ValidationRunError("validation_reproduction_status_mismatch")
    if evidence["accessLedgerSha256"] != sha256_file(required[0]):
        raise ValidationRunError("validation_reproduction_ledger_mismatch")
    if evidence["renderSealSha256"] != sha256_file(required[1]):
        raise ValidationRunError("validation_reproduction_seal_mismatch")
    if evidence["validationRenderIndexSha256"] != sha256_file(required[2]):
        raise ValidationRunError("validation_reproduction_render_mismatch")
    validate_schema(
        evidence,
        ROOT / "schemas" / "saliency-validation-evidence-v1.schema.json",
        "validation_reproduction",
    )
    print(json.dumps({"status": evidence["status"], "reproduction": "VERIFIED"}, sort_keys=True))
    return 0 if evidence["status"] == "PASSED" else 9


def execute(
    *,
    native: Path,
    checkpoint_root: Path,
    phase_output: Path,
    source_bundle_id: str,
    processing_platform_sha256: str,
    reproduction: bool,
) -> int:
    require_private_directory(phase_output, "validation_phase_output")
    require_private_directory(checkpoint_root.parent, "validation_phase_root")
    if checkpoint_root.name != "validation-checkpoint" or checkpoint_root.parent == Path(
        checkpoint_root.anchor
    ):
        raise ValidationRunError("validation_checkpoint_path_invalid")
    checkpoint_root.mkdir(mode=0o700, exist_ok=True)
    require_private_directory(checkpoint_root, "validation_checkpoint")
    source_bundle_id = exact_sha256(source_bundle_id, "source_bundle_id")
    processing_platform_sha256 = exact_sha256(
        processing_platform_sha256, "processing_platform_sha256"
    )
    saliency, aq = validate_selections()
    protocol_sha256 = validate_protocol_lock()
    identity = access_identity(
        protocol_sha256=protocol_sha256,
        saliency=saliency,
        aq=aq,
        source_bundle_id=source_bundle_id,
        processing_platform_sha256=processing_platform_sha256,
    )
    if reproduction:
        ledger = load_object(checkpoint_root / "access-ledger.json")
        if {key: value for key, value in ledger.items() if key != "openedAtUtc"} != identity:
            raise ValidationRunError("validation_reproduction_identity_mismatch")
        return reproduce(checkpoint_root, phase_output)
    if (phase_output / "validation-evidence.json").exists():
        raise ValidationRunError("validation_complete_requires_reproduction_mode")
    expected_native = ROOT / "build" / "linux-gpu" / "glyphrelay_saliency_validation"
    if native != expected_native or not native.is_file() or not os.access(native, os.X_OK):
        raise ValidationRunError("validation_native_missing_or_not_executable")
    open_or_resume_ledger(checkpoint_root, identity)
    ledger_sha256 = sha256_file(checkpoint_root / "access-ledger.json")
    unused_render_summary, render_sha256 = render_validation(checkpoint_root, ledger_sha256)
    del unused_render_summary
    seal_render(checkpoint_root, ledger_sha256, render_sha256)
    seal_sha256 = sha256_file(checkpoint_root / "render-seal.json")
    ocr = run_lossless_ocr(checkpoint_root)
    prepared = prepare_bundle(
        checkpoint_root / "render",
        checkpoint_root / "input",
        corpus_protocol_sha256=identity["corpusProtocolSha256"],
        configuration_sha256=saliency["configurationSha256"],
    )
    map_path = phase_output / "map-evidence.json"
    if not map_path.exists():
        run_native(
            native,
            prepared.path,
            prepared.sha256,
            map_path,
            identity,
            render_sha256,
            saliency["configuration"],
        )
    map_evidence = load_object(map_path)
    evidence = assemble_evidence(
        phase_output=phase_output,
        identity=identity,
        ledger_sha256=ledger_sha256,
        seal_sha256=seal_sha256,
        render_sha256=render_sha256,
        saliency=saliency,
        ocr=ocr,
        map_evidence=map_evidence,
    )
    print(
        json.dumps(
            {
                "accessLedgerSha256": ledger_sha256,
                "status": evidence["status"],
                "validationRenderIndexSha256": render_sha256,
            },
            sort_keys=True,
        )
    )
    return 0 if evidence["status"] == "PASSED" else 9


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the one-shot saliency validation gate")
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--checkpoint-root", type=Path, required=True)
    parser.add_argument("--phase-output", type=Path, required=True)
    parser.add_argument("--source-bundle-id", required=True)
    parser.add_argument("--processing-platform-sha256", required=True)
    parser.add_argument("--reproduction", action="store_true")
    arguments = parser.parse_args()
    try:
        return execute(
            native=arguments.native.resolve(),
            checkpoint_root=arguments.checkpoint_root.resolve(),
            phase_output=arguments.phase_output.resolve(),
            source_bundle_id=arguments.source_bundle_id,
            processing_platform_sha256=arguments.processing_platform_sha256,
            reproduction=arguments.reproduction,
        )
    except (
        json.JSONDecodeError,
        OSError,
        subprocess.SubprocessError,
        ValidationPreparationError,
        ValidationRunError,
    ) as error:
        print(f"saliency validation run failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
