# Tests And Benchmarks

Every library has a `tests/<lib>` Catch2 bucket and a `bench/<lib>` nanobench
bucket. Xmake names them `test-<lib>` and `bench-<lib>`.

Test public behavior: successful results, errors, authorization, limits,
state transitions, cancellation and storage integrity. Use controlled providers
and transports while retaining the meaningful runtime logic. Tests of deleted
application behavior leave with their callers; retain shared-core regressions.

Async tests use a real executor, explicit synchronization and a hard timeout.
Assign awaited results to locals before Catch assertions. Keep independent
scenarios separate. New or changed tests must fail on the defect they guard.

Run affected build/tests during a slice and `make ci` before a commit. Broad
runtime refactors also run the complete release suite. Debug ASan/UBSan checks
are required for lifetime work. [CICD](../CICD.md) owns hosted gate definitions.

Use existing build and test targets in the working tree for routine verification.
Reusable checks belong in `tests/` or `scripts/`. Separate validation workspaces,
fault-injection campaigns, custom scripts and retained reports are not required
for each change. Additional diagnostics address a concrete failure or an explicit
investigation; clean up their temporary files when finished.

Bench when a meaningful design or performance choice needs evidence. Compare
reasonable alternatives under the same workload; prefer the simpler solution
when measurements do not justify complexity. Report compiler, mode and hardware
limits. [compile-budget](compile-budget.md) owns compile-time thresholds.

```sh
xmake build test-tool
xmake run test-tool
xmake build bench-tool
xmake run bench-tool
```

## Working-Context Evaluation

`eval-context` is an opt-in executable, separate from `xmake test`. It uses
synthetic task transcripts to compare full-history baselines with compacted
sessions, then closes/reopens their databases and probes again after more context
pressure. Cases cover goals/constraints, superseded decisions, completed versus
pending work, and a model-directed read of a large fixed diagnostic tool result.
The tool has no project filesystem or subprocess effects. Each run uses a new
private output directory, independent identities and disabled long-term memory.

```sh
xmake build -j4 eval-context
build/linux/x86_64/release/eval-context --self-test --output /tmp/oran-context-check
build/linux/x86_64/release/eval-context --config eval/context/deepseek.example.json --output /tmp/oran-context-live --repeat 2
```

`--self-test` exercises plumbing with a provider that knows the fixture answers;
it is explicitly labelled `controlled_plumbing_check`, not model evidence. Live
runs require an explicit provider config and its named credential environment
variable. The DeepSeek example uses `ORAN_CONTEXT_EVAL_KEY` and the documented
[Anthropic endpoint](https://api-docs.deepseek.com/guides/anthropic_api). It carries
peak USD [pricing](https://api-docs.deepseek.com/quick_start/pricing) observed on
2026-09-28; refresh rates for current billing. Cost values are configured estimates,
not invoices, and remain null when pricing is unavailable.

The runner accepts `--case`, `--repeat` (1–5), `--max-calls` (1–512),
`--context-tokens` (8192–32768), and `--deadline-seconds` (1–3600). Defaults are all
cases, one repeat, 192 attempts, a 16384-token context budget, 2048 summary bytes,
and a 900-second overall deadline. HTTP requests time out after 60 seconds;
SIGINT/SIGTERM and the deadline request cancellation and await runtime cleanup.
The 262144-token baseline budget avoids compaction for these bounded fixtures;
select a deployment model that can accept their full history. Token counting uses
the runtime's conservative byte estimator, so this is synthetic context pressure,
not a calibration of the model's physical window.

`report.json` records each phase's exact-field answer checks, actual checkpoint
coverage/revisions, tool calls, provider attempts, served models, input/output/cache
tokens, estimated cost and elapsed time. Per-call metadata records summary versus
answer, stop reason, text/thinking byte counts and output tokens, without response
bodies; local context-budget failures include their runtime diagnostic. Arrays grade independent of order; wrong
labels, missing constraints, obsolete decisions and invented completions fail.
Compacted phases must actually advance checkpoints (at least twice in the first
phase); baseline phases must remain uncompacted. A paired regression means a
baseline phase passed and its compacted counterpart failed. Provider errors and
incomplete runs remain visible rather than counting as successful retention.
Account/authentication failures (HTTP 401/402/403) stop further calls. Reports
include the numeric HTTP status but no upstream error body. Operational failures
are counted separately and never scored as malformed model answers.
Original fixture/transcript rows and traces remain in each case's private database
for inspection. Raw provider errors, headers and credential values are not put in
the report. Exit codes are 0 for a complete pass, 1 for failed/incomplete results,
and 2 for invalid setup.

These fixed tasks isolate retention and status errors; they do not establish
arbitrary coding-task success or spontaneous long-term-memory behavior. A strict
field mismatch can be a paraphrase rather than forgotten information: inspect
`grading.answer` and individual checks. Extra inspection logs in completed/pending
fields are task-state contamination, while `false_completion` specifically means
that an explicitly pending task label was reported as completed. Repeat
with the deployment model and inspect failed answers and checkpoints before
changing memory policy. Grader negatives, budget exhaustion and controlled
end-to-end execution are covered by `test-bootstrap`.


## QQ Live Dialogue Evaluation

`eval-qq` processes one authenticated QQ event supplied by a trusted local ingress
harness. It is opt-in and does not connect a Gateway or start public ingress.
The current Tencent `qqbot-nodejs` SDK supports a WebSocket Gateway; a live
experiment can authenticate there and feed C2C events to this runner without
adding a WebSocket dependency to the runtime. The bootstrap webhook API remains
independent of this ingress choice.

```sh
xmake build -j4 eval-qq
python3 tests/eval/test_qq.py build/linux/x86_64/release/eval-qq
build/linux/x86_64/release/eval-qq "$CONFIG" "$WORKSPACE" "$QQ_STATE_DIR"
```

The private state directory must already contain `qq-credentials.json` from
`oran-qq-login` and the trusted harness's `event.json` (at most 1 MiB). The event
file is not independently authenticated; never feed untrusted local files or
public HTTP bodies to this entry point. Only direct text from the saved scanning
user is admitted. Missing owner identity fails closed. State must be outside the
workspace; extra filesystem roots are refused. Provider credentials use the
configuration's ordinary environment references. The runner grants no typing
status requests and disables child delegation; model/tool permissions still
come from the supplied configuration.

`live-journal.json` binds the app, user, workspace and persistent session ID.
It saves input before the model runs, answer before sending, and each confirmed
receipt. A failed or ambiguous operation leaves `pending` intact and blocks the
next invocation. Preserve and inspect it before any manual reconciliation;
there is no automatic retry or acknowledgment command. Delivered event IDs
suppress duplicates. Each evaluation is limited to 16 delivered events; the
runner never evicts them. Private `live-sessions.db`, `live-memory.db` and
`live-audit.db` preserve runtime history independently of the delivery journal.
Do not delete user records to restart an evaluation.

SIGINT/SIGTERM requests cancellation and joins active work. The evaluation runner
is a bounded local test entry point, not a durable production Gateway service.
The CLI regression check covers foreign/missing owner, binding changes, pending
preservation, duplicate suppression, the turn limit and denied-provider recovery.
Live acceptance additionally requires user-originated QQ messages, real provider
responses, remote send receipts and confirmation in the user's QQ client. A
correct answer after a second process opens the same session verifies that
specific continuation case, not general long-term-memory quality.
