# Telegram Host Review

## Scope

Review and simplify the existing Telegram deployment slice. Preserve live user
state and the current JSON journal format. No new channel or messaging feature.

## Implementation

- Represent journal identity, cursor and pending delivery as typed runtime values;
  parse untrusted JSON once at the persistence boundary.
- Remove fabricated transport failures and duplicate cursor transitions. Keep
  necessary external-input validation and secret-safe exception translation.
- Retry transient upstream polling failures with retained retry metadata; keep
  competing-poller conflicts terminal. Do not retry sends or turns.
- Separate recovery/probe setup from normal execution and preserve real failures
  when a shutdown signal arrives.

## Verification

Add regressions for retry classification, recovery/cursor integrity, partial
delivery and shutdown attribution. Run affected release tests, the full release
gate, ASan/UBSan lifetime checks and `make ci`. Review the diff, update only the
owning messaging contract, then remove this plan and commit the verified slice.

Existing runtime files and deployment paths remain local. The running bot's
session history must survive any restart used to verify the changes.
