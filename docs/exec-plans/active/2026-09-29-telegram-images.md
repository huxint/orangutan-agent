# Telegram Image And Reply Input

## Objective

Receive Telegram photos and image documents with captions, download bounded
image bytes through authorized HTTP, and submit typed image content to the
configured vision-capable DeepSeek Flash provider. Include replied-to text,
selected quote fragments and images as explicit conversation context. Preserve images in session
history without exposing credential-bearing Telegram download URLs.

## Slice

- Add an owned base64 image block to core conversation values, session
  serialization, prompt input, context accounting and both provider protocols.
- Decode photo/image-document metadata at the channel boundary. The host fetches
  files from Telegram's fixed endpoint with bounded size/time and safe errors;
  bot credentials never enter provider content or stored image metadata.
- Keep captions and image content together in the admitted session turn. Report
  unsupported/oversized image fetches visibly rather than silently dropping input.
- Decode one level of Telegram reply context; fetch quoted images through the
  same authorization boundary and keep reference text distinct from the new request.
  Test reply-to-bot text, quote fragments, replied images and text-only behavior.
- Retain text behavior and existing user data. Document supported formats/limits.

## Verification

Test photo decoding, denied/oversized fetches, protocol image encoding, session
round trips and image context accounting. Run affected builds/tests, the release
gate and `make ci`, then validate a real image through the configured model and
restart the bot with its current session. Remove this plan after contracts own
the completed behavior and commit the slice.

## Current Verification

The typed image path, Telegram decoding/download boundary and session continuation
are implemented. The initial release suite and repository gate pass. A live
DeepSeek Flash request correctly identified a synthetic red/blue image. The bot
has restarted with its existing session identity and all preceding messages.
Additional authorization and cancellation regressions pass; image compaction
is finishing its release build. The user confirmed live image input and reported
missing reply context. Reply mapping and its tests, the ASan/UBSan host run and
the final release gate remain.
