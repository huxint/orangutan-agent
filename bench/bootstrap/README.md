# bench-bootstrap

`bench-bootstrap` measures the early bootstrap config path.

Current A-vs-B comparison:

- `bootstrap.config_missing_default`: resolves `<workspace>/.orangutan/config.json`
  and falls back to built-in config defaults when the default file is absent.
- `bootstrap.config_explicit_file`: parses `--config <path>` and loads a checked test
  config file through `oran-config`.
- `bootstrap.signal_drain_with_scope` vs. `bootstrap.signal_drain_bare`: the per-drain
  cost of installing `SignalScope` (SIGINT/SIGTERM trap) on a one-shot `io_context`
  against the bare `io.run()` baseline over the same 8-post workload. Quantifies the
  `signal_set` install + `release()` overhead the `--audit-init` path now pays per
  invocation.
- `bootstrap.assembly_build_with_audit` vs. `bootstrap.assembly_build_without_audit`:
  the per-build cost of provisioning the storage-backed audit pipeline.
