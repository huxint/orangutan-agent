# Provider Route Profiles

Slice 103 adds the provider-side route resolution shape needed before real adapter
construction can land. Slice 98/102 gave the loop a `provider::Route` with resolved
profile/model/protocol targets, but an adapter factory also needs the source profile's
provider label, base URL, and API-key environment-variable name. This slice keeps that
metadata beside each resolved `ModelTarget` through `ResolvedProfileTarget` and
`RouteProfileResolution`, while `RouteProfileResolution::route()` derives the existing
loop-facing `Route` for current `agent::Loop` and `provider::execution::Runtime`
callers.

The design deliberately keeps the resolver non-secret and non-networked: it preserves
the configured `api_key_env` string, but it does not read environment variables,
decrypt credentials, construct adapters, or send provider requests. `bootstrap::run`
now preflights this richer bundle for the configured `default` route so the future
adapter factory consumes the same validated route/profile/protocol data path as the
startup summary, while ordinary binary prompts still use the deterministic no-runner
CLI path.

Release note: `docs/releases/feature-release-notes.md` documents the route-profile
resolution companion surface.

Focused validation:

- `xmake run test-provider` (28 cases / 210 assertions)

Files of interest:

- `include/oran/provider/route_resolver.hpp`
- `src/oran-provider/route_resolver.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/provider/test_route_resolver.cpp`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0001-core-react-loop.md`
- `docs/STATUS.md`
