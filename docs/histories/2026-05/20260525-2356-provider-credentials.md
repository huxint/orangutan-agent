# Provider Credentials

Slice 105 adds the explicit provider credential-resolution boundary that sits
between the offline adapter plan and future concrete provider factories.
`<oran/provider/credentials.hpp>` exports `provider::AdapterCredentialTarget`,
`provider::AdapterCredentialBundle`, and
`provider::resolve_adapter_credentials(plan)`. The resolver reads the API-key
environment variables named by each `AdapterConstructionTarget`, stores the
secret values only in the returned in-memory bundle, derives the same
loop-facing `provider::Route`, and reports missing or empty keys as
`ErrorKind::auth` with only non-secret context (`role`, `profile`,
`api_key_env`).

The design intent is to make the secret-read boundary explicit before adding
HTTP transports or real Anthropic/OpenAI protocol adapters. Regular
`bootstrap::run` still does not call this resolver: startup continues to
preflight route/profile/adapter metadata without reading provider credentials,
constructing an adapter, sending network traffic, or starting `agent::Loop` for
ordinary prompts.

Release note: `docs/releases/feature-release-notes.md` documents the credential
resolver surface and the unchanged bootstrap no-secret behavior.

Focused validation:

- `xmake run test-provider` (36 cases / 259 assertions)

Files of interest:

- `include/oran/provider/credentials.hpp`
- `src/oran-provider/credentials.cpp`
- `include/oran/provider.hpp`
- `tests/provider/test_credentials.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/STATUS.md`
