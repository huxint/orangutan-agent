# Memory

`oran-memory` owns conversation serialization, scoped durable notes, discovery
cues and lexical recall. It does not choose an identity or call a provider.
Useful memory closes a loop: discover an applicable lesson, read it, act on it,
correct it when new evidence arrives, and make the correction available later.

| Context | Lifetime and purpose |
| --- | --- |
| Recent conversation | Session-scoped verbatim exchanges for the current task. |
| Memory index | A bounded view of scoped notes, refreshed at each prompt boundary so the agent knows what it can consult. |
| Durable notes | Cross-session preferences, corrections, decisions and references, read in detail when relevant. |

A note should hold one useful lesson. Its title names the topic; its opening
sentence states the fact; the body explains why it matters and when to apply it.
The index derives a cue from existing content, so old records need no rewrite.
Temporary progress stays in the conversation. There is no persisted working
summary or automatic transcript compaction yet.

## Consultation And Learning

MemoryRecall and MemoryRemember are active in the default catalogue. The runtime
preamble and tool descriptions tell the agent to consult applicable notes before
choosing an approach or asking the user to repeat context, and to revisit memory
when the task reveals another relevant topic. A durable correction or preference
is saved in the turn where it arises, without waiting for a "remember this"
request. The agent reads an existing related note and reuses its ID when updating
the lesson. One-off instructions, guesses, secrets and facts readily derived from
code do not become durable notes. MemoryForget remains deferred for explicit
removal.

These are model behavior instructions. The runtime guarantees the discovery,
read and write paths, but does not infer durability, force a write every turn, or
claim that a successful scripted test proves spontaneous model behavior. Current
owner instructions and current evidence take precedence over saved notes.
Writes still require the configured permission; visibility never grants a write.

This design adopts progressive disclosure from
[Claude Code auto memory](https://code.claude.com/docs/en/memory#auto-memory) and
[Letta MemFS](https://docs.letta.com/concepts/memfs/index.md), and the emphasis on
learning that changes future behavior from Letta's
[Context Constitution](https://github.com/letta-ai/context-constitution/blob/main/constitution/CONSTITUTION.md).
The [memory update trigger prompt](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-memory-write-update-triggers-and-timing.md)
is a third-party extraction used to inform same-turn correction handling.
[LangGraph's memory design](https://docs.langchain.com/oss/python/concepts/memory)
separates thread state from cross-thread knowledge and explains the cost of
foreground versus background learning. The current implementation uses one
foreground write path. OpenHands' [condenser design](https://github.com/OpenHands/software-agent-sdk/blob/main/openhands-sdk/openhands/sdk/context/condenser/README.md)
informs the separation of preserved history from a bounded model view; its
compaction mechanism is not implemented here.

## Prompt Boundary

AgentSession resolves `config.memory.longterm.recall` automatically. Orientation
is enabled by default when a backend is present, with up to 20 entries and an
8192-byte text budget. An explicit `AgentSessionOptions::longterm_recall` overrides
configuration. Exact caller-supplied `memory_framing` replaces default orientation;
explicitly enabling both is invalid. Disabling automatic orientation leaves
authorized model-directed memory tools available.

Orientation dispatches MemoryRecall with no search query. It uses the ordinary
prepared-call, permission, hook, audit and output-cap boundaries. The returned
index is fixed for the provider/tool iterations in that prompt; an accepted write
appears in the next prompt's index, while its tool result is available immediately
in the current conversation. Lookup times and scores never enter index text.

An empty index says that no notes match the current page/filter. An unavailable,
denied or incomplete index is identified as unavailable; it never masquerades as
empty memory and does not prevent ordinary work. Cancellation still propagates.
A hook rewrite that changes browsing into a full-content read cannot place that
body in the automatic prefix. The host accepts only a complete `memory_index`
output there.

## Session History

`session::Store` wraps `storage::SessionRepository`. Keys are explicit session
and agent values; messages retain typed text, thinking, tool-use and tool-result
blocks. Its API appends and reads conversations, with serialization owned by
memory and transactions owned by storage.

`load_tail` returns the newest messages in conversation order with row and encoded
byte limits. Defaults are 128 rows and 512 KiB; valid limits are 1–4096 rows and
1–16 MiB. Counting includes content and metadata bytes. Stop at the first row
that does not fit, preserving a contiguous suffix. The full history remains in
SQLite. The agent removes an incomplete leading exchange before prompt assembly.

`Store::append_all` serializes an entire transcript suffix before the repository
acquires its writer. Serialization or storage failure leaves the preceding
conversation intact; success appends every message in order. Single-message
`append` uses the same path. An empty suffix has no effect. Message encoding and
existing tables are preserved. Individual memory-tool effects commit under their
own dispatch contract and are not rolled back with a failed transcript commit.
Explicit import mapping remains tracked persistence work.

## Scoped Reads And Records

A `RecordKey` is `(scope_key, id)`. Every read, write and removal supplies that
scope. Record kinds are user, feedback, project, reference and team. These are
stored classifications, not evidence of a running team subsystem.

MemoryRecall has three uses:

| Input | Result |
| --- | --- |
| `{}` or `{"offset":20,"kinds":["feedback"]}` | A compact index page with actionable IDs and cues. Follow `next_offset` with the same filters to continue. |
| `{"id":"response-style"}` | The complete visible note in the host's scope, independent of lexical matching. |
| `{"query":"release workflow","limit":5}` | Full notes matching literal topic words; optional kinds filter the results. |

ID and query are mutually exclusive. Offset applies only to browsing. The tool
limit is 1–20, defaulting to 20 for the index, one for an exact read, and five for
search. JSON cannot select a scope. Ambiguous selectors, blank/control-containing
text, invalid UTF-8 and out-of-range bounds fail before authorization or approval.

`Fts5Backend` stores records in `memory.db` and updates its FTS index
transactionally. `Backend::list` returns a page plus one look-ahead cue. SQLite
projects bounded title/body prefixes before copying values to the application.
`make_index` performs pure selection and UTF-8-safe rendering within the byte
budget, keeping entire entries and exact IDs. It clips titles to 120 bytes and
cues to 240 bytes before adding an ellipsis. Feedback comes first, then user
notes, then other kinds; each group sorts by importance, update time and ID.
Read frequency does not affect this order. Pagination assumes unchanged records;
concurrent writes can move page boundaries.

The index text budget is 512–8192 bytes, further constrained by a nonzero tool
text cap. A smaller cap makes automatic orientation unavailable. Oversized legacy
IDs that cannot fit even one entry are reported as omissions, without being
shortened into a different actionable ID. Pagination advances past them. Index
JSON has `kind=memory_index`, entries, `next_offset` and `omitted_count`; it
contains no full record bodies. Browsing never acquires a writer or updates
`last_read_at`.

`recall(Backend&, RecallRequest)` reads an exact ID or searches, updates only the
selected notes' read timestamps, and returns owned content snapshots and
framing. Ordinary reads exclude shadow records; raw backend `get` remains able
to inspect them. Search accepts at most 4096 bytes and 100 backend results.
Up to 32 literal terms are OR-combined, so useful topic terms need not match
every word of a question. ASCII punctuation separates terms; non-ASCII runs use
SQLite's existing unicode61 tokenizer. BM25 weights title/body/tags as 4/1/2.
Ties use update time and ID. This is lexical search: paraphrase, translation and
general Chinese word segmentation are not supplied by this index. Browsing and
exact IDs remain available when wording does not match.

Rendering is a pure function of selected content. Timestamps, request IDs and
scores stay out of prompt framing. An existing row's update preserves its
creation and last-read timestamps and keeps update time monotonic; correcting a
lesson replaces that scoped ID and its indexed content instead of duplicating it.

Retrieval values carry one lexical score. Tool-result JSON and memory-read hook
payloads retain `score`, `lexical_score` and a null `vector_score` for compatibility
with recorded results and hook consumers.

Memory tools enter through validation, permissions, hooks and audit. Bindings
receive scope from the host. Writes publish a blocking pre-write gate; accepted
writes, reads and removals publish advisory observations. `memory_read_after`
uses source `MemoryRecall` for full records and `MemoryRecall:index` for the
displayed cues. For index observations, hit title/body hold the cue; undisplayed
record metadata uses neutral defaults. The existing redaction boundary applies.

SQLite operations use the blocking executor. Hook publication and session-state
updates resume on the coordinating strand.

User database contents and migration history are preserved during API reduction.
Existing optional index tables, including sqlite-vec data, are left intact when
opening the lexical store. Record metadata, shadow flags and stored kinds retain
their existing encoding.
[Storage](storage-runtime.md) owns persistence mechanics;
[agent execution](agent-platform.md) owns parent/child session composition.

## Behavioral Acceptance

Controlled-provider tests exercise the complete runtime, including a correction
followed by a reopened session, but supply the tool decisions themselves. A
deployment model needs a separate evaluation with ordinary prompts and an
isolated memory scope. Observe actual tool calls and stored rows as well as the
answer; an assertion that the model "remembers" is not persistence evidence.

| Scenario | Expected behavior |
| --- | --- |
| "以后用中文给我结论，理由放到必要的时候再说。" | Save the durable preference in this turn, updating the related note if one exists. |
| New session: "帮我评估这个改动。" | Discover and consult the applicable preference, then apply it without a memory reminder. |
| "只有这次请用英文写给国外同事。" | Follow this request without replacing the durable language preference. |
| A correction supersedes a saved decision | Read and update the existing ID; subsequent work uses the corrected decision. |
| An unrelated trivial question | Avoid unrelated recall and unsupported durable writes. |
| Read or write permission is refused | Continue using allowed context; do not leak a note or claim that a refused write persisted. |

Record consultation success, durable-write success, stale-note use, unwanted
writes and token cost across repeated runs of the deployment model. Semantic
retrieval, background reflection and compaction need evidence from these failures
before extending the runtime. Pending gates live in
[live debt](../exec-plans/tech-debt-tracker.md).
