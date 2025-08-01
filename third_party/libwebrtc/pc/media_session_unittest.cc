/*
 *  Copyright 2004 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/media_session.h"

#include <stddef.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/audio_codecs/audio_format.h"
#include "api/candidate.h"
#include "api/field_trials_view.h"
#include "api/media_types.h"
#include "api/rtp_parameters.h"
#include "api/rtp_transceiver_direction.h"
#include "api/sctp_transport_interface.h"
#include "api/video_codecs/sdp_video_format.h"
#include "call/fake_payload_type_suggester.h"
#include "call/payload_type.h"
#include "media/base/codec.h"
#include "media/base/codec_list.h"
#include "media/base/media_constants.h"
#include "media/base/rid_description.h"
#include "media/base/stream_params.h"
#include "media/base/test_utils.h"
#include "p2p/base/p2p_constants.h"
#include "p2p/base/transport_description.h"
#include "p2p/base/transport_description_factory.h"
#include "p2p/base/transport_info.h"
#include "pc/codec_vendor.h"
#include "pc/media_options.h"
#include "pc/media_protocol_names.h"
#include "pc/rtp_media_utils.h"
#include "pc/rtp_parameters_conversion.h"
#include "pc/session_description.h"
#include "pc/simulcast_description.h"
#include "rtc_base/checks.h"
#include "rtc_base/fake_ssl_identity.h"
#include "rtc_base/logging.h"
#include "rtc_base/rtc_certificate.h"
#include "rtc_base/ssl_identity.h"
#include "rtc_base/string_encode.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/unique_id_generator.h"
#include "test/gmock.h"
#include "test/gtest.h"
#include "test/scoped_key_value_config.h"

namespace webrtc {
namespace {

using ::testing::Bool;
using ::testing::Combine;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Field;
using ::testing::Gt;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Pointwise;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAreArray;
using ::testing::Values;
using ::testing::ValuesIn;
using ::webrtc::UniqueRandomIdGenerator;
using ::webrtc::test::ScopedKeyValueConfig;

using Candidates = std::vector<Candidate>;

class CodecLookupHelperForTesting : public CodecLookupHelper {
 public:
  explicit CodecLookupHelperForTesting(const FieldTrialsView& field_trials)
      : codec_vendor_(nullptr, false, field_trials) {}
  webrtc::PayloadTypeSuggester* PayloadTypeSuggester() override {
    return &payload_type_suggester_;
  }
  CodecVendor* GetCodecVendor() override { return &codec_vendor_; }

 private:
  FakePayloadTypeSuggester payload_type_suggester_;
  CodecVendor codec_vendor_;
};

Codec CreateRedAudioCodec(absl::string_view encoding_id) {
  Codec red = CreateAudioCodec(63, "red", 48000, 2);
  red.SetParam(kCodecParamNotInNameValueFormat,
               std::string(encoding_id) + '/' + std::string(encoding_id));
  return red;
}

const Codec kAudioCodecs1[] = {CreateAudioCodec(111, "opus", 48000, 2),
                               CreateRedAudioCodec("111"),
                               CreateAudioCodec(103, "G722", 16000, 1),
                               CreateAudioCodec(0, "PCMU", 8000, 1),
                               CreateAudioCodec(8, "PCMA", 8000, 1),
                               CreateAudioCodec(107, "CN", 48000, 1)};

const Codec kAudioCodecs2[] = {
    CreateAudioCodec(126, "foo", 16000, 1),
    CreateAudioCodec(0, "PCMU", 8000, 1),
    CreateAudioCodec(127, "G722", 16000, 1),
};

const Codec kAudioCodecsAnswer[] = {
    CreateAudioCodec(103, "G722", 16000, 1),
    CreateAudioCodec(0, "PCMU", 8000, 1),
};

const Codec kVideoCodecs1[] = {CreateVideoCodec(96, "H264-SVC"),
                               CreateVideoCodec(97, "H264")};

const Codec kVideoCodecs1Reverse[] = {CreateVideoCodec(97, "H264"),
                                      CreateVideoCodec(96, "H264-SVC")};

const Codec kVideoCodecs2[] = {CreateVideoCodec(126, "H264"),
                               CreateVideoCodec(127, "H263")};

const Codec kVideoCodecsAnswer[] = {CreateVideoCodec(97, "H264")};

// H.265 level-id, according to H.265 spec, is calculated this way:
// For any given H.265 level a.b, level-id = (a * 10 + b) * 3. For level 6.0,
// level-id = (6 * 10 + 0) * 3 = 180. Similar for all other H.265 levels.
const char kVideoCodecsH265Level6LevelId[] = "180";
const char kVideoCodecsH265Level52LevelId[] = "156";
const char kVideoCodecsH265Level5LevelId[] = "150";
const char kVideoCodecsH265Level4LevelId[] = "120";
const char kVideoCodecsH265Level31LevelId[] = "93";

const SdpVideoFormat kH265MainProfileLevel31Sdp(
    "H265",
    {{"profile-id", "1"},
     {"tier-flag", "0"},
     {"level-id", kVideoCodecsH265Level31LevelId},
     {"tx-mode", "SRST"}});
const SdpVideoFormat kH265MainProfileLevel4Sdp("H265",
                                               {{"profile-id", "1"},
                                                {"tier-flag", "0"},
                                                {"level-id",
                                                 kVideoCodecsH265Level4LevelId},
                                                {"tx-mode", "SRST"}});
const SdpVideoFormat kH265MainProfileLevel5Sdp("H265",
                                               {{"profile-id", "1"},
                                                {"tier-flag", "0"},
                                                {"level-id",
                                                 kVideoCodecsH265Level5LevelId},
                                                {"tx-mode", "SRST"}});
const SdpVideoFormat kH265MainProfileLevel52Sdp(
    "H265",
    {{"profile-id", "1"},
     {"tier-flag", "0"},
     {"level-id", kVideoCodecsH265Level52LevelId},
     {"tx-mode", "SRST"}});
const SdpVideoFormat kH265MainProfileLevel6Sdp("H265",
                                               {{"profile-id", "1"},
                                                {"tier-flag", "0"},
                                                {"level-id",
                                                 kVideoCodecsH265Level6LevelId},
                                                {"tx-mode", "SRST"}});

const Codec kVideoCodecsH265Level31[] = {
    CreateVideoCodec(96, kH265MainProfileLevel31Sdp)};
const Codec kVideoCodecsH265Level4[] = {
    CreateVideoCodec(96, kH265MainProfileLevel4Sdp)};
const Codec kVideoCodecsH265Level5[] = {
    CreateVideoCodec(96, kH265MainProfileLevel5Sdp)};
const Codec kVideoCodecsH265Level52[] = {
    CreateVideoCodec(96, kH265MainProfileLevel52Sdp)};
const Codec kVideoCodecsH265Level6[] = {
    CreateVideoCodec(96, kH265MainProfileLevel6Sdp)};
// Match two codec lists for content, but ignore the ID.
bool CodecListsMatch(ArrayView<const Codec> list1,
                     ArrayView<const Codec> list2) {
  if (list1.size() != list2.size()) {
    return false;
  }
  for (size_t i = 0; i < list1.size(); ++i) {
    Codec codec1 = list1[i];
    Codec codec2 = list2[i];
    codec1.id = Codec::kIdNotSet;
    codec2.id = Codec::kIdNotSet;
    if (codec1 != codec2) {
      RTC_LOG(LS_ERROR) << "Mismatch at position " << i << " between " << codec1
                        << " and " << codec2;
      return false;
    }
  }
  return true;
}

const RtpExtension kAudioRtpExtension1[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 8),
    RtpExtension("http://google.com/testing/audio_something", 10),
};

const RtpExtension kAudioRtpExtensionEncrypted1[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 8),
    RtpExtension("http://google.com/testing/audio_something", 11, true),
};

const RtpExtension kAudioRtpExtension2[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 2),
    RtpExtension("http://google.com/testing/audio_something_else", 8),
    RtpExtension("http://google.com/testing/both_audio_and_video", 7),
};

const RtpExtension kAudioRtpExtensionEncrypted2[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 2),
    RtpExtension("http://google.com/testing/audio_something", 13, true),
    RtpExtension("http://google.com/testing/audio_something_else", 5, true),
};

const RtpExtension kAudioRtpExtension3[] = {
    RtpExtension("http://google.com/testing/audio_something", 2),
    RtpExtension("http://google.com/testing/both_audio_and_video", 3),
};

const RtpExtension kAudioRtpExtensionMixedEncryption1[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 8),
    RtpExtension("http://google.com/testing/audio_something", 9),
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 10, true),
    RtpExtension("http://google.com/testing/audio_something", 11, true),
    RtpExtension("http://google.com/testing/audio_something_else", 12, true),
};

const RtpExtension kAudioRtpExtensionMixedEncryption2[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 5),
    RtpExtension("http://google.com/testing/audio_something", 6),
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 7, true),
    RtpExtension("http://google.com/testing/audio_something", 8, true),
    RtpExtension("http://google.com/testing/audio_something_else", 9),
};

const RtpExtension kAudioRtpExtensionAnswer[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 8),
};

const RtpExtension kAudioRtpExtensionEncryptedAnswer[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 8),
    RtpExtension("http://google.com/testing/audio_something", 11, true),
};

const RtpExtension kAudioRtpExtensionMixedEncryptionAnswerEncryptionEnabled[] =
    {
        RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 10, true),
        RtpExtension("http://google.com/testing/audio_something", 11, true),
};

const RtpExtension kAudioRtpExtensionMixedEncryptionAnswerEncryptionDisabled[] =
    {
        RtpExtension("urn:ietf:params:rtp-hdrext:ssrc-audio-level", 8),
        RtpExtension("http://google.com/testing/audio_something", 9),
};

const RtpExtension kVideoRtpExtension1[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 14),
    RtpExtension("http://google.com/testing/video_something", 13),
};

const RtpExtension kVideoRtpExtensionEncrypted1[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 14),
    RtpExtension("http://google.com/testing/video_something", 7, true),
};

const RtpExtension kVideoRtpExtension2[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 2),
    RtpExtension("http://google.com/testing/video_something_else", 14),
    RtpExtension("http://google.com/testing/both_audio_and_video", 7),
};

const RtpExtension kVideoRtpExtensionEncrypted2[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 8),
    RtpExtension("http://google.com/testing/video_something", 10, true),
    RtpExtension("http://google.com/testing/video_something_else", 4, true),
};

const RtpExtension kVideoRtpExtension3[] = {
    RtpExtension("http://google.com/testing/video_something", 4),
    RtpExtension("http://google.com/testing/both_audio_and_video", 5),
};

const RtpExtension kVideoRtpExtensionMixedEncryption[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 14),
    RtpExtension("http://google.com/testing/video_something", 13),
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 15, true),
    RtpExtension("http://google.com/testing/video_something", 16, true),
};

const RtpExtension kVideoRtpExtensionAnswer[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 14),
};

const RtpExtension kVideoRtpExtensionEncryptedAnswer[] = {
    RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 14),
    RtpExtension("http://google.com/testing/video_something", 7, true),
};

const RtpExtension kVideoRtpExtensionMixedEncryptionAnswerEncryptionEnabled[] =
    {
        RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 15, true),
        RtpExtension("http://google.com/testing/video_something", 16, true),
};

const RtpExtension kVideoRtpExtensionMixedEncryptionAnswerEncryptionDisabled[] =
    {
        RtpExtension("urn:ietf:params:rtp-hdrext:toffset", 14),
        RtpExtension("http://google.com/testing/video_something", 13),
};

const RtpExtension kRtpExtensionTransportSequenceNumber01[] = {
    RtpExtension("http://www.ietf.org/id/"
                 "draft-holmer-rmcat-transport-wide-cc-extensions-01",
                 1),
};

const RtpExtension kRtpExtensionTransportSequenceNumber01And02[] = {
    RtpExtension("http://www.ietf.org/id/"
                 "draft-holmer-rmcat-transport-wide-cc-extensions-01",
                 1),
    RtpExtension(
        "http://www.webrtc.org/experiments/rtp-hdrext/transport-wide-cc-02",
        2),
};

const RtpExtension kRtpExtensionTransportSequenceNumber02[] = {
    RtpExtension(
        "http://www.webrtc.org/experiments/rtp-hdrext/transport-wide-cc-02",
        2),
};

const RtpExtension kRtpExtensionGenericFrameDescriptorUri00[] = {
    RtpExtension("http://www.webrtc.org/experiments/rtp-hdrext/"
                 "generic-frame-descriptor-00",
                 3),
};

const uint32_t kSimulcastParamsSsrc[] = {10, 11, 20, 21, 30, 31};
const uint32_t kSimSsrc[] = {10, 20, 30};
const uint32_t kFec1Ssrc[] = {10, 11};
const uint32_t kFec2Ssrc[] = {20, 21};
const uint32_t kFec3Ssrc[] = {30, 31};

const char kMediaStream1[] = "stream_1";
const char kMediaStream2[] = "stream_2";
const char kVideoTrack1[] = "video_1";
const char kVideoTrack2[] = "video_2";
const char kAudioTrack1[] = "audio_1";
const char kAudioTrack2[] = "audio_2";
const char kAudioTrack3[] = "audio_3";

const char* kMediaProtocols[] = {"RTP/AVP", "RTP/SAVP", "RTP/AVPF",
                                 "RTP/SAVPF"};
const char* kMediaProtocolsDtls[] = {"TCP/TLS/RTP/SAVPF", "TCP/TLS/RTP/SAVP",
                                     "UDP/TLS/RTP/SAVPF", "UDP/TLS/RTP/SAVP"};

// These constants are used to make the code using "AddMediaDescriptionOptions"
// more readable.
constexpr bool kStopped = true;
constexpr bool kActive = false;

// Helper used for debugging. It reports the media type and the parameters.
std::string FullMimeType(Codec codec) {
  StringBuilder sb;
  switch (codec.type) {
    case Codec::Type::kAudio:
      sb << "audio/";
      break;
    case Codec::Type::kVideo:
      sb << "video/";
      break;
  }
  sb << codec.name;
  for (auto& param : codec.params) {
    sb << ";" << param.first << "=" << param.second;
  }
  return sb.Release();
}

bool IsMediaContentOfType(const ContentInfo* content,
                          webrtc::MediaType media_type) {
  RTC_DCHECK(content);
  return content->media_description()->type() == media_type;
}

RtpTransceiverDirection GetMediaDirection(const ContentInfo* content) {
  RTC_DCHECK(content);
  return content->media_description()->direction();
}

void AddRtxCodec(const Codec& rtx_codec, std::vector<Codec>* codecs) {
  RTC_LOG(LS_VERBOSE) << "Adding RTX codec " << FullMimeType(rtx_codec);
  ASSERT_FALSE(FindCodecById(*codecs, rtx_codec.id));
  codecs->push_back(rtx_codec);
}

std::vector<std::string> GetCodecNames(const std::vector<Codec>& codecs) {
  std::vector<std::string> codec_names;
  codec_names.reserve(codecs.size());
  for (const auto& codec : codecs) {
    codec_names.push_back(codec.name);
  }
  return codec_names;
}

// This is used for test only. MIDs are not the identification of the
// MediaDescriptionOptions since some end points may not support MID and the SDP
// may not contain 'mid'.
std::vector<MediaDescriptionOptions>::iterator FindFirstMediaDescriptionByMid(
    const std::string& mid,
    MediaSessionOptions* opts) {
  return absl::c_find_if(
      opts->media_description_options,
      [&mid](const MediaDescriptionOptions& t) { return t.mid == mid; });
}

std::vector<MediaDescriptionOptions>::const_iterator
FindFirstMediaDescriptionByMid(const std::string& mid,
                               const MediaSessionOptions& opts) {
  return absl::c_find_if(
      opts.media_description_options,
      [&mid](const MediaDescriptionOptions& t) { return t.mid == mid; });
}

// Add a media section to the `session_options`.
void AddMediaDescriptionOptions(webrtc::MediaType type,
                                const std::string& mid,
                                RtpTransceiverDirection direction,
                                bool stopped,
                                MediaSessionOptions* opts) {
  opts->media_description_options.push_back(
      MediaDescriptionOptions(type, mid, direction, stopped));
}

void AddAudioVideoSections(RtpTransceiverDirection direction,
                           MediaSessionOptions* opts) {
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio", direction,
                             kActive, opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video", direction,
                             kActive, opts);
}

void AddDataSection(RtpTransceiverDirection direction,
                    MediaSessionOptions* opts) {
  AddMediaDescriptionOptions(webrtc::MediaType::DATA, "data", direction,
                             kActive, opts);
}

void AttachSenderToMediaDescriptionOptions(
    const std::string& mid,
    webrtc::MediaType type,
    const std::string& track_id,
    const std::vector<std::string>& stream_ids,
    const std::vector<RidDescription>& rids,
    const SimulcastLayerList& simulcast_layers,
    int num_sim_layer,
    MediaSessionOptions* session_options) {
  auto it = FindFirstMediaDescriptionByMid(mid, session_options);
  switch (type) {
    case webrtc::MediaType::AUDIO:
      it->AddAudioSender(track_id, stream_ids);
      break;
    case webrtc::MediaType::VIDEO:
      it->AddVideoSender(track_id, stream_ids, rids, simulcast_layers,
                         num_sim_layer);
      break;
    default:
      RTC_DCHECK_NOTREACHED();
  }
}

void AttachSenderToMediaDescriptionOptions(
    const std::string& mid,
    webrtc::MediaType type,
    const std::string& track_id,
    const std::vector<std::string>& stream_ids,
    int num_sim_layer,
    MediaSessionOptions* session_options) {
  AttachSenderToMediaDescriptionOptions(mid, type, track_id, stream_ids, {},
                                        SimulcastLayerList(), num_sim_layer,
                                        session_options);
}

void DetachSenderFromMediaSection(const std::string& mid,
                                  const std::string& track_id,
                                  MediaSessionOptions* session_options) {
  std::vector<SenderOptions>& sender_options_list =
      FindFirstMediaDescriptionByMid(mid, session_options)->sender_options;
  auto sender_it = absl::c_find_if(
      sender_options_list, [track_id](const SenderOptions& sender_options) {
        return sender_options.track_id == track_id;
      });
  RTC_DCHECK(sender_it != sender_options_list.end());
  sender_options_list.erase(sender_it);
}

// Helper function used to create recv-only audio MediaSessionOptions.
MediaSessionOptions CreateAudioMediaSession() {
  MediaSessionOptions session_options;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &session_options);
  return session_options;
}

// TODO(zhihuang): Most of these tests were written while MediaSessionOptions
// was designed for Plan B SDP, where only one audio "m=" section and one video
// "m=" section could be generated, and ordering couldn't be controlled. Many of
// these tests may be obsolete as a result, and should be refactored or removed.
class MediaSessionDescriptionFactoryTest : public testing::Test {
 public:
  MediaSessionDescriptionFactoryTest()
      : tdf1_(field_trials),
        tdf2_(field_trials),
        codec_lookup_helper_1_(field_trials),
        codec_lookup_helper_2_(field_trials),
        f1_(nullptr, false, &ssrc_generator1, &tdf1_, &codec_lookup_helper_1_),
        f2_(nullptr, false, &ssrc_generator2, &tdf2_, &codec_lookup_helper_2_) {
    codec_lookup_helper_1_.GetCodecVendor()->set_audio_codecs(
        MAKE_VECTOR(kAudioCodecs1), MAKE_VECTOR(kAudioCodecs1));
    codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(
        MAKE_VECTOR(kVideoCodecs1), MAKE_VECTOR(kVideoCodecs1));
    codec_lookup_helper_2_.GetCodecVendor()->set_audio_codecs(
        MAKE_VECTOR(kAudioCodecs2), MAKE_VECTOR(kAudioCodecs2));
    codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(
        MAKE_VECTOR(kVideoCodecs2), MAKE_VECTOR(kVideoCodecs2));
    tdf1_.set_certificate(RTCCertificate::Create(
        std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("id1"))));
    tdf2_.set_certificate(RTCCertificate::Create(
        std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("id2"))));
  }

  // Create a video StreamParamsVec object with:
  // - one video stream with 3 simulcast streams and FEC,
  StreamParamsVec CreateComplexVideoStreamParamsVec() {
    SsrcGroup sim_group("SIM", MAKE_VECTOR(kSimSsrc));
    SsrcGroup fec_group1("FEC", MAKE_VECTOR(kFec1Ssrc));
    SsrcGroup fec_group2("FEC", MAKE_VECTOR(kFec2Ssrc));
    SsrcGroup fec_group3("FEC", MAKE_VECTOR(kFec3Ssrc));

    std::vector<SsrcGroup> ssrc_groups;
    ssrc_groups.push_back(sim_group);
    ssrc_groups.push_back(fec_group1);
    ssrc_groups.push_back(fec_group2);
    ssrc_groups.push_back(fec_group3);

    StreamParams simulcast_params;
    simulcast_params.id = kVideoTrack1;
    simulcast_params.ssrcs = MAKE_VECTOR(kSimulcastParamsSsrc);
    simulcast_params.ssrc_groups = ssrc_groups;
    simulcast_params.cname = "Video_SIM_FEC";
    simulcast_params.set_stream_ids({kMediaStream1});

    StreamParamsVec video_streams;
    video_streams.push_back(simulcast_params);

    return video_streams;
  }

  // Returns true if the transport info contains "renomination" as an
  // ICE option.
  bool GetIceRenomination(const TransportInfo* transport_info) {
    return absl::c_linear_search(transport_info->description.transport_options,
                                 "renomination");
  }

  void TestTransportInfo(bool offer,
                         const MediaSessionOptions& options,
                         bool has_current_desc) {
    const std::string current_audio_ufrag = "current_audio_ufrag";
    const std::string current_audio_pwd = "current_audio_pwd";
    const std::string current_video_ufrag = "current_video_ufrag";
    const std::string current_video_pwd = "current_video_pwd";
    const std::string current_data_ufrag = "current_data_ufrag";
    const std::string current_data_pwd = "current_data_pwd";
    std::unique_ptr<SessionDescription> current_desc;
    std::unique_ptr<SessionDescription> desc;
    if (has_current_desc) {
      current_desc = std::make_unique<SessionDescription>();
      current_desc->AddTransportInfo(TransportInfo(
          "audio",
          TransportDescription(current_audio_ufrag, current_audio_pwd)));
      current_desc->AddTransportInfo(TransportInfo(
          "video",
          TransportDescription(current_video_ufrag, current_video_pwd)));
      current_desc->AddTransportInfo(TransportInfo(
          "data", TransportDescription(current_data_ufrag, current_data_pwd)));
    }
    if (offer) {
      desc = f1_.CreateOfferOrError(options, current_desc.get()).MoveValue();
    } else {
      std::unique_ptr<SessionDescription> offer_desc;
      offer_desc = f1_.CreateOfferOrError(options, nullptr).MoveValue();
      desc =
          f1_.CreateAnswerOrError(offer_desc.get(), options, current_desc.get())
              .MoveValue();
    }
    ASSERT_TRUE(desc);
    const TransportInfo* ti_audio = desc->GetTransportInfoByName("audio");
    if (options.has_audio()) {
      if (has_current_desc) {
        EXPECT_EQ(current_audio_ufrag, ti_audio->description.ice_ufrag);
        EXPECT_EQ(current_audio_pwd, ti_audio->description.ice_pwd);
      } else {
        EXPECT_EQ(static_cast<size_t>(ICE_UFRAG_LENGTH),
                  ti_audio->description.ice_ufrag.size());
        EXPECT_EQ(static_cast<size_t>(ICE_PWD_LENGTH),
                  ti_audio->description.ice_pwd.size());
      }
      auto media_desc_options_it =
          FindFirstMediaDescriptionByMid("audio", options);
      EXPECT_EQ(
          media_desc_options_it->transport_options.enable_ice_renomination,
          GetIceRenomination(ti_audio));
    }
    const TransportInfo* ti_video = desc->GetTransportInfoByName("video");
    if (options.has_video()) {
      auto media_desc_options_it =
          FindFirstMediaDescriptionByMid("video", options);
      if (options.bundle_enabled) {
        EXPECT_EQ(ti_audio->description.ice_ufrag,
                  ti_video->description.ice_ufrag);
        EXPECT_EQ(ti_audio->description.ice_pwd, ti_video->description.ice_pwd);
      } else {
        if (has_current_desc) {
          EXPECT_EQ(current_video_ufrag, ti_video->description.ice_ufrag);
          EXPECT_EQ(current_video_pwd, ti_video->description.ice_pwd);
        } else {
          EXPECT_EQ(static_cast<size_t>(ICE_UFRAG_LENGTH),
                    ti_video->description.ice_ufrag.size());
          EXPECT_EQ(static_cast<size_t>(ICE_PWD_LENGTH),
                    ti_video->description.ice_pwd.size());
        }
      }
      EXPECT_EQ(
          media_desc_options_it->transport_options.enable_ice_renomination,
          GetIceRenomination(ti_video));
    }
    const TransportInfo* ti_data = desc->GetTransportInfoByName("data");
    if (options.has_data()) {
      if (options.bundle_enabled) {
        EXPECT_EQ(ti_audio->description.ice_ufrag,
                  ti_data->description.ice_ufrag);
        EXPECT_EQ(ti_audio->description.ice_pwd, ti_data->description.ice_pwd);
      } else {
        if (has_current_desc) {
          EXPECT_EQ(current_data_ufrag, ti_data->description.ice_ufrag);
          EXPECT_EQ(current_data_pwd, ti_data->description.ice_pwd);
        } else {
          EXPECT_EQ(static_cast<size_t>(ICE_UFRAG_LENGTH),
                    ti_data->description.ice_ufrag.size());
          EXPECT_EQ(static_cast<size_t>(ICE_PWD_LENGTH),
                    ti_data->description.ice_pwd.size());
        }
      }
      auto media_desc_options_it =
          FindFirstMediaDescriptionByMid("data", options);
      EXPECT_EQ(
          media_desc_options_it->transport_options.enable_ice_renomination,
          GetIceRenomination(ti_data));
    }
  }

  // This test that the audio and video media direction is set to
  // `expected_direction_in_answer` in an answer if the offer direction is set
  // to `direction_in_offer` and the answer is willing to both send and receive.
  void TestMediaDirectionInAnswer(
      RtpTransceiverDirection direction_in_offer,
      RtpTransceiverDirection expected_direction_in_answer) {
    MediaSessionOptions offer_opts;
    AddAudioVideoSections(direction_in_offer, &offer_opts);

    std::unique_ptr<SessionDescription> offer =
        f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
    ASSERT_TRUE(offer.get());
    ContentInfo* ac_offer = offer->GetContentByName("audio");
    ASSERT_TRUE(ac_offer);
    ContentInfo* vc_offer = offer->GetContentByName("video");
    ASSERT_TRUE(vc_offer);

    MediaSessionOptions answer_opts;
    AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &answer_opts);
    std::unique_ptr<SessionDescription> answer =
        f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
    const AudioContentDescription* acd_answer =
        GetFirstAudioContentDescription(answer.get());
    EXPECT_EQ(expected_direction_in_answer, acd_answer->direction());
    const VideoContentDescription* vcd_answer =
        GetFirstVideoContentDescription(answer.get());
    EXPECT_EQ(expected_direction_in_answer, vcd_answer->direction());
  }

  bool VerifyNoCNCodecs(const ContentInfo* content) {
    RTC_DCHECK(content);
    RTC_CHECK(content->media_description());
    for (const Codec& codec : content->media_description()->codecs()) {
      if (codec.name == "CN") {
        return false;
      }
    }
    return true;
  }

  void TestTransportSequenceNumberNegotiation(
      const RtpHeaderExtensions& local,
      const RtpHeaderExtensions& offered,
      const RtpHeaderExtensions& expectedAnswer) {
    MediaSessionOptions opts;
    AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
    SetAudioVideoRtpHeaderExtensions(offered, offered, &opts);
    std::unique_ptr<SessionDescription> offer =
        f1_.CreateOfferOrError(opts, nullptr).MoveValue();
    ASSERT_TRUE(offer.get());
    SetAudioVideoRtpHeaderExtensions(local, local, &opts);
    std::unique_ptr<SessionDescription> answer =
        f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

    EXPECT_THAT(
        expectedAnswer,
        UnorderedElementsAreArray(GetFirstAudioContentDescription(answer.get())
                                      ->rtp_header_extensions()));
    EXPECT_THAT(
        expectedAnswer,
        UnorderedElementsAreArray(GetFirstVideoContentDescription(answer.get())
                                      ->rtp_header_extensions()));
  }

  std::vector<RtpHeaderExtensionCapability>
  HeaderExtensionCapabilitiesFromRtpExtensions(RtpHeaderExtensions extensions) {
    std::vector<RtpHeaderExtensionCapability> capabilities;
    for (const auto& extension : extensions) {
      RtpHeaderExtensionCapability capability(
          extension.uri, extension.id, extension.encrypt,
          RtpTransceiverDirection::kSendRecv);
      capabilities.push_back(capability);
    }
    return capabilities;
  }

  void SetAudioVideoRtpHeaderExtensions(RtpHeaderExtensions audio_exts,
                                        RtpHeaderExtensions video_exts,
                                        MediaSessionOptions* opts) {
    std::vector<RtpHeaderExtensionCapability> audio_caps =
        HeaderExtensionCapabilitiesFromRtpExtensions(audio_exts);
    std::vector<RtpHeaderExtensionCapability> video_caps =
        HeaderExtensionCapabilitiesFromRtpExtensions(video_exts);
    for (auto& entry : opts->media_description_options) {
      switch (entry.type) {
        case webrtc::MediaType::AUDIO:
          entry.header_extensions = audio_caps;
          break;
        case webrtc::MediaType::VIDEO:
          entry.header_extensions = video_caps;
          break;
        default:
          break;
      }
    }
  }

 protected:
  ScopedKeyValueConfig field_trials;
  UniqueRandomIdGenerator ssrc_generator1;
  UniqueRandomIdGenerator ssrc_generator2;
  TransportDescriptionFactory tdf1_;
  TransportDescriptionFactory tdf2_;
  CodecLookupHelperForTesting codec_lookup_helper_1_;
  CodecLookupHelperForTesting codec_lookup_helper_2_;
  MediaSessionDescriptionFactory f1_;
  MediaSessionDescriptionFactory f2_;
};

// Create a typical audio offer, and ensure it matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateAudioOffer) {
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(CreateAudioMediaSession(), nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* ac = offer->GetContentByName("audio");
  const ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(ac);
  EXPECT_FALSE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_THAT(codec_lookup_helper_1_.GetCodecVendor()->audio_sendrecv_codecs(),
              ElementsAreArray(acd->codecs()));
  EXPECT_EQ(0U, acd->first_ssrc());             // no sender is attached.
  EXPECT_EQ(kAutoBandwidth,
            acd->bandwidth());                  // default bandwidth (auto)
  EXPECT_TRUE(acd->rtcp_mux());                 // rtcp-mux defaults on
  EXPECT_EQ(kMediaProtocolDtlsSavpf, acd->protocol());
}

// Create an offer with just Opus and RED.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateAudioOfferWithJustOpusAndRed) {
  // First, prefer to only use opus and red.
  std::vector<RtpCodecCapability> preferences;
  preferences.push_back(webrtc::ToRtpCodecCapability(
      codec_lookup_helper_1_.GetCodecVendor()->audio_sendrecv_codecs()[0]));
  preferences.push_back(webrtc::ToRtpCodecCapability(
      codec_lookup_helper_1_.GetCodecVendor()->audio_sendrecv_codecs()[1]));
  EXPECT_EQ("opus", preferences[0].name);
  EXPECT_EQ("red", preferences[1].name);

  auto opts = CreateAudioMediaSession();
  opts.media_description_options.at(0).codec_preferences = preferences;
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* ac = offer->GetContentByName("audio");
  const ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(ac != NULL);
  ASSERT_TRUE(vc == NULL);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_EQ(2U, acd->codecs().size());
  EXPECT_EQ("opus", acd->codecs()[0].name);
  EXPECT_EQ("red", acd->codecs()[1].name);
}

// Create an offer with RED before Opus, which enables RED with Opus encoding.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateAudioOfferWithRedForOpus) {
  // First, prefer to only use opus and red.
  std::vector<RtpCodecCapability> preferences;
  preferences.push_back(webrtc::ToRtpCodecCapability(
      codec_lookup_helper_1_.GetCodecVendor()->audio_sendrecv_codecs()[1]));
  preferences.push_back(webrtc::ToRtpCodecCapability(
      codec_lookup_helper_1_.GetCodecVendor()->audio_sendrecv_codecs()[0]));
  EXPECT_EQ("red", preferences[0].name);
  EXPECT_EQ("opus", preferences[1].name);

  auto opts = CreateAudioMediaSession();
  opts.media_description_options.at(0).codec_preferences = preferences;
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* ac = offer->GetContentByName("audio");
  const ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(ac != NULL);
  ASSERT_TRUE(vc == NULL);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_EQ(2U, acd->codecs().size());
  EXPECT_EQ("red", acd->codecs()[0].name);
  EXPECT_EQ("opus", acd->codecs()[1].name);
}

// Create a typical video offer, and ensure it matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateVideoOffer) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* ac = offer->GetContentByName("audio");
  const ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  EXPECT_EQ(MediaProtocolType::kRtp, vc->type);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_EQ(
      codec_lookup_helper_1_.GetCodecVendor()->audio_sendrecv_codecs().codecs(),
      acd->codecs());
  EXPECT_EQ(0U, acd->first_ssrc());             // no sender is attached
  EXPECT_EQ(kAutoBandwidth,
            acd->bandwidth());                  // default bandwidth (auto)
  EXPECT_TRUE(acd->rtcp_mux());                 // rtcp-mux defaults on
  EXPECT_EQ(kMediaProtocolDtlsSavpf, acd->protocol());
  EXPECT_EQ(webrtc::MediaType::VIDEO, vcd->type());
  EXPECT_EQ(
      codec_lookup_helper_1_.GetCodecVendor()->video_sendrecv_codecs().codecs(),
      vcd->codecs());
  EXPECT_EQ(0U, vcd->first_ssrc());             // no sender is attached
  EXPECT_EQ(kAutoBandwidth,
            vcd->bandwidth());                  // default bandwidth (auto)
  EXPECT_TRUE(vcd->rtcp_mux());                 // rtcp-mux defaults on
  EXPECT_EQ(kMediaProtocolDtlsSavpf, vcd->protocol());
}

TEST_F(MediaSessionDescriptionFactoryTest, TestCreateOfferWithCustomCodecs) {
  MediaSessionOptions opts;

  SdpAudioFormat audio_format("custom-audio", 8000, 2);
  Codec custom_audio_codec = CreateAudioCodec(audio_format);
  custom_audio_codec.id = 123;  // picked at random, but valid
  auto audio_options =
      MediaDescriptionOptions(webrtc::MediaType::AUDIO, "0",
                              RtpTransceiverDirection::kSendRecv, kActive);
  audio_options.codecs_to_include.push_back(custom_audio_codec);
  opts.media_description_options.push_back(audio_options);

  Codec custom_video_codec = CreateVideoCodec("custom-video");
  custom_video_codec.id = 124;  // picked at random, but valid
  auto video_options =
      MediaDescriptionOptions(webrtc::MediaType::VIDEO, "1",
                              RtpTransceiverDirection::kSendRecv, kActive);
  video_options.codecs_to_include.push_back(custom_video_codec);
  opts.media_description_options.push_back(video_options);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* ac = offer->GetContentByName("0");
  const ContentInfo* vc = offer->GetContentByName("1");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  EXPECT_EQ(MediaProtocolType::kRtp, vc->type);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  ASSERT_EQ(acd->codecs().size(), 1U);
  // Fields in codec are set during the gen process, so simple compare
  // does not work.
  EXPECT_EQ(acd->codecs()[0].name, custom_audio_codec.name);

  EXPECT_EQ(webrtc::MediaType::VIDEO, vcd->type());
  ASSERT_EQ(vcd->codecs().size(), 1U);
  EXPECT_EQ(vcd->codecs()[0].name, custom_video_codec.name);
}

TEST_F(MediaSessionDescriptionFactoryTest, TestCreateAnswerWithCustomCodecs) {
  MediaSessionOptions offer_opts;
  MediaSessionOptions answer_opts;

  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &offer_opts);
  // Create custom codecs and add to answer. These will override
  // the normally generated codec list in the answer.
  // This breaks O/A rules - the responsibility for obeying those is
  // on the caller, not on this function.
  SdpAudioFormat audio_format("custom-audio", 8000, 2);
  Codec custom_audio_codec = CreateAudioCodec(audio_format);
  custom_audio_codec.id = 123;  // picked at random, but valid
  auto audio_options =
      MediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                              RtpTransceiverDirection::kSendRecv, kActive);
  audio_options.codecs_to_include.push_back(custom_audio_codec);
  answer_opts.media_description_options.push_back(audio_options);

  Codec custom_video_codec = CreateVideoCodec("custom-video");
  custom_video_codec.id = 124;
  auto video_options =
      MediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                              RtpTransceiverDirection::kSendRecv, kActive);
  video_options.codecs_to_include.push_back(custom_video_codec);
  answer_opts.media_description_options.push_back(video_options);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f1_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  EXPECT_EQ(MediaProtocolType::kRtp, vc->type);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  ASSERT_EQ(acd->codecs().size(), 1U);
  // Fields in codec are set during the gen process, so simple compare
  // does not work.
  EXPECT_EQ(acd->codecs()[0].name, custom_audio_codec.name);

  EXPECT_EQ(webrtc::MediaType::VIDEO, vcd->type());
  ASSERT_EQ(vcd->codecs().size(), 1U);
  EXPECT_EQ(vcd->codecs()[0].name, custom_video_codec.name);
}

// Test creating an offer with bundle where the Codecs have the same dynamic
// RTP paylod type. The test verifies that the offer don't contain the
// duplicate RTP payload types.
TEST_F(MediaSessionDescriptionFactoryTest, TestBundleOfferWithSameCodecPlType) {
  Codec offered_video_codec =
      codec_lookup_helper_2_.GetCodecVendor()->video_sendrecv_codecs()[0];
  Codec offered_audio_codec =
      codec_lookup_helper_2_.GetCodecVendor()->audio_sendrecv_codecs()[0];
  ASSERT_EQ(offered_video_codec.id, offered_audio_codec.id);

  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  opts.bundle_enabled = true;
  std::unique_ptr<SessionDescription> offer =
      f2_.CreateOfferOrError(opts, nullptr).MoveValue();
  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(offer.get());
  const AudioContentDescription* acd =
      GetFirstAudioContentDescription(offer.get());
  ASSERT_TRUE(vcd);
  ASSERT_TRUE(acd);
  EXPECT_NE(vcd->codecs()[0].id, acd->codecs()[0].id);
  EXPECT_EQ(vcd->codecs()[0].name, offered_video_codec.name);
  EXPECT_EQ(acd->codecs()[0].name, offered_audio_codec.name);
}

// Test creating an updated offer with bundle, audio, video and data
// after an audio only session has been negotiated.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateUpdatedVideoOfferWithBundle) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kInactive, kStopped,
                             &opts);
  opts.bundle_enabled = true;
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  MediaSessionOptions updated_opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &updated_opts);
  updated_opts.bundle_enabled = true;
  std::unique_ptr<SessionDescription> updated_offer(
      f1_.CreateOfferOrError(updated_opts, answer.get()).MoveValue());

  const AudioContentDescription* acd =
      GetFirstAudioContentDescription(updated_offer.get());
  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(updated_offer.get());
  EXPECT_TRUE(vcd);
  EXPECT_TRUE(acd);

  EXPECT_EQ(kMediaProtocolDtlsSavpf, acd->protocol());
  EXPECT_EQ(kMediaProtocolDtlsSavpf, vcd->protocol());
}

// Create an SCTP data offer with bundle without error.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateSctpDataOffer) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  EXPECT_TRUE(offer.get());
  EXPECT_TRUE(offer->GetContentByName("data"));
  auto dcd = GetFirstSctpDataContentDescription(offer.get());
  ASSERT_TRUE(dcd);
  // Since this transport is insecure, the protocol should be "SCTP".
  EXPECT_EQ(kMediaProtocolUdpDtlsSctp, dcd->protocol());
}

// Create an SCTP data offer with bundle without error.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateSecureSctpDataOffer) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  EXPECT_TRUE(offer.get());
  EXPECT_TRUE(offer->GetContentByName("data"));
  auto dcd = GetFirstSctpDataContentDescription(offer.get());
  ASSERT_TRUE(dcd);
  // The protocol should now be "UDP/DTLS/SCTP"
  EXPECT_EQ(kMediaProtocolUdpDtlsSctp, dcd->protocol());
}

// Test creating an sctp data channel from an already generated offer.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateImplicitSctpDataOffer) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer1(
      f1_.CreateOfferOrError(opts, nullptr).MoveValue());
  ASSERT_TRUE(offer1.get());
  const ContentInfo* data = offer1->GetContentByName("data");
  ASSERT_TRUE(data);
  ASSERT_EQ(kMediaProtocolUdpDtlsSctp, data->media_description()->protocol());

  std::unique_ptr<SessionDescription> offer2(
      f1_.CreateOfferOrError(opts, offer1.get()).MoveValue());
  data = offer2->GetContentByName("data");
  ASSERT_TRUE(data);
  EXPECT_EQ(kMediaProtocolUdpDtlsSctp, data->media_description()->protocol());
}

// Test that if BUNDLE is enabled and all media sections are rejected then the
// BUNDLE group is not present in the re-offer.
TEST_F(MediaSessionDescriptionFactoryTest, ReOfferNoBundleGroupIfAllRejected) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  opts.media_description_options[0].stopped = true;
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue();

  EXPECT_FALSE(reoffer->GetGroupByName(GROUP_TYPE_BUNDLE));
}

// Test that if BUNDLE is enabled and the remote re-offer does not include a
// BUNDLE group since all media sections are rejected, then the re-answer also
// does not include a BUNDLE group.
TEST_F(MediaSessionDescriptionFactoryTest, ReAnswerNoBundleGroupIfAllRejected) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  opts.media_description_options[0].stopped = true;
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue();
  std::unique_ptr<SessionDescription> reanswer =
      f2_.CreateAnswerOrError(reoffer.get(), opts, answer.get()).MoveValue();

  EXPECT_FALSE(reanswer->GetGroupByName(GROUP_TYPE_BUNDLE));
}

// Test that if BUNDLE is enabled and the previous offerer-tagged media section
// was rejected then the new offerer-tagged media section is the non-rejected
// media section.
TEST_F(MediaSessionDescriptionFactoryTest, ReOfferChangeBundleOffererTagged) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  // Reject the audio m= section and add a video m= section.
  opts.media_description_options[0].stopped = true;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue();

  const ContentGroup* bundle_group = reoffer->GetGroupByName(GROUP_TYPE_BUNDLE);
  ASSERT_TRUE(bundle_group);
  EXPECT_FALSE(bundle_group->HasContentName("audio"));
  EXPECT_TRUE(bundle_group->HasContentName("video"));
}

// Test that if BUNDLE is enabled and the previous offerer-tagged media section
// was rejected and a new media section is added, then the re-answer BUNDLE
// group will contain only the non-rejected media section.
TEST_F(MediaSessionDescriptionFactoryTest, ReAnswerChangedBundleOffererTagged) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Reject the audio m= section and add a video m= section.
  opts.media_description_options[0].stopped = true;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue();
  std::unique_ptr<SessionDescription> reanswer =
      f2_.CreateAnswerOrError(reoffer.get(), opts, answer.get()).MoveValue();

  const ContentGroup* bundle_group =
      reanswer->GetGroupByName(GROUP_TYPE_BUNDLE);
  ASSERT_TRUE(bundle_group);
  EXPECT_FALSE(bundle_group->HasContentName("audio"));
  EXPECT_TRUE(bundle_group->HasContentName("video"));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       CreateAnswerForOfferWithMultipleBundleGroups) {
  // Create an offer with 4 m= sections, initially without BUNDLE groups.
  MediaSessionOptions opts;
  opts.bundle_enabled = false;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "3",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "4",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer->groups().empty());

  // Munge the offer to have two groups. Offers like these cannot be generated
  // without munging, but it is valid to receive such offers from remote
  // endpoints.
  ContentGroup bundle_group1(GROUP_TYPE_BUNDLE);
  bundle_group1.AddContentName("1");
  bundle_group1.AddContentName("2");
  ContentGroup bundle_group2(GROUP_TYPE_BUNDLE);
  bundle_group2.AddContentName("3");
  bundle_group2.AddContentName("4");
  offer->AddGroup(bundle_group1);
  offer->AddGroup(bundle_group2);

  // If BUNDLE is enabled, the answer to this offer should accept both BUNDLE
  // groups.
  opts.bundle_enabled = true;
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  std::vector<const ContentGroup*> answer_groups =
      answer->GetGroupsByName(GROUP_TYPE_BUNDLE);
  ASSERT_EQ(answer_groups.size(), 2u);
  EXPECT_EQ(answer_groups[0]->content_names().size(), 2u);
  EXPECT_TRUE(answer_groups[0]->HasContentName("1"));
  EXPECT_TRUE(answer_groups[0]->HasContentName("2"));
  EXPECT_EQ(answer_groups[1]->content_names().size(), 2u);
  EXPECT_TRUE(answer_groups[1]->HasContentName("3"));
  EXPECT_TRUE(answer_groups[1]->HasContentName("4"));

  // If BUNDLE is disabled, the answer to this offer should reject both BUNDLE
  // groups.
  opts.bundle_enabled = false;
  answer = f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  answer_groups = answer->GetGroupsByName(GROUP_TYPE_BUNDLE);
  // Rejected groups are still listed, but they are empty.
  ASSERT_EQ(answer_groups.size(), 2u);
  EXPECT_TRUE(answer_groups[0]->content_names().empty());
  EXPECT_TRUE(answer_groups[1]->content_names().empty());
}

// Test that if the BUNDLE offerer-tagged media section is changed in a reoffer
// and there is still a non-rejected media section that was in the initial
// offer, then the ICE credentials do not change in the reoffer offerer-tagged
// media section.
TEST_F(MediaSessionDescriptionFactoryTest,
       ReOfferChangeBundleOffererTaggedKeepsIceCredentials) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Reject the audio m= section.
  opts.media_description_options[0].stopped = true;
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue();

  const TransportDescription* offer_tagged =
      offer->GetTransportDescriptionByName("audio");
  ASSERT_TRUE(offer_tagged);
  const TransportDescription* reoffer_tagged =
      reoffer->GetTransportDescriptionByName("video");
  ASSERT_TRUE(reoffer_tagged);
  EXPECT_EQ(offer_tagged->ice_ufrag, reoffer_tagged->ice_ufrag);
  EXPECT_EQ(offer_tagged->ice_pwd, reoffer_tagged->ice_pwd);
}

// Test that if the BUNDLE offerer-tagged media section is changed in a reoffer
// and there is still a non-rejected media section that was in the initial
// offer, then the ICE credentials do not change in the reanswer answerer-tagged
// media section.
TEST_F(MediaSessionDescriptionFactoryTest,
       ReAnswerChangeBundleOffererTaggedKeepsIceCredentials) {
  MediaSessionOptions opts;
  opts.bundle_enabled = true;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Reject the audio m= section.
  opts.media_description_options[0].stopped = true;
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue();
  std::unique_ptr<SessionDescription> reanswer =
      f2_.CreateAnswerOrError(reoffer.get(), opts, answer.get()).MoveValue();

  const TransportDescription* answer_tagged =
      answer->GetTransportDescriptionByName("audio");
  ASSERT_TRUE(answer_tagged);
  const TransportDescription* reanswer_tagged =
      reanswer->GetTransportDescriptionByName("video");
  ASSERT_TRUE(reanswer_tagged);
  EXPECT_EQ(answer_tagged->ice_ufrag, reanswer_tagged->ice_ufrag);
  EXPECT_EQ(answer_tagged->ice_pwd, reanswer_tagged->ice_pwd);
}

// Create an audio, video offer without legacy StreamParams.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateOfferWithoutLegacyStreams) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* ac = offer->GetContentByName("audio");
  const ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();

  EXPECT_FALSE(vcd->has_ssrcs());  // No StreamParams.
  EXPECT_FALSE(acd->has_ssrcs());  // No StreamParams.
}

// Creates an audio+video sendonly offer.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateSendOnlyOffer) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kSendOnly, &opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);
  AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                        kAudioTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  EXPECT_EQ(2u, offer->contents().size());
  EXPECT_TRUE(
      IsMediaContentOfType(&offer->contents()[0], webrtc::MediaType::AUDIO));
  EXPECT_TRUE(
      IsMediaContentOfType(&offer->contents()[1], webrtc::MediaType::VIDEO));

  EXPECT_EQ(RtpTransceiverDirection::kSendOnly,
            GetMediaDirection(&offer->contents()[0]));
  EXPECT_EQ(RtpTransceiverDirection::kSendOnly,
            GetMediaDirection(&offer->contents()[1]));
}

// Verifies that the order of the media contents in the current
// SessionDescription is preserved in the new SessionDescription.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateOfferContentOrder) {
  MediaSessionOptions opts;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);

  std::unique_ptr<SessionDescription> offer1(
      f1_.CreateOfferOrError(opts, nullptr).MoveValue());
  ASSERT_TRUE(offer1.get());
  EXPECT_EQ(1u, offer1->contents().size());
  EXPECT_TRUE(
      IsMediaContentOfType(&offer1->contents()[0], webrtc::MediaType::DATA));

  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer2(
      f1_.CreateOfferOrError(opts, offer1.get()).MoveValue());
  ASSERT_TRUE(offer2.get());
  EXPECT_EQ(2u, offer2->contents().size());
  EXPECT_TRUE(
      IsMediaContentOfType(&offer2->contents()[0], webrtc::MediaType::DATA));
  EXPECT_TRUE(
      IsMediaContentOfType(&offer2->contents()[1], webrtc::MediaType::VIDEO));

  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer3(
      f1_.CreateOfferOrError(opts, offer2.get()).MoveValue());
  ASSERT_TRUE(offer3.get());
  EXPECT_EQ(3u, offer3->contents().size());
  EXPECT_TRUE(
      IsMediaContentOfType(&offer3->contents()[0], webrtc::MediaType::DATA));
  EXPECT_TRUE(
      IsMediaContentOfType(&offer3->contents()[1], webrtc::MediaType::VIDEO));
  EXPECT_TRUE(
      IsMediaContentOfType(&offer3->contents()[2], webrtc::MediaType::AUDIO));
}

// Create a typical audio answer, and ensure it matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateAudioAnswer) {
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(CreateAudioMediaSession(), nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), CreateAudioMediaSession(), nullptr)
          .MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  EXPECT_FALSE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));
  EXPECT_EQ(0U, acd->first_ssrc());             // no sender is attached
  EXPECT_EQ(kAutoBandwidth, acd->bandwidth());  // negotiated auto bw
  EXPECT_TRUE(acd->rtcp_mux());                 // negotiated rtcp-mux
  EXPECT_EQ(kMediaProtocolDtlsSavpf, acd->protocol());
}

// Create a typical audio answer with GCM ciphers enabled, and ensure it
// matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateAudioAnswerGcm) {
  MediaSessionOptions opts = CreateAudioMediaSession();
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  EXPECT_FALSE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));
  EXPECT_EQ(0U, acd->first_ssrc());             // no sender is attached
  EXPECT_EQ(kAutoBandwidth, acd->bandwidth());  // negotiated auto bw
  EXPECT_TRUE(acd->rtcp_mux());                 // negotiated rtcp-mux
  EXPECT_EQ(kMediaProtocolDtlsSavpf, acd->protocol());
}

// Create an audio answer with no common codecs, and ensure it is rejected.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateAudioAnswerWithNoCommonCodecs) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::vector f1_codecs = {CreateAudioCodec(96, "opus", 48000, 1)};
  codec_lookup_helper_1_.GetCodecVendor()->set_audio_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector f2_codecs = {CreateAudioCodec(0, "PCMU", 8000, 1)};
  codec_lookup_helper_2_.GetCodecVendor()->set_audio_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  ASSERT_TRUE(ac);
  EXPECT_TRUE(ac->rejected);
}

// Create a typical video answer, and ensure it matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateVideoAnswer) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  EXPECT_EQ(MediaProtocolType::kRtp, vc->type);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));
  EXPECT_EQ(kAutoBandwidth, acd->bandwidth());  // negotiated auto bw
  EXPECT_EQ(0U, acd->first_ssrc());             // no sender is attached
  EXPECT_TRUE(acd->rtcp_mux());                 // negotiated rtcp-mux
  EXPECT_EQ(webrtc::MediaType::VIDEO, vcd->type());
  EXPECT_THAT(vcd->codecs(), ElementsAreArray(kVideoCodecsAnswer));
  EXPECT_EQ(0U, vcd->first_ssrc());  // no sender is attached
  EXPECT_TRUE(vcd->rtcp_mux());      // negotiated rtcp-mux
  EXPECT_EQ(kMediaProtocolDtlsSavpf, vcd->protocol());
}

// Create a video answer with no common codecs, and ensure it is rejected.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateVideoAnswerWithNoCommonCodecs) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::vector f1_codecs = {CreateVideoCodec(96, "H264")};
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector f2_codecs = {CreateVideoCodec(97, "VP8")};
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(vc);
  EXPECT_TRUE(vc->rejected);
}

// Create a video answer with no common codecs (but a common FEC codec), and
// ensure it is rejected.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateVideoAnswerWithOnlyFecCodecsCommon) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::vector f1_codecs = {CreateVideoCodec(96, "H264"),
                           CreateVideoCodec(118, "flexfec-03")};
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector f2_codecs = {CreateVideoCodec(97, "VP8"),
                           CreateVideoCodec(118, "flexfec-03")};
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(vc);
  EXPECT_TRUE(vc->rejected);
}

// The use_sctpmap flag should be set in an Sctp DataContentDescription by
// default. The answer's use_sctpmap flag should match the offer's.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateDataAnswerUsesSctpmap) {
  MediaSessionOptions opts;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* dc_offer = offer->GetContentByName("data");
  ASSERT_TRUE(dc_offer);
  SctpDataContentDescription* dcd_offer =
      dc_offer->media_description()->as_sctp();
  EXPECT_TRUE(dcd_offer->use_sctpmap());

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* dc_answer = answer->GetContentByName("data");
  ASSERT_TRUE(dc_answer);
  const SctpDataContentDescription* dcd_answer =
      dc_answer->media_description()->as_sctp();
  EXPECT_TRUE(dcd_answer->use_sctpmap());
}

// The answer's use_sctpmap flag should match the offer's.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateDataAnswerWithoutSctpmap) {
  MediaSessionOptions opts;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* dc_offer = offer->GetContentByName("data");
  ASSERT_TRUE(dc_offer);
  SctpDataContentDescription* dcd_offer =
      dc_offer->media_description()->as_sctp();
  dcd_offer->set_use_sctpmap(false);

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* dc_answer = answer->GetContentByName("data");
  ASSERT_TRUE(dc_answer);
  const SctpDataContentDescription* dcd_answer =
      dc_answer->media_description()->as_sctp();
  EXPECT_FALSE(dcd_answer->use_sctpmap());
}

// Test that a valid answer will be created for "DTLS/SCTP", "UDP/DTLS/SCTP"
// and "TCP/DTLS/SCTP" offers.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateDataAnswerToDifferentOfferedProtos) {
  MediaSessionOptions opts;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* dc_offer = offer->GetContentByName("data");
  ASSERT_TRUE(dc_offer);
  SctpDataContentDescription* dcd_offer =
      dc_offer->media_description()->as_sctp();
  ASSERT_TRUE(dcd_offer);

  std::vector<std::string> protos = {"DTLS/SCTP", "UDP/DTLS/SCTP",
                                     "TCP/DTLS/SCTP"};
  for (const std::string& proto : protos) {
    dcd_offer->set_protocol(proto);
    std::unique_ptr<SessionDescription> answer =
        f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
    const ContentInfo* dc_answer = answer->GetContentByName("data");
    ASSERT_TRUE(dc_answer);
    const SctpDataContentDescription* dcd_answer =
        dc_answer->media_description()->as_sctp();
    EXPECT_FALSE(dc_answer->rejected);
    EXPECT_EQ(proto, dcd_answer->protocol());
  }
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateDataAnswerToOfferWithDefinedMessageSize) {
  MediaSessionOptions opts;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* dc_offer = offer->GetContentByName("data");
  ASSERT_TRUE(dc_offer);
  SctpDataContentDescription* dcd_offer =
      dc_offer->media_description()->as_sctp();
  ASSERT_TRUE(dcd_offer);
  dcd_offer->set_max_message_size(1234);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* dc_answer = answer->GetContentByName("data");
  ASSERT_TRUE(dc_answer);
  const SctpDataContentDescription* dcd_answer =
      dc_answer->media_description()->as_sctp();
  EXPECT_FALSE(dc_answer->rejected);
  EXPECT_EQ(1234, dcd_answer->max_message_size());
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateDataAnswerToOfferWithZeroMessageSize) {
  MediaSessionOptions opts;
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* dc_offer = offer->GetContentByName("data");
  ASSERT_TRUE(dc_offer);
  SctpDataContentDescription* dcd_offer =
      dc_offer->media_description()->as_sctp();
  ASSERT_TRUE(dcd_offer);
  dcd_offer->set_max_message_size(0);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* dc_answer = answer->GetContentByName("data");
  ASSERT_TRUE(dc_answer);
  const SctpDataContentDescription* dcd_answer =
      dc_answer->media_description()->as_sctp();
  EXPECT_FALSE(dc_answer->rejected);
  EXPECT_EQ(webrtc::kSctpSendBufferSize, dcd_answer->max_message_size());
}

// Verifies that the order of the media contents in the offer is preserved in
// the answer.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateAnswerContentOrder) {
  MediaSessionOptions opts;

  // Creates a data only offer.
  AddDataSection(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer1(
      f1_.CreateOfferOrError(opts, nullptr).MoveValue());
  ASSERT_TRUE(offer1.get());

  // Appends audio to the offer.
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer2(
      f1_.CreateOfferOrError(opts, offer1.get()).MoveValue());
  ASSERT_TRUE(offer2.get());

  // Appends video to the offer.
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer3(
      f1_.CreateOfferOrError(opts, offer2.get()).MoveValue());
  ASSERT_TRUE(offer3.get());

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer3.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer.get());
  EXPECT_EQ(3u, answer->contents().size());
  EXPECT_TRUE(
      IsMediaContentOfType(&answer->contents()[0], webrtc::MediaType::DATA));
  EXPECT_TRUE(
      IsMediaContentOfType(&answer->contents()[1], webrtc::MediaType::AUDIO));
  EXPECT_TRUE(
      IsMediaContentOfType(&answer->contents()[2], webrtc::MediaType::VIDEO));
}

// TODO(deadbeef): Extend these tests to ensure the correct direction with other
// answerer settings.

// This test that the media direction is set to send/receive in an answer if
// the offer is send receive.
TEST_F(MediaSessionDescriptionFactoryTest, CreateAnswerToSendReceiveOffer) {
  TestMediaDirectionInAnswer(RtpTransceiverDirection::kSendRecv,
                             RtpTransceiverDirection::kSendRecv);
}

// This test that the media direction is set to receive only in an answer if
// the offer is send only.
TEST_F(MediaSessionDescriptionFactoryTest, CreateAnswerToSendOnlyOffer) {
  TestMediaDirectionInAnswer(RtpTransceiverDirection::kSendOnly,
                             RtpTransceiverDirection::kRecvOnly);
}

// This test that the media direction is set to send only in an answer if
// the offer is recv only.
TEST_F(MediaSessionDescriptionFactoryTest, CreateAnswerToRecvOnlyOffer) {
  TestMediaDirectionInAnswer(RtpTransceiverDirection::kRecvOnly,
                             RtpTransceiverDirection::kSendOnly);
}

// This test that the media direction is set to inactive in an answer if
// the offer is inactive.
TEST_F(MediaSessionDescriptionFactoryTest, CreateAnswerToInactiveOffer) {
  TestMediaDirectionInAnswer(RtpTransceiverDirection::kInactive,
                             RtpTransceiverDirection::kInactive);
}

// Test that the media protocol is RTP/AVPF if DTLS is disabled.
TEST_F(MediaSessionDescriptionFactoryTest, AudioOfferAnswerWithCryptoDisabled) {
  MediaSessionOptions opts = CreateAudioMediaSession();
  tdf1_.SetInsecureForTesting();
  tdf1_.set_certificate(nullptr);
  tdf2_.SetInsecureForTesting();
  tdf2_.set_certificate(nullptr);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  const AudioContentDescription* offer_acd =
      GetFirstAudioContentDescription(offer.get());
  ASSERT_TRUE(offer_acd);
  EXPECT_EQ(kMediaProtocolAvpf, offer_acd->protocol());

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  const ContentInfo* ac_answer = answer->GetContentByName("audio");
  ASSERT_TRUE(ac_answer);
  EXPECT_FALSE(ac_answer->rejected);

  const AudioContentDescription* answer_acd =
      GetFirstAudioContentDescription(answer.get());
  ASSERT_TRUE(answer_acd);
  EXPECT_EQ(kMediaProtocolAvpf, answer_acd->protocol());
}

// Create a audio/video offer and answer and ensure that the
// TransportSequenceNumber RTP v1 and v2 header extensions are handled
// correctly.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV1LocalAndV1InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01),   // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01),   // Offer.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01));  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV1LocalAndV1V2InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01),       // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01And02),  // Offer.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01));  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV1LocalAndV2InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01),  // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02),  // Offer.
      {});                                                  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV2LocalAndV1InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02),  // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01),  // Offer.
      {});                                                  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV2LocalAndV1V2InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02),       // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01And02),  // Offer.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02));  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV2LocalAndV2InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02),   // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02),   // Offer.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02));  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV1V2LocalAndV1InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01And02),  // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01),       // Offer.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01));  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV1V2LocalAndV2InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01And02),  // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02),       // Offer.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber02));  // Expected answer.
}
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithTransportSequenceNumberV1V2LocalAndV1V2InOffer) {
  TestTransportSequenceNumberNegotiation(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01And02),  // Local.
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01And02),  // Offer.
      MAKE_VECTOR(
          kRtpExtensionTransportSequenceNumber01And02));  // Expected answer.
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestNegotiateFrameDescriptorWhenUnexposedLocally) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  SetAudioVideoRtpHeaderExtensions(
      MAKE_VECTOR(kRtpExtensionGenericFrameDescriptorUri00),
      MAKE_VECTOR(kRtpExtensionGenericFrameDescriptorUri00), &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  SetAudioVideoRtpHeaderExtensions(
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01),
      MAKE_VECTOR(kRtpExtensionTransportSequenceNumber01), &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(kRtpExtensionGenericFrameDescriptorUri00));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(kRtpExtensionGenericFrameDescriptorUri00));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestNegotiateFrameDescriptorWhenExposedLocally) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  SetAudioVideoRtpHeaderExtensions(
      MAKE_VECTOR(kRtpExtensionGenericFrameDescriptorUri00),
      MAKE_VECTOR(kRtpExtensionGenericFrameDescriptorUri00), &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(kRtpExtensionGenericFrameDescriptorUri00));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(kRtpExtensionGenericFrameDescriptorUri00));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       NegotiateDependencyDescriptorWhenUnexposedLocally) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  RtpExtension offer_dd(RtpExtension::kDependencyDescriptorUri, 7);
  SetAudioVideoRtpHeaderExtensions({}, {offer_dd}, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  RtpExtension local_tsn(RtpExtension::kTransportSequenceNumberUri, 5);
  SetAudioVideoRtpHeaderExtensions({}, {local_tsn}, &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAre(offer_dd));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       NegotiateDependencyDescriptorWhenExposedLocally) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  RtpExtension offer_dd(RtpExtension::kDependencyDescriptorUri, 7);
  RtpExtension local_dd(RtpExtension::kDependencyDescriptorUri, 5);
  SetAudioVideoRtpHeaderExtensions({}, {offer_dd}, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  SetAudioVideoRtpHeaderExtensions({}, {local_dd}, &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAre(offer_dd));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       NegotiateAbsoluteCaptureTimeWhenUnexposedLocally) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  const RtpHeaderExtensions offered_extensions = {
      RtpExtension(RtpExtension::kAbsoluteCaptureTimeUri, 7)};
  const RtpHeaderExtensions local_extensions = {
      RtpExtension(RtpExtension::kTransportSequenceNumberUri, 5)};
  SetAudioVideoRtpHeaderExtensions(offered_extensions, offered_extensions,
                                   &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  SetAudioVideoRtpHeaderExtensions(local_extensions, local_extensions, &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(offered_extensions));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(offered_extensions));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       NegotiateAbsoluteCaptureTimeWhenExposedLocally) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  const RtpHeaderExtensions offered_extensions = {
      RtpExtension(RtpExtension::kAbsoluteCaptureTimeUri, 7)};
  const RtpHeaderExtensions local_extensions = {
      RtpExtension(RtpExtension::kAbsoluteCaptureTimeUri, 5)};
  SetAudioVideoRtpHeaderExtensions(offered_extensions, offered_extensions,
                                   &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  SetAudioVideoRtpHeaderExtensions(local_extensions, local_extensions, &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(offered_extensions));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      ElementsAreArray(offered_extensions));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       DoNotNegotiateAbsoluteCaptureTimeWhenNotOffered) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  const RtpHeaderExtensions offered_extensions = {
      RtpExtension(RtpExtension::kTransportSequenceNumberUri, 7)};
  const RtpHeaderExtensions local_extensions = {
      RtpExtension(RtpExtension::kAbsoluteCaptureTimeUri, 5)};
  SetAudioVideoRtpHeaderExtensions(offered_extensions, offered_extensions,
                                   &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  SetAudioVideoRtpHeaderExtensions(local_extensions, local_extensions, &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      IsEmpty());
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      IsEmpty());
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OffersUnstoppedExtensionsWithAudioVideoExtensionStopped) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 1,
                                   RtpTransceiverDirection::kStopped),
      RtpHeaderExtensionCapability("uri2", 3,
                                   RtpTransceiverDirection::kSendOnly)};
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 1,
                                   RtpTransceiverDirection::kStopped),
      RtpHeaderExtensionCapability("uri3", 7,
                                   RtpTransceiverDirection::kSendOnly)};
  auto offer = f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  EXPECT_THAT(
      offer->contents(),
      ElementsAre(
          Property(&ContentInfo::media_description,
                   Pointee(Property(
                       &MediaContentDescription::rtp_header_extensions,
                       ElementsAre(Field(&RtpExtension::uri, "uri2"))))),
          Property(&ContentInfo::media_description,
                   Pointee(Property(
                       &MediaContentDescription::rtp_header_extensions,
                       ElementsAre(Field(&RtpExtension::uri, "uri3")))))));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OffersUnstoppedExtensionsWithAudioExtensionStopped) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 1,
                                   RtpTransceiverDirection::kSendOnly),
      RtpHeaderExtensionCapability("uri2", 3,
                                   RtpTransceiverDirection::kStopped)};
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri42", 42,
                                   RtpTransceiverDirection::kSendRecv),
      RtpHeaderExtensionCapability("uri3", 7,
                                   RtpTransceiverDirection::kSendOnly)};
  auto offer = f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  EXPECT_THAT(
      offer->contents(),
      ElementsAre(
          Property(&ContentInfo::media_description,
                   Pointee(Property(
                       &MediaContentDescription::rtp_header_extensions,
                       ElementsAre(Field(&RtpExtension::uri, "uri1"))))),
          Property(
              &ContentInfo::media_description,
              Pointee(Property(
                  &MediaContentDescription::rtp_header_extensions,
                  UnorderedElementsAre(Field(&RtpExtension::uri, "uri3"),
                                       Field(&RtpExtension::uri, "uri42")))))));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OffersUnstoppedExtensionsWithVideoExtensionStopped) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 5,
                                   RtpTransceiverDirection::kSendOnly),
      RtpHeaderExtensionCapability("uri2", 7,
                                   RtpTransceiverDirection::kSendRecv)};
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri42", 42,
                                   RtpTransceiverDirection::kSendRecv),
      RtpHeaderExtensionCapability("uri3", 7,
                                   RtpTransceiverDirection::kStopped)};
  auto offer = f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  EXPECT_THAT(
      offer->contents(),
      ElementsAre(
          Property(
              &ContentInfo::media_description,
              Pointee(Property(
                  &MediaContentDescription::rtp_header_extensions,
                  UnorderedElementsAre(Field(&RtpExtension::uri, "uri1"),
                                       Field(&RtpExtension::uri, "uri2"))))),
          Property(&ContentInfo::media_description,
                   Pointee(Property(
                       &MediaContentDescription::rtp_header_extensions,
                       ElementsAre(Field(&RtpExtension::uri, "uri42")))))));
}

TEST_F(MediaSessionDescriptionFactoryTest, AnswersUnstoppedExtensions) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 4,
                                   RtpTransceiverDirection::kStopped),
      RtpHeaderExtensionCapability("uri2", 3,
                                   RtpTransceiverDirection::kSendOnly),
      RtpHeaderExtensionCapability("uri3", 2,
                                   RtpTransceiverDirection::kRecvOnly),
      RtpHeaderExtensionCapability("uri4", 1,
                                   RtpTransceiverDirection::kSendRecv)};
  auto offer = f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 4,
                                   RtpTransceiverDirection::kSendOnly),
      RtpHeaderExtensionCapability("uri2", 3,
                                   RtpTransceiverDirection::kRecvOnly),
      RtpHeaderExtensionCapability("uri3", 2,
                                   RtpTransceiverDirection::kStopped),
      RtpHeaderExtensionCapability("uri4", 1,
                                   RtpTransceiverDirection::kSendRecv)};
  auto answer = f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  EXPECT_THAT(
      answer->contents(),
      ElementsAre(Property(
          &ContentInfo::media_description,
          Pointee(Property(&MediaContentDescription::rtp_header_extensions,
                           ElementsAre(Field(&RtpExtension::uri, "uri2"),
                                       Field(&RtpExtension::uri, "uri4")))))));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       AppendsUnstoppedExtensionsToCurrentDescription) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 1,
                                   RtpTransceiverDirection::kSendRecv)};
  auto offer = f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 2,
                                   RtpTransceiverDirection::kSendRecv),
      RtpHeaderExtensionCapability("uri2", 3,
                                   RtpTransceiverDirection::kRecvOnly),
      RtpHeaderExtensionCapability("uri3", 5,
                                   RtpTransceiverDirection::kStopped),
      RtpHeaderExtensionCapability("uri4", 6,
                                   RtpTransceiverDirection::kSendRecv)};
  auto offer2 = f1_.CreateOfferOrError(opts, offer.get()).MoveValue();
  EXPECT_THAT(
      offer2->contents(),
      ElementsAre(Property(
          &ContentInfo::media_description,
          Pointee(Property(&MediaContentDescription::rtp_header_extensions,
                           ElementsAre(Field(&RtpExtension::uri, "uri1"),
                                       Field(&RtpExtension::uri, "uri2"),
                                       Field(&RtpExtension::uri, "uri4")))))));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       AllowsStoppedExtensionsToBeRemovedFromSubsequentOffer) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 1,
                                   RtpTransceiverDirection::kSendRecv),
      RtpHeaderExtensionCapability("uri2", 2,
                                   RtpTransceiverDirection::kSendRecv)};
  auto offer = f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  // Check that a subsequent offer after setting "uri2" to stopped no longer
  // contains the extension.
  opts.media_description_options.back().header_extensions = {
      RtpHeaderExtensionCapability("uri1", 1,
                                   RtpTransceiverDirection::kSendRecv),
      RtpHeaderExtensionCapability("uri2", 2,
                                   RtpTransceiverDirection::kStopped)};
  auto offer2 = f1_.CreateOfferOrError(opts, offer.get()).MoveValue();
  EXPECT_THAT(
      offer2->contents(),
      ElementsAre(Property(
          &ContentInfo::media_description,
          Pointee(Property(&MediaContentDescription::rtp_header_extensions,
                           ElementsAre(Field(&RtpExtension::uri, "uri1")))))));
}

// Create a video offer and answer and ensure the RTP header extensions
// matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithRtpExtensionHeadersWithNoEncryption) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtension1),
                                   MAKE_VECTOR(kVideoRtpExtension1), &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtension2),
                                   MAKE_VECTOR(kVideoRtpExtension2), &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  EXPECT_THAT(
      GetFirstAudioContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtension1));
  EXPECT_THAT(
      GetFirstVideoContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtension1));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionAnswer));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionAnswer));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestOfferAnswerWithRtpExtensionHeadersWithEncryption) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  f1_.set_enable_encrypted_rtp_header_extensions(true);
  f2_.set_enable_encrypted_rtp_header_extensions(true);

  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtensionEncrypted1),
                                   MAKE_VECTOR(kVideoRtpExtensionEncrypted1),
                                   &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtensionEncrypted2),
                                   MAKE_VECTOR(kVideoRtpExtensionEncrypted2),
                                   &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  EXPECT_THAT(
      GetFirstAudioContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionEncrypted1));
  EXPECT_THAT(
      GetFirstVideoContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionEncrypted1));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionEncryptedAnswer));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionEncryptedAnswer));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       NegotiationWithEncryptedRtpExtensionHeadersDisabledInReceiver) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  f2_.set_enable_encrypted_rtp_header_extensions(false);

  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtensionEncrypted1),
                                   MAKE_VECTOR(kVideoRtpExtensionEncrypted1),
                                   &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtensionEncrypted2),
                                   MAKE_VECTOR(kVideoRtpExtensionEncrypted2),
                                   &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  EXPECT_THAT(
      GetFirstAudioContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionEncrypted1));
  EXPECT_THAT(
      GetFirstVideoContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionEncrypted1));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionAnswer));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionAnswer));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       NegotiationWithEncryptedRtpExtensionHeadersDisabledInSender) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  f1_.set_enable_encrypted_rtp_header_extensions(false);

  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtensionEncrypted1),
                                   MAKE_VECTOR(kVideoRtpExtensionEncrypted1),
                                   &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtensionEncrypted2),
                                   MAKE_VECTOR(kVideoRtpExtensionEncrypted2),
                                   &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  EXPECT_THAT(
      GetFirstAudioContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionAnswer));
  EXPECT_THAT(
      GetFirstVideoContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionAnswer));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionAnswer));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionAnswer));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       PreferEncryptedRtpHeaderExtensionsWhenEncryptionEnabled) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  SetAudioVideoRtpHeaderExtensions(
      MAKE_VECTOR(kAudioRtpExtensionMixedEncryption1),
      MAKE_VECTOR(kVideoRtpExtensionMixedEncryption), &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  SetAudioVideoRtpHeaderExtensions(
      MAKE_VECTOR(kAudioRtpExtensionMixedEncryption2),
      MAKE_VECTOR(kVideoRtpExtensionMixedEncryption), &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer.get());

  EXPECT_THAT(
      GetFirstAudioContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionMixedEncryption1));
  EXPECT_THAT(
      GetFirstVideoContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionMixedEncryption));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(
          kAudioRtpExtensionMixedEncryptionAnswerEncryptionEnabled));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(
          kVideoRtpExtensionMixedEncryptionAnswerEncryptionEnabled));
}

TEST_F(MediaSessionDescriptionFactoryTest,
       UseUnencryptedRtpHeaderExtensionsWhenEncryptionDisabled) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  f1_.set_enable_encrypted_rtp_header_extensions(false);
  f2_.set_enable_encrypted_rtp_header_extensions(false);

  SetAudioVideoRtpHeaderExtensions(
      MAKE_VECTOR(kAudioRtpExtensionMixedEncryption1),
      MAKE_VECTOR(kVideoRtpExtensionMixedEncryption), &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  SetAudioVideoRtpHeaderExtensions(
      MAKE_VECTOR(kAudioRtpExtensionMixedEncryption2),
      MAKE_VECTOR(kVideoRtpExtensionMixedEncryption), &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer.get());

  EXPECT_THAT(
      GetFirstAudioContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(
          kAudioRtpExtensionMixedEncryptionAnswerEncryptionDisabled));
  EXPECT_THAT(
      GetFirstVideoContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(
          kVideoRtpExtensionMixedEncryptionAnswerEncryptionDisabled));
  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(
          kAudioRtpExtensionMixedEncryptionAnswerEncryptionDisabled));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(
          kVideoRtpExtensionMixedEncryptionAnswerEncryptionDisabled));
}

// Create an audio, video, data answer without legacy StreamParams.
TEST_F(MediaSessionDescriptionFactoryTest,
       TestCreateAnswerWithoutLegacyStreams) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();

  EXPECT_FALSE(acd->has_ssrcs());  // No StreamParams.
  EXPECT_FALSE(vcd->has_ssrcs());  // No StreamParams.
}

// Create a typical video answer, and ensure it matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateVideoAnswerRtcpMux) {
  MediaSessionOptions offer_opts;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &offer_opts);

  MediaSessionOptions answer_opts;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &answer_opts);

  std::unique_ptr<SessionDescription> offer;
  std::unique_ptr<SessionDescription> answer;

  offer_opts.rtcp_mux_enabled = true;
  answer_opts.rtcp_mux_enabled = true;
  offer = f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
  ASSERT_TRUE(GetFirstAudioContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstAudioContentDescription(answer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(answer.get()));
  EXPECT_TRUE(GetFirstAudioContentDescription(offer.get())->rtcp_mux());
  EXPECT_TRUE(GetFirstVideoContentDescription(offer.get())->rtcp_mux());
  EXPECT_TRUE(GetFirstAudioContentDescription(answer.get())->rtcp_mux());
  EXPECT_TRUE(GetFirstVideoContentDescription(answer.get())->rtcp_mux());

  offer_opts.rtcp_mux_enabled = true;
  answer_opts.rtcp_mux_enabled = false;
  offer = f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
  ASSERT_TRUE(GetFirstAudioContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstAudioContentDescription(answer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(answer.get()));
  EXPECT_TRUE(GetFirstAudioContentDescription(offer.get())->rtcp_mux());
  EXPECT_TRUE(GetFirstVideoContentDescription(offer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstAudioContentDescription(answer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstVideoContentDescription(answer.get())->rtcp_mux());

  offer_opts.rtcp_mux_enabled = false;
  answer_opts.rtcp_mux_enabled = true;
  offer = f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
  ASSERT_TRUE(GetFirstAudioContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstAudioContentDescription(answer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(answer.get()));
  EXPECT_FALSE(GetFirstAudioContentDescription(offer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstVideoContentDescription(offer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstAudioContentDescription(answer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstVideoContentDescription(answer.get())->rtcp_mux());

  offer_opts.rtcp_mux_enabled = false;
  answer_opts.rtcp_mux_enabled = false;
  offer = f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
  ASSERT_TRUE(GetFirstAudioContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(offer.get()));
  ASSERT_TRUE(GetFirstAudioContentDescription(answer.get()));
  ASSERT_TRUE(GetFirstVideoContentDescription(answer.get()));
  EXPECT_FALSE(GetFirstAudioContentDescription(offer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstVideoContentDescription(offer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstAudioContentDescription(answer.get())->rtcp_mux());
  EXPECT_FALSE(GetFirstVideoContentDescription(answer.get())->rtcp_mux());
}

// Create an audio-only answer to a video offer.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateAudioAnswerToVideo) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());

  opts.media_description_options[1].stopped = true;
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  ASSERT_TRUE(vc->media_description());
  EXPECT_TRUE(vc->rejected);
}

// Create an answer that rejects the contents which are rejected in the offer.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateAnswerToOfferWithRejectedMedia) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* ac = offer->GetContentByName("audio");
  ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  ac->rejected = true;
  vc->rejected = true;
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ac = answer->GetContentByName("audio");
  vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  EXPECT_TRUE(ac->rejected);
  EXPECT_TRUE(vc->rejected);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OfferAndAnswerDoesNotHaveMixedByteSessionAttribute) {
  MediaSessionOptions opts;
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, /*current_description=*/nullptr).MoveValue();
  offer->set_extmap_allow_mixed(false);

  std::unique_ptr<SessionDescription> answer(
      f2_.CreateAnswerOrError(offer.get(), opts,
                              /*current_description=*/nullptr)
          .MoveValue());

  EXPECT_FALSE(answer->extmap_allow_mixed());
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OfferAndAnswerHaveMixedByteSessionAttribute) {
  MediaSessionOptions opts;
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, /*current_description=*/nullptr).MoveValue();
  offer->set_extmap_allow_mixed(true);

  std::unique_ptr<SessionDescription> answer_support(
      f2_.CreateAnswerOrError(offer.get(), opts,
                              /*current_description=*/nullptr)
          .MoveValue());

  EXPECT_TRUE(answer_support->extmap_allow_mixed());
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OfferAndAnswerDoesNotHaveMixedByteMediaAttributes) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, /*current_description=*/nullptr).MoveValue();
  offer->set_extmap_allow_mixed(false);
  MediaContentDescription* audio_offer =
      offer->GetContentDescriptionByName("audio");
  MediaContentDescription* video_offer =
      offer->GetContentDescriptionByName("video");
  ASSERT_EQ(MediaContentDescription::kNo,
            audio_offer->extmap_allow_mixed_enum());
  ASSERT_EQ(MediaContentDescription::kNo,
            video_offer->extmap_allow_mixed_enum());

  std::unique_ptr<SessionDescription> answer(
      f2_.CreateAnswerOrError(offer.get(), opts,
                              /*current_description=*/nullptr)
          .MoveValue());

  MediaContentDescription* audio_answer =
      answer->GetContentDescriptionByName("audio");
  MediaContentDescription* video_answer =
      answer->GetContentDescriptionByName("video");
  EXPECT_EQ(MediaContentDescription::kNo,
            audio_answer->extmap_allow_mixed_enum());
  EXPECT_EQ(MediaContentDescription::kNo,
            video_answer->extmap_allow_mixed_enum());
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OfferAndAnswerHaveSameMixedByteMediaAttributes) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, /*current_description=*/nullptr).MoveValue();
  offer->set_extmap_allow_mixed(false);
  MediaContentDescription* audio_offer =
      offer->GetContentDescriptionByName("audio");
  audio_offer->set_extmap_allow_mixed_enum(MediaContentDescription::kMedia);
  MediaContentDescription* video_offer =
      offer->GetContentDescriptionByName("video");
  video_offer->set_extmap_allow_mixed_enum(MediaContentDescription::kMedia);

  std::unique_ptr<SessionDescription> answer(
      f2_.CreateAnswerOrError(offer.get(), opts,
                              /*current_description=*/nullptr)
          .MoveValue());

  MediaContentDescription* audio_answer =
      answer->GetContentDescriptionByName("audio");
  MediaContentDescription* video_answer =
      answer->GetContentDescriptionByName("video");
  EXPECT_EQ(MediaContentDescription::kMedia,
            audio_answer->extmap_allow_mixed_enum());
  EXPECT_EQ(MediaContentDescription::kMedia,
            video_answer->extmap_allow_mixed_enum());
}

TEST_F(MediaSessionDescriptionFactoryTest,
       OfferAndAnswerHaveDifferentMixedByteMediaAttributes) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, /*current_description=*/nullptr).MoveValue();
  offer->set_extmap_allow_mixed(false);
  MediaContentDescription* audio_offer =
      offer->GetContentDescriptionByName("audio");
  audio_offer->set_extmap_allow_mixed_enum(MediaContentDescription::kNo);
  MediaContentDescription* video_offer =
      offer->GetContentDescriptionByName("video");
  video_offer->set_extmap_allow_mixed_enum(MediaContentDescription::kMedia);

  std::unique_ptr<SessionDescription> answer(
      f2_.CreateAnswerOrError(offer.get(), opts,
                              /*current_description=*/nullptr)
          .MoveValue());

  MediaContentDescription* audio_answer =
      answer->GetContentDescriptionByName("audio");
  MediaContentDescription* video_answer =
      answer->GetContentDescriptionByName("video");
  EXPECT_EQ(MediaContentDescription::kNo,
            audio_answer->extmap_allow_mixed_enum());
  EXPECT_EQ(MediaContentDescription::kMedia,
            video_answer->extmap_allow_mixed_enum());
}

// Create an audio and video offer with:
// - one video track
// - two audio tracks
// and ensure it matches what we expect. Also updates the initial offer by
// adding a new video track and replaces one of the audio tracks.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateMultiStreamVideoOffer) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kSendRecv, &opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);
  AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                        kAudioTrack1, {kMediaStream1}, 1,
                                        &opts);
  AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                        kAudioTrack2, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  ASSERT_TRUE(offer.get());
  const ContentInfo* ac = offer->GetContentByName("audio");
  const ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();
  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_EQ(
      codec_lookup_helper_1_.GetCodecVendor()->audio_sendrecv_codecs().codecs(),
      acd->codecs());

  const StreamParamsVec& audio_streams = acd->streams();
  ASSERT_EQ(2U, audio_streams.size());
  EXPECT_EQ(audio_streams[0].cname, audio_streams[1].cname);
  EXPECT_EQ(kAudioTrack1, audio_streams[0].id);
  ASSERT_EQ(1U, audio_streams[0].ssrcs.size());
  EXPECT_NE(0U, audio_streams[0].ssrcs[0]);
  EXPECT_EQ(kAudioTrack2, audio_streams[1].id);
  ASSERT_EQ(1U, audio_streams[1].ssrcs.size());
  EXPECT_NE(0U, audio_streams[1].ssrcs[0]);

  EXPECT_EQ(kAutoBandwidth,
            acd->bandwidth());                  // default bandwidth (auto)
  EXPECT_TRUE(acd->rtcp_mux());                 // rtcp-mux defaults on

  EXPECT_EQ(webrtc::MediaType::VIDEO, vcd->type());
  EXPECT_EQ(
      codec_lookup_helper_1_.GetCodecVendor()->video_sendrecv_codecs().codecs(),
      vcd->codecs());

  const StreamParamsVec& video_streams = vcd->streams();
  ASSERT_EQ(1U, video_streams.size());
  EXPECT_EQ(video_streams[0].cname, audio_streams[0].cname);
  EXPECT_EQ(kVideoTrack1, video_streams[0].id);
  EXPECT_EQ(kAutoBandwidth,
            vcd->bandwidth());                  // default bandwidth (auto)
  EXPECT_TRUE(vcd->rtcp_mux());                 // rtcp-mux defaults on

  // Update the offer. Add a new video track that is not synched to the
  // other tracks and replace audio track 2 with audio track 3.
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack2, {kMediaStream2}, 1,
                                        &opts);
  DetachSenderFromMediaSection("audio", kAudioTrack2, &opts);
  AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                        kAudioTrack3, {kMediaStream1}, 1,
                                        &opts);
  std::unique_ptr<SessionDescription> updated_offer(
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue());

  ASSERT_TRUE(updated_offer.get());
  ac = updated_offer->GetContentByName("audio");
  vc = updated_offer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  const MediaContentDescription* updated_acd = ac->media_description();
  const MediaContentDescription* updated_vcd = vc->media_description();

  EXPECT_EQ(acd->type(), updated_acd->type());
  EXPECT_EQ(acd->codecs(), updated_acd->codecs());
  EXPECT_EQ(vcd->type(), updated_vcd->type());
  EXPECT_EQ(vcd->codecs(), updated_vcd->codecs());

  const StreamParamsVec& updated_audio_streams = updated_acd->streams();
  ASSERT_EQ(2U, updated_audio_streams.size());
  EXPECT_EQ(audio_streams[0], updated_audio_streams[0]);
  EXPECT_EQ(kAudioTrack3, updated_audio_streams[1].id);  // New audio track.
  ASSERT_EQ(1U, updated_audio_streams[1].ssrcs.size());
  EXPECT_NE(0U, updated_audio_streams[1].ssrcs[0]);
  EXPECT_EQ(updated_audio_streams[0].cname, updated_audio_streams[1].cname);

  const StreamParamsVec& updated_video_streams = updated_vcd->streams();
  ASSERT_EQ(2U, updated_video_streams.size());
  EXPECT_EQ(video_streams[0], updated_video_streams[0]);
  EXPECT_EQ(kVideoTrack2, updated_video_streams[1].id);
  // All the media streams in one PeerConnection share one RTCP CNAME.
  EXPECT_EQ(updated_video_streams[1].cname, updated_video_streams[0].cname);
}

// Create an offer with simulcast video stream.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateSimulcastVideoOffer) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  const int num_sim_layers = 3;
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1},
                                        num_sim_layers, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  ASSERT_TRUE(offer.get());
  const ContentInfo* vc = offer->GetContentByName("video");
  ASSERT_TRUE(vc);
  const MediaContentDescription* vcd = vc->media_description();

  const StreamParamsVec& video_streams = vcd->streams();
  ASSERT_EQ(1U, video_streams.size());
  EXPECT_EQ(kVideoTrack1, video_streams[0].id);
  const SsrcGroup* sim_ssrc_group =
      video_streams[0].get_ssrc_group(kSimSsrcGroupSemantics);
  ASSERT_TRUE(sim_ssrc_group);
  EXPECT_EQ(static_cast<size_t>(num_sim_layers), sim_ssrc_group->ssrcs.size());
}

MATCHER(RidDescriptionEquals, "Verifies that two RidDescriptions are equal.") {
  const RidDescription& rid1 = std::get<0>(arg);
  const RidDescription& rid2 = std::get<1>(arg);
  return rid1.rid == rid2.rid && rid1.direction == rid2.direction;
}

void CheckSimulcastInSessionDescription(
    const SessionDescription* description,
    const std::string& content_name,
    const std::vector<RidDescription>& send_rids,
    const SimulcastLayerList& send_layers) {
  ASSERT_NE(description, nullptr);
  const ContentInfo* content = description->GetContentByName(content_name);
  ASSERT_NE(content, nullptr);
  const MediaContentDescription* cd = content->media_description();
  ASSERT_NE(cd, nullptr);
  const StreamParamsVec& streams = cd->streams();
  ASSERT_THAT(streams, SizeIs(1));
  const StreamParams& stream = streams[0];
  ASSERT_THAT(stream.ssrcs, IsEmpty());
  EXPECT_TRUE(stream.has_rids());
  const std::vector<RidDescription> rids = stream.rids();

  EXPECT_THAT(rids, Pointwise(RidDescriptionEquals(), send_rids));

  EXPECT_TRUE(cd->HasSimulcast());
  const SimulcastDescription& simulcast = cd->simulcast_description();
  EXPECT_THAT(simulcast.send_layers(), SizeIs(send_layers.size()));
  EXPECT_THAT(simulcast.send_layers(), Pointwise(Eq(), send_layers));

  ASSERT_THAT(simulcast.receive_layers().GetAllLayers(), SizeIs(0));
}

// Create an offer with spec-compliant simulcast video stream.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateCompliantSimulcastOffer) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::vector<RidDescription> send_rids;
  send_rids.push_back(RidDescription("f", RidDirection::kSend));
  send_rids.push_back(RidDescription("h", RidDirection::kSend));
  send_rids.push_back(RidDescription("q", RidDirection::kSend));
  SimulcastLayerList simulcast_layers;
  simulcast_layers.AddLayer(SimulcastLayer(send_rids[0].rid, false));
  simulcast_layers.AddLayer(SimulcastLayer(send_rids[1].rid, true));
  simulcast_layers.AddLayer(SimulcastLayer(send_rids[2].rid, false));
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1},
                                        send_rids, simulcast_layers, 0, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  CheckSimulcastInSessionDescription(offer.get(), "video", send_rids,
                                     simulcast_layers);
}

// Create an offer that signals RIDs (not SSRCs) without Simulcast.
// In this scenario, RIDs do not need to be negotiated (there is only one).
TEST_F(MediaSessionDescriptionFactoryTest, TestOfferWithRidsNoSimulcast) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  RidDescription rid("f", RidDirection::kSend);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, {rid},
                                        SimulcastLayerList(), 0, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  ASSERT_NE(offer.get(), nullptr);
  const ContentInfo* content = offer->GetContentByName("video");
  ASSERT_NE(content, nullptr);
  const MediaContentDescription* cd = content->media_description();
  ASSERT_NE(cd, nullptr);
  const StreamParamsVec& streams = cd->streams();
  ASSERT_THAT(streams, SizeIs(1));
  const StreamParams& stream = streams[0];
  ASSERT_THAT(stream.ssrcs, IsEmpty());
  EXPECT_FALSE(stream.has_rids());
  EXPECT_FALSE(cd->HasSimulcast());
}

// Create an answer with spec-compliant simulcast video stream.
// In this scenario, the SFU is the caller requesting that we send Simulcast.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateCompliantSimulcastAnswer) {
  MediaSessionOptions offer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &offer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &offer_opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);

  std::vector<RidDescription> rid_descriptions{
      RidDescription("f", RidDirection::kSend),
      RidDescription("h", RidDirection::kSend),
      RidDescription("q", RidDirection::kSend),
  };
  SimulcastLayerList simulcast_layers;
  simulcast_layers.AddLayer(SimulcastLayer(rid_descriptions[0].rid, false));
  simulcast_layers.AddLayer(SimulcastLayer(rid_descriptions[1].rid, true));
  simulcast_layers.AddLayer(SimulcastLayer(rid_descriptions[2].rid, false));
  AttachSenderToMediaDescriptionOptions(
      "video", webrtc::MediaType::VIDEO, kVideoTrack1, {kMediaStream1},
      rid_descriptions, simulcast_layers, 0, &answer_opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();

  CheckSimulcastInSessionDescription(answer.get(), "video", rid_descriptions,
                                     simulcast_layers);
}

// Create an answer that signals RIDs (not SSRCs) without Simulcast.
// In this scenario, RIDs do not need to be negotiated (there is only one).
// Note that RID Direction is not the same as the transceiver direction.
TEST_F(MediaSessionDescriptionFactoryTest, TestAnswerWithRidsNoSimulcast) {
  MediaSessionOptions offer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &offer_opts);
  RidDescription rid_offer("f", RidDirection::kSend);
  AttachSenderToMediaDescriptionOptions(
      "video", webrtc::MediaType::VIDEO, kVideoTrack1, {kMediaStream1},
      {rid_offer}, SimulcastLayerList(), 0, &offer_opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);

  RidDescription rid_answer("f", RidDirection::kReceive);
  AttachSenderToMediaDescriptionOptions(
      "video", webrtc::MediaType::VIDEO, kVideoTrack1, {kMediaStream1},
      {rid_answer}, SimulcastLayerList(), 0, &answer_opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();

  ASSERT_NE(answer.get(), nullptr);
  const ContentInfo* content = offer->GetContentByName("video");
  ASSERT_NE(content, nullptr);
  const MediaContentDescription* cd = content->media_description();
  ASSERT_NE(cd, nullptr);
  const StreamParamsVec& streams = cd->streams();
  ASSERT_THAT(streams, SizeIs(1));
  const StreamParams& stream = streams[0];
  ASSERT_THAT(stream.ssrcs, IsEmpty());
  EXPECT_FALSE(stream.has_rids());
  EXPECT_FALSE(cd->HasSimulcast());
}

// Create an audio and video answer to a standard video offer with:
// - one video track
// - two audio tracks
// - two data tracks
// and ensure it matches what we expect. Also updates the initial answer by
// adding a new video track and removes one of the audio tracks.
TEST_F(MediaSessionDescriptionFactoryTest, TestCreateMultiStreamVideoAnswer) {
  MediaSessionOptions offer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &offer_opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &offer_opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);
  AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                        kAudioTrack1, {kMediaStream1}, 1,
                                        &answer_opts);
  AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                        kAudioTrack2, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();

  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();

  EXPECT_EQ(webrtc::MediaType::AUDIO, acd->type());
  EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));

  const StreamParamsVec& audio_streams = acd->streams();
  ASSERT_EQ(2U, audio_streams.size());
  EXPECT_TRUE(audio_streams[0].cname == audio_streams[1].cname);
  EXPECT_EQ(kAudioTrack1, audio_streams[0].id);
  ASSERT_EQ(1U, audio_streams[0].ssrcs.size());
  EXPECT_NE(0U, audio_streams[0].ssrcs[0]);
  EXPECT_EQ(kAudioTrack2, audio_streams[1].id);
  ASSERT_EQ(1U, audio_streams[1].ssrcs.size());
  EXPECT_NE(0U, audio_streams[1].ssrcs[0]);

  EXPECT_EQ(kAutoBandwidth,
            acd->bandwidth());                  // default bandwidth (auto)
  EXPECT_TRUE(acd->rtcp_mux());                 // rtcp-mux defaults on

  EXPECT_EQ(webrtc::MediaType::VIDEO, vcd->type());
  EXPECT_THAT(vcd->codecs(), ElementsAreArray(kVideoCodecsAnswer));

  const StreamParamsVec& video_streams = vcd->streams();
  ASSERT_EQ(1U, video_streams.size());
  EXPECT_EQ(video_streams[0].cname, audio_streams[0].cname);
  EXPECT_EQ(kVideoTrack1, video_streams[0].id);
  EXPECT_EQ(kAutoBandwidth,
            vcd->bandwidth());                  // default bandwidth (auto)
  EXPECT_TRUE(vcd->rtcp_mux());                 // rtcp-mux defaults on

  // Update the answer. Add a new video track that is not synched to the
  // other tracks and remove 1 audio track.
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack2, {kMediaStream2}, 1,
                                        &answer_opts);
  DetachSenderFromMediaSection("audio", kAudioTrack2, &answer_opts);
  std::unique_ptr<SessionDescription> updated_answer(
      f2_.CreateAnswerOrError(offer.get(), answer_opts, answer.get())
          .MoveValue());

  ASSERT_TRUE(updated_answer.get());
  ac = updated_answer->GetContentByName("audio");
  vc = updated_answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  const MediaContentDescription* updated_acd = ac->media_description();
  const MediaContentDescription* updated_vcd = vc->media_description();

  EXPECT_EQ(acd->type(), updated_acd->type());
  EXPECT_EQ(acd->codecs(), updated_acd->codecs());
  EXPECT_EQ(vcd->type(), updated_vcd->type());
  EXPECT_EQ(vcd->codecs(), updated_vcd->codecs());

  const StreamParamsVec& updated_audio_streams = updated_acd->streams();
  ASSERT_EQ(1U, updated_audio_streams.size());
  EXPECT_TRUE(audio_streams[0] == updated_audio_streams[0]);

  const StreamParamsVec& updated_video_streams = updated_vcd->streams();
  ASSERT_EQ(2U, updated_video_streams.size());
  EXPECT_EQ(video_streams[0], updated_video_streams[0]);
  EXPECT_EQ(kVideoTrack2, updated_video_streams[1].id);
  // All media streams in one PeerConnection share one CNAME.
  EXPECT_EQ(updated_video_streams[1].cname, updated_video_streams[0].cname);
}

// Create an updated offer after creating an answer to the original offer and
// verify that the codecs that were part of the original answer are not changed
// in the updated offer.
TEST_F(MediaSessionDescriptionFactoryTest,
       RespondentCreatesOfferAfterCreatingAnswer) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  const AudioContentDescription* acd =
      GetFirstAudioContentDescription(answer.get());
  EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));

  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(answer.get());
  EXPECT_THAT(vcd->codecs(), ElementsAreArray(kVideoCodecsAnswer));

  std::unique_ptr<SessionDescription> updated_offer(
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue());

  // The expected audio codecs are the common audio codecs from the first
  // offer/answer exchange plus the audio codecs only `f2_` offer, sorted in
  // preference order.
  // TODO(wu): `updated_offer` should not include the codec
  // (i.e. `kAudioCodecs2[0]`) the other side doesn't support.
  const Codec kUpdatedAudioCodecOffer[] = {
      kAudioCodecsAnswer[0],
      kAudioCodecsAnswer[1],
      kAudioCodecs2[0],
  };

  // The expected video codecs are the common video codecs from the first
  // offer/answer exchange plus the video codecs only `f2_` offer, sorted in
  // preference order.
  const Codec kUpdatedVideoCodecOffer[] = {
      kVideoCodecsAnswer[0],
      kVideoCodecs2[1],
  };

  const AudioContentDescription* updated_acd =
      GetFirstAudioContentDescription(updated_offer.get());
  EXPECT_TRUE(CodecListsMatch(updated_acd->codecs(), kUpdatedAudioCodecOffer));

  const VideoContentDescription* updated_vcd =
      GetFirstVideoContentDescription(updated_offer.get());
  EXPECT_TRUE(CodecListsMatch(updated_vcd->codecs(), kUpdatedVideoCodecOffer));
}

// Test that a reoffer does not reuse audio codecs from a previous media section
// that is being recycled.
TEST_F(MediaSessionDescriptionFactoryTest,
       ReOfferDoesNotReUseRecycledAudioCodecs) {
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(CodecList{},
                                                            CodecList{});
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(CodecList{},
                                                            CodecList{});

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "a0",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Recycle the media section by changing its mid.
  opts.media_description_options[0].mid = "a1";
  std::unique_ptr<SessionDescription> reoffer =
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue();

  // Expect that the results of the first negotiation are ignored. If the m=
  // section was not recycled the payload types would match the initial offerer.
  const AudioContentDescription* acd =
      GetFirstAudioContentDescription(reoffer.get());
  // EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecs2)),
  // except that we don't want to check the PT numbers.
  EXPECT_EQ(acd->codecs().size(),
            sizeof(kAudioCodecs2) / sizeof(kAudioCodecs2[0]));
  for (size_t i = 0; i < acd->codecs().size(); ++i) {
    EXPECT_EQ(acd->codecs()[i].name, kAudioCodecs2[i].name);
  }
}

// Test that a reoffer does not reuse video codecs from a previous media section
// that is being recycled.
TEST_F(MediaSessionDescriptionFactoryTest,
       ReOfferDoesNotReUseRecycledVideoCodecs) {
  codec_lookup_helper_1_.GetCodecVendor()->set_audio_codecs(CodecList{},
                                                            CodecList{});
  codec_lookup_helper_2_.GetCodecVendor()->set_audio_codecs(CodecList{},
                                                            CodecList{});

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "v0",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  auto answer = f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Recycle the media section by changing its mid.
  opts.media_description_options[0].mid = "v1";
  std::unique_ptr<SessionDescription> reoffer =
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue();

  // Expect that the results of the first negotiation are ignored. If the m=
  // section was not recycled the payload types would match the initial offerer.
  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(reoffer.get());
  EXPECT_TRUE(CodecListsMatch(vcd->codecs(), kVideoCodecs2));
}

// Test that a reanswer does not reuse audio codecs from a previous media
// section that is being recycled.
TEST_F(MediaSessionDescriptionFactoryTest,
       ReAnswerDoesNotReUseRecycledAudioCodecs) {
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(CodecList{},
                                                            CodecList{});
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(CodecList{},
                                                            CodecList{});

  // Perform initial offer/answer in reverse (`f2_` as offerer) so that the
  // second offer/answer is forward (`f1_` as offerer).
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "a0",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f2_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f1_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Recycle the media section by changing its mid.
  opts.media_description_options[0].mid = "a1";
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, answer.get()).MoveValue();
  std::unique_ptr<SessionDescription> reanswer =
      f2_.CreateAnswerOrError(reoffer.get(), opts, offer.get()).MoveValue();

  // Expect that the results of the first negotiation are ignored. If the m=
  // section was not recycled the payload types would match the initial offerer.
  const AudioContentDescription* acd =
      GetFirstAudioContentDescription(reanswer.get());
  EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));
}

// Test that a reanswer does not reuse video codecs from a previous media
// section that is being recycled.
TEST_F(MediaSessionDescriptionFactoryTest,
       ReAnswerDoesNotReUseRecycledVideoCodecs) {
  codec_lookup_helper_1_.GetCodecVendor()->set_audio_codecs(CodecList{},
                                                            CodecList{});
  codec_lookup_helper_2_.GetCodecVendor()->set_audio_codecs(CodecList{},
                                                            CodecList{});

  // Perform initial offer/answer in reverse (`f2_` as offerer) so that the
  // second offer/answer is forward (`f1_` as offerer).
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "v0",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f2_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f1_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Recycle the media section by changing its mid.
  opts.media_description_options[0].mid = "v1";
  std::unique_ptr<SessionDescription> reoffer =
      f1_.CreateOfferOrError(opts, answer.get()).MoveValue();
  std::unique_ptr<SessionDescription> reanswer =
      f2_.CreateAnswerOrError(reoffer.get(), opts, offer.get()).MoveValue();

  // Expect that the results of the first negotiation are ignored. If the m=
  // section was not recycled the payload types would match the initial offerer.
  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(reanswer.get());
  EXPECT_THAT(vcd->codecs(), ElementsAreArray(kVideoCodecsAnswer));
}

// Create an updated offer after creating an answer to the original offer and
// verify that the codecs that were part of the original answer are not changed
// in the updated offer. In this test Rtx is enabled.
TEST_F(MediaSessionDescriptionFactoryTest,
       RespondentCreatesOfferAfterCreatingAnswerWithRtx) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::vector<Codec> f1_codecs = MAKE_VECTOR(kVideoCodecs1);
  // This creates rtx for H264 with the payload type `f1_` uses.
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector<Codec> f2_codecs = MAKE_VECTOR(kVideoCodecs2);
  // This creates rtx for H264 with the payload type `f2_` uses.
  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs2[0].id), &f2_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(answer.get());

  std::vector<Codec> expected_codecs = MAKE_VECTOR(kVideoCodecsAnswer);
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &expected_codecs);

  EXPECT_TRUE(CodecListsMatch(expected_codecs, vcd->codecs()));

  // Now, make sure we get same result (except for the order) if `f2_` creates
  // an updated offer even though the default payload types between `f1_` and
  // `f2_` are different.
  std::unique_ptr<SessionDescription> updated_offer(
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue());
  ASSERT_TRUE(updated_offer);
  std::unique_ptr<SessionDescription> updated_answer(
      f1_.CreateAnswerOrError(updated_offer.get(), opts, answer.get())
          .MoveValue());

  const VideoContentDescription* updated_vcd =
      GetFirstVideoContentDescription(updated_answer.get());

  EXPECT_TRUE(CodecListsMatch(expected_codecs, updated_vcd->codecs()));
}

// Regression test for:
// https://bugs.chromium.org/p/webrtc/issues/detail?id=8332
// Existing codecs should always appear before new codecs in re-offers. But
// under a specific set of circumstances, the existing RTX codec was ending up
// added to the end of the list.
TEST_F(MediaSessionDescriptionFactoryTest,
       RespondentCreatesOfferAfterCreatingAnswerWithRemappedRtxPayloadType) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  // We specifically choose different preferred payload types for VP8 to
  // trigger the issue.
  Codec vp8_offerer = CreateVideoCodec(100, "VP8");
  Codec vp8_offerer_rtx = CreateVideoRtxCodec(101, vp8_offerer.id);
  Codec vp8_answerer = CreateVideoCodec(110, "VP8");
  Codec vp8_answerer_rtx = CreateVideoRtxCodec(111, vp8_answerer.id);
  Codec vp9 = CreateVideoCodec(120, "VP9");
  Codec vp9_rtx = CreateVideoRtxCodec(121, vp9.id);

  std::vector<Codec> f1_codecs = {vp8_offerer, vp8_offerer_rtx};
  // We also specifically cause the answerer to prefer VP9, such that if it
  // *doesn't* honor the existing preferred codec (VP8) we'll notice.
  std::vector<Codec> f2_codecs = {vp9, vp9_rtx, vp8_answerer, vp8_answerer_rtx};

  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);
  std::vector<Codec> audio_codecs;
  codec_lookup_helper_1_.GetCodecVendor()->set_audio_codecs(audio_codecs,
                                                            audio_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_audio_codecs(audio_codecs,
                                                            audio_codecs);

  // Offer will be {VP8, RTX for VP8}. Answer will be the same.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  // Updated offer *should* be {VP8, RTX for VP8, VP9, RTX for VP9}.
  // But if the bug is triggered, RTX for VP8 ends up last.
  std::unique_ptr<SessionDescription> updated_offer(
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue());

  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(updated_offer.get());
  std::vector<Codec> codecs = vcd->codecs();
  ASSERT_EQ(4u, codecs.size());
  EXPECT_EQ(vp8_offerer, codecs[0]);
  EXPECT_EQ(vp8_offerer_rtx, codecs[1]);
  EXPECT_EQ(vp9, codecs[2]);
  EXPECT_EQ(vp9_rtx, codecs[3]);
}

// Create an updated offer that adds video after creating an audio only answer
// to the original offer. This test verifies that if a video codec and the RTX
// codec have the same default payload type as an audio codec that is already in
// use, the added codecs payload types are changed.
TEST_F(MediaSessionDescriptionFactoryTest,
       RespondentCreatesOfferWithVideoAndRtxAfterCreatingAudioAnswer) {
  std::vector<Codec> f1_codecs = MAKE_VECTOR(kVideoCodecs1);
  // This creates rtx for H264 with the payload type `f1_` uses.
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  const AudioContentDescription* acd =
      GetFirstAudioContentDescription(answer.get());
  EXPECT_THAT(acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));

  // Now - let `f2_` add video with RTX and let the payload type the RTX codec
  // reference  be the same as an audio codec that was negotiated in the
  // first offer/answer exchange.
  opts.media_description_options.clear();
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  std::vector<Codec> f2_codecs = MAKE_VECTOR(kVideoCodecs2);
  ASSERT_THAT(acd->codecs().size(), Gt(0));
  int used_pl_type = acd->codecs()[0].id;
  f2_codecs[0].id = used_pl_type;  // Set the payload type for H264.
  AddRtxCodec(CreateVideoRtxCodec(125, used_pl_type), &f2_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> updated_offer(
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue());
  ASSERT_TRUE(updated_offer);
  std::unique_ptr<SessionDescription> updated_answer(
      f1_.CreateAnswerOrError(updated_offer.get(), opts, answer.get())
          .MoveValue());

  const AudioContentDescription* updated_acd =
      GetFirstAudioContentDescription(answer.get());
  EXPECT_THAT(updated_acd->codecs(), ElementsAreArray(kAudioCodecsAnswer));

  const VideoContentDescription* updated_vcd =
      GetFirstVideoContentDescription(updated_answer.get());

  ASSERT_EQ("H264", updated_vcd->codecs()[0].name);
  ASSERT_EQ(kRtxCodecName, updated_vcd->codecs()[1].name);
  int new_h264_pl_type = updated_vcd->codecs()[0].id;
  EXPECT_NE(used_pl_type, new_h264_pl_type);
  Codec rtx = updated_vcd->codecs()[1];
  int pt_referenced_by_rtx =
      FromString<int>(rtx.params[kCodecParamAssociatedPayloadType]);
  EXPECT_EQ(new_h264_pl_type, pt_referenced_by_rtx);
}

// Create an updated offer with RTX after creating an answer to an offer
// without RTX, and with different default payload types.
// Verify that the added RTX codec references the correct payload type.
TEST_F(MediaSessionDescriptionFactoryTest,
       RespondentCreatesOfferWithRtxAfterCreatingAnswerWithoutRtx) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  std::vector<Codec> f2_codecs = MAKE_VECTOR(kVideoCodecs2);
  // This creates rtx for H264 with the payload type `f2_` uses.
  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs2[0].id), &f2_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(answer.get());

  std::vector<Codec> expected_codecs = MAKE_VECTOR(kVideoCodecsAnswer);
  EXPECT_EQ(expected_codecs, vcd->codecs());

  // Now, ensure that the RTX codec is created correctly when `f2_` creates an
  // updated offer, even though the default payload types are different from
  // those of `f1_`.
  std::unique_ptr<SessionDescription> updated_offer(
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue());
  ASSERT_TRUE(updated_offer);

  const VideoContentDescription* updated_vcd =
      GetFirstVideoContentDescription(updated_offer.get());

  // New offer should attempt to add H263, and RTX for H264.
  expected_codecs.push_back(kVideoCodecs2[1]);
  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs1[1].id), &expected_codecs);
  EXPECT_TRUE(CodecListsMatch(expected_codecs, updated_vcd->codecs()));
}

// Test that RTX is ignored when there is no associated payload type parameter.
TEST_F(MediaSessionDescriptionFactoryTest, RtxWithoutApt) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::vector<Codec> f1_codecs = MAKE_VECTOR(kVideoCodecs1);
  // This creates RTX without associated payload type parameter.
  AddRtxCodec(CreateVideoCodec(126, kRtxCodecName), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector<Codec> f2_codecs = MAKE_VECTOR(kVideoCodecs2);
  // This creates RTX for H264 with the payload type `f2_` uses.
  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs2[0].id), &f2_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  // kCodecParamAssociatedPayloadType will always be added to the offer when RTX
  // is selected. Manually remove kCodecParamAssociatedPayloadType so that it
  // is possible to test that that RTX is dropped when
  // kCodecParamAssociatedPayloadType is missing in the offer.
  MediaContentDescription* media_desc =
      offer->GetContentDescriptionByName(CN_VIDEO);
  ASSERT_TRUE(media_desc);
  std::vector<Codec> codecs = media_desc->codecs();
  for (Codec& codec : codecs) {
    if (absl::StartsWith(codec.name, kRtxCodecName)) {
      codec.params.clear();
    }
  }
  media_desc->set_codecs(codecs);

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  EXPECT_THAT(
      GetCodecNames(GetFirstVideoContentDescription(answer.get())->codecs()),
      Not(Contains(kRtxCodecName)));
}

// Test that RTX will be filtered out in the answer if its associated payload
// type doesn't match the local value.
TEST_F(MediaSessionDescriptionFactoryTest, FilterOutRtxIfAptDoesntMatch) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::vector<Codec> f1_codecs = MAKE_VECTOR(kVideoCodecs1);
  // This creates RTX for H264 in sender.
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector<Codec> f2_codecs = MAKE_VECTOR(kVideoCodecs2);
  // This creates RTX for H263 in receiver.
  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs2[1].id), &f2_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  // Associated payload type doesn't match, therefore, RTX codec is removed in
  // the answer.
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  EXPECT_THAT(
      GetCodecNames(GetFirstVideoContentDescription(answer.get())->codecs()),
      Not(Contains(kRtxCodecName)));
}

// Test that when multiple RTX codecs are offered, only the matched RTX codec
// is added in the answer, and the unsupported RTX codec is filtered out.
TEST_F(MediaSessionDescriptionFactoryTest,
       FilterOutUnsupportedRtxWhenCreatingAnswer) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::vector<Codec> f1_codecs = MAKE_VECTOR(kVideoCodecs1);
  // This creates RTX for H264-SVC in sender.
  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs1[0].id), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  // This creates RTX for H264 in sender.
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector<Codec> f2_codecs = MAKE_VECTOR(kVideoCodecs2);
  // This creates RTX for H264 in receiver.
  AddRtxCodec(CreateVideoRtxCodec(124, kVideoCodecs2[0].id), &f2_codecs);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f1_codecs);

  // H264-SVC codec is removed in the answer, therefore, associated RTX codec
  // for H264-SVC should also be removed.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(answer.get());
  std::vector<Codec> expected_codecs = MAKE_VECTOR(kVideoCodecsAnswer);
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &expected_codecs);

  EXPECT_TRUE(CodecListsMatch(expected_codecs, vcd->codecs()));
}

// Test that after one RTX codec has been negotiated, a new offer can attempt
// to add another.
TEST_F(MediaSessionDescriptionFactoryTest, AddSecondRtxInNewOffer) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  std::vector<Codec> f1_codecs = MAKE_VECTOR(kVideoCodecs1);
  // This creates RTX for H264 for the offerer.
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  const VideoContentDescription* vcd =
      GetFirstVideoContentDescription(offer.get());

  std::vector<Codec> expected_codecs = MAKE_VECTOR(kVideoCodecs1);
  AddRtxCodec(CreateVideoRtxCodec(126, kVideoCodecs1[1].id), &expected_codecs);
  EXPECT_TRUE(CodecListsMatch(expected_codecs, vcd->codecs()));

  // Now, attempt to add RTX for H264-SVC.
  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs1[0].id), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::unique_ptr<SessionDescription> updated_offer(
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue());
  ASSERT_TRUE(updated_offer);
  vcd = GetFirstVideoContentDescription(updated_offer.get());

  AddRtxCodec(CreateVideoRtxCodec(125, kVideoCodecs1[0].id), &expected_codecs);
  EXPECT_TRUE(CodecListsMatch(expected_codecs, vcd->codecs()));
}

// Test that when RTX is used in conjunction with simulcast, an RTX ssrc is
// generated for each simulcast ssrc and correctly grouped.
TEST_F(MediaSessionDescriptionFactoryTest, SimSsrcsGenerateMultipleRtxSsrcs) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  // Add simulcast streams.
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        "stream1", {"stream1label"}, 3, &opts);

  // Use a single real codec, and then add RTX for it.
  std::vector<Codec> f1_codecs;
  f1_codecs.push_back(CreateVideoCodec(97, "H264"));
  AddRtxCodec(CreateVideoRtxCodec(125, 97), &f1_codecs);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  // Ensure that the offer has an RTX ssrc for each regular ssrc, and that there
  // is a FID ssrc + grouping for each.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  MediaContentDescription* media_desc =
      offer->GetContentDescriptionByName(CN_VIDEO);
  ASSERT_TRUE(media_desc);
  const StreamParamsVec& streams = media_desc->streams();
  // Single stream.
  ASSERT_EQ(1u, streams.size());
  // Stream should have 6 ssrcs: 3 for video, 3 for RTX.
  EXPECT_EQ(6u, streams[0].ssrcs.size());
  // And should have a SIM group for the simulcast.
  EXPECT_TRUE(streams[0].has_ssrc_group("SIM"));
  // And a FID group for RTX.
  EXPECT_TRUE(streams[0].has_ssrc_group("FID"));
  std::vector<uint32_t> primary_ssrcs;
  streams[0].GetPrimarySsrcs(&primary_ssrcs);
  EXPECT_EQ(3u, primary_ssrcs.size());
  std::vector<uint32_t> fid_ssrcs;
  streams[0].GetFidSsrcs(primary_ssrcs, &fid_ssrcs);
  EXPECT_EQ(3u, fid_ssrcs.size());
}

// Test that, when the FlexFEC codec is added, a FlexFEC ssrc is created
// together with a FEC-FR grouping. Guarded by WebRTC-FlexFEC-03 trial.
TEST_F(MediaSessionDescriptionFactoryTest, GenerateFlexfecSsrc) {
  ScopedKeyValueConfig override_field_trials(field_trials,
                                             "WebRTC-FlexFEC-03/Enabled/");
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  // Add single stream.
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        "stream1", {"stream1label"}, 1, &opts);

  // Use a single real codec, and then add FlexFEC for it.
  std::vector<Codec> f1_codecs;
  f1_codecs.push_back(CreateVideoCodec(97, "H264"));
  f1_codecs.push_back(CreateVideoCodec(118, "flexfec-03"));
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  // Ensure that the offer has a single FlexFEC ssrc and that
  // there is no FEC-FR ssrc + grouping for each.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  MediaContentDescription* media_desc =
      offer->GetContentDescriptionByName(CN_VIDEO);
  ASSERT_TRUE(media_desc);
  const StreamParamsVec& streams = media_desc->streams();
  // Single stream.
  ASSERT_EQ(1u, streams.size());
  // Stream should have 2 ssrcs: 1 for video, 1 for FlexFEC.
  EXPECT_EQ(2u, streams[0].ssrcs.size());
  // And should have a FEC-FR group for FlexFEC.
  EXPECT_TRUE(streams[0].has_ssrc_group("FEC-FR"));
  std::vector<uint32_t> primary_ssrcs;
  streams[0].GetPrimarySsrcs(&primary_ssrcs);
  ASSERT_EQ(1u, primary_ssrcs.size());
  uint32_t flexfec_ssrc;
  EXPECT_TRUE(streams[0].GetFecFrSsrc(primary_ssrcs[0], &flexfec_ssrc));
  EXPECT_NE(flexfec_ssrc, 0u);
}

// Test that FlexFEC is disabled for simulcast.
// TODO(brandtr): Remove this test when we support simulcast, either through
// multiple FlexfecSenders, or through multistream protection.
TEST_F(MediaSessionDescriptionFactoryTest, SimSsrcsGenerateNoFlexfecSsrcs) {
  ScopedKeyValueConfig override_field_trials(field_trials,
                                             "WebRTC-FlexFEC-03/Enabled/");
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  // Add simulcast streams.
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        "stream1", {"stream1label"}, 3, &opts);

  // Use a single real codec, and then add FlexFEC for it.
  std::vector<Codec> f1_codecs;
  f1_codecs.push_back(CreateVideoCodec(97, "H264"));
  f1_codecs.push_back(CreateVideoCodec(118, "flexfec-03"));
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  // Ensure that the offer has no FlexFEC ssrcs for each regular ssrc, and that
  // there is no FEC-FR ssrc + grouping for each.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  MediaContentDescription* media_desc =
      offer->GetContentDescriptionByName(CN_VIDEO);
  ASSERT_TRUE(media_desc);
  const StreamParamsVec& streams = media_desc->streams();
  // Single stream.
  ASSERT_EQ(1u, streams.size());
  // Stream should have 3 ssrcs: 3 for video, 0 for FlexFEC.
  EXPECT_EQ(3u, streams[0].ssrcs.size());
  // And should have a SIM group for the simulcast.
  EXPECT_TRUE(streams[0].has_ssrc_group("SIM"));
  // And not a FEC-FR group for FlexFEC.
  EXPECT_FALSE(streams[0].has_ssrc_group("FEC-FR"));
  std::vector<uint32_t> primary_ssrcs;
  streams[0].GetPrimarySsrcs(&primary_ssrcs);
  EXPECT_EQ(3u, primary_ssrcs.size());
  for (uint32_t primary_ssrc : primary_ssrcs) {
    uint32_t flexfec_ssrc;
    EXPECT_FALSE(streams[0].GetFecFrSsrc(primary_ssrc, &flexfec_ssrc));
  }
}

// Create an updated offer after creating an answer to the original offer and
// verify that the RTP header extensions that were part of the original answer
// are not changed in the updated offer.
TEST_F(MediaSessionDescriptionFactoryTest,
       RespondentCreatesOfferAfterCreatingAnswerWithRtpExtensions) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtension1),
                                   MAKE_VECTOR(kVideoRtpExtension1), &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtension2),
                                   MAKE_VECTOR(kVideoRtpExtension2), &opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  EXPECT_THAT(
      GetFirstAudioContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtensionAnswer));
  EXPECT_THAT(
      GetFirstVideoContentDescription(answer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kVideoRtpExtensionAnswer));

  std::unique_ptr<SessionDescription> updated_offer(
      f2_.CreateOfferOrError(opts, answer.get()).MoveValue());

  // The expected RTP header extensions in the new offer are the resulting
  // extensions from the first offer/answer exchange plus the extensions only
  // `f2_` offer.
  // Since the default local extension id `f2_` uses has already been used by
  // `f1_` for another extensions, it is changed to 13.
  const RtpExtension kUpdatedAudioRtpExtensions[] = {
      kAudioRtpExtensionAnswer[0],
      RtpExtension(kAudioRtpExtension2[1].uri, 13),
      kAudioRtpExtension2[2],
  };

  // Since the default local extension id `f2_` uses has already been used by
  // `f1_` for another extensions, is is changed to 12.
  const RtpExtension kUpdatedVideoRtpExtensions[] = {
      kVideoRtpExtensionAnswer[0],
      RtpExtension(kVideoRtpExtension2[1].uri, 12),
      kVideoRtpExtension2[2],
  };

  const AudioContentDescription* updated_acd =
      GetFirstAudioContentDescription(updated_offer.get());
  EXPECT_THAT(updated_acd->rtp_header_extensions(),
              UnorderedElementsAreArray(kUpdatedAudioRtpExtensions));

  const VideoContentDescription* updated_vcd =
      GetFirstVideoContentDescription(updated_offer.get());
  EXPECT_THAT(updated_vcd->rtp_header_extensions(),
              UnorderedElementsAreArray(kUpdatedVideoRtpExtensions));
}

// Verify that if the same RTP extension URI is used for audio and video, the
// same ID is used. Also verify that the ID isn't changed when creating an
// updated offer (this was previously a bug).
TEST_F(MediaSessionDescriptionFactoryTest, RtpExtensionIdReused) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);

  SetAudioVideoRtpHeaderExtensions(MAKE_VECTOR(kAudioRtpExtension3),
                                   MAKE_VECTOR(kVideoRtpExtension3), &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  // Since the audio extensions used ID 3 for "both_audio_and_video", so should
  // the video extensions.
  const RtpExtension kExpectedVideoRtpExtension[] = {
      kVideoRtpExtension3[0],
      kAudioRtpExtension3[1],
  };

  EXPECT_THAT(
      GetFirstAudioContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kAudioRtpExtension3));
  EXPECT_THAT(
      GetFirstVideoContentDescription(offer.get())->rtp_header_extensions(),
      UnorderedElementsAreArray(kExpectedVideoRtpExtension));

  // Nothing should change when creating a new offer
  std::unique_ptr<SessionDescription> updated_offer(
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue());

  EXPECT_THAT(GetFirstAudioContentDescription(updated_offer.get())
                  ->rtp_header_extensions(),
              UnorderedElementsAreArray(kAudioRtpExtension3));
  EXPECT_THAT(GetFirstVideoContentDescription(updated_offer.get())
                  ->rtp_header_extensions(),
              UnorderedElementsAreArray(kExpectedVideoRtpExtension));
}

TEST(MediaSessionDescription, CopySessionDescription) {
  SessionDescription source;
  ContentGroup group(CN_AUDIO);
  source.AddGroup(group);
  std::unique_ptr<AudioContentDescription> acd =
      std::make_unique<AudioContentDescription>();
  acd->set_codecs(MAKE_VECTOR(kAudioCodecs1));
  acd->AddLegacyStream(1);
  source.AddContent(CN_AUDIO, MediaProtocolType::kRtp, acd->Clone());
  std::unique_ptr<VideoContentDescription> vcd =
      std::make_unique<VideoContentDescription>();
  vcd->set_codecs(MAKE_VECTOR(kVideoCodecs1));
  vcd->AddLegacyStream(2);
  source.AddContent(CN_VIDEO, MediaProtocolType::kRtp, vcd->Clone());

  std::unique_ptr<SessionDescription> copy = source.Clone();
  ASSERT_TRUE(copy.get());
  EXPECT_TRUE(copy->HasGroup(CN_AUDIO));
  const ContentInfo* ac = copy->GetContentByName("audio");
  const ContentInfo* vc = copy->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  EXPECT_EQ(MediaProtocolType::kRtp, ac->type);
  const MediaContentDescription* acd_copy = ac->media_description();
  EXPECT_EQ(acd->codecs(), acd_copy->codecs());
  EXPECT_EQ(1u, acd->first_ssrc());

  EXPECT_EQ(MediaProtocolType::kRtp, vc->type);
  const MediaContentDescription* vcd_copy = vc->media_description();
  EXPECT_EQ(vcd->codecs(), vcd_copy->codecs());
  EXPECT_EQ(2u, vcd->first_ssrc());
}

// The below TestTransportInfoXXX tests create different offers/answers, and
// ensure the TransportInfo in the SessionDescription matches what we expect.
TEST_F(MediaSessionDescriptionFactoryTest, TestTransportInfoOfferAudio) {
  MediaSessionOptions options;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &options);
  TestTransportInfo(true, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestTransportInfoOfferIceRenomination) {
  MediaSessionOptions options;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &options);
  options.media_description_options[0]
      .transport_options.enable_ice_renomination = true;
  TestTransportInfo(true, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest, TestTransportInfoOfferAudioCurrent) {
  MediaSessionOptions options;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &options);
  TestTransportInfo(true, options, true);
}

TEST_F(MediaSessionDescriptionFactoryTest, TestTransportInfoOfferMultimedia) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  TestTransportInfo(true, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestTransportInfoOfferMultimediaCurrent) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  TestTransportInfo(true, options, true);
}

TEST_F(MediaSessionDescriptionFactoryTest, TestTransportInfoOfferBundle) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  options.bundle_enabled = true;
  TestTransportInfo(true, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestTransportInfoOfferBundleCurrent) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  options.bundle_enabled = true;
  TestTransportInfo(true, options, true);
}

TEST_F(MediaSessionDescriptionFactoryTest, TestTransportInfoAnswerAudio) {
  MediaSessionOptions options;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &options);
  TestTransportInfo(false, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestTransportInfoAnswerIceRenomination) {
  MediaSessionOptions options;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &options);
  options.media_description_options[0]
      .transport_options.enable_ice_renomination = true;
  TestTransportInfo(false, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestTransportInfoAnswerAudioCurrent) {
  MediaSessionOptions options;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &options);
  TestTransportInfo(false, options, true);
}

TEST_F(MediaSessionDescriptionFactoryTest, TestTransportInfoAnswerMultimedia) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  TestTransportInfo(false, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestTransportInfoAnswerMultimediaCurrent) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  TestTransportInfo(false, options, true);
}

TEST_F(MediaSessionDescriptionFactoryTest, TestTransportInfoAnswerBundle) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  options.bundle_enabled = true;
  TestTransportInfo(false, options, false);
}

TEST_F(MediaSessionDescriptionFactoryTest,
       TestTransportInfoAnswerBundleCurrent) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  options.bundle_enabled = true;
  TestTransportInfo(false, options, true);
}

// Offers UDP/TLS/RTP/SAVPF and verifies the answer can be created and contains
// UDP/TLS/RTP/SAVPF.
TEST_F(MediaSessionDescriptionFactoryTest, TestOfferDtlsSavpfCreateAnswer) {
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(CreateAudioMediaSession(), nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* offer_content = offer->GetContentByName("audio");
  ASSERT_TRUE(offer_content);
  MediaContentDescription* offer_audio_desc =
      offer_content->media_description();
  offer_audio_desc->set_protocol(kMediaProtocolDtlsSavpf);

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), CreateAudioMediaSession(), nullptr)
          .MoveValue();
  ASSERT_TRUE(answer);

  const ContentInfo* answer_content = answer->GetContentByName("audio");
  ASSERT_TRUE(answer_content);
  ASSERT_FALSE(answer_content->rejected);

  const MediaContentDescription* answer_audio_desc =
      answer_content->media_description();
  EXPECT_EQ(kMediaProtocolDtlsSavpf, answer_audio_desc->protocol());
}

// Test that we accept a DTLS offer without SDES and create an appropriate
// answer.
TEST_F(MediaSessionDescriptionFactoryTest, TestCryptoOfferDtlsButNotSdes) {
  /* TODO(hta): Figure this one out.
  f1_.set_secure(SEC_DISABLED);
  f2_.set_secure(SEC_ENABLED);
  tdf1_.set_secure(SEC_ENABLED);
  tdf2_.set_secure(SEC_ENABLED);
  */
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);

  // Generate an offer with DTLS
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(options, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());

  const TransportDescription* audio_offer_trans_desc =
      offer->GetTransportDescriptionByName("audio");
  ASSERT_TRUE(audio_offer_trans_desc->identity_fingerprint.get());
  const TransportDescription* video_offer_trans_desc =
      offer->GetTransportDescriptionByName("video");
  ASSERT_TRUE(video_offer_trans_desc->identity_fingerprint.get());

  // Generate an answer with DTLS.
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), options, nullptr).MoveValue();
  ASSERT_TRUE(answer.get());

  const TransportDescription* audio_answer_trans_desc =
      answer->GetTransportDescriptionByName("audio");
  EXPECT_TRUE(audio_answer_trans_desc->identity_fingerprint.get());
  const TransportDescription* video_answer_trans_desc =
      answer->GetTransportDescriptionByName("video");
  EXPECT_TRUE(video_answer_trans_desc->identity_fingerprint.get());
}

// Verifies if vad_enabled option is set to false, CN codecs are not present in
// offer or answer.
TEST_F(MediaSessionDescriptionFactoryTest, TestVADEnableOption) {
  MediaSessionOptions options;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &options);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(options, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* audio_content = offer->GetContentByName("audio");
  EXPECT_FALSE(VerifyNoCNCodecs(audio_content));

  options.vad_enabled = false;
  offer = f1_.CreateOfferOrError(options, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  audio_content = offer->GetContentByName("audio");
  EXPECT_TRUE(VerifyNoCNCodecs(audio_content));
  std::unique_ptr<SessionDescription> answer =
      f1_.CreateAnswerOrError(offer.get(), options, nullptr).MoveValue();
  ASSERT_TRUE(answer.get());
  audio_content = answer->GetContentByName("audio");
  EXPECT_TRUE(VerifyNoCNCodecs(audio_content));
}

// Test that the generated MIDs match the existing offer.
TEST_F(MediaSessionDescriptionFactoryTest, TestMIDsMatchesExistingOffer) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio_modified",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video_modified",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::DATA, "data_modified",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  // Create offer.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  std::unique_ptr<SessionDescription> updated_offer(
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue());

  const ContentInfo* audio_content = GetFirstAudioContent(updated_offer.get());
  const ContentInfo* video_content = GetFirstVideoContent(updated_offer.get());
  const ContentInfo* data_content = GetFirstDataContent(updated_offer.get());
  ASSERT_TRUE(audio_content);
  ASSERT_TRUE(video_content);
  ASSERT_TRUE(data_content);
  EXPECT_EQ("audio_modified", audio_content->mid());
  EXPECT_EQ("video_modified", video_content->mid());
  EXPECT_EQ("data_modified", data_content->mid());
}

// The following tests verify that the unified plan SDP is supported.
// Test that we can create an offer with multiple media sections of same media
// type.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateOfferWithMultipleAVMediaSections) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio_1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("audio_1", webrtc::MediaType::AUDIO,
                                        kAudioTrack1, {kMediaStream1}, 1,
                                        &opts);

  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video_1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("video_1", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio_2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("audio_2", webrtc::MediaType::AUDIO,
                                        kAudioTrack2, {kMediaStream2}, 1,
                                        &opts);

  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video_2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("video_2", webrtc::MediaType::VIDEO,
                                        kVideoTrack2, {kMediaStream2}, 1,
                                        &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);

  ASSERT_EQ(4u, offer->contents().size());
  EXPECT_FALSE(offer->contents()[0].rejected);
  const MediaContentDescription* acd = offer->contents()[0].media_description();
  ASSERT_EQ(1u, acd->streams().size());
  EXPECT_EQ(kAudioTrack1, acd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, acd->direction());

  EXPECT_FALSE(offer->contents()[1].rejected);
  const MediaContentDescription* vcd = offer->contents()[1].media_description();
  ASSERT_EQ(1u, vcd->streams().size());
  EXPECT_EQ(kVideoTrack1, vcd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, vcd->direction());

  EXPECT_FALSE(offer->contents()[2].rejected);
  acd = offer->contents()[2].media_description();
  ASSERT_EQ(1u, acd->streams().size());
  EXPECT_EQ(kAudioTrack2, acd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, acd->direction());

  EXPECT_FALSE(offer->contents()[3].rejected);
  vcd = offer->contents()[3].media_description();
  ASSERT_EQ(1u, vcd->streams().size());
  EXPECT_EQ(kVideoTrack2, vcd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, vcd->direction());
}

// Test that we can create an answer with multiple media sections of same media
// type.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateAnswerWithMultipleAVMediaSections) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio_1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("audio_1", webrtc::MediaType::AUDIO,
                                        kAudioTrack1, {kMediaStream1}, 1,
                                        &opts);

  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video_1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("video_1", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio_2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("audio_2", webrtc::MediaType::AUDIO,
                                        kAudioTrack2, {kMediaStream2}, 1,
                                        &opts);

  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video_2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("video_2", webrtc::MediaType::VIDEO,
                                        kVideoTrack2, {kMediaStream2}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();

  ASSERT_EQ(4u, answer->contents().size());
  EXPECT_FALSE(answer->contents()[0].rejected);
  const MediaContentDescription* acd =
      answer->contents()[0].media_description();
  ASSERT_EQ(1u, acd->streams().size());
  EXPECT_EQ(kAudioTrack1, acd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, acd->direction());

  EXPECT_FALSE(answer->contents()[1].rejected);
  const MediaContentDescription* vcd =
      answer->contents()[1].media_description();
  ASSERT_EQ(1u, vcd->streams().size());
  EXPECT_EQ(kVideoTrack1, vcd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, vcd->direction());

  EXPECT_FALSE(answer->contents()[2].rejected);
  acd = answer->contents()[2].media_description();
  ASSERT_EQ(1u, acd->streams().size());
  EXPECT_EQ(kAudioTrack2, acd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, acd->direction());

  EXPECT_FALSE(answer->contents()[3].rejected);
  vcd = answer->contents()[3].media_description();
  ASSERT_EQ(1u, vcd->streams().size());
  EXPECT_EQ(kVideoTrack2, vcd->streams()[0].id);
  EXPECT_EQ(RtpTransceiverDirection::kSendRecv, vcd->direction());
}

// Test that the media section will be rejected in offer if the corresponding
// MediaDescriptionOptions is stopped by the offerer.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateOfferWithMediaSectionStoppedByOfferer) {
  // Create an offer with two audio sections and one of them is stopped.
  MediaSessionOptions offer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &offer_opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio2",
                             RtpTransceiverDirection::kInactive, kStopped,
                             &offer_opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(2u, offer->contents().size());
  EXPECT_FALSE(offer->contents()[0].rejected);
  EXPECT_TRUE(offer->contents()[1].rejected);
}

// Test that the media section will be rejected in answer if the corresponding
// MediaDescriptionOptions is stopped by the offerer.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateAnswerWithMediaSectionStoppedByOfferer) {
  // Create an offer with two audio sections and one of them is stopped.
  MediaSessionOptions offer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &offer_opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio2",
                             RtpTransceiverDirection::kInactive, kStopped,
                             &offer_opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(2u, offer->contents().size());
  EXPECT_FALSE(offer->contents()[0].rejected);
  EXPECT_TRUE(offer->contents()[1].rejected);

  // Create an answer based on the offer.
  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
  ASSERT_EQ(2u, answer->contents().size());
  EXPECT_FALSE(answer->contents()[0].rejected);
  EXPECT_TRUE(answer->contents()[1].rejected);
}

// Test that the media section will be rejected in answer if the corresponding
// MediaDescriptionOptions is stopped by the answerer.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateAnswerWithMediaSectionRejectedByAnswerer) {
  // Create an offer with two audio sections.
  MediaSessionOptions offer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &offer_opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &offer_opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(2u, offer->contents().size());
  ASSERT_FALSE(offer->contents()[0].rejected);
  ASSERT_FALSE(offer->contents()[1].rejected);

  // The answerer rejects one of the audio sections.
  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio2",
                             RtpTransceiverDirection::kInactive, kStopped,
                             &answer_opts);
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), answer_opts, nullptr).MoveValue();
  ASSERT_EQ(2u, answer->contents().size());
  EXPECT_FALSE(answer->contents()[0].rejected);
  EXPECT_TRUE(answer->contents()[1].rejected);

  // The TransportInfo of the rejected m= section is expected to be added in the
  // answer.
  EXPECT_EQ(offer->transport_infos().size(), answer->transport_infos().size());
}

// Test the generated media sections has the same order of the
// corresponding MediaDescriptionOptions.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateOfferRespectsMediaDescriptionOptionsOrder) {
  MediaSessionOptions opts;
  // This tests put video section first because normally audio comes first by
  // default.
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();

  ASSERT_TRUE(offer);
  ASSERT_EQ(2u, offer->contents().size());
  EXPECT_EQ("video", offer->contents()[0].mid());
  EXPECT_EQ("audio", offer->contents()[1].mid());
}

// Test that different media sections using the same codec have same payload
// type.
TEST_F(MediaSessionDescriptionFactoryTest,
       PayloadTypesSharedByMediaSectionsOfSameType) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  // Create an offer with two video sections using same codecs.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(2u, offer->contents().size());
  const MediaContentDescription* vcd1 =
      offer->contents()[0].media_description();
  const MediaContentDescription* vcd2 =
      offer->contents()[1].media_description();
  EXPECT_EQ(vcd1->codecs().size(), vcd2->codecs().size());
  ASSERT_EQ(2u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].name, vcd2->codecs()[0].name);
  EXPECT_EQ(vcd1->codecs()[0].id, vcd2->codecs()[0].id);
  EXPECT_EQ(vcd1->codecs()[1].name, vcd2->codecs()[1].name);
  EXPECT_EQ(vcd1->codecs()[1].id, vcd2->codecs()[1].id);

  // Create answer and negotiate the codecs.
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer);
  ASSERT_EQ(2u, answer->contents().size());
  vcd1 = answer->contents()[0].media_description();
  vcd2 = answer->contents()[1].media_description();
  EXPECT_EQ(vcd1->codecs().size(), vcd2->codecs().size());
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].name, vcd2->codecs()[0].name);
  EXPECT_EQ(vcd1->codecs()[0].id, vcd2->codecs()[0].id);
}

#ifdef RTC_ENABLE_H265
// Test verifying that negotiating codecs with the same tx-mode retains the
// tx-mode value.
TEST_F(MediaSessionDescriptionFactoryTest, H265TxModeIsEqualRetainIt) {
  std::vector f1_codecs = {CreateVideoCodec(96, "H265")};
  f1_codecs.back().tx_mode = "mrst";
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector f2_codecs = {CreateVideoCodec(96, "H265")};
  f2_codecs.back().tx_mode = "mrst";
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  // Create an offer with two video sections using same codecs.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(1u, offer->contents().size());
  const MediaContentDescription* vcd1 =
      offer->contents()[0].media_description();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].tx_mode, "mrst");

  // Create answer and negotiate the codecs.
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer);
  ASSERT_EQ(1u, answer->contents().size());
  vcd1 = answer->contents()[0].media_description();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].tx_mode, "mrst");
}

// Test verifying that negotiating codecs with different tx_mode removes
// the tx_mode value.
TEST_F(MediaSessionDescriptionFactoryTest, H265TxModeIsDifferentDropCodecs) {
  std::vector f1_codecs = {CreateVideoCodec(96, "H265")};
  f1_codecs.back().tx_mode = "mrst";
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector f2_codecs = {CreateVideoCodec(96, "H265")};
  f2_codecs.back().tx_mode = "mrmt";
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  // Create an offer with two video sections using same codecs.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(1u, offer->contents().size());
  const VideoContentDescription* vcd1 =
      offer->contents()[0].media_description()->as_video();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].tx_mode, "mrst");

  // Create answer and negotiate the codecs.
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer);
  ASSERT_EQ(1u, answer->contents().size());
  vcd1 = answer->contents()[0].media_description()->as_video();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].tx_mode, std::nullopt);
}
#endif

// Test verifying that negotiating codecs with the same packetization retains
// the packetization value.
TEST_F(MediaSessionDescriptionFactoryTest, PacketizationIsEqual) {
  std::vector f1_codecs = {CreateVideoCodec(96, "H264")};
  f1_codecs.back().packetization = "raw";
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector f2_codecs = {CreateVideoCodec(96, "H264")};
  f2_codecs.back().packetization = "raw";
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  // Create an offer with two video sections using same codecs.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(1u, offer->contents().size());
  const MediaContentDescription* vcd1 =
      offer->contents()[0].media_description();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].packetization, "raw");

  // Create answer and negotiate the codecs.
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer);
  ASSERT_EQ(1u, answer->contents().size());
  vcd1 = answer->contents()[0].media_description();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].packetization, "raw");
}

// Test verifying that negotiating codecs with different packetization removes
// the packetization value.
TEST_F(MediaSessionDescriptionFactoryTest, PacketizationIsDifferent) {
  std::vector f1_codecs = {CreateVideoCodec(96, "H264")};
  f1_codecs.back().packetization = "raw";
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(f1_codecs,
                                                            f1_codecs);

  std::vector f2_codecs = {CreateVideoCodec(96, "H264")};
  f2_codecs.back().packetization = "notraw";
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(f2_codecs,
                                                            f2_codecs);

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  // Create an offer with two video sections using same codecs.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(1u, offer->contents().size());
  const VideoContentDescription* vcd1 =
      offer->contents()[0].media_description()->as_video();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].packetization, "raw");

  // Create answer and negotiate the codecs.
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer);
  ASSERT_EQ(1u, answer->contents().size());
  vcd1 = answer->contents()[0].media_description()->as_video();
  ASSERT_EQ(1u, vcd1->codecs().size());
  EXPECT_EQ(vcd1->codecs()[0].packetization, std::nullopt);
}

// Test that the codec preference order per media section is respected in
// subsequent offer.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateOfferRespectsCodecPreferenceOrder) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  // Create an offer with two video sections using same codecs.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(2u, offer->contents().size());
  MediaContentDescription* vcd1 = offer->contents()[0].media_description();
  const MediaContentDescription* vcd2 =
      offer->contents()[1].media_description();
  auto video_codecs = MAKE_VECTOR(kVideoCodecs1);
  EXPECT_EQ(video_codecs, vcd1->codecs());
  EXPECT_EQ(video_codecs, vcd2->codecs());

  // Change the codec preference of the first video section and create a
  // follow-up offer.
  auto video_codecs_reverse = MAKE_VECTOR(kVideoCodecs1Reverse);
  vcd1->set_codecs(video_codecs_reverse);
  std::unique_ptr<SessionDescription> updated_offer(
      f1_.CreateOfferOrError(opts, offer.get()).MoveValue());
  vcd1 = updated_offer->contents()[0].media_description();
  vcd2 = updated_offer->contents()[1].media_description();
  // The video codec preference order should be respected.
  EXPECT_EQ(video_codecs_reverse, vcd1->codecs());
  EXPECT_EQ(video_codecs, vcd2->codecs());
}

// Test that the codec preference order per media section is respected in
// the answer.
TEST_F(MediaSessionDescriptionFactoryTest,
       CreateAnswerRespectsCodecPreferenceOrder) {
  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video1",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video2",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  // Create an offer with two video sections using same codecs.
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  ASSERT_EQ(2u, offer->contents().size());
  MediaContentDescription* vcd1 = offer->contents()[0].media_description();
  const MediaContentDescription* vcd2 =
      offer->contents()[1].media_description();
  auto video_codecs = MAKE_VECTOR(kVideoCodecs1);
  EXPECT_EQ(video_codecs, vcd1->codecs());
  EXPECT_EQ(video_codecs, vcd2->codecs());

  // Change the codec preference of the first video section and create an
  // answer.
  auto video_codecs_reverse = MAKE_VECTOR(kVideoCodecs1Reverse);
  vcd1->set_codecs(video_codecs_reverse);
  std::unique_ptr<SessionDescription> answer =
      f1_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  vcd1 = answer->contents()[0].media_description();
  vcd2 = answer->contents()[1].media_description();
  // The video codec preference order should be respected.
  EXPECT_EQ(video_codecs_reverse, vcd1->codecs());
  EXPECT_EQ(video_codecs, vcd2->codecs());
}

// Test that when creating an answer, the codecs use local parameters instead of
// the remote ones.
TEST_F(MediaSessionDescriptionFactoryTest, CreateAnswerWithLocalCodecParams) {
  const std::string audio_param_name = "audio_param";
  const std::string audio_value1 = "audio_v1";
  const std::string audio_value2 = "audio_v2";
  const std::string video_param_name = "video_param";
  const std::string video_value1 = "video_v1";
  const std::string video_value2 = "video_v2";

  auto audio_codecs1 = MAKE_VECTOR(kAudioCodecs1);
  auto audio_codecs2 = MAKE_VECTOR(kAudioCodecs1);
  auto video_codecs1 = MAKE_VECTOR(kVideoCodecs1);
  auto video_codecs2 = MAKE_VECTOR(kVideoCodecs1);

  // Set the parameters for codecs.
  audio_codecs1[0].SetParam(audio_param_name, audio_value1);
  video_codecs1[0].SetParam(video_param_name, video_value1);
  audio_codecs2[0].SetParam(audio_param_name, audio_value2);
  video_codecs2[0].SetParam(video_param_name, video_value2);

  codec_lookup_helper_1_.GetCodecVendor()->set_audio_codecs(audio_codecs1,
                                                            audio_codecs1);
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(video_codecs1,
                                                            video_codecs1);
  codec_lookup_helper_2_.GetCodecVendor()->set_audio_codecs(audio_codecs2,
                                                            audio_codecs2);
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(video_codecs2,
                                                            video_codecs2);

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);
  auto offer_acd = offer->contents()[0].media_description();
  auto offer_vcd = offer->contents()[1].media_description();
  std::string value;
  EXPECT_TRUE(offer_acd->codecs()[0].GetParam(audio_param_name, &value));
  EXPECT_EQ(audio_value1, value);
  EXPECT_TRUE(offer_vcd->codecs()[0].GetParam(video_param_name, &value));
  EXPECT_EQ(video_value1, value);

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer);
  auto answer_acd = answer->contents()[0].media_description();
  auto answer_vcd = answer->contents()[1].media_description();
  // Use the parameters from the local codecs.
  ASSERT_TRUE(answer_acd);
  ASSERT_THAT(answer_acd->codecs().size(), Gt(0));
  EXPECT_TRUE(answer_acd->codecs()[0].GetParam(audio_param_name, &value));
  EXPECT_EQ(audio_value2, value);
  EXPECT_TRUE(answer_vcd->codecs()[0].GetParam(video_param_name, &value));
  EXPECT_EQ(video_value2, value);
}

// Test that matching packetization-mode is part of the criteria for matching
// H264 codecs (in addition to profile-level-id). Previously, this was not the
// case, so the first H264 codec with the same profile-level-id would match and
// the payload type in the answer would be incorrect.
// This is a regression test for bugs.webrtc.org/8808
TEST_F(MediaSessionDescriptionFactoryTest,
       H264MatchCriteriaIncludesPacketizationMode) {
  // Create two H264 codecs with the same profile level ID and different
  // packetization modes.
  Codec h264_pm0 = CreateVideoCodec(96, "H264");
  h264_pm0.params[kH264FmtpProfileLevelId] = "42c01f";
  h264_pm0.params[kH264FmtpPacketizationMode] = "0";
  Codec h264_pm1 = CreateVideoCodec(97, "H264");
  h264_pm1.params[kH264FmtpProfileLevelId] = "42c01f";
  h264_pm1.params[kH264FmtpPacketizationMode] = "1";

  // Offerer will send both codecs, answerer should choose the one with matching
  // packetization mode (and not the first one it sees).
  codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(
      {h264_pm0, h264_pm1}, {h264_pm0, h264_pm1});
  codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs({h264_pm1},
                                                            {h264_pm1});

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer);

  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  ASSERT_TRUE(answer);

  // Answer should have one negotiated codec with packetization-mode=1 using the
  // offered payload type.
  ASSERT_EQ(1u, answer->contents().size());
  auto answer_vcd = answer->contents()[0].media_description();
  ASSERT_EQ(1u, answer_vcd->codecs().size());
  auto answer_codec = answer_vcd->codecs()[0];
  EXPECT_EQ(h264_pm1.id, answer_codec.id);
}

class MediaProtocolTest : public testing::TestWithParam<const char*> {
 public:
  MediaProtocolTest()
      : tdf1_(field_trials_),
        tdf2_(field_trials_),
        codec_lookup_helper_1_(field_trials_),
        codec_lookup_helper_2_(field_trials_),
        f1_(nullptr, false, &ssrc_generator1, &tdf1_, &codec_lookup_helper_1_),
        f2_(nullptr, false, &ssrc_generator2, &tdf2_, &codec_lookup_helper_2_) {
    codec_lookup_helper_1_.GetCodecVendor()->set_audio_codecs(
        MAKE_VECTOR(kAudioCodecs1), MAKE_VECTOR(kAudioCodecs1));
    codec_lookup_helper_1_.GetCodecVendor()->set_video_codecs(
        MAKE_VECTOR(kVideoCodecs1), MAKE_VECTOR(kVideoCodecs1));
    codec_lookup_helper_2_.GetCodecVendor()->set_audio_codecs(
        MAKE_VECTOR(kAudioCodecs2), MAKE_VECTOR(kAudioCodecs2));
    codec_lookup_helper_2_.GetCodecVendor()->set_video_codecs(
        MAKE_VECTOR(kVideoCodecs2), MAKE_VECTOR(kVideoCodecs2));
    tdf1_.set_certificate(RTCCertificate::Create(
        std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("id1"))));
    tdf2_.set_certificate(RTCCertificate::Create(
        std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("id2"))));
  }

 protected:
  ScopedKeyValueConfig field_trials_;
  TransportDescriptionFactory tdf1_;
  TransportDescriptionFactory tdf2_;
  CodecLookupHelperForTesting codec_lookup_helper_1_;
  CodecLookupHelperForTesting codec_lookup_helper_2_;
  MediaSessionDescriptionFactory f1_;
  MediaSessionDescriptionFactory f2_;
  UniqueRandomIdGenerator ssrc_generator1;
  UniqueRandomIdGenerator ssrc_generator2;
};

TEST_P(MediaProtocolTest, TestAudioVideoAcceptance) {
  MediaSessionOptions opts;
  AddAudioVideoSections(RtpTransceiverDirection::kRecvOnly, &opts);
  std::unique_ptr<SessionDescription> offer =
      f1_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  // Set the protocol for all the contents.
  for (auto& content : offer->contents()) {
    content.media_description()->set_protocol(GetParam());
  }
  std::unique_ptr<SessionDescription> answer =
      f2_.CreateAnswerOrError(offer.get(), opts, nullptr).MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");
  const ContentInfo* vc = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  ASSERT_TRUE(vc);
  EXPECT_FALSE(ac->rejected);  // the offer is accepted
  EXPECT_FALSE(vc->rejected);
  const MediaContentDescription* acd = ac->media_description();
  const MediaContentDescription* vcd = vc->media_description();
  EXPECT_EQ(GetParam(), acd->protocol());
  EXPECT_EQ(GetParam(), vcd->protocol());
}

INSTANTIATE_TEST_SUITE_P(MediaProtocolPatternTest,
                         MediaProtocolTest,
                         ValuesIn(kMediaProtocols));
INSTANTIATE_TEST_SUITE_P(MediaProtocolDtlsPatternTest,
                         MediaProtocolTest,
                         ValuesIn(kMediaProtocolsDtls));

// Compare the two vectors of codecs ignoring the payload type.
bool CodecsMatch(const std::vector<Codec>& codecs1,
                 const std::vector<Codec>& codecs2) {
  if (codecs1.size() != codecs2.size()) {
    return false;
  }

  for (size_t i = 0; i < codecs1.size(); ++i) {
    if (!codecs1[i].Matches(codecs2[i])) {
      return false;
    }
  }
  return true;
}

void TestAudioCodecsOffer(RtpTransceiverDirection direction) {
  ScopedKeyValueConfig field_trials;
  TransportDescriptionFactory tdf(field_trials);
  tdf.set_certificate(RTCCertificate::Create(
      std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("id"))));

  UniqueRandomIdGenerator ssrc_generator;
  CodecLookupHelperForTesting codec_lookup_helper(field_trials);
  MediaSessionDescriptionFactory sf(nullptr, false, &ssrc_generator, &tdf,
                                    &codec_lookup_helper);
  const std::vector<Codec> send_codecs = MAKE_VECTOR(kAudioCodecs1);
  const std::vector<Codec> recv_codecs = MAKE_VECTOR(kAudioCodecs2);
  const std::vector<Codec> sendrecv_codecs = MAKE_VECTOR(kAudioCodecsAnswer);
  codec_lookup_helper.GetCodecVendor()->set_audio_codecs(send_codecs,
                                                         recv_codecs);

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio", direction,
                             kActive, &opts);

  if (direction == RtpTransceiverDirection::kSendRecv ||
      direction == RtpTransceiverDirection::kSendOnly) {
    AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                          kAudioTrack1, {kMediaStream1}, 1,
                                          &opts);
  }

  std::unique_ptr<SessionDescription> offer =
      sf.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  ContentInfo* ac = offer->GetContentByName("audio");

  // If the factory didn't add any audio content to the offer, we cannot check
  // that the codecs put in are right. This happens when we neither want to
  // send nor receive audio. The checks are still in place if at some point
  // we'd instead create an inactive stream.
  if (ac) {
    MediaContentDescription* acd = ac->media_description();
    // sendrecv and inactive should both present lists as if the channel was
    // to be used for sending and receiving. Inactive essentially means it
    // might eventually be used anything, but we don't know more at this
    // moment.
    if (acd->direction() == RtpTransceiverDirection::kSendOnly) {
      EXPECT_TRUE(CodecsMatch(send_codecs, acd->codecs()));
    } else if (acd->direction() == RtpTransceiverDirection::kRecvOnly) {
      EXPECT_TRUE(CodecsMatch(recv_codecs, acd->codecs()));
    } else {
      EXPECT_TRUE(CodecsMatch(sendrecv_codecs, acd->codecs()));
    }
  }
}

// Since the PT suggester reserves the static range for specific codecs,
// PT numbers from the 36-63 range are used.
const Codec kOfferAnswerCodecs[] = {CreateAudioCodec(40, "codec0", 16000, 1),
                                    CreateAudioCodec(41, "codec1", 8000, 1),
                                    CreateAudioCodec(42, "codec2", 8000, 1),
                                    CreateAudioCodec(43, "codec3", 8000, 1),
                                    CreateAudioCodec(44, "codec4", 8000, 2),
                                    CreateAudioCodec(45, "codec5", 32000, 1),
                                    CreateAudioCodec(46, "codec6", 48000, 1)};

/* The codecs groups below are chosen as per the matrix below. The objective
 * is to have different sets of codecs in the inputs, to get unique sets of
 * codecs after negotiation, depending on offer and answer communication
 * directions. One-way directions in the offer should either result in the
 * opposite direction in the answer, or an inactive answer. Regardless, the
 * choice of codecs should be as if the answer contained the opposite
 * direction. Inactive offers should be treated as sendrecv/sendrecv.
 *
 *         |     Offer   |      Answer  |         Result
 *    codec|send recv sr | send recv sr | s/r  r/s sr/s sr/r sr/sr
 *     0   | x    -    - |  -    x    - |  x    -    -    -    -
 *     1   | x    x    x |  -    x    - |  x    -    -    x    -
 *     2   | -    x    - |  x    -    - |  -    x    -    -    -
 *     3   | x    x    x |  x    -    - |  -    x    x    -    -
 *     4   | -    x    - |  x    x    x |  -    x    -    -    -
 *     5   | x    -    - |  x    x    x |  x    -    -    -    -
 *     6   | x    x    x |  x    x    x |  x    x    x    x    x
 */
// Codecs used by offerer in the AudioCodecsAnswerTest
const int kOfferSendCodecs[] = {0, 1, 3, 5, 6};
const int kOfferRecvCodecs[] = {1, 2, 3, 4, 6};
// Codecs used in the answerer in the AudioCodecsAnswerTest.  The order is
// jumbled to catch the answer not following the order in the offer.
const int kAnswerSendCodecs[] = {6, 5, 2, 3, 4};
const int kAnswerRecvCodecs[] = {6, 5, 4, 1, 0};
// The resulting sets of codecs in the answer in the AudioCodecsAnswerTest
const int kResultSend_RecvCodecs[] = {0, 1, 5, 6};
const int kResultRecv_SendCodecs[] = {2, 3, 4, 6};
const int kResultSendrecv_SendCodecs[] = {3, 6};
const int kResultSendrecv_RecvCodecs[] = {1, 6};
const int kResultSendrecv_SendrecvCodecs[] = {6};

template <typename T, int IDXS>
std::vector<T> VectorFromIndices(const T* array, const int (&indices)[IDXS]) {
  std::vector<T> out;
  out.reserve(IDXS);
  for (int idx : indices)
    out.push_back(array[idx]);

  return out;
}

void TestAudioCodecsAnswer(RtpTransceiverDirection offer_direction,
                           RtpTransceiverDirection answer_direction,
                           bool add_legacy_stream) {
  ScopedKeyValueConfig field_trials;
  TransportDescriptionFactory offer_tdf(field_trials);
  TransportDescriptionFactory answer_tdf(field_trials);
  offer_tdf.set_certificate(RTCCertificate::Create(
      std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("offer_id"))));
  answer_tdf.set_certificate(RTCCertificate::Create(
      std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("answer_id"))));
  UniqueRandomIdGenerator ssrc_generator1, ssrc_generator2;
  CodecLookupHelperForTesting offer_codec_lookup_helper(field_trials);
  MediaSessionDescriptionFactory offer_factory(
      nullptr, false, &ssrc_generator1, &offer_tdf, &offer_codec_lookup_helper);
  CodecLookupHelperForTesting answer_codec_lookup_helper(field_trials);
  MediaSessionDescriptionFactory answer_factory(nullptr, false,
                                                &ssrc_generator2, &answer_tdf,
                                                &answer_codec_lookup_helper);

  offer_codec_lookup_helper.GetCodecVendor()->set_audio_codecs(
      VectorFromIndices(kOfferAnswerCodecs, kOfferSendCodecs),
      VectorFromIndices(kOfferAnswerCodecs, kOfferRecvCodecs));
  answer_codec_lookup_helper.GetCodecVendor()->set_audio_codecs(
      VectorFromIndices(kOfferAnswerCodecs, kAnswerSendCodecs),
      VectorFromIndices(kOfferAnswerCodecs, kAnswerRecvCodecs));

  MediaSessionOptions offer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio", offer_direction,
                             kActive, &offer_opts);

  if (webrtc::RtpTransceiverDirectionHasSend(offer_direction)) {
    AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                          kAudioTrack1, {kMediaStream1}, 1,
                                          &offer_opts);
  }

  std::unique_ptr<SessionDescription> offer =
      offer_factory.CreateOfferOrError(offer_opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::AUDIO, "audio",
                             answer_direction, kActive, &answer_opts);

  if (webrtc::RtpTransceiverDirectionHasSend(answer_direction)) {
    AttachSenderToMediaDescriptionOptions("audio", webrtc::MediaType::AUDIO,
                                          kAudioTrack1, {kMediaStream1}, 1,
                                          &answer_opts);
  }
  std::unique_ptr<SessionDescription> answer =
      answer_factory.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  const ContentInfo* ac = answer->GetContentByName("audio");

  // If the factory didn't add any audio content to the answer, we cannot
  // check that the codecs put in are right. This happens when we neither want
  // to send nor receive audio. The checks are still in place if at some point
  // we'd instead create an inactive stream.
  if (ac) {
    ASSERT_EQ(webrtc::MediaType::AUDIO, ac->media_description()->type());
    const MediaContentDescription* acd = ac->media_description();

    std::vector<Codec> target_codecs;
    // For offers with sendrecv or inactive, we should never reply with more
    // codecs than offered, with these codec sets.
    switch (offer_direction) {
      case RtpTransceiverDirection::kInactive:
        target_codecs = VectorFromIndices(kOfferAnswerCodecs,
                                          kResultSendrecv_SendrecvCodecs);
        break;
      case RtpTransceiverDirection::kSendOnly:
        target_codecs =
            VectorFromIndices(kOfferAnswerCodecs, kResultSend_RecvCodecs);
        break;
      case RtpTransceiverDirection::kRecvOnly:
        target_codecs =
            VectorFromIndices(kOfferAnswerCodecs, kResultRecv_SendCodecs);
        break;
      case RtpTransceiverDirection::kSendRecv:
        if (acd->direction() == RtpTransceiverDirection::kSendOnly) {
          target_codecs =
              VectorFromIndices(kOfferAnswerCodecs, kResultSendrecv_SendCodecs);
        } else if (acd->direction() == RtpTransceiverDirection::kRecvOnly) {
          target_codecs =
              VectorFromIndices(kOfferAnswerCodecs, kResultSendrecv_RecvCodecs);
        } else {
          target_codecs = VectorFromIndices(kOfferAnswerCodecs,
                                            kResultSendrecv_SendrecvCodecs);
        }
        break;
      case RtpTransceiverDirection::kStopped:
        // This does not happen in any current test.
        RTC_DCHECK_NOTREACHED();
    }

    auto format_codecs = [](const std::vector<Codec>& codecs) {
      StringBuilder os;
      bool first = true;
      os << "{";
      for (const auto& c : codecs) {
        os << (first ? " " : ", ") << c.id << ":" << c.name;
        first = false;
      }
      os << " }";
      return os.Release();
    };

    EXPECT_TRUE(acd->codecs() == target_codecs)
        << "Expected: " << format_codecs(target_codecs)
        << ", got: " << format_codecs(acd->codecs()) << "; Offered: "
        << webrtc::RtpTransceiverDirectionToString(offer_direction)
        << ", answerer wants: "
        << webrtc::RtpTransceiverDirectionToString(answer_direction)
        << "; got: "
        << webrtc::RtpTransceiverDirectionToString(acd->direction());
  } else {
    EXPECT_EQ(offer_direction, RtpTransceiverDirection::kInactive)
        << "Only inactive offers are allowed to not generate any audio "
           "content";
  }
}

using AudioCodecsOfferTest = testing::TestWithParam<RtpTransceiverDirection>;

TEST_P(AudioCodecsOfferTest, TestCodecsInOffer) {
  TestAudioCodecsOffer(GetParam());
}

INSTANTIATE_TEST_SUITE_P(MediaSessionDescriptionFactoryTest,
                         AudioCodecsOfferTest,
                         Values(RtpTransceiverDirection::kSendOnly,
                                RtpTransceiverDirection::kRecvOnly,
                                RtpTransceiverDirection::kSendRecv,
                                RtpTransceiverDirection::kInactive));

using AudioCodecsAnswerTest = testing::TestWithParam<
    std::tuple<RtpTransceiverDirection, RtpTransceiverDirection, bool>>;

TEST_P(AudioCodecsAnswerTest, TestCodecsInAnswer) {
  TestAudioCodecsAnswer(std::get<0>(GetParam()), std::get<1>(GetParam()),
                        std::get<2>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(MediaSessionDescriptionFactoryTest,
                         AudioCodecsAnswerTest,
                         Combine(Values(RtpTransceiverDirection::kSendOnly,
                                        RtpTransceiverDirection::kRecvOnly,
                                        RtpTransceiverDirection::kSendRecv,
                                        RtpTransceiverDirection::kInactive),
                                 Values(RtpTransceiverDirection::kSendOnly,
                                        RtpTransceiverDirection::kRecvOnly,
                                        RtpTransceiverDirection::kSendRecv,
                                        RtpTransceiverDirection::kInactive),
                                 Bool()));

#ifdef RTC_ENABLE_H265
class VideoCodecsOfferH265LevelIdTest : public testing::Test {
 public:
  VideoCodecsOfferH265LevelIdTest()
      : tdf_offerer_(field_trials_),
        tdf_answerer_(field_trials_),
        sf_offerer_(nullptr,
                    false,
                    &ssrc_generator_offerer_,
                    &tdf_offerer_,
                    &codec_lookup_helper_offerer_),
        sf_answerer_(nullptr,
                     false,
                     &ssrc_generator_answerer_,
                     &tdf_answerer_,
                     &codec_lookup_helper_answerer_),
        codec_lookup_helper_offerer_(field_trials_),
        codec_lookup_helper_answerer_(field_trials_) {
    tdf_offerer_.set_certificate(RTCCertificate::Create(
        std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("offer_id"))));
    tdf_answerer_.set_certificate(RTCCertificate::Create(
        std::unique_ptr<SSLIdentity>(new FakeSSLIdentity("answer_id"))));
  }

  void CheckH265Level(const std::vector<Codec>& codecs,
                      const std::string& expected_level) {
    for (const auto& codec : codecs) {
      if (codec.name == "H265") {
        auto it = codec.params.find("level-id");
        ASSERT_TRUE(it != codec.params.end());
        EXPECT_EQ(it->second, expected_level);
      }
    }
  }

 protected:
  ScopedKeyValueConfig field_trials_;
  TransportDescriptionFactory tdf_offerer_;
  TransportDescriptionFactory tdf_answerer_;
  UniqueRandomIdGenerator ssrc_generator_offerer_;
  UniqueRandomIdGenerator ssrc_generator_answerer_;
  MediaSessionDescriptionFactory sf_offerer_;
  MediaSessionDescriptionFactory sf_answerer_;
  CodecLookupHelperForTesting codec_lookup_helper_offerer_;
  CodecLookupHelperForTesting codec_lookup_helper_answerer_;
};

// Both sides support H.265 level 5.2 for encoding and decoding.
// Offer: level 5.2, SendRecv
// Answer: level 5.2, SendRecv
TEST_F(VideoCodecsOfferH265LevelIdTest, TestSendRecvSymmetrical) {
  const std::vector<Codec> send_codecs = MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> recv_codecs = MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(send_codecs,
                                                                  recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(recv_codecs,
                                                                   send_codecs);
  EXPECT_EQ(sendrecv_codecs, codec_lookup_helper_offerer_.GetCodecVendor()
                                 ->video_sendrecv_codecs()
                                 .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level52LevelId);
}

// Both sides support H.265 level 6.0 for encoding and decoding.
// Offer: level 6.0, SendOnly
// Answer: level 6.0, RecvOnly
TEST_F(VideoCodecsOfferH265LevelIdTest, TestSendOnlySymmetrical) {
  const std::vector<Codec> send_codecs = MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> recv_codecs = MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(send_codecs,
                                                                  recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(recv_codecs,
                                                                   send_codecs);
  EXPECT_EQ(sendrecv_codecs, codec_lookup_helper_offerer_.GetCodecVendor()
                                 ->video_sendrecv_codecs()
                                 .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level6LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level6LevelId);
}

// Both sides support H.265 level 5.2 for encoding and decoding.
// Offer: level 5.2, RecvOnly
// Answer: level 5.2, SendOnly
TEST_F(VideoCodecsOfferH265LevelIdTest, TestRecvOnlySymmetrical) {
  const std::vector<Codec> send_codecs = MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> recv_codecs = MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(send_codecs,
                                                                  recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(recv_codecs,
                                                                   send_codecs);
  EXPECT_EQ(sendrecv_codecs, codec_lookup_helper_offerer_.GetCodecVendor()
                                 ->video_sendrecv_codecs()
                                 .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level52LevelId);
}

// Offerer encodes up to level 5.2, and decodes up to level 6.0.
// Answerer encodes up to level 6.0, and decodes up to level 5.2.
// Offer: level 5.2, SendRecv
// Answer: level 5.2, SendRecv
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendRecvOffererEncode52Decode60AnswererEncode60Decode52) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level52LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 5.2, and decodes up to level 6.0.
// Offer: level 5.2, SendRecv
// Answer: level 5.2, SendRecv
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendRecvOffererEncode60Decode52AnswererEncode52Decode60) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level52LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 3.1, and decodes up to level 5.0.
// Offer: level 5.2, SendRecv
// Answer: level 3.1, SendRecv
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendRecvOffererEncode60Decode52AnswererEncode31Decode50) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level31);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level5);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level31), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level31LevelId);

  std::unique_ptr<SessionDescription> reoffer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(reoffer.get());
  const ContentInfo* reoffer_oc = reoffer->GetContentByName("video");
  ASSERT_TRUE(reoffer_oc);
  const MediaContentDescription* reoffer_ocd = reoffer_oc->media_description();
  EXPECT_TRUE(
      CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), reoffer_ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 4, and decodes up to level 6.
// Offer: level 5.2, SendRecv
// Answer: level 4, SendRecv
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendRecvOffererEncode60Decode52AnswererEncode40Decode60) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level4LevelId);
}

// Offerer encodes up to level 4, and decodes up to level 6.
// Answerer encodes up to level 6, and decodes up to level 5.2.
// Offer: level 4, SendRecv
// Answer: level 4, SendRecv
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendRecvOffererEncode40Decode60AnswererEncode60Decode52) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level4LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level4LevelId);
}

// Offerer encodes up to level 5.2, and decodes up to level 6.
// Answerer encodes up to level 6, and decodes up to level 5.2.
// Offer: level 6, RecvOnly
// Answer: level 6, SendOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       RecvOnlyOffererEncode52Decode60AnswererEncode60Decode52) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level6LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level6LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 5.2, and decodes up to level 6.
// Offer: level 5.2, RecvOnly
// Answer: level 5.2, SendOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       RecvOnlyOffererEncode60Decode52AnswererEncode52Decode60) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level52LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 3.1, and decodes up to level 5.
// Offer: level 5.2, RecvOnly
// Answer: level 3.1, SendOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       RecvOnlyOffererEncode60Decode52AnswererEncode31Decode50) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level31);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level5);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level31), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level31LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 4, and decodes up to level 6.
// Offer: level 5.2, RecvOnly
// Answer: level 4, SendOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       RecvOnlyOffererEncode60Decode52AnswererEncode40Decode60) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level4LevelId);
}

// Offerer encodes up to level 4, and decodes up to level 6.
// Answerer encodes up to level 6, and decodes up to level 5.2.
// Offer: level 6, RecvOnly
// Answer: level 6, SendOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       RecvOnlyOffererEncode40Decode60AnswererEncode60Decode52) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level6LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &answer_opts);
  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level6LevelId);
}

// Offerer encodes up to level 5.2, and decodes up to level 6.
// Answerer encodes up to level 6, and decodes up to level 5.2.
// Offer: level 5.2, SendOnly
// Answer: level 5.2, RecvOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendOnlyOffererEncode52Decode60AnswererEncode60Decode52) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level52LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level52), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level52LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 5.2, and decodes up to level 6.
// Offer: level 6, SendOnly
// Answer: level 6, RecvOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendOnlyOffererEncode60Decode52AnswererEncode52Decode60) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level6LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level6LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 3.1, and decodes up to level 5.
// Offer: level 6, SendOnly
// Answer: level 5, RecvOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendOnlyOffererEncode60Decode52AnswererEncode31Decode50) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level31);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level5);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level6LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level5), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level5LevelId);
}

// Offerer encodes up to level 6, and decodes up to level 5.2.
// Answerer encodes up to level 4, and decodes up to level 6.
// Offer: level 6, SendOnly
// Answer: level 6, RecvOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendOnlyOffererEncode60Decode52AnswererEncode40Decode60) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level6LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level6), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level6LevelId);
}

// Offerer encodes up to level 4, and decodes up to level 6.
// Answerer encodes up to level 6, and decodes up to level 5.2.
// Offer: level 4, SendOnly
// Answer: level 4, RecvOnly
TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendOnlyOffererEncode40Decode60AnswererEncode60Decode52) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendOnly, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level4LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level4LevelId);
}

TEST_F(VideoCodecsOfferH265LevelIdTest,
       SendOnlyOffererEncode40Decode60AnswererEncode60Decode52WithPreference) {
  const std::vector<Codec> offerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> offerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> offerer_sendrecv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level4);
  const std::vector<Codec> answerer_send_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level6);
  const std::vector<Codec> answerer_recv_codecs =
      MAKE_VECTOR(kVideoCodecsH265Level52);
  codec_lookup_helper_offerer_.GetCodecVendor()->set_video_codecs(
      offerer_send_codecs, offerer_recv_codecs);
  codec_lookup_helper_answerer_.GetCodecVendor()->set_video_codecs(
      answerer_send_codecs, answerer_recv_codecs);
  EXPECT_EQ(offerer_sendrecv_codecs,
            codec_lookup_helper_offerer_.GetCodecVendor()
                ->video_sendrecv_codecs()
                .codecs());

  MediaSessionOptions opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kSendRecv, kActive,
                             &opts);

  AttachSenderToMediaDescriptionOptions("video", webrtc::MediaType::VIDEO,
                                        kVideoTrack1, {kMediaStream1}, 1,
                                        &opts);
  std::vector<RtpCodecCapability> preferences;
  for (const auto& codec :
       codec_lookup_helper_offerer_.GetCodecVendor()->video_recv_codecs()) {
    preferences.push_back(webrtc::ToRtpCodecCapability(codec));
  }
  opts.media_description_options[0].codec_preferences = preferences;

  std::unique_ptr<SessionDescription> offer =
      sf_offerer_.CreateOfferOrError(opts, nullptr).MoveValue();
  ASSERT_TRUE(offer.get());
  const ContentInfo* oc = offer->GetContentByName("video");
  ASSERT_TRUE(oc);
  const MediaContentDescription* ocd = oc->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), ocd->codecs()));
  CheckH265Level(ocd->codecs(), kVideoCodecsH265Level4LevelId);

  MediaSessionOptions answer_opts;
  AddMediaDescriptionOptions(webrtc::MediaType::VIDEO, "video",
                             RtpTransceiverDirection::kRecvOnly, kActive,
                             &answer_opts);

  std::unique_ptr<SessionDescription> answer =
      sf_answerer_.CreateAnswerOrError(offer.get(), answer_opts, nullptr)
          .MoveValue();
  ASSERT_TRUE(answer.get());
  const ContentInfo* ac = answer->GetContentByName("video");
  ASSERT_TRUE(ac);
  const MediaContentDescription* acd = ac->media_description();
  EXPECT_TRUE(CodecsMatch(MAKE_VECTOR(kVideoCodecsH265Level4), acd->codecs()));
  CheckH265Level(acd->codecs(), kVideoCodecsH265Level4LevelId);
}

#endif

}  // namespace
}  // namespace webrtc
