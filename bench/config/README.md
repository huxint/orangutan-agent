# bench-config

`bench-config` measures the `oran-config` loading and typed-section
parse surface.

## Scenarios

### `bench-config` block

- `config.parse_memory`: parses an in-memory JSON document through
  `Config::parse`.
- `config.load_file_example`: reads and parses the checked-in
  `config.example.json` through `Config::load_file`.

### `bench-config/permissions` block

A-vs-B coverage for the `permissions` typed surface added in the
layer-2/3 wiring slice:

- `config.parse_permissions_empty`: parses a config that carries an
  empty `permissions: {}` block. Documents the lift of having the
  typed surface plumbed without any rules to resolve.
- `config.parse_permissions_typed`: parses a config with 16 mixed-
  scope permission rules and one `agents.researcher.permissions`
  overlay. This is the cost the future
  `oran-permission::materialize` consumer pays at startup.
- `config.parse_permissions_with_input_patterns`: parses a 14-rule
  block where four `deny` rules carry an `input_pattern` re2 source
  pattern. Documents the per-pattern re2 compile cost the load-time
  validator pays (re2 compile + discard per pattern; the runtime
  matcher in `oran-permission::materialize` recompiles).
