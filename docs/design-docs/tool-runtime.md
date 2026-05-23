# Tool Runtime

The tool registry is the agent's hand. It is the place where:

- The agent decides what it *can* do (the catalog presented to the LLM).
- The runtime checks whether it *may* do something (permissions).
- The runtime observes when it does (hooks).
- The runtime knows when it's *deferred* (tool-search style discovery).

This doc covers the v2 design. The legacy `ToolRuntimeContext` sprawl is explicitly
replaced.

## Tool Definition

```cpp
// include/oran/core/tool.hpp — PUBLIC
namespace orangutan::core {

struct ToolDef {
  std::string                 name;        // dotted: "file.read", "shell.exec"
  std::string                 description; // for the LLM
  nlohmann::json_fwd          input_schema; // JSON Schema for tool args
  std::vector<Capability>     required_capabilities; // spelled `required_capabilities` in code — `requires` is a reserved C++20 keyword
  bool                        deferred = false; // surfaces via ToolSearch only
  std::optional<std::string>  category;     // for grouping in UIs
};

}  // namespace orangutan::core
```

`Capability` is the v2 mechanism that ties tools to permissions. Examples:

```cpp
enum class Capability {
  // file system
  read_file, write_file, edit_file, delete_path,
  // network
  egress_http, egress_websocket,
  // process
  spawn_subprocess, signal_subprocess,
  // memory
  read_memory, write_memory,
  // orchestration
  spawn_agent, send_message_intra_team, send_message_inter_team,
  // automation
  schedule_job, modify_job, run_job_now,
  // skills
  invoke_skill,
  // misc
  external_mcp, runtime_loader,
};
```

> **Status (2026-05-17):** the enum itself now lives in `oran-core`
> (`include/oran/core/capability.hpp`); its wire spelling and inverse
> parse come from the generic reflection helpers
> `core::enum_name(c)` / `core::parse_enum<Capability>(text)` /
> `core::enum_values<Capability>()` in
> [`include/oran/core/enum_names.hpp`](../../include/oran/core/enum_names.hpp)
> (no per-enum forwarding shims — see
> [`docs/rules/code-style.md`](../rules/code-style.md) "Enums").
> `core::Capability` is the vocabulary the `permission::Rule::capability`
> field already reads and the slice-17 `oran-tool` registry consumes
> via `core::ToolDef::required_capabilities` — that field is the
> in-code realisation of the `requires` field below (renamed because
> `requires` is a reserved C++20 keyword). Built-ins shipped so far:
> `file.read` (slice 17, `Capability::read_file`), `file.write`
> (slice 18, `Capability::write_file`, input
> `{path, content, mode?, create_parents?, max_bytes?,
> expected_version?}` with
> `mode ∈ {truncate, append, fail_if_exists}` and `max_bytes`
> capped at 16 MiB; slice 61 fills `Output::usage.bytes_written`
> and `files_touched` on success), `file.edit` (slice 19,
> `Capability::edit_file`, input
> `{path, old_string, new_string, replace_all?, max_bytes?,
> expected_version?}` —
> `not_found` if `old_string` is absent, `conflict` (`match_count`
> carried) if it is ambiguous and `replace_all` was not set; the
> read and final replacement output are capped by `max_bytes`
> (default / hard ceiling 16 MiB); truncating rewrite via
> `io::write_text_file`; slice 61 fills `Output::usage.bytes_read`,
> `bytes_written`, `files_touched`, and `match_count` on success),
> and `file.search` (slice 20,
> `Capability::read_file`, input
> `{path, pattern, max_matches?, include_hidden?, regex?,
> max_output_bytes?, respect_ignore?}` — literal substring match by
> default; `regex=true` (slice 24) routes through
> `permission::InputPattern`; slice 51 keeps compiled patterns in a
> bounded process-local `core::BoundedCache` (64 entries / 64 KiB /
> 10-minute TTL) keyed by pattern plus line-match mode; recursive walks
> skip NUL-bearing binary
> files, dot-prefixed entries when `include_hidden=false`, slice-48's
> built-in low-signal directories, and slice-49's `.gitignore` /
> `.ignore` common rule subset when `respect_ignore=true`; ripgrep-class
> mmap / extension-binary-skip / parallel-walk optimisations are deferred
> to follow-up slices tracked in
> [`exec-plans/tech-debt-tracker.md`](../exec-plans/tech-debt-tracker.md)).
> Slice 21 (2026-05-17) wired `permission::ApprovalBroker` through
> `Registry::dispatch` so a `Verdict::ask` decision can be mediated
> rather than short-circuited: `DispatchContext` grew optional
> `approval_broker` + `approval_token` + `now` fields; when both
> are supplied, `broker.check(token, name, input, identity, now)`
> drives the audit outcome (`approved` / `rejected`) and either
> runs the handler or forwards the broker's `reason` context entry
> (`expired` / `tool_mismatch` / `identity_mismatch` /
> `input_mismatch` / `mac_mismatch` / `no_grant` /
> `replay_exhausted`). When the agent supplies neither, the slice-17
> short-circuit is preserved but the `permission_denied`
> error now also carries `decision_reason` / `replay_max` /
> `approval_ttl_seconds` so the agent loop can hand them straight
> to `ApprovalBroker::approve` without re-running rule evaluation.
> Slice 22 (2026-05-18) added the hook-bus tap: `DispatchContext`
> now also carries an optional `hook::Bus*`; when non-null, dispatch
> publishes `hook::Event::tool_before` after the registry resolves
> the tool def (so a sink can see every known call attempt
> regardless of how the call is subsequently gated) and
> `hook::Event::tool_after` at every exit (handler success,
> permission deny, broker rejection, audit error) with a
> `ToolAfterPayload { succeeded, output_text, error_kind,
> error_message, started_at, finished_at, duration }` that
> flattens the dispatch outcome for forensic queries. Both events
> are advisory in slice 22 — sinks observe but cannot veto the
> dispatch; blocking semantics for `tool_before` rewrite/short-
> circuit are deferred to a follow-up slice. Unknown tool names
> are silently rejected without a hook publish (the dispatch never
> started). Capability-gated runtime services (`tool::Runtime`
> accessor surface) and config wiring stay on future slices.
> Slice 29 (2026-05-20) extends the built-in catalog with
> `directory.list` (`tool::register_directory_list`, capability
> `list_directory` — a new `core::Capability` enumerator so a
> permission rule can distinguish "list a directory" from "read a
> file"; useful in sandbox modes where the agent should see the
> shape of a tree without reading content). Input
> `{path, include_hidden?, max_entries?}`; output is one
> `<path>:<kind>:<size_bytes or '-'>` line per entry sorted by
> path (the order `oran-io::list_directory` already enforces),
> the literal text `no entries` for an empty directory after the
> hidden filter, and the `io: directory entry limit exceeded`
> error propagated verbatim when the directory exceeds
> `max_entries` (raise the cap and retry — the tool does not
> truncate on overflow because that would require either calling
> `oran-io` twice or extending the helper's contract for every
> other caller).
> Slice 30 (2026-05-20) adds `file.delete`
> (`tool::register_file_delete`, capability `delete_path` — the
> first built-in that exercises this slice-7 capability), built on
> a new `oran-io::delete_file` coroutine helper. Input
> `{path}`; the io helper refuses every path that is not a
> regular file with `invalid_argument` (covers both directories
> AND symlinks even when the symlink points at a regular file —
> the v1 surface is deliberately narrow so a recursive delete or
> a symlink-follow cannot escape the workspace), returns
> `not_found` when no entry exists at `path` (and on the rare
> "vanished between stat and unlink" race so the caller sees a
> single end-state error kind), and on success returns the
> literal text `deleted <path>`. The future direction for
> filesystem mutation built-ins is *consolidation*, not more
> per-kind splits: a single delete tool covering files AND
> folders, and a recursive whole-project list (not a separate
> `directory.remove` / single-level enumeration). The v1
> narrowings here are the entry point, not the dead end.
> Slice 32 (2026-05-21) routes `file.edit` and the dominant
> `file.write` mode (`truncate`) through the new
> `oran-io::WriteTextOptions::atomic` opt-in: the rewrite is
> staged to a sibling `.<name>.orangutan.tmp.<seq>` and committed
> via `std::filesystem::rename`, so a crash or partial write
> leaves the original target intact instead of truncated. The
> deep-review BUG-4.1.1 footgun (`file.edit` composing
> `read_text_file` + `write_text_file` with no rollback) is
> closed; `file.write` keeps its current semantics for `append`
> and `fail_if_exists` because the atomic helper is incompatible
> with both and rejects them with `invalid_argument`. See
> `docs/design-docs/io-runtime.md` "Atomic Writes" for the
> contract.
> Slice 33 (2026-05-21) closes the deep-review SMELL-4.1.3
> cancellation footgun: `file.search`'s `walk_and_scan` and
> `read_text_capped` now poll `asio::cancellation_state` once per
> directory entry and once per 8 KiB read chunk. The handler
> previously checked cancellation only before and after the
> executor hop, so a long-running walk or a multi-MiB file read
> was uncancellable in flight; the polling closes that gap with
> two relaxed atomic loads per iteration (single-digit
> nanoseconds against per-entry stat / per-chunk read cost). A
> regression test arms the chunk-read polling on an 8 MiB file
> read driven from a worker `std::jthread`.
> Slice 34 (2026-05-22) closes the deep-review content-size cap
> P0 item for `file.write` / `file.edit`: both tools now expose an
> optional `max_bytes` positive integer whose default and hard
> ceiling are 16 MiB. `file.write` refuses `content` over the cap
> before touching the path; `file.edit` passes the cap into
> `io::read_text_file` and preflights the final replacement size
> before allocating / writing the replacement. The same slice also
> fixes the `tests/tool/test_registry.cpp`
> `-Wmissing-field-initializers` warnings from the second deep
> review follow-up row by using full default construction for
> captured hook rows and explicit empty `required_capabilities` on
> test-only `ToolDef`s.
> Slice 35 (2026-05-22) closes the deep-review
> `Registry::add` schema-validation P0 item: registration now
> rejects empty `input_schema_json`, unparseable JSON, non-object
> top-level schemas, and malformed common JSON Schema keywords
> (`type`, `properties`, `required`, `additionalProperties`, `enum`,
> `minimum`, `maximum`) with `Error::invalid_argument` carrying
> `tool` and `schema_path` context before mutating `entries_`. This
> is a lightweight declaration sanity check, not a full JSON Schema
> validator; it catches broken tool definitions early while keeping
> heavy `nlohmann/json.hpp` isolated in
> `src/oran-tool/schema_validation.cpp` instead of the dispatch TU.
> Slice 36 (2026-05-22) closes the remaining deep-review P0 on the
> registry hot path: `Registry::entries_` now uses transparent string
> hashing, and `remove`, `find`, and `dispatch` call `entries_.find`
> with the incoming `std::string_view` directly instead of allocating
> a temporary `std::string` key for each lookup.
> Slice 37 (2026-05-22) starts spec 0013's workspace-path policy:
> `<oran/tool/workspace.hpp>` exposes `tool::Workspace`,
> `WorkspaceOptions`, `WriteIntent`, and `ResolvedPath` without pulling
> `<filesystem>` into the public header; `Workspace::create`
> canonicalises the primary root plus extra read/write roots, and the
> four intent-specific entry points reject traversal with
> `reason=outside_workspace`, read/list symlink escapes with
> `reason=symlink_escape`, and mutating symlink paths with
> `reason=symlink_target`. `DispatchContext` now carries an optional
> non-owning `Workspace*`; `file.read` resolves its input through
> `resolve_read` when that pointer is supplied. The remaining filesystem
> built-ins, bootstrap config ownership, audit metadata, and pre-permission
> resolution moved in later workspace slices.
> Slice 38 (2026-05-22) migrates the mutating built-ins through the same
> interim seam: `file.write` calls `resolve_write` with the current write
> mode and parent-creation intent before the `oran-io` write, `file.edit`
> resolves through `resolve_write` before its read+atomic rewrite, and
> `file.delete` resolves through `resolve_delete` before unlinking. When
> a workspace is supplied, relative in-workspace mutations work while
> traversal rejects with `reason=outside_workspace` and mutating symlink
> targets reject with `reason=symlink_target`. `file.search`,
> `directory.list`, bootstrap ownership, audit metadata, and the
> pre-permission resolver boundary moved in later workspace slices.
> Slice 39 (2026-05-22) migrates `file.search` through the same interim
> seam: the handler resolves its input through `tool::Workspace::resolve_list`
> when `DispatchContext::workspace` is supplied, before the executor hop
> that drives the blocking walk. The root-path symlink behaviour now matches
> the workspace policy that already governs `file.read` (follow inside-
> workspace, reject `symlink_escape`); nested entries continue to skip
> symlinks wholesale during the walk, a stricter form of the same rule.
> `directory.list`, bootstrap ownership, audit metadata, and the
> pre-permission resolver boundary moved in later workspace slices.
> Slice 40 (2026-05-22) migrates `directory.list` through the same interim
> seam via `Workspace::resolve_list`, closing the per-tool half of the
> workspace migration: every filesystem built-in (`file.read`, `file.write`,
> `file.edit`, `file.delete`, `file.search`, `directory.list`) now resolves
> at the handler entry. Bootstrap/config ownership for the override roots,
> audit metadata, and moving resolution to the pre-permission dispatch
> boundary moved in later workspace slices.
> Slice 41 (2026-05-22) lands the bootstrap-owned half: `bootstrap::RuntimeAssembly`
> now constructs and owns a `tool::Workspace`, and `oran-config` recognises
> `permissions.workspace.extra_{read,write}_roots` so the override list is
> canonicalised once at boot. Bootstrap converts the typed
> `config::WorkspacePermissionsConfig` into `tool::WorkspaceOptions` before
> calling `RuntimeAssembly::build`; the resulting `Workspace&` lives on the
> assembly for callers to thread into `DispatchContext::workspace`.
> Audit metadata and pre-permission resolution moved in slice 55.
> Slice 55 (2026-05-24) closes that follow-up at the registry boundary:
> `Registry::dispatch` pre-resolves known filesystem built-in `path` inputs
> through the intent-specific `Workspace` method after `tool_before` and
> before permission evaluation. Successful resolution stores
> `DispatchContext::resolved_path` for handlers and writes redacted
> `metadata_json.path_resolution` audit fields; resolver failures are audited
> with the permission verdict but return before handlers run or ask-approval
> replay is spent. Handlers keep their in-handler fallback for callers that
> dispatch without a workspace.

A tool's `required_capabilities` list is **inspected at registration**. The permission
engine knows the universe of capabilities a tool might use; the tool cannot smuggle in
a capability it didn't declare (enforced in `ToolRuntime`'s tool dispatch — see below).
Its `input_schema_json` is also sanity-checked at registration so broken schemas fail
before the catalog can be advertised to a provider.

## ToolRuntime — Per-Invocation Context

The legacy `ToolRuntimeContext` was a parameter object with 8+ pointer fields. v2
replaces it with a small typed handle that exposes capability-gated services:

```cpp
// include/oran/tool/runtime.hpp
namespace orangutan::tool {

class Runtime {
 public:
  // The dispatching code constructs this; tool code consumes it.
  Runtime(Services&, Capabilities granted, Identity);

  // Capability-gated accessors return Result<T>: forbidden -> error.
  core::Result<io::Workspace&>             workspace();
  core::Result<memory::Runtime&>           memory();
  core::Result<orchestration::Mailbox&>    mailbox();
  core::Result<automation::Runtime&>       automation();
  core::Result<skill::Loader&>             skills();
  core::Result<provider::System&>          provider();
  core::Result<http::Client&>              http();

  // Identity passthrough for logging/audit.
  const Identity& identity() const noexcept;

  // The granted capability set (post-permission).
  Capabilities granted() const noexcept;
};

}  // namespace orangutan::tool
```

If a tool calls `runtime.workspace()` but its `required_capabilities` did not include
`Capability::read_file`, the call returns an `Error::capability_not_granted`. This is
defensive against tools that *try* to escalate after registration.

`Services` is owned by `oran-bootstrap`; the `Runtime` holds a reference. No globals.

## Tool Handler Shape

```cpp
namespace orangutan::tool {

struct ToolUsage {
  std::optional<std::uintmax_t> bytes_read;
  std::optional<std::uintmax_t> bytes_written;
  std::optional<std::uint32_t> files_touched;
  std::optional<std::uint64_t> match_count;
  std::optional<double> cost_estimate;
  std::optional<std::chrono::nanoseconds> wall_time;
  bool truncated = false;
  bool data_dropped = false;
};

struct Attachment {
  std::string file_path;
  std::string mime_type;
  std::optional<std::uintmax_t> byte_size;
  std::optional<std::string> fingerprint;
};

struct Output {
  std::string text;                          // required model-facing fallback
  std::optional<std::string> data_json;      // serialized structured JSON bytes
  std::vector<Attachment> attachments;       // files, images, blobs
  ToolUsage usage;                           // measured counters and cap flags
  bool is_error = false;

  static Output text_only(std::string text);
  static Output error(std::string message,
                      std::optional<std::string> data_json = std::nullopt);
};

using Handler =
    std::function<async::Awaitable<core::Result<Output>>(
        std::string_view input_json, DispatchContext&)>;

}  // namespace orangutan::tool
```

Tools are coroutines. Today's registry passes raw JSON bytes so the public
handler type stays free of `nlohmann::json`; handlers parse in their own
implementation TUs. The later `tool::Runtime` handle will replace the
interim `DispatchContext` service bundle once the agent runtime exists.
Tools run on the agent's strand by default; CPU-heavy tools hop through the
runtime's CPU executor.

## Registry

```cpp
namespace orangutan::tool {

class Registry {
 public:
  // Registration.
  core::Result<void> add(core::ToolDef def, Handler handler);
  core::Result<void> remove(std::string_view name);

  // Discovery.
  std::vector<core::ToolDef> catalog() const;
  std::vector<core::ToolDef> deferred_catalog() const;
  std::optional<core::ToolDef> find(std::string_view name) const;

  // Dispatch (called by the agent loop).
  async::Awaitable<core::Result<Output>>
  dispatch(std::string_view name,
           const nlohmann::json& input,
           permission::Evaluator& perms,
           hook::Bus& hooks,
           Identity identity) const;
};

}  // namespace orangutan::tool
```

Note that `dispatch` *takes* the permission evaluator and hook bus by reference. The
registry does **not** own them; bootstrap does.

## Built-in Tool Categories

Each category is a small library that calls `Registry::add` from a single
`register.cpp`:

| Library                       | Tools                                                   |
| ----------------------------- | ------------------------------------------------------- |
| `oran-tool-file`              | `file.read`, `file.write`, `file.edit`, `file.search`   |
| `oran-tool-shell`             | `shell.exec`, `shell.glob`, `shell.ls`, `shell.move`    |
| `oran-tool-memory`            | `memory.recall`, `memory.remember`, `memory.forget`     |
| `oran-tool-orchestration`     | `agent.spawn`, `agent.stop`, `agent.send_message`, …    |
| `oran-tool-automation`        | `automation.schedule`, `automation.list`, …             |
| `oran-tool-mcp`               | external MCP client (one tool per external MCP server)  |
| `oran-tool-skill`             | `skill.invoke`                                          |
| `oran-tool-background`        | async job orchestration                                  |
| `oran-tool-attachments`       | message attachment management                           |
| `oran-tool-runtime-loader`    | dynamic tool reloading                                  |
| `oran-tool-search`            | deferred tool discovery / metadata lookup               |
| `oran-tool-script`            | scriptable batch tool                                   |

Each tool library is **independent** — adding a new category does not recompile the
others.

## File vs. Shell — De-duplicated

The legacy code's 1.5 kLoC of file tools + 0.7 kLoC of shell tools had overlapping
glob / ls / mkdir / move logic. v2 split:

- **`oran-tool-shell`** owns process execution, glob/ls/mkdir/mv as the *raw* surface
  with subprocess semantics and tty-style output.
- **`oran-tool-file`** owns **structured** operations: `file.read` returns content +
  line numbers + checksum; `file.edit` performs patch-style edits with conflict
  detection; `file.search` returns ripgrep-style structured matches.

Operations that exist in both libraries (`glob`, `ls`, `mkdir`, `move`) are **only**
in `oran-tool-shell`. `oran-tool-file::file.search` may call into shell glob
internally, but the public surface is distinct.

## Permission Ordering

The dispatch order is fixed:

```
1. Hook bus → tool.before     (advisory; may short-circuit on error)
2. Permission engine evaluates rules against {tool name, input, identity, capability set}
3. If "ask" → render approval prompt, wait, replay-sign
4. If "allow" → continue; if "deny" → return PermissionDenied error
5. Hook bus → tool.dispatched (after permission, before handler)
6. Handler runs
7. Hook bus → tool.after       (always, even on error)
```

The legacy code's per-tool boilerplate ("check permission inside the tool handler") is
gone; the registry does it once. Tools that need *additional* fine-grained checks (e.g.
network egress allowlist) call `permission::Evaluator::check_capability(...)` inside
their handler.

## Deferred Tools

Some tools are **deferred** — present in the catalog but not surfaced to the LLM until
explicitly looked up via `tool-search`. This pattern compresses the prompt without
losing capability.

> **Status (slice 59, 2026-05-24):** `core::ToolDef` now carries
> `deferred` and `category`, and `tool::CatalogRenderer` can split a
> `Registry::catalog()` snapshot into sorted active full-schema blocks and
> sorted deferred name/description rows. `Registry::catalog()` still returns
> all registered tools; there is no `deferred_catalog()` API yet, and
> `tool.search` / per-session promotion state remain future `oran-agent` /
> `oran-prompt` work.

Implementation:

- `ToolDef::deferred = true` marks the tool for deferred rendering.
  Today this is consumed by `tool::CatalogRenderer`; a future
  `deferred_catalog()` convenience may expose the filtered snapshot.
- The default system prompt builder lists the deferred tool *names + one-line descriptions*
  in a compact "Deferred Tools" section.
- `tool-search` is a non-deferred tool that returns the full schema on demand.
- The registry keeps a per-agent set of "promoted" deferred tools whose full schema is
  now in the prompt; the prompt builder honors it.

`async::Channel<Promotion>` could push promotions across iterations if needed; for now
a per-loop mutable set is simpler.

## Output Scrubbing

Tool output may contain secrets (e.g. environment variables echoed by a shell tool).
v2 routes every tool output through `oran-log::redact` which uses **runtime** (not
compile-time) regex patterns loaded from `config.runtime.redaction_patterns`.

The legacy code used `ctre` (compile-time regex). v2 uses `re2` (small TU, fast,
runtime-configurable). See `docs/rules/libraries.md`.

## Hook Surface

Tool lifecycle hooks:

- `tool.before(name, input, identity)` — may rewrite input or short-circuit.
- `tool.dispatched(name, input, identity)` — after permission, before handler.
- `tool.after(name, input, output, identity, duration)` — always.
- `tool.error(name, input, error, identity)` — if handler returned an error.

See `permissions-and-hooks.md` for sink kinds.

## Anti-Patterns

- Tools that reach into globals (`workspace_root` via a singleton). They take the
  `Runtime` handle.
- Tools that spawn background coroutines without registering them with the runtime's
  cancellation token.
- Tools that ignore the `Capability` declaration and use a service anyway.
- Tools that call `provider::System::send` themselves to "ask another model". That's
  agent territory; use `agent.spawn` or the orchestration tools.

## Workspace Handle

Slices 37-40 closed the per-tool half of the workspace migration: every
filesystem built-in (`file.read`, `file.write`, `file.edit`, `file.delete`,
`file.search`, `directory.list`) resolves its input through the workspace
seam when `DispatchContext::workspace` is supplied. Slice 41 lands the
bootstrap-owned half: `bootstrap::RuntimeAssembly` constructs and owns the
`tool::Workspace`, and `oran-config` recognises
`permissions.workspace.extra_{read,write}_roots` so overrides canonicalise
once at boot rather than per-tool. Slice 55 moves resolution to the registry
pre-permission boundary and threads redacted path-resolution metadata into the
existing audit pipeline.

Current surface and forward shape:

- `tool::Workspace` is a value type with a canonical root, extra read roots,
  extra write roots, and four intent-specific methods:
  `resolve_read`, `resolve_write`, `resolve_delete`, and `resolve_list`.
- `ResolvedPath` carries the canonical absolute path string, a path relative
  to the matching root, and resolution flags (`symlink_followed`,
  `created_parents`, `outside_workspace_explicit_override`,
  `override_root_index`).
- Bootstrap ownership ships in slice 41 — `RuntimeAssembly::build` now
  constructs the workspace from `RuntimeAssemblyOptions::workspace_options`
  (which bootstrap fills from `config::PermissionsConfig::workspace`) and
  exposes it via `RuntimeAssembly::workspace()`. Callers thread this
  `Workspace&` into `DispatchContext::workspace`; the later `tool::Runtime`
  handle promotes this into `Runtime::workspace()` (capability-gated).
- Every filesystem built-in resolves its input *before* permission evaluation
  via `Workspace::resolve_read` / `resolve_write` / `resolve_delete` /
  `resolve_list`. The intent is encoded in the method name; callers cannot
  mix them up at the type level. `Registry::dispatch` stores the successful
  result in `DispatchContext::resolved_path`; built-in handlers consume that
  absolute path and fall back to in-handler resolution only when a caller did
  not supply a workspace.
- Audit rows carry path-resolution metadata under
  `permission::AuditEvent::metadata_json`. Successful resolves include hashed
  input/root values, `resolved_relative_path`, symlink / parent-creation /
  override flags, and `override_root_index`; resolver failures include the
  hashed input/root plus `error_kind` and `error_reason`. Raw input paths are
  not persisted.
- Symlink policy is now uniform across the whole filesystem built-in set:
  follow inside-workspace symlinks for `resolve_read` / `resolve_list`
  (rejecting `symlink_escape` when the target leaves the root), and refuse
  symlinks for `resolve_write` / `resolve_delete` (`symlink_target`).
  Nested entries during a `file.search` walk continue to skip symlinks
  wholesale, a stricter form of the same rule that defers the
  workspace-aware "follow if it stays inside" enhancement to a future
  walker.

Full contract, override roots, audit fields, and acceptance criteria live in
[`../product-specs/0013-workspace-and-path-policy.md`](../product-specs/0013-workspace-and-path-policy.md).

## Scheduler Boundary (Forward-Looking)

The registry is — and stays — **single-threaded**. Concurrency is owned by an
`agent::ToolScheduler` that sits *between* the provider response parser and
`Registry::dispatch`. The scheduler:

- batches the provider's parallel `tool_use` blocks,
- classifies each call as read-only or mutating (via
  `ToolDef::required_capabilities`),
- takes a shared lock for reads and an exclusive lock for writes on the
  canonical workspace-resolved path,
- runs up to `config.agent.max_parallel_tools` calls in flight,
- returns results in the original `tool_use` order regardless of execution
  order (the prompt-cache contract in
  [`../rules/prompt-design.md`](../rules/prompt-design.md) depends on
  byte-stable ordering),
- enforces the per-call timeout and propagates the parent cancellation
  signal,
- consumes `hook::Bus::publish_blocking` for `permission_ask_rendered`
  rendering (the first blocking-hook consumer).

Do **not** add internal locks to `tool::Registry` as a first move. The
registry runs on the agent strand; the scheduler hops to worker executors at
dispatch time.

Full contract, ordering guarantees, bounded-state primitives, and acceptance
criteria live in
[`../product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md).

## Catalog Renderer

`tool::CatalogRenderer` is the pre-`oran-prompt` renderer for prompt-facing
tool-catalog bytes. It accepts a `core::ToolDef` snapshot, sorts by tool
name, renders non-deferred tools as full-schema blocks, and renders
deferred tools as compact name/description rows. Rendering a block depends
only on stable `ToolDef` fields: `name`, `description`,
`input_schema_json`, `required_capabilities`, and `category`; `deferred`
only decides whether that block is emitted as active full schema or as a
deferred index row.
JSON Schema canonicalisation lives in `src/oran-tool/catalog.cpp`; the
public header stays nlohmann-free through a small pimpl.

Full-schema blocks are memoised in a bounded rendered-block cache keyed by
the fields that affect the block bytes plus `renderer_version`. The default cap is 256
entries, matching spec 0012's bounded-state inventory; setting
`max_cached_blocks = 0` disables memoisation rather than creating an
unbounded cache. The public
`ToolCatalogCacheStats` shape exposes only aggregate counters and the
renderer version, never tool schemas or cache keys. `oran-prompt` will
consume this renderer when the prompt builder lands; active-tool config,
`tool.search`, and promotion state remain future work.

Prompt-shape reference: this slice adopts the Piebald Claude Code prompt
corpus' stable tool-description pattern — one deterministic block per tool
with name, description, and schema, plus compact discovery rows for deferred
tools — and rejects per-invocation status or timing text in catalog bytes
because that would violate `prompt-design.md`'s cache-prefix invariants.

## Output Shape v2

Slice 60 moves `tool::Output` out of `registry.hpp` and into
[`../../include/oran/tool/output.hpp`](../../include/oran/tool/output.hpp).
This closes the deep-review "tool output is too small" finding without
claiming the whole spec-0014 transport stack is complete. The shipped public
shape is:

```cpp
struct ToolUsage {
  std::optional<std::uintmax_t> bytes_read;
  std::optional<std::uintmax_t> bytes_written;
  std::optional<std::uint32_t> files_touched;
  std::optional<std::uint64_t> match_count;
  std::optional<double> cost_estimate;
  std::optional<std::chrono::nanoseconds> wall_time;
  bool truncated = false;
  bool data_dropped = false;
};

struct Attachment {
  std::string file_path;
  std::string mime_type;
  std::optional<std::uintmax_t> byte_size;
  std::optional<std::string> fingerprint;
};

struct Output {
  std::string text;
  std::optional<std::string> data_json;
  std::vector<Attachment> attachments;
  ToolUsage usage;
  bool is_error = false;
};
```

`data_json` is serialized JSON rather than a public `nlohmann::json` value.
That keeps `<oran/tool/output.hpp>` third-party-free and cheap to include,
while leaving provider adapters responsible for protocol-specific parsing and
serialization when they land. `Attachment` is a concrete metadata value now;
tools keep the vector empty until a real file/image/blob producer exists.

Current and future policy:

- Keep the public header free of heavy JSON includes so the compile-budget
  rule in [`../rules/compile-budget.md`](../rules/compile-budget.md) stays
  honoured. Heavy JSON lives in handler/provider implementation TUs.
- `Output::text_only(...)` is the v1-compatible path for current handlers;
  `Output::error(...)` marks an error envelope and may carry serialized
  structured error bytes.
- `Registry::dispatch` copies `Output::usage` into
  `hook::ToolAfterPayload::usage` on successful handler returns. Dispatch
  failures keep usage empty.
- Built-ins migrate one at a time. `file.read` still keeps the stable
  spec-0011 text fallback
  `<path>:<start_line>-<end_line> fingerprint=<token> bytes=<n>[ truncated]`
  followed by the requested body, and slice 62 also stores a serialized JSON
  object in `Output::data_json` with `kind=file_read`, `path`, requested
  `text`, `fingerprint`, `start_line`, `end_line`, `returned_bytes`, and
  `truncated`.
- Slice 61 migrates the current mutation built-ins to fill counters:
  `file.write` reports `bytes_written` and `files_touched`; `file.edit`
  reports `bytes_read`, `bytes_written`, `files_touched`, and
  `match_count`; `file.delete` reports `bytes_written=0` and
  `files_touched=1`. Their model-facing summaries stay unchanged and
  `data_json` remains empty.
- Provider adapters will consume `data_json` only when the target protocol
  supports structured tool-result bytes. Anthropic Messages, OpenAI
  Responses, Gemini, and OpenAI-compatible mappings remain spec-0014 follow-up
  work because `oran-provider` does not exist yet.
- Scheduler byte caps, audit usage fan-out, raw `data_json` hook redaction for
  trusted-local sinks, and structured payload migration for `file.search` /
  `directory.list` remain downstream spec-0014 items.
- The `tool::parse_input<T>` helper tracked under the deep-review backlog
  (`exec-plans/tech-debt-tracker.md`) lands in the same arc so handlers
  stop hand-rolling their JSON parsers.

## Bench

`bench/oran-tool/` ships:

- `bench/oran-tool/registry_lookup` — N-tool catalog, repeated `find()` calls.
- `bench/oran-tool/dispatch_overhead` — dispatch path latency without doing real work.
- `bench/oran-tool/permission_eval` — cost of permission engine on realistic rule sets.
- `bench/oran-tool/catalog.render_cold_32_tools` vs.
  `catalog.render_hot_32_tools` — cold JSON Schema canonicalisation and
  block rendering vs. the bounded rendered-block cache hot path.
- `bench/oran-tool/output.text_only` vs.
  `output.with_data_16kib` — v1-compatible text-only envelope construction
  against a structured envelope carrying 16 KiB of serialized payload bytes
  plus usage counters.

`compare.cpp` reports the dispatch overhead as a percentage of typical tool latency
(file.read of 4 KB; shell.exec of `/bin/true`).
