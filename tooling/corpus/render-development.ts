import { createHash } from "node:crypto";
import { mkdir, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";

import { chromium } from "@playwright/test";

import {
  prepareManifestPage,
  renderSampleFrame,
  type CorpusManifest,
} from "./corpus-model.ts";

function parseArguments(argv: readonly string[]): {
  manifest: string;
  output: string;
} {
  let manifest = "";
  let output = "";
  for (let index = 0; index < argv.length; ++index) {
    if (argv[index] === "--manifest" && argv[index + 1]) {
      manifest = argv[++index]!;
    } else if (argv[index] === "--output" && argv[index + 1]) {
      output = argv[++index]!;
    } else {
      throw new Error("usage: render-development --manifest FILE --output DIR");
    }
  }
  if (!manifest || !output) {
    throw new Error("usage: render-development --manifest FILE --output DIR");
  }
  return { manifest: resolve(manifest), output: resolve(output) };
}

async function absent(path: string): Promise<void> {
  await stat(path)
    .then(() => {
      throw new Error(`corpus_render_output_exists:${path}`);
    })
    .catch((error: unknown) => {
      if (
        error instanceof Error &&
        "code" in error &&
        (error as NodeJS.ErrnoException).code === "ENOENT"
      ) {
        return;
      }
      throw error;
    });
}

function sha256(value: Buffer): string {
  return createHash("sha256").update(value).digest("hex");
}

const options = parseArguments(process.argv.slice(2));
await absent(options.output);
const manifest = JSON.parse(
  await readFile(options.manifest, "utf8"),
) as CorpusManifest;
if (
  manifest.protocol !== "corpus_protocol_v1" ||
  manifest.schemaVersion !== 1 ||
  manifest.split !== "development"
) {
  throw new Error("development_renderer_rejects_non_development_manifest");
}
if (process.version !== "v24.18.1") {
  throw new Error(`corpus_node_version_mismatch:${process.version}`);
}

const temporary = `${options.output}.tmp-${process.pid}`;
await absent(temporary);
await mkdir(dirname(temporary), { recursive: true });
await mkdir(temporary, { recursive: false });
const index: Array<{
  frameId: number;
  frameSha256: string;
  ocrInputs: Record<string, string>;
  sequenceId: string;
  sourcePtsNs: number;
}> = [];

const browser = await chromium.launch({ headless: true });
try {
  if (browser.version() !== "151.0.7922.34") {
    throw new Error(`corpus_chromium_version_mismatch:${browser.version()}`);
  }
  const context = await browser.newContext({
    colorScheme: "dark",
    deviceScaleFactor: 1,
    locale: "en-US",
    reducedMotion: "reduce",
    timezoneId: "UTC",
    viewport: { height: 1080, width: 1920 },
  });
  const page = await context.newPage();
  await prepareManifestPage(page, manifest);
  for (const sequence of manifest.sequences) {
    const frameDirectory = join(temporary, "frames", sequence.sequenceId);
    const ocrDirectory = join(temporary, "ocr-inputs", sequence.sequenceId);
    await mkdir(frameDirectory, { recursive: true });
    await mkdir(ocrDirectory, { recursive: true });
    for (const frame of sequence.sampleFrames) {
      const rendered = await renderSampleFrame(page, manifest, sequence, frame);
      if (!rendered.framePng) {
        throw new Error("corpus_frame_png_missing");
      }
      const frameName = `${String(frame.frameId).padStart(3, "0")}.png`;
      await writeFile(join(frameDirectory, frameName), rendered.framePng, {
        flag: "wx",
      });
      const ocrInputs: Record<string, string> = {};
      for (const [regionId, png] of rendered.ocrPngs) {
        const name = `${String(frame.frameId).padStart(3, "0")}-${regionId}.png`;
        await writeFile(join(ocrDirectory, name), png, { flag: "wx" });
        ocrInputs[regionId] = sha256(png);
      }
      index.push({
        frameId: frame.frameId,
        frameSha256: sha256(rendered.framePng),
        ocrInputs,
        sequenceId: sequence.sequenceId,
        sourcePtsNs: frame.sourcePtsNs,
      });
    }
  }
  await writeFile(
    join(temporary, "render-index.json"),
    `${JSON.stringify({
      browserVersion: browser.version(),
      frames: index,
      protocol: manifest.protocol,
      split: manifest.split,
    })}\n`,
    { encoding: "utf8", flag: "wx" },
  );
  await mkdir(dirname(options.output), { recursive: true });
  await rename(temporary, options.output);
  process.stdout.write(
    `${JSON.stringify({ frames: index.length, output: options.output, status: "PASSED" })}\n`,
  );
} catch (error) {
  await rm(temporary, { force: true, recursive: true });
  throw error;
} finally {
  await browser.close();
}
