// src/oran-core/error.cpp — Error implementation.

#include <oran/core/error.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace orangutan::core {

namespace {

constexpr std::string_view kKindNames[] = {
    "ok",       "cancelled", "invalid_argument", "not_found",    "permission_denied", "capability_not_granted",
    "config",   "auth",      "network",          "rate_limit",   "upstream",          "parsing",
    "timeout",  "conflict",  "storage",          "hook_timeout", "hook_failed",       "mailbox_overflowed",
    "internal",
};

}  // namespace

std::string_view to_string_view(ErrorKind k) noexcept {
  const auto idx = static_cast<std::size_t>(k);
  if (idx < std::size(kKindNames)) {
    return kKindNames[idx];
  }
  return "unknown";
}

Error::Error(ErrorKind kind, std::string message) noexcept : kind_{kind}, message_{std::move(message)} {}

bool Error::retryable() const noexcept {
  switch (kind_) {
    case ErrorKind::network:
    case ErrorKind::rate_limit:
    case ErrorKind::timeout:
    case ErrorKind::upstream:
      return true;
    default:
      return false;
  }
}

Error& Error::with(std::string key, std::string value) & {
  context_.emplace_back(std::move(key), std::move(value));
  return *this;
}

Error&& Error::with(std::string key, std::string value) && {
  context_.emplace_back(std::move(key), std::move(value));
  return std::move(*this);
}

Error& Error::with_retry_after(std::chrono::milliseconds d) & noexcept {
  retry_after_ = d;
  return *this;
}

Error&& Error::with_retry_after(std::chrono::milliseconds d) && noexcept {
  retry_after_ = d;
  return std::move(*this);
}

Error Error::cancelled() {
  return Error{ErrorKind::cancelled, "cancelled"};
}

Error Error::invalid_argument(std::string message) {
  return Error{ErrorKind::invalid_argument, std::move(message)};
}

Error Error::not_found(std::string message) {
  return Error{ErrorKind::not_found, std::move(message)};
}

Error Error::permission_denied(std::string message) {
  return Error{ErrorKind::permission_denied, std::move(message)};
}

Error Error::config(std::string message) {
  return Error{ErrorKind::config, std::move(message)};
}

Error Error::network(std::string message) {
  return Error{ErrorKind::network, std::move(message)};
}

Error Error::rate_limit(std::string message) {
  return Error{ErrorKind::rate_limit, std::move(message)};
}

Error Error::upstream(std::string message) {
  return Error{ErrorKind::upstream, std::move(message)};
}

Error Error::timeout(std::chrono::milliseconds elapsed) {
  Error e{ErrorKind::timeout, "operation timed out"};
  e.with("elapsed_ms", std::to_string(elapsed.count()));
  return e;
}

Error Error::parsing(std::string message) {
  return Error{ErrorKind::parsing, std::move(message)};
}

Error Error::storage(std::string message) {
  return Error{ErrorKind::storage, std::move(message)};
}

Error Error::internal(std::string message) {
  return Error{ErrorKind::internal, std::move(message)};
}

}  // namespace orangutan::core
