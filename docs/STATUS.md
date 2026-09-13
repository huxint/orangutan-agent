# Current State

Orangutan runs a C++26 provider/tool/session loop through library composition.
The implemented core includes scoped memory, persisted continuation, explicit
authorization and bounded child collaboration.
[Architecture](ARCHITECTURE.md) owns the library graph.

## Runtime Boundary

Bootstrap converts explicit configuration into owned permission rules and provider
profiles, then assembles workspace, audit, hooks and optional memory services.
Config is consumed at composition; prompt rendering depends only on core values.

Each turn owns one selected native tool catalogue and one joined system prefix.
Conversation remains typed. Cache identity follows submitted text and native
declarations, without manually maintained section versions or old-key guarantees.

Provider sends target one configured endpoint. Execution owns retry/fallback
selection, terminal attribution and cost estimation. The loop consumes those
outcomes for hooks and traces and accumulates usage. Backend error strings do not
choose the attributed profile.

FileRead, FileWrite and FileEdit use prepared, validated calls and pinned
filesystem authority. Memory tools and AgentRun use the same dispatch gates.
Unknown tool selections fail before provider work; visibility grants no authority.
Path locks follow live holders and waiters, and cancellation joins borrowed work.

Sessions load bounded history and a scoped memory index. Exact reads, lexical
search and same-turn note corrections run through memory tools. Successful
transcript suffixes serialize before acquiring the writer and commit atomically.
Browsing does not update read timestamps; corrections preserve record history.

Configured children receive fresh session/approval identities, selected prompt
and tool context, and the intersection of parent/child permissions. They share
workspace, memory scope, provider route, scheduler and strand. Admission defaults
to four children per prompt and one generation; parent cancellation joins them
without waiting for unrelated sessions.

Filesystem, HTTP and SQLite work runs on explicit executors. Durable audit
decisions precede tool effects; terminal traces and cancellation cleanup finish
before services are released. Existing user records remain intact.

## Contract Owners

- [Composition](design-docs/bootstrap-runtime.md): resources and host bindings.
- [Agent](design-docs/agent-platform.md): turns, context and child execution.
- [Provider](design-docs/api-portability.md): attempts, outcomes, streaming and cache policy.
- [Prompt](rules/prompt-design.md): stable text and current fingerprint rules.
- [Tools](design-docs/tool-runtime.md) and [permissions/hooks](design-docs/permissions-and-hooks.md): admission and effects.
- [Memory](design-docs/memory-system.md), [storage](design-docs/storage-runtime.md) and [IO](design-docs/io-runtime.md): durable data and resource ownership.

Development permits breaking APIs, configuration and derived caches. Replace
obsolete surfaces and their tests/docs directly, as defined by
[collaboration](REPO_COLLAB_GUIDE.md); do not maintain parallel historical versions.

## Verification And Next Work

The release gate covers all 14 test targets and `make ci`. Controlled HTTP
integration composes transport, assembly and session continuation. Regressions
cover denied effects, atomic persistence, scoped recall, fallback attribution,
cost, stable prompt snapshots and joined cancellation. These checks establish
runtime behavior, not spontaneous memory use or service-side cache hits.

[Live debt](exec-plans/tech-debt-tracker.md) tracks session working memory,
backup/import tooling, shell execution, real-model memory evaluation, hosted
quality jobs and reference-hardware compile budgets. Persisted working context
needs backup/import boundaries first. Real-model evaluation uses explicitly
supplied credentials.

Run the normal release gate from the repository root:

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
make ci
```

[Build system](BUILD_SYSTEM.md) owns supported options and toolchains;
[testing](rules/testing-and-bench.md) owns affected targets and sanitizer checks.
