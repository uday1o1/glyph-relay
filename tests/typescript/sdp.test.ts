import assert from "node:assert/strict";
import test from "node:test";

import {
  classifyH264Profile,
  evaluateRecordingProfileOffer,
} from "../../signaling/sdp.ts";

function offer(
  profileLevelId: string,
  packetizationMode = "1",
  asymmetry = "1",
): string {
  return [
    "v=0",
    "m=video 9 UDP/TLS/RTP/SAVPF 102 103 104",
    "a=rtpmap:102 H264/90000",
    `a=fmtp:102 level-asymmetry-allowed=${asymmetry};packetization-mode=${packetizationMode};profile-level-id=${profileLevelId}`,
    "a=rtcp-fb:102 nack",
    "a=rtcp-fb:102 nack pli",
    "a=rtpmap:103 rtx/90000",
    "a=fmtp:103 apt=102",
    "a=rtpmap:104 VP8/90000",
    "",
  ].join("\r\n");
}

test("accepts semantic constrained baseline Level 4 with packetization mode 1", () => {
  for (const profile of ["42c028", "42e028"]) {
    const result = evaluateRecordingProfileOffer(offer(profile));
    assert.equal(result.compatible, true);
    assert.equal(result.reason, "recording_profile_offer_compatible");
    assert.equal(result.formats[0]?.profileFamily, "constrained_baseline");
    assert.deepEqual(result.rtxPayloadTypes, [103]);
  }
});

test("rejects the observed Level 3.1 profile and strict predicate mismatches", () => {
  assert.deepEqual(
    evaluateRecordingProfileOffer(offer("42e01f")).compatible,
    false,
  );
  assert.deepEqual(
    evaluateRecordingProfileOffer(offer("42e028", "0")).compatible,
    false,
  );
  assert.deepEqual(
    evaluateRecordingProfileOffer(offer("42e028", "1", "0")).compatible,
    false,
  );
  assert.equal(
    evaluateRecordingProfileOffer(offer("42e01f")).reason,
    "recording_profile_offer_lacks_level4_constrained_baseline_packetization1",
  );
});

test("rejects spoofed, duplicate, oversized, and malformed SDP", () => {
  const spoofed = `${offer("42e01f")}a=fmtp:120 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e028\r\n`;
  assert.equal(
    evaluateRecordingProfileOffer(spoofed).reason,
    "sdp_attribute_payload_not_advertised",
  );
  const duplicate = offer("42e028").replace(
    "a=rtpmap:102 H264/90000",
    "a=rtpmap:102 H264/90000\r\na=rtpmap:102 H264/90000",
  );
  assert.equal(
    evaluateRecordingProfileOffer(duplicate).reason,
    "sdp_rtpmap_duplicate_or_invalid",
  );
  assert.equal(
    evaluateRecordingProfileOffer(
      `v=0\r\nm=video 9 RTP/AVP 102\r\na=x:${"x".repeat(2_049)}`,
    ).reason,
    "sdp_line_too_large",
  );
  assert.equal(
    evaluateRecordingProfileOffer("v=0\r\nm=audio 9 RTP/AVP 0\r\n").reason,
    "sdp_video_section_missing",
  );
});

test("profile classification follows RFC 6184 semantic masks", () => {
  assert.equal(classifyH264Profile(0x42, 0xc0), "constrained_baseline");
  assert.equal(classifyH264Profile(0x42, 0xe0), "constrained_baseline");
  assert.equal(classifyH264Profile(0x4d, 0x80), "constrained_baseline");
  assert.equal(classifyH264Profile(0x64, 0x0c), "constrained_high");
  assert.equal(classifyH264Profile(0x77, 0), "unknown");
});
