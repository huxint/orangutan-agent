# Telegram Host Commands

## Objective And Contract

Add local `/new`, `/status`, `/help` and `/whoami` commands plus Telegram's
scoped command menu. `/start` and `/commands` show help. Commands require the
existing private-owner admission and never call the model or fetch attachments.
The host currently polls serially; commands run after any active turn finishes.

## Complete Slice

- Parse standalone commands and this bot's username suffix. Leave file paths and
  ordinary text alone; skip commands addressed to another bot. Reject unexpected
  arguments without effects. Quoted text and image captions do not execute commands.
- Persist `/new`'s fresh session identity together with its answer before delivery.
  Preserve all old transcript rows, checkpoints and scoped long-term notes. A
  failed/ambiguous delivery retains the existing reconciliation requirement.
- Rebind the cached AgentSession on the next prompt when the durable identity changes.
- Read session message/checkpoint counts and the most recent traced model/usage
  from the existing stores on the worker executor. Report unavailable values
  explicitly. Show session identity, uptime and configured model without secrets.
- Register one command catalogue for the allowed private chat after identity
  validation. Menu failure is advisory; ordinary command handling remains available.
- Keep policy/hooks and durable replies on the current host/dispatcher contracts.

## Verification And Completion

Cover parsing, native menu scope, denied/non-owner commands, no provider work,
durable rotation, failed sends, preserved old sessions and read-only status.
Run affected release tests, the complete release gate, ASan/UBSan host checks and
`make ci`. Update the messaging contract, verify the live scoped command menu,
restart with existing data, remove this plan and commit the slice.

## References

OpenClaw's [slash commands](https://github.com/openclaw/openclaw/blob/main/docs/tools/slash-commands.md)
and [reset handler](https://github.com/openclaw/openclaw/blob/main/src/auto-reply/reply/commands-reset.ts)
inform local handling and preservation of the previous session. This slice does
not add model switching, forced compaction or an interrupting `/stop` command.
