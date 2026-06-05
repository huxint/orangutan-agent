## [2026-06-05 21:36] | Task: Long-term memory FTS5-vs-vector-vs-hybrid search bench (slice 173)

### Execution Context

- Agent: `claude` (Claude Code)
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI
- Linked plan: none — single slice pre-described by `docs/STATUS.md` `Next intended slice`.

### User Query

> Deeply understand the project, grasp current implementation progress, read the
> relevant docs, then start implementing. When offered the two `Next intended
> slice` options, the user chose the self-contained hybrid bench with a
> bench-local cosine `VectorBackend` (over starting the gated sqlite-vec adapter).

### Changes Overview

- Areas: `bench/memory`, `oran-bootstrap` (slice version string).
- Key actions: added the first long-term search A-vs-B-vs-C bench scenario that
  exercises the slice-172 `memory::longterm::HybridRuntime`, registered it in the
  bench entry point, and bumped the reported slice to 173.

### Design Intent

Spec 0005 AC7 wants an FTS5-baseline-vs-vector comparison, and slice 172 landed
`HybridRuntime` with no caller. The two `Next intended slice` options were (A) the
gated sqlite-vec adapter and (B) this comparison. B was chosen because it is one
self-contained slice with no new dependency and no exec-plan, it gives A a baseline
to beat, and it puts the just-landed `HybridRuntime` under real load. The vector
side is a bench-local brute-force cosine `VectorBackend` (linear scan over
L2-normalized 256-dim embeddings, `partial_sort` top-K). It deliberately filters
only by scope and leaves kind/shadow to `HybridRuntime` (which re-checks the
hydrated record) — matching the real contract, where `VectorUpsert` carries no
kind/shadow. Embeddings are deterministic (seeded `std::mt19937_64`) so rankings
reproduce; they are not semantically meaningful, only sized to make the cosine
scan do corpus-proportional work. All three runs share one seeded 10k corpus so
nanobench's relative column is honest. The gated `--vector_memory=y` sqlite-vec
adapter will later satisfy the same `VectorBackend` and re-run this scenario; it
stays downstream (tech-debt P3, spec 0005 Scope v2).

Local result (WSL2, release, low err%): `longterm_search_fts5_only_10k_limit10`
**~15.42 ms/batch**, `longterm_search_vector_cosine_10k_limit10` **~675.6 µs/batch**,
`longterm_search_hybrid_fts5_vector_10k_limit10` **~16.18 ms/batch**. The FTS5-only
number tracks the existing standalone baseline (~15.56 ms) this run, confirming the
corpus/path is comparable. Honest reading: at 10k records the lexical scan dominates,
so hybrid ≈ FTS5 + ~5%, while the in-memory cosine reference is ~23× faster than FTS5
on this synthetic data — a machine-specific, embedding-specific datapoint, not a
ranking-quality claim.

### Files Modified

- `bench/memory/scenarios/search_fts5_vs_vector.cpp` — new scenario (brute-force cosine `VectorBackend` + 3 timed runs).
- `bench/memory/main.cpp` — register `register_search_fts5_vs_vector`.
- `bench/memory/README.md` — document the new scenario; correct the "vector/hybrid land later" note.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` `slice172` → `slice173`.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `bench/memory/README.md` — new bench scenario row + reference-backend note.
- `docs/STATUS.md` — slice 173 snapshot, repointed `Last completed history`, new `Next intended slice`.
- `docs/product-specs/0005-memory-system.md` — AC7 status (FTS5-vs-vector-vs-hybrid bench shipped), Scope v1 slice-173 bullet, Scope v2 benching note.
- `docs/design-docs/memory-system.md` — `bench/memory/` paragraph now records the vector/hybrid comparison; only gated sqlite-vec remains.

No public library API changed, so `docs/ARCHITECTURE.md` library inventory is unchanged.

### Validation

- Commands run: `xmake build bench-memory` (10.9 s), `xmake run bench-memory`, `xmake build orangutan` (10.3 s); `build/.../orangutan --help` reports `v2.0.0-slice173`; `make ci`.
- Tests added/changed: none — bench-only slice. Correctness is guarded in-scenario by `std::abort()` on a wrong hit count (each run must return `limit=10`).
- Bench impact: see Design Intent numbers.
- Compile-budget delta: none — one new bench TU under `bench-memory`; no per-TU cap and no library category affected; `compile_budget.json` unchanged.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: gated sqlite-vec adapter + hybrid ranking policy/wiring remain the P3 row in `docs/exec-plans/tech-debt-tracker.md` (review/deep-2026-05-21) and spec 0005 Scope v2; this slice supplies the baseline they compare against.
- Linked release note: none (bench-only; not user-visible).
