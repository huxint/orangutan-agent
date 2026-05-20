// src/oran-tool/builtins.cpp — aggregate registrar.

#include <oran/tool/builtins.hpp>

#include <oran/core/result.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::tool {

core::Result<void> register_builtins(Registry& registry) {
  if (auto r = register_file_read(registry); !r) {
    return r;
  }
  if (auto r = register_file_write(registry); !r) {
    return r;
  }
  if (auto r = register_file_edit(registry); !r) {
    return r;
  }
  if (auto r = register_file_search(registry); !r) {
    return r;
  }
  if (auto r = register_directory_list(registry); !r) {
    return r;
  }
  if (auto r = register_file_delete(registry); !r) {
    return r;
  }
  return {};
}

}  // namespace orangutan::tool
