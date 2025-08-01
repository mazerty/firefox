/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsWaylandDisplay.h"
#include "DMABufFormats.h"

#include "base/message_loop.h"    // for MessageLoop
#include "base/task.h"            // for NewRunnableMethod, etc
#include "mozilla/gfx/Logging.h"  // for gfxCriticalNote
#include "mozilla/StaticMutex.h"
#include "mozilla/Array.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/StaticPrefs_general.h"
#include "mozilla/Sprintf.h"
#include "WidgetUtilsGtk.h"
#include "mozilla/widget/xx-pip-v1-client-protocol.h"
#include "nsGtkKeyUtils.h"
#include "nsLayoutUtils.h"
#include "nsWindow.h"
#include "wayland-proxy.h"

#undef LOG
#undef LOG_VERBOSE
#ifdef MOZ_LOGGING
#  include "mozilla/Logging.h"
#  include "nsTArray.h"
#  include "Units.h"
extern mozilla::LazyLogModule gWidgetWaylandLog;
#  define LOG(...) \
    MOZ_LOG(gWidgetWaylandLog, mozilla::LogLevel::Debug, (__VA_ARGS__))
#  define LOG_VERBOSE(...) \
    MOZ_LOG(gWidgetWaylandLog, mozilla::LogLevel::Verbose, (__VA_ARGS__))
#else
#  define LOG(...)
#  define LOG_VERBOSE(...)
#endif /* MOZ_LOGGING */

namespace mozilla::widget {

static nsWaylandDisplay* gWaylandDisplay;

void WaylandDisplayRelease() {
  MOZ_RELEASE_ASSERT(NS_IsMainThread(),
                     "WaylandDisplay can be released in main thread only!");
  if (!gWaylandDisplay) {
    return;
  }
  LOG("WaylandDisplayRelease()");
  delete gWaylandDisplay;
  gWaylandDisplay = nullptr;
}

wl_display* WaylandDisplayGetWLDisplay() {
  GdkDisplay* disp = gdk_display_get_default();
  if (!GdkIsWaylandDisplay(disp)) {
    return nullptr;
  }
  return gdk_wayland_display_get_wl_display(disp);
}

nsWaylandDisplay* WaylandDisplayGet() {
  if (!gWaylandDisplay) {
    MOZ_RELEASE_ASSERT(NS_IsMainThread(),
                       "WaylandDisplay can be created in main thread only!");
    wl_display* waylandDisplay = WaylandDisplayGetWLDisplay();
    if (!waylandDisplay) {
      return nullptr;
    }
    // We're setting Wayland client buffer size here (i.e. our write buffer).
    // Server buffer size is set by compositor and we may use the same buffer
    // sizes on both sides. Mutter uses 1024 * 1024 (1M) so let's use the same
    // value.
    wl_display_set_max_buffer_size(waylandDisplay, 1024 * 1024);
    gWaylandDisplay = new nsWaylandDisplay(waylandDisplay);
  }
  return gWaylandDisplay;
}

void nsWaylandDisplay::SetShm(wl_shm* aShm) { mShm = aShm; }

class WaylandPointerEvent {
 public:
  constexpr WaylandPointerEvent() = default;

  RefPtr<nsWindow> TakeWindow(wl_surface* aSurface) {
    if (!aSurface) {
      mWindow = nullptr;
    } else {
      GdkWindow* window =
          static_cast<GdkWindow*>(wl_surface_get_user_data(aSurface));
      mWindow = window ? static_cast<nsWindow*>(
                             g_object_get_data(G_OBJECT(window), "nsWindow"))
                       : nullptr;
    }
    return mWindow;
  }
  already_AddRefed<nsWindow> GetAndClearWindow() { return mWindow.forget(); }
  RefPtr<nsWindow> GetWindow() { return mWindow; }

  void SetSource(int32_t aSource) { mSource = aSource; }

  void SetDelta120(uint32_t aAxis, int32_t aDelta) {
    switch (aAxis) {
      case WL_POINTER_AXIS_VERTICAL_SCROLL:
        mDeltaY = aDelta / 120.0f;
        break;
      case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        mDeltaX = aDelta / 120.0f;
        break;
      default:
        NS_WARNING("WaylandPointerEvent::SetDelta120(): wrong axis!");
        break;
    }
  }

  void SetTime(uint32_t aTime) { mTime = aTime; }

  void SendScrollEvent() {
    if (!mWindow || !nsLayoutUtils::IsSmoothScrollingEnabled()) {
      return;
    }

    // nsWindow::OnSmoothScrollEvent() may spin event loop so
    // mWindow/mSource/delta may be replaced.
    int32_t source = mSource;
    float deltaX = mDeltaX;
    float deltaY = mDeltaY;

    mSource = -1;
    mDeltaX = mDeltaY = 0.0f;

    // We process wheel events only now.
    if (source != WL_POINTER_AXIS_SOURCE_WHEEL) {
      return;
    }

    RefPtr<nsWindow> win = mWindow;
    uint32_t eventTime = mTime;
    win->OnSmoothScrollEvent(eventTime, deltaX, deltaY);
  }

  void Clear() { mWindow = nullptr; }

 private:
  StaticRefPtr<nsWindow> mWindow;
  uint32_t mTime = 0;
  int32_t mSource = 0;
  float mDeltaX = 0;
  float mDeltaY = 0;
};

static WaylandPointerEvent sHoldGesture;

static void gesture_hold_begin(void* data,
                               struct zwp_pointer_gesture_hold_v1* hold,
                               uint32_t serial, uint32_t time,
                               struct wl_surface* surface, uint32_t fingers) {
  RefPtr<nsWindow> window = sHoldGesture.TakeWindow(surface);
  if (!window) {
    return;
  }
  window->OnTouchpadHoldEvent(GDK_TOUCHPAD_GESTURE_PHASE_BEGIN, time, fingers);
}

static void gesture_hold_end(void* data,
                             struct zwp_pointer_gesture_hold_v1* hold,
                             uint32_t serial, uint32_t time,
                             int32_t cancelled) {
  RefPtr<nsWindow> window = sHoldGesture.GetAndClearWindow();
  if (!window) {
    return;
  }
  window->OnTouchpadHoldEvent(cancelled ? GDK_TOUCHPAD_GESTURE_PHASE_CANCEL
                                        : GDK_TOUCHPAD_GESTURE_PHASE_END,
                              time, 0);
}

static const struct zwp_pointer_gesture_hold_v1_listener gesture_hold_listener =
    {gesture_hold_begin, gesture_hold_end};

static WaylandPointerEvent sScrollEvent;

uint32_t gLastSerial = 0;
uint32_t nsWaylandDisplay::GetLastEventSerial() { return gLastSerial; }

static void pointer_handle_enter(void* data, struct wl_pointer* pointer,
                                 uint32_t serial, struct wl_surface* surface,
                                 wl_fixed_t sx, wl_fixed_t sy) {
  sScrollEvent.TakeWindow(surface);
}

static void pointer_handle_leave(void* data, struct wl_pointer* pointer,
                                 uint32_t serial, struct wl_surface* surface) {
  sScrollEvent.Clear();
}

static void pointer_handle_motion(void* data, struct wl_pointer* pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
}

static void pointer_handle_button(void* data, struct wl_pointer* pointer,
                                  uint32_t serial, uint32_t time,
                                  uint32_t button, uint32_t state) {
  gLastSerial = serial;
}

static void pointer_handle_axis(void* data, struct wl_pointer* pointer,
                                uint32_t time, uint32_t axis,
                                wl_fixed_t value) {
  sScrollEvent.SetTime(time);
}

static void pointer_handle_frame(void* data, struct wl_pointer* pointer) {
  sScrollEvent.SendScrollEvent();
}

static void pointer_handle_axis_source(
    void* data, struct wl_pointer* pointer,
    /*enum wl_pointer_axis_source */ uint32_t source) {
  sScrollEvent.SetSource(source);
}

static void pointer_handle_axis_stop(void* data, struct wl_pointer* pointer,
                                     uint32_t time, uint32_t axis) {}

static void pointer_handle_axis_discrete(void* data, struct wl_pointer* pointer,
                                         uint32_t axis, int32_t value) {}

static void pointer_handle_axis_value120(void* data, struct wl_pointer* pointer,
                                         uint32_t axis, int32_t value) {
  sScrollEvent.SetDelta120(axis, value);
}

/*
 * Example of scroll events we get for various devices. Note that
 * even three different devices has the same wl_pointer.
 *
 * Standard mouse wheel:
 *
 *  pointer_handle_axis_source pointer 0x7fd14fd4bac0 source 0
 *  pointer_handle_axis_value120 pointer 0x7fd14fd4bac0 value 120
 *  pointer_handle_axis pointer 0x7fd14fd4bac0 time 9470441 value 10.000000
 *  pointer_handle_frame
 *
 * Hi-res mouse wheel:
 *
 * pointer_handle_axis_source pointer 0x7fd14fd4bac0 source 0
 * pointer_handle_axis_value120 pointer 0x7fd14fd4bac0 value -24
 * pointer_handle_axis pointer 0x7fd14fd4bac0 time 9593205 value -1.992188
 * pointer_handle_frame
 *
 * Touchpad:
 *
 * pointer_handle_axis_source pointer 0x7fd14fd4bac0 source 1
 * pointer_handle_axis pointer 0x7fd14fd4bac0 time 9431830 value 0.312500
 * pointer_handle_axis pointer 0x7fd14fd4bac0 time 9431830 value -1.015625
 * pointer_handle_frame
 */

static const struct moz_wl_pointer_listener pointer_listener = {
    pointer_handle_enter,         pointer_handle_leave,
    pointer_handle_motion,        pointer_handle_button,
    pointer_handle_axis,          pointer_handle_frame,
    pointer_handle_axis_source,   pointer_handle_axis_stop,
    pointer_handle_axis_discrete, pointer_handle_axis_value120,
};

void nsWaylandDisplay::SetPointer(wl_pointer* aPointer) {
  // Don't even try on such old interface
  if (wl_proxy_get_version((struct wl_proxy*)aPointer) <
      WL_POINTER_RELEASE_SINCE_VERSION) {
    return;
  }

  MOZ_DIAGNOSTIC_ASSERT(!mPointer);
  mPointer = aPointer;

  // We're interested in pointer_handle_axis_value120() only for now.
  if (wl_proxy_get_version((struct wl_proxy*)aPointer) >=
      WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
    wl_pointer_add_listener(
        mPointer, (const wl_pointer_listener*)&pointer_listener, this);
  }

  // mPointerGestures is set by zwp_pointer_gestures_v1 if we have it.
  if (mPointerGestures) {
    mPointerGestureHold =
        zwp_pointer_gestures_v1_get_hold_gesture(mPointerGestures, mPointer);
    zwp_pointer_gesture_hold_v1_set_user_data(mPointerGestureHold, this);
    zwp_pointer_gesture_hold_v1_add_listener(mPointerGestureHold,
                                             &gesture_hold_listener, this);
  }
}

void nsWaylandDisplay::RemovePointer() {
  wl_pointer_release(mPointer);
  mPointer = nullptr;
}

static void seat_handle_capabilities(void* data, struct wl_seat* seat,
                                     unsigned int caps) {
  auto* display = static_cast<nsWaylandDisplay*>(data);
  if (!display) {
    return;
  }

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !display->GetPointer()) {
    display->SetPointer(wl_seat_get_pointer(seat));
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && display->GetPointer()) {
    display->RemovePointer();
  }

  wl_keyboard* keyboard = display->GetKeyboard();
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
    display->SetKeyboard(wl_seat_get_keyboard(seat));
  } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
    display->ClearKeyboard();
  }
}

static void seat_handle_name(void* data, struct wl_seat* seat,
                             const char* name) {
  /* We don't care about the name. */
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

void nsWaylandDisplay::SetSeat(wl_seat* aSeat, int aSeatId) {
  mSeat = aSeat;
  mSeatId = aSeatId;
  wl_seat_add_listener(aSeat, &seat_listener, this);
}

void nsWaylandDisplay::RemoveSeat(int aSeatId) {
  if (mSeatId == aSeatId) {
    mSeat = nullptr;
    mSeatId = -1;
  }
}

/* This keymap routine is derived from weston-2.0.0/clients/simple-im.c
 */
static void keyboard_handle_keymap(void* data, struct wl_keyboard* wl_keyboard,
                                   uint32_t format, int fd, uint32_t size) {
  KeymapWrapper::HandleKeymap(format, fd, size);
}

static void keyboard_handle_enter(void* data, struct wl_keyboard* keyboard,
                                  uint32_t serial, struct wl_surface* surface,
                                  struct wl_array* keys) {
  KeymapWrapper::SetFocusIn(surface, serial);
}

static void keyboard_handle_leave(void* data, struct wl_keyboard* keyboard,
                                  uint32_t serial, struct wl_surface* surface) {
  KeymapWrapper::SetFocusOut(surface);
}

static void keyboard_handle_key(void* data, struct wl_keyboard* keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state) {
  gLastSerial = serial;
  // hardware key code is +8.
  // https://gitlab.gnome.org/GNOME/gtk/-/blob/3.24.41/gdk/wayland/gdkdevice-wayland.c#L2341
  KeymapWrapper::KeyboardHandlerForWayland(serial, key + 8, state);
}

static void keyboard_handle_modifiers(void* data, struct wl_keyboard* keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched,
                                      uint32_t mods_locked, uint32_t group) {}
static void keyboard_handle_repeat_info(void* data,
                                        struct wl_keyboard* keyboard,
                                        int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,    keyboard_handle_enter,
    keyboard_handle_leave,     keyboard_handle_key,
    keyboard_handle_modifiers, keyboard_handle_repeat_info};

void nsWaylandDisplay::SetKeyboard(wl_keyboard* aKeyboard) {
  MOZ_ASSERT(aKeyboard);
  MOZ_DIAGNOSTIC_ASSERT(!mKeyboard);
  mKeyboard = aKeyboard;
  wl_keyboard_add_listener(mKeyboard, &keyboard_listener, nullptr);
}

void nsWaylandDisplay::ClearKeyboard() {
  if (mKeyboard) {
    wl_keyboard_destroy(mKeyboard);
    mKeyboard = nullptr;
    KeymapWrapper::ClearKeymap();
  }
}

void nsWaylandDisplay::SetCompositor(wl_compositor* aCompositor) {
  mCompositor = aCompositor;
}

void nsWaylandDisplay::SetSubcompositor(wl_subcompositor* aSubcompositor) {
  mSubcompositor = aSubcompositor;
}

void nsWaylandDisplay::SetIdleInhibitManager(
    zwp_idle_inhibit_manager_v1* aIdleInhibitManager) {
  mIdleInhibitManager = aIdleInhibitManager;
}

void nsWaylandDisplay::SetViewporter(wp_viewporter* aViewporter) {
  mViewporter = aViewporter;
}

void nsWaylandDisplay::SetRelativePointerManager(
    zwp_relative_pointer_manager_v1* aRelativePointerManager) {
  mRelativePointerManager = aRelativePointerManager;
}

void nsWaylandDisplay::SetPointerConstraints(
    zwp_pointer_constraints_v1* aPointerConstraints) {
  mPointerConstraints = aPointerConstraints;
}

void nsWaylandDisplay::SetPointerGestures(
    zwp_pointer_gestures_v1* aPointerGestures) {
  mPointerGestures = aPointerGestures;
}

void nsWaylandDisplay::SetDmabuf(zwp_linux_dmabuf_v1* aDmabuf, int aVersion) {
  if (!aDmabuf || aVersion < ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
    return;
  }
  mDmabuf = aDmabuf;
  mDmabufIsFeedback =
      (aVersion >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION);
  if (mDmabufIsFeedback) {
    mFormats->InitFeedback(mDmabuf, nullptr);
  } else {
    mFormats->InitV3(mDmabuf);
  }
}

void nsWaylandDisplay::EnsureDMABufFormats() {
  if (mDmabuf && !mDmabufIsFeedback) {
    mFormats->InitV3Done();
  }
  mFormats->EnsureBasicFormats();
}

void nsWaylandDisplay::SetXdgActivation(xdg_activation_v1* aXdgActivation) {
  mXdgActivation = aXdgActivation;
}

void nsWaylandDisplay::SetAppMenuManager(
    org_kde_kwin_appmenu_manager* aAppMenuManager) {
  mAppMenuManager = aAppMenuManager;
}

void nsWaylandDisplay::SetCMSupportedFeature(uint32_t aFeature) {
  LOG("nsWaylandDisplay::SetCMSupportedFeature() [%d]", aFeature);
  switch (aFeature) {
    case WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4:
      mColorManagerSupportedFeature.mICC = true;
      break;
    case WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC:
      mColorManagerSupportedFeature.mParametric = true;
      break;
    case WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES:
      mColorManagerSupportedFeature.mPrimaries = true;
      break;
    case WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER:
      mColorManagerSupportedFeature.mFTPower = true;
      break;
    case WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES:
      mColorManagerSupportedFeature.mLuminances = true;
      break;
    case WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES:
      mColorManagerSupportedFeature.mDisplayPrimaries = true;
      break;
  }
}

void nsWaylandDisplay::SetCMSupportedTFNamed(uint32_t aTF) {
  if (aTF < sColorTransfersNum) {
    LOG("nsWaylandDisplay::SetCMSupportedTFNamed() [%d]", aTF);
    mSupportedTransfer[aTF] = aTF;
  } else {
    NS_WARNING("Unknow color transfer function!");
  }
}

void nsWaylandDisplay::SetCMSupportedPrimariesNamed(uint32_t aPrimaries) {
  if (aPrimaries < sColorPrimariesNum) {
    LOG("nsWaylandDisplay::SetCMSupportedPrimariesNamed() [%u]", aPrimaries);
    mSupportedPrimaries[aPrimaries] = aPrimaries;
  } else {
    NS_WARNING("Unknown color primaries!");
  }
}

static void supported_intent(void* data,
                             struct wp_color_manager_v1* color_manager,
                             uint32_t render_intent) {}

static void supported_feature(void* data,
                              struct wp_color_manager_v1* color_manager,
                              uint32_t feature) {
  auto* display = static_cast<nsWaylandDisplay*>(data);
  display->SetCMSupportedFeature(feature);
}

static void supported_tf_named(void* data,
                               struct wp_color_manager_v1* color_manager,
                               uint32_t tf) {
  auto* display = static_cast<nsWaylandDisplay*>(data);
  display->SetCMSupportedTFNamed(tf);
}

static void supported_primaries_named(void* data,
                                      struct wp_color_manager_v1* color_manager,
                                      uint32_t primaries) {
  auto* display = static_cast<nsWaylandDisplay*>(data);
  display->SetCMSupportedPrimariesNamed(primaries);
}

static void supported_done(void* data,
                           struct wp_color_manager_v1* wp_color_manager_v1) {}

static const struct wp_color_manager_v1_listener color_manager_listener = {
    supported_intent,          supported_feature, supported_tf_named,
    supported_primaries_named, supported_done,
};

void nsWaylandDisplay::SetColorManager(wp_color_manager_v1* aColorManager) {
  mColorManager = aColorManager;
  if (mColorManager) {
    LOG("nsWaylandDisplay::SetColorManager()");
    wp_color_manager_v1_add_listener(mColorManager, &color_manager_listener,
                                     this);
  }
}

static void global_registry_handler(void* data, wl_registry* registry,
                                    uint32_t id, const char* interface,
                                    uint32_t version) {
  auto* display = static_cast<nsWaylandDisplay*>(data);
  if (!display) {
    return;
  }

  nsDependentCString iface(interface);
  if (iface.EqualsLiteral("wl_shm")) {
    auto* shm = WaylandRegistryBind<wl_shm>(registry, id, &wl_shm_interface, 1);
    display->SetShm(shm);
  } else if (iface.EqualsLiteral("zwp_idle_inhibit_manager_v1")) {
    auto* idle_inhibit_manager =
        WaylandRegistryBind<zwp_idle_inhibit_manager_v1>(
            registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
    display->SetIdleInhibitManager(idle_inhibit_manager);
  } else if (iface.EqualsLiteral("zwp_relative_pointer_manager_v1")) {
    auto* relative_pointer_manager =
        WaylandRegistryBind<zwp_relative_pointer_manager_v1>(
            registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    display->SetRelativePointerManager(relative_pointer_manager);
  } else if (iface.EqualsLiteral("zwp_pointer_constraints_v1")) {
    auto* pointer_constraints = WaylandRegistryBind<zwp_pointer_constraints_v1>(
        registry, id, &zwp_pointer_constraints_v1_interface, 1);
    display->SetPointerConstraints(pointer_constraints);
  } else if (iface.EqualsLiteral("wl_compositor") &&
             version >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
    auto* compositor = WaylandRegistryBind<wl_compositor>(
        registry, id, &wl_compositor_interface,
        WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION);
    display->SetCompositor(compositor);
  } else if (iface.EqualsLiteral("wl_subcompositor")) {
    auto* subcompositor = WaylandRegistryBind<wl_subcompositor>(
        registry, id, &wl_subcompositor_interface, 1);
    display->SetSubcompositor(subcompositor);
  } else if (iface.EqualsLiteral("wp_viewporter")) {
    auto* viewporter = WaylandRegistryBind<wp_viewporter>(
        registry, id, &wp_viewporter_interface, 1);
    display->SetViewporter(viewporter);
  } else if (iface.EqualsLiteral("zwp_linux_dmabuf_v1")) {
    if (version < ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
      return;
    }
    int vers =
        MIN(version, ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION);
    auto* dmabuf = WaylandRegistryBind<zwp_linux_dmabuf_v1>(
        registry, id, &zwp_linux_dmabuf_v1_interface, vers);
    display->SetDmabuf(dmabuf, vers);
  } else if (iface.EqualsLiteral("xdg_activation_v1")) {
    auto* activation = WaylandRegistryBind<xdg_activation_v1>(
        registry, id, &xdg_activation_v1_interface, 1);
    display->SetXdgActivation(activation);
  } else if (iface.EqualsLiteral("org_kde_kwin_appmenu_manager")) {
    auto* appMenuManager = WaylandRegistryBind<org_kde_kwin_appmenu_manager>(
        registry, id, &org_kde_kwin_appmenu_manager_interface, MIN(version, 2));
    display->SetAppMenuManager(appMenuManager);
  } else if (iface.EqualsLiteral("wl_seat") &&
             version >= WL_POINTER_RELEASE_SINCE_VERSION) {
    auto* seat = WaylandRegistryBind<wl_seat>(
        registry, id, &wl_seat_interface,
        MIN(version, WL_POINTER_AXIS_VALUE120_SINCE_VERSION));
    display->SetSeat(seat, id);
  } else if (iface.EqualsLiteral("wp_fractional_scale_manager_v1")) {
    auto* manager = WaylandRegistryBind<wp_fractional_scale_manager_v1>(
        registry, id, &wp_fractional_scale_manager_v1_interface, 1);
    display->SetFractionalScaleManager(manager);
  } else if (iface.EqualsLiteral("gtk_primary_selection_device_manager") ||
             iface.EqualsLiteral("zwp_primary_selection_device_manager_v1")) {
    display->EnablePrimarySelection();
  } else if (iface.EqualsLiteral("zwp_pointer_gestures_v1") &&
             version >=
                 ZWP_POINTER_GESTURES_V1_GET_HOLD_GESTURE_SINCE_VERSION) {
    auto* gestures = WaylandRegistryBind<zwp_pointer_gestures_v1>(
        registry, id, &zwp_pointer_gestures_v1_interface,
        ZWP_POINTER_GESTURES_V1_GET_HOLD_GESTURE_SINCE_VERSION);
    display->SetPointerGestures(gestures);
  } else if (iface.EqualsLiteral("wp_color_manager_v1")) {
    auto* colorManager = WaylandRegistryBind<wp_color_manager_v1>(
        registry, id, &wp_color_manager_v1_interface, version);
    display->SetColorManager(colorManager);
  } else if (iface.EqualsLiteral("xx_pip_shell_v1")) {
    auto* pipShell = WaylandRegistryBind<xx_pip_shell_v1>(
        registry, id, &xx_pip_shell_v1_interface, version);
    display->SetPipShell(pipShell);
  } else if (iface.EqualsLiteral("xdg_wm_base")) {
    auto* xdgWm = WaylandRegistryBind<xdg_wm_base>(
        registry, id, &xdg_wm_base_interface, version);
    display->SetXdgWm(xdgWm);
  }
}

static void global_registry_remover(void* data, wl_registry* registry,
                                    uint32_t id) {
  auto* display = static_cast<nsWaylandDisplay*>(data);
  if (!display) {
    return;
  }
  display->RemoveSeat(id);
}

static const struct wl_registry_listener registry_listener = {
    global_registry_handler, global_registry_remover};

nsWaylandDisplay::~nsWaylandDisplay() {
  g_list_free_full(mAsyncRoundtrips, (GDestroyNotify)wl_callback_destroy);
}

void nsWaylandDisplay::AsyncRoundtripCallback(void* aData,
                                              wl_callback* aCallback,
                                              uint32_t aTime) {
  auto* display = static_cast<nsWaylandDisplay*>(aData);
  display->mAsyncRoundtrips =
      g_list_remove(display->mAsyncRoundtrips, aCallback);
  wl_callback_destroy(aCallback);
}

static const struct wl_callback_listener async_roundtrip_listener = {
    nsWaylandDisplay::AsyncRoundtripCallback};

void nsWaylandDisplay::RequestAsyncRoundtrip() {
  LOG("nsWaylandDisplay::RequestAsyncRoundtrip()");
  wl_callback* callback = wl_display_sync(mDisplay);
  wl_callback_add_listener(callback, &async_roundtrip_listener, this);
  mAsyncRoundtrips = g_list_append(mAsyncRoundtrips, callback);
}

void nsWaylandDisplay::WaitForAsyncRoundtrips() {
  LOG("nsWaylandDisplay::WaitForAsyncRoundtrips()");
  while (g_list_length(mAsyncRoundtrips) > 0) {
    if (wl_display_dispatch(mDisplay) < 0) {
      NS_WARNING("Failed to get events from Wayland display!");
      return;
    }
  }
}

static void WlLogHandler(const char* format, va_list args) {
  char error[1000];
  VsprintfLiteral(error, format, args);
  gfxCriticalNote << "(" << GetDesktopEnvironmentIdentifier().get()
                  << ") Wayland protocol error: " << error;

  // See Bug 1826583 and Bug 1844653 for reference.
  // "warning: queue %p destroyed while proxies still attached" and variants
  // like "zwp_linux_dmabuf_feedback_v1@%d still attached" are exceptions on
  // Wayland and non-fatal. They are triggered in certain versions of Mesa or
  // the proprietary Nvidia driver and we don't want to crash because of them.
  if (strstr(error, "still attached")) {
    return;
  }

  MOZ_CRASH_UNSAFE_PRINTF("(%s) %s Proxy: %s",
                          GetDesktopEnvironmentIdentifier().get(), error,
                          WaylandProxy::GetState());
}

void WlCompositorCrashHandler() {
  gfxCriticalNote << "Wayland protocol error: Compositor ("
                  << GetDesktopEnvironmentIdentifier().get()
                  << ") crashed, proxy: " << WaylandProxy::GetState();
  MOZ_CRASH_UNSAFE_PRINTF("Compositor crashed (%s) proxy: %s",
                          GetDesktopEnvironmentIdentifier().get(),
                          WaylandProxy::GetState());
}

nsWaylandDisplay::nsWaylandDisplay(wl_display* aDisplay)
    : mThreadId(PR_GetCurrentThread()), mDisplay(aDisplay) {
  MOZ_DIAGNOSTIC_ASSERT(XRE_IsParentProcess());

  // GTK sets the log handler on display creation, thus we overwrite it here
  // in a similar fashion
  wl_log_set_handler_client(WlLogHandler);

  LOG("nsWaylandDisplay::nsWaylandDisplay()");

  mFormats = new DMABufFormats();
  mRegistry = wl_display_get_registry(mDisplay);
  wl_registry_add_listener(mRegistry, &registry_listener, this);
  wl_display_roundtrip(mDisplay);
  RequestAsyncRoundtrip();
  WaitForAsyncRoundtrips();
  EnsureDMABufFormats();

  LOG("nsWaylandDisplay::nsWaylandDisplay() init finished");

  for (auto& e : mSupportedTransfer) {
    e = -1;
  };
  for (auto& e : mSupportedPrimaries) {
    e = -1;
  };

  // Check we have critical Wayland interfaces.
  // Missing ones indicates a compositor bug and we can't continue.
  MOZ_RELEASE_ASSERT(GetShm(), "We're missing shm interface!");
  MOZ_RELEASE_ASSERT(GetCompositor(), "We're missing compositor interface!");
  MOZ_RELEASE_ASSERT(GetSubcompositor(),
                     "We're missing subcompositor interface!");
}

}  // namespace mozilla::widget
