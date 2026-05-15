# bench-config

`bench-config` measures the first `oran-config` loading surface.

Current A-vs-B comparison:

- `config.parse_memory`: parses an in-memory JSON document through `Config::parse`.
- `config.load_file_example`: reads and parses the checked-in `config.example.json`
  through `Config::load_file`.
