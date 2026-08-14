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

    coturn = lock.get("coturn", {})
    _expect_pattern(errors, coturn.get("linux_amd64_digest"), SHA256, "coturn digest")
    _expect_equal(
        errors, coturn.get("transports_eligible_for_cap_claim"), ["udp"], "coturn transports"
    )
    container = lock.get("linux_cpu_container", {})
    _expect_pattern(errors, container.get("digest"), SHA256, "Linux CPU image digest")
    _expect_equal(errors, container.get("platform"), "linux/amd64", "Linux CPU platform")

    package = load_json(root / "package.json")
    playwright = lock.get("playwright", {})
    _expect_equal(
        errors,
        package.get("devDependencies", {}).get("@playwright/test"),
        playwright.get("version"),
        "package.json Playwright version",
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
