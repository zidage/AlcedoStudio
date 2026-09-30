//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/system_icon_image_provider.hpp"

#include <QAbstractFileIconProvider>
#include <QDir>
#include <QFileInfo>
#include <QIcon>

namespace alcedo::ui {
namespace {

constexpr int kDefaultIconSize = 32;

auto FileManagerPath() -> QString {
#if defined(Q_OS_WIN)
  const QString windows = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
  return QDir(windows).filePath(QStringLiteral("explorer.exe"));
#elif defined(Q_OS_MACOS)
  return QStringLiteral("/System/Library/CoreServices/Finder.app");
#else
  return {};
#endif
}

}  // namespace

SystemIconImageProvider::SystemIconImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

QPixmap SystemIconImageProvider::requestPixmap(const QString& id, QSize* size,
                                               const QSize& requestedSize) {
  const QSize target = requestedSize.isValid() && !requestedSize.isEmpty()
                           ? requestedSize
                           : QSize(kDefaultIconSize, kDefaultIconSize);
  QPixmap pixmap;
  if (id == QLatin1String("file-manager")) {
    const QString path = FileManagerPath();
    if (!path.isEmpty() && QFileInfo::exists(path)) {
      QAbstractFileIconProvider provider;
      pixmap = provider.icon(QFileInfo(path)).pixmap(target);
    }
  }
  if (pixmap.isNull()) {
    // A null pixmap makes QML log a load failure; a 1x1 transparent pixmap
    // tells it to show its bundled fallback instead.
    pixmap = QPixmap(1, 1);
    pixmap.fill(Qt::transparent);
  }
  if (size != nullptr) *size = pixmap.size();
  return pixmap;
}

}  // namespace alcedo::ui
