# Environment and capability diagnostics

`glyphrelay doctor` is the supported read-only diagnostic for sender prerequisites and selected runtime capabilities.

Its text output is intended for people, while `glyphrelay doctor --json` is the stable automation interface described by `schemas/doctor-v1.schema.json`.

The command returns diagnostic output successfully even when the sender is unsupported.

Callers must use `decision.mode` and `decision.reasons` rather than the process exit status to interpret capability support.

## Probe behavior

The collector identifies the operating system, architecture, and normalized desktop session without reporting hostnames or account names.

On Linux it queries ScreenCast portal source and cursor flags through the session bus, loads PipeWire to obtain its library version, initializes the CUDA driver, queries the CUDA runtime, and calls `NvEncodeAPIGetMaxSupportedVersion` before any later NVENC function-table load.

The NVENC API decision rejects a driver-reported maximum below the compiled 13.1 API and rejects a discovered NVIDIA driver below the locked 610 minimum.

H.264, emphasis-map, input-format, geometry, and session-count fields remain `not_probed` until the device-level encoder capability adapter implemented by the NVENC feasibility path supplies real results.

The system OpenH264 probe requires the versioned runtime symbol and reports only its numeric version.

Browser probes execute a configured project-local path or a known browser command with `--version` and retain only the numeric version.

`GLYPHRELAY_CHROMIUM_PATH` and `GLYPHRELAY_FIREFOX_PATH` may point qualification at the locked Playwright executables.

The signaling and TURN checks report only validation categories.

They never echo the configured origin, TURN URL, user information, or credentials.

Every spawned diagnostic command has a five-second timeout and a bounded output buffer.

## Decision modes

`unsupported_sender` means the host is not the supported Linux x86-64 sender platform.

`unsupported` means a required capture, privacy, or encoder capability has a definitive unavailable or incompatible result.

`diagnostic_only` means at least one required capability or the frozen recording profile still needs qualification.

`cpu_fallback` requires verified portal capture, shared-memory capture, privacy hooks, the frozen recording profile, and the locked system OpenH264 path.

`uniform_nvenc` adds a compatible NVENC API and verified H.264 support but does not claim emphasis-map operation.

`enhanced_nvenc` additionally requires verified emphasis-map support.

No unqueried capability can produce an enhanced-mode decision.

## Safe attachment contract

The JSON schema permits only fixed probe statuses and machine-readable lowercase reason identifiers.

The serializers replace path-like values, URLs, and invalid dynamic reason strings before output.

The collector never reads window titles, captured content, usernames, home directories, tokens, passwords, or raw network endpoints into the report.

Qualification artifacts apply a second repository-level redaction and manifest check before any public export.
