//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "windows_frameless_window.hpp"

#include <QCoreApplication>
#include <QQuickWindow>
#include <QtCore/qt_windows.h>

#include <algorithm>
#include <cmath>

namespace alcedo::ui {
namespace {

auto NativeHandle(void* handle) -> HWND { return static_cast<HWND>(handle); }

}  // namespace

WindowsFramelessWindow::~WindowsFramelessWindow() {
  if (installed_ && QCoreApplication::instance()) {
    QCoreApplication::instance()->removeNativeEventFilter(this);
  }
}

auto WindowsFramelessWindow::Install(QQuickWindow* window) -> bool {
  if (installed_ || !window || !QCoreApplication::instance()) {
    return false;
  }

  const auto hwnd = reinterpret_cast<HWND>(window->winId());
  if (!hwnd) {
    return false;
  }

  window_        = window;
  native_handle_ = hwnd;
  QCoreApplication::instance()->installNativeEventFilter(this);
  installed_ = true;

  const LONG_PTR current_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR native_style =
      current_style | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
  SetLastError(ERROR_SUCCESS);
  const LONG_PTR previous_style = SetWindowLongPtrW(hwnd, GWL_STYLE, native_style);
  if (previous_style == 0 && GetLastError() != ERROR_SUCCESS) {
    QCoreApplication::instance()->removeNativeEventFilter(this);
    installed_     = false;
    native_handle_ = nullptr;
    window_.clear();
    return false;
  }

  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  return true;
}

auto WindowsFramelessWindow::nativeEventFilter(const QByteArray& event_type, void* message,
                                                qintptr* result) -> bool {
  if (!installed_ || !window_ || !native_handle_ || event_type != "windows_generic_MSG") {
    return false;
  }

  const auto* native_message = static_cast<MSG*>(message);
  const HWND  hwnd           = NativeHandle(native_handle_);
  if (!native_message || native_message->hwnd != hwnd) {
    return false;
  }

  switch (native_message->message) {
    case WM_NCCALCSIZE:
      // Keep the native frame styles for DWM behavior, but make the complete
      // window rectangle available to the Qt Quick scene.
      *result = 0;
      return true;

    case WM_GETMINMAXINFO: {
      auto* min_max = reinterpret_cast<MINMAXINFO*>(native_message->lParam);
      if (!min_max) {
        return false;
      }

      const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO    monitor_info{};
      monitor_info.cbSize = sizeof(monitor_info);
      if (GetMonitorInfoW(monitor, &monitor_info)) {
        const RECT& work    = monitor_info.rcWork;
        const RECT& display = monitor_info.rcMonitor;
        min_max->ptMaxPosition.x = work.left - display.left;
        min_max->ptMaxPosition.y = work.top - display.top;
        min_max->ptMaxSize.x     = work.right - work.left;
        min_max->ptMaxSize.y     = work.bottom - work.top;
        min_max->ptMaxTrackSize  = min_max->ptMaxSize;
      }

      const qreal scale = window_->devicePixelRatio();
      min_max->ptMinTrackSize.x =
          std::max<LONG>(1, static_cast<LONG>(std::lround(window_->minimumWidth() * scale)));
      min_max->ptMinTrackSize.y =
          std::max<LONG>(1, static_cast<LONG>(std::lround(window_->minimumHeight() * scale)));
      *result = 0;
      return true;
    }

    default:
      return false;
  }
}

}  // namespace alcedo::ui
