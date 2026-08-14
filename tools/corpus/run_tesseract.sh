#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 2 ]]; then
  echo "usage: run_tesseract.sh INPUT_DIR OUTPUT_DIR" >&2
  exit 2
fi

input_directory="$1"
output_directory="$2"
if [[ ! -d "$input_directory" ]]; then
  echo "OCR input directory is missing" >&2
  exit 2
fi
if [[ -e "$output_directory" ]]; then
  echo "OCR output already exists" >&2
  exit 2
fi

mkdir -p "$output_directory"
while IFS= read -r -d '' input; do
  relative="${input#"$input_directory"/}"
  output="$output_directory/${relative%.png}.txt"
  mkdir -p "$(dirname "$output")"
  tesseract "$input" stdout \
    --tessdata-dir /opt/glyphrelay/tessdata \
    --oem 1 \
    --psm 7 \
    -l eng \
    2>/dev/null >"$output"
done < <(find "$input_directory" -type f -name '*.png' -print0 | sort -z)

count="$(find "$output_directory" -type f -name '*.txt' | wc -l | tr -d ' ')"
if [[ "$count" -ne 512 ]]; then
  echo "OCR output count is $count, expected 512" >&2
  exit 8
fi
printf '{"outputs":%s,"status":"PASSED"}\n' "$count"
