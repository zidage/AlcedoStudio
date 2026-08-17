//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointer>

class QQuickWindow;

namespace alcedo::ui {

/// Keeps the native macOS traffic lights while the Qt Quick scene occupies the
/// full window. Complements `Qt.ExpandedClientAreaHint` and
/// `Qt.NoTitleBarBackgroundHint` so the title-bar surface stays hidden after
/// maximize / restore. Buttons stay in the system title-bar container; that
/// container is resized so AppKit hit-testing matches the aligned frames.
class MacosFramelessWindow {
 public:
  MacosFramelessWindow()                                       = default;
  ~MacosFramelessWindow()                                      = default;

  MacosFramelessWindow(const MacosFramelessWindow&)            = delete;
  MacosFramelessWindow& operator=(const MacosFramelessWindow&) = delete;

  auto                  Install(QQuickWindow* window) -> bool;

 private:
  static auto            Apply(QQuickWindow* window) -> bool;

  QPointer<QQuickWindow> window_;
  bool                   installed_ = false;
};

}  // namespace alcedo::ui
