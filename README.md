# Orangutan

A C++26 agent runtime for applications that need model calls, permission-checked
tools and persistent conversation memory.

The current executable runs one agent turn through Anthropic Messages or OpenAI
Responses, saves completed turns atomically, and delegates bounded tasks to
configured child agents. Children keep independent conversations and execute
under the intersection of parent and child permissions; see the
[agent contract](docs/design-docs/agent-platform.md).

## Build

Requires Linux, GCC 16.1 or later, xmake 3.1.1 and system libcurl development files.
Xmake resolves the remaining [dependencies](docs/rules/libraries.md).

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
```

## Run

Set the provider model and API-key environment variable referenced by
[config.example.json](config.example.json), then run:

```sh
xmake run orangutan -- --config config.example.json --prompt "Read README.md and describe this project."
```

The answer goes to stdout; the session ID goes to stderr. Continue with
`--session <id>`. `--workspace` selects filesystem scope and `--state` selects
persistent storage; defaults are the current directory and its `.orangutan`
subdirectory. The executable performs real provider calls and requires valid
credentials. See [configuration](docs/design-docs/secrets-and-state.md).

Tool calls pass through input validation, workspace policy, permission decisions
and audit. An operation requiring approval fails closed when the embedding has
no approval handler. Authorize only the operations needed by your task.
The filesystem tools are FileRead, FileWrite and FileEdit. `AgentRun` selects a
child from `agents` and requires `spawn_agent` authority; it admits up to four
child runs per prompt and one generation of delegation.

## Development

Read [CLAUDE.md](CLAUDE.md) and [STATUS.md](docs/STATUS.md). Run `make ci` alongside
C++ build/tests before committing. [Architecture](docs/ARCHITECTURE.md) owns
module boundaries; [live debt](docs/exec-plans/tech-debt-tracker.md) records
remaining reliability and verification work.

No redistribution license is currently provided in this repository.
