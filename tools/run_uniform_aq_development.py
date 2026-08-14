from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import stat
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from tools.corpus.evaluate_ocr import evaluate as evaluate_ocr  # noqa: E402
from tools.corpus.prepare_saliency_development import (  # noqa: E402
    GRID_PATH as SALIENCY_GRID_PATH,
)
from tools.corpus.prepare_saliency_development import (  # noqa: E402
    DevelopmentPreparationError,
    load_object,
    prepare_bundle,
    sha256_file,
    validate_render_index,
)
from tools.corpus.uniform_aq_selector import (  # noqa: E402
    CORPUS_LOCK_PATH,
    DEVELOPMENT_MANIFEST_PATH,
    EVIDENCE_SCHEMA_PATH,
    GRID_PATH,
    SELECTION_PATH,
    AqFields,
    UniformAqSelectionError,
    expected_aq_fields,
    select_development_evidence,
    verify_protocol_lock,
)
from tools.run_saliency_development import render_development  # noqa: E402

TARGETS = (0.5, 0.75, 1.0, 2.0, 4.0)
CORE_STRATA = (
    "animated_typing_scrolling",
    "browser_documentation",
    "code_editor",
    "mixed_video_text",
    "slide_diagram",
    "spreadsheet_table",
    "terminal",
)
SAMPLE_FRAME_ORDINALS = tuple(
    sequence * 240 + sample for sequence in range(64) for sample in (0, 60, 120, 180)
)
SUBMITTED_FRAMES = 64 * 240
MAXIMUM_SEARCH_ATTEMPTS = 4
MINIMUM_REQUESTED_BPS = 100_000
MAXIMUM_REQUESTED_BPS = 20_000_000
MAXIMUM_CAPTURE_BYTES = 256 * 1024
IMPLEMENTATION_PATHS = (
    "CMakeLists.txt",
    "include/glyphrelay/nvenc_encoder.hpp",
    "src/gpu/cuda_preprocess.cu",
    "src/gpu/nvenc_encoder.cpp",
    "tooling/corpus/prepare-decoded-ocr.ts",
    "tooling/corpus/verify-browser-decode.ts",
    "tools/corpus/evaluate_ocr.py",
    "tools/corpus/run_tesseract.sh",
    "tools/evaluate_uniform_aq.cpp",
    "tools/run_uniform_aq_development.py",
)


class UniformAqRunError(RuntimeError):
    """Raised when target AQ development cannot produce trusted evidence."""


def canonical_json(value: object) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n"
    ).encode()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def exact_sha256(value: str, label: str) -> str:
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise UniformAqRunError(f"{label}_invalid")
    return value


def require_private_directory(path: Path, label: str) -> None:
    details = path.lstat()
    if (
        not stat.S_ISDIR(details.st_mode)
        or stat.S_ISLNK(details.st_mode)
        or details.st_uid != os.getuid()
        or stat.S_IMODE(details.st_mode) & 0o077
    ):
        raise UniformAqRunError(f"{label}_must_be_private_owned_directory")


def implementation_sha256(root: Path = ROOT) -> str:
    digest = hashlib.sha256()
    for relative in IMPLEMENTATION_PATHS:
        source = root / relative
        if not source.is_file():
            raise UniformAqRunError(f"uniform_aq_implementation_file_missing:{relative}")
        digest.update(relative.encode())
        digest.update(b"\0")
        digest.update(sha256_file(source).encode())
        digest.update(b"\n")
    return digest.hexdigest()


def effective_encoder_fields(fields: AqFields) -> dict[str, Any]:
    return {
        "aqFields": fields.json(),
        "bFrames": 0,
        "capacity": 4,
        "chromaFormatIdc": 1,
        "entropyCoding": "cavlc",
        "fillerDataInsertion": True,
        "frameRate": 30,
        "gopFrames": 60,
        "height": 1080,
        "inputBitDepth": 8,
        "levelIdc": 40,
        "lookahead": False,
        "maximumBusyRetries": 100,
        "maximumReferenceFrames": 1,
        "mode": "uniform",
        "multipass": "disabled",
        "outputBitDepth": 8,
        "preset": "p4",
        "profile": "baseline",
        "rateControl": "cbr_development_search_variable",
        "repeatSpsPps": True,
        "temporalLayers": 1,
        "tuning": "low_latency",
        "width": 1920,
        "zeroReorderDelay": True,
    }


def effective_encoder_fields_sha256(fields: AqFields) -> str:
    return sha256_bytes(canonical_json(effective_encoder_fields(fields)))


def condition_id(fields: AqFields) -> str:
    return "controlled_uniform" if fields == AqFields(False, 0, False) else fields.candidate_id()


def next_requested_rate(
    requested_bps: int, measured_mbps: float, target_mbps: float, attempted: set[int]
) -> int:
    if requested_bps <= 0 or not math.isfinite(measured_mbps) or measured_mbps <= 0:
        raise UniformAqRunError("uniform_aq_rate_search_measurement_invalid")
    proposed = round(requested_bps * target_mbps / measured_mbps)
    proposed = min(MAXIMUM_REQUESTED_BPS, max(MINIMUM_REQUESTED_BPS, proposed))
    if proposed not in attempted:
        return proposed
    for distance in range(1, 1025):
        for candidate in (proposed - distance, proposed + distance):
            if (
                MINIMUM_REQUESTED_BPS <= candidate <= MAXIMUM_REQUESTED_BPS
                and candidate not in attempted
            ):
                return candidate
    raise UniformAqRunError("uniform_aq_rate_search_no_unique_request")


def run_bounded(
    command: Sequence[str], *, timeout: float, label: str, cwd: Path = ROOT
) -> subprocess.CompletedProcess[bytes]:
    try:
        completed = subprocess.run(
            list(command),
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise UniformAqRunError(f"{label}_timeout") from error
    if (
        len(completed.stdout) > MAXIMUM_CAPTURE_BYTES
        or len(completed.stderr) > MAXIMUM_CAPTURE_BYTES
    ):
        raise UniformAqRunError(f"{label}_output_limit_exceeded")
    return completed


def write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            stream.write(canonical_json(value))
            stream.flush()
            os.fsync(stream.fileno())
    except Exception:
        path.unlink(missing_ok=True)
        raise
    directory = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise UniformAqRunError(f"{label}_json_invalid") from error
    if not isinstance(value, dict):
        raise UniformAqRunError(f"{label}_object_required")
    return value


def validate_native_evidence(
    evidence: dict[str, Any],
    *,
    fields: AqFields,
    identifier: str,
    requested_bps: int,
    bundle_sha256: str,
    effective_sha256: str,
    identities: dict[str, str],
    stream: Path,
) -> None:
    exact = {
        "schemaVersion": 1,
        "status": "PASSED",
        "conditionId": identifier,
        "requestedPayloadBps": requested_bps,
        "aqFields": fields.json(),
        "bundleSha256": bundle_sha256,
        "effectiveEncoderFieldsSha256": effective_sha256,
        "corpusProtocolSha256": identities["corpusProtocolSha256"],
        "developmentManifestSha256": identities["developmentManifestSha256"],
        "developmentRenderIndexSha256": identities["developmentRenderIndexSha256"],
        "saliencyGridSha256": identities["saliencyGridSha256"],
        "submittedFrames": SUBMITTED_FRAMES,
        "encodedFrames": SUBMITTED_FRAMES,
        "width": 1920,
        "height": 1080,
        "warmupFrames": 300,
        "measurementFrames": 300,
        "frameDurationNs": 33_333_333,
        "firstAccessUnitKeyframe": True,
        "firstAccessUnitParameterSets": True,
    }
    for name, expected in exact.items():
        if evidence.get(name) != expected:
            raise UniformAqRunError(f"native_evidence_identity_invalid:{name}")
    numeric = (
        "measuredPayloadMbps",
        "p95PreprocessMs",
        "p95EncodeMs",
        "p99EncodeMs",
        "p95PreprocessEncodeMs",
        "maximumPendingAgeMs",
        "meanSenderCpuPercent",
    )
    if any(
        not isinstance(evidence.get(name), (int, float))
        or isinstance(evidence.get(name), bool)
        or not math.isfinite(float(evidence[name]))
        or float(evidence[name]) < 0
        for name in numeric
    ):
        raise UniformAqRunError("native_evidence_numeric_invalid")
    buckets = evidence.get("pendingBucketMaxima")
    if (
        not isinstance(buckets, list)
        or len(buckets) != 8
        or any(
            not isinstance(value, int) or isinstance(value, bool) or value < 0 for value in buckets
        )
        or evidence.get("pendingPositiveTrend") is not (buckets[-1] > buckets[0])
    ):
        raise UniformAqRunError("native_evidence_pending_invalid")
    if (
        not stream.is_file()
        or evidence.get("streamSha256") != sha256_file(stream)
        or evidence.get("streamBytes") != stream.stat().st_size
    ):
        raise UniformAqRunError("native_evidence_stream_invalid")


def native_command(
    native: Path,
    bundle: Path,
    bundle_sha256: str,
    output: Path,
    fields: AqFields,
    requested_bps: int,
    identities: dict[str, str],
) -> list[str]:
    identifier = condition_id(fields)
    return [
        str(native),
        "--bundle",
        str(bundle),
        "--bundle-sha256",
        bundle_sha256,
        "--output",
        str(output),
        "--condition-id",
        identifier,
        "--requested-payload-bps",
        str(requested_bps),
        "--enable-aq",
        str(fields.enable_aq).lower(),
        "--aq-strength",
        str(fields.aq_strength),
        "--enable-temporal-aq",
        str(fields.enable_temporal_aq).lower(),
        "--effective-encoder-fields-sha256",
        effective_encoder_fields_sha256(fields),
        "--corpus-protocol-sha256",
        identities["corpusProtocolSha256"],
        "--development-manifest-sha256",
        identities["developmentManifestSha256"],
        "--development-render-index-sha256",
        identities["developmentRenderIndexSha256"],
        "--saliency-grid-sha256",
        identities["saliencyGridSha256"],
    ]


def run_native_trial(
    *,
    native: Path,
    bundle: Path,
    bundle_sha256: str,
    trial_root: Path,
    fields: AqFields,
    requested_bps: int,
    identities: dict[str, str],
) -> tuple[dict[str, Any] | None, str | None, Path]:
    trial_root.mkdir(mode=0o700, parents=True, exist_ok=True)
    run_index = 0
    while (trial_root / f"native-{run_index:02d}").exists():
        run_index += 1
    native_output = trial_root / f"native-{run_index:02d}"
    completed = run_bounded(
        native_command(
            native,
            bundle,
            bundle_sha256,
            native_output,
            fields,
            requested_bps,
            identities,
        ),
        timeout=1_800,
        label="uniform_aq_native",
    )
    log = {
        "returncode": completed.returncode,
        "stderr": completed.stderr.decode("utf-8", "replace"),
        "stdout": completed.stdout.decode("utf-8", "replace"),
    }
    write_json_exclusive(trial_root / f"native-{run_index:02d}-process.json", log)
    if completed.returncode != 0:
        return None, f"native_exit_{completed.returncode}", native_output
    evidence_path = native_output / "native-evidence.json"
    stream = native_output / "trial.h264"
    try:
        evidence = load_json(evidence_path, "native_evidence")
        validate_native_evidence(
            evidence,
            fields=fields,
            identifier=condition_id(fields),
            requested_bps=requested_bps,
            bundle_sha256=bundle_sha256,
            effective_sha256=effective_encoder_fields_sha256(fields),
            identities=identities,
            stream=stream,
        )
    except UniformAqRunError as error:
        return None, str(error), native_output
    return evidence, None, native_output


def independent_decode(stream: Path, output: Path) -> int:
    decoded = output / "decoded"
    decoded.mkdir(mode=0o700)
    expression = "+".join(f"eq(n\\,{ordinal})" for ordinal in SAMPLE_FRAME_ORDINALS)
    decode = run_bounded(
        [
            "ffmpeg",
            "-v",
            "error",
            "-f",
            "h264",
            "-i",
            str(stream),
            "-an",
            "-sn",
            "-dn",
            "-vf",
            f"select={expression}",
            "-fps_mode",
            "passthrough",
            "-start_number",
            "0",
            str(decoded / "%06d.png"),
        ],
        timeout=1_800,
        label="uniform_aq_ffmpeg_decode",
    )
    if decode.returncode != 0 or decode.stderr:
        raise UniformAqRunError(f"independent_decode_failed:{decode.returncode}")
    expected = [decoded / f"{index:06d}.png" for index in range(len(SAMPLE_FRAME_ORDINALS))]
    if any(not path.is_file() for path in expected) or len(list(decoded.iterdir())) != len(
        expected
    ):
        raise UniformAqRunError("independent_decode_sample_coverage_invalid")
    probe = run_bounded(
        [
            "ffprobe",
            "-v",
            "error",
            "-count_frames",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,nb_read_frames",
            "-of",
            "json",
            str(stream),
        ],
        timeout=1_800,
        label="uniform_aq_ffprobe",
    )
    if probe.returncode != 0 or probe.stderr:
        raise UniformAqRunError(f"independent_probe_failed:{probe.returncode}")
    try:
        payload = json.loads(probe.stdout)
        video = payload["streams"]
        if len(video) != 1 or video[0] != {
            "height": 1080,
            "nb_read_frames": str(SUBMITTED_FRAMES),
            "width": 1920,
        }:
            raise ValueError
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise UniformAqRunError("independent_probe_contract_invalid") from error
    return SUBMITTED_FRAMES


def prepare_ocr_and_evaluate(trial: Path) -> dict[str, Any]:
    prepared = trial / "ocr-inputs"
    preprocess = run_bounded(
        [
            "corepack",
            "pnpm",
            "run",
            "corpus:decoded-ocr",
            "--",
            "--manifest",
            str(DEVELOPMENT_MANIFEST_PATH),
            "--decoded",
            str(trial / "decoded"),
            "--output",
            str(prepared),
        ],
        timeout=1_800,
        label="uniform_aq_ocr_preprocess",
    )
    if preprocess.returncode != 0:
        raise UniformAqRunError(f"ocr_preprocess_exit_{preprocess.returncode}")
    tesseract = run_bounded(
        [
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
            f"{trial}:/trial",
            "--workdir",
            "/workspace",
            "glyphrelay-corpus:protocol-v1",
            "bash",
            "tools/corpus/run_tesseract.sh",
            "/trial/ocr-inputs",
            "/trial/ocr-results",
        ],
        timeout=1_800,
        label="uniform_aq_tesseract",
    )
    if tesseract.returncode != 0:
        raise UniformAqRunError(f"tesseract_exit_{tesseract.returncode}")
    manifest = load_object(DEVELOPMENT_MANIFEST_PATH)
    report = evaluate_ocr(manifest, trial / "ocr-results")
    write_json_exclusive(trial / "ocr-evidence.json", report)
    return report


def verify_browsers(stream: Path, trial: Path) -> dict[str, Any]:
    browser_output = trial / "browser"
    completed = run_bounded(
        [
            "corepack",
            "pnpm",
            "run",
            "corpus:browser-decode",
            "--",
            "--stream",
            str(stream),
            "--output",
            str(browser_output),
        ],
        timeout=1_800,
        label="uniform_aq_browser_decode",
    )
    if completed.returncode != 0:
        raise UniformAqRunError(f"browser_decode_exit_{completed.returncode}")
    evidence = load_json(browser_output / "evidence.json", "browser_evidence")
    browsers = evidence.get("browsers")
    if (
        evidence.get("status") != "PASSED"
        or evidence.get("streamSha256") != sha256_file(stream)
        or not isinstance(browsers, list)
        or [(item.get("browser"), item.get("browserVersion")) for item in browsers]
        != [
            ("chromium", "151.0.7922.34"),
            ("firefox", "153.0"),
        ]
        or any(item.get("status") != "PASSED" for item in browsers)
    ):
        raise UniformAqRunError("browser_decode_evidence_invalid")
    return evidence


def systems_failure(native: dict[str, Any]) -> str | None:
    predicates = (
        (float(native["p95PreprocessMs"]) <= 5.0, "preprocess_p95_exceeded"),
        (float(native["p95EncodeMs"]) <= 10.0, "encode_p95_exceeded"),
        (float(native["p99EncodeMs"]) <= 16.0, "encode_p99_exceeded"),
        (float(native["p95PreprocessEncodeMs"]) <= 15.0, "combined_p95_exceeded"),
        (float(native["maximumPendingAgeMs"]) <= 33.34, "pending_age_exceeded"),
        (native["pendingPositiveTrend"] is False, "pending_positive_trend"),
    )
    return next((reason for passed, reason in predicates if not passed), None)


def verify_matched_trial(
    native: dict[str, Any], native_output: Path, trial: Path
) -> dict[str, Any]:
    stream = native_output / "trial.h264"
    decoded_frames = independent_decode(stream, trial)
    ocr = prepare_ocr_and_evaluate(trial)
    browser = verify_browsers(stream, trial)
    failure = systems_failure(native)
    if failure:
        raise UniformAqRunError(failure)
    per_stratum_raw = ocr.get("perStratum")
    if not isinstance(per_stratum_raw, dict) or set(per_stratum_raw) != set(CORE_STRATA):
        raise UniformAqRunError("ocr_stratum_coverage_invalid")
    per_stratum = {name: float(per_stratum_raw[name]["boundedCer"]) for name in CORE_STRATA}
    equal_stratum = sum(per_stratum.values()) / len(per_stratum)
    verification = {
        "browserEvidenceSha256": sha256_file(trial / "browser" / "evidence.json"),
        "decodedFrames": decoded_frames,
        "equalStratumCer": equal_stratum,
        "independentDecodePassed": True,
        "ocrEvidenceSha256": sha256_file(trial / "ocr-evidence.json"),
        "perStratumCer": per_stratum,
        "schemaVersion": 1,
        "status": "PASSED",
    }
    write_json_exclusive(trial / "verification.json", verification)
    return {**verification, "browserDecodePassed": browser["status"] == "PASSED"}


def invalid_target(target: float, attempts: list[dict[str, Any]], reason: str) -> dict[str, Any]:
    return {
        "attempts": attempts,
        "browserDecodePassed": None,
        "decodedFrames": None,
        "equalStratumCer": None,
        "height": None,
        "independentDecodePassed": None,
        "invalidReasons": [reason],
        "maximumPendingAgeMs": None,
        "meanSenderCpuPercent": None,
        "p95EncodeMs": None,
        "p95PreprocessEncodeMs": None,
        "p95PreprocessMs": None,
        "p99EncodeMs": None,
        "pendingPositiveTrend": None,
        "perStratumCer": None,
        "selectedAttemptIndex": None,
        "status": "INVALID",
        "submittedFrames": None,
        "targetPayloadMbps": target,
        "width": None,
    }


def run_target_search(
    *,
    native: Path,
    bundle: Path,
    bundle_sha256: str,
    root: Path,
    fields: AqFields,
    target: float,
    identities: dict[str, str],
) -> dict[str, Any]:
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    target_path = root / "target.json"
    if target_path.is_file():
        value = load_json(target_path, "target_checkpoint")
        if value.get("targetPayloadMbps") != target:
            raise UniformAqRunError("target_checkpoint_identity_invalid")
        return value

    summaries = sorted(root.glob("attempt-*/search-attempt.json"))
    attempts = [load_json(path, "search_attempt_checkpoint") for path in summaries]
    if len(attempts) > MAXIMUM_SEARCH_ATTEMPTS:
        raise UniformAqRunError("target_checkpoint_attempt_count_invalid")
    attempted_rates = {
        int(attempt["requestedPayloadBps"])
        for attempt in attempts
        if isinstance(attempt.get("requestedPayloadBps"), int)
    }
    requested_bps = round(target * 1_000_000)
    if attempts:
        previous = attempts[-1]
        previous_requested = previous.get("requestedPayloadBps")
        previous_measured = previous.get("measuredPayloadMbps")
        if not isinstance(previous_requested, int):
            raise UniformAqRunError("search_attempt_checkpoint_invalid")
        if previous.get("status") == "INVALID":
            requested_bps = previous_requested
        elif isinstance(previous_measured, (int, float)) and not isinstance(
            previous_measured, bool
        ):
            measured = float(previous_measured)
            if abs(measured - target) / target <= 0.02 + 1e-12:
                result = invalid_target(target, attempts, "matched_trial_interrupted")
                write_json_exclusive(target_path, result)
                return result
            requested_bps = next_requested_rate(
                previous_requested, measured, target, attempted_rates
            )
        else:
            raise UniformAqRunError("search_attempt_checkpoint_invalid")

    def finish(value: dict[str, Any]) -> dict[str, Any]:
        write_json_exclusive(target_path, value)
        return value

    for attempt_index in range(len(attempts), MAXIMUM_SEARCH_ATTEMPTS):
        attempted_rates.add(requested_bps)
        trial = root / f"attempt-{attempt_index:02d}-{requested_bps}"
        native_evidence, native_error, native_output = run_native_trial(
            native=native,
            bundle=bundle,
            bundle_sha256=bundle_sha256,
            trial_root=trial,
            fields=fields,
            requested_bps=requested_bps,
            identities=identities,
        )
        if native_evidence is None:
            failed_attempt: dict[str, Any] = {
                "invalidReason": native_error,
                "measuredPayloadMbps": None,
                "requestedPayloadBps": requested_bps,
                "status": "INVALID",
            }
            attempts.append(failed_attempt)
            write_json_exclusive(trial / "search-attempt.json", failed_attempt)
            if attempt_index + 1 < MAXIMUM_SEARCH_ATTEMPTS:
                continue
            return finish(invalid_target(target, attempts, native_error or "native_trial_failed"))
        measured = float(native_evidence["measuredPayloadMbps"])
        attempt: dict[str, Any] = {
            "invalidReason": None,
            "measuredPayloadMbps": measured,
            "requestedPayloadBps": requested_bps,
            "status": "PASSED",
        }
        attempts.append(attempt)
        if abs(measured - target) / target <= 0.02 + 1e-12:
            verification_root = trial / f"{native_output.name}-verification"
            verification_root.mkdir(mode=0o700)
            try:
                verification = verify_matched_trial(
                    native_evidence, native_output, verification_root
                )
            except (OSError, subprocess.SubprocessError, UniformAqRunError) as error:
                reason = str(error)
                attempt["status"] = "INVALID"
                attempt["invalidReason"] = reason
                write_json_exclusive(trial / "search-attempt.json", attempt)
                return finish(invalid_target(target, attempts, reason))
            write_json_exclusive(trial / "search-attempt.json", attempt)
            return finish(
                {
                    "attempts": attempts,
                    "browserDecodePassed": verification["browserDecodePassed"],
                    "decodedFrames": verification["decodedFrames"],
                    "equalStratumCer": verification["equalStratumCer"],
                    "height": native_evidence["height"],
                    "independentDecodePassed": verification["independentDecodePassed"],
                    "invalidReasons": [],
                    "maximumPendingAgeMs": native_evidence["maximumPendingAgeMs"],
                    "meanSenderCpuPercent": native_evidence["meanSenderCpuPercent"],
                    "p95EncodeMs": native_evidence["p95EncodeMs"],
                    "p95PreprocessEncodeMs": native_evidence["p95PreprocessEncodeMs"],
                    "p95PreprocessMs": native_evidence["p95PreprocessMs"],
                    "p99EncodeMs": native_evidence["p99EncodeMs"],
                    "pendingPositiveTrend": native_evidence["pendingPositiveTrend"],
                    "perStratumCer": verification["perStratumCer"],
                    "selectedAttemptIndex": len(attempts) - 1,
                    "status": "PASSED",
                    "submittedFrames": native_evidence["submittedFrames"],
                    "targetPayloadMbps": target,
                    "width": native_evidence["width"],
                }
            )
        write_json_exclusive(trial / "search-attempt.json", attempt)
        requested_bps = next_requested_rate(requested_bps, measured, target, attempted_rates)
    return finish(invalid_target(target, attempts, "rate_target_unmatchable"))


def run_condition(
    *,
    native: Path,
    bundle: Path,
    bundle_sha256: str,
    checkpoint: Path,
    fields: AqFields,
    identities: dict[str, str],
) -> dict[str, Any]:
    identifier = condition_id(fields)
    condition_path = checkpoint / identifier / "condition.json"
    if condition_path.is_file():
        value = load_json(condition_path, "condition_checkpoint")
        if (
            value.get("candidateId") != identifier
            or value.get("effectiveAqFields") != fields.json()
            or value.get("effectiveEncoderFieldsSha256") != effective_encoder_fields_sha256(fields)
        ):
            raise UniformAqRunError("condition_checkpoint_identity_invalid")
        return value
    condition_root = condition_path.parent
    condition_root.mkdir(mode=0o700, parents=True, exist_ok=True)
    target_results = [
        run_target_search(
            native=native,
            bundle=bundle,
            bundle_sha256=bundle_sha256,
            root=condition_root / f"target-{target:g}",
            fields=fields,
            target=target,
            identities=identities,
        )
        for target in TARGETS
    ]
    reasons = sorted(
        {
            reason
            for result in target_results
            if result["status"] == "INVALID"
            for reason in result["invalidReasons"]
        }
    )
    passed = not reasons
    value = {
        "candidateId": identifier,
        "effectiveAqFields": fields.json(),
        "effectiveEncoderFieldsSha256": effective_encoder_fields_sha256(fields),
        "invalidReasons": reasons,
        "meanSenderCpuPercent": (
            sum(float(result["meanSenderCpuPercent"]) for result in target_results) / len(TARGETS)
            if passed
            else None
        ),
        "p95PreprocessEncodeMs": (
            max(float(result["p95PreprocessEncodeMs"]) for result in target_results)
            if passed
            else None
        ),
        "status": "PASSED" if passed else "INVALID",
        "targetSearch": target_results,
    }
    write_json_exclusive(condition_path, value)
    return value


def compare_committed_selection(generated: dict[str, Any], committed: dict[str, Any]) -> None:
    if generated != committed:
        raise UniformAqRunError("committed_uniform_aq_selection_mismatch")


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
    expected_native = ROOT / "build" / "linux-gpu" / "glyphrelay_uniform_aq_evaluate"
    if native != expected_native or not native.is_file() or not os.access(native, os.X_OK):
        raise UniformAqRunError("uniform_aq_native_missing_or_not_executable")
    require_private_directory(phase_output, "uniform_aq_phase_output")
    if renderer_directory.parent != phase_output or renderer_directory.name != "render":
        raise UniformAqRunError("uniform_aq_renderer_path_invalid")
    if checkpoint_root.name != "aq-checkpoint" or checkpoint_root.parent == Path(
        checkpoint_root.anchor
    ):
        raise UniformAqRunError("uniform_aq_checkpoint_path_invalid")
    require_private_directory(checkpoint_root.parent, "uniform_aq_phase_root")
    if any(
        (ROOT / "corpus" / "generated" / split).exists() for split in ("validation", "final_test")
    ):
        raise UniformAqRunError("held_out_renderer_output_already_open")

    render_development(renderer_directory)
    saliency_grid = load_object(SALIENCY_GRID_PATH)
    manifest = load_object(DEVELOPMENT_MANIFEST_PATH)
    validate_render_index(renderer_directory, manifest, saliency_grid)
    prepared = prepare_bundle(renderer_directory, checkpoint_root / "input")
    corpus_lock = load_object(CORPUS_LOCK_PATH)
    protocol_lock = verify_protocol_lock()
    identities = {
        "corpusProtocolSha256": exact_sha256(
            corpus_lock["protocol_sha256"], "corpus_protocol_sha256"
        ),
        "developmentManifestSha256": sha256_file(DEVELOPMENT_MANIFEST_PATH),
        "developmentRenderIndexSha256": exact_sha256(
            saliency_grid["developmentRenderIndexSha256"],
            "development_render_index_sha256",
        ),
        "gridSha256": sha256_file(GRID_PATH),
        "protocolSha256": exact_sha256(
            protocol_lock["protocol_sha256"], "uniform_aq_protocol_sha256"
        ),
        "saliencyGridSha256": sha256_file(SALIENCY_GRID_PATH),
    }
    checkpoint_root.mkdir(mode=0o700, parents=True, exist_ok=True)
    conditions_root = checkpoint_root / "conditions"
    controlled = run_condition(
        native=native,
        bundle=prepared.path,
        bundle_sha256=prepared.sha256,
        checkpoint=conditions_root,
        fields=AqFields(False, 0, False),
        identities=identities,
    )
    candidates = [
        run_condition(
            native=native,
            bundle=prepared.path,
            bundle_sha256=prepared.sha256,
            checkpoint=conditions_root,
            fields=fields,
            identities=identities,
        )
        for fields in expected_aq_fields()
    ]
    evidence = {
        "candidates": candidates,
        "controlledUniform": controlled,
        "corpusProtocolSha256": identities["corpusProtocolSha256"],
        "developmentManifestSha256": identities["developmentManifestSha256"],
        "gridSha256": identities["gridSha256"],
        "implementationSha256": implementation_sha256(),
        "processingPlatformSha256": processing_platform_sha256,
        "protocol": "uniform_aq_v1",
        "protocolSha256": identities["protocolSha256"],
        "schemaVersion": 1,
        "split": "development",
    }
    schema = load_object(EVIDENCE_SCHEMA_PATH)
    errors = sorted(Draft202012Validator(schema).iter_errors(evidence), key=str)
    if errors:
        raise UniformAqRunError(f"uniform_aq_evidence_schema_invalid:{errors[0].message}")
    evidence_path = phase_output / "development-evidence.json"
    if not evidence_path.exists():
        write_json_exclusive(evidence_path, evidence)
    elif load_json(evidence_path, "uniform_aq_evidence") != evidence:
        raise UniformAqRunError("uniform_aq_phase_evidence_mismatch")

    selection = select_development_evidence(evidence, load_object(GRID_PATH), protocol_lock)
    selection_path = phase_output / "selected-configuration.json"
    if not selection_path.exists():
        write_json_exclusive(selection_path, selection)
    elif load_json(selection_path, "uniform_aq_selection") != selection:
        raise UniformAqRunError("uniform_aq_phase_selection_mismatch")
    if not SELECTION_PATH.exists():
        handoff = phase_output / "freeze-handoff.json"
        if not handoff.exists():
            write_json_exclusive(
                handoff,
                {
                    "candidateId": selection["bestSupportedUniform"]["candidateId"],
                    "reason": "repository_uniform_aq_selection_freeze_required",
                    "resumeCommand": "./scripts/gpu/qualify_cuda_pm.sh",
                    "schemaVersion": 1,
                    "selectionArtifact": "selected-configuration.json",
                    "sourceBundleId": source_bundle_id,
                    "status": "BLOCKED",
                },
            )
        return 75
    committed = load_json(SELECTION_PATH, "committed_uniform_aq_selection")
    compare_committed_selection(selection, committed)
    accepted = phase_output / "selection-verification.json"
    if not accepted.exists():
        write_json_exclusive(
            accepted,
            {
                "committedSelectionSha256": sha256_file(SELECTION_PATH),
                "developmentEvidenceSha256": sha256_file(evidence_path),
                "implementationSha256": implementation_sha256(),
                "schemaVersion": 1,
                "status": "PASSED",
            },
        )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run and freeze the complete target uniform AQ development grid"
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
        OSError,
        subprocess.SubprocessError,
        UniformAqRunError,
        UniformAqSelectionError,
    ) as error:
        print(f"uniform AQ development run failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
