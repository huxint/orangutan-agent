# Configuration Composition Boundary

## Objective

Keep configuration parsing outside the permission, provider and agent runtime
libraries. Bootstrap translates configuration into owned runtime values; the
runtime libraries expose their own contracts without importing configuration.
Remove the unused config-to-storage edge and obsolete adapter surfaces.

## Current Contracts

- [Architecture](../../ARCHITECTURE.md) owns the library graph.
- [Configuration](../../design-docs/secrets-and-state.md) owns parsed values,
  profile references and host settings.
- [Permissions](../../design-docs/permissions-and-hooks.md) owns rule precedence,
  approval bounds and fail-closed behavior.
- [Provider](../../design-docs/api-portability.md) owns endpoint validation,
  credentials, selected-target policy and transport.
- [Bootstrap](../../design-docs/bootstrap-runtime.md) owns configuration mapping
  and session construction.

Permission materialization currently exports the full config header from
`oran-permission`. Provider route resolution and the credential callback import
config into `oran-provider`. Both then pull configuration into agent execution.
Config declares a storage dependency without any storage caller.

## Complete Slice

1. Move permission materialization to bootstrap. Accept only spans of configured
   rules, keeping workspace configuration out of rule compilation. Preserve
   baseline/global/agent ordering, regex failures and approval bounds; remove the
   redundant overload and the session's temporary permissions copy.
2. Move config profile/alias resolution to bootstrap. Keep endpoint construction
   values and the credential lookup contract in provider. Remove the old route
   resolver and config secrets headers rather than leaving forwarding wrappers.
3. Remove config dependencies from permission/provider and storage from config.
   Enforce the config boundary in the dependency gate: config uses core values,
   and bootstrap is its runtime consumer.
4. Move config-adapter tests and the materialization benchmark into bootstrap;
   keep permission evaluation and provider transport tests in their own buckets.
5. Update owning contracts, migration notes and handoff; delete this completed
   plan after verification.

## Bounds And Risks

No configuration syntax, protocol payload, permission decision, database schema
or async ownership changes. No new libraries or dependencies. Invalid fallback
profiles must still fail before credential lookup; lookup failures must not expose
secret values. Existing configurations, stored sessions and scoped memory remain
readable. Public adapter moves require concrete migration paths. Compile budgets
are unchanged; removing public/config dependencies must not introduce heavy
headers into runtime APIs.

## Verification

- Existing permission-adapter cases retain deny precedence, capability scopes,
  regex validation and approval budgets after the move.
- Existing profile cases retain aliases, explicit protocols, route ordering,
  model/cache/pricing policy and contextual failures.
- Provider construction still validates the whole route before credentials;
  session/child integration continues to enforce permissions.
- Build and test config, permission, provider and bootstrap, then run the full
  release build/test suite. Compile affected config, permission, provider, agent
  and bootstrap benchmark targets. Run `make ci` before commits.
- Inspect source includes and the built dependency graph for removed edges.

## Progress

- [x] Read handoff/contracts and identify adapter ownership and stale dependencies.
- [ ] Implement adapters and migrate their callers/coverage.
- [ ] Update contracts and pass release verification.
- [ ] Delete the completed plan and commit the slice.
