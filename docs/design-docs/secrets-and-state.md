# Secrets And State

This document covers how Orangutan v2 stores configuration, secrets, sessions, memory,
and audit data. The legacy choices (JSON config + AES-256-GCM via mbedtls + four
ad-hoc SQLite tables) are revisited for compile-time and crypto-hygiene reasons.

## Configuration

### File Layout

The default config path is `<workspace>/.orangutan/config.json` (or `--config <path>`).
`oran-bootstrap` loads the default file when present; in the current early runtime
slice, a missing default file falls back to built-in config defaults so a fresh
checkout remains runnable. An explicit `--config` path is strict and must load
successfully. The config file contains:

```jsonc
{
  "runtime":     { /* executor sizing, deadlines, redaction patterns */ },
  "permissions": { /* default, allow, deny, ask */ },
  "profiles":    { /* LLM provider profiles */ },
  "routes":      { /* primary + fallbacks per logical route */ },
  "agents":      { /* per-agent overrides */ },
  "teams":       { /* team definitions */ },
  "channels":    [ /* per-adapter config */ ],
  "hooks":       { /* sinks + bindings */ },
  "memory":      { /* tier policies */ },
  "automation":  { /* job seeds */ },
  "web":         { /* server config */ },
  "session":     { /* auto-save, persistence */ }
}
```

A sample is checked in at `config.example.json` and is loaded by
`tests/config/test_config.cpp`.

Current implementation status:

- `orangutan::config::Config::parse(std::string_view, LoadOptions)` parses JSON text.
- `Config::load_file(std::string_view, LoadOptions)` reads and parses a file.
- Typed fields currently cover `strict_config`, `runtime` (including
  `tool_output.max_text_bytes` / `max_data_bytes` and
  `prompt.active_tools`), top-level `trace` policy
  (`enabled`, `store_raw_bodies`, `retention_days`), top-level hook timeout
  policy (`hooks.timeout_ms`, default 2000), `profiles` (including optional
  per-profile `protocol` and `pricing`), `routes`, `session`, `web`,
  `permissions`, and `agents.<name>.permissions`, optional
  `agents.<name>.prompt_overlay` stable section-6 prompt text, optional
  `agents.<name>.skills_enabled` skill allowlists, and optional
  `agents.<name>.skills_deactivated` / `skills_expirations` skill
  activation-policy inputs. The `memory` root is typed for
  `memory.longterm.recall.enabled`, `memory.longterm.recall.limit`,
  `memory.longterm.recall.query_strategy`, and optional
  `memory.longterm.recall.kinds` `RecordKind` spellings, plus
  `memory.longterm.hybrid_search.enabled`, positive
  `lexical_limit` / `vector_limit` / `result_limit`, and non-negative finite
  `lexical_weight` / `vector_weight` with at least one non-zero weight, plus
  `memory.longterm.retention` policy fields:
  `forget_after_unused_days`, `importance_floor`, `max_records_per_scope`, and
  `decay_check_interval_hours`. Configured-route startup consumes
  `forget_after_unused_days`, `importance_floor`, and `max_records_per_scope`
  for one bounded long-term decay pass before prompt/tool reads and now reports
  the startup shadow count through runtime diagnostics. Slice 187 adds
  `oran-automation` planning that can consume the interval as a periodic
  retention cadence. Slice 188 maps config into the stored
  `MemoryRetentionJob` descriptor for future scheduler ownership. Slice 189
  adds the `oran-automation` repository that can persist that descriptor,
  `last_fired_at`, and retention run rows in `automation.db`; slice 190 adds
  the caller-driven service tick that can consume those rows and run due decay
  through a supplied backend. Slice 191 adds optional advisory `memory_decay`
  publication from that tick owner without adding persisted state or secret
  material. Slice 192 adds `AutomationRuntime::open(...)` as the explicit
  caller-owned state handle: callers provide the database path, parent
  directories are created, the automation pool is opened, migrations run, and
  repository/service lifetime stays stable. Slice 193 adds a caller-started
  retention loop step above that state handle. Slice 194 adds advisory
  retention job lifecycle hook metadata from due ticks without adding persisted
  secret material. Slice 195 adds retention lease rows in `automation.db` and
  has the explicit loop lease only due execution; the lease owner key is an
  operational identifier, not a secret. Slice 196 adds finite caller-owned loop
  policy over the same explicit path without adding secret material. Bootstrap
  still does not open that database or start a scheduler.
  Ordinary configured-route bootstrap maps the recall policy into
  prompt-boundary long-term recall. The hybrid-search block defaults disabled;
  when built with `--vector_memory=y`, configured-route bootstrap now enables the
  assembly-owned `.orangutan/memory-vectors.db` sqlite-vec backend and routes
  hybrid recall through the prompt runner. Default builds keep sqlite-vec off and
  reject `enabled=true` with `reason=build_option_disabled` and
  `option=vector_memory` before runtime assembly or provider side effects.
- `bootstrap::run` now consumes `trace.retention_days` by deriving an explicit
  Unix-nanosecond cutoff and passing it to `RuntimeAssembly`, which purges
  matching old `trace_turns` rows before exposing the long-lived trace
  repository. The purge leaves `audit_events` untouched; audit retention is a
  separate policy. `trace.store_raw_bodies` remains parsed but not consumed by
  the runtime.
- `runtime.prompt.active_tools` accepts `"defaults"` or an explicit string
  array. The loader preserves the authored array order, accepts an empty
  explicit allowlist, rejects empty tool names, and leaves registry-name
  resolution to `prompt::Builder` because `oran-config` sits below
  `oran-tool`; missing explicit names fail at the prompt layer where the
  catalog snapshot is available.
- `profiles` and `routes` are objects keyed by profile/route name. Profile entries
  require `provider`, `model`, `base_url`, and `api_key_env`; profile entries may
  include `protocol`, which is validated as a non-empty string by `oran-config` and
  parsed as an exact `provider::ProtocolKind` spelling by `oran-provider`.
  Profile entries may also include `pricing` with optional non-negative finite
  USD-per-million-token numbers:
  `input_per_million_usd`, `output_per_million_usd`,
  `cache_creation_per_million_usd`, and `cache_read_per_million_usd`.
  Route entries require `primary` and may include `fallbacks`.
- `api_key_env` is still an environment-variable name in config. It is read only
  when a caller explicitly invokes `provider::resolve_adapter_credentials(plan)`
  after provider route/profile resolution and adapter planning. The returned
  in-memory credential bundle can then be passed to
  `provider::make_adapter_system(credentials, factories)`, which constructs
  profile-routed provider backends without adding the key values to logs,
  hook payloads, or error context. Regular configured-route `bootstrap::run`
  uses this boundary when building the provider-backed prompt runner.
- `teams` and `channels` remain recognized but untyped. The config
  `automation.cron.jobs[]` block is typed: each job carries a non-empty
  `job_key`, POSIX 5-field UTC `expression`, UTC `first_fire_at`, and optional
  UTC `last_fired_at`. `oran-config` validates shape, timestamps, and unique
  job keys; bootstrap validates cron expressions through `oran-automation` and
  stores repository seed descriptors on `RuntimeAssembly`. Bootstrap still does
  not open `automation.db`, upsert rows, start timers, or execute jobs. The
  `oran-automation` C++ library exists for periodic planning, retention
  job/run/lease persistence, a caller-owned runtime state handle, a
  caller-started leased retention loop step, and finite caller-owned loop
  policy, but process scheduler/service-loop execution remains unimplemented.
  The `hooks` root has
  the v1 typed timeout field; `sinks` and `bindings` remain
  recognized-but-untyped until external hook sinks land.
- `agents.<name>.skills_enabled` accepts an explicit array of non-empty skill
  names. The parser preserves author order and does not resolve names against
  the filesystem; bootstrap applies the allowlist to the loaded workspace skill
  snapshot for callers that select that agent config. An absent allowlist keeps
  all loaded skills visible, while a present empty array means no skills are
  enabled for that agent.
- `agents.<name>.prompt_overlay` accepts a string. Bootstrap copies it into
  prompt section 6 for callers that select that agent config, unless the caller
  supplied exact `AgentPromptRunnerOptions::per_agent_overlay` bytes. The
  overlay is stable prompt-prefix text, not a permission rule block; permission
  rules remain under `agents.<name>.permissions`.

### Schema Validation

The current loader performs local type checks for the typed fields above. Missing
optional fields take defaults. Unknown fields trigger a warning unless
`strict_config=true` or `LoadOptions::strict_unknown_fields=true` is set, in which
case they are an error. This applies at the root and inside typed nested sections:
`profiles.<name>`, `profiles.<name>.pricing`, `routes.<name>`, `hooks`,
`memory.longterm.recall`, `memory.longterm.hybrid_search`,
`automation.cron`, `automation.cron.jobs[]`, permission blocks, workspace
permission blocks, and `agents.<name>`. Recognized-but-untyped root fields
(`teams`, `channels`) and reserved hook `sinks` / `bindings` remain
forward-compatible placeholders until their typed models land.

Generated **JSON Schema** in `docs/generated/config.schema.json` remains a future
slice. It will be generated from C++ types once the broader channel/team/hook/memory
config models are implemented.

This replaces the legacy "silently ignored unknown fields" behavior, which has been a
recurring source of subtle misconfiguration.

### Environment Substitution

`${VAR}` and `${VAR:-default}` substitutions are supported on string values. Done at
load time. Mismatches are errors unless `:-default` syntax provides a fallback.

## Secrets

### Threat Model

We are not protecting against a fully compromised host. We are protecting against:

- Accidentally committing API keys to git (ban via pre-commit hook).
- Accidentally including secrets in logs / hook payloads / web responses (redaction
  via `oran-log`).
- Sharing a config file with a colleague without revealing the secrets (encrypted at
  rest under a password).

### At-Rest Encryption

Secret fields are stored as `enc::<base64(ciphertext)>` strings in the JSON file. The
ciphertext is produced by `crypto_secretbox_easy` (libsodium):

- Algorithm: XSalsa20-Poly1305 AEAD.
- Key derivation: Argon2id from password (config-supplied via `--secret-password` or
  env `ORAN_SECRET_PASSWORD`), salt stored alongside the ciphertext.
- Nonce: 24 bytes random per field; stored alongside.

**Why libsodium instead of mbedtls.** mbedtls is also used for TLS; coupling the
secrets crypto to the TLS stack made the legacy build pay both costs even when only
one was needed. libsodium is a small static library (~200 KB), single-purpose, and
compiles in well under a second. TLS for `oran-http` is handled by the system curl /
OpenSSL.

### Fields Marked Secret

The schema marks fields with `"secret": true`. The config loader:

- Refuses to log them (the logger shim has a redaction filter).
- Returns them via a dedicated `decrypt(field)` accessor.
- Zeroes plaintext buffers on `Config::~Config`.

Legacy "secret-fields" / "secret-protection" continues conceptually; the library is
just smaller and simpler.

### Rotation

The legacy code had no rotation. v2 adds:

- `Config` supports a `secrets_version` integer. The CLI subcommand
  `orangutan secrets rotate --old-password X --new-password Y` re-encrypts every
  marked field under the new password and bumps the version.
- The rotation flow runs offline; the binary refuses to rotate while a runtime is
  active (uses an advisory file lock under `.orangutan/`).

### Approval Signing

Approval prompts (the `ask` permission flow) are HMAC-signed with a key derived from
the process-startup password. The signing key never persists. If the process restarts,
prior approvals become invalid (replay forbidden) — this is correct, not a bug.

## Logging

### Levels

`oran-log` thin shim over spdlog. Levels: `trace, debug, info, warn, error`. Default
console level is `info`; file level is `debug`.

### Redaction

Every log call passes through a redaction filter that:

- Replaces values of known secret fields with `***`.
- Applies config-defined regex patterns (`config.runtime.redaction_patterns`) — runtime
  re2, not compile-time. Examples:
  - `(?i)(api[_-]?key)[=:][^\s]+`
  - `Bearer [A-Za-z0-9_\-\.]+`

The legacy code redacted at the sink boundary, but only for tool output. v2 redacts
in the log shim itself, so all log paths are covered.

### Sinks

- Console sink (color, default).
- Rolling file sink (`<workspace>/.orangutan/logs/orangutan-YYYY-MM-DD.log`, daily,
  7-day retention).
- Optional syslog / journald sink (Linux); opt-in via config.

## Database Files

Four separate SQLite files (one per concern):

- `<workspace>/.orangutan/sessions.db`
- `<workspace>/.orangutan/memory.db`
- `<workspace>/.orangutan/automation.db` (opened and migrated by caller-owned
  `AutomationRuntime`; the caller-started loop step runs above it and stores
  due-run lease rows, and bootstrap does not open it yet)
- `<workspace>/.orangutan/audit.db`

### Why Separate Files?

- WAL contention is per-file. Splitting prevents memory-decay's long writer from
  blocking session appends.
- Smaller files copy / replicate / backup faster.
- Each DB has its own migration line so unrelated changes don't conflict.

### Connection Pool

`oran-storage::Pool` (per DB):

- 1 writer connection acquired through `Pool::acquire_writer`.
- N reader connections (default configured by `PoolOptions::reader_count`) drawn
  from a FIFO pool via `Pool::acquire_reader`.
- WAL mode; `synchronous=NORMAL`.
- Prepared-statement cache per connection.

### Expected-Only API

`oran-storage` currently ships the synchronous SQLite core and migration runner:

- `storage::Connection::open(ConnectionOptions)`
- `Connection::execute(std::string_view)`
- `Connection::prepare(std::string_view)`
- `Connection::query(std::string_view)`
- `storage::Statement` with bind / step / reset / column reader methods
- `storage::Migration`, `storage::MigrationReport`
- `storage::run_migrations(Connection&, std::span<const Migration>)`
- `storage::load_migrations_from_directory(std::string_view)`
- `storage::run_migrations_from_directory(Connection&, std::string_view)`
- `storage::Pool` with async writer/reader leases and per-slot statement caches
- `storage::StatementCache`
- `storage::SessionRepository` for the `sessions.db` schema and append/load/list
  operations over opaque message JSON. Its default `migrate()` path loads
  `src/oran-storage/migrations/sessions/0001-sessions-initial.sql` from the
  source tree, with an explicit directory override for future packaged layouts.
- `storage::AuditRepository` for the `audit.db` schema and append/list/count
  over `audit_events`. Each row carries scope/agent/tool/identity, the raw
  rule-engine `verdict` (`allow`/`deny`/`ask`), the rendered `outcome`
  (`allow`/`deny`/`ask`/`approved`/`rejected`), a free-form `reason`,
  an optional `input_hash_hex` (set on approval-flow callsites), and a
  `metadata_json` extension column. Indexes cover the scope-by-time hot path
  plus secondary `agent_key`/`outcome` query shapes. Its default `migrate()`
  loads `src/oran-storage/migrations/audit/0001-audit-initial.sql`.

All public APIs return `core::Result<T>` (i.e. `std::expected<T, core::Error>`). The
legacy throwing wrappers (`must_ok`) **do not exist** in v2. Migration debt is
zero-from-day-one. See `docs/rules/error-handling.md` and
[`storage-runtime.md`](storage-runtime.md).

### Migrations

```
src/oran-storage/migrations/
  sessions/
    0001-initial.sql
    0002-add-attachments.sql
  memory/
    0001-initial.sql
    0002-add-team-shared.sql
  ...
```

The runner accepts a compiled `std::span<const storage::Migration>` or a
source-tree migration directory through
`storage::run_migrations_from_directory`. File-backed migrations must use the
`0001-name.sql` convention, are sorted by version, and are validated for
contiguous versions before the database is touched. Installed/runtime migration
asset packaging is still a future bootstrap/release slice.

Each migration is applied in its own transaction. Applied migrations are recorded in:

```sql
CREATE TABLE IF NOT EXISTS schema_versions(
  version INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  applied_at TEXT NOT NULL
);
```

The migration runner verifies a complete contiguous migration set starting at version
`1`; gaps, duplicates, empty names, empty SQL, and database versions newer than the
binary's migration set abort startup. Re-running the same complete set is an idempotent
no-op.

### Backups

`scripts/backup-db.sh` (to be implemented) runs `sqlite3 <file> '.backup ...'` on each
DB. Recommended cron in `docs/CICD.md` once a runtime target exists.

## Identity And Scope

`oran-bootstrap::Identity`:

```cpp
struct Identity {
  std::string agent_key;        // config-defined: "default", "coder", "research"
  std::string runtime_key;      // UUID per process unless pinned by config
  std::string scope_key;        // = runtime_key OR pinned via config
  std::optional<std::string> team_id;
  std::optional<std::string> session_id;
};
```

`scope_key` namespaces:

- Long-term memory records.
- Audit log entries.
- Approval signatures.

Pinning `scope_key` (config: `agent.<name>.scope_pin = true`) causes long-term memory
to persist across runtimes — useful for stable expertise.

## Workspace Layout

```
<workspace>/.orangutan/
├── config.json
├── sessions.db
├── memory.db
├── automation.db
├── audit.db
├── logs/
│   └── orangutan-YYYY-MM-DD.log
├── hooks/                      shell hook scripts
├── skills/                     markdown skills
├── memory/
│   └── MEMORY.md               optional mirror
├── debriefs/                   self-reflective task debriefs (opt-in)
└── lock                        advisory file lock (single live runtime)
```

The presence of `.orangutan/lock` (with PID + start time) tells operators a runtime is
active. Stale locks (PID not running) are cleaned on next startup.

## Anti-Patterns

- Putting secrets in env vars that we then echo elsewhere. Use `${VAR}` substitution
  and let the config layer mark them secret.
- Reading secrets in subprocesses started by tools. The shell-exec tool gets a
  scrubbed environment (no `ORAN_SECRET_PASSWORD` by default).
- Storing derived data in long-term memory. If it's recomputable, leave it out.
- Sharing the same DB file across runtimes without `scope_pin`. The scopes will
  interleave and you'll be confused.

## See Also

- [`memory-system.md`](memory-system.md) — what lives in `memory.db`.
- [`permissions-and-hooks.md`](permissions-and-hooks.md) — what lives in `audit.db`.
- [`../rules/error-handling.md`](../rules/error-handling.md) — `Result<T>` everywhere.
- [`../rules/libraries.md`](../rules/libraries.md) — libsodium / re2 rationales.
