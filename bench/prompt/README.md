# bench-prompt

`bench-prompt` measures the first prompt-builder assembly path.

## Scenarios

### `bench-prompt/catalog_sections`

- `prompt.build_default_active_set`: renders a representative catalog using the
  documented default active-tool selector.
- `prompt.build_explicit_subset`: renders the same catalog with an explicit two-tool
  active allowlist, moving the rest into the deferred index.

This is a startup/turn-assembly sanity bench rather than a cache-hit-rate regression.
The cache-hit-rate fixture remains tied to `oran-agent` once session promotion and the
fake-provider loop exist.
