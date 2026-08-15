#include "glyphrelay/share_command.hpp"

#include "glyphrelay/owner_signaling.hpp"
#include "glyphrelay/peer_sender.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace glyphrelay {
namespace {

constexpr std::size_t kMaximumShareEvents = 32U;
constexpr double kMinimumPacingTargetBps = 100'000.0;

double initial_pacing_target(std::uint64_t wire_cap_bps) {
  const double cap = static_cast<double>(wire_cap_bps);
  return std::max(cap - std::max(0.10 * cap, 64'000.0), kMinimumPacingTargetBps);
}

class WebRtcShareTransport final : public ShareTransport {
public:
  explicit WebRtcShareTransport(const ShareCommandOptions &options)
      : owner_(OwnerSignalingClientConfig{
            .origin = options.signaling_origin,
            .ca_certificate_pem_file =
                options.signaling_ca_path
                    ? std::optional<std::string>(options.signaling_ca_path->string())
                    : std::nullopt,
            .automatically_create_join = true,
        }),
        initial_pacing_target_bps_(initial_pacing_target(
            record_bitrate_bps(options.bitrate_profile).value_or(4'000'000U))) {}

  ~WebRtcShareTransport() override { stop("SHARE_TRANSPORT_DESTROYED"); }

  bool start() override {
    {
      std::scoped_lock lock(mutex_);
      if (diagnostics_.started || diagnostics_.stopped) {
        diagnostics_.reason = "SHARE_TRANSPORT_START_STATE_INVALID";
        return false;
      }
    }
    const bool started = owner_.start();
    std::scoped_lock lock(mutex_);
    diagnostics_.started = started;
    diagnostics_.reason = started ? "SHARE_OWNER_SIGNALING_STARTED" : owner_.diagnostics().reason;
    return started;
  }

  std::optional<ShareTransportEvent> poll_event(std::chrono::milliseconds timeout) override {
    if (auto pending = pop_event()) {
      return pending;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      const auto remaining = deadline > std::chrono::steady_clock::now()
                                 ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                 : std::chrono::milliseconds(0);
      const auto event = owner_.wait_for_event(remaining);
      if (!event) {
        return pop_event();
      }
      handle_owner_event(*event);
      if (auto mapped = pop_event()) {
        return mapped;
      }
      if (timeout.count() == 0 || std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
    }
  }

  bool send_access_unit(const RecordedAccessUnit &access_unit) override {
    std::shared_ptr<PeerSender> peer;
    {
      std::scoped_lock lock(mutex_);
      if (!diagnostics_.peer_ready || diagnostics_.stopped || !peer_ || !access_unit.bytes) {
        diagnostics_.reason = "SHARE_PEER_NOT_READY";
        return false;
      }
      if (diagnostics_.access_units_sent == std::numeric_limits<std::uint64_t>::max() ||
          access_unit.bytes->size() >
              static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max() -
                                       diagnostics_.bytes_sent)) {
        diagnostics_.reason = "SHARE_TRANSPORT_COUNTER_OVERFLOW";
        return false;
      }
      peer = peer_;
    }
    if (!peer->send_access_unit(access_unit)) {
      const auto reason = peer->diagnostics().reason;
      std::scoped_lock lock(mutex_);
      diagnostics_.reason = reason;
      return false;
    }
    std::scoped_lock lock(mutex_);
    ++diagnostics_.access_units_sent;
    diagnostics_.bytes_sent += static_cast<std::uint64_t>(access_unit.bytes->size());
    diagnostics_.reason = "SHARE_ACCESS_UNIT_SENT";
    return true;
  }

  bool set_pacing_target_bits_per_second(double target_bits_per_second) override {
    std::shared_ptr<PeerSender> peer;
    {
      std::scoped_lock lock(mutex_);
      if (!diagnostics_.peer_ready || diagnostics_.stopped || !peer_) {
        diagnostics_.reason = "SHARE_PEER_NOT_READY";
        return false;
      }
      peer = peer_;
    }
    try {
      peer->set_pacing_target_bits_per_second(target_bits_per_second);
      return true;
    } catch (const std::exception &error) {
      std::scoped_lock lock(mutex_);
      diagnostics_.reason = error.what();
      return false;
    }
  }

  void stop(std::string_view reason) override {
    std::shared_ptr<PeerSender> peer;
    bool stop_owner = false;
    {
      std::scoped_lock lock(mutex_);
      if (diagnostics_.stopped) {
        return;
      }
      diagnostics_.stopped = true;
      diagnostics_.peer_ready = false;
      diagnostics_.reason = std::string(reason);
      stopping_ = true;
      peer = std::move(peer_);
      stop_owner = diagnostics_.started;
      events_.clear();
    }
    if (peer) {
      peer->stop(reason);
      const auto peer_diagnostics = peer->diagnostics();
      std::scoped_lock lock(mutex_);
      diagnostics_.peer = peer_diagnostics;
    }
    if (stop_owner) {
      owner_.stop(true);
    }
  }

  ShareTransportDiagnostics diagnostics() const override {
    std::shared_ptr<PeerSender> peer;
    ShareTransportDiagnostics result;
    {
      std::scoped_lock lock(mutex_);
      result = diagnostics_;
      peer = peer_;
    }
    if (peer) {
      result.peer = peer->diagnostics();
    }
    return result;
  }

private:
  void handle_owner_event(const OwnerSignalingEvent &event) {
    switch (event.kind) {
    case OwnerSignalingEventKind::session_created: {
      std::scoped_lock lock(mutex_);
      if (!session_id_.empty() && session_id_ != event.value) {
        fail_locked("SHARE_SESSION_ID_CHANGED");
        return;
      }
      session_id_ = event.value;
      push_locked({ShareTransportEventKind::session_created, event.value, {}});
      break;
    }
    case OwnerSignalingEventKind::join_created:
      push({ShareTransportEventKind::join_created, event.value, {}});
      break;
    case OwnerSignalingEventKind::receiver_reserved:
      break;
    case OwnerSignalingEventKind::receiver_offer:
      accept_offer(event.value);
      break;
    case OwnerSignalingEventKind::receiver_restart_offer:
      fail("SHARE_ICE_RESTART_REQUIRES_NEW_PEER");
      break;
    case OwnerSignalingEventKind::receiver_candidate:
      add_candidate(event.value);
      break;
    case OwnerSignalingEventKind::receiver_disconnected:
      push({ShareTransportEventKind::peer_disconnected,
            event.value.empty() ? "RECEIVER_SIGNALING_DISCONNECTED" : event.value,
            {}});
      break;
    case OwnerSignalingEventKind::terminal:
      push({ShareTransportEventKind::terminal,
            event.value.empty() ? owner_.diagnostics().reason : event.value,
            {}});
      break;
    }
  }

  void accept_offer(const std::string &sdp) {
    std::string session_id;
    {
      std::scoped_lock lock(mutex_);
      if (session_id_.empty() || peer_ || diagnostics_.stopped) {
        fail_locked("SHARE_RECEIVER_OFFER_STATE_INVALID");
        return;
      }
      session_id = session_id_;
    }
    std::shared_ptr<PeerSender> peer;
    try {
      peer = std::make_shared<PeerSender>(PeerSenderConfig{
          .session_id = std::move(session_id),
          .bind_address = std::nullopt,
          .ice_server_urls = {},
          .event_callback = [this](const PeerSenderEvent &event) { handle_peer_event(event); },
          .request_idr_with_parameter_sets =
              [this] {
                push({ShareTransportEventKind::recovery_requested, "PEER_REQUESTED_IDR", {}});
              },
          .initial_pacing_target_bps = initial_pacing_target_bps_,
      });
    } catch (const std::exception &error) {
      fail(std::string("SHARE_PEER_CREATE_FAILED:") + error.what());
      return;
    }
    auto raw_peer = peer;
    {
      std::scoped_lock lock(mutex_);
      if (diagnostics_.stopped) {
        return;
      }
      peer_ = std::move(peer);
    }
    if (!raw_peer->accept_offer(sdp)) {
      fail(raw_peer->diagnostics().reason);
    }
  }

  void add_candidate(const std::string &candidate) {
    std::shared_ptr<PeerSender> peer;
    {
      std::scoped_lock lock(mutex_);
      if (!peer_ || diagnostics_.stopped) {
        fail_locked("SHARE_RECEIVER_CANDIDATE_STATE_INVALID");
        return;
      }
      peer = peer_;
    }
    if (!peer->add_remote_candidate(candidate)) {
      fail(peer->diagnostics().reason);
    }
  }

  void handle_peer_event(const PeerSenderEvent &event) {
    switch (event.kind) {
    case PeerSenderEventKind::local_description:
      if (!owner_.send_answer(event.value, false)) {
        fail(owner_.diagnostics().reason);
      }
      break;
    case PeerSenderEventKind::local_candidate:
      if (!owner_.send_candidate(event.value)) {
        fail(owner_.diagnostics().reason);
      }
      break;
    case PeerSenderEventKind::connected:
      set_peer_flag(PeerFlag::connected);
      break;
    case PeerSenderEventKind::track_open:
      set_peer_flag(PeerFlag::track);
      break;
    case PeerSenderEventKind::control_open:
      set_peer_flag(PeerFlag::control);
      break;
    case PeerSenderEventKind::disconnected:
      push({ShareTransportEventKind::peer_disconnected, "PEER_DISCONNECTED", {}});
      break;
    case PeerSenderEventKind::recovery_requested:
      push({ShareTransportEventKind::recovery_requested, event.value, {}});
      break;
    case PeerSenderEventKind::ended: {
      std::scoped_lock lock(mutex_);
      if (!stopping_) {
        push_locked({ShareTransportEventKind::terminal, event.value, {}});
      }
      break;
    }
    case PeerSenderEventKind::failed:
      fail(event.value);
      break;
    case PeerSenderEventKind::receiver_stats:
      push({ShareTransportEventKind::feedback,
            {},
            {.loss_fraction = std::nullopt,
             .round_trip_time_milliseconds = std::nullopt,
             .remb_bits_per_second = std::nullopt,
             .remb_payload_type_valid = false,
             .remb_rtcp_source_valid = false,
             .receiver_stats = event.receiver_stats}});
      break;
    case PeerSenderEventKind::receiver_report:
      push({ShareTransportEventKind::feedback,
            {},
            {.loss_fraction = event.loss_fraction,
             .round_trip_time_milliseconds = event.round_trip_time_milliseconds,
             .remb_bits_per_second = std::nullopt,
             .remb_payload_type_valid = false,
             .remb_rtcp_source_valid = false,
             .receiver_stats = std::nullopt}});
      break;
    case PeerSenderEventKind::remb:
      push({ShareTransportEventKind::feedback,
            {},
            {.loss_fraction = std::nullopt,
             .round_trip_time_milliseconds = std::nullopt,
             .remb_bits_per_second = static_cast<double>(event.number),
             .remb_payload_type_valid = true,
             .remb_rtcp_source_valid = true,
             .receiver_stats = std::nullopt}});
      break;
    }
  }

  enum class PeerFlag { connected, track, control };

  void set_peer_flag(PeerFlag flag) {
    std::scoped_lock lock(mutex_);
    switch (flag) {
    case PeerFlag::connected:
      peer_connected_ = true;
      break;
    case PeerFlag::track:
      peer_track_open_ = true;
      break;
    case PeerFlag::control:
      peer_control_open_ = true;
      break;
    }
    if (!diagnostics_.peer_ready && peer_connected_ && peer_track_open_ && peer_control_open_) {
      diagnostics_.peer_ready = true;
      diagnostics_.reason = "SHARE_PEER_READY";
      push_locked({ShareTransportEventKind::peer_ready, "PEER_READY", {}});
    }
  }

  void fail(std::string reason) {
    std::scoped_lock lock(mutex_);
    fail_locked(std::move(reason));
  }

  void fail_locked(std::string reason) {
    diagnostics_.peer_ready = false;
    diagnostics_.reason = reason;
    push_locked({ShareTransportEventKind::terminal, std::move(reason), {}});
  }

  void push(ShareTransportEvent event) {
    std::scoped_lock lock(mutex_);
    push_locked(std::move(event));
  }

  void push_locked(ShareTransportEvent event) {
    if (diagnostics_.stopped) {
      return;
    }
    if (events_.size() >= kMaximumShareEvents) {
      events_.clear();
      diagnostics_.peer_ready = false;
      diagnostics_.reason = "SHARE_EVENT_QUEUE_OVERFLOW";
      events_.push_back({ShareTransportEventKind::terminal, diagnostics_.reason, {}});
      return;
    }
    events_.push_back(std::move(event));
  }

  std::optional<ShareTransportEvent> pop_event() {
    std::scoped_lock lock(mutex_);
    if (events_.empty()) {
      return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
  }

  mutable std::mutex mutex_;
  OwnerSignalingClient owner_;
  std::shared_ptr<PeerSender> peer_;
  std::deque<ShareTransportEvent> events_;
  ShareTransportDiagnostics diagnostics_;
  std::string session_id_;
  bool peer_connected_ = false;
  bool peer_track_open_ = false;
  bool peer_control_open_ = false;
  bool stopping_ = false;
  double initial_pacing_target_bps_ = 4'000'000.0;
};

} // namespace

ShareRunResult run_interactive_share(const ShareCommandOptions &options,
                                     RecordStopPredicate stop_requested,
                                     ShareStatusCallback status) {
  std::string reason;
  if (!valid_share_options(options, reason)) {
    ShareRunResult result;
    result.exit_code = 2;
    result.reason = std::move(reason);
    return result;
  }
  std::unique_ptr<WebRtcShareTransport> transport;
  try {
    transport = std::make_unique<WebRtcShareTransport>(options);
  } catch (const std::exception &error) {
    ShareRunResult result;
    result.exit_code = 2;
    result.reason = error.what();
    return result;
  }
  auto source = make_interactive_frame_source();
  if (!source) {
    ShareRunResult result;
    result.exit_code = 3;
    result.reason = "sharing_requires_linux_portal";
    return result;
  }
  return run_share_pipeline(options, *source, *transport, std::move(stop_requested),
                            std::move(status));
}

} // namespace glyphrelay
