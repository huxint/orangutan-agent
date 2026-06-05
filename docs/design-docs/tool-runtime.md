# Tool Runtime

The tool registry is the agent's hand. It is the place where:

- The agent decides what it *can* do (the catalog presented to the LLM).
- The runtime checks whether it *may* do something (permissions).
- The runtime observes when it does (hooks).
- The runtime knows when it's *deferred* (`tool.search`-style discovery).

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
  deactivate_skill,
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
> Slice 168 adds the first memory built-in: `memory.recall`
> (`Capability::read_memory`, deferred, category `memory`). Input
> `{query, limit?, kinds?}` recalls long-term records through
> `DispatchContext::memory_recall`, keeping `oran-tool` independent of
> `oran-memory`; successful output carries deterministic recall text,
> `data_json.kind = "memory_recall"`, recalled record metadata, and
> `usage.match_count`.
> Slice 169 adds the first write-side memory built-in: `memory.remember`
> (`Capability::write_memory`, deferred, category `memory`). Input
> `{id, kind, title, body, importance?, tags?, linked_record_ids?, shadow?}`
> upserts one long-term memory record through
> `DispatchContext::memory_remember`, with bootstrap supplying scope and
> dispatch-time timestamps while keeping `oran-tool` independent of
> `oran-memory`; successful output carries confirmation text,
> `data_json.kind = "memory_remember"`, saved record metadata, and
> `usage.bytes_written`.
> Slice 170 adds the delete-side memory built-in: `memory.forget`
> (`Capability::write_memory`, deferred, category `memory`). Input `{id}`
> removes one scoped long-term memory record through
> `DispatchContext::memory_forget`, keeping `oran-tool` independent of
> `oran-memory`; successful output carries confirmation text,
> `data_json.kind = "memory_forget"`, scoped removed-key metadata, and
> `usage.bytes_written = 0`.
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
> Slice 79 adds the trace/audit correlation seam:
> `DispatchContext::parent_turn_id` is an optional `core::TurnId` copied into
> every permission audit event and metadata update produced by `dispatch`.
> Direct agent-loop callers set it from `RunTurnInputs::turn_id`; when tracing
> is configured and the caller leaves that id unset, slice 85 generates one
> before dispatch. When tracing is disabled the loop clears it for the duration
> of the dispatch so audit rows keep `parent_turn_id = NULL`.
> Slice 22 (2026-05-18) added the hook-bus tap: `DispatchContext`
> now also carries an optional `hook::Bus*`; when non-null, dispatch
> publishes `hook::Event::tool_before` after the registry resolves
> the tool def (so a sink can see every known call attempt
> regardless of how the call is subsequently gated) and
> `hook::Event::tool_after` at every exit (handler success,
> permission deny, broker rejection, audit error) with a
> `ToolAfterPayload { succeeded, output_text, error_kind,
> error_message, started_at, finished_at, duration }` that
> flattens the dispatch outcome for forensic queries. Slice 25 adds
> `tool_dispatched` on the paths where the handler will run and
> `tool_error` on failure exits; slices 60/65/66/67 add structured
> output usage/data fan-out, byte caps, and same-row audit usage
> enrichment. Slice 91 turns `tool_before` into the first blocking
> consumer: `Registry::dispatch` calls
> `Bus::publish_blocking<Event::tool_before>` before workspace
> resolution and permission evaluation, records every consulted sink
> decision in `metadata_json.hook_decisions`, converts veto / hook
> error / malformed rewrite decisions into
> `AuditOutcome::blocked_by_hook`, substitutes rewrite input before
> permission/handler execution, and promotes otherwise-allow calls
> into the broker path on `require_approval`. Unknown tool names
> are still silently rejected without a hook publish (the dispatch
> never started). Slice 92 threads `config.hooks.timeout_ms` through the
> assembly-owned `hook::Bus`, so a blocking sink that exceeds the
> per-sink deadline is recorded as `blocked_by_hook` with
> `reason=hook_timeout` and `metadata_json.hook_decisions[].elapsed_ms`.
> Slice 93 writes a second, joinable audit row for traced blocking
> `tool_before` publishes before the permission-decision row:
> `event_kind=hook_publish`, the same `parent_turn_id`, and
> `metadata_json` fields for `event`, `sink_id`, `decision_kind`,
> `reason`, optional `elapsed_ms` / `error`, and the full
> `hook_decisions[]` trace. The ordinary permission row remains the
> durable permission decision and keeps its existing outcome semantics.
> Slice 94 adds the direct `permission_ask_rendered` bridge: when an
> `ask` rule matches, a broker is present, no replay token was supplied,
> and a bus is attached, dispatch publishes the typed approval payload.
> A subscribed sink returning `proceed` issues a broker grant, optionally
> copies the token into `DispatchContext::approval_token_output`, records
> `outcome=approved`, and runs the handler; `veto` records
> `outcome=rejected` / `reason=operator_denied`, skips the handler, and
> serializes the sink trace under
> `metadata_json.permission_ask_decisions[]`. With no subscribed ask sink,
> the legacy `approval_required` short-circuit is preserved.
> Slice 95 adds the concrete `cli::OperatorPromptSink` that renders this
> payload for terminal operators. Capability-gated runtime services
> (`tool::Runtime` accessor surface) and binary binding of the CLI sink stay
> on future slices.
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
> Slice 147 (2026-06-03) adds the second `oran-tool-skill` built-in,
> `skill.deactivate` (capability `deactivate_skill`, input `{"name": <string>}`,
> non-deferred). Like `skill.invoke` it delegates the concrete lookup through a
> `DispatchContext::skill_deactivate` callback so `oran-tool` stays independent
> of `oran-skill`; bootstrap returns a versioned `skill_deactivation` record in
> `Output::data_json` that the next prompt boundary nets against prior
> activations. `deactivate_skill` carries no explicit `Defaults::for_mode` rule,
> so it inherits the same per-mode catch-all as `invoke_skill` (`ask` in
> `default`, `deny` in `strict`/`sandboxed`, `allow` in `permissive`).

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
The current `DispatchContext` bundle carries the audit identity
(`scope_key`, `agent_key`, `identity`), permission services, optional hook
bus, transient registry/workspace state, output caps, and the optional
`parent_turn_id` used by spec 0018 cause-chain joins. Current-clock callers
use `DispatchContext::for_now(...)`; fixed-clock tests may still initialise the
context directly with aggregate syntax.
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
registry does **not** own them; bootstrap does. The current implementation also sets
`DispatchContext::registry` to the dispatching `Registry` for the duration of a call
and restores the previous pointer on exit. Normal handlers ignore this non-owning
pointer; metadata tools such as `tool.search` use it to inspect the live catalog
without capturing a self-reference inside a movable registry value.

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
| `oran-tool-skill`             | `skill.invoke`, `skill.deactivate`                      |
| `oran-tool-background`        | async job orchestration                                  |
| `oran-tool-attachments`       | message attachment management                           |
| `oran-tool-runtime-loader`    | dynamic tool reloading                                  |
| `oran-tool` core built-in     | `tool.search` deferred tool discovery / metadata lookup |
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
1. Hook bus → tool.before (blocking)
   - proceed: continue with the original input.
   - rewrite: replace the effective input before workspace resolution,
     permission evaluation, broker checks, audit, handler execution, and
     later hook payloads.
   - veto / hook error / malformed rewrite: record
     outcome=blocked_by_hook, skip the handler, and return
     Error::permission_denied.
   - require_approval: continue, but promote an otherwise-allow verdict into
     the existing broker path.
2. Workspace pre-resolution for known filesystem built-ins.
3. Permission engine evaluates rules against {tool name, effective input,
   identity, capability set}.
4. If "ask" → ApprovalBroker (existing flow).
5. Hook bus → tool.dispatched (after approval/permission, before handler).
6. Handler runs.
7. Hook bus → tool.error on failures and tool.after always.
```

The legacy code's per-tool boilerplate ("check permission inside the tool handler") is
gone; the registry does it once. Tools that need *additional* fine-grained checks (e.g.
network egress allowlist) call `permission::Evaluator::check_capability(...)` inside
their handler.

## Deferred Tools

Some tools are **deferred** — present in the catalog but not surfaced to the LLM until
explicitly looked up via `tool.search`. This pattern compresses the prompt without
losing capability.

> **Status (slice 72, 2026-05-24):** `core::ToolDef` carries
> `deferred` and `category`, and `tool::CatalogRenderer` can split a
> `Registry::catalog()` snapshot into sorted active full-schema blocks and
> sorted deferred name/description rows. `Registry::catalog()` still returns
> all registered tools; there is no `deferred_catalog()` API yet. `oran-tool`
> now also registers the non-deferred `tool.search` metadata lookup, which
> searches the current registry snapshot by exact name, category, and/or
> required capability and returns text plus structured `Output::data_json`
> containing matched tool definitions. `oran-config` now parses
> `runtime.prompt.active_tools` as either `"defaults"` or an explicit
> allowlist. `prompt::Builder` now consumes that typed selector and feeds a
> selected catalog snapshot into `tool::CatalogRenderer`. `oran-prompt` now
> ships `prompt::PromotionState`, a session-owned 16-entry / 24-hour LRU+TTL
> promotion set whose sorted snapshot lets the next builder call move selected
> deferred tools into the active catalog. `agent::SessionState` now wires
> successful `tool.search` results into that state by promoting deferred matches
> and ignoring failed or non-search outputs.

Implementation:

- `ToolDef::deferred = true` marks the tool for deferred rendering.
  Today this is consumed by `tool::CatalogRenderer`; a future
  `deferred_catalog()` convenience may expose the filtered snapshot.
- `prompt::Builder` lists the deferred tool *names + one-line descriptions*
  in section 3's compact deferred-tool index.
- `tool.search` is a non-deferred tool that returns the full schema on demand.
- `agent::SessionState` owns `prompt::PromotionState` and calls `promote`
  after successful `tool.search` results. The prompt builder honors the
  resulting snapshot by rendering promoted tools with full schemas.

`async::Channel<Promotion>` could push promotions across iterations if needed; for now
a per-loop `prompt::PromotionState` is simpler.

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

For `file.write` and `file.edit`, default/non-trusted hook sinks receive a
sanitized `input_json` view instead of the raw mutation input. The sanitized
view carries `kind=redacted_tool_input`, `tool_name`, the full input SHA-256,
the input byte count, and the redacted string byte counts; sinks whose
`Sink::kind()` is `trusted_local` receive the original input.

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
- binds the concrete UI sink for `permission_ask_rendered` rendering
  (the registry already owns the blocking publish + broker handoff, and
  `oran-cli` already owns the terminal sink).

Do **not** add internal locks to `tool::Registry` as a first move. The
registry runs on the agent strand; the scheduler hops to worker executors at
dispatch time.

> **Status (slice 116, 2026-05-27):** the skeleton ships in
> `oran-agent` as `agent::ToolScheduler` (`<oran/agent/scheduler.hpp>`,
> `src/oran-agent/scheduler.cpp`). The skeleton lands:
>
> - bounded parallelism via `async::Channel<std::monostate>` filled with one
>   permit per `ToolSchedulerOptions::max_parallel_tools` slot,
> - per-call timeout via
>   `asio::experimental::awaitable_operators::operator||` against
>   `async::sleep_for(state->executor, options.per_call_timeout)` — a
>   timeout returns `Error::cancelled` with
>   `reason=timeout`, `tool=<name>`, `per_call_timeout_ms=<n>` context,
> - parent-cancellation propagation via one `asio::cancellation_signal`
>   per spawned call (held in a `std::deque<asio::cancellation_signal>` so
>   addresses stay stable across `emplace_back` because the signal is
>   neither copyable nor movable). When the parent's `completion.receive()`
>   sees a cancelled error, the scheduler emits on every child signal and
>   drains remaining completions with the parent slot filtered to
>   `disable_cancellation()` so each spawned child can still publish its
>   final completion message,
> - ordered results via indexed `std::vector<std::optional<ToolBatchResult>>`
>   collated into the return vector after every child has completed,
> - per-call `tool::DispatchContext` built from the caller's prototype with
>   `DispatchContext::for_now(prototype, thread_approval_token_output)` so
>   concurrent dispatches do not race on the prototype's `registry`,
>   `resolved_path`, `approval_token_output`, or `now` fields.
>   `Registry::dispatch` stays `const` and the `entries_` map is read-only
>   after boot, so concurrent dispatch is safe; long-lived services
>   referenced by the prototype (audit, hook bus, broker) remain
>   responsible for their own concurrency story
>   (`StorageAuditSink` already serialises writes through the SQLite
>   `Pool` writer).
>
> Slice 155 promotes that clone-and-refresh pattern to the public
> `tool::DispatchContext` surface: `DispatchContext::for_now(executor, rules,
> audit, scope_key, agent_key, identity)` creates a fresh current-clock base
> context, and `DispatchContext::for_now(prototype,
> thread_approval_token_output)` copies long-lived services while clearing the
> dispatch-local `registry` / `resolved_path` fields and refreshing `now`.
> Tests can still aggregate-initialise the struct when they need a pinned
> approval clock.
>
> The scheduler is wired through `agent::Loop` for every production tool batch
> as of slice 120.
>
> **Status (slice 117, 2026-05-28):** the per-canonical-path read/write lock
> table now lives in
> `src/oran-agent/_impl/path_lock_table.hpp` /
> `src/oran-agent/path_lock_table.cpp` and is consumed by
> `ToolScheduler::run_batch`. Tools declaring `Capability::write_file`,
> `edit_file`, or `delete_path` take an exclusive lock; tools declaring
> `read_file` or `list_directory` take a shared lock; tools without a
> filesystem capability skip path locking entirely and run under the
> existing bounded-parallelism slot only. The lock key is the
> workspace-resolved absolute path the scheduler derives by extracting the
> JSON `path` field from `ToolBatchCall::input_json` and passing it through
> the prototype's `tool::Workspace` resolver (`resolve_read` / `resolve_list`
> for shared, `resolve_write` / `resolve_delete` for exclusive); calls
> without a workspace, without a `path` field, or whose path fails
> workspace resolution fall through to bounded-parallelism only (the
> registry's own pre-resolution path then surfaces the same resolution
> failure with audit context intact). Per-entry state is FIFO: a queued
> exclusive waiter blocks new shared acquirers from skipping the line, but
> consecutive shared waiters fan out together when no writer is active.
> Cancellation while waiting is reconciled by removing the cancelled
> waiter from the queue, or — if a release has already pre-incremented the
> counter on the waiter's behalf — by undoing that increment and waking
> the next waiter so the chain does not stall. `ToolSchedulerOptions::idle_lock_ttl`
> reaps idle entries on `ToolScheduler::reap_idle_locks(core::Time)` (the
> spec-0012 background tick lands with a later slice); the public
> `ToolScheduler::lock_stats()` returns a `ToolSchedulerLockStats` snapshot
> of `shared_acquires`, `exclusive_acquires`, `contended_acquires`,
> `cancelled_acquires`, `reaped_entries`, `current_entries`, and
> `peak_entries` for `--explain-rules`-style consumers before
> `oran-log` exists.
>
> **Status (slice 118, 2026-05-29):** the audit / hook fan-out and approval
> invariants are verified under parallelism with **no production change**. An
> N-call batch records exactly N permission-decision rows and emits exactly N
> `tool_after` publishes (failures included) regardless of completion order;
> `Verdict::ask` resolves on each call's own slot (the channel-as-semaphore
> permit is held across the whole `run_call`, so a pending approval does not
> release the slot or hide a denied call behind a successful one); and the
> slice-67 same-row usage enrichment stays correct for identical concurrent
> calls because each `update_metadata` matches on `previous_metadata_json` and
> so consumes exactly one not-yet-enriched row (the two enrichments pair 1:1
> with the two decision rows, and identical calls carry identical usage). The
> three new `tests/agent/test_scheduler.cpp` cases pin this; the same-row case
> doubles as a cross-talk detector.
>
> **Status (slice 119, 2026-05-29):** parent cancellation now ends every
> *cancel-aware* in-flight call within a 100 ms grace window
> (`kCancellationGrace`, spec 0012 AC5). `run_batch` emits on each child's
> `asio::cancellation_signal`, disables its own cancellation filter, then
> races the remaining drain against `async::sleep_for(kCancellationGrace)` and
> returns `Error::cancelled` with `reason=parent_cancelled` instead of waiting
> unbounded. A handler that ignores its cancellation slot cannot be forced to
> stop — asio cancellation is cooperative, and `co_await (dispatch || timeout)`
> does not resolve until that handler returns (the `wait_for_one` parallel
> group cancels the loser but still awaits it) — so the scheduler stops
> awaiting at the deadline, records a `cancellation_lag` audit row
> (`event_kind=cancellation_lag`, `metadata_json.error_kind=cancellation_lag`)
> naming the offending tool, and lets the laggard wind down on its own (the
> shared `BatchState` keeps it alive). A batch of purely cancel-aware tools
> records no such row. Closes AC5.
>
> **Status (slice 120, 2026-05-29):** the scheduler is the production
> tool-dispatch path. `agent::Loop` dispatches every batch (including N=1)
> through `scheduler.run_batch(...)` instead of the sequential
> `for (use : tool_uses) registry.dispatch(...)` loop; ordered batch results
> become `tool_result` blocks with the same model-repairable-vs-fatal error
> semantics, and a parent cancellation or per-call infrastructure error ends
> the turn with `cancellation_phase=tools`. `bootstrap::AgentPromptRunner`
> constructs and owns a persistent `ToolScheduler` from the
> `runtime.tool_scheduler.{max_parallel_tools, per_call_timeout_ms,
> idle_lock_ttl_ms}` config block (defaults 4 / 60000 / 300000) and threads it
> into `RunTurnInputs::scheduler`; a caller that omits it gets a per-turn
> fallback with default options. A single-call batch threads
> `approval_token_output` for blocking-ask replay; a parallel batch drops it
> because one slot cannot disambiguate N issued tokens.
> `bench/agent/scheduler_overhead` (AC12) and `scheduler_audit_fanout` ship.
> The tool-scheduler v1 arc (spec 0012 AC1-AC7, AC10, AC12, ≥90% of AC11) is
> complete, and the registry stays single-threaded throughout.

Full contract, ordering guarantees, bounded-state primitives, and acceptance
criteria live in
[`../product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md).

## Catalog Renderer

`tool::CatalogRenderer` is the registry-owned renderer for prompt-facing
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
renderer version, never tool schemas or cache keys. `oran-prompt` consumes
this renderer for sections 2 and 3; it owns the active/deferred selection
from `runtime.prompt.active_tools` plus the promoted-tool snapshot consumed by
`prompt::Builder`. The registry-owned lookup half of the
deferred-tool design is shipped as `tool.search`: it accepts
`{name?, category?, capability?}`, requires at least one selector, ANDs
provided selectors, and returns `{kind:"tool_search", query,
match_count, matches[]}` in `Output::data_json`. Each match carries
`name`, `description`, nested `input_schema`, `required_capabilities`,
`deferred`, and nullable `category`, with `Output::usage.match_count`
mirroring the number of matches.

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

struct OutputCapOptions {
  std::size_t max_text_bytes = 256 * 1024;
  std::size_t max_data_bytes = 1024 * 1024;
};

OutputCapReport apply_output_caps(Output&, OutputCapOptions = {});
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
- `apply_output_caps(...)` is the shared dispatch/scheduler helper for
  spec-0014 byte caps. It truncates over-budget `text` at a UTF-8
  code-point boundary and sets `usage.truncated`; it drops over-budget
  `data_json` while leaving `text` intact and sets `usage.data_dropped`.
  `OutputCapOptions` defaults match `runtime.tool_output`'s 256 KiB text
  cap and 1 MiB structured-data cap.
- `Registry::dispatch` copies `Output::usage` into
  `hook::ToolAfterPayload::usage` on successful handler returns and now also
  copies successful `Output::data_json` into
  `ToolAfterPayload::data_json`. Dispatch failures keep usage and `data_json`
  empty.
- `Registry::dispatch` also fans non-empty successful `Output::usage` into
  audit metadata in slice 67. The permission decision row is still recorded
  before the handler runs; after the handler succeeds and `apply_output_caps`
  has set any `truncated` / `data_dropped` flags, dispatch best-effort calls
  `permission::AuditSink::update_metadata(...)` so the same row's
  `metadata_json.usage` carries bytes read/written, touched files, match count,
  cost, wall time in milliseconds, and cap flags when present. Slice 79 scopes
  that update by `parent_turn_id` as well as the existing identity/hash fields
  so two concurrent turns cannot enrich each other's same-tool audit row.
- Built-ins migrate one at a time. `file.read` still keeps the stable
  spec-0011 text fallback
  `<path>:<start_line>-<end_line> fingerprint=<token> bytes=<n>[ truncated]`
  followed by the requested body, and slice 62 also stores a serialized JSON
  object in `Output::data_json` with `kind=file_read`, `path`, requested
  `text`, `fingerprint`, `start_line`, `end_line`, `returned_bytes`, and
  `truncated`. Slice 63 migrates `file.search` next: handlers keep the
  `path:line:text` text rendering plus the trailing truncation summary, and
  also store a serialized JSON object in `Output::data_json` with
  `kind=file_search`, `path`, `pattern`, `regex`, `matches[]` (one
  `{path, line_number, text}` object per match), `match_count`,
  `truncated`, `truncation_reason` (null / `matches` / `bytes`),
  `files_scanned`, and `bytes_read`. `Output::usage` carries
  `bytes_read` (cumulative scanned file bytes), `files_touched`
  (non-binary scanned files), `match_count` (post-truncation), and the
  `truncated` cap flag. Slice 64 migrates `directory.list`: handlers
  keep the `<path>:<kind>:<size_bytes or '-'>` text rendering, and also
  store a serialized JSON object in `Output::data_json` with
  `kind=directory_list`, `path`, `include_hidden`, `max_entries`,
  `entry_count`, and an `entries[]` array of `{name, path, kind,
  size_bytes}` (JSON null `size_bytes` for non-regular kinds);
  `Output::usage` carries `files_touched=1` (the directory itself) and
  `match_count=entry_count`. Every filesystem built-in in `oran-tool`
  has now completed its v1 migration to the structured envelope.
- Slice 61 migrates the current mutation built-ins to fill counters:
  `file.write` reports `bytes_written` and `files_touched`; `file.edit`
  reports `bytes_read`, `bytes_written`, `files_touched`, and
  `match_count`; `file.delete` reports `bytes_written=0` and
  `files_touched=1`. Their model-facing summaries stay unchanged and
  `data_json` remains empty.
- Provider adapters consume `data_json` only when the target protocol supports
  structured tool-result bytes. Slice 107 ships the first request-side mapping:
  `agent::Loop` copies successful `tool::Output::data_json` into
  `core::ToolResultContent`, and `provider::make_protocol_request` maps those
  bytes into Anthropic Messages `tool_result.content[]` or serialized OpenAI
  Responses `function_call_output.output` while preserving text-only fallback
  behavior. Slices 108-109 add response decoding and an injected
  `ProtocolTransport` factory seam for Anthropic/OpenAI systems, slice 110 adds the
  `oran-http` body client, slice 111 adds the bootstrap-owned
  `http::Client`-backed `ProtocolTransport` adapter, and slice 112 wires that
  backend into configured-route `bootstrap::run`. Gemini/custom mappings and
  SSE transport remain follow-up work.
- Raw `data_json` hook redaction shipped in slice 65: `hook::Bus` delivers
  the field only to sinks whose `Sink::kind()` returns
  `SinkKind::trusted_local`; default sinks receive the text fallback and usage
  metrics with `data_json` cleared.
- Raw mutation-input hook redaction shipped in slice 152: the
  `file.write` / `file.edit` dispatch path fills
  `redacted_input_json` with a hash-and-byte-count summary, and `hook::Bus`
  substitutes that value for every non-trusted sink across
  `tool_before`, `tool_dispatched`, `tool_after`, `tool_error`, and
  `permission_ask_rendered`. Trusted-local sinks still receive the original
  `input_json`.
- Slice 66 applies output caps at the direct dispatch boundary via
  `DispatchContext::output_caps` before `Registry::dispatch` returns the
  output or publishes `tool_after`. The future scheduler owns those options
  for batched calls and will call the same helper before returning ordered
  results. Slice 67 adds direct-dispatch audit usage metadata enrichment; the
  slice-79 `DispatchContext::parent_turn_id` field gives direct-dispatch audit
  rows the trace join key when `agent::Loop` supplies or generates a turn id. The
  scheduler still owns batched-call correlation and option threading once
  parallel tool calls land.
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
- `bench/oran-tool/output.apply_caps` — cap helper cost for truncating text
  and dropping oversized structured data.

`compare.cpp` reports the dispatch overhead as a percentage of typical tool latency
(file.read of 4 KB; shell.exec of `/bin/true`).
