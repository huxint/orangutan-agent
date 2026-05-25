# Provider Profile Protocol

Slice 102 closes a small but important route-resolution gap before real provider
adapter construction lands. The slice-98 resolver could infer a wire protocol from a
profile's `provider` label, but that forced self-hosted gateways and custom vendor
labels to masquerade as a built-in provider spelling. This slice adds optional
`profiles.<name>.protocol` to the typed config surface and makes
`provider::resolve_route` prefer that exact `ProtocolKind` spelling before falling
back to the existing provider-alias table.

The design keeps `provider` as the operator/vendor label for future adapter factories,
credentials, and base URL lookup, while `protocol` names the wire family the adapter
will use. `oran-config` validates the field only as a non-empty string so the config
library does not depend upward on `oran-provider`; the provider-side resolver owns the
`ProtocolKind` parse and reports unknown explicit protocols with route/profile/role
context during the existing bootstrap route preflight. No credentials are read, no real
adapter is constructed, and ordinary binary prompts still use the deterministic
no-runner CLI path.

Release note: `docs/releases/feature-release-notes.md` documents the profile protocol
config contract.
Focused validation:

- `xmake run test-config` (33 cases / 241 assertions)
- `xmake run test-provider` (26 cases / 181 assertions)
- `xmake run test-bootstrap` (64 cases / 264 assertions)

Files of interest:

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/provider/route_resolver.hpp`
- `src/oran-provider/route_resolver.cpp`
- `config.example.json`
- `tests/config/test_config.cpp`
- `tests/provider/test_route_resolver.cpp`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/STATUS.md`
