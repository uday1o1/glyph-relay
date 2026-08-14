from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
SHA256 = re.compile(r"sha256:[0-9a-f]{64}")

REQUIRED_LIBDATACHANNEL_FLAGS = {
    "BUILD_SHARED_LIBS": False,
    "BUILD_SHARED_DEPS_LIBS": False,
    "NO_EXAMPLES": True,
    "NO_MEDIA": False,
    "NO_TESTS": False,
    "NO_WEBSOCKET": False,
    "PREFER_SYSTEM_LIB": False,
    "USE_GNUTLS": False,
    "USE_MBEDTLS": False,
    "USE_NICE": False,
    "USE_SYSTEM_JSON": False,
    "USE_SYSTEM_JUICE": False,
    "USE_SYSTEM_PLOG": False,
    "USE_SYSTEM_SRTP": False,
    "USE_SYSTEM_USRSCTP": False,
}


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def _expect_pattern(errors: list[str], value: object, pattern: re.Pattern[str], label: str) -> None:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        errors.append(f"{label} has an invalid format")


def _expect_equal(errors: list[str], actual: object, expected: object, label: str) -> None:
    if actual != expected:
        errors.append(f"{label} is {actual!r}, expected {expected!r}")


def validate_lock(lock: dict[str, Any], root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    _expect_equal(errors, lock.get("schema_version"), 1, "schema_version")

    nvenc = lock.get("nvenc_headers", {})
    _expect_pattern(errors, nvenc.get("commit"), HEX40, "nvenc_headers.commit")
    _expect_pattern(errors, nvenc.get("header_sha256"), HEX64, "nvenc_headers.header_sha256")
    _expect_equal(errors, (nvenc.get("api_major"), nvenc.get("api_minor")), (13, 1), "NVENC API")
    _expect_equal(errors, nvenc.get("minimum_linux_driver"), "610.0", "NVENC Linux driver")

    datachannel = lock.get("libdatachannel", {})
    _expect_pattern(errors, datachannel.get("commit"), HEX40, "libdatachannel.commit")
    _expect_pattern(errors, datachannel.get("license_sha256"), HEX64, "libdatachannel license")
    _expect_equal(
        errors,
        datachannel.get("build_flags"),
        REQUIRED_LIBDATACHANNEL_FLAGS,
        "libdatachannel.build_flags",
    )
    submodules = datachannel.get("submodules", [])
    expected_submodules = {
        "deps/json",
        "deps/libjuice",
        "deps/libsrtp",
        "deps/plog",
        "deps/usrsctp",
    }
    actual_submodules: set[object] = set()
    if not isinstance(submodules, list):
        errors.append("libdatachannel.submodules must be a list")
        submodules = []
    for index, submodule in enumerate(submodules):
        if not isinstance(submodule, dict):
            errors.append(f"libdatachannel.submodules[{index}] must be an object")
            continue
        actual_submodules.add(submodule.get("path"))
        _expect_pattern(errors, submodule.get("commit"), HEX40, f"submodule {index} commit")
        _expect_pattern(
            errors, submodule.get("license_sha256"), HEX64, f"submodule {index} license"
        )
    _expect_equal(errors, actual_submodules, expected_submodules, "libdatachannel submodule paths")
    transport = datachannel.get("transport_contract", {})
    _expect_equal(errors, transport.get("final_send_syscall"), "sendto", "final send syscall")
    _expect_equal(
        errors, transport.get("final_datagram_patch_required"), True, "final datagram patch"
    )
    patch_path = transport.get("final_datagram_patch")
    if not isinstance(patch_path, str):
        errors.append("final datagram patch path is missing")
    else:
        patch_file = root / patch_path
        if not patch_file.is_file():
            errors.append(f"final datagram patch is missing: {patch_path}")
        else:
            actual_patch_sha256 = hashlib.sha256(patch_file.read_bytes()).hexdigest()
            _expect_equal(
                errors,
                actual_patch_sha256,
                transport.get("final_datagram_patch_sha256"),
                "final datagram patch SHA-256",
            )
    test_source_path = transport.get("final_datagram_test_source")
    if not isinstance(test_source_path, str):
        errors.append("final datagram test source path is missing")
    else:
        test_source_file = root / test_source_path
        if not test_source_file.is_file():
            errors.append(f"final datagram test source is missing: {test_source_path}")
        else:
            actual_test_source_sha256 = hashlib.sha256(test_source_file.read_bytes()).hexdigest()
            _expect_equal(
                errors,
                actual_test_source_sha256,
                transport.get("final_datagram_test_source_sha256"),
                "final datagram test source SHA-256",
            )
    _expect_equal(
        errors,
        transport.get("relay_policy_enforced_in_libjuice"),
        True,
        "libjuice relay-only policy enforcement",
    )
    _expect_equal(
        errors,
        transport.get("numeric_candidate_resolution_ignores_ai_addrconfig"),
        True,
        "numeric ICE candidate resolution",
    )
    _expect_equal(
        errors, transport.get("builtin_nack_responder_allowed"), False, "built-in NACK responder"
    )
    _expect_equal(
        errors,
        transport.get("vendor_deprecated_declarations_as_errors"),
        False,
        "vendor deprecated declarations as errors",
    )

    openh264 = lock.get("openh264", {})
    _expect_pattern(
        errors,
        openh264.get("development_package_sha256_amd64"),
        HEX64,
        "OpenH264 development package",
    )
    _expect_pattern(
        errors,
        openh264.get("runtime_package_sha256_amd64"),
        HEX64,
        "OpenH264 runtime package",
    )
    _expect_equal(errors, openh264.get("soname"), "libopenh264.so.7", "OpenH264 SONAME")
    _expect_equal(errors, openh264.get("encoder_input_layout"), "I420", "OpenH264 input layout")

    linux_capture = lock.get("linux_capture", {})
    capture_packages = linux_capture.get("packages", [])
    expected_capture_packages = {
        "libclang-rt-18-dev": "1:18.1.3-1ubuntu1",
        "libglib2.0-dev": "2.80.0-6ubuntu3.8",
        "libpipewire-0.3-dev": "1.0.5-1ubuntu3.3",
        "libspa-0.2-dev": "1.0.5-1ubuntu3.3",
        "pkg-config": "1.8.1-2build1",
    }
    actual_capture_packages: dict[object, object] = {}
    if not isinstance(capture_packages, list):
        errors.append("linux_capture.packages must be a list")
        capture_packages = []
    for index, package_lock in enumerate(capture_packages):
        if not isinstance(package_lock, dict):
            errors.append(f"linux_capture.packages[{index}] must be an object")
            continue
        actual_capture_packages[package_lock.get("name")] = package_lock.get("version")
        _expect_pattern(
            errors,
            package_lock.get("sha256_amd64"),
            HEX64,
            f"Linux capture package {index}",
        )
    _expect_equal(
        errors,
        actual_capture_packages,
        expected_capture_packages,
        "Linux capture package pins",
    )
    _expect_equal(
        errors,
        linux_capture.get("portal_interface"),
        "org.freedesktop.portal.ScreenCast",
        "portal interface",
    )
    _expect_equal(
        errors,
        linux_capture.get("mandatory_memory_path"),
        "shared_memory",
        "capture memory path",
    )

    coturn = lock.get("coturn", {})
    _expect_pattern(errors, coturn.get("linux_amd64_digest"), SHA256, "coturn digest")
    _expect_equal(
        errors, coturn.get("transports_eligible_for_cap_claim"), ["udp"], "coturn transports"
    )
    container = lock.get("linux_cpu_container", {})
    _expect_pattern(errors, container.get("digest"), SHA256, "Linux CPU image digest")
    _expect_equal(errors, container.get("platform"), "linux/amd64", "Linux CPU platform")

    cuda_container = lock.get("cuda_compile_container", {})
    _expect_pattern(errors, cuda_container.get("digest"), SHA256, "CUDA compile image digest")
    _expect_equal(errors, cuda_container.get("platform"), "linux/amd64", "CUDA compile platform")
    _expect_equal(errors, cuda_container.get("cuda_toolkit"), "13.3.1", "CUDA toolkit")
    cuda_dockerfile = (root / "containers/cuda-compile.Dockerfile").read_text(encoding="utf-8")
    expected_cuda_from = (
        f"FROM --platform={cuda_container.get('platform')} "
        f"{cuda_container.get('image')}@{cuda_container.get('digest')}"
    )
    if expected_cuda_from not in cuda_dockerfile:
        errors.append("CUDA compile Dockerfile does not use the locked image identity")
    if (
        str(nvenc.get("tag")) not in cuda_dockerfile
        or str(nvenc.get("commit")) not in cuda_dockerfile
    ):
        errors.append("CUDA compile Dockerfile does not verify the locked NVENC source identity")
    if str(nvenc.get("header_sha256")) not in cuda_dockerfile:
        errors.append("CUDA compile Dockerfile does not verify the locked NVENC header hash")

    signaling = lock.get("signaling", {})
    _expect_pattern(
        errors,
        signaling.get("node_image_digest_amd64"),
        SHA256,
        "signaling Node image digest",
    )
    _expect_equal(errors, signaling.get("platform"), "linux/amd64", "signaling platform")
    for package_name, package_path in {
        "ws": root / "node_modules/ws/LICENSE",
        "ws_types": root / "node_modules/@types/ws/LICENSE",
    }.items():
        package_lock = signaling.get(package_name, {})
        _expect_pattern(
            errors,
            package_lock.get("license_sha256"),
            HEX64,
            f"signaling {package_name} license",
        )
        if not package_path.is_file():
            errors.append(f"installed signaling license is missing: {package_path}")
        else:
            _expect_equal(
                errors,
                hashlib.sha256(package_path.read_bytes()).hexdigest(),
                package_lock.get("license_sha256"),
                f"signaling {package_name} installed license SHA-256",
            )

    corpus = lock.get("corpus_protocol", {})
    _expect_equal(errors, corpus.get("name"), "corpus_protocol_v1", "corpus protocol name")
    _expect_pattern(errors, corpus.get("protocol_sha256"), HEX64, "corpus protocol SHA-256")
    _expect_pattern(
        errors,
        corpus.get("manifest_lock_sha256"),
        HEX64,
        "corpus manifest lock SHA-256",
    )
    corpus_manifest_path = root / "protocols/corpus_protocol_v1/manifest.lock"
    corpus_manifest = load_json(corpus_manifest_path)
    _expect_equal(
        errors,
        corpus_manifest.get("protocol_sha256"),
        corpus.get("protocol_sha256"),
        "corpus protocol aggregate SHA-256",
    )
    _expect_equal(
        errors,
        hashlib.sha256(corpus_manifest_path.read_bytes()).hexdigest(),
        corpus.get("manifest_lock_sha256"),
        "corpus protocol manifest lock SHA-256",
    )

    renderer_lock = load_json(root / "corpus/renderer.lock.json")
    expected_renderer = {
        "image_digest": renderer_lock.get("imageDigest"),
        "platform": renderer_lock.get("platform"),
        "node_version": renderer_lock.get("nodeVersion"),
        "playwright_version": renderer_lock.get("playwrightVersion"),
        "chromium_revision": renderer_lock.get("chromiumRevision"),
        "chromium_version": renderer_lock.get("chromiumVersion"),
    }
    _expect_equal(errors, corpus.get("renderer"), expected_renderer, "corpus renderer lock")

    ocr_lock = load_json(root / "corpus/ocr.lock.json")
    expected_ocr_packages: dict[str, str] = {}
    for package_lock in ocr_lock.get("ubuntuRuntimePackages", []):
        if not isinstance(package_lock, dict):
            errors.append("corpus OCR package lock must be an object")
            continue
        corpus_package_name = package_lock.get("name")
        corpus_package_version = package_lock.get("version")
        if not isinstance(corpus_package_name, str) or not isinstance(corpus_package_version, str):
            errors.append("corpus OCR package lock is incomplete")
            continue
        expected_ocr_packages[corpus_package_name] = corpus_package_version
    expected_ocr = {
        "engine_version": ocr_lock.get("engineVersion"),
        "packages": expected_ocr_packages,
        "runtime_sha256": ocr_lock.get("runtimeSha256"),
        "model_commit": ocr_lock.get("modelCommit"),
        "model_sha256": ocr_lock.get("modelSha256"),
        "oem": ocr_lock.get("oem"),
        "page_segmentation_mode": ocr_lock.get("pageSegmentationMode"),
    }
    _expect_equal(errors, corpus.get("ocr"), expected_ocr, "corpus OCR lock")
    for package_name, package_version in expected_ocr_packages.items():
        if f"{package_name}={package_version}" not in (
            root / "containers/corpus.Dockerfile"
        ).read_text(encoding="utf-8"):
            errors.append(f"corpus Dockerfile omits locked {package_name} package")
    runtime_hashes = ocr_lock.get("runtimeSha256", {})
    if not isinstance(runtime_hashes, dict):
        errors.append("corpus OCR runtime hashes must be an object")
        runtime_hashes = {}
    corpus_dockerfile = (root / "containers/corpus.Dockerfile").read_text(encoding="utf-8")
    for runtime_path, runtime_sha256 in runtime_hashes.items():
        if not isinstance(runtime_path, str) or not isinstance(runtime_sha256, str):
            errors.append("corpus OCR runtime hash entry is incomplete")
        elif runtime_path not in corpus_dockerfile or runtime_sha256 not in corpus_dockerfile:
            errors.append(f"corpus Dockerfile omits locked runtime hash for {runtime_path}")

    fonts_lock = load_json(root / "corpus/fonts.lock.json")
    expected_fonts = {
        "repository_commit": fonts_lock.get("commit"),
        "files": {
            font.get("id"): font.get("sha256")
            for font in fonts_lock.get("fonts", [])
            if isinstance(font, dict)
        },
    }
    _expect_equal(errors, corpus.get("fonts"), expected_fonts, "corpus font lock")

    package = load_json(root / "package.json")
    playwright = lock.get("playwright", {})
    _expect_equal(
        errors,
        package.get("devDependencies", {}).get("@playwright/test"),
        playwright.get("version"),
        "package.json Playwright version",
    )
    _expect_equal(
        errors,
        package.get("packageManager"),
        f"pnpm@{signaling.get('pnpm')}",
        "package.json pnpm version",
    )
    _expect_equal(
        errors,
        package.get("dependencies", {}).get("ws"),
        signaling.get("ws", {}).get("version"),
        "package.json ws version",
    )
    _expect_equal(
        errors,
        package.get("devDependencies", {}).get("@types/ws"),
        signaling.get("ws_types", {}).get("version"),
        "package.json ws types version",
    )

    browser_manifests = sorted(
        root.glob("node_modules/.pnpm/playwright-core@*/node_modules/playwright-core/browsers.json")
    )
    if len(browser_manifests) != 1:
        errors.append(
            f"expected one installed Playwright browser manifest, found {len(browser_manifests)}"
        )
    else:
        installed = load_json(browser_manifests[0]).get("browsers", [])
        installed_by_name = {
            browser.get("name"): browser
            for browser in installed
            if isinstance(browser, dict) and browser.get("name") in {"chromium", "firefox"}
        }
        for name in ("chromium", "firefox"):
            expected = playwright.get(name, {})
            actual = installed_by_name.get(name, {})
            _expect_equal(
                errors, actual.get("revision"), expected.get("revision"), f"{name} revision"
            )
            _expect_equal(
                errors,
                actual.get("browserVersion"),
                expected.get("version"),
                f"{name} browser version",
            )

    header = (root / "include/glyphrelay/dependency_versions.hpp").read_text(encoding="utf-8")
    for label, value in {
        "NVENC commit": nvenc.get("commit"),
        "NVENC header hash": nvenc.get("header_sha256"),
        "NVENC driver": nvenc.get("minimum_linux_driver"),
        "libdatachannel commit": datachannel.get("commit"),
        "Playwright version": playwright.get("version"),
    }.items():
        if not isinstance(value, str) or value not in header:
            errors.append(f"native dependency constants omit {label}")

    dockerfile = (root / "containers/linux-cpu.Dockerfile").read_text(encoding="utf-8")
    image_reference = f"{container.get('image')}@{container.get('digest')}"
    if not dockerfile.startswith(f"FROM {image_reference}\n"):
        errors.append("Linux CPU Dockerfile does not use the locked base image")
    for package_name, package_version in expected_capture_packages.items():
        if f"{package_name}={package_version}" not in dockerfile:
            errors.append(f"Linux CPU Dockerfile omits locked {package_name} package")

    signaling_dockerfile = (root / "containers/signaling.Dockerfile").read_text(encoding="utf-8")
    signaling_image = f"{signaling.get('node_image')}@{signaling.get('node_image_digest_amd64')}"
    if not signaling_dockerfile.startswith(f"FROM {signaling_image}\n"):
        errors.append("signaling Dockerfile does not use the locked Node image")
    if f"pnpm@{signaling.get('pnpm')}" not in signaling_dockerfile:
        errors.append("signaling Dockerfile omits locked pnpm version")

    corpus_renderer = corpus.get("renderer", {})
    corpus_dockerfile = (root / "containers/corpus.Dockerfile").read_text(encoding="utf-8")
    if not corpus_dockerfile.startswith(
        f"FROM mcr.microsoft.com/playwright@{corpus_renderer.get('image_digest')}\n"
    ):
        errors.append("corpus Dockerfile does not use the locked Playwright image")

    return errors


def main() -> int:
    errors = validate_lock(load_json(ROOT / "dependencies.lock.json"))
    if errors:
        print("\n".join(errors))
        return 1
    print("dependency lock validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
