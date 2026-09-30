# Desktop Browser Prototype

## Goal

Deliver the accepted [desktop specification](../../product-specs/desktop-prototype.md)
as a runnable, polished React/TypeScript/Vite application under `apps/desktop`.
Complete all five pages, source-isolated demo execution and visual/behavioral
verification. Preserve the existing C++ runtime and user data.

## Scope And Contracts

- Local assistant conversations support execution, approvals, cancellation and retry.
- External-channel conversations, tasks, teams and approvals are read-only observations.
- Connection lifecycle controls do not grant channel execution authority.
- Shared workspace notes and requester-scoped personal memories remain distinct.
- Page-local demo effects stop on close; persisted active attempts recover interrupted.
- Chinese/English, light/dark/system appearance, responsive panels and keyboard access.
- No real providers, credentials, channel transport, filesystem writes, daemon or packaging.

The product specification owns UI behavior. Existing
[bootstrap](../../design-docs/bootstrap-runtime.md),
[channel](../../design-docs/messaging-channels.md),
[permission](../../design-docs/permissions-and-hooks.md) and
[memory](../../design-docs/memory-system.md) contracts remain the future integration boundaries.
Frontend dependencies are optional and isolated from xmake; their C++ compile cost is zero.
This complete application exceeds the usual review-size guideline; the user-authorized
five-page slice is documented here before implementation.

## Implementation

1. Register pinned frontend dependencies and create the optional application scaffold.
2. Implement typed source identity, pure command/event transitions, fixtures, persistence
   validation and a cancellable demo scheduler. Cover isolation and recovery behavior.
3. Build the warm studio visual system and five pages: assistant, tasks, connections,
   memory and settings. Add artifact/team panels, approvals, notifications and bilingual copy.
4. Verify real browser interactions, both themes/languages, narrow windows, keyboard
   focus and all consequential demo state transitions. Fix findings before delivery.
5. Update the owning specification/build/dependency documentation and application usage.
   Remove this completed plan after durable decisions are recorded; commit the completed slice.

## Risks And Mitigations

- UI-only read-only checks: enforce source restrictions in command transitions as well.
- Stale timers or cross-session updates: correlate events with attempt IDs and source IDs;
  stop all scheduler effects at unmount and ignore terminal/old-attempt events.
- Lost/corrupt saved data: validate snapshots before use and disable overwrite on recovery
  failure; expose explicit backup/reset controls rather than silently discarding state.
- Mock states mistaken for integration: persistent demo indicator and sample-only connection
  configuration; no real credentials or external effects.
- Theme/i18n regressions: shared design tokens and bilingual UI labels, with browser checks.

## Validation And Completion

- TypeScript check and production Vite build.
- Node behavioral tests for commands, observation events, source isolation, deduplicated
  notifications, denial/cancellation, scoped memory and snapshot recovery.
- Playwright browser checks for navigation, local execution/approvals, channel read-only
  previews, connections, memory editing, notifications, theme/language and keyboard access.
- Actual screenshots reviewed in light/dark and narrow/wide layouts.
- `make ci` and staged diff hygiene before committing. No C++ changes require compilation.
- All applicable D01–D15 specification acceptance cases pass; no active plan remains.

## Progress

- [x] Product requirements and repository boundaries reviewed.
- [ ] Application and domain behavior implemented.
- [ ] Visual and browser interaction checks completed.
- [ ] Documentation synchronized, gates passed and application committed.
