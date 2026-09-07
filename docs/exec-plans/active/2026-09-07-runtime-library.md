# Runtime Library Boundary

## Objective

Expose the provider/tool/memory loop through C++ libraries. Applications own
process arguments, terminal output, signals and runtime resources.

## Current Contracts

- `RuntimeAssembly`, `HttpProviderBackend` and `AgentSession` already compose the
  configured loop through explicit resources and options.
- `src/main.cpp` and `run_application` currently add process-level argument,
  output, signal and state-directory policy.
- The controlled HTTP continuation test currently calls `run_application`.
- Session, memory and audit schemas and existing user data remain intact.

## Complete Slice

1. Delete the executable entry point, application wrapper and its public header;
   remove the production binary target and unused packaging stub.
2. Drive the controlled HTTP continuation regression through the library
   composition APIs. Keep provider, persistence, permission and child-agent
   behavior covered by the existing integration tests.
3. Update build inventory, configuration/state ownership, README, developer
   routing and reliability documentation around the library boundary. Remove
   obsolete executable budgets and command references.
4. Verify the default library build, affected tests, all release test targets and
   `make ci`. Prove the rewritten continuation test detects lost history in an
   isolated copy and run its instrumented lifetime check.

## Completion

- Production builds contain the runtime libraries; tests and benchmarks retain
  their own runners.
- Applications supply executors, configuration, identities and resource
  lifetimes through the existing bootstrap interfaces.
- Controlled HTTP continuation passes without a command-line/application host.
- Published contracts and generated local build artifacts agree with the source
  inventory. Database files and stored history are preserved.

## Progress

- [x] Trace executable, wrapper, build and documentation consumers.
- [ ] Remove process hosting and migrate the integration caller.
- [ ] Verify the library boundary and update owning documents.
- [ ] Delete the completed plan and commit.
