#pragma once

#include <cstdint>

namespace orangutan::hook {

enum class Event : std::uint8_t {
  provider_request,
  provider_response,
  provider_error,
  provider_fallback,
  tool_before,
  tool_dispatched,
  tool_after,
  tool_error,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  permission_ask_rendered,
};

}  // namespace orangutan::hook
