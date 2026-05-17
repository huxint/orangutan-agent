// include/oran/permission/approval.hpp — signed approval tokens for the
// `Verdict::ask` flow.
//
// Closes `docs/product-specs/0008-permissions.md` criterion 5 ("Approval
// signing key is rotated when the runtime restarts; prior approvals are
// invalidated") together with the `ApprovalSecret` primitive landed in
// the previous slice. This file owns the token shape, the canonical-bytes
// representation that goes into the MAC, and the `ApprovalAuthority`
// façade that operators a single per-process key against issue/verify.
//
// Token shape. `ApprovalToken` is a value type carrying every field a
// downstream verifier needs to decide whether the approval still applies:
//
//   - `tool_name`     — the tool the approval was issued for. Cross-tool
//                       replay is detected by the MAC because the
//                       canonical bytes include the name byte-for-byte.
//   - `identity`      — the operator/agent identity the approval is
//                       bound to. Same MAC discipline as above.
//   - `input_hash`    — SHA-256 of the call input (libsodium
//                       `crypto_hash_sha256`). Storing the hash rather
//                       than the input itself keeps tokens compact and
//                       avoids leaking input through the token surface.
//   - `nonce`         — 16 random bytes; protects against pre-image
//                       reuse in higher layers.
//   - `expires_at`    — absolute UTC instant beyond which the token is
//                       rejected. The `ApprovalRequest::ttl` is added to
//                       `now` at issue time and stored as an absolute
//                       value so verify is `now`-agnostic in its inputs.
//   - `mac`           — HMAC-SHA-256 over the canonical-bytes
//                       representation (everything above plus a
//                       domain-separator prefix and a 1-byte version
//                       tag).
//
// Canonical bytes layout (the MAC input):
//
//   [16 bytes]  "oran-approval-v1"  domain separator + version sentinel
//   [ 1 byte ]   format version (0x01)
//   [ 4 bytes]   tool_name length, little-endian uint32
//   [ N bytes]   tool_name
//   [ 4 bytes]   identity length, little-endian uint32
//   [ N bytes]   identity
//   [32 bytes]   input_hash
//   [16 bytes]   nonce
//   [ 8 bytes]   expires_at as int64 milliseconds since UNIX epoch, LE
//
// The MAC is computed over those bytes by `ApprovalAuthority::issue` and
// verified the same way by `verify`. Any change in any field flips the
// MAC because every field is in the canonical bytes; constant-time
// `ApprovalSecret::macs_equal` compares the result.
//
// Engineering posture. `ApprovalAuthority` is move-only (it owns an
// `ApprovalSecret`, which is move-only) and not thread-safe — calls are
// expected to be serialized at the caller, which in `oran-agent`'s
// eventual ask-flow plumbing means the agent loop iteration owns the
// authority handle for that turn. If concurrent issue/verify ever
// becomes necessary, wrap the authority in an `asio::strand` rather
// than adding a mutex inside the type (the legacy code's mutex-per-
// member pattern was a recurring source of contention).

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission/approval_secret.hpp>

namespace orangutan::permission {

/// Inputs to `ApprovalAuthority::issue`. Cheap view-only struct; the
/// authority copies the strings into the returned `ApprovalToken`.
struct ApprovalRequest {
  std::string_view tool_name;
  std::string_view input;
  std::string_view identity;
  /// Validity window. Default matches the `approval_ttl` design value
  /// (`docs/design-docs/secrets-and-state.md` "Approval Signing"). A
  /// caller may pass a smaller TTL for one-off prompts; passing zero
  /// or a negative value yields a token that is already expired at the
  /// instant of issue — `verify` will reject it.
  std::chrono::seconds ttl{3600};
};

/// Signed approval token. Move-only data carrier; not constructible
/// outside `ApprovalAuthority::issue` in normal use, but the fields are
/// public so deserializers (a future audit-log replay flow) can rebuild
/// it from on-disk bytes.
struct ApprovalToken {
  std::string tool_name;
  std::string identity;
  std::array<std::byte, 32> input_hash{};
  std::array<std::byte, 16> nonce{};
  core::Time expires_at{};
  std::array<std::byte, ApprovalSecret::mac_bytes> mac{};

  friend bool operator==(const ApprovalToken&, const ApprovalToken&) = default;
};

/// Stateless façade over `ApprovalSecret`. Owns the per-process signing
/// key. `issue` builds and MACs a token; `verify` rebuilds the canonical
/// bytes from the *caller-supplied* trio (tool, input, identity) plus
/// the token's own fields and constant-time compares the MAC.
///
/// Verify checks (in order):
///
///   1. `now < expires_at`               — `Error::permission_denied`
///                                          with `reason=expired`.
///   2. `token.tool_name == tool_name`   — `permission_denied`,
///                                          `reason=tool_mismatch`.
///   3. `token.identity == identity`     — `permission_denied`,
///                                          `reason=identity_mismatch`.
///   4. `SHA256(input) == input_hash`    — `permission_denied`,
///                                          `reason=input_mismatch`.
///   5. constant-time MAC equality       — `permission_denied`,
///                                          `reason=mac_mismatch`.
///
/// Failures attach the rejection `reason` as an `Error::with` context
/// entry so the audit-log slice can record *why* a token failed without
/// re-running verification.
class ApprovalAuthority {
public:
  /// Build an authority from a freshly-generated per-process secret.
  /// Convenience wrapper around `ApprovalSecret::generate()` so callers
  /// don't have to spell the two-step out themselves.
  [[nodiscard]] static core::Result<ApprovalAuthority> with_random_secret();

  explicit ApprovalAuthority(ApprovalSecret secret) noexcept;

  /// Issue a token for `req`, with `expires_at = now + req.ttl`.
  /// Random nonce comes from libsodium's CSPRNG.
  [[nodiscard]] ApprovalToken issue(const ApprovalRequest& req, core::Time now) const;

  /// Verify that `token` authorizes the (tool, input, identity) trio at
  /// `now`. See the class-level checklist for the precedence; the first
  /// failed check wins and stops further work.
  [[nodiscard]] core::Result<void> verify(const ApprovalToken& token,
                                          std::string_view tool_name,
                                          std::string_view input,
                                          std::string_view identity,
                                          core::Time now) const;

  /// SHA-256 of `input` using the same digest as `issue`. Exposed so
  /// callers (and tests) can compute the hash without going through the
  /// full issue path — e.g. when checking whether two inputs canonicalize
  /// to the same approval key.
  [[nodiscard]] static std::array<std::byte, 32> input_hash(std::string_view input) noexcept;

  ApprovalAuthority(const ApprovalAuthority&) = delete;
  ApprovalAuthority& operator=(const ApprovalAuthority&) = delete;
  ApprovalAuthority(ApprovalAuthority&&) noexcept = default;
  ApprovalAuthority& operator=(ApprovalAuthority&&) noexcept = default;
  ~ApprovalAuthority() = default;

private:
  [[nodiscard]] static std::vector<std::byte> canonical_bytes(std::string_view tool_name,
                                                              std::string_view identity,
                                                              std::span<const std::byte, 32> input_hash,
                                                              std::span<const std::byte, 16> nonce,
                                                              core::Time expires_at);

  ApprovalSecret secret_;
};

}  // namespace orangutan::permission
