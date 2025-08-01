/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/source_tracker.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "api/rtp_packet_info.h"
#include "api/rtp_packet_infos.h"
#include "api/transport/rtp/rtp_source.h"
#include "api/units/timestamp.h"
#include "rtc_base/checks.h"
#include "rtc_base/trace_event.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {

SourceTracker::SourceTracker(Clock* clock) : clock_(clock) {
  RTC_DCHECK(clock_);
}

void SourceTracker::OnFrameDelivered(const RtpPacketInfos& packet_infos,
                                     Timestamp delivery_time) {
  TRACE_EVENT0("webrtc", "SourceTracker::OnFrameDelivered");
  if (packet_infos.empty()) {
    return;
  }
  if (delivery_time.IsInfinite()) {
    delivery_time = clock_->CurrentTime();
  }

  for (const RtpPacketInfo& packet_info : packet_infos) {
    for (uint32_t csrc : packet_info.csrcs()) {
      SourceKey key(RtpSourceType::CSRC, csrc);
      SourceEntry& entry = UpdateEntry(key);

      const auto packet_time = packet_info.receive_time();
      entry.timestamp = packet_time.ms() ? packet_time : delivery_time;
      entry.audio_level = packet_info.audio_level();
      entry.absolute_capture_time = packet_info.absolute_capture_time();
      entry.local_capture_clock_offset =
          packet_info.local_capture_clock_offset();
      entry.rtp_timestamp = packet_info.rtp_timestamp();
    }

    SourceKey key(RtpSourceType::SSRC, packet_info.ssrc());
    SourceEntry& entry = UpdateEntry(key);

    entry.timestamp = delivery_time;
    entry.audio_level = packet_info.audio_level();
    entry.absolute_capture_time = packet_info.absolute_capture_time();
    entry.local_capture_clock_offset = packet_info.local_capture_clock_offset();
    entry.rtp_timestamp = packet_info.rtp_timestamp();
  }

  PruneEntries(delivery_time);
}

std::vector<RtpSource> SourceTracker::GetSources() const {
  PruneEntries(clock_->CurrentTime());

  std::vector<RtpSource> sources;
  for (const auto& pair : list_) {
    const SourceKey& key = pair.first;
    const SourceEntry& entry = pair.second;

    sources.emplace_back(
        entry.timestamp, key.source, key.source_type, entry.rtp_timestamp,
        RtpSource::Extensions{
            .audio_level = entry.audio_level,
            .absolute_capture_time = entry.absolute_capture_time,
            .local_capture_clock_offset = entry.local_capture_clock_offset});
  }

  std::sort(sources.begin(), sources.end(), [](const auto &a, const auto &b){
    return a.timestamp().ms() > b.timestamp().ms();
  });

  return sources;
}

SourceTracker::SourceEntry& SourceTracker::UpdateEntry(const SourceKey& key) {
  // We intentionally do |find() + emplace()|, instead of checking the return
  // value of `emplace()`, for performance reasons. It's much more likely for
  // the key to already exist than for it not to.
  auto map_it = map_.find(key);
  if (map_it == map_.end()) {
    // Insert a new entry at the front of the list.
    list_.emplace_front(key, SourceEntry());
    map_.emplace(key, list_.begin());
  } else if (map_it->second != list_.begin()) {
    // Move the old entry to the front of the list.
    list_.splice(list_.begin(), list_, map_it->second);
  }

  return list_.front().second;
}

void SourceTracker::PruneEntries(Timestamp now) const {
  if (now < Timestamp::Zero() + kTimeout) {
    return;
  }
  Timestamp prune = now - kTimeout;
  while (!list_.empty() && list_.back().second.timestamp < prune) {
    map_.erase(list_.back().first);
    list_.pop_back();
  }
}

}  // namespace webrtc
