/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "modules/desktop_capture/linux/x11/x_error_trap.h"

#include <stddef.h>

#include <limits>

#include "rtc_base/checks.h"

namespace webrtc {

Bool XErrorTrap::XServerErrorHandler(Display* display, xReply* rep,
                                     char* /* buf */, int /* len */,
                                     XPointer data) {
  XErrorTrap* self = reinterpret_cast<XErrorTrap*>(data);
  if (rep->generic.type != X_Error ||
      // Overflow-safe last_request_read <= last_ignored_request_ for skipping
      // async replies from requests before XErrorTrap was created.
      self->last_ignored_request_ - display->last_request_read <
          std::numeric_limits<unsigned long>::max() >> 1)
    return False;
  self->last_xserver_error_code_ = rep->error.errorCode;
  return True;
}

XErrorTrap::XErrorTrap(Display* display)
    : display_(display), last_xserver_error_code_(0), enabled_(true) {
  // Use async_handlers instead of XSetErrorHandler().  async_handlers can
  // remain in place and then be safely removed at the right time even if a
  // handler change happens concurrently on another thread.  async_handlers
  // are processed first and so can prevent errors reaching the global
  // XSetErrorHandler handler.  They also will not see errors from or affect
  // handling of errors on other Displays, which may be processed on other
  // threads.
  LockDisplay(display);
  async_handler_.next = display->async_handlers;
  async_handler_.handler = XServerErrorHandler;
  async_handler_.data = reinterpret_cast<XPointer>(this);
  display->async_handlers = &async_handler_;
  last_ignored_request_ = display->request;
  UnlockDisplay(display);
}

int XErrorTrap::GetLastErrorAndDisable() {
  assert(enabled_);
  enabled_ = false;
  LockDisplay(display_);
  DeqAsyncHandler(display_, &async_handler_);
  UnlockDisplay(display_);
  return last_xserver_error_code_;
}

XErrorTrap::~XErrorTrap() {
  if (enabled_) GetLastErrorAndDisable();
}

}  // namespace webrtc
