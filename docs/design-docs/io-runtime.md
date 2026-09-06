# Filesystem Runtime

`oran-io` owns filesystem effects and blocking-operation cancellation. Tool policy
is evaluated above it; authorized directory/file handles carry the effect into
this boundary. Diagnostic path strings are never authority.

## Authority

`DirectoryAuthority` pins a root directory. Relative traversal rejects escapes
and unintended symlinks. File read/list/write/edit/delete operations retain
appropriate handles across asynchronous work. Mutation requests pin their target
and compare identity/version before changing it, so approval does not authorize
a replacement inode introduced while the caller waits.

Workspace roots and extra read/write roots are explicit. Reads use read authority;
mutations require write authority. An outside-root override must pass dispatch
approval and keep its selected authority throughout the operation.

`PrivateDirectory` creates/opens an owned private directory, rejects unsafe file
reads, writes mode-0600 files atomically with fsync, and provides exclusive locks.
The application uses it for state ownership. It does not encrypt credentials.

## Reads And Caches

Text reads validate UTF-8 and enforce byte/range bounds. File fingerprints and
version tokens support conflict detection. Range caches are bounded and keyed by
file identity/version and range; coalesced readers must observe the same result
or error. Watcher support is a caller-owned cache invalidation primitive; the
minimal executable does not run a background watcher.

Blocking work runs on the supplied worker executor through `run_blocking`.
Cancellation signals the operation and waits for safe resource cleanup before
returning. A coroutine name alone does not make synchronous IO nonblocking.

## Verification

Tests exercise symlink/path escape rejection, target replacement, private-file
checks, UTF-8/range limits, cancellation and concurrent cache readers. Outstanding
cross-executor singleflight coverage is tracked in
[live debt](../exec-plans/tech-debt-tracker.md).
