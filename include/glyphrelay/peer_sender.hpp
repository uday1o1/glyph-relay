#pragma once

#include "glyphrelay/control_protocol.hpp"
#include "glyphrelay/media_egress.hpp"
#include "glyphrelay/recording.hpp"
#include "glyphrelay/rtp_transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glyphrelay {

enum class PeerSenderEventKind {
  local_description,
  local_candidate,
  connected,
  disconnected,
  control_open,
  receiver_stats,
  remb,
  recovery_requested,
  ended,
  failed,
};

struct PeerSenderEvent {
  PeerSenderEventKind kind = PeerSenderEventKind::failed;
  std::string value;
  std::uint64_t number = 0;
  ReceiverControlStats receiver_stats;
};

struct PeerSenderConfig {
  std::string session_id;
  std::optional<std::string> bind_address;
  std::vector<std::string> ice_server_urls;
  std::function<void(const PeerSenderEvent &)> event_callback;
  std::function<void()> request_idr_with_parameter_sets;
};

struct PeerSenderDiagnostics {
  bool offer_accepted = false;
  bool connected = false;
  bool track_open = false;
  bool control_open = false;
  bool stopped = false;
  std::uint64_t media_epoch = 0;
  std::uint64_t dependency_epoch = 0;
  std::uint64_t access_units_sent = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t latest_remb_bps = 0;
  std::string reason;
  SenderControlDiagnostics control;
  RecoveryDiagnostics recovery;
  RetransmissionCacheSnapshot retransmission_cache;
  DatagramEgressSnapshot egress;
};

class PeerSender {
public:
  explicit PeerSender(PeerSenderConfig config);
  ~PeerSender();

  PeerSender(PeerSender &&) noexcept;
  PeerSender &operator=(PeerSender &&) noexcept;
  PeerSender(const PeerSender &) = delete;
  PeerSender &operator=(const PeerSender &) = delete;

  bool accept_offer(std::string_view sdp);
  bool add_remote_candidate(std::string_view encoded_candidate);
  bool send_access_unit(const RecordedAccessUnit &access_unit);
  void stop(std::string_view reason = "OWNER_STOP");
  PeerSenderDiagnostics diagnostics() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

std::string_view peer_sender_event_name(PeerSenderEventKind kind);

} // namespace glyphrelay
