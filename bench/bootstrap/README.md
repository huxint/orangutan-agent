# bench-bootstrap

`bench-bootstrap` measures the early bootstrap config path.

Current A-vs-B comparison:

- `bootstrap.config_missing_default`: resolves `<workspace>/.orangutan/config.json`
  and falls back to built-in config defaults when the default file is absent.
- `bootstrap.config_explicit_file`: parses `--config <path>` and loads a checked test
  config file through `oran-config`.
