#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::config {
using SecretLookup = std::function<core::Result<std::string>(std::string_view)>;
}  // namespace orangutan::config
