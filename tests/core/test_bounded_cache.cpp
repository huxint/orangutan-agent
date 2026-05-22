// tests/core/test_bounded_cache.cpp — BoundedCache coverage.

#include <chrono>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/bounded_cache.hpp>
#include <oran/core/time.hpp>

namespace core = orangutan::core;

namespace {

using namespace std::chrono_literals;

core::Time at_seconds(int seconds) {
  return core::Time{core::Time::clock::time_point{std::chrono::seconds{seconds}}};
}

struct StringByteCost {
  std::size_t operator()(const std::string& s) const noexcept {
    return s.size();
  }
};

struct PointerByteCost {
  std::size_t operator()(const std::unique_ptr<std::string>& p) const noexcept {
    return p ? p->size() : 0;
  }
};

}  // namespace

TEST_CASE("BoundedCache put/get returns the stored value on hit", "[unit][core][bounded_cache]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 4},
  };
  cache.put("a", 1, at_seconds(0));
  cache.put("b", 2, at_seconds(0));

  auto* a = cache.get("a", at_seconds(1));
  auto* b = cache.get("b", at_seconds(1));
  auto* missing = cache.get("c", at_seconds(1));

  REQUIRE(a != nullptr);
  REQUIRE(*a == 1);
  REQUIRE(b != nullptr);
  REQUIRE(*b == 2);
  REQUIRE(missing == nullptr);
  REQUIRE(cache.stats().hits == 2);
  REQUIRE(cache.stats().misses == 1);
}

TEST_CASE("BoundedCache evicts the least-recently-used entry on overflow", "[unit][core][bounded_cache]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 2},
  };
  cache.put("a", 1, at_seconds(0));
  cache.put("b", 2, at_seconds(1));
  // Touch 'a' so 'b' becomes the LRU candidate.
  REQUIRE(cache.get("a", at_seconds(2)) != nullptr);
  cache.put("c", 3, at_seconds(3));

  REQUIRE(cache.size() == 2);
  REQUIRE(cache.get("a", at_seconds(4)) != nullptr);
  REQUIRE(cache.get("c", at_seconds(4)) != nullptr);
  REQUIRE(cache.get("b", at_seconds(4)) == nullptr);
  REQUIRE(cache.stats().evictions_lru == 1);
}

TEST_CASE("BoundedCache TTL expires on get", "[unit][core][bounded_cache][ttl]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 4, .ttl = 5s},
  };
  cache.put("a", 1, at_seconds(0));
  REQUIRE(cache.get("a", at_seconds(4)) != nullptr);
  REQUIRE(cache.get("a", at_seconds(6)) == nullptr);
  REQUIRE(cache.stats().evictions_ttl == 1);
  REQUIRE(cache.stats().misses >= 1);
}

TEST_CASE("BoundedCache reap evicts expired entries in bulk", "[unit][core][bounded_cache][ttl]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 10, .ttl = 5s},
  };
  cache.put("old1", 1, at_seconds(0));
  cache.put("old2", 2, at_seconds(0));
  cache.put("fresh", 3, at_seconds(4));

  const auto evicted = cache.reap(at_seconds(7));
  REQUIRE(evicted == 2);
  REQUIRE(cache.size() == 1);
  REQUIRE(cache.get("fresh", at_seconds(7)) != nullptr);
  REQUIRE(cache.stats().evictions_ttl == 2);
}

TEST_CASE("BoundedCache reap is a no-op when TTL is disabled", "[unit][core][bounded_cache][ttl]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 10},
  };
  cache.put("a", 1, at_seconds(0));
  REQUIRE(cache.reap(at_seconds(999'999)) == 0);
  REQUIRE(cache.get("a", at_seconds(999'999)) != nullptr);
}

TEST_CASE("BoundedCache byte budget evicts oldest entries when over cap", "[unit][core][bounded_cache][bytes]") {
  using Cache = core::BoundedCache<std::string, std::string, StringByteCost>;
  Cache cache{Cache::Options{.max_entries = 100, .max_bytes = 8}, StringByteCost{}};
  cache.put("a", std::string{"AAAA"}, at_seconds(0));  // 4 bytes
  cache.put("b", std::string{"BBBB"}, at_seconds(1));  // 4 bytes — at the cap
  REQUIRE(cache.byte_size() == 8);
  cache.put("c", std::string{"C"}, at_seconds(2));  // pushes total to 9 -> evict oldest
  REQUIRE(cache.byte_size() <= 8);
  REQUIRE(cache.get("a", at_seconds(3)) == nullptr);
  REQUIRE(cache.get("b", at_seconds(3)) != nullptr);
  REQUIRE(cache.get("c", at_seconds(3)) != nullptr);
  REQUIRE(cache.stats().evictions_bytes >= 1);
}

TEST_CASE("BoundedCache refuses to cache items larger than the byte budget", "[unit][core][bounded_cache][bytes]") {
  using Cache = core::BoundedCache<std::string, std::string, StringByteCost>;
  Cache cache{Cache::Options{.max_entries = 4, .max_bytes = 4}, StringByteCost{}};
  cache.put("ok", std::string{"ABC"}, at_seconds(0));        // 3 bytes — fits
  cache.put("toobig", std::string{"ABCDE"}, at_seconds(1));  // 5 bytes — rejected

  REQUIRE(cache.size() == 1);
  REQUIRE(cache.get("ok", at_seconds(2)) != nullptr);
  REQUIRE(cache.get("toobig", at_seconds(2)) == nullptr);
  REQUIRE(cache.stats().rejected_oversize == 1);
}

TEST_CASE("BoundedCache oversize put invalidates the prior entry under the same key",
          "[unit][core][bounded_cache][bytes]") {
  using Cache = core::BoundedCache<std::string, std::string, StringByteCost>;
  Cache cache{Cache::Options{.max_entries = 4, .max_bytes = 4}, StringByteCost{}};
  cache.put("k", std::string{"AB"}, at_seconds(0));
  REQUIRE(cache.get("k", at_seconds(1)) != nullptr);
  cache.put("k", std::string{"OVERSIZED"}, at_seconds(2));  // rejected, but evicts the old value
  REQUIRE(cache.get("k", at_seconds(3)) == nullptr);
  REQUIRE(cache.stats().rejected_oversize == 1);
}

TEST_CASE("BoundedCache re-put under the same key refreshes the value", "[unit][core][bounded_cache]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 4},
  };
  cache.put("a", 1, at_seconds(0));
  cache.put("a", 99, at_seconds(1));
  auto* a = cache.get("a", at_seconds(2));
  REQUIRE(a != nullptr);
  REQUIRE(*a == 99);
  REQUIRE(cache.size() == 1);
}

TEST_CASE("BoundedCache supports move-only Value types", "[unit][core][bounded_cache][move_only]") {
  using Cache = core::BoundedCache<std::string, std::unique_ptr<std::string>, PointerByteCost>;
  Cache cache{Cache::Options{.max_entries = 4, .max_bytes = 16}, PointerByteCost{}};
  cache.put("k", std::make_unique<std::string>("hello"), at_seconds(0));

  auto* slot = cache.get("k", at_seconds(1));
  REQUIRE(slot != nullptr);
  REQUIRE(*slot != nullptr);
  REQUIRE(**slot == "hello");
}

TEST_CASE("BoundedCache clear empties the cache but preserves stat counters", "[unit][core][bounded_cache]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 4},
  };
  cache.put("a", 1, at_seconds(0));
  REQUIRE(cache.get("a", at_seconds(1)) != nullptr);
  REQUIRE(cache.get("missing", at_seconds(1)) == nullptr);

  cache.clear();
  REQUIRE(cache.empty());
  REQUIRE(cache.size() == 0);
  REQUIRE(cache.byte_size() == 0);
  REQUIRE(cache.get("a", at_seconds(2)) == nullptr);
  // The stat counters describe the cache lifetime and survive clear().
  REQUIRE(cache.stats().hits == 1);
  REQUIRE(cache.stats().misses == 2);  // one missing from before + one missing after clear
}

TEST_CASE("BoundedCache stats track every observable transition", "[unit][core][bounded_cache][stats]") {
  using Cache = core::BoundedCache<std::string, std::string, StringByteCost>;
  Cache cache{Cache::Options{.max_entries = 2, .max_bytes = 16, .ttl = 5s}, StringByteCost{}};

  cache.put("a", std::string{"abc"}, at_seconds(0));
  cache.put("b", std::string{"defg"}, at_seconds(0));
  cache.put("c", std::string{"hij"}, at_seconds(1));  // LRU evicts 'a'

  REQUIRE(cache.stats().current_entries == 2);
  REQUIRE(cache.stats().current_bytes == 7);  // "defg" + "hij"
  REQUIRE(cache.stats().evictions_lru == 1);

  REQUIRE(cache.get("a", at_seconds(2)) == nullptr);  // miss + 1
  REQUIRE(cache.get("b", at_seconds(2)) != nullptr);  // hit + 1

  REQUIRE(cache.get("b", at_seconds(99)) == nullptr);  // TTL expiry
  REQUIRE(cache.stats().evictions_ttl == 1);
}

TEST_CASE("BoundedCache returns nullptr for misses without changing other stats",
          "[unit][core][bounded_cache][stats]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 4},
  };
  REQUIRE(cache.get("anything", at_seconds(0)) == nullptr);
  REQUIRE(cache.stats().misses == 1);
  REQUIRE(cache.stats().hits == 0);
}

TEST_CASE("BoundedCache get refreshes LRU position", "[unit][core][bounded_cache]") {
  core::BoundedCache<std::string, int> cache{
      core::BoundedCache<std::string, int>::Options{.max_entries = 3},
  };
  cache.put("a", 1, at_seconds(0));
  cache.put("b", 2, at_seconds(0));
  cache.put("c", 3, at_seconds(0));
  REQUIRE(cache.get("a", at_seconds(1)) != nullptr);  // refresh 'a'
  cache.put("d", 4, at_seconds(2));                   // evicts 'b' (now the LRU)

  REQUIRE(cache.get("a", at_seconds(3)) != nullptr);
  REQUIRE(cache.get("b", at_seconds(3)) == nullptr);
  REQUIRE(cache.get("c", at_seconds(3)) != nullptr);
  REQUIRE(cache.get("d", at_seconds(3)) != nullptr);
}
