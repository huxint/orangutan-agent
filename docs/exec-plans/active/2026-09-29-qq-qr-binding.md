# QQ QR Binding

## Goal

Bind an official QQ bot by scanning a terminal QR code, obtain its credentials
without manual AppID/AppSecret input, and persist them privately for the existing
QQ token/webhook boundaries. Provide an executable binding and credential probe.

## Contract and scope

Follow messaging-channels, bootstrap-runtime, secrets-and-state, async-model and
io-runtime. Tencent's official OpenClaw plugin delegates onboarding to
`@tencent-connect/qqbot-connector` 1.2.0: POST a random 32-byte base64 key to
`https://q.qq.com/lite/create_bind_task`, display the returned task's official
connect URL, poll `poll_bind_result`, and authenticate/decrypt the returned
nonce/ciphertext/tag with AES-256-GCM. Reimplement this wire contract using the
existing HTTP client and libsodium; do not incorporate the JavaScript package.

The complete slice is a bounded, cancellable bootstrap operation plus an opt-in
`oran-qq-login` executable that renders a QR, stores credentials atomically, and
can verify the saved credentials through QQTokenSource. A message-receiving QQ
deployment host remains separate work; login must not claim to start chat service.

## Boundaries and risks

- Each bind request requires QQBind/egress_http and publishes a hook before HTTP.
- Fixed official endpoints, bounded responses, strict protocol states and generic
  errors prevent credential or upstream-body disclosure.
- Keep the encryption key local, clear temporary secret buffers, verify the GCM
  tag, and fail before HTTP if the existing AES implementation is unavailable.
- Bound polling and QR refreshes; cancellation joins borrowed HTTP/display work.
- The host owns a private locked directory outside the selected workspace;
  authorize credential reads/writes and never replace an existing binding.
- Use system libqrencode 4.1.1 only in the opt-in executable for QR rendering
  (LGPL-2.1, small C header, estimated <0.1 s per including TU). No runtime-library
  dependency or compile-budget increase is needed.
- The user requested only main remain; implementation commits stay on main.

## Verification and completion

- Controlled tests: create/pending/complete/expired states, known GCM vectors and
  tamper rejection, denied requests and persistence, redaction, bounded retries,
  cancellation, saved binding integrity and no overwrite.
- Build the login executable and affected bootstrap tests; run release tests,
  ASan/UBSan on the new async boundary, and make ci.
- Check the real create/poll endpoint and existing supplied account credentials
  without printing secrets. Completing a new binding requires the user's scan.
- Update owning contracts and build usage, remove this plan when the slice is
  verified, commit, and push main. Track message-host follow-up in live debt.

## Progress

- [x] Merge existing work into main, remove other branches and push.
- [x] Verify the official QR protocol and existing reusable boundaries.
- [ ] Implement and verify binding, private persistence and credential probe.
- [ ] Update current contracts, commit and push.
