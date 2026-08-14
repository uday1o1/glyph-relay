#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glyphrelay {

inline constexpr std::string_view kSignalingProtocol = "glyphrelay-signal-v1";

enum class OwnerSignalingPhase {
  idle,
  creating,
  owner_only,
  join_pending,
  join_open,
  join_reserved,
  negotiating,
  connected,
  restart_negotiating,
  revoked,
  expired,
  failed,
  stopped,
};

enum class OwnerSignalingEventKind {
  session_created,
  join_created,
  receiver_reserved,
  receiver_offer,
  receiver_restart_offer,
  receiver_candidate,
  receiver_disconnected,
  terminal,
};

struct OwnerSignalingEvent {
  OwnerSignalingEventKind kind = OwnerSignalingEventKind::terminal;
  std::string value;
};

struct OwnerSignalingOutput {
  bool valid = true;
  bool close_transport = false;
  std::vector<std::string> outbound_messages;
  std::vector<OwnerSignalingEvent> events;
};

struct OwnerSignalingDiagnostics {
  OwnerSignalingPhase phase = OwnerSignalingPhase::idle;
  bool transport_open = false;
  bool owner_capability_present = false;
  std::uint64_t client_sequence = 0;
  std::uint64_t server_sequence = 0;
  std::string session_id;
  std::string reason;
};

class OwnerSignalingProtocol {
public:
  explicit OwnerSignalingProtocol(std::string expected_origin);
  ~OwnerSignalingProtocol();

  OwnerSignalingProtocol(const OwnerSignalingProtocol &) = delete;
  OwnerSignalingProtocol &operator=(const OwnerSignalingProtocol &) = delete;

  OwnerSignalingOutput begin(bool automatically_create_join);
  OwnerSignalingOutput receive(std::string_view message);
  OwnerSignalingOutput create_join();
  OwnerSignalingOutput send_answer(std::string_view sdp, bool restart);
  OwnerSignalingOutput send_candidate(std::string_view candidate);
  OwnerSignalingOutput stop(bool revoke);
  OwnerSignalingOutput transport_failed(std::string reason);
  OwnerSignalingDiagnostics diagnostics() const;
  bool active() const;

private:
  OwnerSignalingOutput fail(std::string reason);
  OwnerSignalingOutput authenticated_message(std::string_view type,
                                             std::string_view field_name = {},
                                             std::string_view field_value = {});
  void clear_owner_capability();

  std::string expected_origin_;
  std::string owner_capability_;
  std::string session_id_;
  OwnerSignalingPhase phase_ = OwnerSignalingPhase::idle;
  std::uint64_t client_sequence_ = 0;
  std::uint64_t server_sequence_ = 0;
  std::string reason_ = "OWNER_SIGNALING_IDLE";
  bool automatically_create_join_ = false;
};

struct OwnerSignalingClientConfig {
  std::string origin;
  std::optional<std::string> ca_certificate_pem_file;
  bool automatically_create_join = true;
};

class OwnerSignalingClient {
public:
  explicit OwnerSignalingClient(OwnerSignalingClientConfig config);
  ~OwnerSignalingClient();

  OwnerSignalingClient(OwnerSignalingClient &&) noexcept;
  OwnerSignalingClient &operator=(OwnerSignalingClient &&) noexcept;
  OwnerSignalingClient(const OwnerSignalingClient &) = delete;
  OwnerSignalingClient &operator=(const OwnerSignalingClient &) = delete;

  bool start();
  bool create_join();
  bool send_answer(std::string_view sdp, bool restart = false);
  bool send_candidate(std::string_view candidate);
  void stop(bool revoke = false);
  std::optional<OwnerSignalingEvent> wait_for_event(std::chrono::milliseconds timeout);
  OwnerSignalingDiagnostics diagnostics() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

std::string_view owner_signaling_phase_name(OwnerSignalingPhase phase);

} // namespace glyphrelay
