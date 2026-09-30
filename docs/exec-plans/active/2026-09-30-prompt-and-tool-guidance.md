# Task-Oriented Prompt And Tool Guidance

## Objective

Improve the default system preamble and all seven native tool declarations using
the reviewed Claude Code prompt patterns, adapted to Orangutan's actual runtime.
Instructions should help the model choose actions and recover from errors while
keeping replies focused on the user's task and routine memory use quiet.

## Scope

- Rewrite default guidance around completing requested work, inspecting relevant
  context, choosing bounded tools, sequencing dependencies, respecting existing
  authorization, verifying effects and giving useful task-focused answers.
- Keep cross-tool behavior in the preamble. Tool descriptions own selection,
  prerequisites, meaningful output and recovery; field descriptions own argument
  meaning, defaults, units and relationships.
- Clarify all built-in fields, especially conditional memory-recall defaults,
  byte limits, replacement versus append content, exact-match edits, explicit
  forgetting and self-contained child tasks with shared workspace effects.
- Preserve current inputs, effects, permission gates, schemas' simple provider-
  portable shape and content-derived cache identities. No new tool capabilities,
  storage changes, prompt version system or runtime schema engine.

## Reference And Owners

Reviewed the public third-party extractions of Claude Code's doing-tasks,
outcome-first communication, action-care, independent-tool-call, read, edit and
write guidance in `Piebald-AI/claude-code-system-prompts`. Adopt decision-oriented
wording, not its product-specific tools, fixed path rules or approval defaults.
Durable reference links and policy belong in `docs/rules/prompt-design.md` and
`docs/design-docs/tool-runtime.md`; memory semantics remain in its design document.

## Verification And Risks

- Review declarations against actual preparers and effects. JSON string length
  counts characters; any UTF-8 byte cap must also be stated in the description.
- Verify schema-advertised defaults through dispatch and byte-boundary rejection,
  plus existing provider mapping, stable prompt, file, memory and child regressions.
- Run release build, all 15 test targets and `make ci`, inspect and commit.
- Controlled tests establish declaration/runtime alignment, not model compliance;
  live behavior requires deployment-model evaluation with supplied credentials.
- This complete slice exceeds six files because seven tool owners and the system
  preamble must align. No new dependencies or compile-budget increase is needed.
- Absorb current policy into contract owners and delete this plan on completion.

## Progress

- [x] Inspect current prompts, schemas, validators and public reference prompts.
- [ ] Implement and review coherent task and tool guidance.
- [ ] Verify contracts, update documentation and commit.
