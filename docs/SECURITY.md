# Security

Use this document to keep secure defaults legible to agents and operators.

## Authentication And Authorization

- The desktop app is local and in-process: no network listener, so no HTTP auth token
  or bound port to secure.
- Channel adapters carry their own auth model — documented per adapter under
  `docs/design-docs/channel-<name>.md` when noteworthy.
- Multi-user runtimes are a v2 concern; v1 assumes a single trust principal.

## Secret Handling

Current slice:

- `oran-config` loads provider profile metadata such as `api_key_env`; built-in
  no-route startup treats it as absent metadata and reads no secret values.
- Configured-route `bootstrap::run` now calls `HttpProviderBackend::build`, which
  invokes `resolve_adapter_credentials(plan)` and
  `make_adapter_system(credentials, factories)` after route/profile/endpoint
  preflight. The credential resolver reads API-key environment variables only at
  that explicit boundary, the factory passes key values only through in-memory
  targets to caller-registered protocol factories, and both surfaces report
  failures with non-secret identifiers rather than secret values.
- `${VAR}` substitution exists for string config values. Missing variables are errors
  unless `${VAR:-default}` provides a fallback.
- Secret values must not be placed in `config.example.json`.

Planned secret slice:

- Secrets are encrypted at rest via `oran-config` (libsodium `crypto_secretbox`).
  Argon2id KDF; per-field random nonces.
- Plaintext secrets live in memory only behind a `SecretField` accessor; zeroized
  on `Config::~Config`.
- Secrets are never logged. The log shim's redaction filter applies known secret
  field names and runtime regex patterns from config.
- `${VAR}` substitution preserves the "marked secret" flag through the substitution.
- Rotation: `orangutan secrets rotate` re-encrypts every marked field under a new
  password.

## Permissions

- Every effectful action passes through `oran-permission`. Bypassing is a critical
  rule violation (see [`rules/critical-rules.md#C10`](rules/critical-rules.md)).
- Default modes documented in [`design-docs/permissions-and-hooks.md`](design-docs/permissions-and-hooks.md).
- Approval prompts are HMAC-signed; the signing key is process-local and discarded
  on shutdown.

## Hook-Driven Veto

- Blocking `tool_before` hooks now run inside `tool::Registry::dispatch` before
  workspace resolution and permission evaluation. A veto or hook failure records
  `AuditOutcome::blocked_by_hook`, skips the handler, emits advisory failure
  events, and returns `Error::permission_denied`.
- Blocking hook sinks are bounded by `hooks.timeout_ms` (default 2000). A timed-out
  sink is treated as a hook veto with `reason=hook_timeout`; direct tool dispatch
  records the blocking sink id plus `elapsed_ms` in audit metadata and does not run
  the handler.
- Rewrite decisions replace the effective input before permission, approval,
  audit, handler execution, and later hook payloads. Audit metadata keeps the
  original and rewritten input hashes plus the consulted sink decisions, so the
  security boundary stays reviewable without storing raw redacted input twice.

## Sandbox Posture

- `shell.exec` runs subprocesses with the runtime's UID. We do not run as root and
  refuse to start if running as root unless `--allow-root` is set.
- Workspace-scoped file operations: slice 37 introduces `tool::Workspace`;
  slices 37-40 make every filesystem built-in (`file.read`, `file.write`,
  `file.edit`, `file.delete`, `file.search`, `directory.list`) use it when
  the runtime supplies `DispatchContext::workspace`. Slice 41 moves
  workspace ownership into `bootstrap::RuntimeAssembly` and routes
  `permissions.workspace.extra_{read,write}_roots` from `oran-config`
  into `tool::WorkspaceOptions`, so overrides canonicalise once at boot. Slice
  55 moves known filesystem built-ins to registry-boundary pre-resolution:
  path policy runs before permission evaluation, resolver failures are audited
  under `permission::AuditEvent::metadata_json`, and handlers do not run on
  path-policy failures. The remaining workspace work is v1.1 structure
  (`Workspace::is_ignored` / display helper) and the future capability-gated
  `tool::Runtime::workspace()` accessor.
- Hardening flags compiled in by default:
  - `_FORTIFY_SOURCE=3`
  - `-fstack-protector-strong`
  - `-fcf-protection`
  - `-fstack-clash-protection`
- ASLR / NX rely on system defaults; CI verifies a release binary has
  `RELRO=full,Now,BindNow`.

## Data Handling

- Session histories may contain PII; the workspace is the responsibility of the
  operator. Document data classification in your deployment's runbook.
- Logs go to `<workspace>/.orangutan/logs/`; rolling daily, 7-day retention by
  default. Operator can extend.
- `audit.db` is append-only (no `DELETE`); retention is a separate operator concern.

## Webhooks And External APIs

- Webhook inbound URLs are HMAC-signed if the caller provides a secret in config.
- Outbound HTTP requests use OpenSSL TLS (via libcurl). Cert pinning is configurable
  via `config.http.pinned_certs`.
- We do not currently support outbound proxy configuration; planned for v1.1.

## Supply Chain

See [`SUPPLY_CHAIN_SECURITY.md`](SUPPLY_CHAIN_SECURITY.md).

## Reporting Vulnerabilities

See repository-root [`../SECURITY.md`](../SECURITY.md).
