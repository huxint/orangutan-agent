# Bootstrap Provider Route Preflight

Slice 98 made provider route resolution a provider-owned API, but the binary still
loaded config and moved straight to the pre-loop CLI shell without consuming that
boundary. This slice wires the first bootstrap handoff step: regular startup now
preflights the configured `default` provider route whenever config declares routes,
prints the resolved primary/fallback summary, and returns the resolver's structured
config error before CLI handoff if profile references or provider spellings are bad.

The slice deliberately stops before constructing any provider adapter, reading
credentials, or running `agent::Loop`. Built-in empty defaults still report no
provider route and continue to the deterministic CLI shell, preserving fresh-checkout
startup.

Release note: `docs/releases/feature-release-notes.md` documents the user-visible
startup preflight. Validation: `xmake run test-bootstrap`, `xmake run orangutan --
--help`, `make ci`, and `git diff --check`.

Files of interest:

- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `xmake/targets.lua`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/STATUS.md`
