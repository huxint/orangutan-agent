# Memory Redesign

## Objective

Make consulting and maintaining memory part of ordinary work. The user should
not have to say "search your memory" or "remember this" for the agent to use an
applicable lesson or retain a durable correction. A successful loop is: see the
available knowledge, read the relevant note, apply it, update what was learned,
and see that update in a later conversation.

## Evidence And Design Choices

Read the following primary documentation and implementation references before
choosing the slice:

| Reference | Useful principle | Application here |
| --- | --- | --- |
| [Claude Code auto memory](https://code.claude.com/docs/en/memory#auto-memory) | A concise index loads automatically; one note holds one durable fact; details load on demand. | Always-visible scoped orientation, actionable record IDs, concise relevance cues. |
| [Claude Code memory update triggers](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-memory-write-update-triggers-and-timing.md) | Save durable corrections in the turn where they arise; distinguish lasting preferences from one-off instructions. | Explicit consultation and write triggers in the tool descriptions and runtime preamble. This is a third-party extraction of prompts, not published Claude Code source. |
| [Letta MemFS](https://docs.letta.com/concepts/memfs/index.md) and [Context Constitution](https://github.com/letta-ai/context-constitution/blob/main/constitution/CONSTITUTION.md) | Compact context and discovery paths govern what the agent can use. Learning should change future behavior; a larger archive alone does not do that. Current MemFS does not require vector search. | Keep the memory map in context and make its references directly readable. Adopt the context-management mechanisms, not claims about agent selfhood or self-directed goals. |
| [LangGraph memory](https://docs.langchain.com/oss/python/concepts/memory) | Thread state and cross-thread knowledge have different lifetimes. Immediate writes and background extraction have different latency and consistency costs. | Retain a single foreground, permissioned write path. Background reflection needs separate evidence before adding another writer. |
| [OpenHands condenser](https://github.com/OpenHands/software-agent-sdk/blob/main/openhands-sdk/openhands/sdk/context/condenser/README.md) and [summary prompt](https://github.com/OpenHands/software-agent-sdk/blob/main/openhands-sdk/openhands/sdk/context/condenser/prompts/summarizing_prompt.j2) | An append-only history and a compact model view are separate; preserve goals, pending work and recent complete exchanges. | Keep transcript preservation separate from durable notes. Working-context compaction is a subsequent slice, not something a note index can claim to solve. |

## Current Failure Shape

- The unfinished changes make memory tools visible, but automatic orientation is
  disabled in the default session and example configuration.
- The preliminary index only exposes titles, has no exact-ID read path, and
  still puts complete records in structured tool output.
- Listing calls use the same read-touch path as content retrieval. Merely
  presenting an index therefore counts as reading every note.
- Lexical recall requires every whitespace-separated term to match. Natural
  questions commonly miss a record containing only the useful topic terms.
- Replacing a record overwrites its creation/read timestamps.
- There is no explicit distinction between empty memory and unavailable memory.
- The inherited worktree has two failing memory tests for the unfinished empty
  query/list contract. Existing prepared-call changes must be preserved.

## Complete Slice

1. Keep MemoryRecall and MemoryRemember in the ordinary catalogue. Describe
   when and why to use them, including consulting memory before asking the user
   to repeat context and updating an existing note after a durable correction.
2. Load a bounded, deterministic orientation at every prompt boundary when the
   backend is available. Resolve configuration in the session, support explicit
   opt-out and caller framing, and keep the prefix stable during tool iterations.
3. Give MemoryRecall three explicit uses: browse a paginated index, read an
   exact scoped ID, or search literal topic words. Index entries include a short
   cue derived from existing content. Index output excludes full bodies and does
   not update read timestamps. Report pagination and unavailable memory honestly.
4. Bound candidate and prompt sizes before rendering. Preserve whole index
   entries and UTF-8. Search can match useful terms independently, with title
   matches preferred; it remains lexical, not semantic search.
5. Update existing records in place while preserving creation and read history.
   Memory effects retain the prepared-call, permission, hook, audit and worker
   executor boundaries. Host-supplied scope cannot be changed by tool input.
6. Cover the complete cross-session loop and old-record preservation through
   the real runtime with a controlled provider, including denied effects and
   hook rewrites. Distinguish these mechanism checks from real-model behavior.

No database format change, new dependency, autonomous background agent, default
permission expansion, or embedding service is required for this slice. Adding
record provenance or compaction snapshots requires the storage backup/import
contract to be addressed first.

## Verification

- Measure affected translation units before and after under the same local
  release configuration; these measurements are not reference-hardware certification.
- Run memory, tool, agent, prompt, config and bootstrap builds/tests while
  iterating, then the complete release suite and `make ci`.
- Deliberately break default orientation, exact-ID scope filtering, index
  projection and update preservation in an isolated copy; the relevant public
  regressions must fail.
- Evaluate prompts without "remember" or "recall" hints: durable correction,
  cross-session application, one-off instruction, superseded preference,
  unrelated task, and read/write refusal. Scripted provider responses prove
  runtime behavior only. A real-model run needs explicitly supplied credentials;
  keep that acceptance gate visible if unavailable.

## Progress

- [x] Read existing contracts and preserve the inherited worktree.
- [x] Compare primary designs and identify the concrete discovery/write failures.
- [ ] Implement the default orientation, browse/read/search and update loop.
- [ ] Verify behavior, preservation and deliberate fault detection.
- [ ] Update owning contracts, record remaining gates, delete this plan and commit.
