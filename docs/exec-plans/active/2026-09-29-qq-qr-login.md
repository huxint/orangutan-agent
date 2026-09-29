# Official QQ QR Binding

## Objective

Let the user bind an official QQ robot by scanning and confirming a QR code,
without entering AppID or AppSecret. Follow Tencent's current Connector protocol;
preserve existing credentials and keep secrets out of QR URLs, logs and config.
The supplied manual credentials have already passed live token and bot-identity
checks; they are not copied into tracked files.

## Complete Slice

- Implement create/poll/decrypt transitions through injected bounded HTTP,
  permission/hooks and cancellation. Use a fresh 32-byte key and the existing
  libsodium AES-256-GCM implementation; verify authentication tags before using
  any returned credential.
- Add an opt-in `oran-qq-login` executable. Render the official authorization URL
  as terminal QR and SVG, handle waiting/expiry/cancellation, and atomically store
  a private credential bundle after token verification. Existing bindings are
  never overwritten silently.
- Use system libqrencode 4.1.1 only in the login application/tests. Its small C
  interface supplies QR encoding; no copied QR or cryptographic implementation.
- Treat QR binding separately from delivery hosting. Correct the earlier blanket
  WebSocket deprecation statement using the current official plugin documentation.
- Cover waiting/completed/expired/error states, GCM tamper rejection, redaction,
  denied IO, private persistence and cancellation. Run affected/release tests,
  ASan/UBSan and `make ci`, then exercise a real QR authorization with the user.

## Source

Tencent's [official QQ plugin](https://github.com/tencent-connect/openclaw-qqbot)
uses `@tencent-connect/qqbot-connector` 1.2.0. Its published protocol uses
`q.qq.com/lite/create_bind_task`, `q.qq.com/lite/poll_bind_result`, and an
AES-256-GCM envelope (12-byte IV, ciphertext, 16-byte tag). Only protocol behavior
is reimplemented; the Connector package is not a runtime dependency.

## Completion

Document the supported command, credential storage and hardware requirements,
remove this plan once verified, and commit the completed slice. Live binding
requires the user's QR confirmation; do not claim it occurred from a fixture.
