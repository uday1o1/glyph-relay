import { createHash } from "node:crypto";
import { mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import { basename, join, resolve } from "node:path";

import { chromium } from "@playwright/test";

interface Arguments {
  decoded: string;
  manifest: string;
  output: string;
}

interface TextRegion {
  boundingBox: readonly [number, number, number, number];
  id: string;
}

interface SampleFrame {
  frameId: number;
  textRegions: readonly TextRegion[];
}

interface Sequence {
  sampleFrames: readonly SampleFrame[];
  sequenceId: string;
}

interface Manifest {
  protocol: string;
  schemaVersion: number;
  sequences: readonly Sequence[];
  split: string;
}

const SAFE_IDENTIFIER = /^[a-z0-9_-]{1,96}$/;
const EXPECTED_FRAME_COUNT = 256;

function parseArguments(values: readonly string[]): Arguments {
  if (values[0] === "--") {
    values = values.slice(1);
  }
  const parsed = new Map<string, string>();
  for (let index = 0; index < values.length; index += 2) {
    const name = values[index];
    const value = values[index + 1];
    if (!name?.startsWith("--") || !value || parsed.has(name)) {
      throw new Error("decoded_ocr_arguments_invalid");
    }
    parsed.set(name, value);
  }
  if (
    parsed.size !== 3 ||
    !parsed.has("--decoded") ||
    !parsed.has("--manifest") ||
    !parsed.has("--output")
  ) {
    throw new Error("decoded_ocr_arguments_invalid");
  }
  return {
    decoded: resolve(parsed.get("--decoded")!),
    manifest: resolve(parsed.get("--manifest")!),
    output: resolve(parsed.get("--output")!),
  };
}

function pngFromDataUrl(value: string): Buffer {
  const match = /^data:image\/png;base64,([A-Za-z0-9+/=]+)$/.exec(value);
  if (!match) {
    throw new Error("decoded_ocr_png_data_url_invalid");
  }
  return Buffer.from(match[1]!, "base64");
}

function sha256(value: Buffer): string {
  return createHash("sha256").update(value).digest("hex");
}

const arguments_ = parseArguments(process.argv.slice(2));
await stat(arguments_.decoded);
const manifest = JSON.parse(
  await readFile(arguments_.manifest, "utf8"),
) as Manifest;
if (
  manifest.protocol !== "corpus_protocol_v1" ||
  manifest.schemaVersion !== 1 ||
  manifest.split !== "development" ||
  !Array.isArray(manifest.sequences)
) {
  throw new Error("decoded_ocr_manifest_invalid");
}
const decodedNames = (await readdir(arguments_.decoded)).sort();
const expectedNames = Array.from(
  { length: EXPECTED_FRAME_COUNT },
  (_, index) => `${String(index).padStart(6, "0")}.png`,
);
if (JSON.stringify(decodedNames) !== JSON.stringify(expectedNames)) {
  throw new Error("decoded_ocr_frame_coverage_invalid");
}
await mkdir(arguments_.output, { recursive: false });
const browser = await chromium.launch({ headless: true });
try {
  if (browser.version() !== "151.0.7922.34") {
    throw new Error(
      `decoded_ocr_browser_version_mismatch:${browser.version()}`,
    );
  }
  const page = await browser.newPage();
  const entries = [];
  let decodedIndex = 0;
  for (const sequence of manifest.sequences) {
    if (
      !SAFE_IDENTIFIER.test(sequence.sequenceId) ||
      sequence.sampleFrames.length !== 4
    ) {
      throw new Error("decoded_ocr_sequence_invalid");
    }
    const sequenceOutput = join(arguments_.output, sequence.sequenceId);
    await mkdir(sequenceOutput, { recursive: false });
    for (const frame of sequence.sampleFrames) {
      const name = expectedNames[decodedIndex++]!;
      const input = await readFile(join(arguments_.decoded, name));
      const outputs = await page.evaluate(
        async ({ encoded, regions }) => {
          const image = new Image();
          image.src = `data:image/png;base64,${encoded}`;
          await image.decode();
          if (image.naturalWidth !== 1920 || image.naturalHeight !== 1080) {
            throw new Error(
              `decoded_ocr_frame_geometry_invalid:${image.naturalWidth}:${image.naturalHeight}`,
            );
          }
          const canvas = document.createElement("canvas");
          canvas.width = image.naturalWidth;
          canvas.height = image.naturalHeight;
          const context = canvas.getContext("2d", {
            alpha: false,
            willReadFrequently: true,
          });
          if (!context) {
            throw new Error("decoded_ocr_canvas_unavailable");
          }
          context.drawImage(image, 0, 0);
          const rendered: Record<string, string> = {};
          for (const region of regions) {
            const [sourceX, sourceY, sourceWidth, sourceHeight] =
              region.boundingBox;
            if (
              sourceWidth <= 0 ||
              sourceHeight <= 0 ||
              sourceX < 0 ||
              sourceY < 0 ||
              sourceX + sourceWidth > canvas.width ||
              sourceY + sourceHeight > canvas.height
            ) {
              throw new Error("decoded_ocr_region_bounds_invalid");
            }
            const source = context.getImageData(
              sourceX,
              sourceY,
              sourceWidth,
              sourceHeight,
            );
            const cornerLuma = Math.round(
              0.2126 * source.data[0]! +
                0.7152 * source.data[1]! +
                0.0722 * source.data[2]!,
            );
            for (let index = 0; index < source.data.length; index += 4) {
              let luma = Math.round(
                0.2126 * source.data[index]! +
                  0.7152 * source.data[index + 1]! +
                  0.0722 * source.data[index + 2]!,
              );
              if (cornerLuma < 128) {
                luma = 255 - luma;
              }
              source.data[index] = luma;
              source.data[index + 1] = luma;
              source.data[index + 2] = luma;
              source.data[index + 3] = 255;
            }
            const sourceCanvas = document.createElement("canvas");
            sourceCanvas.width = sourceWidth;
            sourceCanvas.height = sourceHeight;
            sourceCanvas.getContext("2d")!.putImageData(source, 0, 0);
            const scale = 4;
            const border = 40;
            const output = document.createElement("canvas");
            output.width = sourceWidth * scale + border * 2;
            output.height = sourceHeight * scale + border * 2;
            const outputContext = output.getContext("2d", { alpha: false });
            if (!outputContext) {
              throw new Error("decoded_ocr_output_canvas_unavailable");
            }
            outputContext.fillStyle = "#ffffff";
            outputContext.fillRect(0, 0, output.width, output.height);
            outputContext.imageSmoothingEnabled = false;
            outputContext.drawImage(
              sourceCanvas,
              0,
              0,
              sourceWidth,
              sourceHeight,
              border,
              border,
              sourceWidth * scale,
              sourceHeight * scale,
            );
            rendered[region.id] = output.toDataURL("image/png");
          }
          return rendered;
        },
        {
          encoded: input.toString("base64"),
          regions: frame.textRegions,
        },
      );
      for (const region of frame.textRegions) {
        if (!SAFE_IDENTIFIER.test(region.id) || !(region.id in outputs)) {
          throw new Error("decoded_ocr_region_identity_invalid");
        }
        const output = pngFromDataUrl(outputs[region.id]!);
        const outputName = `${String(frame.frameId).padStart(3, "0")}-${region.id}.png`;
        await writeFile(join(sequenceOutput, outputName), output, {
          flag: "wx",
        });
        entries.push({
          frameId: frame.frameId,
          inputPngSha256: sha256(input),
          outputPngSha256: sha256(output),
          regionId: region.id,
          sequenceId: sequence.sequenceId,
        });
      }
    }
  }
  if (decodedIndex !== EXPECTED_FRAME_COUNT || entries.length !== 512) {
    throw new Error("decoded_ocr_output_coverage_invalid");
  }
  const evidence = {
    browserVersion: browser.version(),
    decodedDirectory: basename(arguments_.decoded),
    entries,
    schemaVersion: 1,
    status: "PASSED",
  };
  await writeFile(
    join(arguments_.output, "preprocess-evidence.json"),
    `${JSON.stringify(evidence)}\n`,
    {
      encoding: "utf8",
      flag: "wx",
    },
  );
  process.stdout.write(
    `${JSON.stringify({ outputs: entries.length, status: evidence.status })}\n`,
  );
} finally {
  await browser.close();
}
