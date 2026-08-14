import { readFileSync } from "node:fs";
import { request } from "node:http";
import { request as secureRequest } from "node:https";

const port = Number(process.env.GLYPHRELAY_PORT ?? "8443");
const origin = new URL(
  process.env.GLYPHRELAY_SIGNALING_ORIGIN ?? `http://127.0.0.1:${port}`,
);
const makeRequest = origin.protocol === "https:" ? secureRequest : request;
const healthcheckCaPath = process.env.GLYPHRELAY_HEALTHCHECK_CA_PATH;

const healthRequest = makeRequest(
  {
    ca: healthcheckCaPath ? readFileSync(healthcheckCaPath) : undefined,
    headers: { Host: origin.host },
    host: "127.0.0.1",
    method: "GET",
    path: "/healthz",
    port,
    protocol: origin.protocol,
    servername: origin.hostname,
    timeout: 2_000,
  },
  (response) => {
    response.resume();
    process.exitCode = response.statusCode === 200 ? 0 : 1;
  },
);

healthRequest.once("error", () => {
  process.exitCode = 1;
});
healthRequest.once("timeout", () => {
  healthRequest.destroy();
  process.exitCode = 1;
});
healthRequest.end();
