# Configuration And State

The host supplies parsed configuration through `Config::parse` or
`Config::load_file`. Strict loading rejects unknown root, runtime, trace, hook,
provider, memory, permission and agent fields. `LoadOptions` selects strict
errors or warnings. Parse configuration before constructing provider services.

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

## Settings And Ownership

| Section | Consumer |
| --- | --- |
| `runtime.workers` | Host maps the blocking worker count to `async::RuntimeConfig`. |
| `runtime.request_timeout_ms`, `runtime.stream.max_bytes` | Host maps transport bounds to `HttpProviderBackendOptions`. |
| `runtime.tool_output` | `AgentSession` applies model-visible text and structured output byte limits. |
| `runtime.tool_scheduler` | `AgentSession` applies parallelism, timeout and idle path-lock limits. |
| `runtime.prompt.active_tools` | `AgentSession` selects the active catalogue. |
| `trace.enabled` | Host maps the trace switch to `RuntimeAssemblyOptions`. |
| `profiles`, `routes` | `HttpProviderBackend` selects model, protocol, endpoint, credentials and model policy; `route_name` defaults to `default`. |
| `permissions`, `agents.<name>.permissions` | `AgentSession` materializes global and selected-agent rules. Host maps workspace roots to `WorkspaceOptions`. |
| `agents.<name>.prompt_overlay` | `AgentSession` selects stable agent instructions. |
| `hooks.timeout_ms` | Host maps the hook deadline to `RuntimeAssemblyOptions`. |
| `memory.longterm.recall` | Host maps recall enablement, limit and kinds to `AgentSessionOptions`. |

## Credentials

Profiles store an `api_key_env` name. Credential resolution reads the named
variable or an explicitly injected lookup at the provider construction boundary.
Values are excluded from diagnostics. The parser supports `${NAME}` and
`${NAME:-fallback}` for configuration substitution; prefer references over
embedding credentials in JSON. No credential encryption/store is implemented.

## State

The host selects the workspace and database paths. `RuntimeAssembly` defaults to
`sessions.db`, `memory.db` and `audit.db` under `<workspace>/.orangutan`. The host
owns directory privacy and exclusion; `io::PrivateDirectory` provides private
files and a directory lock.

`AgentSessionOptions` carries the session ID, agent key, approval identity and
memory scope. Reusing a session ID and agent key resumes the stored conversation.
Long-term recall uses the supplied scope; workspace-to-scope mapping is host policy.

No automatic import or unrelated-history merging occurs. Preserve database
files and migration records through refactors. Backup/import tooling remains
explicit work in [live debt](../exec-plans/tech-debt-tracker.md).
