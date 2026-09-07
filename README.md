# Orangutan

A C++26 agent runtime for applications that need model calls, permission-checked
tools and persistent conversation memory.

The libraries run agent turns through Anthropic Messages or OpenAI Responses,
save completed turns atomically, and delegate bounded tasks to
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

## Embed

Link `oran-bootstrap` and include `<oran/bootstrap.hpp>`. Compose the runtime
through three interfaces:

| Interface | Responsibility |
| --- | --- |
| `HttpProviderBackend` | Construct a provider route and HTTP transport from explicit configuration. |
| `RuntimeAssembly` | Own workspace, permissions, hooks and storage services. |
| `AgentSession` | Run prompts, recall scoped memory, dispatch tools and persist completed turns. |

The host supplies executors, workspace, session identity and resource lifetimes.
Await `AgentSession::run_prompt` before releasing its services. The
[composition contract](docs/design-docs/bootstrap-runtime.md) describes ownership;
the [HTTP continuation test](tests/bootstrap/test_provider_backend.cpp) exercises
the complete setup against a controlled provider.

[config.example.json](config.example.json) shows provider and permission settings.
Real providers require the named API-key environment variable; the test suite
uses local fixtures. See [configuration](docs/design-docs/secrets-and-state.md).

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
