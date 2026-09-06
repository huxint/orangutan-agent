# Configuration And State

The executable requires `--config FILE` and loads it strictly. Configuration is
parsed before credentials or provider operations. Unknown root, runtime, trace,
hook, provider, memory, permission and agent fields are errors under this mode.
Library callers may choose warnings instead through `LoadOptions`.

The config file contains:

```jsonc
{
  "strict_config": true,
  "runtime": {},
  "trace": {},
  "profiles": {},
  "routes": {},
  "permissions": {},
  "agents": {},
  "hooks": {},
  "memory": {}
}
```

## Consumed Settings

| Section | Runtime meaning |
| --- | --- |
| `runtime.workers` | Blocking worker count; coordination uses one IO worker and a strand. |
| `runtime.request_timeout_ms`, `runtime.stream.max_bytes` | Provider request deadline and response byte cap. |
| `runtime.tool_output` | Model-visible text and structured output byte limits. |
| `runtime.tool_scheduler` | Parallel tool count, per-call timeout and idle path-lock TTL. |
| `runtime.prompt.active_tools` | Default or explicit active catalogue selection. |
| `trace.enabled` | Persist redacted turn metadata alongside audit. |
| `profiles` | Model, protocol, endpoint, credential reference and per-profile cache/thinking/pricing policy. |
| `routes` | Primary and fallback profile names; the executable uses `default`. |
| `permissions`, `agents.<name>.permissions` | Materialized global and selected-agent tool policy. |
| `agents.<name>.prompt_overlay` | Stable agent instructions. |
| `hooks.timeout_ms` | Blocking hook deadline. |
| `memory.longterm.recall` | Enabled flag, result limit (1–20) and record-kind filter. |

## Credentials

Profiles store an `api_key_env` name. Credential resolution reads the named
variable or an explicitly injected lookup at the provider construction boundary.
Values are excluded from diagnostics. The parser supports `${NAME}` and
`${NAME:-fallback}` for configuration substitution; prefer references over
embedding credentials in JSON. No credential encryption/store is implemented.

## State

The default workspace is the current directory, canonicalized before execution.
The default state directory is `<workspace>/.orangutan`; `--state` selects an
explicit location. It contains `sessions.db`, `memory.db`, `audit.db` and the
runtime ownership lock. The host requires a private, owned directory.

A generated session ID is printed to stderr. `--session` resumes that ID with the
selected agent key. Long-term memory scope is the canonical workspace path.
Changing the workspace deliberately changes that scope.

No automatic import or unrelated-history merging occurs. Preserve database
files and migration records through refactors. Atomic turn commits and
backup/import tooling remain explicit work in [live debt](../exec-plans/tech-debt-tracker.md).
