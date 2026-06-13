// include/oran/channel-qq.hpp — umbrella header for the optional QQ channel adapter.
//
// Built only under `xmake f --channel_qq=y`; see
// docs/exec-plans/active/2026-06-10-channel-qq-port.md for the port plan.

#pragma once

#include <oran/channel-qq/api_client.hpp>
#include <oran/channel-qq/gateway.hpp>
#include <oran/channel-qq/gateway_transport.hpp>
#include <oran/channel-qq/token_store.hpp>
