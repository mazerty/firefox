/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(__has_feature) || !__has_feature(objc_arc)
#  error "This file requires ARC support."
#endif

#include "device_info_objc.h"
#include "rtc_video_capture_objc.h"
#include "rtc_base/ref_counted_object.h"
#include "api/scoped_refptr.h"
#include "video_capture_avfoundation.h"
#include "mozilla/StaticPrefs_media.h"

using namespace mozilla;
using namespace webrtc;
using namespace videocapturemodule;

webrtc::scoped_refptr<VideoCaptureModule> VideoCaptureImpl::Create(
    const char* deviceUniqueIdUTF8) {
  if (StaticPrefs::media_getusermedia_camera_macavf_enabled_AtStartup()) {
    return VideoCaptureAvFoundation::Create(deviceUniqueIdUTF8);
  }
  return VideoCaptureIos::Create(deviceUniqueIdUTF8);
}

VideoCaptureIos::VideoCaptureIos() : is_capturing_(false) {
  capability_.width = kDefaultWidth;
  capability_.height = kDefaultHeight;
  capability_.maxFPS = kDefaultFrameRate;
  capture_device_ = nil;
}

VideoCaptureIos::~VideoCaptureIos() {
  if (is_capturing_) {
    [capture_device_ stopCapture];
    capture_device_ = nil;
  }
}

webrtc::scoped_refptr<VideoCaptureModule> VideoCaptureIos::Create(
    const char* deviceUniqueIdUTF8) {
  if (!deviceUniqueIdUTF8[0]) {
    return NULL;
  }

  webrtc::scoped_refptr<VideoCaptureIos> capture_module(
      new webrtc::RefCountedObject<VideoCaptureIos>());

  const int32_t name_length = strlen(deviceUniqueIdUTF8);
  if (name_length >= kVideoCaptureUniqueNameLength) return nullptr;

  RTC_DCHECK_RUN_ON(&capture_module->api_checker_);
  capture_module->_deviceUniqueId = new char[name_length + 1];
  strncpy(capture_module->_deviceUniqueId, deviceUniqueIdUTF8, name_length + 1);
  capture_module->_deviceUniqueId[name_length] = '\0';

  capture_module->capture_device_ =
      [[RTCVideoCaptureIosObjC alloc] initWithOwner:capture_module.get()];
  if (!capture_module->capture_device_) {
    return nullptr;
  }

  if (![capture_module->capture_device_
          setCaptureDeviceByUniqueId:
              [[NSString alloc] initWithCString:deviceUniqueIdUTF8
                                       encoding:NSUTF8StringEncoding]]) {
    return nullptr;
  }
  return capture_module;
}

int32_t VideoCaptureIos::StartCapture(
    const VideoCaptureCapability& capability) {
  capability_ = capability;

  if (![capture_device_ startCaptureWithCapability:capability]) {
    return -1;
  }

  is_capturing_ = true;

  return 0;
}

int32_t VideoCaptureIos::StopCapture() {
  if (![capture_device_ stopCapture]) {
    return -1;
  }

  is_capturing_ = false;
  return 0;
}

bool VideoCaptureIos::CaptureStarted() { return is_capturing_; }

int32_t VideoCaptureIos::CaptureSettings(VideoCaptureCapability& settings) {
  settings = capability_;
  settings.videoType = VideoType::kNV12;
  return 0;
}
