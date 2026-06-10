// bench/channel-qq/scenarios/normalize_response.cpp
//
// A-vs-B comparison: normalizing an empty-body response (header capture only)
// vs. a JSON business-envelope body (header capture plus the nlohmann parse
// the biz-code extraction pays). This prices the per-response normalization
// cost the QQ client adds on top of `http::Client`, and justifies the
// empty-body guard ordering inside `normalize_api_response`.

#include <nanobench.h>

#include <oran/channel-qq/api_client.hpp>
#include <oran/http/client.hpp>

namespace orangutan::bench {

namespace {

namespace qq = orangutan::channel::qq;

[[nodiscard]] http::BodyResponse empty_body_response() {
  return http::BodyResponse{
      .status_code = 204,
      .headers = {{.name = "x-tps-trace-id", .value = "trace-bench-0001"},
                  {.name = "content-type", .value = "application/json"}},
      .body = {},
  };
}

[[nodiscard]] http::BodyResponse biz_envelope_response() {
  return http::BodyResponse{
      .status_code = 200,
      .headers = {{.name = "x-tps-trace-id", .value = "trace-bench-0001"},
                  {.name = "content-type", .value = "application/json"}},
      .body = R"({"code":11244,"message":"bot not in guild","data":{"channel_id":"42","detail":"padding"}})",
  };
}

}  // namespace

void register_channel_qq_normalize_response(ankerl::nanobench::Bench& b) {
  b.run("normalize: empty body (headers only)", [&] {
    auto normalized = qq::normalize_api_response(empty_body_response());
    ankerl::nanobench::doNotOptimizeAway(normalized);
  });

  b.run("normalize: JSON biz envelope (headers + parse)", [&] {
    auto normalized = qq::normalize_api_response(biz_envelope_response());
    ankerl::nanobench::doNotOptimizeAway(normalized);
  });
}

}  // namespace orangutan::bench
