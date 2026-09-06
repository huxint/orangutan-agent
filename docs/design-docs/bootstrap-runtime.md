# Runtime Composition

`oran-bootstrap` constructs services and drives a session. The executable calls
`run_application(ApplicationOptions)` from `<oran/bootstrap/application.hpp>`.
The host requires a configuration file and prompt; workspace, state directory,
session ID and agent selector are explicit options.

## Resource Ownership

The host creates the Asio runtime, provider transport, runtime assembly and
session in that order. The session runs on a strand. Blocking filesystem, HTTP
and memory operations use the runtime's worker executor. The host awaits the
turn, including tool cleanup, before destroying borrowed services.

`RuntimeAssembly` owns the workspace resolver, approval broker, hook bus, audit
sink and optional session/long-term repositories. Storage paths supplied by the
host select `audit.db`, `sessions.db` and `memory.db` within the state directory.
The host holds a directory lock to prevent two command processes from owning
that state simultaneously. Existing databases use versioned migrations.

`HttpProviderBackend` owns HTTP transport, protocol factories and the resolved
provider route. Credentials are resolved only after route validation. Missing
configuration or credentials is an error before execution.

## Session Boundary

`AgentSession` loads bounded history, prepares an owned conversation through
`agent::prepare_conversation`, drives `agent::Loop`, and appends the successful
transcript suffix atomically through `Store::append_all`. The prepared value
records the history boundary independently of storage. Prompt recall runs once through `MemoryRecall` before the loop;
the returned text stays stable across model/tool iterations.

Memory adapters borrow a runtime and backend and capture the host's scope. They
do not capture session state or discover configuration. Filesystem and catalogue
tools are registered together; the session adds memory tools when memory services
exist. Every turn joins its borrowed tool context before returning or persisting.

All callers use the same rules, broker, workspace, hook and audit services.
Without an approval consumer, an `ask` decision fails closed. Embedders may bind
an explicit approval sink through the hook bus.

## Invocation

```sh
xmake run orangutan -- --config config.example.json --prompt "Read README.md"
```

The executable writes the answer to stdout and the session ID to stderr. Supply
that ID with `--session` to continue; `--agent` selects an entry in `agents`.
The default state directory is `<workspace>/.orangutan`; `--state` selects an
explicit alternative. The default workspace is the current directory.

SIGINT/SIGTERM requests cancellation. Tool dispatches must release their borrowed
context before the runtime stops. A cancelled result is an error, never a
successful partial response.
