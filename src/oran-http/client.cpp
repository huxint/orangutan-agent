// src/oran-http/client.cpp - libcurl-backed body HTTP client.

#include <oran/http/client.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <curl/curl.h>

#include <oran/core/error.hpp>

namespace orangutan::http {
namespace {

using orangutan::core::Error;

constexpr long kPollTimeoutMs = 50;

[[nodiscard]] bool is_cancelled(const asio::cancellation_state& cancellation) noexcept {
  return cancellation.cancelled() != asio::cancellation_type::none;
}

[[nodiscard]] bool is_http_url(std::string_view url) noexcept {
  return url.starts_with("http://") || url.starts_with("https://");
}

[[nodiscard]] bool is_known_method(std::string_view method) noexcept {
  return method == "GET" || method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE" ||
         method == "HEAD";
}

[[nodiscard]] Error curl_error(CURLcode code, std::string_view action) {
  const auto message = std::string{curl_easy_strerror(code)};
  switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
      return Error{core::ErrorKind::timeout, "http request timed out"}.with("curl_error", message);
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
      return Error::network("http transport failed").with("curl_error", message).with("action", std::string{action});
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
      return Error::network("http TLS verification failed")
          .with("curl_error", message)
          .with("action", std::string{action});
    default:
      return Error::upstream("http client failed").with("curl_error", message).with("action", std::string{action});
  }
}

[[nodiscard]] Error curl_multi_error(CURLMcode code, std::string_view action) {
  return Error::network("http multi transport failed")
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

[[nodiscard]] CurlGlobal& curl_global() {
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

  [[nodiscard]] core::Result<void> append(const Header& header) {
    if (header.name.empty()) {
      return std::unexpected(Error::invalid_argument("http header name must be non-empty"));
    }
    const auto line = header.name + ": " + header.value;
    auto* next = curl_slist_append(headers_, line.c_str());
    if (next == nullptr) {
      return std::unexpected(Error::internal("failed to allocate curl header list"));
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

[[nodiscard]] std::string_view trim_header_value(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  const auto bytes = size * count;
  body->append(data, bytes);
  return bytes;
}

std::size_t write_header(char* data, std::size_t size, std::size_t count, void* userdata) {
  auto* headers = static_cast<std::vector<Header>*>(userdata);
  const auto bytes = size * count;
  auto line = std::string_view{data, bytes};
  if (line.ends_with("\r\n")) {
    line.remove_suffix(2);
  } else if (line.ends_with('\n')) {
    line.remove_suffix(1);
  }

  const auto colon = line.find(':');
  if (colon == std::string_view::npos) {
    return bytes;
  }
  auto name = line.substr(0, colon);
  auto value = trim_header_value(line.substr(colon + 1));
  if (!name.empty()) {
    headers->push_back(Header{.name = std::string{name}, .value = std::string{value}});
  }
  return bytes;
}

[[nodiscard]] core::Result<void> validate_request(const BodyRequest& request) {
  if (request.url.empty()) {
    return std::unexpected(Error::invalid_argument("http request url must be non-empty"));
  }
  if (!is_http_url(request.url)) {
    return std::unexpected(Error::invalid_argument("http request url must use http or https").with("url", request.url));
  }
  if (!is_known_method(request.method)) {
    return std::unexpected(
        Error::invalid_argument("http request method is not supported").with("method", request.method));
  }
  if (request.timeout.count() <= 0) {
    return std::unexpected(Error::invalid_argument("http request timeout must be positive"));
  }
  if (request.timeout.count() > std::numeric_limits<long>::max()) {
    return std::unexpected(Error::invalid_argument("http request timeout is too large"));
  }
  return {};
}

[[nodiscard]] core::Result<void>
configure_easy(CURL* easy, const BodyRequest& request, CurlHeaders& headers, BodyResponse& response) {
  for (const auto& header : request.headers) {
    auto appended = headers.append(header);
    if (!appended) {
      return std::unexpected(std::move(appended).error());
    }
  }

  if (curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str()) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response.body) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_header) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response.headers) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.get()) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count())) != CURLE_OK) {
    return std::unexpected(Error::internal("failed to configure curl easy handle"));
  }

  if (request.method == "HEAD") {
    if (curl_easy_setopt(easy, CURLOPT_NOBODY, 1L) != CURLE_OK) {
      return std::unexpected(Error::internal("failed to configure curl HEAD request"));
    }
  } else if (request.method == "GET") {
    if (curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L) != CURLE_OK) {
      return std::unexpected(Error::internal("failed to configure curl GET request"));
    }
  } else {
    if (curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, request.method.c_str()) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.data()) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size())) != CURLE_OK) {
      return std::unexpected(Error::internal("failed to configure curl body request"));
    }
  }

  return {};
}

[[nodiscard]] core::Result<void> run_multi(CURLM* multi, CURL* easy, const asio::cancellation_state& cancellation) {
  auto add = curl_multi_add_handle(multi, easy);
  if (add != CURLM_OK) {
    return std::unexpected(curl_multi_error(add, "add_handle"));
  }
  auto registration = CurlEasyRegistration{multi, easy};

  int running = 0;
  auto code = curl_multi_perform(multi, &running);
  if (code != CURLM_OK) {
    return std::unexpected(curl_multi_error(code, "perform"));
  }

  while (running != 0) {
    if (is_cancelled(cancellation)) {
      return std::unexpected(Error::cancelled());
    }
    int active_fds = 0;
    code = curl_multi_poll(multi, nullptr, 0, kPollTimeoutMs, &active_fds);
    static_cast<void>(active_fds);
    if (code != CURLM_OK) {
      return std::unexpected(curl_multi_error(code, "poll"));
    }
    code = curl_multi_perform(multi, &running);
    if (code != CURLM_OK) {
      return std::unexpected(curl_multi_error(code, "perform"));
    }
  }

  CURLMsg* message = nullptr;
  int queued = 0;
  while ((message = curl_multi_info_read(multi, &queued)) != nullptr) {
    if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
      if (message->data.result != CURLE_OK) {
        return std::unexpected(curl_error(message->data.result, "perform"));
      }
      return {};
    }
  }

  return std::unexpected(Error::network("http request finished without curl completion message"));
}

[[nodiscard]] core::Result<std::uint16_t> response_code(CURL* easy) {
  long status = 0;
  const auto code = curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  if (code != CURLE_OK) {
    return std::unexpected(curl_error(code, "response_code"));
  }
  if (status < 0 || status > static_cast<long>(std::numeric_limits<std::uint16_t>::max())) {
    return std::unexpected(Error::upstream("http response status is out of range"));
  }
  return static_cast<std::uint16_t>(status);
}

}  // namespace

struct Client::Impl {
  explicit Impl(asio::any_io_executor executor) : blocking_executor{std::move(executor)} {}

  asio::any_io_executor blocking_executor;

  [[nodiscard]] core::Result<BodyResponse> send_blocking(const BodyRequest& request,
                                                         const asio::cancellation_state& cancellation) const {
    if (!curl_global().ok()) {
      return std::unexpected(Error::internal("curl global initialization failed"));
    }
    if (auto valid = validate_request(request); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    if (is_cancelled(cancellation)) {
      return std::unexpected(Error::cancelled());
    }

    auto easy = CurlEasy{};
    if (easy.get() == nullptr) {
      return std::unexpected(Error::internal("failed to allocate curl easy handle"));
    }
    auto multi = CurlMulti{};
    if (multi.get() == nullptr) {
      return std::unexpected(Error::internal("failed to allocate curl multi handle"));
    }

    auto response = BodyResponse{};
    auto headers = CurlHeaders{};
    if (auto configured = configure_easy(easy.get(), request, headers, response); !configured) {
      return std::unexpected(std::move(configured).error());
    }
    if (auto performed = run_multi(multi.get(), easy.get(), cancellation); !performed) {
      return std::unexpected(std::move(performed).error());
    }

    auto status = response_code(easy.get());
    if (!status) {
      return std::unexpected(std::move(status).error());
    }
    response.status_code = *status;
    return response;
  }
};

template <typename ImplPtr, typename CompletionToken>
auto async_send_on(asio::any_io_executor completion_executor,
                   asio::any_io_executor blocking_executor,
                   ImplPtr impl,
                   BodyRequest request,
                   asio::cancellation_state cancellation,
                   CompletionToken&& token) {
  return asio::async_initiate<CompletionToken, void(core::Result<BodyResponse>)>(
      [completion_executor = std::move(completion_executor),
       blocking_executor = std::move(blocking_executor),
       impl = std::move(impl),
       request = std::move(request),
       cancellation](auto handler) mutable {
        auto work = asio::make_work_guard(completion_executor);
        asio::post(blocking_executor,
                   [impl = std::move(impl),
                    request = std::move(request),
                    cancellation,
                    handler = std::move(handler),
                    work = std::move(work)]() mutable {
                     auto result = impl->send_blocking(request, cancellation);
                     auto completion_executor = work.get_executor();
                     asio::post(completion_executor,
                                [handler = std::move(handler),
                                 result = std::move(result),
                                 work = std::move(work)]() mutable { handler(std::move(result)); });
                   });
      },
      token);
}

Client::Client(asio::any_io_executor blocking_executor) : impl_{std::make_shared<Impl>(std::move(blocking_executor))} {}

Client::~Client() = default;

Client::Client(Client&&) noexcept = default;

Client& Client::operator=(Client&&) noexcept = default;

async::Awaitable<core::Result<BodyResponse>> Client::send(BodyRequest request) const {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto completion_executor = co_await asio::this_coro::executor;
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (is_cancelled(cancellation)) {
    co_return std::unexpected(Error::cancelled());
  }

  auto impl = impl_;
  co_return co_await async_send_on(std::move(completion_executor),
                                   impl->blocking_executor,
                                   std::move(impl),
                                   std::move(request),
                                   cancellation,
                                   asio::use_awaitable);
}

}  // namespace orangutan::http
