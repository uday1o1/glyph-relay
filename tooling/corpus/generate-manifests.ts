import { mkdir, stat, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

import { chromium } from "@playwright/test";

import { generateCorpusManifest, stableJson } from "./corpus-model.ts";

function argumentsFrom(argv: readonly string[]): {
  output: string;
  split: "development" | "validation";
} {
  let output = "";
  let split: "development" | "validation" | undefined;
  for (let index = 0; index < argv.length; ++index) {
    if (argv[index] === "--output" && argv[index + 1]) {
      output = argv[++index]!;
    } else if (
      argv[index] === "--split" &&
      (argv[index + 1] === "development" || argv[index + 1] === "validation")
    ) {
      split = argv[++index] as "development" | "validation";
    } else {
      throw new Error(
        "usage: generate-manifests --split development|validation --output FILE",
      );
    }
  }
  if (!split || !output) {
    throw new Error(
      "usage: generate-manifests --split development|validation --output FILE",
    );
  }
  return { output, split };
}

const options = argumentsFrom(process.argv.slice(2));
const output = resolve(options.output);
await stat(output)
  .then(() => {
    throw new Error(`corpus_manifest_output_exists:${output}`);
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

const browser = await chromium.launch({ headless: true });
try {
  const context = await browser.newContext({
    colorScheme: "dark",
    deviceScaleFactor: 1,
    locale: "en-US",
    reducedMotion: "reduce",
    timezoneId: "UTC",
    viewport: { height: 1080, width: 1920 },
  });
  const page = await context.newPage();
  const manifest = await generateCorpusManifest(page, options.split);
  await mkdir(dirname(output), { recursive: true });
  await writeFile(output, stableJson(manifest), {
    encoding: "utf8",
    flag: "wx",
  });
  process.stdout.write(
    `${JSON.stringify({ counts: manifest.counts, output, split: manifest.split })}\n`,
  );
} finally {
  await browser.close();
}
