//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractNativeEventFilter>
#include <QPointer>

class QQuickWindow;

namespace alcedo::ui {

/// Preserves the standard Win32 top-level styles while extending the Qt Quick
/// client area through the native frame. DWM therefore retains minimize,
/// maximize, restore, shadow, and taskbar behavior for the custom title bar.
class WindowsFramelessWindow final : public QAbstractNativeEventFilter {
 public:
  WindowsFramelessWindow() = default;
  ~WindowsFramelessWindow() override;

  WindowsFramelessWindow(const WindowsFramelessWindow&)            = delete;
  WindowsFramelessWindow& operator=(const WindowsFramelessWindow&) = delete;

  auto Install(QQuickWindow* window) -> bool;

  auto nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result)
      -> bool override;

 private:
  QPointer<QQuickWindow> window_;
  void*                  native_handle_ = nullptr;
  bool                   installed_     = false;
};

}  // namespace alcedo::ui
