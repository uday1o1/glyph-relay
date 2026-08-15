/**
 * Copyright (c) 2026 GlyphRelay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "glyphrelay/media_pacer.hpp"
#include "glyphrelay/rtp_transport.hpp"

#include <rtc/h264rtppacketizer.hpp>
#include <rtc/mediahandler.hpp>
#include <rtc/rtppacketizationconfig.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace glyphrelay::rtc_adapter {

class StrictH264Packetizer final : public rtc::MediaHandler {
public:
  explicit StrictH264Packetizer(std::shared_ptr<rtc::RtpPacketizationConfig> config);

  void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override;

  std::optional<std::string> take_last_rejection();

private:
  std::shared_ptr<rtc::RtpPacketizationConfig> config_;
  rtc::H264RtpPacketizer packetizer_;
  std::mutex mutex_;
  std::optional<std::uint64_t> last_extended_timestamp_;
  std::optional<std::string> last_rejection_;
};

class BoundedNackResponder final : public rtc::MediaHandler {
public:
  using Clock = std::function<std::uint64_t()>;
  using Callback = std::function<void()>;
  using NetworkFeedbackCallback = std::function<void(std::optional<double>, std::optional<double>)>;

  BoundedNackResponder(std::uint32_t media_ssrc, Clock now_milliseconds,
                       Callback request_idr_with_parameter_sets, Callback terminate_session,
                       NetworkFeedbackCallback network_feedback = {});
  ~BoundedNackResponder() override;

  bool begin_epoch(std::uint64_t media_epoch, std::uint64_t dependency_epoch,
                   RecoveryTrigger trigger);
  bool request_forced_idr(RecoveryTrigger trigger);
  void set_target_bits_per_second(double target_bits_per_second);
  void stop();

  void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
  void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override;

  RecoveryDiagnostics diagnostics() const;
  RetransmissionCacheSnapshot cache_snapshot() const;
  MediaPacerSnapshot pacer_snapshot() const;
  std::uint64_t retransmission_bytes_sent() const;
  std::uint64_t malformed_feedback_messages() const;
  std::optional<std::string> take_last_pacer_rejection();

private:
  void drain_locked(rtc::message_vector &messages, std::uint64_t now_milliseconds);
  void run_pacer();

  std::uint32_t media_ssrc_;
  Clock now_milliseconds_;
  Callback request_idr_with_parameter_sets_;
  Callback terminate_session_;
  NetworkFeedbackCallback network_feedback_;
  mutable std::mutex mutex_;
  std::condition_variable pacer_changed_;
  RetransmissionCache cache_;
  MediaPacerQueue pacer_;
  RtpRecoveryController recovery_;
  rtc::message_callback send_callback_;
  std::thread pacer_worker_;
  std::uint64_t media_epoch_ = 0;
  std::uint64_t dependency_epoch_ = 0;
  std::uint64_t retransmission_bytes_sent_ = 0;
  std::uint64_t malformed_feedback_messages_ = 0;
  std::optional<std::string> last_pacer_rejection_;
  bool termination_notified_ = false;
  bool pacer_stopping_ = false;
};

} // namespace glyphrelay::rtc_adapter
