/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "media/engine/internal_decoder_factory.h"

#include <memory>
#include <vector>

#include "absl/strings/match.h"
#include "api/environment/environment.h"
#include "api/video/video_codec_type.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

#if defined(RTC_DAV1D_IN_INTERNAL_DECODER_FACTORY)
#include "modules/video_coding/codecs/av1/dav1d_decoder.h"  // nogncheck
#endif

namespace webrtc {
namespace {
#if defined(RTC_DAV1D_IN_INTERNAL_DECODER_FACTORY)
constexpr bool kDav1dIsIncluded = true;
#else
constexpr bool kDav1dIsIncluded = false;
std::unique_ptr<VideoDecoder> CreateDav1dDecoder(const Environment& env) {
  return nullptr;
}
#endif

}  // namespace

std::vector<SdpVideoFormat> InternalDecoderFactory::GetSupportedFormats()
    const {
  std::vector<SdpVideoFormat> formats;
  formats.push_back(SdpVideoFormat::VP8());
  for (const SdpVideoFormat& format : SupportedVP9DecoderCodecs())
    formats.push_back(format);
  for (const SdpVideoFormat& h264_format : SupportedH264DecoderCodecs())
    formats.push_back(h264_format);

#if !defined(WEBRTC_MOZILLA_BUILD)
  if (kDav1dIsIncluded) {
    formats.push_back(SdpVideoFormat::AV1Profile0());
    formats.push_back(SdpVideoFormat::AV1Profile1());
  }
#endif

  return formats;
}

VideoDecoderFactory::CodecSupport InternalDecoderFactory::QueryCodecSupport(
    const SdpVideoFormat& format,
    bool reference_scaling) const {
  // Query for supported formats and check if the specified format is supported.
  // Return unsupported if an invalid combination of format and
  // reference_scaling is specified.
  if (reference_scaling) {
    VideoCodecType codec = PayloadStringToCodecType(format.name);
    if (codec != kVideoCodecVP9 && codec != kVideoCodecAV1) {
      return {/*is_supported=*/false, /*is_power_efficient=*/false};
    }
  }

  CodecSupport codec_support;
  codec_support.is_supported = format.IsCodecInList(GetSupportedFormats());
  return codec_support;
}

std::unique_ptr<VideoDecoder> InternalDecoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  if (!format.IsCodecInList(GetSupportedFormats())) {
    RTC_LOG(LS_WARNING) << "Trying to create decoder for unsupported format. "
                        << format.ToString();
    return nullptr;
  }

  if (absl::EqualsIgnoreCase(format.name, kVp8CodecName))
    return CreateVp8Decoder(env);
  if (absl::EqualsIgnoreCase(format.name, kVp9CodecName))
    return VP9Decoder::Create();
  if (absl::EqualsIgnoreCase(format.name, kH264CodecName))
    return H264Decoder::Create();

  if (absl::EqualsIgnoreCase(format.name, kAv1CodecName) && kDav1dIsIncluded) {
    return CreateDav1dDecoder(env);
  }

  RTC_DCHECK_NOTREACHED();
  return nullptr;
}

}  // namespace webrtc
