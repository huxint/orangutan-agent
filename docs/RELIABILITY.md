# Reliability

The runtime is meant to be **boring to operate**: start it, observe it, restart it,
trust the audit log. This doc captures the operational expectations.

## Startup And Health

- `orangutan` checks for a stale `<workspace>/.orangutan/lock` file on start; if the
  PID isn't live, removes it; otherwise refuses to start.
- Health: the desktop app surfaces a status panel (uptime, active agents); the headless
  `orangutan-server` reports liveness via structured logs plus the lock/PID file. There
  is no HTTP health endpoint (an `orangutan-server`-only `GET /healthz` is a possible
  future option).
- Liveness for systemd: `Type=notify` + `sd_notify(READY=1)` once the runtime's
  executors are running.

## Logging

- spdlog through the `oran-log` shim.
- Default level: `info` (console), `debug` (file).
- File sink: `<workspace>/.orangutan/logs/orangutan-YYYY-MM-DD.log`, daily rotation,
  7-day retention. Configurable.
- Structured fields preferred over composed messages: `log::info("tool dispatched",
  field("tool", name), field("ms", duration_ms))`.

## Metrics And Tracing

- v1: counters are surfaced in the desktop app's status panel and emitted to structured
  logs; there is no HTTP metrics endpoint (an `orangutan-server`-only `GET /metrics` in
  Prometheus exposition format is a possible future option).
- Trace inspection is SQLite-native today: `orangutan --trace <turn-id>` prints a
  human-readable `trace_turns` row plus joined audit rows, and
  `orangutan --trace-export <turn-id>` emits the same single turn as one JSON
  Lines object to stdout for log/SIEM ingestion. Both commands read
  `<workspace>/.orangutan/audit.db`, run the idempotent audit migration first,
  and preserve the redacted trace/audit contract: no raw prompt bytes, tool
  inputs, provider bodies, or secrets are added by the exporter.
- v1.1: distributed tracing via OpenTelemetry (stretch — adds a dependency; gate it
  behind `--obs_otel=y`).

## Required Environment

| Variable                  | Required? | Default | Purpose |
| ------------------------- | --------- | ------- | ------- |
| `ANTHROPIC_API_KEY`       | Yes when a configured route uses an Anthropic profile | — | Anthropic provider creds; read by configured-route `bootstrap::run` through the explicit provider credential resolver. |
| `OPENAI_API_KEY`          | Yes when a configured route uses an OpenAI profile | — | OpenAI provider creds; read by configured-route `bootstrap::run` through the explicit provider credential resolver. |
| `QQ_APP_ID` (or the env named by `channels[].qq_app_id_env`) | Yes when registering a QQ channel under `--channel_qq=y` | — | QQ bot app id; read by `register_configured_channels(...)` only in enabled QQ builds. |
| `QQ_CLIENT_SECRET` (or the env named by `channels[].qq_client_secret_env`) | Yes when registering a QQ channel under `--channel_qq=y` | — | QQ bot client secret; read by `register_configured_channels(...)` only in enabled QQ builds and never logged. |
| `ORAN_SECRET_PASSWORD`    | Recommended | —     | Decrypts secret-protected config fields. |
| `ORAN_WORKSPACE`          | No         | `cwd` | Workspace path. |
| `ORAN_TEST_REAL_PROVIDERS` | No (tests only) | unset | Enable real-provider integration tests. |

## Opt-In Smoke Environment

Real-network smoke tests are hidden and excluded from ordinary `xmake test` /
`make ci` runs. Explicit smoke commands no-op successfully until the opt-in
variable and required credentials are present. They are operator/nightly gates:
set the opt-in variable plus the required credentials, run the named test, and
send one message to the bot before the timeout. Secret values must stay in the
environment and must not be printed in logs or copied into config files.

### QQ Registered-Path Smoke

Command shape:

```sh
xmake f -m release --channel_qq=y
xmake build test-bootstrap
ORAN_TEST_QQ_REAL_SMOKE=1 \
ORAN_TEST_QQ_APP_ID=... \
ORAN_TEST_QQ_CLIENT_SECRET=... \
build/linux/x86_64/release/test-bootstrap \
  "registered QQ channels real-smoke one operator message through the routed prompt path"
```

| Variable | Required? | Default | Purpose |
| -------- | --------- | ------- | ------- |
| `ORAN_TEST_QQ_REAL_SMOKE` | Yes (`1`) | unset | Opts into the hidden real QQ smoke test. Without it, an explicitly selected smoke command returns a no-op success. |
| `ORAN_TEST_QQ_APP_ID` | Yes | — | QQ bot app id consumed through `channels[].qq_app_id_env`; do not log the value. |
| `ORAN_TEST_QQ_CLIENT_SECRET` | Yes | — | QQ bot client secret consumed through `channels[].qq_client_secret_env`; do not log the value. |
| `ORAN_TEST_QQ_GATEWAY_URL` | No | discovered through `GET /gateway/bot` | Optional real QQ gateway WebSocket URL override. Leave unset for the smoke to discover the current bot gateway through the QQ API. |
| `ORAN_TEST_QQ_TOKEN_URL` | No | `https://bots.qq.com/app/getAppAccessToken` | Override token endpoint for platform staging or diagnostics. |
| `ORAN_TEST_QQ_API_BASE_URL` | No | `https://api.sgroup.qq.com` | Override QQ API base URL for gateway discovery, platform staging, or diagnostics. |
| `ORAN_TEST_QQ_CHANNEL_ID` | No | `qq-real-smoke` | Configured channel id used by the smoke. |
| `ORAN_TEST_QQ_AGENT_KEY` | No | `qq-smoke` | Configured agent key used for the trace row. |
| `ORAN_TEST_QQ_REPLY_TEXT` | No | `orangutan qq real smoke ok` | Deterministic fake-provider reply text sent through QQ passive reply. |
| `ORAN_TEST_QQ_TIMEOUT_MS` | No | `60000` | Time allowed for gateway connect, operator message arrival, reply send, and shutdown. |

## Retries / Backoff

- Provider retries on `network`, `rate_limit`, `upstream`. Backoff:
  exponential with jitter; cap 30 s; max attempts 5 (config: `provider.retry`).
- Fallback model switch on retryable failure of primary after the configured
  attempt count.
- Channel adapters retry on transient HTTP errors with the same shape.

## Timeouts

- Provider request: `provider.request_timeout_seconds` (default 90).
- Tool execution: per-tool `timeout_seconds` (default 60).
- Hook execution: `hooks.timeout_ms` (default 2000).
- Channel inbound: `channel.<id>.message_deadline_seconds` (default 300).

## Cancellation

- SIGINT / SIGTERM signals the runtime's root cancellation_signal.
- Runtime waits up to `shutdown_grace_seconds` (default 10) for in-flight work,
  then force-closes channels and exits.

## Backups

- SQLite databases are routine backup targets:
  `<workspace>/.orangutan/{sessions,memory,automation,audit}.db`.
- `scripts/backup-db.sh` (TBD) snapshots all four with `sqlite3 .backup`.
- Recommended cadence depends on operator workload; document in your deployment
  runbook.

## Incident Notes

A `docs/histories/YYYY-MM/incidents-*.md` slot is appropriate for postmortems of
production incidents. Keep them short, technical, and blameless.

## Failure Modes

| Failure                       | Behavior |
| ----------------------------- | -------- |
| Disk full                     | Storage layer returns `Error::storage`; agent surfaces error to user. Audit log entry. |
| Out of memory                 | OS kills process; supervisor restarts. |
| Provider 5xx storm            | Retry + fallback model. Hook `provider.fallback` fires. |
| Channel API rate-limit        | Adapter pauses inbound, surfaces hook `channel.rate_limited` (planned). |
| Mailbox overflow              | `try_send` returns `MailboxOverflowed`; team strategy may divert / drop. |
| Hook timeout (blocking sink)  | Triggering action denied; logged at WARN. |

## See Also

- [`SECURITY.md`](SECURITY.md)
- [`CICD.md`](CICD.md)
- [`design-docs/async-model.md`](design-docs/async-model.md) (cancellation,
  backpressure)
