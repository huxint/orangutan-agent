# Tool Context Reduction

## Objective

Give each turn one explicit, deterministic native tool selection. Remove the
discovery/promotion pipeline and duplicate prompt schemas. Make prompt rendering
a synchronous value transformation with no configuration, registry or executor
dependency.

## Current Contracts And Callers

- `AgentSession` is the composition boundary for configured active tool names,
  host-bound memory services and child restrictions. Its injected registry and
  scheduler also serve custom-tool hosts in the integration tests.
- `Loop` and `Builder` currently select tools independently. ToolSearch output
  is reparsed after persistence to update SessionState and PromotionState; only
  MemoryForget remains deferred among built-ins.
- Repository callers of discovery/promotion are the built-in session wiring,
  tests and benchmarks. No independent application host consumes that state.
- Both supported protocols consume `provider::Request::tools`. Their declarations
  already carry tool descriptions and input schemas.
- Explicit tool selection controls model exposure. Authorization remains in
  registry dispatch, including calls naming tools outside the advertised set.

## Complete Slice

1. Replace catalogue rendering with a pure selection function over ToolDef
   values and an optional list of names. Absence selects all registered tools;
   an empty list selects none; unknown names fail before provider execution.
   Sort and deduplicate the selected names deterministically.
2. Select once at the turn boundary. The session maps configuration into that
   value interface and removes disabled AgentRun from its catalogue and list.
   Custom registered tools and all three bound memory tools are directly visible
   by default.
3. Remove ToolSearch, SessionState, PromotionState, their transcript observer,
   registry self-binding and unused ToolDef presentation metadata. Delete their
   obsolete tests and benchmarks while retaining dispatch/permission coverage.
4. Replace the stateful asynchronous Builder with pure prompt rendering. Send
   tool schemas only in native declarations. Hash native name/description/schema
   bytes separately and include them in the effective prefix identity and byte
   estimate, so changed tools invalidate cache hints without entering text.
5. Remove prompt dependencies on async/config/tool and agent's now-unused JSON
   package. Update callers, dependency checks and owning contracts.

## Compatibility And Bounds

The source APIs for promotion and textual tool catalogues are intentionally
removed. `runtime.prompt.active_tools` retains its configuration shape; defaults
now mean every registered tool, while explicit names remain a strict selection.
Hosts listing ToolSearch must remove that name. No database schema, persisted
message encoding, permission rule or stored user data is changed. Existing trace
columns remain readable; new rows use the native catalogue fingerprint and zero
for the retired deferred catalogue hash.

Tool results cannot alter subsequent declarations. Child policy intersection,
approval, audit and cancellation joins retain their existing dispatch boundary.
Working-memory persistence, build-option repair and script-stub cleanup are
separate handoff slices.

## Verification And Completion

- Selection tests cover custom names, explicit subsets/none, unknown names and
  deterministic order. Prompt tests cover pure rendering and tool-aware cache
  invalidation without duplicated schema text.
- Loop and session tests cover direct tool use in the same turn, unchanged
  declarations across iterations, scoped memory, child restrictions and denied
  effects. Supported protocol requests contain each selected schema once.
- Run affected release targets, the complete release suite and `make ci` using
  ordinary repository build/test targets. Update affected benchmark callers and
  compile those targets; no separate validation workspace or report bundle.
- Update architecture, prompt/tool/provider/session contracts and handoff. Move
  remaining obligations to live debt, then delete this plan after completion.

## Progress

- [x] Read owning contracts and inspect repository hosts and custom selection.
- [x] Scope the complete native-tool and prompt dependency reduction.
- [ ] Implement and migrate runtime, tests and benchmarks.
- [ ] Verify behavior and update durable contracts.
