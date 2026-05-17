## [2026-05-17 23:30] | Task: add `file.search` built-in on top of slice-17/18/19 `tool::Registry`

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in
  [`STATUS.md`](../../STATUS.md) ("the remaining file built-in
  (`file.search`) on top of the slice-17/18/19 `tool::Registry`"),
  matching `PLANS_GUIDE.md` "When NOT To Create A Plan".

### User Query

> 深度了解项目，查看当前项目真实进度, 继续推进项目代码的实现.
> (Understand the project deeply, check the real current
> progress, and continue advancing the project code
> implementation.)

The user asked for autonomous forward motion. Of the four
`Next intended slice` candidates (`file.search` / approval-broker
wiring / first Anthropic adapter / signal-aware shutdown),
`file.search` is item #1 and the natural extension of slice
17/18/19's pattern — slice 19's history already flagged it as
"the remaining file built-in" and the approval-broker mediation
as a uniform cross-cutting change that should wait for its own
slice rather than getting bolted onto the next file built-in.
Slice 20 picks `file.search` and rounds out the file tool family
so the agent loop will be able to read, write, edit, and locate
files through one consistent dispatch surface.

### Changes Overview

- **New built-in `file.search`.** Registered via
  `tool::register_file_search` and aggregated into
  `tool::register_builtins`. Capability `Capability::read_file`
  (search is reading; a future slice can split `search_file` out
  if operator pressure for finer rules emerges). Schema:
  `{"path": <string>, "pattern": <string>, "max_matches"?: uint
  (default 100, ≥1), "include_hidden"?: bool (default false)}`,
  `additionalProperties:false`. The handler parses with
  `nlohmann::json` in its own TU, validates the four fields,
  performs one `co_await asio::post(ctx.executor)` so the
  blocking filesystem walk runs off the calling strand (the
  same discipline `io::read_text_file` uses), then runs a
  single in-process walk:
  - If `path` is a regular file, scan it line-by-line for
    literal occurrences of `pattern`. Single-file mode skips
    the binary heuristic — the caller named the file
    explicitly, so we trust their intent.
  - If `path` is a directory, traverse with
    `std::filesystem::recursive_directory_iterator` under
    `skip_permission_denied`. Dot-prefixed files and
    directories are skipped when `include_hidden=false`
    (descent into a dot-directory is suppressed via
    `disable_recursion_pending()` so an entire subtree is
    skipped, not just the directory entry). Symlinks are
    skipped wholesale (defensive default that keeps symlink
    loops off the table and matches `oran-io::list_directory`'s
    posture). Regular files are read via a private
    `read_text_capped` helper (mirror of `io::read_text_file`'s
    16 MiB ceiling), then a `looks_binary` probe rejects files
    with NUL bytes in their first 8 KiB.
  - For each file's contents, `scan_text` walks line-by-line
    (`contents.find('\n', cursor)`) and emits a `Match{path,
    line_number, line_text}` for every line whose
    `std::string_view::contains(pattern)` is true.
- **Budget+1 truncation signal.** Scans collect up to
  `max_matches + 1` matches. If the final tally overshoots,
  `truncated=true` and the vector is resized down to
  `max_matches`. This avoids paying for the rest of the file or
  tree once we know we're truncating while still distinguishing
  "exactly max matches" (not truncated) from "more were
  available" (truncated). The single extra match is the smallest
  state needed to make the boundary unambiguous.
- **Output text format.** Zero matches → the literal text
  `no matches` (non-error). Otherwise each match is rendered as
  `path:line:text` joined by `\n`; if truncated, a trailing
  `(truncated; matches capped at <N>)` line follows. The format
  is the same one ripgrep uses by default so an LLM that has
  seen grep output knows what to do.
- **`register_builtins` order.** `file.read` → `file.write` →
  `file.edit` → `file.search`; the slice's catalog test
  asserts size 4 and the insertion-order quadruple. The test
  case name lost the `slice-17` tag (it would keep ratcheting)
  in favour of the slice-tag-less "file tool catalog".
- **`tests/tool` extension.** 9 new cases / 82 new assertions
  cover:
  - `register_file_search` advertises `Capability::read_file`
    and the four-field schema.
  - Happy path on a single file finds the one literal match at
    the right line number and records one allow audit row.
  - Recursive walk over a 3-file / 4-match tree (`a.txt`,
    `sub/b.txt`, `sub/deep/c.txt`) reports every match by full
    path + line number.
  - `max_matches=3` against a 5-line / 5-match file yields
    three match lines plus the truncation summary (asserted
    via newline count == 3 and the summary substring).
  - NUL-bytes-in-first-8KiB rule skips a `.bin` file while a
    sibling `.txt` with the same pattern still matches.
  - `include_hidden=false` (default) skips `.hidden.txt` and
    descent into `.cache/`; the same call with
    `include_hidden=true` reaches them.
  - Zero matches return `text="no matches"` (non-error) and
    still record one allow audit row.
  - Missing path returns `Error::not_found` with the path in
    the error context.
  - A nine-shot malformed-input table covers bad JSON, missing
    path, missing pattern, non-string path, non-string pattern,
    non-int `max_matches`, non-bool `include_hidden`, empty
    pattern, and `max_matches=0` — and asserts each malformed
    call still recorded one `allow` audit row, proving the
    permission decision is independent of payload validity
    (same invariant as `file.write` / `file.edit`).
- **`bench-tool` extension.** New scenario file
  `bench/tool/scenarios/file_search.cpp` adds
  `file_search.single_file_one_match` (~8.2 µs on a ~14-line
  file with one match) vs.
  `file_search.recursive_dir_many_matches` (~27.6 µs on a 4-file
  / 14-line tree with 5 matches). The ~19 µs A/B delta is the
  `recursive_directory_iterator` walk + per-file `ifstream`
  open/read overhead the agent loop pays when a tool call
  reaches for a tree rather than a single file — the baseline a
  future memory-mapped scan or parallel walker would have to
  beat. The existing `registry.lookup` (~8.7 ns),
  `registry.dispatch_allow` (~2.66 µs), `file_write.*`
  (~12.2 µs each), and `file_edit.*` (~16.1 / 16.7 µs) baselines
  are unchanged within noise.
- **No xmake plumbing change.** `oran_lib` globs `**.cpp` under
  `src/oran-tool/`, so the new `file_search.cpp` is picked up
  without a target edit. `bench-tool` was already wired into
  `xmake/bench.lua` in slice 17; only its `main.cpp` adds the
  `register_tool_file_search` hook.
- **Slice-version bump.** `kVersion` 19 → 20; the binary's CLI
  surface is unchanged. `xmake run orangutan --help` reports
  `orangutan v2.0.0-slice20`.

### Design Intent

**Why `file.search` is the right next-slice candidate.** It is
item #1 on the refreshed `Next intended slice` list and the
natural extension of slice 17/18/19's "fill in the file tool
family" pattern. The approval-broker mediation, the second
candidate, modifies the `DispatchContext` shape and the dispatch
contract — it should land as a cross-cutting slice that touches
every built-in uniformly rather than bolted onto a new built-in's
own slice. The Anthropic adapter is too big for a single session
(provider transport + protocol + execution + first integration
test bench) and warrants an exec plan; the signal-aware shutdown
is small but lower-value mid-construction.

**Why literal substring rather than regex.** ripgrep-style
output format does not imply ripgrep-style match semantics — the
design doc says "ripgrep-style structured matches" but the v2
`tool::Output` shape is text-only at slice 19, so the "structured"
part is deferred regardless. Literal substring is what
`file.edit` already takes via `old_string`, so the file-tool
family shares one mental model: "the LLM emits the exact bytes
to find". This also keeps the slice off `re2/re2.h` — even
though `oran-permission` already pulls re2 in transitively, the
direct include is heavy. A future slice can add `"regex": true`
without rewriting the existing API.

**Why recursive walks + binary heuristic + dotfile-skip by
default.** Agents typically reach for `file.search` to answer
"where is X in this codebase?" — single-file search is the
narrower case. The NUL-byte heuristic is the same rule ripgrep
uses to keep PNG/JPEG/object-file output out of the response,
and dot-prefix skipping matches what most developers expect
when they `grep` a project root (`.git`, `.cache`, etc.).
`include_hidden=true` is the explicit opt-in.

**Why budget+1 truncation detection rather than scanning the
whole tree.** Scanning past the cap is wasted work; scanning
exactly to the cap leaves "did we have to stop?" ambiguous. The
overshoot-by-one technique pays at most one extra match's worth
of work and makes the truncation signal mathematical: `count >
max` iff there was at least one more match the cap forced us to
drop. Same idea used in pagination APIs that return
`{items: [...max], has_more: bool}` by fetching `limit + 1`.

**Why single-file mode skips the binary heuristic.** If the
caller passed `path=/tmp/some.bin`, they meant it. The binary
filter is for tree walks where the caller is asking "search this
folder" without knowing what every file inside is; it's not a
content-policy filter — it's a "don't drown the output" filter.

**Why symlinks are skipped wholesale.** Symlinked directories
can introduce loops `recursive_directory_iterator` does not
detect by default; symlinked files are an ergonomic ambiguity
(is the result keyed by the symlink path or the target?). MVP
just skips both. A future slice can revisit if a real workflow
needs it. `oran-io::list_directory` classifies symlinks as a
distinct entry kind for the same reason.

**Why `read_file` capability rather than a new
`search_file`.** Search is read at the operating-system layer
(stat + open + read). Operators who allow `read_file` will
allow search; operators who deny `read_file` already deny
search through the same rule. Adding a `search_file`
capability would mean adding it to `core/capability.hpp`'s
enum, its `enum_name`/`parse_enum` tests, and the design-doc
inventory — non-trivial surface for a benefit that materializes
only when a user wants "may read individual files but not scan
trees" policy, which is unusual. If that pressure shows up, a
follow-up slice can introduce the capability without breaking
the existing rule shape.

**Why one audit row, exactly as in slices 17/18/19.** The
permission decision is what audit records; the handler outcome
is hook-bus territory (still pending). Every built-in now
demonstrates the same invariant: the registry alone is
responsible for the audit record; the handler is responsible
for the *action*. The malformed-input table asserts this
explicitly — all nine reject paths still produce one `allow`
audit row each because the permission gate ran before the
handler validated the payload.

**Why bench `single_file_vs_recursive_dir` instead of
`scan_with_match_vs_no_match`.** The single-file mode's cost is
already documented by `file_write.dispatch_truncate` / 
`file_edit.dispatch_unique_replace` patterns; the informative
new contrast for `file.search` is "how much does the directory
walk cost over the single-file baseline?" — and the measured
answer (~19 µs over the ~8 µs single-file floor for a 4-file
tree) is the documented baseline a future memory-mapped scan or
parallel walker would have to beat.

### Files Modified

- `include/oran/tool/builtins.hpp` — new `kFileSearchName`
  constant and `register_file_search` declaration; refreshed
  module comment.
- `src/oran-tool/file_search.cpp` — new TU; handler + registrar.
- `src/oran-tool/builtins.cpp` — `register_builtins` now also
  wires `file.search` after `file.edit`.
- `tests/tool/test_registry.cpp` — 9 new file.search test
  cases (+82 assertions), a `TempDir` helper for trees, a
  `search_rule_set()` helper, and the existing
  `register_builtins` test refactored to assert the four-entry
  catalog (and renamed to drop the slice tag).
- `bench/tool/main.cpp` — registers the new
  `register_tool_file_search` block.
- `bench/tool/scenarios/file_search.cpp` — new scenario:
  single-file-one-match vs. recursive-dir-many-matches.
- `bench/tool/README.md` — documents the new A/B.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 19 → 20.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 20, history pointer, library
  surfaces row for `oran-tool` (40 / 309), refreshed
  `Next intended slice` bullet (down to approval-broker +
  Anthropic + signal shutdown), and two new tech-debt rows
  (regex support / ripgrep-class optimisations).
- `docs/exec-plans/tech-debt-tracker.md` — two new rows under
  the `tool/search` area documenting the deferred regex
  support and the deferred ripgrep-class optimisations
  (mmap + extension skip + `.gitignore` + parallel walk).
- `docs/QUALITY_SCORE.md` — Tool registry row refreshed with
  the fourth built-in's surface, schema, and bench numbers;
  Test framework row refreshed with the new `oran-tool` counts;
  Bench harness row refreshed with the new `file_search` A/B
  numbers.
- `docs/ARCHITECTURE.md` — slice-status preamble bumped to
  2026-05-20 and now lists all four file built-ins; `oran-tool`
  inventory row promoted from "built-ins `file.read`,
  `file.write`, and `file.edit`" to include `file.search`.
- `docs/design-docs/tool-runtime.md` — Status box (2026-05-20)
  notes all four built-ins.
- `docs/releases/feature-release-notes.md` — new top row for
  `oran-tool-file-search`.
- `docs/histories/2026-05/20260517-2330-oran-tool-file-search.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-tool` — clean (5 TUs, ~11 s).
  - `xmake build test-tool && xmake run test-tool` — 40 cases /
    309 assertions, all green.
  - `xmake test` — all 9 buckets green
    (test-async / cli / core / config / io / tool / bootstrap /
    permission / storage).
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `registry.lookup ~8.70 ns`,
    `registry.dispatch_allow ~2,656 ns`,
    `file_write.dispatch_truncate ~12,187 ns`,
    `file_write.dispatch_append ~12,173 ns`,
    `file_edit.dispatch_unique_replace ~16,053 ns`,
    `file_edit.dispatch_replace_all_many ~16,719 ns`,
    `file_search.single_file_one_match ~8,171 ns`,
    `file_search.recursive_dir_many_matches ~27,555 ns`.
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-20 banner; the CLI surface is unchanged.
- Tests added/changed: 9 new tool-bucket cases (+82
  assertions); the existing `register_builtins` test was
  updated to assert the four-entry catalog.
- Bench impact: existing scenarios unchanged within noise; new
  `file_search` scenarios baselined above.
- Compile-budget delta: one new TU in `oran-tool`
  (`file_search.cpp` ~3 s for the nlohmann + filesystem
  includes); one new TU in `bench-tool` (`file_search.cpp`
  scenario); the headers were already amortised over the
  bucket's existing budget envelope.

### Follow-ups

- Issues to file: none.
- Tech-debt entries:
  - Added 2026-05-17 — `file.search` does not support regex; pattern
    is literal substring. Follow-up: add `"regex"?: bool (default
    false)` route through `re2::RE2::PartialMatch`. Filed in
    `docs/exec-plans/tech-debt-tracker.md`.
  - Added 2026-05-17 — `file.search` does not yet ship ripgrep-class
    optimisations (mmap, extension-based binary skip, `.gitignore`,
    multi-threaded walk). Adequate at slice 20 (~27 µs / 4-file
    tree) but 3-10× slower than a tuned scanner on repo-scale
    inputs. Filed in `docs/exec-plans/tech-debt-tracker.md`; re-bench
    once `oran-agent` lands a real workload measurement.
  - The "remaining file built-ins" bullet is now fully retired;
    the approval-broker mediation, the Anthropic adapter, and the
    signal-aware shutdown are unchanged.
- Linked release note: 2026-05-17 `oran-tool-file-search` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the agent loop
  (`oran-agent`) lands, the natural extraction is the same as
  for slices 17 / 18 / 19 — wire `bootstrap::RuntimeAssembly`'s
  `AuditSink` + a `permission::RuleSet` built via
  `materialize_rules` into a single `tool::DispatchContext`
  shared by every iteration. All four file built-ins now live
  in the catalog. When `tool::Runtime` lands, all four file
  built-ins should be migrated together onto the
  `runtime.workspace()` capability-gated handle so workspace
  confinement applies uniformly. A `"regex": true` opt-in for
  `file.search` is the obvious follow-up shape if an LLM needs
  regex anchors / character classes; the existing literal
  substring path remains the default.
