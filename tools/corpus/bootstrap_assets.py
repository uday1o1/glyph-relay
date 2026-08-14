from __future__ import annotations

import hashlib
import json
import os
import urllib.request
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / ".cache" / "corpus"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object: {path}")
    return value


def fetch(url: str, destination: Path, expected_sha256: str) -> None:
    if destination.is_file() and sha256_file(destination) == expected_sha256:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "GlyphRelay-corpus-v1"})
    digest = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as sink:
        while block := response.read(1024 * 1024):
            digest.update(block)
            sink.write(block)
        sink.flush()
        os.fsync(sink.fileno())
    actual = digest.hexdigest()
    if actual != expected_sha256:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(
            f"download hash mismatch for {destination.name}: {actual} != {expected_sha256}"
        )
    os.replace(temporary, destination)


def main() -> int:
    fonts = load_json(ROOT / "corpus" / "fonts.lock.json")
    for raw in fonts.get("fonts", []):
        if not isinstance(raw, dict):
            raise ValueError("font lock entry must be an object")
        name = raw.get("file")
        url = raw.get("url")
        expected = raw.get("sha256")
        font_id = raw.get("id")
        license_url = raw.get("licenseUrl")
        license_expected = raw.get("licenseSha256")
        if not isinstance(name, str) or not isinstance(url, str) or not isinstance(expected, str):
            raise ValueError("font lock entry is incomplete")
        if not isinstance(font_id, str):
            raise ValueError("font lock identifier is incomplete")
        if not isinstance(license_url, str) or not isinstance(license_expected, str):
            raise ValueError("font license lock entry is incomplete")
        fetch(url, CACHE / "fonts" / name, expected)
        fetch(license_url, CACHE / "licenses" / f"{font_id}-OFL.txt", license_expected)

    ocr = load_json(ROOT / "corpus" / "ocr.lock.json")
    model_url = ocr.get("modelUrl")
    model_sha256 = ocr.get("modelSha256")
    if not isinstance(model_url, str) or not isinstance(model_sha256, str):
        raise ValueError("OCR model lock is incomplete")
    fetch(model_url, CACHE / "eng.traineddata", model_sha256)
    print("corpus assets verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
