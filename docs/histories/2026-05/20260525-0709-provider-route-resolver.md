# Provider Route Resolver

The provider execution runtime from slice 97 still needed a config boundary
before the loop or binary could construct real routes. This slice adds
`provider::resolve_route(const config::Config&, std::string_view)`, which turns
typed `profiles` / `routes` config into the existing `provider::Route` value
without teaching adapters about provider-name strings.

The resolver stays intentionally narrow: it preserves configured fallback order,
maps current provider aliases and exact `ProtocolKind` spellings, and reports
config errors with route/profile/role context when a route references an
unknown profile or provider spelling. Policy fields that the typed config does
not yet expose, such as thinking budget and prompt-cache options, are left
unset rather than guessed.

Files of interest:

- `include/oran/provider/route_resolver.hpp`
- `src/oran-provider/route_resolver.cpp`
- `tests/provider/test_route_resolver.cpp`
- `docs/design-docs/api-portability.md`
- `docs/STATUS.md`
