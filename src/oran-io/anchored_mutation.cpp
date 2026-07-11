// src/oran-io/anchored_mutation.cpp — executor wrapper for dirfd mutations.

#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>

#include <exception>
#include <string>
#include <utility>

#include <oran/io/blocking.hpp>

namespace orangutan::io {

async::Awaitable<core::Result<void>>
write_text_file(asio::any_io_executor executor, FileMutation mutation, std::string contents, WriteTextOptions options) {
  co_return co_await run_blocking(
      std::move(executor),
      [mutation = std::move(mutation), contents = std::move(contents), options]() mutable {
        try {
          return mutation.write_text(contents, options);
        } catch (const std::exception& error) {
          return core::Result<void>{
              std::unexpected(core::Error::io("anchored write failed").with("detail", error.what()))};
        } catch (...) {
          return core::Result<void>{std::unexpected(core::Error::io("anchored write failed"))};
        }
      });
}

}  // namespace orangutan::io
