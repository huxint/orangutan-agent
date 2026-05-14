# Static Analysis

GCC 16.1 ships a mature `-fanalyzer` and a wide set of `-Wanalyzer-*` warnings. This
rule defines **when** the analyzer runs, **what** it flags as a hard failure, and
**how** to suppress a false positive without weakening the safety net for the rest of
the codebase.

> Background: in legacy `orangutan/` the analyzer was never wired into CI, so the
> codebase accumulated `null-dereference`, `use-after-free`, and `tainted-allocation`
> shaped bugs that surfaced only in production. v2 ships analyzer wiring on day one,
> opt-in by build mode, opt-in to **harden** specific TUs.

## Modes

| Mode               | xmake invocation                            | When           | Warnings → errors? |
| ------------------ | -------------------------------------------- | -------------- | ------------------ |
| Off (default)      | `xmake f -m release`                         | Local devel.   | n/a                |
| Analyze (manual)   | `xmake f -m release --analyze=y`             | Author audit.  | yes                |
| CI Hardening       | `xmake f -m release --analyze=y --hardened=y`| Nightly CI.    | yes                |

The `--analyze=y` option enables `-fanalyzer` and the analyzer-specific warning
escalation. `--hardened=y` (already documented in
[`../BUILD_SYSTEM.md`](../BUILD_SYSTEM.md)) layers on `_FORTIFY_SOURCE=3`,
`-fstack-protector-strong`, `-fcf-protection`, `-fstack-clash-protection`.

Slice 0 ships the option; CI wiring lands when `oran-async` does (analyzer is most
valuable on the parts that handle pointers and ownership, and the agent loop is the
first such target).

## Required Warnings (Hard Failures)

The analyzer escalates these to errors via `-Werror=` when `--analyze=y` is on:

- `-Wanalyzer-null-dereference`
- `-Wanalyzer-use-after-free`
- `-Wanalyzer-double-free`
- `-Wanalyzer-malloc-leak`
- `-Wanalyzer-tainted-allocation-size`
- `-Wanalyzer-tainted-array-index`
- `-Wanalyzer-out-of-bounds`
- `-Wanalyzer-write-to-string-literal`
- `-Wanalyzer-fd-leak`
- `-Wanalyzer-fd-use-without-check`

The full set is in `xmake/toolchain.lua`'s `oran-gcc.on_load`. Add a warning to the
list only with rationale in the PR (and in `docs/exec-plans/tech-debt-tracker.md` if
the addition would be retroactively painful).

## Suppression Rules

- **No file-wide pragmas.** They hide real bugs in code that didn't trigger the
  initial false positive.
- **Function-local pragma is the unit of suppression**:
  ```cpp
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
  // narrow scope: only the call below confuses the analyzer (see explanation).
  Bar* b = third_party_take_ownership(make());
  #pragma GCC diagnostic pop
  ```
- The pragma **must** be preceded by a one-line comment explaining why the
  analyzer is wrong here (an external-ownership transfer, a known GCC bug with a link,
  …). The comment is the contract the next agent reads.
- Reviewers reject suppressions without that comment.

## When To Add Analyzer Coverage

- Any TU that handles raw memory (smart pointers don't need it; `make_unique` paths
  are already safe), file descriptors, sockets, or libc handles.
- Any TU that does parsing on untrusted bytes (`oran-config`, JSON loading paths in
  `oran-provider`, `oran-channel-*` inbound).
- Any TU that does subprocess plumbing (`oran-io`'s spawn paths).

The TU's library README enumerates which TUs are analyzer-clean and which are
analyzer-exempt and why.

## Performance Implications

`-fanalyzer` adds **roughly 2-3×** to a TU's compile time on the heaviest
`oran-agent` / `oran-provider` files. That breaks the compile budget in
[`compile-budget.md`](compile-budget.md) if it ran on every TU on every build. The
two-step workflow (default off, opt-in for audits and nightly CI) is the documented
compromise.

If the analyzer + the project's own compile budget come into conflict for a
particular TU, the **right** fix is to split the TU (per
[`compile-budget.md`](compile-budget.md) "When You Hit The Budget") rather than to
disable the analyzer.

## Interaction With clang-tidy

- clang-tidy runs on every CI build and enforces `core-guidelines-*`,
  `cppcoreguidelines-*`, and our project-specific checks
  ([`critical-rules.md#C9`](critical-rules.md)).
- The analyzer covers a different shape of bug (path-sensitive dataflow). Both are
  required; one does not substitute for the other.
- Where clang-tidy and `-fanalyzer` overlap (e.g., null-deref), the analyzer's
  verdict wins because it is path-sensitive.

## Enforcement

- `xmake f --analyze=y` exits non-zero if any required warning fires.
- `scripts/check-analyzer-coverage.sh` (TBD; lands with the first lib that opts in)
  parses each `src/oran-*/README.md` and asserts that the TU list there matches
  reality.

## See Also

- [`critical-rules.md`](critical-rules.md) — analyzer rule line.
- [`compile-budget.md`](compile-budget.md) — why analyzer is opt-in.
- [`../BUILD_SYSTEM.md`](../BUILD_SYSTEM.md) — `--analyze=y` and `--hardened=y`
  invocation.
- [GCC 16.1 analyzer manual](https://gcc.gnu.org/onlinedocs/gcc/Static-Analyzer-Options.html)
