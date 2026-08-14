import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import { createServer, type ServerResponse } from "node:http";
import { resolve } from "node:path";

import { chromium, firefox, type BrowserType } from "@playwright/test";

const EXPECTED_VERSIONS = {
  chromium: "151.0.7922.34",
  firefox: "153.0",
} as const;
const MAXIMUM_CHILD_OUTPUT_BYTES = 256 * 1024;

interface Arguments {
  output: string;
  stream: string;
}

interface BrowserObservation {
  browser: keyof typeof EXPECTED_VERSIONS;
  browserVersion: string;
  durationSeconds: number;
  height: number;
  samples: readonly {
    lumaMaximum: number;
    lumaMinimum: number;
    mediaTime: number;
    pixelDigest: number;
  }[];
  status: "PASSED";
  width: number;
}

function parseArguments(values: readonly string[]): Arguments {
  if (values[0] === "--") {
    values = values.slice(1);
  }
  const parsed = new Map<string, string>();
  for (let index = 0; index < values.length; index += 2) {
    const name = values[index];
    const value = values[index + 1];
    if (!name?.startsWith("--") || !value || parsed.has(name)) {
      throw new Error("browser_decode_arguments_invalid");
    }
    parsed.set(name, value);
  }
  if (parsed.size !== 2 || !parsed.has("--stream") || !parsed.has("--output")) {
    throw new Error("browser_decode_arguments_invalid");
  }
  return {
    output: resolve(parsed.get("--output")!),
    stream: resolve(parsed.get("--stream")!),
  };
}

async function runChild(
  program: string,
  args: readonly string[],
): Promise<void> {
  const child = spawn(program, args, {
    env: { PATH: process.env.PATH ?? "" },
    shell: false,
    stdio: ["ignore", "ignore", "pipe"],
  });
  let stderr = "";
  child.stderr.on("data", (chunk: Buffer) => {
    stderr += chunk.toString("utf8");
    if (Buffer.byteLength(stderr) > MAXIMUM_CHILD_OUTPUT_BYTES) {
      child.kill("SIGTERM");
    }
  });
  const result = await new Promise<{
    code: number | null;
    signal: NodeJS.Signals | null;
  }>((accept, reject) => {
    child.once("error", reject);
    child.once("exit", (code, signal) => accept({ code, signal }));
  });
  if (
    result.code !== 0 ||
    result.signal !== null ||
    Buffer.byteLength(stderr) > MAXIMUM_CHILD_OUTPUT_BYTES
  ) {
    throw new Error(
      `browser_decode_remux_failed:${result.code}:${result.signal}:${stderr.slice(0, 2048)}`,
    );
  }
}

function sendError(response: ServerResponse, status: number): void {
  response.writeHead(status, {
    "Cache-Control": "no-store",
    "Content-Length": "0",
  });
  response.end();
}

async function startMediaServer(path: string): Promise<{
  close(): Promise<void>;
  url: string;
}> {
  const details = await stat(path);
  const server = createServer((request, response) => {
    const host = request.headers.host;
    const address = server.address();
    if (
      typeof address !== "object" ||
      address === null ||
      host !== `127.0.0.1:${address.port}` ||
      request.url !== "/trial.mp4" ||
      (request.method !== "GET" && request.method !== "HEAD")
    ) {
      sendError(response, 404);
      return;
    }
    const common = {
      "Accept-Ranges": "bytes",
      "Access-Control-Allow-Origin": "*",
      "Cache-Control": "no-store",
      "Content-Type": "video/mp4",
    };
    const range = request.headers.range;
    if (!range) {
      response.writeHead(200, {
        ...common,
        "Content-Length": String(details.size),
      });
      if (request.method === "HEAD") {
        response.end();
      } else {
        createReadStream(path).pipe(response);
      }
      return;
    }
    const match = /^bytes=(\d+)-(\d*)$/.exec(range);
    if (!match) {
      sendError(response, 416);
      return;
    }
    const start = Number(match[1]);
    const requestedEnd = match[2] ? Number(match[2]) : details.size - 1;
    const end = Math.min(requestedEnd, details.size - 1);
    if (
      !Number.isSafeInteger(start) ||
      !Number.isSafeInteger(end) ||
      start < 0 ||
      start > end ||
      start >= details.size
    ) {
      sendError(response, 416);
      return;
    }
    response.writeHead(206, {
      ...common,
      "Content-Length": String(end - start + 1),
      "Content-Range": `bytes ${start}-${end}/${details.size}`,
    });
    if (request.method === "HEAD") {
      response.end();
    } else {
      createReadStream(path, { end, start }).pipe(response);
    }
  });
  server.maxHeadersCount = 32;
  server.headersTimeout = 5_000;
  server.requestTimeout = 15_000;
  await new Promise<void>((accept, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => accept());
  });
  const address = server.address();
  assert(typeof address === "object" && address !== null);
  return {
    close: () =>
      new Promise<void>((accept, reject) => {
        server.close((error) => (error ? reject(error) : accept()));
      }),
    url: `http://127.0.0.1:${address.port}/trial.mp4`,
  };
}

async function observeBrowser(
  name: keyof typeof EXPECTED_VERSIONS,
  type: BrowserType,
  mediaUrl: string,
): Promise<BrowserObservation> {
  const browser = await type.launch({ headless: true });
  try {
    const version = browser.version();
    if (version !== EXPECTED_VERSIONS[name]) {
      throw new Error(`browser_decode_version_mismatch:${name}:${version}`);
    }
    const page = await browser.newPage();
    await page.setContent(
      "<!doctype html><meta charset=utf-8><video crossorigin=anonymous></video>",
    );
    const observation = await page.evaluate(async (url) => {
      const video = document.querySelector("video");
      if (!video) {
        throw new Error("browser_decode_video_missing");
      }
      video.muted = true;
      video.preload = "auto";
      const event = (name: "loadedmetadata" | "seeked") =>
        new Promise<void>((accept, reject) => {
          const timer = setTimeout(
            () => reject(new Error(`browser_decode_${name}_timeout`)),
            15_000,
          );
          const failed = () => {
            clearTimeout(timer);
            reject(
              new Error(`browser_decode_media_error:${video.error?.code ?? 0}`),
            );
          };
          video.addEventListener(
            name,
            () => {
              clearTimeout(timer);
              video.removeEventListener("error", failed);
              accept();
            },
            { once: true },
          );
          video.addEventListener("error", failed, { once: true });
        });
      const metadata = event("loadedmetadata");
      video.src = url;
      video.load();
      await metadata;
      if (
        video.videoWidth !== 1920 ||
        video.videoHeight !== 1080 ||
        !Number.isFinite(video.duration) ||
        video.duration < 500 ||
        video.duration > 520
      ) {
        throw new Error(
          `browser_decode_metadata_invalid:${video.videoWidth}:${video.videoHeight}:${video.duration}`,
        );
      }
      const targets = [
        1,
        video.duration * 0.25,
        video.duration * 0.5,
        video.duration * 0.75,
        video.duration - 1,
      ];
      const canvas = document.createElement("canvas");
      canvas.width = video.videoWidth;
      canvas.height = video.videoHeight;
      const context = canvas.getContext("2d", {
        alpha: false,
        willReadFrequently: true,
      });
      if (!context) {
        throw new Error("browser_decode_canvas_unavailable");
      }
      const samples = [];
      for (const target of targets) {
        const seeked = event("seeked");
        video.currentTime = target;
        await seeked;
        const mediaTime = await new Promise<number>((accept, reject) => {
          const timer = setTimeout(
            () => reject(new Error("browser_decode_frame_timeout")),
            15_000,
          );
          video.requestVideoFrameCallback((_now, metadataValue) => {
            clearTimeout(timer);
            accept(metadataValue.mediaTime);
          });
        });
        context.drawImage(video, 0, 0);
        const pixels = context.getImageData(
          0,
          0,
          canvas.width,
          canvas.height,
        ).data;
        let lumaMinimum = 255;
        let lumaMaximum = 0;
        let pixelDigest = 2166136261;
        for (let index = 0; index < pixels.length; index += 4096) {
          const luma = Math.round(
            0.2126 * pixels[index]! +
              0.7152 * pixels[index + 1]! +
              0.0722 * pixels[index + 2]!,
          );
          lumaMinimum = Math.min(lumaMinimum, luma);
          lumaMaximum = Math.max(lumaMaximum, luma);
          pixelDigest = Math.imul(pixelDigest ^ luma, 16777619) >>> 0;
        }
        if (lumaMaximum - lumaMinimum < 8) {
          throw new Error("browser_decode_frame_has_no_contrast");
        }
        samples.push({ lumaMaximum, lumaMinimum, mediaTime, pixelDigest });
      }
      return {
        durationSeconds: video.duration,
        height: video.videoHeight,
        samples,
        width: video.videoWidth,
      };
    }, mediaUrl);
    return {
      browser: name,
      browserVersion: version,
      ...observation,
      status: "PASSED",
    };
  } finally {
    await browser.close();
  }
}

const arguments_ = parseArguments(process.argv.slice(2));
await stat(arguments_.stream);
await mkdir(arguments_.output, { recursive: false });
const mp4 = resolve(arguments_.output, "trial.mp4");
await runChild("ffmpeg", [
  "-v",
  "error",
  "-f",
  "h264",
  "-framerate",
  "30",
  "-i",
  arguments_.stream,
  "-map",
  "0:v:0",
  "-c:v",
  "copy",
  "-movflags",
  "+faststart",
  "-n",
  mp4,
]);
const server = await startMediaServer(mp4);
try {
  const browsers = [
    await observeBrowser("chromium", chromium, server.url),
    await observeBrowser("firefox", firefox, server.url),
  ];
  const evidence = {
    browsers,
    mp4Sha256: createHash("sha256")
      .update(await readFile(mp4))
      .digest("hex"),
    schemaVersion: 1,
    status: "PASSED",
    streamSha256: createHash("sha256")
      .update(await readFile(arguments_.stream))
      .digest("hex"),
  };
  await writeFile(
    resolve(arguments_.output, "evidence.json"),
    `${JSON.stringify(evidence)}\n`,
    { encoding: "utf8", flag: "wx" },
  );
  process.stdout.write(`${JSON.stringify(evidence)}\n`);
} finally {
  await server.close();
}
