# Tech Debt Tracker

Track known debt that is real enough to preserve but not urgent enough to block the
current task.

Order rows by *date discovered* (newest at the top). Add a row in the same PR that
created the debt.

| Date discovered | Area | Debt | Why It Exists | Planned Follow-Up |
| --------------- | ---- | ---- | ------------- | ------------------ |
| 2026-05-17 | storage / bootstrap | Audit migrations directory is found by walking up from `CWD` until `src/oran-storage/migrations/audit` appears — running `orangutan` (or `bootstrap::RuntimeAssembly::build` with `audit_enabled=true`) from outside the repo fails to migrate the audit DB. `bootstrap::run` therefore defaults to `audit_enabled=false`; `--audit-init` keeps working because xmake's `set_rundir` pins the project root. | Migration assets are still loose `.sql` files, not packaged into the binary. The runtime cannot read them once installed system-wide. | Embed the migration SQL strings into `oran-storage` (consteval `string_view` table or `xmake.lua`-generated header) and let `AuditRepository` consume the embedded copy when no `migrations_directory` is configured. Tracked alongside the broader "packaged migration asset lookup" debt called out in the `QUALITY_SCORE.md` Storage row. |
| 2026-05-17 | prompt | `scripts/check-prompt-preamble` static grep promised in [`rules/prompt-design.md`](../rules/prompt-design.md) "Enforcement" is not yet implemented. | The prompt builder does not exist yet; the grep would have no preamble to scan. | Author when the first stable preamble template lands in `oran-agent`; then list under "Mechanical Enforcement" in [`rules/docs-in-sync.md`](../rules/docs-in-sync.md). |
| 2026-05-17 | prompt | `bench/oran-agent/prompt_cache_hit_rate.cpp` regression scenario promised in [`rules/prompt-design.md`](../rules/prompt-design.md) "Enforcement" is not yet implemented. | The `oran-agent` library does not exist yet; the bench bucket and the fixture have no host. | Ship alongside `oran-agent` slice 1 (system preamble builder). Fixture shape lives in [`product-specs/0010-benchmark-harness.md`](../product-specs/0010-benchmark-harness.md) once written. |
| 2026-05-14 | docs | Build skeleton scripts (`scripts/check-deps.sh`, `scripts/check-includes.sh`, `scripts/measure-tu.sh`, `scripts/check-compile-budget.sh`) referenced from rules but not yet implemented. | Framework lands before code; scripts will be authored alongside the first C++ slice. | Implement when MVP loop lands (spec 0001). |
| 2026-05-14 | docs | Generated `docs/generated/config.schema.json` is not yet implemented. | The first config slice ships a typed loader and checked `config.example.json`; schema generation waits for broader config section models. | Generate from `oran-config` C++ types via build step. |
| 2026-05-14 | bench | A-vs-B scenarios listed in `bench/<lib>/README.md` are placeholders. | Bench code follows each library's MVP. | Land with the corresponding library's first PR. |
| 2026-05-14 | docs/web | Frontend stack choice (Preact vs. plain JS) not yet decided. | Defer until the first useful UI flow. | Decide in `docs/exec-plans/active/<date>-web-mvp.md`. |
