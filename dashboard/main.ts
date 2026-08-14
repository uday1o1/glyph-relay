import { startDashboardServer } from "./server.ts";

function configuredPort(): number {
  const raw = process.env.GLYPHRELAY_DASHBOARD_PORT;
  if (raw === undefined) {
    return 0;
  }
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || value < 1 || value > 65_535) {
    throw new Error("glyphrelay_dashboard_port_invalid");
  }
  return value;
}

async function main(): Promise<void> {
  const dashboard = await startDashboardServer({ port: configuredPort() });
  process.stdout.write(
    `Open the GlyphRelay dashboard: ${dashboard.launchUrl}\n`,
  );

  let stopping = false;
  const stop = (): void => {
    if (stopping) {
      return;
    }
    stopping = true;
    dashboard
      .close()
      .then(() => process.exit(0))
      .catch((error: unknown) => {
        process.stderr.write(
          `dashboard shutdown failed: ${error instanceof Error ? error.message : "unknown"}\n`,
        );
        process.exit(1);
      });
  };
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
}

main().catch((error: unknown) => {
  process.stderr.write(
    `dashboard startup failed: ${error instanceof Error ? error.message : "unknown"}\n`,
  );
  process.exitCode = 1;
});
