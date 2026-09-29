# Telegram Image Input

## Objective

Receive Telegram photos and image documents with captions, download bounded
image bytes through authorized HTTP, and submit typed image content to the
configured vision-capable DeepSeek Flash provider. Preserve images in session
history without exposing credential-bearing Telegram download URLs.

## Slice

- Add an owned base64 image block to core conversation values, session
  serialization, prompt input, context accounting and both provider protocols.
- Decode photo/image-document metadata at the channel boundary. The host fetches
  files from Telegram's fixed endpoint with bounded size/time and safe errors;
  bot credentials never enter provider content or stored image metadata.
- Keep captions and image content together in the admitted session turn. Report
  unsupported/oversized image fetches visibly rather than silently dropping input.
- Retain text behavior and existing user data. Document supported formats/limits.

## Verification

Test photo decoding, denied/oversized fetches, protocol image encoding, session
round trips and image context accounting. Run affected builds/tests, the release
gate and `make ci`, then validate a real image through the configured model and
restart the bot with its current session. Remove this plan after contracts own
the completed behavior and commit the slice.
