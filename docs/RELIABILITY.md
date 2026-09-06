# Reliability

The executable runs a bounded agent turn and exits after its owned work completes.
Provider, tool, permission and storage failures remain explicit results. Partial
streaming text is not a successful answer. SIGINT/SIGTERM requests cancellation;
borrowed contexts remain alive until tool dispatches finish.

Trace and audit metadata is stored in the selected state directory. Correlate
turn IDs with tool outcomes through the SQLite repositories. There is no trace
export command or metrics server in the minimal executable.

## Required Environment

Install the toolchain and libcurl development files in [BUILD_SYSTEM](BUILD_SYSTEM.md).
Supply the provider API-key variable named by the selected profile. Configuration
may use `${NAME}` or `${NAME:-fallback}` substitution; missing required values
fail configuration loading. No external account is needed for tests.

## Verification

Controlled provider/transport fixtures and temporary databases cover normal
turns, protocol errors, permissions, persistence and cancellation. Real-model
runs require supplied credentials and remain a separate acceptance gate.
[STATUS](STATUS.md) records evidence; [live debt](exec-plans/tech-debt-tracker.md)
records unresolved ownership, persistence and hosted-quality work.
