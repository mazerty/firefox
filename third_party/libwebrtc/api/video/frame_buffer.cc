/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "api/video/frame_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>

#include "absl/algorithm/container.h"
#include "absl/container/inlined_vector.h"
#include "api/array_view.h"
#include "api/field_trials_view.h"
#include "api/video/encoded_frame.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/sequence_number_util.h"
#include "rtc_base/trace_event.h"

namespace webrtc {
namespace {
bool ValidReferences(const EncodedFrame& frame) {
  // All references must point backwards, and duplicates are not allowed.
  for (size_t i = 0; i < frame.num_references; ++i) {
    if (frame.references[i] >= frame.Id())
      return false;

    for (size_t j = i + 1; j < frame.num_references; ++j) {
      if (frame.references[i] == frame.references[j])
        return false;
    }
  }

  return true;
}

// Since FrameBuffer::FrameInfo is private it can't be used in the function
// signature, hence the FrameIteratorT type.
template <typename FrameIteratorT>
ArrayView<const int64_t> GetReferences(const FrameIteratorT& it) {
  return {it->second.encoded_frame->references,
          std::min<size_t>(it->second.encoded_frame->num_references,
                           EncodedFrame::kMaxFrameReferences)};
}

template <typename FrameIteratorT>
int64_t GetFrameId(const FrameIteratorT& it) {
  return it->first;
}

template <typename FrameIteratorT>
uint32_t GetTimestamp(const FrameIteratorT& it) {
  return it->second.encoded_frame->RtpTimestamp();
}

template <typename FrameIteratorT>
bool IsLastFrameInTemporalUnit(const FrameIteratorT& it) {
  return it->second.encoded_frame->is_last_spatial_layer;
}
}  // namespace

FrameBuffer::FrameBuffer(int max_size,
                         int max_decode_history,
                         const FieldTrialsView& field_trials)
    : legacy_frame_id_jump_behavior_(
          !field_trials.IsDisabled("WebRTC-LegacyFrameIdJumpBehavior")),
      max_size_(max_size),
      decoded_frame_history_(max_decode_history) {}

bool FrameBuffer::InsertFrame(std::unique_ptr<EncodedFrame> frame) {
  const uint32_t ssrc =
      frame->PacketInfos().empty() ? 0 : frame->PacketInfos()[0].ssrc();
  if (!ValidReferences(*frame)) {
    TRACE_EVENT2("webrtc",
                 "FrameBuffer::InsertFrame Frame dropped (Invalid references)",
                 "remote_ssrc", ssrc, "frame_id", frame->Id());
    RTC_DLOG(LS_WARNING) << "Frame " << frame->Id()
                         << " has invalid references, dropping frame.";
    return false;
  }

  if (frame->Id() <= decoded_frame_history_.GetLastDecodedFrameId()) {
    if (legacy_frame_id_jump_behavior_ && frame->is_keyframe() &&
        AheadOf(frame->RtpTimestamp(),
                *decoded_frame_history_.GetLastDecodedFrameTimestamp())) {
      TRACE_EVENT2("webrtc",
                   "FrameBuffer::InsertFrame Frames dropped (OOO + PicId jump)",
                   "remote_ssrc", ssrc, "frame_id", frame->Id());
      RTC_DLOG(LS_WARNING)
          << "Keyframe " << frame->Id()
          << " has newer timestamp but older picture id, clearing buffer.";
      Clear();
    } else {
      // Already decoded past this frame.
      TRACE_EVENT2("webrtc",
                   "FrameBuffer::InsertFrame Frame dropped (Out of order)",
                   "remote_ssrc", ssrc, "frame_id", frame->Id());
      return false;
    }
  }

  if (frames_.size() == max_size_) {
    if (frame->is_keyframe()) {
      TRACE_EVENT2("webrtc",
                   "FrameBuffer::InsertFrame Frames dropped (KF + Full buffer)",
                   "remote_ssrc", ssrc, "frame_id", frame->Id());
      RTC_DLOG(LS_WARNING) << "Keyframe " << frame->Id()
                           << " inserted into full buffer, clearing buffer.";
      Clear();
    } else {
      // No space for this frame.
      TRACE_EVENT2("webrtc",
                   "FrameBuffer::InsertFrame Frame dropped (Full buffer)",
                   "remote_ssrc", ssrc, "frame_id", frame->Id());
      return false;
    }
  }

  const int64_t frame_id = frame->Id();
  auto insert_res = frames_.emplace(frame_id, FrameInfo{std::move(frame)});
  if (!insert_res.second) {
    // Frame has already been inserted.
    return false;
  }

  if (frames_.size() == max_size_) {
    RTC_DLOG(LS_WARNING) << "Frame " << frame_id
                         << " inserted, buffer is now full.";
  }

  PropagateContinuity(insert_res.first);
  FindNextAndLastDecodableTemporalUnit();
  return true;
}

absl::InlinedVector<std::unique_ptr<EncodedFrame>, 4>
FrameBuffer::ExtractNextDecodableTemporalUnit() {
  absl::InlinedVector<std::unique_ptr<EncodedFrame>, 4> res;
  if (!next_decodable_temporal_unit_) {
    return res;
  }

  auto end_it = std::next(next_decodable_temporal_unit_->last_frame);
  for (auto it = next_decodable_temporal_unit_->first_frame; it != end_it;
       ++it) {
    decoded_frame_history_.InsertDecoded(GetFrameId(it), GetTimestamp(it));
    res.push_back(std::move(it->second.encoded_frame));
  }

  DropNextDecodableTemporalUnit();
  return res;
}

void FrameBuffer::DropNextDecodableTemporalUnit() {
  if (!next_decodable_temporal_unit_) {
    return;
  }

  auto end_it = std::next(next_decodable_temporal_unit_->last_frame);

  UpdateDroppedFramesAndDiscardedPackets(frames_.begin(), end_it);

  frames_.erase(frames_.begin(), end_it);
  FindNextAndLastDecodableTemporalUnit();
}

void FrameBuffer::UpdateDroppedFramesAndDiscardedPackets(FrameIterator begin_it,
                                                         FrameIterator end_it) {
  uint32_t dropped_ssrc = 0;
  int64_t dropped_frame_id = 0;
  unsigned int num_discarded_packets = 0;
  unsigned int num_dropped_frames =
      std::count_if(begin_it, end_it, [&](const auto& f) {
        if (f.second.encoded_frame) {
          const auto& packetInfos = f.second.encoded_frame->PacketInfos();
          dropped_frame_id = f.first;
          if (!packetInfos.empty()) {
            dropped_ssrc = packetInfos[0].ssrc();
          }
          num_discarded_packets += packetInfos.size();
        }
        return f.second.encoded_frame != nullptr;
      });

  if (num_dropped_frames > 0) {
    TRACE_EVENT2("webrtc", "FrameBuffer Dropping Old Frames", "remote_ssrc",
                 dropped_ssrc, "frame_id", dropped_frame_id);
  }
  if (num_discarded_packets > 0) {
    TRACE_EVENT2("webrtc", "FrameBuffer Discarding Old Packets", "remote_ssrc",
                 dropped_ssrc, "frame_id", dropped_frame_id);
  }

  num_dropped_frames_ += num_dropped_frames;
  num_discarded_packets_ += num_discarded_packets;
}

std::optional<int64_t> FrameBuffer::LastContinuousFrameId() const {
  return last_continuous_frame_id_;
}

std::optional<int64_t> FrameBuffer::LastContinuousTemporalUnitFrameId() const {
  return last_continuous_temporal_unit_frame_id_;
}

std::optional<FrameBuffer::DecodabilityInfo>
FrameBuffer::DecodableTemporalUnitsInfo() const {
  return decodable_temporal_units_info_;
}

int FrameBuffer::GetTotalNumberOfContinuousTemporalUnits() const {
  return num_continuous_temporal_units_;
}
int FrameBuffer::GetTotalNumberOfDroppedFrames() const {
  return num_dropped_frames_;
}
int FrameBuffer::GetTotalNumberOfDiscardedPackets() const {
  return num_discarded_packets_;
}

size_t FrameBuffer::CurrentSize() const {
  return frames_.size();
}

bool FrameBuffer::IsContinuous(const FrameIterator& it) const {
  for (int64_t reference : GetReferences(it)) {
    if (decoded_frame_history_.WasDecoded(reference)) {
      continue;
    }

    auto reference_frame_it = frames_.find(reference);
    if (reference_frame_it != frames_.end() &&
        reference_frame_it->second.continuous) {
      continue;
    }

    return false;
  }

  return true;
}

void FrameBuffer::PropagateContinuity(const FrameIterator& frame_it) {
  for (auto it = frame_it; it != frames_.end(); ++it) {
    if (!it->second.continuous) {
      if (IsContinuous(it)) {
        it->second.continuous = true;
        if (last_continuous_frame_id_ < GetFrameId(it)) {
          last_continuous_frame_id_ = GetFrameId(it);
        }
        if (IsLastFrameInTemporalUnit(it)) {
          num_continuous_temporal_units_++;
          if (last_continuous_temporal_unit_frame_id_ < GetFrameId(it)) {
            last_continuous_temporal_unit_frame_id_ = GetFrameId(it);
          }
        }
      }
    }
  }
}

void FrameBuffer::FindNextAndLastDecodableTemporalUnit() {
  next_decodable_temporal_unit_.reset();
  decodable_temporal_units_info_.reset();

  if (!last_continuous_temporal_unit_frame_id_) {
    return;
  }

  FrameIterator first_frame_it = frames_.begin();
  FrameIterator last_frame_it = frames_.begin();
  absl::InlinedVector<int64_t, 4> frames_in_temporal_unit;
  uint32_t last_decodable_temporal_unit_timestamp;
  for (auto frame_it = frames_.begin(); frame_it != frames_.end();) {
    if (GetFrameId(frame_it) > *last_continuous_temporal_unit_frame_id_) {
      break;
    }

    if (GetTimestamp(frame_it) != GetTimestamp(first_frame_it)) {
      frames_in_temporal_unit.clear();
      first_frame_it = frame_it;
    }

    frames_in_temporal_unit.push_back(GetFrameId(frame_it));

    last_frame_it = frame_it++;

    if (IsLastFrameInTemporalUnit(last_frame_it)) {
      bool temporal_unit_decodable = true;
      for (auto it = first_frame_it; it != frame_it && temporal_unit_decodable;
           ++it) {
        for (int64_t reference : GetReferences(it)) {
          if (!decoded_frame_history_.WasDecoded(reference) &&
              !absl::c_linear_search(frames_in_temporal_unit, reference)) {
            // A frame in the temporal unit has a non-decoded reference outside
            // the temporal unit, so it's not yet ready to be decoded.
            temporal_unit_decodable = false;
            break;
          }
        }
      }

      if (temporal_unit_decodable) {
        if (!next_decodable_temporal_unit_) {
          next_decodable_temporal_unit_ = {first_frame_it, last_frame_it};
        }

        last_decodable_temporal_unit_timestamp = GetTimestamp(first_frame_it);
      }
    }
  }

  if (next_decodable_temporal_unit_) {
    decodable_temporal_units_info_ = {
        .next_rtp_timestamp =
            GetTimestamp(next_decodable_temporal_unit_->first_frame),
        .last_rtp_timestamp = last_decodable_temporal_unit_timestamp};
  }
}

void FrameBuffer::Clear() {
  UpdateDroppedFramesAndDiscardedPackets(frames_.begin(), frames_.end());
  frames_.clear();
  next_decodable_temporal_unit_.reset();
  decodable_temporal_units_info_.reset();
  last_continuous_frame_id_.reset();
  last_continuous_temporal_unit_frame_id_.reset();
  decoded_frame_history_.Clear();
}

}  // namespace webrtc
