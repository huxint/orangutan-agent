# Telegram Live Host

## Objective

Run an authenticated Telegram private conversation through the existing channel
dispatcher, DeepSeek provider and persistent AgentSession. Credentials remain in
local private storage/environment and never enter tracked files or diagnostics.

## Smallest complete slice

- Add an opt-in C++ long-polling application using existing HTTP, channel and
  bootstrap contracts, with one explicitly allowed private user per state root.
- Verify bot identity and refuse active webhooks; never discard queued updates.
- Persist the bot/user/session binding and update cursor. Journal intake before
  running a turn, and retain ambiguous failures across restart without replaying
  model/tool/send effects. Serial processing bounds admission.
- Route polling through explicit permission decisions and channel hooks. Session
  and reply effects retain their existing authorization boundaries.
- Use private locked state, separate workspace, bounded HTTP requests and joined
  signal cancellation. Provide a read-only probe for deployment verification.

## Verification and completion

Controlled tests cover private-user admission, cursor/intake recovery and API
failures. Build the application and affected tests, run the release suite and
`make ci`, then validate Telegram identity and a real DeepSeek turn. A user-sent
message establishes live end-to-end delivery; do not label controlled or direct
provider tests as that evidence. Update the messaging/build contracts and remove
this plan when implemented. Commit the verified slice.

## Progress

Telegram identity and absent webhook verified. Model credentials supplied for
DeepSeek Flash. Implementation and live conversation verification remain.
