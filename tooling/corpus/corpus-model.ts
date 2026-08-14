import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";

import type { Page } from "@playwright/test";

export const CORPUS_PROTOCOL = "corpus_protocol_v1";
export const FRAME_COUNT = 240;
export const FRAME_RATE = 30;
export const FRAME_INTERVAL_NS = 33_333_333;
export const SAMPLE_FRAMES = [0, 60, 120, 180] as const;
export const WIDTH = 1920;
export const HEIGHT = 1080;

export const CORE_STRATA = [
  "code_editor",
  "terminal",
  "spreadsheet_table",
  "slide_diagram",
  "browser_documentation",
  "mixed_video_text",
  "animated_typing_scrolling",
] as const;

const NORMAL_TOKENS = [
  "CACHE",
  "QUEUE",
  "STATUS",
  "WINDOW",
  "FRAME",
  "TOKEN",
  "RECORD",
  "SIGNAL",
  "STREAM",
  "READY",
  "BOUND",
] as const;
const SMALL_TOKENS = ["SMALL", "GLYPHS", "STAY", "CLEAR"] as const;
const FONT_SIZES = [22, 12] as const;

export interface FontLockEntry {
  file: string;
  id: string;
  sha256: string;
  split: string;
}

export interface GlyphRaster {
  advanceWidth: number;
  alphaBase64: string;
  alphaSha256: string;
  character: string;
  fontId: string;
  fontSizePx: number;
  height: number;
  id: string;
  offsetX: number;
  offsetY: number;
  width: number;
}

export interface GlyphInstance {
  glyphIndex: number;
  rasterId: string;
  smallGlyphSubset: boolean;
  x: number;
  y: number;
}

export interface TextRegion {
  boundingBox: [number, number, number, number];
  glyphs: GlyphInstance[];
  id: string;
  smallGlyphRegion: boolean;
  truth: string;
}

export interface SampleFrame {
  frameId: number;
  geometryEpoch: 1;
  sourcePtsNs: number;
  textRegions: TextRegion[];
  uiPrimitives: Array<{
    boundingBox: [number, number, number, number];
    type: string;
  }>;
}

export interface SequenceSpec {
  durationFrames: number;
  fontId: string;
  layoutId: string;
  logicalDeviceScale: 1 | 2;
  motionCategory: "static" | "slow" | "rapid";
  sampleFrames: SampleFrame[];
  sceneDescription: string;
  seed: string;
  sequenceId: string;
  stratum: string;
  themeId: string;
}

export interface CorpusManifest {
  counts: {
    characterInstances: number;
    sampledFrames: number;
    sequences: number;
    smallGlyphInstances: number;
  };
  frameContract: {
    durationSeconds: 8;
    frameCount: number;
    frameIntervalNs: number;
    frameRate: number;
    geometryEpoch: 1;
    height: number;
    sampleFrames: readonly number[];
    width: number;
  };
  glyphRasterCatalog: GlyphRaster[];
  protocol: typeof CORPUS_PROTOCOL;
  rendererOutputOpened: false;
  schemaVersion: 1;
  sequences: SequenceSpec[];
  split: "development" | "validation";
  splitContract: {
    fontId: string;
    layoutIds: string[];
    stratumWeights: Record<string, number>;
    themeIds: string[];
  };
}

interface SplitConfig {
  font: FontLockEntry;
  fontFamily: string;
  layouts: readonly string[];
  motion: readonly ("static" | "slow" | "rapid")[];
  split: "development" | "validation";
  themes: readonly Theme[];
}

interface Theme {
  accent: string;
  background: string;
  foreground: string;
  id: string;
  panel: string;
}

export interface RenderedFrame {
  framePng?: Buffer;
  ocrPngs: Map<string, Buffer>;
}

function sha256(value: string | Buffer): string {
  return createHash("sha256").update(value).digest("hex");
}

function rotate<T>(values: readonly T[], offset: number): T[] {
  return values.map((_, index) => values[(index + offset) % values.length]!);
}

function textFor(
  tokens: readonly string[],
  sequenceIndex: number,
  sampleIndex: number,
): string {
  return rotate(tokens, (sequenceIndex + sampleIndex) % tokens.length).join(
    " ",
  );
}

function nonspaceCharacters(value: string): number {
  return [...value].filter((character) => character !== " ").length;
}

function rasterId(
  fontId: string,
  fontSizePx: number,
  character: string,
): string {
  return `${fontId}-${fontSizePx}-${character.codePointAt(0)!.toString(16).padStart(4, "0")}`;
}

async function fontEntry(
  split: "development" | "validation",
): Promise<FontLockEntry> {
  const lock = JSON.parse(
    await readFile(resolve("corpus/fonts.lock.json"), "utf8"),
  ) as { fonts?: FontLockEntry[] };
  const entry = lock.fonts?.find((candidate) => candidate.split === split);
  if (!entry) {
    throw new Error(`corpus_font_missing_${split}`);
  }
  return entry;
}

async function splitConfig(
  split: "development" | "validation",
): Promise<SplitConfig> {
  const font = await fontEntry(split);
  if (split === "development") {
    return {
      font,
      fontFamily: "GlyphRelayDevelopment",
      layouts: ["dev_split_pane", "dev_centered_grid"],
      motion: ["static", "slow", "rapid"],
      split,
      themes: [
        {
          accent: "#69e0b4",
          background: "#07110f",
          foreground: "#eef8f2",
          id: "dev_dark_teal",
          panel: "#10231d",
        },
        {
          accent: "#245a77",
          background: "#edf4f2",
          foreground: "#15211e",
          id: "dev_light_slate",
          panel: "#d7e5e0",
        },
      ],
    };
  }
  return {
    font,
    fontFamily: "GlyphRelayValidation",
    layouts: ["validation_sidebar", "validation_command_deck"],
    motion: ["rapid", "static", "slow"],
    split,
    themes: [
      {
        accent: "#f3b95f",
        background: "#17120b",
        foreground: "#fff5df",
        id: "validation_dark_amber",
        panel: "#302317",
      },
      {
        accent: "#7158a6",
        background: "#f4f0fa",
        foreground: "#211a2e",
        id: "validation_light_violet",
        panel: "#e2d9ee",
      },
    ],
  };
}

export async function loadCorpusFont(
  page: Page,
  font: FontLockEntry,
  family: string,
): Promise<void> {
  const bytes = await readFile(resolve(".cache/corpus/fonts", font.file));
  if (sha256(bytes) !== font.sha256) {
    throw new Error(`corpus_font_hash_mismatch_${font.id}`);
  }
  await page.evaluate(
    async ({ base64, familyName }) => {
      const face = new FontFace(
        familyName,
        `url(data:font/ttf;base64,${base64}) format('truetype')`,
        { display: "block", style: "normal", weight: "400" },
      );
      await face.load();
      document.fonts.add(face);
      await document.fonts.ready;
    },
    { base64: bytes.toString("base64"), familyName: family },
  );
}

export async function buildGlyphCatalog(
  page: Page,
  font: FontLockEntry,
  family: string,
): Promise<GlyphRaster[]> {
  const characters = [...new Set([...NORMAL_TOKENS, ...SMALL_TOKENS].join(""))]
    .filter((character) => character !== " ")
    .sort();
  const raw = await page.evaluate(
    ({ chars, familyName, sizes }) => {
      const output: Array<{
        advanceWidth: number;
        alphaBase64: string;
        character: string;
        fontSizePx: number;
        height: number;
        offsetX: number;
        offsetY: number;
        width: number;
      }> = [];
      for (const fontSizePx of sizes) {
        for (const character of chars) {
          const canvas = document.createElement("canvas");
          canvas.width = 64;
          canvas.height = 64;
          const context = canvas.getContext("2d", {
            alpha: true,
            willReadFrequently: true,
          });
          if (!context) {
            throw new Error("corpus_glyph_context_unavailable");
          }
          context.clearRect(0, 0, canvas.width, canvas.height);
          context.fillStyle = "rgba(255,255,255,1)";
          context.font = `400 ${fontSizePx}px '${familyName}'`;
          context.textBaseline = "alphabetic";
          const drawX = 12;
          const baseline = 40;
          context.fillText(character, drawX, baseline);
          const pixels = context.getImageData(
            0,
            0,
            canvas.width,
            canvas.height,
          ).data;
          let left = canvas.width;
          let top = canvas.height;
          let right = -1;
          let bottom = -1;
          for (let y = 0; y < canvas.height; ++y) {
            for (let x = 0; x < canvas.width; ++x) {
              if (pixels[(y * canvas.width + x) * 4 + 3] !== 0) {
                left = Math.min(left, x);
                top = Math.min(top, y);
                right = Math.max(right, x);
                bottom = Math.max(bottom, y);
              }
            }
          }
          if (right < left || bottom < top) {
            throw new Error("corpus_empty_glyph");
          }
          const width = right - left + 1;
          const height = bottom - top + 1;
          const alpha = new Uint8Array(width * height);
          for (let y = 0; y < height; ++y) {
            for (let x = 0; x < width; ++x) {
              alpha[y * width + x] =
                pixels[((top + y) * canvas.width + left + x) * 4 + 3]!;
            }
          }
          let binary = "";
          for (const value of alpha) {
            binary += String.fromCharCode(value);
          }
          output.push({
            advanceWidth: Math.ceil(context.measureText("M").width),
            alphaBase64: btoa(binary),
            character,
            fontSizePx,
            height,
            offsetX: left - drawX,
            offsetY: top - baseline,
            width,
          });
        }
      }
      return output;
    },
    { chars: characters, familyName: family, sizes: FONT_SIZES },
  );
  return raw.map((glyph) => {
    const alpha = Buffer.from(glyph.alphaBase64, "base64");
    return {
      ...glyph,
      alphaSha256: sha256(alpha),
      fontId: font.id,
      id: rasterId(font.id, glyph.fontSizePx, glyph.character),
    };
  });
}

function region(
  id: string,
  truth: string,
  fontId: string,
  fontSizePx: number,
  x: number,
  baseline: number,
  catalog: ReadonlyMap<string, GlyphRaster>,
): TextRegion {
  const first = catalog.get(rasterId(fontId, fontSizePx, "M"));
  if (!first) {
    throw new Error("corpus_reference_glyph_missing");
  }
  const glyphs: GlyphInstance[] = [];
  let visibleIndex = 0;
  for (let index = 0; index < truth.length; ++index) {
    const character = truth[index]!;
    if (character === " ") {
      continue;
    }
    const raster = catalog.get(rasterId(fontId, fontSizePx, character));
    if (!raster) {
      throw new Error(`corpus_glyph_missing_${character}`);
    }
    const glyphX = x + index * first.advanceWidth + raster.offsetX;
    const glyphY = baseline + raster.offsetY;
    glyphs.push({
      glyphIndex: visibleIndex++,
      rasterId: raster.id,
      smallGlyphSubset: raster.height >= 8 && raster.height <= 10,
      x: glyphX,
      y: glyphY,
    });
  }
  return {
    boundingBox: [
      x - 12,
      baseline - fontSizePx - 12,
      truth.length * first.advanceWidth + 24,
      fontSizePx + 24,
    ],
    glyphs,
    id,
    smallGlyphRegion: fontSizePx === 12,
    truth,
  };
}

function sampleFrame(
  config: SplitConfig,
  catalog: ReadonlyMap<string, GlyphRaster>,
  stratum: string,
  sequenceIndex: number,
  sampleIndex: number,
  motionCategory: "static" | "slow" | "rapid",
): SampleFrame {
  const frameId = SAMPLE_FRAMES[sampleIndex]!;
  const motionIndex =
    motionCategory === "static"
      ? 0
      : motionCategory === "slow"
        ? Math.floor(sampleIndex / 2)
        : sampleIndex;
  const horizontalOffset = ((sequenceIndex * 17 + motionIndex * 13) % 5) * 11;
  const verticalOffset = ((sequenceIndex * 7 + motionIndex * 3) % 4) * 9;
  const x = 132 + horizontalOffset;
  const normalBaseline = 342 + verticalOffset;
  const smallBaseline = 448 + verticalOffset;
  const normal = textFor(NORMAL_TOKENS, sequenceIndex, motionIndex);
  const small = textFor(SMALL_TOKENS, sequenceIndex, motionIndex);
  if (nonspaceCharacters(normal) !== 60 || nonspaceCharacters(small) !== 20) {
    throw new Error("corpus_text_character_contract_changed");
  }
  return {
    frameId,
    geometryEpoch: 1,
    sourcePtsNs: frameId * FRAME_INTERVAL_NS,
    textRegions: [
      region(
        "primary_line",
        normal,
        config.font.id,
        22,
        x,
        normalBaseline,
        catalog,
      ),
      region(
        "small_glyph_line",
        small,
        config.font.id,
        12,
        x,
        smallBaseline,
        catalog,
      ),
    ],
    uiPrimitives: [
      { boundingBox: [88, 110, 12, 820], type: "foreground_border" },
      { boundingBox: [112, 174, 620, 2], type: "selection_outline" },
      { boundingBox: [x - 4, smallBaseline + 14, 2, 18], type: "caret" },
      {
        boundingBox: [1240, 230, 430, 2],
        type:
          stratum === "slide_diagram" ? "diagram_stroke" : "control_affordance",
      },
    ],
  };
}

function stratumCounts(): Map<string, number> {
  return new Map(
    CORE_STRATA.map((stratum, index) => [stratum, index === 0 ? 10 : 9]),
  );
}

export async function generateCorpusManifest(
  page: Page,
  split: "development" | "validation",
): Promise<CorpusManifest> {
  const config = await splitConfig(split);
  await loadCorpusFont(page, config.font, config.fontFamily);
  const glyphRasterCatalog = await buildGlyphCatalog(
    page,
    config.font,
    config.fontFamily,
  );
  const catalog = new Map(glyphRasterCatalog.map((glyph) => [glyph.id, glyph]));
  const smallCharacters = new Set(SMALL_TOKENS.join(""));
  const invalidSmallGlyphs = glyphRasterCatalog.filter(
    (glyph) =>
      glyph.fontSizePx === 12 &&
      smallCharacters.has(glyph.character) &&
      (glyph.height < 8 || glyph.height > 10),
  );
  if (invalidSmallGlyphs.length !== 0) {
    throw new Error(
      `corpus_small_glyph_height_out_of_range_${invalidSmallGlyphs
        .map((glyph) => `${glyph.character}-${glyph.height}`)
        .join("_")}`,
    );
  }

  const sequences: SequenceSpec[] = [];
  let globalIndex = 0;
  for (const [stratum, count] of stratumCounts()) {
    for (let index = 0; index < count; ++index) {
      const sequenceId = `${split}-${stratum}-${String(index).padStart(2, "0")}`;
      const seed = sha256(
        `${CORPUS_PROTOCOL}|${split}|${stratum}|${index}|seed-v1`,
      );
      const motionCategory = config.motion[globalIndex % config.motion.length]!;
      sequences.push({
        durationFrames: FRAME_COUNT,
        fontId: config.font.id,
        layoutId: config.layouts[globalIndex % config.layouts.length]!,
        logicalDeviceScale: globalIndex % 3 === 0 ? 2 : 1,
        motionCategory,
        sampleFrames: SAMPLE_FRAMES.map((_, sampleIndex) =>
          sampleFrame(
            config,
            catalog,
            stratum,
            globalIndex,
            sampleIndex,
            motionCategory,
          ),
        ),
        sceneDescription: `${stratum} generated canvas application scene with deterministic post-scale text truth and typed UI primitives`,
        seed,
        sequenceId,
        stratum,
        themeId: config.themes[globalIndex % config.themes.length]!.id,
      });
      ++globalIndex;
    }
  }

  let characterInstances = 0;
  let smallGlyphInstances = 0;
  for (const sequence of sequences) {
    for (const frame of sequence.sampleFrames) {
      for (const textRegion of frame.textRegions) {
        characterInstances += textRegion.glyphs.length;
        smallGlyphInstances += textRegion.glyphs.filter(
          (glyph) => glyph.smallGlyphSubset,
        ).length;
      }
    }
  }
  const weight = 1 / CORE_STRATA.length;
  return {
    counts: {
      characterInstances,
      sampledFrames: sequences.length * SAMPLE_FRAMES.length,
      sequences: sequences.length,
      smallGlyphInstances,
    },
    frameContract: {
      durationSeconds: 8,
      frameCount: FRAME_COUNT,
      frameIntervalNs: FRAME_INTERVAL_NS,
      frameRate: FRAME_RATE,
      geometryEpoch: 1,
      height: HEIGHT,
      sampleFrames: SAMPLE_FRAMES,
      width: WIDTH,
    },
    glyphRasterCatalog,
    protocol: CORPUS_PROTOCOL,
    rendererOutputOpened: false,
    schemaVersion: 1,
    sequences,
    split,
    splitContract: {
      fontId: config.font.id,
      layoutIds: [...config.layouts],
      stratumWeights: Object.fromEntries(
        CORE_STRATA.map((stratum) => [stratum, weight]),
      ),
      themeIds: config.themes.map((theme) => theme.id),
    },
  };
}

function bufferFromDataUrl(value: string): Buffer {
  const match = /^data:image\/png;base64,([A-Za-z0-9+/=]+)$/.exec(value);
  if (!match) {
    throw new Error("corpus_png_data_url_invalid");
  }
  return Buffer.from(match[1]!, "base64");
}

export async function renderSampleFrame(
  page: Page,
  manifest: CorpusManifest,
  sequence: SequenceSpec,
  frame: SampleFrame,
): Promise<RenderedFrame> {
  const config = await splitConfig(manifest.split);
  const theme = config.themes.find(
    (candidate) => candidate.id === sequence.themeId,
  );
  if (!theme) {
    throw new Error("corpus_theme_missing");
  }
  const firstRaster = manifest.glyphRasterCatalog.find(
    (glyph) => glyph.fontSizePx === 22,
  );
  if (!firstRaster) {
    throw new Error("corpus_render_catalog_empty");
  }
  const rendered = await page.evaluate(
    ({ familyName, frameValue, height, stratum, themeValue, width }) => {
      document.body.replaceChildren();
      Object.assign(document.body.style, {
        margin: "0",
        overflow: "hidden",
      });
      const canvas = document.createElement("canvas");
      canvas.width = width;
      canvas.height = height;
      document.body.append(canvas);
      const context = canvas.getContext("2d", { alpha: false });
      if (!context) {
        throw new Error("corpus_scene_context_unavailable");
      }
      context.fillStyle = themeValue.background;
      context.fillRect(0, 0, width, height);
      context.fillStyle = themeValue.panel;
      context.fillRect(88, 110, 1080, 820);
      context.fillRect(1210, 180, 560, 610);
      context.strokeStyle = themeValue.accent;
      context.lineWidth = 2;
      context.strokeRect(112, 174, 620, 2);
      context.strokeRect(1240, 230, 430, 250);
      if (stratum === "code_editor") {
        context.fillStyle = themeValue.background;
        context.fillRect(112, 520, 980, 330);
        for (let line = 0; line < 9; ++line) {
          context.fillStyle =
            line % 3 === 0 ? themeValue.accent : themeValue.foreground;
          context.globalAlpha = line % 3 === 0 ? 0.92 : 0.34;
          context.fillRect(176, 552 + line * 30, 240 + ((line * 83) % 440), 7);
        }
        context.globalAlpha = 1;
      } else if (stratum === "terminal") {
        context.fillStyle = "#030807";
        context.fillRect(112, 520, 980, 330);
        context.fillStyle = themeValue.accent;
        for (let line = 0; line < 7; ++line) {
          context.globalAlpha = 0.48 + (line % 2) * 0.22;
          context.fillRect(148, 556 + line * 36, 320 + ((line * 97) % 480), 5);
        }
        context.globalAlpha = 1;
        context.fillRect(148, 812, 3, 18);
      } else if (stratum === "spreadsheet_table") {
        context.strokeStyle = themeValue.accent;
        context.globalAlpha = 0.52;
        for (let x = 132; x <= 1060; x += 116) {
          context.beginPath();
          context.moveTo(x, 520);
          context.lineTo(x, 850);
          context.stroke();
        }
        for (let y = 520; y <= 850; y += 47) {
          context.beginPath();
          context.moveTo(132, y);
          context.lineTo(1060, y);
          context.stroke();
        }
        for (let x = 1240; x <= 1680; x += 88) {
          context.beginPath();
          context.moveTo(x, 520);
          context.lineTo(x, 720);
          context.stroke();
        }
        for (let y = 520; y <= 720; y += 40) {
          context.beginPath();
          context.moveTo(1240, y);
          context.lineTo(1680, y);
          context.stroke();
        }
        context.globalAlpha = 1;
      } else if (stratum === "slide_diagram") {
        context.strokeStyle = themeValue.accent;
        context.lineWidth = 5;
        for (let index = 0; index < 4; ++index) {
          const nodeX = 1280 + (index % 2) * 240;
          const nodeY = 560 + Math.floor(index / 2) * 160;
          context.strokeRect(nodeX, nodeY, 150, 82);
          if (index !== 3) {
            context.beginPath();
            context.moveTo(nodeX + 75, nodeY + 82);
            context.lineTo(
              1355 + ((index + 1) % 2) * 240,
              560 + Math.floor((index + 1) / 2) * 160,
            );
            context.stroke();
          }
        }
      } else if (stratum === "browser_documentation") {
        context.fillStyle = themeValue.background;
        context.fillRect(1238, 254, 434, 48);
        context.fillStyle = themeValue.accent;
        context.globalAlpha = 0.34;
        context.fillRect(1262, 271, 250, 12);
        for (let line = 0; line < 8; ++line) {
          context.fillRect(1258, 340 + line * 43, 170 + ((line * 71) % 230), 8);
        }
        context.globalAlpha = 1;
      } else if (stratum === "mixed_video_text") {
        for (let row = 0; row < 5; ++row) {
          for (let column = 0; column < 7; ++column) {
            const phase = (frameValue.frameId / 60 + row * 3 + column * 5) % 12;
            context.fillStyle =
              phase < 6 ? themeValue.accent : themeValue.foreground;
            context.globalAlpha = 0.12 + phase * 0.045;
            context.fillRect(1244 + column * 60, 236 + row * 47, 56, 43);
          }
        }
        context.globalAlpha = 1;
      } else if (stratum === "animated_typing_scrolling") {
        context.fillStyle = themeValue.accent;
        context.fillRect(1128, 520 + ((frameValue.frameId / 2) % 270), 8, 70);
        for (let line = 0; line < 8; ++line) {
          const widthValue = 280 + ((frameValue.frameId / 2 + line * 61) % 560);
          context.globalAlpha = 0.24 + (line % 3) * 0.18;
          context.fillRect(148, 546 + line * 36, widthValue, 6);
        }
        context.globalAlpha = 1;
      }
      context.fillStyle = themeValue.foreground;
      context.textBaseline = "alphabetic";
      for (const region of frameValue.textRegions) {
        const fontSize = region.smallGlyphRegion ? 12 : 22;
        const firstGlyph = region.glyphs[0];
        if (!firstGlyph) {
          throw new Error("corpus_region_has_no_glyphs");
        }
        const x = region.boundingBox[0] + 12;
        const baseline = region.boundingBox[1] + fontSize + 12;
        const advance = Math.round(
          (region.boundingBox[2] - 24) / region.truth.length,
        );
        context.font = `400 ${fontSize}px '${familyName}'`;
        for (let index = 0; index < region.truth.length; ++index) {
          context.fillText(region.truth[index]!, x + index * advance, baseline);
        }
      }

      const ocr: Record<string, string> = {};
      for (const region of frameValue.textRegions) {
        const [sourceX, sourceY, sourceWidth, sourceHeight] =
          region.boundingBox;
        const scale = 4;
        const border = 40;
        const output = document.createElement("canvas");
        output.width = sourceWidth * scale + border * 2;
        output.height = sourceHeight * scale + border * 2;
        const outputContext = output.getContext("2d", {
          alpha: false,
          willReadFrequently: true,
        });
        if (!outputContext) {
          throw new Error("corpus_ocr_context_unavailable");
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
        ocr[region.id] = output.toDataURL("image/png");
      }
      return { frame: canvas.toDataURL("image/png"), ocr };
    },
    {
      familyName: config.fontFamily,
      frameValue: frame,
      height: HEIGHT,
      stratum: sequence.stratum,
      themeValue: theme,
      width: WIDTH,
    },
  );
  const ocrPngs = new Map<string, Buffer>();
  for (const [id, dataUrl] of Object.entries(rendered.ocr)) {
    ocrPngs.set(id, bufferFromDataUrl(dataUrl));
  }
  return { framePng: bufferFromDataUrl(rendered.frame), ocrPngs };
}

export async function prepareManifestPage(
  page: Page,
  manifest: CorpusManifest,
): Promise<void> {
  const config = await splitConfig(manifest.split);
  if (config.font.id !== manifest.splitContract.fontId) {
    throw new Error("corpus_manifest_font_mismatch");
  }
  await loadCorpusFont(page, config.font, config.fontFamily);
}

export function stableJson(value: unknown): string {
  return `${JSON.stringify(value)}\n`;
}
