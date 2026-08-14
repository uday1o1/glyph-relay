import { readFile, stat, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

import {
  freezeOracleTolerance,
  parseOracleFreezeInput,
} from "./browser-oracle.ts";

const MAXIMUM_INPUT_BYTES = 4 * 1024 * 1024;

function parseArguments(arguments_: readonly string[]): {
  input: string;
  output: string;
} {
  if (
    arguments_.length !== 4 ||
    arguments_[0] !== "--input" ||
    !arguments_[1] ||
    arguments_[2] !== "--output" ||
    !arguments_[3]
  ) {
    throw new Error(
      "usage: node tooling/m0/freeze-browser-oracle.ts --input OBSERVATIONS.json --output FROZEN.json",
    );
  }
  const input = resolve(arguments_[1]);
  const output = resolve(arguments_[3]);
  if (input === output) {
    throw new Error("oracle_freeze_input_output_must_differ");
  }
  return { input, output };
}

async function main(): Promise<void> {
  const { input, output } = parseArguments(process.argv.slice(2));
  if ((await stat(input)).size > MAXIMUM_INPUT_BYTES) {
    throw new Error("oracle_freeze_input_too_large");
  }
  const observations = parseOracleFreezeInput(
    JSON.parse(await readFile(input, "utf8")),
  );
  const frozen = freezeOracleTolerance(
    observations.runs,
    observations.requiredFrameKeys,
  );
  await writeFile(output, `${JSON.stringify(frozen, null, 2)}\n`, {
    encoding: "utf8",
    flag: "wx",
    mode: 0o600,
  });
  process.stdout.write(
    `${JSON.stringify({ output, sourceDigestSha256: frozen.sourceDigestSha256 })}\n`,
  );
}

const invokedPath = process.argv[1]
  ? pathToFileURL(resolve(process.argv[1])).href
  : undefined;
if (invokedPath === import.meta.url) {
  await main();
}
