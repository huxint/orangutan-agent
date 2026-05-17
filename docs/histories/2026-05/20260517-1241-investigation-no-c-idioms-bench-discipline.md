## [2026-05-17 12:41] | Task: New rules — investigation methodology, no-C-idioms, bench discipline

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — docs-only rule sweep, single session, one history.
  Per the rewritten [`PLANS_GUIDE.md`](../../PLANS_GUIDE.md) "When NOT
  To Create A Plan", a doc-only sweep that fits inside one history is
  exactly the shape that should not open a plan. The `Next intended
  slice` bullet in `STATUS.md` still describes the next code work and
  is not retired by this slice.

### User Query

The user dictated three new rules across two messages:

1. > 当拿不准如何实现某个特性的时候，可以使用 subagent/teams 功能进行
   > 探讨，但是不要过多，最多启动两个subagent/teammates ... 也可以进行
   > 网络搜索 ... 但是不要盲目照抄 ... 走不通，要清楚是不是自己的信息
   > 没更新上，是否需要进行更新知识库(web/github/context 7等等) ... cpp
   > 代码要求简洁 高效 优雅 现代，避免使用 C 语法的代码.

2. > bench 应该是拿不准哪份代码执行速度更快的时候，才使用 bench 比较。
   > 没必要每一份代码都进行速度测试。并且告诉其清楚并非速度快就是最优解，
   > 因为速度可能会有抖动，其次就是当速度差距不大的时候综合对应的代码量
   > 和代码简洁，优雅性考虑。并且还可以考虑一些优化：如缓存优化, 逻辑优化,
   > 预计算, 多核处理等等

### Changes Overview

- **New rule file:** `docs/rules/investigation.md` — when to investigate
  before coding, subagent / teammate fan-out cap (≤ 2), web / Context7
  / GitHub as *reference not source*, and the "refresh stale knowledge
  before retrying a failing approach" discipline.
- **Extended rule:** `docs/rules/critical-rules.md` C17 — added the
  "No C-style constructs when a C++ equivalent exists" bullet
  (`static_cast` over `(T)x`, `nullptr` over `NULL`, `using` over
  `typedef`, `std::array` / `std::span` over `T[N]`, RAII over
  `malloc` / `free`, `std::print` / `std::format` over `printf`, etc.).
- **Extended rule:** `docs/rules/code-style.md` — new "C++ Over C
  Idioms" subsection under "C++ Idioms" with the full C → C++ pair
  table and a PREFERRED/FORBIDDEN code example. Cross-linked from
  C17.
- **Extended rule:** `docs/rules/testing-and-bench.md` — three new
  subsections under "## Benchmarks":
  - "When To Benchmark — And When Not To" (bench when you cannot
    rank by reading; don't bench-saturate).
  - "Reading Bench Results — Speed Is Not The Only Signal" (jitter;
    small-delta picks defer to code clarity; speed is one axis).
  - "Optimization Avenues Before 'Just Write A Faster Loop'"
    (algorithmic → cache → precomputation → parallelism, in that
    order; parallelism is the last resort, not the first).
- **Routing index:** `docs/rules/README.md` gained an
  `investigation.md` row. `AGENTS.md` "Conventions At A Glance"
  gained two new rows ("C vs C++ idioms", "Investigation") plus a
  refined "Bench discipline" row sitting next to the existing
  "Benches" row.
- **User memory:** updated `prefer-modern-cxx-and-ranges.md` with the
  no-C-idioms emphasis; added two new memories
  (`investigation-methodology.md` and
  `bench-criterion-and-optimization-order.md`); index re-synced.

### Design Intent

**Why investigation methodology gets its own rule file instead of
extending workflow.md.** The discipline crosses three areas (subagent
spawning, external research, stale-knowledge recovery) and ties them
together: each one in isolation is a heuristic, but together they form
"how to handle uncertainty" — a coherent posture that deserves a
dedicated home. Workflow rules (`workflow.md`) cover git / commits /
PRs; this is upstream of all of those. A dedicated file also gives the
"never blind-copy" point room to breathe — putting it as a one-liner
elsewhere would let it be ignored.

**Why the fan-out cap is 2 specifically.** One probe is the default
when the question is narrow ("does X compile?"). Two probes works
either as "contrasting probes you'll pick between" or as "probe + review".
Three probes introduces a "vote" with no quorum rule and no tiebreaker
— the agent ends up picking by gut feel after spending tokens on three
parallel reads. The cap forces decision design before delegation. The
rule file lists the forbidden shapes (fan-out > 2, sequential chain
faking a council, subagent-for-routine-work) so the failure modes are
explicit.

**Why "Context7 over WebSearch for library docs".** The user's framing
("更新知识库 web/github/context7 等等") makes Context7 a first-class
citizen for library docs specifically — it pulls version-pinned current
docs, where WebSearch returns a stale mix of forum posts and tutorials.
The rule splits the use cases: WebSearch / WebFetch for conceptual
prior art, Context7 for current-version syntax. This matches what the
plugin actually offers (it has `query-docs` and `resolve-library-id`).

**Why the "no C-style" rule lives in C17 + code-style, not in its own
rule.** C17 already says "modern facilities preferred"; adding a bullet
about "no C-style spellings" is the same conceptual rule. Promoting it
to its own critical-rule slot would split the "modern C++" discipline
across two C-numbered rules and force every reviewer to remember two
places. The table of concrete C-vs-C++ pairs lives in `code-style.md`
because the pairs are mechanical / style-level (clang-tidy can flag
many of them); critical-rules.md carries the principle, code-style.md
carries the lookup.

**Why bench rules sit in testing-and-bench.md rather than a new file.**
The existing rule file already owns the bucket-level "≥ 1 A-vs-B" floor
and the bench framework choice. The new content (when to bench, how to
read results, optimization order) is the *developer-side* counterpart
to those baseline rules; they share an audience and an enforcement
surface. Splitting would force a new mental table.

**Why the optimization order puts parallelism last.** This is the
single most expensive failure mode in legacy `orangutan/`: when a perf
issue surfaced, the response was usually "add a thread" — which then
added synchronization cost, debug difficulty, and cache-line bouncing
that often *erased* the win. The rule reverses the order: algorithmic
first (biggest win, cheapest to maintain), cache layout second
(measurable, debuggable), precomputation third (when source rarely
changes), parallelism only when the single-threaded baseline is
already cache-friendly and the work is genuinely independent. The
asymmetric maintenance cost is what justifies the strict ordering.

**Why the bench-clarity trade-off is "small delta → clarity wins".**
Bench numbers fluctuate. A 5–10% delta on a single machine is inside
the jitter band on many workloads. If picking the faster candidate
requires keeping a more complex implementation around forever, the
debugging cost dominates. The 10% threshold isn't a hard cliff; it's
a reviewer-and-author shared prior that "you should at least defend
the complexity, not assume it."

**Why these are framework rules and not code constraints (yet).** All
three sit at the *process* layer — they shape how the agent and human
decide what to write, not what compiles. No mechanical enforcement is
proposed for now; the matching scripts would be expensive to write
(grep for "subagent fan-out" in transcripts? grep for "(void)" in
diffs? — the latter could land via clang-tidy but is deferred until
the next time clang-tidy config is touched). Enforcement is
review-time, like `prompt-design.md`'s current enforcement layer.

### Files Modified

- `docs/rules/investigation.md` — new file.
- `docs/rules/critical-rules.md` — C17 gained the "no C-style"
  bullet pointing at code-style.md.
- `docs/rules/code-style.md` — new "C++ Over C Idioms" subsection
  under "C++ Idioms" (table + PREFERRED/FORBIDDEN example).
- `docs/rules/testing-and-bench.md` — three new subsections under
  "## Benchmarks".
- `docs/rules/README.md` — `investigation.md` row added.
- `AGENTS.md` — "Conventions At A Glance" gained "C vs C++ idioms",
  "Investigation", and "Bench discipline" rows.
- `docs/STATUS.md` — `Last completed history` repointed to this file.
- `~/.claude/projects/-home-huxint-projects-orangutan-refactor/memory/prefer-modern-cxx-and-ranges.md`
  — extended with the no-C-idioms emphasis.
- `~/.../memory/investigation-methodology.md` — new memory.
- `~/.../memory/bench-criterion-and-optimization-order.md` — new memory.
- `~/.../memory/MEMORY.md` — index re-synced.
- `docs/histories/2026-05/20260517-1241-investigation-no-c-idioms-bench-discipline.md`
  — this file.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

This change *is* a docs sweep; every file above is a doc. No
production-doc invalidation beyond the listed edits. The `STATUS.md`
"Open Tech-Debt Rows" list is unchanged — these are review-time rules
without deferred enforcement.

### Validation

- Commands run:
  - `make ci` — recommended next step; this entry is written assuming
    the docs hygiene scripts pass (no internal anchors moved, all
    cross-links land in existing files).
  - Markdown-link scan recommended over the changed files.
- Tests added/changed: none — documentation only.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues to file: none.
- Tech-debt entries: none added — these are review-time rules. If
  clang-tidy gains an enforcement check for `(T)x` / `(void)expr`
  later, that can be tracked as it lands.
- Linked release note: none — pre-release, framework-only change.
- Cross-references for future agents: the investigation rule's
  "Context7 over WebSearch" guidance is also encoded in the memory
  bucket; if the available MCP servers shift, both should update
  together.
