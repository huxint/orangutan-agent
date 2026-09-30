# Dynamic Child Agents

## Objective

Create a fresh child directly from each AgentRun task, without preset names or
an agents configuration block. This replaces configured child templates rather
than adding another mode alongside them.

## Slice

- AgentRun requires only prompt; background and label remain host-supported
  options. Remove agent selectors, named-agent configuration and session preset
  lookup. Reject obsolete fields through ordinary schema/config validation.
- Generate a fresh session and agent identity for each child. Inherit provider
  route, workspace, memory scope, active tools and external-effect restrictions.
  Keep parent permission snapshots, independent approval identity, one-generation
  admission and joined cancellation. Do not copy the parent's chat transcript.
- Enable delegation without configured agents. Background support still depends
  on a retained host with worker capacity; one-shot hosts must not promise it.
- Replace preset-specific tests and fixtures with dynamic identity, inherited
  authority, no-preset admission and existing background lifecycle regressions.
  Preserve all stored sessions and notes without migrations or record rewrites.
- Update owning tool, config, bootstrap, agent and messaging contracts and
  examples. Remove the completed plan after verification.

## Verification

Run affected config/tool/bootstrap tests and app/benchmark builds, debug
ASan/UBSan for child ownership, the complete release suite and make ci. Commit
the completed slice. No new dependencies or worker pools.
