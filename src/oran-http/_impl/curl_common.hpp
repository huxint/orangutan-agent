// src/oran-http/_impl/curl_common.hpp — shared libcurl RAII wrappers and
// error translation for the oran-http transport TUs (client.cpp,
// websocket.cpp). Internal: never included from public headers (C6).

#pragma once

#include <string>
#include <string_view>

#include <asio/cancellation_state.hpp>
#include <asio/cancellation_type.hpp>

#include <curl/curl.h>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

namespace orangutan::http::detail {

[[nodiscard]] inline bool is_cancelled(const asio::cancellation_state& cancellation) noexcept {
  return cancellation.cancelled() != asio::cancellation_type::none;
}

[[nodiscard]] inline core::Error curl_error(CURLcode code, std::string_view action) {
  const auto message = std::string{curl_easy_strerror(code)};
  switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
      return core::Error{core::ErrorKind::timeout, "http request timed out"}.with("curl_error", message);
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
      return core::Error::network("http transport failed")
          .with("curl_error", message)
          .with("action", std::string{action});
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
      return core::Error::network("http TLS verification failed")
          .with("curl_error", message)
          .with("action", std::string{action});
    default:
      return core::Error::upstream("http client failed")
          .with("curl_error", message)
          .with("action", std::string{action});
  }
}

[[nodiscard]] inline core::Error curl_multi_error(CURLMcode code, std::string_view action) {
  return core::Error::network("http multi transport failed")
      .with("curl_multi_error", curl_multi_strerror(code))
      .with("action", std::string{action});
}

class CurlGlobal {
public:
  CurlGlobal() : ok_{curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK} {}

  ~CurlGlobal() {
    if (ok_) {
      curl_global_cleanup();
    }
  }

  CurlGlobal(const CurlGlobal&) = delete;
  CurlGlobal& operator=(const CurlGlobal&) = delete;

  [[nodiscard]] bool ok() const noexcept {
    return ok_;
  }

private:
  bool ok_{false};
};

[[nodiscard]] inline CurlGlobal& curl_global() {
  static CurlGlobal global;
  return global;
}

class CurlEasy {
public:
  CurlEasy() : handle_{curl_easy_init()} {}

  ~CurlEasy() {
    if (handle_ != nullptr) {
      curl_easy_cleanup(handle_);
    }
  }

  CurlEasy(const CurlEasy&) = delete;
  CurlEasy& operator=(const CurlEasy&) = delete;

  CurlEasy(CurlEasy&& other) noexcept : handle_{other.handle_} {
    other.handle_ = nullptr;
  }

  CurlEasy& operator=(CurlEasy&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        curl_easy_cleanup(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] CURL* get() const noexcept {
    return handle_;
  }

private:
  CURL* handle_{nullptr};
};

class CurlMulti {
public:
  CurlMulti() : handle_{curl_multi_init()} {}

  ~CurlMulti() {
    if (handle_ != nullptr) {
      curl_multi_cleanup(handle_);
    }
  }

  CurlMulti(const CurlMulti&) = delete;
  CurlMulti& operator=(const CurlMulti&) = delete;

  CurlMulti(CurlMulti&& other) noexcept : handle_{other.handle_} {
    other.handle_ = nullptr;
  }

  CurlMulti& operator=(CurlMulti&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        curl_multi_cleanup(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] CURLM* get() const noexcept {
    return handle_;
  }

private:
  CURLM* handle_{nullptr};
};

class CurlHeaders {
public:
  CurlHeaders() = default;

  ~CurlHeaders() {
    if (headers_ != nullptr) {
      curl_slist_free_all(headers_);
    }
  }

  CurlHeaders(const CurlHeaders&) = delete;
  CurlHeaders& operator=(const CurlHeaders&) = delete;

  [[nodiscard]] core::Result<void> append(std::string_view name, std::string_view value) {
    if (name.empty()) {
      return std::unexpected(core::Error::invalid_argument("http header name must be non-empty"));
    }
    const auto line = std::string{name} + ": " + std::string{value};
    auto* next = curl_slist_append(headers_, line.c_str());
    if (next == nullptr) {
      return std::unexpected(core::Error::internal("failed to allocate curl header list"));
    }
    headers_ = next;
    return {};
  }

  [[nodiscard]] curl_slist* get() const noexcept {
    return headers_;
  }

private:
  curl_slist* headers_{nullptr};
};

/// Unregisters the easy handle from the multi handle on scope exit.
class CurlEasyRegistration {
public:
  CurlEasyRegistration(CURLM* multi, CURL* easy) : multi_{multi}, easy_{easy} {}

  ~CurlEasyRegistration() {
    if (multi_ != nullptr && easy_ != nullptr) {
      curl_multi_remove_handle(multi_, easy_);
    }
  }

  CurlEasyRegistration(const CurlEasyRegistration&) = delete;
  CurlEasyRegistration& operator=(const CurlEasyRegistration&) = delete;

private:
  CURLM* multi_{nullptr};
  CURL* easy_{nullptr};
};

}  // namespace orangutan::http::detail
