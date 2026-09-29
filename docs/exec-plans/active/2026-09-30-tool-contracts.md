# Lean File Tools And Complete Tool Feedback

## Objective

Reduce model-facing filesystem complexity and make partial tool results explicit
through the provider/tool loop, preserving authorization and stored user data.

## Current Contract And Scope

FileRead exposes two range dialects and conditional metadata caching. Its native
description repeats its schema. Tool usage records truncation but the loop drops
that fact when building model messages. Filesystem writes already have pinned
authority, exact replacement and optional version checks; retain those boundaries.

Implement two complete slices:

1. Use described `offset`/`limit` line windows (default 1/2000), remove model-facing
   byte ranges and conditional reads, and provide explicit continuation/EOF hints.
   Keep byte limits, raw text, version tokens and existing path policy. Simplify
   file-tool descriptions and redundant edit state. Do not add aliases or a new
   dependency. IO byte-range APIs remain available to non-model callers.
2. Preserve truncation and dropped-data notices in model-visible tool results,
   including persisted transcript values, without changing success/error status.

References: Claude Code's collected Read/Edit/Write descriptions in
`Piebald-AI/claude-code-system-prompts`, and the official DeepSeek harness
`packages/fs/tool-fs/src/read-render.ts` and filesystem contract. Adopt bounded
windows, actionable continuation and exact edits; avoid speculative training
claims, plugin infrastructure, shell execution and unrelated product expansion.

## Verification And Completion

Exercise default/explicit windows, EOF, limits, fresh reads, denied malformed
inputs, stale edits, and provider-visible truncation with controlled tests.
Run affected tool/agent targets, the release build/test suite and `make ci`.
Update owning contracts, inspect the diff and commit verified slices. Remove
this plan when complete; remaining product gaps belong in live debt.
