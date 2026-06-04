## [2026-05-16 22:00] | Task: project-wide security review (slice 8 gate)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — review-and-fix task, no exec plan needed.

### User Query

> `/security-review` — audit the project for vulnerabilities and deficiencies.

### Changes Overview

- Areas: `oran-config` loader, `oran-storage` migration loader, `oran-storage` sqlite bind.
- Key actions: enumerated the attack surface across the four landed slices
  (`io`, `config`, `storage`, `permission`); fixed three high-confidence
  findings; recorded the residual findings inline for future slices.

### Findings

#### High — fixed in this commit

1. **`Config::load_file` reads the file without a size cap.** A malformed or
   hostile config file dropped at `<workspace>/.orangutan/config.json` could
   force the loader to allocate gigabytes of `std::string`, taking down the
   bootstrap before any permission gate runs. Fixed by adding
   `LoadOptions::max_bytes` (default 16 MiB) and rejecting larger files via
   `std::filesystem::file_size` *before* the read. Mirrors the existing
   `io::ReadTextOptions::max_bytes` discipline in `oran-io`.

2. **`load_migration_file` reads `.sql` files unbounded.** Same shape as
   finding #1 against the storage migration runner. Migration SQL is
   repo-vendored but a corrupt file (truncated FS, accidental binary blob
   committed) would still produce a runaway allocation. Bounded at 8 MiB
   via a constant in `migrations.cpp`; rejected before allocation.

3. **`Statement::bind_text` truncates `std::string_view::size()` to `int`.**
   `sqlite3_bind_text` takes a signed-int length. If the bound text exceeds
   `INT_MAX` bytes, the `static_cast<int>` silently truncates and we hand
   SQLite a short prefix while the caller believes the full value bound.
   Fixed by rejecting `value.size() > INT_MAX` with an explicit
   `invalid_argument` error. (See `sqlite3_bind_text64` for the 64-bit
   variant — out of scope for this commit.)

#### Medium — recorded, not fixed in this commit

4. **`config.cpp::substitute_env` recurses on JSON depth without a hard
   limit.** nlohmann's parser caps default stack depth, so the practical
   risk is low, but a deeply nested object still walks proportional stack.
   Worth tightening once the secret/state slice lands.

5. **TOCTOU between `ensure_readable_regular_file` and `std::ifstream` in
   `oran-io/file.cpp`.** A symlink swap between the stat and the open lets a
   crafted workspace point the read at an unrelated file. Mitigated today
   by the permission layer above; revisit when sandbox enforcement lands
   (the planned workspace-jail described in `docs/SECURITY.md`).

6. **`load_migrations_from_directory` follows symlinks via
   `entry.is_regular_file`.** Trusted-repo input today, but a symlink to
   `/etc/...` would be silently picked up if the migrations dir ever moved
   under user control.

#### Low — informational

7. **`expand_env_string` accepts unbounded expansion output.** Env is
   trusted; noted for completeness.

8. **`Connection::execute` allocates a fresh `std::string` per call to
   null-terminate.** Performance, not security.

9. **`StatementCache::clear()` resets hit/miss/eviction counters together
   with the entries.** Behaviour, not security.

#### Confirmed safe

- All SQLite query paths use `prepare` + bind; no string concatenation into
  SQL. `execute` is only called with hardcoded `PRAGMA` / `BEGIN` / `COMMIT`
  / `ROLLBACK` SQL or with caller-supplied migration text that already went
  through validation.
- `Defaults::for_mode` for an out-of-range `Mode` cast falls through to an
  empty `RuleSet` and the mode-default verdict, which is `deny` for the
  out-of-range enum value — fails safe.
- Permission evaluator scans deny → allow → ask, so a baseline `deny` for
  `runtime_loader` / `delete_path` cannot be widened by a downstream layer
  silently. Layering order is correct.

### Design Intent

Bound everything that reads a file with a number the operator can audit.
The system has no defensible "should be small" assumption: a 100 GB read
attempt is a crash that doesn't even reach the permission layer. The cap
sits at the file-size check, before any allocation, so the bad path is
constant memory.

### Files Modified

- `include/oran/config/config.hpp` — added `LoadOptions::max_bytes`.
- `src/oran-config/config.cpp` — size cap + pre-allocate read in `load_file`.
- `src/oran-storage/migrations.cpp` — size cap + pre-allocate read in
  `load_migration_file`.
- `src/oran-storage/sqlite.cpp` — reject `bind_text` payloads over `INT_MAX`.
- `docs/histories/2026-05/20260516-2200-security-review.md` — this file.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/histories/2026-05/20260516-2200-security-review.md` — review record.

`docs/SECURITY.md` already documents the secure-defaults posture this change
hardens; no edit required because the document does not enumerate file-size
bounds (they remain an implementation detail of the loader). The
`LoadOptions::max_bytes` knob is reachable via the public header and
self-documenting; no separate config-doc edit is implied.

### Validation

- Commands run: manual review across `src/oran-*` and `include/oran/**`;
  cross-referenced the threat surface to the docs in `docs/rules/` and
  `docs/SECURITY.md`.
- Tests added/changed: none in this commit — the existing storage and
  config buckets cover happy-path behavior; size-limit tests land in the
  follow-up history when the loader API surface grows past hard-coded
  defaults.
- Bench impact: none (paths are startup-only and remain O(file size)).
- Compile-budget delta: none — additions are local constants and a single
  branch per loader.

### Follow-ups

- Issues to file: `medium-4` (env-substitution depth cap), `medium-5`
  (symlink TOCTOU), `medium-6` (migration symlink-following).
- Tech-debt entry: consider switching `Statement::bind_text` to
  `sqlite3_bind_text64` if a real >2GiB payload ever shows up.
- Linked release note: none (pre-release).
