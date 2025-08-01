/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AppleDecoderModule_h
#define mozilla_AppleDecoderModule_h

#include "MediaCodecsSupport.h"
#include "PlatformDecoderModule.h"

namespace mozilla {

class AppleDecoderModule : public PlatformDecoderModule {
  template <typename T, typename... Args>
  friend already_AddRefed<T> MakeAndAddRef(Args&&...);

 public:
  const char* Name() const override { return "Apple"; }
  static already_AddRefed<PlatformDecoderModule> Create();

  nsresult Startup() override;

  // Decode thread.
  already_AddRefed<MediaDataDecoder> CreateVideoDecoder(
      const CreateDecoderParams& aParams) override;

  // Decode thread.
  already_AddRefed<MediaDataDecoder> CreateAudioDecoder(
      const CreateDecoderParams& aParams) override;

  media::DecodeSupportSet SupportsMimeType(
      const nsACString& aMimeType,
      DecoderDoctorDiagnostics* aDiagnostics) const override;

  media::DecodeSupportSet Supports(
      const SupportDecoderParams& aParams,
      DecoderDoctorDiagnostics* aDiagnostics) const override;

  static void Init();

  static constexpr int kCMVideoCodecType_H264{'avc1'};
  static constexpr int kCMVideoCodecType_VP9{'vp09'};

 private:
  AppleDecoderModule() = default;
  virtual ~AppleDecoderModule() = default;

  static inline bool sInitialized = false;
  static inline EnumeratedArray<media::MediaCodec, bool,
                                size_t(media::MediaCodec::SENTINEL)>
      sCanUseHWDecoder;

  bool IsVideoSupported(const VideoInfo& aConfig,
                        const CreateDecoderParams::OptionSet& aOptions =
                            CreateDecoderParams::OptionSet()) const;
  // Register a HW decoder if it's not registered by default.
  static bool RegisterSupplementalDecoder(const media::MediaCodec& aCodec);
  // Return true if a dummy hardware decoder could be created.
  static bool CanCreateHWDecoder(const media::MediaCodec& aCodec);
};

}  // namespace mozilla

#endif  // mozilla_AppleDecoderModule_h
