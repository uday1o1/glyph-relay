import assert from "node:assert/strict";
import test from "node:test";
import { resolve } from "node:path";

import { parseArguments } from "../../tooling/controller/qualify-live-receiver.ts";

const JOIN =
  "http://127.0.0.1:8443/#join=abcdefghijklmnopqrstuv.ABCDEFGHIJKLMNOPQRSTUV";

test("controller receiver accepts the frozen loopback entry path", () => {
  assert.deepEqual(
    parseArguments([
      "--browser",
      "chromium",
      "--duration-ms",
      "130000",
      "--join-url",
      JOIN,
      "--output",
      "result.json",
    ]),
    {
      browser: "chromium",
      durationMilliseconds: 130000,
      joinUrl: JOIN,
      output: resolve("result.json"),
    },
  );
});

test("controller receiver rejects non-loopback and malformed arguments", () => {
  assert.throws(
    () =>
      parseArguments([
        "--browser",
        "firefox",
        "--duration-ms",
        "58000",
        "--join-url",
        JOIN.replace("127.0.0.1", "10.77.2.2"),
        "--output",
        "result.json",
      ]),
    /controller_receiver_join_url_invalid/,
  );
  assert.throws(
    () =>
      parseArguments([
        "--browser",
        "webkit",
        "--duration-ms",
        "58000",
        "--join-url",
        JOIN,
        "--output",
        "result.json",
      ]),
    /controller_receiver_browser_invalid/,
  );
});
