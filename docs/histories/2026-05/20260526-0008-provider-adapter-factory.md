# Provider Adapter Factory

Slice 106 adds the provider-owned adapter factory dispatch seam that sits after
offline adapter planning and explicit credential resolution. New
`<oran/provider/adapter_factory.hpp>` exports
`provider::ProtocolAdapterFactory`, `ProtocolAdapterFactoryBinding`, and
`make_adapter_system(credentials, factories)`. The factory matches each
primary/fallback credential target's adapter-family name to a caller-registered
protocol factory, builds one backend per route profile, and returns a
profile-routed `provider::System` that expects the execution layer to pass one
selected target per call. It rejects malformed binding tables, duplicate route
profiles, null factory results, factory errors, unknown routed profiles, and
accidental multi-target routes as config errors.

The design intent is to finish the construction boundary before introducing
HTTP transport or vendor protocol adapters. Retry and fallback stay in
`provider::execution::Runtime`; concrete protocol factories only own the
single-target backend for their wire format. Regular `bootstrap::run` still
does not call the credential resolver or this factory, so ordinary startup
keeps the current no-secret/no-network preflight behavior until concrete
Anthropic/OpenAI factories and transport exist.

Release note: `docs/releases/feature-release-notes.md` documents the factory
seam and unchanged bootstrap behavior.

Focused validation:

- `xmake run test-provider` (45 cases / 329 assertions)

Files of interest:

- `include/oran/provider/adapter_factory.hpp`
- `src/oran-provider/adapter_factory.cpp`
- `include/oran/provider.hpp`
- `tests/provider/test_adapter_factory.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/STATUS.md`
