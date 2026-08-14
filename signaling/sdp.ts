const MAXIMUM_SDP_BYTES = 64 * 1024;
const MAXIMUM_SDP_LINE_BYTES = 2_048;

export interface H264OfferFormat {
  payloadType: number;
  profileLevelId: string;
  profileIdc: number;
  profileIop: number;
  levelIdc: number;
  profileFamily: string;
  packetizationMode: number;
  levelAsymmetryAllowed: boolean;
  feedback: readonly string[];
}

export interface RecordingProfileOfferResult {
  compatible: boolean;
  reason: string;
  presentation: string;
  requiredLevelIdc: number;
  formats: readonly H264OfferFormat[];
  videoPayloadTypes: readonly number[];
  rtxPayloadTypes: readonly number[];
}

const SHARING_PRESENTATIONS: Readonly<Record<string, number>> = Object.freeze({
  "720p30": 31,
  "720p24": 31,
  "720p15": 31,
});

function fail(
  reason: string,
  presentation: string,
  requiredLevelIdc: number,
): RecordingProfileOfferResult {
  return {
    compatible: false,
    reason,
    presentation,
    requiredLevelIdc,
    formats: [],
    videoPayloadTypes: [],
    rtxPayloadTypes: [],
  };
}

function parsePayloadType(raw: string): number | undefined {
  if (!/^\d{1,3}$/.test(raw)) {
    return undefined;
  }
  const value = Number(raw);
  return Number.isSafeInteger(value) && value >= 0 && value <= 127
    ? value
    : undefined;
}

function parseParameters(raw: string): ReadonlyMap<string, string> | undefined {
  const parameters = new Map<string, string>();
  for (const rawItem of raw.split(";")) {
    const item = rawItem.trim();
    const equals = item.indexOf("=");
    if (equals <= 0 || equals === item.length - 1) {
      return undefined;
    }
    const name = item.slice(0, equals).trim().toLowerCase();
    const value = item
      .slice(equals + 1)
      .trim()
      .toLowerCase();
    if (!name || !value || parameters.has(name)) {
      return undefined;
    }
    parameters.set(name, value);
  }
  return parameters;
}

export function classifyH264Profile(
  profileIdc: number,
  profileIop: number,
): string {
  if (
    (profileIdc === 0x42 && (profileIop & 0x40) !== 0) ||
    (profileIdc === 0x4d && (profileIop & 0x80) !== 0) ||
    (profileIdc === 0x58 && (profileIop & 0xc0) === 0xc0)
  ) {
    return "constrained_baseline";
  }
  if (profileIdc === 0x42) {
    return "baseline";
  }
  if (profileIdc === 0x4d) {
    return "main";
  }
  if (profileIdc === 0x58) {
    return "extended";
  }
  if (profileIdc === 0x64 && (profileIop & 0x0c) === 0x0c) {
    return "constrained_high";
  }
  if (profileIdc === 0x64) {
    return "high";
  }
  return "unknown";
}

export function evaluateRecordingProfileOffer(
  sdp: string,
  presentation = "720p30",
): RecordingProfileOfferResult {
  const requiredLevelIdc = SHARING_PRESENTATIONS[presentation];
  if (requiredLevelIdc === undefined) {
    return fail("sharing_presentation_not_supported", presentation, 0);
  }
  if (
    !sdp ||
    Buffer.byteLength(sdp, "utf8") > MAXIMUM_SDP_BYTES ||
    sdp.includes("\0")
  ) {
    return fail("sdp_size_or_encoding_invalid", presentation, requiredLevelIdc);
  }

  const videoPayloadTypes: number[] = [];
  const advertised = new Set<number>();
  const codecs = new Map<number, string>();
  const parameters = new Map<number, ReadonlyMap<string, string>>();
  const feedback = new Map<number, string[]>();
  let inVideoSection = false;
  let sawVideoSection = false;

  for (const rawLine of sdp.split("\n")) {
    const line = rawLine.endsWith("\r") ? rawLine.slice(0, -1) : rawLine;
    if (Buffer.byteLength(line, "utf8") > MAXIMUM_SDP_LINE_BYTES) {
      return fail("sdp_line_too_large", presentation, requiredLevelIdc);
    }
    if (line.startsWith("m=")) {
      inVideoSection = line.startsWith("m=video ");
      sawVideoSection ||= inVideoSection;
      if (inVideoSection) {
        const fields = line.split(" ");
        if (fields.length < 4) {
          return fail(
            "sdp_video_media_line_malformed",
            presentation,
            requiredLevelIdc,
          );
        }
        for (const rawPayload of fields.slice(3)) {
          const payload = parsePayloadType(rawPayload);
          if (payload === undefined || advertised.has(payload)) {
            return fail(
              "sdp_video_payload_list_invalid",
              presentation,
              requiredLevelIdc,
            );
          }
          advertised.add(payload);
          videoPayloadTypes.push(payload);
        }
      }
      continue;
    }
    if (!inVideoSection) {
      continue;
    }

    const attribute = /^(a=(?:rtpmap|fmtp|rtcp-fb)):(\d{1,3}) (.+)$/.exec(line);
    if (!attribute) {
      continue;
    }
    const kind = attribute[1];
    const payload = parsePayloadType(attribute[2] ?? "");
    const value = attribute[3] ?? "";
    if (payload === undefined || !advertised.has(payload)) {
      return fail(
        "sdp_attribute_payload_not_advertised",
        presentation,
        requiredLevelIdc,
      );
    }
    if (kind === "a=rtpmap") {
      if (codecs.has(payload)) {
        return fail(
          "sdp_rtpmap_duplicate_or_invalid",
          presentation,
          requiredLevelIdc,
        );
      }
      codecs.set(payload, value.toLowerCase());
    } else if (kind === "a=fmtp") {
      const parsed = parseParameters(value);
      if (!parsed || parameters.has(payload)) {
        return fail(
          "sdp_fmtp_duplicate_or_invalid",
          presentation,
          requiredLevelIdc,
        );
      }
      parameters.set(payload, parsed);
    } else {
      const values = feedback.get(payload) ?? [];
      values.push(value.toLowerCase());
      feedback.set(payload, values);
    }
  }

  if (!sawVideoSection) {
    return fail("sdp_video_section_missing", presentation, requiredLevelIdc);
  }
  if (videoPayloadTypes.length === 0) {
    return fail(
      "sdp_video_payload_list_invalid",
      presentation,
      requiredLevelIdc,
    );
  }

  const formats: H264OfferFormat[] = [];
  const rtxPayloadTypes: number[] = [];
  for (const payloadType of videoPayloadTypes) {
    const codec = codecs.get(payloadType);
    if (codec === "rtx/90000") {
      rtxPayloadTypes.push(payloadType);
      continue;
    }
    if (codec !== "h264/90000") {
      continue;
    }
    const fmtp = parameters.get(payloadType);
    const profileLevelId = fmtp?.get("profile-level-id");
    const rawPacketizationMode = fmtp?.get("packetization-mode");
    const rawLevelAsymmetry = fmtp?.get("level-asymmetry-allowed");
    if (
      !profileLevelId ||
      !/^[0-9a-f]{6}$/.test(profileLevelId) ||
      !rawPacketizationMode ||
      !/^[0-2]$/.test(rawPacketizationMode) ||
      (rawLevelAsymmetry !== "0" && rawLevelAsymmetry !== "1")
    ) {
      continue;
    }
    const profileIdc = Number.parseInt(profileLevelId.slice(0, 2), 16);
    const profileIop = Number.parseInt(profileLevelId.slice(2, 4), 16);
    const levelIdc = Number.parseInt(profileLevelId.slice(4, 6), 16);
    formats.push({
      payloadType,
      profileLevelId,
      profileIdc,
      profileIop,
      levelIdc,
      profileFamily: classifyH264Profile(profileIdc, profileIop),
      packetizationMode: Number(rawPacketizationMode),
      levelAsymmetryAllowed: rawLevelAsymmetry === "1",
      feedback: Object.freeze([...(feedback.get(payloadType) ?? [])]),
    });
  }

  if (formats.length === 0) {
    return {
      compatible: false,
      reason: "sdp_no_explicit_h264_format",
      presentation,
      requiredLevelIdc,
      formats,
      videoPayloadTypes,
      rtxPayloadTypes,
    };
  }
  const compatible = formats.some(
    (format) =>
      format.profileFamily === "constrained_baseline" &&
      format.levelIdc >= requiredLevelIdc &&
      format.packetizationMode === 1 &&
      format.levelAsymmetryAllowed,
  );
  return {
    compatible,
    reason: compatible
      ? "sharing_profile_offer_compatible"
      : "sharing_profile_offer_lacks_selected_level_constrained_baseline_packetization1",
    presentation,
    requiredLevelIdc,
    formats,
    videoPayloadTypes,
    rtxPayloadTypes,
  };
}
