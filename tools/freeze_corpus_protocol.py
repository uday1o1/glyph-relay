from __future__ import annotations

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "protocols" / "corpus_protocol_v1" / "manifest.lock"
COMPONENTS = [
    "containers/corpus.Dockerfile",
    "corpus/final-test-pool.json",
    "corpus/final-test-seed-rule.json",
    "corpus/fonts.lock.json",
    "corpus/manifests/development.json",
    "corpus/manifests/validation.json",
    "corpus/metrics-v1.json",
    "corpus/ocr.lock.json",
    "corpus/ontology-v1.json",
    "corpus/renderer.lock.json",
    "corpus/sampling-v1.json",
    "corpus/strata-v1.json",
    "corpus/uniform-aq-v1.json",
    "schemas/corpus-manifest-v1.schema.json",
    "scripts/build_corpus_image.sh",
    "scripts/check_corpus_lossless.sh",
    "scripts/run_corpus_container.sh",
    "tooling/corpus/corpus-model.ts",
    "tooling/corpus/generate-manifests.ts",
    "tooling/corpus/render-development.ts",
    "tools/check_corpus_protocol.py",
    "tools/corpus/aq_selector.py",
    "tools/corpus/bootstrap_assets.py",
    "tools/corpus/evaluate_ocr.py",
    "tools/corpus/run_tesseract.sh",
]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    if OUTPUT.exists():
        raise FileExistsError(f"refusing to overwrite frozen protocol: {OUTPUT}")
    files = [{"path": path, "sha256": digest(ROOT / path)} for path in sorted(COMPONENTS)]
    material = "".join(f"{entry['path']}\0{entry['sha256']}\n" for entry in files).encode()
    lock = {
        "schema_version": 1,
        "protocol": "corpus_protocol_v1",
        "files": files,
        "protocol_sha256": hashlib.sha256(material).hexdigest(),
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("x", encoding="utf-8") as sink:
        json.dump(lock, sink, indent=2)
        sink.write("\n")
    print(lock["protocol_sha256"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
