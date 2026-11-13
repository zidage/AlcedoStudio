//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPixmap>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>

namespace alcedo::ui {

inline constexpr const char* kSystemIconImageProviderId = "alcedo-system-icon";

/**
 * @brief Icons that the operating system draws, for buttons that hand work to the system.
 *
 * `image://alcedo-system-icon/file-manager` is the platform file manager's own icon
 * (File Explorer on Windows, Finder on macOS). When the system has no icon (other
 * platforms, the offscreen test platform, unknown ids) the result is a 1x1 transparent
 * pixmap, and QML shows a bundled fallback icon.
 */
class SystemIconImageProvider final : public QQuickImageProvider {
 public:
  SystemIconImageProvider();

  QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requestedSize) override;
};

}  // namespace alcedo::ui
