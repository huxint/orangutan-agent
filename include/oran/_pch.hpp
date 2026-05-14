// include/oran/_pch.hpp — project-wide precompiled header.
//
// Slice 0: stdlib stable headers only. The fmt/json_fwd entries documented in
// docs/rules/module-and-pch.md land with the slice that introduces a library
// consuming them (oran-log for fmt, oran-storage for json_fwd). Keeping the
// dependency surface minimal here keeps the slice-0 build self-contained.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
