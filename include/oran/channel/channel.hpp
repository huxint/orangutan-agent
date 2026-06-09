// include/oran/channel/channel.hpp — channel adapter trait and message envelopes.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/content.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::channel {

struct Capabilities {
  bool text{true};
  bool rich_text{false};
  bool attachments_image{false};
  bool attachments_file{false};
  bool attachments_audio{false};
  bool attachments_video{false};
  bool reactions{false};
  bool mentions{false};
  bool threads{false};
  bool ephemeral_messages{false};
  bool typing_indicator{false};
  bool message_edit{false};
  bool message_delete{false};
  bool reply_quoting{false};
  std::size_t max_text_bytes{4 * 1024};

  friend bool operator==(const Capabilities&, const Capabilities&) = default;
};

struct Origin {
  std::string kind;
  std::string source;

  friend bool operator==(const Origin&, const Origin&) = default;
};

struct Reference {
  std::string message_id;
  std::optional<std::string> thread_id{};

  friend bool operator==(const Reference&, const Reference&) = default;
};

struct Reaction {
  std::string name;

  friend bool operator==(const Reaction&, const Reaction&) = default;
};

struct DeliveryHint {
  bool ephemeral{false};
  bool high_priority{false};
  bool mention_user{false};

  friend bool operator==(const DeliveryHint&, const DeliveryHint&) = default;
};

struct InboundMessage {
  std::string channel_id;
  std::string conversation_id;
  std::string user_id;
  std::string display_name;
  std::vector<core::Content> content;
  std::vector<Reference> replies_to;
  core::Time received_at{};
  Origin origin{};
  Capabilities caps{};

  friend bool operator==(const InboundMessage&, const InboundMessage&) = default;
};

struct OutboundMessage {
  std::string conversation_id;
  std::vector<core::Content> content;
  std::optional<std::string> reply_to_message_id{};
  std::optional<std::string> thread_id{};
  std::vector<Reaction> reactions;
  DeliveryHint hint{};

  friend bool operator==(const OutboundMessage&, const OutboundMessage&) = default;
};

struct DeliveryReceipt {
  std::string message_id;
  core::Time accepted_at{};

  friend bool operator==(const DeliveryReceipt&, const DeliveryReceipt&) = default;
};

class Channel {
public:
  virtual ~Channel() = default;

  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;
  [[nodiscard]] virtual Capabilities capabilities() const noexcept = 0;

  [[nodiscard]] virtual async::Awaitable<core::Result<void>> start() = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> stop() = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<InboundMessage>> next_message() = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<DeliveryReceipt>> send(OutboundMessage message) = 0;
};

}  // namespace orangutan::channel
