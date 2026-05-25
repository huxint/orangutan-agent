# Provider Adapter Plan

Slice 104 adds the last offline seam before real provider adapter construction. Slice
103 preserved route-profile endpoint metadata, but a concrete factory still needs one
validated construction target per primary/fallback profile plus the protocol adapter
family it should dispatch to. This slice introduces
`provider::AdapterConstructionTarget`, `provider::AdapterConstructionPlan`, and
`provider::make_adapter_construction_plan(resolution)`, which keep each
`ResolvedProfileTarget` beside its `ProtocolKind` reflection spelling, derive the
existing loop-facing `provider::Route`, and preflight the endpoint fields needed by
the future factory.

The plan remains deliberately non-secret and non-networked: it validates non-empty
profile/model/provider/base-url/API-key-env metadata and `http://` / `https://`
endpoint schemes, but it does not read environment variables, decrypt credentials,
allocate an HTTP client, construct an adapter, send a provider request, or start
`agent::Loop` from the ordinary binary path. `bootstrap::run` now resolves the
configured `default` route profiles and builds this offline plan before CLI handoff,
so the binary catches malformed adapter endpoint metadata at startup while preserving
the existing non-secret route summary.

Release note: `docs/releases/feature-release-notes.md` documents the offline adapter
construction plan surface.

Focused validation:

- `xmake run test-provider` (32 cases / 233 assertions)
- `xmake run test-bootstrap` (65 cases / 269 assertions)

Files of interest:

- `include/oran/provider/adapter_plan.hpp`
- `src/oran-provider/adapter_plan.cpp`
- `include/oran/provider.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/provider/test_adapter_plan.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0001-core-react-loop.md`
- `docs/STATUS.md`
