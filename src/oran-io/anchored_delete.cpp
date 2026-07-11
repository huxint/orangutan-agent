// src/oran-io/anchored_delete.cpp — executor wrapper for anchored deletes.

#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>

#include <exception>
#include <utility>

#include <oran/io/blocking.hpp>

namespace orangutan::io {

async::Awaitable<core::Result<DeletePathResult>>
delete_path(asio::any_io_executor executor, DeleteMutation mutation, DeletePathOptions options) {
  // Cancellation note: run_blocking observes cancellation before traversal,
  // but POSIX directory enumeration is synchronous and this first authority
  // backend does not interrupt an in-progress recursive walk. The operation
  // reports partial progress on errors; a future batched walker can add
  // between-entry suspension without weakening descriptor confinement.
  co_return co_await run_blocking(std::move(executor), [mutation = std::move(mutation), options]() mutable {
    try {
      return mutation.delete_path(options);
    } catch (const std::exception& error) {
      return core::Result<DeletePathResult>{
          std::unexpected(core::Error::io("anchored delete failed").with("detail", error.what()))};
    } catch (...) {
      return core::Result<DeletePathResult>{std::unexpected(core::Error::io("anchored delete failed"))};
    }
  });
}

}  // namespace orangutan::io
